/* The neural network kernels (nnkit.h has the contracts and layouts).
 *
 * Written for GCC's and clang's auto-vectorisers at the build's -O2, where GCC
 * only vectorises a loop whose trip count is a known multiple of the vector
 * width: every hot loop here runs 16 or 64 times, over a block, with a short
 * scalar tail where a size may not be a multiple. No intrinsics, no threads.
 *
 * The integer kernels are dot products of int16 rows (q - zp) accumulated in
 * int32 -- the shape the vectorisers turn into multiply-add-pairs instructions
 * -- over 4 output channels by 2 time steps at a time, so each row loaded serves
 * several sums. The 8-bit weights are widened to int16, four rows at a time,
 * into a small scratch reused across every time step; the file keeps them at
 * 8 bits. Float reductions use 16 partial sums, an order fixed by the source. */
#include "nnkit.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define QB 64      /* integer dot products: block length */
#define QT 16      /* ... and the tail step (rows are multiples of NN_CPAD) */
#define FL 16      /* float lanes */
#define KT 16      /* quantised products: weight rows taken together */

static inline int imin(int a, int b) { return a < b ? a : b; }

/* ---- quantisation ---------------------------------------------------------- */

/* Round half to even for |v| < 2^22, in the default rounding mode. */
static inline float rint_even(float v) {
    const float magic = 12582912.0f;            /* 1.5 * 2^23 */
    return (v + magic) - magic;
}

/* ---- the reference's activation functions ---------------------------------------
 * Its CPU kernels (on x86-64 with FMA) compute these as rational or polynomial
 * approximations with fused multiply-adds, not as the C library does; these are
 * the same steps on the same constants, so the results are the same bits.
 *
 * The array versions run 16 values at a time with each fused multiply-add done
 * in double (exact products, so one rounding to double, as nn_fma) and any
 * step that lands halfway between two floats noted; a block with one is done
 * again a value at a time with nn_fma. Both give the fused results; the
 * first vectorises. */

/* Sigmoid: x clamped to +-18, then p(x) / q(x) + 0.5, clamped to [0, 1]. */
#define LOGISTIC_STEPS(in, out)                                                   \
    do {                                                                          \
        float v_ = (in) < -18.0f ? -18.0f : (in);                                 \
        v_ = v_ > 18.0f ? 18.0f : v_;                                             \
        float x2_ = v_ * v_;                                                      \
        float p_ = FMA(x2_, 4.37031012579801e-11f, 1.15627324459942e-07f);       \
        p_ = FMA(p_, x2_, 6.08574864600143e-05f);                                 \
        p_ = FMA(p_, x2_, 8.51377133304701e-03f);                                 \
        p_ = FMA(p_, x2_, 2.48287947061529e-01f);                                 \
        p_ = p_ * v_;                                                             \
        float q_ = FMA(x2_, 6.10247389755681e-13f, 5.76102136993427e-09f);       \
        q_ = FMA(q_, x2_, 6.29106785017040e-06f);                                 \
        q_ = FMA(q_, x2_, 1.70198817374094e-03f);                                 \
        q_ = FMA(q_, x2_, 1.16817656904453e-01f);                                 \
        q_ = FMA(q_, x2_, 9.93151921023180e-01f);                                 \
        float r_ = p_ / q_ + 0.5f;                                                \
        r_ = r_ < 0.0f ? 0.0f : r_;                                               \
        (out) = r_ > 1.0f ? 1.0f : r_;                                            \
    } while (0)

/* tanh: x clamped to +-9, then p(x) / q(x). */
#define TANH_STEPS(in, out)                                                       \
    do {                                                                          \
        float v_ = (in) < -9.0f ? -9.0f : (in);                                   \
        v_ = v_ > 9.0f ? 9.0f : v_;                                               \
        float x2_ = v_ * v_;                                                      \
        float p_ = FMA(x2_, -2.76076847742355e-16f, 2.00018790482477e-13f);      \
        p_ = FMA(p_, x2_, -8.60467152213735e-11f);                                \
        p_ = FMA(p_, x2_, 5.12229709037114e-08f);                                 \
        p_ = FMA(p_, x2_, 1.48572235717979e-05f);                                 \
        p_ = FMA(p_, x2_, 6.37261928875436e-04f);                                 \
        p_ = FMA(p_, x2_, 4.89352455891786e-03f);                                 \
        p_ = p_ * v_;                                                             \
        float q_ = FMA(x2_, 1.19825839466702e-06f, 1.18534705686654e-04f);       \
        q_ = FMA(q_, x2_, 2.26843463243900e-03f);                                 \
        q_ = FMA(q_, x2_, 4.89352518554385e-03f);                                 \
        (out) = p_ / q_;                                                          \
    } while (0)

/* exp as the softmax computes it: x clamped below at -88.376, x = m ln 2 + r
 * with m rounded to nearest by the 1.5 * 2^23 trick and ln 2 in two parts, a
 * degree-6 polynomial in r, scaled by 2^m built in the exponent bits. */
#define EXP_STEPS(in, out)                                                        \
    do {                                                                          \
        float v_ = (in) < -88.3762626647949f ? -88.3762626647949f : (in);         \
        float b_ = FMA(v_, 1.44269504088896341f, 12582912.0f);                    \
        float m_ = b_ - 12582912.0f;                                              \
        v_ = FMA(m_, -6.93145752e-1f, v_);                                        \
        v_ = FMA(m_, -1.42860677e-6f, v_);                                        \
        uint32_t bu_, su_;                                                        \
        float sc_;                                                                \
        memcpy(&bu_, &b_, 4);                                                     \
        su_ = (bu_ << 23) + 0x3F800000u;                                          \
        memcpy(&sc_, &su_, 4);                                                    \
        float p_ = 0x1.694000p-10f;                                               \
        p_ = FMA(p_, v_, 0x1.125edcp-7f);                                         \
        p_ = FMA(p_, v_, 0x1.555b5ap-5f);                                         \
        p_ = FMA(p_, v_, 0x1.555450p-3f);                                         \
        p_ = FMA(p_, v_, 0x1.fffff6p-2f);                                         \
        p_ = FMA(p_, v_, 1.0f);                                                   \
        p_ = FMA(p_, v_, 1.0f);                                                   \
        (out) = p_ * sc_;                                                         \
    } while (0)

#define FMA(a, b, c) nn_fma(a, b, c)
static float logistic1(float v) { float r; LOGISTIC_STEPS(v, r); return r; }
static float tanh1(float v) { float r; TANH_STEPS(v, r); return r; }
static float exp1(float v) { float r; EXP_STEPS(v, r); return r; }
#undef FMA

/* The same over 16 lanes, a step at a time so the loops vectorise. */
#define AL 16

/* o = a b + c per lane, each one fused multiply-add done in double; a halfway
 * landing (see nn_fma) noted in *bad. */
static inline void fma16(const float *a, const float *b, const float *c, float *o, uint32_t *bad) {
    double s[AL];
    uint64_t u[AL];
    uint32_t nb = 0;
    for (int l = 0; l < AL; l++) s[l] = (double)a[l] * (double)b[l] + (double)c[l];
    memcpy(u, s, sizeof u);
    for (int l = 0; l < AL; l++) nb |= ((uint32_t)u[l] & 0x1FFFFFFFu) == 0x10000000u;
    *bad |= nb;
    for (int l = 0; l < AL; l++) o[l] = (float)s[l];
}

static inline void fill16(float *o, float k) {
    for (int l = 0; l < AL; l++) o[l] = k;
}

/* p = c[0]; p = p x + c[i] for i = 1..n-1 (Horner, as the kernels evaluate). */
static inline void horner16(const float *x, const float *c, int n, float *p, uint32_t *bad) {
    float k[AL];
    fill16(p, c[0]);
    for (int i = 1; i < n; i++) {
        fill16(k, c[i]);
        fma16(p, x, k, p, bad);
    }
}

static const float LOG_P[5] = { 4.37031012579801e-11f, 1.15627324459942e-07f, 6.08574864600143e-05f,
                                8.51377133304701e-03f, 2.48287947061529e-01f };
static const float LOG_Q[6] = { 6.10247389755681e-13f, 5.76102136993427e-09f, 6.29106785017040e-06f,
                                1.70198817374094e-03f, 1.16817656904453e-01f, 9.93151921023180e-01f };
static const float TANH_P[7] = { -2.76076847742355e-16f, 2.00018790482477e-13f, -8.60467152213735e-11f,
                                 5.12229709037114e-08f, 1.48572235717979e-05f, 6.37261928875436e-04f,
                                 4.89352455891786e-03f };
static const float TANH_Q[4] = { 1.19825839466702e-06f, 1.18534705686654e-04f, 2.26843463243900e-03f,
                                 4.89352518554385e-03f };
static const float EXP_P[7] = { 0x1.694000p-10f, 0x1.125edcp-7f, 0x1.555b5ap-5f, 0x1.555450p-3f,
                                0x1.fffff6p-2f, 1.0f, 1.0f };

/* The first Horner step multiplies the leading constant by x2 and adds the next,
 * which is fma(x2, c0, c1) -- the same as fma(c0, x2, c1). */
static uint32_t logistic16(const float *x, float *y) {
    float v[AL], x2[AL], p[AL], q[AL];
    uint32_t bad = 0;
    for (int l = 0; l < AL; l++) {
        float t = x[l] < -18.0f ? -18.0f : x[l];
        v[l] = t > 18.0f ? 18.0f : t;
    }
    for (int l = 0; l < AL; l++) x2[l] = v[l] * v[l];
    horner16(x2, LOG_P, 5, p, &bad);
    horner16(x2, LOG_Q, 6, q, &bad);
    for (int l = 0; l < AL; l++) {
        float r = p[l] * v[l] / q[l] + 0.5f;
        r = r < 0.0f ? 0.0f : r;
        y[l] = r > 1.0f ? 1.0f : r;
    }
    return bad;
}

static uint32_t tanh16(const float *x, float *y) {
    float v[AL], x2[AL], p[AL], q[AL];
    uint32_t bad = 0;
    for (int l = 0; l < AL; l++) {
        float t = x[l] < -9.0f ? -9.0f : x[l];
        v[l] = t > 9.0f ? 9.0f : t;
    }
    for (int l = 0; l < AL; l++) x2[l] = v[l] * v[l];
    horner16(x2, TANH_P, 7, p, &bad);
    horner16(x2, TANH_Q, 4, q, &bad);
    for (int l = 0; l < AL; l++) y[l] = p[l] * v[l] / q[l];
    return bad;
}

static uint32_t exp16(const float *x, float *y) {
    float v[AL], b[AL], m[AL], k1[AL], k2[AL], p[AL], sc[AL];
    uint32_t bad = 0, bu[AL];
    for (int l = 0; l < AL; l++) v[l] = x[l] < -88.3762626647949f ? -88.3762626647949f : x[l];
    fill16(k1, 1.44269504088896341f);
    fill16(k2, 12582912.0f);
    fma16(v, k1, k2, b, &bad);
    for (int l = 0; l < AL; l++) m[l] = b[l] - 12582912.0f;
    fill16(k1, -6.93145752e-1f);
    fma16(m, k1, v, v, &bad);
    fill16(k1, -1.42860677e-6f);
    fma16(m, k1, v, v, &bad);
    memcpy(bu, b, sizeof bu);
    for (int l = 0; l < AL; l++) bu[l] = (bu[l] << 23) + 0x3F800000u;
    memcpy(sc, bu, sizeof sc);
    horner16(v, EXP_P, 7, p, &bad);
    for (int l = 0; l < AL; l++) y[l] = p[l] * sc[l];
    return bad;
}

/* In place over n values: 16 at a time, a block with a halfway landing (or the
 * last, short one) one value at a time with nn_fma. */
