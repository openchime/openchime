/*
 * OpenChime — recognizer.bin from the published recognizer (recognizer_pack.h).
 *
 * An .ort file is a FlatBuffers buffer whose root is ONNX Runtime's
 * InferenceSession table, file identifier "ORTM". Only the part of its schema
 * the converter reads is spelled out below, by field number (a field's number
 * is its slot in the table's vtable):
 *
 *   InferenceSession  0 ort_version: string, 1 model: Model
 *   Model             0 ir_version: int64, 1 opset_import, 2 producer_name: string,
 *                     3 producer_version: string, ..., 7 graph: Graph
 *   Graph             0 initializers: [Tensor], 1 node_args, 2 nodes: [Node],
 *                     3 max_node_index, 4 node_edges, 5 inputs: [string],
 *                     6 outputs: [string]
 *   Node              0 name, 1 doc_string, 2 domain: string, 3 since_version,
 *                     4 index: uint32, 5 op_type: string, 6 type, 7 execution
 *                     provider, 8 inputs: [string], 9 outputs: [string],
 *                     10 attributes: [Attribute]
 *   Attribute         0 name: string, 1 doc_string, 2 type: int32, 3 f: float,
 *                     4 i: int64, 5 s: string, 6 t: Tensor, 7 g: Graph,
 *                     8 floats: [float], 9 ints: [int64], 10 strings: [string]
 *   Tensor            0 name: string, 1 doc_string, 2 dims: [int64],
 *                     3 data_type: int32 (ONNX's codes), 4 raw_data: [uint8]
 *
 * Every offset is checked against the buffer before it is followed: a damaged
 * or hostile file is refused, never read past.
 */
#define _POSIX_C_SOURCE 200809L
#include "recognizer_pack.h"
#include "sttkit.h"
#include "tts_data.h"

#include <mbedtls/sha256.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <unistd.h>

const char *const OC_RECOGNIZER_INPUTS[RP_FILES] = {
    "frontend.model.ort", "frontend.weights.ort", "encoder.ort", "adapter.ort",
    "cross_kv.ort",       "decoder_kv.ort",       "tokenizer.bin", "streaming_config.json",
};

