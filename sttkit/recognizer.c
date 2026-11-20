/* recognizer.bin: mapping the recognizer's data and binding the forward pass to
 * its tensors (sttkit.h has the format). The file is used where it lies;
 * opening it reads the header and the index, checks every entry against the
 * file, and finds every tensor the forward pass reads with the shape it reads,
 * so a file that opens can be run. */
#define _POSIX_C_SOURCE 200809L
#include "stt_priv.h"

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static void seterr(char *err, size_t cap, const char *fmt, ...) {
    if (!err || !cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }
static size_t dtype_size(uint32_t t) { return t == REC_F32 || t == REC_I32 ? 4 : t == REC_U8 ? 1 : 0; }
static uint64_t round64(uint64_t n) { return (n + (REC_ALIGN - 1)) & ~(uint64_t)(REC_ALIGN - 1); }

/* Entry i against the file: why it is wrong, or NULL. */
static const char *bad_entry(const uint8_t *e, const uint8_t *prev, size_t n, uint64_t *prev_end) {
    const char *name = (const char *)e;
    if (!memchr(e, '\0', REC_NAME_MAX) || !name[0]) return "a name that is empty or unterminated";
    if (prev && strcmp((const char *)prev, name) >= 0) return "a name out of order or repeated";
    uint32_t dtype = rd32(e + 48), ndim = rd32(e + 52);
    size_t es = dtype_size(dtype);
    if (!es) return "an unknown element type";
    if (ndim < 1 || ndim > REC_MAX_DIMS) return "a rank outside 1..4";
    uint64_t count = 1;
    for (uint32_t i = 0; i < REC_MAX_DIMS; i++) {
        uint32_t d = rd32(e + 56 + 4 * i);
        if (i >= ndim) {
            if (d) return "a dimension past its rank";
            continue;
        }
        if (!d || count > UINT64_MAX / 4 / d) return "an empty or impossible dimension";
        count *= d;
    }
    uint64_t off = rd64(e + 72), bytes = rd64(e + 80);
    if (bytes != count * es) return "a size that disagrees with its shape";
    if (off % REC_ALIGN) return "data that is not 64-byte aligned";
    if (off < *prev_end) return "data that overlaps the index or another tensor";
    if (off > n || bytes > n - off) return "data past the end of the file";
    *prev_end = off + bytes;
    return NULL;
}

static int check_file(recognizer *r, const char *what, char *err, size_t cap) {
    const uint8_t *p = r->p;
    size_t n = r->n;
    if (n < REC_HEADER || memcmp(p, REC_MAGIC, 8) != 0 || rd32(p + 8) != REC_VERSION) {
        seterr(err, cap, "%s is not a version %u recognizer file", what, REC_VERSION);
        return -1;
    }
    if ((uintptr_t)p % REC_ALIGN) { seterr(err, cap, "%s is not 64-byte aligned in memory", what); return -1; }
    uint32_t count = rd32(p + 12);
    uint64_t data = rd64(p + 24), size = rd64(p + 32);
    if (rd32(p + 16) != REC_HEADER || rd32(p + 20) != REC_ENTRY || count == 0 ||
        (n - REC_HEADER) / REC_ENTRY < count || data != round64(REC_HEADER + (uint64_t)count * REC_ENTRY) ||
        size != n || data > n) {
        seterr(err, cap, "%s is truncated or corrupt", what);
        return -1;
    }
    uint64_t end = data;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = p + REC_HEADER + (size_t)i * REC_ENTRY;
        const char *why = bad_entry(e, i ? e - REC_ENTRY : NULL, n, &end);
        if (why) { seterr(err, cap, "%s is corrupt: tensor %u has %s", what, i, why); return -1; }
    }
    if (end != n) { seterr(err, cap, "%s is truncated or corrupt", what); return -1; }
    r->count = count;
    r->index = p + REC_HEADER;
    return 0;
}

size_t rec_tensor_count(const recognizer *r) { return r ? r->count : 0; }

int rec_tensor_at(const recognizer *r, size_t i, rec_tensor *t) {
    if (!r || i >= r->count) return 0;
    const uint8_t *e = r->index + i * REC_ENTRY;
    memset(t, 0, sizeof *t);
    t->name = (const char *)e;
    t->dtype = (int)rd32(e + 48);
    t->ndim = (int)rd32(e + 52);
    for (int d = 0; d < REC_MAX_DIMS; d++) t->dims[d] = (int)rd32(e + 56 + 4 * d);
    t->data = r->p + rd64(e + 72);
    t->bytes = (size_t)rd64(e + 80);
    uint32_t s = rd32(e + 88);
    memcpy(&t->scale, &s, sizeof s);
    t->zero_point = (int32_t)rd32(e + 92);
    return 1;
}

