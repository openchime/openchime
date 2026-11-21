/* Tests for sumkit, the summary inference engine (sumkit/, docs/SUMKIT.md),
 * and the converter that makes its file (daemon/summary_pack.c): a small model
 * of the family, written as Qwen publishes one (sumkit_fixture.h), converted,
 * mapped and checked; the files the converter and sumkit must refuse. */
#include "check.h"
#include "nnkit.h"
#include "sum_core.h"
#include "summary_pack.h"
#include "sumkit.h"
#include "sumkit_fixture.h"
#include "testenv.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int pack(const char *dir, const char *out, int full, char *err, size_t cap) {
    return oc_summary_pack(dir, out, full, NULL, NULL, NULL, NULL, err, cap);
}

static void *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *p = NULL;
    if (len > 0 && posix_memalign(&p, 64, (size_t)len) == 0 && fread(p, 1, (size_t)len, f) != (size_t)len) {
        free(p);
        p = NULL;
    }
    fclose(f);
    *n = (size_t)len;
    return p;
}

/* The bf16 value of tensor `name`'s element i in the fixture's safetensors. */
static float src_value(const char *dir, const char *name, size_t i) {
    char path[1200];
    snprintf(path, sizeof path, "%s/model.safetensors", dir);
    size_t n;
    uint8_t *p = slurp(path, &n);
    if (!p) return NAN;
    uint64_t hl = 0;
    for (int k = 7; k >= 0; k--) hl = hl << 8 | p[k];
    char key[160];
    snprintf(key, sizeof key, "\"%s\":", name);
    char *h = (char *)p + 8, *at = strstr(h, key), *dt = at ? strstr(at, "\"dtype\":\"") : NULL,
         *of = at ? strstr(at, "data_offsets\":[") : NULL;
    float v = NAN;
    if (at && dt && of && (size_t)(of - h) < hl) {
        unsigned long long a = strtoull(of + 15, NULL, 10);
        const uint8_t *d = p + 8 + hl + a;
        if (!strncmp(dt + 9, "F32", 3)) memcpy(&v, d + 4 * i, 4);
        else v = nn_bf16_to_f32((uint16_t)(d[2 * i] | d[2 * i + 1] << 8));
    }
    free(p);
    return v;
}

static int file_has(const char *msg, const char *want) {
    int ok = strstr(msg, want) != NULL;
    if (!ok) printf("    message \"%s\" lacks \"%s\"\n", msg, want);
    return ok;
}

/* The fixture converted both ways: what the file records, what each tensor
 * became, the tokenizer's tables. */
