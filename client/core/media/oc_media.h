/* Shared types for recorded video messages (REQ-162–166, ARCH-110,
 * docs/VIDEO-MESSAGES.md): the one frame format every capture backend delivers,
 * the one clock video and audio are stamped with, and the conversions between
 * the frame format and what cameras and screens actually use.
 *
 * The frame format is fixed here, before any backend: planar I420, BT.709
 * limited range, even dimensions. libvpx takes I420 as its own input, so the
 * encoder and the preview never see a camera's native format. */
#ifndef OC_MEDIA_H
#define OC_MEDIA_H

#include <stddef.h>
#include <stdint.h>

#include "oc_frame.h"   /* the frame itself, shared with the daemon's export */

/* A monotonic clock in microseconds. Video frames and microphone buffers are
 * stamped from it, so neither device's own timestamps have to be trusted. */
int64_t oc_media_clock_us(void);
/* Wait until the clock reads `at_us` or later, for at most `max_ms` of real
 * time: 1 when it does, 0 when the wait ran out first. What the media threads
 * wait on, so that a clock a test holds holds them too. */
int     oc_media_clock_wait_until(int64_t at_us, int max_ms);

/* A test's clock. oc_media_clock_hold() stops it where it reads; from then on
 * it moves only by oc_media_clock_advance_us(), a step at a time, and the
 * media threads, which wait on it, with it. Unheld, an advance puts the clock
 * `us` further ahead at once, as if that much time had passed. 0 puts back the
 * system's clock, unheld, which is for when nothing that reads it is running.
 * oc_media_clock_waiting() is how many threads are waiting on the held clock
 * for a time still to come: the work they had for the time it reads is done.
 * Any thread. */
void    oc_media_clock_hold(void);
void    oc_media_clock_advance_us(int64_t us);
int     oc_media_clock_waiting(void);

/* An I420 frame whose planes live in one allocation (stride == plane width).
 * Returns 0 on success; the frame is zeroed on failure. */
int  oc_frame_alloc(oc_frame *f, int width, int height);
void oc_frame_free(oc_frame *f);
/* Copy pixels (and pts) from `src` into an allocated frame of the same size. */
void oc_frame_copy(oc_frame *dst, const oc_frame *src);

/* Conversions into I420, BT.709 limited range. `w`, `h` are the source
 * dimensions and must match the destination frame. */
void oc_i420_from_nv12(oc_frame *dst, const uint8_t *y, int y_stride,
                       const uint8_t *uv, int uv_stride);
void oc_i420_from_yuy2(oc_frame *dst, const uint8_t *src, int stride);
/* BGRA (as Windows and Direct2D order it), 8 bits per channel. */
void oc_i420_from_bgra(oc_frame *dst, const uint8_t *src, int stride);
/* RGBA (as a browser's canvas and most cameras elsewhere order it). */
void oc_i420_from_rgba(oc_frame *dst, const uint8_t *src, int stride);

/* I420 to BGRA with alpha 255, for the preview and the player. `dst` holds
 * width*height*4 bytes. */
void oc_i420_to_bgra(const oc_frame *src, uint8_t *dst, int dst_stride);

/* Scale an I420 frame to another size (bilinear). Used when the encoder steps
 * down resolution while recording. */
void oc_i420_scale(const oc_frame *src, oc_frame *dst);

/* A frame that is a window onto part of another: its planes point into `f`'s at
 * (x, y) with `f`'s strides, so scaling or filling into it draws into `f`. The
 * rectangle must lie inside `f`, with even position and size. 0, or -1. */
int  oc_i420_view(const oc_frame *f, int x, int y, int w, int h, oc_frame *view);
/* Paint every pixel of `f` one colour, given in Y, U and V. */
void oc_i420_fill(oc_frame *f, uint8_t y, uint8_t u, uint8_t v);
/* Scale `src` into `dst` keeping its shape, centred, with black bars where the
 * shapes differ -- a window that is resized while it is recorded still fills
 * the one size the recording has. */
void oc_i420_fit(const oc_frame *src, oc_frame *dst);

/* The camera box of a screen recording (REQ-162): a fifth of the frame's width,
 * the camera's shape, a margin of 2% of the width from the edges, in one corner.
 * Even position and size throughout, so it can be a view. */
enum { OC_CORNER_BR = 0, OC_CORNER_BL, OC_CORNER_TR, OC_CORNER_TL };
void oc_inset_rect(int fw, int fh, int cw, int ch, int corner, int *x, int *y, int *w, int *h);
/* Draw `cam` into `dst` in that box, with a 2-pixel light border around it. */
void oc_i420_inset(oc_frame *dst, const oc_frame *cam, int corner);

#endif