static void seterr(char *err, size_t cap, const char *fmt, ...) {
    if (!err || !cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

/* ---- FlatBuffers ---------------------------------------------------------- */

typedef struct { const uint8_t *p; size_t n; } span;

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint64_t le64(const uint8_t *p) { return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32; }

typedef struct {
    const uint8_t *b;
    size_t n;
    size_t pos;          /* the table */
    size_t vt;           /* its vtable */
    uint16_t vlen, tlen; /* the vtable's and the table's sizes */
} ftab;

/* The table at `pos`: 0, or -1 when its vtable or extent is outside the buffer. */
static int fb_table(const uint8_t *b, size_t n, size_t pos, ftab *t) {
    if (n < 4 || pos > n - 4) return -1;
    int64_t vt = (int64_t)pos - (int64_t)(int32_t)le32(b + pos);
    if (vt < 0 || (uint64_t)vt > n - 4) return -1;
    uint16_t vlen = le16(b + vt), tlen = le16(b + vt + 2);
    if (vlen < 4 || vlen % 2 || (uint64_t)vt + vlen > n || tlen < 4 || pos + tlen > n) return -1;
    t->b = b;
    t->n = n;
    t->pos = pos;
    t->vt = (size_t)vt;
    t->vlen = vlen;
    t->tlen = tlen;
    return 0;
}

/* Where field `i` is in the table, `size` bytes of it, or 0 if it is absent; -1
 * when it does not fit in the table. */
static int64_t fb_field(const ftab *t, int i, size_t size) {
    size_t o = 4 + 2 * (size_t)i;
    if (o + 2 > t->vlen) return 0;
    uint16_t off = le16(t->b + t->vt + o);
    if (!off) return 0;
    if ((size_t)off + size > t->tlen) return -1;
    return (int64_t)(t->pos + off);
}

/* A scalar field, `dflt` when absent; -1 from *bad when it does not fit. */
static uint64_t fb_scalar(const ftab *t, int i, size_t size, uint64_t dflt, int *bad) {
    int64_t at = fb_field(t, i, size);
    if (at < 0) { *bad = 1; return dflt; }
    if (!at) return dflt;
    const uint8_t *p = t->b + at;
    return size == 1 ? p[0] : size == 2 ? le16(p) : size == 4 ? le32(p) : le64(p);
}

/* The position an offset field points at: 0 absent, -1 damaged. */
static int64_t fb_ref(const ftab *t, int i) {
    int64_t at = fb_field(t, i, 4);
    if (at <= 0) return at;
    uint64_t target = (uint64_t)at + le32(t->b + at);
    if (target > t->n - 4) return -1;
    return (int64_t)target;
}

static int fb_string_at(const uint8_t *b, size_t n, size_t pos, span *s) {
    if (pos > n - 4) return -1;
    uint32_t len = le32(b + pos);
    if (len > n - pos - 4) return -1;
    s->p = b + pos + 4;
    s->n = len;
    return 0;
}

/* A string field: an empty span when absent; -1 damaged. */
static int fb_string(const ftab *t, int i, span *s) {
    int64_t at = fb_ref(t, i);
    s->p = (const uint8_t *)"";
    s->n = 0;
    if (at < 0) return -1;
    if (!at) return 0;
    return fb_string_at(t->b, t->n, (size_t)at, s);
}

/* A vector field of `count` elements of `esize` bytes from `*data`; an absent
 * one is empty. -1 damaged. */
static int fb_vector(const ftab *t, int i, size_t esize, uint32_t *count, size_t *data) {
    int64_t at = fb_ref(t, i);
    *count = 0;
    *data = 0;
    if (at < 0) return -1;
    if (!at) return 0;
    uint32_t c = le32(t->b + at);
    if (esize && (uint64_t)c * esize > t->n - (size_t)at - 4) return -1;
    *count = c;
    *data = (size_t)at + 4;
    return 0;
}

/* Element k of a vector of tables or strings: where it points. -1 damaged. */
static int64_t fb_elem(const ftab *t, size_t data, uint32_t k) {
    size_t p = data + 4 * (size_t)k;
    uint64_t target = (uint64_t)p + le32(t->b + p);
    if (target > t->n - 4) return -1;
    return (int64_t)target;
}

/* ---- the graph ------------------------------------------------------------ */

#define MAX_DIM 8
#define MAX_IO  8

typedef struct {
    span name;
    int dtype;              /* ONNX's codes */
    int ndim;
    int64_t dims[MAX_DIM];
    span data;              /* raw_data */
    int used;
} oinit;

typedef struct {
    span op, domain;
    span in[MAX_IO], out[MAX_IO];
    int nin, nout;
    ftab tab;               /* for its attributes */
} onode;

typedef struct {
    span version, producer;
    oinit *inits;
    size_t ninit;
    onode *nodes;
    size_t nnode;
    span in[16], out[16];
    int nin, nout;
} ograph;

enum { OT_F32 = 1, OT_U8 = 2, OT_I8 = 3, OT_I32 = 6, OT_I64 = 7, OT_BOOL = 9, OT_F16 = 10 };

static size_t ot_size(int t) {
    switch (t) {
    case OT_F32: case OT_I32: return 4;
    case OT_U8: case OT_I8: case OT_BOOL: return 1;
    case OT_I64: return 8;
    case OT_F16: return 2;
    default: return 0;
    }
}

static void graph_free(ograph *g) {
    free(g->inits);
    free(g->nodes);
    memset(g, 0, sizeof *g);
}

static int strings(const ftab *t, int field, span *out, int cap, int *count) {
    uint32_t c;
    size_t data;
    if (fb_vector(t, field, 4, &c, &data) != 0 || c > (uint32_t)cap) return -1;
    for (uint32_t k = 0; k < c; k++) {
        int64_t at = fb_elem(t, data, k);
        if (at < 0 || fb_string_at(t->b, t->n, (size_t)at, &out[k]) != 0) return -1;
    }
    *count = (int)c;
    return 0;
}

static int parse_tensor(const uint8_t *b, size_t n, size_t pos, oinit *x) {
    ftab t;
    int bad = 0;
    if (fb_table(b, n, pos, &t) != 0 || fb_string(&t, 0, &x->name) != 0) return -1;
    uint32_t nd;
    size_t dd;
    if (fb_vector(&t, 2, 8, &nd, &dd) != 0 || nd > MAX_DIM) return -1;
    x->ndim = (int)nd;
    uint64_t count = 1;
    for (uint32_t k = 0; k < nd; k++) {
        x->dims[k] = (int64_t)le64(b + dd + 8 * k);
        if (x->dims[k] < 0 || (x->dims[k] && count > UINT64_MAX / 8 / (uint64_t)x->dims[k])) return -1;
        count *= (uint64_t)x->dims[k];
    }
    x->dtype = (int)(int32_t)fb_scalar(&t, 3, 4, 0, &bad);
    uint32_t nraw;
    size_t raw;
    if (bad || fb_vector(&t, 4, 1, &nraw, &raw) != 0) return -1;
    x->data.p = b + raw;
    x->data.n = nraw;
    size_t es = ot_size(x->dtype);
    if (!es || (uint64_t)nraw != count * es) return -1;   /* every constant is raw and whole */
    return 0;
}

static int graph_parse(const uint8_t *b, size_t n, ograph *g, char *err, size_t cap) {
    memset(g, 0, sizeof *g);
    ftab root, model, graph;
    int64_t at;
    if (n < 8 || memcmp(b + 4, "ORTM", 4) != 0) { seterr(err, cap, "not an ONNX Runtime model file"); return -1; }
    if (fb_table(b, n, le32(b), &root) != 0 || fb_string(&root, 0, &g->version) != 0 ||
        (at = fb_ref(&root, 1)) <= 0 || fb_table(b, n, (size_t)at, &model) != 0 ||
        fb_string(&model, 2, &g->producer) != 0 || (at = fb_ref(&model, 7)) <= 0 ||
        fb_table(b, n, (size_t)at, &graph) != 0) {
        seterr(err, cap, "the model file is damaged (its header)");
        return -1;
    }
    uint32_t ni, nn;
    size_t di, dn;
    if (fb_vector(&graph, 0, 4, &ni, &di) != 0 || fb_vector(&graph, 2, 4, &nn, &dn) != 0 ||
        strings(&graph, 5, g->in, 16, &g->nin) != 0 || strings(&graph, 6, g->out, 16, &g->nout) != 0) {
        seterr(err, cap, "the model file is damaged (its graph)");
        return -1;
    }
    g->inits = calloc(ni ? ni : 1, sizeof *g->inits);
    g->nodes = calloc(nn ? nn : 1, sizeof *g->nodes);
    if (!g->inits || !g->nodes) { graph_free(g); seterr(err, cap, "out of memory"); return -1; }
    for (uint32_t k = 0; k < ni; k++) {
        at = fb_elem(&graph, di, k);
        if (at < 0 || parse_tensor(b, n, (size_t)at, &g->inits[k]) != 0) {
            graph_free(g);
            seterr(err, cap, "the model file is damaged (constant %u)", k);
            return -1;
        }
    }
    g->ninit = ni;
    for (uint32_t k = 0; k < nn; k++) {
        onode *x = &g->nodes[k];
        at = fb_elem(&graph, dn, k);
        if (at < 0 || fb_table(b, n, (size_t)at, &x->tab) != 0 || fb_string(&x->tab, 2, &x->domain) != 0 ||
            fb_string(&x->tab, 5, &x->op) != 0 || strings(&x->tab, 8, x->in, MAX_IO, &x->nin) != 0 ||
            strings(&x->tab, 9, x->out, MAX_IO, &x->nout) != 0) {
            graph_free(g);
            seterr(err, cap, "the model file is damaged (node %u)", k);
            return -1;
        }
    }
    g->nnode = nn;
    return 0;
}

/* ---- attributes ----------------------------------------------------------- */

enum { AT_FLOAT = 1, AT_INT = 2, AT_STRING = 3, AT_TENSOR = 4, AT_GRAPH = 5, AT_FLOATS = 6, AT_INTS = 7 };


static void print_span(FILE *f, span s) { fwrite(s.p, 1, s.n, f); }

static const char *ot_name(int t) {
    switch (t) {
    case OT_F32: return "f32";
    case OT_U8: return "u8";
    case OT_I8: return "i8";
    case OT_I32: return "i32";
    case OT_I64: return "i64";
    case OT_BOOL: return "bool";
    case OT_F16: return "f16";
    default: return "?";
    }
}

/* One attribute, for --list. */
static void print_attr(FILE *f, const ftab *a) {
    span nm, s;
    int bad = 0;
    fb_string(a, 0, &nm);
    int type = (int)(int32_t)fb_scalar(a, 2, 4, 0, &bad);
    print_span(f, nm);
    fputc('=', f);
    if (type == AT_FLOAT) {
        uint32_t u = (uint32_t)fb_scalar(a, 3, 4, 0, &bad);
        float v;
        memcpy(&v, &u, 4);
        fprintf(f, "%.9g", v);
    } else if (type == AT_INT) {
        fprintf(f, "%lld", (long long)(int64_t)fb_scalar(a, 4, 8, 0, &bad));
    } else if (type == AT_STRING) {
        if (fb_string(a, 5, &s) == 0) print_span(f, s);
    } else if (type == AT_INTS || type == AT_FLOATS) {
        uint32_t c;
        size_t d;
        int ints = type == AT_INTS;
        if (fb_vector(a, ints ? 9 : 8, ints ? 8 : 4, &c, &d) == 0) {
            fputc('[', f);
            for (uint32_t k = 0; k < c; k++) {
                if (k) fputc(',', f);
                if (ints) fprintf(f, "%lld", (long long)(int64_t)le64(a->b + d + 8 * k));
                else {
                    uint32_t u = le32(a->b + d + 4 * k);
                    float v;
                    memcpy(&v, &u, 4);
                    fprintf(f, "%.9g", v);
                }
            }
            fputc(']', f);
        }
    } else {
        fprintf(f, "<type %d>", type);
    }
}

/* A constant's first values, for --list: all of a small one. */
static void print_values(FILE *f, const oinit *x) {
    size_t es = ot_size(x->dtype), count = es ? x->data.n / es : 0;
    if (count > 8) return;
    fputs(" =", f);
    for (size_t k = 0; k < count; k++) {
        const uint8_t *p = x->data.p + k * es;
        switch (x->dtype) {
        case OT_F32: { uint32_t u = le32(p); float v; memcpy(&v, &u, 4); fprintf(f, " %.9g", v); break; }
        case OT_I64: fprintf(f, " %lld", (long long)(int64_t)le64(p)); break;
        case OT_I32: fprintf(f, " %d", (int)(int32_t)le32(p)); break;
        case OT_U8: case OT_BOOL: fprintf(f, " %u", p[0]); break;
        case OT_I8: fprintf(f, " %d", (int)(int8_t)p[0]); break;
        default: fprintf(f, " ?"); break;
        }
    }
}

int oc_recognizer_list_mem(const unsigned char *ort, size_t len, FILE *out, char *err, size_t errcap) {
    ograph g;
    if (graph_parse(ort, len, &g, err, errcap) != 0) return -1;
    fputs("format ", out);
    print_span(out, g.version);
    fputs(", producer ", out);
    print_span(out, g.producer);
    fputs("\ninputs:", out);
    for (int k = 0; k < g.nin; k++) { fputc(' ', out); print_span(out, g.in[k]); }
    fputs("\noutputs:", out);
    for (int k = 0; k < g.nout; k++) { fputc(' ', out); print_span(out, g.out[k]); }
    fprintf(out, "\nconstants: %zu\n", g.ninit);
    for (size_t k = 0; k < g.ninit; k++) {
        const oinit *x = &g.inits[k];
        fputs("  ", out);
        print_span(out, x->name);
        fprintf(out, " %s [", ot_name(x->dtype));
        for (int d = 0; d < x->ndim; d++) fprintf(out, d ? ",%lld" : "%lld", (long long)x->dims[d]);
        fputc(']', out);
        print_values(out, x);
        fputc('\n', out);
    }
    fprintf(out, "nodes: %zu\n", g.nnode);
    for (size_t k = 0; k < g.nnode; k++) {
        const onode *x = &g.nodes[k];
        fputs("  ", out);
        if (x->domain.n) { print_span(out, x->domain); fputc(':', out); }
        print_span(out, x->op);
        fputs(" (", out);
        for (int i = 0; i < x->nin; i++) { if (i) fputs(", ", out); print_span(out, x->in[i]); }
        fputs(") -> (", out);
        for (int i = 0; i < x->nout; i++) { if (i) fputs(", ", out); print_span(out, x->out[i]); }
        fputc(')', out);
        uint32_t c;
        size_t data;
        if (fb_vector(&x->tab, 10, 4, &c, &data) == 0)
            for (uint32_t i = 0; i < c; i++) {
                int64_t at = fb_elem(&x->tab, data, i);
                ftab a;
                if (at >= 0 && fb_table(x->tab.b, x->tab.n, (size_t)at, &a) == 0) {
                    fputc(' ', out);
                    print_attr(out, &a);
                }
            }
        fputc('\n', out);
    }
    graph_free(&g);
    return 0;
}

/* ---- finding things in a graph ---------------------------------------------- */

static int span_is(span s, const char *z) { size_t l = strlen(z); return s.n == l && memcmp(s.p, z, l) == 0; }
static int span_eq(span a, span b) { return a.n == b.n && memcmp(a.p, b.p, a.n) == 0; }

static oinit *init_named(const ograph *g, span name) {
    for (size_t k = 0; k < g->ninit; k++)
        if (span_eq(g->inits[k].name, name)) return &g->inits[k];
    return NULL;
}

static oinit *init_z(const ograph *g, const char *name) {
    span s = { (const uint8_t *)name, strlen(name) };
    return init_named(g, s);
}

/* The node that writes `name`. */
static const onode *producer(const ograph *g, span name) {
    for (size_t k = 0; k < g->nnode; k++)
        for (int i = 0; i < g->nodes[k].nout; i++)
            if (span_eq(g->nodes[k].out[i], name)) return &g->nodes[k];
    return NULL;
}

static const onode *producer_z(const ograph *g, const char *name) {
    span s = { (const uint8_t *)name, strlen(name) };
    return producer(g, s);
}

/* The first node of type `op` that reads `name`. */
static const onode *consumer(const ograph *g, span name, const char *op) {
    for (size_t k = 0; k < g->nnode; k++) {
        const onode *x = &g->nodes[k];
        if (!span_is(x->op, op)) continue;
        for (int i = 0; i < x->nin; i++)
            if (span_eq(x->in[i], name)) return x;
    }
    return NULL;
}

/* The node's attribute `name`: 1 with `a` its table, 0 if it has none, -1 damaged. */
static int node_attr(const onode *x, const char *name, ftab *a) {
    uint32_t c;
    size_t data;
    if (fb_vector(&x->tab, 10, 4, &c, &data) != 0) return -1;
    for (uint32_t k = 0; k < c; k++) {
        int64_t at = fb_elem(&x->tab, data, k);
        span nm;
        if (at < 0 || fb_table(x->tab.b, x->tab.n, (size_t)at, a) != 0 || fb_string(a, 0, &nm) != 0) return -1;
        if (span_is(nm, name)) return 1;
    }
    return 0;
}

static int attr_float(const onode *x, const char *name, float *v) {
    ftab a;
    int bad = 0;
    if (node_attr(x, name, &a) != 1) return -1;
    uint32_t u = (uint32_t)fb_scalar(&a, 3, 4, 0, &bad);
    memcpy(v, &u, 4);
    return bad ? -1 : 0;
}

/* An int or a one-element ints attribute. */
static int attr_int(const onode *x, const char *name, int64_t *v) {
    ftab a;
    int bad = 0;
    if (node_attr(x, name, &a) != 1) return -1;
    int type = (int)(int32_t)fb_scalar(&a, 2, 4, 0, &bad);
    if (type == AT_INT) { *v = (int64_t)fb_scalar(&a, 4, 8, 0, &bad); return bad ? -1 : 0; }
    uint32_t c;
    size_t d;
    if (type != AT_INTS || fb_vector(&a, 9, 8, &c, &d) != 0 || c < 1) return -1;
    *v = (int64_t)le64(a.b + d);
    for (uint32_t k = 1; k < c; k++)
        if ((int64_t)le64(a.b + d + 8 * k) != *v) return -1;
    return 0;
}

static int64_t init_i64(const oinit *x) { return x && x->dtype == OT_I64 && x->data.n == 8 ? (int64_t)le64(x->data.p) : INT64_MIN; }

static float init_f32(const oinit *x) {
    if (!x || x->dtype != OT_F32 || x->data.n != 4) return NAN;
    uint32_t u = le32(x->data.p);
    float v;
    memcpy(&v, &u, 4);
    return v;
}

static float f32_at(const oinit *x, size_t k) {
    uint32_t u = le32(x->data.p + 4 * k);
    float v;
    memcpy(&v, &u, 4);
    return v;
}

static int has_shape(const oinit *x, int dtype, int ndim, int64_t a, int64_t b, int64_t c) {
    if (!x || x->dtype != dtype || x->ndim != ndim) return 0;
    int64_t want[3] = { a, b, c };
    for (int d = 0; d < ndim && d < 3; d++)
        if (x->dims[d] != want[d]) return 0;
    return 1;
}

/* ---- what goes into the file ------------------------------------------------ */

enum {
    E_F32 = 1,      /* a float constant, as it is */
    E_U8,           /* a uint8 constant, as it is */
    E_U8T,          /* a uint8 [k][n] matrix, transposed to [n][k] */
    E_RAW,          /* bytes of an input file */
    E_I32,          /* the dimensions */
    E_SCALAR,       /* one float the converter read */
    E_DEQ_MAT,      /* the frontend's linear layer: int8 [k][n] * scale[n] + zero[n] */
    E_DEQ_CONV      /* a frontend convolution: int8 [cout][cin][k] dequantised, to [cin * k][cout] */
};

typedef struct {
    char name[REC_NAME_MAX];
    int dtype, ndim, dims[REC_MAX_DIMS];
    uint64_t bytes, off;
    float scale;
    int zp;
    int kind;
    const oinit *src, *sc, *zr;
    span raw;
    int32_t iv[REC_CFG_COUNT];
    float fv;
} entry;

typedef struct {
    entry *e;
    size_t n, cap;
    char *err;
    size_t errcap;
} plan;

static entry *add(plan *p, const char *fmt, ...) {
    if (p->n == p->cap) {
        size_t cap = p->cap ? p->cap * 2 : 256;
        entry *e = realloc(p->e, cap * sizeof *e);
        if (!e) { seterr(p->err, p->errcap, "out of memory"); return NULL; }
        p->e = e;
        p->cap = cap;
    }
    entry *x = &p->e[p->n++];
    memset(x, 0, sizeof *x);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(x->name, sizeof x->name, fmt, ap);
    va_end(ap);
    return x;
}

static void dims(entry *x, int dtype, int nd, int a, int b) {
    x->dtype = dtype;
    x->ndim = nd;
    x->dims[0] = a;
    x->dims[1] = nd > 1 ? b : 0;
    uint64_t count = (uint64_t)a * (uint64_t)(nd > 1 ? b : 1);
    x->bytes = count * (dtype == REC_U8 ? 1 : 4);
}

static int f32_vec(plan *p, const oinit *x, int n, const char *what, const char *fmt, ...) {
    if (!has_shape(x, OT_F32, 1, n, 0, 0)) { seterr(p->err, p->errcap, "%s is not %d floats", what, n); return -1; }
    char name[REC_NAME_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(name, sizeof name, fmt, ap);
    va_end(ap);
    entry *e = add(p, "%s", name);
    if (!e) return -1;
    ((oinit *)x)->used = 1;
    e->kind = E_F32;
    e->src = x;
    dims(e, REC_F32, 1, n, 0);
    return 0;
}

/* A quantised matrix product's weight: B uint8 [k][n] with a scale and a zero
 * point per output column, as <base>.w uint8 [n][k], <base>.s f32 [n] and
 * <base>.zp uint8 [n]. */
static int qweight(plan *p, const oinit *b, const oinit *s, const oinit *z, int k, int n, const char *base) {
    if (!has_shape(b, OT_U8, 2, k, n, 0) || !has_shape(s, OT_F32, 1, n, 0, 0) || !has_shape(z, OT_U8, 1, n, 0, 0)) {
        seterr(p->err, p->errcap, "%s is not a uint8 %d x %d weight with a scale and zero point per column", base, k, n);
        return -1;
    }
    ((oinit *)b)->used = ((oinit *)s)->used = ((oinit *)z)->used = 1;
    entry *e = add(p, "%s.w", base);
    if (!e) return -1;
    e->kind = E_U8T;
    e->src = b;
    dims(e, REC_U8, 2, n, k);
    if (!(e = add(p, "%s.s", base))) return -1;
    e->kind = E_F32;
    e->src = s;
    dims(e, REC_F32, 1, n, 0);
    if (!(e = add(p, "%s.zp", base))) return -1;
    e->kind = E_U8;
    e->src = z;
    dims(e, REC_U8, 1, n, 0);
    return 0;
}

/* The weight of the product that writes `out`: MatMulIntegerToFloat (A, B,
 * a_scale, b_scale, a_zero, b_zero) or DynamicQuantizeMatMul (A, B, b_scale,
 * b_zero, bias). A bias, when `bias_n` > 0, is <base>.b. */
static int product(plan *p, const ograph *g, const char *out, int k, int n, int bias_n, const char *base) {
    const onode *x = producer_z(g, out);
    if (!x) { seterr(p->err, p->errcap, "no node writes %s", out); return -1; }
    int dq = span_is(x->op, "DynamicQuantizeMatMul");
    if (!span_is(x->domain, "com.microsoft") || !(dq || span_is(x->op, "MatMulIntegerToFloat")) || x->nin < (dq ? 4 : 6)) {
        seterr(p->err, p->errcap, "%s is not written by a quantised matrix product", out);
        return -1;
    }
    oinit *b = init_named(g, x->in[1]), *s = init_named(g, x->in[dq ? 2 : 3]), *z = init_named(g, x->in[dq ? 3 : 5]);
    if (qweight(p, b, s, z, k, n, base) != 0) return -1;
    span bn = dq && x->nin > 4 ? x->in[4] : (!dq && x->nin > 6 ? x->in[6] : (span){ (const uint8_t *)"", 0 });
    if (!bias_n) {
        if (bn.n) { seterr(p->err, p->errcap, "%s has a bias the forward pass does not add", out); return -1; }
        return 0;
    }
    return f32_vec(p, init_named(g, bn), bias_n, out, "%s.b", base);
}

static const char *ln_name(char *b, size_t cap, int k) {
    if (k) snprintf(b, cap, "layer_norm_%d", k);
    else snprintf(b, cap, "layer_norm");
    return b;
}

/* The gain after layer norm `k`: Mul(layer_norm_k, gain). The norm itself has
 * scale 1, no bias and the model's epsilon, checked. */
static int gain(plan *p, const ograph *g, int k, float *eps, const char *fmt, int i) {
    char ln[32];
    ln_name(ln, sizeof ln, k);
    span s = { (const uint8_t *)ln, strlen(ln) };
    const onode *norm = producer(g, s), *mul = consumer(g, s, "Mul");
    if (!norm || !mul || mul->nin != 2 || !span_eq(mul->in[0], s)) { seterr(p->err, p->errcap, "%s has no gain after it", ln); return -1; }
    int skip = span_is(norm->op, "SkipLayerNormalization");
    if (!(skip || span_is(norm->op, "LayerNormalization")) || norm->nin < (skip ? 3 : 2) ||
        (norm->nin > (skip ? 3 : 2) && norm->in[skip ? 3 : 2].n)) {
        seterr(p->err, p->errcap, "%s is not a layer norm without a bias", ln);
        return -1;
    }
    oinit *one = init_named(g, norm->in[skip ? 2 : 1]);
    if (!has_shape(one, OT_F32, 1, 320, 0, 0)) { seterr(p->err, p->errcap, "%s's scale is not 320 floats", ln); return -1; }
    for (int c = 0; c < 320; c++)
        if (f32_at(one, (size_t)c) != 1.0f) { seterr(p->err, p->errcap, "%s's scale is not 1", ln); return -1; }
    one->used = 1;
    float e;
    if (attr_float(norm, "epsilon", &e) != 0 || (*eps == *eps && e != *eps)) {
        seterr(p->err, p->errcap, "%s's epsilon differs from the others", ln);
        return -1;
    }
    *eps = e;
    char name[REC_NAME_MAX];
    snprintf(name, sizeof name, fmt, i);
    return f32_vec(p, init_named(g, mul->in[1]), 320, ln, "%s", name);
}

/* The score scale of every attention, the same everywhere. */
static int check_alpha(plan *p, const onode *mm, float *alpha) {
    float a;
    int64_t tb = 0;
    if (!mm || !span_is(mm->op, "FusedMatMul") || attr_float(mm, "alpha", &a) != 0) {
        seterr(p->err, p->errcap, "an attention's scores are not a scaled product");
        return -1;
    }
    attr_int(mm, "transB", &tb);
    if (*alpha == *alpha && a != *alpha) { seterr(p->err, p->errcap, "the attentions' score scales differ"); return -1; }
    *alpha = a;
    return 0;
}

/* One encoder block's attention window, read from the mask its scores get:
 * Where(And(i - j >= lo, i - j <= hi), 0, -inf). Rows back = hi, ahead = -lo. */
static int window(plan *p, const ograph *g, int b, int32_t *back, int32_t *ahead, float *alpha) {
    char q[32];
    snprintf(q, sizeof q, b ? "linear_%d" : "linear", 6 * b);
    span s = { (const uint8_t *)q, strlen(q) };
    const onode *r = consumer(g, s, "Reshape");
    const onode *t = r ? consumer(g, r->out[0], "Transpose") : NULL;
    const onode *mm = t ? consumer(g, t->out[0], "FusedMatMul") : NULL;
    if (check_alpha(p, mm, alpha) != 0) return -1;
    const onode *add = consumer(g, mm->out[0], "Add");
    const onode *u1 = add && add->nin == 2 ? producer(g, add->in[1]) : NULL;
    const onode *wh = u1 && span_is(u1->op, "Unsqueeze") ? producer(g, u1->in[0]) : NULL;
    const onode *u2 = wh && span_is(wh->op, "Where") ? producer(g, wh->in[0]) : NULL;
    const onode *and = u2 && span_is(u2->op, "Unsqueeze") ? producer(g, u2->in[0]) : NULL;
    if (!and || !span_is(and->op, "And") || and->nin != 2 || init_f32(init_named(g, wh->in[1])) != 0.0f ||
        init_f32(init_named(g, wh->in[2])) != -INFINITY) {
        seterr(p->err, p->errcap, "encoder layer %d's attention mask is not a window", b);
        return -1;
    }
    int have_lo = 0, have_hi = 0;
    for (int i = 0; i < 2; i++) {
        const onode *c = producer(g, and->in[i]);
        int64_t v = c && c->nin == 2 ? init_i64(init_named(g, c->in[1])) : INT64_MIN;
        if (!c || v == INT64_MIN) break;
        if (span_is(c->op, "GreaterOrEqual")) { *ahead = (int32_t)-v; have_lo = 1; }
        if (span_is(c->op, "LessOrEqual")) { *back = (int32_t)v; have_hi = 1; }
    }
    if (!have_lo || !have_hi || *back < 0 || *ahead < 0) {
        seterr(p->err, p->errcap, "encoder layer %d's attention window is not a range", b);
        return -1;
    }
    return 0;
}

/* streaming_config.json: one integer, read from the shape of one pinned file. */
static int config_int(span b, const char *key, int *out) {
    char pat[48];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    size_t plen = strlen(pat);
    for (size_t i = 0; i + plen < b.n; i++) {
        if (memcmp(b.p + i, pat, plen)) continue;
        size_t j = i + plen;
        while (j < b.n && (b.p[j] == ' ' || b.p[j] == ':' || b.p[j] == '\n')) j++;
        long v = 0;
        int any = 0;
        while (j < b.n && b.p[j] >= '0' && b.p[j] <= '9' && v < 1000000) v = v * 10 + (b.p[j++] - '0'), any = 1;
        if (!any) return -1;
        *out = (int)v;
        return 0;
    }
    return -1;
}

/* A constant every graph may hold that the forward pass has no use for: shape
 * arithmetic (int64), the graph's float scalars (0, 1, 0.5, sqrt 2, -inf),
 * which are the forward pass's own code. */
static int incidental(const oinit *x) {
    if (x->dtype == OT_I64 || x->dtype == OT_BOOL) return 1;
    return x->dtype == OT_F32 && x->data.n <= 4;
}

static int leftovers(plan *p, const ograph *g, const char *file) {
    for (size_t k = 0; k < g->ninit; k++) {
        const oinit *x = &g->inits[k];
        if (x->used || incidental(x)) continue;
        seterr(p->err, p->errcap, "%s has a constant the forward pass does not know: %.*s", file, (int)x->name.n,
               (const char *)x->name.p);
        return -1;
    }
    return 0;
}

/* ---- the graphs, one by one -------------------------------------------------- */

static int take_frontend(plan *p, ograph *fm, ograph *fw, float *eps_fe) {
    /* frontend.weights: each output Add(Mul(Cast(q), scale), zero). */
    struct { const char *node, *name; int conv; } W[] = {
        { "/lin/MatMul_output_0", "fe.lin.w", 0 },
        { "/Conv_output_0", "fe.c1.w", 1 },
        { "/Conv_1_output_0", "fe.c2.w", 1 },
    };
    for (int i = 0; i < 3; i++) {
        const onode *use = producer_z(fm, W[i].node);
        if (!use || use->nin < 2) { seterr(p->err, p->errcap, "the frontend has no %s", W[i].node); return -1; }
        const onode *a = producer(fw, use->in[1]);
        const onode *m = a && span_is(a->op, "Add") ? producer(fw, a->in[0]) : NULL;
        const onode *c = m && span_is(m->op, "Mul") ? producer(fw, m->in[0]) : NULL;
        oinit *q = c && span_is(c->op, "Cast") ? init_named(fw, c->in[0]) : NULL;
        oinit *s = m ? init_named(fw, m->in[1]) : NULL, *z = a ? init_named(fw, a->in[1]) : NULL;
        if (!q || !s || !z || q->dtype != OT_I8 || s->dtype != OT_F32 || z->dtype != OT_F32) {
            seterr(p->err, p->errcap, "the frontend's weight for %s is not int8 * scale + zero", W[i].node);
            return -1;
        }
        q->used = s->used = z->used = 1;
        entry *e = add(p, "%s", W[i].name);
        if (!e) return -1;
        e->src = q;
        e->sc = s;
        e->zr = z;
        if (!W[i].conv) {
            if (!has_shape(q, OT_I8, 2, 80, 320, 0) || !has_shape(s, OT_F32, 2, 1, 320, 0) || !has_shape(z, OT_F32, 2, 1, 320, 0)) {
                seterr(p->err, p->errcap, "the frontend's linear weight is not 80 x 320 per column");
                return -1;
            }
            e->kind = E_DEQ_MAT;
            dims(e, REC_F32, 2, 80, 320);
            continue;
        }
        int cout = i == 1 ? 640 : 320, cin = i == 1 ? 320 : 640;
        if (!has_shape(q, OT_I8, 3, cout, cin, 5) || s->ndim != 3 || z->ndim != 3) {
            seterr(p->err, p->errcap, "the frontend's convolution %d is not %d x %d x 5", i, cout, cin);
            return -1;
        }
        for (int d = 0; d < 3; d++)
            if ((s->dims[d] != 1 && s->dims[d] != q->dims[d]) || (z->dims[d] != 1 && z->dims[d] != q->dims[d]) || (d == 2 && (s->dims[d] != 1 || z->dims[d] != 1))) {
                seterr(p->err, p->errcap, "the frontend's convolution %d has a scale of another shape", i);
                return -1;
            }
        int64_t st, ks, dl, gr, pd;
        if (attr_int(use, "strides", &st) || attr_int(use, "kernel_shape", &ks) || attr_int(use, "dilations", &dl) ||
            attr_int(use, "group", &gr) || attr_int(use, "pads", &pd) || st != 2 || ks != 5 || dl != 1 || gr != 1 || pd != 0) {
            seterr(p->err, p->errcap, "the frontend's convolution %d is not kernel 5, stride 2, unpadded", i);
            return -1;
        }
        e->kind = E_DEQ_CONV;
        dims(e, REC_F32, 2, 5 * cin, cout);
        if (f32_vec(p, init_z(fm, i == 1 ? "conv1.conv.bias" : "conv2.conv.bias"), cout, "a frontend bias",
                    i == 1 ? "fe.c1.b" : "fe.c2.b") != 0)
            return -1;
    }
    /* The carried rows: the last 4 of each convolution's input. */
    oinit *keep = init_z(fm, "/Constant_20_output_0");
    if (init_i64(keep) != -4) { seterr(p->err, p->errcap, "the frontend does not carry 4 rows"); return -1; }
    /* The normalisation's epsilon and the compression's scale. */
    oinit *e1 = init_z(fm, "/cmvn/Constant_3_output_0"), *cs = init_z(fm, "/comp/Exp_output_0");
    if (!(init_f32(e1) > 0) || !(init_f32(cs) > 0)) { seterr(p->err, p->errcap, "the frontend's constants are not as expected"); return -1; }
    *eps_fe = init_f32(e1);
    entry *e = add(p, "fe.comp");
    if (!e) return -1;
    e->kind = E_SCALAR;
    e->fv = init_f32(cs);
    dims(e, REC_F32, 1, 1, 0);
    if (!(e = add(p, "fe.eps"))) return -1;
    e->kind = E_SCALAR;
    e->fv = *eps_fe;
    dims(e, REC_F32, 1, 1, 0);
    /* The outputs the graph returns as constants: an empty sample buffer. */
    oinit *sb = init_z(fm, "sample_buffer_out");
    if (sb) sb->used = 1;
    return leftovers(p, fm, "frontend.model.ort") || leftovers(p, fw, "frontend.weights.ort") ? -1 : 0;
}

static int take_encoder(plan *p, ograph *g, float *eps, float *alpha, int32_t *win) {
    static const char *const part[6] = { "q", "k", "v", "o", "f1", "f2" };
    char out[32];
    for (int b = 0; b < 6; b++) {
        for (int j = 0; j < 6; j++) {
            int k = j == 5 ? 1280 : 320, n = j == 4 ? 1280 : 320, bias = j >= 4 ? n : 0;
            snprintf(out, sizeof out, b || j ? "linear_%d" : "linear", 6 * b + j);
            char base[REC_NAME_MAX];
            snprintf(base, sizeof base, "enc.%d.%s", b, part[j]);
            if (product(p, g, out, k, n, bias, base) != 0) return -1;
        }
        if (gain(p, g, 2 * b, eps, "enc.%d.g1", b) != 0 || gain(p, g, 2 * b + 1, eps, "enc.%d.g2", b) != 0) return -1;
        if (window(p, g, b, &win[2 * b], &win[2 * b + 1], alpha) != 0) return -1;
    }
    if (gain(p, g, 12, eps, "enc.g%.0d", 0) != 0) return -1;
    return leftovers(p, g, "encoder.ort");
}

static int take_adapter(plan *p, ograph *g) {
    oinit *q = init_z(g, "pos_embed.weight_quantized"), *s = init_z(g, "pos_embed.weight_scale"),
          *z = init_z(g, "pos_embed.weight_zero_point");
    if (!has_shape(q, OT_U8, 2, 4096, 320, 0) || !s || s->dtype != OT_F32 || s->data.n != 4 || !z ||
        z->dtype != OT_U8 || z->data.n != 1) {
        seterr(p->err, p->errcap, "the adapter's position table is not uint8 4096 x 320 with one scale");
        return -1;
    }
    q->used = s->used = z->used = 1;
    entry *e = add(p, "pos");
    if (!e) return -1;
    e->kind = E_U8;
    e->src = q;
    e->scale = init_f32(s);
    e->zp = z->data.p[0];
    dims(e, REC_U8, 2, 4096, 320);
    return leftovers(p, g, "adapter.ort");
}

static int take_cross(plan *p, ograph *g) {
    char out[32], base[REC_NAME_MAX];
    for (int b = 0; b < 6; b++)
        for (int j = 0; j < 2; j++) {
            snprintf(out, sizeof out, b || j ? "linear_%d" : "linear", 2 * b + j);
            snprintf(base, sizeof base, "dec.%d.x%s", b, j ? "v" : "k");
            if (product(p, g, out, 320, 320, 0, base) != 0) return -1;
        }
    return leftovers(p, g, "cross_kv.ort");
}

static int take_decoder(plan *p, ograph *g, float *eps, float *alpha) {
    static const char *const part[8] = { "q", "k", "v", "o", "cq", "co", "f1", "f2" };
    char out[32], base[REC_NAME_MAX];
    for (int b = 0; b < 6; b++) {
        for (int j = 0; j < 8; j++) {
            int k = j == 7 ? 1280 : 320, n = j == 6 ? 2560 : 320, bias = j >= 6 ? n : 0;
            snprintf(out, sizeof out, b || j ? "linear_%d" : "linear", 8 * b + j);
            snprintf(base, sizeof base, "dec.%d.%s", b, part[j]);
            if (product(p, g, out, k, n, bias, base) != 0) return -1;
        }
        for (int j = 0; j < 3; j++) {
            char fmt[24];
            snprintf(fmt, sizeof fmt, "dec.%%d.g%d", j + 1);
            if (gain(p, g, 3 * b + j, eps, fmt, b) != 0) return -1;
        }
    }
    if (gain(p, g, 18, eps, "dec.g%.0d", 0) != 0) return -1;
    for (size_t k = 0; k < g->nnode; k++)
        if (span_is(g->nodes[k].op, "FusedMatMul") && check_alpha(p, &g->nodes[k], alpha) != 0) return -1;
    if (f32_vec(p, init_z(g, "rotary.inv_freq"), 16, "the rotary frequencies", "dec.rope") != 0) return -1;
    /* Rotary positions on the first 32 of each head's 40 dimensions. */
    for (size_t k = 0; k < g->ninit; k++) {
        const oinit *x = &g->inits[k];
        if (x->name.n > 12 && !memcmp(x->name.p, "splits_token", 12) && x->data.n == 16 && le64(x->data.p) != 1 &&
            (le64(x->data.p) != 32 || le64(x->data.p + 8) != 8)) {
            seterr(p->err, p->errcap, "the decoder's rotary positions do not cover 32 of 40 dimensions");
            return -1;
        }
    }
    /* The embedding, and the output layer: the same table, transposed. */
    oinit *q = init_z(g, "embedding.weight_quantized"), *t = init_z(g, "t_quantized"),
          *s = init_z(g, "embedding.weight_scale"), *z = init_z(g, "embedding.weight_zero_point");
    if (!has_shape(q, OT_U8, 2, 32768, 320, 0) || !has_shape(t, OT_U8, 2, 320, 32768, 0) || !s || s->data.n != 4 ||
        s->dtype != OT_F32 || !z || z->dtype != OT_U8 || z->data.n != 1) {
        seterr(p->err, p->errcap, "the decoder's embedding is not uint8 32768 x 320 with one scale");
        return -1;
    }
    for (size_t v = 0; v < 32768; v++)
        for (size_t c = 0; c < 320; c++)
            if (q->data.p[v * 320 + c] != t->data.p[c * 32768 + v]) {
                seterr(p->err, p->errcap, "the decoder's output layer is not its embedding transposed");
                return -1;
            }
    const onode *dl = producer_z(g, "t");
    if (!dl || !span_is(dl->op, "DequantizeLinear") || dl->nin < 3 || !span_eq(dl->in[1], s->name) || !span_eq(dl->in[2], z->name)) {
        seterr(p->err, p->errcap, "the decoder's output layer does not share its embedding's scale");
        return -1;
    }
    q->used = t->used = s->used = z->used = 1;
    entry *e = add(p, "emb");
    if (!e) return -1;
    e->kind = E_U8;
    e->src = q;
    e->scale = init_f32(s);
    e->zp = z->data.p[0];
    dims(e, REC_U8, 2, 32768, 320);
    return leftovers(p, g, "decoder_kv.ort");
}

/* ---- writing ---------------------------------------------------------------- */

static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void put64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint64_t round64(uint64_t n) { return (n + 63) & ~(uint64_t)63; }
static void putf(uint8_t *p, float v) { uint32_t u; memcpy(&u, &v, 4); put32(p, u); }

/* q * scale + zero as the published graph computes it (Cast, Mul, Add), in two
 * statements so no compiler fuses them into one rounding. */
static float deq(int8_t q, float s, float z) {
    float m = (float)q * s;
    return m + z;
}

static void fill(const entry *e, uint8_t *out) {
    const oinit *x = e->src;
    switch (e->kind) {
    case E_F32:
    case E_U8:
        memcpy(out, x->data.p, (size_t)e->bytes);
        break;
    case E_U8T: {
        size_t k = (size_t)x->dims[0], n = (size_t)x->dims[1];
        for (size_t c = 0; c < n; c++)
            for (size_t i = 0; i < k; i++) out[c * k + i] = x->data.p[i * n + c];
        break;
    }
    case E_RAW:
        memcpy(out, e->raw.p, e->raw.n);
        break;
    case E_I32:
        for (int i = 0; i < REC_CFG_COUNT; i++) put32(out + 4 * i, (uint32_t)e->iv[i]);
        break;
    case E_SCALAR:
        putf(out, e->fv);
        break;
    case E_DEQ_MAT: {
        size_t k = (size_t)x->dims[0], n = (size_t)x->dims[1];
        for (size_t i = 0; i < k; i++)
            for (size_t c = 0; c < n; c++)
                putf(out + 4 * (i * n + c), deq((int8_t)x->data.p[i * n + c], f32_at(e->sc, c), f32_at(e->zr, c)));
        break;
    }
    case E_DEQ_CONV: {
        size_t cout = (size_t)x->dims[0], cin = (size_t)x->dims[1], kk = (size_t)x->dims[2];
        for (size_t o = 0; o < cout; o++)
            for (size_t i = 0; i < cin; i++)
                for (size_t j = 0; j < kk; j++) {
                    size_t si = (e->sc->dims[0] > 1 ? o : 0) + (e->sc->dims[1] > 1 ? i : 0);
                    size_t zi = (e->zr->dims[0] > 1 ? o : 0) + (e->zr->dims[1] > 1 ? i : 0);
                    float v = deq((int8_t)x->data.p[(o * cin + i) * kk + j], f32_at(e->sc, si), f32_at(e->zr, zi));
                    putf(out + 4 * ((i * kk + j) * cout + o), v);
                }
        break;
    }
    }
}

static int entry_cmp(const void *a, const void *b) { return strcmp(((const entry *)a)->name, ((const entry *)b)->name); }

static int write_file(const char *out, entry *e, size_t ne, const uint8_t sha[RP_FILES][32], size_t *written,
                      char *err, size_t cap) {
    uint64_t data = round64(REC_HEADER + (uint64_t)ne * REC_ENTRY), off = data;
    for (size_t i = 0; i < ne; i++) {
        e[i].off = off;
        off = round64(off + e[i].bytes);
    }
    uint64_t size = e[ne - 1].off + e[ne - 1].bytes;
    uint8_t *head = calloc(1, (size_t)data);
    if (!head) { seterr(err, cap, "out of memory"); return -1; }
    memcpy(head, REC_MAGIC, 8);
    put32(head + 8, REC_VERSION);
    put32(head + 12, (uint32_t)ne);
    put32(head + 16, REC_HEADER);
    put32(head + 20, REC_ENTRY);
    put64(head + 24, data);
    put64(head + 32, size);
    for (int i = 0; i < RP_FILES; i++) memcpy(head + 40 + 32 * i, sha[i], 32);
    for (size_t i = 0; i < ne; i++) {
        uint8_t *x = head + REC_HEADER + i * REC_ENTRY;
        memcpy(x, e[i].name, strlen(e[i].name));
        put32(x + 48, (uint32_t)e[i].dtype);
        put32(x + 52, (uint32_t)e[i].ndim);
        for (int d = 0; d < REC_MAX_DIMS; d++) put32(x + 56 + 4 * d, (uint32_t)e[i].dims[d]);
        put64(x + 72, e[i].off);
        put64(x + 80, e[i].bytes);
        putf(x + 88, e[i].scale);
        put32(x + 92, (uint32_t)e[i].zp);
    }
    char tmp[4200];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", out) >= (int)sizeof tmp) { free(head); seterr(err, cap, "the path is too long"); return -1; }
    FILE *f = fopen(tmp, "wb");
    if (!f) { free(head); seterr(err, cap, "cannot create %s", tmp); return -1; }
    int bad = fwrite(head, 1, (size_t)data, f) != (size_t)data;
    free(head);
    static const uint8_t zeros[64];
    uint64_t at = data;
    for (size_t i = 0; i < ne && !bad; i++) {
        if (e[i].off > at) bad = fwrite(zeros, 1, (size_t)(e[i].off - at), f) != (size_t)(e[i].off - at);
        uint8_t *buf = malloc((size_t)e[i].bytes);
        if (!buf) { bad = 1; break; }
        fill(&e[i], buf);
        if (!bad) bad = fwrite(buf, 1, (size_t)e[i].bytes, f) != (size_t)e[i].bytes;
        free(buf);
        at = e[i].off + e[i].bytes;
    }
    if (fclose(f) != 0) bad = 1;
    if (bad || rename(tmp, out) != 0) {
        unlink(tmp);
        seterr(err, cap, "cannot write %s", out);
        return -1;
    }
    if (written) *written = (size_t)size;
    return 0;
}

int oc_recognizer_pack_mem(const oc_rp_blob in[RP_FILES], const char *out, size_t *written, char *err,
                           size_t errcap) {
    static const int graphs[] = { RP_FRONTEND, RP_FRONTEND_WEIGHTS, RP_ENCODER, RP_ADAPTER, RP_CROSS_KV, RP_DECODER };
    ograph g[RP_FILES];
    memset(g, 0, sizeof g);
    plan p = { NULL, 0, 0, err, errcap };
    int rc = -1;
    for (size_t i = 0; i < sizeof graphs / sizeof graphs[0]; i++) {
        char why[256];
        if (graph_parse(in[graphs[i]].data, in[graphs[i]].len, &g[graphs[i]], why, sizeof why) != 0) {
            seterr(err, errcap, "%s: %s", OC_RECOGNIZER_INPUTS[graphs[i]], why);
            goto done;
        }
    }
    /* The configuration: the dimensions the forward pass is written for. */
    span cfg = { in[RP_CONFIG].data, in[RP_CONFIG].len };
    static const struct { const char *key; int want; } C[] = {
        { "encoder_dim", 320 }, { "decoder_dim", 320 }, { "depth", 6 }, { "nheads", 8 }, { "head_dim", 40 },
        { "vocab_size", 32768 }, { "frame_len", 80 }, { "d_model_frontend", 320 }, { "c1", 640 }, { "c2", 320 },
    };
    for (size_t i = 0; i < sizeof C / sizeof C[0]; i++) {
        int v;
        if (config_int(cfg, C[i].key, &v) != 0 || v != C[i].want) {
            seterr(err, errcap, "streaming_config.json: %s is not %d", C[i].key, C[i].want);
            goto done;
        }
    }
    int bos, eos;
    if (config_int(cfg, "bos_id", &bos) != 0 || config_int(cfg, "eos_id", &eos) != 0 || bos < 0 || eos < 0 ||
        bos >= 32768 || eos >= 32768) {
        seterr(err, errcap, "streaming_config.json: no start and end tokens");
        goto done;
    }
    float eps = NAN, eps_fe = NAN, alpha = NAN;
    int32_t win[12];
    if (take_frontend(&p, &g[RP_FRONTEND], &g[RP_FRONTEND_WEIGHTS], &eps_fe) != 0 ||
        take_encoder(&p, &g[RP_ENCODER], &eps, &alpha, win) != 0 || take_adapter(&p, &g[RP_ADAPTER]) != 0 ||
        take_cross(&p, &g[RP_CROSS_KV]) != 0 || take_decoder(&p, &g[RP_DECODER], &eps, &alpha) != 0)
        goto done;
    entry *e;
    if (!(e = add(&p, "eps"))) goto done;
    e->kind = E_SCALAR;
    e->fv = eps;
    dims(e, REC_F32, 1, 1, 0);
    if (!(e = add(&p, "alpha"))) goto done;
    e->kind = E_SCALAR;
    e->fv = alpha;
    dims(e, REC_F32, 1, 1, 0);
    if (!(e = add(&p, "tokens"))) goto done;
    e->kind = E_RAW;
    e->raw.p = in[RP_TOKENIZER].data;
    e->raw.n = in[RP_TOKENIZER].len;
    if (!e->raw.n || e->raw.n > INT32_MAX) { seterr(err, errcap, "tokenizer.bin is empty"); goto done; }
    dims(e, REC_U8, 1, (int)e->raw.n, 0);
    if (!(e = add(&p, "cfg"))) goto done;
    e->kind = E_I32;
    int32_t iv[REC_CFG_WINDOW] = { 320, 6, 8, 40, 32768, bos, eos, 80, 640, 5, 2, 1280, 1280, 32, 4096 };
    memcpy(e->iv, iv, sizeof iv);
    memcpy(e->iv + REC_CFG_WINDOW, win, sizeof win);
    dims(e, REC_I32, 1, REC_CFG_COUNT, 0);
    qsort(p.e, p.n, sizeof *p.e, entry_cmp);
    for (size_t i = 1; i < p.n; i++)
        if (!strcmp(p.e[i - 1].name, p.e[i].name)) { seterr(err, errcap, "the name %s is used twice", p.e[i].name); goto done; }
    uint8_t sha[RP_FILES][32];
    for (int i = 0; i < RP_FILES; i++) mbedtls_sha256(in[i].data, in[i].len, sha[i], 0);
    rc = write_file(out, p.e, p.n, sha, written, err, errcap);
done:
    free(p.e);
    for (int i = 0; i < RP_FILES; i++) graph_free(&g[i]);
    return rc;
}

int oc_recognizer_pack(const char *dir, const char *out, size_t *written, char *err, size_t errcap) {
    oc_tts_map m[RP_FILES];
    oc_rp_blob in[RP_FILES];
    memset(m, 0, sizeof m);
    int rc = -1;
    for (int i = 0; i < RP_FILES; i++) {
        char path[4200];
        snprintf(path, sizeof path, "%s/%s", dir, OC_RECOGNIZER_INPUTS[i]);
        if (oc_tts_map_open(path, &m[i]) != 0) { seterr(err, errcap, "cannot read %s", path); goto done; }
        in[i].data = m[i].p;
        in[i].len = m[i].n;
    }
    rc = oc_recognizer_pack_mem(in, out, written, err, errcap);
done:
    for (int i = 0; i < RP_FILES; i++) oc_tts_map_close(&m[i]);
    return rc;
}

/* ---- the command ---------------------------------------------------------- */

static int list_dir(const char *dir) {
    int rc = 0;
    for (int i = 0; i < RP_FILES; i++) {
        const char *name = OC_RECOGNIZER_INPUTS[i];
        size_t l = strlen(name);
        if (l < 4 || strcmp(name + l - 4, ".ort") != 0) continue;
        char path[4200], err[300] = "";
        snprintf(path, sizeof path, "%s/%s", dir, name);
        oc_tts_map m;
        if (oc_tts_map_open(path, &m) != 0) {
            fprintf(stderr, "openchimed: cannot read %s\n", path);
            rc = 1;
            continue;
        }
        printf("== %s\n", name);
        if (oc_recognizer_list_mem(m.p, m.n, stdout, err, sizeof err) != 0) {
            fprintf(stderr, "openchimed: %s: %s\n", path, err);
            rc = 1;
        }
        oc_tts_map_close(&m);
    }
    return rc;
}

int oc_recognizer_pack_cli(int argc, char **argv) {
    if (argc == 4 && strcmp(argv[2], "--list") == 0) return list_dir(argv[3]);
    if (argc != 4) {
        fprintf(stderr, "usage: openchimed --recognizer-pack DIR OUT | --recognizer-pack --list DIR\n");
        return 2;
    }
    char err[600] = "";
    size_t n = 0;
    if (oc_recognizer_pack(argv[2], argv[3], &n, err, sizeof err) != 0) {
        fprintf(stderr, "openchimed: %s\n", err);
        return 1;
    }
    fprintf(stderr, "openchimed: wrote %s (%zu bytes)\n", argv[3], n);
    return 0;
}
