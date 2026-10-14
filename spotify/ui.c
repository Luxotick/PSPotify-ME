#include "ui.h"
#include "../ui/draw.h"
#include "../ui/font.h"
#include "../ui/icons.h"
#include "../ui/palette.h"
#include "../ui/ui.h"
#include "config.h"
#include "http.h"
#include "log.h"
#include "memory.h"
#include "player.h"
#include "render_gu.h"
#include "session.h"
#include "webapi.h"
#include <pspaudio.h>
#include <pspctrl.h>
#include <pspiofilemgr.h>
#include <psppower.h>
#include <psprtc.h>
#include <psputility.h>
#include <psputility_osk.h>
#include <pspthreadman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "stb_image.h"

/* Subset fonts linked in by scripts/ui_assets.py. */
extern const unsigned char ui_font_regular[], ui_font_bold[], ui_font_xbold[], ui_font_icons[];
extern const unsigned int ui_font_regular_len, ui_font_bold_len, ui_font_xbold_len,
    ui_font_icons_len;

#define MAX_PLAYLISTS 100
#define MAX_TRACKS 500
#define MAX_THEMES 16
#define COVER 128

typedef enum { PS_IDLE, PS_LOADING, PS_PLAYING, PS_ERROR } play_state_t;

static spotify_ctx ctx;
static ui_state xmb;

/* ------------------------------------------------------------ library */

/* playlists[0] is unused (Liked Songs has its own category). */
static spotify_playlist playlists[MAX_PLAYLISTS + 1];
static int playlist_count;
static ui_row playlist_rows[MAX_PLAYLISTS];

/* Lists load on the loader thread so the interface keeps moving. The UI
 * thread only reads a buffer after the loader marked it done. */
static spotify_track liked[MAX_TRACKS], opened[MAX_TRACKS];
static ui_row liked_rows[MAX_TRACKS], opened_rows[MAX_TRACKS];
static volatile int liked_done, liked_count, liked_shown;
static volatile unsigned open_req_seq, open_done_seq, open_shown_seq;
static volatile int open_req_playlist, open_done_playlist, open_done_count;

/* Search results, loaded the same way. */
#define MAX_RESULTS 50
static spotify_track found[MAX_RESULTS];
static ui_row found_rows[MAX_RESULTS];
static char search_text[128];
static volatile unsigned search_req_seq, search_done_seq, search_shown_seq;
static volatile int search_done_count;

/* ------------------------------------------------------------ playback */

/* Play queue: a copy of the list a track was started from, so browsing
 * does not change what plays next. */
static spotify_track queue[MAX_TRACKS];
static volatile int queue_len;
static int queue_key = -2;          /* -1 Liked Songs, else a playlist index */
static char queue_name[96];

static player_control control;
static volatile int request_index = -1;
static volatile int current_index = -1;
static volatile play_state_t play_state = PS_IDLE;
static volatile int player_quit;
static volatile int me_dead;

/* ------------------------------------------------------------ cover */

/* The cover thread downloads and decodes the playing track's cover, scales
 * it to COVER x COVER and finds its palette; the UI thread turns it into a
 * texture. A slow i.scdn.co response must never block the UI. */
static spotify_track cover_request;
static volatile unsigned cover_request_seq, cover_done_seq;
static uint32_t cover_argb[COVER * COVER];
static cover_palette cover_colors;
static char cover_id[24];
static volatile int cover_pending;
static r_tex *cover_tex;
static char cover_tex_id[24];

/* ------------------------------------------------------------ themes */

static char theme_names[MAX_THEMES][32];
static uint32_t theme_swatch[MAX_THEMES];   /* accent, shown in Settings */
static int theme_count, theme_active = -1;
static ui_row settings_rows[MAX_THEMES + 1];
static ui_theme base_theme;
static int cover_colors_on = 1;
static int have_palette;

/* ------------------------------------------------------------ helpers */

