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

#include "callsig.h"
#include "queue.h"
#include "secret.h"

typedef struct oc_net oc_net;

/* One way a workspace signs people in, as its AUTH_CHALLENGE lists it. `kind` is
 * OC_SOURCE_LOCAL (a password form) or a browser source (a button). */
typedef struct { char id[64]; char label[96]; uint8_t kind; } oc_signin_source;
#define OC_PROBE_UNREACHABLE (-1)
#define OC_PROBE_VERSION     (-2)   /* the server speaks another protocol version */

/* Connect, read how the workspace signs people in, and leave — so a frontend can
 * draw the right controls before asking for anything. Blocking (a few seconds at
 * worst); returns the number of sources written to `out`, or OC_PROBE_*. */
int oc_net_probe(const char *host, int port, oc_signin_source *out, int max);

/* Start the network thread. `token` carries local credentials as
 * "username:password"; NULL or "" means a browser sign-in through the first such
 * source the workspace offers — OC_EV_AUTH_BROWSER then carries the URL for the
 * frontend to open, and the thread waits for the browser to come back. `store_path` (or NULL for in-memory only) is a local
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

/* Stop waiting for the browser (the person pressed cancel). */
void oc_net_cancel_signin(oc_net *n);

/* Set the synced-settings bucket id for this client (default "tui"). Call once
 * right after start, before soliciting settings; identifies the per-frontend
 * bucket so a TUI's synced prefs stay separate from a future GUI's. */
void oc_net_set_client_type(oc_net *n, const char *client_type);

/* The media engine calls go to (callsig.h); NULL removes it. Returns once no call
 * into the old one is running, so it may be freed after. */
void oc_net_set_call_media(oc_net *n, const oc_call_media *media, void *ctx);

/* Signal the thread to stop, join it, and free. */
void oc_net_stop(oc_net *n);

/* Sends queued in the in-memory outbox but not yet acked (REQ-102/ARCH-88).
 * Safe from the UI thread: the net thread only ever publishes a count here. */
int oc_net_outbox_pending(oc_net *n);

#endif /* OC_NET_H */
