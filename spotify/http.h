#ifndef SPOTIFY_HTTP_H
#define SPOTIFY_HTTP_H

#include <stdint.h>
#include <stddef.h>

/* Minimal HTTP/1.1 client over PSP sockets, with TLS 1.2 via BearSSL for
 * https:// URLs. Follows up to 5 redirects; decodes chunked bodies. */

struct tls_conn;

typedef struct {
    int sock;
    struct tls_conn *tls;
    /* Body framing */
    int chunked;
    int length_known;
    int chunk_started;
    uint64_t remaining;      /* bytes left in body (or current chunk) */
    int body_done;
    /* Bytes already pulled from the transport but not yet returned */
    unsigned char buf[2048];
    size_t buf_pos, buf_len;
    /* Keep-alive: where the connection goes, and whether the server lets
     * it carry another request once this body has been read to the end. */
    char host[64];
    unsigned short port;
    unsigned char https;
    unsigned char keep;
} http_stream;

struct in_addr;
/* DNS lookup with a short cache, serialised across threads. Returns < 0 on
 * failure. Used for HTTP and for the Spotify access point. */
int net_resolve(const char *host, struct in_addr *out);

/* Seeds TLS entropy. Call once after the network is up. */
int http_init(void);
void http_term(void);

/* Opens a request. method is "GET" or "POST"; extra_headers is NULL or
 * CRLF-terminated header lines. Returns the HTTP status (>0) or a negative
 * error. *content_length is 0 when unknown (chunked or absent). After a
 * positive return the caller must http_stream_close(). */
int http_stream_open_ex(http_stream *s, const char *method, const char *url,
                        const char *extra_headers, const void *body, size_t body_len,
                        uint64_t *content_length);
int http_stream_open(http_stream *s, const char *url, uint64_t *content_length);
/* Returns bytes read, 0 at end of body, or < 0 on error. */
int http_stream_read(http_stream *s, void *buf, unsigned int len);
/* Closes the stream. A connection whose body was read to the end is kept
 * for a few seconds and reused by the next request to the same host. */
void http_stream_close(http_stream *s);
/* Connections opened and reused since start, for the log. */
void http_stats(unsigned int *opened, unsigned int *reused);

#endif /* SPOTIFY_HTTP_H */
