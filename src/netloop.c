/*
 * OpenChime network event loop. See netloop.h, tls.h, framebuf.h, protocol.h.
 */

#include "netloop.h"
#include "framebuf.h"
#include "protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define OC_NETLOOP_MAX_FD 4096
#define OC_CONN_OUT_CAP   256   /* WELCOME/REJECT are tiny; one pending response */

typedef enum { CONN_HANDSHAKE, CONN_ESTABLISHED } conn_state;

typedef struct {
    int          fd;
    oc_tls_conn  tls;
    oc_framebuf  fb;
    conn_state   state;
    int          did_hello;
    uint8_t      out[OC_CONN_OUT_CAP];
    size_t       out_len;   /* pending response length */
    size_t       out_sent;  /* bytes already written */
    uint32_t     events;    /* current epoll interest, to avoid redundant MODs */
} conn;

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static int set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    return (fl < 0) ? -1 : fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static void conn_set_events(int ep, conn *c, uint32_t events) {
    if (events == c->events) return;
    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    ev.events = events;
    ev.data.fd = c->fd;
    epoll_ctl(ep, EPOLL_CTL_MOD, c->fd, &ev);
    c->events = events;
}

static void conn_close(int ep, conn **conns, int fd) {
    conn *c = conns[fd];
    if (!c) return;
    epoll_ctl(ep, EPOLL_CTL_DEL, fd, NULL);
    oc_tls_conn_free(&c->tls);
    oc_framebuf_free(&c->fb);
    close(fd);
    free(c);
    conns[fd] = NULL;
}

/* Build the WELCOME (or REJECT) answer to a HELLO frame. Returns 0 to keep the
 * connection, -1 to close it (fatal REJECT or malformed). */
static int handle_hello(conn *c, oc_rbuf *payload) {
    oc_hello h;
    oc_wbuf w;
    if (oc_decode_hello(payload, &h) != OC_OK) {
        oc_reject rej = { OC_ERR_MALFORMED_FRAME, oc_slice_str("bad HELLO") };
        oc_wbuf_init(&w, c->out, sizeof c->out);
        oc_encode_reject(&w, &rej);
        c->out_len = w.len; c->out_sent = 0;
        return -1;
    }

    uint16_t chosen = 0, code = 0;
    if (oc_negotiate_version(h.min_version, h.max_version,
                             OC_PROTOCOL_VERSION, OC_PROTOCOL_VERSION,
                             &chosen, &code) != OC_OK) {
        oc_reject rej = { code, oc_slice_str("unsupported protocol version") };
        oc_wbuf_init(&w, c->out, sizeof c->out);
        oc_encode_reject(&w, &rej);
        c->out_len = w.len; c->out_sent = 0;
        return -1; /* REJECT is fatal */
    }

    oc_welcome wel = { chosen, now_ms() };
    oc_wbuf_init(&w, c->out, sizeof c->out);
    oc_encode_welcome(&w, &wel);
    c->out_len = w.len; c->out_sent = 0;
    return 0;
}

/* Dispatch every complete frame currently buffered. Returns 0 to keep the
 * connection or -1 to close it. */
static int drain_frames(conn *c) {
    const uint8_t *frame; size_t flen;
    for (;;) {
        int r = oc_framebuf_next(&c->fb, &frame, &flen);
        if (r == 0) return 0;              /* need more bytes */
        if (r < 0)  return -1;             /* malformed / too large: drop */

        oc_header hdr; oc_rbuf payload;
        if (oc_parse_frame(frame, flen, &hdr, &payload) != OC_OK) return -1;

        if (!c->did_hello) {
            if (hdr.msg_type != OC_MSG_HELLO) return -1; /* first frame must be HELLO */
            c->did_hello = 1;
            int keep = handle_hello(c, &payload);
            return keep; /* one response queued; -1 also closes after flushing */
        }
        /* Post-HELLO frames (AUTH, messaging) are not handled by the skeleton
         * yet — ignore them until the next milestone. */
    }
}

/* Try to flush any queued response. Returns 1 if fully flushed, 0 if it would
 * block (retry on EPOLLOUT), -1 on error. */
static int flush_out(conn *c) {
    while (c->out_sent < c->out_len) {
        size_t n = 0;
        oc_tls_status st = oc_tls_write(&c->tls, c->out + c->out_sent,
                                        c->out_len - c->out_sent, &n);
        if (st == OC_TLS_OK)         { c->out_sent += n; continue; }
        if (st == OC_TLS_WANT_WRITE) return 0;
        if (st == OC_TLS_WANT_READ)  return 0; /* rare: renegotiation */
        return -1;
    }
    c->out_len = c->out_sent = 0;
    return 1;
}

/* Drive an established connection's readable event: read, reassemble, dispatch.
 * Returns 0 to keep the connection, -1 to close it. */