/* Copies src into dst (cap bytes), cutting it if needed. */
static void copy_text(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static uint32_t row_color(const char *key)
{
    static const uint32_t colors[] = {
        0xFF3B4FD8u, 0xFF0F766Eu, 0xFFB45309u, 0xFF9D174Du,
        0xFF6D28D9u, 0xFF15803Du, 0xFF0E7490u, 0xFFA16207u,
    };
    unsigned h = 2166136261u;
    for (; *key; ++key) h = (h ^ (unsigned char)*key) * 16777619u;
    return colors[h % (sizeof colors / sizeof colors[0])];
}

static void path_in_app(char *out, size_t cap, const char *name)
{
    snprintf(out, cap, "%s/%s", spotify_config_dir(), name);
}

static void frame_message(const char *line1, const char *line2)
{
    const ui_theme *t = &xmb.theme;
    render_gu_begin();
    r_rect(0, 0, 480, 272, t->bg);
    font_draw(FONT_XBOLD, 20, 24, 18, t->accent, "PSPotify ME");
    float w = font_width(FONT_BOLD, 15, line1);
    font_draw(FONT_BOLD, 15, (480 - w) / 2, 118, t->text, line1);
    if (line2) {
        w = font_width(FONT_REGULAR, 12, line2);
        font_draw(FONT_REGULAR, 12, (480 - w) / 2, 142, t->dim, line2);
    }
    render_gu_end();
}

static void on_status(const char *message)
{
    frame_message(message, NULL);
}

static void on_pair_code(const char *code)
{
    const ui_theme *t = &xmb.theme;
    render_gu_begin();
    r_rect(0, 0, 480, 272, t->bg);
    font_draw(FONT_XBOLD, 20, 24, 18, t->accent, "PSPotify ME");
    static const char *const lines[] = {
        "Pair this PSP with your Spotify account",
        "On a phone or computer, open spotify.com/pair and enter:",
    };
    float w = font_width(FONT_BOLD, 15, lines[0]);
    font_draw(FONT_BOLD, 15, (480 - w) / 2, 70, t->text, lines[0]);
    w = font_width(FONT_REGULAR, 12, lines[1]);
    font_draw(FONT_REGULAR, 12, (480 - w) / 2, 96, t->dim, lines[1]);

    /* One rounded box per character of the code. */
    const int n = (int)strlen(code), box = 34, gap = 8;
    float x = (480.0f - (float)(n * box + (n - 1) * gap)) / 2;
    for (int i = 0; i < n; ++i, x += box + gap) {
        char ch[2] = { code[i], 0 };
        draw_round_rect(x, 128, (float)box, 44, 8, t->surface);
        float cw = font_width(FONT_XBOLD, 20, ch);
        font_draw(FONT_XBOLD, 20, x + (box - cw) / 2, 137, t->text, ch);
    }
    w = font_width(FONT_REGULAR, 12, "Waiting for approval\xE2\x80\xA6 (only once)");
    font_draw(FONT_REGULAR, 12, (480 - w) / 2, 196, t->dim,
              "Waiting for approval\xE2\x80\xA6 (only once)");
    render_gu_end();
}

/* ------------------------------------------------------------ themes */

static void theme_file(int i, char *out, size_t cap)
{
    char rel[64];
    snprintf(rel, sizeof rel, "THEMES/%s/theme.ini", theme_names[i]);
    path_in_app(out, cap, rel);
}

static int read_theme(int i, ui_theme *t)
{
    char path[320];
    theme_file(i, path, sizeof path);
    theme_defaults(t);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    static char text[4096];
    size_t len = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[len] = 0;
    int ignored = theme_parse(t, text, len);
    if (ignored) log_printf("UI: theme %s: %d line(s) ignored\n", theme_names[i], ignored);
    return 0;
}

static void scan_themes(void)
{
    char dir[300];
    path_in_app(dir, sizeof dir, "THEMES");
    theme_count = 0;
    SceUID d = sceIoDopen(dir);
    if (d < 0) {
        log_printf("UI: no THEMES folder (%s), using the built-in theme\n", dir);
        return;
    }
    SceIoDirent e;
    memset(&e, 0, sizeof e);
    while (sceIoDread(d, &e) > 0 && theme_count < MAX_THEMES) {
        if (FIO_S_ISDIR(e.d_stat.st_mode) && e.d_name[0] != '.')
            copy_text(theme_names[theme_count++], sizeof theme_names[0], e.d_name);
        memset(&e, 0, sizeof e);
    }
    sceIoDclose(d);
    for (int i = 1; i < theme_count; ++i)
        for (int j = i; j > 0 && strcmp(theme_names[j - 1], theme_names[j]) > 0; --j) {
            char tmp[32];
            memcpy(tmp, theme_names[j], sizeof tmp);
            memcpy(theme_names[j], theme_names[j - 1], sizeof tmp);
            memcpy(theme_names[j - 1], tmp, sizeof tmp);
        }
    for (int i = 0; i < theme_count; ++i) {
        ui_theme t;
        read_theme(i, &t);
        theme_swatch[i] = t.accent;
    }
    log_printf("UI: %d themes\n", theme_count);
}

static void retint(int instant)
{
    ui_theme target = base_theme;
    if (cover_colors_on && have_palette) theme_tint(&base_theme, &cover_colors, &target);
    xmb.theme_target = target;
    if (instant) xmb.theme = target;
}

static void fill_settings(void)
{
    ui_row *r = &settings_rows[0];
    memset(r, 0, sizeof *r);
    snprintf(r->title, sizeof r->title, "Cover colors");
    snprintf(r->subtitle, sizeof r->subtitle, cover_colors_on
             ? "On \xC2\xB7 the theme follows the album art"
             : "Off \xC2\xB7 the theme keeps its own colors");
    r->color = cover_colors_on ? xmb.theme_target.accent : 0xFF475569u;
    for (int k = 0; k < theme_count; ++k) {
        r = &settings_rows[k + 1];
        memset(r, 0, sizeof *r);
        copy_text(r->title, sizeof r->title, theme_names[k]);
        snprintf(r->subtitle, sizeof r->subtitle,
                 k == theme_active ? "Theme \xC2\xB7 active" : "Theme");
        r->color = theme_swatch[k];
        r->playing = k == theme_active;
    }
    xmb.lists[UI_CAT_SETTINGS][0].rows = settings_rows;
    xmb.lists[UI_CAT_SETTINGS][0].count = theme_count + 1;
}

static void save_settings(void)
{
    char path[300];
    path_in_app(path, sizeof path, "settings.ini");
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "# PSPotify ME settings\ntheme = %s\ncover_colors = %s\n",
            theme_active >= 0 ? theme_names[theme_active] : "", cover_colors_on ? "on" : "off");
    fclose(f);
}

static void apply_theme(int i, int instant)
{
    theme_defaults(&base_theme);
    if (i >= 0 && i < theme_count) read_theme(i, &base_theme);
    theme_active = i;
    retint(instant);
    fill_settings();
}

static void load_settings(void)
{
    char path[300], line[128], wanted[32] = "Midnight";
    path_in_app(path, sizeof path, "settings.ini");
    FILE *f = fopen(path, "r");
    if (f) {
        while (fgets(line, sizeof line, f)) {
            char key[32], value[64];
            if (sscanf(line, " %31[a-z_] = %63[^\r\n]", key, value) != 2) continue;
            if (!strcmp(key, "theme")) copy_text(wanted, sizeof wanted, value);
            else if (!strcmp(key, "cover_colors")) cover_colors_on = strcmp(value, "off") != 0;
        }
        fclose(f);
    }
    scan_themes();
    int active = theme_count ? 0 : -1;
    for (int i = 0; i < theme_count; ++i)
        if (!strcmp(theme_names[i], wanted)) active = i;
    apply_theme(active, 1);
}

/* ------------------------------------------------------------ loader thread */

