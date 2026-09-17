# ttskit — pronunciation for read-aloud

`ttskit` is an in-tree C library that turns English text into the phonemes a neural voice
model reads. It is the front half of read-aloud (ARCH-111, [READ-ALOUD.md](./READ-ALOUD.md)):
the daemon cleans a message, hands the words to ttskit, and feeds ttskit's phonemes to the
voice model. It has no dependencies, no copyleft anywhere in it or its data, and reads its
data where it lies — a memory-mapped file, or the copy embedded in `openchimed` — rather
than loading it.

This document covers the whole chain: why ttskit exists (§1), where it sits (§2), how text
becomes sounds (§3–§5), the data files (§6), the API with examples (§7), rebuilding the data
from sources (§8), the tests (§9) and what it does not do (§10).

## 1. Why it exists

A voice model such as Kitten does not read letters. It was trained on **phonemes** — the
sounds of the words, written in IPA — so something has to decide that "migration" is
`maɪɡɹˈeɪʃən` and that "3:15" is "three fifteen". That step is called **G2P**,
grapheme-to-phoneme: letters (graphemes) in, sounds (phonemes) out.

Every ready-made English G2P used with these models is **espeak-ng**, which is GPL-3.0.
Kitten's own Python package calls it, Piper links it, and sherpa-onnx compiles it into its
library, so none of them can ship in OpenChime. ttskit does the same job with parts whose
licences allow it:

- **CMUdict** (BSD-style), a pronunciation dictionary of about 126,000 English words,
  converted to IPA by our own table;
- a **guesser** for words the dictionary lacks, a statistical model we train on that same
  converted dictionary, so it is ours too;
- rules for **numbers, times, money and abbreviations**.

The voice model was trained on espeak-ng's output, so ttskit writes the same *style* of IPA
(§3). Measured against espeak-ng on common words the dictionary differs on 7.6% of phonemes,
mostly in stress placement and vowel length, and the difference was judged by ear on
rendered speech before ttskit was adopted.

## 2. The chain, message to audio

| Step | What happens | Where | Licence |
|---|---|---|---|
| 1 | Markdown, code, links, mentions and emoji turned into plain sentences | `shared/speakable.c` | ours |
| 2 | Numbers, times, money, ordinals, abbreviations written as words | `ttskit/normalize_en.c` | ours |
| 3 | Each word looked up in the dictionary | `ttskit/tts_lexicon.c`, `ttskit/data/en-US/lexicon.bin` | ours; data from CMUdict |
| 4 | Words not in it guessed; short capitals spelled | `ttskit/tts_guess.c`, `ttskit/data/en-US/guesses.bin` | ours; model trained on the data above |
| 5 | Function words in their unstressed forms, words joined with pause marks | `ttskit/tts_text.c` | ours |
| 6 | IPA characters mapped to the model's token ids | `daemon/tts_kitten_tokens.c`, Kitten's symbol table | ours; table from Kitten (Apache-2.0) |
| 7 | Token ids and a voice's style vector to 24 kHz audio | `daemon/tts_kitten.c`, Kitten mini through a minimal static ONNX Runtime | Apache-2.0 / MIT |
| 8 | Audio encoded and stored | the daemon's render worker, libopus and `oc_mp4` | BSD-3 / ours |

ttskit is steps 2–5. It is linked by the daemon and by the tests, and by nothing that
ships to clients.

Tools used only to *build* the data — Phonetisaurus (BSD-3) and the OpenFst (Apache-2.0) and
MITLM (MIT) programs bundled with it — never ship and are not needed to build OpenChime.

## 3. Words to sounds: ARPAbet, IPA and the table

CMUdict writes pronunciations in **ARPAbet**, one ASCII symbol per sound with a stress digit
on each vowel (1 primary, 2 secondary, 0 unstressed):

```
MIGRATION  M AY0 G R EY1 SH AH0 N
```

