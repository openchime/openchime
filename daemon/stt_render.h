/*
 * OpenChime — the voice-input recognizer interface (ARCH-112,
 * docs/VOICE-INPUT.md §6).
 *
 * The recognizer is behind a small engine interface so the worker and the daemon
 * above it are tested with a stub that turns known audio into known words, and
 * only the build job runs the real model -- the arrangement read-aloud uses
 * (tts_render.h).
 */
#ifndef OC_STT_RENDER_H
#define OC_STT_RENDER_H

#include <stddef.h>
#include <stdint.h>

typedef struct oc_stt_engine {
    /* Names the model and its data together, as STT_INFO announces it. */
    const char *version;
    /* The language the recognizer hears, as a BCP 47 tag ("en-US"). */
    const char *lang;
    void       *ctx;        /* passed to open */

    /* Load what recognition needs; NULL with a reason on failure. */
    void *(*open)(void *ctx, char *err, size_t errcap);
    void  (*close)(void *engine);
    /* One utterance of 16 kHz mono PCM to text: *text malloc'd (possibly empty),
     * freed by the caller; returns 0, or -1 with a reason. */
    int   (*hear)(void *engine, const int16_t *pcm, size_t samples, char **text,
                  char *err, size_t errcap);
} oc_stt_engine;

/* The engine for `lang`, or NULL if this binary has none built in (stt.c); only
 * in a daemon built with voice input. */
const oc_stt_engine *oc_stt_engine_for(const char *lang);

#endif
