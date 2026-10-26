#include "ui.h"
#include "../ui/draw.h"
#include "../ui/font.h"
#include "../ui/icons.h"
#include "../ui/palette.h"
#include "../ui/ui.h"
#include "config.h"
#include "connect.h"
#include "http.h"
#include "tls.h"
#include "log.h"
#include "memory.h"
#include "player.h"
#include "prefs.h"
#include "render_gu.h"
#include "session.h"
#include "site.h"
#include "webapi.h"
#include <pspaudio.h>
#include <pspctrl.h>
#include <pspdisplay.h>
#include <psphprm.h>
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
/* Liked Songs load in steps so a user request never waits behind them:
 * the ids first, then names one batch at a time; rows appear as they come.
 * liked_ready counts the songs with names, liked_shown those turned into
 * rows. */
static volatile int liked_done, liked_count, liked_ready, liked_ids_loaded;
static int liked_shown;
static volatile unsigned open_req_seq, open_done_seq, open_shown_seq;
static volatile int open_req_playlist, open_done_playlist, open_done_count;

/* Recently played contexts, loaded the first time Recent is shown, and the
 * songs of the one opened. */
#define MAX_RECENT 30
#define MAX_CONTEXT_TRACKS 200
static spotify_context recent[MAX_RECENT];
static ui_row recent_rows[MAX_RECENT];
static volatile int recent_requested, recent_done, recent_count, recent_shown;
static spotify_track ctx_tracks[MAX_CONTEXT_TRACKS];
static ui_row ctx_rows[MAX_CONTEXT_TRACKS];
static volatile unsigned ctx_req_seq, ctx_done_seq, ctx_shown_seq;
static volatile int ctx_req_index, ctx_done_count;

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
static char queue_uri[96];          /* Spotify context URI, for Spotify Connect */
/* Where the next track starts, and whether paused (a Connect transfer). */
static volatile int start_at_ms, start_paused;

static player_control control;
static volatile int request_index = -1;
static volatile int current_index = -1;
static volatile play_state_t play_state = PS_IDLE;
static volatile int player_quit;
static volatile int me_dead;
static volatile int last_error;    /* why the last track could not play */
/* Set by the player thread, shown once by the UI thread. */
static volatile int list_ended, keys_refused;

/* Play order over the queue: identity, or a shuffle that starts with the
 * track the user picked. Repeat: 0 off, 1 the queue, 2 the current track. */
static int order[MAX_TRACKS];
static volatile int order_len;
/* Songs added with "Add to queue": appended to queue[] and slotted into the
 * play order right after the playing one (after earlier additions). */
static unsigned char queued[MAX_TRACKS];

/* The Queue category: what plays next, in play order (queue[] indices). */
#define QUEUE_VIEW 100
static ui_row queue_rows[QUEUE_VIEW];
static int queue_view[QUEUE_VIEW];
static unsigned int queue_version, queue_shown_version = ~0u;
static int queue_shown_current = -2;

/* The song a △ menu was opened for, and what its items do. */
enum {
    MENU_ADD, MENU_LIKE, MENU_UNLIKE, MENU_PLAY_NOW, MENU_REMOVE, MENU_PIN, MENU_UNPIN,
    MENU_UPDATE, MENU_UPDATE_LATER, MENU_QUIT, MENU_QUIT_LATER
};
static spotify_track menu_track;
static int menu_actions[UI_MENU_ITEMS];
static int menu_queue_row;
static int menu_playlist;           /* playlists[] index of a Playlists row */

/* Liked Songs edits run on the loader thread. */
static volatile unsigned like_req_seq, like_done_seq, like_seen_seq;

/* The theme gallery and app updates (spotify/site.c), one job at a time on
 * the loader thread. */
enum { SITE_GALLERY, SITE_THEME, SITE_CHECK, SITE_INSTALL };
static volatile unsigned site_req_seq, site_done_seq, site_seen_seq;
static volatile int site_job, site_arg, site_result;
#define GALLERY_MAX 32
static site_theme gallery[GALLERY_MAX];
static volatile int gallery_count = -1;     /* -1: not loaded, -2: failed */
enum {
    UPDATE_UNKNOWN, UPDATE_CHECKING, UPDATE_CURRENT, UPDATE_AVAILABLE, UPDATE_FAILED,
    UPDATE_INSTALLING, UPDATE_INSTALLED, UPDATE_INSTALL_FAILED
};
static site_update update_info;
static volatile int update_state, update_progress;
static int update_auto;                     /* the check was the one at start */
/* Quit from a menu or Settings (after an update: the new EBOOT runs next
 * time; a user app cannot start an EBOOT.PBP itself). */
static int quit_requested;
/* The Quit row at the end of Settings' groups, in group_view. */
#define GROUP_QUIT (-2)
static spotify_track like_track;
static volatile int like_remove, like_status;
static unsigned int rng_state = 0x9E3779B9u;
/* While L/R is held: the time the seek will jump to, shown on screen. */
static int seek_preview_ms = -1, seek_steps;
/* Set after a system dialog held the UI loop, so frame stats skip that gap. */
static int skip_frame_timing;

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
/* Spotify Connect is work in progress (branch wip/spotify-connect): built
 * in but switched off, and without its Settings row, unless
 * PSPOTIFY_CONNECT is 1. */
#ifndef PSPOTIFY_CONNECT
#define PSPOTIFY_CONNECT 0
#endif
/* Settings: level 0 lists the groups, level 1 the rows of the open one. */
static ui_row group_rows[PREF_GROUPS + 1];
static int group_view[PREF_GROUPS + 1], group_count;    /* row -> PREF_GROUP_*, or GROUP_QUIT */
#define OPTION_ROWS (PREF_COUNT > MAX_THEMES ? PREF_COUNT : MAX_THEMES)
static ui_row option_rows[OPTION_ROWS];
static int option_view[OPTION_ROWS];    /* row -> pref, theme or category */
static int open_group = -1;
static char theme_wallpaper[MAX_THEMES];    /* 1: the theme names a wallpaper */

static const char *connect_name(void)
{
    return spotify_device_model == 0 ? "PSP-1000" : spotify_device_model == 1 ? "PSP-2000"
         : spotify_device_model == 4 ? "PSP Go" : "PSP-3000";
}
static ui_theme base_theme;
static int have_palette;

/* The current cover as downloaded (before cutting it to a shape), for
 * reshaping and the blurred background. */
static uint32_t cover_keep[COVER * COVER];
static int cover_keep_valid;
static r_tex *backdrop_tex;
/* Set when the style changed: the next frame bakes its font sizes first. */
static int prebake_pending = 1;

/* Categories: all of them in the user's order, and which are hidden. */
static const char *const cat_tokens[UI_CATS] = {
    "search", "recent", "liked", "playlists", "now", "queue", "settings"
};
static const char *const cat_titles[UI_CATS] = {
    "Search", "Recent", "Liked Songs", "Playlists", "Now Playing", "Queue", "Settings"
};
static const unsigned cat_icon[UI_CATS] = {
    ICON_SEARCH, ICON_HISTORY, ICON_HEART, ICON_LIST_MUSIC, ICON_MUSIC, ICON_QUEUE, ICON_SLIDERS
};
static int cat_all[UI_CATS] = { 0, 1, 2, 3, 4, 5, 6 };
static unsigned char cat_hidden[UI_CATS];
/* PREF_START values, in order. */
static const int start_categories[] = {
    UI_CAT_PLAYLISTS, UI_CAT_LIKED, UI_CAT_SEARCH, UI_CAT_RECENT, UI_CAT_NOW
};

/* Playlists pinned to the top, by id; and the order they are listed in. */
#define MAX_PINS 16
static char pinned[MAX_PINS][24];
static int pinned_count;
static int playlist_view[MAX_PLAYLISTS];    /* row -> playlists[] index */
static void arrange_playlists(void);
static void site_finished(void);
static int site_request(int job, int arg);
static int pin_index(const char *id);

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
        theme_wallpaper[i] = t.wallpaper[0] != 0;
    }
    log_printf("UI: %d themes\n", theme_count);
}

static void retint(int instant)
{
    ui_theme target = base_theme;
    if (prefs[PREF_COVER_COLORS] && have_palette) theme_tint(&base_theme, &cover_colors, &target);
    xmb.theme_target = target;
    if (instant) xmb.theme = target;
}

static void apply_style(void)
{
    ui_style *st = &xmb.style;
    st->density = prefs[PREF_DENSITY];
    st->text_size = prefs[PREF_TEXT_SIZE];
    st->anim = prefs[PREF_ANIMATIONS];
    st->hide_hints = prefs[PREF_HINTS];
    st->now_layout = prefs[PREF_NOW_LAYOUT];
    st->swap_confirm = prefs[PREF_CONFIRM];
    st->lr_mode = prefs[PREF_LR];
    static const int steps[] = { 2, 5, 10 };
    st->volume_step = steps[prefs[PREF_VOLUME_STEP]];
    st->visualizer = prefs[PREF_VISUALIZER];
    control.visualizer = prefs[PREF_VISUALIZER];
    prebake_pending = 1;
}

/* The equalizer settings to the player; it reconfigures between frames. */
static void apply_equalizer(void)
{
    for (int b = 0; b < 5; ++b) control.eq_custom[b] = pref_number((pref_id)(PREF_EQ_60 + b));
    control.eq_preset = prefs[PREF_EQUALIZER];
    control.normalize = prefs[PREF_NORMALIZE];
    ++control.eq_version;
}

static void apply_cpu(void)
{
    static const int cpu[] = { 333, 266, 222 }, bus[] = { 166, 133, 111 };
    const int i = prefs[PREF_CPU];
    scePowerSetClockFrequency(cpu[i], cpu[i], bus[i]);
    log_printf("UI: CPU %d MHz\n", cpu[i]);
}

/* Sleep timer: minutes per PREF_SLEEP value, and when it runs out. */
static const int sleep_minutes[] = { 0, 15, 30, 45, 60, 90 };
static SceUInt64 sleep_deadline;

static void apply_categories(void)
{
    int shown[UI_CATS], n = 0;
    for (int i = 0; i < UI_CATS; ++i)
        if (!cat_hidden[cat_all[i]] || cat_all[i] == UI_CAT_SETTINGS) shown[n++] = cat_all[i];
    ui_set_categories(&xmb, shown, n);
}

/* ------------------------------------------------------------ backgrounds */

