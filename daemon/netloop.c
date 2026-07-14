/*
 * OpenChime network event loop. See netloop.h, dbwriter.h, tls.h, framebuf.h.
 */

#include "netloop.h"
#include "framebuf.h"
#include "protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define OC_NETLOOP_MAX_FD 4096
/* Cap channels per CHANNEL_LIST frame so it never exceeds the wire limit; a
 * client with more would page (not needed at current scale). */
#define OC_CHANNEL_LIST_MAX 512
#define OC_USER_LIST_MAX    512
#define OC_REACTION_LIST_MAX 1024

/* Per-connection pending-output cap. A client that stops reading while the
 * daemon keeps fanning out broadcasts would otherwise grow its output buffer
 * without bound (a single-client memory-exhaustion DoS). At the cap the
 * connection is dropped; nothing is lost because delivery is recoverable via
 * reconnect + backfill. Sized to the ~256MB lean profile (REQ-210/211): even a
 * few hundred simultaneously-backlogged connections stay bounded, while a normal
 * client's buffer is near-empty. */
#define OC_MAX_OUT_BUFFER    (1u << 20)   /* 1 MiB */

/* Per-connection message-send rate limit (REQ-190): a fixed window bounds how
 * fast one authenticated client can create messages (SEND / SEND_REPLY), which
 * the broadcast fan-out would otherwise amplify to every channel member. Excess
 * sends get a non-fatal SEND_RATE_LIMITED and are dropped. Generous for a human
 * (10/s sustained, 30 burst); a tight send loop trips it immediately. */
#define OC_SEND_RATE_MAX       30u
#define OC_SEND_RATE_WINDOW_MS 3000u

typedef enum { CONN_HANDSHAKE, CONN_ESTABLISHED } conn_state;

typedef struct {
    int          fd;
    uint64_t     conn_id;
    oc_tls_conn  tls;
    oc_framebuf  fb;
    conn_state   state;
    int          did_hello;
    int          authed;
    uint64_t     user_id;
    char         source[46]; /* peer IP string, for per-source rate limiting */
    uint8_t     *out;       /* growable pending-output buffer (capped, see out_append) */
    size_t       out_cap, out_len, out_sent;
    uint32_t     events;    /* current epoll interest */
    uint64_t     send_win_start; /* fixed-window start for the send rate limit */
    uint32_t     send_count;     /* sends counted in the current window */
} conn;

/* Scratch for encoding one outgoing frame; net thread only, so a single static
 * buffer is safe and avoids per-send allocation (bodies can be ~64KB). */
static uint8_t g_enc[OC_MAX_FRAME_SIZE];
static uint64_t g_next_conn_id = 1;

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static int set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    return (fl < 0) ? -1 : fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* Fixed-window send rate limit (REQ-190). Returns 1 if this send is within
 * budget for the current window, 0 if the cap is exceeded. */
static int send_rate_ok(conn *c) {
    uint64_t now = now_ms();
    if (now - c->send_win_start >= OC_SEND_RATE_WINDOW_MS) {
        c->send_win_start = now;
        c->send_count = 0;
    }
    c->send_count++;
    return c->send_count <= OC_SEND_RATE_MAX;
}

/* --- Outgoing buffer ---------------------------------------------------- */

static int out_append(conn *c, const uint8_t *buf, size_t len) {
    if (c->out_sent > 0) {                       /* drop the already-sent prefix */
        size_t rem = c->out_len - c->out_sent;
        if (rem) memmove(c->out, c->out + c->out_sent, rem);
        c->out_len = rem;
        c->out_sent = 0;
    }
    /* Bound the backlog: a stuck/malicious reader can't grow this without limit.
     * Over the cap, fail — send_bytes/flush_out then drop the connection. */
    if (c->out_len + len > OC_MAX_OUT_BUFFER) return -1;
    if (c->out_len + len > c->out_cap) {
        size_t ncap = c->out_cap ? c->out_cap : 2048;
        while (ncap < c->out_len + len) ncap *= 2;
        uint8_t *g = realloc(c->out, ncap);
        if (!g) return -1;
        c->out = g;
        c->out_cap = ncap;
    }
    memcpy(c->out + c->out_len, buf, len);
    c->out_len += len;
    return 0;
}

/* Returns 1 if fully flushed, 0 if it would block, -1 on error. */
static int flush_out(conn *c) {
    while (c->out_sent < c->out_len) {
        size_t n = 0;
        oc_tls_status st = oc_tls_write(&c->tls, c->out + c->out_sent,
                                        c->out_len - c->out_sent, &n);
        if (st == OC_TLS_OK)         { c->out_sent += n; continue; }
        if (st == OC_TLS_WANT_WRITE || st == OC_TLS_WANT_READ) return 0;
        return -1;
    }
    c->out_len = c->out_sent = 0;
    return 1;
}

/* --- epoll interest ----------------------------------------------------- */

static void conn_set_events(int ep, conn *c, uint32_t events) {
    if (events == c->events) return;
    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    ev.events = events;
    ev.data.fd = c->fd;
    epoll_ctl(ep, EPOLL_CTL_MOD, c->fd, &ev);
    c->events = events;
}

static void update_interest(int ep, conn *c) {
    uint32_t ev = EPOLLIN;
    if (c->out_len > c->out_sent) ev |= EPOLLOUT;
    conn_set_events(ep, c, ev);
}

static void conn_close(int ep, conn **conns, int fd) {
    conn *c = conns[fd];
    if (!c) return;
    epoll_ctl(ep, EPOLL_CTL_DEL, fd, NULL);
    oc_tls_conn_free(&c->tls);
    oc_framebuf_free(&c->fb);
    free(c->out);
    close(fd);
    free(c);
    conns[fd] = NULL;
}

static conn *find_by_id(conn **conns, uint64_t id) {
    for (int fd = 0; fd < OC_NETLOOP_MAX_FD; fd++)
        if (conns[fd] && conns[fd]->conn_id == id) return conns[fd];
    return NULL;
}

/* Count live connections from a peer IP (for the accept throttle). */
static int conns_from_ip(conn **conns, const char *src) {
    if (!src[0]) return 0;
    int n = 0;
    for (int fd = 0; fd < OC_NETLOOP_MAX_FD; fd++)
        if (conns[fd] && strcmp(conns[fd]->source, src) == 0) n++;
    return n;
}

