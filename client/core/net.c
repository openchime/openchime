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
                if (b.author_name.len) {
                    size_t an = b.author_name.len < sizeof e->author_name - 1
                                    ? b.author_name.len : sizeof e->author_name - 1;
                    memcpy(e->author_name, b.author_name.ptr, an);
                    e->author_name[an] = '\0';
                }
                e->body = malloc(b.body.len + 1);
                if (e->body) { memcpy(e->body, b.body.ptr, b.body.len); e->body[b.body.len] = '\0'; }
                oc_queue_push(to_ui, e);
            }
        } else if (hdr.msg_type == OC_MSG_CHANNEL_LIST) {
            oc_channel_list_entry ents[256]; uint16_t count = 0;
            if (oc_decode_channel_list(&p, ents, 256, &count) != OC_OK) return -1;
            if (count > 256) count = 256;
            for (uint16_t i = 0; i < count; i++) {
                oc_ev *e = oc_ev_new(OC_EV_CHANNEL);
                if (!e) continue;
                e->channel_id = ents[i].channel_id;
                e->status = ents[i].joined;
                e->body = malloc(ents[i].name.len + 1);
                if (e->body) { memcpy(e->body, ents[i].name.ptr, ents[i].name.len); e->body[ents[i].name.len] = '\0'; }
                oc_queue_push(to_ui, e);
            }
        } else if (hdr.msg_type == OC_MSG_PRESENCE_UPDATE) {
            oc_presence_update pu;
            if (oc_decode_presence_update(&p, &pu) == OC_OK) {
                oc_ev *e = oc_ev_new(OC_EV_PRESENCE);
                if (e) { e->user_id = pu.user_id; e->status = pu.status; oc_queue_push(to_ui, e); }
            }
        } else if (hdr.msg_type == OC_MSG_REACTION_UPDATED) {
            oc_reaction_updated ru;
            if (oc_decode_reaction_updated(&p, &ru) == OC_OK) {
                oc_ev *e = oc_ev_new(OC_EV_REACTION);
                if (e) {
                    e->channel_id = ru.channel_id;
                    e->message_id = ru.message_id;
                    e->user_id = ru.user_id;
                    e->op = ru.op;
                    e->count = (uint32_t)ru.count;
                    size_t en = ru.emoji.len < sizeof e->emoji - 1 ? ru.emoji.len : sizeof e->emoji - 1;
                    memcpy(e->emoji, ru.emoji.ptr, en);
                    e->emoji[en] = '\0';
                    oc_queue_push(to_ui, e);
                }
            }
        } else if (hdr.msg_type == OC_MSG_TYPING_UPDATE) {
            oc_typing_update tu;
            if (oc_decode_typing_update(&p, &tu) == OC_OK) {
                oc_ev *e = oc_ev_new(OC_EV_TYPING);
                if (e) { e->channel_id = tu.channel_id; e->user_id = tu.user_id; oc_queue_push(to_ui, e); }
            }
        } else if (hdr.msg_type == OC_MSG_MSG_EDITED) {
            oc_msg_edited me;
            if (oc_decode_msg_edited(&p, &me) == OC_OK) {
                oc_ev *e = oc_ev_new(OC_EV_EDIT);
                if (e) {
                    e->channel_id = me.channel_id;
                    e->message_id = me.message_id;
                    e->body = malloc(me.body.len + 1);
                    if (e->body) { memcpy(e->body, me.body.ptr, me.body.len); e->body[me.body.len] = '\0'; }
                    oc_queue_push(to_ui, e);
                }
            }
        } else if (hdr.msg_type == OC_MSG_MSG_DELETED) {
            oc_msg_deleted md;
            if (oc_decode_msg_deleted(&p, &md) == OC_OK) {
                oc_ev *e = oc_ev_new(OC_EV_DELETE);
                if (e) { e->channel_id = md.channel_id; e->message_id = md.message_id; oc_queue_push(to_ui, e); }
            }
        } else if (hdr.msg_type == OC_MSG_THREAD_REPLY) {
            oc_thread_reply tr;
            if (oc_decode_thread_reply(&p, &tr) == OC_OK) {
                oc_ev *e = oc_ev_new(OC_EV_THREAD_REPLY);
                if (e) {
                    e->channel_id = tr.channel_id;
                    e->parent_id = tr.parent_id;
                    e->message_id = tr.message_id;
                    e->author_id = tr.author_id;
                    e->server_time = tr.server_time;
                    e->count = tr.reply_count;
                    e->body = malloc(tr.body.len + 1);
                    if (e->body) { memcpy(e->body, tr.body.ptr, tr.body.len); e->body[tr.body.len] = '\0'; }
                    oc_queue_push(to_ui, e);
                }
            }
        } else if (hdr.msg_type == OC_MSG_THREAD_META) {
            oc_thread_meta tm;
            if (oc_decode_thread_meta(&p, &tm) == OC_OK) {
                oc_ev *e = oc_ev_new(OC_EV_THREAD_META);
                if (e) { e->message_id = tm.message_id; e->count = tm.reply_count; oc_queue_push(to_ui, e); }
            }
        } else if (hdr.msg_type == OC_MSG_SEARCH_RESULTS) {
            oc_search_result_entry se[64]; uint16_t count = 0;
            if (oc_decode_search_results(&p, se, 64, &count, NULL) != OC_OK) return -1;
            if (count > 64) count = 64;
            for (uint16_t i = 0; i < count; i++) {
                oc_ev *e = oc_ev_new(OC_EV_SEARCH_RESULT);
                if (!e) continue;
                e->channel_id = se[i].channel_id;
                e->message_id = se[i].message_id;
                e->author_id = se[i].author_id;
                e->server_time = se[i].server_time;
                e->body = malloc(se[i].snippet.len + 1);
                if (e->body) { memcpy(e->body, se[i].snippet.ptr, se[i].snippet.len); e->body[se[i].snippet.len] = '\0'; }
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
     * fingerprint arrives with the client store phase. Multiple oc_clients in one
     * process (the headless test) set up TLS concurrently; that is safe because
     * the vendored mbedTLS is built with MBEDTLS_THREADING (scripts/build_mbedtls.sh). */
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

    /* WELCOME is followed by AUTH_CHALLENGE advertising the daemon's methods
     * (PROTOCOL.md §4.1). This skeleton only does local auth, so we read and
     * accept the challenge without inspecting the offered set. */
    {
        oc_header hdr; oc_rbuf p;
        if (read_one(&conn, fd, &fb, &hdr, &p, &n->stop) != 0) goto drop;
        if (hdr.msg_type != OC_MSG_AUTH_CHALLENGE) goto drop;
        oc_auth_challenge ch;
        if (oc_decode_auth_challenge(&p, &ch) != OC_OK) goto drop;
    }

    /* Local AUTH -> AUTH_OK. `token` carries "username:password" for now; the
     * real login UI (plus OIDC and session reconnect) arrives in a later client
     * phase. The session token in AUTH_OK is ignored until the store phase. */
    {
        const char *cred = n->token ? n->token : "";
        const char *sep = strchr(cred, ':');
        oc_slice user = sep ? (oc_slice){ (const uint8_t *)cred, (size_t)(sep - cred) }
                            : oc_slice_str(cred);
        oc_slice pass = sep ? oc_slice_str(sep + 1) : (oc_slice){ (const uint8_t *)"", 0 };
        uint8_t cbuf[512]; oc_wbuf cw; oc_wbuf_init(&cw, cbuf, sizeof cbuf);
        if (oc_encode_local_credential(&cw, user, pass) != OC_OK) goto drop;

        uint8_t buf[1024]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
        oc_auth a = { OC_AUTH_LOCAL, { cbuf, cw.len } };
        if (oc_encode_auth(&w, OC_PROTOCOL_VERSION, &a) != OC_OK || write_all(&conn, fd, buf, w.len, &n->stop) != 0) goto drop;
        oc_header hdr; oc_rbuf p;
        if (read_one(&conn, fd, &fb, &hdr, &p, &n->stop) != 0) goto drop;
        if (hdr.msg_type != OC_MSG_AUTH_OK) { push_err(n->to_ui, "auth failed"); goto drop; }
        oc_auth_ok ok;
        oc_decode_auth_ok(&p, &ok);
        push_simple(n->to_ui, OC_EV_CONNECTED, ok.user_id);
        push_simple(n->to_ui, OC_EV_AUTH_OK, ok.user_id);

        /* Ask for the channel list so the model can populate its sidebar. */
        uint8_t lb[16]; oc_wbuf lw; oc_wbuf_init(&lw, lb, sizeof lb);
        if (oc_encode_list_channels(&lw, OC_PROTOCOL_VERSION) == OC_OK)
            (void)write_all(&conn, fd, lb, lw.len, &n->stop);
    }

    /* Serve: interleave reading server frames with sending queued user actions. */
    while (!n->stop) {
        oc_cmd *c;
        while ((c = oc_queue_try_pop(n->from_ui)) != NULL) {
            if (c->type == OC_CMD_QUIT) { oc_cmd_free(c); goto drop; }
            if (c->type == OC_CMD_BACKFILL) {
                /* Replay history for one channel from the start (no local store
                 * yet, so the cursor is 0). Replies arrive as BROADCASTs that
                 * dispatch() already turns into OC_EV_MESSAGE, dedup'd by the
                 * model's high-water mark; a trailing BACKFILL_DONE is ignored. */
                uint8_t buf[64]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                oc_cursor cur = { c->channel_id, 0 };
                oc_backfill_request req = { 1, &cur };
                if (oc_encode_backfill_request(&w, OC_PROTOCOL_VERSION, &req) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_SEND && c->body) {
                uint8_t buf[OC_MAX_FRAME_SIZE]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                oc_send s;
                s.channel_id = c->channel_id ? c->channel_id : 1;
                gen_idem(s.idem);
                s.body = oc_slice_str(c->body);
                if (oc_encode_send(&w, OC_PROTOCOL_VERSION, &s) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_REACT && c->body) {
                uint8_t buf[128]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                oc_react rc = { c->channel_id, c->message_id, oc_slice_str(c->body), c->op };
                if (oc_encode_react(&w, OC_PROTOCOL_VERSION, &rc) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_EDIT && c->body) {
                uint8_t buf[OC_MAX_FRAME_SIZE]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                oc_edit ed = { c->channel_id, c->message_id, oc_slice_str(c->body) };
                if (oc_encode_edit(&w, OC_PROTOCOL_VERSION, &ed) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_DELETE) {
                uint8_t buf[64]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                oc_delete dl = { c->channel_id, c->message_id };
                if (oc_encode_delete(&w, OC_PROTOCOL_VERSION, &dl) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_TYPING) {
                uint8_t buf[32]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                oc_typing ty = { c->channel_id };
                if (oc_encode_typing(&w, OC_PROTOCOL_VERSION, &ty) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_OPEN_THREAD) {
                uint8_t buf[64]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                oc_list_thread lt = { c->channel_id, c->message_id };
                if (oc_encode_list_thread(&w, OC_PROTOCOL_VERSION, &lt) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_REPLY && c->body) {
                uint8_t buf[OC_MAX_FRAME_SIZE]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                oc_send_reply sr = {0};
                sr.channel_id = c->channel_id;
                gen_idem(sr.idem);
                sr.parent_id = c->message_id;
                sr.body = oc_slice_str(c->body);
                if (oc_encode_send_reply(&w, OC_PROTOCOL_VERSION, &sr) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_SEARCH && c->body) {
                uint8_t buf[512]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                oc_search s = { oc_slice_str(c->body), 50 };
                if (oc_encode_search(&w, OC_PROTOCOL_VERSION, &s) == OC_OK)
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
