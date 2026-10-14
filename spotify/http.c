#include "http.h"
#include "log.h"
#include "tls.h"
#include "download_safety.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pspnet_inet.h>
#include <pspnet_resolver.h>
#include <pspthreadman.h>

#define PSP_HTONS(x) ((uint16_t)((((uint16_t)(x) & 0xFF) << 8) | (((uint16_t)(x) >> 8) & 0xFF)))
#define BODY_UNTIL_CLOSE UINT64_MAX
#define MAX_REDIRECTS 5
#define SOCK_TIMEOUT_US (15u * 1000u * 1000u)
/* Internal: a pooled connection turned out to be closed; retry fresh. */
#define STALE_CONNECTION (-50)

typedef struct {
    int https;
    char host[256];
    unsigned short port;
    char path[1024];
} url_parts;

/* Only DNS is serialised: the PSP resolver is not known to be safe from
 * several threads at once. Requests themselves run in parallel (a slow
 * cover download must not hold up the audio stream). */
static SceUID dns_lock = -1;

/* Small DNS cache: the same few hosts (spclient, CDNs, i.scdn.co) are looked
 * up for every track. */
#define DNS_CACHE_SIZE 8
#define DNS_TTL_US (10ll * 60 * 1000 * 1000)
static struct {
    char host[64];
    struct in_addr addr;
    SceInt64 at;
} dns_cache[DNS_CACHE_SIZE];
static unsigned int dns_next;

/* Keep-alive pool: a connection whose response was read to the end waits
 * here for the next request to the same host, which then skips the TCP
 * connect and the TLS handshake (250 ms to 1.5 s of CPU each on the PSP). */
#define POOL_SIZE 4
#define POOL_IDLE_US (15ll * 1000 * 1000)
static struct {
    char host[64];
    unsigned short port;
    int https;
    int sock;
    struct tls_conn *tls;
    SceInt64 since;
} pool[POOL_SIZE];
static SceUID pool_lock = -1;
static unsigned int stat_opened, stat_reused;

void http_stats(unsigned int *opened, unsigned int *reused)
{
    *opened = stat_opened;
    *reused = stat_reused;
}

static void close_transport(int sock, struct tls_conn *tls)
{
    if (tls) tls_close(tls);
    if (sock >= 0) sceNetInetClose(sock);
}

int net_resolve(const char *host, struct in_addr *out)
{
    SceInt64 now = sceKernelGetSystemTimeWide();
    if (dns_lock >= 0) sceKernelWaitSema(dns_lock, 1, NULL);
    for (int i = 0; i < DNS_CACHE_SIZE; ++i) {
        if (dns_cache[i].host[0] && strcmp(dns_cache[i].host, host) == 0 &&
            now - dns_cache[i].at < DNS_TTL_US) {
            *out = dns_cache[i].addr;
            if (dns_lock >= 0) sceKernelSignalSema(dns_lock, 1);
            return 0;
        }
    }
    char res_buf[1024];
    int rid = -1;
    int ret = sceNetResolverCreate(&rid, res_buf, sizeof res_buf);
    if (ret >= 0) {
        log_printf("HTTP: resolving %s\n", host);
        ret = sceNetResolverStartNtoA(rid, host, out, 5, 3);
        sceNetResolverDelete(rid);
    }
    if (ret >= 0 && strlen(host) < sizeof dns_cache[0].host) {
        unsigned int slot = dns_next++ % DNS_CACHE_SIZE;
        snprintf(dns_cache[slot].host, sizeof dns_cache[slot].host, "%s", host);
        dns_cache[slot].addr = *out;
        dns_cache[slot].at = now;
    }
    if (dns_lock >= 0) sceKernelSignalSema(dns_lock, 1);
    return ret;
}