static int in_members(uint64_t uid, const uint64_t *m, size_t n) {
    for (size_t i = 0; i < n; i++) if (m[i] == uid) return 1;
    return 0;
}

/* Append encoded bytes to a connection and try to flush; close it on error. */
static void send_bytes(int ep, conn **conns, int fd, const uint8_t *buf, size_t len) {
    conn *c = conns[fd];
    if (!c) return;
    if (out_append(c, buf, len) != 0 || flush_out(c) < 0) {
        conn_close(ep, conns, fd);
        return;
    }
    update_interest(ep, c);
}

/* --- Handshake response ------------------------------------------------- */

/* Build WELCOME/REJECT for a HELLO into the connection's out buffer.
 * Returns 0 to keep the connection, -1 to close it (fatal REJECT/malformed). */
static int handle_hello(conn *c, oc_rbuf *payload, oc_dbwriter *dbw) {
    oc_hello h;
    oc_wbuf w;
    uint8_t tmp[128];
    if (oc_decode_hello(payload, &h) != OC_OK) {
        oc_wbuf_init(&w, tmp, sizeof tmp);
        oc_reject rej = { OC_ERR_MALFORMED_FRAME, oc_slice_str("bad HELLO") };
        oc_encode_reject(&w, &rej);
        out_append(c, tmp, w.len);
        return -1;
    }
    uint16_t chosen = 0, code = 0;
    if (oc_negotiate_version(h.min_version, h.max_version,
                             OC_PROTOCOL_VERSION, OC_PROTOCOL_VERSION,
                             &chosen, &code) != OC_OK) {
        oc_wbuf_init(&w, tmp, sizeof tmp);
        oc_reject rej = { code, oc_slice_str("unsupported protocol version") };
        oc_encode_reject(&w, &rej);
        out_append(c, tmp, w.len);
        return -1;
    }
    oc_wbuf_init(&w, tmp, sizeof tmp);
    oc_welcome wel = { chosen, now_ms() };
    oc_encode_welcome(&w, &wel);
    out_append(c, tmp, w.len);

    /* Immediately advertise the auth methods this deployment accepts (AUTH.md
     * §5) — local+session, or oidc+session in OIDC mode — plus the OIDC params
     * blob (empty in local mode). Sized for an authorize URL in oidc_params. */
    {
        uint8_t cbuf[1024]; oc_wbuf cw; oc_wbuf_init(&cw, cbuf, sizeof cbuf);
        oc_auth_challenge ch = { oc_dbwriter_auth_methods(dbw),
                                 oc_slice_str(oc_dbwriter_oidc_params(dbw)) };
        oc_encode_auth_challenge(&cw, OC_PROTOCOL_VERSION, &ch);
        out_append(c, cbuf, cw.len);
    }
    return 0;
}

/* --- Frame dispatch ----------------------------------------------------- */

/* Queue a non-fatal SEND_RATE_LIMITED error echoing the offending idempotency
 * token (so the client can correlate the dropped send). Returns out_append's
 * result: 0 keep the connection, -1 (buffer full) -> caller closes. */
static int reject_send_rate(conn *c, const uint8_t idem[OC_IDEM_LEN]) {
    uint8_t tmp[96]; oc_wbuf w; oc_wbuf_init(&w, tmp, sizeof tmp);
    oc_slice ctx = { idem, OC_IDEM_LEN };
    oc_error e = { OC_ERR_SEND_RATE_LIMITED, 0, ctx, oc_slice_str("send rate exceeded") };
    oc_encode_error(&w, OC_PROTOCOL_VERSION, &e);
    return out_append(c, tmp, w.len);
}

/* Dispatch every buffered frame. Returns 0 to keep the connection, -1 to close.
 * AUTH/SEND become jobs for the DB writer; their replies arrive asynchronously
 * via deliver_result. */
