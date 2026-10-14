#include "connect.h"
#include "config.h"
#include "http.h"
#include "log.h"
#include "proto_util.h"
#include "tls.h"
#include <pspnet_inet.h>
#include <psprtc.h>
#include <pspthreadman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* stb_image's zlib decoder (spotify/stb_image_impl.c), for gzip pushes. */
extern int stbi_zlib_decode_noheader_buffer(char *obuffer, int olen, const char *ibuffer,
                                            int ilen);

#define DEALER_HOST "dealer.spotify.com"
#define PING_US (30ll * 1000 * 1000)
#define RX_CAP (96 * 1024)
#define MSG_CAP (128 * 1024)
#define MAX_CONNECT_TRACKS 500

static spotify_ctx *session;
static char device_name[48];
static SceUID thread = -1;
static volatile int quit;
static volatile int online;

static tls_conn *ws;
static int ws_sock = -1;
static char connection_id[256];

/* Command mailbox: one command at a time, taken by the UI thread. */
static connect_command mailbox;
static volatile int mailbox_full, play_tracks_busy;
static spotify_track play_tracks[MAX_CONNECT_TRACKS];

/* Latest status from the UI thread and what was last published. */
static connect_status status_now, status_sent;
static volatile unsigned int status_seq;
static unsigned int status_sent_seq;
static int64_t sent_at_ms, started_playing_at;
static char last_command_device[64];
static unsigned int last_command_id;

/* ------------------------------------------------------------ helpers */

static int64_t now_ms(void)
{
    /* RTC ticks are microseconds since 0001-01-01 UTC. */
    u64 tick = 0;
    sceRtcGetCurrentTick(&tick);
    return (int64_t)(tick / 1000u) - 62135596800000ll;
}

static unsigned int next_random(void)
{
    static unsigned int s = 0x2545F491u;
    s ^= (unsigned int)sceKernelGetSystemTimeLow();
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

static void copy_text(char *dst, size_t cap, const char *src, size_t len)
{
    if (len >= cap) len = cap - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static const char base64_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t base64_encode(const uint8_t *in, size_t len, char *out)
{
    size_t n = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < len) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < len) v |= in[i + 2];
        out[n++] = base64_chars[(v >> 18) & 63];
        out[n++] = base64_chars[(v >> 12) & 63];
        out[n++] = i + 1 < len ? base64_chars[(v >> 6) & 63] : '=';
        out[n++] = i + 2 < len ? base64_chars[v & 63] : '=';
    }
    out[n] = '\0';
    return n;
}

static int base64_value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

/* Decodes len characters (stops at '=' or a non-base64 character). */
static size_t base64_decode(const char *in, size_t len, uint8_t *out, size_t cap)
{
    uint32_t acc = 0;
    int bits = 0;
    size_t n = 0;
    for (size_t i = 0; i < len; ++i) {
        if (in[i] == '\\') continue;    /* JSON-escaped "\/" */
        int v = base64_value(in[i]);
        if (v < 0) break;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n == cap) return n;
            out[n++] = (uint8_t)(acc >> bits);
        }
    }
    return n;
}

/* gzip member -> raw deflate data -> out. Returns the size or -1. */
static int gunzip(const uint8_t *in, size_t len, uint8_t *out, size_t cap)
{
    if (len < 18 || in[0] != 0x1f || in[1] != 0x8b || in[2] != 8) {
        log_printf("CONNECT: not a gzip payload\n");
        return -1;
    }
    const uint8_t flags = in[3];
    size_t p = 10;
    if (flags & 4) p += 2 + (size_t)(in[10] | (in[11] << 8));
    if (flags & 8) while (p < len && in[p++]) {}
    if (flags & 16) while (p < len && in[p++]) {}
    if (flags & 2) p += 2;
    if (p >= len) return -1;
    return stbi_zlib_decode_noheader_buffer((char *)out, (int)cap, (const char *)(in + p),
                                            (int)(len - p));
}

/* Base64 payload, maybe gzip-compressed, into a fresh malloc'd buffer
 * (NUL-terminated). Returns the size or -1. */
static int decode_payload(const char *b64, size_t b64_len, uint8_t **out)
{
    size_t raw_cap = b64_len * 3 / 4 + 4;
    uint8_t *raw = malloc(raw_cap);
    if (!raw) return -1;
    size_t raw_len = base64_decode(b64, b64_len, raw, raw_cap);
    if (raw_len >= 2 && raw[0] == 0x1f && raw[1] == 0x8b) {
        size_t cap = raw_len * 12 + 4096;
        if (cap > 512 * 1024) cap = 512 * 1024;
        uint8_t *plain = malloc(cap + 1);
        int n = plain ? gunzip(raw, raw_len, plain, cap) : -1;
        free(raw);
        if (n < 0) {
            log_printf("CONNECT: gzip payload did not inflate (%u bytes)\n", (unsigned int)raw_len);
            free(plain);
            return -1;
        }
        plain[n] = 0;
        *out = plain;
        return n;
    }
    raw[raw_len < raw_cap ? raw_len : raw_cap - 1] = 0;
    *out = raw;
    return (int)raw_len;
}

