/*
 * OpenChime — sumkit: the summary inference engine, in plain C (ARCH-116,
 * docs/details/SUMKIT.md).
 *
 * Runs a language model of the Qwen3.5 family -- gated DeltaNet layers and
 * gated attention layers -- converted into one file, summary.bin, by
 * `openchimed --summary-pack` (daemon/summary_pack.c). The file is used where
 * it lies (mapped), never copied. One thread, no intrinsics, no library: the
 * kernels are nnkit's, shared with ttskit and sttkit. Nothing here is fixed to
 * one size of model: every dimension comes from the file.
 */
#ifndef SUMKIT_H
#define SUMKIT_H

#include <stddef.h>
#include <stdint.h>

/* ---- the model's data: summary.bin ---------------------------------------------
 *
 * header (512 bytes)  magic "OCSUMKT1", u32 version, u32 count, u32 header size
 *                     (512), u32 entry size (96), u64 data offset, u64 file
 *                     size; at 40, u8 sha256[3][32] of config.json,
 *                     tokenizer.json and the safetensors file; at 136, u32
 *                     full (0 quantised, 1 unquantised); at 140 the model's
 *                     dimensions, u32 each, in sk_config's order (n_layer ..
 *                     conv_kernel); at 192, u64 the linear-attention layers
 *                     (bit i: layer i); at 200, f32 rms_eps, f32 rope_theta;
 *                     at 208, u32 n_eog, u32 eog[4]; at 228, u32 n_tok, u32
 *                     n_merges; zeros to 512
 * index               count x { char name[48] (NUL-terminated, NUL-padded),
 *                     u32 dtype, u32 ndim, u32 dims[4] (unused 0), u64 offset,
 *                     u64 bytes, zeros to 96 }, sorted by name bytes
 * data                from the data offset (the index's end rounded up to 64),
 *                     each tensor at a multiple of 64 from the file's start,
 *                     zeros between; the file ends at the last tensor's end
 *
 * All integers little-endian. A matrix is dims {rows, cols}: one row per
 * output, its type's row layout (nnkit.h) -- F32, BF16, Q4K or Q6K, the same
 * codes as nn_wtype. The tensors:
 *
 *   embed                 [n_vocab][n_embd]  the token embeddings, and the
 *                                            output layer (tied)
 *   norm                  F32 [n_embd]       the last norm
 *   Lnn.in_norm, Lnn.post_norm  F32 [n_embd]  layer nn's two norms (as the
 *                                            model computes them: 1 + weight)
 *   Lnn.ffn_gate, Lnn.ffn_up  [n_ff][n_embd]; Lnn.ffn_down [n_embd][n_ff]
 *   linear attention:     Lnn.qkv [2 kd + vd][n_embd] (q, k, v, each head after
 *                         head), Lnn.z [vd][n_embd], Lnn.a and Lnn.b F32
 *                         [hv][n_embd], Lnn.conv F32 [conv_kernel][2 kd + vd],
 *                         Lnn.dt F32 [hv], Lnn.A F32 [hv] (-exp(A_log)),
 *                         Lnn.gnorm F32 [dv], Lnn.out [n_embd][vd]
 *                         (kd = lin_k_heads x lin_k_dim, vd = lin_v_heads x
 *                         lin_v_dim, hv = lin_v_heads, dv = lin_v_dim)
 *   attention:            Lnn.q [2 n_head head_dim][n_embd] (each head's query
 *                         then its gate), Lnn.k and Lnn.v [n_head_kv
 *                         head_dim][n_embd], Lnn.o [n_embd][n_head head_dim],
 *                         Lnn.q_norm, Lnn.k_norm F32 [head_dim]
 *   tok.bytes  U8 [..]    every token's bytes, end to end
 *   tok.offs   I32 [n_tok + 1]  where token i's bytes start; the last, the end
 *   tok.merges I32 [n_merges][3]  left, right, result: the merges by rank
 *   tok.added  I32 [..]   the tokens matched whole in text before splitting
 */
#define SK_MAGIC      "OCSUMKT1"
#define SK_VERSION    1u
#define SK_HEADER     512u
#define SK_ENTRY      96u
#define SK_ALIGN      64u
#define SK_NAME_MAX   48
#define SK_MAX_DIMS   4
#define SK_MAX_LAYERS 64
#define SK_MAX_EOG    4
#define SK_INPUTS     3
enum { SK_F32 = 0, SK_BF16 = 1, SK_Q4K = 2, SK_Q6K = 3, SK_U8 = 4, SK_I32 = 5 };

typedef struct {
    uint32_t n_layer, n_embd, n_ff, n_vocab;
    uint32_t n_head, n_head_kv, head_dim, rot_dims;
    uint32_t lin_k_heads, lin_v_heads, lin_k_dim, lin_v_dim, conv_kernel;
    uint64_t linear;                /* bit i: layer i is linear attention */
    float    rms_eps, rope_theta;
    uint32_t n_eog, eog[SK_MAX_EOG]; /* the tokens that end an answer */
    uint32_t n_tok, n_merges;
} sk_config;

typedef struct {
    const char *name;
    uint32_t    dtype, ndim, dims[SK_MAX_DIMS];
    const void *data;
    uint64_t    bytes;
} sk_tensor;

typedef struct sk_file sk_file;

/* Bytes of a tensor of this type and shape (rows of the last dimension), or 0
 * when the type cannot hold it (a quantised row that is not whole blocks). */
