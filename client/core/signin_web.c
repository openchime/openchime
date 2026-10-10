/*
 * OpenChime client -- the browser sign-in's seam, for the web client
 * (signin.h; WEB.md). Natively the client opens a loopback listener and the
 * daemon's page sends the person back to it with a token. A page cannot
 * listen, and it does not need to: the daemon serves the client under /app/,
 * so the page's own origin at /app/ is the redirect. The browser leaves the
 * page for the daemon's sign-in page and comes back to /app/?token=..., a
 * new life of the page: what has to survive the trip -- the PKCE verifier and
 * the source signed in with -- is stashed in sessionStorage on the way out
 * and taken back on the way in (oc_signin_stash / oc_signin_unstash), and the
 * application starts the client with the result (oc_client_start_signin_result).
 *
 * The "listener" here therefore never hears anything: oc_loopback_wait
 * reports a timeout until the page is gone.
 */
#include "signin.h"

#include <emscripten.h>
#include <emscripten/threading.h>
#include <mbedtls/sha256.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char B64URL[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static size_t b64url(const uint8_t *in, size_t n, char *out) {
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        size_t rem = n - i;
        uint32_t v = (uint32_t)in[i] << 16;
        if (rem > 1) v |= (uint32_t)in[i + 1] << 8;
        if (rem > 2) v |= (uint32_t)in[i + 2];
        out[o++] = B64URL[(v >> 18) & 63];
        out[o++] = B64URL[(v >> 12) & 63];
        if (rem > 1) out[o++] = B64URL[(v >> 6) & 63];
        if (rem > 2) out[o++] = B64URL[v & 63];
    }
    out[o] = '\0';
    return o;
}

void oc_signin_wipe(void *p, size_t n) {
    volatile unsigned char *v = (volatile unsigned char *)p;
    while (n--) *v++ = 0;
}

/* The browser's entropy, as the native code reads /dev/urandom (which
 * Emscripten also answers from it). */
int oc_signin_verifier(char verifier[OC_SIGNIN_VERIFIER_LEN + 1],
                       char challenge[OC_SIGNIN_CHALLENGE_LEN + 1]) {
    uint8_t raw[32];
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f) return -1;
    size_t r = fread(raw, 1, sizeof raw, f);
    fclose(f);
    if (r != sizeof raw) return -1;
    b64url(raw, sizeof raw, verifier);
    uint8_t dig[32];
    if (mbedtls_sha256((const unsigned char *)verifier, OC_SIGNIN_VERIFIER_LEN, dig, 0) != 0) return -1;
    b64url(dig, sizeof dig, challenge);
    oc_signin_wipe(raw, sizeof raw);
    return 0;
}

/* sessionStorage belongs to the page's thread; the net thread calls from its
 * Worker, so the write is proxied. Kept for one page life (the trip out and
 * back) and taken once. */
void oc_signin_stash(const char *source, const char *verifier) {
    MAIN_THREAD_EM_ASM({
        try { sessionStorage.setItem("oc.signin.source", UTF8ToString($0));
              sessionStorage.setItem("oc.signin.verifier", UTF8ToString($1)); } catch (e) {}
    }, source ? source : "", verifier ? verifier : "");
}

int oc_signin_unstash(char *source, size_t scap, char *verifier, size_t vcap) {
    char *s = (char *)EM_ASM_PTR({
        try {
            const a = sessionStorage.getItem("oc.signin.source");
            const b = sessionStorage.getItem("oc.signin.verifier");
            if (!b) return 0;
            sessionStorage.removeItem("oc.signin.source");
            sessionStorage.removeItem("oc.signin.verifier");
            return stringToNewUTF8((a || "") + "\n" + b);
        } catch (e) { return 0; }
    });
    if (!s) return 0;
    char *nl = strchr(s, '\n');
    if (!nl) { free(s); return 0; }
    *nl = 0;
    snprintf(source, scap, "%s", s);
    snprintf(verifier, vcap, "%s", nl + 1);
    oc_signin_wipe(s, strlen(s)); oc_signin_wipe(nl + 1, strlen(nl + 1));
    free(s);
    return verifier[0] != 0;
}

/* ---- the "listener": the page's own origin ---------------------------------- */

struct oc_loopback { int unused; };

static void origin(char *out, size_t cap) {
    char *o = (char *)EM_ASM_PTR({ return stringToNewUTF8(self.location.origin || ""); });
    snprintf(out, cap, "%s", o ? o : "");
    free(o);
}

oc_loopback *oc_loopback_open(char *redirect_uri, size_t cap) {
    oc_loopback *lb = calloc(1, sizeof *lb);
    if (!lb) return NULL;
    if (redirect_uri) {
        char org[300]; origin(org, sizeof org);
        int n = snprintf(redirect_uri, cap, "%s/app/", org);
        if (n < 0 || (size_t)n >= cap || !org[0]) { free(lb); return NULL; }
    }
    return lb;
}

oc_loopback *oc_loopback_open_provider(char *redirect_uri, size_t cap) {
    return oc_loopback_open(redirect_uri, cap);
}

void oc_loopback_force_v6(int on) { (void)on; }

oc_loopback_result oc_loopback_wait(oc_loopback *lb, int timeout_ms, const atomic_int *cancel,
                                    char *query, size_t qcap) {
    (void)lb; (void)query; (void)qcap;
    /* Nothing comes back to this page life: the browser is leaving it. Wait
     * the interval out so the caller's minutes pass as they would natively. */
    int slept = 0;
    while (slept < timeout_ms) {
        if (cancel && atomic_load(cancel)) return OC_LOOPBACK_CANCELLED;
        emscripten_thread_sleep(100);
        slept += 100;
    }
    return OC_LOOPBACK_TIMEOUT;
}

oc_loopback_result oc_loopback_serve(oc_loopback *lb, int timeout_ms, const atomic_int *cancel) {
    return oc_loopback_wait(lb, timeout_ms, cancel, NULL, 0);
}

void oc_loopback_answer(oc_loopback *lb, oc_loopback_outcome outcome, const char *why) {
    (void)lb; (void)outcome; (void)why;
}

void oc_loopback_close(oc_loopback *lb) { free(lb); }

void oc_loopback_set_tunnel(oc_loopback *lb, const oc_tunnel_target *t) { (void)lb; (void)t; }

/* The daemon's pages are the page's own origin: there is no tunnel to run. */
int oc_loopback_tunnel_base(const oc_loopback *lb, char *out, size_t cap) {
    (void)lb;
    origin(out, cap);
    return out[0] ? 0 : -1;
}

void oc_loopback_tunnel_times(int send_ms, int answer_ms) { (void)send_ms; (void)answer_ms; }
