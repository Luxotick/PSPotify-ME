#include "ui.h"
#include "../ui/draw.h"
#include "../ui/font.h"
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
            snprintf(theme_names[theme_count++], sizeof theme_names[0], "%s", e.d_name);
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
        snprintf(r->title, sizeof r->title, "%s", theme_names[k]);
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
            if (!strcmp(key, "theme")) snprintf(wanted, sizeof wanted, "%s", value);
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
        snprintf(queue_name, sizeof queue_name, "%s", name);
    }
    log_printf("UI: play request %d (list %d, %d tracks)\n", idx, key, queue_len);
    start_track(idx);
}

static void set_volume(int pct)
{
    xmb.volume = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    control.volume = PSP_AUDIO_VOLUME_MAX * xmb.volume / 100;
}

/* ------------------------------------------------------------ cover thread */

static void load_cover(const spotify_track *t)
{
    static uint8_t jpeg[128 * 1024];
    static uint32_t scaled[COVER * COVER];
    if (!t->has_cover) return;

    char url[80];
    int n = snprintf(url, sizeof url, "https://i.scdn.co/image/");
    for (int i = 0; i < 20 && n > 0 && (size_t)n + 3 < sizeof url; ++i)
        n += snprintf(url + n, sizeof url - (size_t)n, "%02x", t->cover[i]);

    http_stream s;
    uint64_t content_length = 0;
    int status = http_stream_open(&s, url, &content_length);
    if (status != 200) {
        if (status > 0) http_stream_close(&s);
        log_printf("UI: cover HTTP %d\n", status);
        return;
    }
    size_t got = 0;
    for (;;) {
        int r = http_stream_read(&s, jpeg + got, (unsigned int)(sizeof jpeg - got));
        if (r <= 0) break;
        got += (size_t)r;
        if (got == sizeof jpeg) break;
    }
    http_stream_close(&s);

    int w = 0, h = 0, comp = 0;
    /* Bound concurrent cover scratch/output allocations on the 32 MB model. */
    if (spotify_device_model == 0 &&
        (got < 2 || jpeg[0] != 0xff || jpeg[1] != 0xd8 ||
         !stbi_info_from_memory(jpeg, (int)got, &w, &h, &comp) ||
         w <= 0 || h <= 0 || w > 512 || h > 512 || (comp != 1 && comp != 3))) {
        log_printf("UI: cover skipped: PSP-1000 requires <=512x512 grayscale/RGB JPEG\n");
        return;
    }
    unsigned char *rgba = stbi_load_from_memory(jpeg, (int)got, &w, &h, &comp, 4);
    if (!rgba) {
        log_printf("UI: cover decode failed (%u bytes)\n", (unsigned int)got);
        return;
    }
    /* Box filter down to COVER x COVER: averages every source pixel. */
    for (int y = 0; y < COVER; ++y) {
        int row0 = y * h / COVER, row1 = (y + 1) * h / COVER;
        if (row1 <= row0) row1 = row0 + 1;
        for (int x = 0; x < COVER; ++x) {
            int col0 = x * w / COVER, col1 = (x + 1) * w / COVER;
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
            scaled[y * COVER + x] = 0xFF000000u | ((r / cnt) << 16) | ((g / cnt) << 8) | (b / cnt);
        }
    }
    stbi_image_free(rgba);

    /* Hand over to the UI thread once it took the previous cover. */
    while (cover_pending && !player_quit) sceKernelDelayThread(10000);
    memcpy(cover_argb, scaled, sizeof cover_argb);
    palette_extract(cover_argb, COVER * COVER, &cover_colors);
    snprintf(cover_id, sizeof cover_id, "%s", t->id);
    cover_pending = 1;
    log_printf("UI: cover %dx%d\n", w, h);
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

/* ------------------------------------------------------------ UI thread */

static void track_rows(const spotify_track *tracks, int count, ui_row *rows)
{
    for (int i = 0; i < count; ++i) {
        ui_row *r = &rows[i];
        memset(r, 0, sizeof *r);
        snprintf(r->title, sizeof r->title, "%s", tracks[i].name[0] ? tracks[i].name : tracks[i].id);
        snprintf(r->subtitle, sizeof r->subtitle, "%s", tracks[i].artist);
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

    static int shown_index = -2;
    static char shown_id[24];
    int idx = current_index;
    ui_now *n = &xmb.now;
    if (idx >= 0 && idx < queue_len) {
        const spotify_track *t = &queue[idx];
        if (idx != shown_index || strcmp(shown_id, t->id) != 0) {
            shown_index = idx;
            snprintf(shown_id, sizeof shown_id, "%s", t->id);
            snprintf(n->title, sizeof n->title, "%s", t->name);
            snprintf(n->artist, sizeof n->artist, "%s", t->artist);
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
        snprintf(n->next, sizeof n->next, "%s", idx + 1 < queue_len ? queue[idx + 1].name : "");
    }

    if (cover_pending) {
        if (!strcmp(cover_id, shown_id)) {
            r_tex_destroy(cover_tex);
            cover_tex = r_tex_create(COVER, COVER, cover_argb, 1);
            snprintf(cover_tex_id, sizeof cover_tex_id, "%s", cover_id);
            have_palette = 1;
            retint(0);
            fill_settings();
        }
        cover_pending = 0;
    }
    n->cover = cover_tex && !strcmp(cover_tex_id, shown_id) ? cover_tex : NULL;

    ScePspDateTime now;
    if (sceRtcGetCurrentClockLocalTime(&now) == 0)
        snprintf(xmb.clock, sizeof xmb.clock, "%02d:%02d", now.hour, now.minute);
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
            snprintf(l->header, sizeof l->header, "%s", playlists[p].name);
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
        snprintf(r->title, sizeof r->title, "%s", p->name);
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
    xmb.search_hint = "Search is coming in a later version";
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
    load_playlists();

    memory_log("pre-player-cover-threads");
    SceUID thid = sceKernelCreateThread("player", player_thread, 0x1C, 0x20000, 0, NULL);
    SceUID cover_thid = sceKernelCreateThread("cover", cover_thread, 0x30, 0x20000, 0, NULL);
    SceUID loader_thid = sceKernelCreateThread("loader", loader_thread, 0x30, 0x10000, 0, NULL);
    if (cover_thid >= 0) sceKernelStartThread(cover_thid, 0, NULL);
    if (loader_thid >= 0) sceKernelStartThread(loader_thid, 0, NULL);
    if (thid >= 0) sceKernelStartThread(thid, 0, NULL);

    SceUInt64 last = sceKernelGetSystemTimeWide();
    for (int quit = 0; !quit && !*exit_requested; ) {
        SceUInt64 now = sceKernelGetSystemTimeWide();
        float dt = (float)(now - last) / 1000000.0f;
        last = now;
        if (dt > 0.1f) dt = 0.1f;

        ui_update(&xmb, read_buttons(), dt);
        for (int i = 0; i < xmb.event_count && !quit; ++i) quit = handle(&xmb.events[i]);
        sync_state();

        render_gu_begin();
        ui_draw(&xmb);
        render_gu_end();
    }

    log_printf("UI: exit requested\n");
    frame_message("Stopping\xE2\x80\xA6", NULL);
    player_quit = 1;
    SceUID helpers[2] = { cover_thid, loader_thid };
    for (int i = 0; i < 2; ++i) {
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
    render_gu_term();
    return me_dead ? PLAYER_ERR_ME_TIMEOUT : 0;
}
