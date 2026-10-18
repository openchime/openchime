/*
 * OpenChime — the read-aloud voice model: Kitten mini v0.8 through the ONNX Runtime
 * C API (ARCH-111, docs/READ-ALOUD.md §4).
 *
 * Takes the IPA ttskit produces and returns 24 kHz mono audio in one of the model's
 * eight voices. The model (ONNX Runtime's .ort format) and the voices table are
 * bytes the caller supplies -- in openchimed, files mapped from the data directory
 * (tts_data.h) -- and are used where they lie, not copied.
 */
#ifndef OC_TTS_KITTEN_H
#define OC_TTS_KITTEN_H

#include <stddef.h>

#define KITTEN_RATE       24000   /* output samples per second */
#define KITTEN_VOICES     8
#define KITTEN_MAX_TOKENS 512     /* longest input, in model tokens; longer is cut */

/* The speaking rate every render uses, chosen by ear. It is part of the model
 * version, so changing it re-renders rather than mixing rates. */
#define KITTEN_SPEED 1.2f

typedef struct kitten kitten;

/* Open the model from `model` (.ort bytes) and `voices` (voices.npz bytes), both of
 * which must outlive the handle. NULL on failure, with a reason in `err`. */
kitten *kitten_open(const unsigned char *model, size_t model_len, const unsigned char *voices,
                    size_t voices_len, char *err, size_t errcap);
void    kitten_close(kitten *k);

/* The voice names, in a fixed order ("expr-voice-2-m" ...); -1 if unknown. */
const char *kitten_voice_name(int voice);
int         kitten_voice_index(const char *name);

/* Synthesize one sentence of IPA in `voice`. On success *pcm is a malloc'd array of
 * *samples floats in [-1, 1] at KITTEN_RATE, which the caller frees; returns 0.
 * Returns -1 with a reason in `err` otherwise.
 *
 * `text_chars` is the length in characters of the TEXT the IPA was made from, not
 * of the IPA: the model's style row is indexed by it (kitten_style_row), and
 * indexing by anything else makes the wrong prosody — long silences, clipped
 * words — out of a model that is otherwise working. */
int kitten_say(kitten *k, const char *ipa, size_t text_chars, int voice, float **pcm,
               size_t *samples, char *err, size_t errcap);

/* The style row for text of `chars` characters, as Kitten chooses it:
 * min(len(text), rows - 1). Exposed so it can be tested without the model. */
size_t kitten_style_row(size_t chars);

/* IPA to the model's token ids, as Kitten's own code does it: words and punctuation
 * separated by single spaces, each character looked up, unknown characters dropped,
 * wrapped in the pad token and the end marker. Returns the count written (at most
 * `cap`, at least 3 when `cap` allows). No ONNX Runtime needed; the tests link it. */
size_t kitten_tokens(const char *ipa, long long *ids, size_t cap);

#endif
