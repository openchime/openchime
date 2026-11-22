/*
 * OpenChime — nnkit: the plain-C neural network kernels shared by ttskit
 * (read-aloud's voice model) and sttkit (voice input's recognizer).
 *
 * No dependencies, no intrinsics, no threads: ordinary loops the compiler's
 * auto-vectoriser turns into vector code, compiled with -ffp-contract=off so
 * every machine computes the same values (docs/details/TTSKIT.md §5b, docs/details/STTKIT.md).
 *
 * "The reference" below is the engine the voice model's and the recognizer's
 * committed references were made with, before it was withdrawn: its arithmetic
 * -- the order of its sums, its approximations -- is what these kernels compute,
 * to the bit, and the references in tests/data are what holds them to it.
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
 * zeros before and after the T rows (the graph pads ConvInteger's input with its zero
 * point, which is 0 here).
 *
 * Weight layouts, as voice.bin stores them (the converter produces these):
 *   ConvInteger   uint8 [round4(cout)][k][cin_pad]: row co, tap j, input channel
 *                 i at (co * k + j) * cin_pad + i, as the model ships it (not
 *                 offset); padding (channels past cin, rows past cout) is the
 *                 weight zero point. One zero point per tensor.
 *   MatMulInteger int8 [round4(n)][k_pad]: the graph's B [k][n] transposed, one row
 *                 per output column, padding the zero point. One zero point.
 *   LSTM          int8 W [dir][4H][in_pad] and R [dir][4H][H], the contrib op's
 *                 [dir][in][4H] and [dir][H][4H] transposed, gates i, o, f, c;
 *                 B [dir][8H] f32 as the model has it; a scale and zero point
 *                 per direction for each of W and R. voice.bin keeps each
 *                 direction's W, R and B as a tensor of its own.
 *   ConvTranspose f32 [s][k/s][cin][round8(cout)]: phase r, window row m,
 *                 input channel i, output channel o holds the graph's W[i][o][r + (k/s
 *                 - 1 - m) * s]; padding 0.
 *   depthwise ConvTranspose  f32 [k][channels]: the graph's W[c][0][j] at
 *                 j * channels + c.
 *   float MatMul  f32 [k][round8(n)], the graph's B as it is with rows padded with 0.
 *   Conv on one input channel (the forward STFT's bases)  f32 [k][round8(cout)]:
 *                 the graph's W[o][0][j] at j * round8(cout) + o, a float MatMul over
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
/* DynamicQuantizeLinear on n floats: q = clamp(round_half_even(x / scale) + zp). */
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

/* DynamicQuantizeLSTM (the reference's contrib operator), batch 1, zero
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
 * between two floats -- then fmaf settles it. The reference's kernels fuse their
 * multiply-adds; this is how the forward pass does the same where it must. */
static inline float nn_fma(float a, float b, float c) {
    union { double d; uint64_t u; } s;
    s.d = (double)a * (double)b + (double)c;
    if ((s.u & 0x1FFFFFFFu) == 0x10000000u) return fmaf(a, b, c);
    return (float)s.d;
}

/* The reference's sigmoid and tanh (its approximations, the same bits), in place. */
void nn_logistic(float *x, size_t n);
void nn_tanh(float *x, size_t n);
/* GELU in the reference's fused tanh form (FastGelu), in place. */
void nn_gelu(float *x, size_t n);
/* LayerNormalization over each row's `cols` values, in place, in the reference's
 * arithmetic; g, b per column (g NULL: scale 1 and bias 0). */
void nn_layernorm(float *x, int rows, int cols, int ld, const float *g, const float *b, float eps);
/* SkipLayerNormalization: x = LayerNorm(x + skip) in the reference's arithmetic
 * (a different one from LayerNormalization's), in place; skip's rows lds apart. */
void nn_skip_layernorm(float *x, const float *skip, int rows, int cols, int ld, int lds, const float *g,
                          const float *b, float eps);
/* InstanceNormalization over time for each channel, in place, in the reference's
 * arithmetic (its sums depend on where each channel starts in its channel-major
 * tensor, which is taken to be 16-byte aligned); g, b per channel (NULL: 1 and
 * 0). Population variance, as the graph defines it. */
void nn_instnorm(float *x, int t, int c, int ld, const float *g, const float *b, float eps);
/* ReduceSum of n floats as the reference adds them (Eigen's 4-lane vectorised
 * sum), the n starting `start` floats into a 16-byte aligned tensor. */
float nn_reduce_sum(const float *a, int n, size_t start);
/* Softmax of n values, in place, in the reference's arithmetic. */
void nn_softmax(float *x, int n);
/* y = x w (m x k by k x n; rows ldx, ldw, ldy apart) in the reference's float
 * matrix product's order: each output a chain of fused multiply-adds over k in
 * blocks of 256. Exact rather than fast: for the small products whose every
 * bit matters. */
