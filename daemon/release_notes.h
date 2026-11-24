/*
 * Release notes for the clients (docs/details/CLIENT.md, "What's new";
 * PROTOCOL.md §5.16r). A client asks its own daemon for a build's notes, and
 * this worker asks central for them with the workspace's credential: a signed
 * GET (push.h's oc_machine_http, ARCH-85's scheme) to
 * <origin of OPENCHIME_ENROLL_URL>/v1/workspace/releases/build/<build id>,
 * on a thread of its own, so the net loop never waits on central.
 *
 * Answers are kept in memory: central's answer (200 with the notes, or 404 for
 * a build that is no release) for OC_RELEASE_NOTES_CACHE_MS, a failure (no
 * answer, any other status, or a body over OC_RELEASE_NOTES_MAX) for
 * OC_RELEASE_NOTES_FAIL_MS, and at most OC_RELEASE_NOTES_CACHE_MAX build ids,
 * the least recently used dropped first. One fetch per build id at a time:
 * everyone who asks while it runs is answered by it.
 */
#ifndef OPENCHIME_RELEASE_NOTES_H
#define OPENCHIME_RELEASE_NOTES_H

#include <stddef.h>
#include <stdint.h>

#define OC_RELEASE_NOTES_CACHE_MS  (6ull * 60 * 60 * 1000)   /* 6 hours */
#define OC_RELEASE_NOTES_FAIL_MS   (5ull * 60 * 1000)        /* 5 minutes */
#define OC_RELEASE_NOTES_CACHE_MAX 32

/* Unix time in milliseconds. */
typedef uint64_t (*oc_rn_clock)(void *ctx);

typedef struct oc_release_notes_worker oc_release_notes_worker;

/* An answer for one asker. `ok` 0 means unavailable (RELEASE_NOTES_UNAVAILABLE);
 * otherwise `status` is central's (200 or 404) and `body` its body (heap, may be
 * NULL when empty). */
typedef struct {
    uint64_t conn_id;
    char     build_id[65];
    int      ok;
    int      status;
    char    *body;
    size_t   body_len;
} oc_rn_answer;

/* Start the worker. `enroll_url` is OPENCHIME_ENROLL_URL (only its origin is
 * used); audience and privkey_pem are the enrollment identity; all are copied.
 * `clock` NULL is the system's. NULL on failure. */
oc_release_notes_worker *oc_release_notes_start(const char *enroll_url, const char *audience,
                                                const char *privkey_pem, oc_rn_clock clock, void *clock_ctx);

/* Readable when an answer is waiting. */
int oc_release_notes_eventfd(oc_release_notes_worker *w);

/* `conn_id` asks for `build_id` (a plain token; the frame's decoder checked it).
 * 1 with `now` filled when the cache answers at once; 0 when it is fetched and
 * answered later through oc_release_notes_next. Never blocks. */
int oc_release_notes_ask(oc_release_notes_worker *w, const char *build_id, uint64_t conn_id, oc_rn_answer *now);

/* The next answer fetched: 1 with `a`, or 0. */
int oc_release_notes_next(oc_release_notes_worker *w, oc_rn_answer *a);

/* Free an answer's body. */
void oc_rn_answer_free(oc_rn_answer *a);

/* Stop: a fetch under way finishes, waiting askers are dropped. A no-op for NULL. */
void oc_release_notes_stop(oc_release_notes_worker *w);

/* ---- exposed for testing ---- */

/* How many requests reached central. */
unsigned oc_release_notes_fetches(oc_release_notes_worker *w);

#endif /* OPENCHIME_RELEASE_NOTES_H */
