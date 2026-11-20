# ttskit — pronunciation and the voice for read-aloud

`ttskit` is an in-tree C library that turns English text into the phonemes a neural voice
model reads, and runs that voice model to turn the phonemes into audio. It is read-aloud's
speech (ARCH-111, [READ-ALOUD.md](./READ-ALOUD.md)): the daemon cleans a message, hands the
words to ttskit, maps ttskit's phonemes to the model's token ids, and has ttskit render
them in the author's voice. It has no dependencies, no copyleft anywhere in it or its data,
and reads its data where it lies — a memory-mapped file, or bytes the caller has already
mapped — rather than loading it.

This document covers the whole chain: why ttskit exists (§1), where it sits (§2), how text
becomes sounds (§3–§5), how sounds become audio (§5b), the data files (§6), the API with
examples (§7), rebuilding the data from sources (§8), the tests (§9) and what it does not
do (§10).

## 1. Why it exists

A neural voice model does not read letters. It was trained on **phonemes** — the
sounds of the words, written in IPA — so something has to decide that "migration" is
`maɪɡɹˈeɪʃən` and that "3:15" is "three fifteen". That step is called **G2P**,
grapheme-to-phoneme: letters (graphemes) in, sounds (phonemes) out.

Every ready-made English G2P used with these models is **espeak-ng**, which is GPL-3.0.
The voice model's own Python package calls it, Piper links it, and sherpa-onnx compiles it into its
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
| 6 | IPA characters mapped to the model's token ids | `daemon/tts_voice_tokens.c`, the model's symbol table | ours; table from the voice model (Apache-2.0) |
| 7 | Token ids and a voice's style vector to 24 kHz audio | `daemon/tts_voice.c`, the voice model run by ttskit (`ttskit/voice*.c`, `ttskit/data/voice.bin`) | ours; model Apache-2.0 |
| 8 | Audio encoded and stored | the daemon's render worker, libopus and `oc_mp4` | BSD-3 / ours |

ttskit is steps 2–5 and 7. It is linked by the daemon and by the tests, and by nothing that
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

## 5b. Sounds to audio: the voice model

The phonemes are spoken by a neural voice model run inside ttskit, in plain C
(`ttskit/voice_model.c` the forward pass, `ttskit/voice.c` its data file; its kernels are
nnkit's, `nnkit/nn_ops.c`, shared with voice input's recognizer, STTKIT.md). Where the model comes from, its licence and how its data file is made are
in [VENDORS.md](./VENDORS.md); here it is **the voice model**.

**What it is.** A StyleTTS 2-style model: a text side that decides how long each sound
lasts and how pitch and loudness move, and a waveform side that turns that into samples.
A sentence goes through it like this:

1. **Token ids.** The daemon maps ttskit's IPA to the model's 178 symbols
   (`daemon/tts_voice_tokens.c`), wrapped in the pad token and an end marker.
2. **A voice's style row.** Each of the eight voices is a table of 400 rows of 256 floats;
   the row is the sentence's length in characters, capped at the last. Its first half
   conditions the decoder and generator, its second half the prosody predictor.
3. **The context encoder,** a transformer whose one layer (12 heads, width 768) is applied
   12 times, reads the tokens. **The prosody predictor** (bidirectional LSTMs with
   style-conditioned layer norms between them) gives each token a duration: the sum of 50
   sigmoids, divided by the speed and rounded to whole frames of 25 ms. The tokens are then
   **aligned** to frames, each token's row repeated for its frames.
4. **The text encoder** (an embedding, three convolutions and an LSTM) is expanded to the
   same frames, and the predictor's residual blocks draw the **pitch (F0) and energy**
   curves at two values a frame.
5. **The decoder** (residual blocks with style-conditioned instance norms, AdaIN) combines
   the text, pitch and energy into 512 channels at two rows a frame.
6. **The generator** builds a **harmonic source** from the pitch — nine harmonics as sine
   waves, with noise mixed in (below), through a linear layer and tanh, and its short-time
   Fourier transform — then upsamples the decoder's output by 10 and 6 with transposed
   convolutions, adds the source at each rate, and runs residual blocks with Snake
   activations. A last convolution gives 11 log-magnitudes and 11 phases per hop of 5
   samples, and an **inverse STFT** (20-point) makes the waveform: 600 samples a frame,
   24 kHz mono.

The model is quantised as it ships: matrix products have int8 weights and convolutions
uint8, each run on 8-bit activations quantised from their own range as they arrive, with
exact integer sums; LSTMs are int8; the rest is float, and its half-precision weights are
widened to float when `voice.bin` is made, not as they are read.

