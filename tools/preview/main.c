/* PSPotify Preview: runs the PSP UI (ui/) on a PC with mock data.
 *
 *   PSPotifyPreview                     window, keyboard = PSP buttons
 *   PSPotifyPreview --theme Daylight    start with themes/Daylight/theme.ini
 *   PSPotifyPreview --shot out.png --keys right,down,cross
 *                                       press the keys, save a 480x272 PNG
 *
 * Keys: arrows = D-pad, Z/Enter = cross, X/Backspace = circle, A = square,
 * S = triangle, Q = L, E = R, Tab = SELECT, Esc = START. The active theme.ini is reloaded
 * as soon as it is saved. */
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#endif
#include "font.h"
#include "palette.h"
#include "render_sdl.h"
#include "ui.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define W 480
#define H 272

static char root[1024];

/* ------------------------------------------------------------ files */

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = size > 0 ? malloc((size_t)size + 1) : NULL;
    if (data && fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        data = NULL;
    }
    fclose(f);
    if (data) {
        data[size] = 0;
        *len = (size_t)size;
    }
    return data;
}

static long long mtime(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (long long)st.st_mtime : -1;
}

static int load_font(font_face face, const char *name)
{
    char path[1200];
    snprintf(path, sizeof path, "%s/.deps/fonts/%s", root, name);
    size_t len;
    uint8_t *ttf = read_file(path, &len);   /* kept for the program's life */
    if (!ttf || font_load(face, ttf, len) < 0) {
        fprintf(stderr, "cannot load %s (run scripts/build_preview.py)\n", path);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------ themes */

#define MAX_THEMES 16
static char theme_names[MAX_THEMES][64];
static int theme_count, theme_active = -1;
static long long theme_mtime;
/* Settings: row 0 is the "Cover colors" switch, then one row per theme. */
static ui_row settings_rows[MAX_THEMES + 1];
static ui_theme base_theme;
static int cover_colors = 1;
static cover_palette playing_palette;
static int have_palette;

/* The theme the UI should show: the chosen one, tinted by the playing
 * track's cover when "Cover colors" is on. */
static void retint(ui_state *ui, int instant)
{
    ui_theme target = base_theme;
    if (cover_colors && have_palette) theme_tint(&base_theme, &playing_palette, &target);
    ui->theme_target = target;
    if (instant) ui->theme = target;
}

static void theme_path(int i, char *out, size_t cap);

/* A theme's accent, for its swatch in Settings. */
static uint32_t theme_swatch(int i)
{
    char path[1300];
    theme_path(i, path, sizeof path);
    size_t len;
    char *text = (char *)read_file(path, &len);
    ui_theme t;
    theme_defaults(&t);
    if (text) {
        theme_parse(&t, text, len);
        free(text);
    }
    return t.accent;
}

static void fill_settings(ui_state *ui)
{
    ui_row *r = &settings_rows[0];
    memset(r, 0, sizeof *r);
    snprintf(r->title, sizeof r->title, "Cover colors");
    snprintf(r->subtitle, sizeof r->subtitle, cover_colors
             ? "On \xC2\xB7 the theme follows the album art"
             : "Off \xC2\xB7 the theme keeps its own colors");
    r->color = cover_colors ? ui->theme_target.accent : 0xFF475569u;
    for (int k = 0; k < theme_count; ++k) {
        r = &settings_rows[k + 1];
        memset(r, 0, sizeof *r);
        snprintf(r->title, sizeof r->title, "%s", theme_names[k]);
        snprintf(r->subtitle, sizeof r->subtitle, k == theme_active ? "Theme \xC2\xB7 active" : "Theme");
        r->color = theme_swatch(k);
        r->playing = k == theme_active;
    }
    ui->lists[UI_CAT_SETTINGS][0].rows = settings_rows;
    ui->lists[UI_CAT_SETTINGS][0].count = theme_count + 1;
}

static void theme_path(int i, char *out, size_t cap)
{
    snprintf(out, cap, "%s/themes/%s/theme.ini", root, theme_names[i]);
}

static void scan_themes(void)
{
    char dir[1200];
    theme_count = 0;
#ifdef _WIN32
    snprintf(dir, sizeof dir, "%s/themes/*", root);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(dir, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.' &&
            theme_count < MAX_THEMES)
            snprintf(theme_names[theme_count++], 64, "%s", fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    snprintf(dir, sizeof dir, "%s/themes", root);
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && theme_count < MAX_THEMES)
        if (e->d_name[0] != '.') snprintf(theme_names[theme_count++], 64, "%s", e->d_name);
    closedir(d);
#endif
    /* Directory order differs between systems; keep the list stable. */
    for (int i = 1; i < theme_count; ++i)
        for (int j = i; j > 0 && strcmp(theme_names[j - 1], theme_names[j]) > 0; --j) {
            char tmp[64];
            memcpy(tmp, theme_names[j], 64);
            memcpy(theme_names[j], theme_names[j - 1], 64);
            memcpy(theme_names[j - 1], tmp, 64);
        }
}

static void apply_theme(ui_state *ui, int i, int instant)
{
    char path[1300];
    theme_path(i, path, sizeof path);
    size_t len;
    char *text = (char *)read_file(path, &len);
    theme_defaults(&base_theme);
    if (text) {
        int ignored = theme_parse(&base_theme, text, len);
        if (ignored) printf("theme %s: %d line(s) ignored\n", theme_names[i], ignored);
        free(text);
    } else {
        printf("theme %s: cannot read %s\n", theme_names[i], path);
    }
    theme_active = i;
    theme_mtime = mtime(path);
    retint(ui, instant);
    fill_settings(ui);
}

/* ------------------------------------------------------------ mock library */

typedef struct mock_track {
    const char *title, *artist, *album;
    unsigned ms;
    uint32_t color;
} mock_track;

static const mock_track TRACKS[] = {
    { "When the Sun Hits", "Slowdive", "Souvlaki", 285000, 0xFF6D28D9u },
    { "Alison", "Slowdive", "Souvlaki", 230000, 0xFF6D28D9u },
    { "Sugar for the Pill", "Slowdive", "Slowdive", 270000, 0xFF334155u },
    { "Duvet", "b\xC3\xB4" "a", "Twilight", 203000, 0xFF0E7490u },
    { "Sparks", "Coldplay", "Parachutes", 227000, 0xFFB45309u },
    { "Air I Breathe", "Sub Focus, Wilkinson", "Portals", 245000, 0xFF1D4ED8u },
    { "ILoveUIHateU", "Playboi Carti", "Whole Lotta Red", 135000, 0xFFB91C1Cu },
    { "Punk Monk", "Playboi Carti", "Whole Lotta Red", 228000, 0xFFB91C1Cu },
    { "G\xC3\xBClpembe", "Bar\xC4\xB1\xC5\x9F Man\xC3\xA7o", "De\xC4\x9Fmesin Ya\xC4\x9Fl\xC4\xB1 Boya", 290000, 0xFFBE185Du },
    { "Sar\xC4\xB1 \xC3\x87izmeli Mehmet A\xC4\x9F" "a", "Bar\xC4\xB1\xC5\x9F Man\xC3\xA7o", "Sar\xC4\xB1 \xC3\x87izmeli Mehmet A\xC4\x9F" "a", 252000, 0xFFA16207u },
};
#define TRACK_COUNT ((int)(sizeof TRACKS / sizeof TRACKS[0]))

static const struct { const char *name; uint32_t color; } PLAYLISTS[] = {
    { "Night drive", 0xFF3B4FD8u }, { "Shoegaze", 0xFF0F766Eu }, { "Gym", 0xFFB45309u },
    { "Old PSP days", 0xFF9D174Du }, { "T\xC3\xBCrk\xC3\xA7" "e klasikler", 0xFFA16207u },
    { "Focus", 0xFF475569u }, { "Road trip 2026", 0xFF15803Du },
    { "A very long playlist name that does not fit on the PSP screen", 0xFF7C3AEDu },
};
#define PLAYLIST_COUNT ((int)(sizeof PLAYLISTS / sizeof PLAYLISTS[0]))

/* Stand-in album art: two tones of the track's color and a light disc, so
 * the cover-color code sees a picture rather than a flat fill. */
#define COVER 64
static uint32_t cover_px[TRACK_COUNT][COVER * COVER];
static r_tex *covers[TRACK_COUNT];

static uint32_t shade(uint32_t c, float f)
{
    uint32_t out = 0xFF000000u;
    for (int shift = 0; shift < 24; shift += 8) {
        float v = (float)((c >> shift) & 0xFF) * f;
        out |= (uint32_t)(v > 255 ? 255 : v) << shift;
    }
    return out;
}

static void make_covers(void)
{
    for (int t = 0; t < TRACK_COUNT; ++t) {
        uint32_t base = TRACKS[t].color, dark = shade(base, 0.45f), light = shade(base, 1.6f);
        for (int y = 0; y < COVER; ++y)
            for (int x = 0; x < COVER; ++x) {
                uint32_t c = x + y < COVER + (t * 7) % 24 ? base : dark;
                /* Anti-aliased disc: 4x4 samples per pixel. */
                int inside = 0;
                for (int sy = 0; sy < 4; ++sy)
                    for (int sx = 0; sx < 4; ++sx) {
                        float dx = x + (sx + 0.5f) / 4 - 40, dy = y + (sy + 0.5f) / 4 - 24;
                        inside += dx * dx + dy * dy < 13 * 13;
                    }
                uint32_t mixed = 0xFF000000u;
                for (int shift = 0; shift < 24; shift += 8) {
                    uint32_t a = (c >> shift) & 0xFF, b = (light >> shift) & 0xFF;
                    mixed |= ((a * (16 - inside) + b * inside) / 16) << shift;
                }
                cover_px[t][y * COVER + x] = mixed;
            }
        covers[t] = r_tex_create(COVER, COVER, cover_px[t], 1);
    }
}

static ui_row liked_rows[TRACK_COUNT], playlist_rows[PLAYLIST_COUNT], open_rows[TRACK_COUNT];
static ui_row queue_rows[TRACK_COUNT];
static ui_row search_rows[8];
static int open_map[TRACK_COUNT], search_map[8], search_kind[8];

/* The play queue: indices into TRACKS. */
static int queue[TRACK_COUNT], queue_len, queue_pos = -1;
static float play_ms;

static void track_row(ui_row *r, int t)
{
    memset(r, 0, sizeof *r);
    snprintf(r->title, sizeof r->title, "%s", TRACKS[t].title);
    snprintf(r->subtitle, sizeof r->subtitle, "%s", TRACKS[t].artist);
    r->color = TRACKS[t].color;
    r->cover = covers[t];
}

static void fill_library(ui_state *ui)
{
    for (int i = 0; i < TRACK_COUNT; ++i) track_row(&liked_rows[i], i);
    ui->lists[UI_CAT_LIKED][0].rows = liked_rows;
    ui->lists[UI_CAT_LIKED][0].count = TRACK_COUNT;
    for (int i = 0; i < PLAYLIST_COUNT; ++i) {
        ui_row *r = &playlist_rows[i];
        memset(r, 0, sizeof *r);
        snprintf(r->title, sizeof r->title, "%s", PLAYLISTS[i].name);
        snprintf(r->subtitle, sizeof r->subtitle, "Playlist \xC2\xB7 %d songs", 6 + i * 7);
        r->color = PLAYLISTS[i].color;
    }
    ui->lists[UI_CAT_PLAYLISTS][0].rows = playlist_rows;
    ui->lists[UI_CAT_PLAYLISTS][0].count = PLAYLIST_COUNT;
    for (int i = 0; i < TRACK_COUNT - 1; ++i) {
        track_row(&queue_rows[i], i + 1);
        if (i < 2) {
            char artist[96];
            snprintf(artist, sizeof artist, "Queued \xC2\xB7 %s", TRACKS[i + 1].artist);
            snprintf(queue_rows[i].subtitle, sizeof queue_rows[i].subtitle, "%s", artist);
        }
    }
    ui_list *q = &ui->lists[UI_CAT_QUEUE][0];
    snprintf(q->header, sizeof q->header, "Up next");
    q->rows = queue_rows;
    q->count = TRACK_COUNT - 1;
    q->movable = 1;
}

static void open_playlist(ui_state *ui, int p)
{
    /* Each mock playlist is a rotation of the track pool. */
    int n = 4 + p % (TRACK_COUNT - 3);
    for (int i = 0; i < n; ++i) {
        open_map[i] = (p * 3 + i) % TRACK_COUNT;
        track_row(&open_rows[i], open_map[i]);
    }
    ui_list *l = &ui->lists[UI_CAT_PLAYLISTS][1];
    snprintf(l->header, sizeof l->header, "%s", PLAYLISTS[p].name);
    l->rows = open_rows;
    l->count = n;
    ui_open_level(ui, UI_CAT_PLAYLISTS);
}

static void mark_playing(ui_state *ui)
{
    int t = queue_pos >= 0 ? queue[queue_pos] : -1;
    for (int i = 0; i < TRACK_COUNT; ++i) liked_rows[i].playing = i == t;
    for (int i = 0; i < ui->lists[UI_CAT_PLAYLISTS][1].count; ++i) open_rows[i].playing = open_map[i] == t;
    for (int i = 0; i < ui->lists[UI_CAT_SEARCH][0].count; ++i)
        search_rows[i].playing = search_kind[i] == 0 && search_map[i] == t;
}

static void play_at(ui_state *ui, int pos)
{
    if (pos < 0 || pos >= queue_len) return;
    queue_pos = pos;
    play_ms = 0;
    const mock_track *t = &TRACKS[queue[pos]];
    ui_now *n = &ui->now;
    n->active = 1;
    n->paused = 0;
    snprintf(n->title, sizeof n->title, "%s", t->title);
    snprintf(n->artist, sizeof n->artist, "%s", t->artist);
    snprintf(n->album, sizeof n->album, "%s", t->album);
    snprintf(n->next, sizeof n->next, "%s", pos + 1 < queue_len ? TRACKS[queue[pos + 1]].title : "");
    n->dur_ms = t->ms;
    n->pos_ms = 0;
    n->color = t->color;
    n->cover = covers[queue[pos]];
    palette_extract(cover_px[queue[pos]], COVER * COVER, &playing_palette);
    have_palette = 1;
    retint(ui, 0);
    fill_settings(ui);
    mark_playing(ui);
}

static void play_list(ui_state *ui, const int *tracks, int count, int start)
{
    queue_len = count;
    for (int i = 0; i < count; ++i) queue[i] = tracks[i];
    play_at(ui, start);
}

static const char *const QUERIES[] = { "slowdive", "playboi carti", "bar\xC4\xB1\xC5\x9F man\xC3\xA7o" };
static int query_index = -1;

static void run_search(ui_state *ui)
{
    query_index = (query_index + 1) % (int)(sizeof QUERIES / sizeof QUERIES[0]);
    const char *q = QUERIES[query_index];
    snprintf(ui->search_query, sizeof ui->search_query, "%s", q);
    int n = 0;
    const char *artist = NULL;
    for (int i = 0; i < TRACK_COUNT && n < 6; ++i) {
        char low[128];
        snprintf(low, sizeof low, "%s", TRACKS[i].artist);
        for (char *c = low; *c; ++c) if (*c >= 'A' && *c <= 'Z') *c = (char)(*c - 'A' + 'a');
        if (strstr(low, q) == NULL) continue;
        track_row(&search_rows[n], i);
        snprintf(search_rows[n].subtitle, sizeof search_rows[n].subtitle, "Song \xC2\xB7 %s",
                 TRACKS[i].artist);
        search_map[n] = i;
        search_kind[n++] = 0;
        artist = TRACKS[i].artist;
    }
    if (artist && n < 8) {
        ui_row *r = &search_rows[n];
        memset(r, 0, sizeof *r);
        snprintf(r->title, sizeof r->title, "%s", artist);
        snprintf(r->subtitle, sizeof r->subtitle, "Artist");
        r->color = 0xFF475569u;
        r->round = 1;
        search_kind[n++] = 1;
    }
    ui->lists[UI_CAT_SEARCH][0].rows = search_rows;
    ui->lists[UI_CAT_SEARCH][0].count = n;
    ui->sel[UI_CAT_SEARCH][0] = 0;
    mark_playing(ui);
}

static int handle(ui_state *ui, const ui_event *e)
{
    switch (e->type) {
    case UI_EV_QUIT:
        return 1;
    case UI_EV_OPEN:
        if (e->category == UI_CAT_PLAYLISTS && e->level == 0) {
            open_playlist(ui, e->index);
        } else if (e->category == UI_CAT_PLAYLISTS) {
            play_list(ui, open_map, ui->lists[UI_CAT_PLAYLISTS][1].count, e->index);
        } else if (e->category == UI_CAT_LIKED) {
            int all[TRACK_COUNT];
            for (int i = 0; i < TRACK_COUNT; ++i) all[i] = i;
            play_list(ui, all, TRACK_COUNT, e->index);
        } else if (e->category == UI_CAT_SEARCH && search_kind[e->index] == 0) {
            play_list(ui, &search_map[e->index], 1, 0);
        } else if (e->category == UI_CAT_SETTINGS && e->index == 0) {
            cover_colors = !cover_colors;
            retint(ui, 0);
            fill_settings(ui);
        } else if (e->category == UI_CAT_SETTINGS) {
            apply_theme(ui, e->index - 1, 0);
        }
        break;
    case UI_EV_PAUSE:
        if (ui->now.active) ui->now.paused = !ui->now.paused;
        break;
    case UI_EV_NEXT:
        play_at(ui, queue_pos + 1);
        break;
    case UI_EV_PREV:
        play_at(ui, play_ms > 3000 ? queue_pos : queue_pos - 1);
        break;
    case UI_EV_SEEK:
        if (ui->now.active) {
            play_ms += e->value * 5000.0f;
            if (play_ms < 0) play_ms = 0;
        }
        break;
    case UI_EV_SEARCH:
        run_search(ui);
        break;
    case UI_EV_SHUFFLE:
        ui->now.shuffle = !ui->now.shuffle;
        break;
    case UI_EV_OPTIONS: {
        static const char *const items[] = { "Add to queue", "Save to Liked Songs" };
        static const char *const queue_items[] = { "Play now", "Remove from queue",
                                                   "Save to Liked Songs" };
        if (e->category == UI_CAT_QUEUE) ui_menu_open(ui, queue_items, 3);
        else if (e->category == UI_CAT_LIKED || e->level == 1) ui_menu_open(ui, items, 2);
        break;
    }
    case UI_EV_MENU:
        ui_toast(ui, e->category == UI_CAT_QUEUE ? (e->value == 1 ? "Removed from queue"
                                                    : "Playing now")
                     : e->value == 0 ? "Added to queue" : "Saved to Liked Songs");
        break;
    case UI_EV_MOVE: {
        ui_row tmp = queue_rows[e->index];
        queue_rows[e->index] = queue_rows[e->index + e->value];
        queue_rows[e->index + e->value] = tmp;
        break;
    }
    case UI_EV_REPEAT:
        ui->now.repeat = (ui->now.repeat + 1) % 3;
        break;
    default:
        break;
    }
    return 0;
}

static void tick(ui_state *ui, float dt)
{
    ui_now *n = &ui->now;
    if (n->active && !n->paused) {
        play_ms += dt * 1000.0f;
        if (play_ms >= (float)n->dur_ms) {
            if (queue_pos + 1 < queue_len) play_at(ui, queue_pos + 1);
            else { play_ms = (float)n->dur_ms; n->paused = 1; }
        }
    }
    n->pos_ms = (unsigned)play_ms;
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    if (lt) snprintf(ui->clock, sizeof ui->clock, "%02d:%02d", lt->tm_hour, lt->tm_min);
}

/* ------------------------------------------------------------ input */

static unsigned key_buttons(const Uint8 *k)
{
    unsigned b = 0;
    if (k[SDL_SCANCODE_UP]) b |= UI_BTN_UP;
    if (k[SDL_SCANCODE_DOWN]) b |= UI_BTN_DOWN;
    if (k[SDL_SCANCODE_LEFT]) b |= UI_BTN_LEFT;
    if (k[SDL_SCANCODE_RIGHT]) b |= UI_BTN_RIGHT;
    if (k[SDL_SCANCODE_Z] || k[SDL_SCANCODE_RETURN]) b |= UI_BTN_CROSS;
    if (k[SDL_SCANCODE_X] || k[SDL_SCANCODE_BACKSPACE]) b |= UI_BTN_CIRCLE;
    if (k[SDL_SCANCODE_A]) b |= UI_BTN_SQUARE;
    if (k[SDL_SCANCODE_S]) b |= UI_BTN_TRIANGLE;
    if (k[SDL_SCANCODE_Q]) b |= UI_BTN_L;
    if (k[SDL_SCANCODE_E]) b |= UI_BTN_R;
    if (k[SDL_SCANCODE_ESCAPE]) b |= UI_BTN_START;
    if (k[SDL_SCANCODE_TAB]) b |= UI_BTN_SELECT;
    return b;
}

static unsigned key_name(const char *name)
{
    static const struct { const char *name; unsigned bit; } names[] = {
        { "up", UI_BTN_UP }, { "down", UI_BTN_DOWN }, { "left", UI_BTN_LEFT },
        { "right", UI_BTN_RIGHT }, { "cross", UI_BTN_CROSS }, { "circle", UI_BTN_CIRCLE },
        { "square", UI_BTN_SQUARE }, { "triangle", UI_BTN_TRIANGLE }, { "l", UI_BTN_L },
        { "r", UI_BTN_R }, { "start", UI_BTN_START }, { "select", UI_BTN_SELECT },
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; ++i)
        if (strcmp(names[i].name, name) == 0) return names[i].bit;
    fprintf(stderr, "unknown key '%s'\n", name);
    return 0;
}

/* ------------------------------------------------------------ main */

static void find_root(const char *arg)
{
    if (arg) {
        snprintf(root, sizeof root, "%s", arg);
        return;
    }
    /* build-preview/ sits in the repository root. */
    char *base = SDL_GetBasePath();
    snprintf(root, sizeof root, "%s..", base ? base : "./");
    SDL_free(base);
}

static int save_png(SDL_Renderer *r, const char *path)
{
    static uint32_t px[W * H];
    if (SDL_RenderReadPixels(r, NULL, SDL_PIXELFORMAT_ABGR8888, px, W * 4) != 0) return -1;
    return stbi_write_png(path, W, H, 4, px, W * 4) ? 0 : -1;
}

int main(int argc, char **argv)
{
    const char *shot = NULL, *keys = NULL, *theme = "Midnight", *root_arg = NULL;
    int scale = 2, play = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--keys") && i + 1 < argc) keys = argv[++i];
        else if (!strcmp(argv[i], "--theme") && i + 1 < argc) theme = argv[++i];
        else if (!strcmp(argv[i], "--root") && i + 1 < argc) root_arg = argv[++i];
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--play")) play = 1;
        else if (!strcmp(argv[i], "--no-cover-colors")) cover_colors = 0;
        else {
            fprintf(stderr, "usage: %s [--theme NAME] [--scale N] [--play] [--no-cover-colors] [--shot FILE --keys a,b]\n",
                    argv[0]);
            return 2;
        }
    }
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    find_root(root_arg);
    if (scale < 1) scale = 1;
    SDL_Window *win = SDL_CreateWindow("PSPotify Preview", SDL_WINDOWPOS_CENTERED,
                                       SDL_WINDOWPOS_CENTERED, W * scale, H * scale,
                                       shot ? SDL_WINDOW_HIDDEN : SDL_WINDOW_RESIZABLE);
    SDL_Renderer *ren = win ? SDL_CreateRenderer(win, -1, shot ? SDL_RENDERER_SOFTWARE
                                                 : SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC)
                            : NULL;
    if (!ren) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Texture *screen = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                            SDL_TEXTUREACCESS_TARGET, W, H);
    SDL_SetTextureScaleMode(screen, SDL_ScaleModeNearest);
    render_sdl_init(ren);

    if (load_font(FONT_REGULAR, "MPLUSRounded1c-Regular.ttf") < 0 ||
        load_font(FONT_BOLD, "MPLUSRounded1c-Bold.ttf") < 0 ||
        load_font(FONT_XBOLD, "MPLUSRounded1c-ExtraBold.ttf") < 0 ||
        load_font(FONT_ICONS, "lucide.ttf") < 0)
        return 1;

    static ui_state ui;
    SDL_SetRenderTarget(ren, screen);
    if (ui_init(&ui) < 0) return 1;
    scan_themes();
    int start_theme = 0;
    for (int i = 0; i < theme_count; ++i)
        if (!strcmp(theme_names[i], theme)) start_theme = i;
    make_covers();
    if (theme_count) apply_theme(&ui, start_theme, 1);
    fill_library(&ui);
    if (play) {
        int all[TRACK_COUNT];
        for (int i = 0; i < TRACK_COUNT; ++i) all[i] = i;
        play_list(&ui, all, TRACK_COUNT, 0);
        play_ms = 108000;
    }
    tick(&ui, 0);

    if (shot) {
        /* Deterministic: each key is one press, then time to settle. */
        char list[512];
        snprintf(list, sizeof list, "%s", keys ? keys : "");
        for (char *k = strtok(list, ","); k; k = strtok(NULL, ",")) {
            unsigned bit = key_name(k);
            for (int f = 0; f < 2; ++f) {
                ui_update(&ui, f == 0 ? bit : 0, 1.0f / 60.0f);
                for (int e = 0; e < ui.event_count; ++e) handle(&ui, &ui.events[e]);
            }
            for (int f = 0; f < 30; ++f) ui_update(&ui, 0, 1.0f / 60.0f);
        }
        for (int f = 0; f < 60; ++f) ui_update(&ui, 0, 1.0f / 60.0f);
        ui_draw(&ui);
        int ret = save_png(ren, shot);
        printf(ret == 0 ? "saved %s\n" : "could not save %s\n", shot);
        return ret == 0 ? 0 : 1;
    }

    Uint64 last = SDL_GetPerformanceCounter();
    float reload_check = 0;
    for (int running = 1; running; ) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev))
            if (ev.type == SDL_QUIT) running = 0;
        Uint64 now = SDL_GetPerformanceCounter();
        float dt = (float)(now - last) / (float)SDL_GetPerformanceFrequency();
        last = now;
        if (dt > 0.1f) dt = 0.1f;

        ui_update(&ui, key_buttons(SDL_GetKeyboardState(NULL)), dt);
        for (int e = 0; e < ui.event_count; ++e)
            if (handle(&ui, &ui.events[e])) running = 0;
        tick(&ui, dt);

        reload_check += dt;
        if (reload_check > 0.5f && theme_active >= 0) {
            reload_check = 0;
            char path[1300];
            theme_path(theme_active, path, sizeof path);
            if (mtime(path) != theme_mtime) {
                printf("theme %s changed, reloading\n", theme_names[theme_active]);
                apply_theme(&ui, theme_active, 0);
            }
        }

        SDL_SetRenderTarget(ren, screen);
        ui_draw(&ui);
        SDL_SetRenderTarget(ren, NULL);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        int ww, wh;
        SDL_GetRendererOutputSize(ren, &ww, &wh);
        int s = ww / W < wh / H ? ww / W : wh / H;
        if (s < 1) s = 1;
        SDL_Rect dst = { (ww - W * s) / 2, (wh - H * s) / 2, W * s, H * s };
        SDL_RenderCopy(ren, screen, NULL, &dst);
        SDL_RenderPresent(ren);
    }
    SDL_Quit();
    return 0;
}
