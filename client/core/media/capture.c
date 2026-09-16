/* The capture front end and the synthetic source (oc_capture.h). */
#define _POSIX_C_SOURCE 200809L
#include "oc_capture.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

static char g_detail[200];
const char *oc_capture_detail(void) { return g_detail; }
void oc_capture_set_detail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_detail, sizeof g_detail, fmt, ap);
    va_end(ap);
}

struct oc_capture {
    const oc_capture_backend *b;
    void *impl;
};

static const oc_capture_backend *pick(void) {
    const char *t = getenv("OPENCHIME_TEST_CAPTURE");
    if (t && strcmp(t, "synthetic") == 0) return &oc_capture_synthetic;
    if (t && strcmp(t, "denied") == 0) return &oc_capture_denied;
    return oc_capture_platform();
}

int oc_capture_list(oc_capture_device *out, int cap) {
    const oc_capture_backend *b = pick();
    return b ? b->list(out, cap) : 0;
}

oc_capture *oc_capture_open(const char *device_id, int want_w, int want_h, int want_fps, int *err) {
    int dummy;
    if (!err) err = &dummy;
    const oc_capture_backend *b = pick();
    if (!b) { *err = OC_CAP_NODEVICE; return NULL; }
    oc_capture *c = calloc(1, sizeof *c);
    if (!c) { *err = OC_CAP_FAILED; return NULL; }
    *err = OC_CAP_OK;
    c->b = b;
    c->impl = b->open(device_id, want_w, want_h, want_fps, err);
    if (!c->impl) { if (*err == OC_CAP_OK) *err = OC_CAP_FAILED; free(c); return NULL; }
    return c;
}

int  oc_capture_start(oc_capture *c) { return c->b->start(c->impl); }
int  oc_capture_next(oc_capture *c, oc_frame *f, int timeout_ms) { return c->b->next(c->impl, f, timeout_ms); }
void oc_capture_stop(oc_capture *c) { c->b->stop(c->impl); }
void oc_capture_close(oc_capture *c) {
    if (!c) return;
    c->b->close(c->impl);
    free(c);
}

/* ---- synthetic --------------------------------------------------------------------- */

/* Moving colour bars, and along the top a row of 16 blocks spelling the frame
 * number in binary (black 0 / white 1). The blocks are large and flat, so the
 * number survives a lossy encode and tests can check frame order after decode. */

#define SYN_BITS 16

typedef struct {
    oc_frame frame;
    int      fps;
    int      running;
    long     n;
    int64_t  next_us;
} syn;

static int syn_list(oc_capture_device *out, int cap) {
    if (cap < 1) return 0;
    memset(out, 0, sizeof *out);
    strcpy(out->id, "synthetic");
    strcpy(out->name, "Test pattern");
    return 1;
}

static void *syn_open(const char *id, int w, int h, int fps, int *err) {
    (void)id;
    syn *s = calloc(1, sizeof *s);
    if (!s) { *err = OC_CAP_FAILED; return NULL; }
    w = w > 0 ? w & ~1 : 1280; h = h > 0 ? h & ~1 : 720;
    if (w < 64) w = 64;
    if (h < 64) h = 64;
    if (oc_frame_alloc(&s->frame, w, h) != 0) { free(s); *err = OC_CAP_FAILED; return NULL; }
    s->fps = fps > 0 && fps <= 120 ? fps : 30;
    return s;
}

static int syn_start(void *impl) {
    syn *s = impl;
    s->running = 1; s->n = 0;
    s->next_us = oc_media_clock_us();
    return OC_CAP_OK;
}

static void sleep_us(int64_t us) {
    if (us <= 0) return;
#ifdef _WIN32
    Sleep((DWORD)((us + 999) / 1000));
#else
    struct timespec ts = { (time_t)(us / 1000000), (long)(us % 1000000) * 1000 };
    nanosleep(&ts, NULL);
#endif
}

static void syn_draw(syn *s) {
    oc_frame *f = &s->frame;
    static const uint8_t bars[8][3] = {   /* Y, U, V of 75% bars, BT.709 limited */
        {180, 128, 128}, {168, 44, 136}, {145, 147, 44}, {133, 63, 52},
        {63, 193, 204},  {51, 109, 212}, {28, 212, 120}, {16, 128, 128},
    };
    int w = f->width, h = f->height;
    int shift = (int)((s->n * 4) % w);
    int band = h / 8;
    for (int y = 0; y < h; y++) {
        uint8_t *row = f->plane[0] + y * f->stride[0];
        if (y < band) {
            int bw = w / SYN_BITS;
            for (int x = 0; x < w; x++) {
                int bit = x / bw;
                row[x] = bit < SYN_BITS && ((s->n >> (SYN_BITS - 1 - bit)) & 1) ? 235 : 16;
            }
            continue;
        }
        for (int x = 0; x < w; x++) row[x] = bars[(((x + shift) % w) * 8 / w)][0];
    }
    for (int y = 0; y < h / 2; y++) {
        uint8_t *u = f->plane[1] + y * f->stride[1], *v = f->plane[2] + y * f->stride[2];
        for (int x = 0; x < w / 2; x++) {
            if (y * 2 < band) { u[x] = 128; v[x] = 128; continue; }
            int i = ((x * 2 + shift) % w) * 8 / w;
            u[x] = bars[i][1]; v[x] = bars[i][2];
        }
    }
}

static int syn_next(void *impl, oc_frame *f, int timeout_ms) {
    syn *s = impl;
    if (!s->running) return OC_CAP_FAILED;
    int64_t now = oc_media_clock_us(), wait = s->next_us - now;
    if (wait > (int64_t)timeout_ms * 1000) { sleep_us((int64_t)timeout_ms * 1000); return 0; }
    sleep_us(wait);
    syn_draw(s);
    s->frame.pts_us = oc_media_clock_us();
    s->n++;
    s->next_us += 1000000 / s->fps;
    /* A stalled consumer does not get a burst of catch-up frames. */
    if (s->frame.pts_us - s->next_us > 1000000 / s->fps) s->next_us = s->frame.pts_us;
    *f = s->frame;
    return 1;
}

static void syn_stop(void *impl) { ((syn *)impl)->running = 0; }
static void syn_close(void *impl) {
    syn *s = impl;
    oc_frame_free(&s->frame);
    free(s);
}

const oc_capture_backend oc_capture_synthetic = { syn_list, syn_open, syn_start, syn_next, syn_stop, syn_close };

int oc_capture_synthetic_frame_number(const oc_frame *f) {
    int band = f->height / 8, bw = f->width / SYN_BITS, y = band / 2, n = 0;
    if (band < 1 || bw < 1) return -1;
    const uint8_t *row = f->plane[0] + y * f->stride[0];
    for (int bit = 0; bit < SYN_BITS; bit++) {
        const uint8_t v = row[bit * bw + bw / 2];
        if (v > 90 && v < 160) return -1;                  /* neither black nor white */
        n = (n << 1) | (v >= 160);
    }
    return n;
}

/* ---- denied ------------------------------------------------------------------------ */

static void *denied_open(const char *id, int w, int h, int fps, int *err) {
    (void)id; (void)w; (void)h; (void)fps;
    *err = OC_CAP_DENIED;
    return NULL;
}

const oc_capture_backend oc_capture_denied = { syn_list, denied_open, syn_start, syn_next, syn_stop, syn_close };

#ifndef _WIN32
/* Built with each platform's client (docs/VIDEO-MESSAGES.md §3.2). */
const oc_capture_backend *oc_capture_platform(void) { return NULL; }
#endif