static int load_list(const char *playlist_id, spotify_track *out)
{
    memset(out, 0, sizeof(spotify_track) * MAX_TRACKS);
    int n = spotify_sp_tracks(ctx.client_token, ctx.access_token, ctx.cfg.username,
                              playlist_id, out, MAX_TRACKS);
    if (n < 0) n = 0;
    /* Names come from batched metadata lookups. */
    for (int i = 0; i < n && !player_quit; i += SPOTIFY_DETAILS_BATCH) {
        int batch = n - i < SPOTIFY_DETAILS_BATCH ? n - i : SPOTIFY_DETAILS_BATCH;
        spotify_sp_track_details(ctx.client_token, ctx.access_token, &out[i], batch);
    }
    return n;
}

static int loader_thread(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    while (!player_quit) {
        if (!liked_done) {
            liked_count = load_list("", liked);
            liked_done = 1;
            log_printf("UI: %d liked songs\n", liked_count);
            continue;
        }
        unsigned search_seq = search_req_seq;
        if (search_seq != search_done_seq) {
            static char query[128];
            copy_text(query, sizeof query, search_text);
            memset(found, 0, sizeof found);
            int n = spotify_sp_search(ctx.client_token, ctx.access_token, query, found,
                                      MAX_RESULTS);
            if (n < 0) n = 0;
            if (n > 0) spotify_sp_track_details(ctx.client_token, ctx.access_token, found, n);
            search_done_count = n;
            search_done_seq = search_seq;
            continue;
        }
        unsigned seq = open_req_seq;
        if (seq != open_done_seq) {
            int p = open_req_playlist;
            open_done_count = load_list(playlists[p].id, opened);
            open_done_playlist = p;
            open_done_seq = seq;
            log_printf("UI: %d tracks in playlist %d\n", open_done_count, p);
            continue;
        }
        sceKernelDelayThread(30000);
    }
    return 0;
}

/* ------------------------------------------------------------ player thread */

static int player_thread(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    int failures = 0;
    while (!player_quit) {
        int idx = request_index;
        if (idx < 0 || idx >= queue_len) {
            sceKernelDelayThread(20000);
            continue;
        }
        request_index = -1;
        current_index = idx;
        log_printf("UI: player starts queue[%d] %s\n", idx, queue[idx].id);
        control.stop = 0;
        control.paused = 0;
        control.position_ms = 0;
        control.fetched_pct = 0;
        play_state = PS_LOADING;
        if (idx + 1 < queue_len) spotify_ctx_prefetch(&ctx, queue[idx + 1].id);

        int ret = spotify_ctx_play(&ctx, queue[idx].id, &control);
        log_printf("UI: track %d ended (%d)\n", idx, ret);
        if (ret == PLAYER_ERR_ME_TIMEOUT) {
            me_dead = 1;
            play_state = PS_ERROR;
            break;
        }
        if (player_quit) break;
        if (request_index >= 0) { failures = 0; continue; }   /* user picked another */
        if (ret == PLAYER_STOPPED) { play_state = PS_IDLE; continue; }
        if (ret < 0) {
            /* Unplayable track: skip ahead, but give up after a few in a row. */
            play_state = PS_ERROR;
            if (++failures >= 3) continue;
            sceKernelDelayThread(500000);
        } else {
            failures = 0;
            play_state = PS_PLAYING;
        }
        if (idx + 1 < queue_len) request_index = idx + 1;
        else play_state = PS_IDLE;
    }
    return 0;
}

static void start_track(int queue_idx)
{
    if (queue_idx < 0 || queue_idx >= queue_len) return;
    request_index = queue_idx;
    control.stop = 1;           /* ends the current track, if any */
}

static void play_from(const spotify_track *tracks, int count, int idx, int key, const char *name)
{
    if (count <= 0 || idx < 0 || idx >= count) return;
    /* Queue this list unless it is already the queue. */
    if (queue_key != key || queue_len != count) {
        control.stop = 1;
        request_index = -1;
        while (play_state == PS_LOADING && current_index >= 0 && !me_dead)
            sceKernelDelayThread(10000);
        memcpy(queue, tracks, sizeof tracks[0] * (size_t)count);
        queue_len = count;
        queue_key = key;
        copy_text(queue_name, sizeof queue_name, name);
    }
    log_printf("UI: play request %d (list %d, %d tracks)\n", idx, key, queue_len);
    start_track(idx);
}

static void set_volume(int pct)
{
    xmb.volume = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    control.volume = PSP_AUDIO_VOLUME_MAX * xmb.volume / 100;
}

/* ------------------------------------------------------------ images */

static void image_url(const uint8_t id[20], char *out, size_t cap)
{
    int n = snprintf(out, cap, "https://i.scdn.co/image/");
    for (int i = 0; i < 20 && n > 0 && (size_t)n + 3 < cap; ++i)
        n += snprintf(out + n, cap - (size_t)n, "%02x", id[i]);
}

/* Downloads a JPEG into buf and scales it down to size x size 0xAARRGGBB
 * pixels with a box filter. Called from the cover and thumbnail threads,
 * each with its own buffers. */
