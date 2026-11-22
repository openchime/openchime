/* Tests for nnkit, the neural network kernels read-aloud's voice model and
 * voice input's recognizer run on (nnkit/, docs/details/TTSKIT.md §5b, docs/details/STTKIT.md):
 * each kernel against a naive version written from the operator's definition,
 * on random inputs from a fixed seed, and the bits the reference's own
 * approximations give where the models' references depend on them. */
#include "check.h"
#include "nnkit.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the kernels against naive versions --------------------------------------
 * Random inputs from a fixed seed. The integer paths are compared exactly; the
 * float paths within a tolerance stated at each check (the kernels sum in a
 * different order from the naive loops, which sum in double). */

static uint32_t g_rng = 0x9e3779b9u;
static uint32_t rnd(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}
static int rnd_int(int lo, int hi) { return lo + (int)(rnd() % (uint32_t)(hi - lo + 1)); }
static float rnd_f(float lo, float hi) { return lo + (hi - lo) * (float)(rnd() >> 8) / 16777216.0f; }

/* DynamicQuantizeLinear, written from the operator's definition. */
static void dql_naive(const float *x, size_t n, uint8_t *q, float *scale, int *zp) {
    float mn = 0, mx = 0;
    for (size_t i = 0; i < n; i++) {
        if (x[i] < mn) mn = x[i];
        if (x[i] > mx) mx = x[i];
    }
    *scale = (mx - mn) / 255.0f;
    if (*scale == 0) {
        *zp = 0;
        memset(q, 0, n);
        return;
    }
    float z = 0.0f - mn / *scale;
    z = z < 0 ? 0 : z > 255 ? 255 : z;
    *zp = (int)nearbyintf(z);
    for (size_t i = 0; i < n; i++) {
        int v = (int)nearbyintf(x[i] / *scale) + *zp;
        q[i] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
}

static int dql_matches(const float *x, size_t n) {
    uint8_t a[512], b[512];
    float s;
    int zp;
    nn_qparam qp;
    dql_naive(x, n, a, &s, &zp);
    nn_dql_u8(x, n, b, &qp);
    int ok = qp.scale == s && qp.zp == zp && memcmp(a, b, n) == 0;
    if (!ok) printf("    dql: scale %g/%g zp %d/%d\n", (double)qp.scale, (double)s, qp.zp, zp);
    return ok;
}

static int dql_checks(void) {
    int ok = 1;
    /* Scale 1, zero point 0: halves round to even. */
    const float ties[] = { 0, 0.5f, 1.5f, 2.5f, 3.5f, 254.5f, 255, 100.25f };
    uint8_t q[8];
    nn_qparam qp;
    nn_dql_u8(ties, 8, q, &qp);
    const uint8_t want[] = { 0, 0, 2, 2, 4, 254, 255, 100 };
    ok &= qp.scale == 1.0f && qp.zp == 0 && memcmp(q, want, 8) == 0;
    /* A zero point of 1.5 rounds to 2. */
    const float zt[] = { -1.5f, 253.5f, 0.5f, -0.5f };
    nn_dql_u8(zt, 4, q, &qp);
    ok &= qp.scale == 1.0f && qp.zp == 2;
    ok &= dql_matches(zt, 4);
    /* The range always includes 0: all positive, all negative, all zero. */
    float x[512];
    for (int i = 0; i < 300; i++) x[i] = rnd_f(0.5f, 3.0f);
    ok &= dql_matches(x, 300);
    for (int i = 0; i < 300; i++) x[i] = -x[i];
    ok &= dql_matches(x, 300);
    memset(x, 0, sizeof x);
    ok &= dql_matches(x, 37);
    for (int round = 0; round < 20; round++) {
        size_t n = (size_t)rnd_int(1, 512);
        float lo = rnd_f(-8, 0), hi = rnd_f(0, 8);
        for (size_t i = 0; i < n; i++) x[i] = rnd_f(lo, hi);
        ok &= dql_matches(x, n);
    }
    /* The int16 form: q - zp, rows padded with zeros. */
    float m[5 * 20];
    int16_t q16[5 * 32];
    uint8_t q8[5 * 20];
    for (int i = 0; i < 100; i++) m[i] = rnd_f(-3, 5);
    nn_quantize(m, 5, 20, 20, q16, 32, &qp);
    float s;
    int zp;
    dql_naive(m, 100, q8, &s, &zp);
    ok &= qp.scale == s && qp.zp == zp;
    for (int r = 0; r < 5; r++)
        for (int c = 0; c < 32; c++) ok &= q16[r * 32 + c] == (c < 20 ? q8[r * 20 + c] - zp : 0);
    return ok;
}

/* ConvInteger from its definition, on the graph's layout W[co][ci][j]. */
static int conv_case(int cin, int cout, int k, int stride, int dil, int t_out, int wzp) {
    int cp = NN_ROUND(cin, NN_CPAD), co4 = NN_ROUND(cout, 4);
    int rows = (t_out - 1) * stride + (k - 1) * dil + 1;
    int16_t *x = calloc((size_t)rows * cp, sizeof *x);
    uint8_t *w = malloc((size_t)cout * cin * k), *wp = malloc((size_t)co4 * k * cp);
    int32_t *y = malloc((size_t)t_out * cout * sizeof *y);
    int16_t *wbuf = malloc(NN_WBUF(k, cp) * sizeof *wbuf);
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cin; c++) x[r * cp + c] = (int16_t)(rnd() % 7 == 0 ? (rnd() & 1 ? 255 : -255) : rnd_int(-255, 255));
    for (int i = 0; i < cout * cin * k; i++) w[i] = (uint8_t)(rnd() % 9 == 0 ? (rnd() & 1 ? 255 : 0) : rnd_int(0, 255));
    memset(wp, wzp, (size_t)co4 * k * cp);
    for (int co = 0; co < cout; co++)
        for (int ci = 0; ci < cin; ci++)
            for (int j = 0; j < k; j++) wp[((size_t)co * k + j) * cp + ci] = w[(co * cin + ci) * k + j];
    nn_conv_q(x, t_out, stride, dil, k, cp, wp, wzp, cout, y, cout, wbuf);
    int ok = 1;
    for (int t = 0; t < t_out; t++)
        for (int co = 0; co < cout; co++) {
            int64_t s = 0;
            for (int j = 0; j < k; j++)
                for (int ci = 0; ci < cin; ci++)
                    s += (int64_t)x[(t * stride + j * dil) * cp + ci] * (w[(co * cin + ci) * k + j] - wzp);
            if (s != y[t * cout + co]) ok = 0;
        }
    if (!ok) printf("    conv cin %d cout %d k %d stride %d dil %d t %d wzp %d differs\n", cin, cout, k, stride, dil, t_out, wzp);
    free(x); free(w); free(wp); free(y); free(wbuf);
    return ok;
}