static int drain_frames(conn *c, oc_dbwriter *dbw) {
    const uint8_t *frame; size_t flen;
    for (;;) {
        int r = oc_framebuf_next(&c->fb, &frame, &flen);
        if (r == 0) return 0;
        if (r < 0)  return -1;

        oc_header hdr; oc_rbuf p;
        if (oc_parse_frame(frame, flen, &hdr, &p) != OC_OK) return -1;

        if (!c->did_hello) {
            if (hdr.msg_type != OC_MSG_HELLO) return -1;
            c->did_hello = 1;
            if (handle_hello(c, &p, dbw) < 0) return -1;
            continue;
        }

        if (!c->authed) {
            if (hdr.msg_type == OC_MSG_AUTH) {
                oc_auth a;
                if (oc_decode_auth(&p, &a) != OC_OK) return -1;
                oc_job *j = oc_job_new(OC_JOB_AUTH, c->conn_id);
                if (!j) return -1;
                j->method = a.method;
                memcpy(j->source, c->source, sizeof j->source);
                if (oc_job_set_token(j, a.credential.ptr, a.credential.len) != 0) return -1;
                oc_dbwriter_submit(dbw, j);
                continue;
            }
            if (hdr.msg_type == OC_MSG_REDEEM_INVITE) {
                /* Pre-auth account creation: redeem an invite, which both creates
                 * the account and authenticates (reply is AUTH_OK). */
                oc_redeem_invite ri;
                if (oc_decode_redeem_invite(&p, &ri) != OC_OK) return -1;
                oc_job *j = oc_job_new(OC_JOB_REDEEM, c->conn_id);
                if (!j) return -1;
                char *u = strndup((const char *)ri.username.ptr, ri.username.len);
                char *pw = strndup((const char *)ri.password.ptr, ri.password.len);
                int ok = u && pw &&
                         oc_job_set_register(j, u, pw, 0, 0) == 0 &&
                         oc_job_set_token(j, ri.token.ptr, ri.token.len) == 0;
                free(u); free(pw);
                if (!ok) return -1;
                oc_dbwriter_submit(dbw, j);
                continue;
            }
            /* A messaging frame before AUTH is a fatal protocol error. */
            oc_wbuf w; uint8_t tmp[64];
            oc_wbuf_init(&w, tmp, sizeof tmp);
            oc_error e = { OC_ERR_AUTH_REQUIRED, 1, { NULL, 0 }, oc_slice_str("auth required") };
            oc_encode_error(&w, OC_PROTOCOL_VERSION, &e);
            out_append(c, tmp, w.len);
            return -1;
        }

        if (hdr.msg_type == OC_MSG_SEND) {
            oc_send s;
            if (oc_decode_send(&p, &s) != OC_OK) return -1;
            if (!send_rate_ok(c)) { if (reject_send_rate(c, s.idem) != 0) return -1; continue; }
            oc_job *j = oc_job_new(OC_JOB_SEND, c->conn_id);
            if (!j) return -1;
            j->user_id = c->user_id;
            j->channel_id = s.channel_id;
            memcpy(j->idem, s.idem, OC_IDEM_LEN);
            if (oc_job_set_body(j, s.body.ptr, s.body.len) != 0) return -1;
            oc_dbwriter_submit(dbw, j);
            continue;
        }
        if (hdr.msg_type == OC_MSG_EDIT) {
            oc_edit e;
            if (oc_decode_edit(&p, &e) != OC_OK) return -1;
            oc_job *j = oc_job_new(OC_JOB_EDIT, c->conn_id);
            if (!j) return -1;
            j->user_id = c->user_id;
            j->channel_id = e.channel_id;
            j->message_id = e.message_id;
            if (oc_job_set_body(j, e.body.ptr, e.body.len) != 0) return -1;
            oc_dbwriter_submit(dbw, j);
            continue;
        }
        if (hdr.msg_type == OC_MSG_DELETE) {
            oc_delete d;
            if (oc_decode_delete(&p, &d) != OC_OK) return -1;
            oc_job *j = oc_job_new(OC_JOB_DELETE, c->conn_id);
            if (!j) return -1;
            j->user_id = c->user_id;
            j->channel_id = d.channel_id;
            j->message_id = d.message_id;
            oc_dbwriter_submit(dbw, j);
            continue;
        }
        if (hdr.msg_type == OC_MSG_CREATE_CHANNEL) {
            oc_create_channel cc;
            if (oc_decode_create_channel(&p, &cc) != OC_OK) return -1;
            oc_job *j = oc_job_new(OC_JOB_CREATE_CHANNEL, c->conn_id);
            if (!j) return -1;
            j->user_id = c->user_id;
            j->ch_is_public = cc.is_public;
            j->ch_name = malloc(cc.name.len + 1);
            if (!j->ch_name) return -1;
            if (cc.name.len) memcpy(j->ch_name, cc.name.ptr, cc.name.len);
            j->ch_name[cc.name.len] = '\0';
            oc_dbwriter_submit(dbw, j);
            continue;
        }
        if (hdr.msg_type == OC_MSG_LIST_CHANNELS) {
            if (oc_decode_list_channels(&p) != OC_OK) return -1;
            oc_job *j = oc_job_new(OC_JOB_LIST_CHANNELS, c->conn_id);
            if (!j) return -1;
            j->user_id = c->user_id;
            oc_dbwriter_submit(dbw, j);
            continue;
        }
        if (hdr.msg_type == OC_MSG_JOIN_CHANNEL || hdr.msg_type == OC_MSG_LEAVE_CHANNEL) {
            oc_channel_ref cr;
            if (oc_decode_join_channel(&p, &cr) != OC_OK) return -1;
            int jt = (hdr.msg_type == OC_MSG_JOIN_CHANNEL) ? OC_JOB_JOIN_CHANNEL : OC_JOB_LEAVE_CHANNEL;
            oc_job *j = oc_job_new(jt, c->conn_id);
            if (!j) return -1;
            j->user_id = c->user_id;
            j->channel_id = cr.channel_id;
            oc_dbwriter_submit(dbw, j);
            continue;
        }
        if (hdr.msg_type == OC_MSG_INVITE_TO_CHANNEL || hdr.msg_type == OC_MSG_REMOVE_FROM_CHANNEL) {
            oc_channel_member_op op;
            if (oc_decode_invite_to_channel(&p, &op) != OC_OK) return -1;
            int jt = (hdr.msg_type == OC_MSG_INVITE_TO_CHANNEL) ? OC_JOB_INVITE_CHANNEL : OC_JOB_REMOVE_CHANNEL;
            oc_job *j = oc_job_new(jt, c->conn_id);
            if (!j) return -1;
            j->user_id = c->user_id;
            j->channel_id = op.channel_id;
            j->target_user_id = op.user_id;
            oc_dbwriter_submit(dbw, j);
            continue;
        }
        if (hdr.msg_type == OC_MSG_LIST_USERS) {
            if (oc_decode_list_users(&p) != OC_OK) return -1;
            oc_job *j = oc_job_new(OC_JOB_LIST_USERS, c->conn_id);
            if (!j) return -1;
            j->user_id = c->user_id;
            oc_dbwriter_submit(dbw, j);
            continue;
        }
        if (hdr.msg_type == OC_MSG_SET_ROLE) {
            oc_set_role sr;
            if (oc_decode_set_role(&p, &sr) != OC_OK) return -1;
            oc_job *j = oc_job_new(OC_JOB_SET_ROLE, c->conn_id);
            if (!j) return -1;
            j->user_id = c->user_id;
            j->target_user_id = sr.user_id;
            j->role = sr.role;
            oc_dbwriter_submit(dbw, j);
            continue;
        }
        if (hdr.msg_type == OC_MSG_INVITE_USER) {
            oc_invite_user iu;
            if (oc_decode_invite_user(&p, &iu) != OC_OK) return -1;
            oc_job *j = oc_job_new(OC_JOB_INVITE_USER, c->conn_id);
            if (!j) return -1;
            j->user_id = c->user_id;
            j->role = iu.role;
            oc_dbwriter_submit(dbw, j);
            continue;
        }
        if (hdr.msg_type == OC_MSG_REMOVE_USER) {
            oc_remove_user ru;
            if (oc_decode_remove_user(&p, &ru) != OC_OK) return -1;
            oc_job *j = oc_job_new(OC_JOB_REMOVE_USER, c->conn_id);
            if (!j) return -1;
            j->user_id = c->user_id;
            j->target_user_id = ru.user_id;
            oc_dbwriter_submit(dbw, j);
            continue;
        }
        if (hdr.msg_type == OC_MSG_REACT) {
            oc_react rc;
            if (oc_decode_react(&p, &rc) != OC_OK) return -1;
            oc_job *j = oc_job_new(OC_JOB_REACT, c->conn_id);
            if (!j) return -1;
            j->user_id = c->user_id;
            j->channel_id = rc.channel_id;
            j->message_id = rc.message_id;
            j->react_op = rc.op;
            j->emoji = malloc(rc.emoji.len + 1);
            if (!j->emoji) return -1;
            if (rc.emoji.len) memcpy(j->emoji, rc.emoji.ptr, rc.emoji.len);
            j->emoji[rc.emoji.len] = '\0';
            oc_dbwriter_submit(dbw, j);
            continue;
        }
        if (hdr.msg_type == OC_MSG_LIST_REACTIONS) {
            oc_list_reactions lr;
            if (oc_decode_list_reactions(&p, &lr) != OC_OK) return -1;
            oc_job *j = oc_job_new(OC_JOB_LIST_REACTIONS, c->conn_id);
            if (!j) return -1;
            j->user_id = c->user_id;
            j->channel_id = lr.channel_id;
            j->message_id = lr.message_id;
            oc_dbwriter_submit(dbw, j);
            continue;
        }
        if (hdr.msg_type == OC_MSG_SEND_REPLY) {
            oc_send_reply sr;
            if (oc_decode_send_reply(&p, &sr) != OC_OK) return -1;
            if (!send_rate_ok(c)) { if (reject_send_rate(c, sr.idem) != 0) return -1; continue; }
            oc_job *j = oc_job_new(OC_JOB_SEND_REPLY, c->conn_id);
            if (!j) return -1;
            j->user_id = c->user_id;
            j->channel_id = sr.channel_id;
            j->parent_id = sr.parent_id;
            memcpy(j->idem, sr.idem, OC_IDEM_LEN);
            if (oc_job_set_body(j, sr.body.ptr, sr.body.len) != 0) return -1;
            oc_dbwriter_submit(dbw, j);
            continue;
        }
        if (hdr.msg_type == OC_MSG_LIST_THREAD) {
            oc_list_thread lt;
            if (oc_decode_list_thread(&p, &lt) != OC_OK) return -1;
            oc_job *j = oc_job_new(OC_JOB_LIST_THREAD, c->conn_id);
            if (!j) return -1;
            j->user_id = c->user_id;
            j->channel_id = lt.channel_id;
            j->parent_id = lt.parent_id;
            oc_dbwriter_submit(dbw, j);
            continue;
        }
        if (hdr.msg_type == OC_MSG_SEARCH) {
            oc_search sq;
            if (oc_decode_search(&p, &sq) != OC_OK) return -1;
            oc_job *j = oc_job_new(OC_JOB_SEARCH, c->conn_id);
            if (!j) return -1;
            j->user_id = c->user_id;
            j->search_limit = sq.limit;
            if (oc_job_set_body(j, sq.query.ptr, sq.query.len) != 0) return -1;
            oc_dbwriter_submit(dbw, j);
            continue;
        }
        if (hdr.msg_type == OC_MSG_CLIENT_ACK) {
            oc_client_ack ca;
            oc_decode_client_ack(&p, &ca); /* accepted; the client drives backfill via its own cursors */
            continue;
        }
        if (hdr.msg_type == OC_MSG_LOGOUT) {
            oc_logout lo;
            if (oc_decode_logout(&p, &lo) != OC_OK) return -1;
            oc_job *j = oc_job_new(OC_JOB_LOGOUT, c->conn_id);
            if (!j) return -1;
            j->user_id = c->user_id;
            j->scope = lo.scope;
            if (oc_job_set_token(j, lo.session_token.ptr, lo.session_token.len) != 0) return -1;
            oc_dbwriter_submit(dbw, j);
            continue;
        }
        if (hdr.msg_type == OC_MSG_BACKFILL_REQUEST) {
            oc_cursor cursors[256];
            uint16_t count = 0;
            if (oc_decode_backfill_request(&p, cursors, 256, &count) != OC_OK) return -1;
            if (count > 256) count = 256; /* wire count may exceed our capacity */
            oc_job *j = oc_job_new(OC_JOB_BACKFILL, c->conn_id);
            if (!j) return -1;
            j->user_id = c->user_id;
            if (count > 0) {
                j->cursors = malloc((size_t)count * sizeof *j->cursors);
                if (j->cursors) {
                    for (uint16_t i = 0; i < count; i++) {
                        j->cursors[i].channel_id = cursors[i].channel_id;
                        j->cursors[i].after_message_id = cursors[i].after_message_id;
                    }
                    j->n_cursors = count;
                }
            }
            oc_dbwriter_submit(dbw, j);
            continue;
        }
        /* Other post-auth frame types are ignored by this skeleton. */
    }
}

