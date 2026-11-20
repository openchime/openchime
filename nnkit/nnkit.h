/*
 * OpenChime — nnkit: the plain-C neural network kernels shared by ttskit
 * (read-aloud's voice model) and sttkit (voice input's recognizer).
 *
 * No dependencies, no intrinsics, no threads: ordinary loops the compiler's
 * auto-vectoriser turns into vector code, compiled with -ffp-contract=off so
 * every machine computes the same values (docs/TTSKIT.md §5b, docs/STTKIT.md).
 */
#ifndef NNKIT_H
#define NNKIT_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>

/* ---- the kernels (nn_ops.c) --------------------------------------------------
 * Plain C, one thread, written for the compiler's auto-vectoriser at -O2: the
 * hot loops have a fixed trip count (16 or 64), integer products accumulate in
 * int32, and float work is in an order fixed by the source (-ffp-contract=off),
 * so every machine computes the same samples.
 *
 * Activations are time-major: a sequence of T steps of C channels is T rows,
 * row t holding its C channels contiguously, `ld` values apart.
 *
 * Quantised activations are int16 rows holding q - zero_point (in -255..255),
 * `cin` values per row with `cin` a multiple of NN_CPAD; the columns past the
 * real channel count are 0. A convolution's input carries its padding as rows of
 * zeros before and after the T rows (ONNX pads ConvInteger's input with its zero
 * point, which is 0 here).
 *
 * Weight layouts, as voice.bin stores them (the converter produces these):
 *   ConvInteger   uint8 [round4(cout)][k][cin_pad]: row co, tap j, input channel
 *                 i at (co * k + j) * cin_pad + i, as the model ships it (not
 *                 offset); padding (channels past cin, rows past cout) is the
 *                 weight zero point. One zero point per tensor.
 *   MatMulInteger int8 [round4(n)][k_pad]: the ONNX B [k][n] transposed, one row
 *                 per output column, padding the zero point. One zero point.
 *   LSTM          int8 W [dir][4H][in_pad] and R [dir][4H][H], the contrib op's
 *                 [dir][in][4H] and [dir][H][4H] transposed, gates i, o, f, c;
 *                 B [dir][8H] f32 as the model has it; a scale and zero point
 *                 per direction for each of W and R. voice.bin keeps each
 *                 direction's W, R and B as a tensor of its own.
 *   ConvTranspose f32 [s][k/s][cin][round8(cout)]: phase r, window row m,
 *                 input channel i, output channel o holds ONNX W[i][o][r + (k/s
 *                 - 1 - m) * s]; padding 0.
 *   depthwise ConvTranspose  f32 [k][channels]: ONNX W[c][0][j] at
 *                 j * channels + c.
 *   float MatMul  f32 [k][round8(n)], ONNX B as it is with rows padded with 0.
 *   Conv on one input channel (the forward STFT's bases)  f32 [k][round8(cout)]:
 *                 ONNX W[o][0][j] at j * round8(cout) + o, a float MatMul over
 *                 windows `stride` apart.
 *   iSTFT         f32 [bins][nfft] for each of the real and imaginary bases. */
#define NN_CPAD  16                     /* quantised rows: channels padded to this */
#define NN_ROUND(n, m) (((n) + (m) - 1) / (m) * (m))
/* int16 scratch for nn_conv_q / nn_matmul_q: four widened weight rows */
#define NN_WBUF(k, cin_pad) ((size_t)4 * (size_t)(k) * (size_t)(cin_pad))

/* DynamicQuantizeLinear's parameters: scale = (max - min) / 255 over the range
 * widened to include 0, zero point = round_half_even(clamp(-min / scale, 0, 255)).
 * An all-zero tensor gets scale 0 and zero point 0 (every product is then 0). */
typedef struct { float scale; int zp; } nn_qparam;
void nn_dql_params(float min, float max, nn_qparam *qp);
/* ONNX DynamicQuantizeLinear on n floats: q = clamp(round_half_even(x / scale) + zp). */
void nn_dql_u8(const float *restrict x, size_t n, uint8_t *restrict q, nn_qparam *qp);
/* The same over a rows x cols tensor (rows `ldx` apart), written as q - zp into
 * int16 rows `ldq` apart, columns cols..ldq-1 set to 0. */
