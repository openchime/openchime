/*
 * OpenChime network event loop (ARCH-22).
 *
 * A single-threaded epoll loop multiplexes all client connections with
 * non-blocking I/O — not thread-per-connection. It owns the listening socket
 * and, per connection, the TLS session (src/tls.c) and the frame reassembler
 * (src/framebuf.c). Accepted writes (AUTH, SEND) are handed to the DB-writer
 * thread (ARCH-5) as jobs; results come back through the writer's eventfd,
 * which this loop polls, and are delivered to the right connections here — so
 * all socket I/O stays on this thread and all DB writes stay on the writer.
 */

#ifndef OPENCHIME_NETLOOP_H
#define OPENCHIME_NETLOOP_H

#include <signal.h>

#include "dbwriter.h"
#include "tls.h"

/* Serve the binary protocol on `port` (TCP, all interfaces), terminating TLS
 * with `tls` and routing DB work through `dbw`. Blocks until *stop becomes
 * non-zero (checked between epoll cycles). Returns 0 on clean shutdown, -1 on a
 * fatal setup error. */
int oc_netloop_run(int port, oc_tls_server *tls, oc_dbwriter *dbw,
                   volatile sig_atomic_t *stop);

/* Wire the audio relay sidecar (REQ-150, ARCH-31): the IPC socket to it and the
 * UDP port it listens on. Call before oc_netloop_run. With ipc_fd < 0 (the
 * default) calls still form but carry no media endpoint. */
void oc_netloop_set_audio(int ipc_fd, uint16_t udp_port);

/* Wire the outbound push emitter (ARCH-85). When set, a committed SEND fans a
 * contentless notify decision to it for offline mobile delivery. Call before
 * oc_netloop_run; NULL (the default) disables push. */
struct oc_push;
void oc_netloop_set_push(struct oc_push *push);

/* Wire the link-unfurl worker (REQ-222, ARCH-105). When set, a committed SEND
 * or EDIT has its URLs extracted and queued for fetching, and a stored unfurl
 * fans an UNFURL frame to the channel. Call before oc_netloop_run; NULL (the
 * default, and the OPENCHIME_UNFURL=off state) disables unfurls. */
struct oc_unfurler;
void oc_netloop_set_unfurler(struct oc_unfurler *u);

#endif /* OPENCHIME_NETLOOP_H */
