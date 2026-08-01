/*
 * OpenChime client — network thread. See net.h.
 */

#include "net.h"
#include "searchq.h"
#include "event.h"
#include "model.h"        /* oc_model_now_ms: one clock for the backoff deadline */
#include "store.h"

#include "protocol.h"
#include "tls.h"
#include "framebuf.h"
#include "sock.h"       /* POSIX/Winsock shim (also pulls in getaddrinfo) */

#include "oc_thread.h"
#include "oc_port.h"
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <bcrypt.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <time.h>

struct oc_net {
    oc_thread_t   thread;
    volatile int  stop;
    volatile int  reconnect_now;   /* set by oc_net_reconnect: cut short the backoff */
    char          host[256];
    int           port;
    char         *token;
    char         *invite;       /* one-shot signup token (WIN-32), else NULL */
    char         *store_path;   /* local store for token/pin persistence, or NULL */
    oc_secret    *secret;       /* borrowed OS keyring for the session token, or NULL */
    char          client_type[32]; /* synced-settings bucket id (default "tui") */
    oc_queue     *to_ui;
    oc_queue     *from_ui;
};

/* ---- the offline outbox, in memory (REQ-102, ARCH-88) ----------------------
 * A send is recorded here before it goes out and removed on its SEND_ACK, so a
 * drop mid-flight leaves it to be resent on reconnect — deduped by the daemon on
 * the idempotency token. It lives for the life of the process and no longer: the
 * requirement is "queued locally, sent automatically on reconnect", and a client
 * that writes nothing to disk cannot honour more than that. A message still
 * queued when the app exits is lost, which is why a frontend should warn on quit
 * while oc_net_outbox_pending() is non-zero. */
typedef struct { uint8_t idem[OC_IDEM_SIZE]; uint64_t channel_id; char *body; } obox_row;
typedef struct { obox_row *v; size_t n, cap; } obox;

/* The UI thread must not walk the outbox (net-thread-owned), so the net thread
 * publishes just its size here. A stale-by-one-tick read is fine: this drives a
 * confirm-on-quit prompt, not a correctness decision. */
static volatile int g_obox_pending;
static void obox_publish(const obox *o) { g_obox_pending = (int)o->n; }
int oc_net_outbox_pending(oc_net *n) { (void)n; return g_obox_pending; }

static void obox_add(obox *o, const uint8_t idem[OC_IDEM_SIZE], uint64_t cid, const char *body) {
    for (size_t i = 0; i < o->n; i++)
        if (memcmp(o->v[i].idem, idem, OC_IDEM_SIZE) == 0) return;
    if (o->n == o->cap) {
        size_t want = o->cap ? o->cap * 2 : 16;
        obox_row *q = realloc(o->v, want * sizeof *q);
        if (!q) return;
        o->v = q; o->cap = want;
    }
    memcpy(o->v[o->n].idem, idem, OC_IDEM_SIZE);
    o->v[o->n].channel_id = cid;
    o->v[o->n].body = body ? strdup(body) : NULL;
    o->n++;
    obox_publish(o);
}
static void obox_remove(obox *o, const uint8_t idem[OC_IDEM_SIZE]) {
    size_t w = 0;
    for (size_t i = 0; i < o->n; i++) {
        if (memcmp(o->v[i].idem, idem, OC_IDEM_SIZE) == 0) { free(o->v[i].body); continue; }
        o->v[w++] = o->v[i];
    }
    o->n = w;
    obox_publish(o);
}
static void obox_free(obox *o) {
    for (size_t i = 0; i < o->n; i++) free(o->v[i].body);
    free(o->v); o->v = NULL; o->n = o->cap = 0;
    obox_publish(o);
}

/* The store-backed connection context (session token + TOFU pin persistence),
 * threaded through run_connection so the net thread persists across restarts. */
typedef struct {
    oc_store   *store;                          /* NULL = no persistence */
    const char *workspace;                       /* "host:port" key */
    uint8_t     pin[OC_TLS_FINGERPRINT_LEN];
    int         have_pin;                       /* pin loaded/captured this run */
    int         logged_out;                     /* set on /logout: drop the stored token */
    obox       *obox;                           /* in-memory outbox (REQ-102) */
} conn_store;

/* ---- helpers ---- */

/* Copy a wire slice into a fixed buffer, NUL-terminated and truncated to fit. */
static void slice_to_buf(oc_slice sl, char *out, size_t cap) {
    size_t n = sl.len < cap - 1 ? sl.len : cap - 1;
    if (n && sl.ptr) memcpy(out, sl.ptr, n);
    out[n] = '\0';
}

static void push_err(oc_queue *to_ui, const char *msg) {
    oc_ev *e = oc_ev_new(OC_EV_ERROR);
    if (e) { e->body = strdup(msg); oc_queue_push(to_ui, e); }
}

static void push_simple(oc_queue *to_ui, int type, uint64_t user_id) {
    oc_ev *e = oc_ev_new(type);
    if (e) { e->user_id = user_id; oc_queue_push(to_ui, e); }
}

