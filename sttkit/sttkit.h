/*
 * OpenChime — sttkit: voice input's recognizer, in plain C (ARCH-112,
 * docs/details/STTKIT.md).
 *
 * Takes one utterance of 16 kHz mono audio and returns its words. The model is
 * one file, recognizer.bin, used where it lies (mapped, or bytes the caller has
 * mapped), never copied. One thread, no intrinsics, no library: the kernels are
 * nnkit's, shared with ttskit.
 */
#ifndef STTKIT_H
#define STTKIT_H

#include <stddef.h>
#include <stdint.h>

#define STT_RATE 16000              /* input samples per second */

/* Tokens allowed per second of audio: the model's guard against a decoder that
 * repeats itself on noise, at its default for Latin-script languages. */
#define STT_TOKENS_PER_SECOND 6.5f
#define STT_MAX_TOKENS        256

/* ---- the tokenizer: token ids to text ------------------------------------------
 *
 * The table is one record per token id, in order: a length byte, or two when
 * the first has its top bit set (len = second * 128 + first - 128), then that
 * many bytes. A zero length is an unused id. Decoding is each id's bytes in
 * turn, special tokens ("<...>") skipped, the word marker U+2581 read as a
 * space, and the result trimmed; an id outside the table is skipped. */
typedef struct {
    uint32_t count;
    const uint8_t **bytes;     /* count pointers into the table */
    uint16_t *len;
} stt_tokens;

int  stt_tokens_load(stt_tokens *t, const uint8_t *data, size_t len);
void stt_tokens_free(stt_tokens *t);
/* Returns the length written to `out` (NUL-terminated within `cap`, cut back to
 * a whole UTF-8 character), or -1 if the table is empty. */
long stt_detok(const stt_tokens *t, const int64_t *ids, size_t n, char *out, size_t cap);

/* The most tokens an utterance of `samples` may decode to. */
size_t stt_max_tokens(size_t samples);

/* ---- the model's data: recognizer.bin -----------------------------------------
 *
 * Every tensor the forward pass reads, in the layout its kernels read, the
 * tokenizer and the model's dimensions. Made from the published model by
 * `openchimed --recognizer-pack` (daemon/recognizer_pack.c, which documents the
 * tensor names); mapped, never copied.
 *
 * header (320 bytes)  magic "OCSTTRC1", u32 version, u32 count, u32 header size
 *                     (320), u32 entry size (96), u64 data offset, u64 file size,
 *                     u8 sha256[8][32] of the eight published files in the
 *                     order daemon/recognizer_pack.h lists them, zeros to 320
 * index               count x { char name[48] (NUL-terminated, NUL-padded),
 *                     u32 dtype, u32 ndim, u32 dims[4] (unused 0), u64 offset,
 *                     u64 bytes, f32 scale, i32 zero point }, sorted by name bytes
 * data                from the data offset (the index's end rounded up to 64),
 *                     each tensor at a multiple of 64 from the file's start, in
 *                     index order, zeros between; the file ends with the last
 * All integers little-endian. A tensor quantised with one scale and zero point
 * has them in its entry; every other tensor has 0 and 0 there. */
#define REC_MAGIC     "OCSTTRC1"
#define REC_VERSION   1u
#define REC_HEADER    320u
#define REC_ENTRY     96u
#define REC_NAME_MAX  48         /* name bytes, the NUL included */
#define REC_ALIGN     64
#define REC_MAX_DIMS  4
#define REC_SOURCES   8          /* SHA-256s in the header */
/* Element types: f32, uint8, int32 (the model format's codes for the same). */
enum { REC_F32 = 1, REC_U8 = 2, REC_I32 = 6 };

/* The model's dimensions, the int32 tensor "cfg", in this order. */
enum {
    REC_CFG_DIM,        /* the width of both stacks: 320 */
    REC_CFG_DEPTH,      /* layers on each side: 6 */
    REC_CFG_HEADS,      /* attention heads: 8 */
    REC_CFG_HEAD_DIM,   /* their width: 40 */
    REC_CFG_VOCAB,      /* tokens: 32768 */
    REC_CFG_BOS,        /* the start token */
    REC_CFG_EOS,        /* the end token */
    REC_CFG_FRAME,      /* samples in a frontend frame: 80 */
    REC_CFG_FE_MID,     /* the frontend's widest convolution: 640 */
    REC_CFG_FE_KERNEL,  /* its convolutions' kernel: 5 */
    REC_CFG_FE_STRIDE,  /* ... and stride: 2 */
    REC_CFG_ENC_FF,     /* the encoder's feed-forward width: 1280 */
    REC_CFG_DEC_FF,     /* the decoder's, each half of its gated layer: 1280 */
    REC_CFG_ROTARY,     /* dimensions of a head with rotary positions: 32 */
    REC_CFG_POSITIONS,  /* rows of the adapter's position table: 4096 */
    REC_CFG_WINDOW,     /* the encoder's attention windows: for each layer, the
                           rows back and the rows ahead a row sees */
    REC_CFG_COUNT = REC_CFG_WINDOW + 2 * 6
};

typedef struct recognizer recognizer;
typedef struct {
    const char *name;
    int         dtype, ndim, dims[REC_MAX_DIMS];
    const void *data;                  /* in the mapping: 64-byte aligned */
    size_t      bytes;
    float       scale;
    int         zero_point;
} rec_tensor;

/* Map `path`, or use `len` bytes the caller lends (nothing is copied: they must
 * outlive the handle, and start 64-byte aligned). NULL on failure, with the
 * reason in `err`: a file that is not recognizer.bin of this version, is cut
 * short, has an index that points outside it or out of line, or lacks a tensor
 * the forward pass reads, is refused. */
recognizer *recognizer_open(const char *path, char *err, size_t errcap);
recognizer *recognizer_open_mem(const void *bytes, size_t len, char *err, size_t errcap);
void        recognizer_close(recognizer *r);
/* The tensor called `name`: 1 with `t` filled, 0 if there is none. */
int    rec_tensor_get(const recognizer *r, const char *name, rec_tensor *t);
/* The tensors in index order, for tools and tests. */
size_t rec_tensor_count(const recognizer *r);
int    rec_tensor_at(const recognizer *r, size_t i, rec_tensor *t);

/* ---- hearing ------------------------------------------------------------------
 *
 * One utterance of `samples` floats in [-1, 1] at STT_RATE, heard as a whole:
 * the frontend, the encoder, the adapter, the cross-attention keys and values,
 * then greedy decoding from the start token, stopped at the end token or at
 * stt_max_tokens(samples). On success *text is a malloc'd UTF-8 string
 * (possibly empty) the caller frees; returns 0. Returns -1 with the reason in
 * `err` otherwise (an utterance longer than the position table, no memory). */
int recognizer_hear(const recognizer *r, const float *pcm, size_t samples, char **text, char *err, size_t errcap);

#endif
