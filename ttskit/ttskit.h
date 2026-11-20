/*
 * ttskit — pronunciation for read-aloud (ARCH-111, docs/TTSKIT.md).
 *
 * Turns English text into the IPA phonemes a neural voice model reads, with no
 * copyleft anywhere in the chain: a dictionary generated from CMUdict, a guesser
 * for words the dictionary does not have, trained on that same dictionary, and
 * rules for numbers and abbreviations. Pure C, no dependencies.
 *
 * The two data files are memory-mapped, not loaded: a lookup binary-searches a
 * sorted table and touches only the pages it needs, so the dictionary and the
 * n-gram model cost disk, not resident memory.
 *
 * Output is espeak-ng-style en-US IPA — stress marks before the vowel they
 * stress, long vowels marked, the flap `ɾ` — because that is what the voice model
 * was trained on (TTSKIT.md §3).
 */
#ifndef TTSKIT_H
#define TTSKIT_H

#include <stddef.h>

typedef struct tts tts;

/* Open the data in `dir` (`lexicon.bin` and `guesses.bin`). NULL on failure;
 * `err` (if given) receives a reason. Either file may be absent: without the
 * lexicon every word is guessed, without the guesser unknown words are spelled.
 *
 * `lang` is the BCP 47 tag the caller expects ("en-US"); the files carry their
 * own, and a pair that disagrees with each other or with `lang` is refused. Pass
 * NULL to accept whatever the files say, which is for tools, not for a program
 * that is going to speak the result. */
tts *tts_load(const char *dir, const char *lang, char *err, size_t errcap);
/* Why a load returned NULL, for a caller that acts on it (or a test): the data is
 * present and damaged, or is for another language than asked, or the two files
 * disagree. An absent file is not a refusal (above). */
typedef enum {
    TTS_LOADED = 0,
    TTS_REFUSED_DAMAGED,   /* a file is there and does not read as ttskit data */
    TTS_REFUSED_LANG,      /* the files are for another language than `lang`, or for two */
    TTS_REFUSED_NOMEM
} tts_status;
/* tts_load, with why in `*st` (may be NULL). */
tts *tts_load_ex(const char *dir, const char *lang, tts_status *st, char *err, size_t errcap);
/* The same, over the two files' bytes already in memory -- embedded in the
 * program, say. Nothing is copied: the bytes must outlive the handle. Both must be
 * given and valid. */
tts *tts_load_mem(const void *lexicon, size_t lexicon_len, const void *guesses, size_t guesses_len,
                  const char *lang, char *err, size_t errcap);
void tts_free(tts *t);

/* The language the loaded data is for, or "" if nothing was loaded. */
const char *tts_lang(const tts *t);

/* The pronunciation of one word (letters and apostrophes; case ignored), written
 * to `ipa` (NUL-terminated). Returns 1 from the dictionary, 2 guessed, 3 spelled
 * letter by letter, 0 if nothing could be produced. */
int tts_word(tts *t, const char *word, char *ipa, size_t cap);

/* Numbers, times, money, percentages, ordinals, years and common abbreviations
 * written out as words: "Meet at 3:15 on the 2nd, $40" -> "Meet at three fifteen
 * on the second, forty dollars". Returns the length written. */
size_t tts_normalize(const char *text, char *out, size_t cap);

/* A whole speakable sentence to IPA: normalize, split into words and punctuation,
 * pronounce each (function words in their unstressed forms), and join with
 * spaces, keeping , . ! ? ; : as the model's pause marks. Returns the length
 * written, 0 if nothing was pronounceable. */
size_t tts_text(tts *t, const char *text, char *ipa, size_t cap);

/* ---- the voice model's data: voice.bin ------------------------------------
 *
 * Every tensor the voice model's forward pass reads, already in the precision
 * and layout its kernels run fastest on (tts_priv.h, "Weight layouts"), and the
 * voices' style tables. Made from the upstream model by `openchimed
 * --voice-pack` (daemon/voice_pack.c, which documents the tensor names); mapped,
 * never copied.
 *
 * header (128 bytes)  magic "OCTTSVC1", u32 version, u32 count, u32 header size
 *                     (128), u32 entry size (96), u64 data offset, u64 file size,
 *                     u8 sha256 of the model[32], u8 sha256 of the voices[32],
 *                     zeros to 128
 * index               count x { char name[48] (NUL-terminated, NUL-padded),
 *                     u32 dtype, u32 ndim, u32 dims[4] (unused 0), u64 offset,
 *                     u64 bytes, f32 scale, i32 zero point }, sorted by name bytes
 * data                from the data offset (the index's end rounded up to 64),
 *                     each tensor at a multiple of 64 from the file's start, in
 *                     index order, zeros between; the file ends with the last
 * All integers little-endian. A quantised tensor's scale and zero point are its
 * entry's; every other tensor has 0 and 0 there. */