The model wants **IPA**, in the conventions espeak-ng uses for US English: the primary-stress
mark `ˈ` written immediately before the stressed vowel (not at the syllable start), long
vowels marked `ː`, `ɹ` for r, `ɡ` (the IPA g), and the flap `ɾ` for the t in "water".
`tts_arpa_to_ipa` converts one pronunciation; the table is in `ttskit/arpa_ipa_en.c`.

**Vowels** (stressed / unstressed):

| ARPAbet | IPA | | ARPAbet | IPA | | ARPAbet | IPA |
|---|---|---|---|---|---|---|---|
| AA | ɑː | | EH | ɛ | | OW | oʊ |
| AE | æ | | ER | ɜː / ɚ | | OY | ɔɪ |
| AH | ʌ / ə | | EY | eɪ | | UH | ʊ |
| AO | ɔː | | IH | ɪ | | UW | uː |
| AW | aʊ | | IY | iː / i | | | |
| AY | aɪ | | | | | | |

**Consonants:** B b, CH tʃ, D d, DH ð, F f, G ɡ, HH h, JH dʒ, K k, L l, M m, N n, NG ŋ, P p,
R ɹ, S s, SH ʃ, T t, TH θ, V v, W w, Y j, Z z, ZH ʒ.

**Context rules,** each one a place where espeak-ng's output differs from a symbol-for-symbol
mapping:

| Rule | Example |
|---|---|
| Primary stress only; secondary stress is not marked | CONTRACT (noun) `kˈɑːntɹækt` |
| AO before L or NG is short `ɔ` | BALL `bˈɔl` |
| T between a vowel (or R) and an unstressed vowel is the flap `ɾ` | WATER `wˈɔːɾɚ` |
| T before a word-final IY2 is the flap too; that IY is `i` | CITY `sˈɪɾi` |
| Unstressed IY before OW is `ɪ` | RADIO `ɹˈeɪdɪoʊ` |
| Unstressed IH/AH in de-, re-, be-, pre- or an -ed/-es ending is `ᵻ` | WANTED `wˈɔːntᵻd` |
| An unstressed ER before a vowel keeps a linking `ɹ` | COUNTERACT `kˈaʊntɚɹækt` |

**Weak forms.** CMUdict gives every one-syllable word full stress, so "the" and "to" would be
read as if emphasised. `tts_text` replaces about sixty function words with their unstressed
forms (`a` ɐ, `the` ðə, `to` tə, `of` ʌv, `you` juː …) unless the word is written in capitals.

**Spelling.** A word of two or three capitals ("API", "CPU", "US") is an initialism and is said
letter by letter, even when the dictionary has a word spelled the same. Four or five capitals
are looked up first ("NASA" is a word) and spelled if absent ("HTTPS"). A word nothing else
can pronounce is spelled too.

## 4. Numbers and abbreviations

`tts_normalize` rewrites what a voice would otherwise read symbol by symbol. English only.

| Written | Said |
|---|---|
| `42`, `1,200,000` | forty two, one million two hundred thousand |
| `3.5` | three point five |
| `2nd`, `21st`, `100th`, `40th` | second, twenty first, one hundredth, fortieth |
| `1984`, `2026`, `2005`, `1900` | nineteen eighty four, twenty twenty six, two thousand five, nineteen hundred |
| `3:15`, `3:05`, `3:00`, `3:00pm` | three fifteen, three oh five, three o'clock, three p m |
| `$40`, `$3.50`, `$1` | forty dollars, three dollars and fifty cents, one dollar |
| `50%` | fifty percent |
| `&`, a lone `+` | and, plus |
| `Dr.`, `Mr.`, `e.g.`, `i.e.`, `etc.`, `vs.`, `Sept.` … | doctor, mister, for example, that is, et cetera, versus, september |
| a run of more than 18 digits | its digits one by one |

Four-digit numbers from 1100 to 2099 are read as years; any other number as a quantity.

## 5. The guesser

The dictionary cannot hold every word — names, product names, new coinages ("Kubernetes",
"openchime"). The guesser predicts a pronunciation from the spelling.

**What training produces.** Phonetisaurus first *aligns* every dictionary word with its
pronunciation, pairing chunks of one or two letters with zero, one or two phonemes:

