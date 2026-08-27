/*
 * sdltext — the DirectWrite backend's constructor (Windows only).
 *
 * The portable API (sdltext.h) never names a platform type; creation is where
 * a client chooses its backend, so the constructor lives here.
 *
 * Pixels leave through a SINK: st_draw rasterizes the layout (premultiplied
 * BGRA, grayscale-antialiased — the mode the client has always used) and
 * hands the buffer to the sink, which uploads it wherever presentation
 * happens (an SDL texture, a test's memory buffer). The raster surface is
 * chosen over per-glyph GPU drawing because it was measured (spike 3): a WIC
 * software target rasters a styled transcript line in ~0.3 ms, while a DC
 * render target pays a ~3 ms GDI-interop tax per layout.
 *
 * st_dwrite_draw_rt is the transition path: it draws a layout straight into a
 * live ID2D1RenderTarget (passed as void* so this header needs no d2d1.h),
 * which lets the existing Direct2D-presented client adopt the sdltext API
 * wholesale before presentation moves to SDL. It is temporary by design and
 * leaves with the Direct2D presentation code.
 */

#ifndef ST_DWRITE_H
#define ST_DWRITE_H

#include "sdltext.h"

typedef struct {
    void *user;
    /* `bgra` is premultiplied, `stride` bytes per row, (px_w, px_h) pixels.
     * (dip_x, dip_y) is where st_draw was asked to place the layout, in DIPs;
     * the pixel dimensions already include the context scale. The buffer is
     * valid only for the duration of the call. */
    void (*blit)(void *user, const void *bgra, int stride,
                 int px_w, int px_h, float dip_x, float dip_y);
} st_sink;

/* `sink` may be NULL for a context used only to measure and hit-test (or one
 * that draws exclusively through st_dwrite_draw_rt). */
st_ctx *st_dwrite_create(const st_sink *sink);

void st_dwrite_draw_rt(st_ctx *, st_layout *, void *d2d_render_target,
                       float x, float y, uint32_t rgb, float alpha);

#endif /* ST_DWRITE_H */
