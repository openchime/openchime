/* The summary model's forward pass (docs/SUMKIT.md §3): a decoder of the Qwen3.5
 * family. Each layer is RMS norm, a mixer, a residual add, RMS norm, a gated
 * feed-forward layer and a residual add; the mixer is either
 *
 *   linear attention (gated DeltaNet): one projection to q, k and v, a causal
 *   depthwise convolution over time with SiLU, q and k L2-normalised per head,
 *   the gated delta rule (decay exp(softplus(a + dt) A), step beta =
 *   sigmoid(b)) carrying a dk x dv state per value head, the output RMS-normed
 *   per head and multiplied by SiLU(z), projected back; or
 *
 *   gated attention: a projection to each head's query and gate, keys and
 *   values for fewer heads, the query and key RMS-normed per head with rotary
 *   positions on their first rot_dims, attention over the cache (8 bits),
 *   multiplied by sigmoid(gate), projected back.
 *
 * The output layer is the token embeddings. Tokens are read SK_BATCH at a
 * time: every matrix is read once for the batch. Every dimension comes from the
 * file. */
#include "sk_priv.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    nn_wtype t;
    const void *w;
    int rows, cols;
} mat;

typedef struct {
    int linear;
    const float *in_norm, *post_norm;
    mat ffn_gate, ffn_up, ffn_down;
    /* linear attention */
    mat qkv, z, out, a, b;
    const float *conv, *dt, *A, *gnorm;
    float *conv_state, *ssm;
    /* attention */
    mat q, k, v, o;
    const float *q_norm, *k_norm;
    nn_q8_0 *kc, *vc;                  /* the cache: n_ctx rows of kv_blocks */
} layer;

struct sk_model {
    const sk_file *f;
    sk_config c;
    int n_ctx, used;
    mat embed;
    const float *norm;
    layer *L;
    int kv_blocks;                     /* nn_q8_0 blocks a cached row */
    float *inv_freq;
    /* scratch, for SK_BATCH tokens */
    float *x, *h, *big1, *big2, *big3, *small, *att_scr;
    void *mm;
};

static mat mat_of(const sk_file *f, const char *name) {
    const sk_tensor *t = sk_find(f, name);
    mat m = { (nn_wtype)t->dtype, t->data, (int)t->dims[0], (int)t->dims[1] };
    return m;
}
static const float *vec_of(const sk_file *f, const char *name) { return sk_find(f, name)->data; }

static size_t imaxz(size_t a, size_t b) { return a > b ? a : b; }

void sk_model_free(sk_model *m) {
    if (!m) return;
    if (m->L)
        for (uint32_t l = 0; l < m->c.n_layer; l++) {
            free(m->L[l].conv_state);
            free(m->L[l].ssm);
            free(m->L[l].kc);
            free(m->L[l].vc);
        }
    free(m->L);
    free(m->inv_freq);
    free(m->x); free(m->h); free(m->big1); free(m->big2); free(m->big3); free(m->small); free(m->att_scr);
    free(m->mm);
    free(m);
}

