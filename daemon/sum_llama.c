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
/* DRY ("don't repeat yourself") at its published defaults: a token that would
 * extend a sequence already written, longer than the allowed length, is
 * penalized by multiplier * base^(length - allowed). It stops greedy decoding
 * looping without penalizing the names and line numbers a summary repeats
 * (SUMMARIES.md §6). Colons, quotes and asterisks break sequences. */
#define SUM_DRY_MULTIPLIER   0.8f
#define SUM_DRY_BASE         1.75f
#define SUM_DRY_ALLOWED      2
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
    int                   used;        /* tokens in the context now */
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

static uint32_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
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
    /* The context cache at 8 bits: about half the memory of 16, which is what
     * keeps a model and its context inside the budget (SUMMARIES.md §6). A
     * quantized V cache needs flash attention. */
    cp.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_ENABLED;
    cp.type_k = GGML_TYPE_Q8_0;
    cp.type_v = GGML_TYPE_Q8_0;
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

/* Read `tok` into the context after what is there, in batches, asking the gate
 * between them. 0, or -1 with a reason. */
static int feed(llama_handle *h, llama_token *tok, int nt, oc_sum_gate_fn gate, void *gate_ctx, char *err,
                size_t errcap) {
    for (int at = 0; at < nt; at += SUM_UBATCH) {
        if (gate && gate(gate_ctx) != 0) { snprintf(err, errcap, "stopped"); return -1; }
        int n = nt - at < SUM_UBATCH ? nt - at : SUM_UBATCH;
        if (llama_decode(h->ctx, llama_batch_get_one(tok + at, n)) != 0) {
            snprintf(err, errcap, "the model could not read the prompt");
            return -1;
        }
        h->used += n;
    }
    return 0;
}

/* `text` as tokens (special tokens read as such; the model's start of text first
 * when `start` and the model has one), on the heap; how many, or -1. */
static int tokenize(llama_handle *h, const char *text, int start, llama_token **out) {
    int len = (int)strlen(text), cap = len + 16;
    *out = malloc((size_t)cap * sizeof **out);
    if (!*out) return -1;
    int n = llama_tokenize(h->vocab, text, len, *out, cap, start != 0, true);
    if (n < 0) { free(*out); *out = NULL; }
    return n;
}

/* Write an answer: greedy, after DRY, held to `grammar` when there is one, until
 * the model ends it or the context is full. */
static int generate(llama_handle *h, const char *grammar, int max_out, oc_sum_gate_fn gate, void *gate_ctx,
                    char **out, oc_sum_run_stats *st, char *err, size_t errcap) {
    /* The answer may use whatever the context has left. */
    if (max_out <= 0 || max_out > h->n_ctx - h->used) max_out = h->n_ctx - h->used;
    if (max_out <= 0) {
        snprintf(err, errcap, "the prompt is %d tokens, too long for a context of %d", h->used, h->n_ctx);
        return -1;
    }
    struct llama_sampler *smpl = llama_sampler_chain_init(llama_sampler_chain_default_params());
    if (grammar) {
        struct llama_sampler *g = llama_sampler_init_grammar(h->vocab, grammar, "root");
        if (!g) {
            llama_sampler_free(smpl);
            snprintf(err, errcap, "the grammar does not parse");
            return -1;
        }
        llama_sampler_chain_add(smpl, g);
    }
    /* Not the line break: a line repeated in a list is the repetition to stop. */
    static const char *breakers[] = { ":", "\"", "*" };
    /* Over the whole context: the library takes a count (its callers' -1 for
     * "the context" means nothing to it, and 0 turns DRY off). */
    llama_sampler_chain_add(smpl, llama_sampler_init_dry(h->vocab, SUM_DRY_MULTIPLIER, SUM_DRY_BASE, SUM_DRY_ALLOWED,
                                                         h->n_ctx, breakers, sizeof breakers / sizeof *breakers));
    llama_sampler_chain_add(smpl, llama_sampler_init_greedy());

    int rc = -1, produced = 0;
    size_t ocap = 4096, on = 0;
    char *o = malloc(ocap);
    if (!o) { snprintf(err, errcap, "out of memory"); goto done; }
    o[0] = '\0';
    for (;;) {
        if (produced >= max_out) {
            snprintf(err, errcap, "the answer reached its %d tokens without ending", max_out);
            *out = o;           /* as far as it got, for the log */
            o = NULL;
            goto done;
        }
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
        h->used++;
    }
    *out = o;
    o = NULL;
    rc = 0;
done:
    if (st) st->output_tokens += (uint32_t)produced;
    free(o);
    llama_sampler_free(smpl);
    return rc;
}

/* A model that thinks before it answers (its template has <think>): the empty
 * thought its own template writes when thinking is not asked for, which the
 * library's built-in formats leave out. */
static const char *no_think(const llama_handle *h) {
    return h->tmpl && strstr(h->tmpl, "<think>") ? "<think>\n\n</think>\n\n" : "";
}

static int l_run(void *vh, const char *system, const char *user, const char *grammar, int max_out,
                 oc_sum_gate_fn gate, void *gate_ctx, char **out, oc_sum_run_stats *st, char *err,
                 size_t errcap) {
    llama_handle *h = vh;
    uint32_t cpu0 = thread_cpu_ms(), t0 = mono_ms();
    *out = NULL;
    if (st) memset(st, 0, sizeof *st);

    /* The model's own chat format. */
    llama_chat_message msgs[2] = { { "system", system }, { "user", user } };
    const char *nt_text = no_think(h);
    size_t cap = 2 * (strlen(system) + strlen(user)) + 1024;
    char *prompt = malloc(cap);
    if (!prompt) { snprintf(err, errcap, "out of memory"); return -1; }
    int pl = llama_chat_apply_template(h->tmpl, msgs, 2, true, prompt, (int32_t)cap);
    if (pl < 0) {
        /* No template the library knows: a plain one. */
        pl = snprintf(prompt, cap, "%s\n\n%s\n\nAnswer:\n", system, user);
        nt_text = "";
    } else if ((size_t)pl + strlen(nt_text) >= cap) {
        cap = (size_t)pl + strlen(nt_text) + 1;
        char *np = realloc(prompt, cap);
        if (!np) { free(prompt); snprintf(err, errcap, "out of memory"); return -1; }
        prompt = np;
        llama_chat_apply_template(h->tmpl, msgs, 2, true, prompt, (int32_t)cap);
    }
    memcpy(prompt + pl, nt_text, strlen(nt_text) + 1);

    llama_token *tok = NULL;
    int nt = tokenize(h, prompt, 1, &tok);
    free(prompt);
    if (nt < 0) { snprintf(err, errcap, "the prompt does not tokenize"); return -1; }
    if (nt >= h->n_ctx) {
        free(tok);
        snprintf(err, errcap, "the prompt is %d tokens, too long for a context of %d", nt, h->n_ctx);
        return -1;
    }
    llama_memory_clear(llama_get_memory(h->ctx), true);
    h->used = 0;
    int rc = -1;
    uint32_t t1 = mono_ms(), t2 = t1;
    if (feed(h, tok, nt, gate, gate_ctx, err, errcap) != 0) goto done;
    t2 = mono_ms();
    if (st) st->prompt_tokens = (uint32_t)h->used;
    rc = generate(h, grammar, max_out, gate, gate_ctx, out, st, err, errcap);
done:
    free(tok);
    if (st) {
        uint32_t t3 = mono_ms();
        st->cpu_ms = thread_cpu_ms() - cpu0;
        st->read_ms = t2 - t1;
        st->write_ms = t3 - t2;
        st->wall_ms = t3 - t0;
    }
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
