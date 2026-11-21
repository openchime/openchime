/* summary.bin: mapping the summary model's data and checking it (sumkit.h has
 * the format). The file is used where it lies; opening it reads the header and
 * the index, checks every entry against the file, and finds every tensor the
 * model reads with the shape and a type it runs, so a file that opens can be
 * run. */
#define _POSIX_C_SOURCE 200809L
#include "sk_priv.h"

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

void sk_seterr(char *err, size_t cap, const char *fmt, ...) {
    if (!err || !cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }
static float rdf(const uint8_t *p) {
    union { uint32_t u; float f; } v = { .u = rd32(p) };
    return v.f;
}
static uint64_t round64(uint64_t n) { return (n + (SK_ALIGN - 1)) & ~(uint64_t)(SK_ALIGN - 1); }

/* Bytes of a tensor of `count` elements laid out as `dims` (rows of the last
 * dimension), or 0 when the type cannot hold that shape. */
uint64_t sk_tensor_bytes(uint32_t dtype, uint32_t ndim, const uint32_t *dims) {
    uint64_t rows = 1;
    for (uint32_t i = 0; i + 1 < ndim; i++) rows *= dims[i];
    uint32_t cols = dims[ndim - 1];
    switch (dtype) {
    case SK_F32: case SK_I32: return rows * cols * 4;
    case SK_BF16: return rows * cols * 2;
    case SK_U8: return rows * cols;
    case SK_Q4K: return cols % 256 ? 0 : rows * (cols / 256) * 144;
    case SK_Q6K: return cols % 256 ? 0 : rows * (cols / 256) * 210;
    }
    return 0;
}

static const char *bad_entry(const uint8_t *e, const uint8_t *prev, size_t n, uint64_t *prev_end) {
    const char *name = (const char *)e;
    if (!memchr(e, '\0', SK_NAME_MAX) || !name[0]) return "a name that is empty or unterminated";
    if (prev && strcmp((const char *)prev, name) >= 0) return "a name out of order or repeated";
    uint32_t dtype = rd32(e + 48), ndim = rd32(e + 52), dims[SK_MAX_DIMS];
    if (dtype > SK_I32) return "an unknown element type";
    if (ndim < 1 || ndim > SK_MAX_DIMS) return "a rank outside 1..4";
    for (uint32_t i = 0; i < SK_MAX_DIMS; i++) {
        dims[i] = rd32(e + 56 + 4 * i);
        if (i >= ndim ? dims[i] != 0 : dims[i] == 0 || dims[i] > (1u << 30)) return "an empty or impossible dimension";
    }
    uint64_t off = rd64(e + 72), bytes = rd64(e + 80), want = sk_tensor_bytes(dtype, ndim, dims);
    if (!want || bytes != want) return "a size that disagrees with its shape";
    if (off % SK_ALIGN) return "data that is not 64-byte aligned";
    if (off < *prev_end) return "data that overlaps the index or another tensor";
    if (off > n || bytes > n - off) return "data past the end of the file";
    *prev_end = off + bytes;
    return NULL;
}

/* The tensors the model reads: name, element types allowed (a bit each), rows
 * (0 for a vector) and columns. */
typedef struct { char name[SK_NAME_MAX]; unsigned types; uint32_t rows, cols; } need;
#define MATS ((1u << SK_F32) | (1u << SK_BF16) | (1u << SK_Q4K) | (1u << SK_Q6K))
#define VEC  (1u << SK_F32)

static int add_need(need *v, int n, const char *name, unsigned types, uint32_t rows, uint32_t cols) {
    snprintf(v[n].name, sizeof v[n].name, "%s", name);
    v[n].types = types;
    v[n].rows = rows;
    v[n].cols = cols;
    return n + 1;
}

static int needs(const sk_config *c, need *v) {
    int n = 0;
    char nm[SK_NAME_MAX];
    uint32_t kd = c->lin_k_heads * c->lin_k_dim, vd = c->lin_v_heads * c->lin_v_dim, hd = c->head_dim;
    n = add_need(v, n, "embed", MATS, c->n_vocab, c->n_embd);
    n = add_need(v, n, "norm", VEC, 0, c->n_embd);
    for (uint32_t l = 0; l < c->n_layer; l++) {
#define N(suffix) (snprintf(nm, sizeof nm, "L%02u.%s", l, suffix), nm)
        n = add_need(v, n, N("in_norm"), VEC, 0, c->n_embd);
        n = add_need(v, n, N("post_norm"), VEC, 0, c->n_embd);
        n = add_need(v, n, N("ffn_gate"), MATS, c->n_ff, c->n_embd);
        n = add_need(v, n, N("ffn_up"), MATS, c->n_ff, c->n_embd);
        n = add_need(v, n, N("ffn_down"), MATS, c->n_embd, c->n_ff);
        if (c->linear >> l & 1) {
            n = add_need(v, n, N("qkv"), MATS, 2 * kd + vd, c->n_embd);
            n = add_need(v, n, N("z"), MATS, vd, c->n_embd);
            n = add_need(v, n, N("a"), VEC, c->lin_v_heads, c->n_embd);
            n = add_need(v, n, N("b"), VEC, c->lin_v_heads, c->n_embd);
            n = add_need(v, n, N("conv"), VEC, c->conv_kernel, 2 * kd + vd);
            n = add_need(v, n, N("dt"), VEC, 0, c->lin_v_heads);
            n = add_need(v, n, N("A"), VEC, 0, c->lin_v_heads);
            n = add_need(v, n, N("gnorm"), VEC, 0, c->lin_v_dim);
            n = add_need(v, n, N("out"), MATS, c->n_embd, vd);
        } else {
            n = add_need(v, n, N("q"), MATS, 2 * c->n_head * hd, c->n_embd);
            n = add_need(v, n, N("k"), MATS, c->n_head_kv * hd, c->n_embd);
            n = add_need(v, n, N("v"), MATS, c->n_head_kv * hd, c->n_embd);
            n = add_need(v, n, N("o"), MATS, c->n_embd, c->n_head * hd);
            n = add_need(v, n, N("q_norm"), VEC, 0, hd);
            n = add_need(v, n, N("k_norm"), VEC, 0, hd);
        }
#undef N
    }
    n = add_need(v, n, "tok.offs", 1u << SK_I32, 0, c->n_tok + 1);
    n = add_need(v, n, "tok.merges", 1u << SK_I32, c->n_merges, 3);
    return n;
}

static int check_config(const sk_config *c, char *err, size_t cap) {
    if (!c->n_layer || c->n_layer > SK_MAX_LAYERS || !c->n_embd || !c->n_ff || !c->n_vocab || !c->n_tok ||
        c->n_tok > c->n_vocab || c->n_eog < 1 || c->n_eog > SK_MAX_EOG) {
        sk_seterr(err, cap, "the model's dimensions are not ones it can have");
        return -1;
    }
    if (c->n_layer < 64 && c->linear >> c->n_layer) { sk_seterr(err, cap, "layers past the last marked linear"); return -1; }
    int any_attn = 0, any_lin = 0;
    for (uint32_t l = 0; l < c->n_layer; l++) {
        if (c->linear >> l & 1) any_lin = 1;
        else any_attn = 1;
    }
    if (any_attn && (!c->n_head || !c->n_head_kv || c->n_head % c->n_head_kv || !c->head_dim ||
                     c->head_dim % 32 || !c->rot_dims || c->rot_dims % 2 || c->rot_dims > c->head_dim)) {
        sk_seterr(err, cap, "the attention layers' dimensions are not ones they can have");
        return -1;
    }
    if (any_lin && (!c->lin_k_heads || !c->lin_v_heads || c->lin_v_heads % c->lin_k_heads || !c->lin_k_dim ||
                    !c->lin_v_dim || !c->conv_kernel || c->conv_kernel > 16)) {
        sk_seterr(err, cap, "the linear-attention layers' dimensions are not ones they can have");
        return -1;
    }
    for (uint32_t i = 0; i < c->n_eog; i++)
        if (c->eog[i] >= c->n_tok) { sk_seterr(err, cap, "an end token past the vocabulary"); return -1; }
    if (!(c->rms_eps > 0) || !(c->rope_theta > 0)) { sk_seterr(err, cap, "a norm epsilon or rotary base that is not positive"); return -1; }
    return 0;
}

static int load(sk_file *f, const char *what, int tok_only, char *err, size_t cap) {
    const uint8_t *p = f->p;
    size_t n = f->n;
    if (n < SK_HEADER || memcmp(p, SK_MAGIC, 8) != 0 || rd32(p + 8) != SK_VERSION) {
        sk_seterr(err, cap, "%s is not a version %u summary model file", what, SK_VERSION);
        return -1;
    }
    if ((uintptr_t)p % SK_ALIGN) { sk_seterr(err, cap, "%s is not 64-byte aligned in memory", what); return -1; }
    uint32_t count = rd32(p + 12);
    uint64_t data = rd64(p + 24), size = rd64(p + 32);
    if (rd32(p + 16) != SK_HEADER || rd32(p + 20) != SK_ENTRY || count == 0 ||
        (n - SK_HEADER) / SK_ENTRY < count || data != round64(SK_HEADER + (uint64_t)count * SK_ENTRY) ||
        size != n) {
        sk_seterr(err, cap, "%s has a header that disagrees with its length", what);
        return -1;
    }
    sk_config *c = &f->cfg;
    f->full = rd32(p + 136);
    uint32_t *dims[13] = { &c->n_layer, &c->n_embd, &c->n_ff, &c->n_vocab, &c->n_head, &c->n_head_kv,
                           &c->head_dim, &c->rot_dims, &c->lin_k_heads, &c->lin_v_heads, &c->lin_k_dim,
                           &c->lin_v_dim, &c->conv_kernel };
    for (int i = 0; i < 13; i++) *dims[i] = rd32(p + 140 + 4 * i);
    c->linear = rd64(p + 192);
    c->rms_eps = rdf(p + 200);
    c->rope_theta = rdf(p + 204);
    c->n_eog = rd32(p + 208);
    for (int i = 0; i < SK_MAX_EOG; i++) c->eog[i] = rd32(p + 212 + 4 * i);
    c->n_tok = rd32(p + 228);
    c->n_merges = rd32(p + 232);
    if (f->full > 1 || check_config(c, err, cap) != 0) {
        if (f->full > 1) sk_seterr(err, cap, "%s has an unknown precision", what);
        return -1;
    }
    f->t = calloc(count, sizeof *f->t);
    if (!f->t) { sk_seterr(err, cap, "out of memory"); return -1; }
    f->count = count;
    uint64_t end = data;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = p + SK_HEADER + (size_t)i * SK_ENTRY;
        const char *why = bad_entry(e, i ? e - SK_ENTRY : NULL, n, &end);
        if (why) { sk_seterr(err, cap, "%s: entry %u has %s", what, i, why); return -1; }
        sk_tensor *t = &f->t[i];
        t->name = (const char *)e;
        t->dtype = rd32(e + 48);
        t->ndim = rd32(e + 52);
        for (int d = 0; d < SK_MAX_DIMS; d++) t->dims[d] = rd32(e + 56 + 4 * d);
        t->data = p + rd64(e + 72);
        t->bytes = rd64(e + 80);
    }
    if (end != n) { sk_seterr(err, cap, "%s does not end at its last tensor", what); return -1; }
    need *nv = malloc((size_t)(4 + 16 * c->n_layer) * sizeof *nv);
    if (!nv) { sk_seterr(err, cap, "out of memory"); return -1; }
    int nn = needs(c, nv), rc = 0;
    for (int i = tok_only ? nn - 2 : 0; i < nn && rc == 0; i++) {
        const sk_tensor *t = sk_find(f, nv[i].name);
        int shape = t && (nv[i].rows ? t->ndim == 2 && t->dims[0] == nv[i].rows && t->dims[1] == nv[i].cols
                                     : t->ndim == 1 && t->dims[0] == nv[i].cols);
        if (!t || !(nv[i].types >> t->dtype & 1) || !shape) {
            sk_seterr(err, cap, "%s: %s is %s", what, nv[i].name, !t ? "missing" : !shape ? "not the shape the model needs" : "of a type the model cannot run");
            rc = -1;
        }
    }
    free(nv);
    if (rc) return -1;
    const sk_tensor *tb = sk_find(f, "tok.bytes"), *to = sk_find(f, "tok.offs"), *ta = sk_find(f, "tok.added");
    if (!tb || tb->dtype != SK_U8 || tb->ndim != 1 || (ta && (ta->dtype != SK_I32 || ta->ndim != 1))) {
        sk_seterr(err, cap, "%s: the tokenizer's tables are missing or of the wrong type", what);
        return -1;
    }
    /* The token offsets rise and end at the bytes' end; the merges and the
     * added tokens name tokens that exist. */
    const int32_t *off = to->data;
    for (uint32_t i = 0; i < c->n_tok; i++)
        if (off[i] < 0 || off[i] > off[i + 1]) { sk_seterr(err, cap, "%s: the token offsets do not rise", what); return -1; }
    if (off[0] != 0 || (uint64_t)off[c->n_tok] != tb->dims[0]) { sk_seterr(err, cap, "%s: the token offsets do not span the bytes", what); return -1; }
    const int32_t *m = sk_find(f, "tok.merges")->data;
    for (uint64_t i = 0; i < 3ull * c->n_merges; i++)
        if (m[i] < 0 || (uint32_t)m[i] >= c->n_tok) { sk_seterr(err, cap, "%s: a merge names a token that does not exist", what); return -1; }
    if (ta)
        for (uint32_t i = 0; i < ta->dims[0]; i++) {
            int32_t id = ((const int32_t *)ta->data)[i];
            if (id < 0 || (uint32_t)id >= c->n_tok || off[id] == off[id + 1]) {
                sk_seterr(err, cap, "%s: an added token that does not exist", what);
                return -1;
            }
        }
    return 0;
}

