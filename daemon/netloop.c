/*
 * OpenChime network event loop. See netloop.h, dbwriter.h, tls.h, framebuf.h.
 */

#include "netloop.h"
#include "audio.h"
#include "auth.h"
#include "blobstore.h"
#include "config.h"
#include "push.h"
#include "xferpool.h"
#include "storage.h"
#include "framebuf.h"
#include "http.h"
#include "protocol.h"
#include "ratelimit.h"

#include <mbedtls/sha256.h>

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
#define OC_WEBHOOK_LIST_MAX 256
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

/* Attachment transfer state (REQ-140/141, ARCH-69). A connection carries at most
 * one transfer at a time. Uploads stream client->blob (net-thread writes to the
 * a worker pool (ARCH-69, daemon/xferpool.c) rather than inline, so a slow S3
 * endpoint cannot stall the event loop. At most one blob job per transfer is in
 * flight; while one is, the connection stops being read, so a client cannot
 * outrun the store and the frame buffer cannot grow unbounded. Downloads stream
 * blob->client, paced by output-buffer occupancy so a large file never buffers
 * in full. */
typedef enum {
    XFER_NONE = 0,
    XFER_UP_AWAIT_CREATE,   /* sent ATTACH_CREATE; awaiting the id + storage key */
    XFER_UP_AWAIT_OPEN,     /* asked the pool to open the blob sink */
    XFER_UP_ACTIVE,         /* streaming UPLOAD_CHUNKs into the blob */
    XFER_UP_AWAIT_COMMIT,   /* asked the pool to commit the blob */
    XFER_UP_AWAIT_FINAL,    /* blob committed; awaiting ATTACH_FINALIZE */
    XFER_DOWN_AWAIT_LOOKUP, /* sent ATTACH_LOOKUP; awaiting authz + metadata */
    XFER_DOWN_AWAIT_OPEN,   /* asked the pool to open the blob source */
    XFER_DOWN_ACTIVE        /* streaming DOWNLOAD_CHUNKs from the blob */
} xfer_state;

typedef struct {
    xfer_state       state;
    uint64_t         attachment_id;
    uint64_t         declared_size;   /* upload: bytes promised by UPLOAD_BEGIN */
    uint64_t         received;        /* upload: bytes streamed so far */
    uint32_t         next_seq;        /* next expected (upload) / next sent (download) */
    oc_blob_writer  *bw;              /* upload sink */
    mbedtls_sha256_context sha;       /* upload: running digest */
    int              sha_init;        /* sha context needs freeing */
    uint8_t          digest[32];      /* upload: final digest, echoed in UPLOAD_OK */
    oc_blob_reader  *br;              /* download source */
    uint64_t         remaining;       /* download: bytes still to send */
    /* A blob job is with a worker. While set, the handle above belongs to that
     * job and must NOT be released here (xferpool.h), and the connection stops
     * being read so the client cannot outrun the store (ARCH-69). */
    int              in_flight;
    /* DOWNLOAD_INFO fields, stashed from the DB result because it is freed
     * before the blob finishes opening. */
    char            *dl_filename;
    char            *dl_mime;
    uint8_t          dl_sha[32];
    int              dl_have_sha;
} conn_xfer;

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
    uint8_t      presence;       /* OC_PRESENCE_ONLINE / _AWAY (per connection) */
    conn_xfer    xfer;           /* in-flight attachment transfer, if any */
    /* HTTP mode (ARCH-32/54): a connection that did not negotiate the oc/1 ALPN
     * is a webhook/HTTP client, not a binary-protocol peer. `hin` accumulates the
     * request; `http_pending` marks it as awaiting a webhook-post result. */
    int          http;
    uint8_t     *hin;
    size_t       hlen, hcap;
    int          http_pending;
} conn;

/* Scratch for encoding one outgoing frame; net thread only, so a single static
 * buffer is safe and avoids per-send allocation (bodies can be ~64KB). */
static uint8_t g_enc[OC_MAX_FRAME_SIZE];
static uint64_t g_next_conn_id = 1;

/* Attachment blob store + the upload size cap (ARCH-70). Set once at the top of
 * oc_netloop_run; the loop is single-threaded so file-scope state is safe, as
 * with g_enc/g_next_conn_id. */
static oc_blobstore *g_blobs;
static oc_xferpool  *g_xfers;   /* blob I/O off the net thread (ARCH-69) */
/* Storage maintenance (ARCH-78): policy, the last free-space sample, and when
 * the pass last ran. The sample is refreshed by the pass and read by the upload
 * admission check, so a refusal never costs a statvfs on the hot path. */
static oc_storage_policy g_spol;
static oc_storage_stats  g_sstat;
static uint64_t          g_last_maint_ms;
static char              g_blob_dir[1024];
static uint64_t      g_max_attach = OC_MAX_ATTACHMENT_SIZE;

/* Per-webhook-token rate limit for the incoming-webhook endpoint (REQ-170). A
 * fixed window keyed by the token, so one noisy integration can't flood a
 * channel. Created in oc_netloop_run. */
static oc_ratelimit *g_webhook_rl;
#define OC_WEBHOOK_RATE_MAX     60u
#define OC_WEBHOOK_RATE_WINDOW  60000u

/* Release any transfer state on a connection and reset it. A still-open upload
 * writer is aborted (its staged bytes discarded), so an interrupted upload never
 * publishes a partial blob.
 *
 * Two rules make this safe now that blob I/O is asynchronous (ARCH-69):
 *
 *   1. If a job is in flight, the handle belongs to that job, not to us. The
 *      completion path finds the connection gone and releases it there.
 *      Aborting it here would be a double free — this is the sharp edge of the
 *      whole change, and it is why `in_flight` exists.
 *   2. Otherwise we own the handle, but must not release it inline: put_abort
 *      and get_close can both block on an S3 round trip, which is exactly what
 *      this work exists to keep off the net thread. So it goes to the pool as a
 *      fire-and-forget job (conn_id 0), which runs it and frees it with no
 *      result coming back. */
static void xfer_reset(conn_xfer *x) {
    if (!x->in_flight) {
        if (x->bw) {
            oc_xfer_job *j = oc_xfer_job_new(OC_XFER_ABORT, 0);
            if (j) { j->bw = x->bw; oc_xferpool_submit(g_xfers, j); }
        }
        if (x->br) {
            oc_xfer_job *j = oc_xfer_job_new(OC_XFER_CLOSE, 0);
            if (j) { j->br = x->br; oc_xferpool_submit(g_xfers, j); }
        }
    }
    x->bw = NULL;
    x->br = NULL;
    if (x->sha_init) { mbedtls_sha256_free(&x->sha); x->sha_init = 0; }
    free(x->dl_filename);
    free(x->dl_mime);

    /* `in_flight` must SURVIVE the reset. It describes the connection, not the
     * transfer: a job is still out with a worker, and the memset below would
     * otherwise report the connection as idle. It would then un-pause, accept a
     * new UPLOAD_BEGIN, and put a second operation in flight — breaking the
     * one-op-per-transfer rule the whole design rests on, and ending in a
     * double abort of the same handle. Keeping the flag set holds the
     * connection paused until the outstanding completion lands, which then
     * clears it. */
    int outstanding = x->in_flight;
    memset(x, 0, sizeof *x);
    x->in_flight = outstanding;
}

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
    /* Pending output, or an active download with bytes still to stream, both need
     * writability. epoll here is level-triggered, so keeping EPOLLOUT set while a
     * download has bytes left drives the pump each time the socket is writable and
     * naturally stalls when its buffer fills (backpressure, ARCH-69). */
    if (c->out_len > c->out_sent ||
        (c->xfer.state == XFER_DOWN_ACTIVE && c->xfer.remaining > 0))
        ev |= EPOLLOUT;
    /* Blob job in flight: stop reading this connection so a client streaming
     * faster than the store can absorb is throttled by TCP itself, and so no
     * further push can overflow the frame buffer while the drain is paused
     * (ARCH-69). */
    if (c->xfer.in_flight) ev &= ~(uint32_t)EPOLLIN;
    conn_set_events(ep, c, ev);
}

/* Broadcast that a user has gone offline if the just-closed connection was their
 * last one (REQ-120). Defined below; declared here for conn_close. */
static void presence_offline_if_gone(int ep, conn **conns, uint64_t user_id);
/* Drop a closed connection from any audio call it was in (REQ-152). Defined
 * below; declared here for conn_close. */
static void call_conn_closed(int ep, conn **conns, uint64_t conn_id);
/* Append + flush to a connection (closes it on error). Defined below; declared
 * here for the call roster helpers. */
static void send_bytes(int ep, conn **conns, int fd, const uint8_t *buf, size_t len);

