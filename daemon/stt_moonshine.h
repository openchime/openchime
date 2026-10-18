/*
 * OpenChime — the voice-input recognizer: Moonshine Tiny Streaming (English)
 * through the ONNX Runtime C API (ARCH-112, docs/VOICE-INPUT.md §6).
 *
 * Takes one whole utterance of 16 kHz mono audio and returns its text. The six
 * graphs (ONNX Runtime's .ort format) and the tokenizer are bytes the caller
 * supplies -- in openchimed, files mapped from the data directory -- and are
 * used where they lie, not copied.
 *
 * The utterance is run the way Moonshine's own library runs a final segment:
 * the frontend over 80 ms chunks, the encoder and adapter over every frame at
 * once, cross-attention keys and values computed once, then greedy decoding
 * from the start token with the self-attention cache carried from step to step.
 */
#ifndef OC_STT_MOONSHINE_H
#define OC_STT_MOONSHINE_H

#include <stddef.h>
#include <stdint.h>

#define MOONSHINE_RATE 16000        /* input samples per second */

/* Tokens allowed per second of audio: Moonshine's guard against a decoder that
 * repeats itself on noise, at its default for Latin-script languages. */
#define MOONSHINE_TOKENS_PER_SECOND 6.5f
#define MOONSHINE_MAX_TOKENS        256

/* The files a model directory holds, in the order moonshine_open takes them. */
enum {
    MOONSHINE_FRONTEND = 0,     /* frontend.model.ort   */
    MOONSHINE_FRONTEND_WEIGHTS, /* frontend.weights.ort */
    MOONSHINE_ENCODER,          /* encoder.ort          */
    MOONSHINE_ADAPTER,          /* adapter.ort          */
    MOONSHINE_CROSS_KV,         /* cross_kv.ort         */
    MOONSHINE_DECODER,          /* decoder_kv.ort       */
    MOONSHINE_TOKENIZER,        /* tokenizer.bin        */
    MOONSHINE_CONFIG,           /* streaming_config.json */
    MOONSHINE_FILES
};

extern const char *const MOONSHINE_FILE_NAMES[MOONSHINE_FILES];

typedef struct { const uint8_t *data; size_t len; } moonshine_blob;

typedef struct moonshine moonshine;

/* Open the model from its files, which must outlive the handle. NULL on
 * failure, with a reason in `err`. */
moonshine *moonshine_open(const moonshine_blob files[MOONSHINE_FILES], char *err, size_t errcap);
void       moonshine_close(moonshine *m);

/* Recognize one utterance of `samples` floats in [-1, 1] at MOONSHINE_RATE.
 * On success *text is a malloc'd UTF-8 string (possibly empty) the caller
 * frees; returns 0. Returns -1 with a reason in `err` otherwise. */
int moonshine_hear(moonshine *m, const float *pcm, size_t samples, char **text,
                   char *err, size_t errcap);

/* --- No ONNX Runtime below: the tests link these. --- */

/* The tokenizer: token ids to text, as Moonshine's own does it -- each id's
 * bytes in turn, special tokens ("<...>") skipped, the word marker U+2581 read
 * as a space, and the result trimmed. An id outside the table is skipped.
 * Returns the length written to `out` (NUL-terminated within `cap`), or -1 if
 * the table is malformed. */
typedef struct {
    uint32_t count;
    const uint8_t **bytes;     /* count pointers into the file */
    uint16_t *len;
} moonshine_tokens;

int  moonshine_tokens_load(moonshine_tokens *t, const uint8_t *data, size_t len);
void moonshine_tokens_free(moonshine_tokens *t);
long moonshine_detok(const moonshine_tokens *t, const int64_t *ids, size_t n, char *out, size_t cap);

/* The most tokens an utterance of `samples` may decode to. */
size_t moonshine_max_tokens(size_t samples);

#endif
