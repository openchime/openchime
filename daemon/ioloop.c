/* The daemon's I/O threads. See ioloop.h. */

#include "ioloop.h"
#include "framebuf.h"
#include "http.h"
#include "netloop.h"    /* oc_netloop_stats_note_read */
#include "protocol.h"

#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* One connection is read for at most this much per turn of its thread, and is
 * served again on the next, so one client streaming as fast as it can shares
 * the thread with the others (ARCH-22). */
#define IO_READ_BUDGET (256u * 1024u)
#define IO_MAX_FD      4096   /* as the event loop's table (netloop.c) */

typedef struct io_chunk { struct io_chunk *next; size_t len, off; uint8_t data[]; } io_chunk;

enum { S_PROXY, S_HOLD, S_HANDSHAKE, S_OPEN, S_DONE };

typedef struct {
    int         fd;
    uint64_t    conn_id;
    int         state;
    int         http;
    char        source[46];
    oc_tls_conn tls;
    oc_framebuf fb;
    io_chunk   *oh, *ot;         /* output, oldest first */
    size_t      queued;          /* bytes in the chunks, unwritten */
    uint64_t    written;         /* bytes written since the connection opened */
    int         above_soft;      /* queued was above OC_IO_SOFT at the last report */
    int         paused, close_after;
    uint32_t    events;
    size_t      rb_left;
    uint64_t    rb_turn;
    int         resume;          /* on the resume list */
    int         plain;           /* no TLS: the health port's (ARCH-25) */
    char       *hin;             /* HTTP: the request so far */
    size_t      hlen, hcap;
    const oc_http_route *route;  /* HTTP: its route, once the head is read */
    size_t      need;            /* HTTP: head + body, once the head is read */
    uint64_t    deadline;        /* HTTP: when an unfinished request is answered 408 */
    int         http_done;       /* HTTP: answered, or reported to the loop */
    oc_io_event *closed_ev;      /* CLOSED, made at adoption so reporting it cannot fail */
} io_conn;

enum { C_ADOPT, C_ADOPT_PLAIN, C_SEND, C_PAUSE, C_PROCEED, C_CLOSE };
typedef struct io_cmd {
    int      kind;
    uint64_t conn_id;
    int      fd;
    int      flag;               /* ADOPT: via_proxy; SEND: close_after; PAUSE: paused */
    char     source[46];
    size_t   len;
    struct io_cmd *next;
    uint8_t  data[];
} io_cmd;

typedef struct {
    struct oc_ioloop *io;
    pthread_t   th;
    int         started;
    int         ep, wake;
    pthread_mutex_t mu;
    io_cmd     *ch, *ct;
    io_conn   **conns;           /* by descriptor */
    uint8_t    *orphan;          /* by descriptor: adopted without memory for it; closed on CLOSE */
    uint64_t   *resume;          /* conn ids whose budget ran out */
    size_t      nresume;
    uint64_t    turn;
    uint64_t    next_sweep;      /* when unfinished HTTP requests are next checked */
    int         stop;
} io_thread;

struct oc_ioloop {
    int         n;
    io_thread  *t;
    oc_tls_server *tls;
    const oc_trusted_proxies *trusted;
    const oc_http_site *tls_site, *plain_site;
    pthread_mutex_t emu;
    oc_io_event *eh, *et;
    int         efd;
};

static uint64_t g_http_timeout_ms;   /* 0: OC_HTTP_REQUEST_TIMEOUT_MS */

void oc_ioloop_set_http_timeout_ms(uint64_t ms) {
    __atomic_store_n(&g_http_timeout_ms, ms, __ATOMIC_RELAXED);
}

static uint64_t http_timeout_ms(void) {
    uint64_t ms = __atomic_load_n(&g_http_timeout_ms, __ATOMIC_RELAXED);
    return ms ? ms : OC_HTTP_REQUEST_TIMEOUT_MS;
}

static uint64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* --- events to the loop ----------------------------------------------------- */

static oc_io_event *ev_new(oc_io_kind kind, const io_conn *c, const uint8_t *data, size_t len) {
    oc_io_event *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    e->kind = kind;
    e->conn_id = c->conn_id;
    e->fd = c->fd;
    if (len) {
        e->data = malloc(len);
        if (!e->data) { free(e); return NULL; }
        memcpy(e->data, data, len);
        e->len = len;
    }
    return e;
}

