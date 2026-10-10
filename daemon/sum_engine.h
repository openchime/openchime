/*
 * OpenChime — the model a summary is written by (ARCH-116, docs/SUMMARIES.md).
 *
 * One table of functions, as read-aloud has (tts_render.h): the daemon runs the
 * local model through it (sum_llama.c, llama.cpp linked in), and the tests run a
 * stub. An engine answers one prompt at a time, on one thread: the summary
 * worker is its only caller.
 */
#ifndef OC_SUM_ENGINE_H
#define OC_SUM_ENGINE_H

#include <stddef.h>
#include <stdint.h>

/* Asked between steps of a generation: 0 to carry on (after waiting, if the
 * system is busy -- the generation keeps its place), non-zero to abandon it. */
typedef int (*oc_sum_gate_fn)(void *ctx);

typedef struct {
    uint32_t prompt_tokens, output_tokens;
    uint32_t cpu_ms;            /* the worker thread's CPU time for this answer */
    /* Where the time went (SUMMARIES.md §6): reading the prompt, writing the
     * answer, and the whole call as the worker waited for it -- for a hosted
     * model that includes the network and the server's queue, and reading and
     * writing are as the server reports them (0 when it does not). */
    uint32_t read_ms, write_ms, wall_ms;
    /* A hosted model's server asked for a pause (429) or failed (5xx): how
     * many times the request was sent again, and how long was waited in all. */
    uint32_t retries, wait_ms;
} oc_sum_run_stats;

typedef struct oc_sum_engine {
    void       *ctx;
    const char *version;        /* names the model; part of every summary's version */
    /* Load the model: a handle, or NULL with a reason. */
    void *(*open)(void *ctx, char *err, size_t errcap);
    void  (*close)(void *handle);
    /* Answer `user` under `system`, held to `grammar` (GBNF, root rule "root";
     * NULL: free), writing at most `max_out` tokens (0: whatever the context
     * has left). The answer, NUL-terminated, into a heap string at *out. 0, or
     * -1 with a reason (or abandoned by the gate); an answer that reached
     * `max_out` is left at *out as far as it got, for the log. */
    int   (*run)(void *handle, const char *system, const char *user, const char *grammar, int max_out,
                 oc_sum_gate_fn gate, void *gate_ctx, char **out, oc_sum_run_stats *st,
                 char *err, size_t errcap);
    int   remote;               /* the model runs elsewhere: this machine's load does not hold it */
} oc_sum_engine;

/* The local engine over the GGUF file at `path` (borrowed), with a context of
 * `n_ctx` tokens, on one thread. Exists only in a daemon built with SUM=1. */
const oc_sum_engine *oc_sum_llama_engine(const char *path, const char *version, int n_ctx);

/* The hosted engine: each answer is one request to the chat-completions
 * endpoint under `url` (an OpenAI-style API; https, or http to a loopback
 * address), asking for `model`, with `api_key` (may be empty) as its bearer
 * token, in the fields every such API reads. All three are borrowed. */
const oc_sum_engine *oc_sum_cloud_engine(const char *url, const char *model, const char *api_key);

#endif
