/* A test's sign-in through the daemon's own pages (signin.h). */
#include "check.h"        /* oc_test_deadline, oc_test_pause: waits on the monotonic clock */
#include "testsignin.h"

#include "protocol.h"   /* OC_ALPN_HTTP11 */
#include "tls.h"
#include "url.h"        /* oc_query_get */

#include <mbedtls/sha256.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static const char *HTTP_ALPN[] = { OC_ALPN_HTTP11, NULL };

/* --- PKCE ------------------------------------------------------------------- */

static void b64url(const uint8_t *in, size_t n, char *out) {
    static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < n) v |= in[i + 2];
        out[o++] = A[(v >> 18) & 63];
        out[o++] = A[(v >> 12) & 63];
        if (i + 1 < n) out[o++] = A[(v >> 6) & 63];
        if (i + 2 < n) out[o++] = A[v & 63];
    }
    out[o] = '\0';
}

int oc_test_pkce(char verifier[OC_TEST_VERIFIER_CAP], char challenge[OC_TEST_VERIFIER_CAP]) {
    uint8_t raw[32], hash[32];
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f) return -1;
    size_t got = fread(raw, 1, sizeof raw, f);
    fclose(f);
    if (got != sizeof raw) return -1;
    b64url(raw, sizeof raw, verifier);
    /* The challenge is over the verifier AS SENT: its base64url text. */
    if (mbedtls_sha256((const unsigned char *)verifier, strlen(verifier), hash, 0) != 0) return -1;
    b64url(hash, sizeof hash, challenge);
    return 0;
}

/* --- one HTTPS request --------------------------------------------------------- */

static int dial(const char *host, int port) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host && *host ? host : "127.0.0.1", &addr.sin_addr) != 1) return -1;
    /* Until the daemon listens, or four seconds on the monotonic clock. */
    uint64_t end = oc_test_deadline(4000);
    do {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        if (connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0) {
            struct timeval tv = { 20, 0 };
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            return fd;
        }
        close(fd);
    } while (oc_test_pause(end));
    return -1;
}

int oc_test_web_call(const char *host, int port, const uint8_t *pin,
                     const char *method, const char *path, const char *origin,
                     const char *ctype, const char *body, char *resp, size_t cap) {
    resp[0] = '\0';
    int fd = dial(host, port);
    if (fd < 0) return -1;
    oc_tls_client cli; oc_tls_conn conn;
    int rc = -1;
    if (oc_tls_client_init_ex(&cli, pin, HTTP_ALPN) != 0) { close(fd); return -1; }
    if (oc_tls_conn_init(&conn, &cli.conf, fd) != 0) { oc_tls_client_free(&cli); close(fd); return -1; }
    oc_tls_status st;
    do st = oc_tls_handshake(&conn); while (st == OC_TLS_WANT_READ || st == OC_TLS_WANT_WRITE);
    if (st == OC_TLS_OK) {
        char req[8192], ob[300] = "", cb[160] = "";
        if (origin) snprintf(ob, sizeof ob, "Origin: %s\r\n", origin);
        if (ctype) snprintf(cb, sizeof cb, "Content-Type: %s\r\n", ctype);
        size_t bl = body ? strlen(body) : 0;
        int n = snprintf(req, sizeof req, "%s %s HTTP/1.1\r\nHost: web.test\r\n%s%sContent-Length: %zu\r\n"
                         "Connection: close\r\n\r\n%s", method, path, ob, cb, bl, body ? body : "");
        size_t sent = 0;
        int ok = n > 0 && (size_t)n < sizeof req;
        while (ok && sent < (size_t)n) {
            size_t w = 0;
            if (oc_tls_write(&conn, req + sent, (size_t)n - sent, &w) != OC_TLS_OK) ok = 0;
            sent += w;
        }
        size_t total = 0;
        struct timeval t0, t; gettimeofday(&t0, NULL);
        while (ok && total < cap - 1) {
            size_t r = 0;
            st = oc_tls_read(&conn, (uint8_t *)resp + total, cap - 1 - total, &r);
            gettimeofday(&t, NULL);
            if ((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_usec - t0.tv_usec) / 1000 > 20000) break;
            if (st == OC_TLS_WANT_READ || st == OC_TLS_WANT_WRITE) continue;
            if (r == 0) break;
            total += r;
            if (st != OC_TLS_OK) break;
        }
        resp[total] = '\0';
        if (ok && strncmp(resp, "HTTP/1.1 ", 9) == 0) rc = atoi(resp + 9);
    }
    oc_tls_conn_free(&conn);
    oc_tls_client_free(&cli);
    close(fd);
    return rc;
}

/* --- the forms ------------------------------------------------------------------ */

#define FORM   "application/x-www-form-urlencoded"
#define ORIGIN "https://web.test"
/* Nothing listens here: the token is read from the 303's Location. */
#define REDIRECT "http%3A%2F%2F127.0.0.1%3A5%2Fcb"