static void conn_close(int ep, conn **conns, int fd) {
    conn *c = conns[fd];
    if (!c) return;
    uint64_t uid = c->user_id;
    uint64_t cid = c->conn_id;
    int was_authed = c->authed;
    epoll_ctl(ep, EPOLL_CTL_DEL, fd, NULL);
    xfer_reset(&c->xfer);
    oc_tls_conn_free(&c->tls);
    oc_framebuf_free(&c->fb);
    free(c->out);
    free(c->hin);
    close(fd);
    free(c);
    conns[fd] = NULL;
    if (was_authed) presence_offline_if_gone(ep, conns, uid);
    call_conn_closed(ep, conns, cid);   /* leave any call; roster remaining (REQ-152) */
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

/* --- Presence (REQ-120, in-memory net-thread state, ARCH-67) ------------ */

/* A user's aggregate presence across their connections: online if any is online,
 * away if all connected are away, offline if none are connected. */
static uint8_t presence_of(conn **conns, uint64_t uid) {
    uint8_t st = OC_PRESENCE_OFFLINE;
    for (int fd = 0; fd < OC_NETLOOP_MAX_FD; fd++) {
        conn *c = conns[fd];
        if (c && c->authed && c->user_id == uid) {
            if (c->presence == OC_PRESENCE_ONLINE) return OC_PRESENCE_ONLINE;
            st = OC_PRESENCE_AWAY;
        }
    }
    return st;
}

/* Best-effort append+flush that never closes the connection — so presence fan-out
 * can't recurse into conn_close (which itself broadcasts presence). A dead
 * connection is reaped on its next epoll event. */
static void presence_send(int ep, conn *c, const uint8_t *buf, size_t len) {
    if (!c) return;
    if (out_append(c, buf, len) == 0) { flush_out(c); update_interest(ep, c); }
}

static void encode_presence(uint8_t *buf, size_t cap, size_t *outlen, uint64_t uid, uint8_t status) {
    oc_wbuf w; oc_wbuf_init(&w, buf, cap);
    oc_presence_update pu = { uid, status };
    oc_encode_presence_update(&w, OC_PROTOCOL_VERSION, &pu);
    *outlen = w.len;
}

/* Tell every other authenticated connection that `uid` is now `status`
 * (tenant-wide). The subject's own connections are skipped — a client tracks its
 * own presence locally. */
static void broadcast_presence(int ep, conn **conns, uint64_t uid, uint8_t status) {
    uint8_t buf[32]; size_t len = 0;
    encode_presence(buf, sizeof buf, &len, uid, status);
    for (int fd = 0; fd < OC_NETLOOP_MAX_FD; fd++)
        if (conns[fd] && conns[fd]->authed && conns[fd]->user_id != uid)
            presence_send(ep, conns[fd], buf, len);
}

static void presence_offline_if_gone(int ep, conn **conns, uint64_t user_id) {
    if (presence_of(conns, user_id) == OC_PRESENCE_OFFLINE)
        broadcast_presence(ep, conns, user_id, OC_PRESENCE_OFFLINE);
}

/* --- Audio calls (REQ-150, ephemeral net-thread state) ------------------ */

/* One call per channel; call_id == channel_id. State is in-memory on the net
 * thread (like presence, ARCH-67) — calls reset on restart. */
#define OC_MAX_CALLS 64
typedef struct { uint64_t user_id, conn_id; uint8_t token[OC_AUDIO_TOKEN_LEN]; } call_part;
typedef struct { uint64_t channel_id; call_part parts[OC_MAX_CALL_PARTICIPANTS]; int n; } call_t;
static call_t g_calls[OC_MAX_CALLS];   /* channel_id == 0 marks a free slot */

/* Audio sidecar (ARCH-31): the IPC socket to it and the UDP port it listens on,
 * set by oc_netloop_set_audio before the loop runs. ipc_fd < 0 => no sidecar
 * (calls still form, but with no media endpoint). */
static int      g_audio_ipc = -1;
static uint16_t g_audio_udp_port;

void oc_netloop_set_audio(int ipc_fd, uint16_t udp_port) {
    g_audio_ipc = ipc_fd;
    g_audio_udp_port = udp_port;
}

/* Outbound push emitter (ARCH-85), NULL = push disabled. Set before the loop. */
static oc_push *g_push;

void oc_netloop_set_push(struct oc_push *push) {
    g_push = push;
}

/* Send one length-prefixed IPC message (type + payload) to the sidecar. */
static void audio_ipc_send(uint8_t type, const uint8_t *payload, size_t plen) {
    if (g_audio_ipc < 0) return;
    uint8_t buf[64];
    if (5 + plen > sizeof buf) return;
    uint32_t mlen = (uint32_t)(1 + plen);
    buf[0] = (uint8_t)(mlen >> 24); buf[1] = (uint8_t)(mlen >> 16);
    buf[2] = (uint8_t)(mlen >> 8);  buf[3] = (uint8_t)mlen;
    buf[4] = type;
    memcpy(buf + 5, payload, plen);
    ssize_t n = write(g_audio_ipc, buf, 5 + plen); (void)n;   /* best-effort */
}

static void audio_authorize(uint64_t call_id, uint64_t user_id, const uint8_t *token) {
    uint8_t p[16 + OC_AUDIO_TOKEN_LEN];
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(call_id >> (56 - 8 * i));
    for (int i = 0; i < 8; i++) p[8 + i] = (uint8_t)(user_id >> (56 - 8 * i));
    memcpy(p + 16, token, OC_AUDIO_TOKEN_LEN);
    audio_ipc_send(OC_AUDIO_IPC_AUTHORIZE, p, sizeof p);
}

static void audio_revoke(const uint8_t *token) {
    audio_ipc_send(OC_AUDIO_IPC_REVOKE, token, OC_AUDIO_TOKEN_LEN);
}

static call_t *call_find(uint64_t channel_id) {
    if (!channel_id) return NULL;
    for (int i = 0; i < OC_MAX_CALLS; i++)
        if (g_calls[i].channel_id == channel_id) return &g_calls[i];
    return NULL;
}

static call_t *call_get_or_create(uint64_t channel_id) {
    call_t *c = call_find(channel_id);
    if (c) return c;
    for (int i = 0; i < OC_MAX_CALLS; i++)
        if (g_calls[i].channel_id == 0) { g_calls[i].channel_id = channel_id; g_calls[i].n = 0; return &g_calls[i]; }
    return NULL;   /* all call slots busy */
}

/* Add a user (or refresh their connection) in a call with their media token.
 * 1 ok, 0 if full. */
static int call_add(call_t *c, uint64_t user_id, uint64_t conn_id, const uint8_t *token) {
    for (int i = 0; i < c->n; i++)
        if (c->parts[i].user_id == user_id) {
            c->parts[i].conn_id = conn_id;
            memcpy(c->parts[i].token, token, OC_AUDIO_TOKEN_LEN);
            return 1;
        }
    if (c->n >= (int)OC_MAX_CALL_PARTICIPANTS) return 0;
    c->parts[c->n].user_id = user_id; c->parts[c->n].conn_id = conn_id;
    memcpy(c->parts[c->n].token, token, OC_AUDIO_TOKEN_LEN);
    c->n++;
    return 1;
}

/* Remove the participant on `conn_id` from whatever call it is in; returns that
 * call's channel_id (for a roster push) or 0, and copies the removed token to
 * `out_token` (for a sidecar revoke). Frees the call if it empties. */
static uint64_t call_remove_conn(uint64_t conn_id, uint8_t *out_token) {
    for (int i = 0; i < OC_MAX_CALLS; i++) {
        call_t *c = &g_calls[i];
        if (!c->channel_id) continue;
        for (int j = 0; j < c->n; j++)
            if (c->parts[j].conn_id == conn_id) {
                uint64_t ch = c->channel_id;
                if (out_token) memcpy(out_token, c->parts[j].token, OC_AUDIO_TOKEN_LEN);
                c->parts[j] = c->parts[--c->n];
                if (c->n == 0) c->channel_id = 0;
                return ch;
            }
    }
    return 0;
}

/* Send a CALL_ROSTER for `c` to every participant except `except_conn` (0 = all).
 * Snapshots the recipient set first so a send that drops a connection (and thus
 * mutates the call) can't corrupt the iteration. */
static void call_send_roster(int ep, conn **conns, call_t *c, uint64_t except_conn) {
    uint64_t parts[OC_MAX_CALL_PARTICIPANTS], cids[OC_MAX_CALL_PARTICIPANTS];
    int n = c->n;
    for (int i = 0; i < n; i++) { parts[i] = c->parts[i].user_id; cids[i] = c->parts[i].conn_id; }
    oc_call_roster ro = { c->channel_id, c->channel_id, (uint16_t)n, parts };
    oc_wbuf w; oc_wbuf_init(&w, g_enc, sizeof g_enc);
    oc_encode_call_roster(&w, OC_PROTOCOL_VERSION, &ro);
    size_t len = w.len;
    for (int i = 0; i < n; i++) {
        if (cids[i] == except_conn) continue;
        conn *pc = find_by_id(conns, cids[i]);
        if (pc) send_bytes(ep, conns, pc->fd, g_enc, len);
    }
}

static void call_conn_closed(int ep, conn **conns, uint64_t conn_id) {
    uint8_t token[OC_AUDIO_TOKEN_LEN];
    uint64_t ch = call_remove_conn(conn_id, token);
    if (ch) {
        audio_revoke(token);
        call_t *c = call_find(ch);
        if (c) call_send_roster(ep, conns, c, 0);
    }
}

static int in_members(uint64_t uid, const uint64_t *m, size_t n) {
    for (size_t i = 0; i < n; i++) if (m[i] == uid) return 1;
    return 0;
}

/* Fill an on-wire attachment-entry array from the DB's attachment metadata
 * (REQ-140). Returns the count written (capped at OC_MAX_ATTACH). Shared by the
 * BROADCAST and THREAD_REPLY encoders so every recipient — live or via backfill/
 * thread listing — sees the attachments inline. */
static uint16_t fill_attach_entries(oc_attach_entry *out, const oc_attach_meta *att, size_t n) {
    if (n > OC_MAX_ATTACH) n = OC_MAX_ATTACH;
    for (size_t i = 0; i < n; i++) {
        out[i].id = att[i].id;
        out[i].filename = oc_slice_str(att[i].filename ? att[i].filename : "");
        out[i].mime = oc_slice_str(att[i].mime ? att[i].mime : "");
        out[i].size = att[i].size;
        out[i].reclaimed = att[i].reclaimed;
    }
    return (uint16_t)n;
}

static void broadcast_set_attach(oc_broadcast *b, const oc_attach_meta *att, size_t n) {
    b->n_attach = fill_attach_entries(b->attach, att, n);
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

/* --- Attachment transfer (REQ-140/141, ARCH-69) ------------------------- */

/* Soft pending-output target while streaming a download: the pump tops the
 * output buffer up to here and lets the rest follow on the next writable
 * wakeup, so a 100 MiB download never buffers in full. Well under the 1 MiB
 * hard output cap. Also the upload receive window advertised to the client. */
#define OC_DOWNLOAD_SOFT_CAP  (256u * 1024u)
#define OC_UPLOAD_WINDOW      (OC_ATTACH_CHUNK_SIZE * 8u)

/* Non-fatal transfer error carrying the attachment id in `context` (8 bytes,
 * big-endian) so the client can correlate, then reset the transfer. Returns
 * out_append's result (0 keep, -1 buffer full -> caller closes). */
static int send_transfer_error(conn *c, uint64_t aid, uint16_t code) {
    uint8_t ctxb[8];
    for (int i = 0; i < 8; i++) ctxb[i] = (uint8_t)(aid >> (56 - 8 * i));
    uint8_t tmp[96]; oc_wbuf w; oc_wbuf_init(&w, tmp, sizeof tmp);
    oc_slice ctx = { ctxb, 8 };
    oc_error e = { code, 0, ctx, oc_slice_str("transfer error") };
    oc_encode_error(&w, OC_PROTOCOL_VERSION, &e);
    int rc = out_append(c, tmp, w.len);
    xfer_reset(&c->xfer);
    return rc;
}

/* Ask the pool for the next slice of the blob, if the output buffer has room.
 * The read completes asynchronously; xfer_read_done appends the chunk and calls
 * back in here, so the loop is driven by completions rather than by a blocking
 * read on the net thread (ARCH-69). Backpressure is unchanged in spirit: while
 * the output buffer is above the soft cap we simply don't ask for more, and the
 * EPOLLOUT drain calls us again once it empties. */
static void download_pump(conn *c) {
    conn_xfer *x = &c->xfer;
    if (x->state != XFER_DOWN_ACTIVE || x->in_flight) return;
    if (x->remaining == 0) {
        oc_download_end de = { x->attachment_id };
        oc_wbuf w; oc_wbuf_init(&w, g_enc, sizeof g_enc);
        oc_encode_download_end(&w, OC_PROTOCOL_VERSION, &de);
        out_append(c, g_enc, w.len);
        /* Closing can block on S3, so hand it to the pool and forget it. */
        if (x->br) {
            oc_xfer_job *cl = oc_xfer_job_new(OC_XFER_CLOSE, 0);
            if (cl) { cl->br = x->br; oc_xferpool_submit(g_xfers, cl); }
            x->br = NULL;
        }
        x->state = XFER_NONE;
        return;
    }
    if ((c->out_len - c->out_sent) >= OC_DOWNLOAD_SOFT_CAP) return;  /* resume on drain */
    size_t want = x->remaining < OC_ATTACH_CHUNK_SIZE ? (size_t)x->remaining : OC_ATTACH_CHUNK_SIZE;
    oc_xfer_job *j = oc_xfer_job_new(OC_XFER_READ, c->conn_id);
    if (!j) return;
    j->br = x->br;
    j->attachment_id = x->attachment_id;
    j->len = want;
    j->data = malloc(want);
    if (!j->data) { oc_xfer_job_free(j); return; }
    x->in_flight = 1;
    oc_xferpool_submit(g_xfers, j);
}

/* Dispatch every buffered frame. Returns 0 to keep the connection, -1 to close.
 * AUTH/SEND become jobs for the DB writer; their replies arrive asynchronously
 * via deliver_result. */
/* Defined below with the HTTP webhook path; declared here for the redeem path. */
static int hex_decode(const char *hex, size_t hexlen, uint8_t *out, size_t outcap);

static int drain_frames(int ep, conn **conns, conn *c, oc_dbwriter *dbw) {
    const uint8_t *frame; size_t flen;
    for (;;) {
        /* A blob job is with a worker: leave the remaining frames buffered and
         * stop reading (update_interest drops EPOLLIN). The completion handler
         * calls back in here. */
        if (c->xfer.in_flight) return 0;
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
                /* The client holds the hex form (see hex_encode above); accept
                 * the raw bytes too, so a token captured before this change, or
                 * pasted from the daemon's own bootstrap log, still redeems. */
                uint8_t raw[OC_INVITE_TOKEN_LEN];
                const uint8_t *tokp = ri.token.ptr;
                size_t toklen = ri.token.len;
                if (hex_decode((const char *)ri.token.ptr, ri.token.len,
                               raw, sizeof raw) == (int)sizeof raw) {
                    tokp = raw; toklen = sizeof raw;
                }
                int ok = u && pw &&
                         oc_job_set_register(j, u, pw, 0, 0) == 0 &&
                         oc_job_set_token(j, tokp, toklen) == 0;
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
            oc_send s = {0};
            if (oc_decode_send(&p, &s) != OC_OK) return -1;
            if (!send_rate_ok(c)) { if (reject_send_rate(c, s.idem) != 0) return -1; continue; }
            oc_job *j = oc_job_new(OC_JOB_SEND, c->conn_id);
            if (!j) return -1;
            j->user_id = c->user_id;
            j->channel_id = s.channel_id;
            memcpy(j->idem, s.idem, OC_IDEM_LEN);
            if (oc_job_set_body(j, s.body.ptr, s.body.len) != 0) return -1;
            /* Attachments to link to this message (REQ-140). */
            uint16_t na = s.n_attach > OC_MAX_ATTACH ? OC_MAX_ATTACH : s.n_attach;
            for (uint16_t i = 0; i < na; i++) j->attach_ids[i] = s.attach_ids[i];
            j->n_attach = na;
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
        if (hdr.msg_type == OC_MSG_OPEN_DM) {
            oc_open_dm od;
            if (oc_decode_open_dm(&p, &od) != OC_OK) return -1;
            oc_job *j = oc_job_new(OC_JOB_OPEN_DM, c->conn_id);
            if (!j) return -1;
            j->user_id = c->user_id;
            j->target_user_id = od.user_id;
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
        if (hdr.msg_type == OC_MSG_PIN) {
            oc_pin pn;
            if (oc_decode_pin(&p, &pn) != OC_OK) return -1;
            oc_job *j = oc_job_new(OC_JOB_PIN, c->conn_id);
            if (!j) return -1;
            j->user_id = c->user_id;
            j->channel_id = pn.channel_id;
            j->message_id = pn.message_id;
            j->pin_op = pn.op;
            oc_dbwriter_submit(dbw, j);
            continue;
        }
        if (hdr.msg_type == OC_MSG_LIST_PINS) {
            oc_list_pins lp;
            if (oc_decode_list_pins(&p, &lp) != OC_OK) return -1;
            oc_job *j = oc_job_new(OC_JOB_LIST_PINS, c->conn_id);
            if (!j) return -1;
            j->user_id = c->user_id;
            j->channel_id = lp.channel_id;
            oc_dbwriter_submit(dbw, j);
            continue;
        }
        if (hdr.msg_type == OC_MSG_SEND_REPLY) {
            oc_send_reply sr = {0};
            if (oc_decode_send_reply(&p, &sr) != OC_OK) return -1;
            if (!send_rate_ok(c)) { if (reject_send_rate(c, sr.idem) != 0) return -1; continue; }
            oc_job *j = oc_job_new(OC_JOB_SEND_REPLY, c->conn_id);
            if (!j) return -1;
            j->user_id = c->user_id;
            j->channel_id = sr.channel_id;
            j->parent_id = sr.parent_id;
            memcpy(j->idem, sr.idem, OC_IDEM_LEN);
            if (oc_job_set_body(j, sr.body.ptr, sr.body.len) != 0) return -1;
            uint16_t na = sr.n_attach > OC_MAX_ATTACH ? OC_MAX_ATTACH : sr.n_attach;
            for (uint16_t i = 0; i < na; i++) j->attach_ids[i] = sr.attach_ids[i];
            j->n_attach = na;
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
        if (hdr.msg_type == OC_MSG_SET_PRESENCE) {
            oc_set_presence sp;
            if (oc_decode_set_presence(&p, &sp) != OC_OK) return -1;
            /* Only online/away are settable while connected (REQ-120). */
            c->presence = (sp.status == OC_PRESENCE_AWAY) ? OC_PRESENCE_AWAY : OC_PRESENCE_ONLINE;
            broadcast_presence(ep, conns, c->user_id, presence_of(conns, c->user_id));
            continue;
        }
        if (hdr.msg_type == OC_MSG_TYPING) {
            oc_typing t;
            if (oc_decode_typing(&p, &t) != OC_OK) return -1;
            oc_job *j = oc_job_new(OC_JOB_TYPING, c->conn_id);   /* read job -> members */
            if (!j) return -1;
            j->user_id = c->user_id;
            j->channel_id = t.channel_id;
            oc_dbwriter_submit(dbw, j);
            continue;
        }
        if (hdr.msg_type == OC_MSG_CLIENT_ACK) {
            oc_client_ack ca;
            if (oc_decode_client_ack(&p, &ca) != OC_OK) return -1;
            /* Record the delivery cursor (REQ-090); no reply. */
            oc_job *j = oc_job_new(OC_JOB_CLIENT_ACK, c->conn_id);
            if (!j) return -1;
            j->user_id = c->user_id;
            j->channel_id = ca.channel_id;
            j->message_id = ca.message_id;
            oc_dbwriter_submit(dbw, j);
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
        if (hdr.msg_type == OC_MSG_HISTORY_REQUEST) {
            oc_history_request hr;
            if (oc_decode_history_request(&p, &hr) != OC_OK) return -1;
            oc_job *j = oc_job_new(OC_JOB_HISTORY, c->conn_id);
            if (!j) return -1;
            j->user_id      = c->user_id;
            j->channel_id   = hr.channel_id;
            j->message_id   = hr.before_message_id;
            j->search_limit = hr.limit;
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
        if (hdr.msg_type == OC_MSG_CREATE_WEBHOOK) {
            oc_create_webhook cw;
            if (oc_decode_create_webhook(&p, &cw) != OC_OK) return -1;
            oc_job *j = oc_job_new(OC_JOB_CREATE_WEBHOOK, c->conn_id);
            if (!j) return -1;
            j->user_id = c->user_id;
            j->channel_id = cw.channel_id;
            j->ch_name = cw.label.len ? strndup((const char *)cw.label.ptr, cw.label.len) : strdup("");
            if (!j->ch_name) return -1;
            oc_dbwriter_submit(dbw, j);
            continue;
        }
        if (hdr.msg_type == OC_MSG_LIST_WEBHOOKS) {
            oc_list_webhooks lw;
            if (oc_decode_list_webhooks(&p, &lw) != OC_OK) return -1;
            oc_job *j = oc_job_new(OC_JOB_LIST_WEBHOOKS, c->conn_id);
            if (!j) return -1;
            j->user_id = c->user_id; j->channel_id = lw.channel_id;
            oc_dbwriter_submit(dbw, j);
            continue;
        }
        if (hdr.msg_type == OC_MSG_DELETE_WEBHOOK) {
            oc_delete_webhook dw;
            if (oc_decode_delete_webhook(&p, &dw) != OC_OK) return -1;
            oc_job *j = oc_job_new(OC_JOB_DELETE_WEBHOOK, c->conn_id);
            if (!j) return -1;
            j->user_id = c->user_id; j->message_id = dw.webhook_id;   /* id carried in message_id */
            oc_dbwriter_submit(dbw, j);
            continue;
        }
        if (hdr.msg_type == OC_MSG_SET_NOTIFY_PREF) {
            oc_set_notify_pref sp;
            if (oc_decode_set_notify_pref(&p, &sp) != OC_OK) return -1;
            oc_job *j = oc_job_new(OC_JOB_SET_NOTIFY_PREF, c->conn_id);
            if (!j) return -1;
            j->user_id = c->user_id; j->channel_id = sp.channel_id; j->notify_level = sp.level;
            oc_dbwriter_submit(dbw, j);
            continue;
        }
        if (hdr.msg_type == OC_MSG_SET_DND) {
            oc_set_dnd sd;
            if (oc_decode_set_dnd(&p, &sd) != OC_OK) return -1;
            oc_job *j = oc_job_new(OC_JOB_SET_DND, c->conn_id);
            if (!j) return -1;
            j->user_id = c->user_id; j->dnd_enabled = sd.enabled;
            j->dnd_start_min = sd.start_min; j->dnd_end_min = sd.end_min;
            oc_dbwriter_submit(dbw, j);
            continue;
        }
        if (hdr.msg_type == OC_MSG_REGISTER_DEVICE_TOKEN) {
            oc_register_device_token rd;
            if (oc_decode_register_device_token(&p, &rd) != OC_OK) return -1;
            oc_job *j = oc_job_new(OC_JOB_REGISTER_DEVICE_TOKEN, c->conn_id);
            if (!j) return -1;
            size_t tl = rd.token.len < OC_DEVICE_TOKEN_MAX - 1 ? rd.token.len : OC_DEVICE_TOKEN_MAX - 1;
            j->user_id = c->user_id;
            j->device_platform = rd.platform;
            j->device_token = strndup((const char *)rd.token.ptr, tl);
            if (!j->device_token) return -1;
            oc_dbwriter_submit(dbw, j);
            continue;
        }
        if (hdr.msg_type == OC_MSG_UNREGISTER_DEVICE_TOKEN) {
            oc_slice tok;
            if (oc_decode_unregister_device_token(&p, &tok) != OC_OK) return -1;
            oc_job *j = oc_job_new(OC_JOB_UNREGISTER_DEVICE_TOKEN, c->conn_id);
            if (!j) return -1;
            size_t tl = tok.len < OC_DEVICE_TOKEN_MAX - 1 ? tok.len : OC_DEVICE_TOKEN_MAX - 1;
            j->user_id = c->user_id;
            j->device_token = strndup((const char *)tok.ptr, tl);
            if (!j->device_token) return -1;
            oc_dbwriter_submit(dbw, j);
            continue;
        }
        if (hdr.msg_type == OC_MSG_LIST_NOTIFY_PREFS) {
            if (oc_decode_list_notify_prefs(&p) != OC_OK) return -1;
            oc_job *j = oc_job_new(OC_JOB_LIST_NOTIFY_PREFS, c->conn_id);
            if (!j) return -1;
            j->user_id = c->user_id;
            oc_dbwriter_submit(dbw, j);
            continue;
        }
        if (hdr.msg_type == OC_MSG_SET_CLIENT_SETTING) {
            oc_set_client_setting cs;
            if (oc_decode_set_client_setting(&p, &cs) != OC_OK) return -1;
            oc_job *j = oc_job_new(OC_JOB_SET_CLIENT_SETTING, c->conn_id);
            if (!j) return -1;
            size_t ctl = cs.client_type.len < OC_CLIENT_TYPE_MAX ? cs.client_type.len : OC_CLIENT_TYPE_MAX;
            size_t kl  = cs.key.len   < OC_SETTING_KEY_MAX   ? cs.key.len   : OC_SETTING_KEY_MAX;
            size_t vl  = cs.value.len < OC_SETTING_VALUE_MAX ? cs.value.len : OC_SETTING_VALUE_MAX;
            j->user_id = c->user_id;
            j->cs_client_type = strndup((const char *)cs.client_type.ptr, ctl);
            j->cs_key   = strndup((const char *)cs.key.ptr, kl);
            j->cs_value = strndup((const char *)cs.value.ptr, vl);
            if (!j->cs_client_type || !j->cs_key || !j->cs_value) return -1;
            oc_dbwriter_submit(dbw, j);
            continue;
        }
        if (hdr.msg_type == OC_MSG_LIST_CLIENT_SETTINGS) {
            oc_list_client_settings ls;
            if (oc_decode_list_client_settings(&p, &ls) != OC_OK) return -1;
            oc_job *j = oc_job_new(OC_JOB_LIST_CLIENT_SETTINGS, c->conn_id);
            if (!j) return -1;
            size_t ctl = ls.client_type.len < OC_CLIENT_TYPE_MAX ? ls.client_type.len : OC_CLIENT_TYPE_MAX;
            j->user_id = c->user_id;
            j->cs_client_type = strndup((const char *)ls.client_type.ptr, ctl);
            if (!j->cs_client_type) return -1;
            oc_dbwriter_submit(dbw, j);
            continue;
        }
        if (hdr.msg_type == OC_MSG_SET_DISPLAY_NAME) {
            oc_set_display_name sn;
            if (oc_decode_set_display_name(&p, &sn) != OC_OK) return -1;
            oc_job *j = oc_job_new(OC_JOB_SET_DISPLAY_NAME, c->conn_id);
            if (!j) return -1;
            size_t nl = sn.name.len < OC_MAX_DISPLAY_NAME ? sn.name.len : OC_MAX_DISPLAY_NAME;
            j->user_id = c->user_id;
            j->pf_name = strndup((const char *)sn.name.ptr, nl);
            if (!j->pf_name) return -1;
            oc_dbwriter_submit(dbw, j);
            continue;
        }
        if (hdr.msg_type == OC_MSG_CHANGE_PASSWORD) {
            oc_change_password cp;
            if (oc_decode_change_password(&p, &cp) != OC_OK) return -1;
            oc_job *j = oc_job_new(OC_JOB_CHANGE_PASSWORD, c->conn_id);
            if (!j) return -1;
            j->user_id = c->user_id;
            j->pf_old_pw = strndup((const char *)cp.old_password.ptr, cp.old_password.len);
            j->pf_new_pw = strndup((const char *)cp.new_password.ptr, cp.new_password.len);
            if (!j->pf_old_pw || !j->pf_new_pw) return -1;
            oc_dbwriter_submit(dbw, j);
            continue;
        }
        if (hdr.msg_type == OC_MSG_CALL_JOIN) {
            oc_call_join cj;
            if (oc_decode_call_join(&p, &cj) != OC_OK) return -1;
            oc_job *j = oc_job_new(OC_JOB_CALL_AUTH, c->conn_id);   /* read job: access gate */
            if (!j) return -1;
            j->user_id = c->user_id; j->channel_id = cj.channel_id;
            oc_dbwriter_submit(dbw, j);
            continue;
        }
        if (hdr.msg_type == OC_MSG_CALL_LEAVE) {
            oc_call_leave cl;
            if (oc_decode_call_leave(&p, &cl) != OC_OK) return -1;
            uint8_t token[OC_AUDIO_TOKEN_LEN];
            uint64_t ch = call_remove_conn(c->conn_id, token);
            if (ch) {
                audio_revoke(token);
                call_t *cc = call_find(ch);
                if (cc) call_send_roster(ep, conns, cc, 0);
            }
            continue;
        }
        if (hdr.msg_type == OC_MSG_AUDIT_QUERY) {
            oc_audit_query aq;
            if (oc_decode_audit_query(&p, &aq) != OC_OK) return -1;
            /* Owner/admin only; checked in the writer against the current role. */
            oc_job *j = oc_job_new(OC_JOB_AUDIT_QUERY, c->conn_id);
            if (!j) return -1;
            j->user_id = c->user_id;
            j->audit_limit = aq.limit ? aq.limit : 50;
            j->audit_before_ms = aq.before_ms;
            oc_dbwriter_submit(dbw, j);
            continue;
        }
        if (hdr.msg_type == OC_MSG_STORAGE_STATUS_REQ) {
            /* Owner/admin only (REQ-214). The check lives in the writer, which
             * reads the user's CURRENT role, rather than here — the same shape
             * as SET_ROLE and the other admin operations. A role revoked
             * mid-session therefore takes effect immediately. */
            oc_job *j = oc_job_new(OC_JOB_STORAGE_STATUS, c->conn_id);
            if (!j) return -1;
            j->user_id = c->user_id;
            oc_dbwriter_submit(dbw, j);
            continue;
        }
        if (hdr.msg_type == OC_MSG_UPLOAD_BEGIN) {
            oc_upload_begin ub;
            if (oc_decode_upload_begin(&p, &ub) != OC_OK) return -1;
            if (c->xfer.state != XFER_NONE) {
                if (send_transfer_error(c, 0, OC_ERR_TRANSFER_PROTOCOL) != 0) return -1;
                continue;
            }
            if (ub.total_size > g_max_attach) {
                if (send_transfer_error(c, 0, OC_ERR_ATTACHMENT_TOO_LARGE) != 0) return -1;
                continue;
            }
            /* Admission control (REQ-216): refuse at declaration, before a byte
             * moves, rather than failing a half-finished transfer. The reserve
             * below this point belongs to SQLite — spending it on attachments is
             * what would turn "attachments unavailable" into "chat down"
             * (REQ-212). Uses the cached sample, so this costs no syscall. */
            if (oc_storage_must_refuse(&g_sstat, &g_spol)) {
                if (send_transfer_error(c, 0, OC_ERR_STORAGE_FULL) != 0) return -1;
                continue;
            }
            oc_job *j = oc_job_new(OC_JOB_ATTACH_CREATE, c->conn_id);
            if (!j) return -1;
            j->user_id = c->user_id;
            j->channel_id = ub.channel_id;
            memcpy(j->idem, ub.idem, OC_IDEM_LEN);
            j->att_size = ub.total_size;
            j->filename = ub.filename.len ? strndup((const char *)ub.filename.ptr, ub.filename.len) : strdup("");
            j->mime     = ub.mime.len     ? strndup((const char *)ub.mime.ptr, ub.mime.len)         : strdup("");
            if (!j->filename || !j->mime) { return -1; }
            oc_dbwriter_submit(dbw, j);
            c->xfer.state = XFER_UP_AWAIT_CREATE;
            c->xfer.declared_size = ub.total_size;
            continue;
        }
        if (hdr.msg_type == OC_MSG_UPLOAD_CHUNK) {
            oc_upload_chunk uc;
            if (oc_decode_upload_chunk(&p, &uc) != OC_OK) return -1;
            conn_xfer *x = &c->xfer;
            if (x->state != XFER_UP_ACTIVE || uc.attachment_id != x->attachment_id ||
                uc.seq != x->next_seq || uc.data.len > OC_ATTACH_CHUNK_SIZE ||
                x->received + uc.data.len > x->declared_size) {
                if (send_transfer_error(c, uc.attachment_id, OC_ERR_TRANSFER_PROTOCOL) != 0) return -1;
                continue;
            }
            /* Hand the bytes to a worker. The job owns a copy because the frame
             * buffer is reused, and because the write may outlive this frame.
             * UPLOAD_ACK is sent on completion, so an ack now means "durably
             * written" rather than merely "received". */
            oc_xfer_job *wj = oc_xfer_job_new(OC_XFER_WRITE, c->conn_id);
            if (!wj) return -1;
            wj->bw = x->bw;
            wj->attachment_id = x->attachment_id;
            wj->len = uc.data.len;
            if (uc.data.len) {
                wj->data = malloc(uc.data.len);
                if (!wj->data) { oc_xfer_job_free(wj); return -1; }
                memcpy(wj->data, uc.data.ptr, uc.data.len);
            }
            x->in_flight = 1;
            oc_xferpool_submit(g_xfers, wj);
            /* Stop draining here. Frames already buffered stay buffered, and
             * update_interest drops EPOLLIN, so no further push can overflow the
             * frame buffer while we are paused (framebuf.h). Draining resumes
             * from the completion handler. */
            return 0;
        }
        if (hdr.msg_type == OC_MSG_UPLOAD_END) {
            oc_upload_end ue;
            if (oc_decode_upload_end(&p, &ue) != OC_OK) return -1;
            conn_xfer *x = &c->xfer;
            if (x->state != XFER_UP_ACTIVE || ue.attachment_id != x->attachment_id ||
                x->received != x->declared_size) {
                if (send_transfer_error(c, ue.attachment_id, OC_ERR_TRANSFER_PROTOCOL) != 0) return -1;
                continue;
            }
            mbedtls_sha256_finish(&x->sha, x->digest);
            mbedtls_sha256_free(&x->sha); x->sha_init = 0;
            /* Committing can be a full request/response against S3, so it goes
             * to the pool; the ATTACH_FINALIZE job is submitted when it lands.
             * Reaching here means no write is in flight: the read pause above
             * guarantees UPLOAD_END is only dispatched once the previous chunk
             * has completed. */
            oc_xfer_job *cj = oc_xfer_job_new(OC_XFER_COMMIT, c->conn_id);
            if (!cj) return -1;
            cj->bw = x->bw;
            /* COMMIT *consumes* the writer (oc_blob_put_commit frees it), so
             * ownership moves to the job here. Leaving x->bw set would let a
             * later xfer_reset abort an already-freed writer — the handle must
             * be reachable from exactly one place at a time. */
            x->bw = NULL;
            cj->attachment_id = x->attachment_id;
            x->state = XFER_UP_AWAIT_COMMIT;
            x->in_flight = 1;
            oc_xferpool_submit(g_xfers, cj);
            return 0;
        }
        if (hdr.msg_type == OC_MSG_DOWNLOAD_BEGIN) {
            oc_download_begin db;
            if (oc_decode_download_begin(&p, &db) != OC_OK) return -1;
            if (c->xfer.state != XFER_NONE) {
                if (send_transfer_error(c, db.attachment_id, OC_ERR_TRANSFER_PROTOCOL) != 0) return -1;
                continue;
            }
            oc_job *j = oc_job_new(OC_JOB_ATTACH_LOOKUP, c->conn_id);
            if (!j) return -1;
            j->user_id = c->user_id;
            j->attachment_id = db.attachment_id;
            oc_dbwriter_submit(dbw, j);
            c->xfer.state = XFER_DOWN_AWAIT_LOOKUP;
            c->xfer.attachment_id = db.attachment_id;
            continue;
        }
        if (hdr.msg_type == OC_MSG_TRANSFER_CANCEL) {
            oc_transfer_cancel tc;
            if (oc_decode_transfer_cancel(&p, &tc) != OC_OK) return -1;
            xfer_reset(&c->xfer);   /* aborts an open upload blob; drops a download */
            continue;
        }
        /* Other post-auth frame types are ignored by this skeleton. */
    }
}

/* --- HTTP / incoming webhooks (ARCH-32, REQ-170) ------------------------ */

/* Total request bytes we'll buffer for an HTTP connection: a full body plus
 * generous header headroom. Beyond this the request is refused (413). */
#define OC_HTTP_MAX_REQUEST (OC_MAX_BODY_SIZE + 16384u)

static int hexval(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

/* Tokens are random BYTES on the daemon side but must reach a human as text:
 * an invite gets shared in a message, and a webhook token goes into a URL —
 * /webhook/<hex-token> already assumed hex at the HTTP end. Both were being sent
 * to clients verbatim, so what the UI displayed was unreadable and, for
 * webhooks, would never have matched the endpoint. Encode on the way out and
 * decode on the way back in. */
static void hex_encode(const uint8_t *in, size_t n, char *out) {
    static const char H[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[2 * i] = H[in[i] >> 4]; out[2 * i + 1] = H[in[i] & 15]; }
    out[2 * n] = '\0';
}

static int hex_decode(const char *hex, size_t hexlen, uint8_t *out, size_t outcap) {
    if (hexlen % 2 != 0 || hexlen / 2 > outcap) return -1;
    for (size_t i = 0; i < hexlen; i += 2) {
        int hi = hexval(hex[i]), lo = hexval(hex[i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i / 2] = (uint8_t)((hi << 4) | lo);
    }
    return (int)(hexlen / 2);
}

/* Queue an HTTP/1.1 response (Connection: close) into the output buffer. */
static void http_reply(conn *c, int status, const char *reason,
                       const char *ctype, const char *body, size_t blen) {
    char hdr[256];
    int n = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n",
        status, reason, ctype, blen);
    if (n < 0 || n >= (int)sizeof hdr) return;
    out_append(c, (const uint8_t *)hdr, (size_t)n);
    if (body && blen) out_append(c, (const uint8_t *)body, blen);
}

/* Dispatch one fully-parsed HTTP request. The only route is
 * POST /webhook/<hex-token>; everything else gets a terminal status. Returns 0
 * to keep the connection (a webhook post is awaiting its result) or -1 to close
 * (a response has been queued). */
static int on_http_request(conn *c, const oc_http_req *req, oc_dbwriter *dbw) {
    if (req->method_len != 4 || memcmp(req->method, "POST", 4) != 0) {
        http_reply(c, 405, "Method Not Allowed", "text/plain", "method not allowed\n", 19);
        return -1;
    }
    static const char PFX[] = "/webhook/";
    size_t pfx = sizeof PFX - 1;
    if (req->path_len <= pfx || memcmp(req->path, PFX, pfx) != 0) {
        http_reply(c, 404, "Not Found", "text/plain", "not found\n", 10);
        return -1;
    }
    const char *tokhex = req->path + pfx;
    size_t tokhexlen = req->path_len - pfx;
    const char *q = memchr(tokhex, '?', tokhexlen);      /* drop any query string */
    if (q) tokhexlen = (size_t)(q - tokhex);

    uint8_t token[OC_SESSION_TOKEN_LEN];
    if (hex_decode(tokhex, tokhexlen, token, sizeof token) != (int)sizeof token) {
        http_reply(c, 404, "Not Found", "text/plain", "not found\n", 10);
        return -1;
    }
    const char *text = NULL; size_t tlen = 0;
    if (!oc_http_webhook_text(req, &text, &tlen) || tlen == 0) {
        http_reply(c, 400, "Bad Request", "text/plain", "empty message\n", 14);
        return -1;
    }
    if (tlen > OC_MAX_BODY_SIZE) tlen = OC_MAX_BODY_SIZE;

    char key[2 * OC_SESSION_TOKEN_LEN + 1];
    size_t klen = tokhexlen < sizeof key - 1 ? tokhexlen : sizeof key - 1;
    memcpy(key, tokhex, klen); key[klen] = '\0';
    uint64_t now = now_ms();
    if (g_webhook_rl && oc_ratelimit_blocked(g_webhook_rl, key, now)) {
        http_reply(c, 429, "Too Many Requests", "text/plain", "rate limited\n", 13);
        return -1;
    }
    if (g_webhook_rl) oc_ratelimit_record(g_webhook_rl, key, now);

    oc_job *j = oc_job_new(OC_JOB_WEBHOOK_POST, c->conn_id);
    if (!j || oc_job_set_token(j, token, sizeof token) != 0 ||
        oc_job_set_body(j, text, tlen) != 0) {
        http_reply(c, 500, "Internal Server Error", "text/plain", "error\n", 6);
        return -1;
    }
    oc_dbwriter_submit(dbw, j);
    c->http_pending = 1;       /* response written when the post result returns */
    return 0;
}

/* Read for an HTTP (non-oc/1) connection: accumulate the request, parse, dispatch.
 * Returns 0 to keep, -1 to close. */
static int on_http_readable(conn *c, oc_dbwriter *dbw) {
    for (;;) {
        uint8_t chunk[OC_READ_CHUNK];
        size_t n = 0;
        oc_tls_status st = oc_tls_read(&c->tls, chunk, sizeof chunk, &n);
        if (st == OC_TLS_WANT_READ || st == OC_TLS_WANT_WRITE) return 0;
        if (st == OC_TLS_CLOSED || st == OC_TLS_ERROR) return -1;
        if (c->http_pending) continue;              /* already dispatched; drain quietly */
        if (c->hlen + n > OC_HTTP_MAX_REQUEST) {
            http_reply(c, 413, "Payload Too Large", "text/plain", "too large\n", 10);
            return -1;
        }
        if (c->hlen + n > c->hcap) {
            size_t nc = c->hcap ? c->hcap : 4096;
            while (nc < c->hlen + n) nc *= 2;
            uint8_t *g = realloc(c->hin, nc);
            if (!g) return -1;
            c->hin = g; c->hcap = nc;
        }
        memcpy(c->hin + c->hlen, chunk, n);
        c->hlen += n;

        oc_http_req req;
        int pr = oc_http_parse((const char *)c->hin, c->hlen, OC_MAX_BODY_SIZE, &req);
        if (pr < 0) { http_reply(c, 400, "Bad Request", "text/plain", "bad request\n", 12); return -1; }
        if (pr == 0) continue;                      /* need more bytes */
        return on_http_request(c, &req, dbw);       /* complete request */
    }
}

/* Read, reassemble, and dispatch. Returns 0 to keep, -1 to close. */
static int on_readable(int ep, conn **conns, conn *c, oc_dbwriter *dbw) {
    if (c->http) return on_http_readable(c, dbw);
    for (;;) {
        uint8_t chunk[OC_READ_CHUNK];
        size_t n = 0;
        oc_tls_status st = oc_tls_read(&c->tls, chunk, sizeof chunk, &n);
        if (st == OC_TLS_WANT_READ || st == OC_TLS_WANT_WRITE) return 0;
        if (st == OC_TLS_CLOSED || st == OC_TLS_ERROR) return -1;
        if (oc_framebuf_push(&c->fb, chunk, n) != 0) return -1;
        if (drain_frames(ep, conns, c, dbw) < 0) return -1;
        /* A blob job went in flight, so the drain is paused. Stop reading here
         * too: dropping EPOLLIN only governs the next epoll wakeup, and this
         * loop would otherwise keep pushing frames nobody is draining until the
         * frame buffer overflows (framebuf.h sizes it for drain-after-push).
         * The completion handler resumes both. */
        if (c->xfer.in_flight) return 0;
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
        c->presence = OC_PRESENCE_ONLINE;
        int fd = c->fd;
        uint64_t uid = r->user_id;
        oc_wbuf_init(&w, g_enc, sizeof g_enc);
        /* Fresh auth carries the session token; a session re-auth omits it
         * (PROTOCOL.md §4.3). */
        oc_slice tok = r->has_session_token
                     ? (oc_slice){ r->session_token, OC_SESSION_TOKEN_LEN }
                     : (oc_slice){ NULL, 0 };
        oc_auth_ok m = { r->user_id, r->role, r->session_expiry, tok };
        oc_encode_auth_ok(&w, OC_PROTOCOL_VERSION, &m);
        send_bytes(ep, conns, fd, g_enc, w.len);
        if (!conns[fd]) break;   /* dropped on the AUTH_OK write */

        /* Workspace facts from static config (deployment mode / user cap / name),
         * so the client can render a branded header instead of a bare host. */
        {
            const oc_config *cfg = oc_config_get();
            oc_wbuf_init(&w, g_enc, sizeof g_enc);
            oc_workspace_info wi = { (uint8_t)cfg->deployment_mode,
                                     (uint32_t)(cfg->max_users > 0 ? cfg->max_users : 0),
                                     oc_slice_str(cfg->workspace_name ? cfg->workspace_name : "") };
            oc_encode_workspace_info(&w, OC_PROTOCOL_VERSION, &wi);
            send_bytes(ep, conns, fd, g_enc, w.len);
            if (!conns[fd]) break;   /* dropped on the WORKSPACE_INFO write */
        }

        /* Presence (REQ-120): send the new client a snapshot of who is currently
         * online/away, then — if this is the user's first connection — announce
         * them online to everyone. */
        for (int f = 0; f < OC_NETLOOP_MAX_FD; f++) {
            conn *v = conns[f];
            if (!v || !v->authed || v->user_id == uid) continue;
            int first = 1;
            for (int g = 0; g < f; g++)
                if (conns[g] && conns[g]->authed && conns[g]->user_id == v->user_id) { first = 0; break; }
            if (!first) continue;
            uint8_t pbuf[32]; size_t plen = 0;
            encode_presence(pbuf, sizeof pbuf, &plen, v->user_id, presence_of(conns, v->user_id));
            presence_send(ep, conns[fd], pbuf, plen);
        }
        int others = 0;
        for (int f = 0; f < OC_NETLOOP_MAX_FD; f++)
            if (conns[f] && conns[f]->authed && conns[f]->user_id == uid && conns[f] != conns[fd]) { others = 1; break; }
        if (!others) broadcast_presence(ep, conns, uid, OC_PRESENCE_ONLINE);
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
            oc_broadcast b = { r->message_id, r->channel_id, r->author_id, r->server_time, body, 0, {{0}}, {0} };
            broadcast_set_attach(&b, r->attach, r->n_attach);
            b.author_name = oc_slice_str(r->author_name ? r->author_name : "");
            oc_encode_broadcast(&w, OC_PROTOCOL_VERSION, &b);
            size_t blen = w.len;
            for (int fd = 0; fd < OC_NETLOOP_MAX_FD; fd++) {
                conn *c = conns[fd];
                if (c && c->authed && in_members(c->user_id, r->members, r->n_members))
                    send_bytes(ep, conns, fd, g_enc, blen);
            }
            /* Offline mobile delivery (ARCH-85): hand the notify decision to the
             * push emitter — members minus author, level/DND-gated, off this
             * thread. Fire-and-forget; a no-op when push is unconfigured. */
            oc_push_notify(g_push, r->channel_id, r->author_id, r->message_id);
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
    case OC_RES_DEVICE_TOKEN_OK:
    case OC_RES_DEVICE_TOKEN_ERR: {
        conn *c = find_by_id(conns, r->conn_id);
        if (!c) break;
        oc_wbuf_init(&w, g_enc, sizeof g_enc);
        oc_device_token_ack ack = { (uint8_t)(r->type == OC_RES_DEVICE_TOKEN_OK ? 1 : 0), r->err_code };
        oc_encode_device_token_ack(&w, OC_PROTOCOL_VERSION, &ack);
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
        /* Ack the actor with the channel's state. For a DM, peer_id is the other
         * participant (from the actor's view). */
        conn *c = find_by_id(conns, r->conn_id);
        uint64_t actor = c ? c->user_id : 0;
        if (c) {
            oc_wbuf_init(&w, g_enc, sizeof g_enc);
            oc_channel_info ci = { r->channel_id, r->ch_kind,
                                   oc_slice_str(r->ch_name ? r->ch_name : ""),
                                   r->ch_is_public, r->ch_joined, r->ch_created_at, r->ch_peer };
            oc_encode_channel_info(&w, OC_PROTOCOL_VERSION, &ci);
            send_bytes(ep, conns, c->fd, g_enc, w.len);
        }
        /* Push the (now-member) channel to the target: an INVITE (regular channel)
         * or the peer of a new DM. For a DM the peer, from the target's view, is
         * the actor. */
        if (r->push_user_id) {
            uint64_t push_peer = (r->ch_kind == OC_CHANNEL_KIND_DM) ? actor : 0;
            oc_wbuf_init(&w, g_enc, sizeof g_enc);
            oc_channel_info ci = { r->channel_id, r->ch_kind,
                                   oc_slice_str(r->ch_name ? r->ch_name : ""),
                                   r->ch_is_public, 1, r->ch_created_at, push_peer };
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
        /* Name the actual problem. "channel op rejected" told a user nothing
         * they could act on, and REQ-263 is about failures being legible. */
        const char *why =
            r->err_code == OC_ERR_CHANNEL_EXISTS   ? "a channel with that name already exists"
          : r->err_code == OC_ERR_INVALID_CHANNEL  ? "that channel name is not valid"
          : r->err_code == OC_ERR_UNKNOWN_CHANNEL  ? "no such channel"
          : r->err_code == OC_ERR_NOT_A_MEMBER     ? "you are not a member of that channel"
          : r->err_code == OC_ERR_FORBIDDEN        ? "you do not have permission to do that"
          :                                          "channel op rejected";
        oc_error e = { r->err_code, 0, cs, oc_slice_str(why) };
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
            ents[i].kind = r->chlist[i].kind;
            ents[i].last_message_at = r->chlist[i].last_message_at;
            ents[i].unread = r->chlist[i].unread;
            ents[i].peer_id = r->chlist[i].peer_id;
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
        char tokhex[2 * OC_INVITE_TOKEN_LEN + 1];
        hex_encode(r->session_token, OC_INVITE_TOKEN_LEN, tokhex);
        oc_slice tok = { (const uint8_t *)tokhex, strlen(tokhex) };
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
    case OC_RES_PIN_OK: {
        /* A pin is channel state, so every connected member learns of it —
         * the same fan-out shape as a reaction (REQ-230). */
        oc_wbuf_init(&w, g_enc, sizeof g_enc);
        oc_pin_updated m = { r->message_id, r->channel_id, r->user_id,
                             r->pin_op, r->pinned_at };
        oc_encode_pin_updated(&w, OC_PROTOCOL_VERSION, &m);
        size_t len = w.len;
        for (int fd = 0; fd < OC_NETLOOP_MAX_FD; fd++) {
            conn *c = conns[fd];
            if (c && c->authed && in_members(c->user_id, r->members, r->n_members))
                send_bytes(ep, conns, fd, g_enc, len);
        }
        break;
    }
    case OC_RES_PIN_ERR: {
        conn *c = find_by_id(conns, r->conn_id);
        if (!c) return;
        uint8_t ctx[8];
        for (int i = 0; i < 8; i++) ctx[i] = (uint8_t)(r->message_id >> (56 - 8 * i));
        oc_wbuf_init(&w, g_enc, sizeof g_enc);
        oc_slice cs = { ctx, sizeof ctx };
        oc_error e = { r->err_code, 0, cs, oc_slice_str("pin rejected") };
        oc_encode_error(&w, OC_PROTOCOL_VERSION, &e);
        send_bytes(ep, conns, c->fd, g_enc, w.len);
        break;
    }
    case OC_RES_PINS: {
        /* Stream each pinned message, then the terminator — the LIST_THREAD
         * shape, chosen because each body needs its own frame. */
        conn *c = find_by_id(conns, r->conn_id);
        if (!c) return;
        for (size_t i = 0; i < r->n_plist && conns[c->fd]; i++) {
            const oc_pin_row *pr = &r->plist[i];
            oc_wbuf_init(&w, g_enc, sizeof g_enc);
            oc_pinned_msg pm = { pr->message_id, r->channel_id, pr->author_id,
                                 pr->created_at_ms, pr->pinned_by, pr->pinned_at,
                                 oc_slice_str(pr->body ? pr->body : "") };
            oc_encode_pinned_msg(&w, OC_PROTOCOL_VERSION, &pm);
            send_bytes(ep, conns, c->fd, g_enc, w.len);
        }
        if (!conns[c->fd]) break;
        oc_wbuf_init(&w, g_enc, sizeof g_enc);
        oc_pins term = { r->channel_id, (uint32_t)r->n_plist };
        oc_encode_pins(&w, OC_PROTOCOL_VERSION, &term);
        send_bytes(ep, conns, c->fd, g_enc, w.len);
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
                                   r->author_id, r->server_time, r->reply_count, body, 0, {{0}} };
            tr.n_attach = fill_attach_entries(tr.attach, r->attach, r->n_attach);
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
                                   m->author_id, m->server_time, (uint32_t)r->n_thread, body, 0, {{0}} };
            tr.n_attach = fill_attach_entries(tr.attach, m->attach, m->n_attach);
            oc_encode_thread_reply(&w, OC_PROTOCOL_VERSION, &tr);
            send_bytes(ep, conns, fd, g_enc, w.len);
        }
        if (!conns[fd]) break;
        oc_wbuf_init(&w, g_enc, sizeof g_enc);
        oc_thread th = { r->parent_id, (uint32_t)r->n_thread, r->truncated };
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
        oc_search_results sr = { (uint16_t)n, ents, r->truncated };
        oc_encode_search_results(&w, OC_PROTOCOL_VERSION, &sr);
        send_bytes(ep, conns, c->fd, g_enc, w.len);
        free(ents);
        break;
    }
    case OC_RES_TYPING: {
        /* Relay to every connected member except the typer (REQ-121). Members is
         * empty if the typer couldn't read the channel, so nothing leaks. */
        oc_wbuf_init(&w, g_enc, sizeof g_enc);
        oc_typing_update tu = { r->channel_id, r->author_id };
        oc_encode_typing_update(&w, OC_PROTOCOL_VERSION, &tu);
        size_t len = w.len;
        for (int fd = 0; fd < OC_NETLOOP_MAX_FD; fd++) {
            conn *c = conns[fd];
            if (c && c->authed && c->user_id != r->author_id &&
                in_members(c->user_id, r->members, r->n_members))
                send_bytes(ep, conns, fd, g_enc, len);
        }
        break;
    }
    case OC_RES_ATTACH_CREATED: {
        /* UPLOAD_BEGIN accepted: open the blob sink and tell the client to stream
         * (REQ-140, ARCH-69). The blob bytes never came near the writer thread. */
        conn *c = find_by_id(conns, r->conn_id);
        if (!c) break;
        conn_xfer *x = &c->xfer;
        if (x->state != XFER_UP_AWAIT_CREATE) break;   /* client canceled/closed */
        /* Opening an S3 object is a connect + TLS handshake, so it goes to the
         * pool; UPLOAD_READY is sent when it completes. */
        oc_xfer_job *j = oc_xfer_job_new(OC_XFER_OPEN_W, c->conn_id);
        if (!j || !(j->key = strdup(r->storage_key ? r->storage_key : ""))) {
            oc_xfer_job_free(j);
            int fd = c->fd;
            send_transfer_error(c, r->attachment_id, OC_ERR_INTERNAL);
            if (conns[fd]) { flush_out(conns[fd]); update_interest(ep, conns[fd]); }
            break;
        }
        j->size_hint = r->att_size;
        j->attachment_id = r->attachment_id;
        x->attachment_id = r->attachment_id;
        x->declared_size = r->att_size;
        x->received = 0;
        x->next_seq = 0;
        x->state = XFER_UP_AWAIT_OPEN;
        x->in_flight = 1;
        oc_xferpool_submit(g_xfers, j);
        update_interest(ep, c);
        break;
    }
    case OC_RES_ATTACH_OK: {
        /* UPLOAD_END finalized: confirm with the digest computed while streaming. */
        conn *c = find_by_id(conns, r->conn_id);
        if (!c) break;
        conn_xfer *x = &c->xfer;
        if (x->state != XFER_UP_AWAIT_FINAL) break;
        int fd = c->fd;
        oc_upload_ok ok = { x->attachment_id, r->att_size, { x->digest, 32 } };
        oc_wbuf_init(&w, g_enc, sizeof g_enc);
        oc_encode_upload_ok(&w, OC_PROTOCOL_VERSION, &ok);
        xfer_reset(x);
        send_bytes(ep, conns, fd, g_enc, w.len);
        break;
    }
    case OC_RES_ATTACH_META: {
        /* DOWNLOAD_BEGIN authorized: open the blob and stream it (REQ-141). */
        conn *c = find_by_id(conns, r->conn_id);
        if (!c) break;
        conn_xfer *x = &c->xfer;
        if (x->state != XFER_DOWN_AWAIT_LOOKUP) break;
        /* Stash what DOWNLOAD_INFO needs: this DB result is freed before the
         * blob finishes opening on the worker. */
        free(x->dl_filename); free(x->dl_mime);
        x->dl_filename = strdup(r->filename ? r->filename : "");
        x->dl_mime = strdup(r->mime ? r->mime : "");
        memcpy(x->dl_sha, r->att_sha256, 32);
        x->dl_have_sha = 1;
        oc_xfer_job *j = oc_xfer_job_new(OC_XFER_OPEN_R, c->conn_id);
        if (!j || !(j->key = strdup(r->storage_key ? r->storage_key : ""))) {
            oc_xfer_job_free(j);
            int fd = c->fd;
            send_transfer_error(c, r->attachment_id, OC_ERR_INTERNAL);
            if (conns[fd]) { flush_out(conns[fd]); update_interest(ep, conns[fd]); }
            break;
        }
        j->attachment_id = r->attachment_id;
        x->attachment_id = r->attachment_id;
        x->remaining = r->att_size;
        x->next_seq = 0;
        x->state = XFER_DOWN_AWAIT_OPEN;
        x->in_flight = 1;
        oc_xferpool_submit(g_xfers, j);
        update_interest(ep, c);
        break;
    }
    case OC_RES_STORAGE_STATUS: {
        conn *c = find_by_id(conns, r->conn_id);
        if (!c) break;
        /* The writer knows the database half; free space comes from the net
         * thread's cached statvfs sample, so the two halves meet here. */
        oc_storage_status ss;
        memset(&ss, 0, sizeof ss);
        ss.total_bytes  = g_sstat.total_bytes;
        ss.avail_bytes  = g_sstat.avail_bytes;
        ss.attach_bytes = r->st_attach_bytes;
        ss.attach_count = r->st_attach_count;
        ss.reclaimed_orphan  = r->st_rec_orphan;
        ss.reclaimed_expired = r->st_rec_expired;
        ss.reclaimed_evicted = r->st_rec_evicted;
        ss.last_reclaim_ms   = r->st_last_reclaim_ms;
        ss.max_age_days   = g_spol.max_age_ms / (24ull * 3600 * 1000);
        ss.reserve_bytes  = g_spol.reserve_bytes;
        ss.evict_enabled  = (uint8_t)g_spol.evict_enabled;
        ss.under_pressure = (uint8_t)oc_storage_under_pressure(&g_sstat, &g_spol);
        oc_wbuf_init(&w, g_enc, sizeof g_enc);
        oc_encode_storage_status(&w, OC_PROTOCOL_VERSION, &ss);
        send_bytes(ep, conns, c->fd, g_enc, w.len);
        break;
    }
    case OC_RES_AUDIT_PAGE: {
        conn *c = find_by_id(conns, r->conn_id);
        if (!c) break;
        oc_audit_entry ents[OC_AUDIT_PAGE_MAX];
        uint16_t n = 0;
        for (size_t i = 0; i < r->n_audit && n < OC_AUDIT_PAGE_MAX; i++, n++) {
            const oc_audit_row *a = &r->audit[i];
            ents[n].at_ms     = a->at_ms;
            ents[n].actor_id  = a->actor_id;
            ents[n].target_id = a->target_id;
            ents[n].family    = a->family;
            ents[n].outcome   = a->outcome;
            ents[n].actor_name = oc_slice_str(a->actor_name ? a->actor_name : "");
            ents[n].action     = oc_slice_str(a->action ? a->action : "");
            ents[n].target     = oc_slice_str(a->target ? a->target : "");
            ents[n].detail     = oc_slice_str(a->detail ? a->detail : "");
        }
        oc_audit_page pg = { n, ents };
        oc_wbuf_init(&w, g_enc, sizeof g_enc);
        oc_encode_audit_page(&w, OC_PROTOCOL_VERSION, &pg);
        send_bytes(ep, conns, c->fd, g_enc, w.len);
        break;
    }
    case OC_RES_AUDIT_ERR: {
        conn *c = find_by_id(conns, r->conn_id);
        if (!c) break;
        oc_wbuf_init(&w, g_enc, sizeof g_enc);
        oc_error e = { r->err_code, 0, { NULL, 0 }, oc_slice_str("audit query denied") };
        oc_encode_error(&w, OC_PROTOCOL_VERSION, &e);
        send_bytes(ep, conns, c->fd, g_enc, w.len);
        break;
    }
    case OC_RES_STORAGE_ERR: {
        conn *c = find_by_id(conns, r->conn_id);
        if (!c) break;
        oc_wbuf_init(&w, g_enc, sizeof g_enc);
        oc_error e = { r->err_code, 0, { NULL, 0 }, oc_slice_str("storage status denied") };
        oc_encode_error(&w, OC_PROTOCOL_VERSION, &e);
        send_bytes(ep, conns, c->fd, g_enc, w.len);
        break;
    }
    case OC_RES_STORAGE_MAINT: {
        /* The rows are already tombstoned; delete the bytes off-thread. These
         * are fire-and-forget (conn_id 0) — no connection is waiting, and a
         * failed delete simply leaves a blob the next orphan sweep will retry. */
        for (size_t i = 0; i < r->n_reclaim; i++) {
            oc_xfer_job *dj = oc_xfer_job_new(OC_XFER_DELETE, 0);
            if (!dj) break;
            dj->key = strdup(r->reclaim[i].storage_key ? r->reclaim[i].storage_key : "");
            dj->attachment_id = r->reclaim[i].attachment_id;
            if (!dj->key) { oc_xfer_job_free(dj); break; }
            oc_xferpool_submit(g_xfers, dj);
        }
        if (r->n_reclaim)
            fprintf(stderr, "openchimed: storage maintenance reclaimed %zu blob(s) "
                            "(%llu orphaned, %llu expired, %llu evicted); %llu MB free\n",
                    r->n_reclaim,
                    (unsigned long long)r->maint_orphans,
                    (unsigned long long)r->maint_expired,
                    (unsigned long long)r->maint_evicted,
                    (unsigned long long)(g_sstat.avail_bytes / (1024 * 1024)));
        break;
    }
    case OC_RES_ATTACH_ERR: {
        conn *c = find_by_id(conns, r->conn_id);
        if (!c) break;
        int fd = c->fd;
        send_transfer_error(c, r->attachment_id, r->err_code);   /* resets the xfer */
        if (conns[fd]) { flush_out(conns[fd]); update_interest(ep, conns[fd]); }
        break;
    }
    case OC_RES_WEBHOOK_CREATED: {
        /* Hand the minted token back to the client that asked (shown once). */
        conn *c = find_by_id(conns, r->conn_id);
        if (!c) break;
        char whhex[2 * OC_SESSION_TOKEN_LEN + 1];
        hex_encode(r->session_token, OC_SESSION_TOKEN_LEN, whhex);
        oc_webhook_info wi = { r->message_id, r->channel_id,
                               { (const uint8_t *)whhex, strlen(whhex) } };
        oc_wbuf_init(&w, g_enc, sizeof g_enc);
        oc_encode_webhook_info(&w, OC_PROTOCOL_VERSION, &wi);
        send_bytes(ep, conns, c->fd, g_enc, w.len);
        break;
    }
    case OC_RES_WEBHOOK_POSTED: {
        /* Fan the posted message out to connected members (like SEND_OK)... */
        oc_wbuf_init(&w, g_enc, sizeof g_enc);
        oc_slice body = { r->body, r->body_len };
        oc_broadcast b = { r->message_id, r->channel_id, r->author_id, r->server_time, body, 0, {{0}}, {0} };
        b.author_name = oc_slice_str(r->author_name ? r->author_name : "");   /* webhook label (REQ-170) */
        oc_encode_broadcast(&w, OC_PROTOCOL_VERSION, &b);
        size_t blen = w.len;
        for (int fd = 0; fd < OC_NETLOOP_MAX_FD; fd++) {
            conn *m = conns[fd];
            if (m && m->authed && in_members(m->user_id, r->members, r->n_members))
                send_bytes(ep, conns, fd, g_enc, blen);
        }
        /* ...then 200 the webhook sender and close its HTTP connection. */
        conn *hc = find_by_id(conns, r->conn_id);
        if (hc) {
            char jb[64];
            int jn = snprintf(jb, sizeof jb, "{\"ok\":true,\"message_id\":%llu}\n",
                              (unsigned long long)r->message_id);
            http_reply(hc, 200, "OK", "application/json", jb, jn > 0 ? (size_t)jn : 0);
            flush_out(hc);
            conn_close(ep, conns, hc->fd);
        }
        break;
    }
    case OC_RES_WEBHOOK_ERR: {
        conn *c = find_by_id(conns, r->conn_id);
        if (!c) break;
        if (c->http) {
            int status = 404; const char *reason = "Not Found"; const char *msg = "unknown webhook\n";
            if (r->err_code == OC_ERR_INTERNAL) { status = 500; reason = "Internal Server Error"; msg = "error\n"; }
            http_reply(c, status, reason, "text/plain", msg, strlen(msg));
            flush_out(c);
            conn_close(ep, conns, c->fd);
        } else {
            /* CREATE_WEBHOOK failure on a binary client -> ERROR frame. */
            oc_wbuf_init(&w, g_enc, sizeof g_enc);
            oc_error e = { r->err_code, 0, { NULL, 0 }, oc_slice_str("webhook error") };
            oc_encode_error(&w, OC_PROTOCOL_VERSION, &e);
            send_bytes(ep, conns, c->fd, g_enc, w.len);
        }
        break;
    }
    case OC_RES_WEBHOOK_LIST: {
        conn *c = find_by_id(conns, r->conn_id);
        if (!c) break;
        oc_webhook_list_entry ents[OC_WEBHOOK_LIST_MAX];
        uint16_t n = 0;
        for (size_t i = 0; i < r->n_whlist && n < OC_WEBHOOK_LIST_MAX; i++) {
            ents[n].webhook_id = r->whlist[i].id;
            ents[n].channel_id = r->whlist[i].channel_id;
            ents[n].label = oc_slice_str(r->whlist[i].label ? r->whlist[i].label : "");
            ents[n].disabled = r->whlist[i].disabled;
            n++;
        }
        oc_webhook_list wl = { n, ents };
        oc_wbuf_init(&w, g_enc, sizeof g_enc);
        oc_encode_webhook_list(&w, OC_PROTOCOL_VERSION, &wl);
        send_bytes(ep, conns, c->fd, g_enc, w.len);
        break;
    }
    case OC_RES_WEBHOOK_DELETED: {
        conn *c = find_by_id(conns, r->conn_id);
        if (!c) break;
        oc_webhook_deleted wd = { r->message_id };
        oc_wbuf_init(&w, g_enc, sizeof g_enc);
        oc_encode_webhook_deleted(&w, OC_PROTOCOL_VERSION, &wd);
        send_bytes(ep, conns, c->fd, g_enc, w.len);
        break;
    }
    case OC_RES_NOTIFY_PREFS: {
        /* Sync the settings snapshot to *all* of the user's connections, so a
         * change on one device updates the others (REQ-130/131). */
        static oc_notify_pref_entry ents[OC_MAX_NOTIFY_PREFS];
        uint16_t n = 0;
        for (size_t i = 0; i < r->n_nprefs && n < OC_MAX_NOTIFY_PREFS; i++) {
            ents[n].channel_id = r->nprefs[i].channel_id;
            ents[n].level = r->nprefs[i].level;
            n++;
        }
        oc_notify_prefs np = { r->np_dnd_enabled, r->np_dnd_start_min, r->np_dnd_end_min, n, ents };
        oc_wbuf_init(&w, g_enc, sizeof g_enc);
        oc_encode_notify_prefs(&w, OC_PROTOCOL_VERSION, &np);
        size_t len = w.len;
        for (int fd = 0; fd < OC_NETLOOP_MAX_FD; fd++) {
            conn *c = conns[fd];
            if (c && c->authed && c->user_id == r->user_id)
                send_bytes(ep, conns, fd, g_enc, len);
        }
        break;
    }
    case OC_RES_NOTIFY_ERR: {
        conn *c = find_by_id(conns, r->conn_id);
        if (!c) break;
        oc_wbuf_init(&w, g_enc, sizeof g_enc);
        oc_error e = { r->err_code, 0, { NULL, 0 }, oc_slice_str("notify pref error") };
        oc_encode_error(&w, OC_PROTOCOL_VERSION, &e);
        send_bytes(ep, conns, c->fd, g_enc, w.len);
        break;
    }
    case OC_RES_CLIENT_SETTINGS: {
        /* Sync the bucket snapshot to *all* of the user's connections, so a change
         * on one device reaches the others; each client folds it only if the
         * client_type matches its own (per-frontend buckets). */
        static oc_client_setting_entry ents[OC_MAX_CLIENT_SETTINGS];
        uint16_t n = 0;
        for (size_t i = 0; i < r->n_cslist && n < OC_MAX_CLIENT_SETTINGS; i++) {
            ents[n].key = oc_slice_str(r->cslist[i].key ? r->cslist[i].key : "");
            ents[n].value = oc_slice_str(r->cslist[i].value ? r->cslist[i].value : "");
            n++;
        }
        oc_client_settings cst = { oc_slice_str(r->cs_client_type ? r->cs_client_type : ""), n, ents };
        oc_wbuf_init(&w, g_enc, sizeof g_enc);
        oc_encode_client_settings(&w, OC_PROTOCOL_VERSION, &cst);
        size_t len = w.len;
        for (int fd = 0; fd < OC_NETLOOP_MAX_FD; fd++) {
            conn *c = conns[fd];
            if (c && c->authed && c->user_id == r->user_id)
                send_bytes(ep, conns, fd, g_enc, len);
        }
        break;
    }
    case OC_RES_PROFILE_UPDATED: {
        /* A display-name change (REQ-020): fan it to every authed connection so all
         * rosters update; the originator reads it as its own ack (a password change
         * echoes the unchanged name, which is a harmless roster no-op). */
        oc_profile_updated pu = { r->user_id, oc_slice_str(r->profile_name ? r->profile_name : "") };
        oc_wbuf_init(&w, g_enc, sizeof g_enc);
        oc_encode_profile_updated(&w, OC_PROTOCOL_VERSION, &pu);
        size_t len = w.len;
        for (int fd = 0; fd < OC_NETLOOP_MAX_FD; fd++) {
            conn *c = conns[fd];
            if (c && c->authed) send_bytes(ep, conns, fd, g_enc, len);
        }
        break;
    }
    case OC_RES_PROFILE_ERR: {
        conn *c = find_by_id(conns, r->conn_id);
        if (!c) break;
        oc_wbuf_init(&w, g_enc, sizeof g_enc);
        oc_error e = { r->err_code, 0, { NULL, 0 }, oc_slice_str("profile update rejected") };
        oc_encode_error(&w, OC_PROTOCOL_VERSION, &e);
        send_bytes(ep, conns, c->fd, g_enc, w.len);
        break;
    }
    case OC_RES_READ_CURSOR: {
        /* Seen-by (REQ-090): tell the channel's other members that this member
         * advanced their read cursor, and backfill the acker with the others'
         * current cursors so opening a channel shows who has already read it. */
        oc_read_cursor rc = { r->channel_id, r->user_id, r->message_id };
        oc_wbuf_init(&w, g_enc, sizeof g_enc);
        oc_encode_read_cursor(&w, OC_PROTOCOL_VERSION, &rc);
        size_t len = w.len;
        for (int fd = 0; fd < OC_NETLOOP_MAX_FD; fd++) {
            conn *c = conns[fd];
            if (c && c->authed && c->user_id != r->user_id &&
                in_members(c->user_id, r->members, r->n_members))
                send_bytes(ep, conns, fd, g_enc, len);
        }
        conn *ac = find_by_id(conns, r->conn_id);
        if (ac) {
            for (size_t i = 0; i < r->n_rcur; i++) {
                oc_read_cursor rb = { r->channel_id, r->rcur[i].user_id, r->rcur[i].message_id };
                oc_wbuf_init(&w, g_enc, sizeof g_enc);
                oc_encode_read_cursor(&w, OC_PROTOCOL_VERSION, &rb);
                send_bytes(ep, conns, ac->fd, g_enc, w.len);
            }
        }
        break;
    }
    case OC_RES_CALL_AUTH: {
        /* Authorized: add to the channel's ephemeral call, tell the joiner (with
         * the roster), and push a roster update to the other participants. */
        conn *jc = find_by_id(conns, r->conn_id);
        if (!jc) break;
        int jfd = jc->fd;
        call_t *c = call_get_or_create(r->channel_id);
        uint8_t token[OC_AUDIO_TOKEN_LEN];
        if (!c || oc_rand_bytes(token, sizeof token) != 0 ||
            !call_add(c, r->user_id, r->conn_id, token)) {
            oc_wbuf_init(&w, g_enc, sizeof g_enc);
            oc_error e = { OC_ERR_INTERNAL, 0, { NULL, 0 }, oc_slice_str("call full") };
            oc_encode_error(&w, OC_PROTOCOL_VERSION, &e);
            send_bytes(ep, conns, jfd, g_enc, w.len);
            break;
        }
        /* Register the participant + token with the media sidecar (ARCH-31). */
        audio_authorize(r->channel_id, r->user_id, token);
        uint64_t parts[OC_MAX_CALL_PARTICIPANTS];
        for (int i = 0; i < c->n; i++) parts[i] = c->parts[i].user_id;
        /* The joiner's private media endpoint: the sidecar's UDP port + its token. */
        oc_call_joined jd = { c->channel_id, c->channel_id, g_audio_udp_port,
                              { token, OC_AUDIO_TOKEN_LEN }, (uint16_t)c->n, parts };
        oc_wbuf_init(&w, g_enc, sizeof g_enc);
        oc_encode_call_joined(&w, OC_PROTOCOL_VERSION, &jd);
        send_bytes(ep, conns, jfd, g_enc, w.len);
        if (conns[jfd]) {   /* joiner still up */
            call_t *cc = call_find(r->channel_id);
            if (cc) call_send_roster(ep, conns, cc, r->conn_id);
        }
        break;
    }
    case OC_RES_CALL_ERR: {
        conn *jc = find_by_id(conns, r->conn_id);
        if (!jc) break;
        oc_wbuf_init(&w, g_enc, sizeof g_enc);
        oc_error e = { r->err_code, 0, { NULL, 0 }, oc_slice_str("call join denied") };
        oc_encode_error(&w, OC_PROTOCOL_VERSION, &e);
        send_bytes(ep, conns, jc->fd, g_enc, w.len);
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
            oc_broadcast b = { m->message_id, m->channel_id, m->author_id, m->server_time, body, 0, {{0}}, {0} };
            broadcast_set_attach(&b, m->attach, m->n_attach);
            b.author_name = oc_slice_str(m->author_name ? m->author_name : "");
            oc_encode_broadcast(&w, OC_PROTOCOL_VERSION, &b);
            send_bytes(ep, conns, fd, g_enc, w.len);
            if (m->reply_count > 0 && conns[fd]) {
                oc_wbuf_init(&w, g_enc, sizeof g_enc);
                oc_thread_meta tm = { m->message_id, m->reply_count, m->last_reply_at };
                oc_encode_thread_meta(&w, OC_PROTOCOL_VERSION, &tm);
                send_bytes(ep, conns, fd, g_enc, w.len);
            }
        }
        /* Pin state for what we just replayed, for the same reason as the
         * reactions below: a BROADCAST has no field for it, so without this a
         * reload silently loses every pin (REQ-230). */
        for (size_t i = 0; i < r->n_replay && conns[fd]; i++) {
            if (!r->replay[i].pinned_by && !r->replay[i].pinned_at) continue;
            oc_wbuf_init(&w, g_enc, sizeof g_enc);
            oc_pin_updated pu = { r->replay[i].message_id, r->replay[i].channel_id,
                                  r->replay[i].pinned_by, OC_PIN_ADD,
                                  r->replay[i].pinned_at };
            oc_encode_pin_updated(&w, OC_PROTOCOL_VERSION, &pu);
            send_bytes(ep, conns, fd, g_enc, w.len);
        }

        /* Then the reaction state for those messages. A BROADCAST carries none,
         * so without this every reaction vanished on reload. op=ADD with the
         * aggregate count reconstructs the chip; user_id is the requester when
         * they reacted, which is what marks the chip as theirs. */
        for (size_t i = 0; i < r->n_rreact && conns[fd]; i++) {
            oc_wbuf_init(&w, g_enc, sizeof g_enc);
            oc_reaction_updated ru = { r->rreact[i].message_id, r->rreact[i].channel_id,
                                       r->rreact[i].user_id,
                                       oc_slice_str(r->rreact[i].emoji ? r->rreact[i].emoji : ""),
                                       OC_REACT_ADD, r->rreact[i].count };
            oc_encode_reaction_updated(&w, OC_PROTOCOL_VERSION, &ru);
            send_bytes(ep, conns, fd, g_enc, w.len);
        }
        if (!conns[fd]) return;
        oc_wbuf_init(&w, g_enc, sizeof g_enc);
        oc_backfill_done done = { r->high_water, r->truncated };
        oc_encode_backfill_done(&w, OC_PROTOCOL_VERSION, &done);
        send_bytes(ep, conns, fd, g_enc, w.len);
        break;
    }
    default: break;
    }
}

/* --- Main loop ---------------------------------------------------------- */


/* Collect one finished blob job (ARCH-69) and advance that transfer.
 *
 * The connection may have closed while the job was with a worker, which is the
 * case this whole design exists to make safe: the handle travels with the job,
 * so if the connection is gone we still own something valid and release it here
 * rather than leaking it. Nothing dereferences a stale `conn *` — the job
 * carries a conn_id and connection ids are never reused. */
/* Run the storage maintenance pass if its interval has elapsed (ARCH-78).
 *
 * Driven by the net loop's regular tick rather than by incoming writes: an idle
 * tenant is exactly the one whose disk fills with nothing prompting a cleanup,
 * so piggybacking on traffic (the way ARCH-44's idempotency prune does) would
 * stop precisely when it matters most. The pass itself is a DB job — finding
 * what to reclaim is a query — and the bytes are deleted by the transfer pool
 * when the result comes back. */
static void maybe_run_maintenance(oc_dbwriter *dbw) {
    uint64_t now = now_ms();
    if (g_last_maint_ms && now - g_last_maint_ms < g_spol.interval_ms) return;
    g_last_maint_ms = now;

    oc_storage_sample(g_blob_dir, now, &g_sstat);

    /* Eviction is the destructive tier, so it is requested only when free space
     * is genuinely below the watermark AND the operator left it enabled. The
     * orphan sweep runs every pass regardless: it frees bytes nobody was ever
     * promised, so there is no reason to wait for pressure. */
    int pressure = oc_storage_under_pressure(&g_sstat, &g_spol);

    oc_job *j = oc_job_new(OC_JOB_STORAGE_MAINT, 0);   /* no connection owns this */
    if (!j) return;
    j->maint_max_age_ms = g_spol.max_age_ms;
    j->maint_grace_ms   = g_spol.grace_ms;
    j->maint_batch      = g_spol.batch;
    j->maint_evict      = (pressure && g_spol.evict_enabled);
    j->audit_max_age_ms = g_spol.audit_max_age_ms;
    oc_dbwriter_submit(dbw, j);
}

static void deliver_xfer_result(int ep, conn **conns, oc_dbwriter *dbw, oc_xfer_job *j) {
    conn *c = find_by_id(conns, j->conn_id);
    if (!c) {
        /* Orphaned: release whatever the job is still holding, off-thread. */
        if (j->bw) {
            oc_xfer_job *ab = oc_xfer_job_new(OC_XFER_ABORT, 0);
            if (ab) { ab->bw = j->bw; oc_xferpool_submit(g_xfers, ab); }
        }
        if (j->br) {
            oc_xfer_job *cl = oc_xfer_job_new(OC_XFER_CLOSE, 0);
            if (cl) { cl->br = j->br; oc_xferpool_submit(g_xfers, cl); }
        }
        return;
    }

    conn_xfer *x = &c->xfer;
    int fd = c->fd;
    oc_wbuf w;

    /* The job is done, so the connection owns its transfer state again. Clear
     * this before anything below, since xfer_reset consults it to decide
     * whether a handle is ours to release. */
    x->in_flight = 0;

    /* A stale completion for a transfer the client already abandoned (it sent
     * TRANSFER_CANCEL, or started a new one). Release the handle and stop. */
    if (j->attachment_id && x->attachment_id != j->attachment_id) {
        if (j->bw) {
            oc_xfer_job *ab = oc_xfer_job_new(OC_XFER_ABORT, 0);
            if (ab) { ab->bw = j->bw; oc_xferpool_submit(g_xfers, ab); }
        }
        if (j->br) {
            oc_xfer_job *cl = oc_xfer_job_new(OC_XFER_CLOSE, 0);
            if (cl) { cl->br = j->br; oc_xferpool_submit(g_xfers, cl); }
        }
        update_interest(ep, c);
        return;
    }

    switch (j->op) {
    case OC_XFER_OPEN_W:
        if (j->rc != 0 || !j->bw || x->state != XFER_UP_AWAIT_OPEN) {
            if (j->bw) {   /* opened, but the client moved on */
                oc_xfer_job *ab = oc_xfer_job_new(OC_XFER_ABORT, 0);
                if (ab) { ab->bw = j->bw; oc_xferpool_submit(g_xfers, ab); }
            }
            send_transfer_error(c, j->attachment_id, OC_ERR_INTERNAL);
            break;
        }
        x->bw = j->bw;
        mbedtls_sha256_init(&x->sha);
        mbedtls_sha256_starts(&x->sha, 0);
        x->sha_init = 1;
        x->state = XFER_UP_ACTIVE;
        {
            oc_upload_ready rd = { x->attachment_id, OC_ATTACH_CHUNK_SIZE, OC_UPLOAD_WINDOW };
            oc_wbuf_init(&w, g_enc, sizeof g_enc);
            oc_encode_upload_ready(&w, OC_PROTOCOL_VERSION, &rd);
            if (out_append(c, g_enc, w.len) != 0) { conn_close(ep, conns, fd); return; }
        }
        break;

    case OC_XFER_WRITE:
        if (j->rc != 0 || x->state != XFER_UP_ACTIVE) {
            send_transfer_error(c, j->attachment_id, OC_ERR_INTERNAL);
            break;
        }
        /* Hash here, not at submit time: completions arrive in submission order
         * (one job per transfer in flight), so this matches what was stored. */
        if (j->len) mbedtls_sha256_update(&x->sha, j->data, j->len);
        x->received += j->len;
        x->next_seq++;
        {
            oc_upload_ack ack = { x->attachment_id, x->next_seq };
            oc_wbuf_init(&w, g_enc, sizeof g_enc);
            oc_encode_upload_ack(&w, OC_PROTOCOL_VERSION, &ack);
            if (out_append(c, g_enc, w.len) != 0) { conn_close(ep, conns, fd); return; }
        }
        break;

    case OC_XFER_COMMIT:
        if (j->rc != 0 || x->state != XFER_UP_AWAIT_COMMIT) {
            send_transfer_error(c, j->attachment_id, OC_ERR_INTERNAL);
            break;
        }
        {
            oc_job *fj = oc_job_new(OC_JOB_ATTACH_FINALIZE, c->conn_id);
            if (!fj) { send_transfer_error(c, j->attachment_id, OC_ERR_INTERNAL); break; }
            fj->user_id = c->user_id;
            fj->attachment_id = x->attachment_id;
            fj->att_size = x->received;
            memcpy(fj->att_sha256, x->digest, 32);
            oc_dbwriter_submit(dbw, fj);
            x->state = XFER_UP_AWAIT_FINAL;
        }
        break;

    case OC_XFER_OPEN_R:
        if (j->rc != 0 || !j->br || x->state != XFER_DOWN_AWAIT_OPEN) {
            if (j->br) {
                oc_xfer_job *cl = oc_xfer_job_new(OC_XFER_CLOSE, 0);
                if (cl) { cl->br = j->br; oc_xferpool_submit(g_xfers, cl); }
            }
            send_transfer_error(c, j->attachment_id, OC_ERR_INTERNAL);
            break;
        }
        x->br = j->br;
        x->state = XFER_DOWN_ACTIVE;
        {
            oc_download_info di = { x->attachment_id,
                                    oc_slice_str(x->dl_filename ? x->dl_filename : ""),
                                    oc_slice_str(x->dl_mime ? x->dl_mime : ""),
                                    x->remaining, { x->dl_sha, 32 } };
            oc_wbuf_init(&w, g_enc, sizeof g_enc);
            oc_encode_download_info(&w, OC_PROTOCOL_VERSION, &di);
            if (out_append(c, g_enc, w.len) != 0) { conn_close(ep, conns, fd); return; }
        }
        download_pump(c);
        break;

    case OC_XFER_READ:
        if (j->rc != 0 || j->len == 0 || x->state != XFER_DOWN_ACTIVE) {
            send_transfer_error(c, j->attachment_id, OC_ERR_INTERNAL);
            break;
        }
        {
            oc_download_chunk ch = { x->attachment_id, x->next_seq, { j->data, j->len } };
            oc_wbuf_init(&w, g_enc, sizeof g_enc);
            oc_encode_download_chunk(&w, OC_PROTOCOL_VERSION, &ch);
            if (out_append(c, g_enc, w.len) != 0) { conn_close(ep, conns, fd); return; }
        }
        x->next_seq++;
        x->remaining -= (uint64_t)j->len;
        download_pump(c);                      /* ask for the next slice */
        break;

    case OC_XFER_ABORT:
    case OC_XFER_CLOSE:
    case OC_XFER_DELETE:
        break;                                  /* submitted fire-and-forget */
    }

    if (!conns[fd]) return;                     /* closed above */
    /* An upload write just finished: resume draining the frames that arrived
     * while we were paused, then re-arm EPOLLIN via update_interest. */
    if (j->op == OC_XFER_WRITE && !x->in_flight) {
        if (drain_frames(ep, conns, c, dbw) < 0) { conn_close(ep, conns, fd); return; }
        if (!conns[fd]) return;
    }
    if (flush_out(c) < 0) { conn_close(ep, conns, fd); return; }
    if (conns[fd]) update_interest(ep, c);
}

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

    /* Attachment blob store (ARCH-70) + upload size cap (REQ-140). Bytes are
     * proxied through this loop to/from the store; keep it beside the daemon. */
    {
        const oc_config *cfg = oc_config_get();
        const char *bd = cfg->blob_dir;
        g_blobs = oc_blobstore_open(bd);
        if (!g_blobs) { close(ep); close(lfd); free(conns); return -1; }
        g_max_attach = cfg->max_attach_size;

        /* Blob I/O runs here, off the net thread (ARCH-69). Worker count from
         * config (default 2, clamped 1..16): each in-flight transfer holds a TLS
         * session plus a chunk buffer, which has to stay modest against the 256MB
         * lean profile (REQ-210). */
        int nw = cfg->xfer_workers;
        snprintf(g_blob_dir, sizeof g_blob_dir, "%s", bd);
        g_spol = cfg->storage;
        oc_storage_sample(g_blob_dir, now_ms(), &g_sstat);
        fprintf(stderr, "openchimed: storage maintenance every %llus, "
                        "max attachment age %llud, eviction %s, %llu MB free\n",
                (unsigned long long)(g_spol.interval_ms / 1000),
                (unsigned long long)(g_spol.max_age_ms / (24ull * 3600 * 1000)),
                g_spol.evict_enabled ? "on" : "off",
                (unsigned long long)(g_sstat.avail_bytes / (1024 * 1024)));

        g_xfers = oc_xferpool_start(g_blobs, nw);
        if (!g_xfers) {
            oc_blobstore_close(g_blobs); g_blobs = NULL;
            close(ep); close(lfd); free(conns); return -1;
        }
    }

    /* Per-webhook rate limit (REQ-170); best-effort — if allocation fails the
     * endpoint still works, just unthrottled. */
    g_webhook_rl = oc_ratelimit_new(OC_WEBHOOK_RATE_MAX, OC_WEBHOOK_RATE_WINDOW, 1024);

    /* Per-source-IP concurrent-connection cap (0 disables). Blunts a
     * connection-exhaustion flood from one host while staying generous enough
     * for a large office behind one NAT; operators tune it. The global cap is
     * OC_NETLOOP_MAX_FD. */
    int max_per_ip = oc_config_get()->max_conns_per_ip;

    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    ev.events = EPOLLIN; ev.data.fd = lfd;
    epoll_ctl(ep, EPOLL_CTL_ADD, lfd, &ev);
    memset(&ev, 0, sizeof ev);
    ev.events = EPOLLIN; ev.data.fd = evfd;
    epoll_ctl(ep, EPOLL_CTL_ADD, evfd, &ev);

    int xfd = oc_xferpool_eventfd(g_xfers);
    ev.events = EPOLLIN; ev.data.fd = xfd;
    epoll_ctl(ep, EPOLL_CTL_ADD, xfd, &ev);

    fprintf(stderr, "netloop: listening on :%d\n", port);

    struct epoll_event events[64];
    while (!*stop) {
        int nfds = epoll_wait(ep, events, 64, 500);
        /* Every tick, timeout included — a quiet box must still be maintained. */
        maybe_run_maintenance(dbw);
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

            if (fd == xfd) {
                uint64_t cnt;
                while (read(xfd, &cnt, sizeof cnt) > 0) { /* drain the counter */ }
                oc_xfer_job *xj;
                while ((xj = oc_xferpool_next_result(g_xfers)) != NULL) {
                    deliver_xfer_result(ep, conns, dbw, xj);
                    oc_xfer_job_free(xj);
                }
                continue;
            }

            conn *c = conns[fd];
            if (!c) continue;

            if (c->state == CONN_HANDSHAKE) {
                oc_tls_status st = oc_tls_handshake(&c->tls);
                if (st == OC_TLS_OK) {
                    c->state = CONN_ESTABLISHED;
                    /* ALPN demux (ARCH-54): a peer that didn't negotiate oc/1 is an
                     * HTTP/webhook client, routed to the HTTP handler (ARCH-32). */
                    const char *alpn = oc_tls_alpn_selected(&c->tls);
                    c->http = (!alpn || strcmp(alpn, OC_ALPN_PROTO) != 0);
                }
                else if (st == OC_TLS_WANT_READ)  { conn_set_events(ep, c, EPOLLIN);  continue; }
                else if (st == OC_TLS_WANT_WRITE) { conn_set_events(ep, c, EPOLLOUT); continue; }
                else { conn_close(ep, conns, fd); continue; }
                /* fall through: drain any app data mbedTLS already buffered */
            }

            if (c->state == CONN_ESTABLISHED) {
                if (on_readable(ep, conns, c, dbw) < 0) { flush_out(c); conn_close(ep, conns, fd); continue; }
            }
            download_pump(c);   /* refill an active download as the socket drains */
            if (flush_out(c) < 0) { conn_close(ep, conns, fd); continue; }
            update_interest(ep, c);
        }
    }

    for (int fd = 0; fd < OC_NETLOOP_MAX_FD; fd++)
        if (conns[fd]) conn_close(ep, conns, fd);
    /* Stop the workers before the store they borrow; this also drains any
     * fire-and-forget cleanup the closes above just queued. */
    oc_xferpool_stop(g_xfers);
    g_xfers = NULL;
    oc_blobstore_close(g_blobs);
    g_blobs = NULL;
    oc_ratelimit_free(g_webhook_rl);
    g_webhook_rl = NULL;
    close(ep);
    close(lfd);
    free(conns);
    fprintf(stderr, "netloop: stopped\n");
    return 0;
}
