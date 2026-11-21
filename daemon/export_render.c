/* One export of a video message (export_render.h). */
#define _POSIX_C_SOURCE 200809L
#include "export_render.h"

#include "oc_mp4.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void seterr(char *err, size_t cap, const char *fmt, ...) {
    if (!err || !cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

/* ---- the source, front to back, with what was read ahead kept ------------------ */

typedef struct {
    oc_export_read_fn read;
    void    *ctx;
    uint64_t pos;                  /* bytes of the source consumed */
    uint8_t *ahead;                /* read but not consumed */
    size_t   ahead_at, ahead_n;
} src;

/* Exactly `n` bytes into `out` (NULL: skip them). 0, or -1 at an early end or error. */
static int src_take(src *s, uint8_t *out, size_t n) {
    while (n) {
        if (s->ahead_at < s->ahead_n) {
            size_t k = s->ahead_n - s->ahead_at < n ? s->ahead_n - s->ahead_at : n;
            if (out) { memcpy(out, s->ahead + s->ahead_at, k); out += k; }
            s->ahead_at += k; s->pos += k; n -= k;
            continue;
        }
        uint8_t skip[65536];
        size_t want = n < sizeof skip ? n : sizeof skip;
        long got = s->read(s->ctx, out ? out : skip, want);
        if (got <= 0) return -1;
        if (out) out += got;
        s->pos += (uint64_t)got; n -= (size_t)got;
    }
    return 0;
}

static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

/* Read the head -- ftyp, moov and mdat's header -- into `*head`. Whatever was
 * read past it stays in `s` for the samples. 0, or -1 (too long, or not boxes). */
static int read_head(src *s, uint64_t size, uint8_t **head, size_t *head_len) {
    size_t cap = 65536, n = 0, need = 8, boxes = 0;
    uint8_t *b = malloc(cap);
    if (!b) return -1;
    for (;;) {
        while (n < need) {
            if (need > OC_EXPORT_MAX_HEAD || need > size) { free(b); return -1; }
            if (need > cap) {
                size_t nc = cap; while (nc < need) nc *= 2;
                uint8_t *nb = realloc(b, nc);
                if (!nb) { free(b); return -1; }
                b = nb; cap = nc;
            }
            long got = s->read(s->ctx, b + n, cap - n);
            if (got <= 0) { free(b); return -1; }
            n += (size_t)got;
        }
        /* `need` covers the header of the box that starts at need - 8. */
        size_t at = need - 8;
        uint32_t sz = be32(b + at);
        if (boxes == 2) {                                  /* ftyp, moov, and this is mdat's header */
            if (memcmp(b + at + 4, "mdat", 4) != 0) { free(b); return -1; }
            break;
        }
        if (sz < 8) { free(b); return -1; }
        boxes++;
        need = at + sz + 8;
    }
    /* What came after mdat's header is the first of the samples. */
    s->ahead = b; s->ahead_at = need; s->ahead_n = n;
    s->pos = need;
    *head = b; *head_len = need;
    return 0;
}

/* ---- the render ------------------------------------------------------------------ */

typedef struct { uint64_t off; uint32_t size; uint64_t dts; uint8_t video; } item;

static int by_offset(const void *a, const void *b) {
    const item *x = a, *y = b;
    return x->off < y->off ? -1 : x->off > y->off ? 1 : 0;
}

oc_export_status oc_export_render(const oc_export_codecs *c, oc_export_read_fn read, void *rctx,
                                  uint64_t size, const char *out_path, const volatile int *cancel,
                                  uint64_t *bytes, uint32_t *duration_ms, char *err, size_t errcap) {
    *bytes = 0;
    *duration_ms = 0;
    if (!c || !read || !out_path) { seterr(err, errcap, "bad export request"); return OC_EXPORT_FAILED; }
    src s = { read, rctx, 0, NULL, 0, 0 };
    uint8_t *head = NULL;
    size_t head_len = 0;
    if (read_head(&s, size, &head, &head_len) != 0) {
        free(s.ahead);
        seterr(err, errcap, "not a video message's file");
        return OC_EXPORT_NOT_VIDEO;
    }
    oc_mp4_info info;
    uint64_t mdat_payload = 0;
    int parsed = oc_mp4_parse_head(head, head_len, size, &info, &mdat_payload);
    if (parsed != 0 || !info.video.present || mdat_payload != head_len) {
        if (parsed == 0) oc_mp4_info_free(&info);
        free(s.ahead);
        seterr(err, errcap, "not a video message's file");
        return OC_EXPORT_NOT_VIDEO;
    }

    /* Every sample, in the order it lies in the file: the order it can be read. */
    uint32_t nv = info.video.n_samples, na = info.audio.present ? info.audio.n_samples : 0;
    item *items = malloc(((size_t)nv + na) * sizeof *items);
    uint32_t largest = 0;
    oc_export_status st = OC_EXPORT_OK;
    if (!items) { seterr(err, errcap, "out of memory"); st = OC_EXPORT_FAILED; }
    for (uint32_t i = 0; items && i < nv; i++) {
        const oc_mp4_sample *x = &info.video.samples[i];
        items[i] = (item){ x->offset, x->size, x->dts, 1 };
        if (x->size > largest) largest = x->size;
    }
    for (uint32_t i = 0; items && i < na; i++) {
        const oc_mp4_sample *x = &info.audio.samples[i];
        items[nv + i] = (item){ x->offset, x->size, x->dts, 0 };
        if (x->size > largest) largest = x->size;
    }
    if (items) qsort(items, (size_t)nv + na, sizeof *items, by_offset);

    oc_export_params p = { info.width, info.height, 30, na > 0, info.opus_channels, info.opus_preskip };
    if (info.video.duration) {
        uint64_t fps = ((uint64_t)nv * info.video.timescale + info.video.duration / 2) / info.video.duration;
        p.fps = (unsigned)(fps < 1 ? 1 : fps > 60 ? 60 : fps);
    }
    uint8_t *sample = st == OC_EXPORT_OK ? malloc(largest ? largest : 1) : NULL;
    if (st == OC_EXPORT_OK && !sample) { seterr(err, errcap, "out of memory"); st = OC_EXPORT_FAILED; }
    void *enc = st == OC_EXPORT_OK ? c->open(out_path, &p, err, errcap) : NULL;
    if (st == OC_EXPORT_OK && !enc) st = OC_EXPORT_FAILED;

    for (uint64_t i = 0; st == OC_EXPORT_OK && i < (uint64_t)nv + na; i++) {
        if (cancel && *cancel) { st = OC_EXPORT_CANCELLED; seterr(err, errcap, "stopped"); break; }
        const item *it = &items[i];
        if (it->off < s.pos || src_take(&s, NULL, (size_t)(it->off - s.pos)) != 0 ||
            src_take(&s, sample, it->size) != 0) {
            seterr(err, errcap, "reading the source failed");
            st = OC_EXPORT_FAILED;
            break;
        }
        int rc = it->video
            ? c->video(enc, sample, it->size, (int64_t)(it->dts * 1000000u / info.video.timescale), err, errcap)
            : c->audio(enc, sample, it->size, err, errcap);
        if (rc != 0) st = OC_EXPORT_FAILED;
    }
    if (enc) {
        if (st == OC_EXPORT_OK) {
            if (c->finish(enc, bytes, duration_ms, err, errcap) != 0) st = OC_EXPORT_FAILED;
        } else {
            c->abort(enc);
        }
    }
    free(sample);
    free(items);
    free(s.ahead);
    oc_mp4_info_free(&info);
    return st;
}

/* ---- the stub ------------------------------------------------------------------- */

typedef struct { FILE *f; char *path; int64_t last_pts_us; } stub;

static void *stub_open(const char *out_path, const oc_export_params *p, char *err, size_t errcap) {
    stub *s = calloc(1, sizeof *s);
    if (!s || !(s->path = strdup(out_path)) || !(s->f = fopen(out_path, "w"))) {
        if (s) free(s->path);
        free(s);
        seterr(err, errcap, "stub: cannot open");
        return NULL;
    }
    fprintf(s->f, "%dx%d %u fps audio %d\n", p->width, p->height, p->fps, p->has_audio);
    return s;
}
static int stub_video(void *v, const uint8_t *d, size_t len, int64_t pts_us, char *err, size_t errcap) {
    stub *s = v; (void)d; (void)err; (void)errcap;
    s->last_pts_us = pts_us;
    return fprintf(s->f, "v %lld %zu\n", (long long)pts_us, len) > 0 ? 0 : -1;
}
static int stub_audio(void *v, const uint8_t *d, size_t len, char *err, size_t errcap) {
    stub *s = v; (void)d; (void)err; (void)errcap;
    return fprintf(s->f, "a %zu\n", len) > 0 ? 0 : -1;
}
static void stub_abort(void *v) {
    stub *s = v;
    if (s->f) fclose(s->f);
    unlink(s->path);
    free(s->path);
    free(s);
}
static int stub_finish(void *v, uint64_t *bytes, uint32_t *ms, char *err, size_t errcap) {
    stub *s = v; (void)err; (void)errcap;
    long n = ftell(s->f);
    int ok = n >= 0 && fclose(s->f) == 0;
    s->f = NULL;
    if (!ok) { stub_abort(s); return -1; }
    *bytes = (uint64_t)n;
    *ms = (uint32_t)(s->last_pts_us / 1000);
    free(s->path);
    free(s);
    return 0;
}

const oc_export_codecs oc_export_stub_codecs = {
    "stub-1", stub_open, stub_video, stub_audio, stub_finish, stub_abort,
};

/* A daemon built without EXPORT=1 carries no codecs: it refuses every export.
 * (The test binary links the real ones for the media suite, which asks for
 * them by name, and still answers NULL here, as such a daemon does.) */
const oc_export_codecs *oc_export_codecs_builtin(void) {
#ifdef OC_EXPORT
    return oc_export_codecs_real();
#else
    return NULL;
#endif
}
