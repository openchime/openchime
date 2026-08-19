/*
 * OpenChime client — network thread (ARCH-62).
 *
 * Owns the TLS socket for one connection: connect → TLS handshake (TOFU) →
 * HELLO/WELCOME → (stub) AUTH → read loop dispatching server frames into `to_ui`
 * as oc_ev, while draining user oc_cmd from `from_ui` to send. Lifts the
 * connect/read/write logic from tests/e2e_client.c; blocking-style over a
 * non-blocking socket polled with a short timeout so it can interleave sends.
 */

#ifndef OC_NET_H
#define OC_NET_H

#include "queue.h"
#include "secret.h"

typedef struct oc_net oc_net;

/* Start the network thread. `token` carries local credentials as
 * "username:password". `store_path` (or NULL for in-memory only) is a local
 * SQLite store persisting the session token + TOFU pin, so a relaunch reconnects
 * silently. `secret` (borrowed; NULL = none) routes the session token into an OS
 * keyring instead of the SQLite file. Returns NULL on failure to spawn. */
oc_net *oc_net_start(const char *host, int port, const char *token,
                     const char *store_path, oc_secret *secret,
                     oc_queue *to_ui, oc_queue *from_ui);

/* Cut short the reconnect backoff so the next attempt happens immediately (no-op
 * if not currently backing off). */
/* Redeem an invite on the FIRST connect instead of authenticating (REQ-268): REDEEM_INVITE creates the account and authenticates in one step, so
 * the reply is an ordinary AUTH_OK and everything after is unchanged. Call right
 * after oc_net_start; cleared once used, so a later reconnect re-auths normally
 * with the session token rather than replaying a spent invite. */
void oc_net_set_invite(oc_net *n, const char *token);

void oc_net_reconnect(oc_net *n);

/* Set the synced-settings bucket id for this client (default "tui"). Call once
 * right after start, before soliciting settings; identifies the per-frontend
 * bucket so a TUI's synced prefs stay separate from a future GUI's. */
void oc_net_set_client_type(oc_net *n, const char *client_type);

/* Signal the thread to stop, join it, and free. */
void oc_net_stop(oc_net *n);

/* Sends queued in the in-memory outbox but not yet acked (REQ-102/ARCH-88).
 * Safe from the UI thread: the net thread only ever publishes a count here. */
int oc_net_outbox_pending(oc_net *n);

#endif /* OC_NET_H */