#define ARRAY_ACT(name, sixteen, one)                                             \
    void name(float *x, size_t n) {                                               \
        for (size_t i = 0; i < n; i += AL) {                                      \
            size_t m = n - i < AL ? n - i : AL;                                   \
            float y[AL];                                                          \
            if (m < AL || sixteen(x + i, y))                                      \
                for (size_t l = 0; l < m; l++) y[l] = one(x[i + l]);              \
            memcpy(x + i, y, sizeof(float) * m);                                  \
        }                                                                         \
    }
ARRAY_ACT(nn_logistic, logistic16, logistic1)
ARRAY_ACT(nn_tanh, tanh16, tanh1)
static ARRAY_ACT(exp_array, exp16, exp1)

/* FastGelu, as the reference fuses the graph's tanh form: x (0.0356774 x^2 +
 * 0.7978846), its tanh, then 0.5 x (t + 1). */
void nn_gelu(float *x, size_t n) {
    float t[AL];
    for (size_t i = 0; i < n; i += AL) {
        size_t m = n - i < AL ? n - i : AL;
        for (size_t l = 0; l < m; l++) t[l] = x[i + l] * (0.035677408136300125f * x[i + l] * x[i + l] + 0.7978845608028654f);
        nn_tanh(t, m);
        for (size_t l = 0; l < m; l++) x[i + l] = 0.5f * x[i + l] * (t[l] + 1.0f);
    }
}

void nn_dql_params(float min, float max, nn_qparam *qp) {
    if (min > 0) min = 0;
    if (max < 0) max = 0;
    if (max == min) {
        qp->scale = 0;
        qp->zp = 0;
        return;
    }
    float scale = (max - min) / 255.0f;
    float z = 0.0f - min / scale;
    z = z < 0 ? 0 : z > 255 ? 255 : z;
    qp->scale = scale;
    qp->zp = (int)rint_even(z);
}

static void minmax(const float *restrict x, size_t n, float *mn, float *mx) {
    float lo[FL], hi[FL];
    size_t i = 0;
    for (int l = 0; l < FL; l++) lo[l] = hi[l] = n ? x[0] : 0;
    for (; i + FL <= n; i += FL)
        for (int l = 0; l < FL; l++) {
            float v = x[i + l];
            lo[l] = v < lo[l] ? v : lo[l];
            hi[l] = v > hi[l] ? v : hi[l];
        }
    for (; i < n; i++) {
        lo[0] = x[i] < lo[0] ? x[i] : lo[0];
        hi[0] = x[i] > hi[0] ? x[i] : hi[0];
    }
    float a = lo[0], b = hi[0];
    for (int l = 1; l < FL; l++) {
        a = lo[l] < a ? lo[l] : a;
        b = hi[l] > b ? hi[l] : b;
    }
    *mn = a;
    *mx = b;
}

/* q - zp for one value: x / scale clamped to the representable range, rounded. */
static inline float qoff(float v, float scale, float lo, float hi) {
    v = v / scale;
    v = v < lo ? lo : v;
    v = v > hi ? hi : v;
    return rint_even(v);
}

void nn_dql_u8(const float *restrict x, size_t n, uint8_t *restrict q, nn_qparam *qp) {
    float mn, mx;
    minmax(x, n, &mn, &mx);
    nn_dql_params(mn, mx, qp);
    if (qp->scale == 0) {
        memset(q, 0, n);
        return;
    }
    float s = qp->scale, lo = (float)-qp->zp, hi = (float)(255 - qp->zp);
    int zp = qp->zp;
    size_t i = 0;
    for (; i + FL <= n; i += FL)
        for (int l = 0; l < FL; l++) q[i + l] = (uint8_t)((int)qoff(x[i + l], s, lo, hi) + zp);
    for (; i < n; i++) q[i] = (uint8_t)((int)qoff(x[i], s, lo, hi) + zp);
}

void nn_quantize(const float *restrict x, int rows, int cols, int ldx, int16_t *restrict q, int ldq,
                    nn_qparam *qp) {
    float mn = 0, mx = 0;
    for (int r = 0; r < rows; r++) {
        float a, b;
        minmax(x + (size_t)r * ldx, (size_t)cols, &a, &b);
        if (r == 0 || a < mn) mn = a;
        if (r == 0 || b > mx) mx = b;
    }
    nn_dql_params(mn, mx, qp);
    float s = qp->scale, lo = (float)-qp->zp, hi = (float)(255 - qp->zp);
    for (int r = 0; r < rows; r++) {
        const float *xr = x + (size_t)r * ldx;
        int16_t *qr = q + (size_t)r * ldq;
        int c = 0;
        if (s != 0) {
            for (; c + FL <= cols; c += FL)
                for (int l = 0; l < FL; l++) qr[c + l] = (int16_t)qoff(xr[c + l], s, lo, hi);
            for (; c < cols; c++) qr[c] = (int16_t)qoff(xr[c], s, lo, hi);
        }
        for (; c < ldq; c++) qr[c] = 0;
    }
}

/* ---- integer products -------------------------------------------------------- */

static void widen_u8(int16_t *restrict d, const uint8_t *restrict s, int zp, size_t n) {
    for (size_t i = 0; i < n; i += QT)                     /* n is a multiple of 16 */
        for (int l = 0; l < QT; l++) d[i + l] = (int16_t)(s[i + l] - zp);
}

static void widen_s8(int16_t *restrict d, const int8_t *restrict s, int zp, size_t n) {
    for (size_t i = 0; i < n; i += QT)
        for (int l = 0; l < QT; l++) d[i + l] = (int16_t)(s[i + l] - zp);
}

/* s[0..3] += a . w rows 0..3 and s[4..7] += b . w rows 0..3, over len values (a
 * multiple of QT); the weight rows are wl apart.
 *
 * The two vectorisers find their multiply-add-pairs instruction in different
 * sources, and each runs the other's at a third of the speed: GCC in a plain
 * dot-product reduction over a block of fixed length (the release builds with
 * gcc), clang in sums of adjacent pairs kept in 4 lanes per output. Both
 * compute the same exact int32 sums; only the speed depends on the choice. */
#if defined(__clang__)
static void qdot42(const int16_t *restrict a, const int16_t *restrict b, const int16_t *restrict w, size_t wl,
                   int len, int32_t *restrict s) {
    const int16_t *w0 = w, *w1 = w + wl, *w2 = w1 + wl, *w3 = w2 + wl;
    int32_t a0[4] = { 0 }, a1[4] = { 0 }, a2[4] = { 0 }, a3[4] = { 0 };
    int32_t b0[4] = { 0 }, b1[4] = { 0 }, b2[4] = { 0 }, b3[4] = { 0 };
    for (int i = 0; i < len; i += 8) {
        const int16_t *pa = a + i, *pb = b + i, *q0 = w0 + i, *q1 = w1 + i, *q2 = w2 + i, *q3 = w3 + i;
        for (int j = 0; j < 4; j++) {
            int32_t u = pa[2 * j], U = pa[2 * j + 1], v = pb[2 * j], V = pb[2 * j + 1];
            a0[j] += u * q0[2 * j] + U * q0[2 * j + 1];
            a1[j] += u * q1[2 * j] + U * q1[2 * j + 1];
            a2[j] += u * q2[2 * j] + U * q2[2 * j + 1];
            a3[j] += u * q3[2 * j] + U * q3[2 * j + 1];
            b0[j] += v * q0[2 * j] + V * q0[2 * j + 1];
            b1[j] += v * q1[2 * j] + V * q1[2 * j + 1];
            b2[j] += v * q2[2 * j] + V * q2[2 * j + 1];
            b3[j] += v * q3[2 * j] + V * q3[2 * j + 1];
        }
    }
    s[0] += a0[0] + a0[1] + a0[2] + a0[3];
    s[1] += a1[0] + a1[1] + a1[2] + a1[3];
    s[2] += a2[0] + a2[1] + a2[2] + a2[3];
    s[3] += a3[0] + a3[1] + a3[2] + a3[3];
    s[4] += b0[0] + b0[1] + b0[2] + b0[3];
    s[5] += b1[0] + b1[1] + b1[2] + b1[3];
    s[6] += b2[0] + b2[1] + b2[2] + b2[3];
    s[7] += b3[0] + b3[1] + b3[2] + b3[3];
}

static void qdot41(const int16_t *restrict a, const int16_t *restrict w, size_t wl, int len, int32_t *restrict s) {
    const int16_t *w0 = w, *w1 = w + wl, *w2 = w1 + wl, *w3 = w2 + wl;
    int32_t a0[4] = { 0 }, a1[4] = { 0 }, a2[4] = { 0 }, a3[4] = { 0 };
    for (int i = 0; i < len; i += 8) {
        const int16_t *pa = a + i, *q0 = w0 + i, *q1 = w1 + i, *q2 = w2 + i, *q3 = w3 + i;
        for (int j = 0; j < 4; j++) {
            int32_t u = pa[2 * j], U = pa[2 * j + 1];
            a0[j] += u * q0[2 * j] + U * q0[2 * j + 1];
            a1[j] += u * q1[2 * j] + U * q1[2 * j + 1];
            a2[j] += u * q2[2 * j] + U * q2[2 * j + 1];
            a3[j] += u * q3[2 * j] + U * q3[2 * j + 1];
        }
    }
    s[0] += a0[0] + a0[1] + a0[2] + a0[3];
    s[1] += a1[0] + a1[1] + a1[2] + a1[3];
    s[2] += a2[0] + a2[1] + a2[2] + a2[3];
    s[3] += a3[0] + a3[1] + a3[2] + a3[3];
}
#else
static void qdot42(const int16_t *restrict a, const int16_t *restrict b, const int16_t *restrict w, size_t wl,
                   int len, int32_t *restrict s) {
    const int16_t *w0 = w, *w1 = w + wl, *w2 = w1 + wl, *w3 = w2 + wl;
    int32_t a0 = 0, a1 = 0, a2 = 0, a3 = 0, b0 = 0, b1 = 0, b2 = 0, b3 = 0;
    int i = 0;
    for (; i + QB <= len; i += QB) {
        const int16_t *pa = a + i, *pb = b + i, *q0 = w0 + i, *q1 = w1 + i, *q2 = w2 + i, *q3 = w3 + i;
        int32_t c0 = 0, c1 = 0, c2 = 0, c3 = 0, d0 = 0, d1 = 0, d2 = 0, d3 = 0;
        for (int l = 0; l < QB; l++) {
            int32_t u = pa[l], v = pb[l];
            c0 += u * q0[l]; c1 += u * q1[l]; c2 += u * q2[l]; c3 += u * q3[l];
            d0 += v * q0[l]; d1 += v * q1[l]; d2 += v * q2[l]; d3 += v * q3[l];
        }
        a0 += c0; a1 += c1; a2 += c2; a3 += c3;
        b0 += d0; b1 += d1; b2 += d2; b3 += d3;
    }
    for (; i < len; i += QT) {
        const int16_t *pa = a + i, *pb = b + i, *q0 = w0 + i, *q1 = w1 + i, *q2 = w2 + i, *q3 = w3 + i;
        int32_t c0 = 0, c1 = 0, c2 = 0, c3 = 0, d0 = 0, d1 = 0, d2 = 0, d3 = 0;
        for (int l = 0; l < QT; l++) {
            int32_t u = pa[l], v = pb[l];
            c0 += u * q0[l]; c1 += u * q1[l]; c2 += u * q2[l]; c3 += u * q3[l];
            d0 += v * q0[l]; d1 += v * q1[l]; d2 += v * q2[l]; d3 += v * q3[l];
        }
        a0 += c0; a1 += c1; a2 += c2; a3 += c3;
        b0 += d0; b1 += d1; b2 += d2; b3 += d3;
    }
    s[0] += a0; s[1] += a1; s[2] += a2; s[3] += a3;
    s[4] += b0; s[5] += b1; s[6] += b2; s[7] += b3;
}