int http_init(void)
{
    for (int i = 0; i < POOL_SIZE; ++i) pool[i].sock = -1;
    if (pool_lock < 0) pool_lock = sceKernelCreateSema("http_pool", 0, 1, 1, NULL);
    if (dns_lock < 0) dns_lock = sceKernelCreateSema("dns", 0, 1, 1, NULL);
    SceInt64 t0 = sceKernelGetSystemTimeWide();
    tls_seed_entropy();
    log_printf("HTTP: TLS entropy seeded in %u ms\n",
               (unsigned int)((sceKernelGetSystemTimeWide() - t0) / 1000));
    return 0;
}

void http_term(void)
{
}

static int parse_url(const char *url, url_parts *u)
{
    const char *p;
    if (strncmp(url, "https://", 8) == 0) {
        u->https = 1; u->port = 443; p = url + 8;
    } else if (strncmp(url, "http://", 7) == 0) {
        u->https = 0; u->port = 80; p = url + 7;
    } else {
        return -1;
    }
    size_t host_len = strcspn(p, ":/?");
    if (host_len == 0 || host_len >= sizeof u->host) return -1;
    memcpy(u->host, p, host_len);
    u->host[host_len] = '\0';
    /* Credentials in URL userinfo must never reach host diagnostics. */
    if (strchr(u->host, '@')) return -1;
    p += host_len;
    if (*p == ':') {
        char *end = NULL;
        unsigned long port = strtoul(p + 1, &end, 10);
        if (port == 0 || port > 65535) return -1;
        u->port = (unsigned short)port;
        p = end;
    }
    const char *prefix = (*p == '/') ? "" : "/";
    int n = snprintf(u->path, sizeof u->path, "%s%s", prefix, p);
    return (n < 0 || (size_t)n >= sizeof u->path) ? -1 : 0;
}

static int tcp_connect(const char *host, unsigned short port)
{
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof sin);
    sin.sin_family = AF_INET;
    sin.sin_port = PSP_HTONS(port);
    int ret = net_resolve(host, &sin.sin_addr);
    if (ret < 0) {
        log_printf("HTTP: DNS %s failed 0x%08X\n", host, (unsigned int)ret);
        return ret;
    }
    const uint8_t *ip = (const uint8_t *)&sin.sin_addr;
    log_printf("HTTP: %s -> %u.%u.%u.%u:%u\n", host, (unsigned int)ip[0], (unsigned int)ip[1],
               (unsigned int)ip[2], (unsigned int)ip[3], (unsigned int)port);

    int sock = sceNetInetSocket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return sock;
    unsigned int timeout = SOCK_TIMEOUT_US;
    sceNetInetSetsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    sceNetInetSetsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
    ret = sceNetInetConnect(sock, (struct sockaddr *)&sin, sizeof sin);
    if (ret < 0) {
        log_printf("HTTP: connect %s:%u failed %d\n", host, (unsigned int)port, ret);
        sceNetInetClose(sock);
        return ret;
    }
    return sock;
}

static int transport_recv(http_stream *s, void *buf, size_t len)
{
    if (s->tls) return tls_read(s->tls, buf, len);
    int r = sceNetInetRecv(s->sock, buf, len, 0);
    return r < 0 ? -1 : r;
}

static int transport_send_all(http_stream *s, const void *buf, size_t len)
{
    if (len == 0) return 0;
    if (s->tls) return tls_write_all(s->tls, buf, len);
    const unsigned char *p = buf;
    while (len > 0) {
        int w = sceNetInetSend(s->sock, p, len, 0);
        if (w <= 0) return -1;
        p += w;
        len -= (size_t)w;
    }
    return 0;
}

/* Returns 1 when buffered bytes are available, 0 at EOF, < 0 on error. */
static int fill(http_stream *s)
{
    if (s->buf_pos < s->buf_len) return 1;
    int r = transport_recv(s, s->buf, sizeof s->buf);
    if (r <= 0) return r;
    s->buf_pos = 0;
    s->buf_len = (size_t)r;
    return 1;
}