/* Read, reassemble, and dispatch. Returns 0 to keep, -1 to close. */
static int on_readable(conn *c, oc_dbwriter *dbw) {
    for (;;) {
        uint8_t chunk[OC_READ_CHUNK];
        size_t n = 0;
        oc_tls_status st = oc_tls_read(&c->tls, chunk, sizeof chunk, &n);
        if (st == OC_TLS_WANT_READ || st == OC_TLS_WANT_WRITE) return 0;
        if (st == OC_TLS_CLOSED || st == OC_TLS_ERROR) return -1;
        if (oc_framebuf_push(&c->fb, chunk, n) != 0) return -1;
        if (drain_frames(c, dbw) < 0) return -1;
    }
}

/* --- Result delivery (from the DB-writer thread) ------------------------ */

static void deliver_result(int ep, conn **conns, oc_dbres *r) {
    oc_wbuf w;
    switch (r->type) {
    case OC_RES_AUTH_OK: {
        conn *c = find_by_id(conns, r->conn_id);
        if (!c) return;
        c->authed = 1;
        c->user_id = r->user_id;
        oc_wbuf_init(&w, g_enc, sizeof g_enc);
        /* Fresh auth carries the session token; a session re-auth omits it
         * (PROTOCOL.md §4.3). */
        oc_slice tok = r->has_session_token
                     ? (oc_slice){ r->session_token, OC_SESSION_TOKEN_LEN }
                     : (oc_slice){ NULL, 0 };
        oc_auth_ok m = { r->user_id, r->role, r->session_expiry, tok };
        oc_encode_auth_ok(&w, OC_PROTOCOL_VERSION, &m);
        send_bytes(ep, conns, c->fd, g_enc, w.len);
        break;
    }
    case OC_RES_AUTH_ERR: {
        conn *c = find_by_id(conns, r->conn_id);
        if (!c) return;
        oc_wbuf_init(&w, g_enc, sizeof g_enc);
        oc_error e = { r->err_code, 1, { NULL, 0 }, oc_slice_str("auth failed") };
        oc_encode_error(&w, OC_PROTOCOL_VERSION, &e);
        send_bytes(ep, conns, c->fd, g_enc, w.len);
        break;
    }
    case OC_RES_LOGOUT_OK: {
        /* The session is revoked; drop the connection (the client re-auths to
         * continue). Any queued output is discarded with the conn. */
        conn *c = find_by_id(conns, r->conn_id);
        if (c) conn_close(ep, conns, c->fd);
        break;
    }
    case OC_RES_SEND_OK: {
        conn *sender = find_by_id(conns, r->conn_id);
        if (sender) {
            oc_wbuf_init(&w, g_enc, sizeof g_enc);
            oc_send_ack ack;
            memcpy(ack.idem, r->idem, OC_IDEM_LEN);
            ack.channel_id = r->channel_id;
            ack.message_id = r->message_id;
            ack.server_time = r->server_time;
            oc_encode_send_ack(&w, OC_PROTOCOL_VERSION, &ack);
            send_bytes(ep, conns, sender->fd, g_enc, w.len);
        }
        if (!r->duplicate) {
            oc_wbuf_init(&w, g_enc, sizeof g_enc);
            oc_slice body = { r->body, r->body_len };
            oc_broadcast b = { r->message_id, r->channel_id, r->author_id, r->server_time, body };
            oc_encode_broadcast(&w, OC_PROTOCOL_VERSION, &b);
            size_t blen = w.len;
            for (int fd = 0; fd < OC_NETLOOP_MAX_FD; fd++) {
                conn *c = conns[fd];
                if (c && c->authed && in_members(c->user_id, r->members, r->n_members))
                    send_bytes(ep, conns, fd, g_enc, blen);
            }
        }
        break;
    }
    case OC_RES_SEND_ERR: {
        conn *c = find_by_id(conns, r->conn_id);
        if (!c) return;
        oc_wbuf_init(&w, g_enc, sizeof g_enc);
        oc_slice ctx = { r->idem, OC_IDEM_LEN };
        oc_error e = { r->err_code, 0, ctx, oc_slice_str("send rejected") };
        oc_encode_error(&w, OC_PROTOCOL_VERSION, &e);
        send_bytes(ep, conns, c->fd, g_enc, w.len);
        break;
    }
    case OC_RES_EDIT_OK: {
        /* Fan the edit out to every connected member (including the editor, whose
         * frame doubles as the confirmation) — same shape as a BROADCAST. */
        oc_wbuf_init(&w, g_enc, sizeof g_enc);
        oc_slice body = { r->body, r->body_len };
        oc_msg_edited m = { r->message_id, r->channel_id, r->author_id, r->server_time, body };
        oc_encode_msg_edited(&w, OC_PROTOCOL_VERSION, &m);
        size_t blen = w.len;
        for (int fd = 0; fd < OC_NETLOOP_MAX_FD; fd++) {
            conn *c = conns[fd];
            if (c && c->authed && in_members(c->user_id, r->members, r->n_members))
                send_bytes(ep, conns, fd, g_enc, blen);
        }
        break;
    }
    case OC_RES_DELETE_OK: {
        oc_wbuf_init(&w, g_enc, sizeof g_enc);
        oc_msg_deleted m = { r->message_id, r->channel_id, r->author_id, r->user_id, r->server_time };
        oc_encode_msg_deleted(&w, OC_PROTOCOL_VERSION, &m);
        size_t blen = w.len;
        for (int fd = 0; fd < OC_NETLOOP_MAX_FD; fd++) {
            conn *c = conns[fd];
            if (c && c->authed && in_members(c->user_id, r->members, r->n_members))
                send_bytes(ep, conns, fd, g_enc, blen);
        }
        break;
    }
    case OC_RES_EDIT_ERR:
    case OC_RES_DELETE_ERR: {
        /* Non-fatal: report to the requester with the offending message_id in
         * `context` (8 bytes, big-endian) so the client can correlate. */
        conn *c = find_by_id(conns, r->conn_id);
        if (!c) return;
        uint8_t ctx[8];
        for (int i = 0; i < 8; i++) ctx[i] = (uint8_t)(r->message_id >> (56 - 8 * i));
        oc_wbuf_init(&w, g_enc, sizeof g_enc);
        oc_slice cs = { ctx, sizeof ctx };
        oc_error e = { r->err_code, 0, cs, oc_slice_str("edit/delete rejected") };
        oc_encode_error(&w, OC_PROTOCOL_VERSION, &e);
        send_bytes(ep, conns, c->fd, g_enc, w.len);
        break;
    }
    case OC_RES_CHANNEL_INFO: {
        /* Ack the actor with the channel's state. */
        conn *c = find_by_id(conns, r->conn_id);
        if (c) {
            oc_wbuf_init(&w, g_enc, sizeof g_enc);
            oc_channel_info ci = { r->channel_id, r->ch_kind,
                                   oc_slice_str(r->ch_name ? r->ch_name : ""),
                                   r->ch_is_public, r->ch_joined, r->ch_created_at };
            oc_encode_channel_info(&w, OC_PROTOCOL_VERSION, &ci);
            send_bytes(ep, conns, c->fd, g_enc, w.len);
        }
        /* On INVITE, push the (now-member) channel to the target's live conns. */
        if (r->push_user_id) {
            oc_wbuf_init(&w, g_enc, sizeof g_enc);
            oc_channel_info ci = { r->channel_id, r->ch_kind,
                                   oc_slice_str(r->ch_name ? r->ch_name : ""),
                                   r->ch_is_public, 1, r->ch_created_at };
            oc_encode_channel_info(&w, OC_PROTOCOL_VERSION, &ci);
            size_t len = w.len;
            for (int fd = 0; fd < OC_NETLOOP_MAX_FD; fd++) {
                conn *t = conns[fd];
                if (t && t->authed && t->user_id == r->push_user_id)
                    send_bytes(ep, conns, fd, g_enc, len);
            }
        }
        break;
    }
    case OC_RES_CHANNEL_ERR: {
        conn *c = find_by_id(conns, r->conn_id);
        if (!c) return;
        uint8_t ctx[8];
        for (int i = 0; i < 8; i++) ctx[i] = (uint8_t)(r->channel_id >> (56 - 8 * i));
        oc_wbuf_init(&w, g_enc, sizeof g_enc);
        oc_slice cs = { ctx, sizeof ctx };
        oc_error e = { r->err_code, 0, cs, oc_slice_str("channel op rejected") };
        oc_encode_error(&w, OC_PROTOCOL_VERSION, &e);
        send_bytes(ep, conns, c->fd, g_enc, w.len);
        break;
    }
    case OC_RES_CHANNEL_LIST: {
        conn *c = find_by_id(conns, r->conn_id);
        if (!c) return;
        size_t n = r->n_chlist > OC_CHANNEL_LIST_MAX ? OC_CHANNEL_LIST_MAX : r->n_chlist;
        oc_channel_list_entry *ents = n ? malloc(n * sizeof *ents) : NULL;
        if (n && !ents) n = 0;
        for (size_t i = 0; i < n; i++) {
            ents[i].channel_id = r->chlist[i].channel_id;
            ents[i].name = oc_slice_str(r->chlist[i].name ? r->chlist[i].name : "");
            ents[i].is_public = r->chlist[i].is_public;
            ents[i].joined = r->chlist[i].joined;
        }
        oc_wbuf_init(&w, g_enc, sizeof g_enc);
        oc_channel_list cl = { (uint16_t)n, ents };
        oc_encode_channel_list(&w, OC_PROTOCOL_VERSION, &cl);
        send_bytes(ep, conns, c->fd, g_enc, w.len);
        free(ents);
        break;
    }
    case OC_RES_USER_LIST: {
        conn *c = find_by_id(conns, r->conn_id);
        if (!c) return;
        size_t n = r->n_ulist > OC_USER_LIST_MAX ? OC_USER_LIST_MAX : r->n_ulist;
        oc_user_list_entry *ents = n ? malloc(n * sizeof *ents) : NULL;
        if (n && !ents) n = 0;
        for (size_t i = 0; i < n; i++) {
            ents[i].user_id = r->ulist[i].user_id;
            ents[i].role = r->ulist[i].role;
            ents[i].disabled = r->ulist[i].disabled;
            ents[i].email = oc_slice_str(r->ulist[i].email ? r->ulist[i].email : "");
            ents[i].display_name = oc_slice_str(r->ulist[i].display_name ? r->ulist[i].display_name : "");
        }
        oc_wbuf_init(&w, g_enc, sizeof g_enc);
        oc_user_list ul = { (uint16_t)n, ents };
        oc_encode_user_list(&w, OC_PROTOCOL_VERSION, &ul);
        send_bytes(ep, conns, c->fd, g_enc, w.len);
        free(ents);
        break;
    }
    case OC_RES_INVITE_OK: {
        conn *c = find_by_id(conns, r->conn_id);
        if (!c) return;
        oc_slice tok = { r->session_token, OC_INVITE_TOKEN_LEN };
        oc_invite_created ic = { tok, r->role, r->session_expiry };
        oc_wbuf_init(&w, g_enc, sizeof g_enc);
        oc_encode_invite_created(&w, OC_PROTOCOL_VERSION, &ic);
        send_bytes(ep, conns, c->fd, g_enc, w.len);
        break;
    }
    case OC_RES_INVITE_ERR:
    case OC_RES_SETROLE_ERR:
    case OC_RES_USER_ERR: {
        conn *c = find_by_id(conns, r->conn_id);
        if (!c) return;
        oc_wbuf_init(&w, g_enc, sizeof g_enc);
        oc_error e = { r->err_code, 0, { NULL, 0 }, oc_slice_str("admin op rejected") };
        oc_encode_error(&w, OC_PROTOCOL_VERSION, &e);
        send_bytes(ep, conns, c->fd, g_enc, w.len);
        break;
    }
    case OC_RES_SETROLE_OK: {
        /* Ack the actor and push the new role to the affected user's live conns
         * (so their client updates its capabilities immediately). */
        oc_user_updated m = { r->user_id, r->role, 0 };
        oc_wbuf_init(&w, g_enc, sizeof g_enc);
        oc_encode_user_updated(&w, OC_PROTOCOL_VERSION, &m);
        size_t len = w.len;
        conn *actor = find_by_id(conns, r->conn_id);
        if (actor) send_bytes(ep, conns, actor->fd, g_enc, len);
        for (int fd = 0; fd < OC_NETLOOP_MAX_FD; fd++) {
            conn *t = conns[fd];
            if (t && t->authed && t->user_id == r->user_id)
                send_bytes(ep, conns, fd, g_enc, len);
        }
        break;
    }
    case OC_RES_USER_UPDATED: {
        /* Removal (disabled=1): ack the actor, notify the removed user's live
         * connections, then drop them (they cannot re-authenticate). */
        oc_user_updated m = { r->user_id, r->role, r->disabled };
        oc_wbuf_init(&w, g_enc, sizeof g_enc);
        oc_encode_user_updated(&w, OC_PROTOCOL_VERSION, &m);
        size_t len = w.len;
        conn *actor = find_by_id(conns, r->conn_id);
        if (actor) send_bytes(ep, conns, actor->fd, g_enc, len);
        for (int fd = 0; fd < OC_NETLOOP_MAX_FD; fd++) {
            conn *t = conns[fd];
            if (t && t->authed && t->user_id == r->user_id) {
                send_bytes(ep, conns, fd, g_enc, len);
                if (r->disabled && conns[fd]) conn_close(ep, conns, fd);
            }
        }
        break;
    }
    case OC_RES_REACTION_OK: {
        /* Fan the reaction change out to every connected member (REQ-070/071). */
        oc_wbuf_init(&w, g_enc, sizeof g_enc);
        oc_reaction_updated m = { r->message_id, r->channel_id, r->user_id,
                                  oc_slice_str(r->emoji ? r->emoji : ""),
                                  r->react_op, r->react_count };
        oc_encode_reaction_updated(&w, OC_PROTOCOL_VERSION, &m);
        size_t len = w.len;
        for (int fd = 0; fd < OC_NETLOOP_MAX_FD; fd++) {
            conn *c = conns[fd];
            if (c && c->authed && in_members(c->user_id, r->members, r->n_members))
                send_bytes(ep, conns, fd, g_enc, len);
        }
        break;
    }
    case OC_RES_REACTION_ERR: {
        conn *c = find_by_id(conns, r->conn_id);
        if (!c) return;
        uint8_t ctx[8];
        for (int i = 0; i < 8; i++) ctx[i] = (uint8_t)(r->message_id >> (56 - 8 * i));
        oc_wbuf_init(&w, g_enc, sizeof g_enc);
        oc_slice cs = { ctx, sizeof ctx };
        oc_error e = { r->err_code, 0, cs, oc_slice_str("reaction rejected") };
        oc_encode_error(&w, OC_PROTOCOL_VERSION, &e);
        send_bytes(ep, conns, c->fd, g_enc, w.len);
        break;
    }
    case OC_RES_REACTIONS: {
        conn *c = find_by_id(conns, r->conn_id);
        if (!c) return;
        size_t n = r->n_rlist > OC_REACTION_LIST_MAX ? OC_REACTION_LIST_MAX : r->n_rlist;
        oc_reaction_entry *ents = n ? malloc(n * sizeof *ents) : NULL;
        if (n && !ents) n = 0;
        for (size_t i = 0; i < n; i++) {
            ents[i].emoji = oc_slice_str(r->rlist[i].emoji ? r->rlist[i].emoji : "");
            ents[i].user_id = r->rlist[i].user_id;
        }
        oc_wbuf_init(&w, g_enc, sizeof g_enc);
        oc_reactions rr = { r->message_id, (uint16_t)n, ents };
        oc_encode_reactions(&w, OC_PROTOCOL_VERSION, &rr);
        send_bytes(ep, conns, c->fd, g_enc, w.len);
        free(ents);
        break;
    }
    case OC_RES_REPLY_OK: {
        /* Ack the sender; then, unless it was an idempotent replay, fan the reply
         * out as a THREAD_REPLY (never to the main scroll, REQ-060). */
        conn *sender = find_by_id(conns, r->conn_id);
        if (sender) {
            oc_wbuf_init(&w, g_enc, sizeof g_enc);
            oc_send_ack ack;
            memcpy(ack.idem, r->idem, OC_IDEM_LEN);
            ack.channel_id = r->channel_id;
            ack.message_id = r->message_id;
            ack.server_time = r->server_time;
            oc_encode_send_ack(&w, OC_PROTOCOL_VERSION, &ack);
            send_bytes(ep, conns, sender->fd, g_enc, w.len);
        }
        if (!r->duplicate) {
            oc_wbuf_init(&w, g_enc, sizeof g_enc);
            oc_slice body = { r->body, r->body_len };
            oc_thread_reply tr = { r->message_id, r->channel_id, r->parent_id,
                                   r->author_id, r->server_time, r->reply_count, body };
            oc_encode_thread_reply(&w, OC_PROTOCOL_VERSION, &tr);
            size_t blen = w.len;
            for (int fd = 0; fd < OC_NETLOOP_MAX_FD; fd++) {
                conn *c = conns[fd];
                if (c && c->authed && in_members(c->user_id, r->members, r->n_members))
                    send_bytes(ep, conns, fd, g_enc, blen);
            }
        }
        break;
    }
    case OC_RES_REPLY_ERR: {
        conn *c = find_by_id(conns, r->conn_id);
        if (!c) return;
        oc_wbuf_init(&w, g_enc, sizeof g_enc);
        oc_slice ctx = { r->idem, OC_IDEM_LEN };
        oc_error e = { r->err_code, 0, ctx, oc_slice_str("reply rejected") };
        oc_encode_error(&w, OC_PROTOCOL_VERSION, &e);
        send_bytes(ep, conns, c->fd, g_enc, w.len);
        break;
    }
    case OC_RES_THREAD: {
        conn *c = find_by_id(conns, r->conn_id);
        if (!c) return;
        if (r->err_code) {
            uint8_t ctx[8];
            for (int i = 0; i < 8; i++) ctx[i] = (uint8_t)(r->parent_id >> (56 - 8 * i));
            oc_wbuf_init(&w, g_enc, sizeof g_enc);
            oc_slice cs = { ctx, sizeof ctx };
            oc_error e = { r->err_code, 0, cs, oc_slice_str("thread unavailable") };
            oc_encode_error(&w, OC_PROTOCOL_VERSION, &e);
            send_bytes(ep, conns, c->fd, g_enc, w.len);
            break;
        }
        /* Stream each reply as a self-framed THREAD_REPLY (a 64KB body is fine),
         * then close with the THREAD terminator (like BACKFILL_DONE). */
        int fd = c->fd;
        for (size_t i = 0; i < r->n_thread && conns[fd]; i++) {
            oc_replay_msg *m = &r->thread[i];
            oc_wbuf_init(&w, g_enc, sizeof g_enc);
            oc_slice body = { m->body, m->body_len };
            oc_thread_reply tr = { m->message_id, m->channel_id, r->parent_id,
                                   m->author_id, m->server_time, (uint32_t)r->n_thread, body };
            oc_encode_thread_reply(&w, OC_PROTOCOL_VERSION, &tr);
            send_bytes(ep, conns, fd, g_enc, w.len);
        }
        if (!conns[fd]) break;
        oc_wbuf_init(&w, g_enc, sizeof g_enc);
        oc_thread th = { r->parent_id, (uint32_t)r->n_thread };
        oc_encode_thread(&w, OC_PROTOCOL_VERSION, &th);
        send_bytes(ep, conns, fd, g_enc, w.len);
        break;
    }
    case OC_RES_SEARCH: {
        conn *c = find_by_id(conns, r->conn_id);
        if (!c) return;
        size_t n = r->n_search;   /* already bounded by OC_SEARCH_MAX */
        oc_search_result_entry *ents = n ? malloc(n * sizeof *ents) : NULL;
        if (n && !ents) n = 0;
        for (size_t i = 0; i < n; i++) {
            ents[i].message_id = r->search[i].message_id;
            ents[i].channel_id = r->search[i].channel_id;
            ents[i].author_id = r->search[i].author_id;
            ents[i].server_time = r->search[i].server_time;
            ents[i].snippet.ptr = r->search[i].body;
            ents[i].snippet.len = r->search[i].body_len;
        }
        oc_wbuf_init(&w, g_enc, sizeof g_enc);
        oc_search_results sr = { (uint16_t)n, ents };
        oc_encode_search_results(&w, OC_PROTOCOL_VERSION, &sr);
        send_bytes(ep, conns, c->fd, g_enc, w.len);
        free(ents);
        break;
    }
    case OC_RES_BACKFILL_OK: {
        conn *c = find_by_id(conns, r->conn_id);
        if (!c) return;
        int fd = c->fd;
        /* Replay each missed top-level message as a BROADCAST, ascending id.
         * A message with thread replies is followed by a THREAD_META so the
         * client can show its reply count without opening the thread (REQ-060). */
        for (size_t i = 0; i < r->n_replay && conns[fd]; i++) {
            oc_replay_msg *m = &r->replay[i];
            oc_wbuf_init(&w, g_enc, sizeof g_enc);
            oc_slice body = { m->body, m->body_len };
            oc_broadcast b = { m->message_id, m->channel_id, m->author_id, m->server_time, body };
            oc_encode_broadcast(&w, OC_PROTOCOL_VERSION, &b);
            send_bytes(ep, conns, fd, g_enc, w.len);
            if (m->reply_count > 0 && conns[fd]) {
                oc_wbuf_init(&w, g_enc, sizeof g_enc);
                oc_thread_meta tm = { m->message_id, m->reply_count, m->last_reply_at };
                oc_encode_thread_meta(&w, OC_PROTOCOL_VERSION, &tm);
                send_bytes(ep, conns, fd, g_enc, w.len);
            }
        }
        if (!conns[fd]) return;
        oc_wbuf_init(&w, g_enc, sizeof g_enc);
        oc_backfill_done done = { r->high_water };
        oc_encode_backfill_done(&w, OC_PROTOCOL_VERSION, &done);
        send_bytes(ep, conns, fd, g_enc, w.len);
        break;
    }
    default: break;
    }
}

