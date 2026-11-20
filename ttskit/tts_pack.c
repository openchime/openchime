/* tts_pack — building ttskit's data files (TTSKIT.md §8).
 *
 *   tts_pack ipa   CMUDICT.dict  lexicon.ipa  train.lex
 *       CMUdict (ARPAbet) to espeak-style IPA: lexicon.ipa is "word<TAB>ipa"
 *       (first pronunciation of each word); train.lex is the same with phonemes
 *       separated by spaces, the form the guesser trains on.
 *   tts_pack lexicon  lexicon.ipa  lexicon.bin
 *   tts_pack guesser  model.arpa   guesses.bin
 *   tts_pack word  DIR  WORD...     pronounce words with packed data (checking)
 *   tts_pack text  DIR  TEXT        pronounce a sentence
 *   tts_pack say DIR VOICE TEXT out.wav
 *                                   speak TEXT in VOICE ("expr-voice-2-m" ...) as
 *                                   the daemon does -- tts_text, the daemon's token
 *                                   function, the style row for TEXT's length, its
 *                                   speed, the model's noise on -- with DIR's text
 *                                   data and voice.bin; the raw waveform (no high
 *                                   pass, trim or fade), 16-bit 24 kHz mono
 *   tts_pack parity DIR FIXTURES    criterion 4 over FIXTURES/manifest
 *                                   (tests/data/voice) with DIR's voice.bin: each
 *                                   fixture rendered with the noise off has the
 *                                   reference's sample count and at least 40 dB SNR
 *                                   against its noise-off reference, and with the
 *                                   noise on the noise source's variance is within
 *                                   10% of the model's; a table, non-zero exit on
 *                                   any failure
 *   tts_pack bench DIR FIXTURES     each fixture rendered in full, one thread, as
 *                                   the daemon renders (noise on): the real-time
 *                                   factor per fixture and overall, peak RSS
 *   tts_pack bench FIXTURES         the voice model's kernels timed on its layer
 *                                   shapes for each fixture, a projected real-time
 *                                   factor and where the time goes
 */
#define _POSIX_C_SOURCE 200809L
#include "ttskit.h"
#include "tts_priv.h"
#include "tts_voice.h"

#include <ctype.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

static int usage(void) {
    fprintf(stderr,
        "usage: tts_pack ipa CMUDICT lexicon.ipa train.lex\n"
        "       tts_pack lexicon lexicon.ipa lexicon.bin LANG\n"
        "       tts_pack guesser model.arpa guesses.bin LANG\n"
        "       tts_pack word DIR WORD...\n"
        "       tts_pack text DIR TEXT\n"
        "       tts_pack say DIR VOICE TEXT out.wav\n"
        "       tts_pack parity DIR FIXTURES\n"
        "       tts_pack bench DIR FIXTURES\n"
        "       tts_pack bench FIXTURES\n");
    return 2;
}

static int do_ipa(const char *dict, const char *lex_path, const char *train_path) {
    FILE *in = fopen(dict, "r"), *lex = fopen(lex_path, "w"), *train = fopen(train_path, "w");
    if (!in || !lex || !train) { fprintf(stderr, "tts_pack: cannot open files\n"); return 1; }
    char line[1024], prev[1024] = "";
    long words = 0, skipped = 0;
    while (fgets(line, sizeof line, in)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = '\0';
        char *sp = strchr(line, ' ');
        if (!sp) continue;
        *sp = '\0';
        char *word = line, *arpa = sp + 1;
        char *paren = strchr(word, '(');
        if (paren) *paren = '\0';                 /* word(2): an alternative pronunciation */
        if (!strcmp(word, prev)) continue;        /* the first pronunciation wins */
        snprintf(prev, sizeof prev, "%s", word);
        char joined[256], units[512];
        if (!tts_arpa_to_ipa(arpa, 0, joined, sizeof joined) || !tts_arpa_to_ipa(arpa, 1, units, sizeof units)) { skipped++; continue; }
        fprintf(lex, "%s\t%s\n", word, joined);
        fprintf(train, "%s\t%s\n", word, units);
        words++;
    }
    fclose(in);
    int bad = ferror(lex) || ferror(train);
    if (fclose(lex) || fclose(train) || bad) { fprintf(stderr, "tts_pack: write failed\n"); return 1; }
    fprintf(stderr, "tts_pack: %ld words, %ld skipped\n", words, skipped);
    return 0;
}

/* ---- bench: the voice kernels on the model's layer shapes ---------------------
 * Each fixture in FIXTURES/manifest gives a token count N and, from its sample
 * count, the number of 40-per-second frames F (600 samples each). Every layer of
 * the model is listed below with the rows it runs over -- N tokens, F frames, or
 * the generator's 2F, 20F and 120F + 1 -- and how often; each is timed on random
 * weights and inputs of its shape and type, every repetition with weights of its
 * own (a render reads each layer's weights once). Integer layers are timed with
 * their input's quantisation and their output's dequantisation. The sum is a
 * projection of a render's time, single-threaded. Not timed: the generator's
 * harmonic source (upsampling the pitch, its phase and sines, the forward STFT
 * and its phase), the residual additions and the style scale and shift
 * between layers, nearest-neighbour upsampling, the alignment of tokens to
 * frames, the GELU in the text encoder, and copies between layers. */

typedef enum { B_CONV, B_MATMUL, B_MATMUL_F, B_LSTM, B_ATTN, B_LN, B_IN, B_SNAKE, B_LEAKY, B_CONVT, B_CONVT_DW,
               B_ISTFT } bench_kind;
