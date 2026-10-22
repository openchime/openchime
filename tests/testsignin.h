/* How a test signs in: the way a person does (AUTH.md §8.10). The daemon takes
 * a password only on the pages it serves on its own TLS port; a sign-in there
 * ends in a short-lived token the client presents on
 * AUTH{oidc, "local", token, verifier}. These play the browser -- the form
 * POST over TLS, the 303 that carries the token -- and hand back what AUTH
 * needs. Built on shared/ alone, so the shared-only tools (e2e_client,
 * demo_client, bench_load) link them as the suite does. (Named apart from
 * client/core/signin.h, which the suites also include.)
 *
 * `pin` is the daemon's certificate fingerprint (OC_TLS_FINGERPRINT_LEN bytes),
 * or NULL to connect without checking it: a test's own daemon on loopback. */
#ifndef OC_TEST_SIGNIN_H
#define OC_TEST_SIGNIN_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define OC_TEST_TOKEN_CAP    4096
#define OC_TEST_VERIFIER_CAP 48

/* POST /signin for `user`. 0 with `token` and `verifier` set; -1 with `why`
 * (the HTTP status and, where the page said, its message). */
int oc_test_signin(const char *host, int port, const uint8_t *pin,
                   const char *user, const char *pass,
                   char *token, size_t tcap, char *verifier, size_t vcap,
                   char *why, size_t wcap);

/* POST /signup with an invitation or the first-run setup token (`invite`, 64
 * hex characters): the account is made and signed in. As oc_test_signin. */
int oc_test_signup(const char *host, int port, const uint8_t *pin,
                   const char *invite, const char *user, const char *pass,
                   char *token, size_t tcap, char *verifier, size_t vcap,
                   char *why, size_t wcap);

/* POST /account/password: `user`'s password from `current` to `next`. 0 when
 * the page reports data-oc-result="password-changed" (every session of the
 * account is then revoked, the page's rule); -1 with `why`. */
int oc_test_change_password(const char *host, int port, const uint8_t *pin,
                            const char *user, const char *current, const char *next,
                            char *why, size_t wcap);

/* One request to the pages over TLS, as a browser sends it: the status (or -1),
 * with the whole response in `resp`. `origin` NULL sends no Origin header; a
 * form POST needs `https://<host header>`. For the suites' own page tests. */
int oc_test_web_call(const char *host, int port, const uint8_t *pin,
                     const char *method, const char *path, const char *origin,
                     const char *ctype, const char *body, char *resp, size_t cap);

/* A fresh PKCE pair: the verifier (43 base64url characters) and the challenge
 * over it, which goes to the page as `nonce`. 0 or -1. */
int oc_test_pkce(char verifier[OC_TEST_VERIFIER_CAP], char challenge[OC_TEST_VERIFIER_CAP]);

/* ---- reading a page by its markers (daemon/webpages.h) -----------------------
 *
 * A page names its screen in data-oc-page, an outcome in data-oc-result and a
 * refusal's kind in data-oc-error. A test reads those, never the page's words,
 * which are for a person and may be reworded without anything breaking. */

/* Whether `resp` carries `attr`="`value`" (attr e.g. "data-oc-page"). */
static inline int oc_test_page_attr(const char *resp, const char *attr, const char *value) {
    char want[160];
    int n = snprintf(want, sizeof want, "%s=\"%s\"", attr, value);
    return resp && n > 0 && (size_t)n < sizeof want && strstr(resp, want) != NULL;
}
/* The page is this screen ("signin", "security-on", ...). */
static inline int oc_test_page_is(const char *resp, const char *screen) {
    return oc_test_page_attr(resp, "data-oc-page", screen);
}
/* The page reports this outcome ("password-changed", "approved", ...). */
static inline int oc_test_page_result(const char *resp, const char *result) {
    return oc_test_page_attr(resp, "data-oc-result", result);
}
/* The page refused for this reason ("pw-mismatch", "code-wrong", ...). */
static inline int oc_test_page_error(const char *resp, const char *code) {
    return oc_test_page_attr(resp, "data-oc-error", code);
}
/* The page refused for any reason. */
static inline int oc_test_page_any_error(const char *resp) {
    return resp && strstr(resp, "data-oc-error=\"") != NULL;
}
/* The refusal's code into `out` ("" when the page refused nothing). */
static inline const char *oc_test_page_error_code(const char *resp, char *out, size_t cap) {
    const char *m = resp ? strstr(resp, "data-oc-error=\"") : NULL;
    if (!m) { if (cap) out[0] = '\0'; return out; }
    m += strlen("data-oc-error=\"");
    snprintf(out, cap, "%.*s", (int)strcspn(m, "\""), m);
    return out;
}

#endif /* OC_TEST_SIGNIN_H */
