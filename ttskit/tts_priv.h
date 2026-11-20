/* ttskit internals shared between its source files. */
#ifndef TTS_PRIV_H
#define TTS_PRIV_H

#include "ttskit.h"

#include <math.h>
#include <stdint.h>
#include <stddef.h>

#define TTS_WORD_MAX 64          /* longest word looked up or guessed, bytes */
#define TTS_IPA_MAX  256         /* longest pronunciation of one word, bytes */

/* Read-only data: a file mapping ttskit owns, or bytes the caller lent (owned 0). */
typedef struct { const uint8_t *p; size_t n; int owned; } tts_map;
int  tts_map_open(const char *path, tts_map *m);
void tts_map_close(tts_map *m);

/* The language a data file is for, as a BCP 47 tag ("en-US", "zh-Hant-HK"), NUL
 * padded. It is written into the file because the only thing that can pair a
 * lexicon with the wrong guesser is a person, and a mispaired pair does not fail
 * -- it pronounces nonsense fluently. 16 bytes takes every tag anyone writes. */
#define TTS_LANG_MAX    16

/* ---- lexicon.bin ----------------------------------------------------------
 * header (48 bytes)   magic "OCTTSLX1", u32 version, u32 count, u32 pool_size,
 *                     u32 reserved x3, char lang[16]
 * index               count x { u32 word_off, u32 ipa_off }, sorted by word bytes
 * pool                NUL-terminated strings; offsets are from the pool's start
 * All integers little-endian. */
#define TTS_LEX_MAGIC   "OCTTSLX1"
#define TTS_LEX_VERSION 2u
#define TTS_LEX_HEADER  48u

typedef struct { tts_map map; uint32_t count; const uint8_t *index; const char *pool; uint32_t pool_size;
                 char lang[TTS_LANG_MAX]; } tts_lexicon;
/* The two opens' results: 0 opened; TTS_OPEN_ABSENT the file could not be mapped
 * (not there: tolerated on the disk path); any other negative value the data is
 * there and wrong. Callers branch on the value, never on the message. */
#define TTS_OPEN_ABSENT (-2)

/* `mem`, if not NULL, supplies the bytes instead of mapping `path` (which then only
 * names them in messages). */
int         tts_lexicon_open(const char *path, const tts_map *mem, tts_lexicon *lx, char *err, size_t errcap);
void        tts_lexicon_close(tts_lexicon *lx);
const char *tts_lexicon_find(const tts_lexicon *lx, const char *word);

/* ---- guesses.bin -----------------------------------------------------------
 * header (80 bytes)   magic "OCTTSGS1", u32 version, u32 order, u32 n_tokens,
 *                     u32 pool_size, u32 count[8] (entries per order, 1-based
 *                     orders in slots 0..7), u32 reserved x2, char lang[16]
 * tokens              n_tokens x { u32 graph_off, u32 phon_off } — graphemes and
 *                     phonemes as written in the ARPA file with '|' removed and
 *                     "_" as the empty phoneme; token 0 is <s>, 1 is </s>
 * pool                NUL-terminated strings
 * per order k=1..N    count[k] x { u16 id[k], f32 logprob, f32 backoff }, sorted
 *                     by the id tuple; logprob and backoff are log10 */
#define TTS_GS_MAGIC    "OCTTSGS1"
#define TTS_GS_VERSION  2u
#define TTS_GS_HEADER   80u
#define TTS_GS_MAXORDER 8

typedef struct {
    tts_map         map;
    uint32_t        order, n_tokens;
    const uint8_t  *tokens;
    const char     *pool;
    const uint8_t  *table[TTS_GS_MAXORDER + 1];   /* 1-based */
    uint32_t        count[TTS_GS_MAXORDER + 1];
    /* Token ids by their first grapheme byte, built at open (a few KB): the
     * decoder asks "which tokens can start here" once per position. */
    uint16_t       *by_first;
    uint32_t        by_first_start[257];
    char            lang[TTS_LANG_MAX];
} tts_guesser;
int  tts_guesser_open(const char *path, const tts_map *mem, tts_guesser *g, char *err, size_t errcap);
void tts_guesser_close(tts_guesser *g);
/* The best pronunciation for a lower-case word, as joined IPA. 0 on failure. */
int  tts_guess(const tts_guesser *g, const char *word, char *ipa, size_t cap);

