# sttkit — the recognizer for voice input

`sttkit` is an in-tree C library that turns 16 kHz speech into text: voice input's
recognizer (ARCH-112, [VOICE-INPUT.md](./VOICE-INPUT.md)). The daemon hands it one
utterance's samples and gets back its words. It has no dependencies beyond `nnkit`, the
kernels it shares with ttskit ([TTSKIT.md](./TTSKIT.md) §5b), and reads its one data file,
`recognizer.bin`, where it lies — memory-mapped, never copied.

Where the model comes from, its licence and how its data file is made are in
[VENDORS.md](./VENDORS.md); here it is **the recognizer**.

## 1. Why it exists

The recognizer is published as ONNX Runtime graphs. Running them took ONNX Runtime: a large
C++ library built from source for every voice-input build, whose source is no longer
published where the build fetched it. sttkit runs the same model, with the same int8 weights,
in plain C under ttskit's rules: one thread, no intrinsics, no library, loops the compiler
vectorises on its own, and `-ffp-contract=off`.

## 2. Where it sits

| Step | What happens | Where |
|---|---|---|
| 1 | The client cuts speech into segments and sends their samples | `client/core/voice/` |
| 2 | The daemon queues a segment and hears it in windows of at most 6 s | `daemon/stt_worker.c` |
| 3 | A window's samples to token ids | `sttkit/recognizer_model.c`, `nnkit/` |
| 4 | Token ids to text | `sttkit/stt_tokens.c` |
| 5 | Spoken mentions, then posted or returned | `daemon/stt_mentions.c`, `daemon/stt.c` |

## 3. The model

An encoder-decoder transformer of 34 million parameters, width 320, 6 layers on each side,
8 heads of 40, a vocabulary of 32,768. Its matrix products have 8-bit weights, each output
column with its own scale and zero point; their inputs are quantised to 8 bits as they
arrive, from their own range (ONNX's `DynamicQuantizeLinear`), and summed exactly in 32-bit
integers. The rest is float. An utterance goes through it like this:

1. **The frontend**, over frames of 80 samples (5 ms). Each frame is normalised to zero
   mean and unit variance over its own 80 samples (epsilon from the model), scaled by a
   learned constant and compressed by `asinh`; a linear layer (80 to 320, no bias) and SiLU
   follow. Two causal convolutions, each with kernel 5 and stride 2 and a SiLU after the
   first (320 to 640, then 640 to 320), take it to one feature row every 20 ms. The
   published graph runs in chunks of 1,280 samples with each convolution's last 4 input
   rows carried to the next chunk; over a whole utterance padded with silence to a whole
   chunk this is the same as running the convolutions once over 4 rows of zeros and every
   frame, which is what sttkit does.
2. **The encoder**, 6 pre-norm blocks over the feature rows. Each block: layer norm (no
   bias) and attention (8 heads, scores scaled by 1/sqrt(40)) whose window is 16 rows back
   and, in blocks 0, 1, 4 and 5, 4 rows ahead (16 rows of lookahead in all, 320 ms), its
   output projection added back; then layer norm and a feed-forward layer (320 to 1,280,
   exact erf GELU, back to 320, with biases) added back. A last layer norm ends it. There
   are no position embeddings: the windows are the only order the encoder sees.
3. **The adapter** adds a learned position embedding (4,096 rows, int8) to each encoded
   row, giving the decoder's memory.
4. **Cross-attention keys and values** are each decoder layer's two projections of the
   memory, made once per utterance.
5. **The decoder**, greedy, one token per step from the start token (1) to the end token
   (2). The token's embedding (int8, the same table the output layer uses) goes through 6
   blocks: layer norm and self-attention over every earlier step, whose query and key have
   rotary positions on their first 32 of 40 dimensions, rotating adjacent pairs; layer norm
   and cross-attention over the memory; layer norm and a gated feed-forward layer (320 to
   2 x 1,280, the first half times SiLU of the second, back to 320, with biases). Each adds
   back. A last layer norm and the embedding table, transposed, give the logits; the
   largest is the next token. Each step's keys and values are kept for the next.
6. **Decoding stops** at the end token or at 6.5 tokens a second of audio (at most 256),
   the model's guard against a decoder repeating itself.

Every layer norm in the model has a scale of 1 and no bias, followed by a learned gain;
sttkit applies the gain as the norm's scale.

The graphs these layers were read from, with every constant and node, are printed by
`openchimed --recognizer-pack --list DIR` over the published files.

## 4. The same numbers as the reference

**Plain C, one thread, nnkit's kernels.** The kernels are nnkit's (`nnkit/nn_ops.c`),
shared with ttskit: ordinary loops written for the compiler's auto-vectoriser, every
nnkit and sttkit object compiled with `-ffp-contract=off`, and fused multiply-adds spelled
out where the arithmetic needs them (`nn_fma`). The recognizer adds to them the products
with a zero point and a scale per output column (`nn_matmul_qc`, `nn_dequant_qc`), SiLU,
the exact GELU, rotary positions, single-query attention over a cache, argmax and the
output layer against the uint8 embedding table.

**To the bit, where it counts.** Every product quantises its input from that input's own
range, so a last-bit difference anywhere upstream can move one value across a rounding
step, and through the encoder's six layers such steps spread over the utterance: with the
frontend computed in an ordinary order (120 dB from the reference) the encoder's output
came out only 20 dB from ONNX Runtime's, though the words were the same. So the frontend
and the encoder do the arithmetic of the published graphs in the order ONNX Runtime
computes it, to the bit: a frame's mean and variance as Eigen sums them (`nn_reduce_sum`);
the linear layer as a chain of fused multiply-adds; each convolution as its im2col and
matrix product compute it for one chunk — the window laid out channel by channel, tap by
tap, summed in blocks of 1,024, then the bias; the quantised products exactly; the
attention scores over every key with the masked ones at minus infinity, its softmax and its
weighted sum as ONNX Runtime's float matrix product adds them (`nn_attend_exact`); erf as its
own approximation (`nn_gelu_erf`); and the last norm as SkipLayerNormalization's. Over the
reference fixtures the encoder's output is then identical to ONNX Runtime's, and the first
step's logits are 129–137 dB from it (the decoder's one-row products are summed in another
order); every token is the same (§8).

