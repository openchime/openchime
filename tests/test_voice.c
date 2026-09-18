/* Tests for voice input's client half that need no daemon (ARCH-112,
 * docs/VOICE-INPUT.md §3-§4, docs/AUDIO.md §6.4):
 *
 *   - the segmenter, over synthetic voiced sound (a harmonic complex with a
 *     syllable-rate envelope, which libfvad classifies as speech) and quiet:
 *     where free talk cuts, what it drops, the cut at the cap, push to talk;
 *   - the ERLE harness AUDIO.md §6.4 specifies, run against the speexdsp
 *     processor: a far-end signal through a synthetic room into the microphone,
 *     echo return loss enhancement measured in dB, re-convergence under clock
 *     drift, and near-end speech surviving double-talk. */
#include "check.h"
#include "oc_processor.h"
#include "oc_segmenter.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.14159265358979323846
#define RATE 16000

static unsigned g_seed = 12345;
static double noise(void) {
    g_seed = g_seed * 1103515245u + 12345u;
    return (double)((g_seed >> 8) & 0xFFFF) / 32768.0 - 1.0;
}

/* Voiced "speech": a harmonic series on a gliding pitch, shaped at syllable
 * rate. `f0` distinguishes two talkers. Appends `ms` of it to pcm at *at. */
static void voiced(int16_t *pcm, size_t *at, int ms, double f0, double amp) {
    static double ph;
    size_t n = (size_t)ms * RATE / 1000;
    for (size_t i = 0; i < n; i++) {
        double t = (double)(*at + i) / RATE;
        double f = f0 + 20 * sin(2 * PI * 3 * t);
        ph += 2 * PI * f / RATE;
        double x = 0;
        for (int h = 1; h <= 25; h++) x += sin(h * ph) / h;
        double env = 0.6 + 0.4 * sin(2 * PI * 4 * t);
        pcm[*at + i] = (int16_t)(x * env * amp + noise() * 200);
    }
    *at += n;
}

static void quiet(int16_t *pcm, size_t *at, int ms) {
    size_t n = (size_t)ms * RATE / 1000;
    for (size_t i = 0; i < n; i++) pcm[*at + i] = (int16_t)(noise() * 60);
    *at += n;
}

/* Run a whole signal through a segmenter; return the segments' lengths in ms. */
static int run_segments(oc_segmenter *s, const int16_t *pcm, size_t n, int finish, int *ms, int max) {
    int got = 0;
    for (size_t at = 0; at + OC_SEG_FRAME <= n; at += OC_SEG_FRAME) {
        if (oc_seg_push(s, pcm + at)) {
            size_t len;
            int16_t *seg = oc_seg_take(s, &len);
            if (seg && got < max) ms[got++] = (int)(len * 1000 / RATE);
            free(seg);
        }
    }
    if (finish && oc_seg_finish(s)) {
        size_t len;
        int16_t *seg = oc_seg_take(s, &len);
        if (seg && got < max) ms[got++] = (int)(len * 1000 / RATE);
        free(seg);
    }
    return got;
}