static void ev_post(oc_ioloop *io, oc_io_event *e) {
    if (!e) return;
    pthread_mutex_lock(&io->emu);
    if (io->et) io->et->next = e; else io->eh = e;
    io->et = e;
    pthread_mutex_unlock(&io->emu);
    uint64_t one = 1;
    ssize_t w = write(io->efd, &one, sizeof one); (void)w;
}

int oc_ioloop_eventfd(const oc_ioloop *io) { return io->efd; }

oc_io_event *oc_ioloop_next(oc_ioloop *io) {
    pthread_mutex_lock(&io->emu);
    oc_io_event *e = io->eh;
    if (e) { io->eh = e->next; if (!io->eh) io->et = NULL; e->next = NULL; }
    pthread_mutex_unlock(&io->emu);
    return e;
}

void oc_io_event_free(oc_io_event *e) {
    if (!e) return;
    free(e->data);
    free(e);
}

/* --- commands from the loop ------------------------------------------------- */

static io_thread *thread_of(oc_ioloop *io, uint64_t conn_id) {
    return &io->t[conn_id % (uint64_t)io->n];
}

static int cmd_post(oc_ioloop *io, io_cmd *c) {
    io_thread *t = thread_of(io, c->conn_id);
    pthread_mutex_lock(&t->mu);
    if (t->ct) t->ct->next = c; else t->ch = c;
    t->ct = c;
    pthread_mutex_unlock(&t->mu);
    uint64_t one = 1;
    ssize_t w = write(t->wake, &one, sizeof one); (void)w;
    return 0;
}

static io_cmd *cmd_new(int kind, uint64_t conn_id, int fd, size_t len) {
    io_cmd *c = calloc(1, sizeof *c + len);
    if (!c) return NULL;
    c->kind = kind; c->conn_id = conn_id; c->fd = fd; c->len = len;
    return c;
}

int oc_ioloop_adopt(oc_ioloop *io, int fd, uint64_t conn_id, const char *source, int via_proxy) {
    io_cmd *c = cmd_new(C_ADOPT, conn_id, fd, 0);
    if (!c) return -1;
    c->flag = via_proxy;
    snprintf(c->source, sizeof c->source, "%s", source ? source : "");
    return cmd_post(io, c);
}

int oc_ioloop_adopt_plain(oc_ioloop *io, int fd, uint64_t conn_id, const char *source) {
    io_cmd *c = cmd_new(C_ADOPT_PLAIN, conn_id, fd, 0);
    if (!c) return -1;
    snprintf(c->source, sizeof c->source, "%s", source ? source : "");
    return cmd_post(io, c);
}

int oc_ioloop_send(oc_ioloop *io, uint64_t conn_id, int fd, const uint8_t *buf, size_t len,
                   int close_after) {
    io_cmd *c = cmd_new(C_SEND, conn_id, fd, len);
    if (!c) return -1;
    if (len) memcpy(c->data, buf, len);
    c->flag = close_after;
    return cmd_post(io, c);
}

void oc_ioloop_pause(oc_ioloop *io, uint64_t conn_id, int fd, int paused) {
    io_cmd *c = cmd_new(C_PAUSE, conn_id, fd, 0);
    if (!c) return;
    c->flag = paused;
    cmd_post(io, c);
}

void oc_ioloop_proceed(oc_ioloop *io, uint64_t conn_id, int fd) {
    io_cmd *c = cmd_new(C_PROCEED, conn_id, fd, 0);
    if (c) cmd_post(io, c);
}

void oc_ioloop_close(oc_ioloop *io, uint64_t conn_id, int fd) {
    io_cmd *c = cmd_new(C_CLOSE, conn_id, fd, 0);
    if (!c) return;   /* cannot happen short of exhaustion; stop() closes what is left */
    cmd_post(io, c);
}

/* --- one connection --------------------------------------------------------- */

static void set_events(io_thread *t, io_conn *c) {
    uint32_t ev = 0;
    if (c->state != S_DONE && c->state != S_HOLD) {
        if (!c->paused || c->state != S_OPEN) ev |= EPOLLIN;
        if (c->queued) ev |= EPOLLOUT;
    }
    if (ev == c->events) return;
    struct epoll_event e; memset(&e, 0, sizeof e);
    e.events = ev; e.data.fd = c->fd;
    epoll_ctl(t->ep, c->events ? (ev ? EPOLL_CTL_MOD : EPOLL_CTL_DEL) : EPOLL_CTL_ADD, c->fd, &e);
    c->events = ev;
}

