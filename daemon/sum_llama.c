/* The local summary model: llama.cpp, linked in (sum_engine.h, ARCH-116).
 *
 * One thread does everything: the context runs on one compute thread for the
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

/* llama.cpp's headers repeat three typedefs, which C99 does not allow and
 * Clang reports as an error under -Werror; it is their headers, not this code. */
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wtypedef-redefinition"
#endif
#include "llama.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

/* Ask the gate every this many tokens. */
#define SUM_CHECK_TOKENS 16
/* The presence penalty, over this many recent tokens (the model card's 1.5). */
#define SUM_PRESENCE_PENALTY 1.5f
#define SUM_PENALTY_LAST_N   256
/* Tokens read per step of the prompt. */
#define SUM_UBATCH 128

typedef struct {
    oc_sum_engine eng;
    char          path[1024];
    int           n_ctx;
} llama_engine;

typedef struct {
    struct llama_model   *model;
    struct llama_context *ctx;
    const struct llama_vocab *vocab;
    const char           *tmpl;
    int                   n_ctx;
} llama_handle;

static void quiet_log(enum ggml_log_level level, const char *text, void *ud) {
    (void)ud;
    if (level == GGML_LOG_LEVEL_ERROR) fprintf(stderr, "summary: llama: %s", text);
}

static uint32_t thread_cpu_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void *l_open(void *vctx, char *err, size_t errcap) {
    llama_engine *e = vctx;
    static int backend_ready;
    if (!backend_ready) {
        llama_log_set(quiet_log, NULL);
        llama_backend_init();
        backend_ready = 1;
    }
    llama_handle *h = calloc(1, sizeof *h);
    if (!h) { snprintf(err, errcap, "out of memory"); return NULL; }
    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0;
    mp.load_mode = LLAMA_LOAD_MODE_MMAP;
    h->model = llama_model_load_from_file(e->path, mp);
    if (!h->model) { snprintf(err, errcap, "cannot load the model %s", e->path); free(h); return NULL; }
    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = (uint32_t)e->n_ctx;
    /* A small physical batch keeps the compute buffers small; the prompt is
     * read in steps of this many tokens. */
    cp.n_batch = SUM_UBATCH;
    cp.n_ubatch = SUM_UBATCH;
    cp.n_threads = 1;
    cp.n_threads_batch = 1;
    cp.no_perf = 1;
    h->ctx = llama_init_from_model(h->model, cp);
    if (!h->ctx) {
        snprintf(err, errcap, "cannot make a context of %d tokens", e->n_ctx);
        llama_model_free(h->model);
        free(h);
        return NULL;
    }
    h->vocab = llama_model_get_vocab(h->model);
    h->tmpl = llama_model_chat_template(h->model, NULL);
    h->n_ctx = e->n_ctx;
    return h;
}

static void l_close(void *vh) {
    llama_handle *h = vh;
    if (!h) return;
    llama_free(h->ctx);
    llama_model_free(h->model);
    free(h);
}