/* The string value of the first "key" at or after from, unescaped only for
 * \" and \/. Returns its length or -1. */
static int json_str(const char *from, const char *key, char *out, size_t cap)
{
    char pattern[64];
    int n = snprintf(pattern, sizeof pattern, "\"%s\"", key);
    if (n < 0 || (size_t)n >= sizeof pattern) return -1;
    const char *p = strstr(from, pattern);
    if (!p) return -1;
    p += n;
    while (*p == ' ' || *p == ':') ++p;
    if (*p++ != '"') return -1;
    size_t len = 0;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) ++p;
        if (len + 1 < cap) out[len++] = *p;
        ++p;
    }
    out[len] = '\0';
    return (int)len;
}

/* Where the string value of "key" starts, with its length; NULL if absent. */
static const char *json_str_span(const char *from, const char *key, size_t *len)
{
    char pattern[64];
    int n = snprintf(pattern, sizeof pattern, "\"%s\"", key);
    if (n < 0 || (size_t)n >= sizeof pattern) return NULL;
    const char *p = strstr(from, pattern);
    if (!p) return NULL;
    p += n;
    while (*p == ' ' || *p == ':') ++p;
    if (*p++ != '"') return NULL;
    const char *end = strchr(p, '"');
    if (!end) return NULL;
    *len = (size_t)(end - p);
    return p;
}

/* Number or boolean value of "key"; fallback when absent. */
static long long json_num(const char *from, const char *key, long long fallback)
{
    char pattern[64];
    int n = snprintf(pattern, sizeof pattern, "\"%s\"", key);
    if (n < 0 || (size_t)n >= sizeof pattern) return fallback;
    const char *p = strstr(from, pattern);
    if (!p) return fallback;
    p += n;
    while (*p == ' ' || *p == ':' || *p == '"') ++p;
    if (!strncmp(p, "true", 4)) return 1;
    if (!strncmp(p, "false", 5)) return 0;
    if (*p != '-' && (*p < '0' || *p > '9')) return fallback;
    return strtoll(p, NULL, 10);
}

/* ------------------------------------------------------------ protobuf writer */

typedef struct {
    uint8_t *buf;
    size_t len, cap;
    int overflow;
} pbw;

static void pbw_raw(pbw *w, const void *data, size_t len)
{
    if (w->len + len > w->cap) {
        w->overflow = 1;
        return;
    }
    memcpy(w->buf + w->len, data, len);
    w->len += len;
}

static void pbw_varint_raw(pbw *w, uint64_t v)
{
    uint8_t b[10];
    size_t n = 0;
    do {
        b[n] = (uint8_t)(v & 0x7F);
        v >>= 7;
        if (v) b[n] |= 0x80;
        ++n;
    } while (v);
    pbw_raw(w, b, n);
}

static void pbw_uint(pbw *w, unsigned int field, uint64_t v)
{
    if (!v) return;     /* proto3 default */
    pbw_varint_raw(w, (uint64_t)field << 3);
    pbw_varint_raw(w, v);
}

static void pbw_bool(pbw *w, unsigned int field, int v)
{
    pbw_uint(w, field, v ? 1 : 0);
}

static void pbw_bytes(pbw *w, unsigned int field, const void *data, size_t len)
{
    pbw_varint_raw(w, ((uint64_t)field << 3) | 2);
    pbw_varint_raw(w, len);
    pbw_raw(w, data, len);
}

static void pbw_str(pbw *w, unsigned int field, const char *s)
{
    if (s && s[0]) pbw_bytes(w, field, s, strlen(s));
}

static void pbw_double(pbw *w, unsigned int field, double v)
{
    uint64_t bits;
    memcpy(&bits, &v, sizeof bits);
    uint8_t b[8];
    for (int i = 0; i < 8; ++i) b[i] = (uint8_t)(bits >> (8 * i));
    pbw_varint_raw(w, ((uint64_t)field << 3) | 1);
    pbw_raw(w, b, 8);
}

static void pbw_msg(pbw *w, unsigned int field, const pbw *inner)
{
    if (inner->overflow) w->overflow = 1;
    pbw_bytes(w, field, inner->buf, inner->len);
}

