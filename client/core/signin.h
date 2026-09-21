/*
 * The client's half of a browser sign-in (AUTH.md §8.1, §8.2): the per-attempt
 * verifier and its challenge, and the loopback listener the browser is sent
 * back to. Portable (POSIX and Winsock); no UI, no protocol frames — the network
 * thread drives it and the frontends only open a URL.
 */

#ifndef OC_SIGNIN_H
#define OC_SIGNIN_H

#include <stddef.h>
#include <stdint.h>

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
oc_loopback_result oc_loopback_wait(oc_loopback *lb, int timeout_ms, const volatile int *cancel,
                                    char *query, size_t qcap);

void oc_loopback_close(oc_loopback *lb);

/* The percent-decoded value of `key` in a query string. 1 found, 0 absent, -1
 * malformed or too long for `cap`. */
int oc_query_get(const char *query, const char *key, char *out, size_t cap);

#endif /* OC_SIGNIN_H */
