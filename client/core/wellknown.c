/*
 * OpenChime client — `.well-known` discovery metadata. See wellknown.h.
 */

#include "wellknown.h"

#include "sock.h"      /* the POSIX/Winsock shim, and getaddrinfo with it */
#include "tls.h"
#include "model.h"    /* oc_model_now_ms: the core's one monotonic clock */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A parser for THIS document, rather than a JSON library.
 *
 * jsmn is vendored, but its implementation lives in one TU by convention
 * (daemon/jwt.c) and the client does not link the daemon -- while the test
 * binary links both, so a second copy in here does not link either. The
 * document is a flat object of scalars, so the twenty lines below cost less
 * than the arrangement needed to share the library, and they are strict in the
 * way REQ-011 needs: anything that is not exactly this shape is malformed
 * rather than partially understood.
 */

/* The whole exchange, connect to last byte. A sign-in is waiting on this, and a
 * domain that answers slowly must not be the reason a client looks hung; the
 * document is optional, so giving up on it is a legitimate outcome. */
#define WK_DEADLINE_MS 4000

/* --- parsing ------------------------------------------------------------- */

static const char *wk_ws(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    return p;
}

/* A JSON string starting at `p` (which must be the quote). Returns the byte
 * after the closing quote, or NULL. The contents go to `out` when it fits; a
 * value too long for its field is a REFUSAL by the caller, not a truncation.
 * A backslash escape is refused outright: nothing this document carries needs
 * one, and half-understanding an escape is worse than declining the whole. */
static const char *wk_string(const char *p, const char *end,
                             const char **val, size_t *val_len) {
    if (p >= end || *p != '"') return NULL;
    p++;
    const char *s = p;
    while (p < end && *p != '"') {
        if (*p == '\\' || (unsigned char)*p < 0x20) return NULL;
        p++;
    }
    if (p >= end) return NULL;
    *val = s; *val_len = (size_t)(p - s);
    return p + 1;
}

/* Skip one value of any shape -- including a nested object or array, whose own
 * keys must never be read as the document's. Returns the byte after it. */
static const char *wk_skip_value(const char *p, const char *end) {
    p = wk_ws(p, end);
    if (p >= end) return NULL;
    if (*p == '"') { const char *v; size_t n; return wk_string(p, end, &v, &n); }
    if (*p == '{' || *p == '[') {
        int depth = 0;
        while (p < end) {
            if (*p == '"') {
                const char *v; size_t n;
                const char *q = wk_string(p, end, &v, &n);
                if (!q) return NULL;
                p = q; continue;
            }
            if (*p == '{' || *p == '[') depth++;
            else if (*p == '}' || *p == ']') { depth--; if (depth == 0) return p + 1; }
            p++;
        }
        return NULL;
    }
    /* A bare primitive: number, true, false or null. */
    const char *s = p;
    while (p < end && *p != ',' && *p != '}' && *p != ' ' && *p != '\t' &&
           *p != '\r' && *p != '\n') p++;
    return p > s ? p : NULL;
}

