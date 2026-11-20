/* Tests for ttskit, read-aloud's pronunciation (ttskit/, ARCH-111, TTSKIT.md):
 * the ARPAbet->IPA table, number and abbreviation expansion, dictionary, guesser
 * and spelling, whole sentences, the committed data against its pins, the C
 * guesser against Phonetisaurus's own decoder, damaged data refused, and the
 * IPA turned into the voice model's token ids (daemon/tts_voice_tokens.c), and
 * the voice model's kernels (voice_ops.c) against naive versions of each, the
 * duration rule and the token-to-frame alignment (voice_model.c), and voice.bin:
 * the loader (voice.c), the converter (daemon/voice_pack.c) and the committed
 * ttskit/data/voice.bin. */
#include "check.h"
#include "testenv.h"
#include "ttskit.h"
#include "tts_priv.h"
#include "tts_voice.h"
#include "voice_pack.h"

#include <mbedtls/sha256.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define LANG "en-US"

/* The tree's data, found from the binary (tests/testenv.h), so the suite runs
 * from any working directory. */
static char g_data[1024];
#define DATA g_data
static const char *data_file(const char *name, char *out, size_t cap) {
    snprintf(out, cap, "%s/%s", g_data, name);
    return out;
}

/* The committed data files. Regenerating them (scripts/build/ttskit_data.sh)
 * changes these, the reference output below and TTSKIT.md §8 together. */
static const char *LEXICON_SHA256 = "b1338cd8ae4d22c8b8a808e4c1fa1879579acbd3157556488d9f43ba5b559bf7";
static const char *GUESSES_SHA256 = "51cf60b34b3f23a12a33bb5d5c4a239f92966e5a92e38ee0a649b2df43e0794e";

static int ipa_is(const char *arpa, int units, const char *want) {
    char out[256];
    tts_arpa_to_ipa(arpa, units, out, sizeof out);
    if (strcmp(out, want) != 0) printf("    arpa   \"%s\"\n    ipa    \"%s\"\n    wanted \"%s\"\n", arpa, out, want);
    return strcmp(out, want) == 0;
}

static int norm_is(const char *text, const char *want) {
    char out[1024];
    tts_normalize(text, out, sizeof out);
    if (strcmp(out, want) != 0) printf("    text   \"%s\"\n    got    \"%s\"\n    wanted \"%s\"\n", text, out, want);
    return strcmp(out, want) == 0;
}

static int word_is(tts *t, const char *word, int how, const char *want) {
    char out[256];
    int r = tts_word(t, word, out, sizeof out);
    if (r != how || strcmp(out, want) != 0)
        printf("    word   \"%s\"\n    got    %d \"%s\"\n    wanted %d \"%s\"\n", word, r, out, how, want);
    return r == how && strcmp(out, want) == 0;
}

static int text_is(tts *t, const char *text, const char *want) {
    char out[1024];
    tts_text(t, text, out, sizeof out);
    if (strcmp(out, want) != 0) printf("    text   \"%s\"\n    got    \"%s\"\n    wanted \"%s\"\n", text, out, want);
    return strcmp(out, want) == 0;
}

static int sha256_is(const char *path, const char *want) {
    FILE *f = fopen(path, "rb");
    if (!f) { printf("    cannot open %s\n", path); return 0; }
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts(&c, 0);
    unsigned char buf[65536], sum[32];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) mbedtls_sha256_update(&c, buf, n);
    fclose(f);
    mbedtls_sha256_finish(&c, sum);
    mbedtls_sha256_free(&c);
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", sum[i]);
    if (strcmp(hex, want) != 0) printf("    %s\n    sha256 %s\n    pinned %s\n", path, hex, want);
    return strcmp(hex, want) == 0;
}

/* tests/data/ttskit_guess_ref.txt is Phonetisaurus's decoder (phonetisaurus-g2pfst,
 * best path) on 3,010 words with the model guesses.bin was packed from. Returns
 * how many of them the C decoder pronounces identically. */
static int guess_agreement(int *total) {
    tts_guesser g;
    char err[256];
    *total = 0;
    char gp[1100], ref[1024];
    if (tts_guesser_open(data_file("guesses.bin", gp, sizeof gp), NULL, &g, err, sizeof err) != 0) { printf("    %s\n", err); return 0; }
    FILE *f = fopen(oc_test_tree_path("tests/data/ttskit_guess_ref.txt", ref, sizeof ref), "r");
    if (!f) { tts_guesser_close(&g); return 0; }
    char line[512], out[256];
    int same = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = '\0';
        (*total)++;
        tts_guess(&g, line, out, sizeof out);
        if (strcmp(out, tab + 1) == 0) same++;
    }
    fclose(f);
    tts_guesser_close(&g);
    return same;
}

static void put(const char *path, const void *p, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(p, 1, n, f);
    fclose(f);
}

static unsigned char *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *p = len > 0 ? malloc((size_t)len) : NULL;
    if (p && fread(p, 1, (size_t)len, f) != (size_t)len) { free(p); p = NULL; }
    fclose(f);
    *n = p ? (size_t)len : 0;
    return p;
}

/* Damaged data: a file that is present but wrong refuses to load, so a bad
 * install fails loudly instead of guessing every word -- refused for what it
 * is (`want`), never taken for a file that is simply absent, which the disk
 * path tolerates and would load as half a kit in silence. */
static int refused(const char *dir, const char *name, const unsigned char *p, size_t n,
                   const char *lang, tts_status want) {
    char path[512], err[256] = "";
    snprintf(path, sizeof path, "%s/%s", dir, name);
    put(path, p, n);
    tts_status st = TTS_LOADED;
    tts *t = tts_load_ex(dir, lang, &st, err, sizeof err);
    unlink(path);
    if (t) { tts_free(t); return 0; }
    if (st != want) printf("    %s: refused as %d, wanted %d (%s)\n", name, (int)st, (int)want, err);
    return st == want && err[0] != '\0';
}

/* ---- the voice model's kernels (ttskit/voice_ops.c) against naive versions ----
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

/* ONNX DynamicQuantizeLinear, written from the operator's definition. */
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
    voice_qparam qp;
    dql_naive(x, n, a, &s, &zp);
    voice_dql_u8(x, n, b, &qp);
    int ok = qp.scale == s && qp.zp == zp && memcmp(a, b, n) == 0;
    if (!ok) printf("    dql: scale %g/%g zp %d/%d\n", (double)qp.scale, (double)s, qp.zp, zp);
    return ok;
}

static int dql_checks(void) {
    int ok = 1;
    /* Scale 1, zero point 0: halves round to even. */
    const float ties[] = { 0, 0.5f, 1.5f, 2.5f, 3.5f, 254.5f, 255, 100.25f };
    uint8_t q[8];
    voice_qparam qp;
    voice_dql_u8(ties, 8, q, &qp);
    const uint8_t want[] = { 0, 0, 2, 2, 4, 254, 255, 100 };
    ok &= qp.scale == 1.0f && qp.zp == 0 && memcmp(q, want, 8) == 0;
    /* A zero point of 1.5 rounds to 2. */
    const float zt[] = { -1.5f, 253.5f, 0.5f, -0.5f };
    voice_dql_u8(zt, 4, q, &qp);
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
    voice_quantize(m, 5, 20, 20, q16, 32, &qp);
    float s;
    int zp;
    dql_naive(m, 100, q8, &s, &zp);
    ok &= qp.scale == s && qp.zp == zp;
    for (int r = 0; r < 5; r++)
        for (int c = 0; c < 32; c++) ok &= q16[r * 32 + c] == (c < 20 ? q8[r * 20 + c] - zp : 0);
    return ok;
}

/* ConvInteger from its definition, on the ONNX layout W[co][ci][j]. */
static int conv_case(int cin, int cout, int k, int stride, int dil, int t_out, int wzp) {
    int cp = VOICE_ROUND(cin, VOICE_CPAD), co4 = VOICE_ROUND(cout, 4);
    int rows = (t_out - 1) * stride + (k - 1) * dil + 1;
    int16_t *x = calloc((size_t)rows * cp, sizeof *x);
    uint8_t *w = malloc((size_t)cout * cin * k), *wp = malloc((size_t)co4 * k * cp);
    int32_t *y = malloc((size_t)t_out * cout * sizeof *y);
    int16_t *wbuf = malloc(VOICE_WBUF(k, cp) * sizeof *wbuf);
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cin; c++) x[r * cp + c] = (int16_t)(rnd() % 7 == 0 ? (rnd() & 1 ? 255 : -255) : rnd_int(-255, 255));
    for (int i = 0; i < cout * cin * k; i++) w[i] = (uint8_t)(rnd() % 9 == 0 ? (rnd() & 1 ? 255 : 0) : rnd_int(0, 255));
    memset(wp, wzp, (size_t)co4 * k * cp);
    for (int co = 0; co < cout; co++)
        for (int ci = 0; ci < cin; ci++)
            for (int j = 0; j < k; j++) wp[((size_t)co * k + j) * cp + ci] = w[(co * cin + ci) * k + j];
    voice_conv_q(x, t_out, stride, dil, k, cp, wp, wzp, cout, y, cout, wbuf);
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