void nn_quantize(const float *restrict x, int rows, int cols, int ldx, int16_t *restrict q, int ldq,
                    nn_qparam *qp);
/* ConvInteger, 1-D: y[t][co] = sum_j sum_i x[t * stride + j * dil][i] * (w[co][j][i] - wzp)
 * for t < t_out, co < cout, x's rows `cin` wide (including the leading padding
 * rows: x points at the first one). Exact int32. k * cin <= 32768. */
void nn_conv_q(const int16_t *restrict x, int t_out, int stride, int dil, int k, int cin,
                  const uint8_t *restrict w, int wzp, int cout, int32_t *restrict y, int ldy,
                  int16_t *restrict wbuf);
/* MatMulInteger: y[r][c] = sum_i a[r][i] * (b[c][i] - bzp) for r < m, c < n; a's rows
 * k wide (k a multiple of NN_CPAD). Exact int32. */
void nn_matmul_q(const int16_t *restrict a, int m, int k, const int8_t *restrict b, int bzp, int n,
                    int32_t *restrict y, int ldy, int16_t *restrict wbuf);
/* The graph's Cast, Mul by the product of the two scales, Add bias (NULL: none). */
void nn_dequant(const int32_t *restrict acc, int rows, int cols, int lda, float scale,
                   const float *restrict bias, float *restrict y, int ldy);

/* com.microsoft DynamicQuantizeLSTM (ONNX Runtime's contrib op), batch 1, zero
 * initial state, no peepholes, default activations: per direction the whole input
 * is quantised once (one scale) and multiplied by W; each step quantises h(t-1)
 * on its own and multiplies it by R; gates = xW + hR + (Wb + Rb), in the order
 * i, o, f, c; c = f * c + i * tanh(g_c); h = o * tanh(c), sigmoid on i, o, f. The
 * forward direction writes y[t][0..H), the reverse y[t][H..2H). */
typedef struct {
    int in_pad, hidden;               /* the input's quantised width; H, a multiple of NN_CPAD */
    const int8_t *w[2], *r[2];        /* per direction, as laid out above */
    const float *b[2];                /* per direction, 8H: Wb then Rb */
    float wscale[2], rscale[2];
    int wzp[2], rzp[2];
} nn_lstm;
size_t nn_lstm_scratch(int t, int in_pad, int hidden);
void   nn_lstm_run(const nn_lstm *l, int dirs, const float *x, int t, int in, int ldx, float *y, int ldy,
                      void *scratch);

/* fmaf(a, b, c) without a library call where the target has no fused
 * multiply-add instruction (x86-64's baseline): the product of two floats is
 * exact in double, so the double sum is rounded once, and rounding that to float
 * can only differ from rounding the exact value when it lands exactly halfway
 * between two floats -- then fmaf settles it. ONNX Runtime's kernels fuse their
 * multiply-adds; this is how the forward pass does the same where it must. */
static inline float nn_fma(float a, float b, float c) {
    union { double d; uint64_t u; } s;
    s.d = (double)a * (double)b + (double)c;
    if ((s.u & 0x1FFFFFFFu) == 0x10000000u) return fmaf(a, b, c);
    return (float)s.d;
}

/* ONNX Runtime's sigmoid and tanh (its approximations, the same bits), in place. */
void nn_logistic(float *x, size_t n);
void nn_tanh(float *x, size_t n);
/* GELU in ONNX Runtime's fused tanh form (FastGelu), in place. */
void nn_gelu(float *x, size_t n);
/* LayerNormalization over each row's `cols` values, in place, in ONNX Runtime's
 * arithmetic; g, b per column (g NULL: scale 1 and bias 0). */
void nn_layernorm(float *x, int rows, int cols, int ld, const float *g, const float *b, float eps);
/* SkipLayerNormalization: x = LayerNorm(x + skip) in ONNX Runtime's arithmetic
 * (a different one from LayerNormalization's), in place; skip's rows lds apart. */