/* The connection is over from this side: stop watching it, say so once. The
 * socket stays open until the loop's CLOSE (ioloop.h). */
static void finish(io_thread *t, io_conn *c) {
    if (c->state == S_DONE) return;
    c->state = S_DONE;
    set_events(t, c);
    ev_post(t->io, c->closed_ev);
    c->closed_ev = NULL;
}

/* Report `e`, or finish the connection when it could not be made: an event
 * lost would be a frame missing from the middle of the stream. */
static int post_or_finish(io_thread *t, io_conn *c, oc_io_event *e) {
    if (!e) { finish(t, c); return -1; }
    ev_post(t->io, e);
    return 0;
}

/* The connection's bytes, through TLS or, on the health port, straight off the
 * socket -- in TLS's terms either way, so the callers need not care which. */
static oc_tls_status conn_read(io_conn *c, uint8_t *buf, size_t cap, size_t *n) {
    if (!c->plain) return oc_tls_read(&c->tls, buf, cap, n);
    *n = 0;
    ssize_t r = recv(c->fd, buf, cap, 0);
    if (r > 0) { *n = (size_t)r; return OC_TLS_OK; }
    if (r == 0) return OC_TLS_CLOSED;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return OC_TLS_WANT_READ;
    return OC_TLS_ERROR;
}

static oc_tls_status conn_write(io_conn *c, const uint8_t *buf, size_t len, size_t *n) {
    if (!c->plain) return oc_tls_write(&c->tls, buf, len, n);
    *n = 0;
    ssize_t r = send(c->fd, buf, len, MSG_NOSIGNAL);
    if (r >= 0) { *n = (size_t)r; return OC_TLS_OK; }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return OC_TLS_WANT_WRITE;
    return OC_TLS_ERROR;
}

static void flush(io_thread *t, io_conn *c);   /* below */

/* Answer an HTTP connection here and finish it once the answer is written: a
 * route that touches no state, or a refusal. Nothing more is read from it. */
static void http_answer(io_thread *t, io_conn *c, int status, const char *ctype,
                        const char *body, size_t blen) {
    c->http_done = 1;
    c->deadline = 0;
    free(c->hin); c->hin = NULL; c->hlen = c->hcap = 0;
    char head[OC_HTTP_HEAD_MAX];
    size_t hl = oc_http_head(head, sizeof head, status, ctype, blen);
    io_chunk *k = hl ? malloc(sizeof *k + hl + blen) : NULL;
    if (!k) { finish(t, c); return; }
    k->next = NULL; k->len = hl + blen; k->off = 0;
    memcpy(k->data, head, hl);
    if (blen) memcpy(k->data + hl, body, blen);
    if (c->ot) c->ot->next = k; else c->oh = k;
    c->ot = k;
    c->queued += k->len;
    c->close_after = 1;
    flush(t, c);
}

static void http_refuse(io_thread *t, io_conn *c, int status) {
    size_t blen = 0;
    const char *b = oc_http_error_body(status, &blen);
    http_answer(t, c, status, "text/plain", b, blen);
}

/* An HTTP connection's plaintext (ARCH-32): gathered until the head is whole,
 * then routed against its listener's site -- refused at once when no route
 * takes it or its declared body is over the route's limit, so nothing more is
 * buffered for it -- then gathered to the end of its body. A static route is
 * answered here; a loop route is reported as the parts the loop dispatches on.
 * What the peer sends after its request is not read. */