static int fetch_image(const char *url, uint8_t *buf, size_t cap, uint32_t *out, int size)
{
    http_stream s;
    uint64_t content_length = 0;
    int status = http_stream_open(&s, url, &content_length);
    if (status != 200) {
        if (status > 0) http_stream_close(&s);
        log_printf("UI: image HTTP %d\n", status);
        return -1;
    }
    size_t got = 0;
    for (;;) {
        int r = http_stream_read(&s, buf + got, (unsigned int)(cap - got));
        if (r <= 0) break;
        got += (size_t)r;
        if (got == cap) break;
    }
    http_stream_close(&s);

    int w = 0, h = 0, comp = 0;
    /* Bound concurrent scratch/output allocations on the 32 MB model. */
    if (spotify_device_model == 0 &&
        (got < 2 || buf[0] != 0xff || buf[1] != 0xd8 ||
         !stbi_info_from_memory(buf, (int)got, &w, &h, &comp) ||
         w <= 0 || h <= 0 || w > 512 || h > 512 || (comp != 1 && comp != 3))) {
        log_printf("UI: image skipped: PSP-1000 requires <=512x512 grayscale/RGB JPEG\n");
        return -1;
    }
    unsigned char *rgba = stbi_load_from_memory(buf, (int)got, &w, &h, &comp, 4);
    if (!rgba) {
        log_printf("UI: image decode failed (%u bytes)\n", (unsigned int)got);
        return -1;
    }
    for (int y = 0; y < size; ++y) {
        int row0 = y * h / size, row1 = (y + 1) * h / size;
        if (row1 <= row0) row1 = row0 + 1;
        for (int x = 0; x < size; ++x) {
            int col0 = x * w / size, col1 = (x + 1) * w / size;
            if (col1 <= col0) col1 = col0 + 1;
            unsigned r = 0, g = 0, b = 0, cnt = 0;
            for (int sy = row0; sy < row1; ++sy)
                for (int sx = col0; sx < col1; ++sx) {
                    const unsigned char *p = rgba + 4 * (sy * w + sx);
                    r += p[0];
                    g += p[1];
                    b += p[2];
                    ++cnt;
                }
            out[y * size + x] = 0xFF000000u | ((r / cnt) << 16) | ((g / cnt) << 8) | (b / cnt);
        }
    }
    stbi_image_free(rgba);
    return w;
}

/* ------------------------------------------------------------ cover thread */

static void load_cover(const spotify_track *t)
{
    static uint8_t jpeg[128 * 1024];
    static uint32_t scaled[COVER * COVER];
    if (!t->has_cover) return;
    char url[80];
    image_url(t->cover, url, sizeof url);
    int w = fetch_image(url, jpeg, sizeof jpeg, scaled, COVER);
    if (w < 0) return;

    /* Hand over to the UI thread once it took the previous cover. */
    while (cover_pending && !player_quit) sceKernelDelayThread(10000);
    memcpy(cover_argb, scaled, sizeof cover_argb);
    palette_extract(cover_argb, COVER * COVER, &cover_colors);
    copy_text(cover_id, sizeof cover_id, t->id);
    cover_pending = 1;
    log_printf("UI: cover %dpx\n", w);
}

static int cover_thread(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    static spotify_track job;
    while (!player_quit) {
        unsigned int seq = cover_request_seq;
        if (seq == cover_done_seq) {
            sceKernelDelayThread(50000);
            continue;
        }
        job = cover_request;
        load_cover(&job);
        cover_done_seq = seq;
    }
    return 0;
}

/* ------------------------------------------------------------ thumbnails */

/* Small covers for list rows, cached by image URL. The UI thread asks for
 * the rows on screen each frame; the thumbnail thread downloads the most
 * recently wanted one; the UI thread turns its pixels into a texture. Slots
 * not shown lately are reused. */
#define THUMB 64
#define THUMB_SLOTS 24

enum { THUMB_FREE, THUMB_WANTED, THUMB_LOADING, THUMB_READY, THUMB_FAILED };

typedef struct thumb_slot {
    char url[160];
    int state;
    r_tex *tex;
    unsigned last_used;     /* last frame the row was on screen */
    unsigned first_wanted;  /* frame it was first asked for */
    int priority;           /* rows from the selected one; lower goes first */
} thumb_slot;

/* Rows must stay on screen this many frames before their cover is fetched,
 * so scrolling past a list downloads nothing. */
#define THUMB_DWELL 12

static thumb_slot thumbs[THUMB_SLOTS];
static SceUID thumb_lock = -1;
static volatile unsigned frame_no;
static uint32_t thumb_px[THUMB * THUMB];
static volatile int thumb_px_slot = -1;     /* slot whose pixels wait for upload */

static void forget_texture(const r_tex *tex)
{
    for (int i = 0; i < MAX_PLAYLISTS; ++i)
        if (playlist_rows[i].cover == tex) playlist_rows[i].cover = NULL;
    for (int i = 0; i < MAX_TRACKS; ++i) {
        if (liked_rows[i].cover == tex) liked_rows[i].cover = NULL;
        if (opened_rows[i].cover == tex) opened_rows[i].cover = NULL;
    }
    for (int i = 0; i < MAX_RESULTS; ++i)
        if (found_rows[i].cover == tex) found_rows[i].cover = NULL;
}

/* The texture for url if it is loaded; otherwise queues it. UI thread. */
static r_tex *thumb_for(const char *url, int priority)
{
    if (!url[0] || thumb_lock < 0) return NULL;
    sceKernelWaitSema(thumb_lock, 1, NULL);
    r_tex *tex = NULL;
    int found = -1, victim = -1;
    for (int i = 0; i < THUMB_SLOTS && found < 0; ++i)
        if (thumbs[i].state != THUMB_FREE && !strcmp(thumbs[i].url, url)) found = i;
    /* Reuse a free slot, else the one unused for longest (never one being
     * downloaded, nor one drawn this or last frame). */
    for (int i = 0; i < THUMB_SLOTS && found < 0; ++i) {
        const thumb_slot *s = &thumbs[i];
        if (s->state == THUMB_FREE) {
            victim = i;
            break;
        }
        if (s->state == THUMB_LOADING || s->last_used + 1 >= frame_no) continue;
        if (victim < 0 || s->last_used < thumbs[victim].last_used) victim = i;
    }
    if (found >= 0) {
        thumbs[found].last_used = frame_no;
        thumbs[found].priority = priority;
        if (thumbs[found].state == THUMB_READY) tex = thumbs[found].tex;
    } else if (victim >= 0) {
        thumb_slot *s = &thumbs[victim];
        if (s->tex) {
            forget_texture(s->tex);
            r_tex_destroy(s->tex);
            s->tex = NULL;
        }
        copy_text(s->url, sizeof s->url, url);
        s->state = THUMB_WANTED;
        s->last_used = s->first_wanted = frame_no;
        s->priority = priority;
    }
    sceKernelSignalSema(thumb_lock, 1);
    return tex;
}