/* MatMulInteger from its definition, on the graph's layout B[k][n]. */
static int matmul_case(int m, int k, int n, int bzp) {
    int kp = NN_ROUND(k, NN_CPAD), n4 = NN_ROUND(n, 4);
    int16_t *a = calloc((size_t)m * kp, sizeof *a);
    int8_t *b = malloc((size_t)k * n), *bp = malloc((size_t)n4 * kp);
    int32_t *y = malloc((size_t)m * n * sizeof *y);
    int16_t *wbuf = malloc(NN_WBUF(1, kp) * sizeof *wbuf);
    for (int r = 0; r < m; r++)
        for (int i = 0; i < k; i++) a[r * kp + i] = (int16_t)rnd_int(-255, 255);
    for (int i = 0; i < k * n; i++) b[i] = (int8_t)(rnd() % 9 == 0 ? (rnd() & 1 ? 127 : -128) : rnd_int(-128, 127));
    memset(bp, (unsigned char)(int8_t)bzp, (size_t)n4 * kp);
    for (int i = 0; i < k; i++)
        for (int c = 0; c < n; c++) bp[(size_t)c * kp + i] = b[i * n + c];
    nn_matmul_q(a, m, kp, bp, bzp, n, y, n, wbuf);
    int ok = 1;
    for (int r = 0; r < m; r++)
        for (int c = 0; c < n; c++) {
            int64_t s = 0;
            for (int i = 0; i < k; i++) s += (int64_t)a[r * kp + i] * (b[i * n + c] - bzp);
            if (s != y[r * n + c]) ok = 0;
        }
    if (!ok) printf("    matmul m %d k %d n %d bzp %d differs\n", m, k, n, bzp);
    free(a); free(b); free(bp); free(y); free(wbuf);
    return ok;
}

static float sigm(float x) { return 1.0f / (1.0f + expf(-x)); }

/* DynamicQuantizeLSTM as the reference defines it, from the contrib op's layouts
 * (W [dir][in][4H], R [dir][H][4H]), one step at a time. */
static int lstm_case(int t, int in, int hidden, int dirs) {
    int ip = NN_ROUND(in, NN_CPAD), G = 4 * hidden;
    float *x = malloc(sizeof(float) * (size_t)t * in), *y = malloc(sizeof(float) * (size_t)t * 2 * hidden);
    float *ref = malloc(sizeof(float) * (size_t)t * 2 * hidden);
    int8_t *w[2], *r[2], *wp[2], *rp[2];
    float *b[2];
    nn_lstm l = { .in_pad = ip, .hidden = hidden };
    for (int d = 0; d < 2; d++) {
        w[d] = malloc((size_t)in * G); r[d] = malloc((size_t)hidden * G);
        wp[d] = malloc((size_t)G * ip); rp[d] = malloc((size_t)G * hidden);
        b[d] = malloc(sizeof(float) * 2 * G);
        for (int i = 0; i < in * G; i++) w[d][i] = (int8_t)rnd_int(-127, 127);
        for (int i = 0; i < hidden * G; i++) r[d][i] = (int8_t)rnd_int(-127, 127);
        for (int i = 0; i < 2 * G; i++) b[d][i] = rnd_f(-0.3f, 0.3f);
        l.wzp[d] = d ? -2 : 0;
        l.rzp[d] = d ? 3 : 0;
        l.wscale[d] = rnd_f(0.002f, 0.01f);
        l.rscale[d] = rnd_f(0.002f, 0.01f);
        memset(wp[d], 0, (size_t)G * ip);
        for (int i = 0; i < in; i++)
            for (int g = 0; g < G; g++) wp[d][(size_t)g * ip + i] = w[d][i * G + g];
        for (int i = 0; i < hidden; i++)
            for (int g = 0; g < G; g++) rp[d][(size_t)g * hidden + i] = r[d][i * G + g];
        l.w[d] = wp[d]; l.r[d] = rp[d]; l.b[d] = b[d];
    }
    for (int i = 0; i < t * in; i++) x[i] = rnd_f(-2, 2);
    void *scratch = malloc(nn_lstm_scratch(t, ip, hidden));
    nn_lstm_run(&l, dirs, x, t, in, in, y, 2 * hidden, scratch);

    uint8_t *qx = malloc((size_t)t * in), qh[64];
    float xs, hs;
    int xz, hz;
    dql_naive(x, (size_t)t * in, qx, &xs, &xz);
    float h[64], c[64], g[256];
    for (int d = 0; d < dirs; d++) {
        memset(h, 0, sizeof h);
        memset(c, 0, sizeof c);
        for (int s = 0; s < t; s++) {
            int step = d ? t - 1 - s : s;
            dql_naive(h, (size_t)hidden, qh, &hs, &hz);
            for (int j = 0; j < G; j++) {
                int32_t ax = 0, ah = 0;
                for (int i = 0; i < in; i++) ax += (qx[step * in + i] - xz) * (w[d][i * G + j] - l.wzp[d]);
                for (int i = 0; i < hidden; i++) ah += (qh[i] - hz) * (r[d][i * G + j] - l.rzp[d]);
                float gx = (float)ax * (xs * l.wscale[d]);
                g[j] = (float)ah * (hs * l.rscale[d]) + gx;
                g[j] = g[j] + (b[d][j] + b[d][G + j]);
            }
            for (int j = 0; j < hidden; j++) {
                float gi = sigm(g[j]), go = sigm(g[hidden + j]), gf = sigm(g[2 * hidden + j]);
                float gc = tanhf(g[3 * hidden + j]);
                c[j] = c[j] * gf + gi * gc;
                h[j] = go * tanhf(c[j]);
                ref[step * 2 * hidden + d * hidden + j] = h[j];
            }
        }
    }
    int ok = 1;
    for (int s = 0; s < t; s++)
        for (int j = 0; j < dirs * hidden; j++)
            if (fabsf(y[s * 2 * hidden + j] - ref[s * 2 * hidden + j]) > 1e-6f) ok = 0;
    if (!ok) printf("    lstm t %d in %d hidden %d dirs %d differs\n", t, in, hidden, dirs);
    for (int d = 0; d < 2; d++) { free(w[d]); free(r[d]); free(wp[d]); free(rp[d]); free(b[d]); }
    free(x); free(y); free(ref); free(scratch); free(qx);
    return ok;
}

static int close_to(double got, double want, double tol) { return fabs(got - want) <= tol * (1.0 + fabs(want)); }

static int norm_checks(void) {
    int ok = 1;
    /* Layer norm over each row: 37 (a tail) and 64 columns. */
    for (int cols = 37; cols <= 64; cols += 27) {
        float x[3 * 64], g[64], b[64], ref[3 * 64];
        for (int i = 0; i < 3 * cols; i++) x[i] = rnd_f(-3, 4);
        for (int i = 0; i < cols; i++) { g[i] = rnd_f(0.5f, 1.5f); b[i] = rnd_f(-1, 1); }
        for (int r = 0; r < 3; r++) {
            double m = 0, v = 0;
            for (int c = 0; c < cols; c++) m += x[r * cols + c];
            m /= cols;
            for (int c = 0; c < cols; c++) v += (x[r * cols + c] - m) * (x[r * cols + c] - m);
            v /= cols;
            for (int c = 0; c < cols; c++) ref[r * cols + c] = (float)((x[r * cols + c] - m) / sqrt(v + 1e-5) * g[c] + b[c]);
        }
        nn_layernorm(x, 3, cols, cols, g, b, 1e-5f);
        for (int i = 0; i < 3 * cols; i++) ok &= close_to(x[i], ref[i], 1e-5);   /* tolerance 1e-5 */
    }
    /* Instance norm over time: 300 steps (more than one run of 256), 70
     * channels (a block of 64 and a tail), with and without the affine. */
    int t = 300, c = 70;
    float *x = malloc(sizeof(float) * t * c), *ref = malloc(sizeof(float) * t * c), g[70], b[70];
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < t * c; i++) x[i] = rnd_f(-2, 6);
        for (int i = 0; i < c; i++) { g[i] = rnd_f(0.5f, 1.5f); b[i] = rnd_f(-1, 1); }
        for (int ch = 0; ch < c; ch++) {
            double m = 0, v = 0;
            for (int r = 0; r < t; r++) m += x[r * c + ch];
            m /= t;
            for (int r = 0; r < t; r++) v += (x[r * c + ch] - m) * (x[r * c + ch] - m);
            v /= t;
            for (int r = 0; r < t; r++)
                ref[r * c + ch] = (float)((x[r * c + ch] - m) / sqrt(v + 1e-5) * (pass ? g[ch] : 1) + (pass ? b[ch] : 0));
        }
        nn_instnorm(x, t, c, c, pass ? g : NULL, pass ? b : NULL, 1e-5f);
        for (int i = 0; i < t * c; i++) ok &= close_to(x[i], ref[i], 1e-5);   /* tolerance 1e-5 */
    }
    free(x);
    free(ref);
    return ok;
}