void nn_matmul_fma(const float *x, int m, int k, int ldx, const float *w, int n, int ldw, float *y, int ldy);
/* The same with blocks of kb (the reference's packing chooses 128 or 256 by the
 * shapes and whether the weights are read transposed), tiled for speed. */
void nn_matmul_fma_kb(const float *x, int m, int k, size_t ldx, const float *w, int n, size_t ldw, float *y,
                         size_t ldy, int kb);
/* Multi-head attention on time-major q, k, v (head h at columns h*dh..), n
 * steps: out = softmax((q * scale) (k * scale)^T + mask) v per head, as the
 * graph has it (q and k each scaled by the root of 1/sqrt(dh)) and in the
 * reference's arithmetic; mask (NULL: none) is added per key. scratch:
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
 * convolution, plus bias, in the reference's arithmetic. */
void nn_convt(const float *x, int t, int cin, int k, int s, int pad_begin, const float *w, const float *bias,
                 int cout, int t_out, float *y, int ldy);
/* ConvTranspose, 1-D, depthwise (groups = channels), on rows `ld` apart: each
 * output the sum of its products in tap order, then the bias, as the reference's
 * col2im adds them. */
void nn_convt_dw(const float *restrict x, int t, int c, int ld, int k, int s, int pad_begin,
                    const float *restrict w, const float *restrict bias, int t_out, float *restrict y, int ldy);
/* LeakyRelu, in place. */
void nn_leaky_relu(float *x, size_t n, float alpha);
/* sin(x) in the reference's arithmetic (its Eigen kernel: within 2 ulp, exact
 * reduction past |x| = 25966), vectorisable. */
void nn_sin(const float *restrict x, float *restrict y, size_t n);
/* atan(x) of n values in the reference's arithmetic (its vectorised rational
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
 * output column (the reference's MatMulIntegerToFloat and DynamicQuantizeMatMul),
 * stored as one row per output column: b [n][k], k a multiple of NN_CPAD and n
 * of 4. */

/* y[r][c] = sum_i a[r][i] * (b[c][i] - bzp[c]) for r < m, c < n; a as
 * nn_quantize writes it (q - zp, rows k wide). Exact int32. wbuf: NN_WBUF(1, k). */
void nn_matmul_qc(const int16_t *restrict a, int m, int k, const uint8_t *restrict b, const uint8_t *restrict bzp,
                  int n, int32_t *restrict y, int ldy, int16_t *restrict wbuf);
/* y[r][c] = acc[r][c] * (ascale * bscale[c]) (+ bias[c]; NULL: none). */
void nn_dequant_qc(const int32_t *restrict acc, int rows, int cols, int lda, float ascale,
                   const float *restrict bscale, const float *restrict bias, float *restrict y, int ldy);
/* SiLU in place: x * sigmoid(x), the sigmoid as nn_logistic (the reference's
 * QuickGelu with alpha 1). */
void nn_silu(float *x, size_t n);
/* GELU in its exact form, in place: x * 0.5 * (1 + erf(x / sqrt 2)), with erf
 * as the reference computes it (its approximation, the same bits). */
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
/* The same in the reference's arithmetic, to the bit: the scores of all n keys
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


/* ---- for the summary model (sumkit) -----------------------------------------
 * A decoder-only language model: its matrices are stored row by row, one row
 * per output, in one of four weight types. The two quantised ones are blocks of
 * 256 values whose bytes are ggml's Q4_K and Q6_K blocks, so a
 * file's tensors can be read by either:
 *
 *   NN_W_Q4K  144 bytes: f16 d, f16 dmin, scales[12] (eight 6-bit scales and
 *             eight 6-bit mins, packed as ggml packs them), qs[128] (256 4-bit
 *             values). Sub-block s (32 values) is d * sc[s] * q - dmin * m[s];
 *             its values are the low nibbles of qs[32 (s/2) ..] when s is even,
 *             the high nibbles when odd.
 *   NN_W_Q6K  210 bytes: ql[128], qh[64], int8 scales[16], f16 d. Value e is
 *             d * scales[e / 16] * (q - 32), q 6 bits from ql and qh as ggml
 *             lays them out.
 *   NN_W_BF16 two bytes a value, bfloat16 (the unquantised model).
 *   NN_W_F32  four bytes a value.
 *
 * Activations multiplied by a quantised matrix are first quantised themselves,
 * in blocks of 256 (nn_q8k: one scale, 8-bit values, sums of each 32). Within a
 * block the products are summed exactly in int32, sub-block scales included;
 * only the block's two integers are turned to float, and the blocks are added
 * in order. So the result does not depend on how the compiler vectorises. */
#define NN_QK 256                       /* values in a quantised block */
typedef enum { NN_W_F32 = 0, NN_W_BF16 = 1, NN_W_Q4K = 2, NN_W_Q6K = 3 } nn_wtype;
#define NN_Q4K_BYTES 144
#define NN_Q6K_BYTES 210
typedef struct {
    float  d;                           /* value = d * q */
    int8_t q[NN_QK];
    int16_t bsum[NN_QK / 32];           /* sum of each 32 q */
} nn_q8k;

