/*
 * OpenChime client — network thread. See net.h.
 */

#include "net.h"
#include "event.h"

#include "protocol.h"
#include "tls.h"
#include "framebuf.h"
#include "sock.h"       /* POSIX/Winsock shim (also pulls in getaddrinfo) */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct oc_net {
    pthread_t     thread;
    volatile int  stop;
    char          host[256];
    int           port;
    char         *token;
    oc_queue     *to_ui;
    oc_queue     *from_ui;
};

/* ---- helpers ---- */

static void push_err(oc_queue *to_ui, const char *msg) {
    oc_ev *e = oc_ev_new(OC_EV_ERROR);
    if (e) { e->body = strdup(msg); oc_queue_push(to_ui, e); }
}

static void push_simple(oc_queue *to_ui, int type, uint64_t user_id) {
    oc_ev *e = oc_ev_new(type);
    if (e) { e->user_id = user_id; oc_queue_push(to_ui, e); }
}

static void gen_idem(uint8_t out[OC_IDEM_SIZE]) {
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) { size_t r = fread(out, 1, OC_IDEM_SIZE, f); (void)r; fclose(f); }
    else   { for (int i = 0; i < (int)OC_IDEM_SIZE; i++) out[i] = (uint8_t)rand(); }
}

static int dial(const char *host, int port) {
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
    return fd;
}

static void wait_io(int fd, oc_tls_status st, int timeout_ms) {
    oc_poll(fd, st == OC_TLS_WANT_WRITE, timeout_ms);
}

/* Drive a TLS handshake to completion. Returns 0 on success, -1 on error. */
static int do_handshake(oc_tls_conn *c, int fd, volatile int *stop) {
    for (;;) {
        if (*stop) return -1;
        oc_tls_status st = oc_tls_handshake(c);
        if (st == OC_TLS_OK) return 0;
        if (st == OC_TLS_WANT_READ || st == OC_TLS_WANT_WRITE) { wait_io(fd, st, 500); continue; }
        return -1;
    }
}

/* Write all bytes, waiting on the socket as needed. 0 ok, -1 error. */
static int write_all(oc_tls_conn *c, int fd, const uint8_t *buf, size_t len, volatile int *stop) {
    size_t sent = 0;
    while (sent < len) {
        if (*stop) return -1;
        size_t n = 0;
        oc_tls_status st = oc_tls_write(c, buf + sent, len - sent, &n);
        if (st == OC_TLS_OK) { sent += n; continue; }
        if (st == OC_TLS_WANT_READ || st == OC_TLS_WANT_WRITE) { wait_io(fd, st, 500); continue; }
        return -1;
    }
    return 0;
}

/* Block until one full frame is read (used only during handshake/auth). */
static int read_one(oc_tls_conn *c, int fd, oc_framebuf *fb, oc_header *hdr,
                    oc_rbuf *payload, volatile int *stop) {
    for (;;) {
        if (*stop) return -1;
        const uint8_t *frame; size_t flen;
        int r = oc_framebuf_next(fb, &frame, &flen);
        if (r == 1) return oc_parse_frame(frame, flen, hdr, payload) == OC_OK ? 0 : -1;
        if (r < 0) return -1;
        uint8_t buf[4096]; size_t n = 0;
        oc_tls_status st = oc_tls_read(c, buf, sizeof buf, &n);
        if (st == OC_TLS_OK) { if (oc_framebuf_push(fb, buf, n) != 0) return -1; continue; }
        if (st == OC_TLS_WANT_READ || st == OC_TLS_WANT_WRITE) { wait_io(fd, st, 500); continue; }
        return -1;
    }
}

/* Dispatch every buffered server frame into UI events. Returns 0 to keep the
 * connection, -1 to drop it. */
static int dispatch(oc_framebuf *fb, oc_queue *to_ui) {
    for (;;) {
        const uint8_t *frame; size_t flen;
        int r = oc_framebuf_next(fb, &frame, &flen);
        if (r == 0) return 0;
        if (r < 0)  return -1;
        oc_header hdr; oc_rbuf p;
        if (oc_parse_frame(frame, flen, &hdr, &p) != OC_OK) return -1;

        if (hdr.msg_type == OC_MSG_BROADCAST) {
            oc_broadcast b;
            if (oc_decode_broadcast(&p, &b) != OC_OK) return -1;
            oc_ev *e = oc_ev_new(OC_EV_MESSAGE);
            if (e) {
                e->channel_id = b.channel_id;
                e->author_id = b.author_id;
                e->message_id = b.message_id;
                e->server_time = b.server_time;
                e->body = malloc(b.body.len + 1);
                if (e->body) { memcpy(e->body, b.body.ptr, b.body.len); e->body[b.body.len] = '\0'; }
                oc_queue_push(to_ui, e);
            }
        } else if (hdr.msg_type == OC_MSG_ERROR) {
            oc_error err;
            if (oc_decode_error(&p, &err) == OC_OK) {
                char msg[256];
                size_t n = err.message.len < sizeof msg - 1 ? err.message.len : sizeof msg - 1;
                memcpy(msg, err.message.ptr, n); msg[n] = '\0';
                push_err(to_ui, msg[0] ? msg : "server error");
                if (err.fatal) return -1;
            }
        }
        /* SEND_ACK and others are ignored in the Phase 1 skeleton. */
    }
}

