/*
 * OpenChime — one read-aloud render: speakable text to an audio-only Opus MP4
 * (ARCH-111, docs/READ-ALOUD.md §4).
 *
 * The synthesizer is behind a small engine interface so the render path, the
 * worker and the daemon above it are tested with a stub that makes a tone of
 * the right length, and only the build job runs the real voice model.
 */
#ifndef OC_TTS_RENDER_H
#define OC_TTS_RENDER_H

#include <stddef.h>
#include <stdint.h>

/* Longest piece synthesized in one model run, in characters of speakable text.
 * The model's working memory grows with its input: three sentences in one run
 * peak at 337 MB where the same three in turn peak at 253 MB. */
#define OC_TTS_SEGMENT_CHARS 120

/* Opus in the render: mono, 20 ms packets, 24 kbit/s VBR. */
#define OC_TTS_BITRATE 24000

typedef struct oc_tts_engine {
    /* Names the model, its data and its settings together: a render made by one
     * version is never served as another's (READ-ALOUD.md §5). */
    const char *version;
    unsigned    rate;       /* samples per second `say` produces: 8, 12, 16, 24 or 48 kHz */
    int         voices;     /* valid voice indexes are 0 .. voices-1 */
    void       *ctx;        /* passed to open */

    /* The id and the human label of voice `voice` (0 .. voices-1), and the
     * sentence a client plays to audition one. The ids are what a profile
     * stores; a trailing "-f" or "-m" declares presentation, which is how a
     * default voice is chosen for someone who states pronouns. */
    const char *(*voice_id)(int voice);
    const char *(*voice_label)(int voice);
    const char *preview;

    /* Load what synthesis needs; NULL with a reason on failure. */
    void *(*open)(void *ctx, char *err, size_t errcap);
    void  (*close)(void *engine);
    /* One segment of speakable text to PCM in [-1, 1] at `rate`: *pcm malloc'd,
     * freed by the caller. Returns 0, 1 if the segment has nothing to say (no
     * audio, not an error), or -1 with a reason. */
    int   (*say)(void *engine, const char *segment, int voice, float **pcm, size_t *samples,
                 char *err, size_t errcap);
} oc_tts_engine;

typedef enum {
    OC_TTS_OK = 0,
    OC_TTS_NOTHING,          /* no segment had anything to say */
    OC_TTS_FAILED,           /* synthesis, encoding or storage failed */
} oc_tts_status;

/* Render `text` in `voice` with an open engine: segment it, synthesize each
 * segment, encode Opus and wrap an audio-only MP4. On OC_TTS_OK `*mp4` is
 * malloc'd (the caller frees it) with its length and duration. */
oc_tts_status oc_tts_render(const oc_tts_engine *e, void *engine, const char *text, int voice,
                            uint8_t **mp4, size_t *len, uint32_t *duration_ms,
                            char *err, size_t errcap);

/* The Kitten mini engine over the data embedded in openchimed (tts.c); only in a
 * daemon built with read-aloud. */
const oc_tts_engine *oc_tts_kitten_engine(void);

#endif