typedef enum { R_ONE, R_TOK, R_F, R_2F, R_20F, R_120F } bench_rate;
static const char *const KIND_NAME[] = { "int8 conv", "int8 matmul", "float matmul", "LSTM", "attention",
                                         "layer norm", "instance norm", "snake", "leaky relu",
                                         "transposed conv", "depthwise transp.", "inverse STFT" };

typedef struct {
    const char *name;
    bench_kind kind;
    int cin, cout, k, stride, dil;      /* channels, taps, stride, dilation (a transposed conv's: its
                                           leading pad); one-input kernels: cin is the width */
    bench_rate in, out;                 /* rows read and written */
    int count;
} bench_layer;

static const bench_layer LAYERS[] = {
    { "text: embedding norm",               B_LN,       128,    0,  0, 0, 0, R_TOK, R_TOK, 1 },
    { "text: embedding projection",         B_MATMUL,   128,  768,  0, 0, 0, R_TOK, R_TOK, 1 },
    { "text: attention projections",        B_MATMUL,   768,  768,  0, 0, 0, R_TOK, R_TOK, 48 },
    { "text: attention",                    B_ATTN,     768,    0,  0, 0, 0, R_TOK, R_TOK, 12 },
    { "text: transformer norms",            B_LN,       768,    0,  0, 0, 0, R_TOK, R_TOK, 24 },
    { "text: feed-forward in",              B_MATMUL,   768, 2048,  0, 0, 0, R_TOK, R_TOK, 12 },
    { "text: feed-forward out",             B_MATMUL,  2048,  768,  0, 0, 0, R_TOK, R_TOK, 12 },
    { "text: to encoder width",             B_MATMUL,   768,  512,  0, 0, 0, R_TOK, R_TOK, 1 },
    { "text: encoder convs",                B_CONV,     512,  512,  5, 1, 1, R_TOK, R_TOK, 3 },
    { "text: encoder norms",                B_LN,       512,    0,  0, 0, 0, R_TOK, R_TOK, 3 },
    { "text: encoder leaky relu",           B_LEAKY,    512,    0,  0, 0, 0, R_TOK, R_TOK, 3 },
    { "text: encoder LSTM",                 B_LSTM,     512,  256,  0, 0, 0, R_TOK, R_TOK, 1 },
    { "text: predictor LSTMs",              B_LSTM,     640,  256,  0, 0, 0, R_TOK, R_TOK, 4 },
    { "text: predictor norms",              B_LN,       512,    0,  0, 0, 0, R_TOK, R_TOK, 3 },
    { "text: duration projection",          B_MATMUL_F, 512,   50,  0, 0, 0, R_TOK, R_TOK, 1 },
    { "style projections",                  B_MATMUL,   128, 2048,  0, 0, 0, R_ONE, R_ONE, 60 },
    { "prosody: LSTM",                      B_LSTM,     640,  256,  0, 0, 0, R_F,   R_F,   1 },
    { "prosody: 512 convs",                 B_CONV,     512,  512,  3, 1, 1, R_F,   R_F,   4 },
    { "prosody: 512 norms",                 B_IN,       512,    0,  0, 0, 0, R_F,   R_F,   6 },
    { "prosody: 512 leaky relu",            B_LEAKY,    512,    0,  0, 0, 0, R_F,   R_F,   6 },
    { "prosody: upsample",                  B_CONVT_DW, 512,    0,  3, 2, 0, R_F,   R_2F,  2 },
    { "prosody: 512->256 convs",            B_CONV,     512,  256,  3, 1, 1, R_2F,  R_2F,  2 },
    { "prosody: shortcuts",                 B_CONV,     512,  256,  1, 1, 1, R_2F,  R_2F,  2 },
    { "prosody: 256 convs",                 B_CONV,     256,  256,  3, 1, 1, R_2F,  R_2F,  6 },
    { "prosody: 256 norms",                 B_IN,       256,    0,  0, 0, 0, R_2F,  R_2F,  6 },
    { "prosody: 256 leaky relu",            B_LEAKY,    256,    0,  0, 0, 0, R_2F,  R_2F,  6 },
    { "prosody: projections",               B_CONV,     256,    1,  1, 1, 1, R_2F,  R_2F,  2 },
    { "decoder: asr projection",            B_CONV,     512,   64,  1, 1, 1, R_F,   R_F,   1 },
    { "decoder: 514->1024 conv",            B_CONV,     514, 1024,  3, 1, 1, R_F,   R_F,   1 },
    { "decoder: 514->1024 shortcut",        B_CONV,     514, 1024,  1, 1, 1, R_F,   R_F,   1 },
    { "decoder: 1090->1024 convs",          B_CONV,    1090, 1024,  3, 1, 1, R_F,   R_F,   3 },
    { "decoder: 1090->1024 shortcuts",      B_CONV,    1090, 1024,  1, 1, 1, R_F,   R_F,   3 },
    { "decoder: 1024 convs",                B_CONV,    1024, 1024,  3, 1, 1, R_F,   R_F,   4 },
    { "decoder: 514 norm",                  B_IN,       514,    0,  0, 0, 0, R_F,   R_F,   1 },
    { "decoder: 1024 norms",                B_IN,      1024,    0,  0, 0, 0, R_F,   R_F,   4 },
    { "decoder: 1090 norms",                B_IN,      1090,    0,  0, 0, 0, R_F,   R_F,   4 },
    { "decoder: leaky relu",                B_LEAKY,   1090,    0,  0, 0, 0, R_F,   R_F,   9 },
    { "decoder: upsample",                  B_CONVT_DW,1090,    0,  3, 2, 0, R_F,   R_2F,  1 },
    { "decoder: 1090->512 conv",            B_CONV,    1090,  512,  3, 1, 1, R_2F,  R_2F,  1 },
    { "decoder: 1090->512 shortcut",        B_CONV,    1090,  512,  1, 1, 1, R_2F,  R_2F,  1 },
    { "decoder: 512 conv",                  B_CONV,     512,  512,  3, 1, 1, R_2F,  R_2F,  1 },
    { "decoder: 512 norm",      B_IN,       512,    0,  0, 0, 0, R_2F,  R_2F,  1 },
    { "generator: upsample x10",            B_CONVT,    512,  256, 20, 10, 5, R_2F, R_20F, 1 },
    { "generator: source conv x1/6",        B_CONV,      22,  256, 12, 6, 1, R_120F, R_20F, 1 },
    { "generator: 256 k7 convs",            B_CONV,     256,  256,  7, 1, 1, R_20F, R_20F, 4 },
    { "generator: 256 k7 dilated 3",        B_CONV,     256,  256,  7, 1, 3, R_20F, R_20F, 1 },
    { "generator: 256 k7 dilated 5",        B_CONV,     256,  256,  7, 1, 5, R_20F, R_20F, 1 },
    { "generator: 256 k3 convs",            B_CONV,     256,  256,  3, 1, 1, R_20F, R_20F, 8 },
    { "generator: 256 k3 dilated 3",        B_CONV,     256,  256,  3, 1, 3, R_20F, R_20F, 2 },
    { "generator: 256 k3 dilated 5",        B_CONV,     256,  256,  3, 1, 5, R_20F, R_20F, 2 },
    { "generator: 256 norms",               B_IN,       256,    0,  0, 0, 0, R_20F, R_20F, 18 },
    { "generator: 256 snakes",              B_SNAKE,    256,    0,  0, 0, 0, R_20F, R_20F, 18 },
    { "generator: upsample x6",             B_CONVT,    256,  128, 12, 6, 3, R_20F, R_120F, 1 },
    { "generator: source conv x1",          B_CONV,      22,  128,  1, 1, 1, R_120F, R_120F, 1 },
    { "generator: 128 k11 convs",           B_CONV,     128,  128, 11, 1, 1, R_120F, R_120F, 4 },
    { "generator: 128 k11 dilated 3",       B_CONV,     128,  128, 11, 1, 3, R_120F, R_120F, 1 },
    { "generator: 128 k11 dilated 5",       B_CONV,     128,  128, 11, 1, 5, R_120F, R_120F, 1 },
    { "generator: 128 k3 convs",            B_CONV,     128,  128,  3, 1, 1, R_120F, R_120F, 8 },
    { "generator: 128 k3 dilated 3",        B_CONV,     128,  128,  3, 1, 3, R_120F, R_120F, 2 },
    { "generator: 128 k3 dilated 5",        B_CONV,     128,  128,  3, 1, 5, R_120F, R_120F, 2 },
    { "generator: 128 norms",               B_IN,       128,    0,  0, 0, 0, R_120F, R_120F, 18 },
    { "generator: 128 snakes",              B_SNAKE,    128,    0,  0, 0, 0, R_120F, R_120F, 18 },
    { "generator: leaky relu",              B_LEAKY,    512,    0,  0, 0, 0, R_2F,  R_2F,  1 },
    { "generator: leaky relu 256",          B_LEAKY,    256,    0,  0, 0, 0, R_20F, R_20F, 1 },
    { "generator: leaky relu 128",          B_LEAKY,    128,    0,  0, 0, 0, R_120F, R_120F, 1 },
    { "generator: output conv",             B_CONV,     128,   22,  7, 1, 1, R_120F, R_120F, 1 },
    { "generator: inverse STFT",            B_ISTFT,     22,    0, 20, 5, 0, R_120F, R_120F, 1 },
};
#define NLAYERS (sizeof LAYERS / sizeof LAYERS[0])

