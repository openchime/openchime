/* sum_check: sumkit against its references, its quantisation measured, its
 * speed timed (docs/SUMKIT.md §6). Not part of the daemon.
 *
 *   sum_check parity DIR FULL.bin       the unquantised model against the
 *                                       references in DIR (tests/data/summary)
 *   sum_check kl DIR SUMMARY.bin FULL.bin   the quantised model's KL divergence
 *                                       from the unquantised over DIR's
 *                                       held-out text, against Unsloth's
 *   sum_check bench DIR SUMMARY.bin     prompt and answer speed, peak memory
 *   sum_check tokenizer SUMMARY.bin OUT the tokenizer's tables alone, for the
 *                                       fast suite (tests/data/summary/tokenizer.bin)
 *
 * Exit 0 when what it checks holds, 1 when not, 2 on a usage or input error. */
#define _POSIX_C_SOURCE 200809L
#include "sk_priv.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>

#define PARITY_SNR_DB  30.0
#define PARITY_MARGIN  0.1f
#define KL_CHUNK       512

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static long peak_kb(void) {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_maxrss;
}

static int32_t *read_ids(const char *path, int *n) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    int cap = 1024;
    int32_t *v = malloc((size_t)cap * sizeof *v);
    *n = 0;
    long x;
    while (v && fscanf(f, "%ld", &x) == 1) {
        if (*n == cap) {
            cap *= 2;
            int32_t *nv = realloc(v, (size_t)cap * sizeof *nv);
            if (!nv) { free(v); v = NULL; break; }
            v = nv;
        }
        v[(*n)++] = (int32_t)x;
    }
    fclose(f);
    return v;
}

static int open_model(const char *path, int n_ctx, sk_file **f, sk_model **m) {
    char err[512];
    *f = sk_open(path, err, sizeof err);
    if (!*f) { fprintf(stderr, "sum_check: %s\n", err); return -1; }
    *m = sk_model_new(*f, n_ctx, err, sizeof err);
    if (!*m) { fprintf(stderr, "sum_check: %s\n", err); sk_close(*f); return -1; }
    return 0;
}

static int argmax(const float *x, int n) {
    int a = 0;
    for (int i = 1; i < n; i++) if (x[i] > x[a]) a = i;
    return a;
}

/* ---- parity ------------------------------------------------------------------ */

static int parity(const char *dir, const char *full) {
    sk_file *f;
    sk_model *m;
    if (open_model(full, 8192, &f, &m)) return 2;
    const sk_config *c = sk_config_of(f);
    char path[1200], line[512], err[512];
    snprintf(path, sizeof path, "%s/manifest", dir);
    FILE *man = fopen(path, "r");
    if (!man) { fprintf(stderr, "sum_check: no %s\n", path); return 2; }
    float *lg = malloc((size_t)c->n_vocab * sizeof *lg), *ref = malloc((size_t)c->n_vocab * sizeof *ref);
    int bad = 0, cases = 0;
    while (fgets(line, sizeof line, man)) {
        char name[64];
        int pt, steps;
        if (line[0] != 'p' || sscanf(line, "%63s %d %d", name, &pt, &steps) != 3) continue;
        cases++;
        int n;
        snprintf(path, sizeof path, "%s/%s.prompt", dir, name);
        int32_t *tok = read_ids(path, &n);
        snprintf(path, sizeof path, "%s/%s.logits0.f32", dir, name);
        FILE *lf = fopen(path, "rb");
        if (!tok || n != pt || !lf || fread(ref, sizeof *ref, c->n_vocab, lf) != c->n_vocab) {
            fprintf(stderr, "sum_check: %s: references missing or short\n", name);
            return 2;
        }
        fclose(lf);
        sk_model_reset(m);
        double t0 = now_s();
        if (sk_model_eval(m, tok, n, lg, 0, err, sizeof err)) { fprintf(stderr, "sum_check: %s\n", err); return 2; }
        double sig = 0, noise = 0;
        for (uint32_t i = 0; i < c->n_vocab; i++) {
            sig += (double)ref[i] * ref[i];
            noise += (double)(ref[i] - lg[i]) * (ref[i] - lg[i]);
        }
        double snr = 10 * log10(sig / (noise > 0 ? noise : 1e-30));
        /* along the reference's answer */
        snprintf(path, sizeof path, "%s/%s.steps", dir, name);
        FILE *sf = fopen(path, "r");
        int checked = 0, missed = 0, s = 0, took, want;
        float margin;
        while (sf && fscanf(sf, "%d %d %f", &took, &want, &margin) == 3) {
            if (margin > PARITY_MARGIN) {
                checked++;
                if (argmax(lg, (int)c->n_vocab) != want) {
                    missed++;
                    printf("  %s step %d: %d, the reference %d (margin %.3f)\n", name, s, argmax(lg, (int)c->n_vocab), want, (double)margin);
                }
            }
            int32_t t = took;
            if (sk_model_eval(m, &t, 1, lg, 0, err, sizeof err)) { fprintf(stderr, "sum_check: %s\n", err); return 2; }
            s++;
        }
        if (sf) fclose(sf);
        int ok = snr >= PARITY_SNR_DB && missed == 0 && s == steps;
        printf("%s: %d prompt tokens, first logits %.1f dB SNR, %d of %d steps checked, %d differ (%.1f s) %s\n", name, n,
               snr, checked, s, missed, now_s() - t0, ok ? "ok" : "FAILS");
        bad |= !ok;
        free(tok);
    }
    fclose(man);
    free(lg);
    free(ref);
    sk_model_free(m);
    sk_close(f);
    if (!cases) { fprintf(stderr, "sum_check: no prompts in the manifest\n"); return 2; }
    return bad;
}