/* Reads the theme's wallpaper (JPEG or PNG) and scales it to fill 480x256
 * pixels, cropping the sides or the top and bottom to keep its shape; drawn
 * stretched to 480x272. A 512x256 16-bit texture: 256 KB. */
static r_tex *load_wallpaper(int theme, const char *file)
{
    char rel[128], path[320];
    snprintf(rel, sizeof rel, "THEMES/%s/%s", theme_names[theme], file);
    path_in_app(path, sizeof path, rel);
    FILE *f = fopen(path, "rb");
    if (!f) {
        log_printf("UI: wallpaper %s: not found\n", rel);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = size > 0 && size <= 2 * 1024 * 1024 ? malloc((size_t)size) : NULL;
    size_t got = data ? fread(data, 1, (size_t)size, f) : 0;
    fclose(f);
    int w = 0, h = 0, comp = 0;
    /* Big pictures need a lot of memory to decode; the PSP-1000 has 32 MB. */
    const int max_side = spotify_device_model == 0 ? 960 : 1600;
    if (!data || got != (size_t)size || !stbi_info_from_memory(data, (int)got, &w, &h, &comp) ||
        w <= 0 || h <= 0 || w > max_side || h > max_side) {
        log_printf("UI: wallpaper %s: unreadable or larger than %dx%d\n", rel, max_side, max_side);
        free(data);
        return NULL;
    }
    unsigned char *rgba = stbi_load_from_memory(data, (int)got, &w, &h, &comp, 4);
    free(data);
    if (!rgba) {
        log_printf("UI: wallpaper %s: decode failed\n", rel);
        return NULL;
    }
    enum { WW = 480, WH = 256 };
    /* The source rectangle with the screen's 480:272 shape. */
    int sx = 0, sy = 0, sw = w, sh = h;
    if ((long)w * 272 > (long)h * 480) {
        sw = h * 480 / 272;
        sx = (w - sw) / 2;
    } else {
        sh = w * 272 / 480;
        sy = (h - sh) / 2;
    }
    uint32_t *px = malloc(WW * WH * 4);
    r_tex *tex = NULL;
    if (px) {
        for (int y = 0; y < WH; ++y) {
            int y0 = sy + y * sh / WH, y1 = sy + (y + 1) * sh / WH;
            if (y1 <= y0) y1 = y0 + 1;
            for (int x = 0; x < WW; ++x) {
                int x0 = sx + x * sw / WW, x1 = sx + (x + 1) * sw / WW;
                if (x1 <= x0) x1 = x0 + 1;
                unsigned r = 0, g = 0, b = 0, n = 0;
                for (int yy = y0; yy < y1; yy += 1 + (y1 - y0) / 4)
                    for (int xx = x0; xx < x1; xx += 1 + (x1 - x0) / 4) {
                        const unsigned char *p = rgba + 4 * (yy * w + xx);
                        r += p[0];
                        g += p[1];
                        b += p[2];
                        ++n;
                    }
                px[y * WW + x] = 0xFF000000u | ((r / n) << 16) | ((g / n) << 8) | (b / n);
            }
        }
        tex = r_tex_create_opaque(WW, WH, px, 1);
        free(px);
    }
    stbi_image_free(rgba);
    log_printf("UI: wallpaper %s %dx%d %s\n", rel, w, h, tex ? "loaded" : "failed");
    return tex;
}

/* Sets the picture behind the interface from the Background setting. */
static void update_backdrop(void)
{
    r_tex_destroy(backdrop_tex);
    backdrop_tex = NULL;
    float dim = 0.0f;
    if (prefs[PREF_BACKGROUND] == BACKGROUND_BLURRED_COVER && cover_keep_valid) {
        static uint32_t px[BACKDROP_W * BACKDROP_H];
        palette_backdrop(cover_keep, COVER, px);
        backdrop_tex = r_tex_create(BACKDROP_W, BACKDROP_H, px, 1);
        dim = 0.6f;
    } else if (prefs[PREF_BACKGROUND] == BACKGROUND_WALLPAPER && theme_active >= 0 &&
               base_theme.wallpaper[0]) {
        backdrop_tex = load_wallpaper(theme_active, base_theme.wallpaper);
        dim = (float)base_theme.wallpaper_dim / 100.0f;
    }
    xmb.style.backdrop = backdrop_tex;
    xmb.style.backdrop_dim = dim;
}

/* The Now Playing cover texture, cut to the chosen shape. */
static void rebuild_cover(void)
{
    if (!cover_keep_valid) return;
    static uint32_t shaped[COVER * COVER];
    memcpy(shaped, cover_keep, sizeof shaped);
    cover_shape(shaped, COVER, prefs[PREF_COVER_SHAPE]);
    r_tex_destroy(cover_tex);
    cover_tex = r_tex_create(COVER, COVER, shaped, 1);
}

/* ------------------------------------------------------------ settings screen */

static const struct {
    const char *title, *subtitle;
    unsigned icon;
    uint32_t color;
} group_info[PREF_GROUPS] = {
    [PREF_GROUP_THEMES] = { "Themes", "Colors and wallpapers from the THEMES folder",
                            ICON_PALETTE, 0xFF7C3AEDu },
    [PREF_GROUP_GALLERY] = { "Theme gallery", "Download themes from the PSPotify ME site",
                             ICON_IMAGES, 0xFFBE185Du },
    [PREF_GROUP_APPEARANCE] = { "Appearance", "Background, covers, text and animations",
                                ICON_SPARKLES, 0xFFDB2777u },
    [PREF_GROUP_LAYOUT] = { "Layout", "Start screen, Now Playing and playlists", ICON_LAYOUT,
                            0xFF0E7490u },
    [PREF_GROUP_CATEGORIES] = { "Categories", "Reorder or hide the categories", ICON_REORDER,
                                0xFFB45309u },
    [PREF_GROUP_SOUND] = { "Sound", "Equalizer and loudness", ICON_SLIDERS, 0xFF15803Du },
    [PREF_GROUP_PLAYBACK] = { "Playback", "Sleep timer, battery and CPU speed", ICON_HEADPHONES,
                              0xFF3B4FD8u },
    [PREF_GROUP_CONTROLS] = { "Controls", "Buttons and the headphone remote", ICON_GAMEPAD,
                              0xFF475569u },
    [PREF_GROUP_ABOUT] = { "About", "Version and updates", ICON_INFO, 0xFF0F766Eu },
};

static int group_has_prefs(int g)
{
    for (int i = 0; i < PREF_COUNT; ++i)
        if (pref_info((pref_id)i)->group == g) return 1;
    return 0;
}

static void fill_groups(void)
{
    group_count = 0;
    for (int g = 0; g < PREF_GROUPS; ++g) {
        if (g != PREF_GROUP_THEMES && g != PREF_GROUP_CATEGORIES && g != PREF_GROUP_GALLERY &&
            g != PREF_GROUP_ABOUT && !group_has_prefs(g))
            continue;
        ui_row *r = &group_rows[group_count];
        memset(r, 0, sizeof *r);
        copy_text(r->title, sizeof r->title, group_info[g].title);
        if (g == PREF_GROUP_THEMES && theme_active >= 0)
            snprintf(r->subtitle, sizeof r->subtitle, "%s \xC2\xB7 %d installed",
                     theme_names[theme_active], theme_count);
        else if (g == PREF_GROUP_ABOUT && update_state == UPDATE_AVAILABLE)
            snprintf(r->subtitle, sizeof r->subtitle, "Version %s is out", update_info.version);
        else
            copy_text(r->subtitle, sizeof r->subtitle, group_info[g].subtitle);
        r->icon = group_info[g].icon;
        r->color = group_info[g].color;
        group_view[group_count++] = g;
    }
    ui_row *quit = &group_rows[group_count];
    memset(quit, 0, sizeof *quit);
    copy_text(quit->title, sizeof quit->title, "Quit");
    copy_text(quit->subtitle, sizeof quit->subtitle, "Back to the PSP menu (START does it too)");
    quit->icon = ICON_POWER;
    quit->color = 0xFF475569u;
    group_view[group_count++] = GROUP_QUIT;
    ui_list *l = &xmb.lists[UI_CAT_SETTINGS][0];
    l->rows = group_rows;
    l->count = group_count;
}

/* The rows of the open group, refreshed after every change. */
static void fill_options(void)
{
    ui_list *l = &xmb.lists[UI_CAT_SETTINGS][1];
    const int g = open_group;
    if (g < 0) return;
    int n = 0;
    if (g == PREF_GROUP_THEMES) {
        for (int k = 0; k < theme_count; ++k) {
            ui_row *r = &option_rows[n];
            memset(r, 0, sizeof *r);
            copy_text(r->title, sizeof r->title, theme_names[k]);
            snprintf(r->subtitle, sizeof r->subtitle, "%s%s", k == theme_active ? "Active" : "Theme",
                     theme_wallpaper[k] ? " \xC2\xB7 has a wallpaper" : "");
            r->icon = ICON_PALETTE;
            r->color = theme_swatch[k];
            r->playing = k == theme_active;
            option_view[n++] = k;
        }
    } else if (g == PREF_GROUP_GALLERY) {
        if (gallery_count == -2) {
            ui_row *r = &option_rows[n];
            memset(r, 0, sizeof *r);
            copy_text(r->title, sizeof r->title, "Couldn't reach the theme site");
            copy_text(r->subtitle, sizeof r->subtitle, "Check the Wi-Fi \xC2\xB7 select to try again");
            r->icon = ICON_IMAGES;
            r->color = 0xFF3A3F4Au;
            option_view[n++] = -1;
        }
        const int busy = site_req_seq != site_done_seq && site_job == SITE_THEME;
        for (int i = 0; i < gallery_count && n < OPTION_ROWS; ++i) {
            ui_row *r = &option_rows[n];
            memset(r, 0, sizeof *r);
            int installed = 0;
            for (int k = 0; k < theme_count; ++k) installed |= !strcmp(theme_names[k], gallery[i].folder);
            copy_text(r->title, sizeof r->title, gallery[i].folder);
            snprintf(r->subtitle, sizeof r->subtitle, "%s%s",
                     busy && site_arg == i ? "Downloading\xE2\x80\xA6"
                     : installed ? "Installed \xC2\xB7 select to download again" : "Select to download",
                     gallery[i].has_wallpaper ? " \xC2\xB7 wallpaper" : "");
            r->icon = installed ? ICON_PALETTE : ICON_DOWNLOAD;
            r->color = gallery[i].accent;
            r->playing = installed;
            option_view[n++] = i;
        }
    } else if (g == PREF_GROUP_ABOUT) {
        ui_row *r = &option_rows[n];
        memset(r, 0, sizeof *r);
        snprintf(r->title, sizeof r->title, "PSPotify ME %s", PSPOTIFY_VERSION);
        switch (update_state) {
        case UPDATE_CHECKING: copy_text(r->subtitle, sizeof r->subtitle, "Checking for updates\xE2\x80\xA6"); break;
        case UPDATE_CURRENT: copy_text(r->subtitle, sizeof r->subtitle, "Up to date"); break;
        case UPDATE_AVAILABLE:
            snprintf(r->subtitle, sizeof r->subtitle, "Version %s is out \xC2\xB7 select to install",
                     update_info.version);
            break;
        case UPDATE_FAILED:
            copy_text(r->subtitle, sizeof r->subtitle, "Couldn't check \xC2\xB7 select to try again");
            break;
        case UPDATE_INSTALLING:
            snprintf(r->subtitle, sizeof r->subtitle, "Downloading %s\xE2\x80\xA6 %d%%",
                     update_info.version, update_progress);
            break;
        case UPDATE_INSTALLED:
            snprintf(r->subtitle, sizeof r->subtitle, "%s installed \xC2\xB7 select to quit, then start it again",
                     update_info.version);
            break;
        case UPDATE_INSTALL_FAILED:
            copy_text(r->subtitle, sizeof r->subtitle, "Update failed, nothing changed \xC2\xB7 select to try again");
            break;
        default: copy_text(r->subtitle, sizeof r->subtitle, "Select to check for updates"); break;
        }
        r->icon = update_state == UPDATE_AVAILABLE ? ICON_DOWNLOAD
                : update_state == UPDATE_INSTALLED ? ICON_POWER : ICON_INFO;
        r->color = update_state == UPDATE_AVAILABLE ? xmb.theme_target.accent : 0xFF0F766Eu;
        option_view[n++] = 0;
        r = &option_rows[n];
        memset(r, 0, sizeof *r);
        copy_text(r->title, sizeof r->title, pref_info(PREF_CHECK_UPDATES)->label);
        copy_text(r->subtitle, sizeof r->subtitle, pref_value_name(PREF_CHECK_UPDATES));
        r->icon = ICON_INFO;
        r->color = prefs[PREF_CHECK_UPDATES] ? xmb.theme_target.accent : 0xFF3A3F4Au;
        option_view[n++] = 1;
    } else if (g == PREF_GROUP_CATEGORIES) {
        for (int i = 0; i < UI_CATS; ++i) {
            const int c = cat_all[i];
            ui_row *r = &option_rows[n];
            memset(r, 0, sizeof *r);
            copy_text(r->title, sizeof r->title, cat_titles[c]);
            copy_text(r->subtitle, sizeof r->subtitle, c == UI_CAT_SETTINGS ? "Always shown"
                      : cat_hidden[c] ? "Hidden" : "Shown");
            r->icon = cat_icon[c];
            r->color = cat_hidden[c] && c != UI_CAT_SETTINGS ? 0xFF3A3F4Au : xmb.theme_target.accent;
            option_view[n++] = c;
        }
    } else {
        for (int i = 0; i < PREF_COUNT && n < OPTION_ROWS; ++i) {
            const pref_def *d = pref_info((pref_id)i);
            if (d->group != g) continue;
            ui_row *r = &option_rows[n];
            memset(r, 0, sizeof *r);
            copy_text(r->title, sizeof r->title, d->label);
            copy_text(r->subtitle, sizeof r->subtitle, pref_value_name((pref_id)i));
            if (i == PREF_BACKGROUND && prefs[i] == BACKGROUND_WALLPAPER && !base_theme.wallpaper[0])
                copy_text(r->subtitle, sizeof r->subtitle, "Theme wallpaper (this theme has none)");
            if (i == PREF_SLEEP && prefs[i] && sleep_deadline) {
                const SceUInt64 now = sceKernelGetSystemTimeWide();
                const unsigned left = sleep_deadline > now
                    ? (unsigned)((sleep_deadline - now) / 60000000u) + 1u : 0u;
                snprintf(r->subtitle, sizeof r->subtitle, "%s \xC2\xB7 %s in %u min",
                         pref_value_name((pref_id)i),
                         prefs[PREF_SLEEP_ACTION] == SLEEP_POWER_OFF ? "turns off" : "pauses", left);
            }
            if (i >= PREF_EQ_60 && i <= PREF_EQ_14K && prefs[PREF_EQUALIZER] != PLAYER_EQ_CUSTOM)
                snprintf(r->subtitle, sizeof r->subtitle, "%s \xC2\xB7 used by Custom",
                         pref_value_name((pref_id)i));
            option_view[n++] = i;
        }
    }
    copy_text(l->header, sizeof l->header, group_info[g].title);
    l->rows = option_rows;
    l->count = n;
    const int own_list = g == PREF_GROUP_THEMES || g == PREF_GROUP_CATEGORIES ||
                         g == PREF_GROUP_GALLERY || g == PREF_GROUP_ABOUT;
    l->plain = !own_list;
    l->adjustable = !own_list || g == PREF_GROUP_CATEGORIES;
    l->movable = g == PREF_GROUP_CATEGORIES;
    l->loading = g == PREF_GROUP_GALLERY && gallery_count == -1;
    if (xmb.sel[UI_CAT_SETTINGS][1] >= n) xmb.sel[UI_CAT_SETTINGS][1] = n ? n - 1 : 0;
}

static void fill_settings(void)
{
    fill_groups();
    fill_options();
}

static void save_settings(void)
{
    char path[300];
    path_in_app(path, sizeof path, "settings.ini");
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "# PSPotify ME settings. Most can be changed in Settings on the PSP.\n");
    fprintf(f, "theme = %s\n", theme_active >= 0 ? theme_names[theme_active] : "");
    prefs_write(f);
    /* Categories left to right; a '-' hides one. */
    fprintf(f, "categories = ");
    for (int i = 0; i < UI_CATS; ++i)
        fprintf(f, "%s%s%s", i ? "," : "", cat_hidden[cat_all[i]] ? "-" : "", cat_tokens[cat_all[i]]);
    fprintf(f, "\npinned = ");
    for (int i = 0; i < pinned_count; ++i) fprintf(f, "%s%s", i ? "," : "", pinned[i]);
    fprintf(f, "\n# on: write spotify.log for bug reports (from the next start)\n"
               "debug = %s\n", log_enabled() ? "on" : "off");
    fclose(f);
}

