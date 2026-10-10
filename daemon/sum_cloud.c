/* The hosted summary model (sum_engine.h, ARCH-116, docs/SUMMARIES.md §6).
 *
 * Each answer is one request to an OpenAI-style chat-completions endpoint: the
 * same system and user prompt the local engine is given, in the fields every
 * such endpoint reads (`model`, `messages`, `temperature`, `max_tokens`) and
 * nothing else, so any provider answers it. The grammar the local engine holds
 * the model to is not sent: the shape is asked for in the prompt and checked by
 * the parser, as it is for every answer. The prompt is the same for every
 * model. A `<think>` block a model opens its answer with is dropped; what it
 * spent on it came out of the answer's tokens.
 *
 * Opening the engine asks the API a one-line prompt. A model that reasons
 * before it answers spends the answer's tokens on that: the API says so in the
 * usage it reports (`completion_tokens_details.reasoning_tokens`, OpenAI's
 * shape) or by answering nothing. Such a model is asked again with each of the
 * fields APIs have for turning that off (OpenAI's, OpenRouter's, llama.cpp's
 * and vLLM's, in turn), and the first the API honours is sent with every
 * request from then on. None is tied to a model's name: what the server
 * answers decides. A model that cannot be told not to reason leaves summaries
 * off, with that as the reason.
 *
 * A server that asks for a pause (429) or fails (500, 502, 503, 504), or
 * cannot be reached, is sent the same request again after a wait: the
 * Retry-After it names, else 2, 4, 8, 16, 32 seconds, never more than
 * SUM_CLOUD_WAIT_MAX_S, at most SUM_CLOUD_RETRIES times. One log line a wait.
 * Still failing after the last, the error is the server's.
 *
 * Blocking, on the summary worker's thread, like the local engine. The gate is
 * asked before the request and during a wait: a request in flight is not
 * paused. */
#define _POSIX_C_SOURCE 200809L
#include "sum_engine.h"
#include "sum_core.h"
#include "https_client.h"
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* How long one read of the answer may wait. A long prompt on a busy server can
 * take minutes before the first byte; the worker waits, as it would for the
 * local model. */
#define SUM_CLOUD_TIMEOUT_MS 600000
/* Sent again after a 429, a 5xx or no connection: how many times, and the
 * longest one wait may be. */
#define SUM_CLOUD_RETRIES    5
#define SUM_CLOUD_WAIT_MAX_S 60

typedef struct {
    oc_sum_engine eng;
    char url[1024];       /* .../chat/completions */
    char model[256];
    char auth[600];       /* the Authorization header line, or "" */
    const char *extra;    /* a field every request carries, once learned; "" */
    int  probed;          /* open has asked its one-line prompt */
    int  tried;           /* the fields were tried on a real request */
} cloud_engine;

/* The fields APIs read to answer without reasoning first, tried in this order. */
static const char *const QUIET[] = {
    "\"reasoning_effort\":\"none\"",                      /* OpenAI's */
    "\"reasoning\":{\"enabled\":false}",                  /* OpenRouter's */
    "\"chat_template_kwargs\":{\"enable_thinking\":false}", /* llama.cpp's and vLLM's */
};
#define N_QUIET ((int)(sizeof QUIET / sizeof QUIET[0]))

