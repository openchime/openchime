/* ttskit internals shared between its source files. */
#ifndef TTS_PRIV_H
#define TTS_PRIV_H

#include "ttskit.h"

#include <stdint.h>
#include <stddef.h>

#define TTS_WORD_MAX 64          /* longest word looked up or guessed, bytes */
#define TTS_IPA_MAX  256         /* longest pronunciation of one word, bytes */

/* Read-only data: a file mapping ttskit owns, or bytes the caller lent (owned 0). */
typedef struct { const uint8_t *p; size_t n; int owned; } tts_map;
int  tts_map_open(const char *path, tts_map *m);
void tts_map_close(tts_map *m);

/* ---- lexicon.bin ----------------------------------------------------------
 * header (32 bytes)   magic "OCTTSLX1", u32 version, u32 count, u32 pool_size,
 *                     u32 reserved x3
 * index               count x { u32 word_off, u32 ipa_off }, sorted by word bytes
 * pool                NUL-terminated strings; offsets are from the pool's start
 * All integers little-endian. */
#define TTS_LEX_MAGIC   "OCTTSLX1"
#define TTS_LEX_VERSION 1u

typedef struct { tts_map map; uint32_t count; const uint8_t *index; const char *pool; uint32_t pool_size; } tts_lexicon;
/* `mem`, if not NULL, supplies the bytes instead of mapping `path` (which then only
 * names them in messages). */
int         tts_lexicon_open(const char *path, const tts_map *mem, tts_lexicon *lx, char *err, size_t errcap);
void        tts_lexicon_close(tts_lexicon *lx);
const char *tts_lexicon_find(const tts_lexicon *lx, const char *word);

/* ---- guesses.bin -----------------------------------------------------------
 * header (64 bytes)   magic "OCTTSGS1", u32 version, u32 order, u32 n_tokens,
 *                     u32 pool_size, u32 count[8] (entries per order, 1-based
 *                     orders in slots 0..7), u32 reserved x2
 * tokens              n_tokens x { u32 graph_off, u32 phon_off } — graphemes and
 *                     phonemes as written in the ARPA file with '|' removed and
 *                     "_" as the empty phoneme; token 0 is <s>, 1 is </s>
 * pool                NUL-terminated strings
 * per order k=1..N    count[k] x { u16 id[k], f32 logprob, f32 backoff }, sorted
 *                     by the id tuple; logprob and backoff are log10 */
#define TTS_GS_MAGIC    "OCTTSGS1"
#define TTS_GS_VERSION  1u
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

#endif
