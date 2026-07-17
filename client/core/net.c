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
#include <time.h>

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
    uint64_t total;       /* declared/expected byte size */
    uint64_t done;        /* upload: bytes handed to chunks; download: bytes written */
    uint32_t chunk;       /* max data bytes per chunk (from UPLOAD_READY) */
    uint32_t win_chunks;  /* in-flight window, in chunks (from UPLOAD_READY) */
    uint32_t next_seq;    /* next chunk seq to send (upload) / expect (download) */
    uint32_t acked_seq;   /* upload: chunks the server has acked */
    int      ended;       /* upload: UPLOAD_END has been sent */
    uint64_t channel;     /* upload: channel to link the finished attachment into */
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

typedef struct {
    oc_queue    *to_ui;
    oc_tls_conn *conn;
    int          fd;
    volatile int *stop;
    oc_xfer     *xfer;
    oc_hwtab    *hw;
} disp_ctx;

/* Push a transfer notice (phase: 0 progress, 1 done, 2 error) to the UI. */
static void xfer_notice(disp_ctx *ctx, uint8_t phase, const char *msg) {
    oc_ev *e = oc_ev_new(OC_EV_XFER);
    if (e) { e->op = phase; e->body = strdup(msg); oc_queue_push(ctx->to_ui, e); }
}

/* Tear down the active transfer (closing the file), leaving it idle. */
static void xfer_reset(oc_xfer *x) {
    if (x->fp) fclose(x->fp);
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
                e->body = malloc(ents[i].name.len + 1);
                if (e->body) { memcpy(e->body, ents[i].name.ptr, ents[i].name.len); e->body[ents[i].name.len] = '\0'; }
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
                    e->user_id = ci.peer_id;           /* DM peer, if any */
                    e->body = malloc(ci.name.len + 1);
                    if (e->body) { memcpy(e->body, ci.name.ptr, ci.name.len); e->body[ci.name.len] = '\0'; }
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
            if (oc_decode_notify_prefs(&p, ne, NPREF_CAP, &count, &dnd) != OC_OK) return -1;
            if (count > NPREF_CAP) count = NPREF_CAP;
            /* Header first (the sync boundary), then one event per channel. */
            oc_ev *h = oc_ev_new(OC_EV_DND);
            if (h) {
                h->status = dnd.enabled;
                h->count = ((uint32_t)dnd.start_min << 16) | dnd.end_min;
                oc_queue_push(to_ui, h);
            }
            for (uint16_t i = 0; i < count; i++) {
                oc_ev *e = oc_ev_new(OC_EV_NOTIFY_PREF);
                if (!e) continue;
                e->channel_id = ne[i].channel_id;
                e->op = ne[i].level;
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
                if (dc.data.len && fwrite(dc.data.ptr, 1, dc.data.len, x->fp) != dc.data.len) {
                    xfer_notice(ctx, 2, "download: write error"); xfer_reset(x);
                } else {
                    x->done += dc.data.len; x->next_seq++;
                }
            }
        } else if (hdr.msg_type == OC_MSG_DOWNLOAD_END) {
            oc_download_end de;
            if (oc_decode_download_end(&p, &de) == OC_OK && ctx && ctx->xfer->mode == 2 &&
                de.attachment_id == ctx->xfer->id) {
                oc_xfer *x = ctx->xfer;
                char msg[256];
                if (x->total && x->done != x->total)
                    snprintf(msg, sizeof msg, "download: size mismatch (%llu/%llu)",
                             (unsigned long long)x->done, (unsigned long long)x->total);
                else
                    snprintf(msg, sizeof msg, "saved %s (%llu bytes)", x->name, (unsigned long long)x->done);
                xfer_notice(ctx, x->total && x->done != x->total ? 2 : 1, msg);
                xfer_reset(x);
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
        } else if (hdr.msg_type == OC_MSG_ERROR) {
            oc_error err;
            if (oc_decode_error(&p, &err) == OC_OK) {
                char msg[256];
                size_t n = err.message.len < sizeof msg - 1 ? err.message.len : sizeof msg - 1;
                memcpy(msg, err.message.ptr, n); msg[n] = '\0';
                push_err(to_ui, msg[0] ? msg : "server error");
                /* An error mid-transfer aborts it; abandon the local file. */
                if (ctx && ctx->xfer->mode != 0) xfer_reset(ctx->xfer);
                if (err.fatal) return -1;
            }
        }
        /* SEND_ACK and others are ignored in the Phase 1 skeleton. */
    }
}

/* ---- the thread ---- */

enum { RC_STOP = 0, RC_LOST = 1, RC_FATAL = 2 };

/* One connection lifecycle: dial → TLS → handshake → auth → serve, then clean up.
 * `reconnecting` selects session-token auth (OC_AUTH_SESSION) over password; the
 * AUTH_OK session token is captured into `sess`/`*have_sess` (kept across
 * reconnects — the daemon issues no new token on reconnect). Returns RC_STOP
 * (graceful quit/logout), RC_LOST (dropped — a session reconnect may follow), or
 * RC_FATAL (version/auth reject — do not retry). `*served` is set once the serve
 * loop is entered, so the caller can shorten the backoff after a live session. */