static void test_segmenter(void) {
    int before = failures;
    int16_t *pcm = malloc((size_t)RATE * 60 * sizeof *pcm);
    CHECK(pcm != NULL);
    int ms[16];

    /* Free talk: two utterances with a real pause between them are two
     * segments, each with its pre-roll and a little of its trailing quiet; a
     * 100 ms click is noise, and a breath inside a sentence does not cut it. */
    size_t n = 0;
    quiet(pcm, &n, 500);
    voiced(pcm, &n, 1200, 120, 3000);
    quiet(pcm, &n, 300);                     /* a breath: shorter than the end */
    voiced(pcm, &n, 800, 120, 3000);
    quiet(pcm, &n, 1200);                    /* a pause: the utterance is over */
    voiced(pcm, &n, 100, 120, 3000);         /* a click */
    quiet(pcm, &n, 1200);
    voiced(pcm, &n, 900, 200, 3000);
    quiet(pcm, &n, 1200);
    oc_segmenter *s = oc_seg_new(0, (size_t)RATE * 30, 2);
    CHECK(s != NULL);
    int got = run_segments(s, pcm, n, 0, ms, 16);
    CHECK(got == 2);
    /* 1200 + 300 + 800 of sound, plus the 300 ms pre-roll, the detector's own
     * hangover past the last syllable (about 60 ms) and 200 ms of tail -- and
     * none of the 1.2 s pause after it. */
    CHECK(got >= 1 && ms[0] >= 2300 && ms[0] <= 3000);
    CHECK(got >= 2 && ms[1] >= 900 && ms[1] <= 1600);
    oc_seg_free(s);

    /* The cap: talk for 12 s with a pause at 7 s into a 10 s cap -- the cut is
     * at the pause, not mid-word, and the rest is the next segment. */
    n = 0;
    quiet(pcm, &n, 300);
    voiced(pcm, &n, 7000, 120, 3000);
    quiet(pcm, &n, 400);                     /* a pause, not an end */
    voiced(pcm, &n, 5000, 120, 3000);
    quiet(pcm, &n, 1200);
    s = oc_seg_new(0, (size_t)RATE * 10, 2);
    got = run_segments(s, pcm, n, 0, ms, 16);
    CHECK(got == 2);
    CHECK(got >= 1 && ms[0] >= 7000 && ms[0] <= 7800);   /* cut at the pause */
    CHECK(got >= 2 && ms[1] >= 5000);
    oc_seg_free(s);

    /* Push to talk: everything held is one segment, released by finish; a
     * press with nothing said is dropped. */
    n = 0;
    quiet(pcm, &n, 200);
    voiced(pcm, &n, 600, 120, 3000);
    quiet(pcm, &n, 900);                     /* free talk would end here */
    voiced(pcm, &n, 600, 120, 3000);
    s = oc_seg_new(1, (size_t)RATE * 30, 2);
    got = run_segments(s, pcm, n, 1, ms, 16);
    CHECK(got == 1 && ms[0] >= 2200);
    oc_seg_free(s);
    n = 0;
    quiet(pcm, &n, 1500);
    s = oc_seg_new(1, (size_t)RATE * 30, 2);
    CHECK(run_segments(s, pcm, n, 1, ms, 16) == 0);
    oc_seg_free(s);

    free(pcm);
    if (failures == before) printf("  segmenter ok\n");
}

/* ---- the ERLE harness (AUDIO.md §6.4) ---- */

/* A synthetic room at 16 kHz: a direct path after `delay` samples and a
 * decaying tail. */
#define RIR_LEN 1600
static void make_rir(double *h, int delay) {
    memset(h, 0, RIR_LEN * sizeof *h);
    h[delay] = 0.6;
    for (int k = delay + 1; k < RIR_LEN; k++) h[k] = 0.25 * noise() * exp(-(double)(k - delay) / 250.0);
}

/* The echo of `src` in a fresh room: 30 ms of acoustic and buffering delay. */
static int16_t *make_echo(const int16_t *src, size_t n) {
    static double h[RIR_LEN];
    make_rir(h, 480);
    int16_t *echo = malloc(n * sizeof *echo);
    if (!echo) return NULL;
    for (size_t i = 0; i < n; i++) {
        double e = 0;
        for (int k = 0; k < RIR_LEN && (size_t)k <= i; k++) e += h[k] * src[i - (size_t)k];
        echo[i] = (int16_t)e;
    }
    return echo;
}

/* Add `near` to `echo`, run the processor frame by frame with `far` as its
 * reference, and report ERLE in dB over [from, to) (echo energy in, over what
 * is left of it out), and the correlation of the output with `near` over the
 * same span when `near` is given. `lag` is how much later than its input the
 * processor's output comes. */