/* Letter names for spelling ("API" -> "eɪ pˈiː aɪ"). */
const char *tts_letter_ipa(char c);
/* Unstressed forms of function words, or NULL. */
const char *tts_weak_form(const char *word);

void tts_seterr(char *err, size_t cap, const char *fmt, ...);

static inline uint32_t tts_rd32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* ---- voice.bin (voice.c; the format is in ttskit.h) ------------------------- */
struct voice { tts_map map; uint32_t count; const uint8_t *index; };
/* As tts_lexicon_open: `mem`, if not NULL, supplies the bytes instead of mapping
 * `path` (which then only names them in messages). 0, TTS_OPEN_ABSENT, or -1
 * with the reason the data is refused. */
int  voice_map_open(const char *path, const tts_map *mem, voice *v, char *err, size_t errcap);
void voice_map_close(voice *v);

/* ---- the voice model's kernels (voice_ops.c) -------------------------------
 * Plain C, one thread, written for the compiler's auto-vectoriser at -O2: the
 * hot loops have a fixed trip count (16 or 64), integer products accumulate in
 * int32, and float work is in an order fixed by the source (-ffp-contract=off),
 * so every machine computes the same samples.
 *
 * Activations are time-major: a sequence of T steps of C channels is T rows,
 * row t holding its C channels contiguously, `ld` values apart.
 *
 * Quantised activations are int16 rows holding q - zero_point (in -255..255),
 * `cin` values per row with `cin` a multiple of VOICE_CPAD; the columns past the
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
#define VOICE_CPAD  16                     /* quantised rows: channels padded to this */
#define VOICE_ROUND(n, m) (((n) + (m) - 1) / (m) * (m))
/* int16 scratch for voice_conv_q / voice_matmul_q: four widened weight rows */
#define VOICE_WBUF(k, cin_pad) ((size_t)4 * (size_t)(k) * (size_t)(cin_pad))

/* DynamicQuantizeLinear's parameters: scale = (max - min) / 255 over the range
 * widened to include 0, zero point = round_half_even(clamp(-min / scale, 0, 255)).
 * An all-zero tensor gets scale 0 and zero point 0 (every product is then 0). */
typedef struct { float scale; int zp; } voice_qparam;
void voice_dql_params(float min, float max, voice_qparam *qp);
/* ONNX DynamicQuantizeLinear on n floats: q = clamp(round_half_even(x / scale) + zp). */
void voice_dql_u8(const float *restrict x, size_t n, uint8_t *restrict q, voice_qparam *qp);
/* The same over a rows x cols tensor (rows `ldx` apart), written as q - zp into
 * int16 rows `ldq` apart, columns cols..ldq-1 set to 0. */
void voice_quantize(const float *restrict x, int rows, int cols, int ldx, int16_t *restrict q, int ldq,
                    voice_qparam *qp);
/* ConvInteger, 1-D: y[t][co] = sum_j sum_i x[t * stride + j * dil][i] * (w[co][j][i] - wzp)
 * for t < t_out, co < cout, x's rows `cin` wide (including the leading padding
 * rows: x points at the first one). Exact int32. k * cin <= 32768. */
void voice_conv_q(const int16_t *restrict x, int t_out, int stride, int dil, int k, int cin,
                  const uint8_t *restrict w, int wzp, int cout, int32_t *restrict y, int ldy,
                  int16_t *restrict wbuf);
/* MatMulInteger: y[r][c] = sum_i a[r][i] * (b[c][i] - bzp) for r < m, c < n; a's rows
 * k wide (k a multiple of VOICE_CPAD). Exact int32. */
void voice_matmul_q(const int16_t *restrict a, int m, int k, const int8_t *restrict b, int bzp, int n,
                    int32_t *restrict y, int ldy, int16_t *restrict wbuf);