/* Turns a downloaded thumbnail into a texture. UI thread. */
static void upload_thumb(void)
{
    int slot = thumb_px_slot;
    if (slot < 0) return;
    sceKernelWaitSema(thumb_lock, 1, NULL);
    thumb_slot *s = &thumbs[slot];
    if (s->state == THUMB_LOADING) {
        s->tex = r_tex_create(THUMB, THUMB, thumb_px, 1);
        s->state = s->tex ? THUMB_READY : THUMB_FAILED;
    }
    sceKernelSignalSema(thumb_lock, 1);
    thumb_px_slot = -1;
}

static int thumb_thread(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    static uint8_t jpeg[64 * 1024];
    static uint32_t scaled[THUMB * THUMB];
    static char url[160];
    while (!player_quit) {
        int slot = -1;
        sceKernelWaitSema(thumb_lock, 1, NULL);
        /* Only rows on screen now that have stayed there a moment; the one
         * nearest the selection first. */
        const unsigned now = frame_no;
        for (int i = 0; i < THUMB_SLOTS; ++i) {
            const thumb_slot *s = &thumbs[i];
            if (s->state != THUMB_WANTED || s->last_used + 2 < now ||
                now - s->first_wanted < THUMB_DWELL)
                continue;
            if (slot < 0 || s->priority < thumbs[slot].priority) slot = i;
        }
        if (slot >= 0) {
            thumbs[slot].state = THUMB_LOADING;
            copy_text(url, sizeof url, thumbs[slot].url);
        }
        sceKernelSignalSema(thumb_lock, 1);
        if (slot < 0) {
            sceKernelDelayThread(30000);
            continue;
        }
        if (fetch_image(url, jpeg, sizeof jpeg, scaled, THUMB) < 0) {
            sceKernelWaitSema(thumb_lock, 1, NULL);
            thumbs[slot].state = THUMB_FAILED;
            sceKernelSignalSema(thumb_lock, 1);
            continue;
        }
        while (thumb_px_slot >= 0 && !player_quit) sceKernelDelayThread(5000);
        memcpy(thumb_px, scaled, sizeof thumb_px);
        thumb_px_slot = slot;
    }
    return 0;
}

/* Points the rows around the visible part of the current lists at their
 * thumbnails, asking for the missing ones. UI thread, once per frame. */
static void assign_thumbs(void)
{
    ++frame_no;
    const int cat = xmb.cat;
    if (cat != UI_CAT_LIKED && cat != UI_CAT_PLAYLISTS && cat != UI_CAT_SEARCH) return;
    for (int lvl = 0; lvl < 2; ++lvl) {
        ui_list *list = &xmb.lists[cat][lvl];
        if (!list->rows || list->count <= 0) continue;
        if (lvl != xmb.level[cat] && (xmb.level_pos[cat] < 0.02f || xmb.level_pos[cat] > 0.98f))
            continue;
        /* The rows on screen: one above the selection and about three below. */
        int center = (int)(xmb.scroll[cat][lvl] + 0.5f);
        for (int i = center - 1; i <= center + 3; ++i) {
            if (i < 0 || i >= list->count) continue;
            char url[160];
            url[0] = '\0';
            if (cat == UI_CAT_PLAYLISTS && lvl == 0) {
                copy_text(url, sizeof url, playlists[i + 1].image);
            } else {
                const spotify_track *t = cat == UI_CAT_LIKED ? &liked[i]
                                       : cat == UI_CAT_SEARCH ? &found[i] : &opened[i];
                if (t->has_thumb) image_url(t->thumb, url, sizeof url);
            }
            list->rows[i].cover = thumb_for(url, i > center ? i - center : center - i);
        }
    }
}

/* ------------------------------------------------------------ UI thread */

static void track_rows(const spotify_track *tracks, int count, ui_row *rows)
{
    for (int i = 0; i < count; ++i) {
        ui_row *r = &rows[i];
        memset(r, 0, sizeof *r);
        copy_text(r->title, sizeof r->title, tracks[i].name[0] ? tracks[i].name : tracks[i].id);
        copy_text(r->subtitle, sizeof r->subtitle, tracks[i].artist);
        r->color = row_color(tracks[i].id);
    }
}

static void mark_playing(const char *id)
{
    if (liked_shown)
        for (int i = 0; i < liked_count; ++i) liked_rows[i].playing = id && !strcmp(liked[i].id, id);
    ui_list *open_list = &xmb.lists[UI_CAT_PLAYLISTS][1];
    if (open_shown_seq == open_done_seq && open_list->count > 0)
        for (int i = 0; i < open_list->count; ++i)
            opened_rows[i].playing = id && !strcmp(opened[i].id, id);
    ui_list *results = &xmb.lists[UI_CAT_SEARCH][0];
    if (search_shown_seq == search_done_seq)
        for (int i = 0; i < results->count; ++i)
            found_rows[i].playing = id && !strcmp(found[i].id, id);
}