static double run_aec_core(const oc_audio_processor *proc, int rate, int frame, int lag, const int16_t *far,
                           const int16_t *echo, const int16_t *near,
                           size_t n, size_t from, size_t to, double *near_corr) {
    int16_t *mic = malloc(n * sizeof *mic);
    for (size_t i = 0; i < n; i++) {
        double m = (double)echo[i] + (near ? near[i] : 0);
        mic[i] = (int16_t)(m > 32767 ? 32767 : m < -32768 ? -32768 : m);
    }
    void *p = proc->open(rate, frame);
    CHECK(p != NULL);
    double ein = 0, eout = 0, xy = 0, yy = 0, xx = 0;
    int16_t *buf = malloc((size_t)frame * sizeof *buf);
    for (size_t at = 0; p && at + (size_t)frame <= n; at += (size_t)frame) {
        memcpy(buf, mic + at, (size_t)frame * sizeof *buf);
        proc->process(p, buf, far + at, frame);
        for (int i = 0; i < frame; i++) {
            size_t k = at + (size_t)i;
            if (k < from || k >= to || k < (size_t)lag) continue;
            size_t j = k - (size_t)lag;        /* the input this output sample is */
            double resid = (double)buf[i] - (near ? near[j] : 0);
            ein += (double)echo[j] * echo[j];
            eout += resid * resid;
            if (near) { xy += (double)buf[i] * near[j]; yy += (double)buf[i] * buf[i]; xx += (double)near[j] * near[j]; }
        }
    }
    free(buf);
    if (p) proc->close(p);
    if (near_corr) *near_corr = (yy > 0 && xx > 0) ? xy / sqrt(yy * xx) : 0;
    free(mic);
    return 10 * log10((ein + 1) / (eout + 1));
}

/* Voice input's canceller: 16 kHz, 20 ms frames, in a fresh room. The echo is
 * kept in `*echo_out` when asked for, so the same room can be heard again. */
static double run_aec(const int16_t *far, const int16_t *echo_src, const int16_t *near, size_t n,
                      size_t from, size_t to, double *near_corr, int16_t **echo_out) {
    int16_t *echo = make_echo(echo_src, n);
    CHECK(echo != NULL);
    if (!echo) return 0;
    double r = run_aec_core(&OC_PROCESSOR_SPEEX, RATE, 320, 0, far, echo, near, n, from, to, near_corr);
    if (echo_out) *echo_out = echo; else free(echo);
    return r;
}

/* A 16 kHz signal at 48 kHz: zero-stuffed and low-passed at 7.5 kHz (a 97-tap
 * windowed sinc), so it has no images above the band -- what the signal would
 * have been had it been made at 48 kHz. Linear interpolation leaves images of
 * the voice and the far end up to 24 kHz, which no microphone would ever carry. */
static int16_t *up3(const int16_t *x, size_t n) {
    enum { TAPS = 97 };
    static double h[TAPS];
    const double pi = 3.141592653589793, fc = 7500.0 / 48000.0;
    double sum = 0;
    for (int k = 0; k < TAPS; k++) {
        int m = k - TAPS / 2;
        h[k] = (m == 0 ? 2 * fc : sin(2 * pi * fc * m) / (pi * m)) * (0.54 - 0.46 * cos(2 * pi * k / (TAPS - 1)));
        sum += h[k];
    }
    int16_t *y = malloc(3 * n * sizeof *y);
    if (!y) return NULL;
    for (size_t i = 0; i < 3 * n; i++) {
        double acc = 0;
        /* Only every third input is non-zero: those at i + TAPS/2 - k divisible by 3. */
        for (int k = 0; k < TAPS; k++) {
            long j = (long)i + TAPS / 2 - k;
            if (j < 0 || j % 3 || (size_t)(j / 3) >= n) continue;
            acc += h[k] * 3.0 / sum * x[j / 3];
        }
        y[i] = (int16_t)(acc > 32767 ? 32767 : acc < -32768 ? -32768 : acc);
    }
    return y;
}