static int attention_checks(void) {
    enum { N = 7, HEADS = 3, DH = 20, LD = HEADS * DH };
    float q[N * LD], k[N * LD], v[N * LD], out[N * LD], mask[N], scratch[N * (DH + 1) + DH];
    for (int i = 0; i < N * LD; i++) { q[i] = rnd_f(-1, 1); k[i] = rnd_f(-1, 1); v[i] = rnd_f(-2, 2); }
    for (int j = 0; j < N; j++) mask[j] = j == 3 ? -10000.0f : 0.0f;
    int ok = 1;
    for (int masked = 0; masked < 2; masked++) {
        /* q and k are each scaled by the root of the 0.125 the product is. */
        nn_attention(q, k, v, LD, N, HEADS, DH, sqrtf(0.125f), masked ? mask : NULL, out, LD, scratch);
        for (int h = 0; h < HEADS; h++)
            for (int i = 0; i < N; i++) {
                double s[N], mx = -1e30, sum = 0;
                for (int j = 0; j < N; j++) {
                    s[j] = 0;
                    for (int d = 0; d < DH; d++) s[j] += (double)q[i * LD + h * DH + d] * k[j * LD + h * DH + d];
                    s[j] = s[j] * 0.125 + (masked ? mask[j] : 0);
                    if (s[j] > mx) mx = s[j];
                }
                for (int j = 0; j < N; j++) sum += (s[j] = exp(s[j] - mx));
                for (int d = 0; d < DH; d++) {
                    double o = 0;
                    for (int j = 0; j < N; j++) o += s[j] / sum * v[j * LD + h * DH + d];
                    ok &= close_to(out[i * LD + h * DH + d], o, 1e-5);   /* tolerance 1e-5 */
                }
            }
    }
    return ok;
}

/* ConvTranspose from its definition: input i, tap j lands on output i*s + j - pad. */
static int convt_case(int t, int cin, int cout, int k, int s, int pad_begin, int pad_end) {
    int mt = k / s, np = NN_ROUND(cout, 8), t_out = (t - 1) * s - pad_begin - pad_end + k;
    float *x = calloc((size_t)(t + 2 * mt) * cin, sizeof(float)), *w = malloc(sizeof(float) * cin * cout * k);
    float *wp = calloc((size_t)s * mt * cin * np, sizeof(float)), *bias = malloc(sizeof(float) * cout);
    float *y = malloc(sizeof(float) * t_out * cout);
    double *ref = calloc((size_t)t_out * cout, sizeof(double));
    float *xv = x + (size_t)(mt - 1) * cin;
    for (int i = 0; i < t * cin; i++) xv[i] = rnd_f(-1, 1);
    for (int i = 0; i < cin * cout * k; i++) w[i] = rnd_f(-1, 1);
    for (int i = 0; i < cout; i++) bias[i] = rnd_f(-1, 1);
    for (int r = 0; r < s; r++)
        for (int m = 0; m < mt; m++)
            for (int ci = 0; ci < cin; ci++)
                for (int co = 0; co < cout; co++)
                    wp[(((size_t)r * mt + m) * cin + ci) * np + co] = w[(ci * cout + co) * k + r + (mt - 1 - m) * s];
    nn_convt(x, t, cin, k, s, pad_begin, wp, bias, cout, t_out, y, cout);
    for (int i = 0; i < t; i++)
        for (int j = 0; j < k; j++) {
            int o = i * s + j - pad_begin;
            if (o < 0 || o >= t_out) continue;
            for (int co = 0; co < cout; co++)
                for (int ci = 0; ci < cin; ci++) ref[o * cout + co] += (double)xv[i * cin + ci] * w[(ci * cout + co) * k + j];
        }
    int ok = 1;
    for (int o = 0; o < t_out; o++)
        for (int co = 0; co < cout; co++) ok &= close_to(y[o * cout + co], ref[o * cout + co] + bias[co], 1e-5);
    if (!ok) printf("    convt t %d cin %d cout %d k %d s %d pads %d %d differs\n", t, cin, cout, k, s, pad_begin, pad_end);
    free(x); free(w); free(wp); free(bias); free(y); free(ref);
    return ok;
}

static int convt_dw_case(int t, int c, int k, int s, int pad_begin, int out_pad) {
    int t_out = (t - 1) * s - 2 * pad_begin + k + out_pad;
    float *x = malloc(sizeof(float) * t * c), *w = malloc(sizeof(float) * k * c), *bias = malloc(sizeof(float) * c);
    float *y = malloc(sizeof(float) * t_out * c);
    for (int i = 0; i < t * c; i++) x[i] = rnd_f(-1, 1);
    for (int i = 0; i < k * c; i++) w[i] = rnd_f(-1, 1);
    for (int i = 0; i < c; i++) bias[i] = rnd_f(-1, 1);
    nn_convt_dw(x, t, c, c, k, s, pad_begin, w, bias, t_out, y, c);
    int ok = 1;
    for (int o = 0; o < t_out; o++)
        for (int ch = 0; ch < c; ch++) {
            double r = bias[ch];
            for (int i = 0; i < t; i++)
                for (int j = 0; j < k; j++)
                    if (i * s + j - pad_begin == o) r += (double)x[i * c + ch] * w[j * c + ch];
            ok &= close_to(y[o * c + ch], r, 1e-6);   /* tolerance 1e-6 */
        }
    free(x); free(w); free(bias); free(y);
    return ok;
}

static int matmul_f_checks(void) {
    /* 11 rows (a tile of 8 and 3 more), 10 columns (padded to 16), windows of 13
     * values 5 apart (overlapping, as a strided convolution reads them). */
    enum { M = 11, K = 13, N = 10, NP = 16, XS = 5 };
    float x[(M - 1) * XS + K], w[K * NP], bias[N], y[M * N];
    for (size_t i = 0; i < sizeof x / sizeof *x; i++) x[i] = rnd_f(-1, 1);
    for (int i = 0; i < K * NP; i++) w[i] = i % NP < N ? rnd_f(-1, 1) : 0;
    for (int i = 0; i < N; i++) bias[i] = rnd_f(-1, 1);
    nn_matmul_f(x, XS, M, K, w, N, bias, y, N);
    int ok = 1;
    for (int r = 0; r < M; r++)
        for (int c = 0; c < N; c++) {
            double s = bias[c];
            for (int i = 0; i < K; i++) s += (double)x[r * XS + i] * w[i * NP + c];
            ok &= close_to(y[r * N + c], s, 1e-5);   /* tolerance 1e-5 */
        }
    return ok;
}

