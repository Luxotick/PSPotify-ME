#include "site.h"
#include "http.h"
#include "log.h"
#include <bearssl.h>
#include <pspiofilemgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PSPOTIFY_VERSION
#define PSPOTIFY_VERSION "0.0.0"
#endif

#define THEME_FILE_MAX (2u * 1024u * 1024u)
#define EBOOT_MAX (8u * 1024u * 1024u)

/* Appends seg to out, percent-encoding all but unreserved characters (theme
 * folders may hold spaces). */
static void url_append(char *out, size_t cap, const char *seg)
{
    size_t n = strlen(out);
    for (const unsigned char *p = (const unsigned char *)seg; *p && n + 4 < cap; ++p) {
        const unsigned c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out[n++] = (char)c;
        } else {
            static const char hex[] = "0123456789ABCDEF";
            out[n++] = '%';
            out[n++] = hex[c >> 4];
            out[n++] = hex[c & 15];
        }
    }
    out[n] = '\0';
}

/* A folder or file name from the site that is safe to create on the Memory
 * Stick: printable ASCII, no path characters, and for files one of the
 * extensions a theme uses. */
static int safe_name(const char *s, size_t cap, int file)
{
    const size_t n = strlen(s);
    if (n == 0 || n >= cap || !strcmp(s, ".") || !strcmp(s, "..")) return 0;
    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c > 0x7E || strchr("/\\:*?\"<>|", c)) return 0;
    }
    if (!file) return 1;
    const char *dot = strrchr(s, '.');
    if (!dot) return 0;
    static const char *const ok[] = { ".ini", ".jpg", ".jpeg", ".png" };
    for (size_t i = 0; i < sizeof ok / sizeof ok[0]; ++i) {
        const char *e = ok[i];
        size_t k = 0;
        while (e[k] && dot[k] && (dot[k] | 0x20) == e[k]) ++k;
        if (!e[k] && !dot[k]) return 1;
    }
    return 0;
}

/* GET SITE_URL + path into buf, NUL-terminated. Returns the length or < 0. */
static int fetch_text(const char *path, char *buf, size_t cap)
{
    char url[256];
    snprintf(url, sizeof url, "%s%s", SITE_URL, path);
    http_stream s;
    uint64_t len = 0;
    const int status = http_stream_open(&s, url, &len);
    if (status != 200) {
        if (status > 0) http_stream_close(&s);
        log_printf("SITE: %s: HTTP %d\n", path, status);
        return -1;
    }
    size_t got = 0;
    for (;;) {
        const int r = http_stream_read(&s, buf + got, (unsigned int)(cap - 1 - got));
        if (r <= 0) break;
        got += (size_t)r;
        if (got >= cap - 1) break;
    }
    http_stream_close(&s);
    buf[got] = '\0';
    return (int)got;
}

/* Streams url into path through path.part. sha (optional) sees every byte;
 * expect (optional) is the size it must have; *progress (optional) goes
 * 0..100 against it. Returns 0 or < 0, leaving nothing behind on failure. */
