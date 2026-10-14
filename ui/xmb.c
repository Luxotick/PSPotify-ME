#include "ui.h"
#include "draw.h"
#include "font.h"
#include "icons.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define SCREEN_W 480
#define SCREEN_H 272
#define CAT_BOTTOM 80       /* category icons and the label sit on this line */
#define CAT_ANCHOR 118      /* x of the selected category's centre */
#define CAT_SPACING 46
#define LIST_X 100
#define LIST_TOP 92
#define MINI_H 34
#define SEEK_HOLD 0.45f     /* L/R held longer than this seeks */

static const char *const cat_names[UI_CATS] = {
    "Search", "Liked Songs", "Playlists", "Now Playing", "Settings"
};
static const unsigned cat_icons[UI_CATS] = {
    ICON_SEARCH, ICON_HEART, ICON_LIST_MUSIC, ICON_MUSIC, ICON_SLIDERS
};

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* Exponential ease towards target; frame-rate independent. */
static float approach(float v, float target, float rate, float dt)
{
    v = target + (v - target) * expf(-rate * dt);
    return fabsf(v - target) < 0.001f ? target : v;
}

static void format_time(unsigned ms, char *out, size_t cap)
{
    unsigned s = ms / 1000u;
    snprintf(out, cap, "%u:%02u", s / 60u, s % 60u);
}

/* ------------------------------------------------------------ input */

static void emit(ui_state *ui, ui_event_type type, int value)
{
    if (ui->event_count >= UI_MAX_EVENTS) return;
    ui_event *e = &ui->events[ui->event_count++];
    e->type = type;
    e->category = ui->cat;
    e->level = ui->level[ui->cat];
    e->index = ui->sel[ui->cat][e->level];
    e->value = value;
}

/* One easing step per channel; always moves at least one unit so the
 * color reaches its target instead of stalling just short of it. */
static uint32_t ease_color(uint32_t from, uint32_t to, float t)
{
    uint32_t out = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        int a = (int)((from >> shift) & 0xFF), d = (int)((to >> shift) & 0xFF) - a;
        int step = (int)((float)d * t);
        if (step == 0 && d != 0) step = d > 0 ? 1 : -1;
        out |= (uint32_t)(a + step) << shift;
    }
    return out;
}

int ui_init(ui_state *ui)
{
    memset(ui, 0, sizeof *ui);
    theme_defaults(&ui->theme);
    ui->theme_target = ui->theme;
    ui->volume = 80;
    ui->cat = UI_CAT_PLAYLISTS;
    ui->cat_pos = (float)ui->cat;
    return draw_init();
}

void ui_open_level(ui_state *ui, int cat)
{
    if (cat < 0 || cat >= UI_CATS) return;
    ui->level[cat] = 1;
    ui->sel[cat][1] = 0;
    ui->scroll[cat][1] = 0.0f;
}