static int elementwise_checks(void) {
    int ok = 1;
    float x[200], y[200], keep[200];
    /* LeakyRelu: exact. */
    for (int i = 0; i < 200; i++) keep[i] = x[i] = i % 10 == 0 ? 0.0f : rnd_f(-5, 5);
    nn_leaky_relu(x, 199, 0.2f);
    for (int i = 0; i < 199; i++) ok &= x[i] == (keep[i] < 0 ? keep[i] * 0.2f : keep[i]);
    ok &= x[199] == keep[199];
    /* sin: within 3e-7 of the true value for |x| <= 100, 1e-6 to 1e4. */
    double worst = 0, worst_far = 0;
    for (int i = 0; i < 200; i++) x[i] = rnd_f(-100, 100);
    x[0] = 0; x[1] = 1.5707963f; x[2] = -3.1415927f; x[3] = 3.1415927f; x[4] = 0.5f;
    nn_sin(x, y, 200);
    for (int i = 0; i < 200; i++) worst = fmax(worst, fabs(y[i] - sin((double)x[i])));
    for (int i = 0; i < 200; i++) x[i] = rnd_f(-1e4f, 1e4f);
    nn_sin(x, y, 197);
    for (int i = 0; i < 197; i++) worst_far = fmax(worst_far, fabs(y[i] - sin((double)x[i])));
    if (worst > 3e-7 || worst_far > 1e-6) printf("    sin: worst error %.3g near, %.3g far\n", worst, worst_far);
    ok &= worst <= 3e-7 && worst_far <= 1e-6;
    /* Snake on 9 rows of 37 channels (two blocks of 16 and a tail). */
    float a[37], inv[37], z[9 * 37], zk[9 * 37];
    for (int c = 0; c < 37; c++) { a[c] = rnd_f(0.5f, 1.5f); inv[c] = 1.0f / a[c]; }
    for (int i = 0; i < 9 * 37; i++) zk[i] = z[i] = rnd_f(-4, 4);
    nn_snake(z, 9, 37, 37, a, inv);
    for (int i = 0; i < 9 * 37; i++) {
        double s = sin((double)a[i % 37] * zk[i]);
        ok &= close_to(z[i], zk[i] + inv[i % 37] * s * s, 1e-6);   /* tolerance 1e-6 */
    }
    return ok;
}

/* sin and atan as the reference computes them, to the bit: outputs its CPU
 * kernels gave for these inputs (where the C library's answer differs, past the
 * huge-argument threshold, and atan both sides of 1). atan leaves the elements
 * past the last whole vector of four to the C library, as its kernel does. */
static int ort_bits_checks(void) {
    static const uint32_t SIN[][2] = {
        { 0x3ec1d616, 0x3ebd3d2e }, { 0xc07fced9, 0x3f413d12 }, { 0x3f41d616, 0x3f2fd6ae }, { 0x46ea6000, 0xbf4d7b7c },
        { 0xc781df0f, 0x3f3d69f0 }, { 0x47c35000, 0x3d126d55 }, { 0x46cadb00, 0xbe70244f }, { 0x3fc90fdb, 0x3f800000 } };
    static const uint32_t ATAN[][2] = {
        { 0xc0400000, 0xbf9fe0bc }, { 0xc03fef9e, 0xbf9fdd75 }, { 0x3f000000, 0x3eed6336 }, { 0x3727c5ac, 0x3727c5ac },
        { 0x447a0000, 0x3fc8ef16 }, { 0xbed1eb85, 0xbec737c1 }, { 0x40e80000, 0x3fb78479 }, { 0x3f7d70a4, 0x3f47c687 } };
    float x[9], y[9];
    int ok = 1;
    for (int i = 0; i < 8; i++) memcpy(&x[i], &SIN[i][0], 4);
    nn_sin(x, y, 8);
    for (int i = 0; i < 8; i++) ok &= memcmp(&y[i], &SIN[i][1], 4) == 0;
    for (int i = 0; i < 8; i++) memcpy(&x[i], &ATAN[i][0], 4);
    x[8] = 0.3f;
    nn_atan(x, y, 9);
    for (int i = 0; i < 8; i++) ok &= memcmp(&y[i], &ATAN[i][1], 4) == 0;
    ok &= y[8] == atanf(0.3f);
    return ok;
}

/* InstanceNormalization over time-major rows against the same arithmetic on
 * each channel's column (ReduceSum's order from where the channel starts in a
 * channel-major tensor), bit for bit: t a multiple of 4, 2 mod 4 and odd (one,
 * two and four classes of channel), and fewer than 8 steps. */
static int instnorm_order_case(int t, int c) {
    float *x = malloc(sizeof(float) * t * c), *ref = malloc(sizeof(float) * t * c), *col = malloc(sizeof(float) * t * 2);
    int ok = x && ref && col;
    for (int i = 0; ok && i < t * c; i++) ref[i] = x[i] = rnd_f(-2, 6);
    for (int ch = 0; ok && ch < c; ch++) {
        float *sq = col + t;
        for (int r = 0; r < t; r++) col[r] = ref[r * c + ch];
        float mean = nn_reduce_sum(col, t, (size_t)ch * t) / (float)t;
        for (int r = 0; r < t; r++) sq[r] = (col[r] - mean) * (col[r] - mean);
        float k = 1.0f / sqrtf(nn_reduce_sum(sq, t, 0) / (float)t + 1e-5f), sh = 0.0f - mean * k;
        for (int r = 0; r < t; r++) ref[r * c + ch] = col[r] * k + sh;
    }
    if (ok) nn_instnorm(x, t, c, c, NULL, NULL, 1e-5f);
    for (int i = 0; ok && i < t * c; i++) ok &= memcmp(&x[i], &ref[i], 4) == 0;
    if (!ok) printf("    instnorm t %d c %d differs from the column order\n", t, c);
    free(x); free(ref); free(col);
    return ok;
}

/* The generator's inverse STFT as the graph has it: Exp, Sin, Cos and Sin,
 * two transposed convolutions, their difference, the ends sliced off. */
static int istft_checks(void) {
    enum { T = 9, BINS = 11, NFFT = 20, HOP = 5, TRIM = 10, LD = 24, LEN = (T - 1) * HOP + NFFT };
    float spec[T * LD], wr[BINS * NFFT], wi[BINS * NFFT], out[LEN - 2 * TRIM], full[LEN];
    for (int i = 0; i < T * LD; i++) spec[i] = rnd_f(-2, 2);
    for (int i = 0; i < BINS * NFFT; i++) { wr[i] = rnd_f(-0.1f, 0.1f); wi[i] = rnd_f(-0.1f, 0.1f); }
    nn_istft(spec, T, LD, BINS, wr, wi, NFFT, HOP, TRIM, out, full);
    double re[LEN] = { 0 }, im[LEN] = { 0 };
    for (int f = 0; f < T; f++)
        for (int b = 0; b < BINS; b++) {
            double m = exp((double)spec[f * LD + b]), ph = sin((double)spec[f * LD + BINS + b]);
            for (int j = 0; j < NFFT; j++) {
                re[f * HOP + j] += m * cos(ph) * wr[b * NFFT + j];
                im[f * HOP + j] += m * sin(ph) * wi[b * NFFT + j];
            }
        }
    int ok = 1;
    for (int i = 0; i < LEN - 2 * TRIM; i++) ok &= close_to(out[i], re[i + TRIM] - im[i + TRIM], 1e-5);   /* 1e-5 */
    return ok;
}

/* ---- the recognizer's kernels ---------------------------------------------------- */

