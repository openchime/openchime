/* The read-aloud voice model through ttskit (tts_voice.h). */
#define _POSIX_C_SOURCE 200809L
#include "tts_voice.h"

#include "ttskit.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAIL_TRIM  5000            /* the upstream flat cut, kept here as a floor */
#define TAIL_WIN   120             /* 5 ms: the window the quiet test measures  */
#define TAIL_QUIET 0.015f          /* rms below this is a pause, not a word     */
#define TAIL_FADE  240             /* 10 ms ramp to zero, so no cut can click   */

/* The model's voices, in the order the helper numbers them. */
static const char *VOICE_NAMES[TTS_VOICE_COUNT] = {
    "expr-voice-2-m", "expr-voice-2-f", "expr-voice-3-m", "expr-voice-3-f",
    "expr-voice-4-m", "expr-voice-4-f", "expr-voice-5-m", "expr-voice-5-f",
};

struct tts_voice {
    voice *model;                  /* over voice.bin's bytes, lent by the caller */
};

static void seterr(char *err, size_t cap, const char *fmt, ...) {
    if (!err || !cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

const char *tts_voice_name(int voice) {
    return voice >= 0 && voice < TTS_VOICE_COUNT ? VOICE_NAMES[voice] : NULL;
}

int tts_voice_index(const char *name) {
    for (int i = 0; name && i < TTS_VOICE_COUNT; i++)
        if (!strcmp(name, VOICE_NAMES[i])) return i;
    return -1;
}

tts_voice *tts_voice_open(const unsigned char *data, size_t len, char *err, size_t errcap) {
    if (!data) { seterr(err, errcap, "no voice data"); return NULL; }
    tts_voice *v = calloc(1, sizeof *v);
    if (!v) { seterr(err, errcap, "out of memory"); return NULL; }
    /* Used in place: the weights cost only the pages a render touches, and the
     * kernel can drop them again. */
    v->model = voice_open_mem(data, len, err, errcap);
    if (!v->model) { free(v); return NULL; }
    /* Every voice this side names must be in the data, or a request for one
     * would fail only when someone picked it. */
    float row[VOICE_STYLE_DIM];
    for (int i = 0; i < TTS_VOICE_COUNT; i++)
        if (voice_style(v->model, VOICE_NAMES[i], 0, row) != 0) {
            seterr(err, errcap, "voice.bin has no voice %s", VOICE_NAMES[i]);
            tts_voice_close(v);
            return NULL;
        }
    return v;
}

void tts_voice_close(tts_voice *v) {
    if (!v) return;
    voice_close(v->model);
    free(v);
}

size_t tts_voice_style_row(size_t chars) { return chars < VOICE_STYLE_ROWS - 1 ? chars : VOICE_STYLE_ROWS - 1; }

int tts_voice_say(tts_voice *v, const char *ipa, size_t text_chars, int voice, float **pcm, size_t *samples,
                  char *err, size_t errcap) {
    *pcm = NULL;
    *samples = 0;
    if (voice < 0 || voice >= TTS_VOICE_COUNT) { seterr(err, errcap, "no voice %d", voice); return -1; }

    long long ids64[TTS_VOICE_MAX_TOKENS];
    int ids[TTS_VOICE_MAX_TOKENS];
    size_t n = tts_voice_tokens(ipa, ids64, TTS_VOICE_MAX_TOKENS);
    if (n <= 3) { seterr(err, errcap, "nothing to say"); return -1; }
    for (size_t i = 0; i < n; i++) ids[i] = (int)ids64[i];

    /* The style row is chosen by how many CHARACTERS were spoken, which is what
     * the model's own code indexes with (`min(len(text), rows - 1)`) and what
     * these rows were fitted to. Indexing with the token count instead -- about
     * twice as large, since the IPA of a word is longer than the word -- reaches
     * rows meant for far longer sentences: the model then paces a two-letter
     * word like a clause, emitting a click and most of a second of silence. */
    float style[VOICE_STYLE_DIM];
    if (voice_style(v->model, VOICE_NAMES[voice], tts_voice_style_row(text_chars), style) != 0) {
        seterr(err, errcap, "voice.bin has no voice %s", VOICE_NAMES[voice]);
        return -1;
    }

    /* The noise on, as the model is meant to sound, from the one fixed seed
     * (TTS_VOICE_SEED). */
    size_t count = 0;
    float *data = voice_say(v->model, ids, (int)n, style, TTS_VOICE_SPEED, 1, TTS_VOICE_SEED, &count, err, errcap);
    if (!data) return -1;
    if (count == 0) { free(data); seterr(err, errcap, "the model produced no audio"); return -1; }
    *pcm = data;   /* filtered in place: each sample is read before it is written */

    /* Take the offset out first. What this model calls silence is not zero: it
     * sits about 0.06 above it, and rises further under the words. Opus removes
     * the offset itself, which is the trouble -- fed a step down from 0.06 to
     * nothing, its filter swings the other way and takes most of a second to
     * settle, so every rendering ended on an audible thump with the last word
     * smeared into it. A 20 Hz high pass, an octave below anything a voice
     * produces, so what is removed is only the offset. */
    const double r = 1.0 - 2.0 * 3.14159265358979 * 20.0 / (double)TTS_VOICE_RATE;
    double x1 = 0, y1 = 0;
    for (size_t i = 0; i < count; i++) {
        double x = data[i];
        double y = x - x1 + r * y1;
        x1 = x;
        y1 = y;
        (*pcm)[i] = (float)(y > 1.0 ? 1.0 : y < -1.0 ? -1.0 : y);
    }

    /* The model appends a burst of noise after the words, and the model's own
     * code drops it by cutting the last 5,000 samples flat. That cut is a guess
     * about how much silence the model left, and at 1.2x it is often wrong: on a
     * short line it lands INSIDE the last word, which ends the render
     * mid-waveform and the word sounds bitten off. So take the flat cut as a
     * floor and then run on to the first quiet moment, which is where the words
     * actually stopped; the burst is always the far side of that quiet, so it is
     * still dropped. When there is no quiet left to find, the flat cut stands.
     * Measured after the high pass, since before it even silence carries the
     * offset's energy. */
    size_t end = count > TAIL_TRIM * 2 ? count - TAIL_TRIM : count;
    for (size_t i = end; i + TAIL_WIN <= count; i += TAIL_WIN) {
        double sum = 0;
        for (size_t j = 0; j < TAIL_WIN; j++) sum += (double)(*pcm)[i + j] * (*pcm)[i + j];
        if (sum / TAIL_WIN < (double)TAIL_QUIET * TAIL_QUIET) { end = i; break; }
    }
    count = end;

    /* Land on zero. Wherever the cut falls, the last sample is whatever the wave
     * happened to be doing, and a speaker asked to jump from there to silence
     * makes a click -- so ramp the final few milliseconds down instead. Short
     * enough to be inaudible as a fade, long enough that no step is left. */
    size_t fade = count < TAIL_FADE ? count : TAIL_FADE;
    for (size_t i = 0; i < fade; i++)
        (*pcm)[count - fade + i] *= (float)(fade - 1 - i) / (float)fade;
    *samples = count;
    return 0;
}
