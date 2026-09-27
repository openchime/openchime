/* The I/O threads (daemon/ioloop.c), driven directly: a real TLS client on
 * loopback, a socket accepted here and handed over, and the events read back as
 * the event loop reads them. Checks that a connection opens with the protocol it
 * negotiated; that a thousand frames arrive whole and in order, and a thousand
 * sends leave in order; that a paused connection delivers nothing until resumed;
 * that output held by a reader that stops is reported written once it drains;
 * that close_after closes after the last byte; that a peer's close is reported
 * once and the socket kept until the loop closes it; and that a command for a
 * connection already closed is ignored; and that an HTTP connection's request
 * is parsed there, sent in pieces or all at once, and reported once, or refused
 * with the status that fits. */

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

int run_ioloop_tests(void) {
    printf("test_ioloop: I/O threads -- the negotiated protocol, frames in order both ways, "
           "pause, written reports, close_after, a peer's close, stale commands\n");
    oc_tls_server srv;
    CHECK(oc_tls_server_init(&srv, NULL, NULL) == 0);
    oc_ioloop *io = oc_ioloop_start(2, &srv, NULL);
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
        usleep(100000);
        uint8_t f[64]; size_t fl;
        for (uint32_t i = 0; i < 5; i++) { fl = mk_frame(f, sizeof f, 500 + i, 0); CHECK(twrite(&c, f, fl) == 0); }
        oc_io_event *e = wait_kind(io, OC_IO_FRAME, 300);
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
        CHECK(tread_u32(&c) == -1);                          /* nothing reaches the client */
    }

    /* close_after: the reply, then the connection is finished and reported. */
    {
        const char *reply = "HTTP/1.1 204 No Content\r\n\r\n";
        CHECK(oc_ioloop_send(io, 12, hfd, (const uint8_t *)reply, strlen(reply), 1) == 0);
        char got[128]; size_t gl = 0;
        for (;;) {
            size_t n = 0;
            oc_tls_status st = oc_tls_read(&h.conn, got + gl, sizeof got - 1 - gl, &n);
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

    /* HTTP, parsed on the I/O thread (ARCH-32): a POST sent a few bytes at a
     * time is reported once, whole, as its parts; what follows it is dropped. */
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
        CHECK(twrite(&w, (const uint8_t *)"GET / HTTP/1.1\r\n\r\n", 18) == 0);
        oc_io_event *e = wait_kind(io, OC_IO_HTTP_REQ, 3000);
        CHECK(e && e->conn_id == 21 && e->http_status == 0 && e->is_json);
        if (e) {
            CHECK(e->method_len == 4 && memcmp(e->data, "POST", 4) == 0);
            CHECK(e->path_len == 12 && memcmp(e->data + 4, "/webhook/abc", 12) == 0);
            CHECK(e->body_len == 13 && memcmp(e->data + 16, "{\"text\":\"hi\"}", 13) == 0);
        }
        oc_io_event_free(e);
        e = wait_kind(io, OC_IO_HTTP_REQ, 300);
        CHECK(e == NULL);                                    /* one request, one report */
        oc_io_event_free(e);
        oc_ioloop_close(io, 21, wfd);
        tclient_close(&w);
    }
    /* Malformed -- a request line with no method and path -- is refused 400;
     * so is a header block that never ends, once it passes the parser's 8 KiB. */
    {
        tclient w; int wfd = -1, whttp = -1;
        CHECK(open_conn(io, lfd, port, 22, &w, http_alpn, &wfd, &whttp) == 0);
        CHECK(twrite(&w, (const uint8_t *)"GARBAGE\r\n\r\n", 11) == 0);
        oc_io_event *e = wait_kind(io, OC_IO_HTTP_REQ, 3000);
        CHECK(e && e->conn_id == 22 && e->http_status == 400);
        oc_io_event_free(e);
        oc_ioloop_close(io, 22, wfd);
        tclient_close(&w);

        CHECK(open_conn(io, lfd, port, 23, &w, http_alpn, &wfd, &whttp) == 0);
        char head[128];
        int hl = snprintf(head, sizeof head, "POST /webhook/abc HTTP/1.1\r\nX-Pad: ");
        CHECK(twrite(&w, (const uint8_t *)head, (size_t)hl) == 0);
        static uint8_t junk[16384];
        memset(junk, 'x', sizeof junk);
        e = NULL;
        for (size_t sent = 0; sent <= OC_HTTP_MAX_REQUEST && !e; sent += sizeof junk) {
            if (twrite(&w, junk, sizeof junk) != 0) break;
            e = oc_ioloop_next(io);
            while (e && e->kind != OC_IO_HTTP_REQ) { oc_io_event_free(e); e = oc_ioloop_next(io); }
        }
        if (!e) e = wait_kind(io, OC_IO_HTTP_REQ, 3000);
        CHECK(e && e->conn_id == 23 && e->http_status == 400);
        oc_io_event_free(e);
        oc_ioloop_close(io, 23, wfd);
        tclient_close(&w);
    }

    oc_ioloop_stop(io);
    close(lfd);
    oc_tls_server_free(&srv);
    return failures;
}