/* Picks up finished list loads and covers, and mirrors the player state. */
static void sync_state(void)
{
    if (liked_done && !liked_shown) {
        track_rows(liked, liked_count, liked_rows);
        xmb.lists[UI_CAT_LIKED][0].rows = liked_rows;
        xmb.lists[UI_CAT_LIKED][0].count = liked_count;
        xmb.lists[UI_CAT_LIKED][0].loading = 0;
        liked_shown = 1;
    }
    ui_list *open_list = &xmb.lists[UI_CAT_PLAYLISTS][1];
    if (open_done_seq == open_req_seq && open_shown_seq != open_done_seq) {
        track_rows(opened, open_done_count, opened_rows);
        open_list->rows = opened_rows;
        open_list->count = open_done_count;
        open_list->loading = 0;
        open_shown_seq = open_done_seq;
    }

    ui_list *results = &xmb.lists[UI_CAT_SEARCH][0];
    if (search_done_seq == search_req_seq && search_shown_seq != search_done_seq) {
        track_rows(found, search_done_count, found_rows);
        results->rows = found_rows;
        results->count = search_done_count;
        results->loading = 0;
        xmb.sel[UI_CAT_SEARCH][0] = 0;
        xmb.scroll[UI_CAT_SEARCH][0] = 0;
        search_shown_seq = search_done_seq;
        int playing = current_index;
        if (playing >= 0 && playing < queue_len) mark_playing(queue[playing].id);
    }

    static int shown_index = -2;
    static char shown_id[24];
    int idx = current_index;
    ui_now *n = &xmb.now;
    if (idx >= 0 && idx < queue_len) {
        const spotify_track *t = &queue[idx];
        if (idx != shown_index || strcmp(shown_id, t->id) != 0) {
            shown_index = idx;
            copy_text(shown_id, sizeof shown_id, t->id);
            copy_text(n->title, sizeof n->title, t->name);
            copy_text(n->artist, sizeof n->artist, t->artist);
            n->color = row_color(t->id);
            n->cover = NULL;
            mark_playing(t->id);
            /* New track: fetch its cover on the cover thread. */
            cover_request = *t;
            ++cover_request_seq;
        }
        n->active = 1;
        n->dur_ms = t->duration_ms;
        n->pos_ms = control.position_ms;
        n->paused = control.paused;
        n->loading = play_state == PS_LOADING && control.position_ms == 0;
        if (play_state == PS_ERROR)
            snprintf(n->album, sizeof n->album, "Couldn't play this track (Spotify Premium needed?)");
        else
            snprintf(n->album, sizeof n->album, "From %s", queue_name);
        copy_text(n->next, sizeof n->next, idx + 1 < queue_len ? queue[idx + 1].name : "");
    }

    if (cover_pending) {
        if (!strcmp(cover_id, shown_id)) {
            r_tex_destroy(cover_tex);
            cover_tex = r_tex_create(COVER, COVER, cover_argb, 1);
            copy_text(cover_tex_id, sizeof cover_tex_id, cover_id);
            have_palette = 1;
            retint(0);
            fill_settings();
        }
        cover_pending = 0;
    }
    n->cover = cover_tex && !strcmp(cover_tex_id, shown_id) ? cover_tex : NULL;

    upload_thumb();
    assign_thumbs();

    ScePspDateTime now;
    if (sceRtcGetCurrentClockLocalTime(&now) == 0)
        snprintf(xmb.clock, sizeof xmb.clock, "%02d:%02d", now.hour % 24, now.minute % 60);
}

/* ------------------------------------------------------------ keyboard */

static void utf8_to_utf16(const char *in, unsigned short *out, size_t cap)
{
    size_t n = 0;
    const unsigned char *s = (const unsigned char *)in;
    while (*s && n + 1 < cap) {
        unsigned c = *s++;
        if (c >= 0xE0 && s[0] && s[1]) {
            c = ((c & 0x0Fu) << 12) | ((s[0] & 0x3Fu) << 6) | (s[1] & 0x3Fu);
            s += 2;
        } else if (c >= 0xC0 && s[0]) {
            c = ((c & 0x1Fu) << 6) | (s[0] & 0x3Fu);
            s += 1;
        }
        out[n++] = (unsigned short)c;
    }
    out[n] = 0;
}

static void utf16_to_utf8(const unsigned short *in, char *out, size_t cap)
{
    size_t n = 0;
    for (; *in; ++in) {
        unsigned c = *in;
        if (c < 0x80 && n + 1 < cap) {
            out[n++] = (char)c;
        } else if (c < 0x800 && n + 2 < cap) {
            out[n++] = (char)(0xC0 | (c >> 6));
            out[n++] = (char)(0x80 | (c & 0x3F));
        } else if (c >= 0x800 && n + 3 < cap) {
            out[n++] = (char)(0xE0 | (c >> 12));
            out[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[n++] = (char)(0x80 | (c & 0x3F));
        } else {
            break;
        }
    }
    out[n] = '\0';
}

/* Asks for a line of text with the system's on-screen keyboard, drawn over
 * the UI. Returns 1 with the text in out (UTF-8) when the user entered one. */
static int ask_text(const char *title, char *out, size_t cap)
{
    static unsigned short desc[64], initial[128], typed[128];
    utf8_to_utf16(title, desc, sizeof desc / sizeof desc[0]);
    utf8_to_utf16(out, initial, sizeof initial / sizeof initial[0]);
    memset(typed, 0, sizeof typed);

    SceUtilityOskData data;
    memset(&data, 0, sizeof data);
    data.language = PSP_UTILITY_OSK_LANGUAGE_DEFAULT;
    data.lines = 1;
    data.inputtype = PSP_UTILITY_OSK_INPUTTYPE_ALL;
    data.desc = desc;
    data.intext = initial;
    data.outtextlength = sizeof typed / sizeof typed[0];
    data.outtextlimit = 64;
    data.outtext = typed;

    SceUtilityOskParams params;
    memset(&params, 0, sizeof params);
    params.base.size = sizeof params;
    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_LANGUAGE, &params.base.language);
    params.base.buttonSwap = PSP_UTILITY_ACCEPT_CROSS;
    params.base.graphicsThread = 0x11;
    params.base.accessThread = 0x13;
    params.base.fontThread = 0x12;
    params.base.soundThread = 0x10;
    params.datacount = 1;
    params.data = &data;
    if (sceUtilityOskInitStart(&params) < 0) {
        log_printf("UI: keyboard did not start\n");
        return 0;
    }
    for (int done = 0, seen = 0; !done; ) {
        sync_state();
        render_gu_begin();
        ui_draw(&xmb);
        render_gu_finish();
        int status = sceUtilityOskGetStatus();
        if (status != PSP_UTILITY_DIALOG_NONE) seen = 1;
        switch (status) {
        case PSP_UTILITY_DIALOG_VISIBLE:
            sceUtilityOskUpdate(1);
            break;
        case PSP_UTILITY_DIALOG_QUIT:
            sceUtilityOskShutdownStart();
            break;
        case PSP_UTILITY_DIALOG_NONE:
            done = seen;
            break;
        default:
            break;
        }
        render_gu_present();
    }
    if (data.result != PSP_UTILITY_OSK_RESULT_CHANGED || !typed[0]) return 0;
    utf16_to_utf8(typed, out, cap);
    return out[0] != '\0';
}