/* IEEE half and bfloat16 to and from float (to: round to nearest, ties to even). */
float    nn_f16_to_f32(uint16_t h);
uint16_t nn_f32_to_f16(float f);
static inline float nn_bf16_to_f32(uint16_t h) {
    union { uint32_t u; float f; } v;
    v.u = (uint32_t)h << 16;
    return v.f;
}
/* Bytes of one row of k values of type t (k a multiple of NN_QK when quantised). */
size_t nn_row_bytes(nn_wtype t, int k);
/* Row r of a matrix of type t, k values a row, to floats. */
void nn_dequant_row(nn_wtype t, const void *w, int k, int r, float *out);
/* x (k values, k a multiple of NN_QK) into k / NN_QK blocks: per block d =
 * max|x| / 127, q = x / d rounded half to even (all 0 when the block is). */
void nn_q8k_quantize(const float *restrict x, int k, nn_q8k *restrict out);
/* y[r][c] = sum_i x[r][i] * W[c][i] for r < m, c < n: W n rows of k values of
 * type t, x's rows `ldx` apart, y's `ldy`. Quantised W multiplies x quantised
 * by nn_q8k_quantize; float W multiplies x as it is, in 16 partial sums. Each
 * weight row is read once for all m rows. scratch: nn_matmul_w_scratch(t, k, m)
 * bytes. */
size_t nn_matmul_w_scratch(nn_wtype t, int k, int m);
void nn_matmul_w(nn_wtype t, const void *w, int n, int k, const float *x, int m, size_t ldx, float *y,
                 size_t ldy, void *scratch);

/* RMS norm of each of `rows` rows of n values, rows `ld` apart, in place:
 * x * g / sqrt(mean(x^2) + eps), the sum of squares in 16 partial sums. */
void nn_rmsnorm(float *x, int rows, int n, int ld, const float *g, float eps);
/* L2 norm of each row, in place: x / max(sqrt(sum x^2), eps) (ggml's l2_norm). */
void nn_l2norm(float *x, int rows, int n, int ld, float eps);
/* x *= sigmoid(g), n values (the sigmoid as nn_logistic). */
void nn_mul_sigmoid(float *restrict x, const float *restrict g, size_t n);
/* x *= silu(g), n values (the gated norm's gate). */
void nn_mul_silu(float *restrict x, const float *restrict g, size_t n);

/* The causal depthwise convolution over time of the summary model's linear
 * attention, with its carried state: the input is the k - 1 rows of `state`
 * followed by the t rows of x (c channels, rows `ldx` apart); y[i][ch] =
 * sum_j in[i + j][ch] * w[j][ch] for j < k; then state holds the input's last
 * k - 1 rows. w is [k][c]; y's rows `ldy` apart (may be x when ldy == ldx). */
void nn_conv_causal(const float *x, int t, int c, size_t ldx, float *state, int k, const float *w, float *y,
                    size_t ldy);

/* The gated delta rule over t steps of the summary model's linear attention,
 * hv value heads of dv values over hk key heads of dk values (value head h reads
 * key head h / (hv / hk)). Per step and value head, with S the head's dk x dv
 * state: S *= exp(g); u = v - S^T k; S += k (beta u)^T; o = S^T q * qscale.
 * q and k rows hold hk heads, v and o rows hv heads (rows `ldq`, `ldv`, `ldo`
 * apart, k beside q at the same stride); g and beta have hv values a step.
 * state is hv x dk x dv floats, carried from call to call. */
void nn_gated_delta(const float *q, const float *k, size_t ldq, const float *v, size_t ldv, const float *g,
                    const float *beta, int t, int hk, int hv, int dk, int dv, float qscale, float *state,
                    float *o, size_t ldo);

/* Rotary positions on the first `rot` values of a head, paired half with half
 * (value i with value i + rot / 2), in place: f = pos * inv_freq[i]. */
void nn_rope_half(float *x, int rot, const float *inv_freq, float pos);

/* The attention cache at 8 bits: each row of dh values (dh a multiple of 32) is
 * dh / 32 blocks of a float scale and 32 int8 values (d = max|x| / 127). */
#define NN_Q8_0 32
typedef struct { float d; int8_t q[NN_Q8_0]; } nn_q8_0;
void nn_q8_0_quantize(const float *restrict x, int n, nn_q8_0 *restrict out);
/* One query of dh values against keys 0..n-1 and their values, each a row of
 * dh / 32 nn_q8_0 blocks (rows `ld` blocks apart): out = softmax(scale q . k) v,
 * the softmax in float with expf. scratch: n floats. */
void nn_attend_q8(const float *q, const nn_q8_0 *k, const nn_q8_0 *v, size_t ld, int n, int dh, float scale,
                  float *out, float *scratch);

#endif