static int l_run(void *vh, const char *system, const char *user, const char *grammar, int max_out,
                 oc_sum_gate_fn gate, void *gate_ctx, char **out, oc_sum_run_stats *st, char *err,
                 size_t errcap) {
    llama_handle *h = vh;
    uint32_t cpu0 = thread_cpu_ms();
    *out = NULL;
    if (st) memset(st, 0, sizeof *st);

    /* The model's own chat format. */
    llama_chat_message msgs[2] = { { "system", system }, { "user", user } };
    size_t cap = 2 * (strlen(system) + strlen(user)) + 1024;
    char *prompt = malloc(cap);
    if (!prompt) { snprintf(err, errcap, "out of memory"); return -1; }
    int pl = llama_chat_apply_template(h->tmpl, msgs, 2, true, prompt, (int32_t)cap);
    if (pl < 0) {
        /* No template the library knows: a plain one. */
        pl = snprintf(prompt, cap, "%s\n\n%s\n\nAnswer:\n", system, user);
    } else if ((size_t)pl >= cap) {
        char *np = realloc(prompt, (size_t)pl + 1);
        if (!np) { free(prompt); snprintf(err, errcap, "out of memory"); return -1; }
        prompt = np;
        cap = (size_t)pl + 1;
        llama_chat_apply_template(h->tmpl, msgs, 2, true, prompt, (int32_t)cap);
    }

    int ntok_cap = pl + 16;
    llama_token *tok = malloc((size_t)ntok_cap * sizeof *tok);
    if (!tok) { free(prompt); snprintf(err, errcap, "out of memory"); return -1; }
    int nt = llama_tokenize(h->vocab, prompt, pl, tok, ntok_cap, true, true);
    free(prompt);
    if (nt < 0) { free(tok); snprintf(err, errcap, "the prompt does not tokenize"); return -1; }
    if (nt + max_out > h->n_ctx) {
        free(tok);
        snprintf(err, errcap, "the prompt is %d tokens, too long for a context of %d", nt, h->n_ctx);
        return -1;
    }

    llama_memory_clear(llama_get_memory(h->ctx), true);
    struct llama_sampler *smpl = llama_sampler_chain_init(llama_sampler_chain_default_params());
    struct llama_sampler *gram = grammar ? llama_sampler_init_grammar(h->vocab, grammar, "root") : NULL;
    if (grammar && !gram) {
        llama_sampler_free(smpl);
        free(tok);
        snprintf(err, errcap, "the grammar does not parse");
        return -1;
    }
    if (gram) llama_sampler_chain_add(smpl, gram);
    /* Greedy decoding on a small model repeats itself; a presence penalty over
     * the recent tokens, as the model's card recommends, breaks the loop. */
    llama_sampler_chain_add(smpl, llama_sampler_init_penalties(llama_vocab_n_tokens(h->vocab), SUM_PENALTY_LAST_N,
                                                               1.0f, 0.0f, SUM_PRESENCE_PENALTY));
    llama_sampler_chain_add(smpl, llama_sampler_init_greedy());

    int rc = -1;
    size_t ocap = 4096, on = 0;
    char *o = malloc(ocap);
    if (!o) { snprintf(err, errcap, "out of memory"); goto done; }
    o[0] = '\0';

    /* The prompt, in batches, asking the gate between them. */
    for (int at = 0; at < nt; at += SUM_UBATCH) {
        if (gate && gate(gate_ctx) != 0) { snprintf(err, errcap, "stopped"); goto done; }
        int n = nt - at < SUM_UBATCH ? nt - at : SUM_UBATCH;
        if (llama_decode(h->ctx, llama_batch_get_one(tok + at, n)) != 0) {
            snprintf(err, errcap, "the model could not read the prompt");
            goto done;
        }
    }
    if (st) st->prompt_tokens = (uint32_t)nt;

    int produced = 0;
    for (;;) {
        if (produced >= max_out) { snprintf(err, errcap, "the answer ran past %d tokens", max_out); goto done; }
        if (gate && produced % SUM_CHECK_TOKENS == 0 && gate(gate_ctx) != 0) {
            snprintf(err, errcap, "stopped");
            goto done;
        }
        llama_token t = llama_sampler_sample(smpl, h->ctx, -1);
        if (llama_vocab_is_eog(h->vocab, t)) break;
        char piece[256];
        int pn = llama_token_to_piece(h->vocab, t, piece, sizeof piece, 0, false);
        if (pn < 0) pn = 0;
        if (on + (size_t)pn + 1 > ocap) {
            ocap = (ocap + (size_t)pn) * 2;
            char *no = realloc(o, ocap);
            if (!no) { snprintf(err, errcap, "out of memory"); goto done; }
            o = no;
        }
        memcpy(o + on, piece, (size_t)pn);
        on += (size_t)pn;
        o[on] = '\0';
        produced++;
        if (llama_decode(h->ctx, llama_batch_get_one(&t, 1)) != 0) {
            snprintf(err, errcap, "the model stopped mid-answer");
            goto done;
        }
        /* The grammar is complete once the closing brace of the root object
         * is out: the next token could only end the answer. */
        if (gram && on && o[on - 1] == '}') {
            int depth = 0, in_str = 0;
            for (size_t k = 0; k < on; k++) {
                char ch = o[k];
                if (in_str) { if (ch == '\\') k++; else if (ch == '"') in_str = 0; continue; }
                if (ch == '"') in_str = 1;
                else if (ch == '{' || ch == '[') depth++;
                else if (ch == '}' || ch == ']') depth--;
            }
            if (depth == 0) break;
        }
    }
    if (st) st->output_tokens = (uint32_t)produced;
    *out = o;
    o = NULL;
    rc = 0;
done:
    free(o);
    free(tok);
    llama_sampler_free(smpl);
    if (st) st->cpu_ms = thread_cpu_ms() - cpu0;
    return rc;
}

const oc_sum_engine *oc_sum_llama_engine(const char *path, const char *version, int n_ctx) {
    static llama_engine e;
    snprintf(e.path, sizeof e.path, "%s", path);
    e.n_ctx = n_ctx;
    e.eng.ctx = &e;
    e.eng.version = version;
    e.eng.open = l_open;
    e.eng.close = l_close;
    e.eng.run = l_run;
    return &e.eng;
}
