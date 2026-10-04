/*
 * OpenChime — the summary worker (REQ-310, ARCH-116, docs/SUMMARIES.md §5).
 *
 * One thread, at the lowest priority the kernel has (SCHED_IDLE, nice 19), with
 * one model call at a time on one core. It builds a period's summary from the
 * bottom up -- chunk summaries, a big thread's summary, sections, the period --
 * reusing every node already stored, and hands the new nodes to the writer in
 * one batch. Requests someone is waiting on come first; when there are none and
 * the machine has been quiet a while, it summarizes recent days of active
 * channels ahead of being asked, and rebuilds what an older model or prompt
 * made. The load gate (sum_load.h) pauses it whenever the machine is busy.
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

/* How many requests may wait; and how long the model stays loaded unused. */
#define SUM_QUEUE_MAX       64
#define SUM_UNLOAD_IDLE_MS  (5u * 60u * 1000u)
/* Background work: how many past days of an active channel to summarize ahead,
 * how often to look for them, and how long an unused node is kept. */
#define SUM_IDLE_DAYS       7
#define SUM_IDLE_SCAN_MS    (10u * 60u * 1000u)
#define SUM_KEEP_MS         (30ull * 24u * 3600u * 1000u)
/* A model's answer is at most this many tokens; the context is sized for the
 * largest piece plus the prompt plus the answer. */
#define SUM_MAX_OUT         2048
#define SUM_CTX_TOKENS      (SUM_THRESHOLD_TOKENS + 1024 + SUM_MAX_OUT)

/* The answer to one request, or a batch the worker made on its own. */
typedef struct {
    uint64_t    conn_id;      /* 0: nobody is waiting */
    uint32_t    req_id;
    int64_t     channel, start_ms, end_ms;
    int         tz_offset_min;
    int         ok;
    int64_t     summary_id;   /* ok, and the period already stored: its id */
    const char *body;         /* ok: the summary as clients read it (SUMMARIES.md §3) */
    const char *version;
    const char *err;          /* !ok: why */
} oc_sum_answer;

/* Where the worker's results go. `store` writes the batch (which may be empty)
 * and answers the request when `a->conn_id` is set; when the batch is not empty
 * its last node is the period answered. It returns 0, 1 if refused because a
 * message changed meanwhile (the worker builds again), or -1. `collect` removes
 * nodes unused for SUM_KEEP_MS. Both run on the worker's thread. */
typedef struct {
    int  (*store)(void *ctx, const oc_sum_answer *a, oc_sum_new *nodes, int n);
    void (*collect)(void *ctx);
    void *ctx;
} oc_sum_sink;

typedef struct {
    const char          *db_path;
    const oc_sum_engine *engine;     /* borrowed */
    const oc_sum_probe  *probe;      /* NULL: the real machine */
    oc_sum_sink          sink;
    int                  background; /* summarize ahead when idle */
    size_t               threshold;  /* SUM_THRESHOLD_TOKENS, varied by the evaluation tool */
    uint64_t             gap_ms;     /* SUM_GAP_MS, likewise */
} oc_sum_worker_cfg;

typedef struct oc_sum_worker oc_sum_worker;

oc_sum_worker *oc_sum_worker_start(const oc_sum_worker_cfg *cfg, char *err, size_t errcap);
/* Stop: a generation in progress is abandoned, queued requests are answered
 * with an error. */
void oc_sum_worker_stop(oc_sum_worker *w);

/* Queue a request someone is waiting on. 0, or -1 when the queue is full. */
int oc_sum_worker_request(oc_sum_worker *w, uint64_t conn_id, uint32_t req_id, int64_t channel,
                          int64_t start_ms, int64_t end_ms, int tz_offset_min);

/* The version every new node is stored with. */
const char *oc_sum_worker_version(const oc_sum_worker *w);

/* Build [start, end) of `channel` now, on the calling thread, with no queue and
 * no gate: for the evaluation tool. The answer goes to the sink. 0 or -1. */
int oc_sum_build_now(const oc_sum_worker_cfg *cfg, int64_t channel, int64_t start_ms, int64_t end_ms,
                     int tz_offset_min, char *err, size_t errcap);

/* Where the local day holding `t` starts, for a zone `tz_offset_min` east of UTC. */
int64_t oc_sum_day_start(int64_t t, int tz_offset_min);

/* The version string for an engine and the two constants. */
void oc_sum_version(const oc_sum_engine *e, size_t threshold, uint64_t gap_ms, char *out, size_t cap);

/* A stored body as clients read it: {"summary":<body>,"people":{"<id>":"name"}}.
 * Heap, or NULL. */
char *oc_sum_client_body(sqlite3 *db, const char *body);

#endif
