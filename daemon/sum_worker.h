/*
 * OpenChime — the summary worker (REQ-310, ARCH-116, docs/SUMMARIES.md §5).
 *
 * One thread, at the lowest priority the kernel has (SCHED_IDLE, nice 19), with
 * one model call at a time on one core. Everything it makes goes through one
 * summarize step and one recursion: what fits SUM_THRESHOLD_TOKENS is
 * summarized once; what does not is cut into sections that fit, each
 * summarized, and the section summaries go through the same recursion. A
 * message too big for a chunk, a thread too big for a chunk and a period are
 * all made that way, and every node already stored is reused. New nodes go to
 * the writer in one batch.
 *
 * Requests someone is waiting on come first: they are rows of
 * summary_requests, taken oldest first, and one arriving stops any idle work
 * in progress (which is done again later). When there are none and the
 * machine has been quiet a while, it summarizes what a changed message left
 * unsummarized, then every channel's chunks of the last SUM_BACKGROUND_DAYS
 * from the newest back, then the periods people asked for before that a
 * change has since purged, then the periods an older model or prompt made.
 * The load gate (sum_load.h) pauses it whenever the machine is busy.
 *
 * It reads through its own read-only connection; it never writes the database
 * itself: the sink does, through the writer.
 */
#ifndef OC_SUM_WORKER_H
#define OC_SUM_WORKER_H

#include <stddef.h>
#include <stdint.h>

#include "sum_engine.h"
#include "sum_load.h"
#include "sum_store.h"

/* How many requests may wait (summary_requests); and how long the model stays
 * loaded unused. */
#define SUM_QUEUE_MAX       64
/* How long a summary's notice is kept once it was seen (opened, dismissed, or
 * delivered live); an unseen one is kept until it is. */
#define SUM_NOTICE_KEEP_MS  (24ull * 60u * 60u * 1000u)
#define SUM_UNLOAD_IDLE_MS  (5u * 60u * 1000u)
/* How far back idle work summarizes each channel ahead of time: the longest
 * span offered as a preset. Older spans are made when someone asks. */
#define SUM_BACKGROUND_DAYS 7

/* The answer to one request, or a batch the worker made on its own. */
typedef struct {
    uint64_t    conn_id;      /* who was watching when it was taken; the store reads it again */
    uint32_t    req_id;
    int64_t     request_id;   /* its row in summary_requests, 0 for none */
    int64_t     channel, start_ms, end_ms;
    int         tz_offset_min;
    int         ok;
    int64_t     summary_id;   /* ok, and the period already stored: its id */
    const char *body;         /* ok: the summary as clients read it (SUMMARIES.md §3) */
    const char *version;
    const char *err;          /* !ok: why */
    int         dropped;      /* notes and sentences the checks took out while making it */
    int         rewrites;     /* calls that wrote a summary or a part of one again, shorter */
    int         reasks;       /* answers that ran away and were asked for once more */
    int         retries;      /* requests sent again after a 429 or a 5xx, and the wait in all */
    uint32_t    wait_ms;
} oc_sum_answer;

/* Where the worker's results go. `store` writes the batch (which may be empty)
 * and answers the request when `a->request_id` is set; when the batch is not empty
 * and a period was asked for and made (`a->ok`), its last node is that period.
 * A build that failed (`!a->ok`) still hands over the nodes it finished, to be
 * stored and found by the next build; the request is answered with the error
 * whatever the store said. It returns 0, 1 if refused because a message
 * changed meanwhile (the worker builds again), or -1. Runs on the worker's
 * thread. */
typedef struct {
    int  (*store)(void *ctx, const oc_sum_answer *a, oc_sum_new *nodes, int n);
    /* Mark queued request `row` running: 1, or 0 when it is no longer queued
     * (its connection closed). NULL: requests are not taken from the queue. */
    int  (*take)(void *ctx, int64_t row);
    void *ctx;
} oc_sum_sink;

/* Every model call, for the evaluation tool: the prompt, the answer as the model
 * wrote it (or as far as it got), and the stored summary it became (NULL when
 * it failed). */
typedef void (*oc_sum_trace_fn)(void *ctx, const char *prompt, const char *answer, const char *body);

typedef struct {
    const char          *db_path;
    const oc_sum_engine *engine;     /* borrowed */
    const oc_sum_probe  *probe;      /* NULL: the real machine */
    oc_sum_sink          sink;
    int                  background; /* summarize ahead when idle */
    size_t               threshold;  /* SUM_THRESHOLD_TOKENS, varied by the evaluation tool */
    uint64_t             gap_ms;     /* SUM_GAP_MS, likewise */
    oc_sum_trace_fn      trace;      /* NULL: none */
    void                *trace_ctx;
} oc_sum_worker_cfg;

typedef struct oc_sum_worker oc_sum_worker;

oc_sum_worker *oc_sum_worker_start(const oc_sum_worker_cfg *cfg, char *err, size_t errcap);
/* Stop: a generation in progress is abandoned, queued requests are answered
 * with an error. */
void oc_sum_worker_stop(oc_sum_worker *w);

/* A request was added to the queue (summary_requests, by the writer): the
 * worker takes it next, stopping any idle work in progress to do so. */
void oc_sum_worker_wake(oc_sum_worker *w);
/* The running request in row `row` was cancelled by its asker: it stops at its
 * next pause (the gate) and nothing of it is answered. Any thread. */
void oc_sum_worker_cancel(oc_sum_worker *w, int64_t row);

/* The version every new node is stored with. */
const char *oc_sum_worker_version(const oc_sum_worker *w);

/* Build [start, end) of `channel` now, on the calling thread, with no queue and
 * no gate: for the evaluation tool. The answer goes to the sink. 0 or -1. */
/* As oc_sum_build_now, keeping the model loaded in *engine (opened on first
 * need) for the next call: the evaluation tool runs a whole set on one load.
 * The caller closes it (cfg->engine->close). */
int oc_sum_build_keep(const oc_sum_worker_cfg *cfg, void **engine, int64_t channel, int64_t start_ms,
                      int64_t end_ms, int tz_offset_min, char *err, size_t errcap);
int oc_sum_build_now(const oc_sum_worker_cfg *cfg, int64_t channel, int64_t start_ms, int64_t end_ms,
                     int tz_offset_min, char *err, size_t errcap);

/* Summarize every chunk of `channel` whose last activity is in [start, end), on
 * the calling thread, as idle work does: for the tests. 0 or -1. */
int oc_sum_chunks_now(const oc_sum_worker_cfg *cfg, int64_t channel, int64_t start_ms, int64_t end_ms,
                      char *err, size_t errcap);

/* Where the local day holding `t` starts, for a zone `tz_offset_min` east of UTC. */
int64_t oc_sum_day_start(int64_t t, int tz_offset_min);

/* The version string for an engine and the two constants. */
void oc_sum_version(const oc_sum_engine *e, size_t threshold, uint64_t gap_ms, char *out, size_t cap);

/* A stored body as clients read it, for [start, end) of `channel`:
 * {"summary":<body>,"posters":[...],"count":N,"sources":{...}} -- who posted
 * and how many messages (oc_sum_span_people), and the messages its details
 * cite (oc_sum_sources). Heap, or NULL. */
char *oc_sum_client_body(sqlite3 *db, const char *body, int64_t channel, int64_t start_ms, int64_t end_ms);

#endif
