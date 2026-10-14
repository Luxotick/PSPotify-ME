#include "draw.h"
#include "render.h"
#include <stdlib.h>

/* An anti-aliased disc; each quadrant is one rounded corner. */
#define DISC 64
static r_tex *disc;

int draw_init(void)
{
    uint8_t *mask = malloc(DISC * DISC);
    if (!mask) return -1;
    const float c = DISC / 2.0f, r2 = c * c;
    for (int y = 0; y < DISC; ++y)
        for (int x = 0; x < DISC; ++x) {
            int inside = 0;
            for (int sy = 0; sy < 4; ++sy)
                for (int sx = 0; sx < 4; ++sx) {
                    float dx = x + (sx + 0.5f) / 4.0f - c, dy = y + (sy + 0.5f) / 4.0f - c;
                    inside += dx * dx + dy * dy <= r2;
                }
            mask[y * DISC + x] = (uint8_t)(inside * 255 / 16);
        }
    disc = r_tex_create_alpha(DISC, DISC, mask, 1);
    free(mask);
    return disc ? 0 : -1;
}

void draw_round_rect(float x, float y, float w, float h, float r, uint32_t argb)
{
    if (w <= 0 || h <= 0 || (argb >> 24) == 0) return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r < 0.5f || !disc) {
        r_rect(x, y, w, h, argb);
        return;
    }
    const float q = DISC / 2.0f;
    r_tex_draw(disc, 0, 0, q, q, x, y, r, r, argb);
    r_tex_draw(disc, q, 0, q, q, x + w - r, y, r, r, argb);
    r_tex_draw(disc, 0, q, q, q, x, y + h - r, r, r, argb);
    r_tex_draw(disc, q, q, q, q, x + w - r, y + h - r, r, r, argb);
    r_rect(x + r, y, w - 2 * r, r, argb);
    r_rect(x, y + r, w, h - 2 * r, argb);
    r_rect(x + r, y + h - r, w - 2 * r, r, argb);
}

uint32_t color_fade(uint32_t argb, float a)
{
    if (a <= 0.0f) return argb & 0x00FFFFFFu;
    if (a >= 1.0f) return argb;
    uint32_t alpha = (uint32_t)((argb >> 24) * a + 0.5f);
    return (alpha << 24) | (argb & 0x00FFFFFFu);
}

uint32_t color_mix(uint32_t a, uint32_t b, float t)
{
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;
    uint32_t out = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        float ca = (float)((a >> shift) & 0xFF), cb = (float)((b >> shift) & 0xFF);
        out |= (uint32_t)(ca + (cb - ca) * t + 0.5f) << shift;
    }
    return out;
}
