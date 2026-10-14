#ifndef UI_THEME_H
#define UI_THEME_H
/* Themes are plain text (theme.ini) so they can be edited without code:
 *
 *   name = Midnight
 *   background = #0E0F13
 *   accent = #1ED760
 *
 * Missing or malformed lines keep the default value. */
#include <stddef.h>
#include <stdint.h>

typedef struct ui_theme {
    char name[32];
    uint32_t bg, surface, text, dim, accent;
} ui_theme;

void theme_defaults(ui_theme *t);
/* Applies the lines of an ini text over *t. Returns the number of lines
 * that were ignored (unknown key or bad color), for logging. */
int theme_parse(ui_theme *t, const char *text, size_t len);

#endif