static void gen_idem(uint8_t out[OC_IDEM_SIZE]) {
#ifdef _WIN32
    /* CSPRNG from the OS (bcrypt). Falls back to rand() only if that fails. */
    if (BCryptGenRandom(NULL, out, OC_IDEM_SIZE, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0)
        return;
    for (int i = 0; i < (int)OC_IDEM_SIZE; i++) out[i] = (uint8_t)rand();
#else
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) { size_t r = fread(out, 1, OC_IDEM_SIZE, f); (void)r; fclose(f); }
    else   { for (int i = 0; i < (int)OC_IDEM_SIZE; i++) out[i] = (uint8_t)rand(); }
#endif
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

/* Encode + write one SEND with a caller-chosen idempotency token (so a resend
 * from the outbox reuses it and the daemon dedups). */
static void send_message(oc_tls_conn *c, int fd, volatile int *stop,
                         uint64_t channel_id, const uint8_t idem[OC_IDEM_SIZE],
                         const char *body) {
    uint8_t buf[OC_MAX_FRAME_SIZE]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
    oc_send s; memset(&s, 0, sizeof s);   /* n_attach = 0: a plain text message */
    s.channel_id = channel_id ? channel_id : 1;
    memcpy(s.idem, idem, OC_IDEM_SIZE);
    s.body = oc_slice_str(body ? body : "");
    if (oc_encode_send(&w, OC_PROTOCOL_VERSION, &s) == OC_OK)
        (void)write_all(c, fd, buf, w.len, stop);
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

/* ---- attachment transfer (REQ-140/141, ARCH-69) ----
 * One transfer at a time per connection (matching the daemon). An upload streams
 * a local file out as UPLOAD_CHUNKs within the server-advertised window, then
 * links it into a SEND; a download writes DOWNLOAD_CHUNKs to a local file. The
 * state lives on the net thread and is driven by the serve loop (commands) and
 * dispatch (server frames). */
typedef struct {
    int      mode;        /* 0 idle, 1 upload, 2 download */
    uint64_t id;          /* attachment id (0 for an upload until UPLOAD_READY) */
    FILE    *fp;          /* the local file (source for upload, sink for download) */
    /* An in-memory download (WIN-17): `fp` is NULL and chunks accumulate here
     * instead. Inline image thumbnails must not write a file — a client stores
     * nothing locally (ARCH-88) — and a temp file for rendering would be exactly
     * the cache that decision removed. */
    uint8_t *buf;
    size_t   buf_len, buf_cap;
    uint64_t total;       /* declared/expected byte size */
    uint64_t done;        /* upload: bytes handed to chunks; download: bytes written */
    uint32_t chunk;       /* max data bytes per chunk (from UPLOAD_READY) */
    uint32_t win_chunks;  /* in-flight window, in chunks (from UPLOAD_READY) */
    uint32_t next_seq;    /* next chunk seq to send (upload) / expect (download) */
    uint32_t acked_seq;   /* upload: chunks the server has acked */
    int      ended;       /* upload: UPLOAD_END has been sent */
    uint64_t channel;     /* upload: channel to link the finished attachment into */
    /* What the finished upload is FOR (WIN-47). 0 posts it as a message, which is
     * every ordinary upload; 1 makes it the user's avatar and posts nothing. Without
     * this an avatar arrived in the transcript as a bare image nobody sent. */
    uint8_t  purpose;
    char     ename[48];   /* purpose 2: the shortcode to claim it as (REQ-072) */
    char     name[128];   /* filename (upload src basename / download dest label) */
} oc_xfer;

/* Per-channel high-water the net thread tracks (from every BROADCAST) so a
 * reconnect can BACKFILL_REQUEST from where each channel left off (REQ-101). It
 * persists across reconnects; the model dedups replays by its own high-water. */
typedef struct { uint64_t channel_id, high_water; } oc_hwrow;
typedef struct { oc_hwrow *v; size_t n, cap; } oc_hwtab;

static void hwtab_note(oc_hwtab *t, uint64_t channel_id, uint64_t message_id) {
    for (size_t i = 0; i < t->n; i++)
        if (t->v[i].channel_id == channel_id) {
            if (message_id > t->v[i].high_water) t->v[i].high_water = message_id;
            return;
        }
    if (t->n == t->cap) {
        size_t cap = t->cap ? t->cap * 2 : 16;
        oc_hwrow *nv = realloc(t->v, cap * sizeof *nv);
        if (!nv) return;
        t->v = nv; t->cap = cap;
    }
    t->v[t->n].channel_id = channel_id; t->v[t->n].high_water = message_id; t->n++;
}
static void hwtab_free(oc_hwtab *t) { free(t->v); t->v = NULL; t->n = t->cap = 0; }

/* The last-seen message id for a channel (0 if none) — the backfill cursor. */
static uint64_t hwtab_get(const oc_hwtab *t, uint64_t channel_id) {
    for (size_t i = 0; i < t->n; i++)
        if (t->v[i].channel_id == channel_id) return t->v[i].high_water;
    return 0;
}

typedef struct {
    oc_queue    *to_ui;
    oc_tls_conn *conn;
    int          fd;
    volatile int *stop;
    oc_xfer     *xfer;
    oc_hwtab    *hw;
    oc_store    *store;      /* token/pin persistence (NULL = none) */
    obox        *obox;       /* in-memory outbox, cleared on each SEND_ACK */
    const char  *workspace;
    const char  *client_type; /* which settings bucket this frontend syncs */
} disp_ctx;

/* Push a transfer notice (phase: 0 progress, 1 done, 2 error) to the UI. */
static void xfer_notice(disp_ctx *ctx, uint8_t phase, const char *msg) {
    oc_ev *e = oc_ev_new(OC_EV_XFER);
    if (e) { e->op = phase; e->body = strdup(msg); oc_queue_push(ctx->to_ui, e); }
}

/* Tear down the active transfer (closing the file), leaving it idle. */
/* Cap on an in-memory download (WIN-17). Big enough for any screenshot someone
 * pastes into chat, small enough that a hostile size cannot be used to grow the
 * client without bound. Anything larger falls back to "download to a file". */
#define OC_INLINE_MAX (8u * 1024u * 1024u)

/* The last path component, splitting on EITHER separator. Splitting only on '/'
 * meant a Windows upload declared its whole path as the filename. */
static const char *path_basename(const char *path) {
    const char *b = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\') b = p + 1;
    return b;
}

/* The content type, from the extension. Every upload used to be declared
 * application/octet-stream, so nothing downstream could tell an image from a
 * zip — which makes inline rendering (REQ-142) impossible however good the
 * client is. The server stores what we declare, so it has to be right here. */
static const char *mime_for(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return "application/octet-stream";
    static const struct { const char *ext, *mime; } T[] = {
        { "png", "image/png" },   { "jpg", "image/jpeg" }, { "jpeg", "image/jpeg" },
        { "gif", "image/gif" },   { "bmp", "image/bmp" },  { "webp", "image/webp" },
        { "svg", "image/svg+xml" },
        { "pdf", "application/pdf" },
        { "txt", "text/plain" },  { "md",  "text/markdown" }, { "csv", "text/csv" },
        { "log", "text/plain" },
        { "json", "application/json" }, { "xml", "application/xml" },
        { "zip", "application/zip" },   { "gz",  "application/gzip" },
        { "mp4", "video/mp4" },   { "webm", "video/webm" },
        { "mp3", "audio/mpeg" },  { "wav",  "audio/wav" },  { "ogg", "audio/ogg" },
    };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) {
        const char *e = dot + 1;
        const char *t = T[i].ext;
        size_t k = 0;
        while (t[k] && e[k] && tolower((unsigned char)e[k]) == t[k]) k++;
        if (!t[k] && !e[k]) return T[i].mime;
    }
    return "application/octet-stream";
}

static void xfer_reset(oc_xfer *x) {
    if (x->fp) fclose(x->fp);
    free(x->buf);
    memset(x, 0, sizeof *x);
}

/* Emit OC_EV_ATTACHMENT events for a message's attachment list (BROADCAST /
 * THREAD_REPLY), each folded onto its message by the model. */
static void push_attachments(oc_queue *to_ui, uint64_t channel_id, uint64_t message_id,
                             const oc_attach_entry *att, uint16_t n) {
    for (uint16_t i = 0; i < n && i < OC_MAX_ATTACH; i++) {
        oc_ev *e = oc_ev_new(OC_EV_ATTACHMENT);
        if (!e) continue;
        e->channel_id = channel_id;
        e->message_id = message_id;
        e->parent_id  = att[i].id;        /* attachment id (used to download) */
        e->server_time = att[i].size;
        e->status = att[i].reclaimed;   /* bytes gone; row is a tombstone */
        size_t fn = att[i].filename.len < sizeof e->author_name - 1 ? att[i].filename.len : 0;
        /* filename in body (heap, may be long), mime in author_name (bounded). */
        e->body = malloc(att[i].filename.len + 1);
        if (e->body) { memcpy(e->body, att[i].filename.ptr, att[i].filename.len); e->body[att[i].filename.len] = '\0'; }
        size_t mn = att[i].mime.len < sizeof e->author_name - 1 ? att[i].mime.len : sizeof e->author_name - 1;
        memcpy(e->author_name, att[i].mime.ptr, mn); e->author_name[mn] = '\0';
        (void)fn;
        oc_queue_push(to_ui, e);
    }
}

/* Send as many UPLOAD_CHUNKs as the window allows, then UPLOAD_END once the whole
 * file has been streamed. Called on UPLOAD_READY and each UPLOAD_ACK. */
static void upload_pump(disp_ctx *ctx) {
    oc_xfer *x = ctx->xfer;
    if (x->mode != 1 || x->id == 0 || x->ended) return;
    static uint8_t frame[OC_MAX_FRAME_SIZE];
    uint8_t data[OC_ATTACH_CHUNK_SIZE];
    while (x->done < x->total && (x->next_seq - x->acked_seq) < x->win_chunks) {
        size_t want = (x->total - x->done) < x->chunk ? (size_t)(x->total - x->done) : x->chunk;
        size_t got = fread(data, 1, want, x->fp);
        if (got != want) { xfer_notice(ctx, 2, "upload: read error"); xfer_reset(x); return; }
        oc_upload_chunk uc = { x->id, x->next_seq, { data, got } };
        oc_wbuf w; oc_wbuf_init(&w, frame, sizeof frame);
        if (oc_encode_upload_chunk(&w, OC_PROTOCOL_VERSION, &uc) != OC_OK ||
            write_all(ctx->conn, ctx->fd, frame, w.len, ctx->stop) != 0) {
            xfer_notice(ctx, 2, "upload: send failed"); xfer_reset(x); return;
        }
        x->next_seq++; x->done += got;
    }
    if (x->done == x->total && !x->ended) {
        oc_upload_end ue = { x->id };
        oc_wbuf w; oc_wbuf_init(&w, frame, sizeof frame);
        if (oc_encode_upload_end(&w, OC_PROTOCOL_VERSION, &ue) == OC_OK)
            (void)write_all(ctx->conn, ctx->fd, frame, w.len, ctx->stop);
        x->ended = 1;
    }
}

/* Dispatch every buffered server frame into UI events. Returns 0 to keep the
 * connection, -1 to drop it. */
static int dispatch(oc_framebuf *fb, oc_queue *to_ui, disp_ctx *ctx) {
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
                /* Trailing attachment metadata (REQ-140): fold each onto the message. */
                push_attachments(to_ui, b.channel_id, b.message_id, b.attach, b.n_attach);
            }
            if (ctx && ctx->hw) hwtab_note(ctx->hw, b.channel_id, b.message_id);
            /* Cache the message so a relaunch shows it instantly (ARCH-45/46). */
            if (ctx && ctx->store) {
                char an[64]; size_t al = b.author_name.len < sizeof an - 1 ? b.author_name.len : sizeof an - 1;
                memcpy(an, b.author_name.ptr, al); an[al] = '\0';
                char *cb = malloc(b.body.len + 1);
                if (cb) {
                    memcpy(cb, b.body.ptr, b.body.len); cb[b.body.len] = '\0';
                    free(cb);
                }
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
                e->op = ents[i].kind;
                e->is_public = ents[i].is_public;
                /* Sidebar ordering + badging without a local cache (ARCH-88). */
                e->server_time = ents[i].last_message_at;
                e->count = ents[i].unread;
                if (ents[i].kind == OC_CHANNEL_KIND_DM) e->user_id = ents[i].peer_id;
                e->n_peers = ents[i].n_peers;                       /* REQ-056 */
                for (uint16_t k = 0; k < ents[i].n_peers && k < 9; k++)
                    e->peers[k] = ents[i].peers[k];
                e->body = malloc(ents[i].name.len + 1);
                if (e->body) { memcpy(e->body, ents[i].name.ptr, ents[i].name.len); e->body[ents[i].name.len] = '\0'; }
                e->archived = ents[i].archived;
                e->created_at = ents[i].created_at;
                e->preview_author = ents[i].preview_author;
                e->preview = malloc(ents[i].preview.len + 1);
                if (e->preview) {
                    if (ents[i].preview.len) memcpy(e->preview, ents[i].preview.ptr, ents[i].preview.len);
                    e->preview[ents[i].preview.len] = '\0';
                }
                e->topic = malloc(ents[i].topic.len + 1);
                if (e->topic) { memcpy(e->topic, ents[i].topic.ptr, ents[i].topic.len); e->topic[ents[i].topic.len] = '\0'; }
                oc_queue_push(to_ui, e);
            }
        } else if (hdr.msg_type == OC_MSG_WORKSPACE_INFO) {
            oc_workspace_info wi;
            if (oc_decode_workspace_info(&p, &wi) != OC_OK) return -1;
            oc_ev *e = oc_ev_new(OC_EV_WORKSPACE_INFO);
            if (e) {
                e->status = wi.deployment_mode;
                e->count  = wi.max_users;
                e->body = malloc(wi.workspace_name.len + 1);
                if (e->body) { memcpy(e->body, wi.workspace_name.ptr, wi.workspace_name.len); e->body[wi.workspace_name.len] = '\0'; }
                oc_queue_push(to_ui, e);
            }
        } else if (hdr.msg_type == OC_MSG_CHANNEL_INFO) {
            oc_channel_info ci;
            if (oc_decode_channel_info(&p, &ci) == OC_OK) {
                oc_ev *e = oc_ev_new(OC_EV_CHANNEL);   /* add/update the channel */
                if (e) {
                    e->channel_id = ci.channel_id;
                    e->status = ci.joined;
                    e->op = ci.kind;
                    e->is_public = ci.is_public;
                    e->user_id = ci.peer_id;           /* DM peer, if any */
                    e->n_peers = ci.n_peers;                        /* REQ-056 */
                    for (uint16_t k = 0; k < ci.n_peers && k < 9; k++)
                        e->peers[k] = ci.peers[k];
                    e->body = malloc(ci.name.len + 1);
                    if (e->body) { memcpy(e->body, ci.name.ptr, ci.name.len); e->body[ci.name.len] = '\0'; }
                    e->archived = ci.archived;
                    e->created_at = ci.created_at;
                    e->topic = malloc(ci.topic.len + 1);
                    if (e->topic) { memcpy(e->topic, ci.topic.ptr, ci.topic.len); e->topic[ci.topic.len] = '\0'; }
                    oc_queue_push(to_ui, e);
                }
            }
        } else if (hdr.msg_type == OC_MSG_USER_LIST) {
            oc_user_list_entry ue[512]; uint16_t count = 0;
            if (oc_decode_user_list(&p, ue, 512, &count) != OC_OK) return -1;
            if (count > 512) count = 512;
            for (uint16_t i = 0; i < count; i++) {
                oc_ev *e = oc_ev_new(OC_EV_USER);
                if (!e) continue;
                e->user_id = ue[i].user_id;
                e->status = ue[i].role;
                e->op = ue[i].disabled;
                e->message_id = ue[i].avatar_id;      /* WIN-47 */
                e->body = malloc(ue[i].display_name.len + 1);
                if (e->body) { memcpy(e->body, ue[i].display_name.ptr, ue[i].display_name.len); e->body[ue[i].display_name.len] = '\0'; }
                oc_queue_push(to_ui, e);
            }
        } else if (hdr.msg_type == OC_MSG_PRESENCE_UPDATE) {
            oc_presence_update pu;
            if (oc_decode_presence_update(&p, &pu) == OC_OK) {
                oc_ev *e = oc_ev_new(OC_EV_PRESENCE);
                if (e) { e->user_id = pu.user_id; e->status = pu.status; oc_queue_push(to_ui, e); }
            }
        } else if (hdr.msg_type == OC_MSG_MENTION_UNRESOLVED) {
            /* REQ-287. Flattened to a display string here rather than in the
             * frontend, so the TUI and the GUI phrase it the same way — the same
             * reason oc_model_dm_title exists. The ids travel alongside, because
             * "add them" needs the person, not the text. */
            oc_mention_unresolved mu;
            if (oc_decode_mention_unresolved(&p, &mu) == OC_OK && mu.count) {
                oc_ev *e = oc_ev_new(OC_EV_MENTION_UNRESOLVED);
                if (e) {
                    e->channel_id = mu.channel_id;
                    e->message_id = mu.message_id;
                    e->status = (uint8_t)((mu.can_add ? 1 : 0) | (mu.is_private ? 2 : 0));
                    char names[256]; size_t at = 0;
                    for (uint16_t i = 0; i < mu.count && i < 9; i++) {
                        e->peers[i] = mu.who[i].user_id;
                        e->n_peers  = (uint16_t)(i + 1);
                        at += (size_t)snprintf(names + at, sizeof names - at, "%s%s",
                                               at ? ", " : "", mu.who[i].name);
                        if (at >= sizeof names - 1) break;
                    }
                    e->body = strdup(names);
                    oc_queue_push(to_ui, e);
                }
            }
        } else if (hdr.msg_type == OC_MSG_MEMBER_ENTRY) {
            oc_member_entry me;
            if (oc_decode_member_entry(&p, &me) == OC_OK) {
                oc_ev *e = oc_ev_new(OC_EV_CHAN_MEMBER);
                if (e) {
                    e->channel_id  = me.channel_id;
                    e->user_id     = me.user_id;
                    e->status      = me.role;
                    e->server_time = me.joined_at;
                    oc_queue_push(to_ui, e);
                }
            }
        } else if (hdr.msg_type == OC_MSG_MEMBERS) {
            oc_members mm;
            if (oc_decode_members(&p, &mm) == OC_OK) {
                oc_ev *e = oc_ev_new(OC_EV_CHAN_MEMBERS_END);
                if (e) { e->channel_id = mm.channel_id; e->count = mm.count; oc_queue_push(to_ui, e); }
            }
        } else if (hdr.msg_type == OC_MSG_FILE_ENTRY) {
            oc_file_entry fe;
            if (oc_decode_file_entry(&p, &fe) == OC_OK) {
                oc_ev *e = oc_ev_new(OC_EV_FILE);
                if (e) {
                    e->attach_id   = fe.attachment_id;
                    e->channel_id  = fe.channel_id;
                    e->message_id  = fe.message_id;
                    e->author_id   = fe.uploader_id;
                    e->size        = fe.size;
                    e->server_time = fe.created_at;
                    e->reclaimed   = fe.reclaimed;
                    size_t mn = fe.mime.len < sizeof e->emoji - 1 ? fe.mime.len : sizeof e->emoji - 1;
                    memcpy(e->emoji, fe.mime.ptr, mn);
                    e->emoji[mn] = '\0';
                    e->body = malloc(fe.filename.len + 1);
                    if (e->body) {
                        if (fe.filename.len) memcpy(e->body, fe.filename.ptr, fe.filename.len);
                        e->body[fe.filename.len] = '\0';
                    }
                    oc_queue_push(to_ui, e);
                }
            }
        } else if (hdr.msg_type == OC_MSG_FILES) {
            oc_files ff;
            if (oc_decode_files(&p, &ff) == OC_OK) {
                oc_ev *e = oc_ev_new(OC_EV_FILES_END);
                if (e) { e->channel_id = ff.channel_id; e->count = ff.count; oc_queue_push(to_ui, e); }
            }
        } else if (hdr.msg_type == OC_MSG_SAVED_UPDATED) {
            oc_saved_updated su;
            if (oc_decode_saved_updated(&p, &su) == OC_OK) {
                oc_ev *e = oc_ev_new(OC_EV_SAVED_UPDATED);
                if (e) { e->message_id = su.message_id; e->op = su.op;
                         e->pinned_at = su.saved_at; oc_queue_push(to_ui, e); }
            }
        } else if (hdr.msg_type == OC_MSG_SAVED_MSG) {
            oc_saved_msg sm;
            if (oc_decode_saved_msg(&p, &sm) == OC_OK) {
                oc_ev *e = oc_ev_new(OC_EV_SAVED_MSG);
                if (e) {
                    e->message_id = sm.message_id; e->channel_id = sm.channel_id;
                    e->author_id = sm.author_id;   e->server_time = sm.server_time;
                    e->pinned_at = sm.saved_at;
                    size_t an = sm.attach_name.len < sizeof e->author_name - 1
                              ? sm.attach_name.len : sizeof e->author_name - 1;
                    memcpy(e->author_name, sm.attach_name.ptr, an);
                    e->author_name[an] = '\0';
                    e->body = malloc(sm.body.len + 1);
                    if (e->body) { if (sm.body.len) memcpy(e->body, sm.body.ptr, sm.body.len);
                                   e->body[sm.body.len] = '\0'; }
                    oc_queue_push(to_ui, e);
                }
            }
        } else if (hdr.msg_type == OC_MSG_DRAFT) {
            /* A list entry and a device-sync push are the same frame (ARCH-101),
             * so this fold does not care which prompted it. */
            oc_draft d;
            if (oc_decode_draft(&p, &d) == OC_OK) {
                oc_ev *e = oc_ev_new(OC_EV_DRAFT);
                if (e) {
                    e->channel_id  = d.channel_id;
                    e->message_id  = d.thread_root;
                    e->server_time = d.updated_ms;
                    e->body = malloc(d.body.len + 1);
                    if (e->body) { if (d.body.len) memcpy(e->body, d.body.ptr, d.body.len);
                                   e->body[d.body.len] = '\0'; }
                    oc_queue_push(to_ui, e);
                }
            }
        } else if (hdr.msg_type == OC_MSG_SCHEDULED) {
            oc_scheduled sc;
            if (oc_decode_scheduled(&p, &sc) == OC_OK) {
                oc_ev *e = oc_ev_new(OC_EV_SCHEDULED);
                if (e) {
                    e->message_id  = sc.id;
                    e->channel_id  = sc.channel_id;
                    e->server_time = sc.send_at_ms;
                    e->op          = sc.state;
                    size_t fn = sc.fail_reason.len < sizeof e->author_name - 1
                              ? sc.fail_reason.len : sizeof e->author_name - 1;
                    memcpy(e->author_name, sc.fail_reason.ptr, fn);
                    e->author_name[fn] = '\0';
                    e->body = malloc(sc.body.len + 1);
                    if (e->body) { if (sc.body.len) memcpy(e->body, sc.body.ptr, sc.body.len);
                                   e->body[sc.body.len] = '\0'; }
                    oc_queue_push(to_ui, e);
                }
            }
        } else if (hdr.msg_type == OC_MSG_SCHEDULED_LIST) {
            oc_scheduled_list sl;
            if (oc_decode_scheduled_list(&p, &sl) == OC_OK) {
                oc_ev *e = oc_ev_new(OC_EV_SCHEDULED_END);
                if (e) { e->count = sl.count; oc_queue_push(to_ui, e); }
            }
        } else if (hdr.msg_type == OC_MSG_DRAFTS) {
            oc_drafts dl;
            if (oc_decode_drafts(&p, &dl) == OC_OK) {
                oc_ev *e = oc_ev_new(OC_EV_DRAFTS_END);
                if (e) { e->count = dl.count; oc_queue_push(to_ui, e); }
            }
        } else if (hdr.msg_type == OC_MSG_SAVED) {
            oc_saved sv;
            if (oc_decode_saved(&p, &sv) == OC_OK) {
                oc_ev *e = oc_ev_new(OC_EV_SAVED_END);
                if (e) { e->count = sv.count; oc_queue_push(to_ui, e); }
            }
        } else if (hdr.msg_type == OC_MSG_ACTIVITY_ENTRY) {
            oc_activity_entry ae;
            if (oc_decode_activity_entry(&p, &ae) == OC_OK) {
                oc_ev *e = oc_ev_new(OC_EV_ACTIVITY);
                if (e) {
                    e->status = ae.kind; e->message_id = ae.message_id;
                    e->channel_id = ae.channel_id; e->user_id = ae.actor_id;
                    e->server_time = ae.at;
                    e->body = malloc(ae.text.len + 1);
                    if (e->body) { if (ae.text.len) memcpy(e->body, ae.text.ptr, ae.text.len);
                                   e->body[ae.text.len] = '\0'; }
                    oc_queue_push(to_ui, e);
                }
            }
        } else if (hdr.msg_type == OC_MSG_ACTIVITY) {
            oc_activity ac;
            if (oc_decode_activity(&p, &ac) == OC_OK) {
                oc_ev *e = oc_ev_new(OC_EV_ACTIVITY_END);
                if (e) { e->count = ac.count; e->pinned_at = ac.seen_at; oc_queue_push(to_ui, e); }
            }
        } else if (hdr.msg_type == OC_MSG_PIN_UPDATED) {
            oc_pin_updated pu;
            if (oc_decode_pin_updated(&p, &pu) == OC_OK) {
                oc_ev *e = oc_ev_new(OC_EV_PIN);
                if (e) {
                    e->channel_id  = pu.channel_id;
                    e->message_id  = pu.message_id;
                    e->user_id     = pu.user_id;      /* the pinner */
                    e->op          = pu.op;
                    e->server_time = pu.pinned_at;
                    oc_queue_push(to_ui, e);
                }
            }
        } else if (hdr.msg_type == OC_MSG_PINNED_MSG) {
            oc_pinned_msg pm;
            if (oc_decode_pinned_msg(&p, &pm) == OC_OK) {
                oc_ev *e = oc_ev_new(OC_EV_PINNED_MSG);
                if (e) {
                    e->channel_id  = pm.channel_id;
                    e->message_id  = pm.message_id;
                    e->author_id   = pm.author_id;
                    e->server_time = pm.server_time;
                    e->user_id     = pm.pinned_by;
                    e->pinned_at   = pm.pinned_at;
                    size_t an = pm.attach_name.len < sizeof e->author_name - 1
                              ? pm.attach_name.len : sizeof e->author_name - 1;
                    memcpy(e->author_name, pm.attach_name.ptr, an);
                    e->author_name[an] = '\0';
                    e->body = malloc(pm.body.len + 1);
                    if (e->body) {
                        if (pm.body.len) memcpy(e->body, pm.body.ptr, pm.body.len);
                        e->body[pm.body.len] = '\0';
                    }
                    oc_queue_push(to_ui, e);
                }
            }
        } else if (hdr.msg_type == OC_MSG_PINS) {
            oc_pins pn;
            if (oc_decode_pins(&p, &pn) == OC_OK) {
                oc_ev *e = oc_ev_new(OC_EV_PINS_END);
                if (e) { e->channel_id = pn.channel_id; e->count = pn.count; oc_queue_push(to_ui, e); }
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
                    push_attachments(to_ui, tr.channel_id, tr.message_id, tr.attach, tr.n_attach);
                }
            }
        } else if (hdr.msg_type == OC_MSG_THREAD_META) {
            oc_thread_meta tm;
            if (oc_decode_thread_meta(&p, &tm) == OC_OK) {
                oc_ev *e = oc_ev_new(OC_EV_THREAD_META);
                if (e) { e->message_id = tm.message_id; e->count = tm.reply_count; oc_queue_push(to_ui, e); }
            }
        } else if (hdr.msg_type == OC_MSG_SEARCH_RESULTS) {
            oc_search_result_entry se[64]; uint16_t count = 0; uint8_t trunc = 0;
            if (oc_decode_search_results(&p, se, 64, &count, &trunc) != OC_OK) return -1;
            /* Our own 64-entry decode cap truncates just as surely as the
             * server's does, so report either. */
            if (count > 64) { count = 64; trunc = 1; }
            for (uint16_t i = 0; i < count; i++) {
                oc_ev *e = oc_ev_new(OC_EV_SEARCH_RESULT);
                if (!e) continue;
                e->status = (i + 1 == count) ? trunc : 0;
                e->channel_id = se[i].channel_id;
                e->message_id = se[i].message_id;
                e->author_id = se[i].author_id;
                e->server_time = se[i].server_time;
                e->body = malloc(se[i].snippet.len + 1);
                if (e->body) { memcpy(e->body, se[i].snippet.ptr, se[i].snippet.len); e->body[se[i].snippet.len] = '\0'; }
                oc_queue_push(to_ui, e);
            }
        } else if (hdr.msg_type == OC_MSG_REACTIONS) {
            oc_reaction_entry re[256]; uint16_t count = 0; uint64_t mid = 0;
            if (oc_decode_reactions(&p, re, 256, &count, &mid) != OC_OK) return -1;
            if (count > 256) count = 256;
            for (uint16_t i = 0; i < count; i++) {
                oc_ev *e = oc_ev_new(OC_EV_REACTIONS);
                if (!e) continue;
                e->message_id = mid;
                e->user_id = re[i].user_id;
                size_t n = re[i].emoji.len < sizeof e->emoji - 1 ? re[i].emoji.len : sizeof e->emoji - 1;
                memcpy(e->emoji, re[i].emoji.ptr, n); e->emoji[n] = '\0';
                oc_queue_push(to_ui, e);
            }
        } else if (hdr.msg_type == OC_MSG_NOTIFY_PREFS) {
            enum { NPREF_CAP = 256 };
            oc_notify_pref_entry ne[NPREF_CAP]; uint16_t count = 0; oc_set_dnd dnd = {0,0,0};
            uint8_t npdefault = 0;
            if (oc_decode_notify_prefs(&p, ne, NPREF_CAP, &count, &dnd, &npdefault) != OC_OK) return -1;
            if (count > NPREF_CAP) count = NPREF_CAP;
            /* Header first (the sync boundary), then one event per channel. */
            oc_ev *h = oc_ev_new(OC_EV_DND);
            if (h) {
                h->status = dnd.enabled;
                h->op = npdefault;                /* REQ-134 */
                h->count = ((uint32_t)dnd.start_min << 16) | dnd.end_min;
                oc_queue_push(to_ui, h);
            }
            for (uint16_t i = 0; i < count; i++) {
                oc_ev *e = oc_ev_new(OC_EV_NOTIFY_PREF);
                if (!e) continue;
                e->channel_id = ne[i].channel_id;
                e->op = ne[i].level;
                e->status = ne[i].muted;      /* WIN-40: distinct from the level */
                oc_queue_push(to_ui, e);
            }
        } else if (hdr.msg_type == OC_MSG_EMOJI_LIST) {
            oc_emoji_entry ee[OC_MAX_CUSTOM_EMOJI]; uint16_t count = 0;
            if (oc_decode_emoji_list(&p, ee, OC_MAX_CUSTOM_EMOJI, &count) != OC_OK) return -1;
            if (count > OC_MAX_CUSTOM_EMOJI) count = OC_MAX_CUSTOM_EMOJI;
            oc_ev *b = oc_ev_new(OC_EV_EMOJI_BEGIN);
            if (b) oc_queue_push(to_ui, b);
            for (uint16_t i = 0; i < count; i++) {
                oc_ev *e = oc_ev_new(OC_EV_EMOJI);
                if (!e) continue;
                e->message_id = ee[i].attachment_id;
                e->user_id = ee[i].created_by;
                e->body = malloc(ee[i].name.len + 1);
                if (e->body) { memcpy(e->body, ee[i].name.ptr, ee[i].name.len); e->body[ee[i].name.len] = '\0'; }
                oc_queue_push(to_ui, e);
            }
        } else if (hdr.msg_type == OC_MSG_CLIENT_SETTINGS) {
            enum { CS_CAP = OC_MAX_CLIENT_SETTINGS };
            oc_client_setting_entry ce[CS_CAP];
            oc_client_settings cst;
            if (oc_decode_client_settings(&p, &cst, ce, CS_CAP) != OC_OK) return -1;
            /* Only fold a bucket meant for this frontend; the daemon fans the sync
             * to all of a user's connections regardless of client_type. */
            const char *ct = ctx->client_type ? ctx->client_type : "";
            if (cst.client_type.len != strlen(ct) ||
                (cst.client_type.len && memcmp(cst.client_type.ptr, ct, cst.client_type.len) != 0))
                continue;
            oc_ev *h = oc_ev_new(OC_EV_SETTINGS_BEGIN);   /* clears the bucket (sync boundary) */
            if (h) oc_queue_push(to_ui, h);
            for (uint16_t i = 0; i < cst.count; i++) {
                oc_ev *e = oc_ev_new(OC_EV_SETTING);
                if (!e) continue;
                size_t kn = ce[i].key.len < sizeof e->author_name - 1 ? ce[i].key.len : sizeof e->author_name - 1;
                memcpy(e->author_name, ce[i].key.ptr, kn); e->author_name[kn] = '\0';
                e->body = malloc(ce[i].value.len + 1);
                if (e->body) { memcpy(e->body, ce[i].value.ptr, ce[i].value.len); e->body[ce[i].value.len] = '\0'; }
                oc_queue_push(to_ui, e);
            }
        } else if (hdr.msg_type == OC_MSG_AUDIT_PAGE) {
            oc_audit_entry ents[OC_AUDIT_PAGE_MAX];
            uint16_t n = 0;
            if (oc_decode_audit_page(&p, ents, OC_AUDIT_PAGE_MAX, &n) != OC_OK) return -1;
            oc_ev *b = oc_ev_new(OC_EV_AUDIT_BEGIN);
            if (b) oc_queue_push(ctx->to_ui, b);
            for (uint16_t i = 0; i < n; i++) {
                oc_ev *e = oc_ev_new(OC_EV_AUDIT);
                if (!e) break;
                e->audit.at_ms     = ents[i].at_ms;
                e->audit.actor_id  = ents[i].actor_id;
                e->audit.target_id = ents[i].target_id;
                e->audit.family    = ents[i].family;
                e->audit.outcome   = ents[i].outcome;
                slice_to_buf(ents[i].actor_name, e->audit.actor_name, sizeof e->audit.actor_name);
                slice_to_buf(ents[i].action,     e->audit.action,     sizeof e->audit.action);
                slice_to_buf(ents[i].target,     e->audit.target,     sizeof e->audit.target);
                slice_to_buf(ents[i].detail,     e->audit.detail,     sizeof e->audit.detail);
                oc_queue_push(ctx->to_ui, e);
            }
        } else if (hdr.msg_type == OC_MSG_STORAGE_STATUS) {
            oc_storage_status ss;
            if (oc_decode_storage_status(&p, &ss) != OC_OK) return -1;
            oc_ev *e = oc_ev_new(OC_EV_STORAGE);
            if (e) {
                e->storage.total_bytes  = ss.total_bytes;
                e->storage.avail_bytes  = ss.avail_bytes;
                e->storage.attach_bytes = ss.attach_bytes;
                e->storage.attach_count = ss.attach_count;
                e->storage.rec_orphan   = ss.reclaimed_orphan;
                e->storage.rec_expired  = ss.reclaimed_expired;
                e->storage.rec_evicted  = ss.reclaimed_evicted;
                e->storage.last_reclaim_ms = ss.last_reclaim_ms;
                e->storage.max_age_days = ss.max_age_days;
                e->storage.reserve_bytes = ss.reserve_bytes;
                e->storage.evict_enabled = ss.evict_enabled;
                e->storage.under_pressure = ss.under_pressure;
                oc_queue_push(ctx->to_ui, e);
            }
        } else if (hdr.msg_type == OC_MSG_READ_CURSOR) {
            oc_read_cursor rc;
            if (oc_decode_read_cursor(&p, &rc) != OC_OK) return -1;
            oc_ev *e = oc_ev_new(OC_EV_READ_CURSOR);
            if (e) { e->channel_id = rc.channel_id; e->user_id = rc.user_id; e->message_id = rc.message_id; oc_queue_push(to_ui, e); }
        } else if (hdr.msg_type == OC_MSG_PROFILE_UPDATED) {
            oc_profile_updated pu;
            if (oc_decode_profile_updated(&p, &pu) != OC_OK) return -1;
            oc_ev *e = oc_ev_new(OC_EV_PROFILE);
            if (e) {
                e->user_id = pu.user_id;
                e->body = malloc(pu.display_name.len + 1);
                if (e->body) { memcpy(e->body, pu.display_name.ptr, pu.display_name.len); e->body[pu.display_name.len] = '\0'; }
                oc_queue_push(to_ui, e);
            }
        } else if (hdr.msg_type == OC_MSG_USER_UPDATED) {
            oc_user_updated uu;
            if (oc_decode_user_updated(&p, &uu) != OC_OK) return -1;
            oc_ev *e = oc_ev_new(OC_EV_USER_UPDATED);
            if (e) { e->user_id = uu.user_id; e->status = uu.role; e->op = uu.disabled; oc_queue_push(to_ui, e); }
        } else if (hdr.msg_type == OC_MSG_INVITE_CREATED) {
            oc_invite_created ic;
            if (oc_decode_invite_created(&p, &ic) != OC_OK) return -1;
            oc_ev *e = oc_ev_new(OC_EV_INVITE);
            if (e) {
                e->op = ic.role;
                e->server_time = ic.expires_at;
                e->body = malloc(ic.token.len + 1);
                if (e->body) { memcpy(e->body, ic.token.ptr, ic.token.len); e->body[ic.token.len] = '\0'; }
                oc_queue_push(to_ui, e);
            }
        } else if (hdr.msg_type == OC_MSG_UPLOAD_READY) {
            oc_upload_ready rd;
            if (oc_decode_upload_ready(&p, &rd) == OC_OK && ctx && ctx->xfer->mode == 1) {
                oc_xfer *x = ctx->xfer;
                x->id = rd.attachment_id;
                x->chunk = rd.chunk_size ? rd.chunk_size : OC_ATTACH_CHUNK_SIZE;
                if (x->chunk > OC_ATTACH_CHUNK_SIZE) x->chunk = OC_ATTACH_CHUNK_SIZE;
                x->win_chunks = rd.window_bytes / x->chunk;
                if (x->win_chunks == 0) x->win_chunks = 1;
                upload_pump(ctx);
            }
        } else if (hdr.msg_type == OC_MSG_UPLOAD_ACK) {
            oc_upload_ack ack;
            if (oc_decode_upload_ack(&p, &ack) == OC_OK && ctx && ctx->xfer->mode == 1 &&
                ack.attachment_id == ctx->xfer->id) {
                ctx->xfer->acked_seq = ack.acked_through;
                upload_pump(ctx);
            }
        } else if (hdr.msg_type == OC_MSG_UPLOAD_OK) {
            oc_upload_ok ok;
            if (oc_decode_upload_ok(&p, &ok) == OC_OK && ctx && ctx->xfer->mode == 1 &&
                ok.attachment_id == ctx->xfer->id) {
                oc_xfer *x = ctx->xfer;
                if (x->purpose == 2) {
                    /* A custom emoji (REQ-072): claim the finished upload by name,
                     * and post nothing — the same shape as an avatar. */
                    uint8_t ef[128]; oc_wbuf ew; oc_wbuf_init(&ew, ef, sizeof ef);
                    oc_add_emoji ae = { oc_slice_str(x->ename), x->id };
                    if (oc_encode_add_emoji(&ew, OC_PROTOCOL_VERSION, &ae) == OC_OK)
                        (void)write_all(ctx->conn, ctx->fd, ef, ew.len, ctx->stop);
                    xfer_notice(ctx, 1, "emoji added");
                    xfer_reset(x);
                    continue;
                }
                if (x->purpose == 1) {
                    /* An avatar (WIN-47): claim it with SET_AVATAR instead of posting
                     * it. The daemon checks that the uploader is the setter, and its
                     * reclaim sweep excludes avatars — an attachment no message
                     * references would otherwise be collected as an orphan. */
                    uint8_t af[24]; oc_wbuf aw; oc_wbuf_init(&aw, af, sizeof af);
                    oc_set_avatar sa = { x->id };
                    if (oc_encode_set_avatar(&aw, OC_PROTOCOL_VERSION, &sa) == OC_OK)
                        (void)write_all(ctx->conn, ctx->fd, af, aw.len, ctx->stop);
                    xfer_notice(ctx, 1, "photo updated");
                    xfer_reset(x);
                    continue;
                }
                /* Publish the pending attachment by linking it into a SEND. */
                uint8_t frame[256]; oc_wbuf w; oc_wbuf_init(&w, frame, sizeof frame);
                oc_send s; memset(&s, 0, sizeof s);
                s.channel_id = x->channel;
                gen_idem(s.idem);
                s.body = oc_slice_str("");
                s.n_attach = 1; s.attach_ids[0] = x->id;
                if (oc_encode_send(&w, OC_PROTOCOL_VERSION, &s) == OC_OK)
                    (void)write_all(ctx->conn, ctx->fd, frame, w.len, ctx->stop);
                char msg[200];
                snprintf(msg, sizeof msg, "uploaded %s (%llu bytes)", x->name, (unsigned long long)ok.size);
                xfer_notice(ctx, 1, msg);
                xfer_reset(x);
            }
        } else if (hdr.msg_type == OC_MSG_DOWNLOAD_INFO) {
            oc_download_info di;
            if (oc_decode_download_info(&p, &di) == OC_OK && ctx && ctx->xfer->mode == 2 &&
                di.attachment_id == ctx->xfer->id) {
                ctx->xfer->total = di.total_size;
            }
        } else if (hdr.msg_type == OC_MSG_DOWNLOAD_CHUNK) {
            oc_download_chunk dc;
            if (oc_decode_download_chunk(&p, &dc) == OC_OK && ctx && ctx->xfer->mode == 2 &&
                dc.attachment_id == ctx->xfer->id && dc.seq == ctx->xfer->next_seq) {
                oc_xfer *x = ctx->xfer;
                int failed = 0;
                if (dc.data.len) {
                    if (x->fp) {
                        failed = fwrite(dc.data.ptr, 1, dc.data.len, x->fp) != dc.data.len;
                    } else {
                        /* Bounded: an image we are about to decode, not a file. */
                        if (x->buf_len + dc.data.len > OC_INLINE_MAX) failed = 1;
                        else {
                            if (x->buf_len + dc.data.len > x->buf_cap) {
                                size_t want = x->buf_cap ? x->buf_cap * 2 : 65536;
                                while (want < x->buf_len + dc.data.len) want *= 2;
                                uint8_t *g = realloc(x->buf, want);
                                if (!g) failed = 1; else { x->buf = g; x->buf_cap = want; }
                            }
                            if (!failed) {
                                memcpy(x->buf + x->buf_len, dc.data.ptr, dc.data.len);
                                x->buf_len += dc.data.len;
                            }
                        }
                    }
                }
                if (failed) { xfer_notice(ctx, 2, "download: write error"); xfer_reset(x); }
                else { x->done += dc.data.len; x->next_seq++; }
            }
        } else if (hdr.msg_type == OC_MSG_DOWNLOAD_END) {
            oc_download_end de;
            if (oc_decode_download_end(&p, &de) == OC_OK && ctx && ctx->xfer->mode == 2 &&
                de.attachment_id == ctx->xfer->id) {
                oc_xfer *x = ctx->xfer;
                int short_read = (x->total && x->done != x->total);
                if (!x->fp) {
                    /* In-memory: hand the bytes to the UI thread, which owns them
                     * from here. No notice — an inline thumbnail is not an event
                     * the user asked for and should not toast. */
                    if (!short_read && x->buf_len) {
                        oc_ev *e = oc_ev_new(OC_EV_ATTACHMENT_DATA);
                        if (e) {
                            e->message_id = x->id;
                            e->count = (uint32_t)x->buf_len;
                            e->body = (char *)x->buf;
                            x->buf = NULL; x->buf_len = x->buf_cap = 0;
                            oc_queue_push(ctx->to_ui, e);
                        }
                    }
                    xfer_reset(x);
                } else {
                    char msg[256];
                    if (short_read)
                        snprintf(msg, sizeof msg, "download: size mismatch (%llu/%llu)",
                                 (unsigned long long)x->done, (unsigned long long)x->total);
                    else
                        snprintf(msg, sizeof msg, "saved %s (%llu bytes)", x->name, (unsigned long long)x->done);
                    xfer_notice(ctx, short_read ? 2 : 1, msg);
                    xfer_reset(x);
                }
            }
        } else if (hdr.msg_type == OC_MSG_WEBHOOK_INFO) {
            oc_webhook_info wi;
            if (oc_decode_webhook_info(&p, &wi) != OC_OK) return -1;
            oc_ev *e = oc_ev_new(OC_EV_WEBHOOK_INFO);
            if (e) {
                e->message_id = wi.webhook_id;
                e->channel_id = wi.channel_id;
                e->body = malloc(wi.token.len + 1);
                if (e->body) { memcpy(e->body, wi.token.ptr, wi.token.len); e->body[wi.token.len] = '\0'; }
                oc_queue_push(to_ui, e);
            }
        } else if (hdr.msg_type == OC_MSG_SESSION_LIST) {
            oc_session_entry se[OC_MAX_SESSIONS]; uint16_t count = 0;
            if (oc_decode_session_list(&p, se, OC_MAX_SESSIONS, &count) != OC_OK) return -1;
            for (uint16_t i = 0; i < count; i++) {
                oc_ev *e = oc_ev_new(OC_EV_SESSION_ROW);
                if (!e) continue;
                e->message_id  = se[i].session_id;
                e->server_time = se[i].created_at;
                e->pinned_at   = se[i].last_seen;
                /* channel_id, not count: `count` is an int and an epoch-ms expiry
                 * truncates in it — the field was showing no expiry at all. A spare
                 * 64-bit slot is right there. */
                e->channel_id  = se[i].expires_at;
                e->op          = se[i].current;
                e->body = malloc(se[i].device_label.len + 1);
                if (e->body) {
                    memcpy(e->body, se[i].device_label.ptr, se[i].device_label.len);
                    e->body[se[i].device_label.len] = '\0';
                }
                oc_queue_push(to_ui, e);
            }
            oc_ev *end = oc_ev_new(OC_EV_SESSIONS_END);
            if (end) oc_queue_push(to_ui, end);
        } else if (hdr.msg_type == OC_MSG_FILE_CHANNELS) {
            oc_file_channel_entry fe[OC_MAX_FILE_CHANNELS]; uint16_t count = 0;
            if (oc_decode_file_channels(&p, fe, OC_MAX_FILE_CHANNELS, &count) != OC_OK) return -1;
            oc_ev *b = oc_ev_new(OC_EV_FILE_CHANNELS_BEGIN);
            if (b) oc_queue_push(to_ui, b);
            for (uint16_t i = 0; i < count; i++) {
                oc_ev *e = oc_ev_new(OC_EV_FILE_CHANNEL);
                if (!e) continue;
                e->channel_id = fe[i].channel_id;
                e->count = (int)fe[i].count;
                oc_queue_push(to_ui, e);
            }
        } else if (hdr.msg_type == OC_MSG_PROFILE_INFO) {
            oc_profile_info pi;
            if (oc_decode_profile_info(&p, &pi) != OC_OK) return -1;
            oc_ev *e = oc_ev_new(OC_EV_PROFILE_INFO);
            if (e) {
                e->user_id     = pi.user_id;
                e->server_time = pi.status_expires;
                e->message_id  = pi.avatar_id;
                e->op          = pi.role;
                /* Four strings in two slots plus the roster's own name field: packed
                 * with \x1f separators rather than growing oc_ev by four pointers for
                 * one event. Unpacked in model.c, next to the struct it fills. */
                size_t need = pi.display_name.len + pi.status_emoji.len +
                              pi.status_text.len + pi.title.len + pi.timezone.len + 8;
                e->body = malloc(need);
                if (e->body) {
                    int k = snprintf(e->body, need, "%.*s\x1f%.*s\x1f%.*s\x1f%.*s\x1f%.*s",
                                     (int)pi.display_name.len, (const char *)pi.display_name.ptr,
                                     (int)pi.status_emoji.len, (const char *)pi.status_emoji.ptr,
                                     (int)pi.status_text.len,  (const char *)pi.status_text.ptr,
                                     (int)pi.title.len,        (const char *)pi.title.ptr,
                                     (int)pi.timezone.len,     (const char *)pi.timezone.ptr);
                    (void)k;
                }
                oc_queue_push(to_ui, e);
            }
        } else if (hdr.msg_type == OC_MSG_INVITE_LIST) {
            /* WIN-46. Cap-safe like the webhook list above: the decoder drains the
             * whole frame and keeps what fits, so an over-long list cannot desync
             * the stream. */
            oc_invite_entry iv[OC_MAX_INVITES]; uint16_t count = 0;
            if (oc_decode_invite_list(&p, iv, OC_MAX_INVITES, &count) != OC_OK) return -1;
            for (uint16_t i = 0; i < count; i++) {
                oc_ev *e = oc_ev_new(OC_EV_INVITE_ROW);
                if (!e) continue;
                e->message_id  = iv[i].invite_id;
                e->op          = iv[i].role;
                e->server_time = iv[i].expires_at;
                e->user_id     = iv[i].created_by;
                oc_queue_push(to_ui, e);
            }
            oc_ev *end = oc_ev_new(OC_EV_INVITE_END);
            if (end) oc_queue_push(to_ui, end);
        } else if (hdr.msg_type == OC_MSG_INVITE_REVOKED) {
            oc_revoke_invite rv;
            if (oc_decode_invite_revoked(&p, &rv) != OC_OK) return -1;
            oc_ev *e = oc_ev_new(OC_EV_INVITE_REVOKED);
            if (e) { e->message_id = rv.invite_id; oc_queue_push(to_ui, e); }
        } else if (hdr.msg_type == OC_MSG_WEBHOOK_LIST) {
            oc_webhook_list_entry we[256]; uint16_t count = 0;
            if (oc_decode_webhook_list(&p, we, 256, &count) != OC_OK) return -1;
            if (count > 256) count = 256;
            for (uint16_t i = 0; i < count; i++) {
                oc_ev *e = oc_ev_new(OC_EV_WEBHOOK);
                if (!e) continue;
                e->message_id = we[i].webhook_id;
                e->channel_id = we[i].channel_id;
                e->op = we[i].disabled;
                e->body = malloc(we[i].label.len + 1);
                if (e->body) { memcpy(e->body, we[i].label.ptr, we[i].label.len); e->body[we[i].label.len] = '\0'; }
                oc_queue_push(to_ui, e);
            }
        } else if (hdr.msg_type == OC_MSG_WEBHOOK_DELETED) {
            oc_webhook_deleted wd;
            if (oc_decode_webhook_deleted(&p, &wd) != OC_OK) return -1;
            oc_ev *e = oc_ev_new(OC_EV_WEBHOOK_DELETED);
            if (e) { e->message_id = wd.webhook_id; oc_queue_push(to_ui, e); }
        } else if (hdr.msg_type == OC_MSG_SEND_ACK) {
            /* The server has durably accepted a send: clear it from the outbox so
             * it isn't resent (REQ-102). The matching BROADCAST already folded it
             * into the model. */
            oc_send_ack ack;
            if (oc_decode_send_ack(&p, &ack) == OC_OK && ctx && ctx->store)
                if (ctx->obox) obox_remove(ctx->obox, ack.idem);
        } else if (hdr.msg_type == OC_MSG_ERROR) {
            oc_error err;
            if (oc_decode_error(&p, &err) == OC_OK) {
                char msg[256];
                size_t n = err.message.len < sizeof msg - 1 ? err.message.len : sizeof msg - 1;
                memcpy(msg, err.message.ptr, n); msg[n] = '\0';
                /* Two storage conditions deserve their own wording: the server
                 * sends a generic "transfer error" string, but "this file was
                 * reclaimed" and "the server is out of space" are different
                 * situations a user can act on, and neither is a bug they
                 * should read as one (REQ-215/216). */
                if (err.code == OC_ERR_ATTACHMENT_GONE)
                    push_err(to_ui, "attachment is no longer available "
                                    "(reclaimed by the server's storage policy)");
                else if (err.code == OC_ERR_STORAGE_FULL)
                    push_err(to_ui, "upload refused: the server is low on storage");
                else
                    push_err(to_ui, msg[0] ? msg : "server error");
                /* An error ABOUT THE TRANSFER aborts it; abandon the local file.
                 *
                 * It used to abort on *any* error that happened to arrive while a
                 * transfer was running, which desynchronised the two ends: the
                 * client freed its slot and the daemon did not, so every later
                 * upload on that connection was refused with `transfer error`
                 * (netloop.c rejects UPLOAD_BEGIN when `c->xfer.state !=
                 * XFER_NONE`). The trigger was ordinary and unrelated — a
                 * "channel already exists" landing mid-upload was enough — and
                 * the symptom appeared much later and nowhere near the cause,
                 * as custom emoji and avatars silently failing to upload.
                 *
                 * The daemon already distinguishes these: send_transfer_error()
                 * puts the attachment id in `context`, and the codes below are
                 * the ones it uses. Everything else is somebody else's error and
                 * must leave the transfer alone. */
                int about_transfer =
                    err.code == OC_ERR_TRANSFER_PROTOCOL   ||
                    err.code == OC_ERR_ATTACHMENT_TOO_LARGE ||
                    err.code == OC_ERR_ATTACHMENT_GONE      ||
                    err.code == OC_ERR_UNKNOWN_ATTACHMENT   ||
                    err.code == OC_ERR_STORAGE_FULL;
                if (ctx && ctx->xfer->mode != 0 && about_transfer) xfer_reset(ctx->xfer);
                if (err.fatal) return -1;
            }
        }
        /* SEND_ACK and others are ignored in the Phase 1 skeleton. */
    }
}

