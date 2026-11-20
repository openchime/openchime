/* The voice model's forward pass (tts_priv.h): from token ids, a voice's style
 * row and the speed to the raw waveform. Its front half gives what the decoder
 * reads -- the text encoder's states expanded to frames, and the pitch (F0) and
 * energy (N) curves -- by way of the context encoder, the prosody predictor's
 * durations and the alignment of tokens to frames; its back half is the decoder,
 * the generator's harmonic source and the generator. voice.bin's names
 * (daemon/voice_pack.c) for the weights; the kernels and their layouts in
 * nnkit/nn_ops.c.
 *
 * The arithmetic is the graph's, in the order ONNX Runtime computes it with its
 * graph optimisations on, to the bit, as far as the inverse STFT at the very
 * end. Two places force it. The durations are rounded: a token's frame count is
 * round(sum of 50 sigmoids / speed), and the references' sums come within 0.003
 * of a rounding boundary, so an error the rest of the model would never show
 * moves a frame. And from the decoder on, every convolution quantises its input
 * to 8 bits with a scale taken from that input: a last-bit difference that moves
 * one value across a rounding boundary changes a whole quantisation step, the
 * blocks after it carry that on, and in the generator's 18 residual blocks such
 * steps multiply -- a 1e-7 difference after its first transposed convolution
 * comes out at 25 dB. So every step is the bits ONNX Runtime computes: its
 * quantisation, its integer products (exact), the scale and bias in its order,
 * its LayerNormalization, SkipLayerNormalization, InstanceNormalization,
 * softmax, FastGelu, sigmoid, tanh, sin and atan (nnkit/nn_ops.c), its float
 * products (fused multiply-adds in its order) and its vectorised ReduceSum. The
 * harmonic source's phase is exact for a third reason: it is a running sum of
 * the pitch, so any difference there grows along the sentence.
 *
 * Where the graph rounds to half precision and ONNX Runtime keeps it: the pitch
 * and energy projections' outputs are rounded to half and widened. Every other
 * half-precision step in the graph (the context layer's output bias on three of
 * its passes, the duration projection, the depthwise transposed convolutions of
 * the predictor and the decoder, the generator's two upsampling convolutions)
 * is a cast to half and straight back that ONNX Runtime's optimiser removes,
 * computing it in float with the half weights widened, as here.
 *
 * Activations are time-major (tts_priv.h): a sequence is T rows of its channels.
 * The style row's first 128 values are the decoder's and the generator's, the
 * second 128 the prosody predictor's (each AdaLayerNorm and AdaIN's projections
 * read them). */
#include "tts_priv.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    CTX_E = 128, CTX_H = 768, CTX_FF = 2048, CTX_HEADS = 12, CTX_LAYERS = 12, CTX_POS = 512,
    VOCAB = 178,       /* token ids 0..177 */
    D = 512,           /* the encoders' and the predictor's width */
    STY = 128,         /* each half of the style row */
    DS = D + STY,      /* the predictor's states with the style appended */
    LH = 256,          /* every LSTM's hidden size, per direction */
    DUR_BINS = 50,
    TXT_K = 5          /* the text encoder's convolutions */
};

typedef struct {
    const voice *v;
    char *err;
    size_t cap;
    const char *missing;
} wctx;

