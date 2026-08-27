/*
 * oc_gfx — portable drawing primitives for the graphical clients over an
 * SDL3 renderer (REQ-200; companion to sdltext, ARCH-106).
 *
 * The shapes here are exactly the ones the client draws — fills, rounded
 * rects and their strokes, lines, axis-aligned clips, textures (avatars,
 * thumbnails, rasterized text) and the Lucide stroke icons — not a general
 * vector API. SDL has no rounded rect, no path stroking and no bezier; this
 * layer tessellates those to triangles once, so the application layer above
 * it never meets SDL_RenderGeometry.
 *
 * Coordinates are DIPs; gfx_set_scale (DPI x zoom) maps them to pixels at
 * the seam, the same arrangement sdltext uses, so the scene code stays
 * scale-free. This header names no SDL type beyond an opaque forward
 * declaration — the application layer includes this, never SDL.h.
 */

#ifndef OC_GFX_H
#define OC_GFX_H

#include <stddef.h>
#include <stdint.h>

typedef struct SDL_Renderer SDL_Renderer;

typedef struct gfx     gfx;
typedef struct gfx_tex gfx_tex;

typedef struct { float x, y, w, h; } gfx_rect;

gfx  *gfx_create(SDL_Renderer *);
void  gfx_destroy(gfx *);
void  gfx_set_scale(gfx *, float scale);
float gfx_scale(const gfx *);

/* One frame: clear, draw, present. */
void gfx_begin(gfx *, uint32_t clear_rgb);
void gfx_end(gfx *);

void gfx_fill        (gfx *, gfx_rect r, uint32_t rgb, float a);
void gfx_fill_round  (gfx *, gfx_rect r, float radius, uint32_t rgb, float a);
void gfx_stroke_round(gfx *, gfx_rect r, float radius, float w, uint32_t rgb, float a);
void gfx_line        (gfx *, float x0, float y0, float x1, float y1,
                      float w, uint32_t rgb, float a);
/* Center + radii, DIPs — presence dots, avatar discs, radio marks. */
void gfx_ellipse       (gfx *, float cx, float cy, float rx, float ry,
                        uint32_t rgb, float a);
void gfx_ellipse_stroke(gfx *, float cx, float cy, float rx, float ry,
                        float w, uint32_t rgb, float a);

/* Axis-aligned clip stack; a push intersects with the current clip, matching
 * how nested Direct2D clips composed in the client. */
void gfx_clip_push(gfx *, gfx_rect r);
void gfx_clip_pop (gfx *);

/* Textures. `rgba` is straight (non-premultiplied) RGBA, the format image
 * decoders produce; `bgra_premul` is what sdltext's sink emits — text
 * uploads take that path so no conversion sits between raster and screen.
 * The pixels are copied; a texture survives its source buffer. */
gfx_tex *gfx_tex_create(gfx *, int w, int h, const void *rgba);
gfx_tex *gfx_tex_create_text(gfx *, const void *bgra_premul, int stride,
                             int w, int h);
void     gfx_tex_destroy(gfx_tex *);
/* radius > 0 rounds the corners (avatars); the texture is mapped onto the
 * rounded polygon by UV, not clipped, so edges stay antialias-free but
 * geometry-exact at any scale. `a` multiplies the texture's own alpha. */
void     gfx_tex_draw(gfx *, gfx_tex *, gfx_rect dst, float radius, float a);
/* Cover-fit: draw the rounded `shape` (a circle when radius reaches half its
 * side) sampling the texture as if it filled `map` — the enlarged, centered
 * rect a crop computes. The shape's geometry is exact; the overflow is what
 * gets cropped, which is what an avatar disc is. */
void     gfx_tex_draw_shaped(gfx *, gfx_tex *, gfx_rect shape, float radius,
                             gfx_rect map, float a);
void     gfx_tex_size(const gfx_tex *, int *w, int *h);

/* A Lucide icon (client/shared/icons.h), stroke-tessellated: beziers
 * flattened, the polyline expanded to `stroke_w` DIPs with round caps and
 * joins. `box` is fitted preserving the 24x24 viewBox aspect. */
void gfx_icon(gfx *, int icon_id, gfx_rect box, float stroke_w,
              uint32_t rgb, float a);

/* Read the current output back as straight RGBA, `w` x `h` from the origin
 * (pixels, not DIPs) — the screenshot path the test harness uses. Returns 0
 * on failure. */
int gfx_readback(gfx *, void *rgba, int w, int h);

#endif /* OC_GFX_H */
