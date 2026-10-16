#ifndef UI_THEME_H
#define UI_THEME_H
/* Themes are plain text (theme.ini) so they can be edited without code:
 *
 *   name = Midnight
 *   background = #0E0F13
 *   accent = #1ED760
 *
 * Missing or malformed lines keep the default value. The detail colors
 * (highlight, progress, player_bar, icons) follow the main five unless a
 * theme sets them. */
#include <stddef.h>
#include <stdint.h>

/* Bits of ui_theme.set: detail colors the theme chose itself. */
enum {
    THEME_SET_HIGHLIGHT = 1 << 0,
    THEME_SET_PROGRESS = 1 << 1,
    THEME_SET_PLAYER_BAR = 1 << 2,
    THEME_SET_ICONS = 1 << 3
};

typedef struct ui_theme {
    char name[32];
    uint32_t bg, surface, text, dim, accent;
    /* Detail colors, always filled in (see theme_resolve). */
    uint32_t highlight;     /* title of the selected row */
    uint32_t progress;      /* filled part of the progress bars */
    uint32_t player_bar;    /* background of the mini player */
    uint32_t icons;         /* category icons */
    unsigned set;           /* THEME_SET_* */
    /* Optional picture behind everything: a JPEG or PNG in the theme's
     * folder, darkened by wallpaper_dim percent of the background color. */
    char wallpaper[48];
    int wallpaper_dim;
} ui_theme;

void theme_defaults(ui_theme *t);
/* Applies the lines of an ini text over *t. Returns the number of lines
 * that were ignored (unknown key or bad value), for logging. */
int theme_parse(ui_theme *t, const char *text, size_t len);
/* Derives the detail colors the theme did not set from the main ones. */
void theme_resolve(ui_theme *t);

#endif