```
p|h}f  o}ˈoʊ  b}b  i}i  a}ə                         phobia
```

Each pair is a **joint token**. Written out this way the whole dictionary becomes a corpus of
token sequences, and a **joint n-gram model** is trained on it: for every short run of
tokens seen, the (log) probability of the last given the ones before, plus a **backoff**
weight for runs not seen. ttskit uses order 6 — up to five tokens of history. Order 8 was no
better on held-out words (68.9% of words exactly right and 7.9% of phonemes wrong at order
6, against 68.8% and 7.9% at order 8) and its model is 64 MB of text against 39 MB.

"Trained" here means counting: nobody labels anything, the model is a table of
probabilities computed from the dictionary, and it contains no one else's work but
CMUdict's.

**How ttskit decodes.** Guessing a word is finding the split of its letters into tokens whose
sequence the model scores highest; the phonemes of those tokens, in order, are the answer.
`tts_guess` does it left to right over letter positions:

1. Start with one hypothesis at position 0 whose history is the start token `<s>`.
2. At each position, for each hypothesis, try every token whose letters match the word there.
   Its cost is the probability of that token after the hypothesis's history, found by
   **Katz backoff**: look up the longest n-gram (history + token) the model has; for each
   token dropped from the front of the history to find one, add that history's backoff
   weight. A token the model has never seen costs as if impossible.
3. The new hypothesis lands at the position after the token's letters. Two hypotheses there
   with the same recent history can never be told apart again, so only the cheaper is kept;
   beyond that, each position keeps the **32 cheapest** (the beam).
4. At the end, add the cost of the end token `</s>` to each finished hypothesis, take the
   cheapest and follow its back-pointers to read off the phonemes.

**Agreement.** A beam can in principle miss the best path, so the C decoder is tested against
Phonetisaurus's own exact decoder on 3,010 words (3,000 from CMUdict and ten names): it agrees
on 3,009. The one difference, "doman", has two paths whose scores are equal to the precision
of the stored floats. A beam of 16 agrees on 99.8%, 64 on the same 3,009 at twice the time,
and 256 on the same 3,009 at eighteen times; 32 takes about 1.7 ms a word.

## 6. The data files