int rec_tensor_get(const recognizer *r, const char *name, rec_tensor *t) {
    if (!r || !name) return 0;
    size_t lo = 0, hi = r->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = strcmp(name, (const char *)(r->index + mid * REC_ENTRY));
        if (c == 0) return rec_tensor_at(r, mid, t);
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return 0;
}

/* ---- binding ------------------------------------------------------------------ */

typedef struct { const recognizer *r; char *err; size_t cap; int bad; } binder;

/* The tensor `name` of type `dtype` and shape a (x b): its data, or NULL with
 * the reason (the first one only). */
static const void *need(binder *b, int dtype, int a, int bb, const char *fmt, ...) {
    char name[REC_NAME_MAX + 16];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(name, sizeof name, fmt, ap);
    va_end(ap);
    rec_tensor t;
    if (!rec_tensor_get(b->r, name, &t)) {
        if (!b->bad) seterr(b->err, b->cap, "the recognizer data has no %s", name);
        b->bad = 1;
        return NULL;
    }
    int nd = bb ? 2 : 1;
    if (t.dtype != dtype || t.ndim != nd || t.dims[0] != a || (bb && t.dims[1] != bb)) {
        if (!b->bad) seterr(b->err, b->cap, "the recognizer data's %s is not the shape the model reads", name);
        b->bad = 1;
        return NULL;
    }
    return t.data;
}

static void qmat(binder *b, rec_qmat *m, int n, int k, int bias, const char *base) {
    m->n = n;
    m->k = k;
    m->w = need(b, REC_U8, n, k, "%s.w", base);
    m->s = need(b, REC_F32, n, 0, "%s.s", base);
    m->zp = need(b, REC_U8, n, 0, "%s.zp", base);
    m->b = bias ? need(b, REC_F32, n, 0, "%s.b", base) : NULL;
}

static float scalar(binder *b, const char *name) {
    const float *p = need(b, REC_F32, 1, 0, "%s", name);
    return p ? *p : 0;
}

