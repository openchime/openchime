# sumkit — the summary inference engine

`sumkit` is an in-tree C library that runs the summary model: a language model of the
Qwen3.5 family, inside the daemon, on one thread (ARCH-116, [SUMMARIES.md](./SUMMARIES.md)).
The summary worker hands it a system and a user prompt and a grammar, and gets back the
answer. It has no dependencies beyond `nnkit`, the kernels it shares with ttskit and sttkit,
and utf8proc for its tokenizer, and reads one data file, `summary.bin`, where it lies —
memory-mapped, never copied.

Where the model comes from, its licence and how its file is made are in
[VENDORS.md](./VENDORS.md).

## 1. Why it exists

Summaries ran on a C++ library built from source with CMake for every summary
build, a C++ runtime linked into the daemon, an x86-64-v3 requirement, and a model file
someone else had converted and quantised. sumkit runs the same model in plain C under
nnkit's rules — one thread, no intrinsics, no library, loops the compiler vectorises on its
own, `-ffp-contract=off` — on any 64-bit CPU, from a file the daemon makes itself out of
Qwen's own release.

## 2. Where it sits

| Step | What happens | Where |
|---|---|---|
| 1 | First start: Qwen's files fetched, each checked, converted, the result checked | `daemon/sum_fetch.c`, `daemon/summary_pack.c` |
| 2 | The worker builds a call's prompt and grammar | `daemon/sum_core.c`, `daemon/sum_worker.c` |
| 3 | The chat format, then text to tokens | `sumkit/lm_tokens.c` |
| 4 | Tokens to logits | `sumkit/lm_model.c`, `nnkit/` |
| 5 | Each next token: greedy after DRY, inside the grammar | `sumkit/lm_sample.c`, `sumkit/lm_grammar.c` |
| 6 | The engine table the worker calls | `daemon/sum_sumkit.c` |

## 3. The model

A decoder of the Qwen3.5 family; every dimension is read from the model's `config.json` by
the converter and from `summary.bin`'s header by sumkit, so a larger model of the family is a
conversion, not new code. The 2B: 24 layers of width 2,048, a feed-forward layer of 6,144,
a vocabulary of 248,320 (248,070 tokens used). Each layer is RMS norm, a mixer, a residual
add, RMS norm, a gated feed-forward layer (`down(silu(gate(x)) * up(x))`) and a residual add.
Every norm's weight is stored as the model computes it, 1 + its published value. Three of
every four layers' mixer is linear attention, the fourth gated attention
(`full_attention_interval`):

1. **Linear attention (gated DeltaNet).** One projection to q, k and v (16 key heads and 16
   value heads of 128 for the 2B), a causal depthwise convolution over time (kernel 4, its
   last 3 inputs carried) and SiLU; q and k L2-normalised per head. Per value head, a
   128 x 128 state S carried from token to token: `S *= exp(g)`, `u = beta (v - Sᵀk)`,
   `S += k uᵀ`, `o = Sᵀq / sqrt(128)`, with `g = softplus(a + dt) * A` (A = -exp(A_log),
   stored as such) and `beta = sigmoid(b)`, a and b the token's projections. The output is
   RMS-normed per head, multiplied by SiLU of a further projection z, and projected back.
   With more value heads than key heads, value head h reads key head h / (hv / hk).
2. **Gated attention.** A projection to each of 8 heads' query and gate (256 each), keys and
   values for 2 heads; query and key RMS-normed per head, rotary positions on their first 64
   values (pairs i and i + 32, base 10,000,000 — the model's multi-axis positions are one
   position for text), attention over the cache, the result multiplied by sigmoid(gate) and
   projected back. The cache is kept at 8 bits, a scale for each 32 values, as the previous
   engine's cache was.

The vision tower and the multi-token-prediction layer Qwen publishes with it are not used
and not converted. The output layer is the token embeddings.

## 4. The file and its quantisation

`summary.bin` is a header, a sorted index and 64-byte-aligned tensors; `sumkit/sumkit.h` has
the format. The tokenizer's tables are tensors of the same file. The matrices are stored row
by row in one of four types (nnkit.h): two quantised, whose 256-value blocks are byte for
byte ggml's `Q4_K` and `Q6_K`, and bfloat16 and float32. The converter quantises with ggml's
reference algorithms, without an importance matrix, and gives the same bytes on every run and
machine, so `daemon/sum_fetch.h` pins the result's SHA-256.

**The plan** (`daemon/summary_pack.c`): 4 bits for the feed-forward layer's gate and up
projections, which are most of the weights; 6 bits for every other matrix; norms and the
linear attention's small vectors and a, b projections in float32. 1,399,305,116 bytes.