/* ------------------------------------------------------------ state */

/* PutStateRequest (connect.proto) for this device and what it plays. */
static size_t build_state(uint8_t *out, size_t cap, int reason, const connect_status *s)
{
    static uint8_t caps_buf[256], info_buf[1024], track_buf[128], options_buf[32],
        player_buf[1024], device_buf[2048];
    pbw caps = { caps_buf, 0, sizeof caps_buf, 0 };
    pbw_bool(&caps, 2, 1);                      /* can_be_player */
    pbw_bool(&caps, 5, 1);                      /* gaia_eq_connect_id */
    pbw_bool(&caps, 7, 1);                      /* is_observable */
    pbw_uint(&caps, 8, 20);                     /* volume_steps */
    pbw_str(&caps, 9, "audio/track");           /* supported_types */
    pbw_bool(&caps, 10, 1);                     /* command_acks */
    pbw_bool(&caps, 15, 1);                     /* supports_playlist_v2 */
    pbw_bool(&caps, 16, 1);                     /* is_controllable */
    pbw_bool(&caps, 19, 1);                     /* supports_transfer_command */
    pbw_bool(&caps, 20, 1);                     /* supports_command_request */
    pbw_bool(&caps, 25, 1);                     /* supports_set_options_command */

    pbw info = { info_buf, 0, sizeof info_buf, 0 };
    pbw_bool(&info, 1, 1);                      /* can_play */
    pbw_uint(&info, 2, (uint64_t)(s->volume * 65535 / 100));
    pbw_str(&info, 3, device_name);
    pbw_msg(&info, 4, &caps);
    pbw_str(&info, 6, "PSPotify ME");           /* device_software_version */
    pbw_uint(&info, 7, 9);                      /* device_type: GAME_CONSOLE */
    pbw_str(&info, 9, "3.2.6");                 /* spirc_version */
    pbw_str(&info, 10, SPOTIFY_DEVICE_ID);
    pbw_str(&info, 13, SPOTIFY_CLIENT_ID_KEYMASTER);
    pbw_str(&info, 14, "Sony");
    pbw_str(&info, 15, "PlayStation Portable");

    const int64_t now = now_ms();
    pbw player = { player_buf, 0, sizeof player_buf, 0 };
    if (s->active && s->track_id[0]) {
        char uri[48], url[128];
        snprintf(uri, sizeof uri, "spotify:track:%s", s->track_id);
        pbw track = { track_buf, 0, sizeof track_buf, 0 };
        pbw_str(&track, 1, uri);
        pbw_str(&track, 6, "context");
        pbw options = { options_buf, 0, sizeof options_buf, 0 };
        pbw_bool(&options, 1, s->shuffle);
        pbw_bool(&options, 2, s->repeat == 1);
        pbw_bool(&options, 3, s->repeat == 2);

        pbw_uint(&player, 1, (uint64_t)now);
        pbw_str(&player, 2, s->context_uri);
        if (s->context_uri[0]) {
            snprintf(url, sizeof url, "context://%s", s->context_uri);
            pbw_str(&player, 3, url);
        }
        pbw_msg(&player, 7, &track);
        pbw_double(&player, 9, 1.0);            /* playback_speed */
        pbw_uint(&player, 10, s->position_ms);  /* position_as_of_timestamp */
        pbw_uint(&player, 11, s->duration_ms);
        pbw_bool(&player, 12, 1);               /* is_playing (a track is loaded) */
        pbw_bool(&player, 13, !s->playing);     /* is_paused */
        pbw_msg(&player, 16, &options);
        pbw_uint(&player, 25, s->position_ms);
    }

    pbw device = { device_buf, 0, sizeof device_buf, 0 };
    pbw_msg(&device, 1, &info);
    pbw_msg(&device, 2, &player);

    pbw req = { out, 0, cap, 0 };
    pbw_msg(&req, 2, &device);
    pbw_uint(&req, 3, 2);                       /* member_type: CONNECT_STATE */
    pbw_bool(&req, 4, s->active);               /* is_active */
    pbw_uint(&req, 5, (uint64_t)reason);
    pbw_str(&req, 7, last_command_device);
    pbw_uint(&req, 8, last_command_id);
    if (s->active && started_playing_at) {
        pbw_uint(&req, 9, (uint64_t)started_playing_at);
        pbw_uint(&req, 11, (uint64_t)(now - started_playing_at));
    }
    pbw_uint(&req, 12, (uint64_t)now);          /* client_side_timestamp */
    return req.overflow ? 0 : req.len;
}

