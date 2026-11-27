/* Invitations queued by central (ARCH-85). Central keeps a queue of invitations
 * for this workspace -- signup's first invitations, say -- and never calls the
 * daemon: with an active enrollment, a worker thread asks central for what is
 * pending (OC_CENTRAL_INVITATIONS_TAKE), has each one made as an invitation made
 * from the Members page is (bound to its address, so its mail goes through the
 * invitation report), and reports what came of each (made, or refused and why) to
 * OC_CENTRAL_INVITATIONS_REPORT, which stops central offering it. Both requests
 * are signed with the enrollment key, as push is.
 *
 * The pace is central's: it asks again when central's Retry-After says, after an
 * hour when it says nothing, and on a failure backs off from a minute, doubling,
 * to a day. A report that fails is tried again with the same ids; an invitation
 * taken and not reported is offered again by central once its lease lapses. */
#ifndef OPENCHIME_CENTRAL_INVITES_H
#define OPENCHIME_CENTRAL_INVITES_H

#include <stddef.h>
#include <stdint.h>

#define OC_CENTRAL_INVITES_MAX     20     /* the most central gives at once */
#define OC_CENTRAL_INVITES_ID_LEN  32     /* an id: 32 lower-case hex digits */
#define OC_CENTRAL_INVITES_ADDR    256    /* an address is at most 254 bytes */

typedef struct oc_central_invites oc_central_invites;

/* Make one invitation, bound to `address`: the outcome comes back later through
 * oc_central_invites_outcome under the same `id`. Called on the worker thread. */
typedef void (*oc_central_invites_make_fn)(void *ctx, const char *id, const char *address);

/* The worker's waits, in milliseconds. */
typedef struct {
    uint64_t idle_ms;       /* central named no Retry-After */
    uint64_t fail_min_ms;   /* the first wait after a failure, doubling... */
    uint64_t fail_max_ms;   /* ...to this */
} oc_central_invites_timing;

/* Production's: an hour; a minute doubling to a day. */
extern const oc_central_invites_timing OC_CENTRAL_INVITES_TIMING;

/* Start the worker. `enroll_url` is OPENCHIME_ENROLL_URL (its origin is used);
 * `audience` and `privkey_pem` are the enrollment identity (copied). NULL on failure. */
oc_central_invites *oc_central_invites_start(const char *enroll_url, const char *audience,
                                             const char *privkey_pem,
                                             oc_central_invites_make_fn make, void *ctx);
/* The same, with other waits, for a test that cannot wait an hour. */
oc_central_invites *oc_central_invites_start_timed(const char *enroll_url, const char *audience,
                                                   const char *privkey_pem,
                                                   oc_central_invites_make_fn make, void *ctx,
                                                   const oc_central_invites_timing *timing);

/* What came of an invitation handed to `make`: made, or refused with `reason`.
 * Queued for the report; never blocks the caller. A no-op for NULL. */
void oc_central_invites_outcome(oc_central_invites *ci, const char *id, int made, const char *reason);

/* Stop: a request under way finishes; outcomes not yet reported are dropped, and
 * central offers those invitations again once their lease lapses. NULL is a no-op. */
void oc_central_invites_stop(oc_central_invites *ci);

/* --- the parts, for the tests ------------------------------------------------ */

/* The invitations in a take's answer, {"invitations":[{"id","address"}]}: up to
 * `max` into ids/addresses. Returns how many, or -1 when the body is not that
 * shape. An entry whose id is not 32 lower-case hex digits, or whose address is
 * empty or too long, is skipped. */
int oc_central_invites_parse(const char *body, size_t len,
                             char ids[][OC_CENTRAL_INVITES_ID_LEN + 1],
                             char addresses[][OC_CENTRAL_INVITES_ADDR], int max);

/* A report's body, {"outcomes":[{"id","made","reason"}]}, `reason` null where
 * made. Returns 0, or -1 when it does not fit `cap` (never a truncated body). */
int oc_central_invites_report_body(const char *const *ids, const int *made,
                                   const char *const *reasons, int n, char *out, size_t cap);

/* How long to wait before the next take: after an answer (`ok`), central's
 * Retry-After in seconds when it named one (> 0), else timing->idle_ms; after
 * the `failures`-th failure in a row (1 for the first), fail_min_ms doubled
 * (failures - 1) times, at most fail_max_ms. */
uint64_t oc_central_invites_wait_ms(int ok, long retry_after_s, int failures,
                                    const oc_central_invites_timing *timing);

#endif