/* s[0..3] += a . w rows 0..3. */
static void qdot41(const int16_t *restrict a, const int16_t *restrict w, size_t wl, int len, int32_t *restrict s) {
    const int16_t *w0 = w, *w1 = w + wl, *w2 = w1 + wl, *w3 = w2 + wl;
    int32_t a0 = 0, a1 = 0, a2 = 0, a3 = 0;
    int i = 0;
    for (; i + QB <= len; i += QB) {
        const int16_t *pa = a + i, *q0 = w0 + i, *q1 = w1 + i, *q2 = w2 + i, *q3 = w3 + i;
        int32_t c0 = 0, c1 = 0, c2 = 0, c3 = 0;
        for (int l = 0; l < QB; l++) {
            int32_t u = pa[l];
            c0 += u * q0[l]; c1 += u * q1[l]; c2 += u * q2[l]; c3 += u * q3[l];
        }
        a0 += c0; a1 += c1; a2 += c2; a3 += c3;
    }
    for (; i < len; i += QT) {
        const int16_t *pa = a + i, *q0 = w0 + i, *q1 = w1 + i, *q2 = w2 + i, *q3 = w3 + i;
        int32_t c0 = 0, c1 = 0, c2 = 0, c3 = 0;
        for (int l = 0; l < QT; l++) {
            int32_t u = pa[l];
            c0 += u * q0[l]; c1 += u * q1[l]; c2 += u * q2[l]; c3 += u * q3[l];
        }
        a0 += c0; a1 += c1; a2 += c2; a3 += c3;
    }
    s[0] += a0; s[1] += a1; s[2] += a2; s[3] += a3;
}
#endif

/* Four widened weight rows (wl = taps * len values each) against every output
 * step: output t reads `taps` runs of len values, at x + t * xstep + j * tstep. */
static void qblock(const int16_t *restrict x, size_t xstep, size_t tstep, int taps, int len, int t_out,
                   const int16_t *restrict w, int nco, int32_t *restrict y, int ldy) {
    size_t wl = (size_t)taps * len;
    int t = 0;
    for (; t + 2 <= t_out; t += 2) {
        int32_t s[8] = { 0 };
        const int16_t *xa = x + (size_t)t * xstep, *xb = xa + xstep;
        for (int j = 0; j < taps; j++)
            qdot42(xa + (size_t)j * tstep, xb + (size_t)j * tstep, w + (size_t)j * len, wl, len, s);
        int32_t *ya = y + (size_t)t * ldy, *yb = ya + ldy;
        for (int r = 0; r < nco; r++) {
            ya[r] = s[r];
            yb[r] = s[4 + r];
        }
    }
    if (t < t_out) {
        int32_t s[4] = { 0 };
        const int16_t *xa = x + (size_t)t * xstep;
        for (int j = 0; j < taps; j++) qdot41(xa + (size_t)j * tstep, w + (size_t)j * len, wl, len, s);
        int32_t *ya = y + (size_t)t * ldy;
        for (int r = 0; r < nco; r++) ya[r] = s[r];
    }
}

void nn_conv_q(const int16_t *restrict x, int t_out, int stride, int dil, int k, int cin,
                  const uint8_t *restrict w, int wzp, int cout, int32_t *restrict y, int ldy,
                  int16_t *restrict wbuf) {
    /* Undilated, a step's k input rows are one contiguous run of k * cin values. */
    int taps = dil == 1 ? 1 : k, len = dil == 1 ? k * cin : cin;
    size_t wl = (size_t)k * cin;
    for (int co = 0; co < cout; co += 4) {
        widen_u8(wbuf, w + (size_t)co * wl, wzp, 4 * wl);
        qblock(x, (size_t)stride * cin, (size_t)dil * cin, taps, len, t_out, wbuf, imin(4, cout - co), y + co, ldy);
    }
}

void nn_matmul_q(const int16_t *restrict a, int m, int k, const int8_t *restrict b, int bzp, int n,
                    int32_t *restrict y, int ldy, int16_t *restrict wbuf) {
    for (int c = 0; c < n; c += 4) {
        widen_s8(wbuf, b + (size_t)c * k, bzp, 4 * (size_t)k);
        qblock(a, (size_t)k, 0, 1, k, m, wbuf, imin(4, n - c), y + c, ldy);
    }
}

void nn_dequant(const int32_t *restrict acc, int rows, int cols, int lda, float scale,
                   const float *restrict bias, float *restrict y, int ldy) {
    for (int r = 0; r < rows; r++) {
        const int32_t *a = acc + (size_t)r * lda;
        float *o = y + (size_t)r * ldy;
        int c = 0;
        if (bias) {
            for (; c + FL <= cols; c += FL)
                for (int l = 0; l < FL; l++) o[c + l] = (float)a[c + l] * scale + bias[c + l];
            for (; c < cols; c++) o[c] = (float)a[c] * scale + bias[c];
        } else {
            for (; c + FL <= cols; c += FL)
                for (int l = 0; l < FL; l++) o[c + l] = (float)a[c + l] * scale;
            for (; c < cols; c++) o[c] = (float)a[c] * scale;
        }
    }
}

/* ---- LSTM ----------------------------------------------------------------------- */

#define ALIGN64(n) (((n) + 63) & ~(size_t)63)

typedef struct {
    int16_t *qx, *r16, *qh, *wbuf;
    int32_t *acc, *hacc;
    float *gx, *bias, *h, *c;
} lstm_parts;

static size_t lstm_layout(int t, int in_pad, int hidden, uint8_t *base, lstm_parts *p) {
    size_t g = (size_t)4 * hidden, off = 0;
#define TAKE(field, type, count)                                       \
    do {                                                               \
        if (p) p->field = (type *)(void *)(base + off);                \
        off += ALIGN64(sizeof(type) * (size_t)(count));                \
    } while (0)
    TAKE(qx, int16_t, (size_t)t * in_pad);
    TAKE(r16, int16_t, g * hidden);
    TAKE(qh, int16_t, hidden);
    TAKE(wbuf, int16_t, NN_WBUF(1, in_pad > hidden ? in_pad : hidden));
    TAKE(acc, int32_t, (size_t)t * g);
    TAKE(hacc, int32_t, g);
    TAKE(gx, float, (size_t)t * g);
    TAKE(bias, float, g);
    TAKE(h, float, hidden);
    TAKE(c, float, hidden);
#undef TAKE
    return off;
}

size_t nn_lstm_scratch(int t, int in_pad, int hidden) { return lstm_layout(t, in_pad, hidden, NULL, NULL) + 64; }

void nn_lstm_run(const nn_lstm *l, int dirs, const float *x, int t, int in, int ldx, float *y, int ldy,
                    void *scratch) {
    int H = l->hidden, G = 4 * H;
    lstm_parts p;
    uint8_t *base = (uint8_t *)scratch;
    base += (64 - ((uintptr_t)base & 63)) & 63;
    lstm_layout(t, l->in_pad, H, base, &p);
    nn_qparam xq;
    nn_quantize(x, t, in, ldx, p.qx, l->in_pad, &xq);
    for (int d = 0; d < dirs; d++) {
        /* The input projection of every step at once. */
        nn_matmul_q(p.qx, t, l->in_pad, l->w[d], l->wzp[d], G, p.acc, G, p.wbuf);
        nn_dequant(p.acc, t, G, G, xq.scale * l->wscale[d], NULL, p.gx, G);
        widen_s8(p.r16, l->r[d], l->rzp[d], (size_t)G * H);
        for (int i = 0; i < G; i++) p.bias[i] = l->b[d][i] + l->b[d][G + i];
        memset(p.h, 0, sizeof(float) * (size_t)H);
        memset(p.c, 0, sizeof(float) * (size_t)H);
        for (int s = 0; s < t; s++) {
            int step = d == 0 ? s : t - 1 - s;
            nn_qparam hq;
            nn_quantize(p.h, 1, H, H, p.qh, H, &hq);
            for (int co = 0; co < G; co += 4)
                qblock(p.qh, 0, 0, 1, H, 1, p.r16 + (size_t)co * H, 4, p.hacc + co, G);
            float *g = p.gx + (size_t)step * G, m = hq.scale * l->rscale[d];
            for (int i = 0; i < G; i++) g[i] = (float)p.hacc[i] * m + g[i];
            for (int i = 0; i < G; i++) g[i] = g[i] + p.bias[i];
            float *out = y + (size_t)step * ldy + (size_t)d * H;
            nn_logistic(g, (size_t)3 * H);                 /* i, o, f */
            nn_tanh(g + 3 * H, (size_t)H);                 /* the cell input */
            for (int j = 0; j < H; j++) p.c[j] = p.c[j] * g[2 * H + j] + g[j] * g[3 * H + j];
            memcpy(p.h, p.c, sizeof(float) * (size_t)H);
            nn_tanh(p.h, (size_t)H);
            for (int j = 0; j < H; j++) out[j] = p.h[j] = p.h[j] * g[H + j];
        }
    }
}

/* ---- normalisation, attention ------------------------------------------------- */

/* The reference's LayerNormalization: the mean from a double sum; the variance
 * from squared deviations accumulated by fused multiply-adds in 8 lanes, the
 * lanes added in halves; y = fma((x - mean) * inv, g, b). */
void nn_layernorm(float *x, int rows, int cols, int ld, const float *g, const float *b, float eps) {
    for (int r = 0; r < rows; r++) {
        float *xr = x + (size_t)r * ld;
        double s = 0;
        for (int c = 0; c < cols; c++) s += xr[c];
        float mean = (float)(s / cols), acc[8] = { 0 };
        int c = 0;
        for (; c + 8 <= cols; c += 8)
            for (int l = 0; l < 8; l++) {
                float d = xr[c + l] - mean;
                acc[l] = nn_fma(d, d, acc[l]);
            }
        for (int l = 0; l < 4; l++) acc[l] += acc[l + 4];
        for (int l = 0; l < 2; l++) acc[l] += acc[l + 2];
        float q = acc[0] + acc[1];
        for (; c < cols; c++) {
            float d = xr[c] - mean;
            q = nn_fma(d, d, q);
        }
        float inv = 1.0f / sqrtf(q / (float)cols + eps);
        for (c = 0; c < cols; c++) {
            float v = (xr[c] - mean) * inv;
            xr[c] = g ? nn_fma(v, g[c], b ? b[c] : 0.0f) : v;
        }
    }
}

/* The reference's SkipLayerNormalization: v = x + skip, then Welford's running
 * mean and sum of squares in float, y = (v - mean) / sqrt(m2 / n + eps) * g + b. */
void nn_skip_layernorm(float *x, const float *skip, int rows, int cols, int ld, int lds, const float *g,
                          const float *b, float eps) {
    for (int r = 0; r < rows; r++) {
        float *xr = x + (size_t)r * ld;
        const float *sr = skip + (size_t)r * lds;
        float mean = 0, m2 = 0;
        for (int c = 0; c < cols; c++) {
            float v = xr[c] + sr[c];
            xr[c] = v;
            float d = v - mean;
            mean = mean + d / (float)(c + 1);
            m2 = m2 + d * (v - mean);
        }
        float sd = sqrtf(m2 / (float)cols + eps);
        for (int c = 0; c < cols; c++) xr[c] = (xr[c] - mean) / sd * g[c] + b[c];
    }
}

/* Sum of n floats as Eigen's vectorised sum on 4-lane vectors: from element
 * `as` (the first 16-byte aligned one), two accumulators over pairs of vectors,
 * a last single vector into the first, the two added, the lanes as (0 + 2) +
 * (1 + 3); then the elements before `as`, then those after the last whole
 * vector. */