/* Publishes the state; NEW_DEVICE once per connection, then
 * PLAYER_STATE_CHANGED. Returns the HTTP status. */
static int put_state(int reason)
{
    static uint8_t body[4096], cluster[512];
    connect_status s = status_now;
    if (s.active && !status_sent.active) started_playing_at = now_ms();
    size_t len = build_state(body, sizeof body, reason, &s);
    if (!len) return -1;
    size_t reply_len = 0;
    int st = spotify_sp_put_state(session->client_token, session->access_token,
                                  SPOTIFY_DEVICE_ID, connection_id, body, len, cluster,
                                  sizeof cluster, &reply_len);
    if (st == 200) {
        status_sent = s;
        sent_at_ms = now_ms();
    } else {
        log_printf("CONNECT: state update HTTP %d\n", st);
    }
    return st;
}

/* True when the state changed enough for the apps to need an update. */
static int status_changed(void)
{
    const connect_status *a = &status_now, *b = &status_sent;
    if (a->active != b->active || a->playing != b->playing || a->shuffle != b->shuffle ||
        a->repeat != b->repeat || strcmp(a->track_id, b->track_id) ||
        strcmp(a->context_uri, b->context_uri))
        return 1;
    if (a->volume / 5 != b->volume / 5) return 1;
    /* A seek: the position moved away from where playing would put it. */
    long long expected = (long long)b->position_ms +
                         (b->playing ? now_ms() - sent_at_ms : 0);
    long long drift = (long long)a->position_ms - expected;
    return a->active && (drift > 3000 || drift < -3000);
}

/* ------------------------------------------------------------ commands */

static void post(const connect_command *c)
{
    while (mailbox_full && !quit) sceKernelDelayThread(10000);
    mailbox = *c;
    mailbox_full = 1;
}

/* Loads a context's songs (with names) for CONNECT_PLAY. */
static int load_context(const char *uri)
{
    while (play_tracks_busy && !quit) sceKernelDelayThread(10000);
    memset(play_tracks, 0, sizeof play_tracks);
    int n;
    size_t len = strlen(uri);
    if (len > 11 && !strcmp(uri + len - 11, ":collection"))
        n = spotify_sp_tracks(session->client_token, session->access_token,
                              session->cfg.username, "", play_tracks, MAX_CONNECT_TRACKS);
    else if (!strncmp(uri, "spotify:playlist:", 17))
        n = spotify_sp_tracks(session->client_token, session->access_token,
                              session->cfg.username, uri + 17, play_tracks, MAX_CONNECT_TRACKS);
    else
        n = spotify_sp_context(session->client_token, session->access_token, uri, NULL, 0,
                               play_tracks, MAX_CONNECT_TRACKS);
    if (n < 0) n = 0;
    for (int i = 0; i < n && !quit; i += SPOTIFY_DETAILS_BATCH) {
        int batch = n - i < SPOTIFY_DETAILS_BATCH ? n - i : SPOTIFY_DETAILS_BATCH;
        spotify_sp_track_details(session->client_token, session->access_token, &play_tracks[i],
                                 batch);
    }
    return n;
}

static int index_of(const char *track_uri, int count)
{
    if (strncmp(track_uri, "spotify:track:", 14)) return -1;
    for (int i = 0; i < count; ++i)
        if (!strcmp(play_tracks[i].id, track_uri + 14)) return i;
    return -1;
}

static void start_context(const char *context_uri, const char *track_uri, int fallback_index,
                          int position_ms, int paused, int shuffle, int repeat)
{
    int n = load_context(context_uri);
    if (n <= 0) {
        log_printf("CONNECT: context has no playable songs\n");
        return;
    }
    int idx = index_of(track_uri, n);
    if (idx < 0) idx = fallback_index >= 0 && fallback_index < n ? fallback_index : 0;
    connect_command c;
    memset(&c, 0, sizeof c);
    c.type = CONNECT_PLAY;
    c.tracks = play_tracks;
    c.count = n;
    c.index = idx;
    c.position_ms = position_ms;
    c.paused = paused;
    c.shuffle = shuffle;
    c.repeat = repeat;
    copy_text(c.context_uri, sizeof c.context_uri, context_uri, strlen(context_uri));
    play_tracks_busy = 1;
    post(&c);
    log_printf("CONNECT: playing %d songs from index %d at %d s\n", n, idx, position_ms / 1000);
}