/* MatMulInteger from its definition, on the ONNX layout B[k][n]. */
static int matmul_case(int m, int k, int n, int bzp) {
    int kp = VOICE_ROUND(k, VOICE_CPAD), n4 = VOICE_ROUND(n, 4);
    int16_t *a = calloc((size_t)m * kp, sizeof *a);
    int8_t *b = malloc((size_t)k * n), *bp = malloc((size_t)n4 * kp);
    int32_t *y = malloc((size_t)m * n * sizeof *y);
    int16_t *wbuf = malloc(VOICE_WBUF(1, kp) * sizeof *wbuf);
    for (int r = 0; r < m; r++)
        for (int i = 0; i < k; i++) a[r * kp + i] = (int16_t)rnd_int(-255, 255);
    for (int i = 0; i < k * n; i++) b[i] = (int8_t)(rnd() % 9 == 0 ? (rnd() & 1 ? 127 : -128) : rnd_int(-128, 127));
    memset(bp, (unsigned char)(int8_t)bzp, (size_t)n4 * kp);
    for (int i = 0; i < k; i++)
        for (int c = 0; c < n; c++) bp[(size_t)c * kp + i] = b[i * n + c];
    voice_matmul_q(a, m, kp, bp, bzp, n, y, n, wbuf);
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

/* DynamicQuantizeLSTM as ONNX Runtime defines it, from the contrib op's layouts
 * (W [dir][in][4H], R [dir][H][4H]), one step at a time. */
static int lstm_case(int t, int in, int hidden, int dirs) {
    int ip = VOICE_ROUND(in, VOICE_CPAD), G = 4 * hidden;
    float *x = malloc(sizeof(float) * (size_t)t * in), *y = malloc(sizeof(float) * (size_t)t * 2 * hidden);
    float *ref = malloc(sizeof(float) * (size_t)t * 2 * hidden);
    int8_t *w[2], *r[2], *wp[2], *rp[2];
    float *b[2];
    voice_lstm l = { .in_pad = ip, .hidden = hidden };
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
    void *scratch = malloc(voice_lstm_scratch(t, ip, hidden));
    voice_lstm_run(&l, dirs, x, t, in, in, y, 2 * hidden, scratch);

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
        voice_layernorm(x, 3, cols, cols, g, b, 1e-5f);
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
        voice_instnorm(x, t, c, c, pass ? g : NULL, pass ? b : NULL, 1e-5f);
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
        voice_attention(q, k, v, LD, N, HEADS, DH, sqrtf(0.125f), masked ? mask : NULL, out, LD, scratch);
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
    int mt = k / s, np = VOICE_ROUND(cout, 8), t_out = (t - 1) * s - pad_begin - pad_end + k;
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
    voice_convt(x, t, cin, k, s, pad_begin, wp, bias, cout, t_out, y, cout);
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
    voice_convt_dw(x, t, c, c, k, s, pad_begin, w, bias, t_out, y, c);
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
    voice_matmul_f(x, XS, M, K, w, N, bias, y, N);
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
    voice_leaky_relu(x, 199, 0.2f);
    for (int i = 0; i < 199; i++) ok &= x[i] == (keep[i] < 0 ? keep[i] * 0.2f : keep[i]);
    ok &= x[199] == keep[199];
    /* sin: within 3e-7 of the true value for |x| <= 100, 1e-6 to 1e4. */
    double worst = 0, worst_far = 0;
    for (int i = 0; i < 200; i++) x[i] = rnd_f(-100, 100);
    x[0] = 0; x[1] = 1.5707963f; x[2] = -3.1415927f; x[3] = 3.1415927f; x[4] = 0.5f;
    voice_sin(x, y, 200);
    for (int i = 0; i < 200; i++) worst = fmax(worst, fabs(y[i] - sin((double)x[i])));
    for (int i = 0; i < 200; i++) x[i] = rnd_f(-1e4f, 1e4f);
    voice_sin(x, y, 197);
    for (int i = 0; i < 197; i++) worst_far = fmax(worst_far, fabs(y[i] - sin((double)x[i])));
    if (worst > 3e-7 || worst_far > 1e-6) printf("    sin: worst error %.3g near, %.3g far\n", worst, worst_far);
    ok &= worst <= 3e-7 && worst_far <= 1e-6;
    /* Snake on 9 rows of 37 channels (two blocks of 16 and a tail). */
    float a[37], inv[37], z[9 * 37], zk[9 * 37];
    for (int c = 0; c < 37; c++) { a[c] = rnd_f(0.5f, 1.5f); inv[c] = 1.0f / a[c]; }
    for (int i = 0; i < 9 * 37; i++) zk[i] = z[i] = rnd_f(-4, 4);
    voice_snake(z, 9, 37, 37, a, inv);
    for (int i = 0; i < 9 * 37; i++) {
        double s = sin((double)a[i % 37] * zk[i]);
        ok &= close_to(z[i], zk[i] + inv[i % 37] * s * s, 1e-6);   /* tolerance 1e-6 */
    }
    return ok;
}

/* sin and atan as ONNX Runtime computes them, to the bit: outputs its CPU
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
    voice_sin(x, y, 8);
    for (int i = 0; i < 8; i++) ok &= memcmp(&y[i], &SIN[i][1], 4) == 0;
    for (int i = 0; i < 8; i++) memcpy(&x[i], &ATAN[i][0], 4);
    x[8] = 0.3f;
    voice_atan(x, y, 9);
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
        float mean = voice_reduce_sum(col, t, (size_t)ch * t) / (float)t;
        for (int r = 0; r < t; r++) sq[r] = (col[r] - mean) * (col[r] - mean);
        float k = 1.0f / sqrtf(voice_reduce_sum(sq, t, 0) / (float)t + 1e-5f), sh = 0.0f - mean * k;
        for (int r = 0; r < t; r++) ref[r * c + ch] = col[r] * k + sh;
    }
    if (ok) voice_instnorm(x, t, c, c, NULL, NULL, 1e-5f);
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
    voice_istft(spec, T, LD, BINS, wr, wi, NFFT, HOP, TRIM, out, full);
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

/* ---- the forward pass's first half: the pieces that need no voice.bin ----------
 * A token's frame count is round(the sum of its 50 duration bins / speed): the
 * sum in ONNX Runtime's order, rounded half to even, at least 1; then each frame
 * takes its token's row. Hand-made inputs, expected values worked out here. */
static int duration_checks(void) {
    static const struct { float raw; int want; } R[] = {
        { 0.0f, 1 }, { 0.3f, 1 }, { 0.5f, 1 },               /* 0.5 rounds to 0: at least 1 */
        { 1.4999999f, 1 }, { 1.5f, 2 }, { 2.5f, 2 },          /* half to even */
        { 2.5000002f, 3 }, { 3.5f, 4 }, { 12.48477f, 12 }, { 49.9f, 50 },
    };
    int ok = 1;
    for (size_t i = 0; i < sizeof R / sizeof R[0]; i++)
        if (voice_round_duration(R[i].raw) != R[i].want) {
            printf("    duration %.7g: %d, want %d\n", (double)R[i].raw, voice_round_duration(R[i].raw), R[i].want);
            ok = 0;
        }
    /* The bins' sum is Eigen's on 4-lane vectors, from the first 16-byte aligned
     * value. 2^24 and seven 1s (float spacing at 2^24 is 2, ties to even):
     *  - aligned: lanes {2^24, 1, 1, 1} + {1, 1, 1, 1} = {2^24, 2, 2, 2} (2^24 + 1
     *    ties down), then (2^24 + 2) + (2 + 2) = 2^24 + 6;
     *  - starting 2 floats past alignment: the vector is values 2..5, its lanes
     *    (1 + 1) + (1 + 1) = 4, then the two before it (2^24, then 1: 2^24 + 4
     *    and 2^24 + 5 ties to 2^24 + 4), then the two after (ties again);
     *  - in order one by one it would stay 2^24. */
    float a[8] = { 16777216.0f, 1, 1, 1, 1, 1, 1, 1 };
    ok &= voice_reduce_sum(a, 8, 0) == 16777222.0f;
    ok &= voice_reduce_sum(a, 8, 2) == 16777220.0f;
    ok &= voice_reduce_sum(a, 8, 6) == 16777220.0f;           /* 6 mod 4 = 2: the same */
    ok &= voice_reduce_sum(a, 3, 0) == 16777216.0f;           /* no whole vector: in order */
    return ok;
}

static int align_checks(void) {
    int dur[3] = { 2, 1, 3 }, ft[8], ok = 1;
    static const int want[6] = { 0, 0, 1, 2, 2, 2 };
    ok &= voice_align(dur, 3, ft, 8) == 6 && memcmp(ft, want, sizeof want) == 0;
    ok &= voice_align(dur, 3, ft, 6) == 6;
    ok &= voice_align(dur, 3, ft, 5) == -1;                    /* more frames than room */
    /* Three tokens' rows of 2 values (3 apart), expanded to rows 4 apart. */
    float x[9] = { 1, 2, -1, 3, 4, -1, 5, 6, -1 }, y[24];
    for (int i = 0; i < 24; i++) y[i] = -9;
    voice_expand(x, 3, 2, ft, 6, y, 4);
    for (int f = 0; f < 6; f++) {
        ok &= y[f * 4] == x[want[f] * 3] && y[f * 4 + 1] == x[want[f] * 3 + 1];
        ok &= y[f * 4 + 2] == -9 && y[f * 4 + 3] == -9;        /* nothing past c */
    }
    return ok;
}

static void voice_kernel_tests(void) {
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
    CHECK(duration_checks());
    CHECK(align_checks());
}

/* ---- voice.bin and the converter that makes it -------------------------------
 * A small voice.bin built here byte by byte (ttskit.h has the format) loads and
 * says what was written; one with a wrong magic, a later version or a length
 * that is not the one its header gives is refused. Then the converter: a small
 * model and voices built here, in the upstream formats, convert; the tensors
 * come out where the kernels read them (each run through its kernel against the
 * operator's definition on the model's own layout); and a model or voices that
 * differ from what the table expects are refused, naming the difference. */

static void wr32le(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void wr64le(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }

/* Two tensors: "a.w" f32 [2][3] = 0.5 .. 3.0, "b.q" int8 [4] = -2..1 with scale
 * 0.25 and zero point -3. 388 bytes into p (at least 448). */
static size_t small_voice_bin(uint8_t *p) {
    memset(p, 0, 448);
    memcpy(p, VOICE_MAGIC, 8);
    wr32le(p + 8, VOICE_VERSION);
    wr32le(p + 12, 2);
    wr32le(p + 16, VOICE_HEADER);
    wr32le(p + 20, VOICE_ENTRY);
    wr64le(p + 24, 320);                                  /* 128 + 2 * 96 */
    wr64le(p + 32, 388);
    uint8_t *e = p + VOICE_HEADER;
    memcpy(e, "a.w", 3);
    wr32le(e + 48, VOICE_F32); wr32le(e + 52, 2); wr32le(e + 56, 2); wr32le(e + 60, 3);
    wr64le(e + 72, 320); wr64le(e + 80, 24);
    e += VOICE_ENTRY;
    memcpy(e, "b.q", 3);
    wr32le(e + 48, VOICE_I8); wr32le(e + 52, 1); wr32le(e + 56, 4);
    wr64le(e + 72, 384); wr64le(e + 80, 4);
    float sc = 0.25f;
    memcpy(e + 88, &sc, 4);
    wr32le(e + 92, (uint32_t)-3);
    for (int i = 0; i < 6; i++) { float f = 0.5f * (float)(i + 1); memcpy(p + 320 + 4 * i, &f, 4); }
    for (int i = 0; i < 4; i++) p[384 + i] = (uint8_t)(int8_t)(i - 2);
    return 388;
}

static int small_voice_reads(const voice *v) {
    voice_tensor t;
    int ok = voice_tensor_count(v) == 2 && !voice_tensor_get(v, "a", &t) && !voice_tensor_get(v, "c.w", &t);
    ok = ok && voice_tensor_get(v, "a.w", &t) && t.dtype == VOICE_F32 && t.ndim == 2 && t.dims[0] == 2 &&
         t.dims[1] == 3 && t.bytes == 24 && ((uintptr_t)t.data & 63) == 0;
    for (int i = 0; ok && i < 6; i++) ok = ((const float *)t.data)[i] == 0.5f * (float)(i + 1);
    ok = ok && voice_tensor_get(v, "b.q", &t) && t.dtype == VOICE_I8 && t.ndim == 1 && t.dims[0] == 4 &&
         t.scale == 0.25f && t.zero_point == -3 && ((const int8_t *)t.data)[0] == -2 && ((const int8_t *)t.data)[3] == 1;
    ok = ok && voice_tensor_at(v, 0, &t) && strcmp(t.name, "a.w") == 0 && !voice_tensor_at(v, 2, &t);
    return ok;
}

/* The bytes as a file in `dir`: refused (NULL, with a reason) or not. */
static int voice_file_refused(const char *dir, const uint8_t *p, size_t n) {
    char path[512], err[256] = "";
    put(oc_test_path(path, sizeof path, dir, "voice.bin"), p, n);
    voice *v = voice_open(path, err, sizeof err);
    unlink(path);
    if (v) { voice_close(v); return 0; }
    return err[0] != '\0';
}

static void voice_bin_checks(const char *dir) {
    uint8_t *p = NULL;
    CHECK(posix_memalign((void **)&p, 64, 512) == 0);
    if (!p) return;
    size_t n = small_voice_bin(p);
    char err[256] = "", path[512];
    voice *v = voice_open_mem(p, n, err, sizeof err);
    CHECK(v && small_voice_reads(v));
    voice_close(v);
    put(oc_test_path(path, sizeof path, dir, "voice.bin"), p, n);
    v = voice_open(path, err, sizeof err);
    CHECK(v && small_voice_reads(v));
    voice_close(v);
    unlink(path);
    CHECK(voice_open(path, err, sizeof err) == NULL && err[0]);                    /* absent */

    p[0] = 'X';                                                                    /* magic */
    CHECK(voice_file_refused(dir, p, n));
    p[0] = VOICE_MAGIC[0];
    wr32le(p + 8, VOICE_VERSION + 1);                                              /* version */
    CHECK(voice_file_refused(dir, p, n));
    wr32le(p + 8, VOICE_VERSION);
    CHECK(voice_file_refused(dir, p, n - 1));                                      /* cut short */
    CHECK(voice_file_refused(dir, p, 64));
    memset(p + n, 0, 8);
    CHECK(voice_file_refused(dir, p, n + 8));                                      /* longer */
    wr64le(p + VOICE_HEADER + VOICE_ENTRY + 80, 8);                                /* a size against its shape */
    CHECK(voice_file_refused(dir, p, n));
    wr64le(p + VOICE_HEADER + VOICE_ENTRY + 80, 4);
    wr64le(p + VOICE_HEADER + VOICE_ENTRY + 72, 336);                              /* not 64-byte aligned */
    CHECK(voice_file_refused(dir, p, n));
    wr64le(p + VOICE_HEADER + VOICE_ENTRY + 72, 384);
    memcpy(p + VOICE_HEADER + VOICE_ENTRY, "a.a", 3);                              /* out of order */
    CHECK(voice_file_refused(dir, p, n));
    memcpy(p + VOICE_HEADER + VOICE_ENTRY, "b.q", 3);
    v = voice_open_mem(p, n, err, sizeof err);                                     /* whole again */
    CHECK(v != NULL);
    voice_close(v);
    memmove(p + 1, p, n);                                                          /* lent unaligned */
    err[0] = '\0';
    CHECK(voice_open_mem(p + 1, n, err, sizeof err) == NULL && err[0]);
    free(p);
}

/* A protobuf writer, enough for a small ONNX model. */
typedef struct { uint8_t *p; size_t n, cap; } pbw;

static void pbw_raw(pbw *b, const void *p, size_t n) {
    if (b->n + n > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->n + n) cap *= 2;
        uint8_t *q = realloc(b->p, cap);
        if (!q) abort();
        b->p = q;
        b->cap = cap;
    }
    memcpy(b->p + b->n, p, n);
    b->n += n;
}
static void pbw_varint(pbw *b, uint64_t v) {
    uint8_t c[10];
    int n = 0;
    do { c[n++] = (uint8_t)((v & 0x7f) | (v > 0x7f ? 0x80 : 0)); v >>= 7; } while (v);
    pbw_raw(b, c, (size_t)n);
}
static void pbw_int(pbw *b, int field, int64_t v) { pbw_varint(b, (uint64_t)field << 3); pbw_varint(b, (uint64_t)v); }
static void pbw_bytes(pbw *b, int field, const void *p, size_t n) {
    pbw_varint(b, (uint64_t)field << 3 | 2);
    pbw_varint(b, n);
    pbw_raw(b, p, n);
}
static void pbw_str(pbw *b, int field, const char *s) { pbw_bytes(b, field, s, strlen(s)); }
static void pbw_msg(pbw *b, int field, pbw *sub) { pbw_bytes(b, field, sub->p, sub->n); free(sub->p); memset(sub, 0, sizeof *sub); }

/* An initializer: raw bytes, or (typed) one value in float_data / int32_data. */
static void onnx_tensor(pbw *g, const char *name, int dtype, int ndim, const int *dims, const void *raw, size_t n) {
    pbw t = { 0 };
    for (int i = 0; i < ndim; i++) pbw_int(&t, 1, dims[i]);
    pbw_int(&t, 2, dtype);
    pbw_str(&t, 8, name);
    pbw_bytes(&t, 9, raw, n);
    pbw_msg(g, 5, &t);
}
static void onnx_scalar_f(pbw *g, const char *name, float v) {
    pbw t = { 0 };
    pbw_int(&t, 2, OT_F32);
    pbw_str(&t, 8, name);
    uint32_t b;
    memcpy(&b, &v, 4);
    uint8_t le[4];
    wr32le(le, b);
    pbw_varint(&t, 4 << 3 | 5);                         /* float_data, one value */
    pbw_raw(&t, le, 4);
    pbw_msg(g, 5, &t);
}
static void onnx_scalar_i(pbw *g, const char *name, int dtype, int v) {
    pbw t = { 0 };
    pbw_int(&t, 2, dtype);
    pbw_str(&t, 8, name);
    pbw t2 = { 0 };
    pbw_varint(&t2, (uint64_t)(uint32_t)v);
    pbw_msg(&t, 5, &t2);                                /* int32_data, packed */
    pbw_msg(g, 5, &t);
}

typedef struct { const char *name; int ints[4], nints; float f; const char *s; } onnx_attr_def;

/* A node; `ins` and `outs` NULL-terminated; attributes until a NULL name. */
static void onnx_node(pbw *g, const char *name, const char *op, const char *const *ins, const char *const *outs,
                      const onnx_attr_def *attrs) {
    pbw n = { 0 };
    for (; *ins; ins++) pbw_str(&n, 1, *ins);
    for (; *outs; outs++) pbw_str(&n, 2, *outs);
    pbw_str(&n, 3, name);
    pbw_str(&n, 4, op);
    for (; attrs && attrs->name; attrs++) {
        pbw a = { 0 };
        pbw_str(&a, 1, attrs->name);
        if (attrs->s) pbw_str(&a, 4, attrs->s);
        else if (attrs->nints == 1 && attrs->ints[0] < 0) {           /* -1: a float */
            uint8_t le[4];
            uint32_t b;
            memcpy(&b, &attrs->f, 4);
            wr32le(le, b);
            pbw_varint(&a, 2 << 3 | 5);
            pbw_raw(&a, le, 4);
        } else if (attrs->nints == 0) pbw_int(&a, 3, attrs->ints[0]);
        else for (int i = 0; i < attrs->nints; i++) pbw_int(&a, 8, attrs->ints[i]);
        pbw_msg(&n, 5, &a);
    }
    pbw_msg(g, 1, &n);
}
#define IO(...) ((const char *const[]){ __VA_ARGS__, NULL })

/* f32 to half, for values a half holds exactly (the test's are small sixteenths). */
static uint16_t to_half(float f) {
    if (f == 0) return 0;
    uint16_t s = f < 0 ? 0x8000 : 0;
    float a = fabsf(f);
    int e = 0;
    while (a >= 2.0f) { a /= 2; e++; }
    while (a < 1.0f) { a *= 2; e--; }
    return (uint16_t)(s | (uint16_t)((e + 15) << 10) | (uint16_t)((a - 1.0f) * 1024.0f));
}

/* The test model's constants, kept to check what comes out. */
#define TM_K 20
#define TM_N 6
#define TM_CO 5
#define TM_CI 7
#define TM_KW 3
#define TM_UI 3
#define TM_UO 5
#define TM_UK 4
#define TM_LI 5
#define TM_H 16
typedef struct {
    int8_t  mw[TM_K * TM_N];
    uint8_t cw[TM_CO * TM_CI * TM_KW];
    float   uw[TM_UI * TM_UO * TM_UK], ub[TM_UO];
    int8_t  lw[2 * TM_LI * 4 * TM_H], lr[2 * TM_H * 4 * TM_H];
    float   lb[2 * 8 * TM_H];
} test_model;

/* Variants of the model, each one difference from what the table expects. */
enum { TM_OK, TM_NO_CONV, TM_WIDER_MATMUL, TM_STRAY, TM_DILATION, TM_SCALED_NORM };

static void build_test_model(pbw *out, test_model *tm, int variant) {
    pbw g = { 0 };
    uint32_t seed = 12345;
#define TRND() (seed = seed * 1103515245u + 12345u, (seed >> 8) & 0xffff)
    for (int i = 0; i < TM_K * TM_N; i++) tm->mw[i] = (int8_t)((int)(TRND() % 256) - 128);
    for (int i = 0; i < TM_CO * TM_CI * TM_KW; i++) tm->cw[i] = (uint8_t)(TRND() % 256);
    for (int i = 0; i < TM_UI * TM_UO * TM_UK; i++) tm->uw[i] = (float)((int)(TRND() % 64) - 32) / 16.0f;
    for (int i = 0; i < TM_UO; i++) tm->ub[i] = (float)(i + 1) / 8.0f;
    for (size_t i = 0; i < sizeof tm->lw; i++) tm->lw[i] = (int8_t)((int)(TRND() % 256) - 128);
    for (size_t i = 0; i < sizeof tm->lr; i++) tm->lr[i] = (int8_t)((int)(TRND() % 256) - 128);
    for (int i = 0; i < 2 * 8 * TM_H; i++) tm->lb[i] = (float)i / 4.0f;
#undef TRND
    /* The quantised product: B, its zero point, its scale beside the input's, a bias. */
    int wide = variant == TM_WIDER_MATMUL;
    int8_t mw2[TM_K * (TM_N + 1)];
    memset(mw2, 0, sizeof mw2);
    for (int i = 0; i < TM_K; i++) memcpy(mw2 + i * (TM_N + wide), tm->mw + i * TM_N, TM_N);
    onnx_tensor(&g, "mw", OT_I8, 2, (int[]){ TM_K, TM_N + wide }, mw2, (size_t)TM_K * (TM_N + wide));
    onnx_scalar_i(&g, "mzp", OT_I8, 3);
    onnx_scalar_f(&g, "msc", 0.5f);
    float mb[TM_N] = { 1, 2, 3, 4, 5, 6 };
    onnx_tensor(&g, "mb", OT_F32, 1, (int[]){ TM_N }, mb, sizeof mb);
    onnx_node(&g, "/m/MatMul_quant", "MatMulInteger", IO("x", "mw", "xzp", "mzp"), IO("mq"), NULL);
    onnx_node(&g, "/m/MatMul_quant_scales_mul", "Mul", IO("xs", "msc"), IO("ms"), NULL);
    onnx_node(&g, "/m/Add", "Add", IO("mb", "my"), IO("mo"), NULL);
    /* The quantised convolution, its bias through a Reshape. */
    onnx_tensor(&g, "cw", OT_U8, 3, (int[]){ TM_CO, TM_CI, TM_KW }, tm->cw, sizeof tm->cw);
    onnx_scalar_i(&g, "czp", OT_U8, 128);
    onnx_scalar_f(&g, "csc", 0.25f);
    float cb[TM_CO] = { -1, -2, -3, -4, -5 };
    onnx_tensor(&g, "cb", OT_F32, 1, (int[]){ TM_CO }, cb, sizeof cb);
    int64_t shape[3] = { 1, -1, 1 };
    onnx_tensor(&g, "cshape", 7, 1, (int[]){ 3 }, shape, sizeof shape);
    onnx_attr_def cat[] = { { "dilations", { variant == TM_DILATION ? 1 : 2 }, 1, 0, NULL },
                            { "pads", { 2, 2 }, 2, 0, NULL }, { "strides", { 1 }, 1, 0, NULL },
                            { "group", { 1 }, 0, 0, NULL }, { "kernel_shape", { TM_KW }, 1, 0, NULL },
                            { NULL, { 0 }, 0, 0, NULL } };
    onnx_node(&g, variant == TM_NO_CONV ? "/c/Conv" : "/c/Conv_quant", "ConvInteger", IO("x2", "cw", "x2zp", "czp"), IO("cq"), cat);
    onnx_node(&g, "/c/Conv_quant_scales_mul", "Mul", IO("x2s", "csc"), IO("cs"), NULL);
    onnx_node(&g, "", "Reshape", IO("cb", "cshape"), IO("cbr"), NULL);
    onnx_node(&g, "/c/bias_add", "Add", IO("cy", "cbr"), IO("co"), NULL);
    /* A half-precision transposed convolution. */
    uint16_t uw16[TM_UI * TM_UO * TM_UK], ub16[TM_UO];
    for (int i = 0; i < TM_UI * TM_UO * TM_UK; i++) uw16[i] = to_half(tm->uw[i]);
    for (int i = 0; i < TM_UO; i++) ub16[i] = to_half(tm->ub[i]);
    onnx_tensor(&g, "uw", OT_F16, 3, (int[]){ TM_UI, TM_UO, TM_UK }, uw16, sizeof uw16);
    onnx_tensor(&g, "ub", OT_F16, 1, (int[]){ TM_UO }, ub16, sizeof ub16);
    onnx_attr_def uat[] = { { "strides", { 2 }, 1, 0, NULL }, { "pads", { 1, 1 }, 2, 0, NULL },
                            { NULL, { 0 }, 0, 0, NULL } };
    onnx_node(&g, "/u/ConvTranspose", "ConvTranspose", IO("h", "uw", "ub"), IO("uo"), uat);
    /* A norm without an affine part, and a layer norm with one. */
    float ones[4] = { 1, 1, 1, variant == TM_SCALED_NORM ? 2.0f : 1.0f }, zeros[4] = { 0 }, lg[4] = { 2, 3, 4, 5 };
    onnx_tensor(&g, "ng", OT_F32, 1, (int[]){ 4 }, ones, sizeof ones);
    onnx_tensor(&g, "nb", OT_F32, 1, (int[]){ 4 }, zeros, sizeof zeros);
    onnx_tensor(&g, "lg", OT_F32, 1, (int[]){ 4 }, lg, sizeof lg);
    onnx_tensor(&g, "lb", OT_F32, 1, (int[]){ 4 }, ones, sizeof ones);
    onnx_attr_def eps5[] = { { "epsilon", { -1 }, 1, 1e-5f, NULL }, { NULL, { 0 }, 0, 0, NULL } };
    onnx_attr_def eps12[] = { { "epsilon", { -1 }, 1, 1e-12f, NULL }, { NULL, { 0 }, 0, 0, NULL } };
    onnx_node(&g, "/n/InstanceNormalization", "InstanceNormalization", IO("h2", "ng", "nb"), IO("no"), eps5);
    onnx_node(&g, "/l/LayerNormalization", "LayerNormalization", IO("h3", "lg", "lb"), IO("lo"), eps12);
    /* A bidirectional quantised LSTM. */
    onnx_tensor(&g, "lw", OT_I8, 3, (int[]){ 2, TM_LI, 4 * TM_H }, tm->lw, sizeof tm->lw);
    onnx_tensor(&g, "lr", OT_I8, 3, (int[]){ 2, TM_H, 4 * TM_H }, tm->lr, sizeof tm->lr);
    onnx_tensor(&g, "lb8", OT_F32, 2, (int[]){ 2, 8 * TM_H }, tm->lb, sizeof tm->lb);
    float lws[2] = { 0.125f, 0.375f }, lrs[2] = { 0.5f, 0.75f };
    int8_t lwz[2] = { 1, -2 }, lrz[2] = { 0, 4 };
    onnx_tensor(&g, "lws", OT_F32, 1, (int[]){ 2 }, lws, sizeof lws);
    onnx_tensor(&g, "lwz", OT_I8, 1, (int[]){ 2 }, lwz, sizeof lwz);
    onnx_tensor(&g, "lrs", OT_F32, 1, (int[]){ 2 }, lrs, sizeof lrs);
    onnx_tensor(&g, "lrz", OT_I8, 1, (int[]){ 2 }, lrz, sizeof lrz);
    onnx_attr_def lat[] = { { "hidden_size", { TM_H }, 0, 0, NULL }, { "direction", { 0 }, 0, 0, "bidirectional" },
                            { NULL, { 0 }, 0, 0, NULL } };
    onnx_node(&g, "/r/LSTM_quant", "DynamicQuantizeLSTM",
              IO("xr", "lw", "lr", "lb8", "", "", "", "", "lws", "lwz", "lrs", "lrz"), IO("ry"), lat);
    /* Plumbing the table need not account for: a Resize's scales. */
    float rs[3] = { 1, 1, 2 };
    onnx_tensor(&g, "rscales", OT_F32, 1, (int[]){ 3 }, rs, sizeof rs);
    onnx_node(&g, "/up/Resize", "Resize", IO("h4", "", "rscales"), IO("ro"), NULL);
    if (variant == TM_STRAY) {
        uint8_t stray[4] = { 1, 2, 3, 4 };
        onnx_tensor(&g, "stray", OT_U8, 1, (int[]){ 4 }, stray, sizeof stray);
    }
    memset(out, 0, sizeof *out);
    pbw_int(out, 1, 8);                                   /* ir_version */
    pbw_msg(out, 7, &g);
}

static const oc_voice_row TEST_ROWS[] = {
    { "t.m.w", "/m/MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { TM_K, TM_N }, { 0 }, 0 },
    { "t.m.b", "/m/Add", VK_F32, 0, 0, OT_F32, 1, { TM_N }, { 0 }, 0 },
    { "t.c.w", "/c/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { TM_CO, TM_CI, TM_KW }, { 1, 2, 2, 2 }, 0 },
    { "t.c.b", "/c/bias_add", VK_F32, 1, 0, OT_F32, 1, { TM_CO }, { 0 }, 0 },
    { "t.u.w", "/u/ConvTranspose", VK_CONVT, 1, 0, OT_F16, 3, { TM_UI, TM_UO, TM_UK }, { 2, 1, 1, 0 }, 0 },
    { "t.u.b", "/u/ConvTranspose", VK_F32, 2, 0, OT_F16, 1, { TM_UO }, { 0 }, 0 },
    { NULL, "/n/InstanceNormalization", VK_NORM1, 1, 0, OT_F32, 1, { 4 }, { 0 }, 1e-5f },
    { "t.l.g", "/l/LayerNormalization", VK_F32, 1, 0, OT_F32, 1, { 4 }, { 0 }, 0 },
    { "t.l.b", "/l/LayerNormalization", VK_F32, 2, 0, OT_F32, 1, { 4 }, { 0 }, 0 },
    { "t.l.eps", "/l/LayerNormalization", VK_EPS, 0, 0, OT_F32, 1, { 1 }, { 0 }, 0 },
    { "t.r.fwd.w", "/r/LSTM_quant", VK_LSTM_W, 1, 0, OT_I8, 3, { 2, TM_LI, 4 * TM_H }, { 0 }, 0 },
    { "t.r.bwd.w", "/r/LSTM_quant", VK_LSTM_W, 1, 1, OT_I8, 3, { 2, TM_LI, 4 * TM_H }, { 0 }, 0 },
    { "t.r.bwd.r", "/r/LSTM_quant", VK_LSTM_R, 2, 1, OT_I8, 3, { 2, TM_H, 4 * TM_H }, { 0 }, 0 },
    { "t.r.bwd.b", "/r/LSTM_quant", VK_LSTM_B, 3, 1, OT_F32, 2, { 2, 8 * TM_H }, { 0 }, 0 },
};
static const char *const TEST_VOICES[] = { "va", "vb" };
static const oc_voice_spec TEST_SPEC = { TEST_ROWS, sizeof TEST_ROWS / sizeof TEST_ROWS[0], TEST_VOICES, 2, 4, 8 };

/* A stored zip of .npy members, as numpy writes voices.npz (zip64 sizes left
 * out: the reader takes either). Voice v's value i is v * 100 + i. */
static void build_test_voices(pbw *z, int with_vb, int extra) {
    const char *names[3] = { "va", with_vb ? "vb" : NULL, extra ? "vc" : NULL };
    memset(z, 0, sizeof *z);
    for (int v = 0; v < 3; v++) {
        if (!names[v]) continue;
        char member[16], hdr[128];
        snprintf(member, sizeof member, "%s.npy", names[v]);
        int hl = snprintf(hdr, sizeof hdr, "{'descr': '<f4', 'fortran_order': False, 'shape': (4, 8), }");
        while ((10 + hl + 1) % 64) hdr[hl++] = ' ';
        hdr[hl++] = '\n';
        size_t size = 10 + (size_t)hl + 4 * 8 * 4;
        uint8_t lh[30] = { 0x50, 0x4b, 3, 4, 20 };
        wr32le(lh + 18, (uint32_t)size);
        wr32le(lh + 22, (uint32_t)size);
        lh[26] = (uint8_t)strlen(member);
        pbw_raw(z, lh, 30);
        pbw_raw(z, member, strlen(member));
        uint8_t nh[10] = { 0x93, 'N', 'U', 'M', 'P', 'Y', 1, 0, (uint8_t)hl, (uint8_t)(hl >> 8) };
        pbw_raw(z, nh, 10);
        pbw_raw(z, hdr, (size_t)hl);
        for (int i = 0; i < 32; i++) {
            float f = (float)(v * 100 + i);
            uint8_t le[4];
            uint32_t b;
            memcpy(&b, &f, 4);
            wr32le(le, b);
            pbw_raw(z, le, 4);
        }
    }
    uint8_t end[22] = { 0x50, 0x4b, 5, 6 };
    pbw_raw(z, end, 22);
}

/* Convert; 0 on success, else -1 with the reason in err. */
static int test_pack(int variant, int with_vb, int extra, const char *out, char *err, size_t cap, test_model *tm) {
    pbw m, z;
    build_test_model(&m, tm, variant);
    build_test_voices(&z, with_vb, extra);
    err[0] = '\0';
    int rc = oc_voice_pack_mem(&TEST_SPEC, m.p, m.n, z.p, z.n, out, NULL, err, cap);
    free(m.p);
    free(z.p);
    return rc;
}

/* The converted tensors, through the kernels that read them, against each
 * operator's definition on the model's layout. */
static int converted_right(const voice *v, const test_model *tm) {
    voice_tensor t;
    int ok = 1;
    /* MatMulInteger: y[r][c] = sum_i a[r][i] (B[i][c] - zp). */
    if (!voice_tensor_get(v, "t.m.w", &t) || t.dtype != VOICE_I8 || t.dims[0] != 8 || t.dims[1] != 32 ||
        t.scale != 0.5f || t.zero_point != 3)
        return 0;
    {
        int16_t a[2 * 32] = { 0 }, wbuf[VOICE_WBUF(1, 32)];
        int32_t y[2 * TM_N];
        for (int i = 0; i < 2 * TM_K; i++) a[(i / TM_K) * 32 + i % TM_K] = (int16_t)(i * 7 % 61 - 30);
        voice_matmul_q(a, 2, 32, t.data, t.zero_point, TM_N, y, TM_N, wbuf);
        for (int r = 0; r < 2; r++)
            for (int c = 0; c < TM_N; c++) {
                int32_t s = 0;
                for (int i = 0; i < TM_K; i++) s += a[r * 32 + i] * (tm->mw[i * TM_N + c] - 3);
                ok &= s == y[r * TM_N + c];
            }
        for (int i = 0; i < 32; i++) ok &= ((const int8_t *)t.data)[7 * 32 + i] == 3;      /* padding is the zero point */
    }
    /* ConvInteger, dilation 2: y[t][co] = sum_j sum_i x[t + 2j][i] (W[co][i][j] - zp). */
    if (!voice_tensor_get(v, "t.c.w", &t) || t.dtype != VOICE_U8 || t.dims[0] != 8 || t.dims[1] != TM_KW ||
        t.dims[2] != 16 || t.scale != 0.25f || t.zero_point != 128)
        return 0;
    {
        int16_t x[9 * 16] = { 0 }, wbuf[VOICE_WBUF(TM_KW, 16)];
        int32_t y[5 * TM_CO];
        for (int r = 0; r < 9; r++)
            for (int i = 0; i < TM_CI; i++) x[r * 16 + i] = (int16_t)((r * 31 + i * 17) % 101 - 50);
        voice_conv_q(x, 5, 1, 2, TM_KW, 16, t.data, t.zero_point, TM_CO, y, TM_CO, wbuf);
        for (int s = 0; s < 5; s++)
            for (int co = 0; co < TM_CO; co++) {
                int32_t sum = 0;
                for (int j = 0; j < TM_KW; j++)
                    for (int i = 0; i < TM_CI; i++) sum += x[(s + 2 * j) * 16 + i] * (tm->cw[(co * TM_CI + i) * TM_KW + j] - 128);
                ok &= sum == y[s * TM_CO + co];
            }
    }
    /* ConvTranspose, stride 2, pads 1: input i, tap j lands on output 2i + j - 1. */
    if (!voice_tensor_get(v, "t.u.w", &t) || t.dtype != VOICE_F32 || t.ndim != 4 || t.dims[0] != 2 ||
        t.dims[1] != 2 || t.dims[2] != TM_UI || t.dims[3] != 8)
        return 0;
    {
        voice_tensor b;
        int T = 4, tout = (T - 1) * 2 - 2 + TM_UK;
        float x[(4 + 3) * TM_UI] = { 0 }, y[8 * TM_UO], ref[8 * TM_UO];
        float *xv = x + TM_UI;                                     /* k/s - 1 = 1 zero row first */
        for (int i = 0; i < T * TM_UI; i++) xv[i] = (float)(i % 5) - 2.0f;
        ok &= voice_tensor_get(v, "t.u.b", &b) && b.dims[0] == TM_UO;
        for (int c = 0; ok && c < TM_UO; c++) ok &= ((const float *)b.data)[c] == tm->ub[c];
        voice_convt(x, T, TM_UI, TM_UK, 2, 1, t.data, b.data, TM_UO, tout, y, TM_UO);
        for (int i = 0; i < tout * TM_UO; i++) ref[i] = tm->ub[i % TM_UO];
        for (int i = 0; i < T; i++)
            for (int j = 0; j < TM_UK; j++) {
                int o = 2 * i + j - 1;
                if (o < 0 || o >= tout) continue;
                for (int co = 0; co < TM_UO; co++)
                    for (int ci = 0; ci < TM_UI; ci++) ref[o * TM_UO + co] += xv[i * TM_UI + ci] * tm->uw[(ci * TM_UO + co) * TM_UK + j];
            }
        for (int i = 0; i < tout * TM_UO; i++) ok &= fabsf(y[i] - ref[i]) <= 1e-5f * (1.0f + fabsf(ref[i]));
    }
    /* The LSTM's, per direction, transposed; and the plain ones. */
    if (!voice_tensor_get(v, "t.r.bwd.w", &t) || t.dims[0] != 4 * TM_H || t.dims[1] != 16 || t.scale != 0.375f ||
        t.zero_point != -2)
        return 0;
    for (int gi = 0; gi < 4 * TM_H; gi++)
        for (int i = 0; i < 16; i++)
            ok &= ((const int8_t *)t.data)[gi * 16 + i] == (i < TM_LI ? tm->lw[(TM_LI + i) * 4 * TM_H + gi] : -2);
    if (!voice_tensor_get(v, "t.r.fwd.w", &t) || t.scale != 0.125f || t.zero_point != 1 ||
        ((const int8_t *)t.data)[3 * 16 + 2] != tm->lw[2 * 4 * TM_H + 3])
        return 0;
    if (!voice_tensor_get(v, "t.r.bwd.r", &t) || t.dims[0] != 4 * TM_H || t.dims[1] != TM_H || t.scale != 0.75f ||
        t.zero_point != 4 || ((const int8_t *)t.data)[5 * TM_H + 7] != tm->lr[(TM_H + 7) * 4 * TM_H + 5])
        return 0;
    if (!voice_tensor_get(v, "t.r.bwd.b", &t) || t.ndim != 1 || t.dims[0] != 8 * TM_H ||
        ((const float *)t.data)[0] != tm->lb[8 * TM_H])
        return 0;
    ok &= voice_tensor_get(v, "t.c.b", &t) && t.dims[0] == TM_CO && ((const float *)t.data)[4] == -5.0f;
    ok &= voice_tensor_get(v, "t.m.b", &t) && ((const float *)t.data)[5] == 6.0f;
    ok &= voice_tensor_get(v, "t.l.g", &t) && ((const float *)t.data)[3] == 5.0f;
    ok &= voice_tensor_get(v, "t.l.eps", &t) && ((const float *)t.data)[0] == 1e-12f;
    ok &= voice_tensor_get(v, "voice.vb", &t) && t.dims[0] == 4 && t.dims[1] == 8 && ((const float *)t.data)[31] == 131.0f;
    ok &= voice_tensor_count(v) == 15;                       /* 13 rows stored, the norm checked, 2 voices */
    return ok;
}

static void voice_pack_checks(const char *dir) {
    char out[512], out2[512], err[400];
    oc_test_path(out, sizeof out, dir, "packed.bin");
    oc_test_path(out2, sizeof out2, dir, "again.bin");
    test_model tm;
    CHECK(test_pack(TM_OK, 1, 0, out, err, sizeof err, &tm) == 0);
    if (err[0]) printf("    %s\n", err);
    voice *v = voice_open(out, err, sizeof err);
    CHECK(v != NULL);
    if (v) CHECK(converted_right(v, &tm));
    voice_close(v);
    /* The same inputs, the same bytes; the inputs' hashes in the header. */
    CHECK(test_pack(TM_OK, 1, 0, out2, err, sizeof err, &tm) == 0);
    size_t n1 = 0, n2 = 0;
    unsigned char *a = slurp(out, &n1), *b = slurp(out2, &n2);
    CHECK(a && b && n1 == n2 && memcmp(a, b, n1) == 0);
    if (a) {
        pbw m;
        unsigned char sha[32];
        build_test_model(&m, &tm, TM_OK);
        mbedtls_sha256(m.p, m.n, sha, 0);
        CHECK(memcmp(a + 40, sha, 32) == 0);
        free(m.p);
    }
    free(a);
    free(b);
    unlink(out2);
    /* Each difference refused, naming what differs; nothing is left behind. */
    CHECK(test_pack(TM_NO_CONV, 1, 0, out2, err, sizeof err, &tm) != 0 && strstr(err, "t.c.w"));
    CHECK(test_pack(TM_WIDER_MATMUL, 1, 0, out2, err, sizeof err, &tm) != 0 && strstr(err, "t.m.w"));
    CHECK(test_pack(TM_DILATION, 1, 0, out2, err, sizeof err, &tm) != 0 && strstr(err, "t.c.w"));
    CHECK(test_pack(TM_STRAY, 1, 0, out2, err, sizeof err, &tm) != 0 && strstr(err, "stray"));
    CHECK(test_pack(TM_SCALED_NORM, 1, 0, out2, err, sizeof err, &tm) != 0 && strstr(err, "/n/InstanceNormalization"));
    CHECK(test_pack(TM_OK, 0, 0, out2, err, sizeof err, &tm) != 0 && strstr(err, "vb"));
    CHECK(test_pack(TM_OK, 1, 1, out2, err, sizeof err, &tm) != 0 && strstr(err, "vc.npy"));
    CHECK(access(out2, F_OK) != 0);
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.tmp", out2);
    CHECK(access(tmp, F_OK) != 0);
    err[0] = '\0';
    CHECK(oc_voice_pack_mem(&TEST_SPEC, (const unsigned char *)"not a model", 11, (const unsigned char *)"", 0, out2,
                            NULL, err, sizeof err) != 0 && err[0]);
    /* The command's exit status for a model that is not there. */
    char missing[512];
    CHECK(oc_voice_pack_cli(oc_test_path(missing, sizeof missing, dir, "absent.onnx"), out, out2) == 1);
    unlink(out);
}

/* The committed ttskit/data/voice.bin, the file read-aloud ships: it loads, holds
 * every voice the daemon names, and a copy damaged in its header or its index is
 * refused. The copy is the file mapped privately, so each change costs a page,
 * not the file. */
static void committed_voice_checks(void) {
    char path[1024], err[256] = "";
    oc_test_tree_path("ttskit/data/voice.bin", path, sizeof path);
    voice *v = voice_open(path, err, sizeof err);
    CHECK(v != NULL);
    if (!v) { printf("    %s\n", err); return; }
    float row[VOICE_STYLE_DIM];
    for (int i = 0; i < TTS_VOICE_COUNT; i++) CHECK(voice_style(v, tts_voice_name(i), 0, row) == 0);
    CHECK(voice_style(v, "no-such-voice", 0, row) != 0);
    CHECK(voice_tensor_count(v) > 0);
    voice_close(v);

    int fd = open(path, O_RDONLY);
    struct stat st;
    CHECK(fd >= 0 && fstat(fd, &st) == 0 && st.st_size > (off_t)VOICE_HEADER);
    if (fd < 0) return;
    size_t n = (size_t)st.st_size;
    uint8_t *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    close(fd);
    CHECK(p != MAP_FAILED);
    if (p == MAP_FAILED) return;
    v = voice_open_mem(p, n, err, sizeof err);                                     /* the copy, whole */
    CHECK(v != NULL);
    voice_close(v);
    p[0] ^= 0xFF;                                                                  /* magic */
    CHECK(voice_open_mem(p, n, err, sizeof err) == NULL);
    p[0] ^= 0xFF;
    p[8] ^= 0x01;                                                                  /* version */
    CHECK(voice_open_mem(p, n, err, sizeof err) == NULL);
    p[8] ^= 0x01;
    CHECK(voice_open_mem(p, n - 1, err, sizeof err) == NULL);                      /* cut short */
    p[VOICE_HEADER + 72] ^= 0x01;                                                  /* the first tensor's offset */
    CHECK(voice_open_mem(p, n, err, sizeof err) == NULL);
    p[VOICE_HEADER + 72] ^= 0x01;
    v = voice_open_mem(p, n, err, sizeof err);                                     /* whole again */
    CHECK(v != NULL);
    voice_close(v);
    munmap(p, n);
}

static void voice_file_tests(void) {
    char dir[256];
    CHECK(oc_test_mkdtemp("voice", dir, sizeof dir) == 0);
    voice_bin_checks(dir);
    voice_pack_checks(dir);
    committed_voice_checks();
    /* The table read-aloud ships: names unique and short enough for the index. */
    int names_ok = 1;
    for (size_t i = 0; i < OC_VOICE_SPEC.nrows; i++) {
        const char *ni = OC_VOICE_SPEC.rows[i].name;
        if (!ni) continue;
        names_ok &= strlen(ni) < VOICE_NAME_MAX && strncmp(ni, "voice.", 6) != 0;
        for (size_t j = i + 1; j < OC_VOICE_SPEC.nrows; j++)
            if (OC_VOICE_SPEC.rows[j].name) names_ok &= strcmp(ni, OC_VOICE_SPEC.rows[j].name) != 0;
    }
    CHECK(names_ok && OC_VOICE_SPEC.nvoices == 8 && OC_VOICE_SPEC.style_rows == 400 && OC_VOICE_SPEC.style_dim == 256);
    oc_test_rmtree(dir);
}

int run_ttskit_tests(void) {
    printf("test_ttskit: IPA table, normalization, lexicon, guesser, spelling, sentences, pins, agreement, damaged data, voice kernels, voice.bin (the committed one too) and its converter\n");
    oc_test_tree_path("ttskit/data/en-US", g_data, sizeof g_data);

    /* The ARPAbet->IPA table and its context rules (TTSKIT.md §3). */
    CHECK(ipa_is("M AY0 G R EY1 SH AH0 N", 0, "maɪɡɹˈeɪʃən"));
    CHECK(ipa_is("M AY0 G R EY1 SH AH0 N", 1, "m aɪ ɡ ɹ ˈeɪ ʃ ə n"));
    CHECK(ipa_is("W AO1 T ER0", 0, "wˈɔːɾɚ"));              /* flap between vowels; unstressed ER */
    CHECK(ipa_is("B AO1 L", 0, "bˈɔl"));                   /* AO before L is short */
    CHECK(ipa_is("HH AE1 P IY0", 0, "hˈæpi"));
    CHECK(ipa_is("S IH1 T IY2", 0, "sˈɪɾi"));              /* flap before a final IY2 */
    CHECK(ipa_is("R IY0 T ER1 N", 0, "ɹitˈɜːn"));
    CHECK(ipa_is("W AA1 N T IH0 D", 0, "wˈɑːntᵻd"));       /* -ed reduced vowel */
    CHECK(ipa_is("K AA1 N T ER0 AE2 K T", 0, "kˈɑːntɚɹækt")); /* linking r; no secondary stress */
    CHECK(ipa_is("M IY0 OW1", 0, "mɪˈoʊ"));
    CHECK(ipa_is("Q Q1", 0, ""));                          /* unknown symbol */

    /* IPA to the voice model's token ids, against ids from its own Python tokenizer:
     * punctuation split off with spaces, the end marker and pads around. */
    {
        static const long long hello[] = { 0, 50, 83, 54, 156, 57, 135, 16, 3, 16, 65, 156, 87, 158, 54, 46, 16, 5, 16,
                                           50, 156, 43, 135, 16, 69, 158, 123, 16, 52, 63, 158, 16, 6, 10, 0 };
        static const long long clock[] = { 0, 156, 102, 62, 61, 16, 48, 156, 43, 102, 64, 16, 83, 53, 54, 156, 69, 158, 53, 10, 0 };
        long long ids[128];
        size_t n = tts_voice_tokens("həlˈoʊ, wˈɜːld! hˈaʊ ɑːɹ juː?", ids, 128);
        CHECK(n == sizeof hello / sizeof hello[0] && memcmp(ids, hello, sizeof hello) == 0);
        n = tts_voice_tokens("ˈɪts   fˈaɪv əklˈɑːk", ids, 128);
        CHECK(n == sizeof clock / sizeof clock[0] && memcmp(ids, clock, sizeof clock) == 0);
        n = tts_voice_tokens("", ids, 128);
        CHECK(n == 3 && ids[0] == 0 && ids[1] == 10 && ids[2] == 0);
        n = tts_voice_tokens("həlˈoʊ wˈɜːld", ids, 8);                      /* cut, still closed */
        CHECK(n == 8 && ids[6] == 10 && ids[7] == 0);
    }

    /* Numbers, times, money, ordinals, years, abbreviations (TTSKIT.md §4). */
    CHECK(norm_is("Meet at 3:15 on the 2nd, $40", "Meet at three fifteen on the second, forty dollars"));
    CHECK(norm_is("$3.50 and $1", "three dollars and fifty cents and one dollar"));
    CHECK(norm_is("21st 100th 12th 40th", "twenty first one hundredth twelfth fortieth"));
    CHECK(norm_is("in 2026 and 2005 and 1900", "in twenty twenty six and two thousand five and nineteen hundred"));
    CHECK(norm_is("3.5 and 1,200,000", "three point five and one million two hundred thousand"));
    CHECK(norm_is("at 3:00 or 3:00pm or 3:05 am", "at three o'clock or three p m or three oh five a m"));
    CHECK(norm_is("50% off", "fifty percent off"));
    CHECK(norm_is("Dr. Who & co, e.g. this", "doctor Who and co, for example this"));
    CHECK(norm_is("10x", "ten x"));
    CHECK(norm_is("1234567890123456789012", "one two three four five six seven eight nine zero one two three four five six seven eight nine zero one two"));
    CHECK(norm_is("", ""));

    char err[256] = "";
    tts *t = tts_load(DATA, LANG, err, sizeof err);
    CHECK(t != NULL);
    if (!t) { printf("    %s\n", err); return failures; }

    /* Dictionary, guesser, spelling. */
    CHECK(word_is(t, "migration", 1, "maɪɡɹˈeɪʃən"));
    CHECK(word_is(t, "Hello", 1, "həlˈoʊ"));
    CHECK(word_is(t, "don't", 1, "dˈoʊnt"));
    CHECK(word_is(t, "NASA", 1, "nˈæsə"));                 /* capitals in the dictionary are a word */
    CHECK(word_is(t, "openchime", 2, "ˈoʊpəntʃaɪm"));
    CHECK(word_is(t, "Kubernetes", 2, "kjˈuːbɚnˈɛtiːz"));
    CHECK(word_is(t, "API", 3, "ˈeɪ pˈiː ˈaɪ"));            /* short capitals are always spelled */
    CHECK(word_is(t, "HTTPS", 3, "ˈeɪtʃ tˈiː tˈiː pˈiː ˈɛs"));
    CHECK(word_is(t, "", 0, ""));
    CHECK(word_is(t, "42", 0, ""));

    /* Sentences: function words weak, initialisms spelled, pauses kept. */
    CHECK(text_is(t, "I can see US troops.", "aɪ kæn sˈiː jˈuː ˈɛs tɹˈuːps."));
    CHECK(text_is(t, "Hello, world!  How are you?", "həlˈoʊ, wˈɜːld! hˈaʊ ɑːɹ juː?"));
    CHECK(text_is(t, "It's 5 o'clock", "ˈɪts fˈaɪv əklˈɑːk"));
    CHECK(text_is(t, "\xF0\x9F\x98\x80 --- !!!", ""));
    tts_free(t);

    /* Without data, words are still said: spelled. */
    t = tts_load("/nonexistent-ttskit-data", LANG, err, sizeof err);
    CHECK(t != NULL);
    CHECK(word_is(t, "hello", 3, "ˈeɪtʃ ˈiː ˈɛl ˈɛl ˈoʊ"));
    tts_free(t);

    /* The committed data is the data that was measured. */
    char lp0[1100], gp0[1100];
    CHECK(sha256_is(data_file("lexicon.bin", lp0, sizeof lp0), LEXICON_SHA256));
    CHECK(sha256_is(data_file("guesses.bin", gp0, sizeof gp0), GUESSES_SHA256));

    /* The C decoder finds Phonetisaurus's best path. One word in the reference
     * ("doman") has two paths within rounding of each other; allow one in 1,000. */
    int total, same = guess_agreement(&total);
    CHECK(total == 3010);
    CHECK(total > 0 && same * 1000 >= total * 999);
    if (same != total) printf("    guesser agrees on %d of %d words\n", same, total);

    /* Damaged files are refused, each in a directory of its own. */
    char dir[256];
    CHECK(oc_test_mkdtemp("ttskit", dir, sizeof dir) == 0);
    size_t ln = 0, gn = 0;
    unsigned char *lex = slurp(data_file("lexicon.bin", lp0, sizeof lp0), &ln), *gs = slurp(data_file("guesses.bin", gp0, sizeof gp0), &gn);
    CHECK(lex && gs && ln > 64 && gn > 64);
    if (lex && gs && ln > 64 && gn > 64) {
        CHECK(refused(dir, "lexicon.bin", lex, ln / 2, LANG, TTS_REFUSED_DAMAGED));                 /* truncated */
        CHECK(refused(dir, "guesses.bin", gs, gn / 2, LANG, TTS_REFUSED_DAMAGED));
        CHECK(refused(dir, "lexicon.bin", (const unsigned char *)"short", 5, LANG, TTS_REFUSED_DAMAGED));
        memcpy(lex, "OCTTSGS1", 8);                                             /* wrong magic */
        CHECK(refused(dir, "lexicon.bin", lex, ln, LANG, TTS_REFUSED_DAMAGED));
        memcpy(lex, TTS_LEX_MAGIC, 8);
        lex[8] = TTS_LEX_VERSION + 1;                                           /* a later version */
        CHECK(refused(dir, "lexicon.bin", lex, ln, LANG, TTS_REFUSED_DAMAGED));
        lex[8] = TTS_LEX_VERSION;
        gs[8] = TTS_GS_VERSION + 1;
        CHECK(refused(dir, "guesses.bin", gs, gn, LANG, TTS_REFUSED_DAMAGED));
        gs[8] = TTS_GS_VERSION;

        /* A pair that is not the language asked for, and a pair that disagrees
         * with itself, are both refused -- the whole point of the tag. Neither
         * fails as it is used: a lexicon of one language with a guesser of
         * another pronounces fluent nonsense and nothing downstream can tell. */
        memcpy(lex + 32, "de-DE", 6);
        CHECK(refused(dir, "lexicon.bin", lex, ln, LANG, TTS_REFUSED_LANG));          /* wrong language */
        {
            /* A pair that disagrees with ITSELF, which needs both files present:
             * refused() places one at a time, and one file alone has nothing to
             * disagree with. Asked for no language in particular, so the only
             * thing refusing this is the two tags differing. */
            char lp[512], gp[512], err2[256] = "";
            snprintf(lp, sizeof lp, "%s/lexicon.bin", dir);
            snprintf(gp, sizeof gp, "%s/guesses.bin", dir);
            put(lp, lex, ln);                                        /* de-DE */
            put(gp, gs, gn);                                         /* en-US */
            tts_status st = TTS_LOADED;
            tts *mixed = tts_load_ex(dir, NULL, &st, err2, sizeof err2);
            CHECK(mixed == NULL && st == TTS_REFUSED_LANG && err2[0]);
            tts_free(mixed);
            unlink(lp);
            unlink(gp);
        }
        memcpy(lex + 32, LANG, strlen(LANG) + 1);
        memcpy(gs + 64, "de-DE", 6);
        CHECK(refused(dir, "guesses.bin", gs, gn, LANG, TTS_REFUSED_LANG));
        memcpy(gs + 64, LANG, strlen(LANG) + 1);
        /* An unterminated tag is corruption, not a very long name. */
        memset(lex + 32, 'x', 16);
        CHECK(refused(dir, "lexicon.bin", lex, ln, NULL, TTS_REFUSED_DAMAGED));
        memcpy(lex + 32, LANG, strlen(LANG) + 1);
        memset(lex + 32 + strlen(LANG) + 1, 0, 16 - strlen(LANG) - 1);
    }
    /* The same data lent from memory says the same thing; damaged bytes are refused. */
    if (lex && gs) {
        memcpy(lex, TTS_LEX_MAGIC, 8);
        lex[8] = TTS_LEX_VERSION;
        gs[8] = TTS_GS_VERSION;
        t = tts_load_mem(lex, ln, gs, gn, LANG, err, sizeof err);
        CHECK(t != NULL);
        CHECK(t && strcmp(tts_lang(t), LANG) == 0);
        CHECK(word_is(t, "migration", 1, "maɪɡɹˈeɪʃən"));
        CHECK(word_is(t, "Kubernetes", 2, "kjˈuːbɚnˈɛtiːz"));
        tts_free(t);
        err[0] = '\0';
        CHECK(tts_load_mem(lex, ln / 2, gs, gn, LANG, err, sizeof err) == NULL && err[0]);
        CHECK(tts_load_mem(lex, ln, NULL, 0, LANG, err, sizeof err) == NULL);
        err[0] = '\0';
        CHECK(tts_load_mem(lex, ln, gs, gn, "de-DE", err, sizeof err) == NULL && err[0]);
    }
    free(lex);
    free(gs);
    oc_test_rmtree(dir);

    voice_kernel_tests();
    voice_file_tests();
    return failures;
}
