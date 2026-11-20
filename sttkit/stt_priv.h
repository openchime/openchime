/* sttkit's internals: the mapped model and the forward pass's view of it. */
#ifndef STT_PRIV_H
#define STT_PRIV_H

#include "sttkit.h"
#include "nnkit.h"

#define REC_LAYERS 6

/* A quantised matrix product's weight (recognizer.bin's <name>.w, .s, .zp and,
 * when it has one, .b): uint8 [n][k], one row per output column, with that
 * column's scale and zero point. */
typedef struct {
    const uint8_t *w, *zp;
    const float *s, *b;
    int n, k;
} rec_qmat;

typedef struct {
    const float *g1, *g2;                    /* the gains after the two layer norms */
    rec_qmat q, k, v, o, f1, f2;
    int back, ahead;                         /* the attention window */
} rec_enc_layer;

typedef struct {
    const float *g1, *g2, *g3;
    rec_qmat q, k, v, o;                     /* self-attention */
    rec_qmat cq, co, xk, xv;                 /* cross-attention: its query, output, and the memory's keys and values */
    rec_qmat f1, f2;                         /* the gated feed-forward layer */
} rec_dec_layer;

struct recognizer {
    const uint8_t *p;
    size_t n;
    int owned;                               /* mapped by recognizer_open: unmapped on close */
    uint32_t count;
    const uint8_t *index;
    int32_t cfg[REC_CFG_COUNT];
    const float *fe_lin, *fe_c1w, *fe_c1b, *fe_c2w, *fe_c2b;
    float fe_comp, fe_eps, eps, alpha;
    rec_enc_layer enc[REC_LAYERS];
    const float *enc_g;
    const uint8_t *pos;
    float pos_scale;
    int pos_zp;
    rec_dec_layer dec[REC_LAYERS];
    const float *dec_g, *rope;
    const uint8_t *emb;
    float emb_scale;
    int emb_zp;
    stt_tokens tok;
};

/* The forward pass over one utterance (recognizer_model.c): the token ids it
 * decodes, at most `cap`, in *nids. With `enc` set, the encoder's output
 * (*frames x 320, malloc'd) is returned in it; with `logits0` set (32,768
 * floats), the first decoding step's logits are written there. 0, or -1 with
 * the reason in `err`. */
int rec_run(const recognizer *r, const float *pcm, size_t samples, int64_t *ids, size_t cap, size_t *nids,
            float **enc, int *frames, float *logits0, char *err, size_t errcap);

#endif