/* ---- the thread ---- */

static void *net_thread(void *arg) {
    oc_net *n = (oc_net *)arg;

    int fd = dial(n->host, n->port);
    if (fd < 0) { push_err(n->to_ui, "could not connect"); push_simple(n->to_ui, OC_EV_DISCONNECTED, 0); return NULL; }

    oc_tls_client cli;
    oc_tls_conn conn;
    oc_framebuf fb;
    /* Phase 1 TOFU: trust the presented cert (pin=NULL); persisting/pinning the
     * fingerprint arrives with the client store phase. */
    if (oc_tls_client_init(&cli, NULL) != 0 || oc_tls_conn_init(&conn, &cli.conf, fd) != 0) {
        oc_closesock(fd); push_err(n->to_ui, "tls init failed"); push_simple(n->to_ui, OC_EV_DISCONNECTED, 0); return NULL;
    }
    oc_framebuf_init(&fb);

    if (do_handshake(&conn, fd, &n->stop) != 0) goto drop;

    /* HELLO -> WELCOME */
    {
        uint8_t buf[128]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
        oc_hello h = { OC_PROTOCOL_VERSION, OC_PROTOCOL_VERSION, oc_slice_str("openchime-client/0.1") };
        if (oc_encode_hello(&w, &h) != OC_OK || write_all(&conn, fd, buf, w.len, &n->stop) != 0) goto drop;
        oc_header hdr; oc_rbuf p;
        if (read_one(&conn, fd, &fb, &hdr, &p, &n->stop) != 0) goto drop;
        if (hdr.msg_type == OC_MSG_REJECT) { push_err(n->to_ui, "version rejected"); goto drop; }
        if (hdr.msg_type != OC_MSG_WELCOME) goto drop;
    }

    /* Stub AUTH -> AUTH_OK (matches today's stubbed daemon; the real
     * AUTH_CHALLENGE/method flow lands with the auth-core milestone). */
    {
        uint8_t buf[512]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
        oc_auth a = { oc_slice_str(n->token ? n->token : "client") };
        if (oc_encode_auth(&w, OC_PROTOCOL_VERSION, &a) != OC_OK || write_all(&conn, fd, buf, w.len, &n->stop) != 0) goto drop;
        oc_header hdr; oc_rbuf p;
        if (read_one(&conn, fd, &fb, &hdr, &p, &n->stop) != 0) goto drop;
        if (hdr.msg_type != OC_MSG_AUTH_OK) { push_err(n->to_ui, "auth failed"); goto drop; }
        oc_auth_ok ok;
        oc_decode_auth_ok(&p, &ok);
        push_simple(n->to_ui, OC_EV_CONNECTED, ok.user_id);
        push_simple(n->to_ui, OC_EV_AUTH_OK, ok.user_id);
    }

    /* Serve: interleave reading server frames with sending queued user actions. */
    while (!n->stop) {
        oc_cmd *c;
        while ((c = oc_queue_try_pop(n->from_ui)) != NULL) {
            if (c->type == OC_CMD_QUIT) { oc_cmd_free(c); goto drop; }
            if (c->type == OC_CMD_SEND && c->body) {
                uint8_t buf[OC_MAX_FRAME_SIZE]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                oc_send s;
                s.channel_id = c->channel_id ? c->channel_id : 1;
                gen_idem(s.idem);
                s.body = oc_slice_str(c->body);
                if (oc_encode_send(&w, OC_PROTOCOL_VERSION, &s) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            oc_cmd_free(c);
        }

        if (oc_poll(fd, 0, 50) > 0) {
            uint8_t buf[4096]; size_t rn = 0;
            oc_tls_status st = oc_tls_read(&conn, buf, sizeof buf, &rn);
            if (st == OC_TLS_OK) {
                if (oc_framebuf_push(&fb, buf, rn) != 0 || dispatch(&fb, n->to_ui) < 0) break;
            } else if (st == OC_TLS_CLOSED || st == OC_TLS_ERROR) {
                break;
            }
        }
    }

drop:
    push_simple(n->to_ui, OC_EV_DISCONNECTED, 0);
    oc_framebuf_free(&fb);
    oc_tls_conn_free(&conn);
    oc_tls_client_free(&cli);
    oc_closesock(fd);
    return NULL;
}

/* ---- lifecycle ---- */

oc_net *oc_net_start(const char *host, int port, const char *token,
                     oc_queue *to_ui, oc_queue *from_ui) {
    oc_net *n = calloc(1, sizeof *n);
    if (!n) return NULL;
    snprintf(n->host, sizeof n->host, "%s", host ? host : "127.0.0.1");
    n->port = port;
    n->token = token ? strdup(token) : NULL;
    n->to_ui = to_ui;
    n->from_ui = from_ui;
    if (pthread_create(&n->thread, NULL, net_thread, n) != 0) {
        free(n->token); free(n); return NULL;
    }
    return n;
}

void oc_net_stop(oc_net *n) {
    if (!n) return;
    n->stop = 1;
    oc_queue_push(n->from_ui, oc_cmd_new(OC_CMD_QUIT)); /* wake it promptly */
    pthread_join(n->thread, NULL);
    free(n->token);
    free(n);
}
