#include "theme.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

void theme_defaults(ui_theme *t)
{
    memset(t, 0, sizeof *t);
    strcpy(t->name, "Midnight");
    t->bg = 0xFF0E0F13u;
    t->surface = 0xFF1B1D24u;
    t->text = 0xFFF2F2F2u;
    t->dim = 0xFF9EA3AEu;
    t->accent = 0xFF1ED760u;
    t->wallpaper_dim = 40;
    theme_resolve(t);
}

void theme_resolve(ui_theme *t)
{
    if (!(t->set & THEME_SET_HIGHLIGHT)) t->highlight = t->text;
    if (!(t->set & THEME_SET_PROGRESS)) t->progress = t->accent;
    if (!(t->set & THEME_SET_PLAYER_BAR)) t->player_bar = t->surface;
    if (!(t->set & THEME_SET_ICONS)) t->icons = t->text;
}

static int hex_digit(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = tolower(c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* #RGB or #RRGGBB, the '#' optional. */
static int parse_color(const char *s, size_t len, uint32_t *out)
{
    if (len > 0 && s[0] == '#') { ++s; --len; }
    if (len != 3 && len != 6) return 0;
    uint32_t v = 0;
    for (size_t i = 0; i < len; ++i) {
        int d = hex_digit((unsigned char)s[i]);
        if (d < 0) return 0;
        v = (v << 4) | (uint32_t)d;
        if (len == 3) v = (v << 4) | (uint32_t)d;
    }
    *out = 0xFF000000u | v;
    return 1;
}

static void trim(const char **s, size_t *len)
{
    while (*len > 0 && isspace((unsigned char)**s)) { ++*s; --*len; }
    while (*len > 0 && isspace((unsigned char)(*s)[*len - 1])) --*len;
}

static void copy_value(char *dst, size_t cap, const char *value, size_t len)
{
    size_t n = len < cap - 1 ? len : cap - 1;
    memcpy(dst, value, n);
    dst[n] = '\0';
}

static int key_is(const char *key, size_t key_len, const char *name)
{
    return strlen(name) == key_len && strncmp(name, key, key_len) == 0;
}

int theme_parse(ui_theme *t, const char *text, size_t len)
{
    static const struct { const char *key; size_t offset; unsigned set; } colors[] = {
        { "background", offsetof(ui_theme, bg), 0 },
        { "surface", offsetof(ui_theme, surface), 0 },
        { "text", offsetof(ui_theme, text), 0 },
        { "text_dim", offsetof(ui_theme, dim), 0 },
        { "accent", offsetof(ui_theme, accent), 0 },
        { "highlight", offsetof(ui_theme, highlight), THEME_SET_HIGHLIGHT },
        { "progress", offsetof(ui_theme, progress), THEME_SET_PROGRESS },
        { "player_bar", offsetof(ui_theme, player_bar), THEME_SET_PLAYER_BAR },
        { "icons", offsetof(ui_theme, icons), THEME_SET_ICONS },
    };
    int ignored = 0;
    size_t pos = 0;
    while (pos < len) {
        size_t end = pos;
        while (end < len && text[end] != '\n') ++end;
        const char *line = text + pos;
        size_t line_len = end - pos;
        pos = end + 1;

        trim(&line, &line_len);
        if (line_len == 0 || line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
        const char *eq = memchr(line, '=', line_len);
        if (!eq) { ++ignored; continue; }
        const char *key = line, *value = eq + 1;
        size_t key_len = (size_t)(eq - line), value_len = line_len - key_len - 1;
        trim(&key, &key_len);
        trim(&value, &value_len);
        /* An inline comment: '#' or ';' after a space ("accent = #1ED760  # green"). */
        for (size_t i = 1; i < value_len; ++i)
            if ((value[i] == '#' || value[i] == ';') && isspace((unsigned char)value[i - 1])) {
                value_len = i;
                trim(&value, &value_len);
                break;
            }

        if (key_is(key, key_len, "name")) {
            copy_value(t->name, sizeof t->name, value, value_len);
            continue;
        }
        if (key_is(key, key_len, "wallpaper")) {
            /* A file name in the theme's folder, nothing more. */
            if (value_len == 0 || value_len >= sizeof t->wallpaper ||
                memchr(value, '/', value_len) || memchr(value, '\\', value_len) ||
                memchr(value, ':', value_len))
                ++ignored;
            else
                copy_value(t->wallpaper, sizeof t->wallpaper, value, value_len);
            continue;
        }
        if (key_is(key, key_len, "wallpaper_dim")) {
            char number[8];
            copy_value(number, sizeof number, value, value_len);
            char *stop;
            long v = strtol(number, &stop, 10);
            if (stop == number || *stop || v < 0 || v > 100) ++ignored;
            else t->wallpaper_dim = (int)v;
            continue;
        }
        int known = 0;
        for (size_t i = 0; i < sizeof colors / sizeof colors[0]; ++i) {
            if (!key_is(key, key_len, colors[i].key)) continue;
            known = 1;
            uint32_t color;
            if (parse_color(value, value_len, &color)) {
                *(uint32_t *)((char *)t + colors[i].offset) = color;
                t->set |= colors[i].set;
            } else {
                ++ignored;
            }
        }
        if (!known) ++ignored;
    }
    theme_resolve(t);
    return ignored;
}
