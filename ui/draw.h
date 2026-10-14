#ifndef UI_DRAW_H
#define UI_DRAW_H
/* Shapes built from the render.h primitives, shared by every screen. */
#include <stdint.h>

int draw_init(void);
void draw_round_rect(float x, float y, float w, float h, float r, uint32_t argb);
/* Multiplies the color's alpha by a (0..1). */
uint32_t color_fade(uint32_t argb, float a);
/* Linear blend from a (t = 0) to b (t = 1), alpha included. */
uint32_t color_mix(uint32_t a, uint32_t b, float t);

#endif