static void http_bytes(io_thread *t, io_conn *c, const uint8_t *chunk, size_t n) {
    if (c->http_done) return;
    if (c->route && n > c->need - c->hlen) n = c->need - c->hlen;
    if (c->hlen + n > c->hcap) {
        size_t nc = c->hcap ? c->hcap : 4096;
        while (nc < c->hlen + n) nc *= 2;
        char *g = realloc(c->hin, nc);
        if (!g) { finish(t, c); return; }
        c->hin = g; c->hcap = nc;
    }
    memcpy(c->hin + c->hlen, chunk, n);
    c->hlen += n;

    oc_http_req req;
    if (!c->route) {
        int pr = oc_http_parse_head(c->hin, c->hlen, &req);
        if (pr == 0) return;                          /* not all here yet */
        if (pr < 0) { http_refuse(t, c, 400); return; }
        int status = 404;
        const oc_http_route *rt = oc_http_route_find(c->plain ? t->io->plain_site : t->io->tls_site,
                                                     &req, &status);
        if (!rt) { http_refuse(t, c, status); return; }
        if (req.content_length > rt->max_body) { http_refuse(t, c, 413); return; }
        c->route = rt;
        c->need = req.head_len + req.content_length;
    }
    if (c->hlen < c->need) return;                    /* the body is still coming */

    const oc_http_route *rt = c->route;
    if (oc_http_parse(c->hin, c->hlen, rt->max_body, &req) != 1) { http_refuse(t, c, 400); return; }
    if (rt->kind == OC_HTTP_STATIC) { http_answer(t, c, 200, rt->ctype, rt->body, rt->body_len); return; }

    c->http_done = 1;
    c->deadline = 0;
    size_t len = req.method_len + req.path_len + req.body_len;
    oc_io_event *e = calloc(1, sizeof *e);
    if (e && len && !(e->data = malloc(len))) { free(e); e = NULL; }
    if (e) {
        e->kind = OC_IO_HTTP_REQ; e->conn_id = c->conn_id; e->fd = c->fd;
        memcpy(e->data, req.method, req.method_len);
        memcpy(e->data + req.method_len, req.path, req.path_len);
        if (req.body_len) memcpy(e->data + req.method_len + req.path_len, req.body, req.body_len);
        e->len = len;
        e->method_len = req.method_len; e->path_len = req.path_len; e->body_len = req.body_len;
        e->is_json = req.is_json;
    }
    free(c->hin); c->hin = NULL; c->hlen = c->hcap = 0;
    post_or_finish(t, c, e);
}

static void report_written(io_thread *t, io_conn *c) {
    oc_io_event *e = ev_new(OC_IO_WRITTEN, c, NULL, 0);
    if (e) e->written = c->written;
    ev_post(t->io, e);
}

/* Write what is queued, as far as the socket takes it. */
static void flush(io_thread *t, io_conn *c) {
    if (c->state != S_OPEN) return;
    size_t before = c->queued;
    while (c->oh) {
        io_chunk *k = c->oh;
        size_t n = 0;
        oc_tls_status st = conn_write(c, k->data + k->off, k->len - k->off, &n);
        if (st == OC_TLS_WANT_WRITE || st == OC_TLS_WANT_READ) break;
        if (st != OC_TLS_OK) { finish(t, c); return; }
        k->off += n; c->queued -= n; c->written += n;
        if (k->off == k->len) { c->oh = k->next; if (!c->oh) c->ot = NULL; free(k); }
    }
    if (c->queued != before) {
        int crossed = c->above_soft && c->queued <= OC_IO_SOFT;
        if (crossed || !c->queued) report_written(t, c);
        c->above_soft = c->queued > OC_IO_SOFT;
    }
    if (!c->queued && c->close_after) finish(t, c);
}

/* Read what is there, within the turn's budget: frames to the loop, or an HTTP
 * connection's plaintext as it comes. */
static void read_some(io_thread *t, io_conn *c) {
    if (c->rb_turn != t->turn) { c->rb_turn = t->turn; c->rb_left = IO_READ_BUDGET; }
    uint64_t got_turn = 0;
    while (c->state == S_OPEN && !c->paused) {
        if (!c->rb_left) {
            if (!c->resume && t->nresume < IO_MAX_FD) { c->resume = 1; t->resume[t->nresume++] = c->conn_id; }
            break;
        }
        uint8_t chunk[OC_READ_CHUNK];
        size_t n = 0;
        oc_tls_status st = conn_read(c, chunk, sizeof chunk, &n);
        if (st == OC_TLS_WANT_READ || st == OC_TLS_WANT_WRITE) break;
        if (st != OC_TLS_OK || !n) { finish(t, c); break; }
        got_turn += n;
        c->rb_left = n < c->rb_left ? c->rb_left - n : 0;
        if (c->http) { http_bytes(t, c, chunk, n); continue; }
        if (oc_framebuf_push(&c->fb, chunk, n) != 0) { finish(t, c); break; }
        const uint8_t *frame; size_t flen; int r;
        while ((r = oc_framebuf_next(&c->fb, &frame, &flen)) == 1)
            if (post_or_finish(t, c, ev_new(OC_IO_FRAME, c, frame, flen)) != 0) break;
        if (r < 0) { finish(t, c); break; }
    }
    if (got_turn) oc_netloop_stats_note_read(got_turn, c->conn_id);
}