/* --- Main loop ---------------------------------------------------------- */

int oc_netloop_run(int port, oc_tls_server *tls, oc_dbwriter *dbw,
                   volatile sig_atomic_t *stop) {
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
    int evfd = oc_dbwriter_eventfd(dbw);

    /* Per-source-IP concurrent-connection cap (0 disables). Blunts a
     * connection-exhaustion flood from one host while staying generous enough
     * for a large office behind one NAT; operators tune it. The global cap is
     * OC_NETLOOP_MAX_FD. */
    int max_per_ip = 256;
    {
        const char *v = getenv("OPENCHIME_MAX_CONNS_PER_IP");
        if (v) { int n = atoi(v); if (n >= 0) max_per_ip = n; }
    }

    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    ev.events = EPOLLIN; ev.data.fd = lfd;
    epoll_ctl(ep, EPOLL_CTL_ADD, lfd, &ev);
    memset(&ev, 0, sizeof ev);
    ev.events = EPOLLIN; ev.data.fd = evfd;
    epoll_ctl(ep, EPOLL_CTL_ADD, evfd, &ev);

    fprintf(stderr, "netloop: listening on :%d\n", port);

    struct epoll_event events[64];
    while (!*stop) {
        int nfds = epoll_wait(ep, events, 64, 500);
        if (nfds < 0) { if (errno == EINTR) continue; break; }

        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;

            if (fd == lfd) {
                for (;;) {
                    struct sockaddr_storage ss;
                    socklen_t sl = sizeof ss;
                    int cfd = accept(lfd, (struct sockaddr *)&ss, &sl);
                    if (cfd < 0) break;
                    if (cfd >= OC_NETLOOP_MAX_FD || set_nonblock(cfd) < 0) { close(cfd); continue; }
                    /* Peer IP (for the accept throttle + per-source auth limit). */
                    char src[46] = {0};
                    if (ss.ss_family == AF_INET) {
                        inet_ntop(AF_INET, &((struct sockaddr_in *)&ss)->sin_addr, src, sizeof src);
                    } else if (ss.ss_family == AF_INET6) {
                        inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&ss)->sin6_addr, src, sizeof src);
                    }
                    /* Throttle a single IP before spending a conn/TLS context on it. */
                    if (max_per_ip > 0 && conns_from_ip(conns, src) >= max_per_ip) {
                        close(cfd);
                        continue;
                    }
                    conn *c = calloc(1, sizeof *c);
                    if (!c || oc_framebuf_init(&c->fb) != 0 ||
                        oc_tls_conn_init(&c->tls, &tls->conf, cfd) != 0) {
                        if (c) { oc_framebuf_free(&c->fb); free(c); }
                        close(cfd);
                        continue;
                    }
                    c->fd = cfd;
                    memcpy(c->source, src, sizeof c->source);
                    c->conn_id = g_next_conn_id++;
                    c->state = CONN_HANDSHAKE;
                    conns[cfd] = c;
                    struct epoll_event cev;
                    memset(&cev, 0, sizeof cev);
                    cev.events = EPOLLIN; cev.data.fd = cfd;
                    epoll_ctl(ep, EPOLL_CTL_ADD, cfd, &cev);
                    c->events = EPOLLIN;
                }
                continue;
            }

            if (fd == evfd) {
                uint64_t cnt;
                while (read(evfd, &cnt, sizeof cnt) > 0) { /* drain the counter */ }
                oc_dbres *r;
                while ((r = oc_dbwriter_next_result(dbw)) != NULL) {
                    deliver_result(ep, conns, r);
                    oc_dbres_free(r);
                }
                continue;
            }

            conn *c = conns[fd];
            if (!c) continue;

            if (c->state == CONN_HANDSHAKE) {
                oc_tls_status st = oc_tls_handshake(&c->tls);
                if (st == OC_TLS_OK)              c->state = CONN_ESTABLISHED;
                else if (st == OC_TLS_WANT_READ)  { conn_set_events(ep, c, EPOLLIN);  continue; }
                else if (st == OC_TLS_WANT_WRITE) { conn_set_events(ep, c, EPOLLOUT); continue; }
                else { conn_close(ep, conns, fd); continue; }
                /* fall through: drain any app data mbedTLS already buffered */
            }

            if (c->state == CONN_ESTABLISHED) {
                if (on_readable(c, dbw) < 0) { flush_out(c); conn_close(ep, conns, fd); continue; }
            }
            if (flush_out(c) < 0) { conn_close(ep, conns, fd); continue; }
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