sk_model *sk_model_new(const sk_file *f, int n_ctx, char *err, size_t errcap) {
    sk_model *m = calloc(1, sizeof *m);
    if (!m) { sk_seterr(err, errcap, "out of memory"); return NULL; }
    m->f = f;
    m->c = *sk_config_of(f);
    m->n_ctx = n_ctx;
    const sk_config *c = &m->c;
    int B = SK_BATCH;
    size_t E = c->n_embd, kd = (size_t)c->lin_k_heads * c->lin_k_dim, vd = (size_t)c->lin_v_heads * c->lin_v_dim,
           cd = 2 * kd + vd, qd = 2 * (size_t)c->n_head * c->head_dim, kvd = (size_t)c->n_head_kv * c->head_dim;
    m->embed = mat_of(f, "embed");
    m->norm = vec_of(f, "norm");
    m->L = calloc(c->n_layer, sizeof *m->L);
    m->kv_blocks = (int)(kvd / NN_Q8_0);
    m->inv_freq = malloc((c->rot_dims / 2 + 1) * sizeof *m->inv_freq);
    if (!m->L || !m->inv_freq) goto oom;
    for (uint32_t i = 0; i < c->rot_dims / 2; i++)
        m->inv_freq[i] = (float)pow((double)c->rope_theta, -2.0 * i / c->rot_dims);
    size_t widest = imaxz(imaxz(c->n_ff, cd), imaxz(qd, imaxz(vd, E)));
    for (uint32_t l = 0; l < c->n_layer; l++) {
        layer *y = &m->L[l];
        char n[SK_NAME_MAX];
#define NM(s) (snprintf(n, sizeof n, "L%02u.%s", l, s), n)
        y->linear = (int)(c->linear >> l & 1);
        y->in_norm = vec_of(f, NM("in_norm"));
        y->post_norm = vec_of(f, NM("post_norm"));
        y->ffn_gate = mat_of(f, NM("ffn_gate"));
        y->ffn_up = mat_of(f, NM("ffn_up"));
        y->ffn_down = mat_of(f, NM("ffn_down"));
        if (y->linear) {
            y->qkv = mat_of(f, NM("qkv"));
            y->z = mat_of(f, NM("z"));
            y->a = mat_of(f, NM("a"));
            y->b = mat_of(f, NM("b"));
            y->out = mat_of(f, NM("out"));
            y->conv = vec_of(f, NM("conv"));
            y->dt = vec_of(f, NM("dt"));
            y->A = vec_of(f, NM("A"));
            y->gnorm = vec_of(f, NM("gnorm"));
            y->conv_state = calloc((c->conv_kernel - 1) * cd + 1, sizeof *y->conv_state);
            y->ssm = calloc((size_t)c->lin_v_heads * c->lin_k_dim * c->lin_v_dim, sizeof *y->ssm);
            if (!y->conv_state || !y->ssm) goto oom;
        } else {
            y->q = mat_of(f, NM("q"));
            y->k = mat_of(f, NM("k"));
            y->v = mat_of(f, NM("v"));
            y->o = mat_of(f, NM("o"));
            y->q_norm = vec_of(f, NM("q_norm"));
            y->k_norm = vec_of(f, NM("k_norm"));
            y->kc = malloc((size_t)n_ctx * m->kv_blocks * sizeof *y->kc);
            y->vc = malloc((size_t)n_ctx * m->kv_blocks * sizeof *y->vc);
            if (!y->kc || !y->vc) goto oom;
        }
#undef NM
    }
    m->x = malloc((size_t)B * E * sizeof *m->x);
    m->h = malloc((size_t)B * E * sizeof *m->h);
    m->big1 = malloc((size_t)B * widest * sizeof *m->big1);
    m->big2 = malloc((size_t)B * widest * sizeof *m->big2);
    m->big3 = malloc((size_t)B * widest * sizeof *m->big3);
    m->small = malloc((size_t)B * 2 * (c->lin_v_heads + 1) * sizeof *m->small);
    m->att_scr = malloc((size_t)(n_ctx + 1) * sizeof *m->att_scr);
    size_t mm = 0;
    int ks[4] = { (int)E, (int)c->n_ff, (int)vd, (int)(c->n_head * c->head_dim) };
    for (int t = 0; t < 4; t++)
        for (int k = 0; k < 4; k++) mm = imaxz(mm, nn_matmul_w_scratch((nn_wtype)t, ks[k], B));
    m->mm = malloc(mm);
    if (!m->x || !m->h || !m->big1 || !m->big2 || !m->big3 || !m->small || !m->att_scr || !m->mm) goto oom;
    sk_model_reset(m);
    return m;
oom:
    sk_model_free(m);
    sk_seterr(err, errcap, "out of memory for a context of %d tokens", n_ctx);
    return NULL;
}

void sk_model_reset(sk_model *m) {
    const sk_config *c = &m->c;
    size_t kd = (size_t)c->lin_k_heads * c->lin_k_dim, vd = (size_t)c->lin_v_heads * c->lin_v_dim;
    for (uint32_t l = 0; l < c->n_layer; l++)
        if (m->L[l].linear) {
            memset(m->L[l].conv_state, 0, (c->conv_kernel - 1) * (2 * kd + vd) * sizeof(float));
            memset(m->L[l].ssm, 0, (size_t)c->lin_v_heads * c->lin_k_dim * c->lin_v_dim * sizeof(float));
        }
    m->used = 0;
}