void nn_skip_layernorm(float *x, const float *skip, int rows, int cols, int ld, int lds, const float *g,
                          const float *b, float eps);
/* InstanceNormalization over time for each channel, in place, in ONNX Runtime's
 * arithmetic (its sums depend on where each channel starts in its channel-major
 * tensor, which is taken to be 16-byte aligned); g, b per channel (NULL: 1 and
 * 0). Population variance, as ONNX. */
void nn_instnorm(float *x, int t, int c, int ld, const float *g, const float *b, float eps);
/* ReduceSum of n floats as ONNX Runtime adds them (Eigen's 4-lane vectorised
 * sum), the n starting `start` floats into a 16-byte aligned tensor. */
float nn_reduce_sum(const float *a, int n, size_t start);
/* Softmax of n values, in place, in ONNX Runtime's arithmetic. */
void nn_softmax(float *x, int n);
/* y = x w (m x k by k x n; rows ldx, ldw, ldy apart) in ONNX Runtime's float
 * matrix product's order: each output a chain of fused multiply-adds over k in
 * blocks of 256. Exact rather than fast: for the small products whose every
 * bit matters. */
void nn_matmul_fma(const float *x, int m, int k, int ldx, const float *w, int n, int ldw, float *y, int ldy);
/* The same with blocks of kb (ONNX Runtime's packing chooses 128 or 256 by the
 * shapes and whether the weights are read transposed), tiled for speed. */
void nn_matmul_fma_kb(const float *x, int m, int k, size_t ldx, const float *w, int n, size_t ldw, float *y,
                         size_t ldy, int kb);
/* Multi-head attention on time-major q, k, v (head h at columns h*dh..), n
 * steps: out = softmax((q * scale) (k * scale)^T + mask) v per head, as the
 * graph has it (q and k each scaled by the root of 1/sqrt(dh)) and in ONNX
 * Runtime's arithmetic; mask (NULL: none) is added per key. scratch:
 * n * (dh + 1) + dh floats. */
void nn_attention(const float *q, const float *k, const float *v, int ld, int n, int heads, int dh,
                     float scale, const float *mask, float *out, int ldo, float *scratch);
/* Float MatMul: y[r][c] = bias[c] + sum_i x[r * xstep + i] * w[i][c], r < m, i < k,
 * c < n; w as laid out above. xstep may be less than k (overlapping windows). */
void nn_matmul_f(const float *restrict x, size_t xstep, int m, int k, const float *restrict w, int n,
                    const float *restrict bias, float *restrict y, size_t ystep);
/* ConvTranspose, 1-D, groups 1, k a multiple of s: x is k/s - 1 zero rows, the
 * t input rows, then k/s zero rows, each cin wide (only the t are read); output
 * row o (o < t_out) is position o + pad_begin of the full transposed
 * convolution, plus bias, in ONNX Runtime's arithmetic. */
void nn_convt(const float *x, int t, int cin, int k, int s, int pad_begin, const float *w, const float *bias,
                 int cout, int t_out, float *y, int ldy);
/* ConvTranspose, 1-D, depthwise (groups = channels), on rows `ld` apart: each
 * output the sum of its products in tap order, then the bias, as ONNX Runtime's
 * col2im adds them. */
void nn_convt_dw(const float *restrict x, int t, int c, int ld, int k, int s, int pad_begin,
                    const float *restrict w, const float *restrict bias, int t_out, float *restrict y, int ldy);
/* LeakyRelu, in place. */
void nn_leaky_relu(float *x, size_t n, float alpha);
/* sin(x) in ONNX Runtime's arithmetic (its Eigen kernel: within 2 ulp, exact
 * reduction past |x| = 25966), vectorisable. */
void nn_sin(const float *restrict x, float *restrict y, size_t n);
/* atan(x) of n values in ONNX Runtime's arithmetic (its vectorised rational
 * approximation; the last n mod 4 through the C library's atanf, as it does). */
void nn_atan(const float *restrict x, float *restrict y, size_t n);
/* Snake, in place: x + inv_alpha[c] * sin(alpha[c] * x)^2 per channel, sin as
 * nn_sin. */
