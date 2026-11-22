/*
 * OpenChime — the export worker (ARCH-135, docs/details/VIDEO-MESSAGES.md "Export").
 *
 * One thread beside the net loop, at a lowered priority, so a transcode never
 * starves the conversations the daemon is serving. The net thread submits an
 * export (the video's attachment and where its bytes are); the worker reads the
 * source from the blob store, writes the copy to a temporary file, stores it in
 * the blob store and posts a result, waking the net loop through an eventfd --
 * the shape of the read-aloud worker (tts_worker.h). The net thread never waits
 * on a transcode.
 *
 * One transcode at a time, and none while a read-aloud rendering runs (the
 * gate): on the managed box the two together would not fit in its memory. A
 * request for a video already queued or being exported joins that export rather
 * than starting another, and each person may have OC_EXPORT_PER_USER requests
 * queued or running at once.
 *
 * Copies are stored under a key made from the attachment and the codecs'
 * version (`<16 hex digits of the attachment>_<16 of version hash>`: the blob
 * store takes hex, '-' and '_' only, and the '_' and length alone tell a copy
 * from an attachment's 16 digits and a rendering's 81 characters).
 */
#ifndef OC_EXPORT_WORKER_H
#define OC_EXPORT_WORKER_H

#include <stddef.h>
#include <stdint.h>
#include "blobstore.h"
#include "export_render.h"

#define OC_EXPORT_KEY_MAX   40
#define OC_EXPORT_PER_USER  3      /* requests one person may have queued or running */
#define OC_EXPORT_QUEUE     16     /* exports waiting, across everyone */

typedef struct oc_export_worker oc_export_worker;

typedef struct {
    uint64_t         attachment_id;
    oc_export_status status;
    char             key[OC_EXPORT_KEY_MAX];   /* OC_EXPORT_OK: where the copy is stored */
    uint64_t         bytes;
    uint32_t         duration_ms;
    uint64_t        *req_ids;                  /* every request this answers; oc_export_result_free */
    size_t           n_req;
    char             reason[160];              /* otherwise: why, for the log */
} oc_export_result;

/* While it returns non-zero the worker starts no transcode: the daemon passes
 * one that says whether a read-aloud rendering is running. NULL: never wait. */
typedef int (*oc_export_gate_fn)(void *ctx);

/* Start the worker over `codecs` and `bs` (both borrowed, outliving it),
 * writing its temporary files in `tmp_dir`, where any it left before (a
 * daemon that stopped mid-export) are removed now. NULL on failure. */
oc_export_worker *oc_export_worker_start(const oc_export_codecs *codecs, oc_blobstore *bs, const char *tmp_dir,
                                         oc_export_gate_fn gate, void *gate_ctx);
/* Stop: the transcode in progress is cancelled and its file removed, queued
 * ones are dropped without results. */
void oc_export_worker_stop(oc_export_worker *w);

/* Readable whenever at least one result is waiting; register it in epoll. The
 * caller reads (and discards) the counter, then drains with next_result. */
int oc_export_worker_eventfd(oc_export_worker *w);

/* Ask for the copy of `attachment_id`, whose `src_size`-byte source is the
 * blob `src_key`, for `user_id`, answering `req_id`. 0: queued; 1: joined the
 * export of the same video already queued or running; -1: the queue is full or
 * the worker stopping; -2: this person already has OC_EXPORT_PER_USER. */
int oc_export_worker_submit(oc_export_worker *w, uint64_t req_id, uint64_t user_id, uint64_t attachment_id,
                            const char *src_key, uint64_t src_size);

/* The next finished export into `out`: 1 if there was one, 0 if none. Free
 * it with oc_export_result_free. */
int  oc_export_worker_next_result(oc_export_worker *w, oc_export_result *out);
void oc_export_result_free(oc_export_result *r);

/* The blob key a copy of `attachment_id` made by `codecs` is stored under. */
void oc_export_copy_key(const oc_export_codecs *codecs, uint64_t attachment_id, char *key, size_t cap);

/* How many transcodes have started (for tests and the log). */
uint64_t oc_export_worker_started(oc_export_worker *w);

#endif
