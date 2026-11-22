/* Choosing each token of an answer (docs/details/SUMKIT.md §5): greedy, after DRY,
 * among the tokens the call's grammar allows -- the same choice as the reference implementation's
 * chain of grammar, DRY and greedy samplers, made without masking the whole
 * vocabulary: the candidates are tried best first and the first the grammar
 * allows is taken.
 *
 * DRY ("don't repeat yourself"), as its reference implementation (from koboldcpp) computes it for
 * sequence breakers of one character: over the answer so far, the run since
 * the last token whose text holds a breaker is the most a repeat may count;
 * for every earlier place where the answer's last tokens occur again, at least
 * `allowed` of them, the token that followed there would extend the repeat,
 * and its logit is lowered by multiplier * base^(length - allowed) for the
 * longest such repeat -- unless that token is itself a breaker. */
#include "sk_priv.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

struct sk_sampler {
    const sk_tokenizer *tok;
    const sk_config *c;
    float mult, base;
    int allowed;
    uint8_t *breaker;          /* per token id: its text holds a breaker */
    int32_t *hist;
    int n, cap;
    int *rep;                  /* scratch: repeat lengths */
    struct cand { float v; int32_t id; } *cand;
};

sk_sampler *sk_sampler_new(const sk_tokenizer *t, const sk_config *c, float mult, float base, int allowed,
                           const char *breakers) {
    sk_sampler *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->tok = t;
    s->c = c;
    s->mult = mult;
    s->base = base;
    s->allowed = allowed;
    s->breaker = calloc(c->n_vocab, 1);
    s->cand = malloc((size_t)c->n_vocab * sizeof *s->cand);
    if (!s->breaker || !s->cand) { sk_sampler_free(s); return NULL; }
    for (uint32_t id = 0; id < c->n_tok; id++) {
        size_t len;
        const char *b = sk_token_bytes(t, (int32_t)id, &len);
        for (size_t i = 0; i < len && !s->breaker[id]; i++) s->breaker[id] = strchr(breakers, b[i]) != NULL && b[i];
    }
    return s;
}

void sk_sampler_free(sk_sampler *s) {
    if (!s) return;
    free(s->breaker);
    free(s->hist);
    free(s->rep);
    free(s->cand);
    free(s);
}

void sk_sampler_reset(sk_sampler *s) { s->n = 0; }

int sk_sampler_accept(sk_sampler *s, int32_t tok) {
    if (s->n == s->cap) {
        int nc = s->cap ? 2 * s->cap : 512;
        int32_t *h = realloc(s->hist, (size_t)nc * sizeof *h);
        int *r = h ? realloc(s->rep, (size_t)nc * sizeof *r) : NULL;
        if (!h || !r) { if (h) s->hist = h; return -1; }
        s->hist = h;
        s->rep = r;
        s->cap = nc;
    }
    s->hist[s->n++] = tok;
    return 0;
}

/* tokens from the end: rat(0) is the last */
static inline int32_t rat(const sk_sampler *s, int i) { return s->hist[s->n - 1 - i]; }

void sk_sampler_dry(const sk_sampler *s, float *logits) {
    int N = s->n;
    if (s->mult == 0.0f || s->base < 1.0f || N <= s->allowed) return;
    int rep_limit = N;
    for (int i = 0; i < N; i++)
        if (s->breaker[rat(s, i)]) { rep_limit = i; break; }
    if (rep_limit < s->allowed) return;
    /* rep[last - k]: how many of the last tokens occur again ending k back */
    int last = N - 1;
    for (int k = 1; k < N; k++) {
        int n = 0;
        while (n + k < N && rat(s, n) == rat(s, n + k)) n++;
        s->rep[last - k] = n < rep_limit ? n : rep_limit;
    }
    s->rep[last] = 0;
    int max_exp = 0;
    if (s->base > 1.000001f) max_exp = (int)(88.7228391f / logf(s->base));
    /* the longest repeat each following token would extend, then the penalty */
    for (int i = 0; i < N - 1; i++) {
        int len = s->rep[i];
        if (len < s->allowed) continue;
        int32_t tok = s->hist[i + 1];
        /* the longest for this token: other positions with the same token */
        int longest = len;
        for (int j = 0; j < N - 1; j++)
            if (j != i && s->hist[j + 1] == tok && s->rep[j] > longest) longest = s->rep[j];
        if (longest != len) continue;         /* applied once, at the longest */
        int dup = 0;
        for (int j = 0; j < i; j++) dup |= s->hist[j + 1] == tok && s->rep[j] == len;
        if (dup || s->breaker[tok]) continue;
        int e = len - s->allowed;
        if (max_exp > 0 && e > max_exp) e = max_exp;
        logits[tok] -= s->mult * powf(s->base, (float)e);
    }
}

static int is_eog(const sk_config *c, int32_t id) {
    for (uint32_t i = 0; i < c->n_eog; i++) if ((int32_t)c->eog[i] == id) return 1;
    return 0;
}

static int cand_cmp(const void *a, const void *b) {
    const struct cand *x = a, *y = b;
    if (x->v != y->v) return x->v > y->v ? -1 : 1;
    return x->id < y->id ? -1 : x->id > y->id;
}

/* Is token `id` allowed now: an end token when the grammar can end, any other
 * when its bytes take the grammar on (always, without a grammar). */
static int allowed(const sk_sampler *s, sk_grammar *g, int32_t id) {
    if ((uint32_t)id >= s->c->n_tok) return 0;
    if (is_eog(s->c, id)) return !g || sk_grammar_accepting(g);
    size_t len;
    const char *b = sk_token_bytes(s->tok, id, &len);
    if (!len) return 0;
    return !g || sk_grammar_allows(g, b, len);
}

int32_t sk_sampler_pick(sk_sampler *s, float *logits, sk_grammar *g) {
    int V = (int)s->c->n_vocab;
    sk_sampler_dry(s, logits);
    /* the best first: the 256 largest sorted, then (rarely) all of them */
    float lim = -INFINITY;
    int k = 256 < V ? 256 : V;
    for (int round = 0; round < 2; round++) {
        int n = 0;
        if (round == 0) {
            /* a threshold under the k-th largest, from a sample of the vocabulary */
            float best[256];
            int nb = 0;
            for (int i = 0; i < V; i++) {
                float v = logits[i];
                if (nb < k) {
                    int j = nb++;
                    while (j > 0 && best[j - 1] < v) { best[j] = best[j - 1]; j--; }
                    best[j] = v;
                } else if (v > best[k - 1]) {
                    int j = k - 1;
                    while (j > 0 && best[j - 1] < v) { best[j] = best[j - 1]; j--; }
                    best[j] = v;
                }
            }
            lim = best[nb - 1];
            for (int i = 0; i < V; i++) if (logits[i] >= lim) s->cand[n++] = (struct cand){ logits[i], i };
        } else {
            for (int i = 0; i < V; i++) if (logits[i] < lim) s->cand[n++] = (struct cand){ logits[i], i };
        }
        qsort(s->cand, (size_t)n, sizeof *s->cand, cand_cmp);
        for (int i = 0; i < n; i++)
            if (s->cand[i].v > -INFINITY && allowed(s, g, s->cand[i].id)) return s->cand[i].id;
    }
    return -1;
}
