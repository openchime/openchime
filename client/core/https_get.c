/*
 * OpenChime client — one verified HTTPS GET. See https_get.h.
 */

#include "https_get.h"

#include "sock.h"      /* the POSIX/Winsock shim, and getaddrinfo with it */
#include "tls.h"
#include "model.h"     /* oc_model_now_ms: the core's one monotonic clock */

#include <stdio.h>
#include <string.h>

static int hg_dial(const char *host, int port, int timeout_ms) {
    /* Each address bounded (sock.h), and never longer than the fetch itself. */
    int per = timeout_ms > 0 && timeout_ms < OC_CONNECT_PER_ADDR_MS ? timeout_ms : OC_CONNECT_PER_ADDR_MS;
    int fd = oc_connect_any(host, port, per);
    if (fd >= 0) oc_sock_setnonblock(fd);
    return fd;
}

/* Drive one TLS step, waiting on the socket while it asks to. Returns 0 when
 * the step completed, -1 on error or past `deadline`, 1 to call again. */
static int hg_pump(int fd, oc_tls_status st, uint64_t now, uint64_t deadline) {
    if (st == OC_TLS_OK) return 0;
    if (st != OC_TLS_WANT_READ && st != OC_TLS_WANT_WRITE) return -1;
    if (now >= deadline) return -1;
    int wait = (int)(deadline - now);
    if (wait > 250) wait = 250;
    oc_poll(fd, st == OC_TLS_WANT_WRITE, wait);
    return 1;
}

long oc_https_get(const char *host, const char *path, const char *accept, int deadline_ms,
                  char *out, size_t cap) {
    if (!host || !host[0] || !path || !out || cap < 2) return -1;
    out[0] = '\0';
    oc_tls_client tls;
    /* No verifier, no fetch: never an unverified one in its place. */
    if (oc_tls_client_init_ca(&tls) != 0) return -1;
    long rc = -1;
    int fd = hg_dial(host, 443, deadline_ms);
    if (fd < 0) { oc_tls_client_free(&tls); return -1; }
    uint64_t deadline = oc_model_now_ms() + (uint64_t)(deadline_ms > 0 ? deadline_ms : 0);
    oc_tls_conn conn;
    if (oc_tls_conn_init(&conn, &tls.conf, fd) != 0) goto done_fd;
    if (oc_tls_conn_set_hostname(&conn, host) != 0) goto done_conn;
    for (;;) {
        int step = hg_pump(fd, oc_tls_handshake(&conn), oc_model_now_ms(), deadline);
        if (step == 0) break;
        if (step < 0) goto done_conn;
    }
    {
        char req[1024];
        /* Host is sent regardless: name-based virtual hosts need it, and every
         * server accepts it. */
        int rn = snprintf(req, sizeof req,
                          "GET %s HTTP/1.0\r\nHost: %s\r\n"
                          "User-Agent: openchime-client\r\nAccept: %s\r\n"
                          "Connection: close\r\n\r\n", path, host, accept ? accept : "*/*");
        if (rn <= 0 || (size_t)rn >= sizeof req) goto done_conn;
        size_t sent = 0;
        while (sent < (size_t)rn) {
            size_t w = 0;
            oc_tls_status st = oc_tls_write(&conn, req + sent, (size_t)rn - sent, &w);
            sent += w;
            if (st == OC_TLS_OK) continue;
            if (hg_pump(fd, st, oc_model_now_ms(), deadline) < 0) goto done_conn;
        }
    }
    {
        size_t len = 0;
        for (;;) {
            if (len >= cap - 1) break;          /* more than the caller takes */
            size_t got = 0;
            oc_tls_status st = oc_tls_read(&conn, out + len, cap - 1 - len, &got);
            len += got;
            if (st == OC_TLS_OK) {
                if (got == 0) break;            /* peer closed */
                continue;
            }
            if (st == OC_TLS_CLOSED) break;
            if (hg_pump(fd, st, oc_model_now_ms(), deadline) < 0) break;
        }
        out[len] = '\0';
        rc = len ? (long)len : -1;
    }
done_conn:
    oc_tls_conn_free(&conn);
done_fd:
    oc_closesock(fd);
    oc_tls_client_free(&tls);
    return rc;
}

int oc_http_response_status(const char *resp, size_t len, const char **body) {
    if (body) *body = NULL;
    if (!resp || len < 12 || strncmp(resp, "HTTP/1.", 7) != 0 || resp[8] != ' ') return 0;
    int st = 0;
    for (int i = 9; i < 12; i++) {
        if (resp[i] < '0' || resp[i] > '9') return 0;
        st = st * 10 + (resp[i] - '0');
    }
    if (body)
        for (size_t i = 0; i + 3 < len; i++)
            if (resp[i] == '\r' && resp[i + 1] == '\n' && resp[i + 2] == '\r' && resp[i + 3] == '\n') {
                *body = resp + i + 4;
                break;
            }
    return st;
}
