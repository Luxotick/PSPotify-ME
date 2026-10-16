/* render.h on SDL2's 2D renderer, for the PC preview. */
#include "render.h"
#include "render_sdl.h"
#include <stdlib.h>

struct r_tex {
    SDL_Texture *tex;
    int w, h;
};

static SDL_Renderer *renderer;

void render_sdl_init(SDL_Renderer *r)
{
    renderer = r;
}

static r_tex *wrap(SDL_Texture *tex, int w, int h, int smooth)
{
    if (!tex) return NULL;
    r_tex *t = malloc(sizeof *t);
    if (!t) {
        SDL_DestroyTexture(tex);
        return NULL;
    }
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(tex, smooth ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
    t->tex = tex;
    t->w = w;
    t->h = h;
    return t;
}

r_tex *r_tex_create(int w, int h, const uint32_t *argb, int smooth)
{
    SDL_Texture *tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                         SDL_TEXTUREACCESS_STATIC, w, h);
    if (tex) SDL_UpdateTexture(tex, NULL, argb, w * 4);
    return wrap(tex, w, h, smooth);
}

r_tex *r_tex_create_opaque(int w, int h, const uint32_t *argb, int smooth)
{
    return r_tex_create(w, h, argb, smooth);
}

r_tex *r_tex_create_alpha(int w, int h, const uint8_t *alpha, int smooth)
{
    uint32_t *argb = malloc((size_t)w * (size_t)h * 4);
    if (!argb) return NULL;
    for (int i = 0; i < w * h; ++i) argb[i] = ((uint32_t)alpha[i] << 24) | 0x00FFFFFFu;
    r_tex *t = r_tex_create(w, h, argb, smooth);
    free(argb);
    return t;
}

void r_tex_destroy(r_tex *t)
{
    if (!t) return;
    SDL_DestroyTexture(t->tex);
    free(t);
}

void r_tex_size(const r_tex *t, int *w, int *h)
{
    *w = t ? t->w : 0;
    *h = t ? t->h : 0;
}

void r_rect(float x, float y, float w, float h, uint32_t argb)
{
    if (w <= 0 || h <= 0) return;
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, (Uint8)(argb >> 16), (Uint8)(argb >> 8), (Uint8)argb,
                           (Uint8)(argb >> 24));
    SDL_FRect r = { x, y, w, h };
    SDL_RenderFillRectF(renderer, &r);
}

void r_tex_draw(r_tex *t, float sx, float sy, float sw, float sh,
                float x, float y, float w, float h, uint32_t tint)
{
    if (!t || w <= 0 || h <= 0) return;
    SDL_SetTextureColorMod(t->tex, (Uint8)(tint >> 16), (Uint8)(tint >> 8), (Uint8)tint);
    SDL_SetTextureAlphaMod(t->tex, (Uint8)(tint >> 24));
    SDL_Rect src = { (int)sx, (int)sy, (int)(sw + 0.5f), (int)(sh + 0.5f) };
    SDL_FRect dst = { x, y, w, h };
    SDL_RenderCopyF(renderer, t->tex, &src, &dst);
}

void r_clip(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) {
        SDL_RenderSetClipRect(renderer, NULL);
        return;
    }
    SDL_Rect r = { x, y, w, h };
    SDL_RenderSetClipRect(renderer, &r);
}