static void ferr(char *err, size_t cap, const char *fmt, ...) {
    if (!err || !cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

/* Tensor `name` with `n` elements of `dtype` (n 0: any), or NULL after noting
 * the first one missing or of the wrong size. */
static const voice_tensor *wget(wctx *w, voice_tensor *t, const char *name, int dtype, size_t n) {
    if (!voice_tensor_get(w->v, name, t) || t->dtype != dtype ||
        (n && t->bytes != n * (dtype == VOICE_F32 ? 4 : 1))) {
        if (!w->missing) w->missing = name;
        memset(t, 0, sizeof *t);
        return NULL;
    }
    return t;
}

static const float *wgetf(wctx *w, const char *name, size_t n) {
    voice_tensor t;
    return wget(w, &t, name, VOICE_F32, n) ? (const float *)t.data : NULL;
}

/* ---- scratch ---------------------------------------------------------------- */

/* Shared integer scratch for the quantised products: q (int16 rows), acc (int32),
 * wbuf (widened weight rows). Sized once per run for the largest use. */
typedef struct {
    int16_t *q;
    int32_t *acc;
    int16_t *wbuf;
    size_t qn, accn, wbn;
} qscratch;

static int qs_need(qscratch *s, size_t qn, size_t accn, size_t wbn) {
    if (qn > s->qn) {
        free(s->q);
        s->q = malloc(qn * sizeof *s->q);
        s->qn = s->q ? qn : 0;
    }
    if (accn > s->accn) {
        free(s->acc);
        s->acc = malloc(accn * sizeof *s->acc);
        s->accn = s->acc ? accn : 0;
    }
    if (wbn > s->wbn) {
        free(s->wbuf);
        s->wbuf = malloc(wbn * sizeof *s->wbuf);
        s->wbn = s->wbuf ? wbn : 0;
    }
    return s->q && s->acc && s->wbuf ? 0 : -1;
}

/* ---- the graph's quantised layers --------------------------------------------- */

/* DynamicQuantizeLinear of x (m rows of k, ldx apart) then MatMulInteger with the
 * stored B (VK_QMM: [round4(n)][kp]), Cast, times a_scale * b_scale, plus bias:
 * y = m rows of n, ldy apart. */
static int qlinear(qscratch *s, const voice_tensor *w, const float *bias, const float *x, int m, int k, int ldx,
                   int n, float *y, int ldy) {
    int kp = w->dims[1];
    if (w->dims[0] < n || kp < k || kp % NN_CPAD) return -1;
    if (qs_need(s, (size_t)m * kp, (size_t)m * n, NN_WBUF(1, kp)) != 0) return -1;
    nn_qparam qp;
    nn_quantize(x, m, k, ldx, s->q, kp, &qp);
    nn_matmul_q(s->q, m, kp, (const int8_t *)w->data, w->zero_point, n, s->acc, n, s->wbuf);
    nn_dequant(s->acc, m, n, n, qp.scale * w->scale, bias, y, ldy);
    return 0;
}

/* The same quantised input against several weights (q, k and v share one
 * DynamicQuantizeLinear). */
static int qlinear_q(qscratch *s, int kp, const nn_qparam *qp, const voice_tensor *w, const float *bias, int m,
                     int n, float *y, int ldy) {
    if (w->dims[0] < n || w->dims[1] != kp) return -1;
    nn_matmul_q(s->q, m, kp, (const int8_t *)w->data, w->zero_point, n, s->acc, n, s->wbuf);
    nn_dequant(s->acc, m, n, n, qp->scale * w->scale, bias, y, ldy);
    return 0;
}

/* ConvInteger, 1-D: DynamicQuantizeLinear of x (t rows of cin, ldx apart), the
 * stored W (VK_QCONV: [round4(cout)][k][cinp]), `pad` rows of the zero point
 * before and after, the stride and dilation; t_out rows of cout, then the scale
 * and bias as qlinear. */
static int qconv_ex(qscratch *s, const voice_tensor *w, const float *bias, const float *x, int t, int cin, int ldx,
                    int stride, int dil, int pad, int t_out, int cout, float *y, int ldy) {
    int k = w->dims[1], cp = w->dims[2];
    if (w->dims[0] < cout || cp < cin || cp % NN_CPAD || (size_t)k * cp > 32768 || t_out < 1) return -1;
    size_t need = (size_t)(t_out - 1) * stride + (size_t)(k - 1) * dil + 1, rows = (size_t)t + 2 * pad;
    if (rows < need) rows = need;
    if (qs_need(s, rows * cp, (size_t)t_out * cout, NN_WBUF(k, cp)) != 0) return -1;
    nn_qparam qp;
    memset(s->q, 0, sizeof *s->q * (size_t)pad * cp);
    nn_quantize(x, t, cin, ldx, s->q + (size_t)pad * cp, cp, &qp);
    memset(s->q + ((size_t)pad + t) * cp, 0, sizeof *s->q * (rows - pad - t) * cp);
    nn_conv_q(s->q, t_out, stride, dil, k, cp, w->data, w->zero_point, cout, s->acc, cout, s->wbuf);
    nn_dequant(s->acc, t_out, cout, cout, qp.scale * w->scale, bias, y, ldy);
    return 0;
}

/* The same, stride 1, padding (k - 1) / 2 each side: t rows in, t out. */
static int qconv(qscratch *s, const voice_tensor *w, const float *bias, const float *x, int t, int cin, int ldx,
                 int cout, float *y, int ldy) {
    if (w->dims[1] > 7) return -1;
    return qconv_ex(s, w, bias, x, t, cin, ldx, 1, 1, (w->dims[1] - 1) / 2, t, cout, y, ldy);
}

/* A DynamicQuantizeLSTM's weights, both directions, from voice.bin: <base>.fwd.*
 * and <base>.bwd.* */
static int get_lstm(wctx *w, const char *base, int in, nn_lstm *l) {
    memset(l, 0, sizeof *l);
    l->hidden = LH;
    l->in_pad = NN_ROUND(in, NN_CPAD);
    static const char *dir[2] = { "fwd", "bwd" };
    for (int d = 0; d < 2; d++) {
        char name[VOICE_NAME_MAX];
        voice_tensor t;
        snprintf(name, sizeof name, "%s.%s.w", base, dir[d]);
        if (!wget(w, &t, name, VOICE_I8, (size_t)4 * LH * l->in_pad)) return -1;
        l->w[d] = (const int8_t *)t.data;
        l->wscale[d] = t.scale;
        l->wzp[d] = t.zero_point;
        snprintf(name, sizeof name, "%s.%s.r", base, dir[d]);
        if (!wget(w, &t, name, VOICE_I8, (size_t)4 * LH * LH)) return -1;
        l->r[d] = (const int8_t *)t.data;
        l->rscale[d] = t.scale;
        l->rzp[d] = t.zero_point;
        snprintf(name, sizeof name, "%s.%s.b", base, dir[d]);
        if (!(l->b[d] = wgetf(w, name, (size_t)8 * LH))) return -1;
    }
    return 0;
}

/* A bidirectional LSTM over t rows of `in` (ldx apart) into t rows of 2 * LH. */
static int lstm(const nn_lstm *l, const float *x, int t, int in, int ldx, float *y) {
    void *scratch = malloc(nn_lstm_scratch(t, l->in_pad, l->hidden));
    if (!scratch) return -1;
    nn_lstm_run(l, 2, x, t, in, ldx, y, 2 * LH, scratch);
    free(scratch);
    return 0;
}

/* The style projections of an adaptive norm: fc(style) -> gamma (its first n,
 * plus 1, as the graph adds it) and beta (its second n). */
static int ada(qscratch *s, wctx *w, const char *base, const float *style, int n, float *gamma, float *beta) {
    char name[VOICE_NAME_MAX];
    voice_tensor t;
    snprintf(name, sizeof name, "%s.w", base);
    if (!wget(w, &t, name, VOICE_I8, 0)) return -1;
    snprintf(name, sizeof name, "%s.b", base);
    const float *b = wgetf(w, name, (size_t)2 * n);
    float fc[2 * 1090];                       /* the widest: the decoder's, 1090 channels */
    if (!b || 2 * n > (int)(sizeof fc / sizeof fc[0]) || qlinear(s, &t, b, style, 1, STY, STY, 2 * n, fc, 2 * n) != 0)
        return -1;
    for (int i = 0; i < n; i++) {
        gamma[i] = fc[i] + 1.0f;
        beta[i] = fc[n + i];
    }
    return 0;
}

/* x = gamma * x + beta per channel (the graph's Mul then Add). */
static void affine(float *x, int t, int c, int ld, const float *gamma, const float *beta) {
    for (int r = 0; r < t; r++) {
        float *xr = x + (size_t)r * ld;
        for (int i = 0; i < c; i++) xr[i] = gamma[i] * xr[i] + beta[i];
    }
}

/* ---- the context encoder (a 12-pass shared-weight transformer) ----------------- */

static int context(qscratch *s, wctx *w, const int *ids, int T, float *out /* T x D */) {
    voice_tensor tw[8];
    const float *tok = wgetf(w, "ctx.emb.tok", (size_t)VOCAB * CTX_E), *pos = wgetf(w, "ctx.emb.pos", (size_t)CTX_POS * CTX_E);
    const float *typ = wgetf(w, "ctx.emb.type", 2 * CTX_E), *eg = wgetf(w, "ctx.emb.ln.g", CTX_E);
    const float *eb = wgetf(w, "ctx.emb.ln.b", CTX_E), *ee = wgetf(w, "ctx.emb.ln.eps", 1);
    const voice_tensor *win = wget(w, &tw[0], "ctx.in.w", VOICE_I8, 0), *wq = wget(w, &tw[1], "ctx.layer.q.w", VOICE_I8, 0);
    const voice_tensor *wk = wget(w, &tw[2], "ctx.layer.k.w", VOICE_I8, 0), *wv = wget(w, &tw[3], "ctx.layer.v.w", VOICE_I8, 0);
    const voice_tensor *wo = wget(w, &tw[4], "ctx.layer.o.w", VOICE_I8, 0), *wf = wget(w, &tw[5], "ctx.layer.ffn.w", VOICE_I8, 0);
    const voice_tensor *wf2 = wget(w, &tw[6], "ctx.layer.ffn_out.w", VOICE_I8, 0), *wout = wget(w, &tw[7], "ctx.out.w", VOICE_I8, 0);
    const float *bin = wgetf(w, "ctx.in.b", CTX_H), *bq = wgetf(w, "ctx.layer.q.b", CTX_H), *bk = wgetf(w, "ctx.layer.k.b", CTX_H);
    const float *bv = wgetf(w, "ctx.layer.v.b", CTX_H), *bo = wgetf(w, "ctx.layer.o.b", CTX_H);
    const float *bf = wgetf(w, "ctx.layer.ffn.b", CTX_FF), *bf2 = wgetf(w, "ctx.layer.ffn_out.b", CTX_H);
    const float *g1 = wgetf(w, "ctx.layer.ln1.g", CTX_H), *b1 = wgetf(w, "ctx.layer.ln1.b", CTX_H), *e1 = wgetf(w, "ctx.layer.ln1.eps", 1);
    const float *g2 = wgetf(w, "ctx.layer.ln2.g", CTX_H), *b2 = wgetf(w, "ctx.layer.ln2.b", CTX_H), *e2 = wgetf(w, "ctx.layer.ln2.eps", 1);
    const float *bout = wgetf(w, "ctx.out.b", D);
    if (w->missing) return -1;
    if (T > CTX_POS) {
        ferr(w->err, w->cap, "%d tokens is more than the voice model's %d", T, CTX_POS);
        return -1;
    }
    int dh = CTX_H / CTX_HEADS;
    size_t th = (size_t)T * CTX_H;
    float *e = malloc(sizeof(float) * (size_t)T * CTX_E), *x = malloc(sizeof(float) * th * 5);
    float *ff = malloc(sizeof(float) * (size_t)T * CTX_FF);
    float *sc = malloc(sizeof(float) * ((size_t)T * (dh + 1) + dh));
    int rc = -1;
    if (!e || !x || !ff || !sc) goto out;
    float *q = x + th, *k = q + th, *v = k + th, *a = v + th;

    /* Embeddings: (word + token type 0) + position, then LayerNorm. */
    for (int t = 0; t < T; t++)
        for (int c = 0; c < CTX_E; c++)
            e[(size_t)t * CTX_E + c] = (tok[(size_t)ids[t] * CTX_E + c] + typ[c]) + pos[(size_t)t * CTX_E + c];
    nn_layernorm(e, T, CTX_E, CTX_E, eg, eb, ee[0]);
    if (qlinear(s, win, bin, e, T, CTX_E, CTX_E, CTX_H, x, CTX_H) != 0) goto out;

    float scale = sqrtf(1.0f / sqrtf((float)dh));
    for (int pass = 0; pass < CTX_LAYERS; pass++) {
        int kp = wq->dims[1];
        nn_qparam qp;
        if (qs_need(s, (size_t)T * kp, (size_t)T * CTX_H, NN_WBUF(1, kp)) != 0) goto out;
        nn_quantize(x, T, CTX_H, CTX_H, s->q, kp, &qp);
        if (qlinear_q(s, kp, &qp, wq, bq, T, CTX_H, q, CTX_H) != 0 || qlinear_q(s, kp, &qp, wk, bk, T, CTX_H, k, CTX_H) != 0 ||
            qlinear_q(s, kp, &qp, wv, bv, T, CTX_H, v, CTX_H) != 0)
            goto out;
        /* The mask adds 0 to every score: one sequence, no padding. */
        nn_attention(q, k, v, CTX_H, T, CTX_HEADS, dh, scale, NULL, a, CTX_H, sc);
        if (qlinear(s, wo, bo, a, T, CTX_H, CTX_H, CTX_H, q, CTX_H) != 0) goto out;
        for (size_t i = 0; i < th; i++) a[i] = x[i] + q[i];
        nn_layernorm(a, T, CTX_H, CTX_H, g1, b1, e1[0]);
        if (qlinear(s, wf, bf, a, T, CTX_H, CTX_H, CTX_FF, ff, CTX_FF) != 0) goto out;
        nn_gelu(ff, (size_t)T * CTX_FF);
        if (qlinear(s, wf2, bf2, ff, T, CTX_FF, CTX_FF, CTX_H, x, CTX_H) != 0) goto out;
        nn_skip_layernorm(x, a, T, CTX_H, CTX_H, CTX_H, g2, b2, e2[0]);
    }
    if (qlinear(s, wout, bout, x, T, CTX_H, CTX_H, D, out, D) != 0) goto out;
    rc = 0;
out:
    free(e); free(x); free(ff); free(sc);
    return rc;
}

/* ---- the prosody predictor's duration encoder --------------------------------- */

/* d (T x DS): three LSTMs, each followed by an AdaLayerNorm, over the context
 * encoder's output with the style appended to every row. */
static int duration_encoder(qscratch *s, wctx *w, const float *ctx, int T, const float *sty, float *d) {
    nn_lstm l[3];
    char name[VOICE_NAME_MAX];
    const float *eps = wgetf(w, "norm.eps", 1);
    for (int i = 0; i < 3; i++) {
        snprintf(name, sizeof name, "pro.enc.%d.lstm", i);
        if (get_lstm(w, name, DS, &l[i]) != 0) return -1;
    }
    if (!eps) return -1;
    float *h = malloc(sizeof(float) * (size_t)T * D);
    if (!h) return -1;
    for (int t = 0; t < T; t++) {
        memcpy(d + (size_t)t * DS, ctx + (size_t)t * D, sizeof(float) * D);
        memcpy(d + (size_t)t * DS + D, sty, sizeof(float) * STY);
    }
    int rc = -1;
    for (int i = 0; i < 3; i++) {
        float gamma[D], beta[D];
        snprintf(name, sizeof name, "pro.enc.%d.ada", i);
        if (lstm(&l[i], d, T, DS, DS, h) != 0 || ada(s, w, name, sty, D, gamma, beta) != 0) goto out;
        nn_layernorm(h, T, D, D, NULL, NULL, eps[0]);
        affine(h, T, D, D, gamma, beta);
        for (int t = 0; t < T; t++) memcpy(d + (size_t)t * DS, h + (size_t)t * D, sizeof(float) * D);
    }
    rc = 0;
out:
    free(h);
    return rc;
}

/* Round half to even, for |v| < 2^22: the sum with 1.5 * 2^23 keeps no fraction. */
static float round_even(float v) {
    const float m = 12582912.0f;
    return (v + m) - m;
}

/* Per token: round(sum of sigmoid(proj(lstm(d)))) / speed), at least 1. */
static int durations(wctx *w, const float *d, int T, float speed, float *raw, int *dur) {
    nn_lstm l;
    if (get_lstm(w, "pro.dur.lstm", DS, &l) != 0) return -1;
    voice_tensor pw;
    const float *pb = wgetf(w, "pro.dur.proj.b", DUR_BINS);
    if (!wget(w, &pw, "pro.dur.proj.w", VOICE_F32, 0) || !pb) return -1;
    if (pw.ndim != 2 || pw.dims[0] != D || pw.dims[1] < DUR_BINS) {
        ferr(w->err, w->cap, "voice data: pro.dur.proj.w has an unexpected shape");
        return -1;
    }
    float *h = malloc(sizeof(float) * (size_t)T * D), *p = malloc(sizeof(float) * (size_t)T * DUR_BINS);
    int rc = -1;
    if (!h || !p || lstm(&l, d, T, DS, DS, h) != 0) goto out;
    nn_matmul_fma(h, T, D, D, (const float *)pw.data, DUR_BINS, pw.dims[1], p, DUR_BINS);
    for (int t = 0; t < T; t++)
        for (int b = 0; b < DUR_BINS; b++) p[(size_t)t * DUR_BINS + b] = pb[b] + p[(size_t)t * DUR_BINS + b];
    nn_logistic(p, (size_t)T * DUR_BINS);
    for (int t = 0; t < T; t++) {
        raw[t] = nn_reduce_sum(p + (size_t)t * DUR_BINS, DUR_BINS, (size_t)t * DUR_BINS) / speed;
        dur[t] = voice_round_duration(raw[t]);
    }
    rc = 0;
out:
    free(h); free(p);
    return rc;
}

int voice_round_duration(float raw) {
    float r = round_even(raw);
    return r < 1.0f ? 1 : (int)r;
}

int voice_align(const int *dur, int T, int *frame_token, int cap) {
    int f = 0;
    for (int t = 0; t < T; t++)
        for (int i = 0; i < dur[t]; i++) {
            if (f >= cap) return -1;
            frame_token[f++] = t;
        }
    return f;
}

void voice_expand(const float *x, int ldx, int c, const int *frame_token, int frames, float *y, int ldy) {
    for (int f = 0; f < frames; f++)
        memcpy(y + (size_t)f * ldy, x + (size_t)frame_token[f] * ldx, sizeof(float) * (size_t)c);
}

/* ---- the text encoder ------------------------------------------------------------- */

static int text_encoder(qscratch *s, wctx *w, const int *ids, int T, float *out /* T x D */) {
    const float *emb = wgetf(w, "txt.emb", (size_t)VOCAB * D);
    voice_tensor cw[3];
    const float *cb[3], *lg[3], *lb[3], *le[3];
    char name[VOICE_NAME_MAX];
    for (int i = 0; i < 3; i++) {
        snprintf(name, sizeof name, "txt.cnn.%d.conv.w", i);
        wget(w, &cw[i], name, VOICE_U8, 0);
        snprintf(name, sizeof name, "txt.cnn.%d.conv.b", i);
        cb[i] = wgetf(w, name, D);
        snprintf(name, sizeof name, "txt.cnn.%d.ln.g", i);
        lg[i] = wgetf(w, name, D);
        snprintf(name, sizeof name, "txt.cnn.%d.ln.b", i);
        lb[i] = wgetf(w, name, D);
        snprintf(name, sizeof name, "txt.cnn.%d.ln.eps", i);
        le[i] = wgetf(w, name, 1);
    }
    nn_lstm l;
    if (get_lstm(w, "txt.lstm", D, &l) != 0 || w->missing) return -1;
    float *x = malloc(sizeof(float) * (size_t)T * D * 2);
    if (!x) return -1;
    float *y = x + (size_t)T * D;
    int rc = -1;
    for (int t = 0; t < T; t++) memcpy(x + (size_t)t * D, emb + (size_t)ids[t] * D, sizeof(float) * D);
    for (int i = 0; i < 3; i++) {
        if (cw[i].dims[1] != TXT_K || qconv(s, &cw[i], cb[i], x, T, D, D, D, y, D) != 0) goto out;
        nn_layernorm(y, T, D, D, lg[i], lb[i], le[i][0]);
        nn_leaky_relu(y, (size_t)T * D, 0.2f);
        memcpy(x, y, sizeof(float) * (size_t)T * D);
    }
    if (lstm(&l, x, T, D, D, out) != 0) goto out;
    rc = 0;
out:
    free(x);
    return rc;
}

/* ---- the pitch and energy predictors ----------------------------------------------- */

/* Half precision and back, round to nearest even (the graph's Cast to float16
 * and back to float). */
static float via_half(float v) {
    union { float f; uint32_t u; } x = { v };
    uint32_t sign = x.u & 0x80000000u, a = x.u & 0x7FFFFFFFu;
    if (a > 0x7F800000u) return v;                          /* NaN */
    if (a >= 0x477FF000u) {                                 /* rounds to 65520 or more: infinity */
        x.u = sign | 0x7F800000u;
        return x.f;
    }
    if (a < 0x38800000u) {                                  /* below 2^-14: half's subnormals, 2^-24 apart */
        float q = round_even(fabsf(v) * 16777216.0f) / 16777216.0f;
        return sign ? -q : q;
    }
    x.u = sign | ((a + 0x0FFFu + ((a >> 13) & 1u)) & ~0x1FFFu);    /* 13 mantissa bits dropped */
    return x.f;
}

/* One residual block of AdaIN, LeakyReLU and convolutions (the predictor's
 * and the decoder's AdainResBlk1d), t rows of cin -> (up ? 2t : t) rows of
 * cout, in place in x (room for the longer). */
static int adain_block(qscratch *s, wctx *w, const char *base, const float *sty, float *x, int t, int cin, int cout,
                       int up) {
    char name[VOICE_NAME_MAX];
    const float *eps = wgetf(w, "norm.eps", 1);
    voice_tensor c1, c2, sk;
    const float *b1, *b2, *pw = NULL, *pb = NULL;
    snprintf(name, sizeof name, "%s.conv1.w", base); wget(w, &c1, name, VOICE_U8, 0);
    snprintf(name, sizeof name, "%s.conv1.b", base); b1 = wgetf(w, name, (size_t)cout);
    snprintf(name, sizeof name, "%s.conv2.w", base); wget(w, &c2, name, VOICE_U8, 0);
    snprintf(name, sizeof name, "%s.conv2.b", base); b2 = wgetf(w, name, (size_t)cout);
    if (cin != cout) { snprintf(name, sizeof name, "%s.skip.w", base); wget(w, &sk, name, VOICE_U8, 0); }
    if (up) {
        snprintf(name, sizeof name, "%s.pool.w", base); pw = wgetf(w, name, (size_t)3 * cin);
        snprintf(name, sizeof name, "%s.pool.b", base); pb = wgetf(w, name, (size_t)cin);
    }
    if (w->missing || !eps) return -1;
    int to = up ? 2 * t : t, wide = cin > cout ? cin : cout;
    float *g = malloc(sizeof(float) * 2 * (size_t)wide), *r = malloc(sizeof(float) * (size_t)to * wide * 2);
    int rc = -1;
    if (!g || !r) goto out;
    float *be = g + wide, *h = r + (size_t)to * wide;
    /* The residual path: AdaIN, LeakyReLU, [upsample], conv1, AdaIN, LeakyReLU, conv2. */
    memcpy(r, x, sizeof(float) * (size_t)t * cin);
    snprintf(name, sizeof name, "%s.ada1", base);
    if (ada(s, w, name, sty, cin, g, be) != 0) goto out;
    nn_instnorm(r, t, cin, cin, NULL, NULL, eps[0]);
    affine(r, t, cin, cin, g, be);
    nn_leaky_relu(r, (size_t)t * cin, 0.2f);
    if (up) {
        /* Depthwise ConvTranspose: kernel 3, stride 2, padding 1, output padding 1. */
        nn_convt_dw(r, t, cin, cin, 3, 2, 1, pw, pb, to, h, cin);
        memcpy(r, h, sizeof(float) * (size_t)to * cin);
    }
    if (qconv(s, &c1, b1, r, to, cin, cin, cout, h, cout) != 0) goto out;
    snprintf(name, sizeof name, "%s.ada2", base);
    if (ada(s, w, name, sty, cout, g, be) != 0) goto out;
    nn_instnorm(h, to, cout, cout, NULL, NULL, eps[0]);
    affine(h, to, cout, cout, g, be);
    nn_leaky_relu(h, (size_t)to * cout, 0.2f);
    if (qconv(s, &c2, b2, h, to, cout, cout, cout, r, cout) != 0) goto out;
    /* The shortcut: [nearest upsample], [1x1 conv without bias]; the sum over sqrt 2. */
    if (up) {
        for (int i = to - 1; i >= 0; i--) memmove(h + (size_t)i * cin, x + (size_t)(i / 2) * cin, sizeof(float) * cin);
    } else memcpy(h, x, sizeof(float) * (size_t)t * cin);
    if (cin != cout) {
        if (qconv(s, &sk, NULL, h, to, cin, cin, cout, x, cout) != 0) goto out;
    } else memcpy(x, h, sizeof(float) * (size_t)to * cout);
    for (size_t i = 0; i < (size_t)to * cout; i++) x[i] = (r[i] + x[i]) / 1.41421354f;
    rc = 0;
out:
    free(g); free(r);
    return rc;
}

/* One curve (F0 or N): three blocks over the shared LSTM's output (F rows of
 * D), the projection, rounded to half; 2F values. */
static int curve(qscratch *s, wctx *w, const char *which, const float *sh, int F, const float *sty, float *outc) {
    char name[VOICE_NAME_MAX];
    float *x = malloc(sizeof(float) * (size_t)2 * F * D);
    if (!x) return -1;
    int rc = -1;
    memcpy(x, sh, sizeof(float) * (size_t)F * D);
    snprintf(name, sizeof name, "pro.%s.0", which);
    if (adain_block(s, w, name, sty, x, F, D, D, 0) != 0) goto out;
    snprintf(name, sizeof name, "pro.%s.1", which);
    if (adain_block(s, w, name, sty, x, F, D, D / 2, 1) != 0) goto out;
    snprintf(name, sizeof name, "pro.%s.2", which);
    if (adain_block(s, w, name, sty, x, 2 * F, D / 2, D / 2, 0) != 0) goto out;
    voice_tensor pw;
    snprintf(name, sizeof name, "pro.%s.proj.w", which);
    wget(w, &pw, name, VOICE_U8, 0);
    snprintf(name, sizeof name, "pro.%s.proj.b", which);
    const float *pb = wgetf(w, name, 1);
    if (w->missing || qconv(s, &pw, pb, x, 2 * F, D / 2, D / 2, 1, outc, 1) != 0) goto out;
    for (int i = 0; i < 2 * F; i++) outc[i] = via_half(outc[i]);
    rc = 0;
out:
    free(x);
    return rc;
}

/* ---- the whole front half ------------------------------------------------------------ */

void voice_front_free(voice_front *f) {
    if (!f) return;
    free(f->dur); free(f->dur_raw); free(f->ctx); free(f->txt); free(f->asr); free(f->f0); free(f->n);
    memset(f, 0, sizeof *f);
}

int voice_front_run(const voice *v, const int *ids, int T, const float *style, float speed, voice_front *f, char *err,
                    size_t errcap) {
    memset(f, 0, sizeof *f);
    if (!v || T < 1 || !(speed > 0)) {
        ferr(err, errcap, "nothing to say");
        return -1;
    }
    for (int t = 0; t < T; t++)
        if (ids[t] < 0 || ids[t] >= VOCAB) {
            ferr(err, errcap, "token %d is outside the voice model's %d", ids[t], VOCAB);
            return -1;
        }
    wctx w = { v, err, errcap, NULL };
    qscratch s = { 0 };
    const float *sty = style + STY;           /* the predictor's half */
    float *d = NULL, *en = NULL, *sh = NULL;
    int *ft = NULL, rc = -1;
    f->tokens = T;
    f->dur = malloc(sizeof(int) * (size_t)T);
    f->dur_raw = malloc(sizeof(float) * (size_t)T);
    f->ctx = malloc(sizeof(float) * (size_t)T * D);
    f->txt = malloc(sizeof(float) * (size_t)T * D);
    d = malloc(sizeof(float) * (size_t)T * DS);
    if (!f->dur || !f->dur_raw || !f->ctx || !f->txt || !d) goto fail;
    if (context(&s, &w, ids, T, f->ctx) != 0 || duration_encoder(&s, &w, f->ctx, T, sty, d) != 0 ||
        durations(&w, d, T, speed, f->dur_raw, f->dur) != 0 || text_encoder(&s, &w, ids, T, f->txt) != 0)
        goto fail;
    long frames = 0;
    for (int t = 0; t < T; t++) frames += f->dur[t];
    if (frames > VOICE_MAX_FRAMES) {
        ferr(err, errcap, "%ld frames is more than %d", frames, VOICE_MAX_FRAMES);
        goto fail;
    }
    int F = (int)frames;
    f->frames = F;
    ft = malloc(sizeof(int) * (size_t)F);
    f->asr = malloc(sizeof(float) * (size_t)F * D);
    f->f0 = malloc(sizeof(float) * (size_t)2 * F);
    f->n = malloc(sizeof(float) * (size_t)2 * F);
    en = malloc(sizeof(float) * (size_t)F * DS);
    sh = malloc(sizeof(float) * (size_t)F * D);
    if (!ft || !f->asr || !f->f0 || !f->n || !en || !sh || voice_align(f->dur, T, ft, F) != F) goto fail;
    /* The alignment's products (d^T aln, txt^T aln) with a 0/1 matrix of one 1 per
     * frame are the token's row, exactly. */
    voice_expand(f->txt, D, D, ft, F, f->asr, D);
    voice_expand(d, DS, DS, ft, F, en, DS);
    nn_lstm l;
    if (get_lstm(&w, "pro.lstm", DS, &l) != 0 || lstm(&l, en, F, DS, DS, sh) != 0) goto fail;
    if (curve(&s, &w, "f0", sh, F, sty, f->f0) != 0 || curve(&s, &w, "n", sh, F, sty, f->n) != 0) goto fail;
    rc = 0;
fail:
    if (rc != 0) {
        if (w.missing) ferr(err, errcap, "voice data: no tensor %s of the expected size", w.missing);
        else if (err && errcap && !err[0]) ferr(err, errcap, "out of memory or malformed voice data");
        voice_front_free(f);
    }
    free(d); free(en); free(sh); free(ft);
    free(s.q); free(s.acc); free(s.wbuf);
    return rc;
}


/* ---- the decoder ---------------------------------------------------------------------- */

enum {
    DEC_ASR = 64,                     /* the text states' projection the decoder blocks re-read */
    DEC_W = 1024,                     /* the decoder's width */
    DEC_CAT = DEC_W + DEC_ASR + 2,    /* a block's input: x, the projection, F0 and N */
    DEC_OUT = 512
};

/* The decoder's stride-2 convolution of a curve (2F values) to one value per
 * frame: kernel 3, padding 1, one channel, in ONNX Runtime's order (its
 * one-row matrix product: each tap's product added in turn, no fused
 * multiply-add; then the bias). */
static void curve_conv(const float *c, int F, const float *w, float b, float *y, int ldy) {
    for (int t = 0; t < F; t++) {
        float a = 0;
        for (int j = 0; j < 3; j++) {
            int i = 2 * t - 1 + j;
            float p = (i >= 0 && i < 2 * F ? c[i] : 0.0f) * w[j];
            a = j ? a + p : p;
        }
        y[(size_t)t * ldy] = a + b;
    }
}

int voice_decode(const voice *v, const voice_front *f, const float *style, float *out, char *err, size_t errcap) {
    wctx w = { v, err, errcap, NULL };
    qscratch s = { 0 };
    int F = f->frames, rc = -1;
    const float *f0w = wgetf(&w, "dec.f0.w", 3), *f0b = wgetf(&w, "dec.f0.b", 1);
    const float *nw = wgetf(&w, "dec.n.w", 3), *nb = wgetf(&w, "dec.n.b", 1), *ab = wgetf(&w, "dec.asr.b", DEC_ASR);
    voice_tensor aw;
    wget(&w, &aw, "dec.asr.w", VOICE_U8, 0);
    if (w.missing) {
        ferr(err, errcap, "voice data: no tensor %s of the expected size", w.missing);
        return -1;
    }
    /* side: per frame the asr projection, F0 and N (the columns every block's
     * input ends with); x: a block's input, then its output in place. */
    float *side = malloc(sizeof(float) * (size_t)F * (DEC_ASR + 2));
    float *x = malloc(sizeof(float) * (size_t)2 * F * DEC_CAT);
    if (!side || !x) goto out;
    if (qconv(&s, &aw, ab, f->asr, F, D, D, DEC_ASR, side, DEC_ASR + 2) != 0) goto out;
    curve_conv(f->f0, F, f0w, f0b[0], side + DEC_ASR, DEC_ASR + 2);
    curve_conv(f->n, F, nw, nb[0], side + DEC_ASR + 1, DEC_ASR + 2);
    /* encode: the text states, F0 and N, 514 -> 1024. */
    for (int t = 0; t < F; t++) {
        memcpy(x + (size_t)t * (D + 2), f->asr + (size_t)t * D, sizeof(float) * D);
        memcpy(x + (size_t)t * (D + 2) + D, side + (size_t)t * (DEC_ASR + 2) + DEC_ASR, sizeof(float) * 2);
    }
    if (adain_block(&s, &w, "dec.enc", style, x, F, D + 2, DEC_W, 0) != 0) goto out;
    /* decode.0..3, each over its input with the side columns appended; the last
     * doubles the length and narrows to 512. */
    for (int b = 0; b < 4; b++) {
        for (int t = F - 1; t >= 0; t--) {
            memmove(x + (size_t)t * DEC_CAT, x + (size_t)t * DEC_W, sizeof(float) * DEC_W);
            memcpy(x + (size_t)t * DEC_CAT + DEC_W, side + (size_t)t * (DEC_ASR + 2), sizeof(float) * (DEC_ASR + 2));
        }
        char name[VOICE_NAME_MAX];
        snprintf(name, sizeof name, "dec.dec.%d", b);
        if (adain_block(&s, &w, name, style, x, F, DEC_CAT, b < 3 ? DEC_W : DEC_OUT, b == 3) != 0) goto out;
    }
    memcpy(out, x, sizeof(float) * (size_t)2 * F * DEC_OUT);
    rc = 0;
out:
    if (rc != 0) {
        if (w.missing) ferr(err, errcap, "voice data: no tensor %s of the expected size", w.missing);
        else if (err && errcap && !err[0]) ferr(err, errcap, "out of memory or malformed voice data");
    }
    free(side); free(x);
    free(s.q); free(s.acc); free(s.wbuf);
    return rc;
}

/* ---- the harmonic source --------------------------------------------------------------- */

enum {
    SRC_UP = 300,          /* output samples per value of the pitch curve (two per frame) */
    SRC_HARM = 9,          /* the fundamental and eight overtones */
    STFT_N = 20, STFT_HOP = 5, STFT_BINS = STFT_N / 2 + 1,
    HAR = 2 * STFT_BINS    /* the source's spectrum per STFT frame: magnitudes, then phases */
};
#define SRC_RATE    24000.0f
#define SRC_AMP     0.1f   /* the sines' amplitude */
#define SRC_VOICED  10.0f  /* a pitch above this many Hz is voiced */
#define SRC_NOISE_V 0.003f /* the noise's standard deviation where voiced */
#define SRC_PI      3.1415927f

/* ONNX Runtime's linear Resize of the graph's [1, 9, n] tensors along their
 * last axis: it takes them as a trilinear resize of depth 1 and height 9, so
 * each output is the sum, in turn, of eight corners weighted by products of the
 * three axes' weights. The depth's two are 1/2 each; the height's are 1 and 0,
 * except on the last row (harmonic 9), which is clamped to itself and has 1/2
 * and 1/2. x1 and x2 are the two neighbours along time, w2 and w1 their weights. */
static inline float resize9(float x1, float x2, float w1, float w2, int last) {
    if (!last) {
        float a = (w2 * x1) * 0.5f, b = (w1 * x2) * 0.5f;
        return ((a + b) + a) + b;
    }
    float a = (w2 * x1) * 0.25f, b = (w1 * x2) * 0.25f, r = a + b;
    for (int i = 0; i < 3; i++) {
        r = r + a;
        r = r + b;
    }
    return r;
}

/* Resize's half-pixel source coordinate of output position o for a scale,
 * clamped into the input's n positions: its two neighbours and their weights. */
static void resize_at(int o, float scale, int n, int *x1, int *x2, float *w1, float *w2) {
    float in = ((float)o + 0.5f) / scale - 0.5f;
    if (in < 0) in = 0;
    if (in > (float)(n - 1)) in = (float)(n - 1);
    int a = (int)in;
    if (a > n - 1) a = n - 1;
    int b = a + 1 < n ? a + 1 : n - 1;
    *x1 = a;
    *x2 = b;
    if (a == b) {
        *w1 = *w2 = 0.5f;
        return;
    }
    *w1 = fabsf(in - (float)a);
    *w2 = fabsf(in - (float)b);
}

uint64_t voice_rng_next(voice_rng *r) {
    uint64_t z = (r->state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

float voice_rng_uniform(voice_rng *r) { return (float)(voice_rng_next(r) >> 40) * (1.0f / 16777216.0f); }

float voice_rng_normal(voice_rng *r) {
    if (r->have) {
        r->have = 0;
        return r->spare;
    }
    /* Box-Muller on two uniforms in (0, 1] and [0, 1), in double. */
    double u = ((double)(voice_rng_next(r) >> 11) + 1.0) * (1.0 / 9007199254740992.0);
    double t = (double)(voice_rng_next(r) >> 11) * (2.0 * 3.14159265358979323846 / 9007199254740992.0);
    double m = sqrt(-2.0 * log(u));
    r->spare = (float)(m * sin(t));
    r->have = 1;
    return (float)(m * cos(t));
}

int voice_source(const voice *v, const float *f0, int frames, int noise, uint64_t seed, float *har, float *noise_out,
                 char *err, size_t errcap) {
    wctx w = { v, err, errcap, NULL };
    const float *harm = wgetf(&w, "gen.src.harm", SRC_HARM), *lb = wgetf(&w, "gen.src.lin.b", 1);
    voice_tensor lw, sre, sim;
    wget(&w, &lw, "gen.src.lin.w", VOICE_F32, 0);
    wget(&w, &sre, "gen.stft.re.w", VOICE_F32, 0);
    wget(&w, &sim, "gen.stft.im.w", VOICE_F32, 0);
    if (w.missing || lw.dims[0] != SRC_HARM || sre.dims[0] != STFT_N || sim.dims[0] != STFT_N ||
        sre.dims[1] < STFT_BINS || sim.dims[1] != sre.dims[1]) {
        ferr(err, errcap, "voice data: no tensor %s of the expected size", w.missing ? w.missing : "gen.src");
        return -1;
    }
    int n2 = 2 * frames;                         /* pitch values */
    size_t L = (size_t)n2 * SRC_UP;              /* samples */
    int T = (int)(L / STFT_HOP) + 1;             /* STFT frames */
    float *rad = malloc(sizeof(float) * (size_t)n2 * SRC_HARM * 2);
    float *sig = malloc(sizeof(float) * (L + STFT_N > (size_t)T * HAR ? L + STFT_N : (size_t)T * HAR));
    float *spec = malloc(sizeof(float) * (size_t)T * 2 * STFT_BINS);
    if (!rad || !sig || !spec) {
        free(rad); free(sig); free(spec);
        ferr(err, errcap, "out of memory");
        return -1;
    }
    float *cum = rad + (size_t)n2 * SRC_HARM;
    voice_rng rng = { seed, 0, 0 };

    /* Each harmonic's phase step per sample, (h f0 / 24000) mod 1, the same for
     * the 300 samples of a pitch value; with the noise on, the first sample's
     * of harmonics 2..9 also has a uniform random initial phase added. */
    float ini[SRC_HARM] = { 0 };
    if (noise)
        for (int h = 1; h < SRC_HARM; h++) ini[h] = voice_rng_uniform(&rng);
    for (int i = 0; i < n2; i++)
        for (int h = 0; h < SRC_HARM; h++) {
            float d = (f0[i] * harm[h]) / SRC_RATE;
            rad[(size_t)i * SRC_HARM + h] = d - floorf(d);
        }
    /* Down to one step per pitch value (Resize by 1/300), summed along time,
     * times 2 pi and 300: the phase at each pitch value. */
    const float down = 1.0f / (float)SRC_UP;
    float acc[SRC_HARM] = { 0 };
    for (int i = 0; i < n2; i++) {
        int a, b;
        float w1, w2;
        resize_at(i, down, (int)L, &a, &b, &w1, &w2);
        for (int h = 0; h < SRC_HARM; h++) {
            float xa = rad[(size_t)(a / SRC_UP) * SRC_HARM + h] + (a == 0 ? ini[h] : 0.0f);
            float xb = rad[(size_t)(b / SRC_UP) * SRC_HARM + h] + (b == 0 ? ini[h] : 0.0f);
            acc[h] = acc[h] + resize9(xa, xb, w1, w2, h == SRC_HARM - 1);
            cum[(size_t)i * SRC_HARM + h] = ((acc[h] * 2.0f) * SRC_PI) * (float)SRC_UP;
        }
    }
    /* Back up to every sample (Resize by 300), the sines, voiced or not, the
     * noise, and their mix through the linear layer and tanh. */
    const float *lwv = (const float *)lw.data;
    for (size_t l = 0; l < L; l++) {
        int a, b;
        float w1, w2, ph[SRC_HARM], sn[SRC_HARM];
        resize_at((int)l, (float)SRC_UP, n2, &a, &b, &w1, &w2);
        for (int h = 0; h < SRC_HARM; h++)
            ph[h] = resize9(cum[(size_t)a * SRC_HARM + h], cum[(size_t)b * SRC_HARM + h], w1, w2, h == SRC_HARM - 1);
        nn_sin(ph, sn, SRC_HARM);
        float uv = f0[l / SRC_UP] > SRC_VOICED ? 1.0f : 0.0f;
        float amp = uv * SRC_NOISE_V + ((1.0f - uv) * SRC_AMP) / 3.0f;
        float m = 0;
        for (int h = 0; h < SRC_HARM; h++) {
            float nz = noise ? amp * voice_rng_normal(&rng) : 0.0f;
            if (noise_out) noise_out[l * SRC_HARM + h] = nz;
            m = nn_fma((sn[h] * SRC_AMP) * uv + nz, lwv[h * lw.dims[1]], m);
        }
        sig[STFT_N / 2 + l] = lb[0] + m;
    }
    nn_tanh(sig + STFT_N / 2, L);
    for (int i = 0; i < STFT_N / 2; i++) {
        sig[i] = sig[STFT_N / 2];
        sig[STFT_N / 2 + L + i] = sig[STFT_N / 2 + L - 1];
    }
    /* The forward STFT (two strided convolutions with fixed bases), then each
     * bin's magnitude and phase as the graph computes them. */
    float *re = spec, *im = spec + (size_t)T * STFT_BINS;
    nn_matmul_fma(sig, T, STFT_N, STFT_HOP, (const float *)sre.data, STFT_BINS, sre.dims[1], re, STFT_BINS);
    nn_matmul_fma(sig, T, STFT_N, STFT_HOP, (const float *)sim.data, STFT_BINS, sim.dims[1], im, STFT_BINS);
    /* The quotients im / re bin by bin (the graph's channel-major order, which
     * decides which few its atan leaves to the C library), their atan, then
     * the quadrant from the signs. */
    float *q = sig, *at = sig + (size_t)T * STFT_BINS;
    for (int k = 0; k < STFT_BINS; k++)
        for (int t = 0; t < T; t++) q[(size_t)k * T + t] = im[(size_t)t * STFT_BINS + k] / re[(size_t)t * STFT_BINS + k];
    nn_atan(q, at, (size_t)T * STFT_BINS);
    for (int t = 0; t < T; t++)
        for (int k = 0; k < STFT_BINS; k++) {
            float r = re[(size_t)t * STFT_BINS + k], i = im[(size_t)t * STFT_BINS + k], a = at[(size_t)k * T + t];
            float p = i > 0 ? a + SRC_PI : a - SRC_PI;
            p = r < 0 ? p : a;
            if (i == 0 && r < 0) p = SRC_PI;
            har[(size_t)t * HAR + k] = sqrtf((r * r + i * i) + 1e-14f);
            har[(size_t)t * HAR + STFT_BINS + k] = p;
        }
    free(rad); free(sig); free(spec);
    return 0;
}

/* ---- the generator ---------------------------------------------------------------------- */

enum { GEN_W0 = 256, GEN_W1 = 128 };

/* One of the generator's residual blocks (AdaINResBlock1), in place on t rows of
 * c: three times, x += conv2(snake(AdaIN(conv1(snake(AdaIN(x)))))), conv1
 * dilated 1, 3 and 5. a and b: t * c floats of scratch each. */
static int gen_block(qscratch *s, wctx *w, const char *base, const float *sty, float *x, int t, int c, float *a,
                     float *b) {
    static const int DIL[3] = { 1, 3, 5 };
    const float *eps = wgetf(w, "norm.eps", 1);
    float *g = malloc(sizeof(float) * 2 * (size_t)c);
    if (!eps || !g) {
        free(g);
        return -1;
    }
    float *be = g + c;
    int rc = -1;
    for (int j = 0; j < 3; j++) {
        char name[VOICE_NAME_MAX];
        voice_tensor c1, c2;
        const float *b1, *b2, *al1, *al2, *in1, *in2;
        snprintf(name, sizeof name, "%s.c1.%d.w", base, j); wget(w, &c1, name, VOICE_U8, 0);
        snprintf(name, sizeof name, "%s.c2.%d.w", base, j); wget(w, &c2, name, VOICE_U8, 0);
        snprintf(name, sizeof name, "%s.c1.%d.b", base, j); b1 = wgetf(w, name, (size_t)c);
        snprintf(name, sizeof name, "%s.c2.%d.b", base, j); b2 = wgetf(w, name, (size_t)c);
        snprintf(name, sizeof name, "%s.alpha1.%d", base, j); al1 = wgetf(w, name, (size_t)c);
        snprintf(name, sizeof name, "%s.alpha2.%d", base, j); al2 = wgetf(w, name, (size_t)c);
        snprintf(name, sizeof name, "%s.inv1.%d", base, j); in1 = wgetf(w, name, (size_t)c);
        snprintf(name, sizeof name, "%s.inv2.%d", base, j); in2 = wgetf(w, name, (size_t)c);
        if (w->missing) goto out;
        int k = c1.dims[1];
        memcpy(a, x, sizeof(float) * (size_t)t * c);
        snprintf(name, sizeof name, "%s.ada1.%d", base, j);
        if (ada(s, w, name, sty, c, g, be) != 0) goto out;
        nn_instnorm(a, t, c, c, NULL, NULL, eps[0]);
        affine(a, t, c, c, g, be);
        nn_snake(a, t, c, c, al1, in1);
        if (qconv_ex(s, &c1, b1, a, t, c, c, 1, DIL[j], (k - 1) * DIL[j] / 2, t, c, b, c) != 0) goto out;
        snprintf(name, sizeof name, "%s.ada2.%d", base, j);
        if (ada(s, w, name, sty, c, g, be) != 0) goto out;
        nn_instnorm(b, t, c, c, NULL, NULL, eps[0]);
        affine(b, t, c, c, g, be);
        nn_snake(b, t, c, c, al2, in2);
        if (qconv_ex(s, &c2, b2, b, t, c, c, 1, 1, (c2.dims[1] - 1) / 2, t, c, a, c) != 0) goto out;
        for (size_t i = 0; i < (size_t)t * c; i++) x[i] = a[i] + x[i];
    }
    rc = 0;
out:
    free(g);
    return rc;
}

/* A stage's two residual blocks over the same input, averaged: x = (res(x) +
 * res'(x)) / 2. a, b, c: t * ch floats of scratch each. */
static int gen_pair(qscratch *s, wctx *w, int first, const float *sty, float *x, int t, int ch, float *a, float *b,
                    float *c) {
    char name[VOICE_NAME_MAX];
    memcpy(c, x, sizeof(float) * (size_t)t * ch);
    snprintf(name, sizeof name, "gen.res.%d", first);
    if (gen_block(s, w, name, sty, x, t, ch, a, b) != 0) return -1;
    snprintf(name, sizeof name, "gen.res.%d", first + 1);
    if (gen_block(s, w, name, sty, c, t, ch, a, b) != 0) return -1;
    for (size_t i = 0; i < (size_t)t * ch; i++) x[i] = (x[i] + c[i]) / 2.0f;
    return 0;
}

/* A transposed convolution of the generator (the f16 weights widened, as ONNX
 * Runtime runs it): LeakyReLU 0.1 of x (t rows of cin), then the stored
 * [s][k/s][cin][cout] weights over the input with its zero rows. */
static int gen_up(wctx *w, int i, const float *x, int t, int cin, int t_out, int cout, float *y, int ldy) {
    char name[VOICE_NAME_MAX];
    voice_tensor wt;
    snprintf(name, sizeof name, "gen.up.%d.w", i);
    wget(w, &wt, name, VOICE_F32, 0);
    snprintf(name, sizeof name, "gen.up.%d.b", i);
    const float *bias = wgetf(w, name, (size_t)cout);
    if (w->missing || wt.ndim != 4 || wt.dims[2] != cin || wt.dims[3] < cout) return -1;
    int st = wt.dims[0], mt = wt.dims[1], k = st * mt, pad = (k - st) / 2;
    float *p = calloc((size_t)(t + 2 * mt) * cin, sizeof(float));
    if (!p) return -1;
    memcpy(p + (size_t)(mt - 1) * cin, x, sizeof(float) * (size_t)t * cin);
    nn_leaky_relu(p + (size_t)(mt - 1) * cin, (size_t)t * cin, 0.1f);
    nn_convt(p, t, cin, k, st, pad, (const float *)wt.data, bias, cout, t_out, y, ldy);
    free(p);
    return 0;
}

int voice_generate(const voice *v, const float *dec, const float *har, int frames, const float *style, float *pcm,
                   char *err, size_t errcap) {
    wctx w = { v, err, errcap, NULL };
    qscratch s = { 0 };
    int F = frames, T0 = 20 * F, T1 = 120 * F + 1, rc = -1;
    voice_tensor n0, n1, post;
    wget(&w, &n0, "gen.noise.0.w", VOICE_U8, 0);
    wget(&w, &n1, "gen.noise.1.w", VOICE_U8, 0);
    wget(&w, &post, "gen.post.w", VOICE_U8, 0);
    const float *n0b = wgetf(&w, "gen.noise.0.b", GEN_W0), *n1b = wgetf(&w, "gen.noise.1.b", GEN_W1);
    const float *pb = wgetf(&w, "gen.post.b", HAR);
    const float *ir = wgetf(&w, "gen.istft.re.w", (size_t)STFT_BINS * STFT_N);
    const float *ii = wgetf(&w, "gen.istft.im.w", (size_t)STFT_BINS * STFT_N);
    if (w.missing) {
        ferr(err, errcap, "voice data: no tensor %s of the expected size", w.missing);
        return -1;
    }
    size_t big = (size_t)T1 * GEN_W1 > (size_t)T0 * GEN_W0 ? (size_t)T1 * GEN_W1 : (size_t)T0 * GEN_W0;
    float *x = malloc(sizeof(float) * big), *src = malloc(sizeof(float) * big);
    float *a = malloc(sizeof(float) * big), *b = malloc(sizeof(float) * big), *c = malloc(sizeof(float) * big);
    float *full = malloc(sizeof(float) * ((size_t)(T1 - 1) * STFT_HOP + STFT_N));
    if (!x || !src || !a || !b || !c || !full) goto out;

    /* Stage 1: 2F -> 20F rows of 256. The source's spectrum by a stride-6
     * convolution and its residual blocks; the decoder's output upsampled by 10. */
    if (qconv_ex(&s, &n0, n0b, har, T1, HAR, HAR, 6, 1, 3, T0, GEN_W0, src, GEN_W0) != 0) goto out;
    if (gen_block(&s, &w, "gen.nres.0", style, src, T0, GEN_W0, a, b) != 0) goto out;
    if (gen_up(&w, 0, dec, 2 * F, DEC_OUT, T0, GEN_W0, x, GEN_W0) != 0) goto out;
    for (size_t i = 0; i < (size_t)T0 * GEN_W0; i++) x[i] = x[i] + src[i];
    if (gen_pair(&s, &w, 0, style, x, T0, GEN_W0, a, b, c) != 0) goto out;

    /* Stage 2: 20F -> 120F + 1 rows of 128. Upsampled by 6, the first row
     * reflected in front (the graph's reflection pad of 1); the source at its
     * own rate through a 1x1 convolution and residual blocks. */
    if (gen_up(&w, 1, x, T0, GEN_W0, T1 - 1, GEN_W1, a + GEN_W1, GEN_W1) != 0) goto out;
    memcpy(a, a + 2 * GEN_W1, sizeof(float) * GEN_W1);
    if (qconv_ex(&s, &n1, n1b, har, T1, HAR, HAR, 1, 1, 0, T1, GEN_W1, src, GEN_W1) != 0 ||
        gen_block(&s, &w, "gen.nres.1", style, src, T1, GEN_W1, b, c) != 0)
        goto out;
    for (size_t i = 0; i < (size_t)T1 * GEN_W1; i++) x[i] = a[i] + src[i];
    if (gen_pair(&s, &w, 2, style, x, T1, GEN_W1, a, b, c) != 0) goto out;

    /* The output convolution to 11 log-magnitudes and 11 phase inputs, and the
     * inverse STFT, trimmed to 600 samples per frame. */
    nn_leaky_relu(x, (size_t)T1 * GEN_W1, 0.01f);
    if (qconv(&s, &post, pb, x, T1, GEN_W1, GEN_W1, HAR, a, HAR) != 0) goto out;
    nn_istft(a, T1, HAR, STFT_BINS, ir, ii, STFT_N, STFT_HOP, STFT_N / 2, pcm, full);
    rc = 0;
out:
    if (rc != 0) {
        if (w.missing) ferr(err, errcap, "voice data: no tensor %s of the expected size", w.missing);
        else if (err && errcap && !err[0]) ferr(err, errcap, "out of memory or malformed voice data");
    }
    free(x); free(src); free(a); free(b); free(c); free(full);
    free(s.q); free(s.acc); free(s.wbuf);
    return rc;
}

/* ---- the whole forward pass ---------------------------------------------------------------- */

int voice_style(const voice *v, const char *id, size_t row, float *style) {
    char name[VOICE_NAME_MAX];
    voice_tensor t;
    if (!v || !id || snprintf(name, sizeof name, "voice.%s", id) >= (int)sizeof name || !voice_tensor_get(v, name, &t) ||
        t.dtype != VOICE_F32 || t.ndim != 2 || t.dims[1] != VOICE_STYLE_DIM || t.dims[0] < 1)
        return -1;
    if (row >= (size_t)t.dims[0]) row = (size_t)t.dims[0] - 1;
    memcpy(style, (const float *)t.data + row * VOICE_STYLE_DIM, sizeof(float) * VOICE_STYLE_DIM);
    return 0;
}

float *voice_say(const voice *v, const int *ids, int tokens, const float *style, float speed, int noise,
                 unsigned long long seed, size_t *samples, char *err, size_t errcap) {
    voice_front f;
    if (err && errcap) err[0] = '\0';
    if (samples) *samples = 0;
    if (!style || voice_front_run(v, ids, tokens, style, speed, &f, err, errcap) != 0) {
        if (!style) ferr(err, errcap, "nothing to say");
        return NULL;
    }
    int F = f.frames;
    size_t n = (size_t)F * VOICE_FRAME;
    float *dec = malloc(sizeof(float) * (size_t)2 * F * DEC_OUT), *har = malloc(sizeof(float) * (120 * (size_t)F + 1) * HAR);
    float *pcm = malloc(sizeof(float) * n);
    int ok = dec && har && pcm;
    if (!ok) ferr(err, errcap, "out of memory");
    /* The decoder and generator read the style row's first half. */
    ok = ok && voice_decode(v, &f, style, dec, err, errcap) == 0 &&
         voice_source(v, f.f0, F, noise, (uint64_t)seed, har, NULL, err, errcap) == 0 &&
         voice_generate(v, dec, har, F, style, pcm, err, errcap) == 0;
    voice_front_free(&f);
    free(dec); free(har);
    if (!ok) {
        free(pcm);
        return NULL;
    }
    if (samples) *samples = n;
    return pcm;
}