static float eigen_sum(const float *a, int n, int as) {
    if (as > n) as = n;
    int a2 = (n - as) / 8 * 8, a1 = (n - as) / 4 * 4, e2 = as + a2, e1 = as + a1;
    if (!a1) {
        float r = a[0];
        for (int i = 1; i < n; i++) r += a[i];
        return r;
    }
    float r0[4], r1[4];
    memcpy(r0, a + as, sizeof r0);
    if (a1 > 4) {
        memcpy(r1, a + as + 4, sizeof r1);
        for (int i = as + 8; i < e2; i += 8)
            for (int l = 0; l < 4; l++) {
                r0[l] += a[i + l];
                r1[l] += a[i + 4 + l];
            }
        for (int l = 0; l < 4; l++) r0[l] += r1[l];
        if (e1 > e2)
            for (int l = 0; l < 4; l++) r0[l] += a[e2 + l];
    }
    float r = (r0[0] + r0[2]) + (r0[1] + r0[3]);
    for (int i = 0; i < as; i++) r += a[i];
    for (int i = e1; i < n; i++) r += a[i];
    return r;
}

float nn_reduce_sum(const float *a, int n, size_t start) { return eigen_sum(a, n, (int)((4 - start % 4) % 4)); }

/* eigen_sum over the rows of time-major data, for the channels ch0, ch0 + cs,
 * ... below c at once (each one's t values rows `ld` apart, its vectors starting
 * `as` rows in): sum[ch] receives exactly what eigen_sum gives on that
 * channel's column -- of the values themselves, or with `mean` given of their
 * squared deviations (x - mean)^2. lanes: 8 * c floats of scratch. */
#define ES_VAL(i, ch) (mean ? (x[(size_t)(i) * ld + (ch)] - mean[ch]) * (x[(size_t)(i) * ld + (ch)] - mean[ch]) \
                            : x[(size_t)(i) * ld + (ch)])
static void eigen_sum_rows(const float *x, int t, int c, int ld, int ch0, int cs, int as, const float *mean,
                           float *sum, float *lanes) {
    if (as > t) as = t;
    int a2 = (t - as) / 8 * 8, a1 = (t - as) / 4 * 4, e2 = as + a2, e1 = as + a1;
    if (!a1) {
        for (int ch = ch0; ch < c; ch += cs) sum[ch] = ES_VAL(0, ch);
        for (int i = 1; i < t; i++)
            for (int ch = ch0; ch < c; ch += cs) sum[ch] += ES_VAL(i, ch);
        return;
    }
    float *r0 = lanes, *r1 = lanes + (size_t)4 * c;           /* [lane][channel] */
    for (int l = 0; l < 4; l++)
        for (int ch = ch0; ch < c; ch += cs) r0[(size_t)l * c + ch] = ES_VAL(as + l, ch);
    if (a1 > 4) {
        for (int l = 0; l < 4; l++)
            for (int ch = ch0; ch < c; ch += cs) r1[(size_t)l * c + ch] = ES_VAL(as + 4 + l, ch);
        for (int i = as + 8; i < e2; i += 8)
            for (int l = 0; l < 4; l++) {
                float *ra = r0 + (size_t)l * c, *rb = r1 + (size_t)l * c;
                if (mean && cs == 1) {
                    const float *xa = x + (size_t)(i + l) * ld, *xb = x + (size_t)(i + 4 + l) * ld;
                    for (int ch = 0; ch < c; ch++) {
                        float da = xa[ch] - mean[ch], db = xb[ch] - mean[ch];
                        ra[ch] += da * da;
                        rb[ch] += db * db;
                    }
                } else if (!mean && cs == 1) {
                    const float *xa = x + (size_t)(i + l) * ld, *xb = x + (size_t)(i + 4 + l) * ld;
                    for (int ch = 0; ch < c; ch++) {
                        ra[ch] += xa[ch];
                        rb[ch] += xb[ch];
                    }
                } else
                    for (int ch = ch0; ch < c; ch += cs) {
                        ra[ch] += ES_VAL(i + l, ch);
                        rb[ch] += ES_VAL(i + 4 + l, ch);
                    }
            }
        for (int l = 0; l < 4; l++)
            for (int ch = ch0; ch < c; ch += cs) r0[(size_t)l * c + ch] += r1[(size_t)l * c + ch];
        if (e1 > e2)
            for (int l = 0; l < 4; l++)
                for (int ch = ch0; ch < c; ch += cs) r0[(size_t)l * c + ch] += ES_VAL(e2 + l, ch);
    }
    for (int ch = ch0; ch < c; ch += cs)
        sum[ch] = (r0[ch] + r0[(size_t)2 * c + ch]) + (r0[(size_t)c + ch] + r0[(size_t)3 * c + ch]);
    for (int i = 0; i < as; i++)
        for (int ch = ch0; ch < c; ch += cs) sum[ch] += ES_VAL(i, ch);
    for (int i = e1; i < t; i++)
        for (int ch = ch0; ch < c; ch += cs) sum[ch] += ES_VAL(i, ch);
}
#undef ES_VAL

/* The reference's InstanceNormalization (Eigen, on channel-major data): channel
 * c's t values start c * t floats into a 16-byte aligned tensor, so its mean's
 * sum starts its vectors (c * t) mod 4 values in; the sum of squared deviations
 * is over an expression, whose vectors start at 0. Then k = g / sqrt(var + eps),
 * y = x k + (b - mean k). Here on time-major rows, a pass over the rows for each
 * sum: the channels fall into at most four classes by (c * t) mod 4, which
 * repeats every 4 / gcd(t, 4) channels. */
void nn_instnorm(float *x, int t, int c, int ld, const float *g, const float *b, float eps) {
    float *mem = malloc(sizeof(float) * (size_t)c * 12);
    if (!mem) return;
    float *mean = mem, *k = mem + c, *sh = mem + 2 * (size_t)c, *lanes = mem + 4 * (size_t)c;
    int cs = 4;
    while (cs > 1 && (size_t)(cs / 2) * t % 4 == 0) cs /= 2;     /* the classes' period */
    for (int ch0 = 0; ch0 < cs && ch0 < c; ch0++)
        eigen_sum_rows(x, t, c, ld, ch0, cs, (int)((4 - (size_t)ch0 * t % 4) % 4), NULL, mean, lanes);
    for (int ch = 0; ch < c; ch++) mean[ch] = mean[ch] / (float)t;
    eigen_sum_rows(x, t, c, ld, 0, 1, 0, mean, k, lanes);
    for (int ch = 0; ch < c; ch++) {
        float inv = 1.0f / sqrtf(k[ch] / (float)t + eps);
        k[ch] = inv * (g ? g[ch] : 1.0f);
        sh[ch] = (b ? b[ch] : 0.0f) - mean[ch] * k[ch];
    }
    for (int r = 0; r < t; r++) {
        float *xr = x + (size_t)r * ld;
        for (int ch = 0; ch < c; ch++) xr[ch] = xr[ch] * k[ch] + sh[ch];
    }
    free(mem);
}

/* The reference's softmax: the exponentials of x - max summed in 8 lanes (the
 * last, partial vector into the same lanes), the lanes added in adjacent
 * pairs, and each exponential multiplied by 1 / sum. */
void nn_softmax(float *x, int n) {
    float mx = x[0];
    for (int i = 1; i < n; i++) mx = x[i] > mx ? x[i] : mx;
    float acc[8] = { 0 }, nm = -mx;
    for (int i = 0; i < n; i++) x[i] = x[i] + nm;
    exp_array(x, (size_t)n);
    for (int i = 0; i < n; i++) acc[i & 7] += x[i];
    for (int w = 0; w < 8; w += 2) acc[w / 2] = acc[w] + acc[w + 1];
    for (int w = 0; w < 4; w += 2) acc[w / 2] = acc[w] + acc[w + 1];
    float inv = 1.0f / (acc[0] + acc[1]);
    for (int i = 0; i < n; i++) x[i] *= inv;
}

/* y[c] = sum_i a[i] * b[i * ldb + c] for c < n, as the reference's float matrix
 * product forms each output: a chain of fused multiply-adds from 0 over each
 * block of 256 of i, the blocks' results added in turn. Sixteen chains at once,
 * in double (exact products, one rounding per step, as nn_fma), noting any
 * step that lands halfway between two floats; a block with one is done again
 * with nn_fma. */
#define FMA_LANES 16
static void vecmat_fma(const float *restrict a, int k, const float *restrict b, size_t ldb, int n,
                       float *restrict y) {
    for (int c0 = 0; c0 < n; c0 += FMA_LANES) {
        int nc = imin(FMA_LANES, n - c0);
        float out[FMA_LANES];
        for (int k0 = 0; k0 < k; k0 += 256) {
            int k1 = imin(k, k0 + 256);
            float acc[FMA_LANES] = { 0 };
            uint32_t bad = 0;
            if (nc == FMA_LANES) {
                for (int i = k0; i < k1; i++) {
                    const float *br = b + (size_t)i * ldb + c0;
                    double ai = a[i], sv[FMA_LANES];
                    uint64_t uv[FMA_LANES];
                    for (int l = 0; l < FMA_LANES; l++) sv[l] = ai * (double)br[l] + (double)acc[l];
                    memcpy(uv, sv, sizeof uv);
                    for (int l = 0; l < FMA_LANES; l++) {
                        acc[l] = (float)sv[l];
                        bad |= ((uint32_t)uv[l] & 0x1FFFFFFFu) == 0x10000000u;
                    }
                }
            }
            if (nc < FMA_LANES || bad)
                for (int l = 0; l < nc; l++) {
                    float c = 0;
                    for (int i = k0; i < k1; i++) c = nn_fma(a[i], b[(size_t)i * ldb + c0 + l], c);
                    acc[l] = c;
                }
            for (int l = 0; l < nc; l++) out[l] = k0 ? acc[l] + out[l] : acc[l];
        }
        memcpy(y + c0, out, sizeof(float) * (size_t)nc);
    }
}

/* The same over a tile of FMA_ROWS rows by FMA_LANES columns at a time, each
 * weight row read once per tile, and blocks of kb. With `ties` set the halfway
 * test is folded into one word per block: a step whose low 29 bits are
 * 0x10000000 makes (bits ^ 0x10000000) - 1 wrap, setting the top bit of the OR
 * of them all; such a block, and a tile at the edges, is done again output by
 * output with nn_fma. Without it a step lands on the float that rounding the
 * double sum gives, which is the fused result unless that sum was itself
 * rounded onto a halfway point (with f16 weights that needs one addend 2^18
 * times the other and 28 particular bits: never seen in the 10.6 billion steps
 * the two transposed convolutions take over the six fixtures). They use it so:
 * their f16 weights make exact halfway sums, which round correctly as they
 * are, so common that the test would send nearly every block to the slow path. */
#define FMA_ROWS 4
static void fma_tiles(const float *x, int m, int k, size_t ldx, const float *w, int n, size_t ldw, float *y,
                      size_t ldy, int kb, int ties) {
    for (int c0 = 0; c0 < n; c0 += FMA_LANES) {
        int nc = imin(FMA_LANES, n - c0);
        for (int r0 = 0; r0 < m; r0 += FMA_ROWS) {
            int nr = imin(FMA_ROWS, m - r0);
            float out[FMA_ROWS][FMA_LANES];
            for (int k0 = 0; k0 < k; k0 += kb) {
                int k1 = imin(k, k0 + kb);
                float acc[FMA_ROWS][FMA_LANES];
                uint64_t flag = 0;
                memset(acc, 0, sizeof acc);
                if (nc == FMA_LANES && nr == FMA_ROWS && ties) {
                    for (int i = k0; i < k1; i++) {
                        const float *wr = w + (size_t)i * ldw + c0;
                        double wd[FMA_LANES];
                        for (int l = 0; l < FMA_LANES; l++) wd[l] = wr[l];
                        for (int u = 0; u < FMA_ROWS; u++) {
                            double xu = x[(size_t)(r0 + u) * ldx + i], sv[FMA_LANES];
                            uint64_t uv[FMA_LANES];
                            for (int l = 0; l < FMA_LANES; l++) sv[l] = xu * wd[l] + (double)acc[u][l];
                            memcpy(uv, sv, sizeof uv);
                            for (int l = 0; l < FMA_LANES; l++) {
                                acc[u][l] = (float)sv[l];
                                flag |= ((uv[l] & 0x1FFFFFFFu) ^ 0x10000000u) - 1u;
                            }
                        }
                    }
                } else if (nc == FMA_LANES && nr == FMA_ROWS) {
                    for (int i = k0; i < k1; i++) {
                        const float *wr = w + (size_t)i * ldw + c0;
                        double wd[FMA_LANES];
                        for (int l = 0; l < FMA_LANES; l++) wd[l] = wr[l];
                        for (int u = 0; u < FMA_ROWS; u++) {
                            double xu = x[(size_t)(r0 + u) * ldx + i], sv[FMA_LANES];
                            for (int l = 0; l < FMA_LANES; l++) sv[l] = xu * wd[l] + (double)acc[u][l];
                            for (int l = 0; l < FMA_LANES; l++) acc[u][l] = (float)sv[l];
                        }
                    }
                }
                if (nc < FMA_LANES || nr < FMA_ROWS || flag >> 63)
                    for (int u = 0; u < nr; u++)
                        for (int l = 0; l < nc; l++) {
                            float c = 0;
                            const float *xr = x + (size_t)(r0 + u) * ldx;
                            for (int i = k0; i < k1; i++) c = nn_fma(xr[i], w[(size_t)i * ldw + c0 + l], c);
                            acc[u][l] = c;
                        }
                for (int u = 0; u < nr; u++)
                    for (int l = 0; l < nc; l++) out[u][l] = k0 ? acc[u][l] + out[u][l] : acc[u][l];
            }
            for (int u = 0; u < nr; u++) memcpy(y + (size_t)(r0 + u) * ldy + c0, out[u], sizeof(float) * (size_t)nc);
        }
    }
}

