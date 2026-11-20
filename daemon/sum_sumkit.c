/* The local summary model: sumkit, the summary inference engine, linked in
 * (sum_engine.h, ARCH-116, docs/SUMKIT.md).
 *
 * One thread does everything: the model runs on the worker's thread for the
 * prompt and for each token, so a summary never takes more than one core. The
 * loop over tokens is ours, so the gate is asked between them and a paused
 * generation keeps its place. The model file is memory-mapped, so its pages are
 * shared with the page cache and given back under pressure. */
#define _POSIX_C_SOURCE 200809L
#include "sum_engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sumkit.h"

/* Ask the gate every this many tokens. */
#define SUM_CHECK_TOKENS 16
/* DRY ("don't repeat yourself") at its published defaults: a token that would
 * extend a sequence already written, longer than the allowed length, is
 * penalized by multiplier * base^(length - allowed). It stops greedy decoding
 * looping without penalizing the names and line numbers a summary repeats
 * (SUMMARIES.md §6). Colons, quotes and asterisks break sequences. */
#define SUM_DRY_MULTIPLIER   0.8f
#define SUM_DRY_BASE         1.75f
#define SUM_DRY_ALLOWED      2
#define SUM_DRY_BREAKERS     ":\"*"

typedef struct {
    oc_sum_engine eng;
    char          path[1024];
    int           n_ctx;
} sk_engine;

typedef struct {
    sk_file      *f;
    sk_model     *m;
    sk_tokenizer *tok;
    sk_sampler   *smp;
    float        *logits;
    int           n_ctx;
} sk_handle;

static uint32_t thread_cpu_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static uint32_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void s_close(void *vh) {
    sk_handle *h = vh;
    if (!h) return;
    sk_sampler_free(h->smp);
    sk_tokenizer_free(h->tok);
    sk_model_free(h->m);
    sk_close(h->f);
    free(h->logits);
    free(h);
}

static void *s_open(void *vctx, char *err, size_t errcap) {
    sk_engine *e = vctx;
    sk_handle *h = calloc(1, sizeof *h);
    if (!h) { snprintf(err, errcap, "out of memory"); return NULL; }
    h->n_ctx = e->n_ctx;
    char why[256] = "";
    if (!(h->f = sk_open(e->path, why, sizeof why)) || !(h->m = sk_model_new(h->f, e->n_ctx, why, sizeof why)) ||
        !(h->tok = sk_tokenizer_new(h->f, why, sizeof why))) {
        snprintf(err, errcap, "cannot load the model %s: %s", e->path, why);
        s_close(h);
        return NULL;
    }
    h->smp = sk_sampler_new(h->tok, sk_config_of(h->f), SUM_DRY_MULTIPLIER, SUM_DRY_BASE, SUM_DRY_ALLOWED, SUM_DRY_BREAKERS);
    h->logits = malloc((size_t)sk_config_of(h->f)->n_vocab * sizeof *h->logits);
    if (!h->smp || !h->logits) { snprintf(err, errcap, "out of memory"); s_close(h); return NULL; }
    return h;
}

/* Read `tok` into the context, SK_BATCH at a time, asking the gate between
 * steps; the last token's logits into h->logits. 0, or -1 with a reason. */
static int feed(sk_handle *h, const int32_t *tok, int nt, oc_sum_gate_fn gate, void *gate_ctx, oc_sum_run_stats *st,
                char *err, size_t errcap) {
    for (int at = 0; at < nt; at += SK_BATCH) {
        if (gate && gate(gate_ctx) != 0) {
            snprintf(err, errcap, "stopped");
            if (st) st->fail = OC_SUM_FAIL_STOPPED;
            return -1;
        }
        int n = nt - at < SK_BATCH ? nt - at : SK_BATCH;
        char why[256];
        if (sk_model_eval(h->m, tok + at, n, at + n == nt ? h->logits : NULL, 0, why, sizeof why) != 0) {
            snprintf(err, errcap, "the model could not read the prompt: %s", why);
            return -1;
        }
    }
    return 0;
}

static int is_eog(const sk_config *c, int32_t t) {
    for (uint32_t i = 0; i < c->n_eog; i++) if ((int32_t)c->eog[i] == t) return 1;
    return 0;
}

/* Write an answer: greedy, after DRY, held to `grammar` when there is one, until
 * the model ends it or the context is full. */