/* ---- kl -------------------------------------------------------------------------- */

/* log-softmax of n logits into out (double sum, as the reference's perplexity tool) */
static void log_softmax(const float *x, int n, float *out) {
    float mx = x[0];
    for (int i = 1; i < n; i++) if (x[i] > mx) mx = x[i];
    double s = 0;
    for (int i = 0; i < n; i++) s += exp((double)(x[i] - mx));
    float ls = (float)log(s);
    for (int i = 0; i < n; i++) out[i] = x[i] - mx - ls;
}

/* The scored positions' log-probabilities of a model over the text: per chunk
 * of KL_CHUNK tokens from an empty context, positions KL_CHUNK/2 .. KL_CHUNK-2
 * (the reference perplexity tool's), written to `out` as float32. */
static int kl_pass(const char *model, const int32_t *tok, int chunks, FILE *base, FILE *out, double *mean) {
    sk_file *f;
    sk_model *m;
    if (open_model(model, KL_CHUNK, &f, &m)) return -1;
    int nv = (int)sk_config_of(f)->n_vocab, first = KL_CHUNK / 2, per = KL_CHUNK - 1 - first, sub = 64;
    float *lg = malloc((size_t)sub * nv * sizeof *lg), *lp = malloc((size_t)nv * sizeof *lp),
          *bp = malloc((size_t)nv * sizeof *bp);
    char err[512];
    double sum = 0;
    long count = 0;
    double t0 = now_s();
    for (int ch = 0; ch < chunks; ch++) {
        const int32_t *t = tok + (size_t)ch * KL_CHUNK;
        sk_model_reset(m);
        if (sk_model_eval(m, t, first, NULL, 0, err, sizeof err)) { fprintf(stderr, "sum_check: %s\n", err); return -1; }
        for (int at = first; at < KL_CHUNK - 1; at += sub) {
            int k = KL_CHUNK - 1 - at < sub ? KL_CHUNK - 1 - at : sub;
            if (sk_model_eval(m, t + at, k, lg, 1, err, sizeof err)) { fprintf(stderr, "sum_check: %s\n", err); return -1; }
            for (int i = 0; i < k; i++) {
                log_softmax(lg + (size_t)i * nv, nv, lp);
                if (out && fwrite(lp, sizeof *lp, (size_t)nv, out) != (size_t)nv) return -1;
                if (base) {
                    if (fread(bp, sizeof *bp, (size_t)nv, base) != (size_t)nv) return -1;
                    double kl = 0;
                    for (int v = 0; v < nv; v++) kl += exp((double)bp[v]) * ((double)bp[v] - lp[v]);
                    sum += kl;
                    count++;
                }
            }
        }
        fprintf(stderr, "  %s: chunk %d of %d (%.0f s)\n", model, ch + 1, chunks, now_s() - t0);
    }
    if (base && count != (long)chunks * per) return -1;
    if (mean) *mean = count ? sum / count : 0;
    free(lg); free(lp); free(bp);
    sk_model_free(m);
    sk_close(f);
    return 0;
}

