/* stt_check: the recognizer against its references, and timed (docs/STTKIT.md §8).
 *
 *   stt_check parity DIR FIXTURES   every fixture in FIXTURES/manifest
 *                                   (tests/data/recognizer) heard with DIR's
 *                                   recognizer.bin: the token ids must equal the
 *                                   reference's, and the encoder's output and the
 *                                   first step's logits each be within 30 dB SNR
 *                                   of the reference's; a table, non-zero exit on
 *                                   any failure
 *   stt_check bench DIR FIXTURES    every fixture heard in full, one thread, as
 *                                   the daemon hears it: the real-time factor per
 *                                   fixture and overall, and the peak RSS
 *   stt_check hear DIR FILE.s16     one 16 kHz int16 recording's words
 */
#define _POSIX_C_SOURCE 200809L
#include "stt_priv.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

#define MIN_SNR 30.0
#define VOCAB   32768

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void *slurp(const char *dir, const char *name, const char *ext, size_t *n) {
    char path[4200];
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

static float *pcm_of(const char *dir, const char *name, size_t *samples) {
    size_t n;
    int16_t *s = slurp(dir, name, ".s16", &n);
    if (!s) return NULL;
    *samples = n / 2;
    float *f = malloc((*samples ? *samples : 1) * sizeof *f);
    for (size_t i = 0; f && i < *samples; i++) f[i] = (float)s[i] / 32768.0f;
    free(s);
    return f;
}

static double snr(const float *got, const float *want, size_t n) {
    double sig = 0, err = 0;
    for (size_t i = 0; i < n; i++) {
        double d = (double)got[i] - want[i];
        sig += (double)want[i] * want[i];
        err += d * d;
    }
    return err == 0 ? INFINITY : 10 * log10(sig / err);
}

typedef struct { char name[64]; size_t samples; int frames; size_t ntok; int64_t ids[300]; } fixture;

/* The next fixture line of the manifest; 0 at its end. */
static int next(FILE *m, fixture *fx) {
    char line[8192];
    while (fgets(line, sizeof line, m)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char ids[4096];
        if (sscanf(line, "%63s %zu %d %zu %4095s", fx->name, &fx->samples, &fx->frames, &fx->ntok, ids) != 5) return -1;
        size_t k = 0;
        if (strcmp(ids, "-")) for (char *t = strtok(ids, ","); t && k < 300; t = strtok(NULL, ",")) fx->ids[k++] = atoll(t);
        if (k != fx->ntok) return -1;
        return 1;
    }
    return 0;
}

static recognizer *open_dir(const char *dir) {
    char path[4200], err[512] = "";
    snprintf(path, sizeof path, "%s/recognizer.bin", dir);
    recognizer *r = recognizer_open(path, err, sizeof err);
    if (!r) fprintf(stderr, "stt_check: %s\n", err);
    return r;
}

static FILE *manifest(const char *fixtures) {
    char path[4200];
    snprintf(path, sizeof path, "%s/manifest", fixtures);
    FILE *m = fopen(path, "r");
    if (!m) fprintf(stderr, "stt_check: cannot read %s\n", path);
    return m;
}

static int do_parity(const char *dir, const char *fixtures) {
    recognizer *r = open_dir(dir);
    FILE *m = r ? manifest(fixtures) : NULL;
    if (!m) { recognizer_close(r); return 1; }
    printf("parity: same token ids; encoder output and first logits >= %.0f dB SNR\n", MIN_SNR);
    printf("%-12s %7s %7s %10s %10s  %s\n", "fixture", "frames", "tokens", "enc dB", "logits dB", "verdict");
    fixture fx;
    int st, bad = 0, count = 0;
    while ((st = next(m, &fx)) == 1) {
        size_t n = 0, nref, lref, nids = 0;
        float *pcm = pcm_of(fixtures, fx.name, &n);
        float *eref = slurp(fixtures, fx.name, ".enc.f32", &nref), *lg = slurp(fixtures, fx.name, ".logits.f32", &lref);
        float *enc = NULL, *logits = malloc(VOCAB * sizeof *logits);
        int64_t ids[300];
        int frames = 0;
        char err[512] = "";
        int ok = pcm && eref && lg && logits && n == fx.samples && lref == VOCAB * sizeof(float) &&
                 rec_run(r, pcm, n, ids, stt_max_tokens(n), &nids, &enc, &frames, logits, err, sizeof err) == 0;
        double se = 0, sl = 0;
        if (ok) {
            ok = frames == fx.frames && nref == (size_t)frames * 320 * sizeof(float) && nids == fx.ntok &&
                 (!nids || !memcmp(ids, fx.ids, nids * sizeof *ids));
            if (frames == fx.frames && nref == (size_t)frames * 320 * sizeof(float)) se = snr(enc, eref, (size_t)frames * 320);
            sl = snr(logits, lg, VOCAB);
            ok = ok && se >= MIN_SNR && sl >= MIN_SNR;
        }
        printf("%-12s %7d %7zu %10.1f %10.1f  %s%s\n", fx.name, frames, nids, se, sl, ok ? "ok" : "FAIL", err[0] ? err : "");
        bad += !ok;
        count++;
        free(pcm); free(eref); free(lg); free(enc); free(logits);
    }
    fclose(m);
    recognizer_close(r);
    if (st < 0) { fprintf(stderr, "stt_check: the manifest is malformed\n"); return 1; }
    printf("%d of %d fixtures %s\n", count - bad, count, bad ? "pass; FAIL" : "pass");
    return bad || !count;
}

static int do_bench(const char *dir, const char *fixtures) {
    recognizer *r = open_dir(dir);
    FILE *m = r ? manifest(fixtures) : NULL;
    if (!m) { recognizer_close(r); return 1; }
    printf("bench: each fixture heard in full, one thread\n");
    fixture fx;
    double audio = 0, spent = 0;
    int st;
    while ((st = next(m, &fx)) == 1) {
        size_t n = 0;
        float *pcm = pcm_of(fixtures, fx.name, &n);
        char *text = NULL, err[512] = "";
        double t0 = now_s();
        int rc = pcm ? recognizer_hear(r, pcm, n, &text, err, sizeof err) : -1;
        double dt = now_s() - t0, secs = (double)n / STT_RATE;
        if (rc != 0) { fprintf(stderr, "stt_check: %s: %s\n", fx.name, err); free(pcm); fclose(m); recognizer_close(r); return 1; }
        printf("%-12s %5.2f s of audio in %5.2f s: %.3fx real time  %s\n", fx.name, secs, dt, dt / secs, text);
        audio += secs;
        spent += dt;
        free(text);
        free(pcm);
    }
    fclose(m);
    recognizer_close(r);
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    printf("overall: %.2f s of audio in %.2f s: %.3fx real time; peak RSS %.1f MB\n", audio, spent, spent / audio,
           (double)ru.ru_maxrss / 1024.0);
    return st < 0;
}

static int do_hear(const char *dir, const char *file) {
    recognizer *r = open_dir(dir);
    if (!r) return 1;
    FILE *f = fopen(file, "rb");
    if (!f) { recognizer_close(r); fprintf(stderr, "stt_check: cannot read %s\n", file); return 1; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    size_t n = len > 0 ? (size_t)len / 2 : 0;
    int16_t *s = malloc((n ? n : 1) * 2);
    float *pcm = malloc((n ? n : 1) * sizeof *pcm);
    if (!s || !pcm || fread(s, 2, n, f) != n) { fclose(f); free(s); free(pcm); recognizer_close(r); return 1; }
    fclose(f);
    for (size_t i = 0; i < n; i++) pcm[i] = (float)s[i] / 32768.0f;
    char *text = NULL, err[512] = "";
    int rc = recognizer_hear(r, pcm, n, &text, err, sizeof err);
    if (rc == 0) printf("%s\n", text);
    else fprintf(stderr, "stt_check: %s\n", err);
    free(text); free(s); free(pcm);
    recognizer_close(r);
    return rc != 0;
}

int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "parity")) return do_parity(argv[2], argv[3]);
    if (argc == 4 && !strcmp(argv[1], "bench")) return do_bench(argv[2], argv[3]);
    if (argc == 4 && !strcmp(argv[1], "hear")) return do_hear(argv[2], argv[3]);
    fprintf(stderr, "usage: stt_check parity DIR FIXTURES\n       stt_check bench DIR FIXTURES\n       stt_check hear DIR FILE.s16\n");
    return 2;
}