static void apply_theme(int i, int instant)
{
    theme_defaults(&base_theme);
    if (i >= 0 && i < theme_count) read_theme(i, &base_theme);
    theme_active = i;
    retint(instant);
    update_backdrop();
    fill_settings();
}

/* "a,-b,c": the categories in order, '-' for hidden ones. */
static void read_categories(const char *value)
{
    int seq[UI_CATS], n = 0, used[UI_CATS] = { 0 };
    unsigned char hidden[UI_CATS] = { 0 };
    char list[160];
    copy_text(list, sizeof list, value);
    for (char *tok = strtok(list, ", "); tok; tok = strtok(NULL, ", ")) {
        const int hide = tok[0] == '-';
        for (int c = 0; c < UI_CATS; ++c)
            if (!strcmp(cat_tokens[c], tok + hide) && !used[c] && n < UI_CATS) {
                used[c] = 1;
                hidden[c] = (unsigned char)hide;
                seq[n++] = c;
            }
    }
    /* Categories the file does not name (new ones) go at the end. */
    for (int c = 0; c < UI_CATS; ++c)
        if (!used[c]) seq[n++] = c;
    memcpy(cat_all, seq, sizeof cat_all);
    memcpy(cat_hidden, hidden, sizeof cat_hidden);
    cat_hidden[UI_CAT_SETTINGS] = 0;
}

static void read_pins(const char *value)
{
    char list[MAX_PINS * 24];
    copy_text(list, sizeof list, value);
    pinned_count = 0;
    for (char *tok = strtok(list, ", "); tok && pinned_count < MAX_PINS; tok = strtok(NULL, ", "))
        copy_text(pinned[pinned_count++], sizeof pinned[0], tok);
}

static void load_settings(void)
{
    char path[300], line[256], wanted[32] = "Midnight";
    path_in_app(path, sizeof path, "settings.ini");
    prefs_defaults();
    pref_set_shown(PREF_CONNECT, PSPOTIFY_CONNECT);
    prefs[PREF_CONNECT] = PSPOTIFY_CONNECT;
    FILE *f = fopen(path, "r");
    int has_debug = 0, has_new_keys = 0;
    if (f) {
        while (fgets(line, sizeof line, f)) {
            char key[32], value[200];
            if (sscanf(line, " %31[a-z_] = %199[^\r\n]", key, value) != 2) continue;
            if (!strcmp(key, "theme")) copy_text(wanted, sizeof wanted, value);
            else if (!strcmp(key, "categories")) { read_categories(value); has_new_keys = 1; }
            else if (!strcmp(key, "pinned")) read_pins(value);
            else if (!strcmp(key, "debug")) has_debug = 1;   /* read by log_init */
            else prefs_read(key, value);
        }
        fclose(f);
    }
    if (!PSPOTIFY_CONNECT) prefs[PREF_CONNECT] = 0;
    log_printf("UI: settings read\n");
    apply_equalizer();
    apply_style();
    scan_themes();
    int active = theme_count ? 0 : -1;
    for (int i = 0; i < theme_count; ++i)
        if (!strcmp(theme_names[i], wanted)) active = i;
    apply_theme(active, 1);
    log_printf("UI: theme %d applied\n", active);
    xmb.cat = start_categories[prefs[PREF_START]];
    apply_categories();
    /* First start, or a file from an older version: write it once so every
     * setting is there to edit. */
    if (!has_debug || !has_new_keys) save_settings();
}

/* ------------------------------------------------------------ loader thread */

static void fill_details(spotify_track *out, int n)
{
    for (int i = 0; i < n && !player_quit; i += SPOTIFY_DETAILS_BATCH) {
        int batch = n - i < SPOTIFY_DETAILS_BATCH ? n - i : SPOTIFY_DETAILS_BATCH;
        spotify_sp_track_details(ctx.client_token, ctx.access_token, &out[i], batch);
    }
}

static int load_list_max(const char *playlist_id, spotify_track *out, int max)
{
    memset(out, 0, sizeof(spotify_track) * (size_t)max);
    int n = spotify_sp_tracks(ctx.client_token, ctx.access_token, ctx.cfg.username,
                              playlist_id, out, max);
    if (n < 0) n = 0;
    fill_details(out, n);
    return n;
}