static int kl(const char *dir, const char *quant, const char *full) {
    char path[1200], line[256];
    int n;
    snprintf(path, sizeof path, "%s/kl.ids", dir);
    int32_t *tok = read_ids(path, &n);
    double ref = -1;
    snprintf(path, sizeof path, "%s/manifest", dir);
    FILE *man = fopen(path, "r");
    while (man && fgets(line, sizeof line, man)) sscanf(line, "kl_unsloth_q4_k_m %lf", &ref);
    if (man) fclose(man);
    if (!tok || n < KL_CHUNK || ref < 0) { fprintf(stderr, "sum_check: no held-out tokens or no baseline in %s\n", dir); return 2; }
    int chunks = n / KL_CHUNK;
    /* the unquantised model's log-probabilities, to a scratch file, then the
     * quantised model's against them: one model mapped at a time */
    /* The unquantised model's side is the slow one (its file is the larger):
     * kept beside FULL.bin as FULL.bin.kl, named for it, and used again while
     * it is newer than FULL.bin and the held-out tokens. */
    char keep[1300], ids[1200];
    snprintf(keep, sizeof keep, "%s.kl", full);
    snprintf(ids, sizeof ids, "%s/kl.ids", dir);
    struct stat sk, sf, si;
    long long want = (long long)chunks * (KL_CHUNK - 1 - KL_CHUNK / 2) * (long long)sizeof(float);
    int fresh = stat(keep, &sk) == 0 && stat(full, &sf) == 0 && stat(ids, &si) == 0 && sk.st_mtime >= sf.st_mtime &&
                sk.st_mtime >= si.st_mtime;
    FILE *b = NULL;
    if (fresh) {
        b = fopen(keep, "rb");
        sk_file *ff;
        char err[512];
        if (b && (ff = sk_open(full, err, sizeof err))) {
            fresh = sk.st_size == want * (long long)sk_config_of(ff)->n_vocab;
            sk_close(ff);
        } else fresh = 0;
        if (!fresh && b) { fclose(b); b = NULL; }
    }
    if (!b) {
        b = fopen(keep, "w+b");
        if (!b) { fprintf(stderr, "sum_check: cannot write %s\n", keep); return 2; }
        if (kl_pass(full, tok, chunks, NULL, b, NULL)) { fprintf(stderr, "sum_check: the unquantised pass failed\n"); return 2; }
        rewind(b);
    } else fprintf(stderr, "sum_check: the unquantised side from %s\n", keep);
    double mean = 0;
    if (kl_pass(quant, tok, chunks, b, NULL, &mean)) { fprintf(stderr, "sum_check: the quantised pass failed\n"); return 2; }
    fclose(b);
    printf("KL divergence from the unquantised model over %d chunks of %d tokens: %.6f (Unsloth's Q4_K_M: %.6f) %s\n",
           chunks, KL_CHUNK, mean, ref, mean <= ref ? "ok" : "FAILS");
    free(tok);
    return mean <= ref ? 0 : 1;
}

/* ---- bench ------------------------------------------------------------------------ */

