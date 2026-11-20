/* Tests for sttkit, voice input's recognizer (sttkit/, ARCH-112, STTKIT.md §8):
 * the tokenizer and the tokens-per-second ceiling; recognizer.bin -- a file
 * built here from a table of every tensor the forward pass reads round-trips
 * through the loader, and one with a wrong magic, version or length, or a
 * tensor missing or of another shape, is refused; the committed
 * sttkit/data/recognizer.bin loads -- and the converter refusing what is not
 * the published model (daemon/recognizer_pack.c).
 *
 * And, as the FULL suite recognizer_parity (run_recognizer_parity_tests), the
 * recognizer against its references (tests/data/recognizer, made by
 * tests/manual/recognizer_ref.c with ONNX Runtime): for every fixture the same
 * token ids, and the encoder's output and the first decoding step's logits
 * within 30 dB SNR of the reference's. It reads the committed
 * sttkit/data/recognizer.bin, and fails, saying so, where that is missing. */
#include "check.h"
#include "testenv.h"
#include "sttkit.h"
#include "stt_priv.h"
#include "recognizer_pack.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the tokenizer ---- */

static size_t put_token(uint8_t *out, const char *s) {
    size_t n = strlen(s), w = 0;
    if (n < 128) out[w++] = (uint8_t)n;
    else { out[w++] = (uint8_t)(n % 128 + 128); out[w++] = (uint8_t)(n / 128); }
    memcpy(out + w, s, n);
    return w + n;
}

static void test_tokens(void) {
    int failures_before = failures;
    uint8_t table[1024];
    size_t n = 0;
    n += put_token(table + n, "<unk>");                 /* 0 */
    n += put_token(table + n, "<s>");                   /* 1 */
    n += put_token(table + n, "</s>");                  /* 2 */
    n += put_token(table + n, "\xE2\x96\x81Send");      /* 3 */
    n += put_token(table + n, "\xE2\x96\x81the");       /* 4 */
    n += put_token(table + n, "\xE2\x96\x81rep");       /* 5 */
    n += put_token(table + n, "ort.");                  /* 6 */
    table[n++] = 0;                                     /* 7: unused id */
    n += put_token(table + n, "\xE2\x96\x81" "caf\xC3\xA9"); /* 8 */
    char long_tok[200];
    memset(long_tok, 'x', sizeof long_tok - 1);
    long_tok[sizeof long_tok - 1] = 0;
    n += put_token(table + n, long_tok);                /* 9: a two-byte length */

    stt_tokens t;
    CHECK(stt_tokens_load(&t, table, n) == 0);
    CHECK(t.count == 10 && t.len[7] == 0 && t.len[9] == 199);

    char out[512];
    int64_t ids[] = { 1, 3, 4, 5, 6, 2 };
    CHECK(stt_detok(&t, ids, 6, out, sizeof out) == (long)strlen("Send the report."));   /* contract: the decoded length */
    CHECK(strcmp(out, "Send the report.") == 0);        /* contract: the text as decoded; specials skipped, marker is a space, trimmed */
    int64_t odd[] = { 3, 7, 999, -1, 8 };
    stt_detok(&t, odd, 5, out, sizeof out);
    CHECK(strcmp(out, "Send caf\xC3\xA9") == 0);        /* contract: the text as decoded; unused and out-of-range ids skipped */
    /* A cap that splits a character never leaves half of it. */
    stt_detok(&t, odd, 5, out, 10);
    CHECK(strcmp(out, "Send caf") == 0);   /* contract: the text as decoded, exact */

    /* A truncated table is refused, not read past. */
    stt_tokens bad;
    CHECK(stt_tokens_load(&bad, table, 5) != 0);
    stt_tokens_free(&t);

    /* The ceiling: 6.5 tokens a second of audio, at most 256. */
    CHECK(stt_max_tokens(16000) == 7);
    CHECK(stt_max_tokens(8000) == 4);
    CHECK(stt_max_tokens(16000 * 60) == 256);
    if (failures == failures_before) printf("  tokenizer ok\n");
}

/* ---- recognizer.bin ---- */

static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void put64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }

typedef struct { char name[REC_NAME_MAX]; int dtype, a, b; } spec;

static size_t add_spec(spec *s, size_t n, int dtype, int a, int b, const char *name) {
    snprintf(s[n].name, sizeof s[n].name, "%s", name);
    s[n].dtype = dtype;
    s[n].a = a;
    s[n].b = b;
    return n + 1;
}

static size_t add_q(spec *s, size_t n, const char *base, int out, int in, int bias) {
    char nm[REC_NAME_MAX];
    snprintf(nm, sizeof nm, "%s.w", base); n = add_spec(s, n, REC_U8, out, in, nm);
    snprintf(nm, sizeof nm, "%s.s", base); n = add_spec(s, n, REC_F32, out, 0, nm);
    snprintf(nm, sizeof nm, "%s.zp", base); n = add_spec(s, n, REC_U8, out, 0, nm);
    if (bias) { snprintf(nm, sizeof nm, "%s.b", base); n = add_spec(s, n, REC_F32, out, 0, nm); }
    return n;
}

static int spec_cmp(const void *a, const void *b) { return strcmp(((const spec *)a)->name, ((const spec *)b)->name); }

/* Every tensor the forward pass reads, written from the format's description,
 * not from the converter. */
static size_t model_spec(spec *s) {
    size_t n = 0;
    char nm[REC_NAME_MAX];
    n = add_spec(s, n, REC_F32, 80, 320, "fe.lin.w");
    n = add_spec(s, n, REC_F32, 5 * 320, 640, "fe.c1.w");
    n = add_spec(s, n, REC_F32, 640, 0, "fe.c1.b");
    n = add_spec(s, n, REC_F32, 5 * 640, 320, "fe.c2.w");
    n = add_spec(s, n, REC_F32, 320, 0, "fe.c2.b");
    n = add_spec(s, n, REC_F32, 1, 0, "fe.comp");
    n = add_spec(s, n, REC_F32, 1, 0, "fe.eps");
    n = add_spec(s, n, REC_F32, 1, 0, "eps");
    n = add_spec(s, n, REC_F32, 1, 0, "alpha");
    n = add_spec(s, n, REC_I32, REC_CFG_COUNT, 0, "cfg");
    n = add_spec(s, n, REC_U8, 32768 * 2, 0, "tokens");
    n = add_spec(s, n, REC_U8, 4096, 320, "pos");
    n = add_spec(s, n, REC_U8, 32768, 320, "emb");
    n = add_spec(s, n, REC_F32, 320, 0, "enc.g");
    n = add_spec(s, n, REC_F32, 320, 0, "dec.g");
    n = add_spec(s, n, REC_F32, 16, 0, "dec.rope");
    for (int l = 0; l < 6; l++) {
        static const char *const ep[6] = { "q", "k", "v", "o", "f1", "f2" };
        for (int j = 0; j < 6; j++) {
            snprintf(nm, sizeof nm, "enc.%d.%s", l, ep[j]);
            n = add_q(s, n, nm, j == 4 ? 1280 : 320, j == 5 ? 1280 : 320, j >= 4);
        }
        static const char *const dp[10] = { "q", "k", "v", "o", "cq", "co", "xk", "xv", "f1", "f2" };
        for (int j = 0; j < 10; j++) {
            snprintf(nm, sizeof nm, "dec.%d.%s", l, dp[j]);
            n = add_q(s, n, nm, j == 8 ? 2560 : 320, j == 9 ? 1280 : 320, j >= 8);
        }
        for (int j = 1; j <= 2; j++) { snprintf(nm, sizeof nm, "enc.%d.g%d", l, j); n = add_spec(s, n, REC_F32, 320, 0, nm); }
        for (int j = 1; j <= 3; j++) { snprintf(nm, sizeof nm, "dec.%d.g%d", l, j); n = add_spec(s, n, REC_F32, 320, 0, nm); }
    }
    qsort(s, n, sizeof *s, spec_cmp);
    return n;
}

/* The file for `s`, every tensor's bytes a pattern of its index, the tokenizer
 * 32,768 one-byte tokens and the dimensions the model's; 64-byte aligned. */
