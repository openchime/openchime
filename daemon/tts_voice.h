/*
 * OpenChime — the read-aloud voice model, run by ttskit in plain C (ARCH-111,
 * docs/details/READ-ALOUD.md §4).
 *
 * Takes the IPA ttskit produces and returns 24 kHz mono audio in one of the model's
 * eight voices. The model and its voices are one file, voice.bin (ttskit.h), whose
 * bytes the caller supplies -- in openchimed, the file mapped from the data
 * directory (tts_data.h) -- and are used where they lie, not copied.
 */
#ifndef OC_TTS_VOICE_H
#define OC_TTS_VOICE_H

#include <stddef.h>

#define TTS_VOICE_RATE       24000   /* output samples per second */
#define TTS_VOICE_COUNT      8
#define TTS_VOICE_MAX_TOKENS 512     /* longest input, in model tokens; longer is cut */

/* The speaking rate every render uses, chosen by ear. It is part of the model
 * version, so changing it re-renders rather than mixing rates. */
#define TTS_VOICE_SPEED 1.2f

/* The seed of the model's noise source, the same for every render: the same
 * text in the same voice is then the same audio, every time and on every
 * machine, so a render made again (a cache entry dropped, a second daemon)
 * matches the first. The noise is a faint breath under the voice; nothing is
 * gained by varying it. Part of the model version, like the rate. */
#define TTS_VOICE_SEED 1u

typedef struct tts_voice tts_voice;

/* Open the model from `data` (voice.bin's bytes, starting 64-byte aligned, as a
 * mapping is), which must outlive the handle. NULL on failure, with a reason in
 * `err`. */
tts_voice *tts_voice_open(const unsigned char *data, size_t len, char *err, size_t errcap);
void       tts_voice_close(tts_voice *v);

/* The voice names, in a fixed order ("expr-voice-2-m" ...); -1 if unknown. */
const char *tts_voice_name(int voice);
int         tts_voice_index(const char *name);

/* Synthesize one sentence of IPA in `voice`. On success *pcm is a malloc'd array of
 * *samples floats in [-1, 1] at TTS_VOICE_RATE, which the caller frees; returns 0.
 * Returns -1 with a reason in `err` otherwise.
 *
 * `text_chars` is the length in characters of the TEXT the IPA was made from, not
 * of the IPA: the model's style row is indexed by it (tts_voice_style_row), and
 * indexing by anything else makes the wrong prosody — long silences, clipped
 * words — out of a model that is otherwise working. */
int tts_voice_say(tts_voice *v, const char *ipa, size_t text_chars, int voice, float **pcm,
                  size_t *samples, char *err, size_t errcap);

/* The style row for text of `chars` characters, as the model's own code chooses
 * it: min(len(text), rows - 1). Exposed so it can be tested without the model. */
size_t tts_voice_style_row(size_t chars);

/* IPA to the model's token ids, as the model's own code does it: words and
 * punctuation separated by single spaces, each character looked up, unknown
 * characters dropped, wrapped in the pad token and the end marker. Returns the
 * count written (at most `cap`, at least 3 when `cap` allows). Needs no model;
 * the tests link it. */
size_t tts_voice_tokens(const char *ipa, long long *ids, size_t cap);

#endif