**Speed and memory.** Measured on one core of an i5-1335U, hearing takes about 0.25× the
length of the speech (`stt_check bench`), and hearing a 6 s segment peaks at about 53 MB
resident, 40 MB of it `recognizer.bin`'s mapped pages, which the kernel may drop.

## 5. The data file: recognizer.bin

One file, committed as `sttkit/data/recognizer.bin` with the model's licence beside it
(`sttkit/data/RECOGNIZER-LICENSE`); the build copies both into `stt/` beside the daemon,
whose manifest checks them. It is made from the eight published files by
`openchimed --recognizer-pack DIR OUT` (VENDORS.md has the pinned inputs and the exact
commands): the same bytes on every run and machine.

```
header (320 bytes)   "OCSTTRC1"  u32 version (1)  u32 count  u32 header size (320)
                     u32 entry size (96)  u64 data offset  u64 file size
                     u8 sha256[8][32]: the published files, in recognizer_pack.h's order
index                count x { char name[48]  u32 dtype (1 f32, 2 uint8, 6 int32)
                     u32 ndim (1..4)  u32 dims[4]  u64 offset  u64 bytes
                     f32 scale  i32 zero point }, sorted by name
data                 each tensor at a multiple of 64 from the file's start, in index order
```

Names are sttkit's own: `fe.*` the frontend (`fe.lin.w` the linear layer, f32 [80][320];
`fe.c1.w`, `fe.c2.w` the convolutions, f32 [cin x 5][cout], channel then tap, as their
im2col lays a window out; their biases `.b`; `fe.comp` and `fe.eps`), `enc.N.*` and `dec.N.*`
the layers (`q`, `k`, `v`, `o`; `cq`, `co`, `xk`, `xv` the decoder's cross-attention; `f1`,
`f2` the feed-forward layers; `g1`… the gains after each norm), `enc.g` and `dec.g` the last
gains, `dec.rope` the rotary frequencies, `pos` the adapter's position table and `emb` the
embedding (uint8, their scale and zero point in their entries), `eps` and `alpha` the
norms' epsilon and the attention's score scale, `tokens` the tokenizer as published, and
`cfg` the dimensions (`sttkit.h`, `REC_CFG_*`, the encoder's windows among them). A
quantised product's weight is `<name>.w`, uint8 [n][k] — one row per output column — with
`<name>.s` its scales and `<name>.zp` its zero points, and `<name>.b` its bias when it has
one. The loader checks the header, every entry against the file, and that every tensor the
forward pass reads is there with the shape it reads; anything else is refused with the
reason.

## 6. Using it

```c
#include "sttkit.h"

char err[256];
recognizer *r = recognizer_open("sttkit/data/recognizer.bin", err, sizeof err);
char *text = NULL;
if (r && recognizer_hear(r, pcm, samples, &text, err, sizeof err) == 0) {
    puts(text);                     /* "Good morning, everyone." */
    free(text);
}
recognizer_close(r);
```

`pcm` is `samples` floats in [-1, 1] at 16 kHz. `recognizer_open_mem` takes bytes the
caller has mapped (64-byte aligned), as the daemon does (`daemon/stt.c`). A handle is
read-only once open, so one may serve several threads.

## 7. Rebuilding the data

`recognizer.bin` changes only when the model does. The published files are pinned by
SHA-256 (VENDORS.md); `openchimed --recognizer-pack` refuses any file whose graphs are not
the ones it expects, naming the first difference. A newer model is a change to sttkit:
list its graphs with `--recognizer-pack --list`, follow them in `recognizer_model.c`, make
new references with `tests/manual/recognizer_ref.c`, and change `OC_STT_MODEL_VERSION`
(`daemon/stt.h`) so clients see a new model.

## 8. Tests

- `tests/test_nnkit.c` (fast): each kernel against a naive version of its operator on
  random inputs.
- `tests/test_sttkit.c` (fast): the tokenizer and the token ceiling; `recognizer.bin`'s
  loader on a file built from §5's description, and a wrong magic, version, length or
  shape, or a missing tensor, each refused; the committed file loading; the converter
  refusing what is not the published model.
- `recognizer_parity` in `tests/test_sttkit.c` (full suite): every fixture in
  `tests/data/recognizer/` heard from the committed file gives the reference's token ids,
  with its encoder output and first logits within 30 dB SNR of ONNX Runtime's. The
  fixtures — seven utterances spoken by read-aloud's own voice, one of them fast enough to
  reach the token ceiling, and silence — were made once by `tests/manual/recognizer_ref.c`
  with ONNX Runtime's prebuilt release, which no suite links.
- `build/stt_check parity sttkit/data tests/data/recognizer` prints the same check as a
  table; `build/stt_check bench sttkit/data tests/data/recognizer` the real-time factor and
  peak memory.

## 9. Limits

English only, one model. Greedy decoding, no beam search. An utterance is heard whole, at
most 4,096 feature rows (about 82 s); the daemon hears long segments in windows of at most
6 s (VOICE-INPUT.md §6). No threads in the kernels.