/* Integer products with a zero point per column: exact. */
static int matmul_qc_case(int m, int k, int n) {
    int16_t *a = malloc((size_t)m * k * sizeof *a), *wbuf = malloc(NN_WBUF(1, k) * sizeof *wbuf);
    uint8_t *b = malloc((size_t)n * k), *zp = malloc((size_t)n);
    int32_t *y = malloc((size_t)m * n * sizeof *y);
    for (int i = 0; i < m * k; i++) a[i] = (int16_t)rnd_int(-255, 255);
    for (int i = 0; i < n * k; i++) b[i] = (uint8_t)rnd_int(0, 255);
    for (int c = 0; c < n; c++) zp[c] = (uint8_t)(c % 3 == 0 ? 0 : c % 3 == 1 ? 255 : rnd_int(0, 255));
    nn_matmul_qc(a, m, k, b, zp, n, y, n, wbuf);
    int ok = 1;
    for (int r = 0; r < m; r++)
        for (int c = 0; c < n; c++) {
            int64_t want = 0;
            for (int i = 0; i < k; i++) want += (int64_t)a[r * k + i] * (b[c * k + i] - zp[c]);
            ok &= y[r * n + c] == want;
        }
    float *f = malloc((size_t)m * n * sizeof *f), sc[8] = { 0.5f, 0.25f, 2.0f, 0.125f, 1.0f, 3.0f, 0.75f, 0.0625f };
    float *bs = malloc((size_t)n * sizeof *bs), *bias = malloc((size_t)n * sizeof *bias);
    for (int c = 0; c < n; c++) { bs[c] = sc[c % 8]; bias[c] = (float)c; }
    nn_dequant_qc(y, m, n, n, 0.01f, bs, bias, f, n);
    for (int r = 0; r < m; r++)
        for (int c = 0; c < n; c++) {
            float mult = 0.01f * bs[c];
            float v = (float)y[r * n + c] * mult;
            ok &= f[r * n + c] == v + bias[c];
        }
    free(a); free(wbuf); free(b); free(zp); free(y); free(f); free(bs); free(bias);
    return ok;
}

static int recognizer_elementwise_checks(void) {
    int ok = 1;
    float x[100], y[100];
    for (int i = 0; i < 100; i++) x[i] = y[i] = rnd_f(-9, 9);
    nn_silu(y, 100);
    for (int i = 0; i < 100; i++) ok &= close_to(y[i], x[i] / (1.0 + exp(-(double)x[i])), 2e-6);     /* 2e-6 */
    for (int i = 0; i < 100; i++) y[i] = x[i];
    nn_gelu_erf(y, 100);
    for (int i = 0; i < 100; i++) ok &= close_to(y[i], 0.5 * x[i] * (1.0 + erf(x[i] / sqrt(2.0))), 2e-6);   /* 2e-6 */
    /* Rotary positions: the first rot dimensions turn in pairs, the rest stay. */
    float inv[16], h[40], g[40];
    for (int i = 0; i < 16; i++) inv[i] = (float)pow(10000.0, -(double)i / 16);
    for (int i = 0; i < 40; i++) h[i] = g[i] = rnd_f(-2, 2);
    nn_rope_pairs(h, 32, inv, 37.0f);
    for (int i = 0; i < 16; i++) {
        double f = 37.0 * inv[i], a = g[2 * i], b = g[2 * i + 1];
        ok &= close_to(h[2 * i], a * cos(f) - b * sin(f), 1e-5);       /* 1e-5 */
        ok &= close_to(h[2 * i + 1], b * cos(f) + a * sin(f), 1e-5);   /* 1e-5 */
    }
    for (int i = 32; i < 40; i++) ok &= h[i] == g[i];
    /* argmax: the first of equals. */
    float z[6] = { 1, 5, 2, 5, -1, 0 };
    ok &= nn_argmax(z, 6) == 1;
    return ok;
}

/* One query against a window of keys, with a cache wider than the head. */
static int attend_case(int n, int lo, int hi, int dh, int ld) {
    float *q = malloc((size_t)dh * sizeof *q), *k = malloc((size_t)n * ld * sizeof *k), *v = malloc((size_t)n * ld * sizeof *v);
    float *out = malloc((size_t)dh * sizeof *out), *scr = malloc((size_t)n * sizeof *scr);
    for (int i = 0; i < dh; i++) q[i] = rnd_f(-1, 1);
    for (int i = 0; i < n * ld; i++) { k[i] = rnd_f(-1, 1); v[i] = rnd_f(-1, 1); }
    float scale = 1.0f / sqrtf((float)dh);
    nn_attend(q, k, ld, v, ld, lo, hi, dh, scale, out, scr);
    double s[64], mx = -1e30, sum = 0;
    for (int j = lo; j < hi; j++) {
        double d = 0;
        for (int i = 0; i < dh; i++) d += (double)q[i] * k[j * ld + i];
        s[j - lo] = d * scale;
        if (s[j - lo] > mx) mx = s[j - lo];
    }
    for (int j = 0; j < hi - lo; j++) { s[j] = exp(s[j] - mx); sum += s[j]; }
    int ok = 1;
    for (int i = 0; i < dh; i++) {
        double want = 0;
        for (int j = lo; j < hi; j++) want += s[j - lo] / sum * v[j * ld + i];
        ok &= close_to(out[i], want, 1e-5);   /* 1e-5 */
    }
    free(q); free(k); free(v); free(out); free(scr);
    return ok;
}

static int matvec_u8_case(int k, int n) {
    float *x = malloc((size_t)k * sizeof *x), *out = malloc((size_t)n * sizeof *out);
    uint8_t *e = malloc((size_t)n * k);
    for (int i = 0; i < k; i++) x[i] = rnd_f(-3, 3);
    for (int i = 0; i < n * k; i++) e[i] = (uint8_t)rnd_int(0, 255);
    nn_matvec_u8(x, k, e, 91, 0.0105f, n, out);
    int ok = 1;
    for (int r = 0; r < n; r++) {
        double want = 0;
        for (int i = 0; i < k; i++) want += (double)x[i] * ((double)(e[r * k + i] - 91) * 0.0105f);
        ok &= close_to(out[r], want, 1e-4);   /* 1e-4 */
    }
    free(x); free(out); free(e);
    return ok;
}

/* ---- the summary model's kernels ---------------------------------------------
 * The quantised products against a naive version written from ggml's block
 * layouts, exactly; the float kernels against double within a tolerance. */

static uint16_t rnd_f16(float lo, float hi) { return nn_f32_to_f16(rnd_f(lo, hi)); }

/* A random Q4_K or Q6_K block: any bytes are a block, given finite scales. */
static void rnd_block(nn_wtype t, uint8_t *b) {
    if (t == NN_W_Q4K) {
        uint16_t d = rnd_f16(0.0005f, 0.01f), dm = rnd_f16(0.0f, 0.01f);
        b[0] = (uint8_t)d; b[1] = (uint8_t)(d >> 8); b[2] = (uint8_t)dm; b[3] = (uint8_t)(dm >> 8);
        for (int i = 4; i < NN_Q4K_BYTES; i++) b[i] = (uint8_t)rnd();
    } else {
        for (int i = 0; i < 208; i++) b[i] = (uint8_t)rnd();
        uint16_t d = rnd_f16(0.0005f, 0.01f);
        b[208] = (uint8_t)d; b[209] = (uint8_t)(d >> 8);
    }
}

/* ggml's dequantisation, written from its reference code, as integers: value e
 * is d * sc[e] * q[e] - dmin * mn[e]. */