#define VOICE_MAGIC     "OCTTSVC1"
#define VOICE_VERSION   1u
#define VOICE_HEADER    128u
#define VOICE_ENTRY     96u
#define VOICE_NAME_MAX  48         /* name bytes, the NUL included */
#define VOICE_ALIGN     64
#define VOICE_MAX_DIMS  4
/* Element types: f32, uint8, int8 (the model format's codes for the same). */
enum { VOICE_F32 = 1, VOICE_U8 = 2, VOICE_I8 = 3 };

typedef struct voice voice;
typedef struct {
    const char *name;
    int         dtype, ndim, dims[VOICE_MAX_DIMS];
    const void *data;                  /* in the mapping: 64-byte aligned */
    size_t      bytes;
    float       scale;
    int         zero_point;
} voice_tensor;

/* Map `path`, or use `len` bytes the caller lends (nothing is copied: they must
 * outlive the handle, and start 64-byte aligned). NULL on failure, with the
 * reason in `err`: a file that is not voice.bin of this version, is cut short or
 * has an index that points outside it or out of line is refused. */
voice *voice_open(const char *path, char *err, size_t errcap);
voice *voice_open_mem(const void *bytes, size_t len, char *err, size_t errcap);
void   voice_close(voice *v);
/* The tensor called `name`: 1 with `t` filled, 0 if there is none. */
int    voice_tensor_get(const voice *v, const char *name, voice_tensor *t);
/* The tensors in index order, for tools and tests. */
size_t voice_tensor_count(const voice *v);
int    voice_tensor_at(const voice *v, size_t i, voice_tensor *t);

/* ---- speaking with it -------------------------------------------------------
 *
 * The model reads token ids (0..177, from the IPA tts_text writes), a style row
 * (256 floats) and a speaking rate, and writes 24 kHz mono audio: 600 samples
 * for every frame it gives the sentence. One thread, plain C. */
#define VOICE_RATE       24000      /* samples per second */
#define VOICE_STYLE_ROWS 400        /* rows of a voice's style table */
#define VOICE_STYLE_DIM  256        /* floats in a style row */

/* Row `row` of voice `id`'s style table ("expr-voice-2-m" ...; past the last row,
 * the last), into style[VOICE_STYLE_DIM]. 0, or -1 if the data has no such voice. */
int    voice_style(const voice *v, const char *id, size_t row, float *style);
/* Render `tokens` token ids with a style row at `speed` (1 is the model's own
 * rate; larger is faster). The model mixes random noise into its pitch source:
 * `noise` 1 does that, drawing from a generator seeded with `seed` (the same
 * seed, the same audio), 0 leaves it out (the parity tests' setting). Returns
 * the raw waveform -- no high pass, trim or fade -- as a malloc'd array of
 * *samples floats the caller frees; NULL with the reason in `err` (a token out
 * of range, too many tokens or frames, damaged data, no memory). */
float *voice_say(const voice *v, const int *ids, int tokens, const float *style, float speed, int noise,
                 unsigned long long seed, size_t *samples, char *err, size_t errcap);

/* ---- building the data (used by tts_pack and the tests) ------------------ */

/* One CMUdict ARPAbet pronunciation ("M AY0 G R EY1 SH AH0 N") to IPA. With
 * `units` set, phonemes are separated by spaces (the guesser's training form);
 * otherwise they are joined ("maɪɡɹˈeɪʃən"). Returns the length, 0 on an
 * unknown symbol. */
size_t tts_arpa_to_ipa(const char *arpa, int units, char *out, size_t cap);

/* Pack `lexicon.ipa` ("word<TAB>ipa" lines, any order) into `lexicon.bin`, stamped
 * with the BCP 47 `lang` it is for. A tag too long for the field is refused, never
 * truncated: a truncated tag names a different language. */
int tts_pack_lexicon(const char *ipa_path, const char *bin_path, const char *lang,
                     char *err, size_t errcap);
/* Pack a Phonetisaurus ARPA joint n-gram model into `guesses.bin`, likewise. */
int tts_pack_guesser(const char *arpa_path, const char *bin_path, const char *lang,
                     char *err, size_t errcap);

#endif
