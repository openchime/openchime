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
    /* A screen: the backend reports changes, and frames are paced out here. */
    int      screen;
    int64_t  period_us, due_us;
    oc_frame last;
    int      have_last;
};

static const oc_capture_backend *pick(void) {
    const char *t = getenv("OPENCHIME_TEST_CAPTURE");
    if (t && (strcmp(t, "synthetic") == 0 || strcmp(t, "synthetic-camera") == 0)) return &oc_capture_synthetic;
    if (t && strcmp(t, "denied") == 0) return &oc_capture_denied;
    return oc_capture_platform();
}

static const oc_capture_backend *pick_screen(void) {
    const char *t = getenv("OPENCHIME_TEST_CAPTURE");
    if (t && strcmp(t, "synthetic") == 0) return &oc_capture_synthetic_screen;
    if (t && strcmp(t, "denied") == 0) return &oc_capture_denied;
    return oc_capture_screen_platform();
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

int oc_capture_list_screens(oc_capture_device *out, int cap) {
    const oc_capture_backend *b = pick_screen();
    return b ? b->list(out, cap) : 0;
}

oc_capture *oc_capture_open_screen(const char *device_id, int max_w, int max_h, int fps, int *err) {
    int dummy;
    if (!err) err = &dummy;
    const oc_capture_backend *b = pick_screen();
    if (!b) { *err = OC_CAP_NODEVICE; return NULL; }
    oc_capture *c = calloc(1, sizeof *c);
    if (!c) { *err = OC_CAP_FAILED; return NULL; }
    *err = OC_CAP_OK;
    c->b = b;
    c->screen = 1;
    c->period_us = 1000000 / (fps > 0 && fps <= 120 ? fps : 30);
    c->impl = b->open(device_id, max_w, max_h, fps, err);
    if (!c->impl) { if (*err == OC_CAP_OK) *err = OC_CAP_FAILED; free(c); return NULL; }
    return c;
}

int oc_capture_start(oc_capture *c) {
    c->due_us = 0;
    return c->b->start(c->impl);
}

static void sleep_us(int64_t us);

/* A screen's frames: whatever changed most recently, handed out once per
 * period. The backend is asked for changes in slices no longer than the time to
 * the next frame, so a change arriving just before it is due is the one shown. */
static int screen_next(oc_capture *c, oc_frame *f, int timeout_ms) {
    int64_t now = oc_media_clock_us(), end = now + (int64_t)timeout_ms * 1000;
    for (;;) {
        int64_t until = c->have_last ? c->due_us - now : (end - now);
        int slice = until > 0 ? (int)((until + 999) / 1000) : 0;
        oc_frame nf;
        int rc = c->b->next(c->impl, &nf, slice);
        if (rc < 0) return rc;
        if (rc == 1) {
            if (!c->have_last || c->last.width != nf.width || c->last.height != nf.height) {
                oc_frame_free(&c->last);
                if (oc_frame_alloc(&c->last, nf.width, nf.height) != 0) return OC_CAP_FAILED;
            }
            oc_frame_copy(&c->last, &nf);
            if (!c->have_last) c->due_us = oc_media_clock_us();   /* the first frame goes out at once */
            c->have_last = 1;
        }
        now = oc_media_clock_us();
        if (c->have_last && now >= c->due_us) {
            c->due_us += c->period_us;
            if (c->due_us < now) c->due_us = now + c->period_us;   /* no burst after a stall */
            c->last.pts_us = now;
            *f = c->last;
            return 1;
        }
        if (now >= end) return 0;
        if (rc == 0 && slice == 0) sleep_us(1000);
    }
}

int  oc_capture_next(oc_capture *c, oc_frame *f, int timeout_ms) {
    return c->screen ? screen_next(c, f, timeout_ms) : c->b->next(c->impl, f, timeout_ms);
}
void oc_capture_stop(oc_capture *c) { c->b->stop(c->impl); }
void oc_capture_close(oc_capture *c) {
    if (!c) return;
    c->b->close(c->impl);
    oc_frame_free(&c->last);
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
    out->kind = OC_SOURCE_CAMERA;
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

/* ---- synthetic screen ------------------------------------------------------------ */

/* A 2560×1600 page: a light background and rows of dark "words", with the change
 * count spelled along the top in the camera pattern's blocks. It changes five
 * times a second -- a screen is mostly still -- and is fitted into the size the
 * capture opened at, so both halves of a screen source are exercised: fitting,
 * and the front end repeating a still frame at the frame rate. */

#define SCR_W 2560
#define SCR_H 1600
#define SCR_CHANGE_US 200000

typedef struct {
    oc_frame page, out;
    int      running;
    long     n;
    int64_t  next_us, gone_at_us;
} scr;

static int scr_list(oc_capture_device *out, int cap) {
    if (cap < 2) return 0;
    memset(out, 0, 2 * sizeof *out);
    strcpy(out[0].id, "screen:synthetic");
    strcpy(out[0].name, "Test screen");
    out[0].kind = OC_SOURCE_SCREEN;
    strcpy(out[1].id, "window:synthetic");
    strcpy(out[1].name, "Test window");
    out[1].kind = OC_SOURCE_WINDOW;
    return 2;
}

static void *scr_open(const char *id, int max_w, int max_h, int fps, int *err) {
    (void)id; (void)fps;
    scr *s = calloc(1, sizeof *s);
    if (!s) { *err = OC_CAP_FAILED; return NULL; }
    if (max_w <= 0) max_w = 1920;
    if (max_h <= 0) max_h = 1080;
    int w = max_w, h = (int)((int64_t)max_w * SCR_H / SCR_W);
    if (h > max_h) { h = max_h; w = (int)((int64_t)max_h * SCR_W / SCR_H); }
    if (oc_frame_alloc(&s->page, SCR_W, SCR_H) != 0 || oc_frame_alloc(&s->out, w & ~1, h & ~1) != 0) {
        oc_frame_free(&s->page); free(s); *err = OC_CAP_FAILED; return NULL;
    }
    return s;
}

static int scr_start(void *impl) {
    scr *s = impl;
    s->running = 1; s->n = 0;
    s->next_us = oc_media_clock_us();
    const char *g = getenv("OPENCHIME_TEST_SCREEN_GONE_MS");
    s->gone_at_us = g && *g ? s->next_us + strtol(g, NULL, 10) * 1000 : 0;
    return OC_CAP_OK;
}

static void scr_draw(scr *s) {
    oc_frame *f = &s->page;
    oc_i420_fill(f, 220, 128, 128);
    int band = SCR_H / 8, bw = SCR_W / SYN_BITS;
    for (int y = 0; y < band; y++) {
        uint8_t *row = f->plane[0] + (size_t)y * f->stride[0];
        for (int x = 0; x < SCR_W; x++) {
            int bit = x / bw;
            row[x] = bit < SYN_BITS && ((s->n >> (SYN_BITS - 1 - bit)) & 1) ? 235 : 16;
        }
    }
    /* Rows of words; the page scrolls by one row per change. */
    for (int line = 0; line < 40; line++) {
        int y0 = band + 40 + line * 32 - (int)(s->n % 32);
        if (y0 < band + 8 || y0 + 16 > SCR_H) continue;
        for (int x0 = 80; x0 < SCR_W - 200; ) {
            int len = 40 + (int)(((unsigned)(line * 131 + x0 * 7) % 9) * 20);
            for (int y = y0; y < y0 + 16; y++)
                memset(f->plane[0] + (size_t)y * f->stride[0] + x0, 40, (size_t)len);
            x0 += len + 24;
        }
    }
}

static int scr_next(void *impl, oc_frame *f, int timeout_ms) {
    scr *s = impl;
    if (!s->running) return OC_CAP_FAILED;
    int64_t now = oc_media_clock_us();
    if (s->gone_at_us && now >= s->gone_at_us) { oc_capture_set_detail("the test screen went away"); return OC_CAP_GONE; }
    int64_t wait = s->next_us - now;
    if (wait > (int64_t)timeout_ms * 1000) { sleep_us((int64_t)timeout_ms * 1000); return 0; }
    sleep_us(wait);
    scr_draw(s);
    oc_i420_fit(&s->page, &s->out);
    s->out.pts_us = oc_media_clock_us();
    s->n++;
    s->next_us += SCR_CHANGE_US;
    if (s->out.pts_us - s->next_us > SCR_CHANGE_US) s->next_us = s->out.pts_us;
    *f = s->out;
    return 1;
}

static void scr_stop(void *impl) { ((scr *)impl)->running = 0; }
static void scr_close(void *impl) {
    scr *s = impl;
    oc_frame_free(&s->page);
    oc_frame_free(&s->out);
    free(s);
}

const oc_capture_backend oc_capture_synthetic_screen = { scr_list, scr_open, scr_start, scr_next, scr_stop, scr_close };

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
const oc_capture_backend *oc_capture_screen_platform(void) { return NULL; }
#endif
