/*
 * OpenChime — the voice-input recognition worker (ARCH-112,
 * docs/VOICE-INPUT.md §6).
 *
 * One thread beside the net loop and beside read-aloud's render worker, so a long
 * render never delays someone mid-sentence. The net thread submits a whole
 * segment of speech to a bounded queue; the worker recognizes it and posts the
 * text, waking the net loop through an eventfd -- the shape of the render worker
 * (tts_worker.h). The engine is opened on the first segment and closed after
 * `idle_ms` with nothing queued. Results come back in the order segments went in.
 *
 * The audio is the worker's once submitted and is freed as soon as it has been
 * heard: it is never written anywhere.
 */
#ifndef OC_STT_WORKER_H
#define OC_STT_WORKER_H

#include <stddef.h>
#include <stdint.h>

#include "stt_render.h"

typedef struct oc_stt_worker oc_stt_worker;

typedef struct {
    uint64_t req_id;
    int      ok;          /* 1: `text` holds the words; 0: recognition failed */
    char    *text;        /* malloc'd; the caller frees it */
    char     reason[160]; /* !ok: why, for the log */
} oc_stt_result;

/* Start over `engine` (borrowed, must outlive the worker). `queue_cap` bounds
 * segments waiting (at least 1). NULL on failure. */
oc_stt_worker *oc_stt_worker_start(const oc_stt_engine *engine, size_t queue_cap, unsigned idle_ms);

/* Stop: the segment in progress finishes, queued ones are dropped without
 * results, the engine is closed. */
void oc_stt_worker_stop(oc_stt_worker *w);

/* Readable whenever a result is waiting. */
int oc_stt_worker_eventfd(oc_stt_worker *w);

/* Queue a segment. Takes ownership of `pcm` (malloc'd) whether it succeeds or
 * not. Returns 0, or -1 if the queue is full. */
int oc_stt_worker_submit(oc_stt_worker *w, uint64_t req_id, int16_t *pcm, size_t samples);

/* The next finished segment into `out`: 1 if there was one, 0 if none. */
int oc_stt_worker_next_result(oc_stt_worker *w, oc_stt_result *out);

/* Whether the engine is open right now (for tests and the log). */
int oc_stt_worker_engine_open(oc_stt_worker *w);

#endif