static void service(io_thread *t, io_conn *c, uint32_t events) {
    if (c->state == S_DONE) return;
    if (c->state == S_PROXY) {
        /* Peek, so nothing is taken from the socket until the whole header is
         * there; then take exactly the header, and TLS starts on the byte after
         * it. A trusted peer that sends anything else is misconfigured, and
         * guessing would mean either trusting a header that is not one or
         * feeding one to TLS. */
        uint8_t hdr[OC_PROXY_V2_MAX];
        ssize_t got = recv(c->fd, hdr, sizeof hdr, MSG_PEEK);
        if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return;
        if (got <= 0) { finish(t, c); return; }
        char real[46];
        long hl = oc_proxy_v2_parse(hdr, (size_t)got, real);
        if (hl == 0) return;                          /* not all here yet */
        if (hl < 0 || recv(c->fd, hdr, (size_t)hl, 0) != hl) { finish(t, c); return; }
        if (real[0]) snprintf(c->source, sizeof c->source, "%s", real);
        /* The loop counts connections by the client the header names, and may
         * refuse this one: wait for its word before spending a handshake. */
        c->state = S_HOLD;
        oc_io_event *e = ev_new(OC_IO_SOURCE, c, NULL, 0);
        if (e) memcpy(e->source, c->source, sizeof e->source);
        ev_post(t->io, e);
        set_events(t, c);
        return;
    }
    if (c->state == S_HOLD) return;
    if (c->state == S_HANDSHAKE) {
        oc_tls_status st = oc_tls_handshake(&c->tls);
        if (st == OC_TLS_WANT_READ || st == OC_TLS_WANT_WRITE) {
            uint32_t want = st == OC_TLS_WANT_READ ? EPOLLIN : EPOLLOUT;
            if (want != c->events) {
                struct epoll_event e; memset(&e, 0, sizeof e);
                e.events = want; e.data.fd = c->fd;
                epoll_ctl(t->ep, EPOLL_CTL_MOD, c->fd, &e);
                c->events = want;
            }
            return;
        }
        if (st != OC_TLS_OK) { finish(t, c); return; }
        const char *alpn = oc_tls_alpn_selected(&c->tls);
        /* An ACME validation (RFC 8737) is the handshake and nothing after it:
         * the CA has seen the challenge certificate, and the connection ends
         * here, never reaching the loop as a client of either kind. */
        if (alpn && strcmp(alpn, OC_TLS_ALPN_ACME) == 0) { finish(t, c); return; }
        c->state = S_OPEN;
        /* ALPN demux (ARCH-54): a peer that did not negotiate oc/1 is an HTTP
         * client -- a webhook sender (ARCH-32). */
        c->http = !alpn || strcmp(alpn, OC_ALPN_PROTO) != 0;
        if (c->http) c->deadline = mono_ms() + http_timeout_ms();
        oc_io_event *e = ev_new(OC_IO_OPENED, c, NULL, 0);
        if (e) { e->http = c->http; memcpy(e->source, c->source, sizeof e->source); }
        ev_post(t->io, e);
        events |= EPOLLIN;   /* mbedTLS may already hold application data */
    }
    if (c->state == S_OPEN && (events & (EPOLLIN | EPOLLHUP | EPOLLERR))) read_some(t, c);
    if (c->state == S_OPEN) flush(t, c);
    set_events(t, c);
}

static void conn_free(io_thread *t, io_conn *c) {
    if (c->events) epoll_ctl(t->ep, EPOLL_CTL_DEL, c->fd, NULL);
    for (io_chunk *k = c->oh, *n; k; k = n) { n = k->next; free(k); }
    if (!c->plain) oc_tls_conn_free(&c->tls);
    oc_framebuf_free(&c->fb);
    free(c->hin);
    oc_io_event_free(c->closed_ev);
    close(c->fd);
    t->conns[c->fd] = NULL;
    free(c);
}