void ui_update(ui_state *ui, unsigned buttons, float dt)
{
    ui->event_count = 0;
    const unsigned pressed = buttons & ~ui->buttons, released = ~buttons & ui->buttons;
    const unsigned dirs = buttons & (UI_BTN_UP | UI_BTN_DOWN | UI_BTN_LEFT | UI_BTN_RIGHT);
    unsigned repeat = 0;
    if (pressed & dirs) {
        ui->repeat_at = 0.35f;
    } else if (dirs) {
        ui->repeat_at -= dt;
        if (ui->repeat_at <= 0.0f) {
            repeat = dirs;
            ui->repeat_at += (dirs & (UI_BTN_LEFT | UI_BTN_RIGHT)) ? 0.18f : 0.07f;
        }
    }
    const unsigned act = pressed | repeat;
    ui->buttons = buttons;

    const int cat = ui->cat, lvl = ui->level[cat];
    const ui_list *list = &ui->lists[cat][lvl];
    int *sel = &ui->sel[cat][lvl];

    if (cat == UI_CAT_NOW) {
        int volume = ui->volume;
        if (act & UI_BTN_UP) volume += 5;
        if (act & UI_BTN_DOWN) volume -= 5;
        volume = volume < 0 ? 0 : volume > 100 ? 100 : volume;
        if (volume != ui->volume) {
            ui->volume = volume;
            emit(ui, UI_EV_VOLUME, volume);
        }
    } else {
        if (act & UI_BTN_UP) --*sel;
        if (act & UI_BTN_DOWN) ++*sel;
        if (*sel >= list->count) *sel = list->count - 1;
        if (*sel < 0) *sel = 0;
    }
    if ((pressed & UI_BTN_CROSS) && cat != UI_CAT_NOW && list->count > 0) emit(ui, UI_EV_OPEN, 0);
    if ((pressed & UI_BTN_CIRCLE) && lvl > 0) {
        emit(ui, UI_EV_BACK, 0);
        ui->level[cat] = 0;
    }
    if (pressed & UI_BTN_SQUARE) emit(ui, cat == UI_CAT_SEARCH ? UI_EV_SEARCH : UI_EV_PAUSE, 0);
    if (pressed & UI_BTN_TRIANGLE) emit(ui, UI_EV_OPTIONS, 0);
    if (pressed & UI_BTN_START) emit(ui, UI_EV_QUIT, 0);

    /* L/R: a tap changes track, holding seeks. */
    if (pressed & (UI_BTN_L | UI_BTN_R)) {
        ui->lr_held = 0.0f;
        ui->lr_seek_at = SEEK_HOLD;
    } else if (buttons & (UI_BTN_L | UI_BTN_R)) {
        ui->lr_held += dt;
        if (ui->lr_held >= ui->lr_seek_at) {
            emit(ui, UI_EV_SEEK, (buttons & UI_BTN_R) ? 1 : -1);
            ui->lr_seek_at += 0.25f;
        }
    }
    if ((released & (UI_BTN_L | UI_BTN_R)) && ui->lr_held < SEEK_HOLD)
        emit(ui, (released & UI_BTN_R) ? UI_EV_NEXT : UI_EV_PREV, 0);

    if ((act & UI_BTN_LEFT) && ui->cat > 0) --ui->cat;
    if ((act & UI_BTN_RIGHT) && ui->cat < UI_CATS - 1) ++ui->cat;

    ui->cat_pos = approach(ui->cat_pos, (float)ui->cat, 14.0f, dt);
    const float fade = 1.0f - expf(-6.0f * dt);
    ui_theme *th = &ui->theme;
    const ui_theme *to = &ui->theme_target;
    memcpy(th->name, to->name, sizeof th->name);
    th->bg = ease_color(th->bg, to->bg, fade);
    th->surface = ease_color(th->surface, to->surface, fade);
    th->text = ease_color(th->text, to->text, fade);
    th->dim = ease_color(th->dim, to->dim, fade);
    th->accent = ease_color(th->accent, to->accent, fade);
    for (int c = 0; c < UI_CATS; ++c) {
        ui->level_pos[c] = approach(ui->level_pos[c], (float)ui->level[c], 14.0f, dt);
        for (int l = 0; l < 2; ++l)
            ui->scroll[c][l] = approach(ui->scroll[c][l], (float)ui->sel[c][l], 16.0f, dt);
    }
}

/* ------------------------------------------------------------ pieces */

static void hint(float *x, unsigned icon, const char *label, uint32_t color)
{
    font_icon(icon, 10, *x, 6, color);
    *x += 13;
    *x += font_draw(FONT_REGULAR, 10, *x, 3, color, label) + 10;
}

static void draw_top(const ui_state *ui)
{
    const ui_theme *t = &ui->theme;
    float x = 14;
    const int cat = ui->cat, lvl = ui->level[cat];
    switch (cat) {
    case UI_CAT_SEARCH:
        hint(&x, ICON_CROSS, "Play", t->dim);
        hint(&x, ICON_SQUARE, "Search", t->dim);
        hint(&x, ICON_TRIANGLE, "Options", t->dim);
        break;
    case UI_CAT_NOW:
        hint(&x, ICON_SQUARE, ui->now.paused ? "Play" : "Pause", t->dim);
        x += font_draw(FONT_REGULAR, 10, x, 3, t->dim, "L / R  Prev / Next") + 10;
        x += font_draw(FONT_REGULAR, 10, x, 3, t->dim, "\xE2\x86\x91\xE2\x86\x93  Volume") + 10;
        break;
    case UI_CAT_SETTINGS:
        hint(&x, ICON_CROSS, "Select", t->dim);
        break;
    default:
        hint(&x, ICON_CROSS, cat == UI_CAT_PLAYLISTS && lvl == 0 ? "Open" : "Play", t->dim);
        if (lvl > 0) hint(&x, ICON_CIRCLE, "Back", t->dim);
        hint(&x, ICON_TRIANGLE, "Options", t->dim);
        break;
    }

    float right = SCREEN_W - 14;
    right -= font_width(FONT_BOLD, 10, ui->clock);
    font_draw(FONT_BOLD, 10, right, 3, t->text, ui->clock);
    char volume[8];
    snprintf(volume, sizeof volume, "%d", ui->volume);
    right -= 8 + font_width(FONT_REGULAR, 10, volume);
    font_draw(FONT_REGULAR, 10, right, 3, t->dim, volume);
    right -= 15;
    font_icon(ui->volume > 0 ? ICON_VOLUME : ICON_VOLUME_MUTE, 12, right, 5, t->dim);
}