/* ---- the thread ---- */

enum { RC_STOP = 0, RC_LOST = 1, RC_FATAL = 2, RC_CERT_CHANGED = 3 };

/* TOFU pinning defends against a network man-in-the-middle substituting the
 * server's certificate. A loopback connection never leaves the host, so there is
 * no MITM vector and pinning it only causes false alarms across local daemon
 * restarts (each fresh daemon self-signs a new cert). So we do not enforce the
 * pin for loopback — matching how tools skip TLS verification for localhost. */
static int is_loopback(const char *host) {
    return strcmp(host, "127.0.0.1") == 0 || strcmp(host, "::1") == 0 ||
           strcmp(host, "localhost") == 0;
}

/* One connection lifecycle: dial → TLS → handshake → auth → serve, then clean up.
 * `reconnecting` selects session-token auth (OC_AUTH_SESSION) over password; the
 * AUTH_OK session token is captured into `sess`/`*have_sess` (kept across
 * reconnects — the daemon issues no new token on reconnect). Returns RC_STOP
 * (graceful quit/logout), RC_LOST (dropped — a session reconnect may follow), or
 * RC_FATAL (version/auth reject — do not retry). `*served` is set once the serve
 * loop is entered, so the caller can shorten the backoff after a live session. */
static int run_connection(oc_net *n, int reconnecting,
                          uint8_t sess[OC_SESSION_TOKEN_LEN], int *have_sess,
                          oc_hwtab *hw, int *served, conn_store *cs) {
    int rc = RC_LOST;
    *served = 0;

    int fd = dial(n->host, n->port);
    if (fd < 0) return RC_LOST;

    oc_tls_client cli;
    oc_tls_conn conn;
    oc_framebuf fb;
    /* TOFU (ARCH-10): pin the stored fingerprint if we have one; otherwise trust
     * the presented cert this once and capture its fingerprint below. Multiple
     * oc_clients in one process (the headless test) set up TLS concurrently; safe
     * because the vendored mbedTLS is built with MBEDTLS_THREADING. */
    int enforce_pin = cs && cs->have_pin && !is_loopback(n->host);
    if (oc_tls_client_init(&cli, enforce_pin ? cs->pin : NULL) != 0 ||
        oc_tls_conn_init(&conn, &cli.conf, fd) != 0) {
        oc_closesock(fd); return RC_LOST;
    }
    oc_framebuf_init(&fb);
    /* Attachment transfer state, valid from here to `drop:` (which may reset it). */
    oc_xfer xfer; memset(&xfer, 0, sizeof xfer);

    if (do_handshake(&conn, fd, &n->stop) != 0) {
        /* A pinned handshake that fails on peer verification means the server's
         * certificate changed (TOFU mismatch) — report that distinctly, not as a
         * generic "unreachable". */
        if (enforce_pin && oc_tls_conn_cert_rejected(&conn)) rc = RC_CERT_CHANGED;
        goto drop;
    }

    /* First contact with this workspace: remember the cert fingerprint so every
     * later connection pins it (TOFU first-use, ARCH-10). */
    if (cs && !cs->have_pin && oc_tls_peer_fingerprint(&conn, cs->pin) == 0) {
        cs->have_pin = 1;
        if (cs->store) oc_store_save_pin(cs->store, cs->workspace, cs->pin);
    }

    /* HELLO -> WELCOME */
    {
        uint8_t buf[128]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
        oc_hello h = { OC_PROTOCOL_VERSION, OC_PROTOCOL_VERSION, oc_slice_str("openchime-client/0.1") };
        if (oc_encode_hello(&w, &h) != OC_OK || write_all(&conn, fd, buf, w.len, &n->stop) != 0) goto drop;
        oc_header hdr; oc_rbuf p;
        if (read_one(&conn, fd, &fb, &hdr, &p, &n->stop) != 0) goto drop;
        if (hdr.msg_type == OC_MSG_REJECT) { push_err(n->to_ui, "version rejected"); rc = RC_FATAL; goto drop; }
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

    /* AUTH -> AUTH_OK. A reconnect re-auths silently with the stored session
     * token (OC_AUTH_SESSION, ARCH-58); the first connect uses "user:password"
     * (`n->token`). The AUTH_OK session token is captured so a later drop can
     * reconnect without the password. */
    {
        uint8_t buf[1024]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
        oc_result er;
        if (reconnecting && *have_sess) {
            oc_auth a = { OC_AUTH_SESSION, { sess, OC_SESSION_TOKEN_LEN } };
            er = oc_encode_auth(&w, OC_PROTOCOL_VERSION, &a);
        } else {
            const char *cred = n->token ? n->token : "";
            const char *sep = strchr(cred, ':');
            oc_slice user = sep ? (oc_slice){ (const uint8_t *)cred, (size_t)(sep - cred) }
                                : oc_slice_str(cred);
            oc_slice pass = sep ? oc_slice_str(sep + 1) : (oc_slice){ (const uint8_t *)"", 0 };
            if (n->invite && n->invite[0]) {
                /* Signup (WIN-32): creates the account AND authenticates, so the
                 * reply below is the same AUTH_OK. Spent once — clearing it here
                 * means a later reconnect re-auths with the session token rather
                 * than replaying an invite the server has already consumed. */
                oc_redeem_invite ri = { oc_slice_str(n->invite), user, pass };
                er = oc_encode_redeem_invite(&w, OC_PROTOCOL_VERSION, &ri);
                free(n->invite);
                n->invite = NULL;
            } else {
                uint8_t cbuf[512]; oc_wbuf cw; oc_wbuf_init(&cw, cbuf, sizeof cbuf);
                if (oc_encode_local_credential(&cw, user, pass) != OC_OK) goto drop;
                oc_auth a = { OC_AUTH_LOCAL, { cbuf, cw.len } };
                er = oc_encode_auth(&w, OC_PROTOCOL_VERSION, &a);
            }
        }
        if (er != OC_OK || write_all(&conn, fd, buf, w.len, &n->stop) != 0) goto drop;
        oc_header hdr; oc_rbuf p;
        if (read_one(&conn, fd, &fb, &hdr, &p, &n->stop) != 0) goto drop;
        if (hdr.msg_type != OC_MSG_AUTH_OK) {
            /* Bad password, or an expired/revoked session on reconnect — either
             * way there is nothing to silently retry. */
            push_err(n->to_ui, reconnecting ? "session expired — please log in again" : "auth failed");
            rc = RC_FATAL; goto drop;
        }
        oc_auth_ok ok;
        oc_decode_auth_ok(&p, &ok);
        if (ok.session_token.len == OC_SESSION_TOKEN_LEN) {   /* fresh token (first auth) */
            memcpy(sess, ok.session_token.ptr, OC_SESSION_TOKEN_LEN);
            *have_sess = 1;
            if (cs && cs->store)   /* persist it so a relaunch reconnects silently */
                oc_store_save_session(cs->store, cs->workspace, sess, ok.session_expiry);
        }
        push_simple(n->to_ui, OC_EV_CONNECTED, ok.user_id);
        push_simple(n->to_ui, OC_EV_AUTH_OK, ok.user_id);

        /* Ask for the channel list + the roster so the model can populate the
         * sidebar and resolve user names (for DMs, mentions). */
        uint8_t lb[16]; oc_wbuf lw; oc_wbuf_init(&lw, lb, sizeof lb);
        if (oc_encode_list_channels(&lw, OC_PROTOCOL_VERSION) == OC_OK)
            (void)write_all(&conn, fd, lb, lw.len, &n->stop);
        oc_wbuf_init(&lw, lb, sizeof lb);
        if (oc_encode_list_users(&lw, OC_PROTOCOL_VERSION) == OC_OK)
            (void)write_all(&conn, fd, lb, lw.len, &n->stop);
        /* And this user's drafts (REQ-223). On connect, not on demand: the
         * sidebar marks conversations holding one, so the answer has to be
         * there before anything is drawn — and it is a small list. */
        oc_wbuf_init(&lw, lb, sizeof lb);
        if (oc_encode_list_drafts(&lw, OC_PROTOCOL_VERSION) == OC_OK)
            (void)write_all(&conn, fd, lb, lw.len, &n->stop);
        /* And anything waiting to be sent (REQ-224): the pane counts them and
         * a failure needs surfacing whether or not anyone opens it. */
        oc_wbuf_init(&lw, lb, sizeof lb);
        if (oc_encode_list_scheduled(&lw, OC_PROTOCOL_VERSION) == OC_OK)
            (void)write_all(&conn, fd, lb, lw.len, &n->stop);

        /* On a reconnect, recover anything missed while offline: backfill each
         * known channel from its last-seen message id (REQ-101). Replays dedup on
         * the model's high-water mark. */
        /* A cold start (nothing seen yet) sends a CURSORLESS backfill: the daemon
         * then resumes every member channel from that user's server-side read
         * position (REQ-090), which is what lets the client keep no local
         * history at all (ARCH-88). This is the FIRST connect as much as a
         * reconnect — a client with no stored state has no cursor either way.
         * Once a high-water table exists we send it, so a mid-session reconnect
         * fetches only the gap. */
        if (hw->n == 0) {
            uint8_t bb[64]; oc_wbuf bw; oc_wbuf_init(&bw, bb, sizeof bb);
            oc_backfill_request req = { 0, NULL };
            if (oc_encode_backfill_request(&bw, OC_PROTOCOL_VERSION, &req) == OC_OK)
                (void)write_all(&conn, fd, bb, bw.len, &n->stop);
        }
        if (reconnecting && hw->n > 0) {
            oc_cursor *curs = malloc(hw->n * sizeof *curs);
            if (curs) {
                for (size_t i = 0; i < hw->n; i++) {
                    curs[i].channel_id = hw->v[i].channel_id;
                    curs[i].after_message_id = hw->v[i].high_water;
                }
                uint8_t bb[OC_MAX_FRAME_SIZE]; oc_wbuf bw; oc_wbuf_init(&bw, bb, sizeof bb);
                oc_backfill_request req = { (uint16_t)(hw->n > 0xFFFF ? 0xFFFF : hw->n), curs };
                if (oc_encode_backfill_request(&bw, OC_PROTOCOL_VERSION, &req) == OC_OK)
                    (void)write_all(&conn, fd, bb, bw.len, &n->stop);
                free(curs);
            }
        }

        /* Flush the offline outbox (REQ-102): resend anything composed while
         * disconnected earlier in this process, reusing each idem so the daemon
         * dedups a partial delivery. */
        if (cs && cs->obox) {
            for (size_t i = 0; i < cs->obox->n; i++)
                send_message(&conn, fd, &n->stop, cs->obox->v[i].channel_id,
                             cs->obox->v[i].idem, cs->obox->v[i].body ? cs->obox->v[i].body : "");
        }
    }

    /* Serve: interleave reading server frames with sending queued user actions.
     * An in-flight attachment transfer (upload/download) is driven by both. */
    *served = 1;
    disp_ctx ctx = { n->to_ui, &conn, fd, &n->stop, &xfer, hw,
                     cs ? cs->store : NULL, cs ? cs->obox : NULL,
                     cs ? cs->workspace : NULL, n->client_type };
    while (!n->stop) {
        oc_cmd *c;
        while ((c = oc_queue_try_pop(n->from_ui)) != NULL) {
            if (c->type == OC_CMD_QUIT) { oc_cmd_free(c); rc = RC_STOP; goto drop; }
            if (c->type == OC_CMD_BACKFILL) {
                /* Replay history for one channel from the last id we already hold
                 * (cached-history cursor, ARCH-45/46) — 0 the first time. Replies
                 * arrive as BROADCASTs that dispatch() turns into OC_EV_MESSAGE,
                 * dedup'd by the model's high-water mark; BACKFILL_DONE is ignored. */
                uint8_t buf[64]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                oc_cursor cur = { c->channel_id, hwtab_get(hw, c->channel_id) };
                oc_backfill_request req = { 1, &cur };
                if (oc_encode_backfill_request(&w, OC_PROTOCOL_VERSION, &req) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_HISTORY) {
                /* Older messages, so nothing here touches the high-water mark:
                 * hw tracks the NEWEST id seen and a backwards page is all
                 * below it. The model dedups by id either way. */
                uint8_t buf[64]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                oc_history_request hr = { c->channel_id, c->message_id, 0 };
                if (oc_encode_history_request(&w, OC_PROTOCOL_VERSION, &hr) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_SEND && c->body) {
                /* Record in the outbox before sending (REQ-102): a drop before the
                 * SEND_ACK leaves it there to be resent, deduped by the idem. */
                uint8_t idem[OC_IDEM_SIZE]; gen_idem(idem);
                uint64_t cid = c->channel_id ? c->channel_id : 1;
                if (ctx.obox) obox_add(ctx.obox, idem, cid, c->body);
                send_message(&conn, fd, &n->stop, cid, idem, c->body);
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
                /* Parse HERE, in the core, with the shared parser (WIN-39): the wire
                 * carries predicates, not a grammar, so the daemon never has to agree
                 * with us about what "from:" means — and the TUI gets the same
                 * behaviour for free because it calls the same function. */
                oc_searchq sq;
                oc_searchq_parse(c->body, &sq);
                /* Dates are resolved to epoch-ms HERE too, because only the client
                 * knows the user's timezone; "after:2026-01-01" means local midnight. */
                uint64_t after_ms = 0, before_ms = 0;
                if (sq.after[0])  after_ms  = oc_day_start_ms(sq.after);
                if (sq.before[0]) before_ms = oc_day_end_ms(sq.before);
                uint8_t buf[768]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                oc_search s;
                memset(&s, 0, sizeof s);
                s.query      = oc_slice_str(sq.text);
                s.limit      = 50;
                s.before_id  = c->message_id;          /* WIN-38 paging cursor */
                s.from_name  = oc_slice_str(sq.from);
                s.in_channel = oc_slice_str(sq.in);
                s.has_mask   = (uint8_t)sq.has;
                s.after_ms   = after_ms;
                s.before_ms  = before_ms;
                if (oc_encode_search(&w, OC_PROTOCOL_VERSION, &s) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_CREATE_CHANNEL && c->body) {
                uint8_t buf[256]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                oc_create_channel cc = { oc_slice_str(c->body), c->op };
                if (oc_encode_create_channel(&w, OC_PROTOCOL_VERSION, &cc) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_JOIN_CHANNEL || c->type == OC_CMD_LEAVE_CHANNEL) {
                uint8_t buf[32]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                oc_channel_ref cr = { c->channel_id };
                oc_result rr = (c->type == OC_CMD_JOIN_CHANNEL)
                    ? oc_encode_join_channel(&w, OC_PROTOCOL_VERSION, &cr)
                    : oc_encode_leave_channel(&w, OC_PROTOCOL_VERSION, &cr);
                if (rr == OC_OK) (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_AUDIT_QUERY) {
                uint8_t buf[32]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                oc_audit_query aq = { c->message_id, 50 };   /* message_id = before_ms */
                if (oc_encode_audit_query(&w, OC_PROTOCOL_VERSION, &aq) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_STORAGE_STATUS) {
                uint8_t buf[16]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                if (oc_encode_storage_status_req(&w, OC_PROTOCOL_VERSION) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_LIST_CHANNELS) {
                uint8_t buf[16]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                if (oc_encode_list_channels(&w, OC_PROTOCOL_VERSION) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_LIST_USERS) {
                uint8_t buf[16]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                if (oc_encode_list_users(&w, OC_PROTOCOL_VERSION) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_SET_PRESENCE) {
                uint8_t buf[16]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                oc_set_presence sp = { c->op };
                if (oc_encode_set_presence(&w, OC_PROTOCOL_VERSION, &sp) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_PIN) {
                uint8_t buf[32]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                oc_pin pn = { c->channel_id, c->message_id, c->op };
                if (oc_encode_pin(&w, OC_PROTOCOL_VERSION, &pn) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_LIST_PINS) {
                uint8_t buf[32]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                oc_list_pins lp = { c->channel_id };
                if (oc_encode_list_pins(&w, OC_PROTOCOL_VERSION, &lp) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_UPDATE_CHANNEL) {
                uint8_t buf[512]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                oc_update_channel uc = { c->channel_id, c->op,
                                         oc_slice_str(c->body ? c->body : "") };
                if (oc_encode_update_channel(&w, OC_PROTOCOL_VERSION, &uc) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_HISTORY_AROUND) {
                uint8_t buf[32]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                oc_history_around ha = { c->channel_id, c->message_id, (uint16_t)c->op };
                if (oc_encode_history_around(&w, OC_PROTOCOL_VERSION, &ha) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_SAVE_ITEM) {
                uint8_t buf[32]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                oc_save_item si = { c->message_id, c->op };
                if (oc_encode_save_item(&w, OC_PROTOCOL_VERSION, &si) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_LIST_SAVED) {
                uint8_t buf[24]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                if (oc_encode_list_saved(&w, OC_PROTOCOL_VERSION) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_LIST_ACTIVITY) {
                uint8_t buf[24]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                if (oc_encode_list_activity(&w, OC_PROTOCOL_VERSION) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_LIST_MEMBERS) {
                uint8_t buf[32]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                oc_list_members lm = { c->channel_id };
                if (oc_encode_list_members(&w, OC_PROTOCOL_VERSION, &lm) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_LIST_FILES) {
                uint8_t buf[32]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                oc_list_files lf = { c->channel_id };
                if (oc_encode_list_files(&w, OC_PROTOCOL_VERSION, &lf) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_LIST_REACTIONS) {
                uint8_t buf[32]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                oc_list_reactions lr = { c->channel_id, c->message_id };
                if (oc_encode_list_reactions(&w, OC_PROTOCOL_VERSION, &lr) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_SET_NOTIFY_PREF) {
                uint8_t buf[24]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                oc_set_notify_pref sp = { c->channel_id, c->op };   /* op = level */
                if (oc_encode_set_notify_pref(&w, OC_PROTOCOL_VERSION, &sp) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_SET_DND) {
                uint8_t buf[24]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                /* op = enabled; channel_id = start_min, message_id = end_min. */
                oc_set_dnd sd = { c->op, (uint16_t)c->channel_id, (uint16_t)c->message_id };
                if (oc_encode_set_dnd(&w, OC_PROTOCOL_VERSION, &sd) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_LIST_NOTIFY_PREFS) {
                uint8_t buf[16]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                if (oc_encode_list_notify_prefs(&w, OC_PROTOCOL_VERSION) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_SET_SETTING) {
                static uint8_t buf[OC_MAX_FRAME_SIZE]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                oc_set_client_setting cs = { oc_slice_str(n->client_type),
                                             oc_slice_str(c->body ? c->body : ""),
                                             oc_slice_str(c->body2 ? c->body2 : "") };
                if (oc_encode_set_client_setting(&w, OC_PROTOCOL_VERSION, &cs) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_SET_DRAFT) {
                static uint8_t buf[OC_MAX_FRAME_SIZE]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                /* body2 carries the recipient list for an UNADDRESSED draft
                 * (REQ-229); empty for the ordinary channel case. */
                oc_set_draft sd = { c->channel_id, c->message_id,
                                    oc_slice_str(c->body2 ? c->body2 : ""),
                                    oc_slice_str(c->body ? c->body : "") };
                if (oc_encode_set_draft(&w, OC_PROTOCOL_VERSION, &sd) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_SCHEDULE) {
                static uint8_t buf[OC_MAX_FRAME_SIZE]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                oc_schedule_message sm = { c->channel_id, c->message_id, c->server_time,
                                           oc_slice_str(c->body ? c->body : "") };
                if (oc_encode_schedule_message(&w, OC_PROTOCOL_VERSION, &sm) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_LIST_SCHEDULED) {
                uint8_t buf[32]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                if (oc_encode_list_scheduled(&w, OC_PROTOCOL_VERSION) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_CANCEL_SCHEDULED) {
                uint8_t buf[64]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                oc_cancel_scheduled cs = { c->message_id };
                if (oc_encode_cancel_scheduled(&w, OC_PROTOCOL_VERSION, &cs) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_UPDATE_SCHEDULED) {
                static uint8_t buf[OC_MAX_FRAME_SIZE]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                oc_update_scheduled us = { c->message_id, c->server_time,
                                           oc_slice_str(c->body ? c->body : "") };
                if (oc_encode_update_scheduled(&w, OC_PROTOCOL_VERSION, &us) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_LIST_DRAFTS) {
                uint8_t buf[32]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                if (oc_encode_list_drafts(&w, OC_PROTOCOL_VERSION) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_LIST_SETTINGS) {
                uint8_t buf[64]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                oc_list_client_settings ls = { oc_slice_str(n->client_type) };
                if (oc_encode_list_client_settings(&w, OC_PROTOCOL_VERSION, &ls) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_MARK_READ) {
                uint8_t buf[32]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                oc_client_ack ca = { c->channel_id, c->message_id };
                if (oc_encode_client_ack(&w, OC_PROTOCOL_VERSION, &ca) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_SET_DISPLAY_NAME) {
                uint8_t buf[128]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                oc_set_display_name sn = { oc_slice_str(c->body ? c->body : "") };
                if (oc_encode_set_display_name(&w, OC_PROTOCOL_VERSION, &sn) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_CHANGE_PASSWORD) {
                uint8_t buf[512]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                oc_change_password cp = { oc_slice_str(c->body ? c->body : ""),
                                          oc_slice_str(c->body2 ? c->body2 : "") };
                if (oc_encode_change_password(&w, OC_PROTOCOL_VERSION, &cp) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_CHANNEL_INVITE || c->type == OC_CMD_CHANNEL_KICK) {
                uint8_t buf[32]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                oc_channel_member_op mo = { c->channel_id, c->message_id };
                oc_result rr = (c->type == OC_CMD_CHANNEL_INVITE)
                    ? oc_encode_invite_to_channel(&w, OC_PROTOCOL_VERSION, &mo)
                    : oc_encode_remove_from_channel(&w, OC_PROTOCOL_VERSION, &mo);
                if (rr == OC_OK) (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_SET_ROLE) {
                uint8_t buf[24]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                oc_set_role sr = { c->channel_id, c->op };   /* channel_id reused as user id, op = role */
                if (oc_encode_set_role(&w, OC_PROTOCOL_VERSION, &sr) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_INVITE_USER) {
                uint8_t buf[16]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                oc_invite_user iu = { c->op };   /* op = role */
                if (oc_encode_invite_user(&w, OC_PROTOCOL_VERSION, &iu) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_REMOVE_USER) {
                uint8_t buf[16]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                oc_remove_user ru = { c->channel_id };   /* channel_id reused as user id */
                if (oc_encode_remove_user(&w, OC_PROTOCOL_VERSION, &ru) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_CREATE_WEBHOOK && c->body) {
                uint8_t buf[256]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                oc_create_webhook cw = { c->channel_id, oc_slice_str(c->body) };
                if (oc_encode_create_webhook(&w, OC_PROTOCOL_VERSION, &cw) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_LIST_WEBHOOKS) {
                uint8_t buf[24]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                oc_list_webhooks lw = { c->channel_id };
                if (oc_encode_list_webhooks(&w, OC_PROTOCOL_VERSION, &lw) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_DELETE_WEBHOOK) {
                uint8_t buf[24]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                oc_delete_webhook dw = { c->message_id };   /* message_id reused as webhook id */
                if (oc_encode_delete_webhook(&w, OC_PROTOCOL_VERSION, &dw) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_LIST_INVITES) {
                uint8_t buf[16]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                if (oc_encode_list_invites(&w, OC_PROTOCOL_VERSION) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_REVOKE_INVITE) {
                uint8_t buf[24]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                oc_revoke_invite ri = { c->message_id };
                if (oc_encode_revoke_invite(&w, OC_PROTOCOL_VERSION, &ri) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_SET_WEBHOOK_STATE) {
                uint8_t buf[24]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                oc_set_webhook_state sw = { c->message_id, c->op };
                if (oc_encode_set_webhook_state(&w, OC_PROTOCOL_VERSION, &sw) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_ROTATE_WEBHOOK) {
                uint8_t buf[24]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                oc_rotate_webhook rw = { c->message_id };
                if (oc_encode_rotate_webhook(&w, OC_PROTOCOL_VERSION, &rw) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_LIST_SESSIONS) {
                uint8_t buf[16]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                if (oc_encode_list_sessions(&w, OC_PROTOCOL_VERSION) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_LIST_FILE_CHANNELS) {
                uint8_t buf[16]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                if (oc_encode_list_file_channels(&w, OC_PROTOCOL_VERSION) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_SET_STATUS) {
                uint8_t buf[256]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                oc_set_status ss = { oc_slice_str(c->body ? c->body : ""),
                                     oc_slice_str(c->body2 ? c->body2 : ""),
                                     c->message_id };
                if (oc_encode_set_status(&w, OC_PROTOCOL_VERSION, &ss) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_SET_PROFILE) {
                uint8_t buf[256]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                oc_set_profile sp = { oc_slice_str(c->body ? c->body : ""),
                                      oc_slice_str(c->body2 ? c->body2 : "") };
                if (oc_encode_set_profile(&w, OC_PROTOCOL_VERSION, &sp) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_SET_MUTE) {
                uint8_t buf[24]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                oc_set_mute sm = { c->channel_id, c->op };
                if (oc_encode_set_mute(&w, OC_PROTOCOL_VERSION, &sm) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_LIST_EMOJI) {
                uint8_t buf[16]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                if (oc_encode_list_emoji(&w, OC_PROTOCOL_VERSION) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_ADD_EMOJI && c->body) {
                uint8_t buf[128]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                oc_add_emoji ae = { oc_slice_str(c->body), c->message_id };
                if (oc_encode_add_emoji(&w, OC_PROTOCOL_VERSION, &ae) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_DELETE_EMOJI && c->body) {
                uint8_t buf[128]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                oc_delete_emoji de = { oc_slice_str(c->body) };
                if (oc_encode_delete_emoji(&w, OC_PROTOCOL_VERSION, &de) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_OPEN_GROUP_DM) {
                uint8_t buf[96]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                oc_open_group_dm gd; memset(&gd, 0, sizeof gd);
                gd.count = (uint16_t)(c->n_gids > (int)OC_MAX_GROUP_DM ? (int)OC_MAX_GROUP_DM : c->n_gids);
                for (uint16_t i = 0; i < gd.count; i++) gd.user_ids[i] = c->gids[i];
                if (oc_encode_open_group_dm(&w, OC_PROTOCOL_VERSION, &gd) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_SET_AVATAR) {
                uint8_t buf[24]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                oc_set_avatar sa = { c->message_id };
                if (oc_encode_set_avatar(&w, OC_PROTOCOL_VERSION, &sa) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_SET_NOTIFY_DEFAULT) {
                uint8_t buf[16]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                oc_set_notify_default sd = { c->op };
                if (oc_encode_set_notify_default(&w, OC_PROTOCOL_VERSION, &sd) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_SET_READ_CURSOR) {
                uint8_t buf[32]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                oc_set_read_cursor sc = { c->channel_id, c->message_id };
                if (oc_encode_set_read_cursor(&w, OC_PROTOCOL_VERSION, &sc) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_UPLOAD && c->body) {
                if (xfer.mode != 0) { xfer_notice(&ctx, 2, "busy: another transfer is in progress"); }
                else {
                    FILE *fp = fopen(c->body, "rb");
                    if (!fp) { xfer_notice(&ctx, 2, "upload: cannot open file"); }
                    else if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); xfer_notice(&ctx, 2, "upload: not a regular file"); }
                    else {
                        long sz = ftell(fp);
                        if (sz < 0 || (unsigned long long)sz > OC_MAX_ATTACHMENT_SIZE) {
                            fclose(fp); xfer_notice(&ctx, 2, "upload: file too large");
                        } else {
                            rewind(fp);
                            memset(&xfer, 0, sizeof xfer);
                            xfer.mode = 1; xfer.fp = fp; xfer.total = (uint64_t)sz;
                            xfer.chunk = OC_ATTACH_CHUNK_SIZE; xfer.win_chunks = 8;
                            xfer.channel = c->channel_id;
                            xfer.purpose = (uint8_t)(c->op == 1 ? 1 : c->op == 2 ? 2 : 0);
                            if (xfer.purpose == 2 && c->body2)
                                snprintf(xfer.ename, sizeof xfer.ename, "%s", c->body2);
                            snprintf(xfer.name, sizeof xfer.name, "%s", path_basename(c->body));
                            uint8_t buf[512]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                            oc_upload_begin ub; memset(&ub, 0, sizeof ub);
                            ub.channel_id = c->channel_id;
                            gen_idem(ub.idem);
                            ub.filename = oc_slice_str(xfer.name);
                            ub.mime = oc_slice_str(mime_for(xfer.name));
                            ub.total_size = (uint64_t)sz;
                            if (oc_encode_upload_begin(&w, OC_PROTOCOL_VERSION, &ub) == OC_OK &&
                                write_all(&conn, fd, buf, w.len, &n->stop) == 0) {
                                char msg[200]; snprintf(msg, sizeof msg, "uploading %s…", xfer.name);
                                xfer_notice(&ctx, 0, msg);
                            } else { xfer_notice(&ctx, 2, "upload: begin failed"); xfer_reset(&xfer); }
                        }
                    }
                }
            }
            if (c->type == OC_CMD_FETCH) {
                /* Same DOWNLOAD_BEGIN, no file: the bytes come back as an event
                 * (WIN-17). Silently skipped while another transfer is running —
                 * a thumbnail must never interrupt a real download, and the
                 * frontend simply re-requests on a later frame. */
                if (xfer.mode == 0) {
                    memset(&xfer, 0, sizeof xfer);
                    xfer.mode = 2; xfer.fp = NULL; xfer.id = c->message_id;
                    uint8_t buf[24]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                    oc_download_begin db = { c->message_id };
                    if (!(oc_encode_download_begin(&w, OC_PROTOCOL_VERSION, &db) == OC_OK &&
                          write_all(&conn, fd, buf, w.len, &n->stop) == 0))
                        xfer_reset(&xfer);
                }
            }
            if (c->type == OC_CMD_DOWNLOAD && c->body) {
                if (xfer.mode != 0) { xfer_notice(&ctx, 2, "busy: another transfer is in progress"); }
                else {
                    FILE *fp = fopen(c->body, "wb");
                    if (!fp) { xfer_notice(&ctx, 2, "download: cannot create file"); }
                    else {
                        memset(&xfer, 0, sizeof xfer);
                        xfer.mode = 2; xfer.fp = fp; xfer.id = c->message_id;
                        const char *slash = strrchr(c->body, '/');
                        snprintf(xfer.name, sizeof xfer.name, "%s", slash ? slash + 1 : c->body);
                        uint8_t buf[24]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                        oc_download_begin db = { c->message_id };
                        if (oc_encode_download_begin(&w, OC_PROTOCOL_VERSION, &db) == OC_OK &&
                            write_all(&conn, fd, buf, w.len, &n->stop) == 0) {
                            char msg[200]; snprintf(msg, sizeof msg, "downloading to %s…", xfer.name);
                            xfer_notice(&ctx, 0, msg);
                        } else { xfer_notice(&ctx, 2, "download: begin failed"); xfer_reset(&xfer); }
                    }
                }
            }
            if (c->type == OC_CMD_OPEN_DM) {
                uint8_t buf[16]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                oc_open_dm od = { c->channel_id };   /* channel_id reused as target user id */
                if (oc_encode_open_dm(&w, OC_PROTOCOL_VERSION, &od) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
            }
            if (c->type == OC_CMD_LOGOUT) {
                uint8_t buf[32]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                oc_logout lo = { c->op, { NULL, 0 } };   /* op = scope; empty token = this session */
                if (oc_encode_logout(&w, OC_PROTOCOL_VERSION, &lo) == OC_OK)
                    (void)write_all(&conn, fd, buf, w.len, &n->stop);
                /* The daemon revokes + closes; treat it as a graceful stop (and
                 * do not session-reconnect — the session is now dead, so drop the
                 * stored token too). */
                if (cs) cs->logged_out = 1;
                oc_cmd_free(c);
                rc = RC_STOP;
                goto drop;
            }
            oc_cmd_free(c);
        }

        if (oc_poll(fd, 0, 50) > 0) {
            uint8_t buf[4096]; size_t rn = 0;
            oc_tls_status st = oc_tls_read(&conn, buf, sizeof buf, &rn);
            if (st == OC_TLS_OK) {
                if (oc_framebuf_push(&fb, buf, rn) != 0 || dispatch(&fb, n->to_ui, &ctx) < 0) break;
            } else if (st == OC_TLS_CLOSED || st == OC_TLS_ERROR) {
                break;
            }
        }
    }

drop:
    xfer_reset(&xfer);   /* close any half-done transfer file */
    oc_framebuf_free(&fb);
    oc_tls_conn_free(&conn);
    oc_tls_client_free(&cli);
    oc_closesock(fd);
    return rc;
}

/* Replay one cached message into the model at startup: push it as an ordinary
 * OC_EV_MESSAGE (folded + deduped by the reducer), plus an OC_EV_EDIT/_DELETE so
 * the "(edited)" marker / tombstone survives a relaunch, and seed the backfill
 * cursor. */
/* The net thread: run one connection after another, silently reconnecting with
 * the session token after an unexpected drop (REQ-100). The model is preserved
 * across reconnects (dedup on high-water), so a blip is invisible beyond a brief
 * status line. Gives up on a graceful stop, a fatal reject, or before ever
 * authenticating (no session token to reconnect with). */
static void *net_thread(void *arg) {
    oc_net *n = (oc_net *)arg;
    oc_hwtab hw; memset(&hw, 0, sizeof hw);
    obox outbox; memset(&outbox, 0, sizeof outbox);
    uint8_t sess[OC_SESSION_TOKEN_LEN];
    int have_sess = 0, reconnecting = 0, backoff_ms = 0;

    /* Local store (ARCH-58): pre-load the pin + a still-valid session token for
     * this workspace, so the first connect can pin the cert and auth silently with
     * the token instead of the password. */
    conn_store cs; memset(&cs, 0, sizeof cs);
    char workspace[288];
    snprintf(workspace, sizeof workspace, "%s:%d", n->host, n->port);
    cs.workspace = workspace;
    cs.obox = &outbox;
    cs.store = n->store_path ? oc_store_open(n->store_path) : NULL;
    if (cs.store) {
        oc_store_set_secret(cs.store, n->secret);   /* token -> keyring if available */
        cs.have_pin = oc_store_load_pin(cs.store, workspace, cs.pin);
        uint64_t now_ms = (uint64_t)time(NULL) * 1000;
        if (oc_store_load_session(cs.store, workspace, sess, NULL, now_ms)) {
            have_sess = 1;
            reconnecting = 1;   /* use OC_AUTH_SESSION on the very first connect */
        }
    }

    int reach_notified = 0;   /* latch so "unreachable" isn't repeated each retry */
    while (!n->stop) {
        int served = 0;
        int rc = run_connection(n, reconnecting, sess, &have_sess, &hw, &served, &cs);
        push_simple(n->to_ui, OC_EV_DISCONNECTED, 0);   /* this connection ended */
        if (cs.store && cs.logged_out) oc_store_clear_session(cs.store, workspace);
        /* Distinguish "unreachable" from "login failed" (REQ-011): a drop before
         * ever serving, without a fatal auth reject, is a connectivity problem. */
        if (served) reach_notified = 0;
        else if (rc == RC_CERT_CHANGED && !reach_notified) {
            push_err(n->to_ui, "the server's security certificate has changed since you "
                               "last connected — if unexpected this may be a security risk; "
                               "forget this workspace (switcher: d) to trust the new one");
            reach_notified = 1;
        }
        else if (rc == RC_LOST && !reach_notified) {
            push_err(n->to_ui, "could not reach the server");
            reach_notified = 1;
        }
        if (rc == RC_STOP || n->stop) break;
        if (rc == RC_FATAL) {
            /* A session token (stored or reconnect) was rejected — drop it. If we
             * still hold a password, fall back to it once; otherwise give up. */
            if (reconnecting && cs.store) oc_store_clear_session(cs.store, workspace);
            if (reconnecting && n->token && n->token[0]) {
                have_sess = 0; reconnecting = 0; backoff_ms = 0;
                continue;   /* retry immediately with the password */
            }
            break;
        }
        if (!have_sess) break;   /* never authenticated: nothing to reconnect with */
        /* Connection lost mid-session: back off, then reconnect with the token.
         * A connection that actually served resets the backoff so the first
         * retry is prompt; repeated connect/auth failures grow it. */
        backoff_ms = served ? 500 : (backoff_ms ? (backoff_ms < 4000 ? backoff_ms * 2 : 4000) : 500);
        {
            char msg[96];
            /* State only, no keybinding: the core is frontend-agnostic, so each
             * UI adds its own affordance (the TUI a Ctrl+R hint, the GUI its
             * "Retry now" button). A TUI chord leaking into the GUI banner is
             * what this used to do. */
            snprintf(msg, sizeof msg, "connection lost — reconnecting in %ds…",
                     (backoff_ms + 999) / 1000);
            push_err(n->to_ui, msg);
        }
        /* Publish the deadline so a frontend can tick it down (WIN-55). The
         * error string can only say the delay once, which is why the GUI banner
         * showed a number that never moved. */
        {
            oc_ev *e = oc_ev_new(OC_EV_BACKOFF);
            if (e) { e->server_time = oc_model_now_ms() + (uint64_t)backoff_ms; oc_queue_push(n->to_ui, e); }
        }
        n->reconnect_now = 0;
        for (int s = 0; s < backoff_ms && !n->stop && !n->reconnect_now; s += 50) {
            oc_nanosleep(50 * 1000 * 1000);
        }
        n->reconnect_now = 0;
        { oc_ev *e = oc_ev_new(OC_EV_BACKOFF);      /* attempting now */
          if (e) { e->server_time = 0; oc_queue_push(n->to_ui, e); } }
        reconnecting = 1;
    }

    oc_store_close(cs.store);
    obox_free(&outbox);
    hwtab_free(&hw);
    return NULL;
}

/* ---- lifecycle ---- */

oc_net *oc_net_start(const char *host, int port, const char *token,
                     const char *store_path, oc_secret *secret,
                     oc_queue *to_ui, oc_queue *from_ui) {
    oc_net *n = calloc(1, sizeof *n);
    if (!n) return NULL;
    snprintf(n->host, sizeof n->host, "%s", host ? host : "127.0.0.1");
    n->port = port;
    n->token = token ? strdup(token) : NULL;
    n->store_path = (store_path && store_path[0]) ? strdup(store_path) : NULL;
    n->secret = secret;
    snprintf(n->client_type, sizeof n->client_type, "%s", "tui");
    n->to_ui = to_ui;
    n->from_ui = from_ui;
    if (oc_thread_create(&n->thread, net_thread, n) != 0) {
        free(n->token); free(n->invite); free(n->store_path); free(n); return NULL;
    }
    return n;
}

void oc_net_set_invite(oc_net *n, const char *token) {
    if (!n) return;
    free(n->invite);
    n->invite = (token && token[0]) ? strdup(token) : NULL;
}

void oc_net_reconnect(oc_net *n) {
    if (n) n->reconnect_now = 1;   /* the backoff loop polls this and retries at once */
}

void oc_net_set_client_type(oc_net *n, const char *client_type) {
    if (n && client_type && client_type[0])
        snprintf(n->client_type, sizeof n->client_type, "%s", client_type);
}

void oc_net_stop(oc_net *n) {
    if (!n) return;
    n->stop = 1;
    oc_queue_push(n->from_ui, oc_cmd_new(OC_CMD_QUIT)); /* wake it promptly */
    oc_thread_join(n->thread);
    free(n->token);
    free(n->store_path);
    free(n);
}