void nn_matmul_fma_kb(const float *x, int m, int k, size_t ldx, const float *w, int n, size_t ldw, float *y,
                         size_t ldy, int kb) {
    fma_tiles(x, m, k, ldx, w, n, ldw, y, ldy, kb, 1);
}

void nn_matmul_fma(const float *x, int m, int k, int ldx, const float *w, int n, int ldw, float *y, int ldy) {
    nn_matmul_fma_kb(x, m, k, (size_t)ldx, w, n, (size_t)ldw, y, (size_t)ldy, 256);
}

void nn_attention(const float *q, const float *k, const float *v, int ld, int n, int heads, int dh,
                     float scale, const float *mask, float *out, int ldo, float *scratch) {
    float *kt = scratch + n, *qs = kt + (size_t)n * dh;
    for (int h = 0; h < heads; h++) {
        size_t c0 = (size_t)h * dh;
        for (int j = 0; j < n; j++)                       /* k scaled, transposed: dh rows of n */
            for (int d = 0; d < dh; d++) kt[(size_t)d * n + j] = k[(size_t)j * ld + c0 + d] * scale;
        for (int i = 0; i < n; i++) {
            for (int d = 0; d < dh; d++) qs[d] = q[(size_t)i * ld + c0 + d] * scale;
            vecmat_fma(qs, dh, kt, (size_t)n, n, scratch);
            if (mask)
                for (int j = 0; j < n; j++) scratch[j] += mask[j];
            nn_softmax(scratch, n);
            vecmat_fma(scratch, n, v + c0, (size_t)ld, dh, out + (size_t)i * ldo + c0);
        }
    }
}

/* ---- float products, transposed convolutions -------------------------------- */

#define FR 8      /* float GEMM tile: 8 rows by 8 columns */
#define FC 8

void nn_matmul_f(const float *restrict x, size_t xstep, int m, int k, const float *restrict w, int n,
                    const float *restrict bias, float *restrict y, size_t ystep) {
    int npad = NN_ROUND(n, FC);
    for (int nb = 0; nb < npad; nb += FC) {
        int nv = imin(FC, n - nb);
        int r = 0;
        for (; r + FR <= m; r += FR) {
            float a[FR][FC];
            memset(a, 0, sizeof a);
            const float *xr = x + (size_t)r * xstep;
            for (int i = 0; i < k; i++) {
                const float *wv = w + (size_t)i * npad + nb;
                for (int u = 0; u < FR; u++) {
                    float xv = xr[(size_t)u * xstep + i];
                    for (int l = 0; l < FC; l++) a[u][l] += xv * wv[l];
                }
            }
            for (int u = 0; u < FR; u++) {
                float *yr = y + (size_t)(r + u) * ystep + nb;
                for (int l = 0; l < nv; l++) yr[l] = bias ? a[u][l] + bias[nb + l] : a[u][l];
            }
        }
        for (; r < m; r++) {
            float a[FC] = { 0 };
            const float *xr = x + (size_t)r * xstep;
            for (int i = 0; i < k; i++) {
                const float *wv = w + (size_t)i * npad + nb;
                float xv = xr[i];
                for (int l = 0; l < FC; l++) a[l] += xv * wv[l];
            }
            float *yr = y + (size_t)r * ystep + nb;
            for (int l = 0; l < nv; l++) yr[l] = bias ? a[l] + bias[nb + l] : a[l];
        }
    }
}

/* As the reference's ConvTranspose: for each tap j, the product of the input
 * rows with that tap's weights (its matrix product: chains of fused
 * multiply-adds in blocks of 128, as its packing has them when the weights are
 * read transposed; fma_tiles without the halfway test); then each output is
 * the sum of its taps' products in tap order (its col2im onto zeros), then the
 * bias. */
void nn_convt(const float *x, int t, int cin, int k, int s, int pad_begin, const float *w, const float *bias,
                 int cout, int t_out, float *y, int ldy) {
    int mt = k / s, npad = NN_ROUND(cout, FC);
    const float *xv = x + (size_t)(mt - 1) * cin;
    float *col = malloc(sizeof(float) * (size_t)t * cout);
    if (!col) return;
    for (int o = 0; o < t_out; o++) memset(y + (size_t)o * ldy, 0, sizeof(float) * (size_t)cout);
    for (int j = 0; j < k; j++) {
        int r = j % s, m = mt - 1 - j / s;
        fma_tiles(xv, t, cin, (size_t)cin, w + ((size_t)r * mt + m) * cin * npad, cout, (size_t)npad, col, (size_t)cout,
                  128, 0);
        for (int i = 0; i < t; i++) {
            int o = i * s + j - pad_begin;
            if (o < 0 || o >= t_out) continue;
            float *yr = y + (size_t)o * ldy;
            const float *cr = col + (size_t)i * cout;
            for (int c = 0; c < cout; c++) yr[c] += cr[c];
        }
    }
    if (bias)
        for (int o = 0; o < t_out; o++)
            for (int c = 0; c < cout; c++) y[(size_t)o * ldy + c] = y[(size_t)o * ldy + c] + bias[c];
    free(col);
}

void nn_convt_dw(const float *restrict x, int t, int c, int ld, int k, int s, int pad_begin,
                    const float *restrict w, const float *restrict bias, int t_out, float *restrict y, int ldy) {
    for (int o = 0; o < t_out; o++) {
        float *yr = y + (size_t)o * ldy;
        memset(yr, 0, sizeof(float) * (size_t)c);
        for (int j = 0; j < k; j++) {
            int u = o + pad_begin - j;
            if (u < 0 || u % s) continue;
            int i = u / s;
            if (i >= t) continue;
            const float *xr = x + (size_t)i * ld, *wr = w + (size_t)j * c;
            int ch = 0;
            for (; ch + FL <= c; ch += FL)
                for (int l = 0; l < FL; l++) yr[ch + l] += xr[ch + l] * wr[ch + l];
            for (; ch < c; ch++) yr[ch] += xr[ch] * wr[ch];
        }
        if (bias)
            for (int ch = 0; ch < c; ch++) yr[ch] += bias[ch];
    }
}

/* ---- elementwise -------------------------------------------------------------------- */

/* As the sum of the positive and the scaled negative part: a select with
 * arithmetic in one arm is not vectorised by GCC at -O2. */
void nn_leaky_relu(float *x, size_t n, float alpha) {
    size_t i = 0;
    for (; i + FL <= n; i += FL)
        for (int l = 0; l < FL; l++) {
            float v = x[i + l], pos = v > 0 ? v : 0.0f, neg = v < 0 ? v : 0.0f;
            x[i + l] = pos + neg * alpha;
        }
    for (; i < n; i++) x[i] = x[i] < 0 ? x[i] * alpha : x[i];
}

/* sin as the reference computes it, to the bit: Eigen's psincos_float as its
 * build has it (no fused multiply-add). x is reduced by the nearest multiple q
 * of pi/2, pi/2 in four parts, each step a product then a sum; then the sine
 * polynomial for even q and the cosine one for odd, the sign flipped for q mod 4
 * of 2 or 3 and for negative x. From |x| = 25966 on, where the four parts no
 * longer suffice, the reduction is Eigen's trig_reduce_huge instead: x times
 * 192 bits of 2/pi in fixed point (Payne and Hanek), the remainder times pi/2
 * in double. Every lane computes both polynomials and selects one, so the loop
 * vectorises; a block with a huge lane does those lanes again one at a time. */
#define SIN_HUGE 25966.0f
static inline uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

static const uint32_t TWO_OVER_PI[26] = {
    0x00000028, 0x000028be, 0x0028be60, 0x28be60db, 0xbe60db93, 0x60db9391, 0xdb939105, 0x9391054a,
    0x91054a7f, 0x054a7f09, 0x4a7f09d5, 0x7f09d5f4, 0x09d5f47d, 0xd5f47d4d, 0xf47d4d37, 0x7d4d3770,
    0x4d377036, 0x377036d8, 0x7036d8a5, 0x36d8a566, 0xd8a5664f, 0xa5664f10, 0x664f10e4, 0x4f10e410,
    0x10e41000, 0xe4100000
};

/* |x| >= SIN_HUGE, finite: x - q pi/2, with q in *quad. */
static float sin_reduce_huge(float a, int32_t *quad) {
    uint32_t xi = f2u(a), e = (xi >> 23) - 118;
    xi = ((xi & 0x007FFFFFu) | 0x00800000u) << (e & 7);
    uint32_t i = e >> 3;
    uint64_t p = (uint64_t)xi * TWO_OVER_PI[i + 7];
    p = (uint64_t)xi * TWO_OVER_PI[i + 3] + (p >> 32);
    p = ((uint64_t)(xi * TWO_OVER_PI[i - 1]) << 32) + p;
    uint64_t q = (p + ((uint64_t)1 << 61)) >> 62;
    *quad = (int32_t)q;
    p -= q << 62;
    return (float)((double)(int64_t)p * 3.4061215800865545e-19);
}

/* The polynomials on the reduced r, the quadrant q, the sign of the input. */
static inline float sin_poly(float x, float r, int32_t q) {
    float r2 = r * r;
    float c = 2.43722661e-05f;
    c = c * r2 + -1.38865202e-03f;
    c = c * r2 + 4.16666195e-02f;
    c = c * r2 + -0.5f;
    c = c * r2 + 1.0f;
    float s = -1.95923411e-04f;
    s = s * r2 + 8.33268743e-03f;
    s = s * r2 + -1.66666627e-01f;
    s = s * r2;
    s = s * r + r;
    float v = (q & 1) ? c : s;
    return u2f(f2u(v) ^ ((f2u(x) ^ ((uint32_t)q << 30)) & 0x80000000u));
}

static inline float sin_quick(float x, int32_t *quad) {
    float a = fabsf(x), y = a * 0.636619747f, yr = y + 12582912.0f;
    *quad = (int32_t)f2u(yr);
    y = yr - 12582912.0f;
    a = y * -1.5703125f + a;
    a = y * -4.83989716e-04f + a;
    a = y * 1.62865035e-07f + a;
    a = y * 5.56443155e-11f + a;
    return a;
}

static float sin1(float x) {
    int32_t q;
    float r = sin_quick(x, &q);
    if (fabsf(x) >= SIN_HUGE && fabsf(x) <= 3.40282347e+38f) r = sin_reduce_huge(fabsf(x), &q);
    return sin_poly(x, r, q);
}