void nn_snake(float *restrict x, int t, int c, int ld, const float *restrict alpha,
                 const float *restrict inv_alpha);
/* The generator's inverse STFT: frame t's spec row holds `bins` log-magnitudes
 * then `bins` phase inputs p; with m = exp(a) and ph = sin(p), the real part
 * m cos(ph) and imaginary part m sin(ph) are each transposed-convolved with
 * their basis (stride hop, no padding) and the imaginary result subtracted from
 * the real; `trim` samples are dropped from each end. Writes (t - 1) * hop + nfft
 * - 2 * trim samples; full: (t - 1) * hop + nfft floats of scratch. bins <= 64. */
void nn_istft(const float *spec, int t, int ld, int bins, const float *wr, const float *wi, int nfft, int hop,
                 int trim, float *out, float *full);


/* ---- for the recognizer (sttkit) --------------------------------------------
 * Its matrix products have uint8 weights with a zero point and a scale per
 * output column (ONNX Runtime's MatMulIntegerToFloat and DynamicQuantizeMatMul),
 * stored as one row per output column: b [n][k], k a multiple of NN_CPAD and n
 * of 4. */

/* y[r][c] = sum_i a[r][i] * (b[c][i] - bzp[c]) for r < m, c < n; a as
 * nn_quantize writes it (q - zp, rows k wide). Exact int32. wbuf: NN_WBUF(1, k). */
void nn_matmul_qc(const int16_t *restrict a, int m, int k, const uint8_t *restrict b, const uint8_t *restrict bzp,
                  int n, int32_t *restrict y, int ldy, int16_t *restrict wbuf);
/* y[r][c] = acc[r][c] * (ascale * bscale[c]) (+ bias[c]; NULL: none). */
void nn_dequant_qc(const int32_t *restrict acc, int rows, int cols, int lda, float ascale,
                   const float *restrict bscale, const float *restrict bias, float *restrict y, int ldy);
/* SiLU in place: x * sigmoid(x), the sigmoid as nn_logistic (ONNX Runtime's
 * QuickGelu with alpha 1). */
void nn_silu(float *x, size_t n);
/* GELU in its exact form, in place: x * 0.5 * (1 + erf(x / sqrt 2)), with erf
 * as ONNX Runtime computes it (its approximation, the same bits). */
void nn_gelu_erf(float *x, size_t n);
/* Rotary positions on adjacent pairs, in place: for i < rot / 2, with
 * f = pos * inv_freq[i], (x[2i], x[2i+1]) becomes
 * (x[2i] cos f - x[2i+1] sin f, x[2i+1] cos f + x[2i] sin f); the rest of the
 * head is left as it is. */
void nn_rope_pairs(float *x, int rot, const float *inv_freq, float pos);
/* One query of one head against keys lo..hi-1 (rows ldk apart) and their
 * values (rows ldv apart): out[0..dh) = softmax((q . k_j) * scale) v. scratch:
 * hi - lo floats. */
void nn_attend(const float *q, const float *k, int ldk, const float *v, int ldv, int lo, int hi, int dh, float scale,
               float *out, float *scratch);
/* The same in ONNX Runtime's arithmetic, to the bit: the scores of all n keys
 * (each a chain of fused multiply-adds over the head, then times scale), the
 * keys outside lo..hi-1 set to -inf (`add` 1: the graph adds a mask of 0 and
 * -inf to them; 0: it selects -inf in their place), the softmax over all n, and
 * the values weighted as its float matrix product sums them. scratch: n floats. */
void nn_attend_exact(const float *q, const float *k, int ldk, const float *v, int ldv, int n, int lo, int hi,
                     int dh, float scale, int add, float *out, float *scratch);
/* The index of the largest of n values, the first of equals. */
int  nn_argmax(const float *x, int n);
/* out[r] = sum_c x[c] * ((e[r][c] - zp) * scale) for r < n, e's rows k wide: a
 * float row against a uint8 table with one scale and zero point (the output
 * layer that is the embedding transposed). */
void nn_matvec_u8(const float *restrict x, int k, const uint8_t *restrict e, int zp, float scale, int n,
                  float *restrict out);

#endif