static int bench_rows(bench_rate r, int n, int f) {
    switch (r) {
    case R_ONE:  return 1;
    case R_TOK:  return n;
    case R_F:    return f;
    case R_2F:   return 2 * f;
    case R_20F:  return 20 * f;
    case R_120F: return 120 * f + 1;
    }
    return 0;
}

static uint32_t g_seed = 0x2545f491u;
static uint32_t brand(void) {
    g_seed ^= g_seed << 13;
    g_seed ^= g_seed >> 17;
    g_seed ^= g_seed << 5;
    return g_seed;
}
static void fill_f(float *p, size_t n, float amp) {
    for (size_t i = 0; i < n; i++) p[i] = amp * ((float)(brand() >> 8) / 8388608.0f - 1.0f);
}
static void fill_b(void *p, size_t n) {
    uint8_t *b = p;
    for (size_t i = 0; i < n; i++) b[i] = (uint8_t)brand();
}
static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) { fprintf(stderr, "tts_pack: out of memory\n"); exit(1); }
    return p;
}
static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* One layer, `count` times over rows for a fixture of n tokens and f frames: seconds. */
static double bench_layer_run(const bench_layer *L, int n, int f) {
    int tin = bench_rows(L->in, n, f), tout = bench_rows(L->out, n, f), reps = L->count;
    int cp = NN_ROUND(L->cin, NN_CPAD);
    double spent = 0;
    switch (L->kind) {
    case B_CONV: case B_MATMUL: {
        int k = L->kind == B_CONV ? L->k : 1, stride = L->kind == B_CONV ? L->stride : 1;
        int dil = L->kind == B_CONV ? L->dil : 1, pad = (k - 1) * dil / 2;
        int rows = (tout - 1) * stride + (k - 1) * dil + 1, real = rows - 2 * pad;
        size_t wn = (size_t)NN_ROUND(L->cout, 4) * k * cp;
        float *x = xmalloc(sizeof(float) * (size_t)real * L->cin), *y = xmalloc(sizeof(float) * (size_t)tout * L->cout);
        float *bias = xmalloc(sizeof(float) * (size_t)L->cout);
        int16_t *q = calloc((size_t)rows * cp, sizeof *q), *wbuf = xmalloc(sizeof(int16_t) * NN_WBUF(k, cp));
        int32_t *acc = xmalloc(sizeof(int32_t) * (size_t)tout * L->cout);
        uint8_t *w = xmalloc(wn * (size_t)reps);
        if (!q) { fprintf(stderr, "tts_pack: out of memory\n"); exit(1); }
        fill_f(x, (size_t)real * L->cin, 2.0f);
        fill_f(bias, (size_t)L->cout, 0.1f);
        fill_b(w, wn * (size_t)reps);
        double t0 = now_s();
        for (int r = 0; r < reps; r++) {
            nn_qparam qp;
            nn_quantize(x, real, L->cin, L->cin, q + (size_t)pad * cp, cp, &qp);
            if (L->kind == B_CONV)
                nn_conv_q(q, tout, stride, dil, k, cp, w + wn * r, 131, L->cout, acc, L->cout, wbuf);
            else
                nn_matmul_q(q, tout, cp, (const int8_t *)(w + wn * r), 0, L->cout, acc, L->cout, wbuf);
            nn_dequant(acc, tout, L->cout, L->cout, qp.scale * 0.01f, bias, y, L->cout);
        }
        spent = now_s() - t0;
        free(x); free(y); free(bias); free(q); free(wbuf); free(acc); free(w);
        break;
    }
    case B_MATMUL_F: {
        int np = NN_ROUND(L->cout, 8);
        float *x = xmalloc(sizeof(float) * (size_t)tin * L->cin), *w = xmalloc(sizeof(float) * (size_t)L->cin * np);
        float *y = xmalloc(sizeof(float) * (size_t)tout * L->cout);
        fill_f(x, (size_t)tin * L->cin, 1.0f);
        fill_f(w, (size_t)L->cin * np, 0.1f);
        double t0 = now_s();
        for (int r = 0; r < reps; r++) nn_matmul_f(x, (size_t)L->cin, tin, L->cin, w, L->cout, NULL, y, (size_t)L->cout);
        spent = now_s() - t0;
        free(x); free(w); free(y);
        break;
    }
    case B_LSTM: {
        int H = L->cout, G = 4 * H;
        float *x = xmalloc(sizeof(float) * (size_t)tin * L->cin), *y = xmalloc(sizeof(float) * (size_t)tin * 2 * H);
        int8_t *w = xmalloc((size_t)2 * G * cp * reps), *rw = xmalloc((size_t)2 * G * H * reps);
        float *b = xmalloc(sizeof(float) * 2 * 2 * G);
        void *scratch = xmalloc(nn_lstm_scratch(tin, cp, H));
        fill_f(x, (size_t)tin * L->cin, 1.0f);
        fill_b(w, (size_t)2 * G * cp * reps);
        fill_b(rw, (size_t)2 * G * H * reps);
        fill_f(b, (size_t)4 * G, 0.1f);
        double t0 = now_s();
        for (int r = 0; r < reps; r++) {
            nn_lstm l = { .in_pad = cp, .hidden = H };
            for (int d = 0; d < 2; d++) {
                l.w[d] = w + ((size_t)r * 2 + d) * G * cp;
                l.r[d] = rw + ((size_t)r * 2 + d) * G * H;
                l.b[d] = b + (size_t)d * 2 * G;
                l.wscale[d] = l.rscale[d] = 0.004f;
                l.wzp[d] = l.rzp[d] = 0;
            }
            nn_lstm_run(&l, 2, x, tin, L->cin, L->cin, y, 2 * H, scratch);
        }
        spent = now_s() - t0;
        free(x); free(y); free(w); free(rw); free(b); free(scratch);
        break;
    }
    case B_ATTN: {
        size_t sz = (size_t)tin * L->cin;
        int dh = L->cin / 12;
        float *q = xmalloc(sizeof(float) * sz * 4), *s = xmalloc(sizeof(float) * ((size_t)tin * (dh + 1) + dh));
        fill_f(q, sz * 3, 1.0f);
        double t0 = now_s();
        for (int r = 0; r < reps; r++)
            nn_attention(q, q + sz, q + 2 * sz, L->cin, tin, 12, dh, sqrtf(1.0f / sqrtf((float)dh)), NULL, q + 3 * sz,
                            L->cin, s);
        spent = now_s() - t0;
        free(q); free(s);
        break;
    }
    case B_LN: case B_IN: case B_SNAKE: case B_LEAKY: {
        size_t sz = (size_t)tin * L->cin;
        float *x = xmalloc(sizeof(float) * sz), *g = xmalloc(sizeof(float) * (size_t)L->cin);
        float *b = xmalloc(sizeof(float) * (size_t)L->cin);
        fill_f(x, sz, 2.0f);
        for (int c = 0; c < L->cin; c++) { g[c] = 1.0f + 0.1f * (float)(c % 7); b[c] = 1.0f / g[c]; }
        double t0 = now_s();
        for (int r = 0; r < reps; r++) {
            if (L->kind == B_LN) nn_layernorm(x, tin, L->cin, L->cin, g, b, 1e-5f);
            else if (L->kind == B_IN) nn_instnorm(x, tin, L->cin, L->cin, g, b, 1e-5f);
            else if (L->kind == B_SNAKE) nn_snake(x, tin, L->cin, L->cin, g, b);
            else nn_leaky_relu(x, sz, 0.2f);
        }
        spent = now_s() - t0;
        free(x); free(g); free(b);
        break;
    }
    case B_CONVT: {
        int mt = L->k / L->stride, np = NN_ROUND(L->cout, 8);
        float *x = calloc((size_t)(tin + 2 * mt) * L->cin, sizeof(float));
        float *w = xmalloc(sizeof(float) * (size_t)L->k * L->cin * np * reps);
        float *bias = xmalloc(sizeof(float) * (size_t)L->cout), *y = xmalloc(sizeof(float) * (size_t)tout * L->cout);
        if (!x) { fprintf(stderr, "tts_pack: out of memory\n"); exit(1); }
        fill_f(x + (size_t)(mt - 1) * L->cin, (size_t)tin * L->cin, 1.0f);
        fill_f(w, (size_t)L->k * L->cin * np * reps, 0.05f);
        fill_f(bias, (size_t)L->cout, 0.1f);
        double t0 = now_s();
        for (int r = 0; r < reps; r++)
            nn_convt(x, tin, L->cin, L->k, L->stride, L->dil, w + (size_t)r * L->k * L->cin * np, bias, L->cout,
                        tout, y, L->cout);
        spent = now_s() - t0;
        free(x); free(w); free(bias); free(y);
        break;
    }
    case B_CONVT_DW: {
        float *x = xmalloc(sizeof(float) * (size_t)tin * L->cin), *w = xmalloc(sizeof(float) * (size_t)L->k * L->cin);
        float *bias = xmalloc(sizeof(float) * (size_t)L->cin), *y = xmalloc(sizeof(float) * (size_t)tout * L->cin);
        fill_f(x, (size_t)tin * L->cin, 1.0f);
        fill_f(w, (size_t)L->k * L->cin, 0.5f);
        fill_f(bias, (size_t)L->cin, 0.1f);
        double t0 = now_s();
        for (int r = 0; r < reps; r++) nn_convt_dw(x, tin, L->cin, L->cin, L->k, L->stride, 1, w, bias, tout, y, L->cin);
        spent = now_s() - t0;
        free(x); free(w); free(bias); free(y);
        break;
    }
    case B_ISTFT: {
        int bins = L->cin / 2, len = (tin - 1) * L->stride + L->k;
        float *spec = xmalloc(sizeof(float) * (size_t)tin * 24), *wr = xmalloc(sizeof(float) * (size_t)bins * L->k);
        float *wi = xmalloc(sizeof(float) * (size_t)bins * L->k);
        float *out = xmalloc(sizeof(float) * (size_t)len), *full = xmalloc(sizeof(float) * (size_t)len);
        fill_f(spec, (size_t)tin * 24, 2.0f);
        fill_f(wr, (size_t)bins * L->k, 0.1f);
        fill_f(wi, (size_t)bins * L->k, 0.1f);
        double t0 = now_s();
        for (int r = 0; r < reps; r++) nn_istft(spec, tin, 24, bins, wr, wi, L->k, L->stride, 10, out, full);
        spent = now_s() - t0;
        free(spec); free(wr); free(wi); free(out); free(full);
        break;
    }
    }
    return spent;
}