int sk_model_used(const sk_model *m) { return m->used; }

/* y[T][rows] = x[T][cols] W^T */
static void mm(sk_model *m, const mat *w, const float *x, int T, size_t ldx, float *y, size_t ldy) {
    nn_matmul_w(w->t, w->w, w->rows, w->cols, x, T, ldx, y, ldy, m->mm);
}

static float softplus(float x) { return x > 20.0f ? x : log1pf(expf(x)); }

static void linear_mixer(sk_model *m, layer *y, int T) {
    const sk_config *c = &m->c;
    size_t E = c->n_embd, hk = c->lin_k_heads, hv = c->lin_v_heads, dk = c->lin_k_dim, dv = c->lin_v_dim;
    size_t kd = hk * dk, vd = hv * dv, cd = 2 * kd + vd;
    float *qkv = m->big1, *z = m->big2, *o = m->big3, *a = m->small, *b = m->small + (size_t)T * hv;
    mm(m, &y->qkv, m->h, T, E, qkv, cd);
    mm(m, &y->z, m->h, T, E, z, vd);
    mm(m, &y->a, m->h, T, E, a, hv);
    mm(m, &y->b, m->h, T, E, b, hv);
    nn_conv_causal(qkv, T, (int)cd, cd, y->conv_state, (int)c->conv_kernel, y->conv, qkv, cd);
    for (int t = 0; t < T; t++) {
        nn_silu(qkv + (size_t)t * cd, cd);
        nn_l2norm(qkv + (size_t)t * cd, (int)(2 * hk), (int)dk, (int)dk, c->rms_eps);   /* q's heads, then k's */
        for (size_t h = 0; h < hv; h++) {
            a[(size_t)t * hv + h] = softplus(a[(size_t)t * hv + h] + y->dt[h]) * y->A[h];
            float bb = b[(size_t)t * hv + h];
            nn_logistic(&bb, 1);
            b[(size_t)t * hv + h] = bb;
        }
    }
    nn_gated_delta(qkv, qkv + kd, cd, qkv + 2 * kd, cd, a, b, T, (int)hk, (int)hv, (int)dk, (int)dv,
                   1.0f / sqrtf((float)dk), y->ssm, o, vd);
    nn_rmsnorm(o, T * (int)hv, (int)dv, (int)dv, y->gnorm, c->rms_eps);
    nn_mul_silu(o, z, (size_t)T * vd);
    mm(m, &y->out, o, T, vd, m->h, E);
}

static void attention_mixer(sk_model *m, layer *y, int T) {
    const sk_config *c = &m->c;
    size_t E = c->n_embd, nh = c->n_head, nkv = c->n_head_kv, hd = c->head_dim;
    size_t qd = 2 * nh * hd, kvd = nkv * hd, od = nh * hd;
    float *qg = m->big1, *kv = m->big2, *o = m->big3;     /* kv: k then v, each kvd a token */
    mm(m, &y->q, m->h, T, E, qg, qd);
    mm(m, &y->k, m->h, T, E, kv, 2 * kvd);
    mm(m, &y->v, m->h, T, E, kv + kvd, 2 * kvd);
    for (int t = 0; t < T; t++) {
        float pos = (float)(m->used + t), *kt = kv + (size_t)t * 2 * kvd, *vt = kt + kvd;
        for (size_t h = 0; h < nh; h++) {
            float *q = qg + (size_t)t * qd + h * 2 * hd;
            nn_rmsnorm(q, 1, (int)hd, (int)hd, y->q_norm, c->rms_eps);
            nn_rope_half(q, (int)c->rot_dims, m->inv_freq, pos);
        }
        for (size_t h = 0; h < nkv; h++) {
            nn_rmsnorm(kt + h * hd, 1, (int)hd, (int)hd, y->k_norm, c->rms_eps);
            nn_rope_half(kt + h * hd, (int)c->rot_dims, m->inv_freq, pos);
        }
        nn_q8_0_quantize(kt, (int)kvd, y->kc + (size_t)(m->used + t) * m->kv_blocks);
        nn_q8_0_quantize(vt, (int)kvd, y->vc + (size_t)(m->used + t) * m->kv_blocks);
    }
    float scale = 1.0f / sqrtf((float)hd);
    size_t per = nh / nkv, hb = hd / NN_Q8_0;
    for (int t = 0; t < T; t++)
        for (size_t h = 0; h < nh; h++) {
            const float *q = qg + (size_t)t * qd + h * 2 * hd;
            size_t g = h / per;
            nn_attend_q8(q, y->kc + g * hb, y->vc + g * hb, (size_t)m->kv_blocks, m->used + t + 1, (int)hd, scale,
                         o + (size_t)t * od + h * hd, m->att_scr);
            nn_mul_sigmoid(o + (size_t)t * od + h * hd, q + hd, hd);
        }
    mm(m, &y->o, o, T, od, m->h, E);
}