static void draw_categories(const ui_state *ui)
{
    const ui_theme *t = &ui->theme;
    for (int i = 0; i < UI_CATS; ++i) {
        float d = (float)i - ui->cat_pos;
        float s = clampf(1.0f - fabsf(d), 0.0f, 1.0f);
        int size = (int)(22.0f + 12.0f * s + 0.5f);
        /* Neighbours step aside so the selected label has room. */
        float cx = CAT_ANCHOR + d * CAT_SPACING + (d < 0 ? -14.0f : 14.0f) * clampf(fabsf(d), 0.0f, 1.0f);
        float bottom = CAT_BOTTOM - 18.0f * s;
        font_icon(cat_icons[i], size, cx - size / 2.0f, bottom - size,
                  color_fade(t->text, 0.45f + 0.55f * s));
        if (s > 0.01f) {
            float w = font_width(FONT_BOLD, 11, cat_names[i]);
            font_draw(FONT_BOLD, 11, cx - w / 2.0f, CAT_BOTTOM - 15, color_fade(t->text, s),
                      cat_names[i]);
        }
    }
}

static void draw_cover(r_tex *tex, uint32_t color, int circle, float x, float y, float size,
                       float alpha, int note)
{
    if (tex) {
        int w, h;
        r_tex_size(tex, &w, &h);
        r_tex_draw(tex, 0, 0, (float)w, (float)h, x, y, size, size, color_fade(0xFFFFFFFFu, alpha));
        return;
    }
    draw_round_rect(x, y, size, size, circle ? size / 2 : (size >= 40 ? 6 : 4), color_fade(color, alpha));
    if (note && size >= 30) {
        int icon = (int)(size * 0.4f);
        font_icon(ICON_MUSIC, icon, x + (size - icon) / 2, y + (size - icon) / 2,
                  color_fade(0xBFFFFFFFu, alpha));
    }
}

typedef struct list_style {
    float small, big;       /* cover sizes */
    int title_small, title_big;
} list_style;

static const list_style STYLE_NORMAL = { 28, 44, 13, 15 };
static const list_style STYLE_COMPACT = { 24, 38, 12, 14 };

static void draw_list(const ui_state *ui, const ui_list *list, float scroll, float x, float top,
                      float bottom, const list_style *st, float alpha)
{
    const ui_theme *t = &ui->theme;
    if (alpha <= 0.01f) return;
    if (list->header[0]) {
        font_draw_fit(FONT_BOLD, 11, x, top - 2, color_fade(t->dim, alpha), list->header,
                      SCREEN_W - x - 20);
        top += 16;
    }
    if (list->count == 0) {
        font_draw(FONT_REGULAR, 12, x, top + 6, color_fade(t->dim, alpha),
                  list->loading ? "Loading\xE2\x80\xA6" : "Nothing here yet");
        return;
    }
    const float small_h = st->small + 6, big_h = st->big + 6;
    r_clip((int)x - 12, (int)top, SCREEN_W - (int)x + 12, (int)(bottom - top));
    /* The selected row sits one row down once there is a row above it, so
     * the previous item stays visible (dimmed). */
    const float focus = top + small_h * clampf(scroll, 0.0f, 1.0f);
    int first = (int)floorf(scroll) - 2;
    if (first < 0) first = 0;
    for (int i = first; i < list->count; ++i) {
        float d = (float)i - scroll;
        float y = focus + d * small_h + (big_h - small_h) * clampf(d, 0.0f, 1.0f);
        if (y > bottom) break;
        float s = clampf(1.0f - fabsf(d), 0.0f, 1.0f);
        float a = d >= 0 ? 1.0f - fminf(0.55f, 0.3f * d)
                : d >= -1 ? 1.0f + 0.55f * d : clampf(0.45f * (2.0f + d), 0.0f, 1.0f);
        a *= alpha;
        if (a <= 0.01f) continue;
        const ui_row *row = &list->rows[i];
        float size = st->small + (st->big - st->small) * s;
        float indent = (1.0f - s) * 8.0f;
        float cx = x + indent;
        draw_cover(row->cover, row->color, row->round, cx, y, size, a, s > 0.5f);

        float tx = cx + size + 10, max_w = SCREEN_W - tx - 16;
        uint32_t title_color = row->playing ? t->accent : t->text;
        if (s > 0.5f) {
            float th = font_line_height(FONT_XBOLD, st->title_big);
            float sh = row->subtitle[0] ? font_line_height(FONT_REGULAR, 11) : 0;
            float ty = y + (size - th - sh) / 2;
            font_draw_fit(FONT_XBOLD, st->title_big, tx, ty, color_fade(title_color, a), row->title,
                          max_w);
            if (sh > 0)
                font_draw_fit(FONT_REGULAR, 11, tx, ty + th, color_fade(t->dim, a), row->subtitle,
                              max_w);
        } else {
            float th = font_line_height(FONT_REGULAR, st->title_small);
            font_draw_fit(FONT_REGULAR, st->title_small, tx, y + (size - th) / 2,
                          color_fade(title_color, a), row->title, max_w);
        }
    }
    r_clip(0, 0, 0, 0);
}