/* sin of FL values; 1 if one of them is huge (then those are left to sin1). */
static inline int sin16(const float *restrict x, float *restrict y) {
    int huge = 0;
    for (int l = 0; l < FL; l++) {
        int32_t q;
        float r = sin_quick(x[l], &q);
        y[l] = sin_poly(x[l], r, q);
        huge |= fabsf(x[l]) >= SIN_HUGE;
    }
    return huge;
}

static void sin_fix(const float *x, float *y, size_t n) {
    for (size_t l = 0; l < n; l++)
        if (fabsf(x[l]) >= SIN_HUGE) y[l] = sin1(x[l]);
}

void nn_sin(const float *restrict x, float *restrict y, size_t n) {
    size_t i = 0;
    for (; i + FL <= n; i += FL)
        if (sin16(x + i, y + i)) sin_fix(x + i, y + i, FL);
    for (; i < n; i++) y[i] = sin1(x[i]);
}

/* atan as the reference computes it: its vectorised kernel (Eigen's, four lanes
 * of SSE) evaluates, on r = |x| or 1/|x| past 1, r times a rational function of
 * r^2 -- (0.112 r^2 + 0.730) r^2 + 0.811 over ((0.0101 r^2 + 0.283) r^2 + 1)
 * r^2 + 0.811 -- takes pi/2 less that past 1, and gives it x's sign. The
 * elements after the last whole vector of four (n mod 4 of them) it hands to
 * the C library's atanf, and so does this. */
static inline float atan_rat(float x) {
    float a = fabsf(x), r = 1.0f / a;
    int large = 1.0f < a;
    r = large ? r : a;
    float z = r * r;
    float num = z * 0.112026982f + 0.729669571f;
    num = num * z + 0.810995102f;
    float den = z * 0.0100917108f + 0.283185601f;
    den = den * z + 1.0f;
    den = den * z + 0.810995102f;
    float p = (num / den) * r;
    float v = large ? 1.57079637f - p : p;
    return u2f(f2u(v) ^ (f2u(x) & 0x80000000u));
}

void nn_atan(const float *restrict x, float *restrict y, size_t n) {
    size_t body = n / 4 * 4;
    for (size_t i = 0; i < body; i++) y[i] = atan_rat(x[i]);
    for (size_t i = body; i < n; i++) y[i] = atanf(x[i]);
}

void nn_snake(float *restrict x, int t, int c, int ld, const float *restrict alpha,
                 const float *restrict inv_alpha) {
    for (int r = 0; r < t; r++) {
        float *restrict xr = x + (size_t)r * ld;
        int ch = 0;
        for (; ch + FL <= c; ch += FL) {
            float a[FL], s[FL];
            for (int l = 0; l < FL; l++) a[l] = alpha[ch + l] * xr[ch + l];
            if (sin16(a, s)) sin_fix(a, s, FL);
            for (int l = 0; l < FL; l++) xr[ch + l] = xr[ch + l] + inv_alpha[ch + l] * (s[l] * s[l]);
        }
        for (; ch < c; ch++) {
            float s = sin1(alpha[ch] * xr[ch]);
            xr[ch] = xr[ch] + inv_alpha[ch] * (s * s);
        }
    }
}

/* ---- inverse STFT ----------------------------------------------------------------- */

#define ISTFT_MAXBINS 64        /* bins <= this */
void nn_istft(const float *spec, int t, int ld, int bins, const float *wr, const float *wi, int nfft, int hop,
                 int trim, float *out, float *full) {
    size_t len = (size_t)(t - 1) * hop + nfft;
    memset(full, 0, sizeof(float) * len);
    float re[ISTFT_MAXBINS], im[ISTFT_MAXBINS];
    for (int f = 0; f < t; f++) {
        const float *sr = spec + (size_t)f * ld;
        for (int b = 0; b < bins; b++) {
            float m = expf(sr[b]), ph = sinf(sr[bins + b]);
            re[b] = m * cosf(ph);
            im[b] = m * sinf(ph);
        }
        float *o = full + (size_t)f * hop;
        for (int j = 0; j < nfft; j++) {
            float a = 0, c = 0;
            for (int b = 0; b < bins; b++) {
                a += re[b] * wr[(size_t)b * nfft + j];
                c += im[b] * wi[(size_t)b * nfft + j];
            }
            o[j] += a - c;
        }
    }
    memcpy(out, full + trim, sizeof(float) * (len - 2 * (size_t)trim));
}

/* ---- for the recognizer ---------------------------------------------------------- */

void nn_matmul_qc(const int16_t *restrict a, int m, int k, const uint8_t *restrict b, const uint8_t *restrict bzp,
                  int n, int32_t *restrict y, int ldy, int16_t *restrict wbuf) {
    for (int c = 0; c < n; c += 4) {
        for (int r = 0; r < 4; r++) widen_u8(wbuf + (size_t)r * k, b + (size_t)(c + r) * k, bzp[c + r], (size_t)k);
        qblock(a, (size_t)k, 0, 1, k, m, wbuf, 4, y + c, ldy);
    }
}

void nn_dequant_qc(const int32_t *restrict acc, int rows, int cols, int lda, float ascale,
                   const float *restrict bscale, const float *restrict bias, float *restrict y, int ldy) {
    for (int r = 0; r < rows; r++) {
        const int32_t *a = acc + (size_t)r * lda;
        float *o = y + (size_t)r * ldy;
        for (int c = 0; c < cols; c++) {
            float mult = ascale * bscale[c];
            float v = (float)a[c] * mult;
            o[c] = bias ? v + bias[c] : v;
        }
    }
}

void nn_silu(float *x, size_t n) {
    float t[64];
    for (size_t i = 0; i < n; i += 64) {
        size_t m = n - i < 64 ? n - i : 64;
        memcpy(t, x + i, m * sizeof *t);
        nn_logistic(t, m);
        for (size_t l = 0; l < m; l++) x[i + l] = x[i + l] * t[l];
    }
}

/* erf(x) as the reference computes it (its MlasErfKernel, the same bits): odd,
 * |x| clamped to 3.925; below 0.921875 an odd polynomial in |x|, above it
 * 1 - exp(-p(|x|)) with its own exp; every multiply-add fused. */
static float erf1(float v) {
    uint32_t vu, sign;
    memcpy(&vu, &v, 4);
    sign = vu & 0x80000000u;
    float a = fabsf(v);
    a = a < 3.925f ? a : 3.925f;
    float y;
    if (!(a > 0.921875f)) {
        float sq = a * a, p = -5.99104969e-4f;
        p = nn_fma(p, sq, 4.99339588e-3f);
        p = nn_fma(p, sq, -2.67667342e-2f);
        p = nn_fma(p, sq, 1.12818025e-1f);
        p = nn_fma(p, sq, -3.76124859e-1f);
        p = nn_fma(p, sq, 1.28379151e-1f);
        y = nn_fma(p, a, a);
    } else {
        float p = 1.72948930e-5f;
        p = nn_fma(p, a, -3.83208680e-4f);
        p = nn_fma(p, a, 3.88393435e-3f);
        p = nn_fma(p, a, -2.42545605e-2f);
        p = nn_fma(p, a, 1.06777847e-1f);
        p = nn_fma(p, a, 6.34846687e-1f);
        p = nn_fma(p, a, 1.28717512e-1f);
        p = -nn_fma(p, a, a);
        p = p < -88.3762626647949f ? -88.3762626647949f : p;
        float r = nn_fma(1.44269504088896341f, p, 12582912.0f) - 12582912.0f;
        float f = nn_fma(r, -6.93145752e-1f, p);
        f = nn_fma(r, -1.42860677e-6f, f);
        float e = 1.38319808e-3f;
        e = nn_fma(e, f, 8.37550033e-3f);
        e = nn_fma(e, f, 4.16689515e-2f);
        e = nn_fma(e, f, 1.66664466e-1f);
        e = nn_fma(e, f, 4.99999851e-1f);
        e = nn_fma(e, f, 1.0f);
        e = nn_fma(e, f, 1.0f);
        uint32_t pu = (uint32_t)((int32_t)r + 127) << 23;
        float p2;
        memcpy(&p2, &pu, 4);
        y = 1.0f - e * p2;
    }
    uint32_t yu;
    memcpy(&yu, &y, 4);
    yu |= sign;
    memcpy(&y, &yu, 4);
    return y;
}

void nn_gelu_erf(float *x, size_t n) {
    for (size_t i = 0; i < n; i++) {
        float v = x[i], e = erf1(v / 1.41421353816986083984375f);
        float h = 0.5f * (e + 1.0f);
        x[i] = v * h;
    }
}

void nn_rope_pairs(float *x, int rot, const float *inv_freq, float pos) {
    for (int i = 0; i < rot / 2; i++) {
        float f = pos * inv_freq[i], c = cosf(f), s = sinf(f);
        float a = x[2 * i], b = x[2 * i + 1];
        float ac = a * c, bs = b * s, bc = b * c, as = a * s;
        x[2 * i] = ac - bs;
        x[2 * i + 1] = bc + as;
    }
}

static float dot(const float *restrict a, const float *restrict b, int n) {
    float acc[FL] = { 0 };
    int i = 0;
    for (; i + FL <= n; i += FL)
        for (int l = 0; l < FL; l++) acc[l] += a[i + l] * b[i + l];
    float s = 0;
    for (int l = 0; l < FL; l++) s += acc[l];
    for (; i < n; i++) s += a[i] * b[i];
    return s;
}

void nn_attend(const float *q, const float *k, int ldk, const float *v, int ldv, int lo, int hi, int dh, float scale,
               float *out, float *scratch) {
    int n = hi - lo;
    for (int d = 0; d < dh; d++) out[d] = 0;
    if (n <= 0) return;
    for (int j = 0; j < n; j++) scratch[j] = dot(q, k + (size_t)(lo + j) * ldk, dh) * scale;
    nn_softmax(scratch, n);
    for (int j = 0; j < n; j++) {
        const float *vr = v + (size_t)(lo + j) * ldv;
        float p = scratch[j];
        for (int d = 0; d < dh; d++) out[d] += p * vr[d];
    }
}

/* A chain of fused multiply-adds from 0 over n products, as the float matrix
 * product computes one output: in double, each step rounded to float, with
 * the rare step that lands exactly halfway done again with nn_fma. */
static float fma_chain(const float *restrict a, const float *restrict b, int n) {
    float acc = 0;
    uint32_t bad = 0;
    for (int i = 0; i < n; i++) {
        double sv = (double)a[i] * (double)b[i] + (double)acc;
        uint64_t u;
        memcpy(&u, &sv, sizeof u);
        bad |= ((uint32_t)u & 0x1FFFFFFFu) == 0x10000000u;
        acc = (float)sv;
    }
    if (bad) {
        acc = 0;
        for (int i = 0; i < n; i++) acc = nn_fma(a[i], b[i], acc);
    }
    return acc;
}

void nn_attend_exact(const float *q, const float *k, int ldk, const float *v, int ldv, int n, int lo, int hi,
                     int dh, float scale, int add, float *out, float *scratch) {
    for (int j = 0; j < n; j++) {
        float s = fma_chain(q, k + (size_t)j * ldk, dh) * scale;
        int in = j >= lo && j < hi;
        scratch[j] = add ? s + (in ? 0.0f : -INFINITY) : (in ? s : -INFINITY);
    }
    nn_softmax(scratch, n);
    vecmat_fma(scratch, n, v, (size_t)ldv, dh, out);
}

int nn_argmax(const float *x, int n) {
    int best = 0;
    for (int i = 1; i < n; i++)
        if (x[i] > x[best]) best = i;
    return best;
}