static uint8_t *build(const spec *s, size_t ns, size_t *len, void **base) {
    uint64_t data = (REC_HEADER + ns * REC_ENTRY + 63) & ~(uint64_t)63, off = data;
    uint64_t *offs = malloc(ns * sizeof *offs), *sizes = malloc(ns * sizeof *sizes);
    for (size_t i = 0; i < ns; i++) {
        uint64_t count = (uint64_t)s[i].a * (uint64_t)(s[i].b ? s[i].b : 1);
        sizes[i] = count * (s[i].dtype == REC_U8 ? 1 : 4);
        offs[i] = off;
        off = (off + sizes[i] + 63) & ~(uint64_t)63;
    }
    size_t size = (size_t)(offs[ns - 1] + sizes[ns - 1]);
    *base = malloc(size + 64);
    uint8_t *p = (uint8_t *)(((uintptr_t)*base + 63) & ~(uintptr_t)63);
    memset(p, 0, size);
    memcpy(p, REC_MAGIC, 8);
    put32(p + 8, REC_VERSION);
    put32(p + 12, (uint32_t)ns);
    put32(p + 16, REC_HEADER);
    put32(p + 20, REC_ENTRY);
    put64(p + 24, data);
    put64(p + 32, size);
    for (size_t i = 0; i < ns; i++) {
        uint8_t *e = p + REC_HEADER + i * REC_ENTRY;
        memcpy(e, s[i].name, strlen(s[i].name));
        put32(e + 48, (uint32_t)s[i].dtype);
        put32(e + 52, s[i].b ? 2u : 1u);
        put32(e + 56, (uint32_t)s[i].a);
        put32(e + 60, (uint32_t)s[i].b);
        put64(e + 72, offs[i]);
        put64(e + 80, sizes[i]);
        uint8_t *d = p + offs[i];
        if (!strcmp(s[i].name, "tokens")) {
            for (size_t k = 0; k < 32768; k++) { d[2 * k] = 1; d[2 * k + 1] = (uint8_t)('a' + k % 26); }
        } else if (!strcmp(s[i].name, "cfg")) {
            static const int32_t cfg[REC_CFG_COUNT] = { 320, 6, 8, 40, 32768, 1, 2, 80, 640, 5, 2, 1280, 1280, 32, 4096,
                                                        16, 4, 16, 4, 16, 0, 16, 0, 16, 4, 16, 4 };
            for (int k = 0; k < REC_CFG_COUNT; k++) put32(d + 4 * k, (uint32_t)cfg[k]);
        } else {
            for (uint64_t k = 0; k < sizes[i]; k++) d[k] = (uint8_t)(i * 7 + k);
        }
    }
    free(offs);
    free(sizes);
    *len = size;
    return p;
}

static int refused(const uint8_t *p, size_t n, const char *want) {
    char err[300] = "";
    recognizer *r = recognizer_open_mem(p, n, err, sizeof err);
    if (r) { recognizer_close(r); return 0; }
    if (want && !strstr(err, want)) printf("    refused for \"%s\", wanted \"%s\"\n", err, want);
    return !want || strstr(err, want) != NULL;
}