static void pct(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (; *in && o + 4 < cap; in++) {
        unsigned char ch = (unsigned char)*in;
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_' || ch == '.' || ch == '~') out[o++] = (char)ch;
        else o += (size_t)snprintf(out + o, cap - o, "%%%02X", ch);
    }
    out[o] = '\0';
}

/* The page's refusal, for a diagnosis: its kind (data-oc-error), or the
 * status line where it gave none. */
static void page_said(const char *resp, char *why, size_t wcap) {
    char code[64];
    if (oc_test_page_error_code(resp, code, sizeof code)[0]) { snprintf(why, wcap, "%s", code); return; }
    snprintf(why, wcap, "%.*s", (int)strcspn(resp, "\r\n"), resp);
}

/* A sign-in or sign-up: the form, the 303, the token. */
static int page_token(const char *host, int port, const uint8_t *pin, const char *path, const char *body,
                      char *token, size_t tcap, char *why, size_t wcap) {
    static _Thread_local char resp[65536];
    int st = oc_test_web_call(host, port, pin, "POST", path, ORIGIN, FORM, body, resp, sizeof resp);
    if (st != 303) {
        char said[256] = "";
        if (st == 200) page_said(resp, said, sizeof said);
        snprintf(why, wcap, "%s: HTTP %d%s%s", path, st, said[0] ? ": " : "", said);
        return -1;
    }
    const char *l = strstr(resp, "\r\nLocation: ");
    const char *q = l ? strchr(l + 12, '?') : NULL;
    char query[OC_TEST_TOKEN_CAP + 64];
    if (!q) { snprintf(why, wcap, "%s: 303 with no token", path); return -1; }
    snprintf(query, sizeof query, "%.*s", (int)strcspn(q + 1, "\r\n"), q + 1);
    if (oc_query_get(query, "token", token, tcap) != 1) { snprintf(why, wcap, "%s: 303 with no token", path); return -1; }
    return 0;
}

int oc_test_signin(const char *host, int port, const uint8_t *pin,
                   const char *user, const char *pass,
                   char *token, size_t tcap, char *verifier, size_t vcap,
                   char *why, size_t wcap) {
    char ver[OC_TEST_VERIFIER_CAP], ch[OC_TEST_VERIFIER_CAP], u[256], p[1024], body[1600];
    if (vcap < sizeof ver || oc_test_pkce(ver, ch) != 0) { snprintf(why, wcap, "no verifier"); return -1; }
    pct(user, u, sizeof u); pct(pass, p, sizeof p);
    snprintf(body, sizeof body, "redirect_uri=%s&nonce=%s&username=%s&password=%s", REDIRECT, ch, u, p);
    if (page_token(host, port, pin, "/signin", body, token, tcap, why, wcap) != 0) return -1;
    memcpy(verifier, ver, sizeof ver);
    return 0;
}

int oc_test_signup(const char *host, int port, const uint8_t *pin,
                   const char *invite, const char *user, const char *pass,
                   char *token, size_t tcap, char *verifier, size_t vcap,
                   char *why, size_t wcap) {
    char ver[OC_TEST_VERIFIER_CAP], ch[OC_TEST_VERIFIER_CAP], u[256], p[1024], body[3000];
    if (vcap < sizeof ver || oc_test_pkce(ver, ch) != 0) { snprintf(why, wcap, "no verifier"); return -1; }
    pct(user, u, sizeof u); pct(pass, p, sizeof p);
    snprintf(body, sizeof body, "redirect_uri=%s&nonce=%s&invite=%s&username=%s&password=%s&confirm=%s",
             REDIRECT, ch, invite, u, p, p);
    if (page_token(host, port, pin, "/signup", body, token, tcap, why, wcap) != 0) return -1;
    memcpy(verifier, ver, sizeof ver);
    return 0;
}

int oc_test_change_password(const char *host, int port, const uint8_t *pin,
                            const char *user, const char *current, const char *next,
                            char *why, size_t wcap) {
    static _Thread_local char resp[65536];
    char u[256], c[1024], n[1024], body[3400];
    pct(user, u, sizeof u); pct(current, c, sizeof c); pct(next, n, sizeof n);
    snprintf(body, sizeof body, "username=%s&current=%s&password=%s&confirm=%s", u, c, n, n);
    int st = oc_test_web_call(host, port, pin, "POST", "/account/password", ORIGIN, FORM, body, resp, sizeof resp);
    if (st == 200 && oc_test_page_result(resp, "password-changed")) return 0;
    char said[256] = "";
    if (st == 200) page_said(resp, said, sizeof said);
    snprintf(why, wcap, "/account/password: HTTP %d%s%s", st, said[0] ? ": " : "", said);
    return -1;
}