static int download_to(const char *url, const char *path, unsigned int max_bytes,
                       br_sha256_context *sha, unsigned int expect, volatile int *progress)
{
    char part[372];
    snprintf(part, sizeof part, "%s.part", path);
    http_stream s;
    uint64_t len = 0;
    const int status = http_stream_open_download(&s, url, NULL, &len);
    if (status != 200) {
        if (status > 0) http_stream_close(&s);
        log_printf("SITE: download HTTP %d\n", status);
        return -1;
    }
    if (len > max_bytes) {
        http_stream_close(&s);
        log_printf("SITE: download too big (%u bytes)\n", (unsigned int)len);
        return -2;
    }
    const SceUID fd = sceIoOpen(part, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) {
        http_stream_close(&s);
        log_printf("SITE: cannot write %s (0x%08X)\n", part, (unsigned int)fd);
        return -3;
    }
    static uint8_t chunk[16 * 1024];
    unsigned int total = 0;
    int ok = 1;
    for (;;) {
        const int r = http_stream_read(&s, chunk, sizeof chunk);
        if (r == 0) break;
        if (r < 0 || total + (unsigned int)r > max_bytes ||
            sceIoWrite(fd, chunk, (SceSize)r) != r) {
            ok = 0;
            break;
        }
        total += (unsigned int)r;
        if (sha) br_sha256_update(sha, chunk, (size_t)r);
        if (progress && expect) *progress = (int)((uint64_t)total * 100u / expect);
    }
    http_stream_close(&s);
    sceIoClose(fd);
    if (!ok || (len && total != len) || (expect && total != expect)) {
        sceIoRemove(part);
        log_printf("SITE: download failed after %u bytes\n", total);
        return -4;
    }
    sceIoRemove(path);
    if (sceIoRename(part, path) < 0) {
        sceIoRemove(part);
        return -5;
    }
    return 0;
}

static uint32_t parse_hex_color(const char *s)
{
    if (*s == '#') ++s;
    const size_t n = strlen(s);
    if (n != 6) return 0xFF475569u;
    return 0xFF000000u | (uint32_t)strtoul(s, NULL, 16);
}

int site_fetch_themes(site_theme *out, int max)
{
    static char text[16 * 1024];
    if (fetch_text("themes/psp.txt", text, sizeof text) < 0) return -1;
    /* One theme per line: folder, accent, then its files, tab-separated. */
    int count = 0;
    for (char *line = text, *next; line && *line && count < max; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = '\0';
        size_t n = strlen(line);
        if (n && line[n - 1] == '\r') line[--n] = '\0';
        if (!n || line[0] == '#') continue;
        site_theme *t = &out[count];
        memset(t, 0, sizeof *t);
        int field = 0;
        for (char *f = line, *tab; f; f = tab, ++field) {
            tab = strchr(f, '\t');
            if (tab) *tab++ = '\0';
            if (field == 0) {
                if (!safe_name(f, sizeof t->folder, 0)) break;
                strcpy(t->folder, f);
            } else if (field == 1) {
                t->accent = parse_hex_color(f);
            } else if (t->file_count < SITE_THEME_FILES && safe_name(f, sizeof t->files[0], 1)) {
                strcpy(t->files[t->file_count++], f);
                const char *dot = strrchr(f, '.');
                if ((dot[1] | 0x20) != 'i') t->has_wallpaper = 1;
            }
        }
        if (t->folder[0] && t->file_count > 0) ++count;
    }
    log_printf("SITE: %d themes in the gallery\n", count);
    return count;
}

int site_download_theme(const site_theme *t, const char *themes_dir)
{
    char dir[300];
    snprintf(dir, sizeof dir, "%s/%s", themes_dir, t->folder);
    sceIoMkdir(themes_dir, 0777);
    sceIoMkdir(dir, 0777);
    for (int i = 0; i < t->file_count; ++i) {
        char url[256], path[360];
        snprintf(url, sizeof url, "%sthemes/", SITE_URL);
        url_append(url, sizeof url, t->folder);
        snprintf(url + strlen(url), sizeof url - strlen(url), "/");
        url_append(url, sizeof url, t->files[i]);
        snprintf(path, sizeof path, "%s/%s", dir, t->files[i]);
        if (download_to(url, path, THEME_FILE_MAX, NULL, 0, NULL) < 0) return -1;
    }
    log_printf("SITE: theme %s installed (%d files)\n", t->folder, t->file_count);
    return 0;
}

