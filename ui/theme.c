#include "theme.h"
#include <ctype.h>
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

int theme_parse(ui_theme *t, const char *text, size_t len)
{
    static const struct { const char *key; size_t offset; } colors[] = {
        { "background", offsetof(ui_theme, bg) },
        { "surface", offsetof(ui_theme, surface) },
        { "text", offsetof(ui_theme, text) },
        { "text_dim", offsetof(ui_theme, dim) },
        { "accent", offsetof(ui_theme, accent) },
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

        if (key_len == 4 && strncmp(key, "name", 4) == 0) {
            size_t n = value_len < sizeof t->name - 1 ? value_len : sizeof t->name - 1;
            memcpy(t->name, value, n);
            t->name[n] = '\0';
            continue;
        }
        int known = 0;
        for (size_t i = 0; i < sizeof colors / sizeof colors[0]; ++i) {
            if (strlen(colors[i].key) != key_len || strncmp(colors[i].key, key, key_len) != 0)
                continue;
            known = 1;
            uint32_t color;
            if (parse_color(value, value_len, &color))
                *(uint32_t *)((char *)t + colors[i].offset) = color;
            else
                ++ignored;
        }
        if (!known) ++ignored;
    }
    return ignored;
}