static int do_bench(const char *dir) {
    char path[1024], line[1024];
    snprintf(path, sizeof path, "%s/manifest", dir);
    FILE *m = fopen(path, "r");
    if (!m) { fprintf(stderr, "tts_pack: cannot open %s\n", path); return 1; }
    double by_layer[NLAYERS] = { 0 }, render = 0, audio = 0;
    int fixtures = 0;
    printf("bench: the voice kernels on the model's layer shapes, one thread, random weights\n");
    while (fgets(line, sizeof line, m)) {
        char name[64], voice[32], ids[64];
        int row, tokens;
        float speed;
        long samples;
        if (line[0] == '#' || sscanf(line, "%63s %31s %d %f %d %63s %ld", name, voice, &row, &speed, &tokens, ids, &samples) != 7)
            continue;
        int frames = (int)(samples / 600);
        double t = 0;
        for (size_t i = 0; i < NLAYERS; i++) {
            double s = bench_layer_run(&LAYERS[i], tokens, frames);
            by_layer[i] += s;
            t += s;
        }
        double secs = (double)samples / 24000.0;
        printf("fixture %-14s tokens %3d frames %4d audio %6.3f s  projected %6.3f s  rtf %.3f\n", name, tokens, frames,
               secs, t, t / secs);
        render += t;
        audio += secs;
        fixtures++;
    }
    fclose(m);
    if (!fixtures) { fprintf(stderr, "tts_pack: no fixtures in %s\n", path); return 1; }
    printf("overall: projected %.3f s for %.3f s of audio, rtf %.3f\n", render, audio, render / audio);
    printf("not timed: the harmonic source, residual additions and style scale and shift, upsampling copies,\n"
           "the token-to-frame alignment, the text encoder's GELU\n");
    printf("where the time goes (all fixtures):\n");
    double by_kind[B_ISTFT + 1] = { 0 };
    for (size_t i = 0; i < NLAYERS; i++) {
        by_kind[LAYERS[i].kind] += by_layer[i];
        printf("  %-34s x%-3d %8.3f s  %5.1f%%\n", LAYERS[i].name, LAYERS[i].count, by_layer[i], 100 * by_layer[i] / render);
    }
    printf("by kernel:\n");
    for (int k = 0; k <= B_ISTFT; k++) printf("  %-20s %8.3f s  %5.1f%%\n", KIND_NAME[k], by_kind[k], 100 * by_kind[k] / render);
    return 0;
}

