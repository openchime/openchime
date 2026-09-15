/* The frame format's allocations, clock and conversions (oc_media.h).
 *
 * BT.709 limited range throughout: Y in 16..235, Cb/Cr in 16..240. The
 * coefficients are the standard integer approximations, scaled by 256, so every
 * conversion here is integer arithmetic with rounding. */
#define _POSIX_C_SOURCE 200809L
#include "oc_media.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
int64_t oc_media_clock_us(void) {
    static LARGE_INTEGER freq;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (int64_t)(now.QuadPart / freq.QuadPart) * 1000000 +
           (int64_t)(now.QuadPart % freq.QuadPart) * 1000000 / freq.QuadPart;
}
#else
#include <time.h>
int64_t oc_media_clock_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
#endif

int oc_frame_alloc(oc_frame *f, int width, int height) {
    memset(f, 0, sizeof *f);
    if (width <= 0 || height <= 0 || (width & 1) || (height & 1)) return -1;
    size_t ys = (size_t)width * (size_t)height, cs = ys / 4;
    uint8_t *buf = malloc(ys + 2 * cs);
    if (!buf) return -1;
    f->width = width; f->height = height;
    f->plane[0] = buf; f->plane[1] = buf + ys; f->plane[2] = buf + ys + cs;
    f->stride[0] = width; f->stride[1] = f->stride[2] = width / 2;
    return 0;
}

void oc_frame_free(oc_frame *f) {
    if (f && f->plane[0]) free(f->plane[0]);
    if (f) memset(f, 0, sizeof *f);
}

void oc_frame_copy(oc_frame *dst, const oc_frame *src) {
    dst->pts_us = src->pts_us;
    for (int p = 0; p < 3; p++) {
        int w = p ? src->width / 2 : src->width, h = p ? src->height / 2 : src->height;
        for (int r = 0; r < h; r++)
            memcpy(dst->plane[p] + r * dst->stride[p], src->plane[p] + r * src->stride[p], (size_t)w);
    }
}

static inline uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

void oc_i420_from_nv12(oc_frame *dst, const uint8_t *y, int y_stride,
                       const uint8_t *uv, int uv_stride) {
    int w = dst->width, h = dst->height;
    for (int r = 0; r < h; r++)
        memcpy(dst->plane[0] + r * dst->stride[0], y + r * y_stride, (size_t)w);
    for (int r = 0; r < h / 2; r++) {
        const uint8_t *s = uv + r * uv_stride;
        uint8_t *u = dst->plane[1] + r * dst->stride[1], *v = dst->plane[2] + r * dst->stride[2];
        for (int c = 0; c < w / 2; c++) { u[c] = s[2 * c]; v[c] = s[2 * c + 1]; }
    }
}

void oc_i420_from_yuy2(oc_frame *dst, const uint8_t *src, int stride) {
    int w = dst->width, h = dst->height;
    for (int r = 0; r < h; r++) {
        const uint8_t *s = src + r * stride;
        uint8_t *yy = dst->plane[0] + r * dst->stride[0];
        for (int c = 0; c < w; c++) yy[c] = s[2 * c];
        if (r & 1) continue;
        /* Chroma from the even row of each pair, averaged with the odd row. */
        const uint8_t *s2 = src + (r + 1) * stride;
        uint8_t *u = dst->plane[1] + (r / 2) * dst->stride[1], *v = dst->plane[2] + (r / 2) * dst->stride[2];
        for (int c = 0; c < w / 2; c++) {
            u[c] = (uint8_t)((s[4 * c + 1] + s2[4 * c + 1] + 1) >> 1);
            v[c] = (uint8_t)((s[4 * c + 3] + s2[4 * c + 3] + 1) >> 1);
        }
    }
}