/* The graph's Cast, Mul by the product of the two scales, Add bias (NULL: none). */
void voice_dequant(const int32_t *restrict acc, int rows, int cols, int lda, float scale,
                   const float *restrict bias, float *restrict y, int ldy);

/* com.microsoft DynamicQuantizeLSTM (ONNX Runtime's contrib op), batch 1, zero
 * initial state, no peepholes, default activations: per direction the whole input
 * is quantised once (one scale) and multiplied by W; each step quantises h(t-1)
 * on its own and multiplies it by R; gates = xW + hR + (Wb + Rb), in the order
 * i, o, f, c; c = f * c + i * tanh(g_c); h = o * tanh(c), sigmoid on i, o, f. The
 * forward direction writes y[t][0..H), the reverse y[t][H..2H). */
typedef struct {
    int in_pad, hidden;               /* the input's quantised width; H, a multiple of VOICE_CPAD */
    const int8_t *w[2], *r[2];        /* per direction, as laid out above */
    const float *b[2];                /* per direction, 8H: Wb then Rb */
    float wscale[2], rscale[2];
    int wzp[2], rzp[2];
} voice_lstm;
size_t voice_lstm_scratch(int t, int in_pad, int hidden);
void   voice_lstm_run(const voice_lstm *l, int dirs, const float *x, int t, int in, int ldx, float *y, int ldy,
                      void *scratch);

/* fmaf(a, b, c) without a library call where the target has no fused
 * multiply-add instruction (x86-64's baseline): the product of two floats is
 * exact in double, so the double sum is rounded once, and rounding that to float
 * can only differ from rounding the exact value when it lands exactly halfway
 * between two floats -- then fmaf settles it. ONNX Runtime's kernels fuse their
 * multiply-adds; this is how the forward pass does the same where it must. */
static inline float voice_fma(float a, float b, float c) {
    union { double d; uint64_t u; } s;
    s.d = (double)a * (double)b + (double)c;
    if ((s.u & 0x1FFFFFFFu) == 0x10000000u) return fmaf(a, b, c);
    return (float)s.d;
}

/* ONNX Runtime's sigmoid and tanh (its approximations, the same bits), in place. */
void voice_logistic(float *x, size_t n);
void voice_tanh(float *x, size_t n);
/* GELU in ONNX Runtime's fused tanh form (FastGelu), in place. */
void voice_gelu(float *x, size_t n);
/* LayerNormalization over each row's `cols` values, in place, in ONNX Runtime's
 * arithmetic; g, b per column (g NULL: scale 1 and bias 0). */
void voice_layernorm(float *x, int rows, int cols, int ld, const float *g, const float *b, float eps);
/* SkipLayerNormalization: x = LayerNorm(x + skip) in ONNX Runtime's arithmetic
 * (a different one from LayerNormalization's), in place; skip's rows lds apart. */
void voice_skip_layernorm(float *x, const float *skip, int rows, int cols, int ld, int lds, const float *g,
                          const float *b, float eps);
/* InstanceNormalization over time for each channel, in place, in ONNX Runtime's
 * arithmetic (its sums depend on where each channel starts in its channel-major
 * tensor, which is taken to be 16-byte aligned); g, b per channel (NULL: 1 and
 * 0). Population variance, as ONNX. */
void voice_instnorm(float *x, int t, int c, int ld, const float *g, const float *b, float eps);
/* ReduceSum of n floats as ONNX Runtime adds them (Eigen's 4-lane vectorised
 * sum), the n starting `start` floats into a 16-byte aligned tensor. */
float voice_reduce_sum(const float *a, int n, size_t start);
/* Softmax of n values, in place, in ONNX Runtime's arithmetic. */
void voice_softmax(float *x, int n);
/* y = x w (m x k by k x n; rows ldx, ldw, ldy apart) in ONNX Runtime's float
 * matrix product's order: each output a chain of fused multiply-adds over k in
 * blocks of 256. Exact rather than fast: for the small products whose every
 * bit matters. */
void voice_matmul_fma(const float *x, int m, int k, int ldx, const float *w, int n, int ldw, float *y, int ldy);
/* The same with blocks of kb (ONNX Runtime's packing chooses 128 or 256 by the
 * shapes and whether the weights are read transposed), tiled for speed. */