/* ---- rendering: say, parity, bench ---------------------------------------- */

#define NOISE_SEED TTS_VOICE_SEED  /* the daemon's seed, for every noise-on render here */
#define PARITY_DB  40.0        /* criterion 4: noise-off SNR against the reference */
#define NOISE_TOL  0.10        /* criterion 4: the noise's variance, relative */

static voice *open_voice(const char *dir) {
    char path[1024], err[512] = "";
    snprintf(path, sizeof path, "%s/voice.bin", dir);
    voice *v = voice_open(path, err, sizeof err);
    if (!v) fprintf(stderr, "tts_pack: %s\n", err);
    return v;
}

/* One fixture of a manifest (tests/manual/voice_ref.c documents the format),
 * with its noise-off reference from the #quiet line after it. */
typedef struct {
    char name[64], voice[32], ids_file[64], quiet_file[64];
    int row, tokens;
    float speed;
    long samples, quiet_samples;
} fixture;

/* The next fixture with its #quiet line: 1, 0 at the end, -1 on a fixture
 * without one. */
static int next_fixture(FILE *m, fixture *fx) {
    char line[1024];
    int have = 0;
    while (fgets(line, sizeof line, m)) {
        if (!strncmp(line, "#quiet ", 7)) {
            char name[64];
            if (have && sscanf(line, "#quiet %63s %63s %ld", name, fx->quiet_file, &fx->quiet_samples) == 3 &&
                !strcmp(name, fx->name))
                return 1;
            continue;
        }
        if (line[0] == '#') continue;
        if (have) return -1;
        long samples;
        if (sscanf(line, "%63s %31s %d %f %d %63s %ld", fx->name, fx->voice, &fx->row, &fx->speed, &fx->tokens,
                   fx->ids_file, &samples) != 7)
            continue;
        fx->samples = samples;
        have = 1;
    }
    return have ? -1 : 0;
}

