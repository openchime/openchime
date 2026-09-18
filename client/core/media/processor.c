/* The capture processors (oc_processor.h). */
#include "oc_processor.h"

#include <speex/speex_echo.h>

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