static int bench(const char *dir, const char *quant) {
    sk_file *f;
    sk_model *m;
    if (open_model(quant, 8192, &f, &m)) return 2;
    int nv = (int)sk_config_of(f)->n_vocab, n;
    char path[1200], err[512];
    snprintf(path, sizeof path, "%s/p01.prompt", dir);
    int32_t *tok = read_ids(path, &n);
    float *lg = malloc((size_t)nv * sizeof *lg);
    if (!tok || !lg) { fprintf(stderr, "sum_check: no %s\n", path); return 2; }
    double t0 = now_s();
    if (sk_model_eval(m, tok, n, lg, 0, err, sizeof err)) { fprintf(stderr, "sum_check: %s\n", err); return 2; }
    double t1 = now_s();
    int gen = 64;
    for (int i = 0; i < gen; i++) {
        int32_t t = argmax(lg, nv);
        if (sk_model_eval(m, &t, 1, lg, 0, err, sizeof err)) { fprintf(stderr, "sum_check: %s\n", err); return 2; }
    }
    double t2 = now_s();
    printf("prompt: %d tokens in %.1f s, %.1f tokens/s\nanswer: %d tokens in %.1f s, %.2f tokens/s\npeak resident: %.0f MB\n",
           n, t1 - t0, n / (t1 - t0), gen, t2 - t1, gen / (t2 - t1), peak_kb() / 1024.0);
    long peak = peak_kb();
    free(tok);
    free(lg);
    sk_model_free(m);
    sk_close(f);
    return peak < 2500L * 1024 ? 0 : 1;
}

/* ---- tokenizer ----------------------------------------------------------------------- */

static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void put64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }

static int tokenizer(const char *in, const char *out) {
    char err[512];
    sk_file *f = sk_open_tokenizer(in, err, sizeof err);
    if (!f) { fprintf(stderr, "sum_check: %s\n", err); return 2; }
    const char *names[4] = { "tok.added", "tok.bytes", "tok.merges", "tok.offs" };   /* sorted */
    const sk_tensor *t[4];
    for (int i = 0; i < 4; i++)
        if (!(t[i] = sk_find(f, names[i]))) { fprintf(stderr, "sum_check: %s has no %s\n", in, names[i]); return 2; }
    uint64_t data = (SK_HEADER + 4 * SK_ENTRY + 63) & ~(uint64_t)63, at = data, off[4];
    for (int i = 0; i < 4; i++) { off[i] = at; at = (at + t[i]->bytes + 63) & ~(uint64_t)63; }
    uint64_t size = off[3] + t[3]->bytes;
    uint8_t *buf = calloc(1, (size_t)size);
    if (!buf) return 2;
    memcpy(buf, f->p, SK_HEADER);                        /* the source's header, its counts changed */
    put32(buf + 12, 4);
    put64(buf + 24, data);
    put64(buf + 32, size);
    for (int i = 0; i < 4; i++) {
        uint8_t *e = buf + SK_HEADER + i * SK_ENTRY;
        memcpy(e, names[i], strlen(names[i]));
        put32(e + 48, t[i]->dtype);
        put32(e + 52, t[i]->ndim);
        for (int d = 0; d < SK_MAX_DIMS; d++) put32(e + 56 + 4 * d, t[i]->dims[d]);
        put64(e + 72, off[i]);
        put64(e + 80, t[i]->bytes);
        memcpy(buf + off[i], t[i]->data, (size_t)t[i]->bytes);
    }
    FILE *o = fopen(out, "wb");
    int bad = !o || fwrite(buf, 1, (size_t)size, o) != (size_t)size;
    if (o && fclose(o)) bad = 1;
    free(buf);
    sk_close(f);
    if (bad) { fprintf(stderr, "sum_check: cannot write %s\n", out); return 2; }
    printf("wrote %s (%llu bytes)\n", out, (unsigned long long)size);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "parity")) return parity(argv[2], argv[3]);
    if (argc == 5 && !strcmp(argv[1], "kl")) return kl(argv[2], argv[3], argv[4]);
    if (argc == 4 && !strcmp(argv[1], "bench")) return bench(argv[2], argv[3]);
    if (argc == 4 && !strcmp(argv[1], "tokenizer")) return tokenizer(argv[2], argv[3]);
    fprintf(stderr, "usage: sum_check parity DIR FULL.bin | kl DIR SUMMARY.bin FULL.bin | bench DIR SUMMARY.bin |\n"
                    "       sum_check tokenizer SUMMARY.bin OUT\n");
    return 2;
}
