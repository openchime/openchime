/* The I/O threads (daemon/ioloop.c), driven directly: a real TLS client on
 * loopback, a socket accepted here and handed over, and the events read back as
 * the event loop reads them. Checks that a connection opens with the protocol it
 * negotiated; that a thousand frames arrive whole and in order, and a thousand
 * sends leave in order; that a paused connection delivers nothing until resumed;
 * that output held by a reader that stops is reported written once it drains;
 * that close_after closes after the last byte; that a peer's close is reported
 * once and the socket kept until the loop closes it; and that a command for a
 * connection already closed is ignored. And HTTP (ARCH-32): a request for a
 * loop route is parsed there, sent in pieces or at once, and reported once; a
 * static route is answered there; every refusal -- 400, 404, 405, 408, 413 --
 * is answered there and never reported; one request per connection; and a
 * plaintext socket (the health port, ARCH-25) is served its own site. */

#include "ioloop.h"
#include "framebuf.h"
#include "protocol.h"
#include "tls.h"
#include "check.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static oc_io_event *wait_event(oc_ioloop *io, int ms) {
    struct pollfd p = { oc_ioloop_eventfd(io), POLLIN, 0 };
    for (int waited = 0; waited <= ms; waited += 10) {
        oc_io_event *e = oc_ioloop_next(io);
        if (e) return e;
        poll(&p, 1, 10);
        uint64_t cnt; ssize_t r = read(p.fd, &cnt, sizeof cnt); (void)r;
    }
    return NULL;
}

/* The next event of `kind` for `conn_id`, skipping WRITTEN reports. */
static oc_io_event *wait_kind(oc_ioloop *io, oc_io_kind kind, int ms) {
    for (;;) {
        oc_io_event *e = wait_event(io, ms);
        if (!e || e->kind == kind) return e;
        oc_io_event_free(e);
    }
}

typedef struct { int fd; oc_tls_client cli; oc_tls_conn conn; oc_framebuf fb; } tclient;

static int tclient_open(tclient *c, uint16_t port, const char **alpn) {
    memset(c, 0, sizeof *c);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(port);
    c->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(c->fd, (struct sockaddr *)&a, sizeof a) != 0) return -1;
    struct timeval tv = { 5, 0 };
    setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (oc_tls_client_init_ex(&c->cli, NULL, alpn) != 0) return -1;
    if (oc_tls_conn_init(&c->conn, &c->cli.conf, c->fd) != 0) return -1;
    return oc_framebuf_init(&c->fb);
}

static int tclient_handshake(tclient *c) {
    for (int i = 0; i < 100000; i++) {
        oc_tls_status st = oc_tls_handshake(&c->conn);
        if (st == OC_TLS_OK) return 0;
        if (st != OC_TLS_WANT_READ && st != OC_TLS_WANT_WRITE) return -1;
    }
    return -1;
}

static void tclient_close(tclient *c) {
    oc_framebuf_free(&c->fb);
    oc_tls_conn_free(&c->conn);
    oc_tls_client_free(&c->cli);
    close(c->fd);
}

static int twrite(tclient *c, const uint8_t *b, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        size_t n = 0;
        oc_tls_status st = oc_tls_write(&c->conn, b + sent, len - sent, &n);
        if (st == OC_TLS_WANT_READ || st == OC_TLS_WANT_WRITE) continue;
        if (st != OC_TLS_OK) return -1;
        sent += n;
    }
    return 0;
}

/* The next frame's payload as a u32 (the tests' frames carry one), -1 at close
 * or timeout. */
static long tread_u32(tclient *c) {
    for (;;) {
        const uint8_t *f; size_t fl;
        int r = oc_framebuf_next(&c->fb, &f, &fl);
        if (r < 0) return -1;
        if (r == 1) {
            oc_header h; oc_rbuf p;
            if (oc_parse_frame(f, fl, &h, &p) != OC_OK) return -1;
            return (long)oc_r_u32(&p);
        }
        uint8_t buf[16384]; size_t n = 0;
        oc_tls_status st = oc_tls_read(&c->conn, buf, sizeof buf, &n);
        if (st != OC_TLS_OK || !n) return -1;
        if (oc_framebuf_push(&c->fb, buf, n) != 0) return -1;
    }
}

