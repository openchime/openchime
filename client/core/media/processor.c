/* The capture processors (oc_processor.h). */
#include "oc_processor.h"

#include <speex/speex_echo.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

static void *none_open(int rate, int frame) { (void)rate; (void)frame; static int token; return &token; }
static void  none_close(void *p) { (void)p; }
static void  none_process(void *p, int16_t *cap, const int16_t *play, int n) { (void)p; (void)cap; (void)play; (void)n; }

const oc_audio_processor OC_PROCESSOR_NONE = { "none", none_open, none_close, none_process };

typedef struct {
    SpeexEchoState *echo;
    int             frame;
    int16_t        *out;
} speex_aec;

static void speex_close(void *p) {
    speex_aec *a = p;
    if (!a) return;
    if (a->echo) speex_echo_state_destroy(a->echo);
    free(a->out);
    free(a);
}

static void *speex_open(int rate, int frame) {
    speex_aec *a = calloc(1, sizeof *a);
    if (!a) return NULL;
    a->frame = frame;
    a->out = malloc((size_t)frame * sizeof *a->out);
    a->echo = speex_echo_state_init(frame, rate * OC_AEC_TAIL_MS / 1000);
    if (!a->out || !a->echo) { speex_close(a); return NULL; }
    speex_echo_ctl(a->echo, SPEEX_ECHO_SET_SAMPLING_RATE, &rate);
    /* The linear canceller alone. speexdsp's residual suppressor is left out on
     * measurement: in the ERLE harness it removed the near-end voice during
     * double-talk (the output kept 5% of it, against 92% without), and a
     * recognizer given residual echo still hears the user where one given a
     * deleted voice hears nothing (AUDIO.md §6.2). */
    return a;
}

static void speex_process(void *p, int16_t *cap, const int16_t *play, int n) {
    speex_aec *a = p;
    if (n != a->frame) return;                     /* the frame size is fixed at open */
    speex_echo_cancellation(a->echo, cap, play, a->out);
    memcpy(cap, a->out, (size_t)n * sizeof *cap);
}

const oc_audio_processor OC_PROCESSOR_SPEEX = { "speexdsp", speex_open, speex_close, speex_process };

/* ---- the full-band canceller -------------------------------------------------- */

/* A linear-phase low-pass at 7 kHz, 49 taps at 48 kHz: the decimator's and the
 * interpolator's. Each delays by 24 samples, so the echo estimate comes back 48
 * samples (1 ms) behind the microphone, which is delayed to meet it. */
#define BAND_TAPS  49
#define BAND_DELAY (BAND_TAPS - 1)
static double g_band_h[BAND_TAPS];

static void band_filter_init(void) {
    if (g_band_h[BAND_TAPS / 2] != 0) return;
    const double fc = 7000.0 / 48000.0, pi = 3.141592653589793;
    double sum = 0;
    for (int k = 0; k < BAND_TAPS; k++) {
        int m = k - BAND_TAPS / 2;
        double sinc = m == 0 ? 2 * fc : sin(2 * pi * fc * m) / (pi * m);
        double win = 0.54 - 0.46 * cos(2 * pi * k / (BAND_TAPS - 1));
        g_band_h[k] = sinc * win;
        sum += g_band_h[k];
    }
    for (int k = 0; k < BAND_TAPS; k++) g_band_h[k] /= sum;
}

/* Run `x` (n samples) through the low-pass, with `hist` the last BAND_TAPS-1
 * inputs of the previous call; `y` receives the output. */
static void band_fir(double *hist, const double *x, int n, double *y) {
    for (int i = 0; i < n; i++) {
        double acc = 0;
        for (int k = 0; k < BAND_TAPS; k++) {
            int j = i - k;
            acc += g_band_h[k] * (j >= 0 ? x[j] : hist[BAND_TAPS - 1 + j]);
        }
        y[i] = acc;
    }
    if (n >= BAND_TAPS - 1) {
        memcpy(hist, x + n - (BAND_TAPS - 1), (BAND_TAPS - 1) * sizeof *hist);
    } else {
        memmove(hist, hist + n, (size_t)(BAND_TAPS - 1 - n) * sizeof *hist);
        memcpy(hist + BAND_TAPS - 1 - n, x, (size_t)n * sizeof *hist);
    }
}

