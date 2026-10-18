/*
 * OpenChime — the read-aloud render worker (ARCH-111, docs/READ-ALOUD.md §4).
 *
 * One thread beside the net loop. The net thread submits a render (a request id,
 * the 32-byte handle, a voice, the speakable text) to a bounded queue; the worker
 * renders it, stores the file in the blob store and posts a result, waking the
 * net loop through an eventfd -- the shape of the transfer pool (ARCH-69) and the
 * DB writer hand-off (ARCH-52). The net thread never waits on synthesis.
 *
 * The engine is opened when the first render needs it and closed after
 * `idle_ms` with nothing queued, so a daemon whose users are not listening holds
 * none of the model's working memory.
 *
 * Renders are stored under a key made from the handle and the engine's version
 * (`<64 hex digits of handle>-<16 of version hash>`: the blob store takes hex,
 * '-' and '_' only, and the length alone tells a render from an attachment's 16
 * digits), so a new model version never overwrites an old render and a retry of
 * the same render writes the same key.
 */
#ifndef OC_TTS_WORKER_H
#define OC_TTS_WORKER_H

#include <stddef.h>
#include <stdint.h>

#include "blobstore.h"
#include "tts_render.h"

#define OC_TTS_KEY_MAX 96

typedef struct oc_tts_worker oc_tts_worker;

typedef struct {
    uint64_t      req_id;
    uint8_t       handle[32];
    oc_tts_status status;
    char          key[OC_TTS_KEY_MAX];      /* OC_TTS_OK: where the render is stored */
    uint64_t      bytes;
    uint32_t      duration_ms;
    char          reason[160];              /* OC_TTS_FAILED: why, for the log */
} oc_tts_result;

/* Start the worker over `engine` (borrowed, must outlive the worker) and `bs`
 * (borrowed). `queue_cap` bounds renders waiting (at least 1); `idle_ms` is how
 * long the engine stays open with nothing to do. NULL on failure. */
oc_tts_worker *oc_tts_worker_start(const oc_tts_engine *engine, oc_blobstore *bs, size_t queue_cap,
                                   unsigned idle_ms);

/* Stop: the render in progress finishes, queued ones are dropped without
 * results, the engine is closed. */
void oc_tts_worker_stop(oc_tts_worker *w);

/* Readable whenever at least one result is waiting; register it in epoll. The
 * caller reads (and discards) the counter, then drains with next_result. */
int oc_tts_worker_eventfd(oc_tts_worker *w);

/* Queue a render. `text` is copied. Returns 0, or -1 if the queue is full or the
 * voice is out of range -- answered as TTS_UNAVAILABLE by the caller. */
int oc_tts_worker_submit(oc_tts_worker *w, uint64_t req_id, const uint8_t handle[32], int voice,
                         const char *text);

/* The next finished render into `out`: 1 if there was one, 0 if none. */
int oc_tts_worker_next_result(oc_tts_worker *w, oc_tts_result *out);

/* The blob key a render of `handle` by `engine` is stored under. */
void oc_tts_render_key(const oc_tts_engine *engine, const uint8_t handle[32], char *key, size_t cap);

/* Whether the engine is open right now (for tests and the log). */
int oc_tts_worker_engine_open(oc_tts_worker *w);

#endif
