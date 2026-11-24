/* The recognizer's forward pass (stt_priv.h, docs/details/STTKIT.md §3): from 16 kHz
 * samples to token ids, on nnkit's kernels.
 *
 * Activations are time-major: a sequence is T rows of its channels, 320 apart
 * unless said otherwise. A head's 40 values are columns h * 40 .. h * 40 + 39
 * of a row. Every quantised product quantises its whole input once, from its
 * own range (one scale and zero point for the tensor), as the published graphs
 * do; the products of q, k and v, and of the memory's keys and values, share
 * one quantisation of their common input, as the graphs share it. */
#include "stt_priv.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define D      320
#define HEADS  8
#define DH     40
#define CHUNK  1280            /* the published frontend's chunk: whole chunks only */
#define FRAME  80
#define CARRY  4               /* rows of zeros before each convolution's input */

static void seterr(char *err, size_t cap, const char *fmt, ...) {
    if (!err || !cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

/* The working memory of one run: every buffer the passes write. */
typedef struct {
    int16_t *q, *wbuf;
    int32_t *acc;
    float *scr;
} work;

/* y = the product of x (rows x k, rows ldx apart) and m, quantised as one
 * tensor, plus m's bias when it has one; y's rows ldy apart. `shared` (may be
 * NULL) is x already quantised, with its parameters in *qp. */
static void product(work *w, const float *x, int rows, int ldx, const rec_qmat *m, float *y, int ldy,
                    const int16_t *shared, const nn_qparam *sqp) {
    nn_qparam qp;
    const int16_t *q = shared;
    if (!q) {
        nn_quantize(x, rows, m->k, ldx, w->q, m->k, &qp);
        q = w->q;
    } else {
        qp = *sqp;
    }
    nn_matmul_qc(q, rows, m->k, m->w, m->zp, m->n, w->acc, m->n, w->wbuf);
    nn_dequant_qc(w->acc, rows, m->n, m->n, qp.scale, m->s, m->b, y, ldy);
}

static void add_rows(float *x, const float *y, size_t n) {
    for (size_t i = 0; i < n; i++) x[i] += y[i];
}

/* ---- the frontend ------------------------------------------------------------ */

/* A convolution of kernel 5 and stride 2 over `rows` input rows of `cin`
 * channels (4 rows of zeros already before them), as the reference computes it
 * on one chunk: the windows laid out channel by channel, tap by tap (its
 * im2col), each output a chain of fused multiply-adds in that order in blocks
 * of 1,024 (the stride its matrix product takes for a chunk's few outputs),
 * then the bias added. w: [cin * 5][cout]. */
static int conv(const float *x, int t_out, int cin, const float *w, const float *bias, int cout, float *y) {
    int k = cin * 5;
    float *col = malloc((size_t)(t_out ? t_out : 1) * k * sizeof *col);
    if (!col) return -1;
    for (int t = 0; t < t_out; t++)
        for (int c = 0; c < cin; c++)
            for (int j = 0; j < 5; j++) col[(size_t)t * k + c * 5 + j] = x[(size_t)(2 * t + j) * cin + c];
    nn_matmul_fma_kb(col, t_out, k, (size_t)k, w, cout, (size_t)cout, y, (size_t)cout, 1024);
    for (int t = 0; t < t_out; t++)
        for (int o = 0; o < cout; o++) y[(size_t)t * cout + o] = y[(size_t)t * cout + o] + bias[o];
    free(col);
    return 0;
}

/* Samples (padded with silence to whole chunks) to feature rows; returns the
 * row count, or -1 out of memory. Every step is the arithmetic of the
 * reference's run of the published graph, to the bit: a frame's mean and
 * variance are its sums as Eigen adds them, the linear layer a chain of fused
 * multiply-adds, the convolutions as conv() says. A last-bit difference here
 * would move a value across a rounding step of the encoder's first
 * quantisation, and from there the whole utterance. */
static int frontend(const recognizer *r, const float *pcm, size_t samples, float **out) {
    size_t padded = (samples + CHUNK - 1) / CHUNK * CHUNK;
    int f0 = (int)(padded / FRAME), f1 = f0 / 2, f2 = f1 / 2;
    float *x = calloc((size_t)f0 * FRAME, sizeof *x);
    float *h = calloc((size_t)(CARRY + f0) * D, sizeof *h);
    float *c1 = calloc((size_t)(CARRY + f1) * 640, sizeof *c1);
    float *feat = malloc((size_t)(f2 ? f2 : 1) * D * sizeof *feat);
    if (!x || !h || !c1 || !feat) { free(x); free(h); free(c1); free(feat); return -1; }
    memcpy(x, pcm, samples * sizeof *pcm);
    /* Each frame to zero mean and unit variance, scaled, compressed. */
    for (int t = 0; t < f0; t++) {
        float *v = x + (size_t)t * FRAME, sq[FRAME];
        float mean = nn_reduce_sum(v, FRAME, (size_t)t * FRAME) / (float)FRAME;
        for (int i = 0; i < FRAME; i++) {
            v[i] = v[i] - mean;
            sq[i] = v[i] * v[i];
        }
        float var = nn_reduce_sum(sq, FRAME, (size_t)t * FRAME) / (float)FRAME;
        float den = sqrtf(var + r->fe_eps);
        for (int i = 0; i < FRAME; i++) {
            float y = v[i] / den;
            v[i] = asinhf(r->fe_comp * y);
        }
    }
    /* The linear layer and SiLU, into the first convolution's input after its
     * rows of zeros; then the two strided convolutions. */
    float *hx = h + (size_t)CARRY * D;
    nn_matmul_fma_kb(x, f0, FRAME, FRAME, r->fe_lin, D, D, hx, D, 256);
    nn_silu(hx, (size_t)f0 * D);
    float *c1x = c1 + (size_t)CARRY * 640;
    int bad = conv(h, f1, D, r->fe_c1w, r->fe_c1b, 640, c1x);
    if (!bad) {
        nn_silu(c1x, (size_t)f1 * 640);
        bad = conv(c1, f2, 640, r->fe_c2w, r->fe_c2b, D, feat);
    }
    free(x);
    free(h);
    free(c1);
    if (bad) { free(feat); return -1; }
    *out = feat;
    return f2;
}

/* Each stack's end, as the graph has it: SkipLayerNormalization of the last
 * residual sum (scale 1, no bias) in its own arithmetic, then the gain. */
static void final_norm(float *x, const float *skip, int rows, const float *gain, float eps) {
    float one[D], zero[D];
    for (int c = 0; c < D; c++) { one[c] = 1.0f; zero[c] = 0.0f; }
    nn_skip_layernorm(x, skip, rows, D, D, D, one, zero, eps);
    for (int i = 0; i < rows; i++)
        for (int c = 0; c < D; c++) x[(size_t)i * D + c] = x[(size_t)i * D + c] * gain[c];
}

/* ---- the encoder ------------------------------------------------------------- */

static int encoder(const recognizer *r, work *w, float *x, int t) {
    float *h = malloc((size_t)t * D * sizeof *h), *q = malloc((size_t)t * D * sizeof *q);
    float *k = malloc((size_t)t * D * sizeof *k), *v = malloc((size_t)t * D * sizeof *v);
    float *a = malloc((size_t)t * D * sizeof *a), *f = malloc((size_t)t * 1280 * sizeof *f);
    int rc = -1;
    if (!h || !q || !k || !v || !a || !f) goto done;
    for (int l = 0; l < REC_LAYERS; l++) {
        const rec_enc_layer *L = &r->enc[l];
        memcpy(h, x, (size_t)t * D * sizeof *h);
        nn_layernorm(h, t, D, D, L->g1, NULL, r->eps);
        nn_qparam qp;
        nn_quantize(h, t, D, D, w->q, D, &qp);
        int16_t *shared = w->q;
        /* product() reuses w->q only when it quantises itself; here the three
         * share it, so it is read before anything quantises again. */
        product(w, h, t, D, &L->q, q, D, shared, &qp);
        product(w, h, t, D, &L->k, k, D, shared, &qp);
        product(w, h, t, D, &L->v, v, D, shared, &qp);
        for (int i = 0; i < t; i++) {
            int lo = i - L->back < 0 ? 0 : i - L->back, hi = i + L->ahead + 1 > t ? t : i + L->ahead + 1;
            for (int hd = 0; hd < HEADS; hd++)
                nn_attend_exact(q + (size_t)i * D + hd * DH, k + hd * DH, D, v + hd * DH, D, t, lo, hi, DH, r->alpha,
                                1, a + (size_t)i * D + hd * DH, w->scr);
        }
        product(w, a, t, D, &L->o, h, D, NULL, NULL);
        add_rows(x, h, (size_t)t * D);
        memcpy(h, x, (size_t)t * D * sizeof *h);
        nn_layernorm(h, t, D, D, L->g2, NULL, r->eps);
        product(w, h, t, D, &L->f1, f, 1280, NULL, NULL);
        nn_gelu_erf(f, (size_t)t * 1280);
        product(w, f, t, 1280, &L->f2, h, D, NULL, NULL);
        if (l < REC_LAYERS - 1) add_rows(x, h, (size_t)t * D);
    }
    final_norm(x, h, t, r->enc_g, r->eps);   /* the last layer's output added in it */
    rc = 0;
done:
    free(h); free(q); free(k); free(v); free(a); free(f);
    return rc;
}

/* ---- the decoder ------------------------------------------------------------- */

typedef struct {
    float *xk[REC_LAYERS], *xv[REC_LAYERS];   /* the memory's keys and values, T x 320 */
    float *sk[REC_LAYERS], *sv[REC_LAYERS];   /* the steps' keys and values, cap x 320 */
    int t;
} dec_state;

/* One step: token `tok` at position `pos`; its logits in `logits`. */
static void step(const recognizer *r, work *w, dec_state *s, int tok, int pos, float *logits) {
    float x[D], h[D], q[D], a[D], o[D], u[2560];
    const uint8_t *e = r->emb + (size_t)tok * D;
    for (int c = 0; c < D; c++) x[c] = (float)(e[c] - r->emb_zp) * r->emb_scale;
    for (int l = 0; l < REC_LAYERS; l++) {
        const rec_dec_layer *L = &r->dec[l];
        float *kr = s->sk[l] + (size_t)pos * D, *vr = s->sv[l] + (size_t)pos * D;
        /* Self-attention over every step so far, this one included. */
        memcpy(h, x, sizeof h);
        nn_layernorm(h, 1, D, D, L->g1, NULL, r->eps);
        nn_qparam qp;
        nn_quantize(h, 1, D, D, w->q, D, &qp);
        int16_t shared[D];
        memcpy(shared, w->q, sizeof shared);
        product(w, h, 1, D, &L->q, q, D, shared, &qp);
        product(w, h, 1, D, &L->k, kr, D, shared, &qp);
        product(w, h, 1, D, &L->v, vr, D, shared, &qp);
        for (int hd = 0; hd < HEADS; hd++) {
            nn_rope_pairs(q + hd * DH, r->cfg[REC_CFG_ROTARY], r->rope, (float)pos);
            nn_rope_pairs(kr + hd * DH, r->cfg[REC_CFG_ROTARY], r->rope, (float)pos);
        }
        for (int hd = 0; hd < HEADS; hd++)
            nn_attend_exact(q + hd * DH, s->sk[l] + hd * DH, D, s->sv[l] + hd * DH, D, pos + 1, 0, pos + 1, DH, r->alpha, 0,
                            a + hd * DH, w->scr);
        product(w, a, 1, D, &L->o, o, D, NULL, NULL);
        add_rows(x, o, D);
        /* Cross-attention over the memory. */
        memcpy(h, x, sizeof h);
        nn_layernorm(h, 1, D, D, L->g2, NULL, r->eps);
        product(w, h, 1, D, &L->cq, q, D, NULL, NULL);
        for (int hd = 0; hd < HEADS; hd++)
            nn_attend_exact(q + hd * DH, s->xk[l] + hd * DH, D, s->xv[l] + hd * DH, D, s->t, 0, s->t, DH, r->alpha, 1,
                            a + hd * DH, w->scr);
        product(w, a, 1, D, &L->co, o, D, NULL, NULL);
        add_rows(x, o, D);
        /* The gated feed-forward layer: the first half times SiLU of the second. */
        memcpy(h, x, sizeof h);
        nn_layernorm(h, 1, D, D, L->g3, NULL, r->eps);
        product(w, h, 1, D, &L->f1, u, 2560, NULL, NULL);
        nn_silu(u + 1280, 1280);
        for (int i = 0; i < 1280; i++) u[i] = u[i] * u[1280 + i];
        product(w, u, 1, 1280, &L->f2, o, D, NULL, NULL);
        if (l < REC_LAYERS - 1) add_rows(x, o, D);
    }
    final_norm(x, o, 1, r->dec_g, r->eps);
    nn_matvec_u8(x, D, r->emb, r->emb_zp, r->emb_scale, r->cfg[REC_CFG_VOCAB], logits);
}

int rec_run(const recognizer *r, const float *pcm, size_t samples, int64_t *ids, size_t cap, size_t *nids,
            float **enc, int *frames, float *logits0, char *err, size_t errcap) {
    *nids = 0;
    if (enc) *enc = NULL;
    if (frames) *frames = 0;
    if (!samples) return 0;
    float *x = NULL, *logits = NULL;
    dec_state s;
    memset(&s, 0, sizeof s);
    work w = { 0 };
    int rc = -1;
    int t = frontend(r, pcm, samples, &x);
    if (t < 0) { seterr(err, errcap, "out of memory"); return -1; }
    if (t > r->cfg[REC_CFG_POSITIONS]) {
        seterr(err, errcap, "an utterance of %d feature rows is longer than the recognizer's %d", t, r->cfg[REC_CFG_POSITIONS]);
        free(x);
        return -1;
    }
    size_t rows = (size_t)(t > 1 ? t : 1);
    w.q = malloc(rows * 2560 * sizeof *w.q);
    w.wbuf = malloc(NN_WBUF(1, 2560) * sizeof *w.wbuf);
    w.acc = malloc(rows * 2560 * sizeof *w.acc);
    w.scr = malloc((rows + cap + 2) * sizeof *w.scr);
    logits = malloc((size_t)r->cfg[REC_CFG_VOCAB] * sizeof *logits);
    if (!w.q || !w.wbuf || !w.acc || !w.scr || !logits) { seterr(err, errcap, "out of memory"); goto done; }
    if (encoder(r, &w, x, t) != 0) { seterr(err, errcap, "out of memory"); goto done; }
    if (enc) {
        if (!(*enc = malloc(rows * D * sizeof **enc))) { seterr(err, errcap, "out of memory"); goto done; }
        memcpy(*enc, x, (size_t)t * D * sizeof **enc);
    }
    if (frames) *frames = t;
    /* The adapter: the position table added to each row, giving the memory. */
    for (int i = 0; i < t; i++)
        for (int c = 0; c < D; c++) x[(size_t)i * D + c] += (float)(r->pos[(size_t)i * D + c] - r->pos_zp) * r->pos_scale;
    /* The memory's keys and values for every layer, from one quantisation. */
    nn_qparam qp;
    nn_quantize(x, t, D, D, w.q, D, &qp);
    int16_t *mq = malloc((size_t)t * D * sizeof *mq);
    if (!mq) { seterr(err, errcap, "out of memory"); goto done; }
    memcpy(mq, w.q, (size_t)t * D * sizeof *mq);
    s.t = t;
    for (int l = 0; l < REC_LAYERS; l++) {
        s.xk[l] = malloc((size_t)t * D * sizeof(float));
        s.xv[l] = malloc((size_t)t * D * sizeof(float));
        s.sk[l] = malloc((cap + 1) * D * sizeof(float));
        s.sv[l] = malloc((cap + 1) * D * sizeof(float));
        if (!s.xk[l] || !s.xv[l] || !s.sk[l] || !s.sv[l]) { free(mq); seterr(err, errcap, "out of memory"); goto done; }
        product(&w, x, t, D, &r->dec[l].xk, s.xk[l], D, mq, &qp);
        product(&w, x, t, D, &r->dec[l].xv, s.xv[l], D, mq, &qp);
    }
    free(mq);
    /* Greedy decoding from the start token, to the end token or the ceiling. */
    int tok = r->cfg[REC_CFG_BOS];
    for (size_t i = 0; i <= cap; i++) {
        step(r, &w, &s, tok, (int)i, logits);
        if (i == 0 && logits0) memcpy(logits0, logits, (size_t)r->cfg[REC_CFG_VOCAB] * sizeof *logits);
        int best = nn_argmax(logits, r->cfg[REC_CFG_VOCAB]);
        if (best == r->cfg[REC_CFG_EOS] || i == cap) break;
        ids[(*nids)++] = best;
        tok = best;
    }
    rc = 0;
done:
    for (int l = 0; l < REC_LAYERS; l++) { free(s.xk[l]); free(s.xv[l]); free(s.sk[l]); free(s.sv[l]); }
    free(w.q); free(w.wbuf); free(w.acc); free(w.scr);
    free(logits);
    free(x);
    if (rc != 0 && enc) { free(*enc); *enc = NULL; }
    return rc;
}

int recognizer_hear(const recognizer *r, const float *pcm, size_t samples, char **text, char *err, size_t errcap) {
    *text = NULL;
    size_t cap = stt_max_tokens(samples), n = 0;
    int64_t *ids = malloc((cap + 1) * sizeof *ids);
    if (!ids) { seterr(err, errcap, "out of memory"); return -1; }
    if (rec_run(r, pcm, samples, ids, cap, &n, NULL, NULL, NULL, err, errcap) != 0) { free(ids); return -1; }
    size_t tcap = n * 64 + 1;
    char *out = malloc(tcap);
    if (!out || stt_detok(&r->tok, ids, n, out, tcap) < 0) {
        free(out);
        free(ids);
        seterr(err, errcap, "cannot decode the tokens");
        return -1;
    }
    free(ids);
    *text = out;
    return 0;
}
