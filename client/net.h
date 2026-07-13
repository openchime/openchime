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

typedef struct oc_net oc_net;

/* Start the network thread. `token` carries local credentials as
 * "username:password" (the real login UI, OIDC, and session reconnect land in a
 * later client phase). Returns NULL on failure to spawn. */
oc_net *oc_net_start(const char *host, int port, const char *token,
                     oc_queue *to_ui, oc_queue *from_ui);

/* Signal the thread to stop, join it, and free. */
void oc_net_stop(oc_net *n);

#endif /* OC_NET_H */
