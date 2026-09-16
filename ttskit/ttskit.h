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