static void apply(io_thread *t, io_cmd *m) {
    if (m->fd < 0 || m->fd >= IO_MAX_FD) return;
    io_conn *c = t->conns[m->fd];
    if (m->kind == C_ADOPT || m->kind == C_ADOPT_PLAIN) {
        if (c) return;   /* cannot happen: a descriptor is adopted once until closed */
        c = calloc(1, sizeof *c);
        if (c) { c->fd = m->fd; c->conn_id = m->conn_id; }
        if (!c || !(c->closed_ev = ev_new(OC_IO_CLOSED, c, NULL, 0))) {
            /* Without memory for the connection the loop is still told it is
             * over, and closes the socket as it closes every other. */
            free(c);
            t->orphan[m->fd] = 1;
            oc_io_event *e = calloc(1, sizeof *e);
            if (e) { e->kind = OC_IO_CLOSED; e->conn_id = m->conn_id; e->fd = m->fd; }
            ev_post(t->io, e);
            return;
        }
        snprintf(c->source, sizeof c->source, "%s", m->source);
        c->plain = m->kind == C_ADOPT_PLAIN;
        c->state = c->plain ? S_OPEN : m->flag ? S_PROXY : S_HANDSHAKE;
        t->conns[m->fd] = c;
        int one = 1;
        setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        if (oc_framebuf_init(&c->fb) != 0 ||
            (!c->plain && oc_tls_conn_init(&c->tls, &t->io->tls->conf, c->fd) != 0)) {
            finish(t, c);
            return;
        }
        if (c->plain) {
            /* Nothing to negotiate: open, and HTTP, from the first byte. */
            c->http = 1;
            c->deadline = mono_ms() + http_timeout_ms();
            oc_io_event *e = ev_new(OC_IO_OPENED, c, NULL, 0);
            if (e) { e->http = 1; memcpy(e->source, c->source, sizeof e->source); }
            ev_post(t->io, e);
        }
        set_events(t, c);
        return;
    }
    if (!c && m->kind == C_CLOSE && t->orphan[m->fd]) { t->orphan[m->fd] = 0; close(m->fd); return; }
    if (!c || c->conn_id != m->conn_id) return;   /* already closed: a stale command */
    if (m->kind == C_SEND) {
        if (c->state == S_DONE) return;
        if (m->len) {
            io_chunk *k = malloc(sizeof *k + m->len);
            if (!k) { finish(t, c); return; }
            k->next = NULL; k->len = m->len; k->off = 0;
            memcpy(k->data, m->data, m->len);
            if (c->ot) c->ot->next = k; else c->oh = k;
            c->ot = k;
            c->queued += m->len;
            if (c->queued > OC_IO_SOFT) c->above_soft = 1;
        }
        if (m->flag) c->close_after = 1;
        flush(t, c);
        set_events(t, c);
    } else if (m->kind == C_PAUSE) {
        int was = c->paused;
        c->paused = m->flag;
        if (was && !c->paused && c->state == S_OPEN && !c->resume && t->nresume < IO_MAX_FD) {
            /* Decrypted bytes may be waiting inside TLS, where epoll cannot see them. */
            c->resume = 1;
            t->resume[t->nresume++] = c->conn_id;
        }
        set_events(t, c);
    } else if (m->kind == C_PROCEED) {
        if (c->state != S_HOLD) return;
        c->state = S_HANDSHAKE;
        set_events(t, c);
        service(t, c, EPOLLIN);
    } else if (m->kind == C_CLOSE) {
        flush(t, c);
        conn_free(t, c);
    }
}

static void drain_commands(io_thread *t) {
    uint64_t cnt;
    while (read(t->wake, &cnt, sizeof cnt) > 0) {}
    pthread_mutex_lock(&t->mu);
    io_cmd *m = t->ch;
    t->ch = t->ct = NULL;
    pthread_mutex_unlock(&t->mu);
    while (m) {
        io_cmd *n = m->next;
        apply(t, m);
        free(m);
        m = n;
    }
}

static io_conn *find_conn(io_thread *t, uint64_t conn_id) {
    /* The resume list is short and rare; a walk is fine. */
    for (int fd = 0; fd < IO_MAX_FD; fd++)
        if (t->conns[fd] && t->conns[fd]->conn_id == conn_id) return t->conns[fd];
    return NULL;
}

/* An HTTP request not whole by its deadline is answered 408: a peer that
 * connects and sends nothing, or dribbles, must not hold a connection. */
static void sweep_http(io_thread *t) {
    uint64_t now = mono_ms();
    if (now < t->next_sweep) return;
    t->next_sweep = now + 250;
    for (int fd = 0; fd < IO_MAX_FD; fd++) {
        io_conn *c = t->conns[fd];
        if (!c || !c->http || c->http_done || c->state != S_OPEN || !c->deadline || now < c->deadline)
            continue;
        http_refuse(t, c, 408);
        set_events(t, c);
    }
}

