#ifndef UI_PALETTE_H
#define UI_PALETTE_H
/* "Cover colors": the theme follows the album art of the playing track.
 * palette_extract finds the art's dominant hue; theme_tint recolors a theme
 * with it while keeping the theme's light or dark character. */
#include <stdint.h>
#include "theme.h"

typedef struct cover_palette {
    int has_hue;        /* 0 for greyscale or near-black/white art */
    float h, s, l;      /* the dominant hue, HSL in 0..1 */
    float mean_l;       /* average lightness of the whole image */
} cover_palette;

/* Samples at most a few thousand of the count 0xAARRGGBB pixels. */
void palette_extract(const uint32_t *argb, int count, cover_palette *out);
void theme_tint(const ui_theme *base, const cover_palette *p, ui_theme *out);

#endif