static void draw_levels(const ui_state *ui, int cat, float x, float top, float bottom,
                        const list_style *st, float alpha, float slide)
{
    float lp = ui->level_pos[cat];
    draw_list(ui, &ui->lists[cat][0], ui->scroll[cat][0], x + slide - 60.0f * lp, top, bottom, st,
              alpha * (1.0f - lp));
    if (lp > 0.0f)
        draw_list(ui, &ui->lists[cat][1], ui->scroll[cat][1], x + slide + 60.0f * (1.0f - lp), top,
                  bottom, st, alpha * lp);
}

static void draw_search(const ui_state *ui, float bottom, float alpha, float slide)
{
    const ui_theme *t = &ui->theme;
    const float x = 30 + slide, y = LIST_TOP - 2, w = SCREEN_W - 54;
    draw_round_rect(x, y, w, 26, 13, color_fade(t->surface, alpha));
    font_icon(ICON_SEARCH, 13, x + 12, y + 7, color_fade(t->dim, alpha));
    const ui_list *results = &ui->lists[UI_CAT_SEARCH][0];
    if (ui->search_query[0]) {
        font_draw_fit(FONT_REGULAR, 12, x + 32, y + 5, color_fade(t->text, alpha), ui->search_query,
                      w - 110);
        char count[24];
        if (results->loading) snprintf(count, sizeof count, "Searching\xE2\x80\xA6");
        else snprintf(count, sizeof count, "%d results", results->count);
        float cw = font_width(FONT_REGULAR, 10, count);
        font_draw(FONT_REGULAR, 10, x + w - 12 - cw, y + 7, color_fade(t->dim, alpha), count);
        draw_levels(ui, UI_CAT_SEARCH, 30 + slide, y + 34, bottom, &STYLE_COMPACT, alpha, 0);
    } else {
        font_draw(FONT_REGULAR, 12, x + 32, y + 5, color_fade(t->dim, alpha),
                  ui->search_hint ? ui->search_hint : "Press \xE2\x96\xA1 to search Spotify");
    }
}

static void draw_progress(const ui_state *ui, float x, float y, float w, float h, float alpha)
{
    const ui_theme *t = &ui->theme;
    float f = ui->now.dur_ms ? clampf((float)ui->now.pos_ms / (float)ui->now.dur_ms, 0, 1) : 0;
    r_rect(x, y, w, h, color_fade(color_mix(t->bg, t->dim, 0.25f), alpha));
    r_rect(x, y, w * f, h, color_fade(t->accent, alpha));
}