int oc_wellknown_parse(const char *doc, size_t len, oc_wellknown *out) {
    if (!doc || !out) return OC_WK_MALFORMED;
    memset(out, 0, sizeof *out);
    if (len == 0 || len > OC_WK_MAX) return OC_WK_MALFORMED;

    const char *p = doc, *end = doc + len;
    p = wk_ws(p, end);
    if (p >= end || *p++ != '{') return OC_WK_MALFORMED;
    p = wk_ws(p, end);
    if (p < end && *p == '}') return OC_WK_OK;            /* {} says nothing */

    for (;;) {
        p = wk_ws(p, end);
        const char *key; size_t key_len;
        p = wk_string(p, end, &key, &key_len);
        if (!p) return OC_WK_MALFORMED;
        p = wk_ws(p, end);
        if (p >= end || *p++ != ':') return OC_WK_MALFORMED;
        p = wk_ws(p, end);
        if (p >= end) return OC_WK_MALFORMED;

        int is_port = (key_len == 4 && memcmp(key, "port", 4) == 0);
        int is_fp   = (key_len == 11 && memcmp(key, "fingerprint", 11) == 0);

        if (is_port) {
            const char *s = p;
            while (p < end && *p >= '0' && *p <= '9') p++;
            /* Digits and nothing else: "8443" as a string, -1, or 84.43 are each
             * a port this client cannot honour, and honouring the part it
             * understands would connect somewhere nobody asked for. */
            if (p == s || (p < end && *p != ',' && *p != '}' && *p != ' ' &&
                           *p != '\t' && *p != '\r' && *p != '\n'))
                return OC_WK_MALFORMED;
            if ((size_t)(p - s) > 5) return OC_WK_MALFORMED;
            char num[8];
            memcpy(num, s, (size_t)(p - s)); num[p - s] = '\0';
            long port = strtol(num, NULL, 10);
            if (port <= 0 || port > 65535) return OC_WK_MALFORMED;
            out->port = (int)port;
        } else if (is_fp) {
            const char *v; size_t vn;
            const char *q = wk_string(p, end, &v, &vn);
            if (!q) return OC_WK_MALFORMED;
            if (vn == 0 || vn >= sizeof out->fingerprint) return OC_WK_MALFORMED;
            memcpy(out->fingerprint, v, vn);
            out->fingerprint[vn] = '\0';
            p = q;
        } else {
            p = wk_skip_value(p, end);          /* a key we do not read */
            if (!p) return OC_WK_MALFORMED;
        }

        p = wk_ws(p, end);
        if (p < end && *p == ',') { p++; continue; }
        if (p < end && *p == '}') { p++; break; }
        return OC_WK_MALFORMED;                 /* no separator, no close */
    }
    p = wk_ws(p, end);
    if (p != end) return OC_WK_MALFORMED;       /* trailing junk is not a document */
    return OC_WK_OK;
}

int oc_wellknown_read_response(const char *resp, size_t len, oc_wellknown *out) {
    if (!resp || !out) return OC_WK_NONE;
    memset(out, 0, sizeof *out);
    /* "HTTP/1.x 200" and nothing else. A redirect, a 404 or an error page is "no
     * document" rather than a malformed one, because none of them claims to be
     * this -- and REQ-011's distinct failure is meant for a workspace that
     * published something wrong, not for a web server going about its day. */
    if (len < 12 || strncmp(resp, "HTTP/1.", 7) != 0) return OC_WK_NONE;
    if (strncmp(resp + 9, "200", 3) != 0) return OC_WK_NONE;

    const char *body = NULL;
    for (size_t i = 0; i + 3 < len; i++)
        if (resp[i] == '\r' && resp[i+1] == '\n' && resp[i+2] == '\r' && resp[i+3] == '\n') {
            body = resp + i + 4; break;
        }
    if (!body) return OC_WK_MALFORMED;          /* a 200 that never ends its headers */
    size_t blen = len - (size_t)(body - resp);
    if (blen > OC_WK_MAX) blen = OC_WK_MAX;
    return oc_wellknown_parse(body, blen, out);
}

int oc_wellknown_fingerprint_bytes(const char *hex, unsigned char out[32]) {
    if (!hex || !out) return -1;
    int n = 0;
    int hi = -1;
    for (const char *p = hex; *p; p++) {
        if (*p == ':' || *p == ' ') continue;       /* the separators people paste */
        int v;
        if (*p >= '0' && *p <= '9') v = *p - '0';
        else if (*p >= 'a' && *p <= 'f') v = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F') v = *p - 'A' + 10;
        else return -1;                             /* not hex at all */
        if (hi < 0) { hi = v; continue; }
        if (n >= 32) return -1;                     /* longer than a SHA-256 */
        out[n++] = (unsigned char)((hi << 4) | v);
        hi = -1;
    }
    if (hi >= 0) return -1;                         /* an odd digit left over */
    return n == 32 ? 0 : -1;
}

/* --- fetching ------------------------------------------------------------ */

static int wk_dial(const char *host, int port, int timeout_ms) {
    oc_sock_startup();
    char portstr[16];
    snprintf(portstr, sizeof portstr, "%d", port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, portstr, &hints, &res) != 0) return -1;
    int fd = -1;
    for (struct addrinfo *a = res; a; a = a->ai_next) {
        fd = (int)socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, a->ai_addr, (int)a->ai_addrlen) == 0) break;
        oc_closesock(fd); fd = -1;
    }
    freeaddrinfo(res);
    if (fd >= 0) oc_sock_setnonblock(fd);
    (void)timeout_ms;
    return fd;
}