static int is_collection(const char *uri)
{
    size_t n = strlen(uri);
    return n > 11 && strcmp(uri + n - 11, ":collection") == 0;
}

static const spotify_playlist *known_playlist(const char *uri)
{
    if (strncmp(uri, "spotify:playlist:", 17) != 0) return NULL;
    for (int i = 1; i <= playlist_count; ++i)
        if (!strcmp(playlists[i].id, uri + 17)) return &playlists[i];
    return NULL;
}

static void load_recent(void)
{
    int n = spotify_sp_recent(ctx.client_token, ctx.access_token, ctx.cfg.username, recent,
                              MAX_RECENT);
    if (n < 0) n = 0;
    for (int i = 0; i < n && !player_quit; ++i) {
        const spotify_playlist *p = known_playlist(recent[i].uri);
        if (is_collection(recent[i].uri))
            copy_text(recent[i].name, sizeof recent[i].name, "Liked Songs");
        else if (p)
            copy_text(recent[i].name, sizeof recent[i].name, p->name);
        else
            spotify_sp_context(ctx.client_token, ctx.access_token, recent[i].uri,
                               recent[i].name, sizeof recent[i].name, NULL, 0);
    }
    recent_count = n;
}

static int load_context(int i)
{
    const char *uri = recent[i].uri;
    memset(ctx_tracks, 0, sizeof ctx_tracks);
    if (is_collection(uri)) {
        int n = liked_ready < MAX_CONTEXT_TRACKS ? liked_ready : MAX_CONTEXT_TRACKS;
        memcpy(ctx_tracks, liked, sizeof ctx_tracks[0] * (size_t)n);
        return n;
    }
    if (!strncmp(uri, "spotify:playlist:", 17))
        return load_list_max(uri + 17, ctx_tracks, MAX_CONTEXT_TRACKS);
    int n = spotify_sp_context(ctx.client_token, ctx.access_token, uri, NULL, 0, ctx_tracks,
                               MAX_CONTEXT_TRACKS);
    if (n < 0) n = 0;
    fill_details(ctx_tracks, n);
    return n;
}

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

static void run_site_job(void)
{
    switch (site_job) {
    case SITE_GALLERY: {
        const int n = site_fetch_themes(gallery, GALLERY_MAX);
        gallery_count = n < 0 ? -2 : n;
        break;
    }
    case SITE_THEME: {
        char dir[300];
        path_in_app(dir, sizeof dir, "THEMES");
        site_result = site_arg >= 0 && site_arg < gallery_count
                    ? site_download_theme(&gallery[site_arg], dir) : -1;
        break;
    }
    case SITE_CHECK: {
        update_state = UPDATE_CHECKING;
        const int r = site_check_update(&update_info);
        update_state = r < 0 ? UPDATE_FAILED : r ? UPDATE_AVAILABLE : UPDATE_CURRENT;
        break;
    }
    case SITE_INSTALL:
        update_state = UPDATE_INSTALLING;
        update_state = site_install_update(&update_info, spotify_config_dir(), &update_progress) == 0
                     ? UPDATE_INSTALLED : UPDATE_INSTALL_FAILED;
        break;
    }
}

static int loader_thread(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    while (!player_quit) {
        if (recent_requested && !recent_done) {
            load_recent();
            recent_done = 1;
            continue;
        }
        unsigned ctx_seq = ctx_req_seq;
        if (ctx_seq != ctx_done_seq) {
            int i = ctx_req_index;
            ctx_done_count = load_context(i);
            ctx_done_seq = ctx_seq;
            log_printf("UI: %d songs in recent context %d\n", ctx_done_count, i);
            continue;
        }
        unsigned site_seq = site_req_seq;
        if (site_seq != site_done_seq) {
            run_site_job();
            site_done_seq = site_seq;
            continue;
        }
        unsigned like_seq = like_req_seq;
        if (like_seq != like_done_seq) {
            like_status = spotify_sp_like(ctx.client_token, ctx.access_token, ctx.cfg.username,
                                          like_track.id, like_remove);
            like_done_seq = like_seq;
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
        if (!liked_ids_loaded) {
            memset(liked, 0, sizeof liked);
            int n = spotify_sp_tracks(ctx.client_token, ctx.access_token, ctx.cfg.username, "",
                                      liked, MAX_TRACKS);
            liked_count = n > 0 ? n : 0;
            liked_ids_loaded = 1;
            if (!liked_count) liked_done = 1;
            continue;
        }
        if (!liked_done) {
            const int i = liked_ready;
            const int batch = liked_count - i < SPOTIFY_DETAILS_BATCH ? liked_count - i
                                                                       : SPOTIFY_DETAILS_BATCH;
            spotify_sp_track_details(ctx.client_token, ctx.access_token, &liked[i], batch);
            liked_ready = i + batch;
            if (liked_ready >= liked_count) {
                liked_done = 1;
                log_printf("UI: %d liked songs\n", liked_count);
            }
            continue;
        }
        sceKernelDelayThread(30000);
    }
    return 0;
}

/* ------------------------------------------------------------ play order */

static unsigned int next_random(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static void build_order(int first)
{
    const int n = queue_len;
    order_len = n;
    ++queue_version;
    for (int i = 0; i < n; ++i) order[i] = i;
    if (!prefs[PREF_SHUFFLE] || n < 2) return;
    rng_state ^= (unsigned int)sceKernelGetSystemTimeLow();
    for (int i = n - 1; i > 0; --i) {
        int j = (int)(next_random() % (unsigned int)(i + 1));
        int t = order[i];
        order[i] = order[j];
        order[j] = t;
    }
    for (int i = 0; i < n; ++i)
        if (order[i] == first) {
            order[i] = order[0];
            order[0] = first;
            break;
        }
}

static int order_position(int idx)
{
    for (int i = 0; i < order_len; ++i)
        if (order[i] == idx) return i;
    return -1;
}

/* The track after idx. automatic: the current one ended by itself, so
 * "repeat this track" plays it again. -1 at the end of the queue. */
static int next_index(int idx, int automatic)
{
    if (automatic && prefs[PREF_REPEAT] == 2) return idx;
    int p = order_position(idx);
    if (p < 0) return -1;
    if (p + 1 < order_len) return order[p + 1];
    return prefs[PREF_REPEAT] ? order[0] : -1;
}

static int prev_index(int idx)
{
    int p = order_position(idx);
    if (p > 0) return order[p - 1];
    return prefs[PREF_REPEAT] && order_len > 0 ? order[order_len - 1] : idx;
}

/* Inserts a song to play after the current one and any songs queued before
 * it. Returns 0, or -1 when the queue is full. */
static int queue_add(const spotify_track *t)
{
    const int q = queue_len;
    if (q >= MAX_TRACKS) return -1;
    queue[q] = *t;
    queued[q] = 1;
    int at = order_position(current_index) + 1;
    while (at < order_len && queued[order[at]]) ++at;
    memmove(&order[at + 1], &order[at], sizeof order[0] * (size_t)(order_len - at));
    order[at] = q;
    queue_len = q + 1;
    order_len = order_len + 1;
    ++queue_version;
    return 0;
}

static void queue_remove(int row)
{
    const int at = order_position(current_index) + 1 + row;
    if (row < 0 || at >= order_len) return;
    memmove(&order[at], &order[at + 1], sizeof order[0] * (size_t)(order_len - at - 1));
    order_len = order_len - 1;
    ++queue_version;
}

static void queue_move(int row, int dir)
{
    const int at = order_position(current_index) + 1 + row;
    if (row < 0 || at + dir <= order_position(current_index) || at + dir >= order_len) return;
    const int t = order[at];
    order[at] = order[at + dir];
    order[at + dir] = t;
    ++queue_version;
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
        control.seek_ms = start_at_ms > 0 ? start_at_ms : -1;
        control.paused = start_paused;
        start_at_ms = 0;
        start_paused = 0;
        control.duration_ms = queue[idx].duration_ms;
        play_state = PS_LOADING;
        int upcoming = next_index(idx, 1);
        if (upcoming >= 0 && upcoming != idx) spotify_ctx_prefetch(&ctx, queue[upcoming].id);

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
            last_error = ret;
            play_state = PS_ERROR;
            if (++failures >= 3) {
                if (ret == SESSION_KEY_REFUSED) keys_refused = 1;
                continue;
            }
            sceKernelDelayThread(500000);
        } else {
            failures = 0;
            play_state = PS_PLAYING;
        }
        int after = next_index(idx, ret >= 0);
        if (after >= 0) {
            request_index = after;
        } else {
            play_state = PS_IDLE;
            list_ended = 1;
        }
    }
    return 0;
}

static void start_track(int queue_idx)
{
    if (queue_idx < 0 || queue_idx >= queue_len) return;
    request_index = queue_idx;
    control.stop = 1;           /* ends the current track, if any */
}

static void play_from(const spotify_track *tracks, int count, int idx, int key, const char *name,
                      const char *uri)
{
    copy_text(queue_uri, sizeof queue_uri, uri);
    if (count <= 0 || idx < 0 || idx >= count) return;
    /* Queue this list unless it is already the queue. */
    if (queue_key != key || queue_len != count) {
        control.stop = 1;
        request_index = -1;
        while (play_state == PS_LOADING && current_index >= 0 && !me_dead)
            sceKernelDelayThread(10000);
        memcpy(queue, tracks, sizeof tracks[0] * (size_t)count);
        memset(queued, 0, sizeof queued);
        queue_len = count;
        queue_key = key;
        copy_text(queue_name, sizeof queue_name, name);
    }
    build_order(idx);
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
    for (int i = 0; i < MAX_RECENT; ++i)
        if (recent_rows[i].cover == tex) recent_rows[i].cover = NULL;
    for (int i = 0; i < MAX_CONTEXT_TRACKS; ++i)
        if (ctx_rows[i].cover == tex) ctx_rows[i].cover = NULL;
    for (int i = 0; i < QUEUE_VIEW; ++i)
        if (queue_rows[i].cover == tex) queue_rows[i].cover = NULL;
}