void nn_matvec_u8(const float *restrict x, int k, const uint8_t *restrict e, int zp, float scale, int n,
                  float *restrict out) {
    float w[FL];
    for (int r = 0; r < n; r++) {
        const uint8_t *row = e + (size_t)r * k;
        float acc[FL] = { 0 };
        int i = 0;
        for (; i + FL <= k; i += FL) {
            for (int l = 0; l < FL; l++) w[l] = (float)(row[i + l] - zp) * scale;
            for (int l = 0; l < FL; l++) acc[l] += x[i + l] * w[l];
        }
        float s = 0;
        for (int l = 0; l < FL; l++) s += acc[l];
        for (; i < k; i++) s += x[i] * ((float)(row[i] - zp) * scale);
        out[r] = s;
    }
}


/* ---- the summary model's kernels ---------------------------------------------
 * The quantised blocks are ggml's Q4_K and Q6_K (nnkit.h). A block's values are
 * unpacked to int8 once and multiplied by every activation row; the products of
 * a block are summed exactly in int32, sub-block scales included, and only the
 * block's integers become float. */

float nn_f16_to_f32(uint16_t h) {
    uint32_t sign = (uint32_t)(h & 0x8000) << 16, exp = (h >> 10) & 0x1f, man = h & 0x3ff, u;
    if (exp == 0) {
        if (man == 0) u = sign;
        else {                                     /* subnormal: normalise */
            exp = 127 - 15 + 1;
            while (!(man & 0x400)) { man <<= 1; exp--; }
            man &= 0x3ff;
            u = sign | exp << 23 | man << 13;
        }
    } else if (exp == 31) u = sign | 0x7f800000u | man << 13;
    else u = sign | (exp + 127 - 15) << 23 | man << 13;
    union { uint32_t u; float f; } v = { .u = u };
    return v.f;
}

uint16_t nn_f32_to_f16(float f) {
    union { float f; uint32_t u; } v = { .f = f };
    uint32_t u = v.u, sign = (u >> 16) & 0x8000;
    int32_t exp = (int32_t)((u >> 23) & 0xff) - 127 + 15;
    uint32_t man = u & 0x7fffff;
    if (((u >> 23) & 0xff) == 0xff) return (uint16_t)(sign | 0x7c00 | (man ? 0x200 : 0));
    if (exp >= 31) return (uint16_t)(sign | 0x7c00);
    if (exp <= 0) {                                /* subnormal or zero */
        if (exp < -10) return (uint16_t)sign;
        man |= 0x800000;
        uint32_t shift = (uint32_t)(14 - exp), h = man >> shift, rem = man & ((1u << shift) - 1),
                 half = 1u << (shift - 1);
        if (rem > half || (rem == half && (h & 1))) h++;
        return (uint16_t)(sign | h);
    }
    uint32_t h = (uint32_t)exp << 10 | man >> 13, rem = man & 0x1fff;
    if (rem > 0x1000 || (rem == 0x1000 && (h & 1))) h++;   /* may carry into the exponent: right */
    return (uint16_t)(sign | h);
}

size_t nn_row_bytes(nn_wtype t, int k) {
    switch (t) {
    case NN_W_F32:  return (size_t)k * 4;
    case NN_W_BF16: return (size_t)k * 2;
    case NN_W_Q4K:  return (size_t)(k / NN_QK) * NN_Q4K_BYTES;
    case NN_W_Q6K:  return (size_t)(k / NN_QK) * NN_Q6K_BYTES;
    }
    return 0;
}

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* A Q4_K block's 6-bit scale and min of sub-block j (ggml's get_scale_min_k4). */
static inline void q4k_scale_min(int j, const uint8_t *q, int *sc, int *m) {
    if (j < 4) {
        *sc = q[j] & 63;
        *m = q[j + 4] & 63;
    } else {
        *sc = (q[j + 4] & 0xF) | ((q[j - 4] >> 6) << 4);
        *m = (q[j + 4] >> 4) | ((q[j] >> 6) << 4);
    }
}

/* A block unpacked: its 256 values as integers and its integer sub-block
 * scales (Q4_K: 8 of 32, with mins; Q6_K: 16 of 16). */
typedef struct {
    int8_t  q[NN_QK];
    int16_t q16[NN_QK];        /* each value times its sub-block's scale (at most 945 or 4064 in size) */
    int     sc[16], mn[8];
    float   d, dmin;
} kblock;

static void unpack_q4k(const uint8_t *b, kblock *u) {
    u->d = nn_f16_to_f32(rd16(b));
    u->dmin = nn_f16_to_f32(rd16(b + 2));
    const uint8_t *scales = b + 4, *qs = b + 16;
    for (int s = 0; s < 8; s++) q4k_scale_min(s, scales, &u->sc[s], &u->mn[s]);
    for (int c = 0; c < 4; c++)
        for (int l = 0; l < 32; l++) {
            u->q[64 * c + l] = (int8_t)(qs[32 * c + l] & 0xF);
            u->q[64 * c + 32 + l] = (int8_t)(qs[32 * c + l] >> 4);
        }
    for (int e = 0; e < NN_QK; e++) u->q16[e] = (int16_t)(u->q[e] * u->sc[e / 32]);
}

static void unpack_q6k(const uint8_t *b, kblock *u) {
    const uint8_t *ql = b, *qh = b + 128;
    const int8_t *sc = (const int8_t *)(b + 192);
    u->d = nn_f16_to_f32(rd16(b + 208));
    u->dmin = 0;
    for (int s = 0; s < 16; s++) u->sc[s] = sc[s];
    for (int h = 0; h < 2; h++) {
        const uint8_t *l0 = ql + 64 * h, *hh = qh + 32 * h;
        int8_t *y = u->q + 128 * h;
        for (int l = 0; l < 32; l++) {
            y[l]      = (int8_t)(((l0[l] & 0xF) | ((hh[l] >> 0 & 3) << 4)) - 32);
            y[l + 32] = (int8_t)(((l0[l + 32] & 0xF) | ((hh[l] >> 2 & 3) << 4)) - 32);
            y[l + 64] = (int8_t)(((l0[l] >> 4) | ((hh[l] >> 4 & 3) << 4)) - 32);
            y[l + 96] = (int8_t)(((l0[l + 32] >> 4) | ((hh[l] >> 6 & 3) << 4)) - 32);
        }
    }
    for (int e = 0; e < NN_QK; e++) u->q16[e] = (int16_t)(u->q[e] * u->sc[e / 16]);
}

void nn_dequant_row(nn_wtype t, const void *w, int k, int r, float *out) {
    const uint8_t *row = (const uint8_t *)w + (size_t)r * nn_row_bytes(t, k);
    if (t == NN_W_F32) { memcpy(out, row, (size_t)k * 4); return; }
    if (t == NN_W_BF16) {
        for (int i = 0; i < k; i++) out[i] = nn_bf16_to_f32(rd16(row + 2 * i));
        return;
    }
    kblock u;
    for (int b = 0; b < k / NN_QK; b++) {
        float *o = out + b * NN_QK;
        if (t == NN_W_Q4K) {
            unpack_q4k(row + (size_t)b * NN_Q4K_BYTES, &u);
            for (int e = 0; e < NN_QK; e++)
                o[e] = u.d * (float)u.sc[e / 32] * (float)u.q[e] - u.dmin * (float)u.mn[e / 32];
        } else {
            unpack_q6k(row + (size_t)b * NN_Q6K_BYTES, &u);
            for (int e = 0; e < NN_QK; e++) o[e] = u.d * (float)u.sc[e / 16] * (float)u.q[e];
        }
    }
}

void nn_q8k_quantize(const float *restrict x, int k, nn_q8k *restrict out) {
    for (int b = 0; b < k / NN_QK; b++) {
        const float *v = x + b * NN_QK;
        nn_q8k *o = out + b;
        float amax = 0;
        for (int i = 0; i < NN_QK; i++) amax = fmaxf(amax, fabsf(v[i]));
        if (amax == 0) {
            memset(o, 0, sizeof *o);
            continue;
        }
        float id = 127.0f / amax;
        o->d = amax / 127.0f;
        for (int i = 0; i < NN_QK; i++) {
            float r = rint_even(v[i] * id);
            o->q[i] = (int8_t)(r > 127 ? 127 : r < -127 ? -127 : r);
        }
        for (int s = 0; s < NN_QK / 32; s++) {
            int sum = 0;
            for (int i = 0; i < 32; i++) sum += o->q[32 * s + i];
            o->bsum[s] = (int16_t)sum;
        }
    }
}

size_t nn_matmul_w_scratch(nn_wtype t, int k, int m) {
    if (t == NN_W_Q4K || t == NN_W_Q6K)
        return (size_t)m * (size_t)(k / NN_QK) * sizeof(nn_q8k) + (size_t)m * (size_t)k * sizeof(int16_t) +
               (size_t)m * KT * sizeof(float) + KT * sizeof(kblock) + 128;
    return (size_t)k * sizeof(float) + 64;
}

/* The exact integer products of one unpacked block with one activation block:
 * Q4_K's sum of sc * (q . a) and of mn * sum(a) per sub-block; Q6_K's sum of
 * sc * (q . a) per 16. */
static inline void kdot(const kblock *u, int q6, const int16_t *x16, const int16_t *bsum, int32_t *isum, int32_t *imin) {
    /* sum of sc * (q . x) over the sub-blocks, as one sum of (sc q) . x */
    int32_t s = 0, mn = 0;
    for (int i = 0; i < NN_QK; i++) s += (int32_t)u->q16[i] * (int32_t)x16[i];
    if (!q6)
        for (int sb = 0; sb < 8; sb++) mn += u->mn[sb] * (int32_t)bsum[sb];
    *isum = s;
    *imin = mn;
}

/* The same integers straight from a block's bytes, for one activation block:
 * no unpacking, the loops the vectoriser turns into multiply-add-pairs. */
static inline void q4k_dot(const uint8_t *b, const nn_q8k *a, const int16_t *x16, int32_t *isum, int32_t *imin) {
    const uint8_t *scales = b + 4, *qs = b + 16;
    int32_t s = 0, mn = 0;
    for (int c = 0; c < 4; c++) {
        const uint8_t *q = qs + 32 * c;
        const int16_t *x = x16 + 64 * c;
        int32_t lo = 0, hi = 0;
        for (int l = 0; l < 32; l++) {
            lo += (int32_t)(int16_t)(q[l] & 0xF) * x[l];
            hi += (int32_t)(int16_t)(q[l] >> 4) * x[l + 32];
        }
        int sc0, m0, sc1, m1;
        q4k_scale_min(2 * c, scales, &sc0, &m0);
        q4k_scale_min(2 * c + 1, scales, &sc1, &m1);
        s += sc0 * lo + sc1 * hi;
        mn += m0 * (int32_t)a->bsum[2 * c] + m1 * (int32_t)a->bsum[2 * c + 1];
    }
    *isum = s;
    *imin = mn;
}

static inline int32_t q6k_dot(const uint8_t *b, const int16_t *x16) {
    const int8_t *sc = (const int8_t *)(b + 192);
    int32_t s = 0;
    for (int h = 0; h < 2; h++) {
        const uint8_t *ql = b + 64 * h, *qh = b + 128 + 32 * h;
        const int16_t *x = x16 + 128 * h;
        for (int half = 0; half < 2; half++) {
            int32_t p1 = 0, p2 = 0, p3 = 0, p4 = 0;
            for (int l = 16 * half; l < 16 * half + 16; l++) {
                p1 += (int32_t)(int16_t)(((ql[l] & 0xF) | ((qh[l] & 3) << 4)) - 32) * x[l];
                p2 += (int32_t)(int16_t)(((ql[l + 32] & 0xF) | ((qh[l] >> 2 & 3) << 4)) - 32) * x[l + 32];
                p3 += (int32_t)(int16_t)(((ql[l] >> 4) | ((qh[l] >> 4 & 3) << 4)) - 32) * x[l + 64];
                p4 += (int32_t)(int16_t)(((ql[l + 32] >> 4) | ((qh[l] >> 6 & 3) << 4)) - 32) * x[l + 96];
            }
            int is = 8 * h + half;
            s += sc[is] * p1 + sc[is + 2] * p2 + sc[is + 4] * p3 + sc[is + 6] * p4;
        }
    }
    return s;
}