/* Reads one line, strips CRLF. Reject rather than trust truncated framing. */
static int read_line(http_stream *s, char *line, size_t cap)
{
    size_t n = 0;
    for (;;) {
        if (fill(s) <= 0) return -1;
        char ch = (char)s->buf[s->buf_pos++];
        if (ch == '\0') return -1;
        if (ch == '\n') {
            if (n > 0 && line[n - 1] == '\r') --n;
            line[n] = '\0';
            return (int)n;
        }
        if (n + 1 >= cap) return -1;
        line[n++] = ch;
    }
}

/* True when CRLF-separated header lines contain one named `name` (with colon). */
static int has_header(const char *headers, const char *name)
{
    size_t name_len = strlen(name);
    for (const char *line = headers; line && *line; ) {
        if (strncasecmp(line, name, name_len) == 0) return 1;
        line = strstr(line, "\r\n");
        if (line) line += 2;
    }
    return 0;
}

/* Takes an idle connection to the same host:port from the pool, dropping
 * ones idle too long. Returns 1 with *sock and *tls set. */
static int pool_take(const url_parts *u, int *sock, struct tls_conn **tls)
{
    if (pool_lock < 0) return 0;
    const SceInt64 now = sceKernelGetSystemTimeWide();
    int found = 0;
    sceKernelWaitSema(pool_lock, 1, NULL);
    for (int i = 0; i < POOL_SIZE; ++i) {
        if (pool[i].sock < 0) continue;
        if (now - pool[i].since > POOL_IDLE_US) {
            close_transport(pool[i].sock, pool[i].tls);
            pool[i].sock = -1;
            pool[i].tls = NULL;
        } else if (!found && pool[i].port == u->port && pool[i].https == u->https &&
                   strcmp(pool[i].host, u->host) == 0) {
            *sock = pool[i].sock;
            *tls = pool[i].tls;
            pool[i].sock = -1;
            pool[i].tls = NULL;
            found = 1;
        }
    }
    sceKernelSignalSema(pool_lock, 1);
    return found;
}

static void pool_put(http_stream *s)
{
    sceKernelWaitSema(pool_lock, 1, NULL);
    int slot = 0;
    for (int i = 0; i < POOL_SIZE; ++i) {
        if (pool[i].sock < 0) {
            slot = i;
            break;
        }
        if (pool[i].since < pool[slot].since) slot = i;
    }
    if (pool[slot].sock >= 0) close_transport(pool[slot].sock, pool[slot].tls);
    memcpy(pool[slot].host, s->host, sizeof pool[slot].host);
    pool[slot].port = s->port;
    pool[slot].https = s->https;
    pool[slot].sock = s->sock;
    pool[slot].tls = s->tls;
    pool[slot].since = sceKernelGetSystemTimeWide();
    sceKernelSignalSema(pool_lock, 1);
}

void http_stream_close(http_stream *s)
{
    /* Reusable only after the whole body, with nothing extra buffered. */
    const int done = s->body_done || (s->length_known && !s->chunked && s->remaining == 0);
    if (s->keep && done && s->buf_pos == s->buf_len && s->sock >= 0 && s->host[0] &&
        pool_lock >= 0)
        pool_put(s);
    else
        close_transport(s->sock, s->tls);
    s->tls = NULL;
    s->sock = -1;
    s->keep = 0;
}