static void start_search(void)
{
    char text[128];
    copy_text(text, sizeof text, xmb.search_query);
    if (!ask_text("Search Spotify", text, sizeof text)) return;
    copy_text(xmb.search_query, sizeof xmb.search_query, text);
    copy_text(search_text, sizeof search_text, text);
    ui_list *results = &xmb.lists[UI_CAT_SEARCH][0];
    results->rows = found_rows;
    results->count = 0;
    results->loading = 1;
    ++search_req_seq;
    log_printf("UI: search requested\n");
}

/* Returns 1 when the user asked to quit. */
static int handle(const ui_event *e)
{
    switch (e->type) {
    case UI_EV_QUIT:
        return 1;
    case UI_EV_OPEN:
        if (e->category == UI_CAT_LIKED && liked_shown) {
            play_from(liked, liked_count, e->index, -1, "Liked Songs");
        } else if (e->category == UI_CAT_PLAYLISTS && e->level == 0) {
            int p = e->index + 1;
            ui_list *l = &xmb.lists[UI_CAT_PLAYLISTS][1];
            copy_text(l->header, sizeof l->header, playlists[p].name);
            l->rows = opened_rows;
            l->count = 0;
            l->loading = 1;
            ui_open_level(&xmb, UI_CAT_PLAYLISTS);
            open_req_playlist = p;
            ++open_req_seq;
        } else if (e->category == UI_CAT_PLAYLISTS) {
            /* Only once the list on screen is the one in the buffer. */
            if (open_req_seq == open_done_seq && open_shown_seq == open_done_seq)
                play_from(opened, open_done_count, e->index, open_done_playlist,
                          playlists[open_done_playlist].name);
        } else if (e->category == UI_CAT_SEARCH) {
            if (search_done_seq == search_req_seq && search_shown_seq == search_done_seq)
                play_from(found, search_done_count, e->index, -10 - (int)search_done_seq,
                          "Search");
        } else if (e->category == UI_CAT_SETTINGS && e->index == 0) {
            cover_colors_on = !cover_colors_on;
            retint(0);
            fill_settings();
            save_settings();
        } else if (e->category == UI_CAT_SETTINGS) {
            apply_theme(e->index - 1, 0);
            save_settings();
        }
        break;
    case UI_EV_PAUSE:
        control.paused = !control.paused;
        break;
    case UI_EV_NEXT:
        start_track(current_index + 1);
        break;
    case UI_EV_PREV:
        /* Restart the song if it has been playing a while, else go back. */
        start_track(control.position_ms > 3000 ? current_index : current_index - 1);
        break;
    case UI_EV_VOLUME:
        set_volume(e->value);
        break;
    case UI_EV_SEARCH:
        start_search();
        break;
    default:
        break;      /* search and seeking come later */
    }
    return 0;
}

static unsigned read_buttons(void)
{
    SceCtrlData pad;
    sceCtrlPeekBufferPositive(&pad, 1);
    unsigned b = 0;
    if (pad.Buttons & PSP_CTRL_UP) b |= UI_BTN_UP;
    if (pad.Buttons & PSP_CTRL_DOWN) b |= UI_BTN_DOWN;
    if (pad.Buttons & PSP_CTRL_LEFT) b |= UI_BTN_LEFT;
    if (pad.Buttons & PSP_CTRL_RIGHT) b |= UI_BTN_RIGHT;
    if (pad.Buttons & PSP_CTRL_CROSS) b |= UI_BTN_CROSS;
    if (pad.Buttons & PSP_CTRL_CIRCLE) b |= UI_BTN_CIRCLE;
    if (pad.Buttons & PSP_CTRL_SQUARE) b |= UI_BTN_SQUARE;
    if (pad.Buttons & PSP_CTRL_TRIANGLE) b |= UI_BTN_TRIANGLE;
    if (pad.Buttons & PSP_CTRL_LTRIGGER) b |= UI_BTN_L;
    if (pad.Buttons & PSP_CTRL_RTRIGGER) b |= UI_BTN_R;
    if (pad.Buttons & PSP_CTRL_START) b |= UI_BTN_START;
    return b;
}

static void load_playlists(void)
{
    frame_message("Loading your library\xE2\x80\xA6", NULL);
    int n = spotify_sp_playlists(ctx.client_token, ctx.access_token, ctx.cfg.username,
                                 &playlists[1], MAX_PLAYLISTS);
    playlist_count = n > 0 ? n : 0;
    for (int i = 0; i < playlist_count; ++i) {
        const spotify_playlist *p = &playlists[i + 1];
        ui_row *r = &playlist_rows[i];
        memset(r, 0, sizeof *r);
        copy_text(r->title, sizeof r->title, p->name);
        if (p->total >= 0) snprintf(r->subtitle, sizeof r->subtitle, "Playlist \xC2\xB7 %d songs", p->total);
        else snprintf(r->subtitle, sizeof r->subtitle, "Playlist");
        r->color = row_color(p->name);
    }
    xmb.lists[UI_CAT_PLAYLISTS][0].rows = playlist_rows;
    xmb.lists[UI_CAT_PLAYLISTS][0].count = playlist_count;
    log_printf("UI: %d playlists\n", n);
}