static int bind(recognizer *r, char *err, size_t cap) {
    binder b = { r, err, cap, 0 };
    const int32_t *cfg = need(&b, REC_I32, REC_CFG_COUNT, 0, "cfg");
    if (!cfg) return -1;
    memcpy(r->cfg, cfg, sizeof r->cfg);
    static const int32_t want[REC_CFG_WINDOW] = { 320, REC_LAYERS, 8, 40, 32768, -1, -1, 80, 640, 5, 2, 1280, 1280, 32, 4096 };
    for (int i = 0; i < REC_CFG_WINDOW; i++)
        if (want[i] >= 0 && r->cfg[i] != want[i]) { seterr(err, cap, "the recognizer data is for another model's dimensions"); return -1; }
    if (r->cfg[REC_CFG_BOS] < 0 || r->cfg[REC_CFG_BOS] >= 32768 || r->cfg[REC_CFG_EOS] < 0 || r->cfg[REC_CFG_EOS] >= 32768) {
        seterr(err, cap, "the recognizer data's start or end token is out of range");
        return -1;
    }
    r->fe_lin = need(&b, REC_F32, 80, 320, "fe.lin.w");
    r->fe_c1w = need(&b, REC_F32, 5 * 320, 640, "fe.c1.w");
    r->fe_c1b = need(&b, REC_F32, 640, 0, "fe.c1.b");
    r->fe_c2w = need(&b, REC_F32, 5 * 640, 320, "fe.c2.w");
    r->fe_c2b = need(&b, REC_F32, 320, 0, "fe.c2.b");
    r->fe_comp = scalar(&b, "fe.comp");
    r->fe_eps = scalar(&b, "fe.eps");
    r->eps = scalar(&b, "eps");
    r->alpha = scalar(&b, "alpha");
    char base[32];
    for (int l = 0; l < REC_LAYERS; l++) {
        rec_enc_layer *e = &r->enc[l];
        e->g1 = need(&b, REC_F32, 320, 0, "enc.%d.g1", l);
        e->g2 = need(&b, REC_F32, 320, 0, "enc.%d.g2", l);
        static const char *const ep[6] = { "q", "k", "v", "o", "f1", "f2" };
        rec_qmat *em[6] = { &e->q, &e->k, &e->v, &e->o, &e->f1, &e->f2 };
        for (int j = 0; j < 6; j++) {
            snprintf(base, sizeof base, "enc.%d.%s", l, ep[j]);
            qmat(&b, em[j], j == 4 ? 1280 : 320, j == 5 ? 1280 : 320, j >= 4, base);
        }
        e->back = r->cfg[REC_CFG_WINDOW + 2 * l];
        e->ahead = r->cfg[REC_CFG_WINDOW + 2 * l + 1];
        if (e->back < 0 || e->back > 4096 || e->ahead < 0 || e->ahead > 4096) { seterr(err, cap, "the recognizer data's attention window is out of range"); return -1; }
        rec_dec_layer *d = &r->dec[l];
        d->g1 = need(&b, REC_F32, 320, 0, "dec.%d.g1", l);
        d->g2 = need(&b, REC_F32, 320, 0, "dec.%d.g2", l);
        d->g3 = need(&b, REC_F32, 320, 0, "dec.%d.g3", l);
        static const char *const dp[10] = { "q", "k", "v", "o", "cq", "co", "xk", "xv", "f1", "f2" };
        rec_qmat *dm[10] = { &d->q, &d->k, &d->v, &d->o, &d->cq, &d->co, &d->xk, &d->xv, &d->f1, &d->f2 };
        for (int j = 0; j < 10; j++) {
            snprintf(base, sizeof base, "dec.%d.%s", l, dp[j]);
            qmat(&b, dm[j], j == 8 ? 2560 : 320, j == 9 ? 1280 : 320, j >= 8, base);
        }
    }
    r->enc_g = need(&b, REC_F32, 320, 0, "enc.g");
    r->dec_g = need(&b, REC_F32, 320, 0, "dec.g");
    r->rope = need(&b, REC_F32, 16, 0, "dec.rope");
    rec_tensor t;
    r->pos = need(&b, REC_U8, 4096, 320, "pos");
    if (r->pos && rec_tensor_get(r, "pos", &t)) { r->pos_scale = t.scale; r->pos_zp = t.zero_point; }
    r->emb = need(&b, REC_U8, 32768, 320, "emb");
    if (r->emb && rec_tensor_get(r, "emb", &t)) { r->emb_scale = t.scale; r->emb_zp = t.zero_point; }
    if (b.bad) return -1;
    if (!rec_tensor_get(r, "tokens", &t) || t.dtype != REC_U8 || stt_tokens_load(&r->tok, t.data, t.bytes) != 0 ||
        r->tok.count != 32768) {
        stt_tokens_free(&r->tok);
        seterr(err, cap, "the recognizer data's tokenizer is missing or malformed");
        return -1;
    }
    if (r->pos_zp < 0 || r->pos_zp > 255 || r->emb_zp < 0 || r->emb_zp > 255) {
        seterr(err, cap, "the recognizer data's zero points are out of range");
        return -1;
    }
    return 0;
}

static recognizer *open_bytes(recognizer *r, const char *what, char *err, size_t cap) {
    if (check_file(r, what, err, cap) != 0 || bind(r, err, cap) != 0) {
        recognizer_close(r);
        return NULL;
    }
    return r;
}

recognizer *recognizer_open(const char *path, char *err, size_t errcap) {
    int fd = open(path, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0 || st.st_size <= 0) {
        if (fd >= 0) close(fd);
        seterr(err, errcap, "cannot read %s", path);
        return NULL;
    }
    void *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (p == MAP_FAILED) { seterr(err, errcap, "cannot map %s", path); return NULL; }
    recognizer *r = calloc(1, sizeof *r);
    if (!r) { munmap(p, (size_t)st.st_size); seterr(err, errcap, "out of memory"); return NULL; }
    r->p = p;
    r->n = (size_t)st.st_size;
    r->owned = 1;
    return open_bytes(r, path, err, errcap);
}

recognizer *recognizer_open_mem(const void *bytes, size_t len, char *err, size_t errcap) {
    if (!bytes) { seterr(err, errcap, "no recognizer data"); return NULL; }
    recognizer *r = calloc(1, sizeof *r);
    if (!r) { seterr(err, errcap, "out of memory"); return NULL; }
    r->p = bytes;
    r->n = len;
    return open_bytes(r, "the recognizer data", err, errcap);
}

void recognizer_close(recognizer *r) {
    if (!r) return;
    stt_tokens_free(&r->tok);
    if (r->owned) munmap((void *)r->p, r->n);
    free(r);
}