void oc_i420_from_bgra(oc_frame *dst, const uint8_t *src, int stride) {
    int w = dst->width, h = dst->height;
    for (int r = 0; r < h; r++) {
        const uint8_t *s = src + r * stride;
        uint8_t *yy = dst->plane[0] + r * dst->stride[0];
        for (int c = 0; c < w; c++) {
            int b = s[4 * c], g = s[4 * c + 1], rr = s[4 * c + 2];
            yy[c] = clamp8(((47 * rr + 157 * g + 16 * b + 128) >> 8) + 16);
        }
    }
    for (int r = 0; r < h / 2; r++) {
        uint8_t *u = dst->plane[1] + r * dst->stride[1], *v = dst->plane[2] + r * dst->stride[2];
        for (int c = 0; c < w / 2; c++) {
            int sb = 0, sg = 0, sr = 0;
            for (int dy = 0; dy < 2; dy++)
                for (int dx = 0; dx < 2; dx++) {
                    const uint8_t *p = src + (2 * r + dy) * stride + 4 * (2 * c + dx);
                    sb += p[0]; sg += p[1]; sr += p[2];
                }
            sb = (sb + 2) >> 2; sg = (sg + 2) >> 2; sr = (sr + 2) >> 2;
            u[c] = clamp8(((-26 * sr - 87 * sg + 112 * sb + 128) >> 8) + 128);
            v[c] = clamp8(((112 * sr - 102 * sg - 10 * sb + 128) >> 8) + 128);
        }
    }
}

void oc_i420_to_bgra(const oc_frame *src, uint8_t *dst, int dst_stride) {
    int w = src->width, h = src->height;
    for (int r = 0; r < h; r++) {
        const uint8_t *yy = src->plane[0] + r * src->stride[0];
        const uint8_t *u = src->plane[1] + (r / 2) * src->stride[1];
        const uint8_t *v = src->plane[2] + (r / 2) * src->stride[2];
        uint8_t *d = dst + r * dst_stride;
        for (int c = 0; c < w; c++) {
            int C = yy[c] - 16, D = u[c / 2] - 128, E = v[c / 2] - 128;
            d[4 * c]     = clamp8((298 * C + 541 * D + 128) >> 8);            /* B */
            d[4 * c + 1] = clamp8((298 * C - 55 * D - 136 * E + 128) >> 8);   /* G */
            d[4 * c + 2] = clamp8((298 * C + 459 * E + 128) >> 8);            /* R */
            d[4 * c + 3] = 255;
        }
    }
}

static void scale_plane(const uint8_t *s, int sw, int sh, int ss,
                        uint8_t *d, int dw, int dh, int ds) {
    for (int y = 0; y < dh; y++) {
        int fy = (int)(((int64_t)y * (sh - 1) << 8) / (dh > 1 ? dh - 1 : 1));
        int y0 = fy >> 8, y1 = y0 + 1 < sh ? y0 + 1 : y0, wy = fy & 255;
        for (int x = 0; x < dw; x++) {
            int fx = (int)(((int64_t)x * (sw - 1) << 8) / (dw > 1 ? dw - 1 : 1));
            int x0 = fx >> 8, x1 = x0 + 1 < sw ? x0 + 1 : x0, wx = fx & 255;
            int a = s[y0 * ss + x0], b = s[y0 * ss + x1], c = s[y1 * ss + x0], e = s[y1 * ss + x1];
            int top = a * (256 - wx) + b * wx, bot = c * (256 - wx) + e * wx;
            d[y * ds + x] = (uint8_t)((top * (256 - wy) + bot * wy + 32768) >> 16);
        }
    }
}

void oc_i420_scale(const oc_frame *src, oc_frame *dst) {
    dst->pts_us = src->pts_us;
    scale_plane(src->plane[0], src->width, src->height, src->stride[0],
                dst->plane[0], dst->width, dst->height, dst->stride[0]);
    for (int p = 1; p < 3; p++)
        scale_plane(src->plane[p], src->width / 2, src->height / 2, src->stride[p],
                    dst->plane[p], dst->width / 2, dst->height / 2, dst->stride[p]);
}