/* TransferState (transfer_state.proto): options 1, playback 2, session 3. */
static void handle_transfer(const uint8_t *data, size_t len)
{
    char context_uri[96] = "", track_uri[64] = "";
    int64_t timestamp = 0, position = 0;
    int paused = 0, shuffle = -1, repeat = -1;
    pb_reader r;
    pb_field f;
    pb_init(&r, data, len);
    while (pb_next(&r, &f) == 1) {
        if (f.wire != 2) continue;
        pb_reader in;
        pb_field g;
        pb_init(&in, f.data, f.len);
        if (f.field == 1) {             /* ContextPlayerOptions */
            int rc = 0, rt = 0;
            shuffle = 0;
            while (pb_next(&in, &g) == 1) {
                if (g.field == 1 && g.wire == 0) shuffle = g.varint != 0;
                if (g.field == 2 && g.wire == 0) rc = g.varint != 0;
                if (g.field == 3 && g.wire == 0) rt = g.varint != 0;
            }
            repeat = rt ? 2 : rc ? 1 : 0;
        } else if (f.field == 2) {      /* Playback */
            while (pb_next(&in, &g) == 1) {
                if (g.field == 1 && g.wire == 0) timestamp = (int64_t)g.varint;
                if (g.field == 2 && g.wire == 0) position = (int32_t)g.varint;
                if (g.field == 4 && g.wire == 0) paused = g.varint != 0;
                if (g.field == 5 && g.wire == 2) {
                    const uint8_t *uri;
                    size_t uri_len;
                    if (pb_find_bytes(g.data, g.len, 1, &uri, &uri_len) == 0)
                        copy_text(track_uri, sizeof track_uri, (const char *)uri, uri_len);
                }
            }
        } else if (f.field == 3) {      /* Session: context 2 { uri 1 } */
            const uint8_t *ctx_msg, *uri;
            size_t ctx_len, uri_len;
            if (pb_find_bytes(f.data, f.len, 2, &ctx_msg, &ctx_len) == 0 &&
                pb_find_bytes(ctx_msg, ctx_len, 1, &uri, &uri_len) == 0)
                copy_text(context_uri, sizeof context_uri, (const char *)uri, uri_len);
        }
    }
    if (!paused && timestamp > 0 && position > 0) position += now_ms() - timestamp;
    log_printf("CONNECT: transfer (%s at %d s%s)\n", context_uri[0] ? "context" : "no context",
               (int)(position / 1000), paused ? ", paused" : "");
    if (!context_uri[0] || !strcmp(context_uri, "-")) {
        if (!track_uri[0]) return;
        copy_text(context_uri, sizeof context_uri, track_uri, strlen(track_uri));
    }
    start_context(context_uri, track_uri, 0, (int)position, paused, shuffle, repeat);
}

static void handle_play(const char *json)
{
    char context_uri[96] = "", track_uri[64] = "";
    const char *ctx = strstr(json, "\"context\"");
    if (ctx) json_str(ctx, "uri", context_uri, sizeof context_uri);
    json_str(json, "track_uri", track_uri, sizeof track_uri);
    int index = (int)json_num(json, "track_index", -1);
    int seek = (int)json_num(json, "seek_to", 0);
    if (!context_uri[0] && track_uri[0])
        copy_text(context_uri, sizeof context_uri, track_uri, strlen(track_uri));
    if (!context_uri[0]) return;
    start_context(context_uri, track_uri, index, seek, 0, -1, -1);
}

/* A player command; returns 1 when handled. */
static int handle_command(const char *json)
{
    char endpoint[48];
    if (json_str(json, "endpoint", endpoint, sizeof endpoint) < 0) return 0;
    json_str(json, "sent_by_device_id", last_command_device, sizeof last_command_device);
    last_command_id = (unsigned int)json_num(json, "message_id", 0);
    log_printf("CONNECT: command %s\n", endpoint);

    connect_command c;
    memset(&c, 0, sizeof c);
    c.shuffle = c.repeat = -1;
    if (!strcmp(endpoint, "transfer")) {
        size_t len;
        const char *data = json_str_span(json, "data", &len);
        if (!data) return 0;
        uint8_t *raw = malloc(len * 3 / 4 + 4);
        if (!raw) return 0;
        size_t n = base64_decode(data, len, raw, len * 3 / 4 + 4);
        handle_transfer(raw, n);
        free(raw);
        return 1;
    }
    if (!strcmp(endpoint, "play")) {
        handle_play(json);
        return 1;
    }
    if (!strcmp(endpoint, "pause")) c.type = CONNECT_PAUSE;
    else if (!strcmp(endpoint, "resume")) c.type = CONNECT_RESUME;
    else if (!strcmp(endpoint, "skip_next")) c.type = CONNECT_NEXT;
    else if (!strcmp(endpoint, "skip_prev")) c.type = CONNECT_PREV;
    else if (!strcmp(endpoint, "seek_to")) {
        c.type = CONNECT_SEEK;
        c.value = (int)json_num(json, "value", 0);
    } else if (!strcmp(endpoint, "set_shuffling_context")) {
        c.type = CONNECT_SHUFFLE;
        c.value = (int)json_num(json, "value", 0);
    } else if (!strcmp(endpoint, "set_repeating_context")) {
        c.type = CONNECT_REPEAT_CONTEXT;
        c.value = (int)json_num(json, "value", 0);
    } else if (!strcmp(endpoint, "set_repeating_track")) {
        c.type = CONNECT_REPEAT_TRACK;
        c.value = (int)json_num(json, "value", 0);
    } else if (!strcmp(endpoint, "set_options")) {
        long long v = json_num(json, "shuffling_context", -1);
        if (v >= 0) {
            c.type = CONNECT_SHUFFLE;
            c.value = (int)v;
            post(&c);
        }
        v = json_num(json, "repeating_track", -1);
        if (v > 0) {
            c.type = CONNECT_REPEAT_TRACK;
            c.value = 1;
            post(&c);
            return 1;
        }
        v = json_num(json, "repeating_context", -1);
        if (v >= 0) {
            c.type = CONNECT_REPEAT_CONTEXT;
            c.value = (int)v;
            post(&c);
        }
        return 1;
    } else {
        return 0;
    }
    post(&c);
    return 1;
}