static void *io_main(void *p) {
    io_thread *t = p;
    struct epoll_event evs[64];
    while (!__atomic_load_n(&t->stop, __ATOMIC_ACQUIRE)) {
        int n = epoll_wait(t->ep, evs, 64, t->nresume ? 0 : 500);
        t->turn++;
        size_t nr = t->nresume;
        uint64_t *ids = NULL;
        if (nr && (ids = malloc(nr * sizeof *ids)) != NULL) {
            memcpy(ids, t->resume, nr * sizeof *ids);
            t->nresume = 0;
        } else {
            nr = 0;
        }
        for (int i = 0; i < n; i++) {
            int fd = evs[i].data.fd;
            if (fd == t->wake) { drain_commands(t); continue; }
            if (fd < 0 || fd >= IO_MAX_FD || !t->conns[fd]) continue;
            service(t, t->conns[fd], evs[i].events);
        }
        for (size_t k = 0; k < nr; k++) {
            io_conn *c = find_conn(t, ids[k]);
            if (!c) continue;
            c->resume = 0;
            service(t, c, EPOLLIN);
        }
        free(ids);
        sweep_http(t);
    }
    return NULL;
}

oc_ioloop *oc_ioloop_start(int nthreads, oc_tls_server *tls, const oc_trusted_proxies *trusted,
                           const oc_http_site *tls_site, const oc_http_site *plain_site) {
    if (nthreads < 1) nthreads = 1;
    oc_ioloop *io = calloc(1, sizeof *io);
    if (!io) return NULL;
    io->n = nthreads;
    io->tls = tls;
    io->trusted = trusted;
    io->tls_site = tls_site;
    io->plain_site = plain_site;
    pthread_mutex_init(&io->emu, NULL);
    io->efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    io->t = calloc((size_t)nthreads, sizeof *io->t);
    if (io->efd < 0 || !io->t) { oc_ioloop_stop(io); return NULL; }
    for (int i = 0; i < nthreads; i++) { io->t[i].ep = -1; io->t[i].wake = -1; }
    for (int i = 0; i < nthreads; i++) {
        io_thread *t = &io->t[i];
        t->io = io;
        pthread_mutex_init(&t->mu, NULL);
        t->ep = epoll_create1(EPOLL_CLOEXEC);
        t->wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        t->conns = calloc(IO_MAX_FD, sizeof *t->conns);
        t->resume = calloc(IO_MAX_FD, sizeof *t->resume);
        t->orphan = calloc(IO_MAX_FD, 1);
        if (t->ep < 0 || t->wake < 0 || !t->conns || !t->resume || !t->orphan) { oc_ioloop_stop(io); return NULL; }
        struct epoll_event e; memset(&e, 0, sizeof e);
        e.events = EPOLLIN; e.data.fd = t->wake;
        epoll_ctl(t->ep, EPOLL_CTL_ADD, t->wake, &e);
        if (pthread_create(&t->th, NULL, io_main, t) != 0) { oc_ioloop_stop(io); return NULL; }
        t->started = 1;
    }
    return io;
}

void oc_ioloop_stop(oc_ioloop *io) {
    if (!io) return;
    for (int i = 0; io->t && i < io->n; i++) {
        io_thread *t = &io->t[i];
        if (t->started) {
            __atomic_store_n(&t->stop, 1, __ATOMIC_RELEASE);
            uint64_t one = 1;
            ssize_t w = write(t->wake, &one, sizeof one); (void)w;
            pthread_join(t->th, NULL);
        }
        for (io_cmd *m = t->ch, *n; m; m = n) {
            n = m->next;
            if ((m->kind == C_ADOPT || m->kind == C_ADOPT_PLAIN) && (!t->conns || !t->conns[m->fd]))
                close(m->fd);
            free(m);
        }
        for (int fd = 0; t->conns && fd < IO_MAX_FD; fd++)
            if (t->conns[fd]) conn_free(t, t->conns[fd]);
        free(t->conns);
        free(t->resume);
        free(t->orphan);
        if (t->ep >= 0) close(t->ep);
        if (t->wake >= 0) close(t->wake);
        if (t->io) pthread_mutex_destroy(&t->mu);
    }
    for (oc_io_event *e = io->eh, *n; e; e = n) { n = e->next; oc_io_event_free(e); }
    if (io->efd >= 0) close(io->efd);
    pthread_mutex_destroy(&io->emu);
    free(io->t);
    free(io);
}