static int open_once(http_stream *s, const char *method, const url_parts *u,
                     const char *extra_headers, const void *body, size_t body_len,
                     uint64_t *content_length, char *location, size_t location_cap,
                     int may_reuse)
{
    memset(s, 0, sizeof *s);
    s->sock = -1;
    location[0] = '\0';
    *content_length = 0; /* Each redirect has independent body framing. */

    const int reused = may_reuse && pool_take(u, &s->sock, &s->tls);
    if (reused) {
        ++stat_reused;
    } else {
        s->sock = tcp_connect(u->host, u->port);
        if (s->sock < 0) return s->sock;
        if (u->https) {
            s->tls = tls_open(s->sock, u->host);
            if (!s->tls) return -3;
        }
        ++stat_opened;
    }
    if (strlen(u->host) < sizeof s->host) memcpy(s->host, u->host, strlen(u->host) + 1);
    s->port = u->port;
    s->https = (unsigned char)u->https;
    s->keep = 1;

    /* Only add a default Accept when the caller has none: with two Accept
     * headers clienttoken.spotify.com answers 200 with an empty body. */
    const char *default_accept = has_header(extra_headers, "Accept:") ? "" : "Accept: */*\r\n";
    char req[2048];
    int n = snprintf(req, sizeof req,
                     "%s %s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "User-Agent: PSP-Spotify/1.0\r\n"
                     "%s"
                     "Accept-Encoding: identity\r\n"
                     "Connection: keep-alive\r\n",
                     method, u->path, u->host, default_accept);
    if (n < 0 || (size_t)n >= sizeof req) return -4;
    size_t used = (size_t)n;
    if (body || strcmp(method, "POST") == 0) {
        n = snprintf(req + used, sizeof req - used, "Content-Length: %u\r\n",
                     (unsigned int)body_len);
        if (n < 0 || (size_t)n >= sizeof req - used) return -4;
        used += (size_t)n;
    }
    n = snprintf(req + used, sizeof req - used, "%s\r\n", extra_headers ? extra_headers : "");
    if (n < 0 || (size_t)n >= sizeof req - used) return -4;
    used += (size_t)n;

    /* A pooled connection the server has meanwhile closed fails here, before
     * any response: the caller retries on a fresh one. */
    if (transport_send_all(s, req, used) < 0 || transport_send_all(s, body, body_len) < 0) {
        if (reused) return STALE_CONNECTION;
        log_printf("HTTP: send failed\n");
        return -5;
    }

    char line[1024];
    if (read_line(s, line, sizeof line) < 0) {
        if (reused) return STALE_CONNECTION;
        log_printf("HTTP: no status line\n");
        return -6;
    }
    const char *sp = strchr(line, ' ');
    int status = sp ? (int)strtol(sp + 1, NULL, 10) : 0;
    if (strncmp(line, "HTTP/", 5) != 0 || status <= 0) {
        log_printf("HTTP: bad status line (content omitted)\n");
        return -6;
    }

    int have_length = 0;
    for (;;) {
        int len = read_line(s, line, sizeof line);
        if (len < 0) return -7;
        if (len == 0) break;
        if (line[0] == ' ' || line[0] == '\t') {
            log_printf("HTTP: folded header refused\n");
            return -9;
        }
        if (strncasecmp(line, "Content-Length:", 15) == 0) {
            if (have_length || download_parse_length(line + 15, content_length) < 0) {
                log_printf("HTTP: invalid or duplicate Content-Length; response refused\n");
                return -9;
            }
            have_length = 1;
        } else if (strncasecmp(line, "Transfer-Encoding:", 18) == 0) {
            char *v = line + 18;
            while (*v == ' ' || *v == '\t') ++v;
            size_t vn = strlen(v);
            while (vn && (v[vn - 1] == ' ' || v[vn - 1] == '\t')) v[--vn] = '\0';
            if (s->chunked || strcasecmp(v, "chunked") != 0) {
                log_printf("HTTP: unsupported Transfer-Encoding; response refused\n");
                return -9;
            }
            s->chunked = 1;
        } else if (strncasecmp(line, "Connection:", 11) == 0) {
            if (strstr(line + 11, "close") || strstr(line + 11, "Close")) s->keep = 0;
        } else if (strncasecmp(line, "Location:", 9) == 0) {
            const char *v = line + 9;
            while (*v == ' ' || *v == '\t') ++v;
            int ln = snprintf(location, location_cap, "%s", v);
            if (ln < 0 || (size_t)ln >= location_cap) location[0] = '\0';
        }
    }

    if (s->chunked) {
        if (have_length) {
            log_printf("HTTP: ambiguous body framing; response refused\n");
            return -9;
        }
        *content_length = 0;
        s->remaining = 0;
    } else if (have_length) {
        s->length_known = 1;
        s->remaining = *content_length;
    } else {
        s->remaining = BODY_UNTIL_CLOSE;
        s->keep = 0;
    }
    return status;
}