uint64_t sk_tensor_bytes(uint32_t dtype, uint32_t ndim, const uint32_t *dims);

/* Map only the tokenizer of a summary.bin (or of a file holding just its
 * header and tok.* tensors): the header and the tokenizer's tables are
 * checked, the model's tensors are not looked for. */
sk_file *sk_open_tokenizer(const char *path, char *err, size_t errcap);
/* Map summary.bin at `path` and check it: the header, every index entry
 * against the file, and every tensor the model needs, with the shape and a
 * type it can be run in. NULL with a reason. */
sk_file *sk_open(const char *path, char *err, size_t errcap);
/* The same over `n` bytes the caller holds (64-byte aligned), not copied. */
sk_file *sk_open_mem(const void *p, size_t n, char *err, size_t errcap);
void     sk_close(sk_file *f);
const sk_config *sk_config_of(const sk_file *f);
int      sk_full(const sk_file *f);
/* The input SHA-256 recorded at index i < SK_INPUTS. */
const uint8_t *sk_input_sha(const sk_file *f, int i);
/* Tensor `name`, or NULL. */
const sk_tensor *sk_find(const sk_file *f, const char *name);
size_t   sk_count(const sk_file *f);
const sk_tensor *sk_at(const sk_file *f, size_t i);


/* ---- the tokenizer ---------------------------------------------------------------- */
typedef struct sk_tokenizer sk_tokenizer;
sk_tokenizer *sk_tokenizer_new(const sk_file *f, char *err, size_t errcap);
void sk_tokenizer_free(sk_tokenizer *t);
/* `text` as token ids, the added tokens (the chat format's) matched whole
 * wherever they appear: a heap array at *out and its length, or -1. */
long sk_tokenize(const sk_tokenizer *t, const char *text, int32_t **out);
/* The tokens' bytes into `out` (NUL-terminated within `cap`): the length, or
 * -1 when they do not fit. Ids outside the vocabulary are skipped. */
long sk_detok(const sk_tokenizer *t, const int32_t *tok, size_t n, char *out, size_t cap);
/* The model's chat format around a system and a user message, ending with
 * the assistant's turn and the empty thought that has it answer straight away
 * (ChatML, as the daemon has always written it, then "<think>\n\n</think>\n\n"): a heap
 * string, or NULL. */
char *sk_chat_prompt(const char *system, const char *user);
/* One token's bytes (not NUL-terminated) and their length. */
const char *sk_token_bytes(const sk_tokenizer *t, int32_t id, size_t *len);

/* ---- the model ---------------------------------------------------------------------- */
#define SK_BATCH 128            /* tokens read in one step of a prompt */
typedef struct sk_model sk_model;
/* The model over the mapped file `f` (which must outlive it), with a context
 * of `n_ctx` tokens: its state and its scratch, allocated once. NULL with a
 * reason. */
sk_model *sk_model_new(const sk_file *f, int n_ctx, char *err, size_t errcap);
void sk_model_free(sk_model *m);
/* Empty the context. */
void sk_model_reset(sk_model *m);
/* Tokens the context holds. */
int  sk_model_used(const sk_model *m);
/* Read `n` tokens after what the context holds, SK_BATCH at a time. `logits`:
 * NULL for none; else n_vocab floats, the last token's (all 0), or n x n_vocab,
 * every token's (all 1). 0, or -1 with a reason (the context is full, a token
 * outside the vocabulary). */
int  sk_model_eval(sk_model *m, const int32_t *tok, int n, float *logits, int all, char *err, size_t errcap);

/* ---- an answer's grammar and the choice of each token ------------------------------- */
/* A GBNF grammar of the subset daemon/sum_core.c writes, root rule "root".
 * NULL with a reason ("the grammar does not parse: ..."). */
typedef struct sk_grammar sk_grammar;
sk_grammar *sk_grammar_new(const char *text, char *err, size_t errcap);
void sk_grammar_free(sk_grammar *g);
/* Back to the start of the root rule. */
void sk_grammar_reset(sk_grammar *g);
/* Whether these bytes can come next (1 or 0); the state is not changed. */
int  sk_grammar_allows(sk_grammar *g, const char *bytes, size_t n);
/* Take these bytes: 0, or -1 when they cannot come next. */
int  sk_grammar_accept(sk_grammar *g, const char *bytes, size_t n);
/* 1 when what was taken is a whole answer the grammar allows. */
int  sk_grammar_accepting(const sk_grammar *g);

/* Greedy choice after DRY: `breakers` the characters that end a repeat. */
typedef struct sk_sampler sk_sampler;
sk_sampler *sk_sampler_new(const sk_tokenizer *t, const sk_config *c, float mult, float base, int allowed,
                           const char *breakers);
void    sk_sampler_free(sk_sampler *s);
/* Forget the answer so far. */
void    sk_sampler_reset(sk_sampler *s);
/* A token taken. 0, or -1 out of memory. */
int     sk_sampler_accept(sk_sampler *s, int32_t tok);
/* DRY's penalties on `logits` (n_vocab), in place. */
void    sk_sampler_dry(const sk_sampler *s, float *logits);
/* The next token: DRY applied to `logits` (in place), then the largest the
 * grammar allows (g NULL: any), an end token only where the grammar can end.
 * -1 when the grammar allows nothing. */
int32_t sk_sampler_pick(sk_sampler *s, float *logits, sk_grammar *g);

#endif