static void draw_now(const ui_state *ui, float alpha, float slide)
{
    const ui_theme *t = &ui->theme;
    const ui_now *n = &ui->now;
    if (!n->active) {
        font_draw(FONT_BOLD, 15, 100 + slide, 120, color_fade(t->text, alpha), "Nothing playing");
        font_draw(FONT_REGULAR, 12, 100 + slide, 142, color_fade(t->dim, alpha),
                  "Pick a song in Liked Songs, Playlists or Search.");
        return;
    }
    const float cx = 30 + slide, cy = 92, size = 150;
    draw_cover(n->cover, n->color, 0, cx, cy, size, alpha, 1);

    const float x = cx + size + 18, w = SCREEN_W - 24 - x;
    float y = cy;
    font_draw_fit(FONT_XBOLD, 20, x, y, color_fade(t->text, alpha), n->title, w);
    y += font_line_height(FONT_XBOLD, 20) + 2;
    font_draw_fit(FONT_REGULAR, 13, x, y, color_fade(color_mix(t->dim, t->text, 0.5f), alpha),
                  n->artist, w);
    y += font_line_height(FONT_REGULAR, 13);
    font_draw_fit(FONT_REGULAR, 11, x, y, color_fade(t->dim, alpha),
                  n->loading ? "Loading\xE2\x80\xA6" : n->album, w);

    const float bar_y = 200;
    draw_round_rect(x, bar_y, w, 4, 2, color_fade(color_mix(t->bg, t->dim, 0.25f), alpha));
    float f = n->dur_ms ? clampf((float)n->pos_ms / (float)n->dur_ms, 0, 1) : 0;
    draw_round_rect(x, bar_y, w * f, 4, 2, color_fade(t->accent, alpha));
    draw_round_rect(x + w * f - 5, bar_y - 3, 10, 10, 5, color_fade(t->text, alpha));
    char pos[16], dur[16];
    format_time(n->pos_ms, pos, sizeof pos);
    format_time(n->dur_ms, dur, sizeof dur);
    font_draw(FONT_REGULAR, 10, x, bar_y + 8, color_fade(t->dim, alpha), pos);
    font_draw(FONT_REGULAR, 10, x + w - font_width(FONT_REGULAR, 10, dur), bar_y + 8,
              color_fade(t->dim, alpha), dur);

    const float cy2 = 241;   /* centre line of the controls */
    font_icon(ICON_SKIP_BACK, 20, x, cy2 - 10, color_fade(t->text, alpha));
    draw_round_rect(x + 38, cy2 - 19, 38, 38, 19, color_fade(t->accent, alpha));
    font_icon(n->paused ? ICON_PLAY : ICON_PAUSE, 16, x + 49, cy2 - 8, color_fade(t->bg, alpha));
    font_icon(ICON_SKIP_FORWARD, 20, x + 94, cy2 - 10, color_fade(t->text, alpha));
    if (n->next[0]) {
        char next[160];
        snprintf(next, sizeof next, "Next: %s", n->next);
        float max_w = x + w - (x + 126);
        float nw = font_width(FONT_REGULAR, 10, next);
        if (nw > max_w) nw = max_w;
        font_draw_fit(FONT_REGULAR, 10, x + w - nw, cy2 - 6, color_fade(t->dim, alpha), next, max_w);
    }
}

static void draw_mini(const ui_state *ui)
{
    const ui_theme *t = &ui->theme;
    const ui_now *n = &ui->now;
    const float y = SCREEN_H - MINI_H;
    r_rect(0, y, SCREEN_W, MINI_H, t->surface);
    draw_progress(ui, 0, y, SCREEN_W, 2, 1.0f);
    draw_cover(n->cover, n->color, 0, 12, y + 7, 22, 1.0f, 0);

    char span[32], pos[16], dur[16];
    format_time(n->pos_ms, pos, sizeof pos);
    format_time(n->dur_ms, dur, sizeof dur);
    snprintf(span, sizeof span, "%s / %s", pos, dur);
    float right = SCREEN_W - 12 - 14;
    font_icon(n->paused ? ICON_PLAY : ICON_PAUSE, 14, right, y + 11, t->text);
    right -= 10 + font_width(FONT_REGULAR, 10, span);
    font_draw(FONT_REGULAR, 10, right, y + 10, t->dim, n->loading ? "" : span);

    float x = 42;
    float tw = font_draw_fit(FONT_BOLD, 12, x, y + 9, t->text, n->title, right - x - 80);
    font_draw_fit(FONT_REGULAR, 11, x + tw + 8, y + 10, t->dim, n->artist, right - (x + tw + 8) - 12);
}

/* ------------------------------------------------------------ frame */

void ui_draw(const ui_state *ui)
{
    const ui_theme *t = &ui->theme;
    r_clip(0, 0, 0, 0);
    r_rect(0, 0, SCREEN_W, SCREEN_H, t->bg);
    draw_top(ui);
    draw_categories(ui);

    const int mini = ui->now.active && ui->cat != UI_CAT_NOW;
    const float bottom = (float)(mini ? SCREEN_H - MINI_H : SCREEN_H);
    /* The new category's content slides in from the side it came from. */
    const float lag = (float)ui->cat - ui->cat_pos;
    const float alpha = clampf(1.0f - fabsf(lag) * 1.6f, 0.0f, 1.0f);
    const float slide = lag * 40.0f;
    switch (ui->cat) {
    case UI_CAT_SEARCH:
        draw_search(ui, bottom, alpha, slide);
        break;
    case UI_CAT_NOW:
        draw_now(ui, alpha, slide);
        break;
    default:
        draw_levels(ui, ui->cat, LIST_X, LIST_TOP, bottom, &STYLE_NORMAL, alpha, slide);
        break;
    }
    if (mini) draw_mini(ui);
}