/* A frame whose payload is `v` and `pad` bytes more, big-endian. */
static size_t mk_frame(uint8_t *out, size_t cap, uint32_t v, size_t pad) {
    oc_wbuf w; oc_wbuf_init(&w, out, cap);
    oc_w_u32(&w, (uint32_t)(4 + 4 + pad));
    oc_w_u16(&w, OC_PROTOCOL_VERSION);
    oc_w_u16(&w, OC_MSG_TYPING);
    oc_w_u32(&w, v);
    for (size_t i = 0; i < pad; i++) oc_w_u8(&w, (uint8_t)i);
    return w.overflow ? 0 : w.len;
}

static uint32_t frame_u32(const oc_io_event *e) {
    oc_header h; oc_rbuf p;
    if (oc_parse_frame(e->data, e->len, &h, &p) != OC_OK) return 0xFFFFFFFFu;
    return oc_r_u32(&p);
}

typedef struct { tclient *c; int done; } hs_arg;
static void *hs_thread(void *p) { hs_arg *a = p; a->done = tclient_handshake(a->c) == 0 ? 1 : -1; return NULL; }

/* Connect a client, accept it here, hand the socket over and wait for OPENED. */
static int open_conn(oc_ioloop *io, int lfd, uint16_t port, uint64_t id, tclient *c,
                     const char **alpn, int *fd_out, int *http_out) {
    if (tclient_open(c, port, alpn) != 0) return -1;
    int fd = accept(lfd, NULL, NULL);
    if (fd < 0) return -1;
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) != 0) return -1;
    hs_arg ha = { c, 0 };
    pthread_t th; pthread_create(&th, NULL, hs_thread, &ha);
    if (oc_ioloop_adopt(io, fd, id, "127.0.0.1", 0) != 0) return -1;
    pthread_join(th, NULL);
    if (ha.done != 1) return -1;
    oc_io_event *e = wait_kind(io, OC_IO_OPENED, 3000);
    if (!e || e->conn_id != id) { oc_io_event_free(e); return -1; }
    *fd_out = fd;
    if (http_out) *http_out = e->http;
    oc_io_event_free(e);
    return 0;
}

static const char *oc1_alpn[] = { OC_ALPN_PROTO, NULL };
static const char *http_alpn[] = { OC_ALPN_HTTP11, NULL };

/* The sites the HTTP checks are served: on TLS a loop route and a static one,
 * with a small body limit so 413 is cheap to reach; in plaintext a health route
 * and a fallback for everything else. */
static const oc_http_route T_ROUTES[] = {
    { "POST", "/webhook/", 1, OC_HTTP_LOOP, 64, NULL, NULL, 0, NULL },
    { "GET",  "/static",   0, OC_HTTP_STATIC, 0, "text/plain", "hello", 5, NULL },
};
static const oc_http_site T_SITE = { T_ROUTES, 2, NULL };
static const oc_http_route P_ROUTES[] = {
    { NULL, "/healthz", 0, OC_HTTP_STATIC, 16, "text/plain", "OK", 2, NULL },
};
static const oc_http_route P_FALLBACK = { NULL, "/", 1, OC_HTTP_STATIC, 16, "text/html", "<p>here</p>", 11, NULL };
static const oc_http_site P_SITE = { P_ROUTES, 1, &P_FALLBACK };

/* Everything the peer sends until it closes, as a string -- or what came within
 * five seconds, so an answer that never comes fails the check instead of
 * hanging the suite. */
static size_t tread_all(tclient *c, char *buf, size_t cap) {
    size_t got = 0;
    struct timeval t0, t; gettimeofday(&t0, NULL);
    while (got + 1 < cap) {
        size_t n = 0;
        oc_tls_status st = oc_tls_read(&c->conn, (uint8_t *)buf + got, cap - 1 - got, &n);
        gettimeofday(&t, NULL);
        if ((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_usec - t0.tv_usec) / 1000 > 5000) break;
        if (st == OC_TLS_WANT_READ || st == OC_TLS_WANT_WRITE) continue;
        if (st != OC_TLS_OK || !n) break;
        got += n;
    }
    buf[got] = 0;
    return got;
}

static size_t plain_read_all(int fd, char *buf, size_t cap) {
    size_t got = 0;
    while (got + 1 < cap) {
        ssize_t r = recv(fd, buf + got, cap - 1 - got, 0);
        if (r <= 0) break;
        got += (size_t)r;
    }
    buf[got] = 0;
    return got;
}