/* The fixture's token ids (int64 little-endian in the file); NULL on failure. */
static int *load_ids(const char *dir, const fixture *fx) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, fx->ids_file);
    FILE *f = fopen(path, "rb");
    int *ids = malloc(sizeof(int) * (size_t)(fx->tokens > 0 ? fx->tokens : 1));
    int ok = f && ids && fx->tokens > 0;
    for (int i = 0; ok && i < fx->tokens; i++) {
        unsigned char b[8];
        ok = fread(b, 1, 8, f) == 8;
        long long v = 0;
        for (int k = 7; k >= 0; k--) v = v << 8 | b[k];
        ids[i] = (int)v;
    }
    if (f) fclose(f);
    if (!ok) { free(ids); fprintf(stderr, "tts_pack: cannot read %s\n", path); return NULL; }
    return ids;
}

static float *load_f32(const char *dir, const char *file, long n) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, file);
    FILE *f = fopen(path, "rb");
    float *a = malloc(sizeof(float) * (size_t)(n > 0 ? n : 1));
    int ok = f && a && n > 0;
    for (long i = 0; ok && i < n; i++) {
        unsigned char b[4];
        ok = fread(b, 1, 4, f) == 4;
        uint32_t u = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
        memcpy(&a[i], &u, 4);
    }
    if (f) fclose(f);
    if (!ok) { free(a); fprintf(stderr, "tts_pack: cannot read %s\n", path); return NULL; }
    return a;
}

/* 10 log10(sum ref^2 / sum (x - ref)^2); 999 for identical. */
static double snr_db(const float *x, const float *ref, size_t n) {
    double s = 0, e = 0;
    for (size_t i = 0; i < n; i++) {
        double d = (double)x[i] - ref[i];
        s += (double)ref[i] * ref[i];
        e += d * d;
    }
    return e > 0 ? 10.0 * log10(s / e) : 999.0;
}

/* The noise the source adds with the noise on, for the fixture's input: its
 * measured variance, and the model's (the mean of 0.003^2 where voiced and
 * (0.1/3)^2 where not, over every sample and harmonic). 0 or -1. */
static int noise_variance(const voice *v, const int *ids, int n, const float *style, float speed, double *measured,
                          double *expected) {
    char err[512] = "";
    voice_front f;
    if (voice_front_run(v, ids, n, style, speed, &f, err, sizeof err) != 0) {
        fprintf(stderr, "tts_pack: %s\n", err);
        return -1;
    }
    size_t L = (size_t)f.frames * VOICE_FRAME;
    float *har = malloc(sizeof(float) * (120 * (size_t)f.frames + 1) * 22), *nz = malloc(sizeof(float) * L * 9);
    int rc = -1;
    if (har && nz && voice_source(v, f.f0, f.frames, 1, NOISE_SEED, har, nz, err, sizeof err) == 0) {
        double sum = 0, sq = 0, want = 0;
        for (size_t l = 0; l < L; l++) {
            double a = f.f0[l / 300] > 10.0f ? 0.003 : 0.1 / 3.0;
            for (int h = 0; h < 9; h++) {
                sum += nz[l * 9 + h];
                sq += (double)nz[l * 9 + h] * nz[l * 9 + h];
            }
            want += 9 * a * a;
        }
        double cnt = (double)L * 9, mean = sum / cnt;
        *measured = sq / cnt - mean * mean;
        *expected = want / cnt;
        rc = 0;
    } else if (err[0]) fprintf(stderr, "tts_pack: %s\n", err);
    free(har); free(nz);
    voice_front_free(&f);
    return rc;
}