static void ffn(sk_model *m, layer *y, int T) {
    size_t E = m->c.n_embd, F = m->c.n_ff;
    mm(m, &y->ffn_gate, m->h, T, E, m->big1, F);
    mm(m, &y->ffn_up, m->h, T, E, m->big2, F);
    for (int t = 0; t < T; t++) nn_mul_silu(m->big2 + (size_t)t * F, m->big1 + (size_t)t * F, F);
    mm(m, &y->ffn_down, m->big2, T, F, m->h, E);
}

static void forward(sk_model *m, const int32_t *tok, int T) {
    const sk_config *c = &m->c;
    size_t E = c->n_embd;
    for (int t = 0; t < T; t++) nn_dequant_row(m->embed.t, m->embed.w, (int)E, tok[t], m->x + (size_t)t * E);
    for (uint32_t l = 0; l < c->n_layer; l++) {
        layer *y = &m->L[l];
        memcpy(m->h, m->x, (size_t)T * E * sizeof *m->h);
        nn_rmsnorm(m->h, T, (int)E, (int)E, y->in_norm, c->rms_eps);
        if (y->linear) linear_mixer(m, y, T);
        else attention_mixer(m, y, T);
        for (size_t i = 0; i < (size_t)T * E; i++) m->x[i] += m->h[i];
        memcpy(m->h, m->x, (size_t)T * E * sizeof *m->h);
        nn_rmsnorm(m->h, T, (int)E, (int)E, y->post_norm, c->rms_eps);
        ffn(m, y, T);
        for (size_t i = 0; i < (size_t)T * E; i++) m->x[i] += m->h[i];
    }
    m->used += T;
}

/* The logits of rows `from`.. of the batch's last hidden states. */
static void head(sk_model *m, int from, int T, float *logits) {
    size_t E = m->c.n_embd;
    float *h = m->h + (size_t)from * E;
    memcpy(h, m->x + (size_t)from * E, (size_t)(T - from) * E * sizeof *h);
    nn_rmsnorm(h, T - from, (int)E, (int)E, m->norm, m->c.rms_eps);
    mm(m, &m->embed, h, T - from, E, logits, m->c.n_vocab);
}

int sk_model_eval(sk_model *m, const int32_t *tok, int n, float *logits, int all, char *err, size_t errcap) {
    if (n <= 0) return 0;
    if (m->used + n > m->n_ctx) {
        sk_seterr(err, errcap, "%d tokens do not fit the %d the context has left", n, m->n_ctx - m->used);
        return -1;
    }
    for (int i = 0; i < n; i++)
        if (tok[i] < 0 || (uint32_t)tok[i] >= m->c.n_vocab) { sk_seterr(err, errcap, "token %d is outside the vocabulary", tok[i]); return -1; }
    for (int at = 0; at < n; at += SK_BATCH) {
        int T = n - at < SK_BATCH ? n - at : SK_BATCH;
        forward(m, tok + at, T);
        if (logits && all) head(m, 0, T, logits + (size_t)at * m->c.n_vocab);
        else if (logits && at + T == n) head(m, T - 1, T, logits);
    }
    return 0;
}