static void naive_unpack(nn_wtype t, const uint8_t *b, int *q, int *sc, int *mn, float *d, float *dmin) {
    if (t == NN_W_Q4K) {
        *d = nn_f16_to_f32((uint16_t)(b[0] | b[1] << 8));
        *dmin = nn_f16_to_f32((uint16_t)(b[2] | b[3] << 8));
        const uint8_t *s = b + 4, *qs = b + 16;
        for (int j = 0; j < 8; j++) {
            int scj, mj;
            if (j < 4) { scj = s[j] & 63; mj = s[j + 4] & 63; }
            else { scj = (s[j + 4] & 0xF) | ((s[j - 4] >> 6) << 4); mj = (s[j + 4] >> 4) | ((s[j] >> 6) << 4); }
            for (int l = 0; l < 32; l++) {
                int e = 32 * j + l;
                q[e] = j % 2 ? qs[32 * (j / 2) + l] >> 4 : qs[32 * (j / 2) + l] & 0xF;
                sc[e] = scj;
                mn[e] = mj;
            }
        }
    } else {
        *d = nn_f16_to_f32((uint16_t)(b[208] | b[209] << 8));
        *dmin = 0;
        const uint8_t *ql = b, *qh = b + 128;
        const int8_t *s = (const int8_t *)(b + 192);
        for (int n = 0; n < 2; n++)
            for (int l = 0; l < 32; l++) {
                int base = 128 * n, is = 8 * n + l / 16;
                const uint8_t *L = ql + 64 * n, *H = qh + 32 * n;
                q[base + l]      = ((L[l] & 0xF) | ((H[l] & 3) << 4)) - 32;
                q[base + l + 32] = ((L[l + 32] & 0xF) | (((H[l] >> 2) & 3) << 4)) - 32;
                q[base + l + 64] = ((L[l] >> 4) | (((H[l] >> 4) & 3) << 4)) - 32;
                q[base + l + 96] = ((L[l + 32] >> 4) | (((H[l] >> 6) & 3) << 4)) - 32;
                sc[base + l] = s[is]; sc[base + l + 32] = s[is + 2];
                sc[base + l + 64] = s[is + 4]; sc[base + l + 96] = s[is + 6];
            }
        for (int e = 0; e < NN_QK; e++) mn[e] = 0;
    }
}

static int f16_checks(void) {
    int ok = 1;
    for (uint32_t h = 0; h < 65536; h++) {
        if ((h & 0x7c00) == 0x7c00 && (h & 0x3ff)) continue;          /* NaN */
        if (nn_f32_to_f16(nn_f16_to_f32((uint16_t)h)) != h) { ok = 0; printf("    f16 %04x\n", h); break; }
    }
    /* Ties to even: 1 + 2^-11 is halfway between 1 and the next half. */
    ok &= nn_f32_to_f16(1.0f + 0x1p-11f) == 0x3c00 && nn_f32_to_f16(1.0f + 3 * 0x1p-11f) == 0x3c02;
    ok &= nn_f32_to_f16(70000.0f) == 0x7c00 && nn_f32_to_f16(0x1p-25f) == 0 && nn_f32_to_f16(0x1p-24f) == 1;
    return ok;
}

/* The quantised product, exactly: the same integers and the same float steps. */
static int kmatmul_case(nn_wtype t, int m, int n, int k) {
    int nb = k / NN_QK, ok = 1;
    size_t rb = nn_row_bytes(t, k);
    uint8_t *w = malloc((size_t)n * rb);
    float *x = malloc((size_t)m * k * sizeof *x), *y = malloc((size_t)m * n * sizeof *y),
          *row = malloc((size_t)k * sizeof *row);
    void *scr = malloc(nn_matmul_w_scratch(t, k, m));
    for (int c = 0; c < n; c++)
        for (int b = 0; b < nb; b++) rnd_block(t, w + (size_t)c * rb + (size_t)b * (t == NN_W_Q4K ? NN_Q4K_BYTES : NN_Q6K_BYTES));
    for (int i = 0; i < m * k; i++) x[i] = rnd_f(-3, 3);
    x[5] = 0;                                         /* and one all-zero block */
    if (m > 1 && k >= 512) for (int i = 0; i < NN_QK; i++) x[(size_t)k + NN_QK + i] = 0;
    nn_matmul_w(t, w, n, k, x, m, (size_t)k, y, (size_t)n, scr);
    int q[NN_QK], sc[NN_QK], mn[NN_QK];
    for (int r = 0; r < m && ok; r++)
        for (int c = 0; c < n && ok; c++) {
            float acc = 0;
            for (int b = 0; b < nb; b++) {
                const float *v = x + (size_t)r * k + b * NN_QK;
                float amax = 0;
                for (int i = 0; i < NN_QK; i++) amax = fmaxf(amax, fabsf(v[i]));
                float ad = amax / 127.0f;
                int8_t a[NN_QK];
                for (int i = 0; i < NN_QK; i++) a[i] = amax == 0 ? 0 : (int8_t)nearbyintf(v[i] * (127.0f / amax));
                if (amax == 0) ad = 0;
                float d, dmin;
                naive_unpack(t, w + (size_t)c * rb + (size_t)b * (t == NN_W_Q4K ? NN_Q4K_BYTES : NN_Q6K_BYTES), q, sc, mn, &d, &dmin);
                long is = 0, im = 0;
                for (int e = 0; e < NN_QK; e++) { is += (long)sc[e] * q[e] * a[e]; im += (long)mn[e] * a[e]; }
                acc += d * ad * (float)is - dmin * ad * (float)im;
            }
            if (acc != y[(size_t)r * n + c]) { ok = 0; printf("    k%d [%d][%d] %.9g / %.9g\n", t, r, c, (double)y[(size_t)r * n + c], (double)acc); }
        }
    /* The dequantised row is the same values. */
    for (int c = 0; c < n && ok; c++) {
        nn_dequant_row(t, w, k, c, row);
        for (int b = 0; b < nb && ok; b++) {
            float d, dmin;
            naive_unpack(t, w + (size_t)c * rb + (size_t)b * (t == NN_W_Q4K ? NN_Q4K_BYTES : NN_Q6K_BYTES), q, sc, mn, &d, &dmin);
            for (int e = 0; e < NN_QK; e++)
                if (row[b * NN_QK + e] != d * (float)sc[e] * (float)q[e] - dmin * (float)mn[e]) { ok = 0; break; }
        }
    }
    free(w); free(x); free(y); free(row); free(scr);
    return ok;
}

static int fmatmul_case(nn_wtype t, int m, int n, int k) {
    size_t rb = nn_row_bytes(t, k);
    uint8_t *w = malloc((size_t)n * rb);
    float *x = malloc((size_t)m * k * sizeof *x), *y = malloc((size_t)m * n * sizeof *y),
          *wf = malloc((size_t)n * k * sizeof *wf);
    void *scr = malloc(nn_matmul_w_scratch(t, k, m));
    for (int i = 0; i < n * k; i++) {
        float f = rnd_f(-1, 1);
        if (t == NN_W_BF16) {
            union { float f; uint32_t u; } v = { .f = f };
            uint16_t h = (uint16_t)(v.u >> 16);
            w[2 * i] = (uint8_t)h; w[2 * i + 1] = (uint8_t)(h >> 8);
            wf[i] = nn_bf16_to_f32(h);
        } else { memcpy(w + 4 * (size_t)i, &f, 4); wf[i] = f; }
    }
    for (int i = 0; i < m * k; i++) x[i] = rnd_f(-1, 1);
    nn_matmul_w(t, w, n, k, x, m, (size_t)k, y, (size_t)n, scr);
    int ok = 1;
    for (int r = 0; r < m; r++)
        for (int c = 0; c < n; c++) {
            double s = 0;
            for (int i = 0; i < k; i++) s += (double)x[(size_t)r * k + i] * wf[(size_t)c * k + i];
            if (fabs(s - y[(size_t)r * n + c]) > 1e-4 * (1 + fabs(s))) ok = 0;
        }
    free(w); free(x); free(y); free(wf); free(scr);
    return ok;
}