/* ------------------------------------------------------------ websocket */

static int ws_send(int opcode, const void *data, size_t len)
{
    static uint8_t frame[4096];
    if (len + 14 > sizeof frame) return -1;
    size_t h = 0;
    frame[h++] = (uint8_t)(0x80 | opcode);
    if (len < 126) {
        frame[h++] = (uint8_t)(0x80 | len);
    } else {
        frame[h++] = 0x80 | 126;
        frame[h++] = (uint8_t)(len >> 8);
        frame[h++] = (uint8_t)len;
    }
    const unsigned int mask = next_random();
    uint8_t m[4] = { (uint8_t)(mask >> 24), (uint8_t)(mask >> 16), (uint8_t)(mask >> 8),
                     (uint8_t)mask };
    memcpy(frame + h, m, 4);
    h += 4;
    const uint8_t *in = data;
    for (size_t i = 0; i < len; ++i) frame[h + i] = in[i] ^ m[i & 3];
    return tls_write_all(ws, frame, h + len);
}

static void reply(const char *key, int success)
{
    char msg[384];
    int n = snprintf(msg, sizeof msg,
                     "{\"type\":\"reply\",\"key\":\"%s\",\"payload\":{\"success\":%s}}", key,
                     success ? "true" : "false");
    if (n > 0 && (size_t)n < sizeof msg) ws_send(1, msg, (size_t)n);
}

/* ClusterUpdate { cluster 1 { active_device_id 2 } }: another device took
 * over when it is active and not us. */
static void handle_cluster(const uint8_t *data, size_t len)
{
    const uint8_t *cluster, *active;
    size_t cluster_len, active_len;
    if (pb_find_bytes(data, len, 1, &cluster, &cluster_len) < 0) return;
    if (pb_find_bytes(cluster, cluster_len, 2, &active, &active_len) < 0 || active_len == 0)
        return;
    const int us = active_len == strlen(SPOTIFY_DEVICE_ID) &&
                   !memcmp(active, SPOTIFY_DEVICE_ID, active_len);
    if (!us && status_now.active && status_now.playing) {
        connect_command c;
        memset(&c, 0, sizeof c);
        c.type = CONNECT_RELEASE;
        post(&c);
        log_printf("CONNECT: another device took over\n");
    }
}

