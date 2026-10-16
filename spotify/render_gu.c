#include "render_gu.h"
#include "../ui/render.h"
#include <malloc.h>
#include <pspdisplay.h>
/* pspgu.h declares a few functions without prototypes. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-prototypes"
#include <pspgu.h>
#pragma GCC diagnostic pop
#include <pspkernel.h>
#include <stdlib.h>
#include <string.h>

#define BUF_W 512
#define SCR_W 480
#define SCR_H 272
#define FB_SIZE (BUF_W * SCR_H * 4)

/* One frame's commands and vertices: ~70 bytes per glyph or rectangle, so
 * 256 KB covers several thousand per frame. */
static unsigned int __attribute__((aligned(64))) gu_list[64 * 1024];
/* 8-bit textures are coverage: entry i is white with alpha i (ABGR). */
static uint32_t __attribute__((aligned(16))) alpha_clut[256];

struct r_tex {
    void *data;
    int w, h;       /* the image */
    int tw, th;     /* power-of-two texture holding it */
    int psm;
    int smooth;
};

typedef struct {
    float u, v;
    uint32_t color;
    float x, y, z;
} tex_vertex;

typedef struct {
    uint32_t color;
    float x, y, z;
} color_vertex;

static const r_tex *bound;
static int texturing = -1;

/* The GE takes colors as 0xAABBGGRR. */
static uint32_t abgr(uint32_t argb)
{
    return (argb & 0xFF00FF00u) | ((argb >> 16) & 0xFFu) | ((argb & 0xFFu) << 16);
}

static int pow2(int v)
{
    int p = 8;
    while (p < v) p <<= 1;
    return p;
}

void render_gu_init(void)
{
    for (int i = 0; i < 256; ++i) alpha_clut[i] = ((uint32_t)i << 24) | 0x00FFFFFFu;
    sceKernelDcacheWritebackRange(alpha_clut, sizeof alpha_clut);

    sceGuInit();
    sceGuStart(GU_DIRECT, gu_list);
    sceGuDrawBuffer(GU_PSM_8888, (void *)0, BUF_W);
    sceGuDispBuffer(SCR_W, SCR_H, (void *)FB_SIZE, BUF_W);
    sceGuDepthBuffer((void *)(2 * FB_SIZE), BUF_W);
    sceGuOffset(2048 - SCR_W / 2, 2048 - SCR_H / 2);
    sceGuViewport(2048, 2048, SCR_W, SCR_H);
    sceGuScissor(0, 0, SCR_W, SCR_H);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuClearColor(0xFF000000u);
    sceGuClear(GU_COLOR_BUFFER_BIT);
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
}

void render_gu_begin(void)
{
    sceGuStart(GU_DIRECT, gu_list);
    bound = NULL;
    texturing = -1;
}

void render_gu_finish(void)
{
    sceGuFinish();
    sceGuSync(0, 0);
}

void render_gu_present(void)
{
    sceDisplayWaitVblankStartCB();
    sceGuSwapBuffers();
}

void render_gu_end(void)
{
    render_gu_finish();
    render_gu_present();
}

void render_gu_term(void)
{
    sceGuDisplay(GU_FALSE);
    sceGuTerm();
}

static r_tex *tex_alloc(int w, int h, int bytes_per_pixel, int psm, int smooth)
{
    if (w <= 0 || h <= 0 || w > 512 || h > 512) return NULL;
    r_tex *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->w = w;
    t->h = h;
    /* 8-bit rows must be a multiple of 16 bytes. */
    t->tw = bytes_per_pixel == 1 && pow2(w) < 16 ? 16 : pow2(w);
    t->th = pow2(h);
    t->psm = psm;
    t->smooth = smooth;
    t->data = memalign(16, (size_t)t->tw * (size_t)t->th * (size_t)bytes_per_pixel);
    if (!t->data) {
        free(t);
        return NULL;
    }
    memset(t->data, 0, (size_t)t->tw * (size_t)t->th * (size_t)bytes_per_pixel);
    return t;
}

r_tex *r_tex_create(int w, int h, const uint32_t *argb, int smooth)
{
    r_tex *t = tex_alloc(w, h, 4, GU_PSM_8888, smooth);
    if (!t) return NULL;
    uint32_t *dst = t->data;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) dst[y * t->tw + x] = abgr(argb[y * w + x]);
    sceKernelDcacheWritebackRange(t->data, (unsigned)(t->tw * t->th * 4));
    return t;
}

/* 0xAARRGGBB to the GE's 5:6:5 (red in the low bits), with a 4x4 ordered
 * dither so gradients in wallpapers do not band. */
