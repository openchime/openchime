/* ttskit internals shared between its source files. */
#ifndef TTS_PRIV_H
#define TTS_PRIV_H

#include "ttskit.h"

#include <math.h>
#include <stdint.h>
#include <stddef.h>

#define TTS_WORD_MAX 64          /* longest word looked up or guessed, bytes */
#define TTS_IPA_MAX  256         /* longest pronunciation of one word, bytes */

/* Read-only data: a file mapping ttskit owns, or bytes the caller lent (owned 0). */
typedef struct { const uint8_t *p; size_t n; int owned; } tts_map;
int  tts_map_open(const char *path, tts_map *m);
void tts_map_close(tts_map *m);

/* The language a data file is for, as a BCP 47 tag ("en-US", "zh-Hant-HK"), NUL
 * padded. It is written into the file because the only thing that can pair a
 * lexicon with the wrong guesser is a person, and a mispaired pair does not fail
 * -- it pronounces nonsense fluently. 16 bytes takes every tag anyone writes. */
#define TTS_LANG_MAX    16

/* ---- lexicon.bin ----------------------------------------------------------
 * header (48 bytes)   magic "OCTTSLX1", u32 version, u32 count, u32 pool_size,
 *                     u32 reserved x3, char lang[16]
 * index               count x { u32 word_off, u32 ipa_off }, sorted by word bytes
 * pool                NUL-terminated strings; offsets are from the pool's start
 * All integers little-endian. */
#define TTS_LEX_MAGIC   "OCTTSLX1"
#define TTS_LEX_VERSION 2u
#define TTS_LEX_HEADER  48u

typedef struct { tts_map map; uint32_t count; const uint8_t *index; const char *pool; uint32_t pool_size;
                 char lang[TTS_LANG_MAX]; } tts_lexicon;
/* The two opens' results: 0 opened; TTS_OPEN_ABSENT the file could not be mapped
 * (not there: tolerated on the disk path); any other negative value the data is
 * there and wrong. Callers branch on the value, never on the message. */
#define TTS_OPEN_ABSENT (-2)

/* `mem`, if not NULL, supplies the bytes instead of mapping `path` (which then only
 * names them in messages). */
int         tts_lexicon_open(const char *path, const tts_map *mem, tts_lexicon *lx, char *err, size_t errcap);
void        tts_lexicon_close(tts_lexicon *lx);
const char *tts_lexicon_find(const tts_lexicon *lx, const char *word);

/* ---- guesses.bin -----------------------------------------------------------
 * header (80 bytes)   magic "OCTTSGS1", u32 version, u32 order, u32 n_tokens,
 *                     u32 pool_size, u32 count[8] (entries per order, 1-based
 *                     orders in slots 0..7), u32 reserved x2, char lang[16]
 * tokens              n_tokens x { u32 graph_off, u32 phon_off } — graphemes and
 *                     phonemes as written in the ARPA file with '|' removed and
 *                     "_" as the empty phoneme; token 0 is <s>, 1 is </s>
 * pool                NUL-terminated strings
 * per order k=1..N    count[k] x { u16 id[k], f32 logprob, f32 backoff }, sorted
 *                     by the id tuple; logprob and backoff are log10 */
#define TTS_GS_MAGIC    "OCTTSGS1"
#define TTS_GS_VERSION  2u
#define TTS_GS_HEADER   80u
#define TTS_GS_MAXORDER 8

typedef struct {
    tts_map         map;
    uint32_t        order, n_tokens;
    const uint8_t  *tokens;
    const char     *pool;
    const uint8_t  *table[TTS_GS_MAXORDER + 1];   /* 1-based */
    uint32_t        count[TTS_GS_MAXORDER + 1];
    /* Token ids by their first grapheme byte, built at open (a few KB): the
     * decoder asks "which tokens can start here" once per position. */
    uint16_t       *by_first;
    uint32_t        by_first_start[257];
    char            lang[TTS_LANG_MAX];
} tts_guesser;
int  tts_guesser_open(const char *path, const tts_map *mem, tts_guesser *g, char *err, size_t errcap);
void tts_guesser_close(tts_guesser *g);
/* The best pronunciation for a lower-case word, as joined IPA. 0 on failure. */
int  tts_guess(const tts_guesser *g, const char *word, char *ipa, size_t cap);

/* Letter names for spelling ("API" -> "eɪ pˈiː aɪ"). */
const char *tts_letter_ipa(char c);
/* Unstressed forms of function words, or NULL. */
const char *tts_weak_form(const char *word);

void tts_seterr(char *err, size_t cap, const char *fmt, ...);

static inline uint32_t tts_rd32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* ---- voice.bin (voice.c; the format is in ttskit.h) ------------------------- */
struct voice { tts_map map; uint32_t count; const uint8_t *index; };
/* As tts_lexicon_open: `mem`, if not NULL, supplies the bytes instead of mapping
 * `path` (which then only names them in messages). 0, TTS_OPEN_ABSENT, or -1
 * with the reason the data is refused. */