void nn_matmul_w(nn_wtype t, const void *w, int n, int k, const float *x, int m, size_t ldx, float *y,
                 size_t ldy, void *scratch) {
    size_t rb = nn_row_bytes(t, k);
    uintptr_t sp = ((uintptr_t)scratch + 63) & ~(uintptr_t)63;
    if (t == NN_W_Q4K || t == NN_W_Q6K) {
        int nb = k / NN_QK, q6 = t == NN_W_Q6K;
        size_t bb = q6 ? NN_Q6K_BYTES : NN_Q4K_BYTES;
        nn_q8k *a = (nn_q8k *)sp;
        int16_t *a16 = (int16_t *)(a + (size_t)m * nb);
        kblock *u = (kblock *)(((uintptr_t)(a16 + (size_t)m * k) + 63) & ~(uintptr_t)63);   /* KT of them */
        float *acc = (float *)(u + KT);                                                        /* m x KT */
        for (int r = 0; r < m; r++) {
            nn_q8k_quantize(x + (size_t)r * ldx, k, a + (size_t)r * nb);
            for (int i = 0; i < k; i++) a16[(size_t)r * k + i] = a[(size_t)r * nb + i / NN_QK].q[i % NN_QK];
        }
        if (m <= 2) {
            /* a token at a time: straight from the bytes */
            for (int c = 0; c < n; c++) {
                const uint8_t *row = (const uint8_t *)w + (size_t)c * rb;
                for (int r = 0; r < m; r++) acc[r] = 0;
                for (int b = 0; b < nb; b++) {
                    const uint8_t *blk = row + (size_t)b * bb;
                    float d = nn_f16_to_f32(rd16(q6 ? blk + 208 : blk)), dmin = q6 ? 0.0f : nn_f16_to_f32(rd16(blk + 2));
                    for (int r = 0; r < m; r++) {
                        const nn_q8k *ab = a + (size_t)r * nb + b;
                        int32_t is, im = 0;
                        const int16_t *x16 = a16 + (size_t)r * k + (size_t)b * NN_QK;
                        if (q6) is = q6k_dot(blk, x16);
                        else q4k_dot(blk, ab, x16, &is, &im);
                        acc[r] += d * ab->d * (float)is - dmin * ab->d * (float)im;
                    }
                }
                for (int r = 0; r < m; r++) y[(size_t)r * ldy + c] = acc[r];
            }
            return;
        }
        /* Several tokens: KT weight rows at a time, each block unpacked once and
         * every token's activations read once for all KT of them. Each output
         * is still its blocks' terms added in order. */
        for (int c0 = 0; c0 < n; c0 += KT) {
            int kt = n - c0 < KT ? n - c0 : KT;
            for (size_t i = 0; i < (size_t)m * KT; i++) acc[i] = 0;
            for (int b = 0; b < nb; b++) {
                for (int cc = 0; cc < kt; cc++) {
                    const uint8_t *blk = (const uint8_t *)w + (size_t)(c0 + cc) * rb + (size_t)b * bb;
                    if (q6) unpack_q6k(blk, &u[cc]);
                    else unpack_q4k(blk, &u[cc]);
                }
                for (int r = 0; r < m; r++) {
                    const nn_q8k *ab = a + (size_t)r * nb + b;
                    const int16_t *x16 = a16 + (size_t)r * k + (size_t)b * NN_QK;
                    float *ar = acc + (size_t)r * KT;
                    for (int cc = 0; cc < kt; cc++) {
                        int32_t is, im;
                        kdot(&u[cc], q6, x16, ab->bsum, &is, &im);
                        ar[cc] += u[cc].d * ab->d * (float)is - u[cc].dmin * ab->d * (float)im;
                    }
                }
            }
            for (int r = 0; r < m; r++)
                for (int cc = 0; cc < kt; cc++) y[(size_t)r * ldy + c0 + cc] = acc[(size_t)r * KT + cc];
        }
        return;
    }
    float *wr = (float *)sp;
    for (int c = 0; c < n; c++) {
        nn_dequant_row(t, w, k, c, wr);
        for (int r = 0; r < m; r++) {
            const float *xr = x + (size_t)r * ldx;
            float p[FL] = { 0 };
            int i = 0;
            for (; i + FL <= k; i += FL)
                for (int l = 0; l < FL; l++) p[l] += xr[i + l] * wr[i + l];
            float s = 0;
            for (int l = 0; l < FL; l++) s += p[l];
            for (; i < k; i++) s += xr[i] * wr[i];
            y[(size_t)r * ldy + c] = s;
        }
    }
}

static float sumsq(const float *x, int n) {
    float p[FL] = { 0 };
    int i = 0;
    for (; i + FL <= n; i += FL)
        for (int l = 0; l < FL; l++) p[l] += x[i + l] * x[i + l];
    float s = 0;
    for (int l = 0; l < FL; l++) s += p[l];
    for (; i < n; i++) s += x[i] * x[i];
    return s;
}

void nn_rmsnorm(float *x, int rows, int n, int ld, const float *g, float eps) {
    for (int r = 0; r < rows; r++) {
        float *v = x + (size_t)r * ld;
        float sc = 1.0f / sqrtf(sumsq(v, n) / (float)n + eps);
        for (int i = 0; i < n; i++) v[i] = v[i] * sc * g[i];
    }
}

void nn_l2norm(float *x, int rows, int n, int ld, float eps) {
    for (int r = 0; r < rows; r++) {
        float *v = x + (size_t)r * ld;
        float sc = 1.0f / fmaxf(sqrtf(sumsq(v, n)), eps);
        for (int i = 0; i < n; i++) v[i] *= sc;
    }
}

void nn_mul_sigmoid(float *restrict x, const float *restrict g, size_t n) {
    float t[64];
    for (size_t i = 0; i < n; i += 64) {
        size_t m = n - i < 64 ? n - i : 64;
        memcpy(t, g + i, m * sizeof *t);
        nn_logistic(t, m);
        for (size_t l = 0; l < m; l++) x[i + l] *= t[l];
    }
}

void nn_mul_silu(float *restrict x, const float *restrict g, size_t n) {
    float t[64];
    for (size_t i = 0; i < n; i += 64) {
        size_t m = n - i < 64 ? n - i : 64;
        memcpy(t, g + i, m * sizeof *t);
        nn_silu(t, m);
        for (size_t l = 0; l < m; l++) x[i + l] *= t[l];
    }
}

void nn_conv_causal(const float *x, int t, int c, size_t ldx, float *state, int k, const float *w, float *y,
                    size_t ldy) {
    /* in[i] is state row i for i < k - 1, else x row i - (k - 1). Each output row
     * is computed from rows already read before it is written, so y may be x:
     * the window is held in `win`, k rows of c, refreshed one row a step. */
    float *win = malloc((size_t)k * (size_t)c * sizeof *win);
    if (!win) return;
    memcpy(win, state, (size_t)(k - 1) * (size_t)c * sizeof *win);
    for (int i = 0; i < t; i++) {
        memcpy(win + (size_t)(k - 1) * c, x + (size_t)i * ldx, (size_t)c * sizeof *win);
        float *o = y + (size_t)i * ldy;
        for (int ch = 0; ch < c; ch++) o[ch] = win[ch] * w[ch];
        for (int j = 1; j < k; j++) {
            const float *wi = win + (size_t)j * c, *wj = w + (size_t)j * c;
            for (int ch = 0; ch < c; ch++) o[ch] += wi[ch] * wj[ch];
        }
        memmove(win, win + c, (size_t)(k - 1) * (size_t)c * sizeof *win);
    }
    memcpy(state, win, (size_t)(k - 1) * (size_t)c * sizeof *win);
    free(win);
}

void nn_gated_delta(const float *q, const float *k, size_t ldq, const float *v, size_t ldv, const float *g,
                    const float *beta, int t, int hk, int hv, int dk, int dv, float qscale, float *state,
                    float *o, size_t ldo) {
    int per = hv / hk;
    float *u = malloc((size_t)dv * 2 * sizeof *u);
    if (!u) return;
    float *ov = u + dv;
    for (int s = 0; s < t; s++)
        for (int h = 0; h < hv; h++) {
            const float *qh = q + (size_t)s * ldq + (size_t)(h / per) * dk;
            const float *kh = k + (size_t)s * ldq + (size_t)(h / per) * dk;
            const float *vh = v + (size_t)s * ldv + (size_t)h * dv;
            float *S = state + (size_t)h * dk * dv;
            float decay = expf(g[(size_t)s * hv + h]), b = beta[(size_t)s * hv + h];
            /* S *= decay; u = v - S^T k */
            for (int j = 0; j < dv; j++) u[j] = 0;
            for (int i = 0; i < dk; i++) {
                float *Si = S + (size_t)i * dv, ki = kh[i];
                for (int j = 0; j < dv; j++) {
                    Si[j] *= decay;
                    u[j] += Si[j] * ki;
                }
            }
            for (int j = 0; j < dv; j++) u[j] = (vh[j] - u[j]) * b;
            /* S += k u^T; o = S^T q */
            for (int j = 0; j < dv; j++) ov[j] = 0;
            for (int i = 0; i < dk; i++) {
                float *Si = S + (size_t)i * dv, ki = kh[i], qi = qh[i] * qscale;
                for (int j = 0; j < dv; j++) {
                    Si[j] += ki * u[j];
                    ov[j] += Si[j] * qi;
                }
            }
            memcpy(o + (size_t)s * ldo + (size_t)h * dv, ov, (size_t)dv * sizeof *ov);
        }
    free(u);
}

void nn_rope_half(float *x, int rot, const float *inv_freq, float pos) {
    int half = rot / 2;
    for (int i = 0; i < half; i++) {
        float f = pos * inv_freq[i], c = cosf(f), s = sinf(f), a = x[i], b = x[i + half];
        x[i] = a * c - b * s;
        x[i + half] = b * c + a * s;
    }
}

void nn_q8_0_quantize(const float *restrict x, int n, nn_q8_0 *restrict out) {
    for (int b = 0; b < n / NN_Q8_0; b++) {
        const float *v = x + b * NN_Q8_0;
        float amax = 0;
        for (int i = 0; i < NN_Q8_0; i++) amax = fmaxf(amax, fabsf(v[i]));
        out[b].d = amax / 127.0f;
        float id = amax > 0 ? 127.0f / amax : 0.0f;
        for (int i = 0; i < NN_Q8_0; i++) out[b].q[i] = (int8_t)rint_even(v[i] * id);
    }
}

void nn_attend_q8(const float *q, const nn_q8_0 *k, const nn_q8_0 *v, size_t ld, int n, int dh, float scale,
                  float *out, float *scratch) {
    int nb = dh / NN_Q8_0;
    float mx = -INFINITY;
    for (int j = 0; j < n; j++) {
        const nn_q8_0 *kj = k + (size_t)j * ld;
        float s = 0;
        for (int b = 0; b < nb; b++) {
            float p = 0;
            for (int i = 0; i < NN_Q8_0; i++) p += q[b * NN_Q8_0 + i] * (float)kj[b].q[i];
            s += p * kj[b].d;
        }
        scratch[j] = s * scale;
        if (scratch[j] > mx) mx = scratch[j];
    }
    float sum = 0;
    for (int j = 0; j < n; j++) {
        scratch[j] = expf(scratch[j] - mx);
        sum += scratch[j];
    }
    for (int i = 0; i < dh; i++) out[i] = 0;
    for (int j = 0; j < n; j++) {
        const nn_q8_0 *vj = v + (size_t)j * ld;
        float p = scratch[j] / sum;
        for (int b = 0; b < nb; b++) {
            float pd = p * vj[b].d;
            for (int i = 0; i < NN_Q8_0; i++) out[b * NN_Q8_0 + i] += pd * (float)vj[b].q[i];
        }
    }
}