static int do_parity(const char *dir, const char *fixtures) {
    voice *v = open_voice(dir);
    if (!v) return 1;
    char path[1024];
    snprintf(path, sizeof path, "%s/manifest", fixtures);
    FILE *m = fopen(path, "r");
    if (!m) { fprintf(stderr, "tts_pack: cannot open %s\n", path); voice_close(v); return 1; }
    printf("parity: noise off against the noise-off references (>= %.0f dB); noise on, the source's variance (within %.0f%%)\n",
           PARITY_DB, NOISE_TOL * 100);
    printf("%-14s %-15s %6s %8s %8s %8s %12s %12s %7s  %s\n", "fixture", "voice", "tokens", "samples", "ref", "snr dB",
           "noise var", "model var", "ratio", "result");
    fixture fx;
    int r, failed = 0, count = 0;
    while ((r = next_fixture(m, &fx)) != 0) {
        if (r < 0) { fprintf(stderr, "tts_pack: a fixture in %s has no #quiet line\n", path); failed++; break; }
        count++;
        float style[VOICE_STYLE_DIM];
        int *ids = load_ids(fixtures, &fx);
        float *ref = load_f32(fixtures, fx.quiet_file, fx.quiet_samples);
        char err[512] = "";
        size_t n = 0;
        float *pcm = NULL;
        double snr = -999, got = 0, want = 0;
        int ok = ids && ref && voice_style(v, fx.voice, (size_t)fx.row, style) == 0;
        if (ok) pcm = voice_say(v, ids, fx.tokens, style, fx.speed, 0, 0, &n, err, sizeof err);
        if (!pcm && err[0]) fprintf(stderr, "tts_pack: %s: %s\n", fx.name, err);
        ok = ok && pcm && (long)n == fx.quiet_samples;
        if (ok) snr = snr_db(pcm, ref, n);
        ok = ok && snr >= PARITY_DB;
        int nok = ids && noise_variance(v, ids, fx.tokens, style, fx.speed, &got, &want) == 0 && want > 0 &&
                  fabs(got / want - 1.0) <= NOISE_TOL;
        printf("%-14s %-15s %6d %8zu %8ld %8.2f %12.4g %12.4g %7.4f  %s\n", fx.name, fx.voice, fx.tokens, n,
               fx.quiet_samples, snr, got, want, want > 0 ? got / want : 0.0, ok && nok ? "ok" : "FAIL");
        failed += !(ok && nok);
        free(ids); free(ref); free(pcm);
    }
    fclose(m);
    voice_close(v);
    if (!count) { fprintf(stderr, "tts_pack: no fixtures in %s\n", path); return 1; }
    printf("%d fixtures, %d failed\n", count, failed);
    return failed ? 1 : 0;
}

static long peak_rss_kb(void) {
    struct rusage ru;
    return getrusage(RUSAGE_SELF, &ru) == 0 ? ru.ru_maxrss : -1;
}

static int do_bench_render(const char *dir, const char *fixtures) {
    voice *v = open_voice(dir);
    if (!v) return 1;
    char path[1024];
    snprintf(path, sizeof path, "%s/manifest", fixtures);
    FILE *m = fopen(path, "r");
    if (!m) { fprintf(stderr, "tts_pack: cannot open %s\n", path); voice_close(v); return 1; }
    printf("bench: full renders, one thread, noise on (seed %u)\n", NOISE_SEED);
    fixture fx;
    double spent = 0, audio = 0;
    int r, count = 0, failed = 0;
    while ((r = next_fixture(m, &fx)) != 0) {
        if (r < 0) break;
        float style[VOICE_STYLE_DIM];
        int *ids = load_ids(fixtures, &fx);
        char err[512] = "";
        size_t n = 0;
        if (!ids || voice_style(v, fx.voice, (size_t)fx.row, style) != 0) { free(ids); failed++; continue; }
        double t0 = now_s();
        float *pcm = voice_say(v, ids, fx.tokens, style, fx.speed, 1, NOISE_SEED, &n, err, sizeof err);
        double t = now_s() - t0, secs = (double)n / VOICE_RATE;
        if (!pcm) { fprintf(stderr, "tts_pack: %s: %s\n", fx.name, err); free(ids); failed++; continue; }
        printf("fixture %-14s tokens %3d frames %4zu audio %6.3f s  render %6.3f s  rtf %.3f\n", fx.name, fx.tokens,
               n / VOICE_FRAME, secs, t, t / secs);
        spent += t;
        audio += secs;
        count++;
        free(pcm); free(ids);
    }
    fclose(m);
    voice_close(v);
    if (!count) { fprintf(stderr, "tts_pack: no fixtures rendered from %s\n", path); return 1; }
    printf("overall: %.3f s for %.3f s of audio, rtf %.3f\n", spent, audio, spent / audio);
    printf("peak RSS %ld KB, voice.bin's mapped pages included\n", peak_rss_kb());
    return failed ? 1 : 0;
}

/* UTF-8 characters, as the daemon counts the text for the style row. */
static size_t utf8_chars(const char *s) {
    size_t n = 0;
    for (; *s; s++) n += ((unsigned char)*s & 0xC0) != 0x80;
    return n;
}