static void handle_message(char *json)
{
    char type[16];
    if (json_str(json, "type", type, sizeof type) < 0) return;
    if (!strcmp(type, "request")) {
        char key[256], ident[96] = "";
        json_str(json, "key", key, sizeof key);
        json_str(json, "message_ident", ident, sizeof ident);
        size_t len = 0;
        const char *b64 = json_str_span(json, "compressed", &len);
        int ok = 0, size = -1;
        uint8_t *payload = NULL;
        if (b64 && (size = decode_payload(b64, len, &payload)) > 0) {
            ok = handle_command((const char *)payload);
            if (!ok) log_printf("CONNECT: unhandled request: %.160s\n", (const char *)payload);
            free(payload);
        } else if (!b64) {
            /* Without gzip support the command comes as plain JSON:
             * "payload":{"message_id":..,"command":{"endpoint":..}}. */
            const char *plain = strstr(json, "\"payload\"");
            if (plain) {
                size = (int)strlen(plain);
                ok = handle_command(plain);
                if (!ok) log_printf("CONNECT: unhandled request: %.160s\n", plain);
            } else {
                log_printf("CONNECT: request without payload: %.200s\n", json);
            }
        }
        log_printf("CONNECT: request %s (%u chars, %d bytes decoded), reply %s\n", ident,
                   (unsigned int)len, size, ok ? "ok" : "failed");
        reply(key, ok);
        return;
    }
    char uri[160] = "";
    json_str(json, "uri", uri, sizeof uri);
    if (strcmp(type, "message") != 0) {
        if (strcmp(type, "pong") != 0)
            log_printf("CONNECT: dealer %s %s (%u bytes)\n", type, uri,
                       (unsigned int)strlen(json));
        return;
    }
    log_printf("CONNECT: message %s (%u bytes)\n", uri, (unsigned int)strlen(json));
    if (!strncmp(uri, "hm://pusher/v1/connections/", 27)) {
        if (json_str(json, "Spotify-Connection-Id", connection_id, sizeof connection_id) > 0) {
            int st = put_state(3);      /* NEW_DEVICE */
            online = st == 200;
            log_printf("CONNECT: %s as \"%s\"\n", online ? "online" : "registration failed",
                       device_name);
        }
        return;
    }
    const char *payloads = strstr(json, "\"payloads\"");
    if (!payloads) return;
    /* The first string in the "payloads" array. */
    const char *b64 = strchr(payloads + 10, '[');
    b64 = b64 ? strchr(b64, '"') : NULL;
    if (!b64) return;
    ++b64;
    const char *end = strchr(b64, '"');
    if (!end) return;
    uint8_t *payload = NULL;
    int n = decode_payload(b64, (size_t)(end - b64), &payload);
    if (n <= 0) return;
    if (!strncmp(uri, "hm://connect-state/v1/cluster", 29)) {
        handle_cluster(payload, (size_t)n);
    } else if (!strncmp(uri, "hm://connect-state/v1/connect/volume", 36)) {
        /* SetVolumeCommand { volume 1 } in 0..65535 */
        pb_reader r;
        pb_field f;
        pb_init(&r, payload, (size_t)n);
        while (pb_next(&r, &f) == 1)
            if (f.field == 1 && f.wire == 0) {
                connect_command c;
                memset(&c, 0, sizeof c);
                c.type = CONNECT_VOLUME;
                c.value = (int)(f.varint * 100u / 65535u);
                post(&c);
            }
    }
    free(payload);
}