static int count_of(const char *hay, const char *needle) {
    int n = 0;
    for (const char *p = strstr(hay, needle); p; p = strstr(p + 1, needle)) n++;
    return n;
}

/* Send `req` on a fresh HTTP connection `id` and return what comes back, having
 * seen the connection answered on its I/O thread: finished (CLOSED) with no
 * request reported, then closed here as the loop would. */
static int http_exchange(oc_ioloop *io, int lfd, uint16_t port, uint64_t id,
                         const char *req, size_t rl, char *resp, size_t cap) {
    tclient w; int wfd = -1, whttp = -1;
    if (open_conn(io, lfd, port, id, &w, http_alpn, &wfd, &whttp) != 0 || whttp != 1) return -1;
    int ok = twrite(&w, (const uint8_t *)req, rl) == 0;
    oc_io_event *e = wait_event(io, 3000);
    int reported = 0, closed = 0;
    while (e) {
        if (e->kind == OC_IO_HTTP_REQ) reported = 1;
        if (e->kind == OC_IO_CLOSED && e->conn_id == id) closed = 1;
        oc_io_event_free(e);
        if (closed) break;
        e = wait_event(io, 3000);
    }
    oc_ioloop_close(io, id, wfd);
    tread_all(&w, resp, cap);
    tclient_close(&w);
    return ok && closed && !reported ? 0 : -1;
}

/* A plaintext connection, accepted here and adopted as the health port's. */
static int plain_open(oc_ioloop *io, int lfd, uint16_t port, uint64_t id, int *cfd, int *sfd) {
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(port);
    *cfd = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(*cfd, (struct sockaddr *)&a, sizeof a) != 0) return -1;
    struct timeval tv = { 5, 0 };
    setsockopt(*cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    *sfd = accept(lfd, NULL, NULL);
    if (*sfd < 0) return -1;
    int fl = fcntl(*sfd, F_GETFL, 0);
    if (fl < 0 || fcntl(*sfd, F_SETFL, fl | O_NONBLOCK) != 0) return -1;
    if (oc_ioloop_adopt_plain(io, *sfd, id, "127.0.0.1") != 0) return -1;
    oc_io_event *e = wait_kind(io, OC_IO_OPENED, 3000);
    int ok = e && e->conn_id == id && e->http == 1;
    oc_io_event_free(e);
    return ok ? 0 : -1;
}

/* Wait for connection `id`'s CLOSED, then close it as the loop would. */
static int finished(oc_ioloop *io, uint64_t id, int sfd, int ms) {
    oc_io_event *e = wait_kind(io, OC_IO_CLOSED, ms);
    int ok = e && e->conn_id == id;
    oc_io_event_free(e);
    oc_ioloop_close(io, id, sfd);
    return ok;
}