static int convert_checks(const char *dir) {
    char out[1200], full[1200], err[512] = "";
    snprintf(out, sizeof out, "%s/summary.bin", dir);
    snprintf(full, sizeof full, "%s/full.bin", dir);
    int ok = pack(dir, out, 0, err, sizeof err) == 0;
    if (!ok) { printf("    pack: %s\n", err); return 0; }
    ok &= pack(dir, full, 1, err, sizeof err) == 0;
    sk_file *q = sk_open(out, err, sizeof err), *u = sk_open(full, err, sizeof err);
    if (!q || !u) { printf("    open: %s\n", err); sk_close(q); sk_close(u); return 0; }
    const sk_config *c = sk_config_of(q);
    ok &= c->n_layer == SKF_LAYERS && c->n_embd == SKF_EMBD && c->n_ff == SKF_FF && c->n_vocab == SKF_VOCAB;
    ok &= c->linear == 1 && c->n_head == 2 && c->n_head_kv == 1 && c->head_dim == 64 && c->rot_dims == 16;
    ok &= c->lin_k_heads == 2 && c->lin_v_heads == 4 && c->lin_k_dim == 32 && c->lin_v_dim == 32 && c->conv_kernel == 4;
    ok &= c->rms_eps == 1e-6f && c->rope_theta == 1e7f && c->n_tok == 256 + 7 + 5 && c->n_merges == 7;
    ok &= c->n_eog == 2 && sk_full(q) == 0 && sk_full(u) == 1;
    /* The quantisation plan: 6 bits for the embeddings and qkv, 4 for the
     * gate; rows that are not whole blocks stay bfloat16; all bfloat16 in full. */
    ok &= sk_find(q, "embed")->dtype == SK_Q6K && sk_find(q, "L00.qkv")->dtype == SK_Q6K &&
          sk_find(q, "L00.ffn_gate")->dtype == SK_Q4K && sk_find(q, "L00.out")->dtype == SK_BF16 &&
          sk_find(q, "L01.o")->dtype == SK_BF16 && sk_find(u, "embed")->dtype == SK_BF16 &&
          sk_find(u, "L01.q")->dtype == SK_BF16;
    /* What the model's code applies at load: 1 + norm weight (not the gated
     * norm's), -exp(A_log), conv1d's [channels][1][k] as [k][channels]. */
    const float *nw = sk_find(q, "L01.q_norm")->data, *A = sk_find(q, "L00.A")->data,
                *gn = sk_find(q, "L00.gnorm")->data, *cv = sk_find(q, "L00.conv")->data;
    ok &= nw[3] == 1.0f + src_value(dir, "model.language_model.layers.1.self_attn.q_norm.weight", 3);
    ok &= A[2] == -expf(src_value(dir, "model.language_model.layers.0.linear_attn.A_log", 2));
    ok &= gn[5] == src_value(dir, "model.language_model.layers.0.linear_attn.norm.weight", 5);
    ok &= cv[2 * 256 + 7] == src_value(dir, "model.language_model.layers.0.linear_attn.conv1d.weight", 7 * 4 + 2);
    /* The full model is the source's values; the quantised close to them. */
    float row[SKF_EMBD], rowq[SKF_EMBD];
    nn_dequant_row(NN_W_BF16, sk_find(u, "embed")->data, SKF_EMBD, 17, row);
    nn_dequant_row(NN_W_Q6K, sk_find(q, "embed")->data, SKF_EMBD, 17, rowq);
    double e2 = 0, s2 = 0;
    for (int i = 0; i < SKF_EMBD; i++) {
        ok &= row[i] == src_value(dir, "model.language_model.embed_tokens.weight", (size_t)17 * SKF_EMBD + i) || i > 3;
        e2 += (row[i] - rowq[i]) * (row[i] - rowq[i]);
        s2 += row[i] * row[i];
    }
    ok &= 10 * log10(s2 / e2) > 30;                    /* 6 bits: well over 30 dB */
    float g[SKF_EMBD], gq[SKF_EMBD];
    nn_dequant_row(NN_W_BF16, sk_find(u, "L00.ffn_gate")->data, SKF_EMBD, 3, g);
    nn_dequant_row(NN_W_Q4K, sk_find(q, "L00.ffn_gate")->data, SKF_EMBD, 3, gq);
    e2 = s2 = 0;
    for (int i = 0; i < SKF_EMBD; i++) { e2 += (g[i] - gq[i]) * (g[i] - gq[i]); s2 += g[i] * g[i]; }
    ok &= 10 * log10(s2 / e2) > 15;                    /* 4 bits */
    /* The tokenizer: byte b is token b as its own byte; "hello" is a merge's
     * result; the added tokens are there whole; the end tokens are two of them. */
    const uint8_t *tb = sk_find(q, "tok.bytes")->data;
    const int32_t *to = sk_find(q, "tok.offs")->data, *tm = sk_find(q, "tok.merges")->data;
    ok &= to[0x20] + 1 == to[0x21] && tb[to[0x20]] == 0x20 && tb[to[0xc4]] == 0xc4;
    ok &= to[259 + 1] - to[259] == 5 && !memcmp(tb + to[259], "hello", 5);
    ok &= tm[3 * 3] == 258 && tm[3 * 3 + 1] == 'o' && tm[3 * 3 + 2] == 259;
    ok &= to[264] < to[265] && !memcmp(tb + to[265], "<|im_end|>", 10);
    ok &= (c->eog[0] == 263 && c->eog[1] == 265) || (c->eog[0] == 265 && c->eog[1] == 263);
    /* The same inputs, the same bytes. */
    char again[1200];
    snprintf(again, sizeof again, "%s/again.bin", dir);
    ok &= pack(dir, again, 0, err, sizeof err) == 0;
    size_t n1, n2;
    void *a = slurp(out, &n1), *b = slurp(again, &n2);
    ok &= a && b && n1 == n2 && !memcmp(a, b, n1);
    /* The input hashes are recorded; a damaged file is refused. */
    ok &= sk_input_sha(q, 2)[0] != 0 || sk_input_sha(q, 2)[1] != 0;
    if (a) {
        char e2b[256];
        uint8_t *m = a;
        m[0] ^= 1;
        sk_file *x = sk_open_mem(m, n1, e2b, sizeof e2b);
        ok &= !x && file_has(e2b, "not a version");
        m[0] ^= 1;
        x = sk_open_mem(m, n1 - 64, e2b, sizeof e2b);
        ok &= !x && file_has(e2b, "disagrees with its length");
        m[SK_HEADER + 48] = 9;                       /* the first entry's type */
        x = sk_open_mem(m, n1, e2b, sizeof e2b);
        ok &= !x && file_has(e2b, "unknown element type");
    }
    free(a);
    free(b);
    sk_close(q);
    sk_close(u);
    return ok;
}

/* Models that are not the one the configuration describes, each refused with
 * the first difference named. */
