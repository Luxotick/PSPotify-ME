#include "font.h"
#include "icons.h"
#include "render.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

/* Glyph slots in a text atlas: ASCII, then U+00A0-U+017F, then extras. */
#define ASCII_FIRST 0x20
#define ASCII_COUNT 95
#define LATIN_FIRST 0xA0
#define LATIN_COUNT 224
static int text_extras[] = {
    0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2026,
    0x2190, 0x2191, 0x2192, 0x2193, 0x25A1, 0x25B3, 0x25CB,
};
#define EXTRA_COUNT ((int)(sizeof text_extras / sizeof text_extras[0]))
#define TEXT_SLOTS (ASCII_COUNT + LATIN_COUNT + EXTRA_COUNT)

static int icon_codepoints[] = { ICON_LIST };
#define ICON_SLOTS ((int)(sizeof icon_codepoints / sizeof icon_codepoints[0]))

typedef struct atlas {
    int used;
    font_face face;
    int size;
    r_tex *tex;
    int w, h;
    stbtt_packedchar *chars;
    float ascent, descent;
} atlas;

static struct {
    const uint8_t *data;
    stbtt_fontinfo info;
    int loaded;
} faces[FONT_FACES];

#define MAX_ATLASES 24
static atlas atlases[MAX_ATLASES];

int font_load(font_face face, const uint8_t *ttf, size_t len)
{
    (void)len;
    if (face < 0 || face >= FONT_FACES) return -1;
    if (!stbtt_InitFont(&faces[face].info, ttf, stbtt_GetFontOffsetForIndex(ttf, 0))) return -1;
    faces[face].data = ttf;
    faces[face].loaded = 1;
    return 0;
}

static int bake(atlas *a)
{
    const int slots = a->face == FONT_ICONS ? ICON_SLOTS : TEXT_SLOTS;
    a->chars = calloc((size_t)slots, sizeof *a->chars);
    if (!a->chars) return -1;
    /* Smallest power-of-two atlas the glyphs fit in (the PSP's GE wants
     * power-of-two textures, at most 512x512). */
    for (int w = 128, h = 128; h <= 512; ) {
        uint8_t *pixels = calloc((size_t)w * (size_t)h, 1);
        if (!pixels) return -1;
        stbtt_pack_context pc;
        int ok = stbtt_PackBegin(&pc, pixels, w, h, 0, 1, NULL);
        if (ok) {
            stbtt_pack_range ranges[3];
            int n = 0;
            memset(ranges, 0, sizeof ranges);
            float font_size = STBTT_POINT_SIZE((float)a->size);
            if (a->face == FONT_ICONS) {
                ranges[n].font_size = font_size;
                ranges[n].array_of_unicode_codepoints = icon_codepoints;
                ranges[n].num_chars = ICON_SLOTS;
                ranges[n].chardata_for_range = a->chars;
                ++n;
            } else {
                ranges[n].font_size = font_size;
                ranges[n].first_unicode_codepoint_in_range = ASCII_FIRST;
                ranges[n].num_chars = ASCII_COUNT;
                ranges[n].chardata_for_range = a->chars;
                ++n;
                ranges[n].font_size = font_size;
                ranges[n].first_unicode_codepoint_in_range = LATIN_FIRST;
                ranges[n].num_chars = LATIN_COUNT;
                ranges[n].chardata_for_range = a->chars + ASCII_COUNT;
                ++n;
                ranges[n].font_size = font_size;
                ranges[n].array_of_unicode_codepoints = text_extras;
                ranges[n].num_chars = EXTRA_COUNT;
                ranges[n].chardata_for_range = a->chars + ASCII_COUNT + LATIN_COUNT;
                ++n;
            }
            ok = stbtt_PackFontRanges(&pc, faces[a->face].data, 0, ranges, n);
            stbtt_PackEnd(&pc);
        }
        if (ok) {
            a->tex = r_tex_create_alpha(w, h, pixels, 0);
            a->w = w;
            a->h = h;
            free(pixels);
            return a->tex ? 0 : -1;
        }
        free(pixels);
        if (w == h) w *= 2; else h *= 2;
        if (w > 512) break;
    }
    return -1;
}

static atlas *get_atlas(font_face face, int size)
{
    if (face < 0 || face >= FONT_FACES || !faces[face].loaded || size <= 0) return NULL;
    for (int i = 0; i < MAX_ATLASES; ++i)
        if (atlases[i].used && atlases[i].face == face && atlases[i].size == size)
            return atlases[i].tex ? &atlases[i] : NULL;
    for (int i = 0; i < MAX_ATLASES; ++i) {
        if (atlases[i].used) continue;
        atlas *a = &atlases[i];
        a->used = 1;
        a->face = face;
        a->size = size;
        const stbtt_fontinfo *info = &faces[face].info;
        float scale = stbtt_ScaleForMappingEmToPixels(info, (float)size);
        int ascent, descent, gap;
        stbtt_GetFontVMetrics(info, &ascent, &descent, &gap);
        a->ascent = ascent * scale;
        a->descent = descent * scale;
        if (bake(a) < 0) return NULL;   /* stays used: do not retry every frame */
        return a;
    }
    return NULL;
}