static int on_readable(conn *c) {
    for (;;) {
        uint8_t chunk[OC_READ_CHUNK];
        size_t n = 0;
        oc_tls_status st = oc_tls_read(&c->tls, chunk, sizeof chunk, &n);
        if (st == OC_TLS_WANT_READ)  return 0;
        if (st == OC_TLS_WANT_WRITE) return 0;
        if (st == OC_TLS_CLOSED || st == OC_TLS_ERROR) return -1;
        /* OC_TLS_OK */
        if (oc_framebuf_push(&c->fb, chunk, n) != 0) return -1;
        int keep = drain_frames(c);
        if (keep < 0) {
            /* Flush a queued fatal REJECT best-effort, then close. */
            flush_out(c);
            return -1;
        }
        if (c->out_len > 0) return 0; /* a response is queued; go write it */
    }
}

/* Compute and apply the epoll interest a connection currently needs. */
static void update_interest(int ep, conn *c) {
    uint32_t ev = EPOLLIN;
    if (c->out_len > c->out_sent) ev |= EPOLLOUT;
    conn_set_events(ep, c, ev);
}

int oc_netloop_run(int port, oc_tls_server *tls, volatile sig_atomic_t *stop) {
    conn **conns = calloc(OC_NETLOOP_MAX_FD, sizeof *conns);
    if (!conns) return -1;

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) { free(conns); return -1; }
    int yes = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) < 0 ||
        listen(lfd, 128) < 0 || set_nonblock(lfd) < 0) {
        close(lfd); free(conns); return -1;
    }

    int ep = epoll_create1(0);
    if (ep < 0) { close(lfd); free(conns); return -1; }
    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    ev.events = EPOLLIN;
    ev.data.fd = lfd;
    epoll_ctl(ep, EPOLL_CTL_ADD, lfd, &ev);

    fprintf(stderr, "netloop: listening on :%d\n", port);

    struct epoll_event events[64];
    while (!*stop) {
        int nfds = epoll_wait(ep, events, 64, 500 /* ms: poll *stop */);
        if (nfds < 0) { if (errno == EINTR) continue; break; }

        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;

            if (fd == lfd) {
                for (;;) {
                    int cfd = accept(lfd, NULL, NULL);
                    if (cfd < 0) break; /* EAGAIN: drained */
                    if (cfd >= OC_NETLOOP_MAX_FD || set_nonblock(cfd) < 0) { close(cfd); continue; }
                    conn *c = calloc(1, sizeof *c);
                    if (!c || oc_framebuf_init(&c->fb) != 0 ||
                        oc_tls_conn_init(&c->tls, &tls->conf, cfd) != 0) {
                        if (c) { oc_framebuf_free(&c->fb); free(c); }
                        close(cfd);
                        continue;
                    }
                    c->fd = cfd;
                    c->state = CONN_HANDSHAKE;
                    conns[cfd] = c;
                    struct epoll_event cev;
                    memset(&cev, 0, sizeof cev);
                    cev.events = EPOLLIN;
                    cev.data.fd = cfd;
                    epoll_ctl(ep, EPOLL_CTL_ADD, cfd, &cev);
                    c->events = EPOLLIN;
                }
                continue;
            }

            conn *c = conns[fd];
            if (!c) continue;

            /* Flush a pending response first, if this was an EPOLLOUT wakeup. */
            if ((events[i].events & EPOLLOUT) && c->out_len > c->out_sent) {
                if (flush_out(c) < 0) { conn_close(ep, conns, fd); continue; }
            }

            if (c->state == CONN_HANDSHAKE) {
                oc_tls_status st = oc_tls_handshake(&c->tls);
                if (st == OC_TLS_OK)              c->state = CONN_ESTABLISHED;
                else if (st == OC_TLS_WANT_READ)  { conn_set_events(ep, c, EPOLLIN);  continue; }
                else if (st == OC_TLS_WANT_WRITE) { conn_set_events(ep, c, EPOLLOUT); continue; }
                else { conn_close(ep, conns, fd); continue; }
            }

            if (c->state == CONN_ESTABLISHED && (events[i].events & EPOLLIN)) {
                if (on_readable(c) < 0) {
                    if (c->out_len > c->out_sent) flush_out(c);
                    conn_close(ep, conns, fd);
                    continue;
                }
            }

            /* If a response finished draining, close a connection that had a
             * fatal REJECT queued is handled above; otherwise re-arm interest. */
            update_interest(ep, c);
        }
    }

    for (int fd = 0; fd < OC_NETLOOP_MAX_FD; fd++)
        if (conns[fd]) conn_close(ep, conns, fd);
    close(ep);
    close(lfd);
    free(conns);
    fprintf(stderr, "netloop: stopped\n");
    return 0;
}