/* The texture for url if it is loaded; otherwise queues it. UI thread. */
static r_tex *thumb_for(const char *url, int priority)
{
    if (!url[0] || thumb_lock < 0) return NULL;
    sceKernelWaitSema(thumb_lock, 1, NULL);
    r_tex *tex = NULL;
    int hit = -1, victim = -1;
    for (int i = 0; i < THUMB_SLOTS && hit < 0; ++i)
        if (thumbs[i].state != THUMB_FREE && !strcmp(thumbs[i].url, url)) hit = i;
    /* Reuse a free slot, else the one unused for longest (never one being
     * downloaded, nor one drawn this or last frame). */
    for (int i = 0; i < THUMB_SLOTS && hit < 0; ++i) {
        const thumb_slot *s = &thumbs[i];
        if (s->state == THUMB_FREE) {
            victim = i;
            break;
        }
        if (s->state == THUMB_LOADING || s->last_used + 1 >= frame_no) continue;
        if (victim < 0 || s->last_used < thumbs[victim].last_used) victim = i;
    }
    if (hit >= 0) {
        thumbs[hit].last_used = frame_no;
        thumbs[hit].priority = priority;
        if (thumbs[hit].state == THUMB_READY) tex = thumbs[hit].tex;
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
    if (cat != UI_CAT_LIKED && cat != UI_CAT_PLAYLISTS && cat != UI_CAT_SEARCH &&
        cat != UI_CAT_RECENT && cat != UI_CAT_QUEUE)
        return;
    if (prefs[PREF_DENSITY] == UI_DENSITY_TEXT) return;     /* no covers to show */
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
                copy_text(url, sizeof url, playlists[playlist_view[i]].image);
            } else if (cat == UI_CAT_RECENT && lvl == 0) {
                const spotify_playlist *p = known_playlist(recent[i].uri);
                if (p) copy_text(url, sizeof url, p->image);
            } else if (cat == UI_CAT_RECENT) {
                if (ctx_tracks[i].has_thumb) image_url(ctx_tracks[i].thumb, url, sizeof url);
            } else if (cat == UI_CAT_QUEUE) {
                const spotify_track *t = &queue[queue_view[i]];
                if (t->has_thumb) image_url(t->thumb, url, sizeof url);
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
    for (int i = 0; i < liked_shown; ++i) liked_rows[i].playing = id && !strcmp(liked[i].id, id);
    ui_list *open_list = &xmb.lists[UI_CAT_PLAYLISTS][1];
    if (open_shown_seq == open_done_seq && open_list->count > 0)
        for (int i = 0; i < open_list->count; ++i)
            opened_rows[i].playing = id && !strcmp(opened[i].id, id);
    ui_list *ctx_list = &xmb.lists[UI_CAT_RECENT][1];
    if (ctx_shown_seq == ctx_done_seq)
        for (int i = 0; i < ctx_list->count; ++i)
            ctx_rows[i].playing = id && !strcmp(ctx_tracks[i].id, id);
    ui_list *results = &xmb.lists[UI_CAT_SEARCH][0];
    if (search_shown_seq == search_done_seq)
        for (int i = 0; i < results->count; ++i)
            found_rows[i].playing = id && !strcmp(found[i].id, id);
}

/* Picks up finished list loads and covers, and mirrors the player state. */
static void sync_state(void)
{
    if (liked_ready > liked_shown) {
        const int ready = liked_ready;
        track_rows(&liked[liked_shown], ready - liked_shown, &liked_rows[liked_shown]);
        xmb.lists[UI_CAT_LIKED][0].rows = liked_rows;
        xmb.lists[UI_CAT_LIKED][0].count = ready;
        liked_shown = ready;
        const int playing = current_index;
        if (playing >= 0 && playing < queue_len) mark_playing(queue[playing].id);
    }
    xmb.lists[UI_CAT_LIKED][0].loading = !liked_done;
    ui_list *open_list = &xmb.lists[UI_CAT_PLAYLISTS][1];
    if (open_done_seq == open_req_seq && open_shown_seq != open_done_seq) {
        track_rows(opened, open_done_count, opened_rows);
        open_list->rows = opened_rows;
        open_list->count = open_done_count;
        open_list->loading = 0;
        open_shown_seq = open_done_seq;
    }

    if (xmb.cat == UI_CAT_RECENT && !recent_requested) {
        recent_requested = 1;
        xmb.lists[UI_CAT_RECENT][0].loading = 1;
    }
    if (recent_done && !recent_shown) {
        for (int i = 0; i < recent_count; ++i) {
            ui_row *r = &recent_rows[i];
            const char *uri = recent[i].uri;
            memset(r, 0, sizeof *r);
            copy_text(r->title, sizeof r->title, recent[i].name[0] ? recent[i].name : uri);
            copy_text(r->subtitle, sizeof r->subtitle,
                      is_collection(uri) ? "Your Liked Songs"
                      : !strncmp(uri, "spotify:playlist:", 17) ? "Playlist"
                      : !strncmp(uri, "spotify:album:", 14) ? "Album" : "Artist");
            r->color = row_color(uri);
            r->round = !strncmp(uri, "spotify:artist:", 15);
        }
        xmb.lists[UI_CAT_RECENT][0].rows = recent_rows;
        xmb.lists[UI_CAT_RECENT][0].count = recent_count;
        xmb.lists[UI_CAT_RECENT][0].loading = 0;
        recent_shown = 1;
    }
    if (ctx_done_seq == ctx_req_seq && ctx_shown_seq != ctx_done_seq) {
        track_rows(ctx_tracks, ctx_done_count, ctx_rows);
        xmb.lists[UI_CAT_RECENT][1].rows = ctx_rows;
        xmb.lists[UI_CAT_RECENT][1].count = ctx_done_count;
        xmb.lists[UI_CAT_RECENT][1].loading = 0;
        ctx_shown_seq = ctx_done_seq;
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
        n->remote = connect_online();
        n->dur_ms = t->duration_ms;
        n->pos_ms = control.position_ms;
        n->paused = control.paused;
        n->loading = play_state == PS_LOADING && control.position_ms == 0;
        if (play_state == PS_ERROR)
            copy_text(n->album, sizeof n->album,
                      last_error == PLAYER_ERR_NO_MEMORY ? "Couldn't play: not enough memory"
                      : last_error == SESSION_KEY_REFUSED ? "Spotify refused this track for this account"
                      : last_error <= -4 && last_error > -10
                          ? "Couldn't get this track from Spotify"
                          : last_error == -2 ? "No playable version of this track"
                          : "Couldn't play this track");
        else
            snprintf(n->album, sizeof n->album, "From %s", queue_name);
        int upcoming = next_index(idx, 1);
        copy_text(n->next, sizeof n->next,
                  upcoming >= 0 && upcoming != idx ? queue[upcoming].name : "");
        n->shuffle = prefs[PREF_SHUFFLE];
        /* Bars jump up with the music and fall back slowly. */
        for (int b = 0; b < PLAYER_BANDS; ++b) {
            const float level = control.paused ? 0.0f : control.spectrum[b];
            n->spectrum[b] = level > n->spectrum[b] ? level : n->spectrum[b] * 0.9f;
        }
        n->repeat = prefs[PREF_REPEAT];
        if (queue_version != queue_shown_version || idx != queue_shown_current) {
            queue_shown_version = queue_version;
            queue_shown_current = idx;
            ui_list *ql = &xmb.lists[UI_CAT_QUEUE][0];
            int count = 0;
            for (int at = order_position(idx) + 1; at < order_len && count < QUEUE_VIEW; ++at) {
                const int qi = order[at];
                queue_view[count] = qi;
                track_rows(&queue[qi], 1, &queue_rows[count]);
                if (queued[qi]) {
                    char sub[96];
                    snprintf(sub, sizeof sub, "Queued \xC2\xB7 %.80s", queue[qi].artist);
                    copy_text(queue_rows[count].subtitle, sizeof queue_rows[count].subtitle, sub);
                }
                ++count;
            }
            snprintf(ql->header, sizeof ql->header, "Up next \xC2\xB7 %d songs", count);
            ql->rows = queue_rows;
            ql->count = count;
            ql->movable = 1;
            if (xmb.sel[UI_CAT_QUEUE][0] >= count) xmb.sel[UI_CAT_QUEUE][0] = count ? count - 1 : 0;
        }
        n->seeking = seek_preview_ms >= 0;
        if (n->seeking) n->pos_ms = (unsigned int)seek_preview_ms;
    }

    if (cover_pending) {
        if (!strcmp(cover_id, shown_id)) {
            memcpy(cover_keep, cover_argb, sizeof cover_keep);
            cover_keep_valid = 1;
            rebuild_cover();
            copy_text(cover_tex_id, sizeof cover_tex_id, cover_id);
            have_palette = 1;
            retint(0);
            if (prefs[PREF_BACKGROUND] == BACKGROUND_BLURRED_COVER) update_backdrop();
            fill_settings();
        }
        cover_pending = 0;
    }
    n->cover = cover_tex && !strcmp(cover_tex_id, shown_id) ? cover_tex : NULL;

    if (like_done_seq != like_seen_seq && like_done_seq == like_req_seq) {
        like_seen_seq = like_done_seq;
        if (like_status == 200) {
            ui_toast(&xmb, like_remove ? "Removed from Liked Songs" : "Saved to Liked Songs");
            /* Keep the list on screen in step once it is fully loaded. */
            if (liked_done) {
                int at = -1;
                for (int i = 0; i < liked_shown && at < 0; ++i)
                    if (!strcmp(liked[i].id, like_track.id)) at = i;
                if (like_remove && at >= 0) {
                    const size_t tail = (size_t)(liked_shown - at - 1);
                    memmove(&liked[at], &liked[at + 1], sizeof liked[0] * tail);
                    memmove(&liked_rows[at], &liked_rows[at + 1], sizeof liked_rows[0] * tail);
                    liked_count = liked_count - 1;
                    liked_ready = liked_ready - 1;
                    --liked_shown;
                } else if (!like_remove && at < 0 && liked_shown < MAX_TRACKS) {
                    const size_t tail = (size_t)liked_shown;
                    memmove(&liked[1], &liked[0], sizeof liked[0] * tail);
                    memmove(&liked_rows[1], &liked_rows[0], sizeof liked_rows[0] * tail);
                    liked[0] = like_track;
                    track_rows(&liked[0], 1, &liked_rows[0]);
                    liked_count = liked_count + 1;
                    liked_ready = liked_ready + 1;
                    ++liked_shown;
                }
                xmb.lists[UI_CAT_LIKED][0].count = liked_shown;
            }
        } else {
            ui_toast(&xmb, "Couldn't update Liked Songs");
        }
    }

    site_finished();
    if (list_ended) {
        list_ended = 0;
        ui_toast(&xmb, "End of the list");
    }
    if (keys_refused) {
        keys_refused = 0;
        ui_toast(&xmb, "Spotify refuses keys to this account");
    }
    if (open_group == PREF_GROUP_ABOUT && update_state == UPDATE_INSTALLING) fill_options();
    if (open_group == PREF_GROUP_GALLERY && gallery_count >= 0 && xmb.lists[UI_CAT_SETTINGS][1].loading)
        fill_options();

    upload_thumb();
    assign_thumbs();

    ScePspDateTime now;
    if (prefs[PREF_CLOCK] == CLOCK_HIDDEN) {
        xmb.clock[0] = '\0';
    } else if (sceRtcGetCurrentClockLocalTime(&now) == 0) {
        const int hour = now.hour % 24, minute = now.minute % 60;
        if (prefs[PREF_CLOCK] == CLOCK_12H)
            snprintf(xmb.clock, sizeof xmb.clock, "%d:%02d %s", hour % 12 ? hour % 12 : 12, minute,
                     hour < 12 ? "AM" : "PM");
        else
            snprintf(xmb.clock, sizeof xmb.clock, "%02d:%02d", hour, minute);
    }
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
    params.base.buttonSwap = prefs[PREF_CONFIRM] ? PSP_UTILITY_ACCEPT_CIRCLE : PSP_UTILITY_ACCEPT_CROSS;
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
    skip_frame_timing = 1;
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

/* ------------------------------------------------------------ Spotify Connect */

/* A new queue key for every Connect play: the apps may resend a context. */
static int connect_key = -100000;

static void handle_connect(void)
{
    connect_command c;
    if (!connect_poll(&c)) return;
    switch (c.type) {
    case CONNECT_PLAY:
        if (c.shuffle >= 0) prefs[PREF_SHUFFLE] = c.shuffle;
        if (c.repeat >= 0) prefs[PREF_REPEAT] = c.repeat;
        start_at_ms = c.position_ms;
        start_paused = c.paused;
        play_from(c.tracks, c.count, c.index, connect_key--, "Spotify Connect", c.context_uri);
        connect_command_done();
        break;
    case CONNECT_PAUSE:
    case CONNECT_RELEASE:
        control.paused = 1;
        break;
    case CONNECT_RESUME:
        control.paused = 0;
        break;
    case CONNECT_NEXT:
        start_track(next_index(current_index, 0));
        break;
    case CONNECT_PREV:
        start_track(control.position_ms > 3000 ? current_index : prev_index(current_index));
        break;
    case CONNECT_SEEK:
        control.seek_ms = c.value;
        break;
    case CONNECT_SHUFFLE:
        prefs[PREF_SHUFFLE] = c.value != 0;
        if (current_index >= 0) build_order(current_index);
        break;
    case CONNECT_REPEAT_CONTEXT:
        prefs[PREF_REPEAT] = c.value ? 1 : (prefs[PREF_REPEAT] == 2 ? 2 : 0);
        break;
    case CONNECT_REPEAT_TRACK:
        prefs[PREF_REPEAT] = c.value ? 2 : (prefs[PREF_REPEAT] == 2 ? 0 : prefs[PREF_REPEAT]);
        break;
    case CONNECT_VOLUME:
        set_volume(c.value);
        break;
    }
}

static void report_connect(void)
{
    connect_status s;
    memset(&s, 0, sizeof s);
    const int idx = current_index;
    s.active = idx >= 0 && idx < queue_len && play_state != PS_IDLE;
    if (s.active) {
        copy_text(s.track_id, sizeof s.track_id, queue[idx].id);
        copy_text(s.context_uri, sizeof s.context_uri, queue_uri);
        s.position_ms = control.position_ms;
        s.duration_ms = queue[idx].duration_ms;
        s.playing = !control.paused;
    }
    s.shuffle = prefs[PREF_SHUFFLE];
    s.repeat = prefs[PREF_REPEAT];
    s.volume = xmb.volume;
    connect_report(&s);
}

/* ------------------------------------------------------------ △ menu */

/* The song behind a row, or NULL for rows that are not songs. */
static const spotify_track *row_track(int cat, int level, int i)
{
    if (i < 0) return NULL;
    if (cat == UI_CAT_LIKED && i < liked_shown) return &liked[i];
    if (cat == UI_CAT_PLAYLISTS && level == 1 && open_shown_seq == open_done_seq &&
        i < open_done_count)
        return &opened[i];
    if (cat == UI_CAT_SEARCH && search_shown_seq == search_done_seq && i < search_done_count)
        return &found[i];
    if (cat == UI_CAT_RECENT && level == 1 && ctx_shown_seq == ctx_done_seq && i < ctx_done_count)
        return &ctx_tracks[i];
    if (cat == UI_CAT_QUEUE && i < xmb.lists[UI_CAT_QUEUE][0].count) return &queue[queue_view[i]];
    return NULL;
}

static int is_liked(const char *id)
{
    for (int i = 0; i < liked_shown; ++i)
        if (!strcmp(liked[i].id, id)) return 1;
    return 0;
}

static void options_menu(const ui_event *e)
{
    if (e->category == UI_CAT_PLAYLISTS && e->level == 0) {
        if (e->index < 0 || e->index >= playlist_count) return;
        menu_playlist = playlist_view[e->index];
        const int on = pin_index(playlists[menu_playlist].id) >= 0;
        const char *item = on ? "Unpin" : "Pin to the top";
        menu_actions[0] = on ? MENU_UNPIN : MENU_PIN;
        ui_menu_open(&xmb, &item, 1);
        return;
    }
    const spotify_track *t = row_track(e->category, e->level, e->index);
    if (!t) return;
    menu_track = *t;
    menu_queue_row = e->index;
    const char *items[UI_MENU_ITEMS];
    int n = 0;
    if (e->category == UI_CAT_QUEUE) {
        items[n] = "Play now";
        menu_actions[n++] = MENU_PLAY_NOW;
        items[n] = "Remove from queue";
        menu_actions[n++] = MENU_REMOVE;
    } else {
        items[n] = "Add to queue";
        menu_actions[n++] = MENU_ADD;
    }
    if (is_liked(t->id)) {
        items[n] = "Remove from Liked Songs";
        menu_actions[n++] = MENU_UNLIKE;
    } else {
        items[n] = "Save to Liked Songs";
        menu_actions[n++] = MENU_LIKE;
    }
    ui_menu_open(&xmb, items, n);
}

static void menu_choice(const ui_event *e)
{
    if (e->value < 0 || e->value >= UI_MENU_ITEMS) return;
    switch (menu_actions[e->value]) {
    case MENU_UPDATE:
        if (site_request(SITE_INSTALL, 0)) ui_toast(&xmb, "Downloading the update\xE2\x80\xA6");
        else ui_toast(&xmb, "Still busy: try in Settings \xE2\x86\x92 About");
        fill_settings();
        break;
    case MENU_UPDATE_LATER:
        ui_toast(&xmb, "Settings \xE2\x86\x92 About installs it later");
        break;
    case MENU_QUIT:
        quit_requested = 1;
        break;
    case MENU_QUIT_LATER:
        ui_toast(&xmb, "The new version starts next time");
        break;
    case MENU_ADD:
        if (current_index < 0) {
            /* Nothing playing yet: just play it. */
            play_from(&menu_track, 1, 0, -3000 - (int)queue_version, "Queue", "");
            ui_toast(&xmb, "Playing");
        } else if (queue_add(&menu_track) == 0) {
            ui_toast(&xmb, "Added to queue");
        } else {
            ui_toast(&xmb, "The queue is full");
        }
        break;
    case MENU_LIKE:
    case MENU_UNLIKE:
        if (like_req_seq != like_done_seq) {
            ui_toast(&xmb, "Still saving the last change");
            break;
        }
        like_track = menu_track;
        like_remove = menu_actions[e->value] == MENU_UNLIKE;
        ++like_req_seq;
        break;
    case MENU_PLAY_NOW:
        if (menu_queue_row < xmb.lists[UI_CAT_QUEUE][0].count)
            start_track(queue_view[menu_queue_row]);
        break;
    case MENU_REMOVE:
        queue_remove(menu_queue_row);
        ui_toast(&xmb, "Removed from queue");
        break;
    case MENU_PIN:
    case MENU_UNPIN: {
        const char *id = playlists[menu_playlist].id;
        const int at = pin_index(id);
        if (menu_actions[e->value] == MENU_UNPIN && at >= 0) {
            memmove(pinned[at], pinned[at + 1], sizeof pinned[0] * (size_t)(pinned_count - at - 1));
            --pinned_count;
            ui_toast(&xmb, "Unpinned");
        } else if (at < 0 && pinned_count < MAX_PINS) {
            copy_text(pinned[pinned_count++], sizeof pinned[0], id);
            ui_toast(&xmb, "Pinned to the top");
        } else if (at < 0) {
            ui_toast(&xmb, "Up to 16 pins");
        }
        arrange_playlists();
        /* Keep the selection on the playlist that moved. */
        for (int i = 0; i < playlist_count; ++i)
            if (playlist_view[i] == menu_playlist) xmb.sel[UI_CAT_PLAYLISTS][0] = i;
        save_settings();
        break;
    }
    }
}

/* Starts a theme gallery or update job; returns 0 if one is still running. */
static int site_request(int job, int arg)
{
    if (site_req_seq != site_done_seq) return 0;
    site_job = job;
    site_arg = arg;
    if (job == SITE_CHECK) update_state = UPDATE_CHECKING;
    if (job == SITE_INSTALL) {
        update_progress = 0;
        update_state = UPDATE_INSTALLING;
    }
    ++site_req_seq;
    return 1;
}

/* Picks up a finished site job. UI thread. */
static void site_finished(void)
{
    if (site_done_seq == site_seen_seq || site_done_seq != site_req_seq) return;
    site_seen_seq = site_done_seq;
    switch (site_job) {
    case SITE_THEME:
        if (site_result == 0) {
            const char *folder = gallery[site_arg].folder;
            scan_themes();
            for (int i = 0; i < theme_count; ++i)
                if (!strcmp(theme_names[i], folder)) apply_theme(i, 0);
            save_settings();
            ui_toast(&xmb, "Theme installed");
        } else {
            ui_toast(&xmb, "Couldn't download the theme");
        }
        break;
    case SITE_CHECK:
        if (update_state == UPDATE_AVAILABLE && update_auto && !xmb.menu.open && !xmb.moving) {
            /* The check at start found one: ask right away. */
            static const char *const items[] = { "Install now", "Not now" };
            menu_actions[0] = MENU_UPDATE;
            menu_actions[1] = MENU_UPDATE_LATER;
            ui_menu_open(&xmb, items, 2);
            snprintf(xmb.menu.title, sizeof xmb.menu.title, "Update %.15s is out", update_info.version);
        } else if (update_state == UPDATE_AVAILABLE) {
            char note[64];
            snprintf(note, sizeof note, "Update %.15s: Settings \xE2\x86\x92 About", update_info.version);
            ui_toast(&xmb, note);
        } else if (!update_auto) {
            ui_toast(&xmb, update_state == UPDATE_CURRENT ? "Up to date" : "Couldn't check for updates");
        }
        update_auto = 0;
        break;
    case SITE_INSTALL:
        if (update_state == UPDATE_INSTALLED && !xmb.menu.open && !xmb.moving) {
            static const char *const items[] = { "Quit now and start it again", "Later" };
            menu_actions[0] = MENU_QUIT;
            menu_actions[1] = MENU_QUIT_LATER;
            ui_menu_open(&xmb, items, 2);
            snprintf(xmb.menu.title, sizeof xmb.menu.title, "%.15s is installed", update_info.version);
        } else {
            ui_toast(&xmb, update_state == UPDATE_INSTALLED ? "Updated: the new version starts next time"
                                                            : "Update failed, nothing changed");
        }
        break;
    default:
        break;
    }
    fill_settings();
}

/* ✕ on a row of the theme gallery or About. */
static void site_open(int index)
{
    if (open_group == PREF_GROUP_GALLERY) {
        if (gallery_count == -2) {
            gallery_count = -1;
            site_request(SITE_GALLERY, 0);
        } else if (index >= 0 && index < xmb.lists[UI_CAT_SETTINGS][1].count) {
            const int i = option_view[index];
            int installed = 0;
            for (int k = 0; k < theme_count; ++k) installed |= !strcmp(theme_names[k], gallery[i].folder);
            if (!installed && theme_count >= MAX_THEMES)
                ui_toast(&xmb, "16 themes at most: remove one from THEMES");
            else if (!site_request(SITE_THEME, i))
                ui_toast(&xmb, "Still busy, try again in a moment");
        }
    } else if (open_group == PREF_GROUP_ABOUT && index == 0) {
        const int s = update_state;
        if (s == UPDATE_INSTALLED) quit_requested = 1;
        else if (s == UPDATE_AVAILABLE || s == UPDATE_INSTALL_FAILED) site_request(SITE_INSTALL, 0);
        else if (s == UPDATE_UNKNOWN || s == UPDATE_CURRENT || s == UPDATE_FAILED) site_request(SITE_CHECK, 0);
    } else if (open_group == PREF_GROUP_ABOUT && index == 1) {
        prefs_step(PREF_CHECK_UPDATES, 1);
        save_settings();
    }
    fill_settings();
}

/* A setting changed: make it take effect now. */
static void pref_changed(pref_id id)
{
    switch (id) {
    case PREF_COVER_COLORS:
        retint(0);
        break;
    case PREF_BACKGROUND:
        update_backdrop();
        if (prefs[id] == BACKGROUND_WALLPAPER && !base_theme.wallpaper[0])
            ui_toast(&xmb, "This theme has no wallpaper");
        else if (prefs[id] == BACKGROUND_BLURRED_COVER && !cover_keep_valid)
            ui_toast(&xmb, "Shows once a song with a cover plays");
        break;
    case PREF_COVER_SHAPE:
        rebuild_cover();
        break;
    case PREF_VISUALIZER:
    case PREF_TEXT_SIZE:
    case PREF_DENSITY:
    case PREF_ANIMATIONS:
    case PREF_HINTS:
    case PREF_NOW_LAYOUT:
        apply_style();
        break;
    case PREF_PLAYLIST_SORT:
        arrange_playlists();
        break;
    case PREF_EQUALIZER:
    case PREF_NORMALIZE:
        apply_equalizer();
        break;
    case PREF_EQ_60:
    case PREF_EQ_230:
    case PREF_EQ_910:
    case PREF_EQ_3600:
    case PREF_EQ_14K:
        /* Shaping a band means the user wants their own curve. */
        prefs[PREF_EQUALIZER] = PLAYER_EQ_CUSTOM;
        apply_equalizer();
        break;
    case PREF_CPU:
        apply_cpu();
        break;
    case PREF_SLEEP:
        sleep_deadline = prefs[id] ? sceKernelGetSystemTimeWide() +
                         (SceUInt64)sleep_minutes[prefs[id]] * 60000000u : 0;
        if (prefs[id])
            ui_toast(&xmb, prefs[PREF_SLEEP_ACTION] == SLEEP_POWER_OFF
                     ? "The PSP turns off when the timer ends" : "Playback pauses when the timer ends");
        break;
    case PREF_CONFIRM:
    case PREF_LR:
    case PREF_VOLUME_STEP:
        apply_style();
        break;
    case PREF_CONNECT:
        if (prefs[id]) connect_start(&ctx, connect_name());
        else connect_stop();
        log_printf("UI: Spotify Connect %s\n", prefs[id] ? "on" : "off");
        break;
    default:
        break;
    }
}

/* ←/→ or ✕ on a row of an open Settings group. */
static void settings_adjust(const ui_event *e)
{
    if (e->category != UI_CAT_SETTINGS || e->level != 1 || open_group < 0) return;
    if (e->index < 0 || e->index >= xmb.lists[UI_CAT_SETTINGS][1].count) return;
    const int v = option_view[e->index];
    if (open_group == PREF_GROUP_CATEGORIES) {
        if (v == UI_CAT_SETTINGS) {
            ui_toast(&xmb, "Settings stays, so you can come back");
            return;
        }
        cat_hidden[v] = !cat_hidden[v];
        apply_categories();
    } else {
        prefs_step((pref_id)v, e->value < 0 ? -1 : 1);
        pref_changed((pref_id)v);
    }
    fill_settings();
    save_settings();
}

/* Returns 1 when the user asked to quit. */
static int handle(const ui_event *e)
{
    switch (e->type) {
    case UI_EV_QUIT:
        return 1;
    case UI_EV_OPEN:
        if (e->category == UI_CAT_LIKED && liked_shown > 0) {
            static char uri[sizeof ctx.cfg.username + 32];
            snprintf(uri, sizeof uri, "spotify:user:%s:collection", ctx.cfg.username);
            play_from(liked, liked_shown, e->index, -1, "Liked Songs", uri);
        } else if (e->category == UI_CAT_PLAYLISTS && e->level == 0) {
            if (e->index < 0 || e->index >= playlist_count) break;
            int p = playlist_view[e->index];
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
            {
                char uri[48];
                snprintf(uri, sizeof uri, "spotify:playlist:%s", playlists[open_done_playlist].id);
                play_from(opened, open_done_count, e->index, open_done_playlist,
                          playlists[open_done_playlist].name, uri);
            }
        } else if (e->category == UI_CAT_RECENT && e->level == 0) {
            ui_list *l = &xmb.lists[UI_CAT_RECENT][1];
            copy_text(l->header, sizeof l->header, recent_rows[e->index].title);
            l->rows = ctx_rows;
            l->count = 0;
            l->loading = 1;
            ui_open_level(&xmb, UI_CAT_RECENT);
            ctx_req_index = e->index;
            ++ctx_req_seq;
        } else if (e->category == UI_CAT_RECENT) {
            if (ctx_req_seq == ctx_done_seq && ctx_shown_seq == ctx_done_seq)
                play_from(ctx_tracks, ctx_done_count, e->index, -2000 - (int)ctx_done_seq,
                          recent_rows[ctx_req_index].title, recent[ctx_req_index].uri);
        } else if (e->category == UI_CAT_QUEUE) {
            if (e->index >= 0 && e->index < xmb.lists[UI_CAT_QUEUE][0].count)
                start_track(queue_view[e->index]);
        } else if (e->category == UI_CAT_SEARCH) {
            if (search_done_seq == search_req_seq && search_shown_seq == search_done_seq)
                play_from(found, search_done_count, e->index, -10 - (int)search_done_seq,
                          "Search", "");
        } else if (e->category == UI_CAT_SETTINGS && e->level == 0) {
            if (e->index < 0 || e->index >= group_count) break;
            if (group_view[e->index] == GROUP_QUIT) return 1;
            open_group = group_view[e->index];
            ui_open_level(&xmb, UI_CAT_SETTINGS);
            if (open_group == PREF_GROUP_GALLERY && gallery_count < 0) {
                gallery_count = -1;
                site_request(SITE_GALLERY, 0);
            }
            fill_options();
        } else if (e->category == UI_CAT_SETTINGS &&
                   (open_group == PREF_GROUP_GALLERY || open_group == PREF_GROUP_ABOUT)) {
            site_open(e->index);
            if (quit_requested) return 1;
        } else if (e->category == UI_CAT_SETTINGS && open_group == PREF_GROUP_THEMES) {
            if (e->index < 0 || e->index >= theme_count) break;
            apply_theme(option_view[e->index], 0);
            save_settings();
        }
        break;
    case UI_EV_BACK:
        if (e->category == UI_CAT_SETTINGS) open_group = -1;
        break;
    case UI_EV_ADJUST:
        settings_adjust(e);
        break;
    case UI_EV_PAUSE:
        control.paused = !control.paused;
        break;
    case UI_EV_NEXT: {
        const int next = next_index(current_index, 0);
        if (next < 0 && current_index >= 0) ui_toast(&xmb, "End of the list");
        start_track(next);
        break;
    }
    case UI_EV_PREV:
        /* Restart the song if it has been playing a while, else go back. */
        start_track(control.position_ms > 3000 ? current_index : prev_index(current_index));
        break;
    case UI_EV_VOLUME:
        set_volume(e->value);
        break;
    case UI_EV_SEARCH:
        start_search();
        break;
    case UI_EV_OPTIONS:
        options_menu(e);
        break;
    case UI_EV_MENU:
        menu_choice(e);
        if (quit_requested) return 1;
        break;
    case UI_EV_MOVE:
        if (e->category == UI_CAT_QUEUE) queue_move(e->index, e->value);
        if (e->category == UI_CAT_SETTINGS && open_group == PREF_GROUP_CATEGORIES) {
            const int at = e->index, to = e->index + e->value;
            if (at >= 0 && to >= 0 && at < UI_CATS && to < UI_CATS) {
                const int c = cat_all[at];
                cat_all[at] = cat_all[to];
                cat_all[to] = c;
                apply_categories();
                fill_options();
                save_settings();
            }
        }
        break;
    case UI_EV_SHUFFLE:
        prefs[PREF_SHUFFLE] = !prefs[PREF_SHUFFLE];
        if (current_index >= 0) build_order(current_index);
        save_settings();
        break;
    case UI_EV_REPEAT:
        prefs[PREF_REPEAT] = (prefs[PREF_REPEAT] + 1) % 3;
        save_settings();
        break;
    case UI_EV_SEEK: {
        if (current_index < 0) break;
        if (e->value == 0) {
            /* Released: jump to the previewed time. */
            if (seek_preview_ms >= 0) control.seek_ms = seek_preview_ms;
            seek_preview_ms = -1;
            seek_steps = 0;
            break;
        }
        if (seek_preview_ms < 0) seek_preview_ms = (int)control.position_ms;
        /* 5 s steps, 15 s once held for a couple of seconds. */
        seek_preview_ms += e->value * (seek_steps++ < 8 ? 5000 : 15000);
        const int end = (int)queue[current_index].duration_ms - 2000;
        if (seek_preview_ms > end) seek_preview_ms = end;
        if (seek_preview_ms < 0) seek_preview_ms = 0;
        break;
    }
    default:
        break;      /* search and seeking come later */
    }
    return 0;
}

/* Play/pause and next/previous on the headphone remote (PSP-2000 and later).
 * Its volume keys already set the system volume. */
static void handle_remote(void)
{
    static u32 held;
    u32 key = 0;
    if (!prefs[PREF_REMOTE] || sceHprmPeekCurrentKey(&key) < 0) return;
    const u32 pressed = key & ~held;
    held = key;
    if (key & PSP_HPRM_HOLD) return;
    if ((pressed & PSP_HPRM_PLAYPAUSE) && current_index >= 0) control.paused = !control.paused;
    if (pressed & PSP_HPRM_FORWARD) start_track(next_index(current_index, 0));
    if (pressed & PSP_HPRM_BACK)
        start_track(control.position_ms > 3000 ? current_index : prev_index(current_index));
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
    if (pad.Buttons & PSP_CTRL_SELECT) b |= UI_BTN_SELECT;
    return b;
}

static int pin_index(const char *id)
{
    for (int i = 0; i < pinned_count; ++i)
        if (!strcmp(pinned[i], id)) return i;
    return -1;
}

/* Case-insensitive for ASCII; other bytes compare as they are. */
static int name_order(const char *a, const char *b)
{
    for (;; ++a, ++b) {
        int ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb || !ca) return ca - cb;
    }
}

/* 1 when playlist a goes before b: pinned ones first (in pin order), then
 * by the chosen sort. */
static int playlist_before(int a, int b)
{
    const spotify_playlist *pa = &playlists[a], *pb = &playlists[b];
    int ia = pin_index(pa->id), ib = pin_index(pb->id);
    if (ia >= 0 || ib >= 0) {
        if (ia < 0) return 0;
        if (ib < 0) return 1;
        return ia < ib;
    }
    switch (prefs[PREF_PLAYLIST_SORT]) {
    case SORT_NAME: return name_order(pa->name, pb->name) < 0;
    case SORT_SIZE: return pa->total > pb->total;
    default: return a < b;
    }
}

/* Lists the playlists in the chosen order; rows hold no covers until the
 * thumbnails are assigned again on the next frame. */
static void arrange_playlists(void)
{
    for (int i = 0; i < playlist_count; ++i) {
        int p = i + 1, j = i;
        while (j > 0 && playlist_before(p, playlist_view[j - 1])) {
            playlist_view[j] = playlist_view[j - 1];
            --j;
        }
        playlist_view[j] = p;
    }
    for (int i = 0; i < playlist_count; ++i) {
        const spotify_playlist *p = &playlists[playlist_view[i]];
        ui_row *r = &playlist_rows[i];
        memset(r, 0, sizeof *r);
        copy_text(r->title, sizeof r->title, p->name);
        const char *pin = pin_index(p->id) >= 0 ? "Pinned \xC2\xB7 " : "";
        if (p->total >= 0)
            snprintf(r->subtitle, sizeof r->subtitle, "%sPlaylist \xC2\xB7 %d songs", pin, p->total);
        else
            snprintf(r->subtitle, sizeof r->subtitle, "%sPlaylist", pin);
        r->color = row_color(p->name);
    }
    xmb.lists[UI_CAT_PLAYLISTS][0].rows = playlist_rows;
    xmb.lists[UI_CAT_PLAYLISTS][0].count = playlist_count;
}

static void load_playlists(void)
{
    frame_message("Loading your library\xE2\x80\xA6", NULL);
    int n = spotify_sp_playlists(ctx.client_token, ctx.access_token, ctx.cfg.username,
                                 &playlists[1], MAX_PLAYLISTS);
    playlist_count = n > 0 ? n : 0;
    arrange_playlists();
    log_printf("UI: %d playlists\n", n);
}

static int setup_ui(void)
{
    log_printf("UI: setup: graphics\n");
    render_gu_init();
    log_printf("UI: setup: fonts\n");
    if (font_load(FONT_REGULAR, ui_font_regular, ui_font_regular_len) < 0 ||
        font_load(FONT_BOLD, ui_font_bold, ui_font_bold_len) < 0 ||
        font_load(FONT_XBOLD, ui_font_xbold, ui_font_xbold_len) < 0 ||
        font_load(FONT_ICONS, ui_font_icons, ui_font_icons_len) < 0) {
        log_printf("UI: font load failed\n");
        return -1;
    }
    log_printf("UI: setup: interface\n");
    if (ui_init(&xmb) < 0) {
        log_printf("UI: init failed\n");
        return -1;
    }
    log_printf("UI: setup: font atlases\n");
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
    log_printf("UI: setup: settings\n");
    xmb.lists[UI_CAT_LIKED][0].loading = 1;
    load_settings();
    return 0;
}

/* Clears the FPU's exception enables (FCSR bits 7-11) for this thread.
 * stb_truetype hits invalid operations on some glyph curves at some sizes
 * (Lucide's disc-3 at 16 px, for one); a PC gets a NaN, but a trap on the
 * PSP took the whole console down while the font atlases were baked. */
static void fpu_no_traps(void)
{
    unsigned int fcsr;
    __asm__ volatile("cfc1 %0, $31" : "=r"(fcsr));
    fcsr &= ~0x0003FF80u;   /* the enables, and any cause bits already set */
    __asm__ volatile("ctc1 %0, $31" : : "r"(fcsr));
}

int spotify_ui_run(volatile int *exit_requested)
{
    fpu_no_traps();
    log_set_echo(0);   /* the debug console would scribble over our frames */
    /* Full speed for the whole session: at 222 MHz each TLS handshake took
     * 0.6-1.3 s on hardware (about 0.25 s at 333 MHz). */
    scePowerSetClockFrequency(333, 333, 166);
    if (setup_ui() < 0) return 0;
    memory_log("ui-ready");

    spotify_ui_hooks hooks = { on_status, on_pair_code };
    int ret = spotify_ctx_start(&ctx, &hooks);
    if (ret < 0) {
        frame_message("Could not sign in to Spotify", log_enabled()
                      ? "See spotify.log. START: exit"
                      : "debug = on in settings.ini logs it. START: exit");
        SceCtrlData pad;
        do {
            sceCtrlPeekBufferPositive(&pad, 1);
            sceKernelDelayThreadCB(50000);
        } while (!(pad.Buttons & PSP_CTRL_START) && !*exit_requested);
        render_gu_term();
        return 0;
    }
    set_volume(80);
    apply_cpu();
    log_set_buffered(1);
    load_playlists();
    if (prefs[PREF_CHECK_UPDATES]) {
        update_auto = 1;
        site_request(SITE_CHECK, 0);
    }
    if (prefs[PREF_CONNECT]) connect_start(&ctx, connect_name());

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

    SceUInt64 last = sceKernelGetSystemTimeWide(), stats_start = last, last_input = last;
    unsigned idle_frames = 0;
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
            unsigned int conn_new, conn_reused, tls_full, tls_resumed;
            http_stats(&conn_new, &conn_reused);
            tls_stats(&tls_full, &tls_resumed);
            log_printf("UI: %u frames in 10 s, %u slow (>20 ms), worst %u ms; "
                       "connections %u new, %u reused; TLS %u full, %u resumed\n", frames,
                       slow_frames, worst_us / 1000u, conn_new, conn_reused, tls_full, tls_resumed);
            stats_start = now;
            frames = slow_frames = worst_us = 0;
        }

        const unsigned buttons = read_buttons();
        if (buttons) last_input = now;
        ui_update(&xmb, buttons, dt);
        handle_remote();
        if (sleep_deadline && now >= sleep_deadline) {
            sleep_deadline = 0;
            prefs[PREF_SLEEP] = 0;
            control.paused = 1;
            if (prefs[PREF_SLEEP_ACTION] == SLEEP_POWER_OFF) {
                /* Like holding the power switch: the console shuts down. */
                log_printf("UI: sleep timer: turning the PSP off\n");
                log_flush();
                frame_message("Good night", NULL);
                scePowerRequestStandby();
            }
            ui_toast(&xmb, "Sleep timer: paused");
            fill_settings();
        }
        for (int i = 0; i < xmb.event_count && !quit; ++i) quit = handle(&xmb.events[i]);
        if (skip_frame_timing) {
            skip_frame_timing = 0;
            last = sceKernelGetSystemTimeWide();
        }
        handle_connect();
        sync_state();
        report_connect();
        log_tick();

        /* Battery saver: after 20 s without a button, draw every third frame. */
        const int idle = prefs[PREF_BATTERY] && now - last_input > 20000000u && !xmb.menu.open;
        if (idle && idle_frames++ % 3 != 0) {
            sceDisplayWaitVblankStartCB();
            continue;
        }
        if (!idle) idle_frames = 0;
        render_gu_begin();
        if (prebake_pending) {
            prebake_pending = 0;
            ui_prebake(&xmb);
            skip_frame_timing = 1;
        }
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
    connect_stop();
    spotify_ctx_stop(&ctx);
    if (thumb_lock >= 0) sceKernelDeleteSema(thumb_lock);
    log_set_buffered(0);
    render_gu_term();
    return me_dead ? PLAYER_ERR_ME_TIMEOUT : 0;
}
