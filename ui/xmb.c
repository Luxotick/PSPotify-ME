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
    "Search", "Recent", "Liked Songs", "Playlists", "Now Playing", "Queue", "Settings"
};
static const unsigned cat_icons[UI_CATS] = {
    ICON_SEARCH, ICON_HISTORY, ICON_HEART, ICON_LIST_MUSIC, ICON_MUSIC, ICON_QUEUE, ICON_SLIDERS
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

/* An easing rate under the user's animation setting. */
static float anim_rate(const ui_state *ui, float rate)
{
    switch (ui->style.anim) {
    case UI_ANIM_FAST: return rate * 2.2f;
    case UI_ANIM_OFF: return 1000.0f;
    default: return rate;
    }
}

static void format_time(unsigned ms, char *out, size_t cap)
{
    unsigned s = ms / 1000u;
    snprintf(out, cap, "%u:%02u", s / 60u, s % 60u);
}

/* Position of a category among the ones shown. */
static int cat_slot(const ui_state *ui, int cat)
{
    for (int i = 0; i < ui->cat_count; ++i)
        if (ui->cat_order[i] == cat) return i;
    return 0;
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
    for (int i = 0; i < UI_CATS; ++i) ui->cat_order[i] = i;
    ui->cat_count = UI_CATS;
    ui->cat = UI_CAT_PLAYLISTS;
    ui->cat_pos = (float)cat_slot(ui, ui->cat);
    return draw_init();
}

void ui_set_categories(ui_state *ui, const int *order, int count)
{
    int n = 0, seen[UI_CATS] = { 0 };
    for (int i = 0; i < count; ++i) {
        const int c = order[i];
        if (c < 0 || c >= UI_CATS || seen[c]) continue;
        seen[c] = 1;
        ui->cat_order[n++] = c;
    }
    if (!seen[UI_CAT_SETTINGS]) ui->cat_order[n++] = UI_CAT_SETTINGS;
    ui->cat_count = n;
    int shown = 0;
    for (int i = 0; i < n; ++i) shown |= ui->cat_order[i] == ui->cat;
    if (!shown) ui->cat = ui->cat_order[0];
    ui->cat_pos = (float)cat_slot(ui, ui->cat);
}

static void copy_label(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void ui_menu_open(ui_state *ui, const char *const *items, int count)
{
    ui_menu *m = &ui->menu;
    if (count > UI_MENU_ITEMS) count = UI_MENU_ITEMS;
    for (int i = 0; i < count; ++i) copy_label(m->items[i], sizeof m->items[i], items[i]);
    m->count = count;
    m->title[0] = '\0';
    m->sel = 0;
    m->open = count > 0;
}

void ui_toast(ui_state *ui, const char *text)
{
    copy_label(ui->toast, sizeof ui->toast, text);
    ui->toast_left = 2.0f;
}

static void animate(ui_state *ui, float dt);

/* Input while the menu is open: it takes every button. */
static void menu_input(ui_state *ui, unsigned pressed, unsigned act)
{
    ui_menu *m = &ui->menu;
    if (act & UI_BTN_UP) m->sel = m->sel > 0 ? m->sel - 1 : m->count - 1;
    if (act & UI_BTN_DOWN) m->sel = m->sel + 1 < m->count ? m->sel + 1 : 0;
    if (pressed & UI_BTN_CROSS) {
        if (ui->event_count < UI_MAX_EVENTS) {
            ui_event *e = &ui->events[ui->event_count++];
            e->type = UI_EV_MENU;
            e->category = m->category;
            e->level = m->level;
            e->index = m->index;
            e->value = m->sel;
        }
        m->open = 0;
    }
    if (pressed & (UI_BTN_CIRCLE | UI_BTN_TRIANGLE)) m->open = 0;
}

/* Input while a row is picked up: ↑/↓ move it, anything else drops it. */
static void move_input(ui_state *ui, unsigned pressed, unsigned act)
{
    const int lvl = ui->level[ui->cat];
    const ui_list *list = &ui->lists[ui->cat][lvl];
    int *sel = &ui->sel[ui->cat][lvl];
    int dir = (act & UI_BTN_UP) ? -1 : (act & UI_BTN_DOWN) ? 1 : 0;
    if (dir && *sel + dir >= 0 && *sel + dir < list->count) {
        emit(ui, UI_EV_MOVE, dir);
        *sel += dir;
    }
    if (pressed & (UI_BTN_CROSS | UI_BTN_CIRCLE | UI_BTN_SELECT)) ui->moving = 0;
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
    if (ui->style.swap_confirm) {
        /* Everything below treats ✕ as confirm and ○ as back. */
        const unsigned cross = buttons & UI_BTN_CROSS, circle = buttons & UI_BTN_CIRCLE;
        buttons &= ~(unsigned)(UI_BTN_CROSS | UI_BTN_CIRCLE);
        if (cross) buttons |= UI_BTN_CIRCLE;
        if (circle) buttons |= UI_BTN_CROSS;
    }
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
    if (ui->menu.open) {
        menu_input(ui, pressed, act);
        animate(ui, dt);
        return;
    }
    if (ui->moving) {
        move_input(ui, pressed, act);
        animate(ui, dt);
        return;
    }

    const int cat = ui->cat, lvl = ui->level[cat];
    const ui_list *list = &ui->lists[cat][lvl];
    int *sel = &ui->sel[cat][lvl];
    const int adjust = list->adjustable && list->count > 0;

    if (cat == UI_CAT_NOW) {
        int volume = ui->volume;
        const int step = ui->style.volume_step > 0 ? ui->style.volume_step : 5;
        if (act & UI_BTN_UP) volume += step;
        if (act & UI_BTN_DOWN) volume -= step;
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
    if ((pressed & UI_BTN_CROSS) && adjust) emit(ui, UI_EV_ADJUST, 1);
    else if ((pressed & UI_BTN_CROSS) && cat != UI_CAT_NOW && list->count > 0) emit(ui, UI_EV_OPEN, 0);
    if ((pressed & UI_BTN_CROSS) && cat == UI_CAT_NOW) emit(ui, UI_EV_REPEAT, 0);
    if ((pressed & UI_BTN_CIRCLE) && lvl > 0) {
        emit(ui, UI_EV_BACK, 0);
        ui->level[cat] = 0;
    }
    if (pressed & UI_BTN_SQUARE) emit(ui, cat == UI_CAT_SEARCH ? UI_EV_SEARCH : UI_EV_PAUSE, 0);
    if (pressed & UI_BTN_TRIANGLE) {
        emit(ui, cat == UI_CAT_NOW ? UI_EV_SHUFFLE : UI_EV_OPTIONS, 0);
        /* The host answers UI_EV_OPTIONS with ui_menu_open for this row. */
        ui->menu.category = cat;
        ui->menu.level = lvl;
        ui->menu.index = *sel;
    }
    if ((pressed & UI_BTN_SELECT) && list->movable && list->count > 0) ui->moving = 1;
    if (pressed & UI_BTN_START) emit(ui, UI_EV_QUIT, 0);

    /* L/R: a tap changes track and holding seeks, or only one of the two. */
    const int lr_mode = ui->style.lr_mode;
    if (lr_mode == UI_LR_SKIP) {
        if (pressed & (UI_BTN_L | UI_BTN_R))
            emit(ui, (pressed & UI_BTN_R) ? UI_EV_NEXT : UI_EV_PREV, 0);
    } else {
        const float hold = lr_mode == UI_LR_SEEK ? 0.0f : SEEK_HOLD;
        if (pressed & (UI_BTN_L | UI_BTN_R)) {
            ui->lr_held = 0.0f;
            ui->lr_seek_at = hold;
        }
        if (buttons & (UI_BTN_L | UI_BTN_R)) {
            if (!(pressed & (UI_BTN_L | UI_BTN_R))) ui->lr_held += dt;
            if (ui->lr_held >= ui->lr_seek_at) {
                emit(ui, UI_EV_SEEK, (buttons & UI_BTN_R) ? 1 : -1);
                ui->lr_seek_at += 0.25f;
            }
        }
        if ((released & (UI_BTN_L | UI_BTN_R)) && ui->lr_held < SEEK_HOLD && lr_mode == UI_LR_BOTH)
            emit(ui, (released & UI_BTN_R) ? UI_EV_NEXT : UI_EV_PREV, 0);
        else if (released & (UI_BTN_L | UI_BTN_R))
            emit(ui, UI_EV_SEEK, 0);    /* end of a seek: jump to the previewed time */
    }

    if (adjust) {
        /* ←/→ change the value instead of the category; ○ leaves first. */
        if (act & UI_BTN_LEFT) emit(ui, UI_EV_ADJUST, -1);
        if (act & UI_BTN_RIGHT) emit(ui, UI_EV_ADJUST, 1);
    } else {
        const int slot = cat_slot(ui, ui->cat);
        if ((act & UI_BTN_LEFT) && slot > 0) ui->cat = ui->cat_order[slot - 1];
        if ((act & UI_BTN_RIGHT) && slot + 1 < ui->cat_count) ui->cat = ui->cat_order[slot + 1];
    }
    animate(ui, dt);
}

static void animate(ui_state *ui, float dt)
{
    ui->cat_pos = approach(ui->cat_pos, (float)cat_slot(ui, ui->cat), anim_rate(ui, 14.0f), dt);
    ui->menu_pos = approach(ui->menu_pos, ui->menu.open ? 1.0f : 0.0f, anim_rate(ui, 18.0f), dt);
    if (ui->toast_left > 0) ui->toast_left -= dt;
    const float fade = 1.0f - expf(-anim_rate(ui, 6.0f) * dt);
    ui_theme *th = &ui->theme;
    const ui_theme *to = &ui->theme_target;
    memcpy(th->name, to->name, sizeof th->name);
    th->bg = ease_color(th->bg, to->bg, fade);
    th->surface = ease_color(th->surface, to->surface, fade);
    th->text = ease_color(th->text, to->text, fade);
    th->dim = ease_color(th->dim, to->dim, fade);
    th->accent = ease_color(th->accent, to->accent, fade);
    th->highlight = ease_color(th->highlight, to->highlight, fade);
    th->progress = ease_color(th->progress, to->progress, fade);
    th->player_bar = ease_color(th->player_bar, to->player_bar, fade);
    th->icons = ease_color(th->icons, to->icons, fade);
    for (int c = 0; c < UI_CATS; ++c) {
        ui->level_pos[c] = approach(ui->level_pos[c], (float)ui->level[c], anim_rate(ui, 14.0f), dt);
        for (int l = 0; l < 2; ++l)
            ui->scroll[c][l] = approach(ui->scroll[c][l], (float)ui->sel[c][l], anim_rate(ui, 16.0f), dt);
    }
}

/* ------------------------------------------------------------ list styles */

typedef struct list_style {
    float small, big;       /* row heights; also the cover sizes when covers is set */
    int title_small, title_big;
    int covers;
} list_style;

/* Rows for a density and text size; search results are a little tighter. */
static list_style style_for(const ui_state *ui, int search)
{
    static const list_style base[3][2] = {
        { { 28, 44, 13, 15, 1 }, { 24, 38, 12, 14, 1 } },     /* comfortable */
        { { 22, 34, 12, 14, 1 }, { 20, 32, 12, 13, 1 } },     /* compact */
        { { 20, 36, 13, 15, 0 }, { 18, 34, 12, 14, 0 } },     /* text only */
    };
    int d = ui->style.density;
    if (d < 0 || d > 2) d = 0;
    list_style st = base[d][search ? 1 : 0];
    if (ui->style.text_size == UI_TEXT_SMALL) {
        st.title_small -= 1;
        st.title_big -= 1;
    } else if (ui->style.text_size == UI_TEXT_LARGE) {
        st.title_small += 2;
        st.title_big += 2;
        st.small += 4;
        st.big += 4;
    }
    return st;
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
    int cat = ui->cat;
    const int lvl = ui->level[cat];
    /* With ○ as confirm the two icons trade places. */
    const unsigned ok = ui->style.swap_confirm ? ICON_CIRCLE : ICON_CROSS;
    const unsigned back = ui->style.swap_confirm ? ICON_CROSS : ICON_CIRCLE;
    if (ui->moving) {
        x += font_draw(FONT_REGULAR, 10, x, 3, t->dim, "\xE2\x86\x91\xE2\x86\x93 Move") + 10;
        hint(&x, ok, "Done", t->dim);
        cat = -1;
    } else if (ui->style.hide_hints) {
        cat = -1;
    } else if (ui->lists[cat][lvl].adjustable) {
        x += font_draw(FONT_REGULAR, 10, x, 3, t->dim, "\xE2\x86\x90\xE2\x86\x92") + 3;
        hint(&x, ok, "Change", t->dim);
        hint(&x, back, "Back", t->dim);
        if (ui->lists[cat][lvl].movable)
            x += font_draw(FONT_REGULAR, 10, x, 3, t->dim, "SELECT Move") + 10;
        cat = -1;
    }
    switch (cat) {
    case -1:
        break;
    case UI_CAT_QUEUE:
        hint(&x, ok, "Play", t->dim);
        hint(&x, ICON_TRIANGLE, "Options", t->dim);
        x += font_draw(FONT_REGULAR, 10, x, 3, t->dim, "SELECT Move") + 10;
        break;
    case UI_CAT_SEARCH:
        hint(&x, ok, "Play", t->dim);
        hint(&x, ICON_SQUARE, "Search", t->dim);
        hint(&x, ICON_TRIANGLE, "Options", t->dim);
        break;
    case UI_CAT_NOW:
        hint(&x, ICON_SQUARE, ui->now.paused ? "Play" : "Pause", t->dim);
        hint(&x, ok, "Repeat", t->dim);
        hint(&x, ICON_TRIANGLE, "Shuffle", t->dim);
        x += font_draw(FONT_REGULAR, 10, x, 3, t->dim, "\xE2\x86\x91\xE2\x86\x93 Vol") + 10;
        break;
    case UI_CAT_RECENT:
        hint(&x, ok, lvl == 0 ? "Open" : "Play", t->dim);
        if (lvl > 0) hint(&x, back, "Back", t->dim);
        break;
    case UI_CAT_SETTINGS:
        hint(&x, ok, lvl == 0 ? "Open" : "Select", t->dim);
        if (lvl > 0) hint(&x, back, "Back", t->dim);
        break;
    case UI_CAT_PLAYLISTS:
        hint(&x, ok, lvl == 0 ? "Open" : "Play", t->dim);
        if (lvl > 0) hint(&x, back, "Back", t->dim);
        hint(&x, ICON_TRIANGLE, lvl == 0 ? "Pin" : "Options", t->dim);
        break;
    default:
        hint(&x, ok, "Play", t->dim);
        if (lvl > 0) hint(&x, back, "Back", t->dim);
        hint(&x, ICON_TRIANGLE, "Options", t->dim);
        break;
    }

    float right = SCREEN_W - 14;
    if (ui->clock[0]) {
        right -= font_width(FONT_BOLD, 10, ui->clock);
        font_draw(FONT_BOLD, 10, right, 3, t->text, ui->clock);
        right -= 8;
    }
    char volume[8];
    snprintf(volume, sizeof volume, "%d", ui->volume);
    right -= font_width(FONT_REGULAR, 10, volume);
    font_draw(FONT_REGULAR, 10, right, 3, t->dim, volume);
    right -= 15;
    font_icon(ui->volume > 0 ? ICON_VOLUME : ICON_VOLUME_MUTE, 12, right, 5, t->dim);
}

static void draw_categories(const ui_state *ui)
{
    const ui_theme *t = &ui->theme;
    for (int i = 0; i < ui->cat_count; ++i) {
        const int cat = ui->cat_order[i];
        float d = (float)i - ui->cat_pos;
        float s = clampf(1.0f - fabsf(d), 0.0f, 1.0f);
        int size = (int)(22.0f + 12.0f * s + 0.5f);
        /* Neighbours step aside so the selected label has room. */
        float cx = CAT_ANCHOR + d * CAT_SPACING + (d < 0 ? -14.0f : 14.0f) * clampf(fabsf(d), 0.0f, 1.0f);
        if (cx < -40 || cx > SCREEN_W + 40) continue;
        float bottom = CAT_BOTTOM - 18.0f * s;
        font_icon(cat_icons[cat], size, cx - size / 2.0f, bottom - size,
                  color_fade(t->icons, 0.45f + 0.55f * s));
        if (s > 0.01f) {
            float w = font_width(FONT_BOLD, 11, cat_names[cat]);
            font_draw(FONT_BOLD, 11, cx - w / 2.0f, CAT_BOTTOM - 15, color_fade(t->text, s),
                      cat_names[cat]);
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

static void draw_list(const ui_state *ui, const ui_list *list, float scroll, float x, float top,
                      float bottom, const list_style *st, float alpha, int moving_row)
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
        if (i == moving_row) {
            draw_round_rect(cx - 6, y - 3, SCREEN_W - cx - 8, size + 6, 8,
                            color_fade(t->accent, 0.25f * a));
            font_draw(FONT_BOLD, 12, SCREEN_W - 32, y + (size - 16) / 2, color_fade(t->accent, a),
                      "\xE2\x86\x91\xE2\x86\x93");
        }
        float tx = cx;
        if (row->icon && !list->plain) {
            draw_round_rect(cx, y, size, size, size >= 40 ? 8.0f : 5.0f, color_fade(row->color, a));
            const int icon = (int)(size * 0.5f);
            font_icon(row->icon, icon, cx + (size - icon) / 2, y + (size - icon) / 2,
                      color_fade(0xFFFFFFFFu, a));
            tx += size + 10;
        } else if (st->covers && !list->plain) {
            draw_cover(row->cover, row->color, row->round, cx, y, size, a, s > 0.5f);
            tx += size + 10;
        } else if (row->playing) {
            /* Without covers, a bar marks the playing row. */
            draw_round_rect(cx - 8, y + size * 0.2f, 3, size * 0.6f, 1.5f, color_fade(t->accent, a));
        }
        const float max_w = SCREEN_W - tx - 16;
        uint32_t title_color = row->playing ? t->accent : s > 0.5f ? t->highlight : t->text;
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
    const int moving = ui->moving && cat == ui->cat ? ui->sel[cat][ui->level[cat]] : -1;
    draw_list(ui, &ui->lists[cat][0], ui->scroll[cat][0], x + slide - 60.0f * lp, top, bottom, st,
              alpha * (1.0f - lp), ui->level[cat] == 0 ? moving : -1);
    if (lp > 0.0f)
        draw_list(ui, &ui->lists[cat][1], ui->scroll[cat][1], x + slide + 60.0f * (1.0f - lp), top,
                  bottom, st, alpha * lp, ui->level[cat] == 1 ? moving : -1);
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
        const list_style st = style_for(ui, 1);
        draw_levels(ui, UI_CAT_SEARCH, 30 + slide, y + 34, bottom, &st, alpha, 0);
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
    r_rect(x, y, w * f, h, color_fade(t->progress, alpha));
}

/* The seek bar with its knob and the two times under it. */
static void draw_seek_bar(const ui_state *ui, float x, float y, float w, float alpha)
{
    const ui_theme *t = &ui->theme;
    const ui_now *n = &ui->now;
    draw_round_rect(x, y, w, 4, 2, color_fade(color_mix(t->bg, t->dim, 0.25f), alpha));
    float f = n->dur_ms ? clampf((float)n->pos_ms / (float)n->dur_ms, 0, 1) : 0;
    draw_round_rect(x, y, w * f, 4, 2, color_fade(t->progress, alpha));
    draw_round_rect(x + w * f - 5, y - 3, 10, 10, 5,
                    color_fade(n->seeking ? t->accent : t->text, alpha));
    char pos[16], dur[16];
    format_time(n->pos_ms, pos, sizeof pos);
    format_time(n->dur_ms, dur, sizeof dur);
    font_draw(FONT_REGULAR, 10, x, y + 8, color_fade(t->dim, alpha), pos);
    font_draw(FONT_REGULAR, 10, x + w - font_width(FONT_REGULAR, 10, dur), y + 8,
              color_fade(t->dim, alpha), dur);
}

/* Previous, play/pause, next, shuffle and repeat around centre line cy;
 * returns the x where the row ends. */
static float draw_controls(const ui_state *ui, float x, float cy, float alpha)
{
    const ui_theme *t = &ui->theme;
    const ui_now *n = &ui->now;
    font_icon(ICON_SKIP_BACK, 20, x, cy - 10, color_fade(t->text, alpha));
    draw_round_rect(x + 38, cy - 19, 38, 38, 19, color_fade(t->accent, alpha));
    font_icon(n->paused ? ICON_PLAY : ICON_PAUSE, 16, x + 49, cy - 8, color_fade(t->bg, alpha));
    font_icon(ICON_SKIP_FORWARD, 20, x + 94, cy - 10, color_fade(t->text, alpha));
    /* Shuffle and repeat, lit in the accent color when on. */
    font_icon(ICON_SHUFFLE, 16, x + 126, cy - 8, color_fade(n->shuffle ? t->accent : t->dim, alpha));
    font_icon(n->repeat == 2 ? ICON_REPEAT_ONE : ICON_REPEAT, 16, x + 150, cy - 8,
              color_fade(n->repeat ? t->accent : t->dim, alpha));
    return x + 176;
}

static void draw_next(const ui_state *ui, float from, float right, float y, float alpha)
{
    const ui_now *n = &ui->now;
    if (!n->next[0] || right - from < 40) return;
    char next[160];
    snprintf(next, sizeof next, "Next: %s", n->next);
    float max_w = right - from;
    float nw = font_width(FONT_REGULAR, 10, next);
    if (nw > max_w) nw = max_w;
    font_draw_fit(FONT_REGULAR, 10, right - nw, y, color_fade(ui->theme.dim, alpha), next, max_w);
}

static void draw_now_centered(const ui_state *ui, float alpha, float slide)
{
    const ui_theme *t = &ui->theme;
    const ui_now *n = &ui->now;
    const float size = 104, cx = (SCREEN_W - size) / 2 + slide, cy = 86;
    draw_cover(n->cover, n->color, 0, cx, cy, size, alpha, 1);
    const float w = SCREEN_W - 80;
    float tw = fminf(font_width(FONT_XBOLD, 17, n->title), w);
    font_draw_fit(FONT_XBOLD, 17, (SCREEN_W - tw) / 2 + slide, cy + size + 4, color_fade(t->text, alpha),
                  n->title, w);
    const char *sub = n->loading ? "Loading\xE2\x80\xA6" : n->artist;
    float sw = fminf(font_width(FONT_REGULAR, 12, sub), w);
    font_draw_fit(FONT_REGULAR, 12, (SCREEN_W - sw) / 2 + slide, cy + size + 26,
                  color_fade(color_mix(t->dim, t->text, 0.5f), alpha), sub, w);
    const float bar_x = 110 + slide, bar_w = SCREEN_W - 220, bar_y = 247;
    draw_round_rect(bar_x, bar_y, bar_w, 4, 2, color_fade(color_mix(t->bg, t->dim, 0.25f), alpha));
    float f = n->dur_ms ? clampf((float)n->pos_ms / (float)n->dur_ms, 0, 1) : 0;
    draw_round_rect(bar_x, bar_y, bar_w * f, 4, 2, color_fade(t->progress, alpha));
    draw_round_rect(bar_x + bar_w * f - 5, bar_y - 3, 10, 10, 5,
                    color_fade(n->seeking ? t->accent : t->text, alpha));
    char pos[16], dur[16];
    format_time(n->pos_ms, pos, sizeof pos);
    format_time(n->dur_ms, dur, sizeof dur);
    font_draw(FONT_REGULAR, 10, bar_x - 10 - font_width(FONT_REGULAR, 10, pos), bar_y - 5,
              color_fade(t->dim, alpha), pos);
    font_draw(FONT_REGULAR, 10, bar_x + bar_w + 10, bar_y - 5, color_fade(t->dim, alpha), dur);
    /* State icons at the sides: play/pause left, shuffle and repeat right. */
    font_icon(n->paused ? ICON_PLAY : ICON_PAUSE, 14, 30 + slide, bar_y - 5, color_fade(t->text, alpha));
    font_icon(ICON_SHUFFLE, 14, SCREEN_W - 64 + slide, bar_y - 5,
              color_fade(n->shuffle ? t->accent : t->dim, alpha));
    font_icon(n->repeat == 2 ? ICON_REPEAT_ONE : ICON_REPEAT, 14, SCREEN_W - 42 + slide, bar_y - 5,
              color_fade(n->repeat ? t->accent : t->dim, alpha));
    if (n->remote) font_icon(ICON_CAST, 14, cx + size + 8, cy, color_fade(t->accent, alpha));
}

/* Faint spectrum bars along the bottom edge, under the Now Playing screen. */
static void draw_visualizer(const ui_state *ui, float alpha)
{
    const int bands = 16;
    const float gap = 3, w = (SCREEN_W - gap * (float)(bands + 1)) / (float)bands;
    for (int b = 0; b < bands; ++b) {
        const float h = 4.0f + 56.0f * clampf(ui->now.spectrum[b], 0.0f, 1.0f);
        draw_round_rect(gap + (float)b * (w + gap), SCREEN_H - h, w, h + 4, 3,
                        color_fade(ui->theme.accent, 0.22f * alpha));
    }
}

static void draw_now(const ui_state *ui, float alpha, float slide)
{
    const ui_theme *t = &ui->theme;
    const ui_now *n = &ui->now;
    if (n->active && ui->style.visualizer) draw_visualizer(ui, alpha);
    if (!n->active) {
        font_draw(FONT_BOLD, 15, 100 + slide, 120, color_fade(t->text, alpha), "Nothing playing");
        font_draw(FONT_REGULAR, 12, 100 + slide, 142, color_fade(t->dim, alpha),
                  "Pick a song in Liked Songs, Playlists or Search.");
        return;
    }
    if (ui->style.now_layout == UI_NOW_CENTERED) {
        draw_now_centered(ui, alpha, slide);
        return;
    }
    const int text_only = ui->style.now_layout == UI_NOW_TEXT;
    const float cx = 30 + slide, cy = 92, size = text_only ? 0.0f : 150.0f;
    if (!text_only) draw_cover(n->cover, n->color, 0, cx, cy, size, alpha, 1);

    const float x = text_only ? 40 + slide : cx + size + 18, w = SCREEN_W - 24 - x;
    const int title = text_only ? 24 : 20, artist = text_only ? 15 : 13, album = text_only ? 12 : 11;
    float y = cy;
    font_draw_fit(FONT_XBOLD, title, x, y, color_fade(t->text, alpha), n->title, w);
    y += font_line_height(FONT_XBOLD, title) + 2;
    font_draw_fit(FONT_REGULAR, artist, x, y, color_fade(color_mix(t->dim, t->text, 0.5f), alpha),
                  n->artist, w);
    y += font_line_height(FONT_REGULAR, artist);
    font_draw_fit(FONT_REGULAR, album, x, y, color_fade(t->dim, alpha),
                  n->loading ? "Loading\xE2\x80\xA6" : n->album, w);

    draw_seek_bar(ui, x, 200, w, alpha);
    const float end = draw_controls(ui, x, 241, alpha);
    if (n->remote)
        font_icon(ICON_CAST, 14, x + w - 14, cy - 2, color_fade(t->accent, alpha));
    draw_next(ui, end, x + w, 241 - 6, alpha);
}

static void draw_mini(const ui_state *ui)
{
    const ui_theme *t = &ui->theme;
    const ui_now *n = &ui->now;
    const float y = SCREEN_H - MINI_H;
    r_rect(0, y, SCREEN_W, MINI_H, t->player_bar);
    draw_progress(ui, 0, y, SCREEN_W, 2, 1.0f);
    draw_cover(n->cover, n->color, 0, 12, y + 7, 22, 1.0f, 0);

    char span[48], pos[16], dur[16];
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

void ui_prebake(const ui_state *ui)
{
    /* Transparent draws bake an atlas without drawing anything. */
    for (int search = 0; search < 2; ++search) {
        const list_style st = style_for(ui, search);
        font_draw(FONT_REGULAR, st.title_small, 0, 0, 0, "a");
        font_draw(FONT_XBOLD, st.title_big, 0, 0, 0, "a");
    }
    if (ui->style.now_layout == UI_NOW_CENTERED) font_draw(FONT_XBOLD, 17, 0, 0, 0, "a");
    if (ui->style.now_layout == UI_NOW_TEXT) {
        font_draw(FONT_XBOLD, 24, 0, 0, 0, "a");
        font_draw(FONT_REGULAR, 15, 0, 0, 0, "a");
    }
}

void ui_draw(const ui_state *ui)
{
    const ui_theme *t = &ui->theme;
    r_clip(0, 0, 0, 0);
    if (ui->style.backdrop) {
        int w, h;
        r_tex_size(ui->style.backdrop, &w, &h);
        r_tex_draw(ui->style.backdrop, 0, 0, (float)w, (float)h, 0, 0, SCREEN_W, SCREEN_H, 0xFFFFFFFFu);
        r_rect(0, 0, SCREEN_W, SCREEN_H, color_fade(t->bg, clampf(ui->style.backdrop_dim, 0.0f, 1.0f)));
    } else {
        r_rect(0, 0, SCREEN_W, SCREEN_H, t->bg);
    }
    draw_top(ui);
    draw_categories(ui);

    const int mini = ui->now.active && ui->cat != UI_CAT_NOW;
    const float bottom = (float)(mini ? SCREEN_H - MINI_H : SCREEN_H);
    /* The new category's content slides in from the side it came from. */
    const float lag = (float)cat_slot(ui, ui->cat) - ui->cat_pos;
    const float alpha = clampf(1.0f - fabsf(lag) * 1.6f, 0.0f, 1.0f);
    const float slide = lag * 40.0f;
    switch (ui->cat) {
    case UI_CAT_SEARCH:
        draw_search(ui, bottom, alpha, slide);
        break;
    case UI_CAT_NOW:
        draw_now(ui, alpha, slide);
        break;
    default: {
        const list_style st = style_for(ui, 0);
        draw_levels(ui, ui->cat, LIST_X, LIST_TOP, bottom, &st, alpha, slide);
        break;
    }
    }
    if (mini) draw_mini(ui);

    if (ui->toast_left > 0 && ui->toast[0]) {
        const float a = clampf(ui->toast_left / 0.3f, 0.0f, 1.0f);
        /* Whole pixels, so the pill's pieces meet without a seam. */
        const float w = floorf(font_width(FONT_BOLD, 12, ui->toast) + 28);
        const float x = floorf((SCREEN_W - w) / 2), y = bottom - 34;
        draw_round_rect(x, y, w, 26, 13, color_fade(t->text, a));
        font_draw(FONT_BOLD, 12, x + 14, y + 5, color_fade(t->bg, a), ui->toast);
    }

    const float mp = ui->menu_pos;
    if (mp > 0.01f) {
        const ui_menu *m = &ui->menu;
        r_rect(0, 0, SCREEN_W, SCREEN_H, color_fade(0xFF000000u, 0.55f * mp));
        const float head = m->title[0] ? 26.0f : 0.0f;
        const float w = 230, h = (float)m->count * 26 + 16 + head;
        const float x = (SCREEN_W - w) / 2, y = (SCREEN_H - h) / 2 + (1.0f - mp) * 16;
        draw_round_rect(x, y, w, h, 12, color_fade(t->surface, mp));
        if (head > 0)
            font_draw_fit(FONT_BOLD, 13, x + 16, y + 10, color_fade(t->text, mp), m->title, w - 32);
        for (int i = 0; i < m->count; ++i) {
            const float iy = y + 8 + head + (float)i * 26;
            const int on = i == m->sel;
            if (on) draw_round_rect(x + 6, iy, w - 12, 24, 8, color_fade(t->accent, mp));
            font_draw_fit(on ? FONT_BOLD : FONT_REGULAR, 13, x + 16, iy + 4,
                          color_fade(on ? t->bg : t->text, mp), m->items[i], w - 32);
        }
    }
}