void voice_matmul_fma_kb(const float *x, int m, int k, size_t ldx, const float *w, int n, size_t ldw, float *y,
                         size_t ldy, int kb);
/* Multi-head attention on time-major q, k, v (head h at columns h*dh..), n
 * steps: out = softmax((q * scale) (k * scale)^T + mask) v per head, as the
 * graph has it (q and k each scaled by the root of 1/sqrt(dh)) and in ONNX
 * Runtime's arithmetic; mask (NULL: none) is added per key. scratch:
 * n * (dh + 1) + dh floats. */
void voice_attention(const float *q, const float *k, const float *v, int ld, int n, int heads, int dh,
                     float scale, const float *mask, float *out, int ldo, float *scratch);
/* Float MatMul: y[r][c] = bias[c] + sum_i x[r * xstep + i] * w[i][c], r < m, i < k,
 * c < n; w as laid out above. xstep may be less than k (overlapping windows). */
void voice_matmul_f(const float *restrict x, size_t xstep, int m, int k, const float *restrict w, int n,
                    const float *restrict bias, float *restrict y, size_t ystep);
/* ConvTranspose, 1-D, groups 1, k a multiple of s: x is k/s - 1 zero rows, the
 * t input rows, then k/s zero rows, each cin wide (only the t are read); output
 * row o (o < t_out) is position o + pad_begin of the full transposed
 * convolution, plus bias, in ONNX Runtime's arithmetic. */
void voice_convt(const float *x, int t, int cin, int k, int s, int pad_begin, const float *w, const float *bias,
                 int cout, int t_out, float *y, int ldy);
/* ConvTranspose, 1-D, depthwise (groups = channels), on rows `ld` apart: each
 * output the sum of its products in tap order, then the bias, as ONNX Runtime's
 * col2im adds them. */
void voice_convt_dw(const float *restrict x, int t, int c, int ld, int k, int s, int pad_begin,
                    const float *restrict w, const float *restrict bias, int t_out, float *restrict y, int ldy);
/* LeakyRelu, in place. */
void voice_leaky_relu(float *x, size_t n, float alpha);
/* sin(x) in ONNX Runtime's arithmetic (its Eigen kernel: within 2 ulp, exact
 * reduction past |x| = 25966), vectorisable. */
void voice_sin(const float *restrict x, float *restrict y, size_t n);
/* atan(x) of n values in ONNX Runtime's arithmetic (its vectorised rational
 * approximation; the last n mod 4 through the C library's atanf, as it does). */
void voice_atan(const float *restrict x, float *restrict y, size_t n);
/* Snake, in place: x + inv_alpha[c] * sin(alpha[c] * x)^2 per channel, sin as
 * voice_sin. */
void voice_snake(float *restrict x, int t, int c, int ld, const float *restrict alpha,
                 const float *restrict inv_alpha);
/* The generator's inverse STFT: frame t's spec row holds `bins` log-magnitudes
 * then `bins` phase inputs p; with m = exp(a) and ph = sin(p), the real part
 * m cos(ph) and imaginary part m sin(ph) are each transposed-convolved with
 * their basis (stride hop, no padding) and the imaginary result subtracted from
 * the real; `trim` samples are dropped from each end. Writes (t - 1) * hop + nfft
 * - 2 * trim samples; full: (t - 1) * hop + nfft floats of scratch. bins <= 64. */
void voice_istft(const float *spec, int t, int ld, int bins, const float *wr, const float *wi, int nfft, int hop,
                 int trim, float *out, float *full);


/* ---- the voice model's forward pass, its first half (voice_model.c) --------------
 * From token ids (0..177), a voice's style row (256 floats: the first 128 the
 * decoder's, the second 128 the prosody predictor's) and the speed, what the
 * decoder reads: the text encoder's states expanded to one row per frame, and
 * the pitch (F0) and energy (N) curves at two values per frame. A frame is 600
 * samples of the 24 kHz output. Every array is the struct's own (malloc'd);
 * time-major, rows of 512. */
