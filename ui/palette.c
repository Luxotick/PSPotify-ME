#include "palette.h"
#include <math.h>
#include <string.h>

#define HUE_BINS 24
#define MAX_SAMPLES 4096
#define TWO_PI 6.28318530718f

static void to_hsl(uint32_t argb, float *h, float *s, float *l)
{
    float r = ((argb >> 16) & 0xFF) / 255.0f, g = ((argb >> 8) & 0xFF) / 255.0f,
          b = (argb & 0xFF) / 255.0f;
    float max = fmaxf(r, fmaxf(g, b)), min = fminf(r, fminf(g, b)), d = max - min;
    *l = (max + min) / 2;
    if (d < 1e-5f) {
        *h = *s = 0;
        return;
    }
    *s = *l > 0.5f ? d / (2 - max - min) : d / (max + min);
    float hue;
    if (max == r) hue = (g - b) / d + (g < b ? 6 : 0);
    else if (max == g) hue = (b - r) / d + 2;
    else hue = (r - g) / d + 4;
    *h = hue / 6;
}

static float hue_channel(float p, float q, float t)
{
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1.0f / 6) return p + (q - p) * 6 * t;
    if (t < 0.5f) return q;
    if (t < 2.0f / 3) return p + (q - p) * (2.0f / 3 - t) * 6;
    return p;
}

static uint32_t from_hsl(float h, float s, float l)
{
    float r, g, b;
    if (s <= 0) {
        r = g = b = l;
    } else {
        float q = l < 0.5f ? l * (1 + s) : l + s - l * s, p = 2 * l - q;
        r = hue_channel(p, q, h + 1.0f / 3);
        g = hue_channel(p, q, h);
        b = hue_channel(p, q, h - 1.0f / 3);
    }
    return 0xFF000000u | ((uint32_t)(r * 255 + 0.5f) << 16) | ((uint32_t)(g * 255 + 0.5f) << 8) |
           (uint32_t)(b * 255 + 0.5f);
}

void palette_extract(const uint32_t *argb, int count, cover_palette *out)
{
    /* Per hue bin, a weighted sum of unit vectors (hue is circular), plus
     * saturation and lightness, so the winner's average color comes out. */
    float weight[HUE_BINS], vx[HUE_BINS], vy[HUE_BINS], sat[HUE_BINS], lit[HUE_BINS];
    memset(weight, 0, sizeof weight);
    memset(vx, 0, sizeof vx);
    memset(vy, 0, sizeof vy);
    memset(sat, 0, sizeof sat);
    memset(lit, 0, sizeof lit);
    memset(out, 0, sizeof *out);
    if (!argb || count <= 0) return;

    int step = count / MAX_SAMPLES;
    if (step < 1) step = 1;
    float total_l = 0, samples = 0;
    for (int i = 0; i < count; i += step) {
        if ((argb[i] >> 24) < 128) continue;
        float h, s, l;
        to_hsl(argb[i], &h, &s, &l);
        total_l += l;
        samples += 1;
        /* Grey, near-black and near-white pixels name no hue. */
        if (s < 0.15f || l < 0.08f || l > 0.95f) continue;
        /* Vivid and brighter pixels count more: on a dark sleeve the small
         * bright detail is the color people notice. */
        float w = s * (0.25f + 0.75f * l);
        int bin = (int)(h * HUE_BINS);
        if (bin >= HUE_BINS) bin = HUE_BINS - 1;
        weight[bin] += w;
        vx[bin] += cosf(h * TWO_PI) * w;
        vy[bin] += sinf(h * TWO_PI) * w;
        sat[bin] += s * w;
        lit[bin] += l * w;
    }
    out->mean_l = samples > 0 ? total_l / samples : 0;

    /* The strongest hue, counting half of each neighbour bin. */
    int best = -1;
    float best_score = 0;
    for (int i = 0; i < HUE_BINS; ++i) {
        float score = weight[i] + 0.5f * (weight[(i + HUE_BINS - 1) % HUE_BINS] +
                                          weight[(i + 1) % HUE_BINS]);
        if (score > best_score) {
            best_score = score;
            best = i;
        }
    }
    /* A hue needs a real share of the image, not a few stray pixels. */
    if (best < 0 || best_score < fmaxf(samples * 0.02f, 8.0f)) return;

    float w = 0, x = 0, y = 0, s = 0, l = 0;
    for (int k = -1; k <= 1; ++k) {
        int i = (best + k + HUE_BINS) % HUE_BINS;
        w += weight[i];
        x += vx[i];
        y += vy[i];
        s += sat[i];
        l += lit[i];
    }
    float h = atan2f(y, x) / TWO_PI;
    out->has_hue = 1;
    out->h = h < 0 ? h + 1 : h;
    out->s = s / w;
    out->l = l / w;
}

static float luminance(uint32_t argb)
{
    return (0.2126f * ((argb >> 16) & 0xFF) + 0.7152f * ((argb >> 8) & 0xFF) +
            0.0722f * (argb & 0xFF)) / 255.0f;
}

static float clamp01(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

void theme_tint(const ui_theme *base, const cover_palette *p, ui_theme *out)
{
    *out = *base;
    const int dark = luminance(base->bg) < 0.5f;
    if (!p->has_hue) {
        /* Greyscale art: a neutral accent that still stands out. */
        out->accent = dark ? 0xFFE6E6E6u : 0xFF2A2A2Au;
        return;
    }
    const float h = p->h, s = p->s;
    /* Fixed lightness per role keeps text readable whatever the cover:
     * only hue and (bounded) saturation come from the art. */
    const float wash = clamp01(s, 0.0f, 0.6f);
    const float accent_s = clamp01(s, 0.45f, 0.85f);
    if (dark) {
        out->bg = from_hsl(h, wash * 0.55f, 0.10f);
        out->surface = from_hsl(h, wash * 0.55f, 0.16f);
        out->dim = from_hsl(h, 0.18f, 0.70f);
        out->accent = from_hsl(h, accent_s, 0.62f);
    } else {
        out->bg = from_hsl(h, wash * 0.5f, 0.93f);
        out->surface = from_hsl(h, wash * 0.5f, 0.98f);
        out->dim = from_hsl(h, 0.12f, 0.36f);
        out->accent = from_hsl(h, accent_s, 0.34f);
    }
}
