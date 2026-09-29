#include "https_client.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "sock.h"   /* oc_connect_any */
#include "tls.h"
#include "url.h"    /* oc_url_authority, oc_url_hostheader */

/* A response larger than this is not an ACME object or a certificate chain. */
#define RESP_MAX (1024 * 1024)

static void fail(char *err, size_t cap, const char *fmt, ...) {
    if (!err || !cap) return;
    va_list ap; va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

typedef struct {
    int           fd, tls;
    oc_tls_client cli;
    oc_tls_conn   conn;
} hconn;

static int hwrite(hconn *c, const void *buf, size_t len) {
    const char *p = buf; size_t sent = 0;
    while (sent < len) {
        if (c->tls) {
            size_t n = 0;
            oc_tls_status st = oc_tls_write(&c->conn, p + sent, len - sent, &n);
            if (st == OC_TLS_WANT_READ || st == OC_TLS_WANT_WRITE) continue;
            if (st != OC_TLS_OK || n == 0) return -1;
            sent += n;
        } else {
            ssize_t n = send(c->fd, p + sent, len - sent, 0);
            if (n <= 0) return -1;
            sent += (size_t)n;
        }
    }
    return 0;
}

static long hread(hconn *c, void *buf, size_t cap) {
    if (c->tls) {
        for (;;) {
            size_t n = 0;
            oc_tls_status st = oc_tls_read(&c->conn, buf, cap, &n);
            if (st == OC_TLS_WANT_READ || st == OC_TLS_WANT_WRITE) continue;
            if (st == OC_TLS_CLOSED) return 0;
            return st == OC_TLS_OK ? (long)n : -1;
        }
    }
    ssize_t n = recv(c->fd, buf, cap, 0);
    return n < 0 ? -1 : (long)n;
}

/* Undo chunked transfer coding in place; the decoded length. */
static size_t dechunk(char *b, size_t len) {
    size_t in = 0, out = 0;
    while (in < len) {
        char *nl = memchr(b + in, '\n', len - in);
        if (!nl) break;
        long sz = strtol(b + in, NULL, 16);
        in = (size_t)(nl - b) + 1;
        if (sz <= 0) break;
        if ((size_t)sz > len - in) sz = (long)(len - in);
        memmove(b + out, b + in, (size_t)sz);
        out += (size_t)sz; in += (size_t)sz;
        if (in < len && b[in] == '\r') in++;
        if (in < len && b[in] == '\n') in++;
    }
    return out;
}

int oc_https_request(const char *method, const char *url, const char *ctype,
                     const void *body, size_t body_len, const char *extra,
                     int timeout_ms, oc_https_resp *r, char *err, size_t errcap) {
    memset(r, 0, sizeof *r);
    int tls;
    char host[256], port[8], hosthdr[300];
    const char *p = url;
    if (!strncmp(p, "https://", 8)) { tls = 1; p += 8; snprintf(port, sizeof port, "443"); }
    else if (!strncmp(p, "http://", 7)) { tls = 0; p += 7; snprintf(port, sizeof port, "80"); }
    else { fail(err, errcap, "not an http(s) URL: %s", url); return -1; }
    size_t alen = strcspn(p, "/?#");
    if (oc_url_authority(p, alen, host, sizeof host, port, sizeof port) != 0) {
        fail(err, errcap, "bad address in %s", url); return -1;
    }
    const char *path = p[alen] ? p + alen : "/";
    if (!tls) {
        /* Plain HTTP only to loopback: a test's fake CA. */
        if (strcmp(host, "127.0.0.1") && strcmp(host, "::1") && strcmp(host, "localhost")) {
            fail(err, errcap, "refusing plain http to %s", host); return -1;
        }
    }
    int def = (tls && !strcmp(port, "443")) || (!tls && !strcmp(port, "80"));
    if (oc_url_hostheader(host, def ? NULL : port, hosthdr, sizeof hosthdr) != 0) {
        fail(err, errcap, "bad host %s", host); return -1;
    }

    hconn c; memset(&c, 0, sizeof c);
    c.fd = oc_connect_any(host, atoi(port), OC_CONNECT_PER_ADDR_MS);
    if (c.fd < 0) { fail(err, errcap, "could not connect to %s:%s", host, port); return -1; }
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    setsockopt(c.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(c.fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    int rc = -1, have_cli = 0;
    char *buf = NULL;
    if (tls) {
        have_cli = oc_tls_client_init_ca(&c.cli) == 0;
        if (!have_cli || oc_tls_conn_init(&c.conn, &c.cli.conf, c.fd) != 0) {
            fail(err, errcap, "TLS setup failed"); goto out;
        }
        c.tls = 1;
        if (oc_tls_conn_set_hostname(&c.conn, host) != 0) { fail(err, errcap, "TLS setup failed"); goto out; }
        for (;;) {
            oc_tls_status st = oc_tls_handshake(&c.conn);
            if (st == OC_TLS_OK) break;
            if (st == OC_TLS_WANT_READ || st == OC_TLS_WANT_WRITE) continue;
            fail(err, errcap, "TLS handshake with %s failed%s", host,
                 oc_tls_conn_cert_rejected(&c.conn) ? " (certificate not trusted)" : "");
            goto out;
        }
    }

    char head[2048];
    int n = snprintf(head, sizeof head, "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: openchimed\r\n"
                     "Accept: */*\r\nConnection: close\r\n%s%s%s",
                     method, path, hosthdr, ctype ? "Content-Type: " : "", ctype ? ctype : "",
                     ctype ? "\r\n" : "");
    if (n < 0 || (size_t)n >= sizeof head) { fail(err, errcap, "request too long"); goto out; }
    if (body || !strcmp(method, "POST")) {
        int m = snprintf(head + n, sizeof head - (size_t)n, "Content-Length: %zu\r\n", body ? body_len : 0);
        if (m < 0 || (size_t)m >= sizeof head - (size_t)n) { fail(err, errcap, "request too long"); goto out; }
        n += m;
    }
    if (hwrite(&c, head, (size_t)n) != 0 ||
        (extra && hwrite(&c, extra, strlen(extra)) != 0) ||
        hwrite(&c, "\r\n", 2) != 0 ||
        (body && body_len && hwrite(&c, body, body_len) != 0)) {
        fail(err, errcap, "could not send to %s", host); goto out;
    }

    size_t cap = 16384, len = 0;
    buf = malloc(cap + 1);
    if (!buf) { fail(err, errcap, "out of memory"); goto out; }
    for (;;) {
        if (len == cap) {
            if (cap >= RESP_MAX) { fail(err, errcap, "response too large"); goto out; }
            char *nb = realloc(buf, cap * 2 + 1);
            if (!nb) { fail(err, errcap, "out of memory"); goto out; }
            buf = nb; cap *= 2;
        }
        long got = hread(&c, buf + len, cap - len);
        if (got < 0) { fail(err, errcap, "read from %s failed", host); goto out; }
        if (got == 0) break;
        len += (size_t)got;
    }
    buf[len] = '\0';
    char *end = strstr(buf, "\r\n\r\n");
    if (!end || strncmp(buf, "HTTP/1.", 7) != 0) { fail(err, errcap, "not an HTTP response"); goto out; }
    size_t hlen = (size_t)(end - buf) + 2;
    r->head = malloc(hlen + 1);
    char *b = end + 4;
    size_t blen = len - (size_t)(b - buf);
    r->body = malloc(blen + 1);
    if (!r->head || !r->body) { fail(err, errcap, "out of memory"); oc_https_resp_free(r); goto out; }
    memcpy(r->head, buf, hlen); r->head[hlen] = '\0';
    memcpy(r->body, b, blen);
    r->status = atoi(buf + 9);
    char te[64];
    if (oc_https_header(r, "Transfer-Encoding", te, sizeof te) && strcasestr(te, "chunked"))
        blen = dechunk(r->body, blen);
    r->body[blen] = '\0';
    r->body_len = blen;
    rc = 0;
out:
    free(buf);
    if (c.tls) oc_tls_conn_free(&c.conn);
    if (have_cli) oc_tls_client_free(&c.cli);
    close(c.fd);
    return rc;
}

int oc_https_header(const oc_https_resp *r, const char *name, char *out, size_t cap) {
    if (!r || !r->head || !cap) return 0;
    size_t nl = strlen(name);
    const char *p = strstr(r->head, "\r\n");
    while (p && p[2]) {
        const char *line = p + 2;
        const char *eol = strstr(line, "\r\n");
        if (!eol) break;
        if ((size_t)(eol - line) > nl && !strncasecmp(line, name, nl) && line[nl] == ':') {
            const char *v = line + nl + 1;
            while (v < eol && (*v == ' ' || *v == '\t')) v++;
            const char *e = eol;
            while (e > v && (e[-1] == ' ' || e[-1] == '\t')) e--;
            size_t n = (size_t)(e - v);
            if (n >= cap) n = cap - 1;
            memcpy(out, v, n); out[n] = '\0';
            return 1;
        }
        p = eol;
    }
    return 0;
}

void oc_https_resp_free(oc_https_resp *r) {
    if (!r) return;
    free(r->head); free(r->body);
    r->head = r->body = NULL;
    r->body_len = 0;
}
