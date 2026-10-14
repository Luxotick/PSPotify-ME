#ifndef UI_FONT_H
#define UI_FONT_H
/* Anti-aliased text from TrueType fonts (stb_truetype). Each face and pixel
 * size is baked once into an atlas texture covering Latin-1, Latin
 * Extended-A (Turkish and most European letters) and common punctuation;
 * other characters draw as '?'. The icon face holds the Lucide icons. */
#include <stddef.h>
#include <stdint.h>

typedef enum {
    FONT_REGULAR,
    FONT_BOLD,
    FONT_XBOLD,
    FONT_ICONS,
    FONT_FACES
} font_face;

/* The TTF bytes must stay valid while the font is in use. */
int font_load(font_face face, const uint8_t *ttf, size_t len);

/* Size is the em size in pixels, like a CSS font-size. y is the top of the
 * line box (ascent above the baseline). Returns the advance width. */
float font_draw(font_face face, int size, float x, float y, uint32_t color, const char *utf8);
float font_width(font_face face, int size, const char *utf8);
/* Like font_draw, cut with an ellipsis to fit max_w. */
float font_draw_fit(font_face face, int size, float x, float y, uint32_t color,
                    const char *utf8, float max_w);
/* Line height (ascent - descent) for vertical layout. */
float font_line_height(font_face face, int size);

/* One icon from FONT_ICONS, drawn in a size x size box at (x, y). */
void font_icon(unsigned codepoint, int size, float x, float y, uint32_t color);

#endif