sk_file *sk_open_mem(const void *p, size_t n, char *err, size_t errcap) {
    sk_file *f = calloc(1, sizeof *f);
    if (!f) { sk_seterr(err, errcap, "out of memory"); return NULL; }
    f->p = p;
    f->n = n;
    if (load(f, "the summary model", 0, err, errcap) != 0) { sk_close(f); return NULL; }
    return f;
}

static sk_file *open_path(const char *path, int tok_only, char *err, size_t errcap) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { sk_seterr(err, errcap, "cannot open %s", path); return NULL; }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < (off_t)SK_HEADER) {
        close(fd);
        sk_seterr(err, errcap, "%s is not a summary model file", path);
        return NULL;
    }
    void *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (p == MAP_FAILED) { sk_seterr(err, errcap, "cannot map %s", path); return NULL; }
    sk_file *f = calloc(1, sizeof *f);
    if (!f) { munmap(p, (size_t)st.st_size); sk_seterr(err, errcap, "out of memory"); return NULL; }
    f->p = p;
    f->n = (size_t)st.st_size;
    f->mapped = 1;
    if (load(f, path, tok_only, err, errcap) != 0) { sk_close(f); return NULL; }
    return f;
}

sk_file *sk_open(const char *path, char *err, size_t errcap) { return open_path(path, 0, err, errcap); }
sk_file *sk_open_tokenizer(const char *path, char *err, size_t errcap) { return open_path(path, 1, err, errcap); }

void sk_close(sk_file *f) {
    if (!f) return;
    if (f->mapped) munmap((void *)f->p, f->n);
    free(f->t);
    free(f);
}

const sk_config *sk_config_of(const sk_file *f) { return &f->cfg; }
int sk_full(const sk_file *f) { return (int)f->full; }
const uint8_t *sk_input_sha(const sk_file *f, int i) { return f->p + 40 + 32 * i; }
size_t sk_count(const sk_file *f) { return f->count; }
const sk_tensor *sk_at(const sk_file *f, size_t i) { return i < f->count ? &f->t[i] : NULL; }

const sk_tensor *sk_find(const sk_file *f, const char *name) {
    size_t lo = 0, hi = f->count;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = strcmp(f->t[mid].name, name);
        if (c == 0) return &f->t[mid];
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}
