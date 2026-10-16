#ifndef UI_RENDER_H
#define UI_RENDER_H
/* The few drawing primitives the UI needs. Each platform implements them:
 * sceGu on the PSP, SDL in the PC preview (tools/preview/render_sdl.c).
 * Coordinates are 480x272 screen pixels; colors are 0xAARRGGBB. */
#include <stdint.h>

typedef struct r_tex r_tex;

/* Full-color texture from 0xAARRGGBB words, w*h of them. */
r_tex *r_tex_create(int w, int h, const uint32_t *argb, int smooth);
/* Opaque picture (wallpapers): the alpha is ignored and the platform may
 * store it with fewer bits per pixel (16 on the PSP, dithered). */
r_tex *r_tex_create_opaque(int w, int h, const uint32_t *argb, int smooth);
/* Coverage-only texture (font atlas, masks): drawn as white * tint. */
r_tex *r_tex_create_alpha(int w, int h, const uint8_t *alpha, int smooth);
void r_tex_destroy(r_tex *t);
void r_tex_size(const r_tex *t, int *w, int *h);

void r_rect(float x, float y, float w, float h, uint32_t argb);
/* Draws the source rectangle (texels) of t into the destination rectangle,
 * multiplied by tint (0xFFFFFFFF = unchanged). */
void r_tex_draw(r_tex *t, float sx, float sy, float sw, float sh,
                float x, float y, float w, float h, uint32_t tint);
/* Limits drawing to a rectangle; w <= 0 turns clipping off. */
void r_clip(int x, int y, int w, int h);

#endif