#define VOICE_MAX_FRAMES (1 << 20)
typedef struct {
    int    tokens, frames;   /* T; F = the sum of dur */
    int   *dur;              /* T: frames per token, each at least 1 */
    float *dur_raw;          /* T: the sum of the duration bins over the speed, before rounding */
    float *ctx;              /* T x 512: the context encoder's output */
    float *txt;              /* T x 512: the text encoder's output */
    float *asr;              /* F x 512: txt expanded, row f the token frame f belongs to */
    float *f0, *n;           /* 2F each: the pitch and energy curves */
} voice_front;
/* 0, or -1 with the reason in err (a token out of range, a tensor missing from
 * the data, too many frames, no memory); f is then empty. */
int  voice_front_run(const voice *v, const int *ids, int tokens, const float *style, float speed, voice_front *f,
                     char *err, size_t errcap);
void voice_front_free(voice_front *f);
/* The duration rule: round half to even, then at least 1. */
int  voice_round_duration(float raw);
/* The alignment: frame_token[f] = the token frame f belongs to, frames in token
 * order, dur[t] of them for token t. Returns the frame count, or -1 if it would
 * pass cap. */
int  voice_align(const int *dur, int tokens, int *frame_token, int cap);
/* y's row f = x's row frame_token[f] (c values; rows ldx and ldy apart): the
 * graph's product with the 0/1 alignment matrix, which has one 1 per frame. */
void voice_expand(const float *x, int ldx, int c, const int *frame_token, int frames, float *y, int ldy);

/* ---- the voice model's forward pass, its second half (voice_model.c) -------------
 * From the front half to the raw waveform, in three parts that voice_say runs in
 * turn and the tests and tools can run alone. Each reads the style row's first
 * 128 values; each returns 0, or -1 with the reason in err. F is the frame count. */
#define VOICE_FRAME 600          /* output samples per frame */
/* The decoder: the expanded text states, F0 and N through its five residual
 * blocks to out, 2F rows of 512. */
int  voice_decode(const voice *v, const voice_front *f, const float *style, float *out, char *err, size_t errcap);

/* The noise generator: SplitMix64 (Steele, Lea and Flood's mixer of a Weyl
 * sequence), one 64-bit state, seeded with the seed as it is; uniform floats
 * from its top 24 bits; normal ones by Box-Muller, in pairs, from two of its
 * 53-bit uniforms in double. Repeatable for a seed; the normals go through the C
 * library's log, sin and cos, so their last bits may differ between libraries. */
typedef struct { uint64_t state; int have; float spare; } voice_rng;
uint64_t voice_rng_next(voice_rng *r);
float    voice_rng_uniform(voice_rng *r);       /* [0, 1) */
float    voice_rng_normal(voice_rng *r);        /* mean 0, variance 1 */

/* The harmonic source: from the pitch curve (2F values), the sum of nine
 * harmonics of the pitch (where it is above 10 Hz, silence elsewhere) through a
 * linear layer and tanh, sampled at 24 kHz, and its STFT (n_fft 20, hop 5):
 * har, 120F + 1 rows of 11 magnitudes then 11 phases. The phases' arithmetic
 * is ONNX Runtime's to the bit, since its running sum carries any difference
 * into every later sample. With noise on (the model's own behaviour), the
 * random operators it replaces draw from voice_rng seeded with `seed`: each
 * sample and harmonic gets normal noise of standard deviation 0.003 where
 * voiced and 0.1 / 3 where not, and harmonics 2..9 a uniform initial phase on
 * the first sample (which the graph's resampling never reads); off, both are 0.
 * noise_out, if not NULL, receives the normal noise as added (600F rows of 9). */
int  voice_source(const voice *v, const float *f0, int frames, int noise, uint64_t seed, float *har, float *noise_out,
                  char *err, size_t errcap);
/* The generator: the decoder's output (2F rows of 512) and the source's
 * spectrum to 600F samples of the raw waveform (no high pass, no trim). */
int  voice_generate(const voice *v, const float *dec, const float *har, int frames, const float *style, float *pcm,
                    char *err, size_t errcap);

#endif