static void test_format(void) {
    int failures_before = failures;
    spec *s = malloc(512 * sizeof *s);
    size_t ns = model_spec(s), len;
    void *base;
    uint8_t *p = build(s, ns, &len, &base);
    char err[300] = "";
    recognizer *r = recognizer_open_mem(p, len, err, sizeof err);
    CHECK(r != NULL);
    if (!r) printf("    %s\n", err);
    if (r) {
        CHECK(rec_tensor_count(r) == ns);   /* contract: every tensor indexed */
        int same = 1;
        for (size_t i = 0; i < ns; i++) {
            rec_tensor t;
            if (!rec_tensor_get(r, s[i].name, &t) || t.dtype != s[i].dtype || t.dims[0] != s[i].a || t.dims[1] != s[i].b ||
                (uintptr_t)t.data % REC_ALIGN) { same = 0; break; }
            if (s[i].dtype == REC_U8 && strcmp(s[i].name, "tokens") && ((const uint8_t *)t.data)[1] != (uint8_t)(i * 7 + 1)) same = 0;
        }
        CHECK(same);                         /* contract: each tensor's name, shape, place and bytes as written */
        CHECK(r->cfg[REC_CFG_WINDOW + 4] == 16 && r->enc[2].ahead == 0 && r->enc[0].ahead == 4);
        CHECK(r->tok.count == 32768);
        recognizer_close(r);
    }
    /* Damaged: a wrong magic, version, length, alignment; a tensor missing or
     * of another shape. Each is refused, saying why. */
    uint8_t *q = malloc(len + 64);
    uint8_t *qa = (uint8_t *)(((uintptr_t)q + 63) & ~(uintptr_t)63);
    memcpy(qa, p, len); qa[0] ^= 1;
    CHECK(refused(qa, len, "not a version"));   /* contract: the reason the loader gives */
    memcpy(qa, p, len); put32(qa + 8, REC_VERSION + 1);
    CHECK(refused(qa, len, "not a version"));   /* contract: the reason the loader gives */
    memcpy(qa, p, len);
    CHECK(refused(qa, len - 1, "truncated or corrupt"));   /* contract: the reason the loader gives */
    CHECK(refused(qa, 100, "not a version"));   /* contract: the reason the loader gives */
    memcpy(qa, p, len);
    {   /* emb (index of "emb") one row short */
        for (size_t i = 0; i < ns; i++)
            if (!strcmp(s[i].name, "emb")) put32(qa + REC_HEADER + i * REC_ENTRY + 56, 32767);
        CHECK(refused(qa, len, "a size that disagrees"));   /* contract: the reason the loader gives */
    }
    memcpy(qa, p, len);
    {   /* a tensor renamed: the forward pass's one is missing */
        for (size_t i = 0; i < ns; i++)
            if (!strcmp(s[i].name, "dec.rope")) qa[REC_HEADER + i * REC_ENTRY + 4] = 'q';   /* "dec.qope", still in order */
        CHECK(refused(qa, len, "has no dec.rope"));   /* contract: the reason the loader gives */
    }
    free(q);
    free(base);
    free(s);

    /* The committed file. */
    char path[1024];
    oc_test_tree_path("sttkit/data/recognizer.bin", path, sizeof path);
    r = recognizer_open(path, err, sizeof err);
    CHECK(r != NULL);   /* contract: the committed data loads */
    if (!r) printf("    %s\n", err);
    recognizer_close(r);
    if (failures == failures_before) printf("  recognizer.bin ok\n");
}

/* The converter refuses what is not the published model, naming why. */
static void test_pack_refuses(void) {
    int failures_before = failures;
    static const unsigned char junk[64] = "not a model";
    oc_rp_blob in[RP_FILES];
    for (int i = 0; i < RP_FILES; i++) { in[i].data = junk; in[i].len = sizeof junk; }
    char dir[512], out[600], err[600] = "";
    CHECK(oc_test_mkdtemp("sttkit", dir, sizeof dir) == 0);
    snprintf(out, sizeof out, "%s/recognizer.bin", dir);
    CHECK(oc_recognizer_pack_mem(in, out, NULL, err, sizeof err) != 0);
    CHECK(strstr(err, "frontend.model.ort: not an ONNX Runtime model file") != NULL);   /* contract: the refusal names the file and why */
    /* An .ort header whose root points outside the buffer. */
    unsigned char bad[64] = { 0xF0, 0xFF, 0, 0, 'O', 'R', 'T', 'M' };
    in[RP_FRONTEND].data = bad;
    in[RP_FRONTEND].len = sizeof bad;
    err[0] = 0;
    CHECK(oc_recognizer_pack_mem(in, out, NULL, err, sizeof err) != 0);
    CHECK(strstr(err, "damaged") != NULL);   /* contract: a damaged file is called that, not read past */
    FILE *f = fopen(out, "rb");
    CHECK(f == NULL);   /* contract: nothing written */
    if (f) fclose(f);
    oc_test_rmtree(dir);
    if (failures == failures_before) printf("  converter refusals ok\n");
}

int run_sttkit_tests(void) {
    printf("test_sttkit: tokenizer, ceiling, recognizer.bin (the committed one too), converter refusals\n");
    test_tokens();
    test_format();
    test_pack_refuses();
    return failures;
}

