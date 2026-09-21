/*
 * PROXY protocol v2, receiving side: how a daemon behind a TCP forwarder learns
 * which client a connection belongs to. The per-address connection cap and the
 * per-source sign-in limiter (REQ-191) both key on that address, and behind a
 * forwarder every client would otherwise be the forwarder.
 *
 * The header is believed only from the peers OPENCHIME_TRUSTED_PROXIES names:
 * anybody else who sent one could claim any address and walk past both limits.
 * Pure — no sockets, no clock.
 */

#ifndef OPENCHIME_PROXYPROTO_H
#define OPENCHIME_PROXYPROTO_H

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#define OC_PROXY_V2_MAX 552   /* 16-byte preamble + the 536 bytes of TLVs we will skip */

typedef struct oc_trusted_proxies oc_trusted_proxies;

/* Parse a comma-separated list of addresses and CIDR blocks, IPv4 or IPv6
 * ("10.0.0.0/8, fdaa::/16, 192.0.2.7"). NULL or "" is the empty list: nobody is
 * trusted and no header is ever read. Returns NULL with a reason in `err` on an
 * entry it does not understand — a typo here must stop the boot, because the two
 * ways it can be wrong are "every client is one address" and "every connection
 * is refused". */
oc_trusted_proxies *oc_trusted_proxies_parse(const char *spec, char *err, size_t errcap);
void oc_trusted_proxies_free(oc_trusted_proxies *t);

/* 1 if the peer is one of them. An IPv4 peer seen through a dual-stack socket
 * (::ffff:a.b.c.d) matches an IPv4 entry. */
int oc_trusted_proxies_match(const oc_trusted_proxies *t, const struct sockaddr_storage *peer);

/* Read a v2 header from the first `len` bytes of a connection. Returns its length
 * (the bytes to consume before TLS begins), 0 when more bytes are needed, or -1
 * when this is not a v2 header we accept. On success `src` is the client's
 * address as text, or "" for a LOCAL header (the forwarder's own health check:
 * keep the peer's address). TCP over IPv4 or IPv6 only. */
long oc_proxy_v2_parse(const uint8_t *buf, size_t len, char src[46]);

#endif /* OPENCHIME_PROXYPROTO_H */