static int lm_elementwise_checks(void) {
    int ok = 1;
    float x[3 * 300], g[300], y[3 * 300];
    for (int i = 0; i < 900; i++) x[i] = y[i] = rnd_f(-4, 4);
    for (int i = 0; i < 300; i++) g[i] = rnd_f(0.5f, 1.5f);
    nn_rmsnorm(y, 3, 290, 300, g, 1e-6f);
    for (int r = 0; r < 3; r++) {
        double ss = 0;
        for (int i = 0; i < 290; i++) ss += (double)x[r * 300 + i] * x[r * 300 + i];
        double sc = 1 / sqrt(ss / 290 + 1e-6);
        for (int i = 0; i < 290; i++) if (fabs(x[r * 300 + i] * sc * g[i] - y[r * 300 + i]) > 1e-5) ok = 0;
        for (int i = 290; i < 300; i++) if (y[r * 300 + i] != x[r * 300 + i]) ok = 0;
    }
    for (int i = 0; i < 900; i++) y[i] = x[i];
    nn_l2norm(y, 3, 128, 300, 1e-6f);
    for (int r = 0; r < 3; r++) {
        double ss = 0;
        for (int i = 0; i < 128; i++) ss += (double)x[r * 300 + i] * x[r * 300 + i];
        for (int i = 0; i < 128; i++) if (fabs(x[r * 300 + i] / sqrt(ss) - y[r * 300 + i]) > 1e-6) ok = 0;
    }
    float z[8] = { 0 };
    nn_l2norm(z, 1, 8, 8, 1e-6f);                    /* all zero stays zero */
    for (int i = 0; i < 8; i++) ok &= z[i] == 0;
    for (int i = 0; i < 300; i++) y[i] = x[i];
    nn_mul_sigmoid(y, x + 300, 300);
    for (int i = 0; i < 300; i++) if (fabs(x[i] / (1 + exp(-(double)x[300 + i])) - y[i]) > 1e-5) ok = 0;
    for (int i = 0; i < 300; i++) y[i] = x[i];
    nn_mul_silu(y, x + 300, 300);
    for (int i = 0; i < 300; i++) {
        double s = x[300 + i] / (1 + exp(-(double)x[300 + i]));
        if (fabs(x[i] * s - y[i]) > 1e-4) ok = 0;
    }
    /* Rotary positions: pairs i and i + rot/2, the rest untouched. */
    float h[16], h0[16], inv[4] = { 1.0f, 0.1f, 0.01f, 0.001f };
    for (int i = 0; i < 16; i++) h[i] = h0[i] = rnd_f(-1, 1);
    nn_rope_half(h, 8, inv, 37.0f);
    for (int i = 0; i < 4; i++) {
        double f = 37.0 * inv[i];
        if (fabs(h0[i] * cos(f) - h0[i + 4] * sin(f) - h[i]) > 1e-5 ||
            fabs(h0[i + 4] * cos(f) + h0[i] * sin(f) - h[i + 4]) > 1e-5) ok = 0;
    }
    for (int i = 8; i < 16; i++) ok &= h[i] == h0[i];
    return ok;
}

/* The causal convolution: against the naive sum, and run in pieces the same
 * bits as in one call (the state carried). */
static int conv_causal_case(int t, int c, int k) {
    float *x = malloc((size_t)t * c * sizeof *x), *y = malloc((size_t)t * c * sizeof *y),
          *y2 = malloc((size_t)t * c * sizeof *y2), *w = malloc((size_t)k * c * sizeof *w),
          *st = malloc((size_t)k * c * sizeof *st), *st0 = malloc((size_t)k * c * sizeof *st0),
          *st2 = malloc((size_t)k * c * sizeof *st2);
    for (int i = 0; i < t * c; i++) x[i] = rnd_f(-1, 1);
    for (int i = 0; i < k * c; i++) w[i] = rnd_f(-1, 1);
    for (int i = 0; i < (k - 1) * c; i++) st[i] = st0[i] = st2[i] = rnd_f(-1, 1);
    nn_conv_causal(x, t, c, (size_t)c, st, k, w, y, (size_t)c);
    int ok = 1;
    for (int i = 0; i < t; i++)
        for (int ch = 0; ch < c; ch++) {
            double s = 0;
            for (int j = 0; j < k; j++) {
                int at = i + j - (k - 1);
                s += (double)w[j * c + ch] * (at < 0 ? st0[(k - 1 + at) * c + ch] : x[at * c + ch]);
            }
            if (fabs(s - y[i * c + ch]) > 1e-5) ok = 0;
        }
    for (int i = 0; i < (k - 1) * c; i++) {
        int at = t - (k - 1) + i / c;
        float want = at < 0 ? st0[(k - 1 + at) * c + i % c] : x[at * c + i % c];
        if (st[i] != want) ok = 0;
    }
    int a = t / 3;                                    /* in two pieces, and in place */
    memcpy(y2, x, (size_t)t * c * sizeof *y2);
    nn_conv_causal(y2, a, c, (size_t)c, st2, k, w, y2, (size_t)c);
    nn_conv_causal(y2 + (size_t)a * c, t - a, c, (size_t)c, st2, k, w, y2 + (size_t)a * c, (size_t)c);
    ok &= memcmp(y, y2, (size_t)t * c * sizeof *y) == 0 && memcmp(st, st2, (size_t)(k - 1) * c * sizeof *st) == 0;
    free(x); free(y); free(y2); free(w); free(st); free(st0); free(st2);
    return ok;
}

/* The gated delta rule: against the rule in double, and one step at a time the
 * same bits as all steps in one call. */
static int delta_case(int t, int hk, int hv, int dk, int dv) {
    size_t ldq = (size_t)hk * dk, ldv = (size_t)hv * dv, ns = (size_t)hv * dk * dv;
    float *q = malloc((size_t)t * ldq * sizeof *q), *k = malloc((size_t)t * ldq * sizeof *k),
          *v = malloc((size_t)t * ldv * sizeof *v), *g = malloc((size_t)t * hv * sizeof *g),
          *be = malloc((size_t)t * hv * sizeof *be), *o = malloc((size_t)t * ldv * sizeof *o),
          *o2 = malloc((size_t)t * ldv * sizeof *o2), *S = calloc(ns, sizeof *S), *S2 = calloc(ns, sizeof *S2),
          *qk = malloc((size_t)t * 2 * ldq * sizeof *qk);
    double *D = calloc(ns, sizeof *D);
    for (size_t i = 0; i < (size_t)t * ldq; i++) { q[i] = rnd_f(-0.3f, 0.3f); k[i] = rnd_f(-0.3f, 0.3f); }
    for (size_t i = 0; i < (size_t)t * ldv; i++) v[i] = rnd_f(-1, 1);
    for (int i = 0; i < t * hv; i++) { g[i] = rnd_f(-1, 0); be[i] = rnd_f(0, 1); }
    /* q and k side by side in one row, as the model has them */
    for (int s = 0; s < t; s++) {
        memcpy(qk + (size_t)s * 2 * ldq, q + (size_t)s * ldq, ldq * sizeof *q);
        memcpy(qk + (size_t)s * 2 * ldq + ldq, k + (size_t)s * ldq, ldq * sizeof *k);
    }
    float qs = 1.0f / sqrtf((float)dk);
    nn_gated_delta(qk, qk + ldq, 2 * ldq, v, ldv, g, be, t, hk, hv, dk, dv, qs, S, o, ldv);
    int ok = 1, per = hv / hk;
    for (int s = 0; s < t; s++)
        for (int h = 0; h < hv; h++) {
            const float *qh = q + (size_t)s * ldq + (size_t)(h / per) * dk, *kh = k + (size_t)s * ldq + (size_t)(h / per) * dk;
            double *M = D + (size_t)h * dk * dv, dec = exp((double)g[s * hv + h]), u[64];
            for (int i = 0; i < dk * dv; i++) M[i] *= dec;
            for (int j = 0; j < dv; j++) {
                double kv = 0;
                for (int i = 0; i < dk; i++) kv += M[i * dv + j] * kh[i];
                u[j] = (v[(size_t)s * ldv + (size_t)h * dv + j] - kv) * be[s * hv + h];
            }
            for (int i = 0; i < dk; i++) for (int j = 0; j < dv; j++) M[i * dv + j] += kh[i] * u[j];
            for (int j = 0; j < dv; j++) {
                double r = 0;
                for (int i = 0; i < dk; i++) r += M[i * dv + j] * qh[i] * qs;
                if (fabs(r - o[(size_t)s * ldv + (size_t)h * dv + j]) > 1e-4) ok = 0;
            }
        }
    for (int s = 0; s < t; s++)
        nn_gated_delta(qk + (size_t)s * 2 * ldq, qk + (size_t)s * 2 * ldq + ldq, 2 * ldq, v + (size_t)s * ldv, ldv,
                       g + (size_t)s * hv, be + (size_t)s * hv, 1, hk, hv, dk, dv, qs, S2, o2 + (size_t)s * ldv, ldv);
    ok &= memcmp(o, o2, (size_t)t * ldv * sizeof *o) == 0 && memcmp(S, S2, ns * sizeof *S) == 0;
    free(q); free(k); free(v); free(g); free(be); free(o); free(o2); free(S); free(S2); free(qk); free(D);
    return ok;
}

