/*
 * The client's half of a browser sign-in (AUTH.md §8.1, §8.2): the per-attempt
 * verifier and its challenge, and the loopback listener the browser is sent
 * back to. Portable (POSIX and Winsock); no UI, no protocol frames — the network
 * thread drives it and the frontends only open a URL.
 */

#ifndef OC_SIGNIN_H
#define OC_SIGNIN_H

#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>

#include "url.h"   /* oc_query_get, for what the browser brings back */

#define OC_SIGNIN_VERIFIER_LEN  43   /* base64url of 32 random bytes */
#define OC_SIGNIN_CHALLENGE_LEN 43   /* base64url(SHA-256(verifier)) */

/* A fresh verifier and the challenge that goes in AUTH_BEGIN — RFC 7636's
 * construction. Both NUL-terminated. Returns 0, or -1 when the operating system
 * gives no random bytes: there is no weaker fallback, because a guessable
 * verifier is no proof of anything. */
int oc_signin_verifier(char verifier[OC_SIGNIN_VERIFIER_LEN + 1],
                       char challenge[OC_SIGNIN_CHALLENGE_LEN + 1]);

/* Wipe a secret in a way the compiler may not remove. */
void oc_signin_wipe(void *p, size_t n);

typedef struct oc_loopback oc_loopback;

/* Listen on 127.0.0.1, port chosen by the kernel, and write the redirect to give
 * the daemon — "http://127.0.0.1:<port>/cb/<secret>" — into `redirect_uri`. The
 * secret is per attempt: another local process that finds the port still cannot
 * hand this listener an answer. NULL on failure. */
oc_loopback *oc_loopback_open(char *redirect_uri, size_t cap);
/* A test's knob: open the listener on IPv6 loopback, as a host without IPv4
 * loopback does (the redirect is then http://[::1]:<port>/...). */
void oc_loopback_force_v6(int on);

typedef enum {
    OC_LOOPBACK_OK        = 0,
    OC_LOOPBACK_TIMEOUT   = -1,
    OC_LOOPBACK_CANCELLED = -2,
    OC_LOOPBACK_ERROR     = -3
} oc_loopback_result;

/* Serve until ONE GET arrives on the secret path, answer it with a page saying
 * the tab can be closed, and copy its query string (after the '?', undecoded)
 * into `query`. Requests for any other path or method are answered 404 and
 * ignored, so a port scanner or a second tab ends nothing. `cancel` (may be
 * NULL) is polled; set it non-zero from another thread to stop. */
oc_loopback_result oc_loopback_wait(oc_loopback *lb, int timeout_ms, const atomic_int *cancel,
                                    char *query, size_t qcap);

void oc_loopback_close(oc_loopback *lb);

/* The tunnel (AUTH.md §8.10): for a daemon the browser cannot be sent to
 * directly -- its certificate is one this client trusts by fingerprint, not one
 * a browser would -- the listener also carries the daemon's sign-in pages, under
 * "/p/<secret>/", to the daemon over TLS. The browser sees a loopback origin,
 * which it treats as secure, so there is no certificate warning. Only the
 * sign-in pages are carried, only for a request naming this listener as its
 * Host, and only to the certificate the client's own connection accepted. */
typedef struct {
    char          host[256];       /* where the daemon is dialled */
    int           port;
    char          name[256];       /* the name its certificate is checked for (SNI), or "" */
    char          authority[300];  /* its origin's host[:port]: the Host and Origin the pages see */
    unsigned char fp[32];          /* the certificate the client accepted */
} oc_tunnel_target;
void oc_loopback_set_tunnel(oc_loopback *lb, const oc_tunnel_target *t);
/* "http://127.0.0.1:<port>/p/<secret>": a page's path goes after it. */
int  oc_loopback_tunnel_base(const oc_loopback *lb, char *out, size_t cap);
/* Carry pages only -- no callback is awaited -- until `timeout_ms` passes or
 * `cancel` is set: a page opened on its own (the password page). */
oc_loopback_result oc_loopback_serve(oc_loopback *lb, int timeout_ms, const atomic_int *cancel);

#endif /* OC_SIGNIN_H */