static uint32_t now_ms(clockid_t c) {
    struct timespec ts;
    clock_gettime(c, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void c_close(void *h) { (void)h; }

/* The number at token `i` (an integer or not), or 0. */
static double num(const oc_json *d, int i) {
    if (i < 0 || d->t[i].type != JSMN_PRIMITIVE) return 0;
    char b[64];
    int n = d->t[i].end - d->t[i].start;
    if (n <= 0 || n >= (int)sizeof b) return 0;
    memcpy(b, d->js + d->t[i].start, (size_t)n);
    b[n] = '\0';
    return strtod(b, NULL);
}

/* The string at token `i`, unescaped, on the heap; NULL if it is not one. */
static char *str_dup(const oc_json *d, int i) {
    if (i < 0 || d->t[i].type != JSMN_STRING) return NULL;
    size_t cap = (size_t)(d->t[i].end - d->t[i].start) + 1;
    char *s = malloc(cap);
    if (s && oc_json_str(d, i, s, cap) != 0) { free(s); s = NULL; }
    return s;
}

/* One request. 0 with the answer's text at *out (heap; "" when the model wrote
 * nothing), how many of its tokens went to reasoning at *reasoning, and the
 * finish reason in `fin`; -1 with a reason when the API did not answer. */
/* 1 when status is one the server may answer differently in a moment. */
static int again_later(int status) {
    return status == 429 || status == 500 || status == 502 || status == 503 || status == 504;
}

/* Wait `secs` seconds, asking the gate every quarter second; 0, or -1 when the
 * gate said to stop. */
static int pause_for(int secs, oc_sum_gate_fn gate, void *gctx) {
    for (int q = 0; q < secs * 4; q++) {
        if (gate && gate(gctx) != 0) return -1;
        struct timespec ts = { 0, 250 * 1000000L };
        nanosleep(&ts, NULL);
    }
    return 0;
}

static int request(cloud_engine *e, const char *system, const char *user, int max_out, const char *extra,
                   oc_sum_gate_fn gate, void *gctx, char **out, uint32_t *reasoning, char *fin, size_t fincap,
                   oc_sum_run_stats *st, char *err, size_t errcap) {
    *out = NULL;
    *reasoning = 0;
    fin[0] = '\0';
    oc_sum_buf b = {0};
    oc_sum_buf_puts(&b, "{\"model\":");
    oc_sum_buf_json(&b, e->model);
    oc_sum_buf_puts(&b, ",\"messages\":[{\"role\":\"system\",\"content\":");
    oc_sum_buf_json(&b, system);
    oc_sum_buf_puts(&b, "},{\"role\":\"user\",\"content\":");
    oc_sum_buf_json(&b, user);
    oc_sum_buf_puts(&b, "}],\"stream\":false,\"temperature\":0");
    if (max_out > 0) oc_sum_buf_printf(&b, ",\"max_tokens\":%d", max_out);
    if (extra && *extra) oc_sum_buf_printf(&b, ",%s", extra);
    oc_sum_buf_puts(&b, "}");
    if (b.oom) { oc_sum_buf_free(&b); snprintf(err, errcap, "out of memory"); return -1; }

    oc_https_resp r;
    memset(&r, 0, sizeof r);
    char herr[256] = "";
    int rc = -1;
    oc_json d;
    memset(&d, 0, sizeof d);
    for (int try = 0; ; try++) {
        oc_https_resp_free(&r);
        memset(&r, 0, sizeof r);
        int sent = oc_https_request("POST", e->url, "application/json", b.p, b.n, e->auth[0] ? e->auth : NULL,
                                    SUM_CLOUD_TIMEOUT_MS, &r, herr, sizeof herr) == 0;
        if (sent && r.status == 200) break;
        if (sent) snprintf(err, errcap, "the model server answered %d: %.200s", r.status, r.body ? r.body : "");
        else snprintf(err, errcap, "the model server could not be reached: %s", herr);
        if (try >= SUM_CLOUD_RETRIES || (sent && !again_later(r.status))) goto done;
        /* The server's own wait when it names one in seconds; else doubling. */
        int wait = 2 << try;
        char ra[64] = "";
        if (sent && oc_https_header(&r, "Retry-After", ra, sizeof ra) && ra[0] >= '0' && ra[0] <= '9') {
            long v = strtol(ra, NULL, 10);
            if (v >= 0) wait = (int)v;
        }
        if (wait > SUM_CLOUD_WAIT_MAX_S) wait = SUM_CLOUD_WAIT_MAX_S;
        if (sent)
            fprintf(stderr, "summary: the model server answered %d; waiting %d s, then try %d of %d\n", r.status,
                    wait, try + 2, SUM_CLOUD_RETRIES + 1);
        else
            fprintf(stderr, "summary: the model server could not be reached (%s); waiting %d s, then try %d of %d\n",
                    herr, wait, try + 2, SUM_CLOUD_RETRIES + 1);
        if (st) { st->retries++; st->wait_ms += (uint32_t)wait * 1000u; }
        if (pause_for(wait, gate, gctx) != 0) { snprintf(err, errcap, "stopped"); goto done; }
    }
    if (oc_json_parse(&d, r.body, r.body_len) != 0 || d.n < 1 || d.t[0].type != JSMN_OBJECT) {
        snprintf(err, errcap, "the model server's answer is not JSON");
        goto done;
    }
    int ch = oc_json_get(&d, 0, "choices");
    if (ch < 0 || d.t[ch].type != JSMN_ARRAY || d.t[ch].size < 1 || d.t[ch + 1].type != JSMN_OBJECT) {
        snprintf(err, errcap, "the model server's answer has no choices");
        goto done;
    }
    int c0 = ch + 1;
    int msg = oc_json_get(&d, c0, "message");
    char *text = msg >= 0 ? str_dup(&d, oc_json_get(&d, msg, "content")) : NULL;
    if (!text && !(text = strdup(""))) { snprintf(err, errcap, "out of memory"); goto done; }
    /* A thinking model may open with its reasoning; the answer is what follows. */
    if (!strncmp(text, "<think>", 7)) {
        const char *end = strstr(text, "</think>");
        if (end) {
            end += 8;
            while (*end == '\n' || *end == ' ') end++;
            memmove(text, end, strlen(end) + 1);
        }
    }
    int usage = oc_json_get(&d, 0, "usage");
    int tm = oc_json_get(&d, 0, "timings");
    if (usage >= 0) {
        int det = oc_json_get(&d, usage, "completion_tokens_details");
        if (det >= 0) *reasoning = (uint32_t)num(&d, oc_json_get(&d, det, "reasoning_tokens"));
    }
    if (st) {
        if (usage >= 0) {
            st->prompt_tokens = (uint32_t)num(&d, oc_json_get(&d, usage, "prompt_tokens"));
            st->output_tokens = (uint32_t)num(&d, oc_json_get(&d, usage, "completion_tokens"));
        }
        /* llama.cpp's server says where its time went. */
        if (tm >= 0) {
            st->read_ms = (uint32_t)num(&d, oc_json_get(&d, tm, "prompt_ms"));
            st->write_ms = (uint32_t)num(&d, oc_json_get(&d, tm, "predicted_ms"));
        }
    }
    oc_json_get_str(&d, c0, "finish_reason", fin, fincap);
    *out = text;
    rc = 0;
done:
    oc_json_free(&d);
    oc_https_resp_free(&r);
    oc_sum_buf_free(&b);
    return rc;
}

static int c_run(void *vh, const char *system, const char *user, const char *grammar, int max_out,
                 oc_sum_gate_fn gate, void *gate_ctx, char **out, oc_sum_run_stats *st, char *err,
                 size_t errcap) {
    cloud_engine *e = vh;
    uint32_t t0 = now_ms(CLOCK_MONOTONIC), cpu0 = now_ms(CLOCK_THREAD_CPUTIME_ID);
    *out = NULL;
    if (st) memset(st, 0, sizeof *st);
    (void)grammar;
    int rc = -1;
    if (gate && gate(gate_ctx) != 0) { snprintf(err, errcap, "stopped"); goto done; }
    uint32_t reasoning = 0;
    char fin[32];
    if (request(e, system, user, max_out, e->extra, gate, gate_ctx, out, &reasoning, fin, sizeof fin, st, err,
                errcap) != 0)
        goto done;
    /* A model that reasons only when it finds a prompt worth it passed the
     * one-line prompt at open; the first request it reasons on is asked again
     * with each field for turning that off, and the first honoured is kept. */
    if (reasoning > 0 && !*e->extra && !e->tried) {
        e->tried = 1;
        for (int i = 0; i < N_QUIET; i++) {
            char *again = NULL, qfin[32], qerr[300];
            uint32_t r = 0;
            oc_sum_run_stats qst;
            memset(&qst, 0, sizeof qst);
            if (request(e, system, user, max_out, QUIET[i], gate, gate_ctx, &again, &r, qfin, sizeof qfin, &qst, qerr,
                        sizeof qerr) == 0 &&
                r == 0 && *again) {
                free(*out);
                *out = again;
                snprintf(fin, sizeof fin, "%s", qfin);
                if (st) { qst.retries += st->retries; qst.wait_ms += st->wait_ms; *st = qst; }
                e->extra = QUIET[i];
                fprintf(stderr, "summary: the hosted model reasons before answering; every request now carries %s\n",
                        QUIET[i]);
                break;
            }
            free(again);
        }
        if (!*e->extra)
            fprintf(stderr, "summary: the hosted model reasons before answering (%u tokens of this answer) and cannot be told not to\n",
                    (unsigned)reasoning);
    }
    if (!strcmp(fin, "length")) {
        /* As the local engine reports it; the text is left for the log. */
        snprintf(err, errcap, "the answer reached its %d tokens without ending", max_out);
        goto done;
    }
    if (!**out) { snprintf(err, errcap, "the model server's answer has no text"); goto done; }
    rc = 0;
done:
    if (st) {
        st->wall_ms = now_ms(CLOCK_MONOTONIC) - t0;
        st->cpu_ms = now_ms(CLOCK_THREAD_CPUTIME_ID) - cpu0;
    }
    return rc;
}

/* The one-line prompt: answered with text and without reasoning, or not. */
static int probe(cloud_engine *e, const char *extra, uint32_t *reasoning, char *err, size_t errcap) {
    char *ans = NULL, fin[32];
    int rc = request(e, "Answer in one word.", "Say OK.", 16, extra, NULL, NULL, &ans, reasoning, fin, sizeof fin, NULL,
                     err, errcap);
    if (rc == 0 && !*ans) { snprintf(err, errcap, "the model server's answer has no text"); rc = -1; }
    free(ans);
    return rc;
}

/* The handle is the engine. The first open asks the one-line prompt and
 * learns whether a field is needed to keep the model from reasoning first. */
static void *c_open(void *ctx, char *err, size_t errcap) {
    cloud_engine *e = ctx;
    char local[300];
    if (!err) { err = local; errcap = sizeof local; }
    if (e->probed) return e;
    uint32_t reasoning = 0;
    int plain = probe(e, "", &reasoning, err, errcap);
    if (plain == 0 && reasoning == 0) { e->probed = 1; return e; }
    char first[300];
    snprintf(first, sizeof first, "%s", err);
    for (int i = 0; i < N_QUIET; i++) {
        uint32_t r = 0;
        char qerr[300];
        if (probe(e, QUIET[i], &r, qerr, sizeof qerr) == 0 && r == 0) {
            e->extra = QUIET[i];
            e->probed = 1;
            fprintf(stderr, "summary: the hosted model reasons before answering; every request now carries %s\n",
                    QUIET[i]);
            return e;
        }
    }
    if (plain == 0) {
        /* It answers, spending some of every answer on reasoning it cannot be
         * told to skip. */
        e->probed = 1;
        fprintf(stderr, "summary: the hosted model reasons before answering (%u tokens of 16) and cannot be told not to\n",
                (unsigned)reasoning);
        return e;
    }
    snprintf(err, errcap, "the hosted model reasons before answering and cannot be told not to (%s)", first);
    return NULL;
}

const oc_sum_engine *oc_sum_cloud_engine(const char *url, const char *model, const char *api_key) {
    static cloud_engine e;
    size_t n = strlen(url);
    while (n && url[n - 1] == '/') n--;
    snprintf(e.url, sizeof e.url, "%.*s/chat/completions", (int)n, url);
    snprintf(e.model, sizeof e.model, "%s", model);
    e.auth[0] = '\0';
    if (api_key && *api_key) snprintf(e.auth, sizeof e.auth, "Authorization: Bearer %s\r\n", api_key);
    e.extra = "";
    e.probed = 0;
    e.tried = 0;
    e.eng.ctx = &e;
    e.eng.version = e.model;
    e.eng.remote = 1;
    e.eng.open = c_open;
    e.eng.close = c_close;
    e.eng.run = c_run;
    return &e.eng;
}