/* ---- recognizer_parity (FULL) ---- */

static double snr(const float *got, const float *want, size_t n) {
    double sig = 0, err = 0;
    for (size_t i = 0; i < n; i++) {
        double d = (double)got[i] - want[i];
        sig += (double)want[i] * want[i];
        err += d * d;
    }
    return err == 0 ? INFINITY : 10 * log10(sig / err);
}

static void *slurp_fx(const char *dir, const char *name, const char *ext, size_t *n) {
    char path[1200];
    snprintf(path, sizeof path, "%s/%s%s", dir, name, ext);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *p = malloc(len > 0 ? (size_t)len : 1);
    if (p && len > 0 && fread(p, 1, (size_t)len, f) != (size_t)len) { free(p); p = NULL; }
    fclose(f);
    *n = p ? (size_t)len : 0;
    return p;
}

int run_recognizer_parity_tests(void) {
    printf("test_recognizer_parity: every fixture's tokens, encoder output and first logits against ONNX Runtime's\n");
    char data[1024], fixtures[1024], path[1200], err[512] = "";
    oc_test_tree_path("sttkit/data/recognizer.bin", data, sizeof data);
    oc_test_tree_path("tests/data/recognizer", fixtures, sizeof fixtures);
    recognizer *r = recognizer_open(data, err, sizeof err);
    CHECK(r != NULL);   /* contract: the committed data loads */
    if (!r) { printf("    %s\n", err); return failures; }
    snprintf(path, sizeof path, "%s/manifest", fixtures);
    FILE *m = fopen(path, "r");
    CHECK(m != NULL);
    char line[8192];
    int count = 0;
    while (m && fgets(line, sizeof line, m)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char name[64], ids[4096];
        size_t samples, ntok;
        int frames;
        if (sscanf(line, "%63s %zu %d %zu %4095s", name, &samples, &frames, &ntok, ids) != 5) { CHECK(0); break; }
        int64_t want[300], got[300];
        size_t k = 0, n16 = 0, ne = 0, nl = 0, nids = 0;
        if (strcmp(ids, "-")) for (char *t = strtok(ids, ","); t && k < 300; t = strtok(NULL, ",")) want[k++] = atoll(t);
        int16_t *s16 = slurp_fx(fixtures, name, ".s16", &n16);
        float *eref = slurp_fx(fixtures, name, ".enc.f32", &ne), *lref = slurp_fx(fixtures, name, ".logits.f32", &nl);
        float *pcm = malloc((n16 / 2 + 1) * sizeof *pcm), *enc = NULL, *logits = malloc(32768 * sizeof *logits);
        for (size_t i = 0; s16 && i < n16 / 2; i++) pcm[i] = (float)s16[i] / 32768.0f;
        int fr = 0;
        int ok = s16 && eref && lref && n16 / 2 == samples && k == ntok &&
                 rec_run(r, pcm, samples, got, stt_max_tokens(samples), &nids, &enc, &fr, logits, err, sizeof err) == 0;
        CHECK(ok);
        if (ok) {
            CHECK(fr == frames && ne == (size_t)frames * 320 * sizeof(float));   /* contract: the reference's frame count */
            CHECK(nids == ntok && (!nids || !memcmp(got, want, nids * sizeof *got)));   /* contract: the same tokens */
            double se = fr == frames && ne == (size_t)frames * 320 * sizeof(float) ? snr(enc, eref, (size_t)frames * 320) : 0;
            double sl = nl == 32768 * sizeof(float) ? snr(logits, lref, 32768) : 0;
            CHECK(se >= 30.0 && sl >= 30.0);   /* contract: within 30 dB of the reference */
            printf("  %-11s %3zu tokens, encoder %5.1f dB, logits %5.1f dB\n", name, nids, se, sl);
        }
        count++;
        free(s16); free(eref); free(lref); free(pcm); free(enc); free(logits);
    }
    if (m) fclose(m);
    CHECK(count == 7);   /* contract: every fixture the manifest lists */
    recognizer_close(r);
    return failures;
}