static int refusal_checks(void) {
    int ok = 1;
    static const struct { int flags; const char *want; } cases[] = {
        { SKF_MISSING, "has no model.language_model.layers.0.linear_attn.out_proj.weight" },
        { SKF_EXTRA, "model.language_model.layers.1.self_attn.extra.weight, which the configuration does not imply" },
        { SKF_SHAPE, "layers.0.mlp.down_proj.weight is not the shape" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        char dir[512], out[1200], err[512] = "";
        if (oc_test_mkdtemp("sumkit-bad", dir, sizeof dir) || oc_test_sumkit_model(dir, cases[i].flags)) return 0;
        snprintf(out, sizeof out, "%s/summary.bin", dir);
        struct stat st;
        ok &= pack(dir, out, 0, err, sizeof err) != 0 && file_has(err, cases[i].want) && stat(out, &st) != 0;
        oc_test_rmtree(dir);
    }
    /* A configuration of another model family. */
    char dir[512], out[1200], err[512] = "";
    if (oc_test_mkdtemp("sumkit-bad", dir, sizeof dir) || oc_test_sumkit_model(dir, SKF_NONE)) return 0;
    char cfg[1200];
    snprintf(cfg, sizeof cfg, "%s/config.json", dir);
    FILE *f = fopen(cfg, "w");
    if (f) { fputs("{\"model_type\":\"mistral\",\"hidden_size\":256}", f); fclose(f); }
    snprintf(out, sizeof out, "%s/summary.bin", dir);
    ok &= pack(dir, out, 0, err, sizeof err) != 0 && file_has(err, "not a Qwen3.5 text model");
    oc_test_rmtree(dir);
    return ok;
}

/* ---- the forward pass, against the model written out naively -------------------
 * In double, one token at a time, from the unquantised file's own values,
 * straight from the definitions (sumkit.h, lm_model.c's comment): the cache at
 * full precision, so sumkit's 8-bit cache is the only difference but for the
 * order of sums. */

typedef struct { const sk_file *f; const sk_config *c; } naive;

static double *dvec(const sk_file *f, const char *name, size_t *n) {
    const sk_tensor *t = sk_find(f, name);
    size_t rows = t->ndim == 2 ? t->dims[0] : 1, cols = t->ndim == 2 ? t->dims[1] : t->dims[0];
    double *v = malloc(rows * cols * sizeof *v);
    float *row = malloc(cols * sizeof *row);
    for (size_t r = 0; r < rows; r++) {
        nn_dequant_row((nn_wtype)t->dtype, t->data, (int)cols, (int)r, row);
        for (size_t i = 0; i < cols; i++) v[r * cols + i] = row[i];
    }
    free(row);
    if (n) *n = rows * cols;
    return v;
}
static double *dnm(const sk_file *f, unsigned l, const char *s) {
    char n[48];
    snprintf(n, sizeof n, "L%02u.%s", l, s);
    return dvec(f, n, NULL);
}
/* y[r] = sum_i W[r][i] x[i] */
static void dmv(const double *W, const double *x, int rows, int cols, double *y) {
    for (int r = 0; r < rows; r++) {
        double s = 0;
        for (int i = 0; i < cols; i++) s += W[(size_t)r * cols + i] * x[i];
        y[r] = s;
    }
}
static void drms(double *x, int n, const double *g, double eps) {
    double ss = 0;
    for (int i = 0; i < n; i++) ss += x[i] * x[i];
    double sc = 1 / sqrt(ss / n + eps);
    for (int i = 0; i < n; i++) x[i] *= sc * g[i];
}
static double dsig(double x) { return 1 / (1 + exp(-x)); }

/* Logits of each of `n` tokens, the model run from an empty context. */
static void naive_run(const sk_file *f, const int32_t *tok, int n, double *logits) {
    const sk_config *c = sk_config_of(f);
    int E = (int)c->n_embd, F = (int)c->n_ff, V = (int)c->n_vocab, L = (int)c->n_layer;
    int hk = (int)c->lin_k_heads, hv = (int)c->lin_v_heads, dk = (int)c->lin_k_dim, dv = (int)c->lin_v_dim;
    int kd = hk * dk, vd = hv * dv, cd = 2 * kd + vd, K = (int)c->conv_kernel;
    int nh = (int)c->n_head, nkv = (int)c->n_head_kv, hd = (int)c->head_dim, rot = (int)c->rot_dims;
    double *emb = dvec(f, "embed", NULL), *fn = dvec(f, "norm", NULL);
    double **conv_hist = calloc((size_t)L, sizeof *conv_hist), **S = calloc((size_t)L, sizeof *S);
    double **kc = calloc((size_t)L, sizeof *kc), **vc = calloc((size_t)L, sizeof *vc);
    for (int l = 0; l < L; l++) {
        conv_hist[l] = calloc((size_t)(K - 1) * cd, sizeof **conv_hist);
        S[l] = calloc((size_t)hv * dk * dv, sizeof **S);
        kc[l] = calloc((size_t)n * nkv * hd, sizeof **kc);
        vc[l] = calloc((size_t)n * nkv * hd, sizeof **vc);
    }
    double *x = malloc((size_t)E * sizeof *x), *h = malloc((size_t)E * sizeof *h), *y = malloc(8192 * sizeof *y),
           *u = malloc(8192 * sizeof *u), *w = malloc(8192 * sizeof *w);
    for (int t = 0; t < n; t++) {
        for (int i = 0; i < E; i++) x[i] = emb[(size_t)tok[t] * E + i];
        for (int l = 0; l < L; l++) {
            double *in_n = dnm(f, (unsigned)l, "in_norm"), *post_n = dnm(f, (unsigned)l, "post_norm");
            memcpy(h, x, (size_t)E * sizeof *h);
            drms(h, E, in_n, c->rms_eps);
            if (c->linear >> l & 1) {
                double *Wqkv = dnm(f, (unsigned)l, "qkv"), *Wz = dnm(f, (unsigned)l, "z"), *Wa = dnm(f, (unsigned)l, "a"),
                       *Wb = dnm(f, (unsigned)l, "b"), *cw = dnm(f, (unsigned)l, "conv"), *dt = dnm(f, (unsigned)l, "dt"),
                       *A = dnm(f, (unsigned)l, "A"), *gn = dnm(f, (unsigned)l, "gnorm"), *Wo = dnm(f, (unsigned)l, "out");
                double *qkv = y, *z = u, a[64], b[64];
                dmv(Wqkv, h, cd, E, qkv);
                dmv(Wz, h, vd, E, z);
                dmv(Wa, h, hv, E, a);
                dmv(Wb, h, hv, E, b);
                /* the causal convolution over the last K inputs, then SiLU */
                double *hist = conv_hist[l], co[8192];
                for (int ch = 0; ch < cd; ch++) {
                    double s = cw[(size_t)(K - 1) * cd + ch] * qkv[ch];
                    for (int j = 0; j < K - 1; j++) s += cw[(size_t)j * cd + ch] * hist[(size_t)j * cd + ch];
                    co[ch] = s * dsig(s);
                }
                for (int j = 0; j < K - 2; j++) memcpy(hist + (size_t)j * cd, hist + (size_t)(j + 1) * cd, (size_t)cd * sizeof *hist);
                if (K > 1) memcpy(hist + (size_t)(K - 2) * cd, qkv, (size_t)cd * sizeof *hist);
                for (int hh = 0; hh < 2 * hk; hh++) {               /* L2 norm of q's and k's heads */
                    double ss = 0;
                    for (int i = 0; i < dk; i++) ss += co[hh * dk + i] * co[hh * dk + i];
                    double sc = 1 / fmax(sqrt(ss), c->rms_eps);
                    for (int i = 0; i < dk; i++) co[hh * dk + i] *= sc;
                }
                double *o = w;
                for (int vh = 0; vh < hv; vh++) {
                    int kh = vh / (hv / hk);
                    const double *q = co + kh * dk, *k = co + kd + kh * dk, *v = co + 2 * kd + vh * dv;
                    double ap = a[vh] + dt[vh], g = (ap > 20 ? ap : log1p(exp(ap))) * A[vh], bt = dsig(b[vh]);
                    double *M = S[l] + (size_t)vh * dk * dv, uu[256];
                    for (int i = 0; i < dk * dv; i++) M[i] *= exp(g);
                    for (int j = 0; j < dv; j++) {
                        double s = 0;
                        for (int i = 0; i < dk; i++) s += M[i * dv + j] * k[i];
                        uu[j] = (v[j] - s) * bt;
                    }
                    for (int i = 0; i < dk; i++) for (int j = 0; j < dv; j++) M[i * dv + j] += k[i] * uu[j];
                    for (int j = 0; j < dv; j++) {
                        double s = 0;
                        for (int i = 0; i < dk; i++) s += M[i * dv + j] * q[i] / sqrt((double)dk);
                        o[vh * dv + j] = s;
                    }
                    drms(o + vh * dv, dv, gn, c->rms_eps);
                    for (int j = 0; j < dv; j++) { double zz = z[vh * dv + j]; o[vh * dv + j] *= zz * dsig(zz); }
                }
                dmv(Wo, o, E, vd, h);
                free(Wqkv); free(Wz); free(Wa); free(Wb); free(cw); free(dt); free(A); free(gn); free(Wo);
            } else {
                double *Wq = dnm(f, (unsigned)l, "q"), *Wk = dnm(f, (unsigned)l, "k"), *Wv = dnm(f, (unsigned)l, "v"),
                       *Wo = dnm(f, (unsigned)l, "o"), *qn = dnm(f, (unsigned)l, "q_norm"), *kn = dnm(f, (unsigned)l, "k_norm");
                double *qg = y, *kk = kc[l] + (size_t)t * nkv * hd, *vv = vc[l] + (size_t)t * nkv * hd, *o = w;
                dmv(Wq, h, 2 * nh * hd, E, qg);
                dmv(Wk, h, nkv * hd, E, kk);
                dmv(Wv, h, nkv * hd, E, vv);
                for (int hh = 0; hh < nh + nkv; hh++) {
                    double *r = hh < nh ? qg + hh * 2 * hd : kk + (hh - nh) * hd;
                    drms(r, hd, hh < nh ? qn : kn, c->rms_eps);
                    for (int i = 0; i < rot / 2; i++) {
                        double fr = t * pow((double)c->rope_theta, -2.0 * i / rot), p = r[i], q2 = r[i + rot / 2];
                        r[i] = p * cos(fr) - q2 * sin(fr);
                        r[i + rot / 2] = q2 * cos(fr) + p * sin(fr);
                    }
                }
                for (int hh = 0; hh < nh; hh++) {
                    int g = hh / (nh / nkv);
                    double sc[512], mx = -1e300, sum = 0;
                    for (int j = 0; j <= t; j++) {
                        double s = 0;
                        for (int i = 0; i < hd; i++) s += qg[hh * 2 * hd + i] * kc[l][((size_t)j * nkv + g) * hd + i];
                        sc[j] = s / sqrt((double)hd);
                        if (sc[j] > mx) mx = sc[j];
                    }
                    for (int j = 0; j <= t; j++) { sc[j] = exp(sc[j] - mx); sum += sc[j]; }
                    for (int i = 0; i < hd; i++) {
                        double s = 0;
                        for (int j = 0; j <= t; j++) s += sc[j] / sum * vc[l][((size_t)j * nkv + g) * hd + i];
                        o[hh * hd + i] = s * dsig(qg[hh * 2 * hd + hd + i]);
                    }
                }
                dmv(Wo, o, E, nh * hd, h);
                free(Wq); free(Wk); free(Wv); free(Wo); free(qn); free(kn);
            }
            for (int i = 0; i < E; i++) x[i] += h[i];
            memcpy(h, x, (size_t)E * sizeof *h);
            drms(h, E, post_n, c->rms_eps);
            double *Wg = dnm(f, (unsigned)l, "ffn_gate"), *Wu = dnm(f, (unsigned)l, "ffn_up"), *Wd = dnm(f, (unsigned)l, "ffn_down");
            dmv(Wg, h, F, E, y);
            dmv(Wu, h, F, E, u);
            for (int i = 0; i < F; i++) u[i] *= y[i] * dsig(y[i]);
            dmv(Wd, u, E, F, h);
            for (int i = 0; i < E; i++) x[i] += h[i];
            free(Wg); free(Wu); free(Wd); free(in_n); free(post_n);
        }
        memcpy(h, x, (size_t)E * sizeof *h);
        drms(h, E, fn, c->rms_eps);
        dmv(emb, h, V, E, logits + (size_t)t * V);
    }
    for (int l = 0; l < L; l++) { free(conv_hist[l]); free(S[l]); free(kc[l]); free(vc[l]); }
    free(conv_hist); free(S); free(kc); free(vc); free(emb); free(fn); free(x); free(h); free(y); free(u); free(w);
}

static double snr_db(const double *ref, const float *got, size_t n) {
    double s = 0, e = 0;
    for (size_t i = 0; i < n; i++) { s += ref[i] * ref[i]; e += (ref[i] - got[i]) * (ref[i] - got[i]); }
    return 10 * log10(s / (e > 0 ? e : 1e-300));
}

/* sumkit's run of the fixture against the naive one; and the same tokens read
 * all at once, a few at a time and one at a time give the same bits. */
static int forward_checks(const char *dir) {
    char full[1200], quant[1200], err[512];
    snprintf(full, sizeof full, "%s/full.bin", dir);
    snprintf(quant, sizeof quant, "%s/summary.bin", dir);
    sk_file *f = sk_open(full, err, sizeof err), *q = sk_open(quant, err, sizeof err);
    if (!f || !q) { printf("    %s\n", err); return 0; }
    enum { N = 9 };
    int32_t tok[N] = { 259, 260, 32, 104, 101, 300, 261, 7, 263 };
    int V = SKF_VOCAB, ok = 1;
    double *ref = malloc((size_t)N * V * sizeof *ref);
    float *all = malloc((size_t)N * V * sizeof *all), *step = malloc((size_t)N * V * sizeof *step),
          *mix = malloc((size_t)N * V * sizeof *mix);
    naive_run(f, tok, N, ref);
    sk_model *m = sk_model_new(f, 64, err, sizeof err);
    ok &= m && sk_model_eval(m, tok, N, all, 1, err, sizeof err) == 0 && sk_model_used(m) == N;
    for (int t = 0; t < N && ok; t++) {
        double s = snr_db(ref + (size_t)t * V, all + (size_t)t * V, (size_t)V);
        if (s < 30) { ok = 0; printf("    token %d: %.1f dB from the naive model\n", t, s); }
    }
    sk_model_reset(m);
    for (int t = 0; t < N && ok; t++) ok &= sk_model_eval(m, tok + t, 1, step + (size_t)t * V, 1, err, sizeof err) == 0;
    sk_model_reset(m);
    ok &= sk_model_eval(m, tok, 4, mix, 1, err, sizeof err) == 0 && sk_model_eval(m, tok + 4, N - 4, mix + 4 * V, 1, err, sizeof err) == 0;
    ok &= !memcmp(all, step, (size_t)N * V * sizeof *all) && !memcmp(all, mix, (size_t)N * V * sizeof *all);
    /* the last token's logits alone are the same as among all of them */
    float *last = malloc((size_t)V * sizeof *last);
    sk_model_reset(m);
    ok &= sk_model_eval(m, tok, N, last, 0, err, sizeof err) == 0 && !memcmp(last, all + (size_t)(N - 1) * V, (size_t)V * sizeof *last);
    /* a full context and a token outside the vocabulary are refused */
    ok &= sk_model_eval(m, tok, 64 - N + 1, NULL, 0, err, sizeof err) != 0 && file_has(err, "do not fit");
    int32_t badt = SKF_VOCAB;
    ok &= sk_model_eval(m, &badt, 1, NULL, 0, err, sizeof err) != 0 && file_has(err, "outside the vocabulary");
    sk_model_free(m);
    /* the quantised file runs, close to the unquantised */
    sk_model *mq = sk_model_new(q, 64, err, sizeof err);
    ok &= mq && sk_model_eval(mq, tok, N, all, 1, err, sizeof err) == 0;
    for (int t = 0; t < N && ok; t++) {
        double s = snr_db(ref + (size_t)t * V, all + (size_t)t * V, (size_t)V);
        if (s < 10) { ok = 0; printf("    quantised token %d: %.1f dB\n", t, s); }
    }
    sk_model_free(mq);
    free(ref); free(all); free(step); free(mix); free(last);
    sk_close(f);
    sk_close(q);
    return ok;
}

/* ---- the tokenizer ------------------------------------------------------------------
 * The fixture's: bytes, merges, added tokens. The real one's: the reference's
 * ids for the texts in tests/data/summary/tokens.txt and for the prompts, from
 * the tokenizer's tables (tests/data/summary/tokenizer.bin, extracted by
 * `sum_check tokenizer`). */

static int ids_are(const int32_t *got, long n, const int32_t *want, long wn) {
    if (n != wn) return 0;
    for (long i = 0; i < n; i++) if (got[i] != want[i]) return 0;
    return 1;
}

static int fixture_tokenizer_checks(const char *dir) {
    char path[1200], err[512];
    snprintf(path, sizeof path, "%s/summary.bin", dir);
    sk_file *f = sk_open(path, err, sizeof err);
    sk_tokenizer *t = f ? sk_tokenizer_new(f, err, sizeof err) : NULL;
    if (!t) { printf("    %s\n", err); sk_close(f); return 0; }
    int ok = 1;
    int32_t *ids;
    long n = sk_tokenize(t, "hello world<|im_end|>hi", &ids);
    /* "hello" merges whole; " world" is " w"+"or"+"ld" -> "Ġwor","l","d"; the
     * added token whole; "hi" two bytes */
    int32_t want[] = { 259, 262, 'l', 'd', 265, 'h', 'i' };
    ok &= ids_are(ids, n, want, 7);
    char back[64];
    ok &= sk_detok(t, ids, (size_t)n, back, sizeof back) == 23 && !strcmp(back, "hello world<|im_end|>hi");
    free(ids);
    sk_tokenizer_free(t);
    sk_close(f);
    return ok;
}

static char *unescape(const char *s) {
    char *o = malloc(strlen(s) + 1), *w = o;
    for (; *s; s++) {
        if (*s == '\\' && s[1]) {
            s++;
            *w++ = *s == 'n' ? '\n' : *s == 't' ? '\t' : *s;
        } else *w++ = *s;
    }
    *w = 0;
    return o;
}

static int32_t *read_id_lines(const char *path, long *n) {
    *n = 0;
    FILE *fp = fopen(path, "r");
    if (!fp) return NULL;
    long cap = 4096, x;
    int32_t *v = malloc((size_t)cap * sizeof *v);
    while (fscanf(fp, "%ld", &x) == 1) {
        if (*n == cap) v = realloc(v, (size_t)(cap *= 2) * sizeof *v);
        v[(*n)++] = (int32_t)x;
    }
    fclose(fp);
    return v;
}

static int real_tokenizer_checks(void) {
    char root[1024], path[1200], err[512];
    if (!oc_test_tree_path("tests/data/summary", root, sizeof root)) { printf("    no tests/data/summary\n"); return 0; }
    snprintf(path, sizeof path, "%s/tokenizer.bin", root);
    sk_file *f = sk_open_tokenizer(path, err, sizeof err);
    sk_tokenizer *t = f ? sk_tokenizer_new(f, err, sizeof err) : NULL;
    if (!t) { printf("    %s\n", err); sk_close(f); return 0; }
    int ok = 1, cases = 0;
    snprintf(path, sizeof path, "%s/tokens.txt", root);
    FILE *tf = fopen(path, "r");
    char line[8192];
    while (tf && fgets(line, sizeof line, tf)) {
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        size_t l = strlen(tab + 1);
        if (l && tab[l] == '\n') tab[l] = 0;
        int32_t want[1024];
        long wn = 0;
        for (char *p = line; *p && wn < 1024;) { want[wn++] = (int32_t)strtol(p, &p, 10); if (*p == ',') p++; }
        char *text = unescape(tab + 1), back[8192];
        int32_t *ids;
        long n = sk_tokenize(t, text, &ids);
        int same = ids_are(ids, n, want, wn);
        if (!same) printf("    tokens of \"%s\" differ from the reference's\n", tab + 1);
        ok &= same && sk_detok(t, ids, (size_t)n, back, sizeof back) == (long)strlen(text) && !strcmp(back, text);
        cases++;
        free(ids);
        free(text);
    }
    if (tf) fclose(tf);
    ok &= cases >= 9;
    /* The prompts, templated as the engine templates them. */
    for (int p = 1; p <= 3; p++) {
        char name[16], *sys, *user;
        size_t sn, un;
        snprintf(name, sizeof name, "p%02d", p);
        snprintf(path, sizeof path, "%s/%s.system", root, name);
        sys = slurp(path, &sn);
        snprintf(path, sizeof path, "%s/%s.user", root, name);
        user = slurp(path, &un);
        if (!sys || !user) { ok = 0; free(sys); free(user); continue; }
        char *s0 = calloc(1, sn + 1), *u0 = calloc(1, un + 1);
        memcpy(s0, sys, sn);
        memcpy(u0, user, un);
        char *prompt = sk_chat_prompt(s0, u0);
        snprintf(path, sizeof path, "%s/%s.prompt", root, name);
        long wn = 0;
        int32_t *want = read_id_lines(path, &wn), *ids;
        long n = prompt ? sk_tokenize(t, prompt, &ids) : -1;
        int same = n > 0 && ids_are(ids, n, want, wn);
        if (!same) printf("    %s's prompt tokens differ from the reference's (%ld, %ld)\n", name, n, wn);
        ok &= same;
        if (n > 0) free(ids);
        free(want); free(prompt); free(s0); free(u0); free(sys); free(user);
    }
    sk_tokenizer_free(t);
    sk_close(f);
    return ok;
}

/* ---- grammars and the choice of tokens -------------------------------------------
 * Every grammar the daemon's prompt builders write parses; what the subset does
 * not have is refused; answers chosen under a grammar stay inside it; DRY
 * against a naive version written from its definition. */

/* The grammars daemon/sum_core.c writes, over lines of each size it meets. */
static int builder_grammar_checks(void) {
    int ok = 1;
    char err[256];
    oc_sum_lines l = { 0 };
    for (int i = 0; i < 40; i++) {
        int64_t id = i + 1;
        oc_sum_lines_add(&l, "Rosa: the invoice is 10,000 and it is late", i % 3 == 1, NULL, &id, 1);
        if (i == 0 || i == 4 || i == 39) {
            oc_sum_buf pr = { 0 }, gr = { 0 };
            ok &= oc_sum_notes_prompt("Messages from #x.", &l, oc_sum_notes_for(oc_sum_lines_tokens(&l), l.n), &pr, &gr) == 0;
            sk_grammar *g = sk_grammar_new(gr.p, err, sizeof err);
            if (!g) printf("    notes grammar: %s\n", err);
            ok &= g != NULL;
            sk_grammar_free(g);
            oc_sum_buf_free(&pr); oc_sum_buf_free(&gr);
            size_t sizes[] = { 12, 300, 4000, 20000 };
            for (size_t k = 0; k < 4; k++) {
                oc_sum_caps c;
                oc_sum_caps_for(sizes[k], &c);
                ok &= oc_sum_final_prompt("Messages from #x.", &l, &c, &pr, &gr) == 0;
                g = sk_grammar_new(gr.p, err, sizeof err);
                if (!g) printf("    summary grammar: %s\n", err);
                ok &= g != NULL;
                sk_grammar_free(g);
                oc_sum_buf_free(&pr); oc_sum_buf_free(&gr);
            }
        }
    }
    oc_sum_buf pr = { 0 }, gr = { 0 };
    ok &= oc_sum_rewrite_prompt("[3] The release moved to Friday because the review found a bug.", 8, &pr, &gr) == 0;
    sk_grammar *g = gr.p ? sk_grammar_new(gr.p, err, sizeof err) : NULL;
    ok &= gr.p == NULL || g != NULL;
    sk_grammar_free(g);
    oc_sum_buf_free(&pr); oc_sum_buf_free(&gr);
    oc_sum_lines_free(&l);
    /* What the subset does not have, each refused with the reason. */
    static const struct { const char *g, *want; } bad[] = {
        { "root ::= x\n", "a rule used but not defined" },
        { "root ::= [a-z\n", "does not end" },
        { "root ::= \"a\" !\n", "do not have" },
        { "start ::= \"a\"\n", "no root rule" },
        { "root ::= \"a\"\nroot ::= \"b\"\n", "defined twice" },
        { "root ::= \"\\q\"\n", "escape" },
        { "root ::= \"a\"{3,1}\n", "counts are wrong" },
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        sk_grammar *b = sk_grammar_new(bad[i].g, err, sizeof err);
        ok &= !b && file_has(err, "does not parse") && file_has(err, bad[i].want);
        sk_grammar_free(b);
    }
    return ok;
}

/* A grammar matched by hand: what it allows and refuses, character by
 * character and token by token, and where it can end. */
static int grammar_match_checks(void) {
    char err[256];
    sk_grammar *g = sk_grammar_new("root ::= item{1,2}\nitem ::= \"- [\" id \"] \" [^\\n]{2,4} \"\\n\"\nid ::= \"1\" | \"2\" | \"12\"\n",
                                   err, sizeof err);
    if (!g) { printf("    %s\n", err); return 0; }
    int ok = 1;
    ok &= sk_grammar_allows(g, "- [1", 4) && sk_grammar_allows(g, "- [12] ab", 9) && !sk_grammar_allows(g, "- [3", 4);
    ok &= !sk_grammar_allows(g, "x", 1) && !sk_grammar_accepting(g);
    ok &= sk_grammar_accept(g, "- [12] ", 7) == 0;
    ok &= !sk_grammar_allows(g, "a\n", 2);                     /* fewer than 2 */
    ok &= sk_grammar_allows(g, "é\xc3", 3);                     /* a token ending inside a character */
    ok &= sk_grammar_accept(g, "ab", 2) == 0 && sk_grammar_allows(g, "cd\n", 3) && !sk_grammar_allows(g, "cde\n", 4);
    ok &= sk_grammar_accept(g, "\n", 1) == 0 && sk_grammar_accepting(g);
    ok &= sk_grammar_accept(g, "- [2] xyz\n", 10) == 0 && sk_grammar_accepting(g);
    ok &= !sk_grammar_allows(g, "-", 1) && sk_grammar_accept(g, "-", 1) == -1;   /* at most 2 items */
    sk_grammar_reset(g);
    ok &= !sk_grammar_accepting(g) && sk_grammar_accept(g, "- [1] é\xc3", 9) == 0 && sk_grammar_accept(g, "\xa9\n", 2) == 0 &&
          sk_grammar_accepting(g);
    sk_grammar_free(g);
    return ok;
}

/* Answers chosen under a grammar, from random logits over the real
 * vocabulary: every token taken is one the grammar allows, the answer ends only
 * where it can, and what is written reads back as notes. */
static int forced_answer_checks(void) {
    char root[1024], path[1200], err[256];
    oc_test_tree_path("tests/data/summary", root, sizeof root);
    snprintf(path, sizeof path, "%s/tokenizer.bin", root);
    sk_file *f = sk_open_tokenizer(path, err, sizeof err);
    sk_tokenizer *t = f ? sk_tokenizer_new(f, err, sizeof err) : NULL;
    if (!t) { printf("    %s\n", err); sk_close(f); return 0; }
    const sk_config *c = sk_config_of(f);
    sk_sampler *s = sk_sampler_new(t, c, 0.8f, 1.75f, 2, ":\"*");
    oc_sum_lines l = { 0 };
    for (int i = 0; i < 6; i++) {
        int64_t id = i + 1;
        oc_sum_lines_add(&l, "Dev: the backup took 3 hours", 0, NULL, &id, 1);
    }
    oc_sum_buf pr = { 0 }, gr = { 0 };
    oc_sum_notes_prompt("Messages from #ops.", &l, 3, &pr, &gr);
    sk_grammar *g = sk_grammar_new(gr.p, err, sizeof err);
    float *lg = malloc((size_t)c->n_vocab * sizeof *lg);
    int ok = s && g && lg, ended = 0;
    uint32_t seed = 12345;
    char text[8192];
    size_t tn = 0;
    for (int run = 0; run < 3 && ok; run++) {
        sk_grammar_reset(g);
        sk_sampler_reset(s);
        tn = 0;
        for (int step = 0; step < 400 && ok; step++) {
            for (uint32_t i = 0; i < c->n_vocab; i++) {
                seed = seed * 1664525u + 1013904223u;
                lg[i] = (float)(seed >> 8) / 16777216.0f * 10.0f;
            }
            /* now and then the end token is the model's favourite */
            if (step > 20 && step % 7 == 0) lg[c->eog[0]] = 100.0f;
            int32_t tok = sk_sampler_pick(s, lg, g);
            if (tok < 0) { ok = 0; break; }
            int eog = 0;
            for (uint32_t e = 0; e < c->n_eog; e++) eog |= (int32_t)c->eog[e] == tok;
            if (eog) { ok &= sk_grammar_accepting(g); ended++; break; }
            size_t len;
            const char *b = sk_token_bytes(t, tok, &len);
            ok &= sk_grammar_accept(g, b, len) == 0 && tn + len < sizeof text;
            if (ok) { memcpy(text + tn, b, len); tn += len; }
            sk_sampler_accept(s, tok);
        }
        text[tn] = 0;
        /* every line a note: "- [n] kind | tag | fact" */
        for (char *line = text; ok && *line; ) {
            char *nl = strchr(line, '\n');
            if (!nl) break;
            ok &= !strncmp(line, "- [", 3) && strstr(line, " | ") != NULL;
            line = nl + 1;
        }
        oc_sum_buf out = { 0 };
        int dropped = 0;
        ok &= oc_sum_parse_notes(text, &l, &out, &dropped, 3) >= 0;
        oc_sum_buf_free(&out);
    }
    ok &= ended >= 1;
    free(lg);
    sk_grammar_free(g);
    sk_sampler_free(s);
    oc_sum_buf_free(&pr); oc_sum_buf_free(&gr);
    oc_sum_lines_free(&l);
    sk_tokenizer_free(t);
    sk_close(f);
    return ok;
}

/* DRY written from its definition: for each token, the longest repeat it would
 * extend -- the longest k such that the last k tokens, none a breaker, also
 * occur ending just before an earlier occurrence of the token. */
static int dry_checks(void) {
    char root[1024], path[1200], err[256];
    oc_test_tree_path("tests/data/summary", root, sizeof root);
    snprintf(path, sizeof path, "%s/tokenizer.bin", root);
    sk_file *f = sk_open_tokenizer(path, err, sizeof err);
    sk_tokenizer *t = f ? sk_tokenizer_new(f, err, sizeof err) : NULL;
    if (!t) { sk_close(f); return 0; }
    const sk_config *c = sk_config_of(f);
    sk_sampler *s = sk_sampler_new(t, c, 0.8f, 1.75f, 2, ":\"*");
    int32_t colon = -1;
    for (uint32_t i = 0; i < c->n_tok && colon < 0; i++) {
        size_t len;
        const char *b = sk_token_bytes(t, (int32_t)i, &len);
        if (len == 1 && b[0] == ':') colon = (int32_t)i;
    }
    int ok = s && colon >= 0;
    uint32_t seed = 99;
    float *lg = malloc((size_t)c->n_vocab * sizeof *lg), *want = malloc((size_t)c->n_vocab * sizeof *want);
    for (int trial = 0; trial < 30 && ok; trial++) {
        int32_t h[80];
        int n = 10 + trial * 2;
        for (int i = 0; i < n; i++) {
            seed = seed * 1103515245u + 12345u;
            h[i] = 100 + (int32_t)((seed >> 16) % 5);            /* a small alphabet: many repeats */
            if (trial % 4 == 3 && i == n / 2) h[i] = colon;       /* and sometimes a breaker */
        }
        sk_sampler_reset(s);
        for (int i = 0; i < n; i++) sk_sampler_accept(s, h[i]);
        for (uint32_t i = 0; i < c->n_vocab; i++) lg[i] = want[i] = 0;
        int limit = n;
        for (int i = n - 1; i >= 0; i--) if (h[i] == colon) { limit = n - 1 - i; break; }
        for (int tok = 100; tok < 105 && limit >= 2 && n > 2; tok++) {
            int best = 0;
            for (int j = 1; j < n; j++) {
                if (h[j] != tok) continue;
                int k = 0;
                while (k < j && k < n - 1 && h[j - 1 - k] == h[n - 1 - k]) k++;
                if (j == n) continue;
                if (k > limit) k = limit;
                if (k > best) best = k;
            }
            if (best >= 2) want[tok] = -0.8f * powf(1.75f, (float)(best - 2));
        }
        sk_sampler_dry(s, lg);
        for (int tok = 100; tok < 105; tok++)
            if (fabsf(lg[tok] - want[tok]) > 1e-5f) { ok = 0; printf("    DRY trial %d token %d: %g, want %g\n", trial, tok, (double)lg[tok], (double)want[tok]); }
    }
    free(lg);
    free(want);
    sk_sampler_free(s);
    sk_tokenizer_free(t);
    sk_close(f);
    return ok;
}

int run_sumkit_tests(void) {
    printf("test_sumkit: the converter and summary.bin on a small model of the family\n");
    char dir[512];
    CHECK(oc_test_mkdtemp("sumkit", dir, sizeof dir) == 0);
    CHECK(oc_test_sumkit_model(dir, SKF_NONE) == 0);
    CHECK(convert_checks(dir));
    CHECK(forward_checks(dir));
    CHECK(fixture_tokenizer_checks(dir));
    oc_test_rmtree(dir);
    CHECK(real_tokenizer_checks());
    CHECK(builder_grammar_checks());
    CHECK(grammar_match_checks());
    CHECK(forced_answer_checks());
    CHECK(dry_checks());
    CHECK(refusal_checks());
    return failures;
}