static unsigned next_codepoint(const char **p)
{
    const unsigned char *s = (const unsigned char *)*p;
    unsigned c = s[0];
    if (c < 0x80) { *p += 1; return c; }
    if ((c & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
        *p += 2;
        return ((c & 0x1Fu) << 6) | (s[1] & 0x3Fu);
    }
    if ((c & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        *p += 3;
        return ((c & 0x0Fu) << 12) | ((s[1] & 0x3Fu) << 6) | (s[2] & 0x3Fu);
    }
    if ((c & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 &&
        (s[3] & 0xC0) == 0x80) {
        *p += 4;
        return ((c & 0x07u) << 18) | ((s[1] & 0x3Fu) << 12) | ((s[2] & 0x3Fu) << 6) |
               (s[3] & 0x3Fu);
    }
    *p += 1;
    return '?';
}

static int text_slot(unsigned cp)
{
    if (cp >= ASCII_FIRST && cp < ASCII_FIRST + ASCII_COUNT) return (int)(cp - ASCII_FIRST);
    if (cp >= LATIN_FIRST && cp < LATIN_FIRST + LATIN_COUNT)
        return ASCII_COUNT + (int)(cp - LATIN_FIRST);
    for (int i = 0; i < EXTRA_COUNT; ++i)
        if ((unsigned)text_extras[i] == cp) return ASCII_COUNT + LATIN_COUNT + i;
    return '?' - ASCII_FIRST;
}

/* Draws at most len bytes of utf8 (len < 0: all). Returns the advance. */
static float draw_text(atlas *a, float x, float y, uint32_t color, const char *s, int len)
{
    float pen = floorf(x + 0.5f);
    float base = floorf(y + a->ascent + 0.5f);
    const char *end = len < 0 ? NULL : s + len;
    while (*s && (!end || s < end)) {
        unsigned cp = next_codepoint(&s);
        stbtt_aligned_quad q;
        float baseline = base;
        stbtt_GetPackedQuad(a->chars, a->w, a->h, text_slot(cp), &pen, &baseline, &q, 1);
        if (q.x1 > q.x0)
            r_tex_draw(a->tex, q.s0 * a->w, q.t0 * a->h, (q.s1 - q.s0) * a->w,
                       (q.t1 - q.t0) * a->h, q.x0, q.y0, q.x1 - q.x0, q.y1 - q.y0, color);
    }
    return pen - floorf(x + 0.5f);
}

float font_draw(font_face face, int size, float x, float y, uint32_t color, const char *utf8)
{
    atlas *a = get_atlas(face, size);
    return a && utf8 ? draw_text(a, x, y, color, utf8, -1) : 0.0f;
}

float font_width(font_face face, int size, const char *utf8)
{
    atlas *a = get_atlas(face, size);
    if (!a || !utf8) return 0.0f;
    float w = 0.0f;
    while (*utf8) w += a->chars[text_slot(next_codepoint(&utf8))].xadvance;
    return w;
}

float font_draw_fit(font_face face, int size, float x, float y, uint32_t color,
                    const char *utf8, float max_w)
{
    atlas *a = get_atlas(face, size);
    if (!a || !utf8) return 0.0f;
    if (font_width(face, size, utf8) <= max_w) return draw_text(a, x, y, color, utf8, -1);
    static const char ellipsis[] = "\xE2\x80\xA6";
    float room = max_w - a->chars[text_slot(0x2026)].xadvance, w = 0.0f;
    const char *p = utf8, *cut = utf8;
    while (*p) {
        float adv = a->chars[text_slot(next_codepoint(&p))].xadvance;
        if (w + adv > room) break;
        w += adv;
        cut = p;
    }
    /* No space before the ellipsis. */
    while (cut > utf8 && cut[-1] == ' ') --cut;
    float drawn = draw_text(a, x, y, color, utf8, (int)(cut - utf8));
    return drawn + draw_text(a, x + drawn, y, color, ellipsis, -1);
}

float font_line_height(font_face face, int size)
{
    atlas *a = get_atlas(face, size);
    return a ? a->ascent - a->descent : (float)size;
}

void font_icon(unsigned codepoint, int size, float x, float y, uint32_t color)
{
    atlas *a = get_atlas(FONT_ICONS, size);
    if (!a) return;
    int slot = -1;
    for (int i = 0; i < ICON_SLOTS; ++i)
        if ((unsigned)icon_codepoints[i] == codepoint) slot = i;
    if (slot < 0) return;
    /* Lucide glyphs fill the em square, which sits on the baseline. */
    float pen = floorf(x + 0.5f), baseline = floorf(y + 0.5f) + (float)size;
    stbtt_aligned_quad q;
    stbtt_GetPackedQuad(a->chars, a->w, a->h, slot, &pen, &baseline, &q, 1);
    if (q.x1 > q.x0)
        r_tex_draw(a->tex, q.s0 * a->w, q.t0 * a->h, (q.s1 - q.s0) * a->w, (q.t1 - q.t0) * a->h,
                   q.x0, q.y0, q.x1 - q.x0, q.y1 - q.y0, color);
}