r_tex *r_tex_create_opaque(int w, int h, const uint32_t *argb, int smooth)
{
    static const unsigned char bayer[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
    r_tex *t = tex_alloc(w, h, 2, GU_PSM_5650, smooth);
    if (!t) return NULL;
    uint16_t *dst = t->data;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const uint32_t c = argb[y * w + x];
            const unsigned d = bayer[(y & 3) * 4 + (x & 3)];
            unsigned r = ((c >> 16) & 0xFFu) + (d >> 1), g = ((c >> 8) & 0xFFu) + (d >> 2),
                     b = (c & 0xFFu) + (d >> 1);
            r = r > 255 ? 255 : r;
            g = g > 255 ? 255 : g;
            b = b > 255 ? 255 : b;
            dst[y * t->tw + x] = (uint16_t)((r >> 3) | ((g >> 2) << 5) | ((b >> 3) << 11));
        }
    sceKernelDcacheWritebackRange(t->data, (unsigned)(t->tw * t->th * 2));
    return t;
}

r_tex *r_tex_create_alpha(int w, int h, const uint8_t *alpha, int smooth)
{
    r_tex *t = tex_alloc(w, h, 1, GU_PSM_T8, smooth);
    if (!t) return NULL;
    uint8_t *dst = t->data;
    for (int y = 0; y < h; ++y) memcpy(dst + y * t->tw, alpha + y * w, (size_t)w);
    sceKernelDcacheWritebackRange(t->data, (unsigned)(t->tw * t->th));
    return t;
}

void r_tex_destroy(r_tex *t)
{
    if (!t) return;
    if (bound == t) bound = NULL;
    free(t->data);
    free(t);
}

void r_tex_size(const r_tex *t, int *w, int *h)
{
    *w = t ? t->w : 0;
    *h = t ? t->h : 0;
}

static void set_texturing(int on)
{
    if (texturing == on) return;
    texturing = on;
    if (on) sceGuEnable(GU_TEXTURE_2D);
    else sceGuDisable(GU_TEXTURE_2D);
}

static void bind(const r_tex *t)
{
    if (bound == t) return;
    bound = t;
    if (t->psm == GU_PSM_T8) {
        sceGuClutMode(GU_PSM_8888, 0, 0xFF, 0);
        sceGuClutLoad(256 / 8, alpha_clut);
    }
    sceGuTexMode(t->psm, 0, 0, 0);
    sceGuTexImage(0, t->tw, t->th, t->tw, t->data);
    sceGuTexFilter(t->smooth ? GU_LINEAR : GU_NEAREST, t->smooth ? GU_LINEAR : GU_NEAREST);
    sceGuTexFlush();
}

void r_rect(float x, float y, float w, float h, uint32_t argb)
{
    if (w <= 0 || h <= 0 || (argb >> 24) == 0) return;
    set_texturing(0);
    color_vertex *v = sceGuGetMemory(2 * sizeof *v);
    const uint32_t c = abgr(argb);
    v[0] = (color_vertex){ c, x, y, 0 };
    v[1] = (color_vertex){ c, x + w, y + h, 0 };
    sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, 0, v);
}

void r_tex_draw(r_tex *t, float sx, float sy, float sw, float sh,
                float x, float y, float w, float h, uint32_t tint)
{
    if (!t || w <= 0 || h <= 0 || (tint >> 24) == 0) return;
    set_texturing(1);
    bind(t);
    const uint32_t c = abgr(tint);
    /* Wide sprites from a big texture thrash the GE's texture cache: draw
     * them as vertical strips of at most 64 texels. */
    const int strips = sw > 64.0f && t->tw >= 256 ? (int)((sw + 63.0f) / 64.0f) : 1;
    tex_vertex *v = sceGuGetMemory((unsigned)(2 * strips) * sizeof *v);
    for (int i = 0; i < strips; ++i) {
        const float u0 = sw * (float)i / (float)strips, u1 = sw * (float)(i + 1) / (float)strips;
        const float x0 = w * (float)i / (float)strips, x1 = w * (float)(i + 1) / (float)strips;
        v[2 * i] = (tex_vertex){ sx + u0, sy, c, x + x0, y, 0 };
        v[2 * i + 1] = (tex_vertex){ sx + u1, sy + sh, c, x + x1, y + h, 0 };
    }
    sceGuDrawArray(GU_SPRITES,
                   GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D,
                   2 * strips, 0, v);
}

void r_clip(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) {
        sceGuScissor(0, 0, SCR_W, SCR_H);
        return;
    }
    /* pspsdk's sceGuScissor takes a width and height. */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SCR_W) w = SCR_W - x;
    if (y + h > SCR_H) h = SCR_H - y;
    if (w <= 0 || h <= 0) { x = y = 0; w = h = 1; }
    sceGuScissor(x, y, w, h);
}
