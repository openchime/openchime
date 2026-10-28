/*
 * The daemon's listening sockets: the protocol port, the health port and the
 * audio relay's UDP port. Each is bound once for both address families, so a
 * client that reaches the host over IPv6 is served as one over IPv4 is.
 */

#ifndef OPENCHIME_LISTEN_H
#define OPENCHIME_LISTEN_H

#include <stddef.h>

/* A socket of `type` (SOCK_STREAM or SOCK_DGRAM) bound to every address on
 * `port` (0: the kernel picks). IPv6 with IPv4 mapped in (IPV6_V6ONLY off),
 * falling back to IPv4 alone on a host with no IPv6. SO_REUSEADDR is set on a
 * stream socket. Returns the descriptor, or -1 with errno set and `op` naming
 * the call that failed ("socket" or "bind"). */
int oc_listen_bind(int type, int port, const char **op);

/* An accepted or received peer address as text, an IPv4 address mapped into
 * IPv6 written as the IPv4 address it is ("192.0.2.7", not
 * "::ffff:192.0.2.7"), so a client counts and reads the same whichever socket
 * family took it. `out` holds at least 46 bytes; "" for anything else. */
void oc_listen_peer_text(const void *sockaddr_storage, char *out, size_t cap);

#endif
