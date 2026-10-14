#ifndef SPOTIFY_RENDER_GU_H
#define SPOTIFY_RENDER_GU_H
/* ui/render.h on the PSP's graphics engine (sceGu): double-buffered 32-bit
 * framebuffers, sprites for rectangles and textured quads, 8-bit alpha
 * textures (font atlases) through a white-to-transparent palette. */

void render_gu_init(void);
/* Starts a frame's display list; all r_* drawing goes between the two. */
void render_gu_begin(void);
/* Runs the list, waits for vblank and shows the frame. */
void render_gu_end(void);
void render_gu_term(void);

#endif