static void test_erle(void) {
    int before = failures;
    const size_t n = (size_t)RATE * 8;
    int16_t *far = malloc(n * sizeof *far), *drifted = malloc(n * sizeof *drifted), *near = malloc(n * sizeof *near);
    CHECK(far && drifted && near);
    size_t at = 0;
    voiced(far, &at, 8000, 110, 6000);         /* the far end: read-aloud, say */

    /* Converged: measured over the last four seconds, after the filter has had
     * four to learn the room. */
    int16_t *echo_conv = NULL, *echo_dt = NULL;
    double erle = run_aec(far, far, NULL, n, (size_t)RATE * 4, n, NULL, &echo_conv);
    printf("  ERLE, converged: %.1f dB\n", erle);
    CHECK(erle >= 20.0);

    /* Clock drift between the two sides (a separate microphone and speaker,
     * AUDIO.md §2): the echo is the far end played 100 ppm fast -- two ordinary
     * crystals at opposite ends of their tolerance. The filter must keep up
     * rather than decay to nothing. */
    for (size_t i = 0; i < n; i++) {
        double x = (double)i * 1.0001;
        size_t k = (size_t)x;
        double f = x - (double)k;
        drifted[i] = (int16_t)(k + 1 < n ? far[k] * (1 - f) + far[k + 1] * f : 0);
    }
    double erle_drift = run_aec(far, drifted, NULL, n, (size_t)RATE * 4, n, NULL, NULL);
    printf("  ERLE, 100 ppm drift: %.1f dB\n", erle_drift);
    CHECK(erle_drift >= 12.0);

    /* Double-talk: the user speaks over the far end. The canceller must remove
     * the echo and keep the voice -- the failure that makes a canceller
     * dangerous is deleting the person, and it is invisible without this. */
    at = 0;
    quiet(near, &at, 5000);
    voiced(near, &at, 3000, 220, 4000);
    double corr = 0;
    double erle_dt = run_aec(far, far, near, n, (size_t)RATE * 5 + 3200, n, &corr, &echo_dt);
    printf("  double-talk: %.1f dB of echo removed, output correlates %.2f with the near-end voice\n", erle_dt, corr);
    CHECK(corr >= 0.8);
    CHECK(erle_dt >= 10.0);

    /* A screen recording's canceller (REQ-162): 48 kHz and 20 ms frames, the
     * computer's own sound as the reference, the narrator's voice as the near
     * end. The 16 kHz canceller runs on the band where speech and echo are, and
     * its echo estimate is taken out of the full-band microphone, which comes
     * out 48 samples (1 ms) late for the band filters. The rooms are the ones
     * above, band-limited up to 48 kHz, so the numbers compare directly. */
    {
        int16_t *far48 = up3(far, n), *near48 = up3(near, n);
        int16_t *conv48 = echo_conv ? up3(echo_conv, n) : NULL, *dt48 = echo_dt ? up3(echo_dt, n) : NULL;
        CHECK(far48 && near48 && conv48 && dt48);
        if (far48 && near48 && conv48 && dt48) {
            size_t n48 = 3 * n;
            double e48 = run_aec_core(&OC_PROCESSOR_SPEEX_48K, 48000, 960, 48, far48, conv48, NULL,
                                      n48, 48000 * 4, n48, NULL);
            double c48 = 0;
            double d48 = run_aec_core(&OC_PROCESSOR_SPEEX_48K, 48000, 960, 48, far48, dt48, near48,
                                      n48, 48000 * 5 + 9600, n48, &c48);
            printf("  48 kHz (16 kHz band): ERLE converged %.1f dB; double-talk %.1f dB, voice correlates %.2f\n",
                   e48, d48, c48);
            CHECK(e48 >= 20.0);
            CHECK(c48 >= 0.8);
            CHECK(d48 >= 10.0);
        }
        free(far48); free(near48); free(conv48); free(dt48);
    }
    free(echo_conv);
    free(echo_dt);

    free(far);
    free(drifted);
    free(near);
    if (failures == before) printf("  echo canceller ok\n");
}

int run_voice_tests(void) {
    printf("test_voice: segmenter (free talk, the cap, push to talk), ERLE harness (converged, 100 ppm drift, double-talk)\n");
    test_segmenter();
    test_erle();
    return failures;
}