**The output is raw.** `voice_say` returns what the model writes. The 20 Hz high pass that
takes out its offset, the trim of the noise it appends after the words and the 10 ms fade
are the daemon's (`daemon/tts_voice.c`, READ-ALOUD.md §4), not ttskit's.

**Plain C, one thread.** No intrinsics, no SIMD headers, no threads and no library: the
kernels are ordinary loops over blocks of 16 or 64 values, written so GCC's and clang's
auto-vectorisers turn them into vector code at `-O2`. Every ttskit and nnkit object is compiled with
`-ffp-contract=off`, so the compiler never fuses a multiply and an add behind the source's
back; where the arithmetic needs a fused multiply-add it is spelled out (`nn_fma`, exact
in double, settled by `fmaf` only on an exact tie). The samples are then the order the
source writes, the same on x86-64 and arm64 whatever the compiler. Measured on one core of
an i5-1335U, a render takes 0.81× its length in time built with GCC (`tts_pack bench`),
0.83–0.87× through `openchimed --tts-say`, and about 0.99× in a release build compiled with
zig's clang. One render peaks at about 156 MB resident, about 86 MB of it `voice.bin`'s
mapped pages, which the kernel may drop.

**The same numbers as the reference.** The forward pass does the arithmetic of the
model's graph in the order ONNX Runtime computes it, to the bit, up to the inverse STFT at
the very end: its quantisation and rounding, its integer products, its scale and bias in
its order, its norms, softmax, GELU, sigmoid, tanh, sin and atan approximations, its fused
multiply-adds and its vectorised sums. Anything less is not enough, for three reasons. A
token's frame count is a rounded sum, and the reference sentences come within 0.003 of a
rounding boundary, so an error nothing else would show moves a frame. From the decoder on
every convolution quantises its input with a scale taken from that input, so a last-bit
difference that moves one value across a rounding step changes a whole quantisation step,
and through the generator's residual blocks such steps multiply — a 1e-7 difference after
its first upsampling comes out at 25 dB. And the harmonic source's phase is a running sum
of the pitch, so any difference there grows along the sentence. With the noise off on both
sides, ttskit's renders of the reference sentences have a signal-to-noise ratio of 136–141 dB
against ONNX Runtime's (the test requires 40 dB, §9).

**The noise and its seed.** The model mixes random noise into its harmonic source: normal
noise on every sample of every harmonic, of standard deviation 0.003 where the pitch is
voiced (above 10 Hz) and 0.1/3 where it is not, and a uniform random starting phase for
harmonics 2 to 9. ttskit draws both from its own generator, SplitMix64 (`voice_rng`), with
normal values by the Box–Muller transform in double: `noise` 1 with a `seed`, and the same
seed gives the same audio, every time and on every machine. `noise` 0 leaves both out,
which is how parity is checked: the reference's two random operators cannot be reproduced,
and two of ONNX Runtime's own renders of one input agree to only 17–20 dB. The daemon
renders with the noise on and one fixed seed, `TTS_VOICE_SEED` (1, `daemon/tts_voice.h`),
so a message rendered again sounds the same as the first time; the seed is part of the
model version renders are cached under.

## 6. The data files