static int run_connection(oc_net *n, int reconnecting,
                          uint8_t sess[OC_SESSION_TOKEN_LEN], int *have_sess,
                          oc_hwtab *hw, int *served) {
    int rc = RC_LOST;
    *served = 0;

    int fd = dial(n->host, n->port);
    if (fd < 0) return RC_LOST;

    oc_tls_client cli;
    oc_tls_conn conn;
    oc_framebuf fb;
    /* Phase 1 TOFU: trust the presented cert (pin=NULL); persisting/pinning the
     * fingerprint arrives with the client store phase. Multiple oc_clients in one
     * process (the headless test) set up TLS concurrently; that is safe because
     * the vendored mbedTLS is built with MBEDTLS_THREADING (scripts/build_mbedtls.sh). */
    if (oc_tls_client_init(&cli, NULL) != 0 || oc_tls_conn_init(&conn, &cli.conf, fd) != 0) {
        oc_closesock(fd); return RC_LOST;
    }
    oc_framebuf_init(&fb);
    /* Attachment transfer state, valid from here to `drop:` (which may reset it). */
    oc_xfer xfer; memset(&xfer, 0, sizeof xfer);

    if (do_handshake(&conn, fd, &n->stop) != 0) goto drop;

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
            uint8_t cbuf[512]; oc_wbuf cw; oc_wbuf_init(&cw, cbuf, sizeof cbuf);
            if (oc_encode_local_credential(&cw, user, pass) != OC_OK) goto drop;
            oc_auth a = { OC_AUTH_LOCAL, { cbuf, cw.len } };
            er = oc_encode_auth(&w, OC_PROTOCOL_VERSION, &a);
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

        /* On a reconnect, recover anything missed while offline: backfill each
         * known channel from its last-seen message id (REQ-101). Replays dedup on
         * the model's high-water mark. */
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
    }

    /* Serve: interleave reading server frames with sending queued user actions.
     * An in-flight attachment transfer (upload/download) is driven by both. */
    *served = 1;
    disp_ctx ctx = { n->to_ui, &conn, fd, &n->stop, &xfer, hw };
    while (!n->stop) {
        oc_cmd *c;
        while ((c = oc_queue_try_pop(n->from_ui)) != NULL) {
            if (c->type == OC_CMD_QUIT) { oc_cmd_free(c); rc = RC_STOP; goto drop; }
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
            if (c->type == OC_CMD_CREATE_CHANNEL && c->body) {
                uint8_t buf[256]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                oc_create_channel cc = { oc_slice_str(c->body), 1 };   /* public */
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
                            const char *slash = strrchr(c->body, '/');
                            snprintf(xfer.name, sizeof xfer.name, "%s", slash ? slash + 1 : c->body);
                            uint8_t buf[512]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
                            oc_upload_begin ub; memset(&ub, 0, sizeof ub);
                            ub.channel_id = c->channel_id;
                            gen_idem(ub.idem);
                            ub.filename = oc_slice_str(xfer.name);
                            ub.mime = oc_slice_str("application/octet-stream");
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
                 * do not session-reconnect — the session is now dead). */
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

/* The net thread: run one connection after another, silently reconnecting with
 * the session token after an unexpected drop (REQ-100). The model is preserved
 * across reconnects (dedup on high-water), so a blip is invisible beyond a brief
 * status line. Gives up on a graceful stop, a fatal reject, or before ever
 * authenticating (no session token to reconnect with). */
static void *net_thread(void *arg) {
    oc_net *n = (oc_net *)arg;
    oc_hwtab hw; memset(&hw, 0, sizeof hw);
    uint8_t sess[OC_SESSION_TOKEN_LEN];
    int have_sess = 0, reconnecting = 0, backoff_ms = 0;

    while (!n->stop) {
        int served = 0;
        int rc = run_connection(n, reconnecting, sess, &have_sess, &hw, &served);
        push_simple(n->to_ui, OC_EV_DISCONNECTED, 0);   /* this connection ended */
        if (rc == RC_STOP || rc == RC_FATAL || n->stop || !have_sess) break;
        /* Connection lost mid-session: back off, then reconnect with the token.
         * A connection that actually served resets the backoff so the first
         * retry is prompt; repeated connect/auth failures grow it. */
        backoff_ms = served ? 500 : (backoff_ms ? (backoff_ms < 4000 ? backoff_ms * 2 : 4000) : 500);
        push_err(n->to_ui, "connection lost — reconnecting…");
        for (int s = 0; s < backoff_ms && !n->stop; s += 50) {
            struct timespec ts = { 0, 50 * 1000 * 1000 };
            nanosleep(&ts, NULL);
        }
        reconnecting = 1;
    }

    hwtab_free(&hw);
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
