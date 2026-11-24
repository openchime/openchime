/*
 * OpenChime — the recordings' worker (CALLS.md §8).
 *
 * One thread beside the net loop, below it in priority, as the export's is
 * (export_worker.h): the net thread hands it a stopped recording -- its id and
 * its spool -- and it makes the recording (callrec_render.h) in a temporary
 * file beside the spools, stores it in the blob store and posts a result,
 * waking the net loop through an eventfd. A recording made, or one whose spool
 * cannot be made into one, has its spool removed; a daemon stopping in the
 * middle leaves the spool, and the recording is made again from it when the
 * daemon starts. One at a time.
 *
 * A recording is stored under `ca11_<16 hex digits of its id>`: the blob
 * store takes only hex, '-' and '_', and the prefix tells it from an
 * attachment's 16 digits and an export's two sets of them.
 */
#ifndef OC_CALLREC_WORKER_H
#define OC_CALLREC_WORKER_H

#include <stddef.h>
#include <stdint.h>

#include "blobstore.h"
#include "callrec_render.h"

#define OC_CALLREC_KEY_MAX  32
#define OC_CALLREC_QUEUE    64

typedef struct oc_callrec_worker oc_callrec_worker;

typedef struct {
    uint64_t          rec_id;
    oc_callrec_status status;
    char              key[OC_CALLREC_KEY_MAX];   /* OC_CALLREC_OK: where it is stored */
    uint64_t          bytes;
    uint32_t          duration_ms;
    uint8_t           sha256[32];                /* OC_CALLREC_OK: the file's digest, as an attachment has */
    char              reason[160];               /* otherwise: why */
} oc_callrec_result;

/* While it returns non-zero the worker starts nothing: the daemon passes one
 * that says whether a read-aloud rendering is running. NULL: never wait. */
typedef int (*oc_callrec_gate_fn)(void *ctx);

/* Start over `codecs` and `bs` (borrowed), its temporary files in `tmp_dir`,
 * where any it left before are removed now. NULL on failure. */
oc_callrec_worker *oc_callrec_worker_start(const oc_callrec_codecs *codecs, oc_blobstore *bs, const char *tmp_dir,
                                           oc_callrec_gate_fn gate, void *gate_ctx);
/* Stop: the recording in progress is cancelled, its spool kept for the next
 * start; queued ones are dropped, their spools kept. */
void oc_callrec_worker_stop(oc_callrec_worker *w);
int  oc_callrec_worker_eventfd(oc_callrec_worker *w);
/* Make recording `rec_id` from the spool at `spool_dir`. 0: queued (or
 * already queued or being made); -1: the queue is full or the worker stopping. */
int  oc_callrec_worker_submit(oc_callrec_worker *w, uint64_t rec_id, const char *spool_dir);
/* The next result into `out`: 1, or 0 if none. */
int  oc_callrec_worker_next_result(oc_callrec_worker *w, oc_callrec_result *out);
/* For tests: while held, nothing is started. */
void oc_callrec_worker_hold(oc_callrec_worker *w, int on);

/* The blob key recording `rec_id` is stored under. */
void oc_callrec_blob_key(uint64_t rec_id, char *key, size_t cap);

#endif