int run_ioloop_tests(void) {
    printf("test_ioloop: I/O threads -- the negotiated protocol, frames in order both ways, "
           "pause, written reports, close_after, a peer's close, stale commands\n");
    oc_tls_server srv;
    CHECK(oc_tls_server_init(&srv, NULL, NULL) == 0);
    oc_ioloop *io = oc_ioloop_start(2, &srv, NULL, &T_SITE, &P_SITE);
    CHECK(io != NULL);
    if (!io) return failures;

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(lfd, (struct sockaddr *)&a, sizeof a) == 0 && listen(lfd, 16) == 0);
    socklen_t al = sizeof a; getsockname(lfd, (struct sockaddr *)&a, &al);
    uint16_t port = ntohs(a.sin_port);

    /* oc/1, then an HTTP client: OPENED says which. */
    tclient c, h; int fd = -1, hfd = -1, http = -1;
    CHECK(open_conn(io, lfd, port, 11, &c, oc1_alpn, &fd, &http) == 0 && http == 0);
    CHECK(open_conn(io, lfd, port, 12, &h, http_alpn, &hfd, &http) == 0 && http == 1);

    /* A thousand frames from the client, written in one go: each arrives whole,
     * in order. */
    {
        enum { N = 1000 };
        uint8_t *burst = malloc(N * 64); size_t bl = 0;
        for (uint32_t i = 0; i < N; i++) bl += mk_frame(burst + bl, 64, i, (size_t)(i % 40));
        CHECK(twrite(&c, burst, bl) == 0);
        free(burst);
        uint32_t next = 0; int bad = 0;
        while (next < N) {
            oc_io_event *e = wait_kind(io, OC_IO_FRAME, 3000);
            if (!e) break;
            if (e->conn_id != 11 || frame_u32(e) != next) bad = 1;
            next++;
            oc_io_event_free(e);
        }
        CHECK(next == N && !bad);
    }

    /* A thousand sends from the loop: they leave in order. */
    {
        for (uint32_t i = 0; i < 1000; i++) {
            uint8_t f[64]; size_t fl = mk_frame(f, sizeof f, i, 8);
            CHECK(oc_ioloop_send(io, 11, fd, f, fl, 0) == 0);
        }
        int bad = 0;
        for (uint32_t i = 0; i < 1000; i++) if (tread_u32(&c) != (long)i) { bad = 1; break; }
        CHECK(!bad);
    }

    /* Paused, nothing is delivered; resumed, it all is, in order. */
    {
        oc_ioloop_pause(io, 11, fd, 1);
        usleep(20000);
        uint8_t f[64]; size_t fl;
        for (uint32_t i = 0; i < 5; i++) { fl = mk_frame(f, sizeof f, 500 + i, 0); CHECK(twrite(&c, f, fl) == 0); }
        oc_io_event *e = wait_kind(io, OC_IO_FRAME, 100);
        CHECK(e == NULL);
        oc_io_event_free(e);
        oc_ioloop_pause(io, 11, fd, 0);
        int bad = 0;
        for (uint32_t i = 0; i < 5; i++) {
            e = wait_kind(io, OC_IO_FRAME, 3000);
            if (!e || frame_u32(e) != 500 + i) bad = 1;
            oc_io_event_free(e);
            if (bad) break;
        }
        CHECK(!bad);
    }

    /* Held output: 2 MB sent to a client that is not reading. Once it reads it
     * all, a WRITTEN report accounts for every byte. */
    {
        uint64_t total = 0;
        static uint8_t f[OC_MAX_FRAME_SIZE];
        for (uint32_t i = 0; i < 40; i++) {
            size_t fl = mk_frame(f, sizeof f, 1000 + i, 50000);
            CHECK(oc_ioloop_send(io, 11, fd, f, fl, 0) == 0);
            total += fl;
        }
        int bad = 0;
        for (uint32_t i = 0; i < 40; i++) if (tread_u32(&c) != (long)(1000 + i)) { bad = 1; break; }
        CHECK(!bad);
        uint64_t written = 0;
        for (int k = 0; k < 50 && written < total; k++) {
            oc_io_event *e = wait_kind(io, OC_IO_WRITTEN, 200);
            if (e && e->conn_id == 11) written = e->written;
            oc_io_event_free(e);
        }
        CHECK(written >= total);
    }

    /* A command for a connection that is not there is ignored. */
    {
        uint8_t f[64]; size_t fl = mk_frame(f, sizeof f, 1, 0);
        CHECK(oc_ioloop_send(io, 999, fd, f, fl, 0) == 0);   /* wrong id, live descriptor */
        oc_ioloop_pause(io, 999, fd, 1);
        /* Then one for the connection that is there. Commands are taken in
         * order, so had the stale send gone out it would be read first, and had
         * the stale pause taken hold this would not arrive at all. */
        fl = mk_frame(f, sizeof f, 7, 0);
        CHECK(oc_ioloop_send(io, 11, fd, f, fl, 0) == 0);
        CHECK(tread_u32(&c) == 7);
    }

    /* close_after: the reply, then the connection is finished and reported. */
    {
        const char *reply = "HTTP/1.1 204 No Content\r\n\r\n";
        CHECK(oc_ioloop_send(io, 12, hfd, (const uint8_t *)reply, strlen(reply), 1) == 0);
        /* The reply, read to its length: the socket stays open until the loop
         * closes it below, so reading to the end would wait out the client's
         * timeout for a close that has not been asked for yet. */
        char got[128]; size_t gl = 0, want = strlen(reply);
        while (gl < want) {
            size_t n = 0;
            oc_tls_status st = oc_tls_read(&h.conn, got + gl, want - gl, &n);
            if (st != OC_TLS_OK || !n) break;
            gl += n;
        }
        got[gl] = 0;
        CHECK(strcmp(got, reply) == 0);
        oc_io_event *e = wait_kind(io, OC_IO_CLOSED, 3000);
        CHECK(e && e->conn_id == 12);
        oc_io_event_free(e);
        oc_ioloop_close(io, 12, hfd);
        tclient_close(&h);
    }

    /* The peer closes: reported once; the socket is the loop's to close. */
    {
        tclient_close(&c);
        oc_io_event *e = wait_kind(io, OC_IO_CLOSED, 3000);
        CHECK(e && e->conn_id == 11);
        oc_io_event_free(e);
        CHECK(fcntl(fd, F_GETFD) != -1);                     /* still open */
        oc_ioloop_close(io, 11, fd);
        e = wait_event(io, 200);
        CHECK(e == NULL || e->kind == OC_IO_WRITTEN);
        oc_io_event_free(e);
        usleep(50000);
    }

    /* HTTP, parsed on the I/O thread (ARCH-32): a POST for the loop's route, sent
     * a few bytes at a time, is reported once, whole, as its parts; the request
     * after it is not read. */
    {
        tclient w; int wfd = -1, whttp = -1;
        CHECK(open_conn(io, lfd, port, 21, &w, http_alpn, &wfd, &whttp) == 0 && whttp == 1);
        const char *req = "POST /webhook/abc HTTP/1.1\r\nHost: x\r\n"
                          "Content-Type: application/json\r\nContent-Length: 13\r\n\r\n{\"text\":\"hi\"}";
        size_t rl = strlen(req);
        for (size_t off = 0; off < rl; off += 7) {
            size_t n = rl - off < 7 ? rl - off : 7;
            CHECK(twrite(&w, (const uint8_t *)req + off, n) == 0);
            usleep(2000);
        }
        oc_io_event *e = wait_kind(io, OC_IO_HTTP_REQ, 3000);
        CHECK(e && e->conn_id == 21 && e->is_json);
        if (e) {
            CHECK(e->method_len == 4 && memcmp(e->data, "POST", 4) == 0);
            CHECK(e->path_len == 12 && memcmp(e->data + 4, "/webhook/abc", 12) == 0);
            CHECK(e->body_len == 13 && memcmp(e->data + 16, "{\"text\":\"hi\"}", 13) == 0);
        }
        oc_io_event_free(e);
        /* The same request again, while the first is the loop's: it is not a
         * second request, and nothing more is reported. */
        CHECK(twrite(&w, (const uint8_t *)req, rl) == 0);
        e = wait_kind(io, OC_IO_HTTP_REQ, 300);
        CHECK(e == NULL);                                    /* one request, one report */
        oc_io_event_free(e);
        oc_ioloop_close(io, 21, wfd);
        tclient_close(&w);
    }
    /* A static route is answered on the I/O thread, and the loop hears only
     * that the connection finished. So is every refusal, each with its status:
     * a path nothing names, a known path under the wrong method, a declared body
     * over the route's limit (before the body is sent), a malformed request
     * line, a head that never ends, a chunked body. */
    {
        static char resp[8192];
        const char *r200 = "GET /static?x=1 HTTP/1.1\r\nHost: x\r\n\r\n";
        CHECK(http_exchange(io, lfd, port, 30, r200, strlen(r200), resp, sizeof resp) == 0);
        CHECK(strncmp(resp, "HTTP/1.1 200 OK\r\n", 17) == 0 && strstr(resp, "\r\n\r\nhello") &&
              strstr(resp, "Connection: close\r\n") && strstr(resp, "Content-Length: 5\r\n"));

        const char *r404 = "GET /nope HTTP/1.1\r\n\r\n";
        CHECK(http_exchange(io, lfd, port, 31, r404, strlen(r404), resp, sizeof resp) == 0);
        CHECK(strncmp(resp, "HTTP/1.1 404 ", 13) == 0);

        const char *r405 = "GET /webhook/abc HTTP/1.1\r\n\r\n";
        CHECK(http_exchange(io, lfd, port, 32, r405, strlen(r405), resp, sizeof resp) == 0);
        CHECK(strncmp(resp, "HTTP/1.1 405 ", 13) == 0);

        const char *r413 = "POST /webhook/abc HTTP/1.1\r\nContent-Length: 65\r\n\r\n";
        CHECK(http_exchange(io, lfd, port, 33, r413, strlen(r413), resp, sizeof resp) == 0);
        CHECK(strncmp(resp, "HTTP/1.1 413 ", 13) == 0);

        const char *r400 = "GARBAGE\r\n\r\n";
        CHECK(http_exchange(io, lfd, port, 34, r400, strlen(r400), resp, sizeof resp) == 0);
        CHECK(strncmp(resp, "HTTP/1.1 400 ", 13) == 0);

        static char big[OC_HTTP_MAX_HEAD + 64];
        int hl = snprintf(big, sizeof big, "GET /static HTTP/1.1\r\nX-Pad: ");
        memset(big + hl, 'x', sizeof big - (size_t)hl);
        CHECK(http_exchange(io, lfd, port, 35, big, sizeof big, resp, sizeof resp) == 0);
        CHECK(strncmp(resp, "HTTP/1.1 400 ", 13) == 0);

        const char *rte = "POST /webhook/abc HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nhi\r\n0\r\n\r\n";
        CHECK(http_exchange(io, lfd, port, 36, rte, strlen(rte), resp, sizeof resp) == 0);
        CHECK(strncmp(resp, "HTTP/1.1 400 ", 13) == 0);

        /* One request per connection: the second, sent with the first, gets
         * nothing. */
        const char *two = "GET /static HTTP/1.1\r\n\r\nGET /static HTTP/1.1\r\n\r\n";
        CHECK(http_exchange(io, lfd, port, 37, two, strlen(two), resp, sizeof resp) == 0);
        CHECK(count_of(resp, "HTTP/1.1 ") == 1);
    }
    /* A request that does not arrive whole in time is answered 408, and one
     * that never starts is too: neither holds its connection. */
    {
        oc_ioloop_set_http_timeout_ms(300);
        static char resp[4096];
        tclient w; int wfd = -1, whttp = -1;
        CHECK(open_conn(io, lfd, port, 40, &w, http_alpn, &wfd, &whttp) == 0);
        CHECK(twrite(&w, (const uint8_t *)"GET /sta", 8) == 0);
        struct timeval t0, t1; gettimeofday(&t0, NULL);
        CHECK(finished(io, 40, wfd, 3000));
        gettimeofday(&t1, NULL);
        long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_usec - t0.tv_usec) / 1000;
        CHECK(ms < 2000);
        tread_all(&w, resp, sizeof resp);
        CHECK(strncmp(resp, "HTTP/1.1 408 ", 13) == 0);
        tclient_close(&w);

        int cfd = -1, sfd = -1;
        CHECK(plain_open(io, lfd, port, 41, &cfd, &sfd) == 0);   /* sends nothing */
        CHECK(finished(io, 41, sfd, 3000));
        plain_read_all(cfd, resp, sizeof resp);
        CHECK(strncmp(resp, "HTTP/1.1 408 ", 13) == 0);
        close(cfd);
        oc_ioloop_set_http_timeout_ms(0);
    }
    /* Plaintext, as the health port is: OPENED at once as HTTP, served its own
     * site -- its route, and its fallback for any other path -- and the TLS
     * site's routes are not its. */
    {
        static char resp[4096];
        int cfd = -1, sfd = -1;
        CHECK(plain_open(io, lfd, port, 50, &cfd, &sfd) == 0);
        const char *hz = "GET /healthz HTTP/1.1\r\nHost: x\r\n\r\n";
        CHECK(send(cfd, hz, strlen(hz), 0) == (ssize_t)strlen(hz));
        CHECK(finished(io, 50, sfd, 3000));
        plain_read_all(cfd, resp, sizeof resp);
        CHECK(strncmp(resp, "HTTP/1.1 200 OK\r\n", 17) == 0 && strstr(resp, "\r\n\r\nOK"));
        close(cfd);

        CHECK(plain_open(io, lfd, port, 51, &cfd, &sfd) == 0);
        const char *st = "GET /static HTTP/1.1\r\n\r\n";
        CHECK(send(cfd, st, strlen(st), 0) == (ssize_t)strlen(st));
        CHECK(finished(io, 51, sfd, 3000));
        plain_read_all(cfd, resp, sizeof resp);
        CHECK(strncmp(resp, "HTTP/1.1 200 OK\r\n", 17) == 0 && strstr(resp, "<p>here</p>"));
        close(cfd);
    }

    oc_ioloop_stop(io);
    close(lfd);
    oc_tls_server_free(&srv);
    return failures;
}