static int setup_ui(void)
{
    render_gu_init();
    if (font_load(FONT_REGULAR, ui_font_regular, ui_font_regular_len) < 0 ||
        font_load(FONT_BOLD, ui_font_bold, ui_font_bold_len) < 0 ||
        font_load(FONT_XBOLD, ui_font_xbold, ui_font_xbold_len) < 0 ||
        font_load(FONT_ICONS, ui_font_icons, ui_font_icons_len) < 0) {
        log_printf("UI: font load failed\n");
        return -1;
    }
    if (ui_init(&xmb) < 0) {
        log_printf("UI: init failed\n");
        return -1;
    }
    /* Bake every font atlas now: baking one mid-animation drops frames.
     * Transparent draws bake without drawing anything. */
    render_gu_begin();
    static const struct { font_face face; int size; } sizes[] = {
        { FONT_REGULAR, 10 }, { FONT_REGULAR, 11 }, { FONT_REGULAR, 12 }, { FONT_REGULAR, 13 },
        { FONT_BOLD, 10 }, { FONT_BOLD, 11 }, { FONT_BOLD, 12 }, { FONT_BOLD, 15 },
        { FONT_XBOLD, 14 }, { FONT_XBOLD, 15 }, { FONT_XBOLD, 20 },
    };
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; ++i)
        font_draw(sizes[i].face, sizes[i].size, 0, 0, 0, "a");
    static const int icon_sizes[] = { 12, 16, 24, 36 };
    for (size_t i = 0; i < sizeof icon_sizes / sizeof icon_sizes[0]; ++i)
        font_icon(ICON_SEARCH, icon_sizes[i], 0, 0, 0);
    render_gu_end();
    xmb.lists[UI_CAT_LIKED][0].loading = 1;
    load_settings();
    return 0;
}

int spotify_ui_run(volatile int *exit_requested)
{
    log_set_echo(0);   /* the debug console would scribble over our frames */
    /* Full speed for the whole session: at 222 MHz each TLS handshake took
     * 0.6-1.3 s on hardware (about 0.25 s at 333 MHz). */
    scePowerSetClockFrequency(333, 333, 166);
    if (setup_ui() < 0) return 0;
    memory_log("ui-ready");

    spotify_ui_hooks hooks = { on_status, on_pair_code };
    int ret = spotify_ctx_start(&ctx, &hooks);
    if (ret < 0) {
        frame_message("Could not sign in to Spotify", "See spotify.log. START: exit");
        SceCtrlData pad;
        do {
            sceCtrlPeekBufferPositive(&pad, 1);
            sceKernelDelayThreadCB(50000);
        } while (!(pad.Buttons & PSP_CTRL_START) && !*exit_requested);
        render_gu_term();
        return 0;
    }
    set_volume(80);
    log_set_buffered(1);
    load_playlists();

    memory_log("pre-player-cover-threads");
    /* Priorities: UI 0x20 > player 0x22 > download 0x24 > keepalive 0x26 >
     * covers and lists 0x30. The UI runs a few ms per frame and then waits for
     * vblank, so the player keeps plenty of time; above the UI, its TLS
     * handshakes at track changes froze frames for up to 110 ms. */
    SceUID thid = sceKernelCreateThread("player", player_thread, 0x22, 0x20000, 0, NULL);
    SceUID cover_thid = sceKernelCreateThread("cover", cover_thread, 0x30, 0x20000, 0, NULL);
    SceUID loader_thid = sceKernelCreateThread("loader", loader_thread, 0x30, 0x10000, 0, NULL);
    thumb_lock = sceKernelCreateSema("thumbs", 0, 1, 1, NULL);
    SceUID thumb_thid = thumb_lock >= 0
        ? sceKernelCreateThread("thumbs", thumb_thread, 0x30, 0x20000, 0, NULL) : -1;
    if (cover_thid >= 0) sceKernelStartThread(cover_thid, 0, NULL);
    if (loader_thid >= 0) sceKernelStartThread(loader_thid, 0, NULL);
    if (thumb_thid >= 0) sceKernelStartThread(thumb_thid, 0, NULL);
    if (thid >= 0) sceKernelStartThread(thid, 0, NULL);

    SceUInt64 last = sceKernelGetSystemTimeWide(), stats_start = last;
    unsigned frames = 0, slow_frames = 0, worst_us = 0;
    for (int quit = 0; !quit && !*exit_requested; ) {
        SceUInt64 now = sceKernelGetSystemTimeWide();
        unsigned frame_us = (unsigned)(now - last);
        float dt = (float)frame_us / 1000000.0f;
        last = now;
        if (dt > 0.1f) dt = 0.1f;
        /* A frame is 16.7 ms; anything over 20 ms showed as a hitch. */
        ++frames;
        if (frame_us > 20000) ++slow_frames;
        if (frame_us > worst_us) worst_us = frame_us;
        if (now - stats_start >= 10000000u) {
            log_printf("UI: %u frames in 10 s, %u slow (>20 ms), worst %u ms\n", frames,
                       slow_frames, worst_us / 1000u);
            stats_start = now;
            frames = slow_frames = worst_us = 0;
        }

        ui_update(&xmb, read_buttons(), dt);
        for (int i = 0; i < xmb.event_count && !quit; ++i) quit = handle(&xmb.events[i]);
        sync_state();
        log_tick();

        render_gu_begin();
        ui_draw(&xmb);
        render_gu_end();
    }

    log_printf("UI: exit requested\n");
    frame_message("Stopping\xE2\x80\xA6", NULL);
    player_quit = 1;
    SceUID helpers[3] = { cover_thid, loader_thid, thumb_thid };
    for (int i = 0; i < 3; ++i) {
        if (helpers[i] < 0) continue;
        SceUInt timeout = 20u * 1000u * 1000u;
        if (sceKernelWaitThreadEnd(helpers[i], &timeout) < 0) sceKernelTerminateThread(helpers[i]);
        sceKernelDeleteThread(helpers[i]);
    }
    control.stop = 1;
    if (thid >= 0) {
        SceUInt timeout = 25u * 1000u * 1000u;
        if (sceKernelWaitThreadEnd(thid, &timeout) < 0) sceKernelTerminateThread(thid);
        sceKernelDeleteThread(thid);
    }
    spotify_ctx_stop(&ctx);
    if (thumb_lock >= 0) sceKernelDeleteSema(thumb_lock);
    log_set_buffered(0);
    render_gu_term();
    return me_dead ? PLAYER_ERR_ME_TIMEOUT : 0;
}