static int generate(sk_handle *h, sk_grammar *g, int max_out, oc_sum_gate_fn gate, void *gate_ctx, char **out,
                    oc_sum_run_stats *st, char *err, size_t errcap) {
    const sk_config *c = sk_config_of(h->f);
    int used = sk_model_used(h->m);
    if (max_out <= 0 || max_out > h->n_ctx - used) max_out = h->n_ctx - used;
    if (max_out <= 0) {
        snprintf(err, errcap, "the prompt is %d tokens, too long for a context of %d", used, h->n_ctx);
        return -1;
    }
    sk_sampler_reset(h->smp);
    int rc = -1, produced = 0;
    size_t ocap = 4096, on = 0;
    char *o = malloc(ocap);
    if (!o) { snprintf(err, errcap, "out of memory"); goto done; }
    o[0] = '\0';
    for (;;) {
        if (produced >= max_out) {
            snprintf(err, errcap, "the answer reached its %d tokens without ending", max_out);
            if (st) st->fail = OC_SUM_FAIL_RUNAWAY;
            *out = o;           /* as far as it got, for the log */
            o = NULL;
            goto done;
        }
        if (gate && produced % SUM_CHECK_TOKENS == 0 && gate(gate_ctx) != 0) {
            snprintf(err, errcap, "stopped");
            if (st) st->fail = OC_SUM_FAIL_STOPPED;
            goto done;
        }
        int32_t t = sk_sampler_pick(h->smp, h->logits, g);
        if (t < 0) {
            snprintf(err, errcap, "the grammar allows nothing more");
            if (st) st->fail = OC_SUM_FAIL_MODEL;
            goto done;
        }
        if (is_eog(c, t)) break;
        size_t pn;
        const char *piece = sk_token_bytes(h->tok, t, &pn);
        if (g) sk_grammar_accept(g, piece, pn);
        sk_sampler_accept(h->smp, t);
        if (on + pn + 1 > ocap) {
            ocap = (ocap + pn) * 2;
            char *no = realloc(o, ocap);
            if (!no) { snprintf(err, errcap, "out of memory"); goto done; }
            o = no;
        }
        memcpy(o + on, piece, pn);
        on += pn;
        o[on] = '\0';
        produced++;
        char why[256];
        if (sk_model_eval(h->m, &t, 1, h->logits, 0, why, sizeof why) != 0) {
            snprintf(err, errcap, "the model stopped mid-answer: %s", why);
            goto done;
        }
    }
    *out = o;
    o = NULL;
    rc = 0;
done:
    if (st) st->output_tokens += (uint32_t)produced;
    free(o);
    return rc;
}

static int s_run(void *vh, const char *system, const char *user, const char *grammar, int max_out,
                 oc_sum_gate_fn gate, void *gate_ctx, char **out, oc_sum_run_stats *st, char *err,
                 size_t errcap) {
    sk_handle *h = vh;
    uint32_t cpu0 = thread_cpu_ms(), t0 = mono_ms();
    *out = NULL;
    if (st) memset(st, 0, sizeof *st);
    sk_grammar *g = NULL;
    if (grammar) {
        char why[256];
        if (!(g = sk_grammar_new(grammar, why, sizeof why))) {
            snprintf(err, errcap, "%s", why);
            if (st) st->fail = OC_SUM_FAIL_PROMPT;
            return -1;
        }
    }
    /* The model's own chat format, with the empty thought that has it answer
     * straight away. */
    char *prompt = sk_chat_prompt(system, user);
    int32_t *tok = NULL;
    long nt = prompt ? sk_tokenize(h->tok, prompt, &tok) : -1;
    free(prompt);
    if (nt < 0) {
        sk_grammar_free(g);
        snprintf(err, errcap, "the prompt does not tokenize");
        if (st) st->fail = OC_SUM_FAIL_PROMPT;
        return -1;
    }
    if (nt >= h->n_ctx) {
        free(tok);
        sk_grammar_free(g);
        snprintf(err, errcap, "the prompt is %ld tokens, too long for a context of %d", nt, h->n_ctx);
        if (st) st->fail = OC_SUM_FAIL_PROMPT;
        return -1;
    }
    sk_model_reset(h->m);
    int rc = -1;
    uint32_t t1 = mono_ms(), t2 = t1;
    if (feed(h, tok, (int)nt, gate, gate_ctx, st, err, errcap) != 0) goto done;
    t2 = mono_ms();
    if (st) st->prompt_tokens = (uint32_t)nt;
    rc = generate(h, g, max_out, gate, gate_ctx, out, st, err, errcap);
done:
    free(tok);
    sk_grammar_free(g);
    if (st) {
        uint32_t t3 = mono_ms();
        st->cpu_ms = thread_cpu_ms() - cpu0;
        st->read_ms = t2 - t1;
        st->write_ms = t3 - t2;
        st->wall_ms = t3 - t0;
    }
    return rc;
}

const oc_sum_engine *oc_sum_local_engine(const char *path, const char *version, int n_ctx) {
    static sk_engine e;
    snprintf(e.path, sizeof e.path, "%s", path);
    e.n_ctx = n_ctx;
    e.eng.ctx = &e;
    e.eng.version = version;
    e.eng.open = s_open;
    e.eng.close = s_close;
    e.eng.run = s_run;
    return &e.eng;
}