All three files are committed. The pronunciation data, `lexicon.bin` and `guesses.bin`,
lives in `ttskit/data/en-US/` — a directory per language, so adding one is adding a
directory rather than renaming files; the voice model's, `voice.bin`, in `ttskit/data/`
beside its licence. `openchimed` maps the copies in its read-aloud data directory and opens
them with `tts_load_mem` and `voice_open_mem`; `tts_load` and `voice_open` map them from a
path themselves (the tests and `tts_pack`). Either way nothing is copied: a lookup is a
binary search over a sorted table and reads only the pages it passes through, and a
render reads the weights where they lie; the kernel pages them in on demand, shares them
between processes and can drop them under pressure. Nothing is parsed at load and no copy
of the dictionary or the model is made on the heap — opening the pronunciation data costs a
few kilobytes of heap (the guesser's token index), and opening `voice.bin` reads its
header and index and checks every entry against the file.

| File | Size | gzipped | Contents |
|---|---|---|---|
| `lexicon.bin` | 3.8 MB | 1.2 MB | 126,052 words and their IPA, tagged `en-US` |
| `guesses.bin` | 16.6 MB | 7.3 MB | the order-6 joint n-gram model, tagged `en-US` |
| `../CMUDICT-LICENSE` (in `ttskit/data/`) | | | CMUdict's notice, which the derived data carries |
| `../voice.bin` (in `ttskit/data/`) | 86.9 MB | 61.2 MB | the voice model: 441 tensors (274 float, 93 int8, 74 uint8) and the eight voices' style tables |
| `../VOICE-LICENSE` (in `ttskit/data/`) | | | the voice model's licence (Apache-2.0), as its authors publish it |

Measured resident memory: a process that only spells is 1.5 MB; looking words up in the
dictionary adds about 1 MB of mapped pages; guessing an unknown word touches more of the
model, and a sentence with two guessed words peaks at 14 MB — all of it file-backed pages the
kernel may reclaim, not memory ttskit allocated.

All integers are little-endian. All three formats carry a magic and a version; a file with the
wrong magic, a different version, or sizes that do not add up to the file's length is refused.

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

**`voice.bin`**

```
header   128 bytes   "OCTTSVC1"  u32 version=1  u32 count  u32 header_size=128  u32 entry_size=96
                     u64 data_off  u64 file_size
                     u8 sha256[32] of the upstream model file  u8 sha256[32] of its voices file
                     zeros to 128
index    count x 96  { char name[48]  (NUL-terminated, NUL-padded)
                       u32 dtype  (1 f32, 2 uint8, 3 int8: ONNX's codes)  u32 ndim (1..4)
                       u32 dims[4]  (unused 0)  u64 offset  u64 bytes
                       f32 scale  i32 zero_point  (a quantised tensor's; 0 and 0 otherwise) }
                     sorted by the name's bytes
data     from data_off, the index's end rounded up to 64: each tensor at a multiple of 64 from
         the file's start, in index order, zeros between; the file ends with the last tensor
```

Every tensor starts 64-byte aligned, so the kernels read it in place; bytes a caller lends
must start 64-byte aligned too, as a mapping does. On opening, each entry is checked: a name
empty, unterminated, out of order or repeated, an unknown type, a rank outside 1..4, a size
that is not its shape's, or data outside the file or out of line, refuses the file.

The weights are stored in the layout the kernels run fastest on, not the model's
(`ttskit/tts_priv.h`, "Weight layouts"): quantised rows padded to a multiple of 16 channels
with the zero point, matrices transposed to one row per output, transposed convolutions
split by phase, half-precision weights widened to float. The arithmetic constants of the
graph (0.5, √2, the sample rate) are the forward pass's code, and norms whose scale is 1 and
bias 0 are left out.

**Names.** A tensor is named `<part>.<module>.<what>`, in ttskit's terms rather than the
upstream graph's (`daemon/voice_pack.c` has the full table):

| Part | What | Modules |
|---|---|---|
| `ctx` | the context encoder | `emb.tok`, `emb.pos`, `emb.type`, `emb.ln`; `in`; `layer.*` (the one layer: `q`, `k`, `v`, `o`, `ln1`, `ffn`, `ffn_out`, `ln2`); `out` |
| `pro` | the prosody predictor | `enc.N.lstm`, `enc.N.ada`; `dur.lstm`, `dur.proj`; `lstm`; `f0.N`, `n.N`, `f0.proj`, `n.proj` |
| `txt` | the text encoder | `emb`, `cnn.N.conv`, `cnn.N.ln`, `lstm` |
| `dec` | the decoder | `f0`, `n` (the curves' stride-2 convolutions), `asr`, `enc`, `dec.N` |
| `gen` | the generator | `src.harm`, `src.lin` (the harmonic source); `stft.re`, `stft.im`; `noise.N`, `nres.N`, `res.N`; `up.N`; `post`; `istft.re`, `istft.im` |
| `voice` | a voice | `voice.<id>`: its style table, 400 rows of 256 (`voice.expr-voice-2-m` …) |
| `norm` | | `norm.eps`: the epsilon of every norm without an affine part |

A residual block has `conv1`, `conv2`, `ada1` and `ada2` (the style projections of its two
adaptive norms), `skip` (the 1×1 shortcut, where the width changes) and `pool` (the
depthwise transposed convolution that doubles its length, where it upsamples). A generator
block has `c1.J` and `c2.J`, `ada1.J` and `ada2.J`, and its Snake activations' `alpha1.J`,
`alpha2.J`, `inv1.J` and `inv2.J`. The last part says what the tensor is: `w` a weight (a
quantised one's scale and zero point in its entry), `b` a bias, `g` a layer norm's gain,
`eps` its epsilon; an LSTM's are `fwd.*` and `bwd.*`, each with `w` (input), `r`
(recurrent) and `b`.

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

**Speaking.** The voice model takes token ids, which the daemon makes from the IPA
(`tts_voice_tokens` in `daemon/tts_voice_tokens.c`):

```c
char err[256];
voice *v = voice_open("ttskit/data/voice.bin", err, sizeof err);
if (!v) { fprintf(stderr, "voice: %s\n", err); return 1; }

float style[VOICE_STYLE_DIM];
voice_style(v, "expr-voice-2-f", 25, style);          /* row 25: a 25-character sentence */

size_t n;
float *pcm = voice_say(v, ids, tokens, style, 1.2f, 1, 1, &n, err, sizeof err);
/* n samples at VOICE_RATE (24 kHz), the raw waveform; NULL with a reason on failure */
free(pcm);
voice_close(v);
```

| Call | Does |
|---|---|
| `voice_open(path, err, cap)` | maps `voice.bin` and checks its header and every index entry (§6); NULL with the reason for a file that is not `voice.bin` of this version, is cut short, or has an entry that points outside it or out of line. |
| `voice_open_mem(bytes, len, err, cap)` | the same over bytes the caller lends, which must start 64-byte aligned and outlive the handle; nothing is copied. |
| `voice_close(v)` | unmaps (or lets go of lent bytes). |
| `voice_style(v, id, row, style)` | row `row` of voice `id`'s style table into 256 floats (past the last row, the last); -1 for a voice the file does not have. |
| `voice_say(v, ids, tokens, style, speed, noise, seed, &samples, err, cap)` | renders the token ids (0..177) at `speed` (1 the model's own rate, larger faster), with the noise on from `seed` or off (§5b): a malloc'd array of floats, the raw waveform, which the caller frees. NULL with the reason for a token out of range, more than 512 tokens, damaged data or no memory. |
| `voice_tensor_get`, `voice_tensor_count`, `voice_tensor_at` | a tensor by name, or all of them in index order, for tools and tests. |

A `voice` is read-only after opening; `voice_say` allocates its working memory per call and
frees it before returning, so one handle can serve several threads, each rendering on its
own. The daemon renders on one.

**From the command line,** `make build/tts_pack` builds a small tool that uses the same code:

```
$ build/tts_pack word ttskit/data/en-US migration Kubernetes API readme
migration   maɪɡɹˈeɪʃən        lexicon
Kubernetes  kjˈuːbɚnˈɛtiːz     guessed
API         ˈeɪ pˈiː ˈaɪ       spelled
readme      ɹiːədm             guessed

$ build/tts_pack text ttskit/data/en-US "It's 5 o'clock"
ˈɪts fˈaɪv əklˈɑːk
```

With the voice model, `DIR` is laid out as the daemon's data directory: `voice.bin` at its
top and the pronunciation data in `en-US/` (`ttskit/data` in the tree, `tts/` beside a built
daemon). The fixtures are the reference renderings (§9):

```
build/tts_pack say ttskit/data VOICE TEXT out.wav    # the model's own output, noise on, as a WAV
build/tts_pack parity ttskit/data tests/data/voice    # §9's parity, as a table
build/tts_pack bench ttskit/data tests/data/voice     # the real-time factor and peak memory
build/tts_pack bench tests/data/voice                 # the kernels alone, timed on the model's shapes
```

`say` writes the model's raw output, before read-aloud's high pass and tail trim;
`openchimed --tts-say VOICE TEXT OUT.wav` in a read-aloud build speaks exactly as read-aloud
does, those included (READ-ALOUD.md §4).

## 8. Building the data from sources

The committed files are the output of one script. Rerun it when CMUdict's pin, the IPA table
(§3), the packer or the training settings change; nobody needs to run it to build OpenChime.

```
scripts/build/ttskit_data.sh            # rebuild in build/ttskit-data and compare with the committed files
scripts/build/ttskit_data.sh install    # rebuild and replace them
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

**After `install`:** update the two sums in `tests/test_ttskit.c` and in this section,
update `OC_TTS_MODEL_VERSION` in `daemon/tts.h` (its `ttskit-<lexicon 8>.<guesses 8>` part,
the first eight hex digits of each new sum), run `make test` (the agreement test reads the
new reference), and listen to rendered speech before committing — a changed table changes
how every message sounds. The model version is what renders are cached under, so changing
it makes existing renders again rather than mixing them; it does not change by itself.

**`voice.bin`** is not made by this script. It is converted from the upstream model's two
files by the daemon, `openchimed --voice-pack <model.onnx> <voices.npz> <out>`
(`daemon/voice_pack.c`), in any build, with or without read-aloud. The converter reads the
ONNX file and the stored zip itself, with no library; takes each tensor it expects by its
place in the graph, checking its type, shape and the settings of the operator that uses it;
refuses inputs whose tensors are not the ones it expects, naming the first; and writes the
same bytes on every run and machine. [VENDORS.md](./VENDORS.md) gives the pinned inputs, the
commands and the SHA-256 the result must have. After converting a different model, update
the `voice-` part of `OC_TTS_MODEL_VERSION` in `daemon/tts.h` (the first eight hex digits of
the new file's SHA-256), remake the references (`tests/manual/voice_ref.c`, §9) and listen,
as above.

## 9. Tests

`tests/test_ttskit.c`, in `make test`:

- the ARPAbet→IPA table and each context rule in §3;
- number, time, money, ordinal, year and abbreviation expansion;
- dictionary hits, guesses, spelling of initialisms, and words with no letters;
- sentences, with weak forms, capitals and pause marks;
- loading without data (everything spelled);
- both data files against their SHA-256 pins;
- the C guesser against Phonetisaurus's decoder on all 3,010 reference words (at least 999
  in 1,000 identical; all but one are);
- truncated files, a wrong magic and a newer version, each refused on load with a reason;
- the duration rounding and the alignment of tokens to frames (the kernels the model runs
  on — the 8-bit quantisation, the integer matrix product and convolution, the LSTM, layer
  and instance norms, attention, the transposed convolutions, the float matrix product, the
  activations and the inverse STFT — are tested in `tests/test_nnkit.c`, each against a
  naive version of the operator's definition on random inputs);
- `voice.bin`: a small file built byte by byte loads and says what was written, and one with
  a wrong magic, a later version or a wrong length is refused; the converter, on a small
  model and voices built in the upstream formats, puts each tensor where its kernel reads it,
  and refuses a model or voices that differ from what it expects, naming the difference;
- the committed `ttskit/data/voice.bin` loads and holds every voice the daemon names, and a
  copy damaged in its magic, version, length or an entry's offset is refused.

**Against the reference,** the full suite's `voice_parity` (`tests/test_voice.c`, `FULL` in
`tests/main.c`) renders each fixture in `tests/data/voice/` — six sentences, in
`expr-voice-2-m` and `expr-voice-2-f`, with numbers, punctuation and a long one — from the
committed `voice.bin`. With the noise off, each has the reference's sample count and at least
40 dB SNR against ONNX Runtime's noise-off rendering (measured: 136–141 dB); with it on, the
variance of the noise the harmonic source adds is within 10% of what the model's operators
specify for the same input. `build/tts_pack parity ttskit/data tests/data/voice` prints the
same as a table.

The references were made once by `tests/manual/voice_ref.c`, which runs the upstream model
file with ONNX Runtime 1.30.0's prebuilt release on one thread (VENDORS.md has the files),
and are committed: per fixture the token ids (`.ids`), the raw output with the noise on
(`.f32`) and with the model's two random operators replaced by zeros (`.quiet.f32`), and one
`manifest` with the voice, style row, speed, sample counts and the SNR between two noisy
runs, kept as a record (17–20 dB). No suite runs that program; rerun it only to remake them.

## 10. Limits

- **English only,** US pronunciation.
- **Heteronyms** take the dictionary's first pronunciation: "read" is always `ɹˈɛd`, "live"
  one way whatever the sentence means.
- **The guesser is a guess:** about two words in three exactly right on words it never saw,
  and most of the rest off by a sound. Names and made-up words are where it is heard.
- **Mixed letters and digits** ("r2d2", "v2") are read as the letters around the digits.
- **No secondary stress, no sentence intonation marks:** the voice model supplies the melody
  from punctuation.
- **One voice model, fixed:** its eight voices, 24 kHz, at most 512 tokens a call (the
  daemon renders a sentence of at most 120 characters at a time). A different model is a
  new converter table and forward pass, not a setting.
- **One thread, and only just faster than real time** in a release build (about 0.99×,
  §5b), so a message takes about as long to render as to hear. No threads in the kernels,
  by choice: renders are cached, and the render worker is one thread.
- **No streaming:** a sentence is rendered whole before any of it is returned.
- **Bit-exact only with the noise off,** and only to the inverse STFT's last roundings: with
  the noise on, ttskit's generator is not ONNX Runtime's, so a render matches the
  reference's sound, not its samples.