typedef struct {
    void    *inner;                      /* the 16 kHz canceller */
    int      frame;                      /* 48 kHz samples */
    double   hist_cap[BAND_TAPS - 1], hist_play[BAND_TAPS - 1], hist_up[BAND_TAPS - 1];
    int16_t  delay[BAND_DELAY];          /* the microphone, waiting for its echo estimate */
    double  *x, *y;                      /* frame-sized scratch */
    int16_t *cap16, *play16, *clean16;
} band_aec;

static void band_close(void *p) {
    band_aec *a = p;
    if (!a) return;
    if (a->inner) OC_PROCESSOR_SPEEX.close(a->inner);
    free(a->x); free(a->y); free(a->cap16); free(a->play16); free(a->clean16);
    free(a);
}

static void *band_open(int rate, int frame) {
    if (rate != 48000 || frame < BAND_DELAY || frame % 3) return NULL;
    band_filter_init();
    band_aec *a = calloc(1, sizeof *a);
    if (!a) return NULL;
    a->frame = frame;
    a->x = malloc((size_t)frame * sizeof *a->x);
    a->y = malloc((size_t)frame * sizeof *a->y);
    a->cap16 = malloc((size_t)(frame / 3) * sizeof *a->cap16);
    a->play16 = malloc((size_t)(frame / 3) * sizeof *a->play16);
    a->clean16 = malloc((size_t)(frame / 3) * sizeof *a->clean16);
    a->inner = OC_PROCESSOR_SPEEX.open(16000, frame / 3);
    if (!a->x || !a->y || !a->cap16 || !a->play16 || !a->clean16 || !a->inner) { band_close(a); return NULL; }
    return a;
}

static int16_t sat16(double v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : lrint(v)); }

/* Low-pass `in` (48 kHz) and keep every third sample. */
static void band_down(band_aec *a, double *hist, const int16_t *in, int16_t *out16) {
    for (int i = 0; i < a->frame; i++) a->x[i] = in[i];
    band_fir(hist, a->x, a->frame, a->y);
    for (int i = 0; i < a->frame / 3; i++) out16[i] = sat16(a->y[3 * i]);
}

static void band_process(void *p, int16_t *cap, const int16_t *play, int n) {
    band_aec *a = p;
    if (n != a->frame) return;
    band_down(a, a->hist_cap, cap, a->cap16);
    band_down(a, a->hist_play, play, a->play16);
    memcpy(a->clean16, a->cap16, (size_t)(n / 3) * sizeof *a->clean16);
    OC_PROCESSOR_SPEEX.process(a->inner, a->clean16, a->play16, n / 3);
    /* The echo the 16 kHz canceller found, back at 48 kHz: zero-stuffed, low-
     * passed, and scaled for the two samples in three that were zero. */
    for (int i = 0; i < n; i++) a->x[i] = i % 3 ? 0.0 : 3.0 * ((double)a->cap16[i / 3] - (double)a->clean16[i / 3]);
    band_fir(a->hist_up, a->x, n, a->y);
    /* ... taken out of the microphone as it was BAND_DELAY samples ago. */
    for (int i = 0; i < n; i++) {
        int16_t late = i < BAND_DELAY ? a->delay[i] : cap[i - BAND_DELAY];
        a->x[i] = (double)late - a->y[i];
    }
    /* The frame's last BAND_DELAY samples wait for the next one (a frame is
     * longer than the delay: 960 against 48). */
    memcpy(a->delay, cap + n - BAND_DELAY, BAND_DELAY * sizeof *cap);
    for (int i = 0; i < n; i++) cap[i] = sat16(a->x[i]);
}

const oc_audio_processor OC_PROCESSOR_SPEEX_48K = { "speexdsp, 16 kHz band, 48 kHz out", band_open, band_close, band_process };