int http_stream_open_ex(http_stream *s, const char *method, const char *url,
                        const char *extra_headers, const void *body, size_t body_len,
                        uint64_t *content_length)
{
    url_parts u;
    char location[1024];
    *content_length = 0;
    s->sock = -1;
    s->tls = NULL;
    if (parse_url(url, &u) < 0) {
        log_printf("HTTP: bad URL\n");
        return -1;
    }

    for (int hop = 0; hop <= MAX_REDIRECTS; ++hop) {
        int status = open_once(s, method, &u, extra_headers, body, body_len,
                               content_length, location, sizeof location, 1);
        if (status == STALE_CONNECTION) {
            s->keep = 0;
            http_stream_close(s);
            status = open_once(s, method, &u, extra_headers, body, body_len,
                               content_length, location, sizeof location, 0);
        }
        if (status < 0) {
            http_stream_close(s);
            return status;
        }
        int redirect = status == 301 || status == 302 || status == 303 ||
                       status == 307 || status == 308;
        if (!redirect || location[0] == '\0') return status;

        http_stream_close(s);
        log_printf("HTTP: redirect %d (destination omitted)\n", status);
        if (location[0] == '/') {
            int n = snprintf(u.path, sizeof u.path, "%s", location);
            if (n < 0 || (size_t)n >= sizeof u.path) return -1;
        } else if (parse_url(location, &u) < 0) {
            log_printf("HTTP: bad redirect URL\n");
            return -1;
        }
        if (status == 303) {
            method = "GET";
            body = NULL;
            body_len = 0;
        }
    }
    log_printf("HTTP: too many redirects\n");
    return -8;
}

int http_stream_open(http_stream *s, const char *url, uint64_t *content_length)
{
    return http_stream_open_ex(s, "GET", url, NULL, NULL, 0, content_length);
}

int http_stream_read(http_stream *s, void *out, unsigned int len)
{
    if (s->body_done) return 0;
    if (s->chunked && s->remaining == 0) {
        char line[64];
        /* Each chunk's data is followed by CRLF before the next size line. */
        if (s->chunk_started && read_line(s, line, sizeof line) < 0) return -1;
        if (read_line(s, line, sizeof line) < 0) return -1;
        s->chunk_started = 1;
        s->remaining = strtoull(line, NULL, 16);
        if (s->remaining == 0) {
            /* Skip the trailer up to its blank line, so a kept connection
             * starts the next response cleanly. */
            int n;
            while ((n = read_line(s, line, sizeof line)) > 0) {}
            if (n < 0) s->keep = 0;
            s->body_done = 1;
            return 0;
        }
    }
    if (s->remaining == 0) {
        s->body_done = 1;
        return 0;
    }

    size_t want = len;
    if (want > s->remaining) want = (size_t)s->remaining;
    int got;
    if (s->buf_pos < s->buf_len) {
        size_t avail = s->buf_len - s->buf_pos;
        if (want > avail) want = avail;
        memcpy(out, s->buf + s->buf_pos, want);
        s->buf_pos += want;
        got = (int)want;
    } else {
        got = transport_recv(s, out, want);
        if (got == 0) {
            if (s->remaining == BODY_UNTIL_CLOSE) {
                s->body_done = 1;
                return 0;
            }
            log_printf("HTTP: body truncated\n");
            return -1;
        }
        if (got < 0) return -1;
    }
    if (s->remaining != BODY_UNTIL_CLOSE) s->remaining -= (uint64_t)got;
    return got;
}
