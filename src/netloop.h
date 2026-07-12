/*
 * OpenChime network event loop (ARCH-22).
 *
 * A single-threaded epoll loop multiplexes all client connections with
 * non-blocking I/O — not thread-per-connection. It owns the listening socket
 * and, per connection, the TLS session (src/tls.c) and the frame reassembler
 * (src/framebuf.c). This skeleton drives the handshake and answers HELLO with
 * WELCOME/REJECT (PROTOCOL.md §3); AUTH and messaging land in later milestones,
 * at which point accepted writes are handed to the DB-writer thread (ARCH-5).
 */

#ifndef OPENCHIME_NETLOOP_H
#define OPENCHIME_NETLOOP_H

#include <signal.h>

#include "tls.h"

/* Serve the binary protocol on `port` (TCP, all interfaces), terminating TLS
 * with `tls`. Blocks until *stop becomes non-zero, which is checked between
 * epoll cycles. Returns 0 on clean shutdown, -1 on a fatal setup error. */
int oc_netloop_run(int port, oc_tls_server *tls, volatile sig_atomic_t *stop);

#endif /* OPENCHIME_NETLOOP_H */