/* One dealer connection, until it drops or the app quits. */
static void run_dealer(void)
{
    static uint8_t rx[RX_CAP];
    static char msg[MSG_CAP + 1];
    online = 0;
    connection_id[0] = '\0';
    ws_sock = http_connect(DEALER_HOST, 443);
    if (ws_sock < 0) return;
    ws = tls_open(ws_sock, DEALER_HOST);
    if (!ws) {
        sceNetInetClose(ws_sock);
        return;
    }
    uint8_t nonce[16];
    for (int i = 0; i < 16; ++i) nonce[i] = (uint8_t)next_random();
    char key[32];
    base64_encode(nonce, sizeof nonce, key);
    static char request[1536];
    int n = snprintf(request, sizeof request,
                     "GET /?access_token=%s HTTP/1.1\r\nHost: " DEALER_HOST "\r\n"
                     "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                     "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n",
                     session->access_token, key);
    size_t rx_len = 0;
    if (n <= 0 || (size_t)n >= sizeof request || tls_write_all(ws, request, (size_t)n) < 0)
        goto done;
    /* Upgrade response headers. */
    for (int waited = 0; waited < 50; ) {
        int r = tls_read_wait(ws, rx + rx_len, sizeof rx - rx_len - 1, 200);
        if (r < 0) goto done;
        if (r == 0) { ++waited; continue; }
        rx_len += (size_t)r;
        rx[rx_len] = 0;
        char *end = strstr((char *)rx, "\r\n\r\n");
        if (!end) continue;
        if (strncmp((char *)rx, "HTTP/1.1 101", 12) != 0) {
            log_printf("CONNECT: dealer refused the websocket\n");
            goto done;
        }
        size_t used = (size_t)(end + 4 - (char *)rx);
        memmove(rx, rx + used, rx_len - used);
        rx_len -= used;
        break;
    }

    SceInt64 last_ping = sceKernelGetSystemTimeWide();
    size_t msg_len = 0, skip = 0;
    while (!quit) {
        int r = tls_read_wait(ws, rx + rx_len, sizeof rx - rx_len, 200);
        if (r < 0) break;
        rx_len += (size_t)r;
        /* Frames that do not fit are skipped (huge cluster pushes). */
        if (skip) {
            size_t k = skip < rx_len ? skip : rx_len;
            memmove(rx, rx + k, rx_len - k);
            rx_len -= k;
            skip -= k;
        }
        while (!skip && rx_len >= 2) {
            const int fin = rx[0] & 0x80, op = rx[0] & 0x0F;
            uint64_t plen = rx[1] & 0x7F;
            size_t pos = 2;
            if (plen == 126) {
                if (rx_len < 4) break;
                plen = ((uint64_t)rx[2] << 8) | rx[3];
                pos = 4;
            } else if (plen == 127) {
                if (rx_len < 10) break;
                plen = 0;
                for (int i = 0; i < 8; ++i) plen = (plen << 8) | rx[2 + i];
                pos = 10;
            }
            if (rx[1] & 0x80) pos += 4;
            if (plen > RX_CAP - 16 || msg_len + plen > MSG_CAP) {
                log_printf("CONNECT: skipping a %u-byte frame\n", (unsigned int)plen);
                /* Drop this frame (and the message it belongs to). */
                skip = (size_t)plen + pos;
                msg_len = 0;
                size_t k = skip < rx_len ? skip : rx_len;
                memmove(rx, rx + k, rx_len - k);
                rx_len -= k;
                skip -= k;
                continue;
            }
            if (rx_len < pos + plen) break;
            const uint8_t *payload = rx + pos;
            if (op == 0 || op == 1 || op == 2) {
                memcpy(msg + msg_len, payload, (size_t)plen);
                msg_len += (size_t)plen;
                if (fin) {
                    msg[msg_len] = '\0';
                    handle_message(msg);
                    msg_len = 0;
                }
            } else if (op == 8) {
                goto done;      /* close: the thread reconnects */
            } else if (op == 9) {
                ws_send(10, payload, (size_t)plen);
            }
            const size_t used = pos + (size_t)plen;
            memmove(rx, rx + used, rx_len - used);
            rx_len -= used;
        }
        SceInt64 now = sceKernelGetSystemTimeWide();
        if (now - last_ping > PING_US) {
            if (ws_send(9, NULL, 0) < 0) break;
            last_ping = now;
        }
        /* Publish what the UI plays, at most twice a second. */
        if (online && status_seq != status_sent_seq && now_ms() - sent_at_ms > 500) {
            status_sent_seq = status_seq;
            if (status_changed()) put_state(4);     /* PLAYER_STATE_CHANGED */
        }
    }
done:
    online = 0;
    tls_close(ws);
    ws = NULL;
    sceNetInetClose(ws_sock);
    ws_sock = -1;
}

static int connect_thread(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    int backoff = 2;
    while (!quit) {
        SceInt64 started = sceKernelGetSystemTimeWide();
        run_dealer();
        if (quit) break;
        /* A connection that lasted a while is a normal drop: retry soon. */
        if (sceKernelGetSystemTimeWide() - started > 60ll * 1000 * 1000) backoff = 2;
        log_printf("CONNECT: dealer connection closed, retrying in %d s\n", backoff);
        for (int i = 0; i < backoff * 10 && !quit; ++i) sceKernelDelayThread(100000);
        if (backoff < 60) backoff *= 2;
    }
    return 0;
}

/* ------------------------------------------------------------ public */

void connect_start(spotify_ctx *ctx, const char *name)
{
    if (thread >= 0) return;
    session = ctx;
    copy_text(device_name, sizeof device_name, name, strlen(name));
    quit = 0;
    thread = sceKernelCreateThread("connect", connect_thread, 0x30, 0x10000, 0, NULL);
    if (thread >= 0) sceKernelStartThread(thread, 0, NULL);
}

void connect_stop(void)
{
    if (thread < 0) return;
    quit = 1;
    mailbox_full = 0;
    play_tracks_busy = 0;
    SceUInt timeout = 5u * 1000u * 1000u;
    if (sceKernelWaitThreadEnd(thread, &timeout) < 0) sceKernelTerminateThread(thread);
    sceKernelDeleteThread(thread);
    thread = -1;
    online = 0;
}

int connect_online(void)
{
    return online;
}

int connect_poll(connect_command *out)
{
    if (!mailbox_full) return 0;
    *out = mailbox;
    mailbox_full = 0;
    return 1;
}

void connect_command_done(void)
{
    play_tracks_busy = 0;
}

void connect_report(const connect_status *s)
{
    if (memcmp(&status_now, s, sizeof *s) == 0) return;
    status_now = *s;
    ++status_seq;
}