/* Drive one TLS step, waiting on the socket while it asks to. Returns 0 when
 * the step completed, -1 on error or past `deadline`. */
static int wk_pump(int fd, oc_tls_status st, uint64_t now, uint64_t deadline) {
    if (st == OC_TLS_OK) return 0;
    if (st != OC_TLS_WANT_READ && st != OC_TLS_WANT_WRITE) return -1;
    if (now >= deadline) return -1;
    int wait = (int)(deadline - now);
    if (wait > 250) wait = 250;
    oc_poll(fd, st == OC_TLS_WANT_WRITE, wait);
    return 1;                                   /* call again */
}

int oc_wellknown_fetch(const char *domain, const char *ca_bundle, oc_wellknown *out) {
    if (!domain || !domain[0] || !out) return OC_WK_NONE;
    memset(out, 0, sizeof *out);

    oc_tls_client tls;
    /* No anchors, no metadata. Refusing here rather than falling back to an
     * unverified fetch is the whole point (wellknown.h). */
    if (oc_tls_client_init_ca(&tls, ca_bundle) != 0) return OC_WK_NONE;

    int rc = OC_WK_NONE;
    int fd = wk_dial(domain, 443, WK_DEADLINE_MS);
    if (fd < 0) { oc_tls_client_free(&tls); return OC_WK_NONE; }

    uint64_t deadline = oc_model_now_ms() + WK_DEADLINE_MS;
    oc_tls_conn conn;
    if (oc_tls_conn_init(&conn, &tls.conf, fd) != 0) goto done_fd;
    if (oc_tls_conn_set_hostname(&conn, domain) != 0) goto done_conn;

    for (;;) {
        int step = wk_pump(fd, oc_tls_handshake(&conn), oc_model_now_ms(), deadline);
        if (step == 0) break;
        if (step < 0) goto done_conn;
    }

    {
        char req[512];
        /* HTTP/1.0, deliberately. A 1.1 server may answer with chunked transfer
         * encoding, whose framing would arrive inside the body and read as a
         * malformed document -- reporting a workspace as misconfigured because
         * its web server chose an encoding. 1.0 has no chunked encoding to
         * choose, and the response ends when the connection closes, which is
         * what this reads anyway. Host is sent regardless, because name-based
         * virtual hosts need it and every server accepts it. */
        int rn = snprintf(req, sizeof req,
                          "GET %s HTTP/1.0\r\nHost: %s\r\n"
                          "User-Agent: openchime-client\r\nAccept: application/json\r\n"
                          "Connection: close\r\n\r\n", OC_WK_PATH, domain);
        if (rn <= 0 || (size_t)rn >= sizeof req) goto done_conn;
        size_t sent = 0;
        while (sent < (size_t)rn) {
            size_t w = 0;
            oc_tls_status st = oc_tls_write(&conn, req + sent, (size_t)rn - sent, &w);
            sent += w;
            if (st == OC_TLS_OK) continue;
            int step = wk_pump(fd, st, oc_model_now_ms(), deadline);
            if (step < 0) goto done_conn;
        }
    }

    {
        char buf[OC_WK_MAX + 1024];             /* the body's cap, plus headers */
        size_t len = 0;
        for (;;) {
            if (len >= sizeof buf - 1) break;   /* more than a document can be */
            size_t got = 0;
            oc_tls_status st = oc_tls_read(&conn, buf + len, sizeof buf - 1 - len, &got);
            len += got;
            if (st == OC_TLS_OK) {
                if (got == 0) break;            /* peer closed */
                continue;
            }
            if (st == OC_TLS_CLOSED) break;
            int step = wk_pump(fd, st, oc_model_now_ms(), deadline);
            if (step < 0) break;
        }
        buf[len] = '\0';
        rc = oc_wellknown_read_response(buf, len, out);
    }

done_conn:
    oc_tls_conn_free(&conn);
done_fd:
    oc_closesock(fd);
    oc_tls_client_free(&tls);
    return rc;
}