Both files live in `ttskit/data/en-US/` and are committed — a directory per language, so adding one is adding a directory rather than renaming files. `openchimed` embeds them in its
executable and opens them with `tts_load_mem`; `tts_load` maps them from a directory
instead (the tests and `tts_pack`). Either way nothing is copied: a lookup is a binary
search over a sorted table and reads only the pages it passes through; the kernel pages
them in on demand, shares them between processes and can drop them under pressure.
Nothing is parsed at load and no copy of the dictionary or the model is made on the heap —
opening both costs a few kilobytes of heap (the guesser's token index).

| File | Size | gzipped | Contents |
|---|---|---|---|
| `lexicon.bin` | 3.8 MB | 1.2 MB | 126,052 words and their IPA, tagged `en-US` |
| `guesses.bin` | 16.6 MB | 7.3 MB | the order-6 joint n-gram model, tagged `en-US` |
| `CMUDICT-LICENSE` | | | CMUdict's notice, which the derived data carries |

Measured resident memory: a process that only spells is 1.5 MB; looking words up in the
dictionary adds about 1 MB of mapped pages; guessing an unknown word touches more of the
model, and a sentence with two guessed words peaks at 14 MB — all of it file-backed pages the
kernel may reclaim, not memory ttskit allocated.

All integers are little-endian. Both formats carry a magic and a version; a file with the wrong
magic, a different version, or sizes that do not add up to the file's length is refused.

**`lexicon.bin`**

```
header   48 bytes   "OCTTSLX1"  u32 version=2  u32 count  u32 pool_size  u32 reserved x3
                    char lang[16]  the BCP 47 tag this data is for, NUL padded
index    count x 8  { u32 word_off, u32 ipa_off }  sorted by the word's bytes
pool     pool_size  NUL-terminated strings; offsets count from the pool's start
```

Words are lower case. When CMUdict lists several pronunciations of a word, the first is kept.

**`guesses.bin`**

```
header   80 bytes          "OCTTSGS1"  u32 version=2  u32 order  u32 n_tokens  u32 pool_size
                           u32 count[8] (n-grams per order, orders 1..8)  u32 reserved x2
                           char lang[16]  the BCP 47 tag this data is for, NUL padded
tokens   n_tokens x 8      { u32 graph_off, u32 phon_off }   token 0 is <s>, 1 is </s>
pool     pool_size         NUL-terminated letters and phonemes of each token
order k  count[k] x (2k+8) { u16 id[k], f32 log10 prob, f32 log10 backoff }  sorted by id tuple
```

A token's letters and phonemes are stored as the ARPA model writes them with the `|`
separators removed and `_` (no phoneme) as the empty string.

## 7. Using it

```c
#include "ttskit.h"

char err[256];
tts *t = tts_load("ttskit/data/en-US", "en-US", err, sizeof err);
if (!t) { fprintf(stderr, "ttskit: %s\n", err); return 1; }

char ipa[1024];
int how = tts_word(t, "migration", ipa, sizeof ipa);    /* 1: dictionary */
/* ipa = "maɪɡɹˈeɪʃən" */

tts_text(t, "Meet Dana at 3:15 on the 2nd, the API costs $40.", ipa, sizeof ipa);
/* ipa = "mˈiːt dˈeɪnə æt θɹˈiː fɪftˈiːn ɔn ðə sˈɛkənd, ðə ˈeɪ pˈiː ˈaɪ kˈɑːsts fˈɔːɹɾi dˈɑːlɚz." */

tts_free(t);
```

| Call | Does |
|---|---|
| `tts_load(dir, lang, err, cap)` | maps `dir/lexicon.bin` and `dir/guesses.bin`. A missing file is allowed (without the lexicon every word is guessed, without the guesser unknown words are spelled); a file that is present but damaged fails the load with a reason. `lang` is the BCP 47 tag expected: a pair whose tags disagree with each other or with `lang` is refused, since a lexicon of one language and a guesser of another pronounce fluent nonsense with nothing to show for it. `NULL` accepts whatever the files say, which is for tools. |
| `tts_load_mem(lex, lex_len, guess, guess_len, lang, err, cap)` | the same over the two files' bytes already in memory, such as the files the daemon maps from its data directory. Nothing is copied, so the bytes must outlive the handle; both must be present and valid. |
| `tts_lang(t)` | the language the loaded data is for. |
| `tts_free(t)` | unmaps (or lets go of lent bytes). |
| `tts_word(t, word, ipa, cap)` | one word; returns 1 dictionary, 2 guessed, 3 spelled, 0 nothing to say (no letters). |
| `tts_normalize(text, out, cap)` | §4 only; returns the length written. |
| `tts_text(t, text, ipa, cap)` | a sentence: normalize, split, pronounce, weak forms, keep `, . ! ? ; :` as pause marks; returns the length, 0 if nothing was pronounceable. |
| `tts_arpa_to_ipa(arpa, units, out, cap)` | §3's table on one ARPAbet pronunciation; with `units` the phonemes are space-separated. |
| `tts_pack_lexicon`, `tts_pack_guesser` | build the two files (§8). |

A `tts` is read-only after loading and can be shared by threads.

**From the command line,** `make tts_pack` builds a small tool that uses the same code:

```
$ build/tts_pack word ttskit/data/en-US migration Kubernetes API readme
migration   maɪɡɹˈeɪʃən        lexicon
Kubernetes  kjˈuːbɚnˈɛtiːz     guessed
API         ˈeɪ pˈiː ˈaɪ       spelled
readme      ɹiːədm             guessed

$ build/tts_pack text ttskit/data/en-US "It's 5 o'clock"
ˈɪts fˈaɪv əklˈɑːk
```

## 8. Building the data from sources

The committed files are the output of one script. Rerun it when CMUdict's pin, the IPA table
(§3), the packer or the training settings change; nobody needs to run it to build OpenChime.

```
scripts/build_ttskit_data.sh            # rebuild in build/ttskit-data and compare with the committed files
scripts/build_ttskit_data.sh install    # rebuild and replace them
```

**Prerequisites:** x86-64 Linux, a C compiler, `curl`, `python3` (only to unpack a wheel) and
about 1 GB of free memory. Phonetisaurus is fetched as its prebuilt Python wheel, pinned by
SHA-256; it is run from the unpacked wheel, not installed.

**What it does:**

1. Builds `build/tts_pack`.
2. Fetches `cmudict.dict` from CMUdict at commit `74790861f652b15e4ac49015a90074ad62a27690`
   and the `phonetisaurus-0.3.0` wheel, and checks both SHA-256s.
3. `tts_pack ipa cmudict.dict lexicon.ipa train.lex` — applies §3's table to the first
   pronunciation of each word. `lexicon.ipa` is `word<TAB>ipa`; `train.lex` is the same with
   phonemes separated by spaces, the form the aligner needs.
4. `phonetisaurus-train --lexicon train.lex --seq2_del --ngram_order 6` — aligns letters to
   phonemes (`--seq2_del` lets a letter stand for no sound, like the e in "make") and trains the
   joint n-gram model, `train/model.o6.arpa`. This is most of the run time.
5. `tts_pack lexicon lexicon.ipa lexicon.bin en-US` and
   `tts_pack guesser train/model.o6.arpa guesses.bin en-US` — pack both (§6), stamped with
   the language they are for. A tag of 16 bytes or more is refused rather than truncated:
   a truncated tag names a different language.
6. Decodes the reference words (the first column of `tests/data/ttskit_guess_ref.txt`) with
   Phonetisaurus's own decoder, `phonetisaurus-g2pfst`, to regenerate the reference the C
   decoder is tested against.
7. Prints the SHA-256s and sizes, and says whether all three files are identical to the
   committed ones.

**Run time and output,** on one core of an i5-1335U: about 2 minutes, peak memory about
1 GB, and:

```
b1338cd8ae4d22c8b8a808e4c1fa1879579acbd3157556488d9f43ba5b559bf7  lexicon.bin   3,814,030 bytes
51cf60b34b3f23a12a33bb5d5c4a239f92966e5a92e38ee0a649b2df43e0794e  guesses.bin  16,595,956 bytes
```

The build is deterministic: a rerun from a clean `build/ttskit-data` reproduces these bytes.

**Verifying the pins.** `make test` hashes both committed files and compares them with the
sums above, which are also in `tests/test_ttskit.c`. `sha256sum ttskit/data/en-US/*.bin` checks
by hand.

**After `install`:** update the two sums in `tests/test_ttskit.c` and in this section, run
`make test` (the agreement test reads the new reference), and listen to rendered speech
before committing — a changed table changes how every message sounds. Changing the data
changes read-aloud's model version, so existing renders are made again rather than mixed.

## 9. Tests

`tests/test_ttskit.c`, in `make test`:

- the ARPAbet→IPA table and each context rule in §3;
- number, time, money, ordinal, year and abbreviation expansion;
- dictionary hits, guesses, spelling of initialisms, and words with no letters;
- sentences, with weak forms, capitals and pause marks;
- loading without data (everything spelled);
- both data files against their SHA-256 pins;
- the C guesser against Phonetisaurus's decoder on the 3,010 reference words (at least 999 in
  1,000 identical);
- truncated files, a wrong magic and a newer version, each refused on load with a reason.

## 10. Limits

- **English only,** US pronunciation.
- **Heteronyms** take the dictionary's first pronunciation: "read" is always `ɹˈɛd`, "live"
  one way whatever the sentence means.
- **The guesser is a guess:** about two words in three exactly right on words it never saw,
  and most of the rest off by a sound. Names and made-up words are where it is heard.
- **Mixed letters and digits** ("r2d2", "v2") are read as the letters around the digits.
- **No secondary stress, no sentence intonation marks:** the voice model supplies the melody
  from punctuation.