/* Attention over the 8-bit cache, against double over the cache's own values. */
static int attend_q8_case(int n, int dh) {
    int nb = dh / NN_Q8_0;
    nn_q8_0 *k = malloc((size_t)n * nb * sizeof *k), *v = malloc((size_t)n * nb * sizeof *v);
    float *row = malloc((size_t)dh * sizeof *row), q[256], out[256], *scr = malloc((size_t)n * sizeof *scr);
    double sc[512];
    for (int j = 0; j < n; j++) {
        for (int i = 0; i < dh; i++) row[i] = rnd_f(-2, 2);
        nn_q8_0_quantize(row, dh, k + (size_t)j * nb);
        for (int i = 0; i < dh; i++) row[i] = rnd_f(-2, 2);
        nn_q8_0_quantize(row, dh, v + (size_t)j * nb);
    }
    for (int i = 0; i < dh; i++) q[i] = rnd_f(-1, 1);
    float scale = 1.0f / sqrtf((float)dh);
    nn_attend_q8(q, k, v, (size_t)nb, n, dh, scale, out, scr);
    double mx = -1e300, sum = 0;
    for (int j = 0; j < n; j++) {
        double s = 0;
        for (int i = 0; i < dh; i++) s += (double)q[i] * k[(size_t)j * nb + i / 32].d * k[(size_t)j * nb + i / 32].q[i % 32];
        sc[j] = s * scale;
        if (sc[j] > mx) mx = sc[j];
    }
    for (int j = 0; j < n; j++) { sc[j] = exp(sc[j] - mx); sum += sc[j]; }
    int ok = 1;
    for (int i = 0; i < dh; i++) {
        double o = 0;
        for (int j = 0; j < n; j++) o += sc[j] / sum * v[(size_t)j * nb + i / 32].d * v[(size_t)j * nb + i / 32].q[i % 32];
        if (fabs(o - out[i]) > 1e-4) ok = 0;
    }
    /* The quantised cache holds each value to within half a step. */
    for (int i = 0; i < dh; i++) row[i] = rnd_f(-2, 2);
    nn_q8_0_quantize(row, dh, k);
    for (int i = 0; i < dh; i++) if (fabs(row[i] - k[i / 32].d * k[i / 32].q[i % 32]) > k[i / 32].d * 0.5001) ok = 0;
    free(k); free(v); free(row); free(scr);
    return ok;
}

int run_nnkit_tests(void) {
    printf("test_nnkit: the kernels against naive versions\n");
    CHECK(dql_checks());
    /* Undilated (one contiguous window per step), dilated, strided; output
     * channels not a multiple of 4, odd step counts, windows of more than 64
     * values; weight zero points 0, 255 and between. */
    CHECK(conv_case(32, 5, 3, 1, 1, 7, 128));
    CHECK(conv_case(80, 8, 3, 1, 1, 6, 0));
    CHECK(conv_case(80, 7, 5, 1, 3, 9, 255));
    CHECK(conv_case(22, 3, 12, 6, 1, 5, 37));
    CHECK(conv_case(130, 6, 3, 1, 5, 4, 137));
    CHECK(conv_case(16, 1, 1, 1, 1, 3, 200));
    /* Zero points 0, negative and positive; one row; extreme values. */
    CHECK(matmul_case(5, 37, 6, 0));
    CHECK(matmul_case(1, 128, 4, -3));
    CHECK(matmul_case(4, 300, 9, 5));
    CHECK(matmul_case(3, 1024, 2, -128));
    CHECK(lstm_case(5, 20, 16, 2));
    CHECK(lstm_case(4, 40, 32, 1));
    CHECK(norm_checks());
    CHECK(attention_checks());
    CHECK(matmul_f_checks());
    /* The generator's two upsamplers' shapes, scaled down, and an odd one. */
    CHECK(convt_case(6, 5, 6, 20, 10, 5, 5));
    CHECK(convt_case(9, 4, 3, 12, 6, 3, 3));
    CHECK(convt_case(5, 3, 9, 6, 3, 2, 1));
    CHECK(convt_dw_case(7, 20, 3, 2, 1, 1));
    CHECK(elementwise_checks());
    CHECK(ort_bits_checks());
    CHECK(instnorm_order_case(300, 70));
    CHECK(instnorm_order_case(302, 37));
    CHECK(instnorm_order_case(37, 70));
    CHECK(instnorm_order_case(6, 9));
    CHECK(istft_checks());
    /* The recognizer's: products of every width it runs, one row and several. */
    CHECK(matmul_qc_case(1, 320, 320));
    CHECK(matmul_qc_case(7, 320, 1280));
    CHECK(matmul_qc_case(3, 1280, 8));
    CHECK(recognizer_elementwise_checks());
    CHECK(attend_case(40, 0, 40, 40, 40));
    CHECK(attend_case(40, 5, 22, 40, 320));
    CHECK(attend_case(3, 0, 1, 8, 8));
    CHECK(matvec_u8_case(320, 50));
    CHECK(matvec_u8_case(37, 9));
    /* The summary model's: both block types at the model's widths, one row and
     * several; the float types; its layers' kernels. */
    CHECK(f16_checks());
    CHECK(kmatmul_case(NN_W_Q4K, 1, 5, 256));
    CHECK(kmatmul_case(NN_W_Q4K, 4, 7, 2048));
    CHECK(kmatmul_case(NN_W_Q6K, 1, 3, 512));
    CHECK(kmatmul_case(NN_W_Q6K, 3, 6, 1536));
    CHECK(fmatmul_case(NN_W_BF16, 3, 5, 300));
    CHECK(fmatmul_case(NN_W_F32, 2, 4, 37));
    CHECK(lm_elementwise_checks());
    CHECK(conv_causal_case(9, 40, 4));
    CHECK(conv_causal_case(2, 16, 4));
    CHECK(conv_causal_case(7, 8, 1));
    CHECK(delta_case(6, 2, 2, 16, 8));
    CHECK(delta_case(5, 2, 4, 8, 16));
    CHECK(attend_q8_case(1, 64));
    CHECK(attend_q8_case(37, 256));
    return failures;
}