int  voice_map_open(const char *path, const tts_map *mem, voice *v, char *err, size_t errcap);
void voice_map_close(voice *v);

/* ---- the kernels: nnkit/nnkit.h ------------------------------------------- */
#include "nnkit.h"

/* ---- the voice model's forward pass, its first half (voice_model.c) --------------
 * From token ids (0..177), a voice's style row (256 floats: the first 128 the
 * decoder's, the second 128 the prosody predictor's) and the speed, what the
 * decoder reads: the text encoder's states expanded to one row per frame, and
 * the pitch (F0) and energy (N) curves at two values per frame. A frame is 600
 * samples of the 24 kHz output. Every array is the struct's own (malloc'd);
 * time-major, rows of 512. */
#define VOICE_MAX_FRAMES (1 << 20)
typedef struct {
    int    tokens, frames;   /* T; F = the sum of dur */
    int   *dur;              /* T: frames per token, each at least 1 */
    float *dur_raw;          /* T: the sum of the duration bins over the speed, before rounding */
    float *ctx;              /* T x 512: the context encoder's output */
    float *txt;              /* T x 512: the text encoder's output */
    float *asr;              /* F x 512: txt expanded, row f the token frame f belongs to */
    float *f0, *n;           /* 2F each: the pitch and energy curves */
} voice_front;
/* 0, or -1 with the reason in err (a token out of range, a tensor missing from
 * the data, too many frames, no memory); f is then empty. */
int  voice_front_run(const voice *v, const int *ids, int tokens, const float *style, float speed, voice_front *f,
                     char *err, size_t errcap);
void voice_front_free(voice_front *f);
/* The duration rule: round half to even, then at least 1. */
int  voice_round_duration(float raw);
/* The alignment: frame_token[f] = the token frame f belongs to, frames in token
 * order, dur[t] of them for token t. Returns the frame count, or -1 if it would
 * pass cap. */
int  voice_align(const int *dur, int tokens, int *frame_token, int cap);
/* y's row f = x's row frame_token[f] (c values; rows ldx and ldy apart): the
 * graph's product with the 0/1 alignment matrix, which has one 1 per frame. */
void voice_expand(const float *x, int ldx, int c, const int *frame_token, int frames, float *y, int ldy);

/* ---- the voice model's forward pass, its second half (voice_model.c) -------------
 * From the front half to the raw waveform, in three parts that voice_say runs in
 * turn and the tests and tools can run alone. Each reads the style row's first
 * 128 values; each returns 0, or -1 with the reason in err. F is the frame count. */
#define VOICE_FRAME 600          /* output samples per frame */
/* The decoder: the expanded text states, F0 and N through its five residual
 * blocks to out, 2F rows of 512. */
int  voice_decode(const voice *v, const voice_front *f, const float *style, float *out, char *err, size_t errcap);

/* The noise generator: SplitMix64 (Steele, Lea and Flood's mixer of a Weyl
 * sequence), one 64-bit state, seeded with the seed as it is; uniform floats
 * from its top 24 bits; normal ones by Box-Muller, in pairs, from two of its
 * 53-bit uniforms in double. Repeatable for a seed; the normals go through the C
 * library's log, sin and cos, so their last bits may differ between libraries. */
typedef struct { uint64_t state; int have; float spare; } voice_rng;
uint64_t voice_rng_next(voice_rng *r);
float    voice_rng_uniform(voice_rng *r);       /* [0, 1) */
float    voice_rng_normal(voice_rng *r);        /* mean 0, variance 1 */

/* The harmonic source: from the pitch curve (2F values), the sum of nine
 * harmonics of the pitch (where it is above 10 Hz, silence elsewhere) through a
 * linear layer and tanh, sampled at 24 kHz, and its STFT (n_fft 20, hop 5):
 * har, 120F + 1 rows of 11 magnitudes then 11 phases. The phases' arithmetic
 * is ONNX Runtime's to the bit, since its running sum carries any difference
 * into every later sample. With noise on (the model's own behaviour), the
 * random operators it replaces draw from voice_rng seeded with `seed`: each
 * sample and harmonic gets normal noise of standard deviation 0.003 where
 * voiced and 0.1 / 3 where not, and harmonics 2..9 a uniform initial phase on
 * the first sample (which the graph's resampling never reads); off, both are 0.
 * noise_out, if not NULL, receives the normal noise as added (600F rows of 9). */
int  voice_source(const voice *v, const float *f0, int frames, int noise, uint64_t seed, float *har, float *noise_out,
                  char *err, size_t errcap);
/* The generator: the decoder's output (2F rows of 512) and the source's
 * spectrum to 600F samples of the raw waveform (no high pass, no trim). */
int  voice_generate(const voice *v, const float *dec, const float *har, int frames, const float *style, float *pcm,
                    char *err, size_t errcap);

#endif