int version_compare(const char *a, const char *b)
{
    for (int part = 0; part < 3; ++part) {
        char *end;
        const long x = strtol(a, &end, 10);
        a = *end == '.' ? end + 1 : end;
        const long y = strtol(b, &end, 10);
        b = *end == '.' ? end + 1 : end;
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

static int hex_digest(const char *s, uint8_t out[32])
{
    if (strlen(s) != 64) return 0;
    for (int i = 0; i < 32; ++i) {
        unsigned v = 0;
        for (int k = 0; k < 2; ++k) {
            const char c = s[i * 2 + k];
            const int d = c >= '0' && c <= '9' ? c - '0' : (c | 0x20) >= 'a' && (c | 0x20) <= 'f'
                        ? (c | 0x20) - 'a' + 10 : -1;
            if (d < 0) return 0;
            v = v * 16 + (unsigned)d;
        }
        out[i] = (uint8_t)v;
    }
    return 1;
}

/* A relative path under SITE_URL: letters, digits and ._-/ only, no "..". */
static int safe_path(const char *s)
{
    if (!*s || *s == '/' || strstr(s, "..")) return 0;
    for (; *s; ++s)
        if (!((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') ||
              *s == '.' || *s == '_' || *s == '-' || *s == '/'))
            return 0;
    return 1;
}

int site_check_update(site_update *u)
{
    static char text[2048];
    memset(u, 0, sizeof *u);
    if (fetch_text("update.txt", text, sizeof text) < 0) return -1;
    int have_sha = 0;
    for (char *line = text, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = '\0';
        char key[16], value[96];
        if (sscanf(line, " %15[a-z0-9_] = %95[^\r\n]", key, value) != 2) continue;
        if (!strcmp(key, "version")) snprintf(u->version, sizeof u->version, "%.15s", value);
        else if (!strcmp(key, "tag")) snprintf(u->tag, sizeof u->tag, "%.31s", value);
        else if (!strcmp(key, "size")) u->size = (unsigned int)strtoul(value, NULL, 10);
        else if (!strcmp(key, "sha256")) have_sha = hex_digest(value, u->sha256);
        else if (!strcmp(key, "eboot") && safe_path(value))
            snprintf(u->eboot, sizeof u->eboot, "%.63s", value);
    }
    if (!u->version[0] || !u->eboot[0] || !have_sha || u->size == 0 || u->size > EBOOT_MAX) {
        log_printf("SITE: update.txt incomplete\n");
        return -2;
    }
    const int newer = version_compare(u->version, PSPOTIFY_VERSION) > 0;
    log_printf("SITE: latest %s, this is %s\n", u->version, PSPOTIFY_VERSION);
    return newer;
}

int site_install_update(const site_update *u, const char *app_dir, volatile int *progress)
{
    char url[256], current[300], fresh[300], old[300];
    snprintf(url, sizeof url, "%s%s", SITE_URL, u->eboot);
    snprintf(current, sizeof current, "%s/EBOOT.PBP", app_dir);
    snprintf(fresh, sizeof fresh, "%s/EBOOT.NEW", app_dir);
    snprintf(old, sizeof old, "%s/EBOOT.OLD", app_dir);
    *progress = 0;
    br_sha256_context sha;
    br_sha256_init(&sha);
    if (download_to(url, fresh, EBOOT_MAX, &sha, u->size, progress) < 0) return -1;
    uint8_t digest[32];
    br_sha256_out(&sha, digest);
    if (memcmp(digest, u->sha256, sizeof digest) != 0) {
        log_printf("SITE: update checksum mismatch, not installed\n");
        sceIoRemove(fresh);
        return -2;
    }
    /* Keep the running EBOOT as EBOOT.OLD; put it back if the swap fails. */
    sceIoRemove(old);
    if (sceIoRename(current, old) < 0) {
        sceIoRemove(fresh);
        log_printf("SITE: cannot move EBOOT.PBP aside\n");
        return -3;
    }
    if (sceIoRename(fresh, current) < 0) {
        sceIoRename(old, current);
        sceIoRemove(fresh);
        log_printf("SITE: cannot put the new EBOOT in place\n");
        return -4;
    }
    *progress = 100;
    log_printf("SITE: updated to %s\n", u->version);
    return 0;
}