static void put16(FILE *f, unsigned v) { fputc((int)(v & 0xFF), f); fputc((int)(v >> 8 & 0xFF), f); }
static void put32(FILE *f, uint32_t v) { put16(f, v & 0xFFFF); put16(f, v >> 16); }

/* The language `say` renders in: the daemon's one (OC_TTS_LANG, daemon/tts.h). */
#define SAY_LANG "en-US"

static int do_say(const char *dir, const char *voice_id, const char *text, const char *out) {
    char err[512] = "";
    /* Laid out as the daemon's data directory is (READ-ALOUD.md): the voice
     * model at the top, the pronunciation data in the language's directory.
     * Required, not optional: without the lexicon every word is spelled out. */
    char textdir[4096], lexicon[4200];
    snprintf(textdir, sizeof textdir, "%s/%s", dir, SAY_LANG);
    snprintf(lexicon, sizeof lexicon, "%s/lexicon.bin", textdir);
    if (access(lexicon, R_OK) != 0) { fprintf(stderr, "tts_pack: no pronunciation data at %s\n", lexicon); return 1; }
    tts *t = tts_load(textdir, SAY_LANG, err, sizeof err);
    if (!t) { fprintf(stderr, "tts_pack: %s\n", err); return 1; }
    voice *v = open_voice(dir);
    if (!v) { tts_free(t); return 1; }
    char ipa[8192];
    long long ids64[TTS_VOICE_MAX_TOKENS];
    int ids[TTS_VOICE_MAX_TOKENS], rc = 1;
    float style[VOICE_STYLE_DIM];
    size_t n = 0, tokens = 0;
    float *pcm = NULL;
    if (!tts_text(t, text, ipa, sizeof ipa)) { fprintf(stderr, "tts_pack: nothing to say\n"); goto out; }
    tokens = tts_voice_tokens(ipa, ids64, TTS_VOICE_MAX_TOKENS);
    for (size_t i = 0; i < tokens; i++) ids[i] = (int)ids64[i];
    /* Row min(characters, 399): voice_style takes a row past the table as its last. */
    if (voice_style(v, voice_id, utf8_chars(text), style) != 0) {
        fprintf(stderr, "tts_pack: no voice %s\n", voice_id);
        goto out;
    }
    pcm = voice_say(v, ids, (int)tokens, style, TTS_VOICE_SPEED, 1, NOISE_SEED, &n, err, sizeof err);
    if (!pcm) { fprintf(stderr, "tts_pack: %s\n", err); goto out; }
    FILE *f = fopen(out, "wb");
    if (!f) { fprintf(stderr, "tts_pack: cannot write %s\n", out); goto out; }
    fwrite("RIFF", 1, 4, f); put32(f, (uint32_t)(36 + 2 * n)); fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16); put16(f, 1); put16(f, 1); put32(f, VOICE_RATE); put32(f, 2 * VOICE_RATE); put16(f, 2); put16(f, 16);
    fwrite("data", 1, 4, f); put32(f, (uint32_t)(2 * n));
    for (size_t i = 0; i < n; i++) {
        float s = pcm[i] * 32767.0f;
        s = s > 32767.0f ? 32767.0f : s < -32768.0f ? -32768.0f : s;
        put16(f, (unsigned)(uint16_t)(int16_t)lrintf(s));
    }
    rc = fclose(f) ? 1 : 0;
    if (!rc) printf("%s\n%zu tokens, %zu samples (%.3f s)\n", ipa, tokens, n, (double)n / VOICE_RATE);
out:
    free(pcm);
    voice_close(v);
    tts_free(t);
    return rc;
}

int main(int argc, char **argv) {
    char err[512] = "";
    if (argc < 2) return usage();
    if (!strcmp(argv[1], "bench") && argc == 3) return do_bench(argv[2]);
    if (!strcmp(argv[1], "bench") && argc == 4) return do_bench_render(argv[2], argv[3]);
    if (!strcmp(argv[1], "parity") && argc == 4) return do_parity(argv[2], argv[3]);
    if (!strcmp(argv[1], "say") && argc == 6) return do_say(argv[2], argv[3], argv[4], argv[5]);
    if (!strcmp(argv[1], "ipa") && argc == 5) return do_ipa(argv[2], argv[3], argv[4]);
    if (!strcmp(argv[1], "lexicon") && argc == 5) {
        if (tts_pack_lexicon(argv[2], argv[3], argv[4], err, sizeof err)) { fprintf(stderr, "tts_pack: %s\n", err); return 1; }
        return 0;
    }
    if (!strcmp(argv[1], "guesser") && argc == 5) {
        if (tts_pack_guesser(argv[2], argv[3], argv[4], err, sizeof err)) { fprintf(stderr, "tts_pack: %s\n", err); return 1; }
        return 0;
    }
    if ((!strcmp(argv[1], "word") || !strcmp(argv[1], "text")) && argc >= 4) {
        /* NULL: a tool says whatever the data says, rather than asserting a
         * language it was not told. */
        tts *t = tts_load(argv[2], NULL, err, sizeof err);
        if (!t) { fprintf(stderr, "tts_pack: %s\n", err); return 1; }
        char out[4096];
        if (!strcmp(argv[1], "text")) {
            tts_text(t, argv[3], out, sizeof out);
            printf("%s\n", out);
        } else {
            static const char *how[] = { "none", "lexicon", "guessed", "spelled" };
            for (int i = 3; i < argc; i++) {
                int r = tts_word(t, argv[i], out, sizeof out);
                printf("%s\t%s\t%s\n", argv[i], out, how[r]);
            }
        }
        tts_free(t);
        return 0;
    }
    return usage();
}