**How it was chosen: the KL divergence** of its next-token distributions from the
unquantised model's, over held-out text (the daemon's own prompts over two synthetic
channels, `tests/data/summary/kl.txt`, 4,427 tokens, 8 chunks of 512 scored on their second
halves, as perplexity tools score them), held to no more than that of Unsloth's
`Q4_K_M` — the file the daemon ran on its previous engine — against the same unquantised weights:

| File | Mean KL divergence |
|---|---|
| Unsloth's `Q4_K_M` (importance matrix, 4, 5 and 6 bits) | 0.020438 |
| 4 bits but for the embeddings, qkv, the linear attention's output, values and some down projections | 0.029209 |
| **The plan above** | **0.016463** |

## 5. The answer: grammar, DRY, greedy

Every local answer is held to the grammar its call builds (SUMMARIES.md §2). sumkit reads the
GBNF subset `daemon/sum_core.c` writes — rules, string literals, character classes with
ranges and negation, alternation, grouping, rule references, `?`, `*`, `+`, `{m}`, `{m,}`,
`{m,n}` — and refuses anything else as a prompt failure. Matching is over code points: the
state is a set of stacks, each the rest of what may follow, popped down to its next character
class; a token is allowed when its bytes take some stack through (a token ending inside a
character is allowed when its first byte could start one). An end token is allowed only where
the grammar can end.

Each token is chosen greedily after DRY ("don't repeat yourself", multiplier 0.8, base 1.75,
allowed length 2, colons, quotes and asterisks breaking sequences, over the answer so far):
the candidates best first, the first the grammar allows taken — the same choice as masking
the vocabulary, without testing all of it.

The prompt is ChatML with the empty thought (`<think>\n\n</think>\n\n`), so the model
answers straight away; the chat format's tokens in it are read as such.

## 6. Checked against the reference

`build/sum_check` (`make build/sum_check`) runs these, on this machine, over the references in
`tests/data/summary/`, which were made once from the same unquantised weights, before the
engine they were made with was withdrawn. They cannot be made again; the committed files are
the reference. A changed model is checked without them: by the naive forward pass in the
tests (`tests/test_sumkit.c`) and by §4's KL divergence against its own unquantised run,
neither of which needs another engine. Over this model:

- **`sum_check parity tests/data/summary FULL.bin`**, with `FULL.bin` from
  `openchimed --summary-pack --full`: for each of three prompts (notes on a channel, the
  summary of another, a rewrite), the first step's logits within 30 dB SNR of the reference's,
  and, along the reference's answer, the largest logit the reference's wherever the
  reference's top two differ by more than 0.1. Measured: 39.2, 41.0 and 38.2 dB; 85 steps
  checked, none differing.
- **`sum_check kl tests/data/summary SUMMARY.bin FULL.bin`**: §4's measure, which passes when
  the file's KL divergence is no more than Unsloth's (recorded in the manifest). It keeps the
  unquantised side beside `FULL.bin` as `FULL.bin.kl` and uses it again while it is current.
- **The tokenizer**: the reference's token ids for texts with accents, other scripts, emoji,
  digits, whitespace runs, contractions and the chat format's tokens, and for the three
  templated prompts — checked in the fast suite (`tests/test_sumkit.c`) against the
  tokenizer's tables alone, `tests/data/summary/tokenizer.bin` (`sum_check tokenizer`).

The fast suite also runs a small model of the family (`tests/sumkit_fixture.c`), converted,
against the same model written naively in double; the same tokens read at once, in pieces and
one at a time give the same bits.

## 7. Speed and memory

`sum_check bench tests/data/summary SUMMARY.bin`: the notes prompt (1,137 tokens) read, then
64 tokens written, on one core of the development machine (x86-64, WSL2), against the
previous engine on the same core with Unsloth's file, measured before it was withdrawn:

| | Reading the prompt | Writing the answer | Peak resident |
|---|---|---|---|
| sumkit, `summary.bin` | 12.0 tokens/s | 3.05 tokens/s | 1,375 MB |
| the previous engine, Unsloth's `Q4_K_M` | 38.9 tokens/s | 9.4 tokens/s | — |

The quantised products are the cost. Their sums are exact integers, so the kernels can be made
faster without changing a single result: a token at a time they work straight from a block's
bytes; several at a time they unpack 16 rows' blocks once, as 16-bit values times their
sub-block scales, and read each token's activations once for all 16 (one multiply-add-pairs
sum a block). Vector intrinsics are out of scope (ARCH-116); a kernel written with them could
be held to the plain one bit for bit, because the sums are integers.
