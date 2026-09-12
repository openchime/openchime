/* Outbound mobile-push emitter (ARCH-85, REQ-132/133). The daemon owns the device
 * registry and the notify decision; this worker turns a freshly-sent message into
 * a contentless notification batch and POSTs it to the control-plane push gateway
 * (CP-13) over CA-verified HTTPS, signed with the enrollment key (CP-12). Central
 * never dials the daemon (ARCH-56); this is the one daemon->central outbound
 * runtime channel. Recipient selection runs on the worker's own read-only SQLite
 * connection, off the net/writer hot path (ARCH-66). See docs/AUTH.md / PUSH.
 */
#ifndef OPENCHIME_PUSH_H
#define OPENCHIME_PUSH_H

#include <stddef.h>
#include <stdint.h>

#include <sqlite3.h>

#include "dbwriter.h"
#include "protocol.h"   /* OC_DEVICE_TOKEN_MAX, OC_PUSH_* */

typedef struct oc_push oc_push;

/* Start the emitter. Opens its own read-only connection to db_path; `dbw` is used
 * to prune stale tokens central reports. Returns NULL on failure or if push_url is
 * empty. audience + privkey_pem are the enrollment identity (copied). */
oc_push *oc_push_start(const char *db_path, oc_dbwriter *dbw,
                       const char *push_url, const char *ca_bundle,
                       const char *audience, const char *privkey_pem);

/* Enqueue a notify decision for a just-committed message. Fire-and-forget; never
 * blocks the caller (the net loop). A no-op if p is NULL. */
void oc_push_notify(oc_push *p, uint64_t channel_id, uint64_t author_id,
                    uint64_t message_id, uint64_t root_id);

void oc_push_stop(oc_push *p);

/* ---- exposed for testing ---- */

/* True when now_min (minutes-of-day UTC, 0..1439) is inside the DND window
 * [start,end); handles the wrap-around case start > end. enabled==0 -> 0. */
int oc_push_dnd_active(int enabled, int start_min, int end_min, int now_min);
/* The schedule predicate lives in shared/notify.h: the client has to agree with
 * it, and a copy on each side is a disagreement waiting to happen (ARCH-103). */

typedef struct { uint8_t platform; char token[OC_DEVICE_TOKEN_MAX]; } oc_push_target;

/* Collect push recipients for a message in channel_id from author_id: members
 * minus the author, notification level ALL (absent==ALL; MENTIONS/NONE excluded),
 * not in DND at now_min, with a device token. Fills up to `max`; returns count. */
/* `message_id` is what makes the MENTIONS level answerable; pass 0 to mean "no
 * particular message", which then selects only level-ALL recipients. */
/* `now_min` is the minute of the day the recurring window is compared against;
 * `now_ms` is the wall instant a pause is compared against (REQ-278). Both are
 * passed in rather than read here, so a test can state the moment it means. */
/* `root_id` is the thread root for a reply and 0 for a channel send; a reply
 * additionally notifies the thread's participants at level MENTIONS (REQ-061,
 * ARCH-104). */
int oc_push_collect(sqlite3 *db, uint64_t channel_id, uint64_t author_id,
                    uint64_t message_id, uint64_t root_id, int now_min,
                    uint64_t now_ms, oc_push_target *out, int max);

/* Sign the CP-12 canonical string for `body` with the enrollment key ->
 * base64(DER ECDSA) into sig_b64. Returns 0 on success. */
int oc_push_sign(const char *privkey_pem, const char *audience, const char *body,
                 long ts, char *sig_b64, size_t sig_cap);

/* Build the contentless notify JSON body from targets. Returns 0 on success. */
int oc_push_build_body(uint64_t channel_id, const oc_push_target *targets, int n,
                       char *out, size_t cap);

#endif /* OPENCHIME_PUSH_H */
