/* Call signaling and media keys — see callsig.h and docs/details/CALLS.md §5. */

#include "callsig.h"

#include "e2e_hpke.h"
#include "model.h"

#include <stdio.h>
#include <stdlib.h>
#include "env.h"           /* oc_env_dev */
#include <string.h>

void oc_callsig_init(oc_callsig *cs) {
    memset(cs, 0, sizeof *cs);
    oc_mutex_init(&cs->mu);
    oc_mutex_init(&cs->tq_mu);
}

void oc_callsig_destroy(oc_callsig *cs) {
    oc_e2e_wipe(cs->sk, sizeof cs->sk);
    oc_mutex_destroy(&cs->mu);
    oc_mutex_destroy(&cs->tq_mu);
}

/* The engine's way onto the connection: queue one packet, dropping the oldest
 * when full, and wake the connection thread to write it. Any thread; never
 * blocks on the connection. The wake is called under the queue's lock, so one
 * cleared by oc_callsig_set_wake is never called after. */
static int tcp_enqueue(void *sctx, uint16_t seq, uint8_t stream, const uint8_t *ct, size_t len) {
    oc_callsig *cs = sctx;
    if (len > OC_CALLSIG_TCPMAX) return -1;
    oc_mutex_lock(&cs->tq_mu);
    if (cs->tq_n == OC_CALLSIG_TCPQ) { cs->tq_head = (cs->tq_head + 1) % OC_CALLSIG_TCPQ; cs->tq_n--; }
    int k = (cs->tq_head + cs->tq_n) % OC_CALLSIG_TCPQ;
    cs->tq[k].seq = seq;
    cs->tq[k].stream = stream;
    cs->tq[k].len = (uint16_t)len;
    if (len) memcpy(cs->tq[k].ct, ct, len);
    cs->tq_n++;
    if (cs->wake) cs->wake(cs->wake_ctx);
    oc_mutex_unlock(&cs->tq_mu);
    return 0;
}

void oc_callsig_set_wake(oc_callsig *cs, void (*wake)(void *wctx), void *wctx) {
    oc_mutex_lock(&cs->tq_mu);
    cs->wake = wake;
    cs->wake_ctx = wctx;
    oc_mutex_unlock(&cs->tq_mu);
}

int oc_callsig_pump(oc_callsig *cs, oc_callsig_write write, void *wctx) {
    for (;;) {
        uint8_t ct[OC_CALLSIG_TCPMAX];
        uint16_t seq, len;
        uint8_t stream;
        oc_mutex_lock(&cs->tq_mu);
        if (!cs->tq_n) { oc_mutex_unlock(&cs->tq_mu); break; }
        seq = cs->tq[cs->tq_head].seq;
        stream = cs->tq[cs->tq_head].stream;
        len = cs->tq[cs->tq_head].len;
        memcpy(ct, cs->tq[cs->tq_head].ct, len);
        cs->tq_head = (cs->tq_head + 1) % OC_CALLSIG_TCPQ;
        cs->tq_n--;
        oc_mutex_unlock(&cs->tq_mu);
        uint8_t buf[OC_CALLSIG_TCPMAX + 32];
        oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
        oc_call_media_pkt m = { 0, seq, { ct, len }, stream };
        if (oc_encode_call_media_up(&w, OC_PROTOCOL_VERSION, &m) == OC_OK) write(wctx, buf, w.len);
    }
    return 0;
}

void oc_callsig_set_media(oc_callsig *cs, const oc_call_media *media, void *ctx) {
    oc_mutex_lock(&cs->mu);
    cs->media = media;
    cs->mctx = ctx;
    oc_mutex_unlock(&cs->mu);
}

void oc_callsig_info(uint64_t call_id, uint32_t epoch, uint64_t sender, uint64_t recipient,
                     uint8_t out[OC_CALLSIG_INFO_LEN]) {
    static const char label[] = "OpenChime call key v1";   /* 21 bytes, no NUL */
    size_t n = 0;
    memcpy(out, label, sizeof label - 1); n += sizeof label - 1;
    for (int i = 0; i < 8; i++) out[n++] = (uint8_t)(call_id >> (56 - 8 * i));
    for (int i = 0; i < 4; i++) out[n++] = (uint8_t)(epoch >> (24 - 8 * i));
    for (int i = 0; i < 8; i++) out[n++] = (uint8_t)(sender >> (56 - 8 * i));
    for (int i = 0; i < 8; i++) out[n++] = (uint8_t)(recipient >> (56 - 8 * i));
}

/* --- events for the model --------------------------------------------------- */

static void push_view(oc_queue *to_ui, int type, const oc_callsig *cs, uint64_t starter, uint64_t started) {
    oc_ev *e = oc_ev_new(type);
    if (!e) return;
    e->call_id = cs->call_id;
    if (!(e->call = calloc(1, sizeof *e->call))) { oc_ev_free(e); return; }
    oc_call_view *v = e->call;
    v->call_id = cs->call_id;
    v->starter = starter;
    v->started_at = started;
    v->epoch = cs->epoch;
    v->my_slot = cs->slot;
    v->n_parts = cs->n;
    for (uint16_t i = 0; i < cs->n; i++) {
        v->parts[i] = cs->parts[i].user_id;
        v->slots[i] = cs->parts[i].slot;
        v->away[i] = (cs->parts[i].flags & OC_CALL_PART_AWAY) != 0;
        v->camera[i] = (cs->parts[i].flags & OC_CALL_PART_CAMERA) != 0;
        v->recorder[i] = (cs->parts[i].flags & OC_CALL_PART_RECORDER) != 0;
    }
    oc_queue_push(to_ui, e);
}

static void push_left(oc_queue *to_ui, uint64_t call_id) {
    oc_ev *e = oc_ev_new(OC_EV_CALL_LEFT);
    if (!e) return;
    e->call_id = call_id;
    oc_queue_push(to_ui, e);
}

/* --- keys ------------------------------------------------------------------- */

/* A new epoch: make this device's key for it, hand it to the engine, and seal a
 * copy to every other participant's device key in one CALL_KEY (CALLS.md §5.3). */
static void rekey(oc_callsig *cs, oc_callsig_write write, void *wctx) {
    uint8_t key[OC_CALL_KEY_LEN];
    if (oc_e2e_random(key, sizeof key) != 0) return;
    oc_mutex_lock(&cs->mu);
    if (cs->media && cs->media->tx_key) cs->media->tx_key(cs->mctx, cs->epoch, cs->slot, key);
    oc_mutex_unlock(&cs->mu);

    uint8_t sealed[OC_MAX_CALL_PARTICIPANTS][OC_CALL_SEALED_LEN];
    oc_call_key_entry ents[OC_MAX_CALL_PARTICIPANTS];
    uint16_t ne = 0;
    for (uint16_t i = 0; i < cs->n; i++) {
        const oc_call_part *pt = &cs->parts[i];
        if (pt->slot == cs->slot) continue;          /* not to ourselves */
        uint8_t info[OC_CALLSIG_INFO_LEN];
        oc_callsig_info(cs->call_id, cs->epoch, cs->self_user, pt->user_id, info);
        uint8_t *s = sealed[ne];
        if (oc_hpke_seal_auth(pt->device_key, cs->sk, info, sizeof info, NULL, 0,
                              key, sizeof key, s, s + OC_X25519_LEN) != 0) continue;
        ents[ne].recipient = pt->user_id;
        ents[ne].sealed = (oc_slice){ s, OC_CALL_SEALED_LEN };
        ne++;
    }
    oc_e2e_wipe(key, sizeof key);
    if (ne) {
        uint8_t buf[OC_MAX_CALL_PARTICIPANTS * (OC_CALL_SEALED_LEN + 16) + 64];
        oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
        oc_call_key ck = { cs->call_id, cs->epoch, ne, ents };
        if (oc_encode_call_key(&w, OC_PROTOCOL_VERSION, &ck) == OC_OK) write(wctx, buf, w.len);
    }
    oc_e2e_wipe(sealed, sizeof sealed);
}

static void media_roster(oc_callsig *cs) {
    oc_mutex_lock(&cs->mu);
    if (cs->media && cs->media->roster) cs->media->roster(cs->mctx, cs->epoch, cs->parts, cs->n);
    oc_mutex_unlock(&cs->mu);
}

static void media_stop(oc_callsig *cs) {
    oc_mutex_lock(&cs->mu);
    if (cs->media && cs->media->stop) cs->media->stop(cs->mctx);
    oc_mutex_unlock(&cs->mu);
}

static void leave_local(oc_callsig *cs, oc_queue *to_ui) {
    if (!cs->in_call) return;
    uint64_t call = cs->call_id;
    media_stop(cs);
    cs->in_call = 0;
    cs->n = 0;
    cs->call_id = 0;
    push_left(to_ui, call);
}

/* --- getting back in (PROTOCOL.md §5.17) -------------------------------------- */

static uint64_t rejoin_window(const oc_callsig *cs) { return cs->rejoin_ms ? cs->rejoin_ms : OC_CALLSIG_REJOIN_MS; }

static void push_call(oc_queue *to_ui, int type, uint64_t call_id) {
    oc_ev *e = oc_ev_new(type);
    if (!e) return;
    e->call_id = call_id;
    oc_queue_push(to_ui, e);
}

/* Out of the call by no act of this device's: media stops -- its token is dead
 * -- but the call is remembered, to get back into. `connected`: on a
 * connection that is signed in now, as after a sweep or a moved path. */
static void rejoin_begin(oc_callsig *cs, oc_queue *to_ui, int connected) {
    uint64_t call = cs->call_id;
    media_stop(cs);
    cs->in_call = 0;
    cs->n = 0;
    cs->call_id = 0;
    cs->rj.on = 1;
    cs->rj.call_id = call;
    cs->rj.since_ms = cs->now_ms;
    cs->rj.authed_ms = connected ? cs->now_ms : 0;
    cs->rj.sent = 0;
    push_call(to_ui, OC_EV_CALL_REJOINING, call);
}

/* No more rejoining: LOST when it could not be done, LEFT when this device
 * left, 0 when a join of its own took over. */
static void rejoin_end(oc_callsig *cs, oc_queue *to_ui, int ev) {
    uint64_t call = cs->rj.call_id;
    memset(&cs->rj, 0, sizeof cs->rj);
    if (ev) push_call(to_ui, ev, call);
}

static int send_join(oc_callsig *cs, uint64_t call_id, oc_callsig_write write, void *wctx) {
    uint8_t buf[512];
    oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
    oc_mutex_lock(&cs->mu);
    uint8_t codecs = cs->media ? cs->media->codecs : 0;
    oc_mutex_unlock(&cs->mu);
    oc_call_join cj = { call_id, {0}, codecs };
    memcpy(cj.device_key, cs->pk, OC_CALL_DEVICE_KEY_LEN);
    if (oc_encode_call_join(&w, OC_PROTOCOL_VERSION, &cj) != OC_OK) return -1;
    return write(wctx, buf, w.len);
}

/* The join that gets back in -- unless this device has rejoined too often
 * lately, which a path that keeps failing would make it do for ever. */
static void rejoin_send(oc_callsig *cs, oc_callsig_write write, void *wctx, oc_queue *to_ui) {
    int recent = 0, oldest = 0;
    for (int i = 0; i < OC_CALLSIG_REJOINS; i++) {
        if (cs->rj_at[i] && cs->now_ms - cs->rj_at[i] < rejoin_window(cs)) recent++;
        if (cs->rj_at[i] < cs->rj_at[oldest]) oldest = i;
    }
    if (recent >= OC_CALLSIG_REJOINS) { rejoin_end(cs, to_ui, OC_EV_CALL_LOST); return; }
    cs->rj_at[oldest] = cs->now_ms ? cs->now_ms : 1;
    cs->rj.sent = 1;
    if (send_join(cs, cs->rj.call_id, write, wctx) != 0) rejoin_end(cs, to_ui, OC_EV_CALL_LOST);
}

void oc_callsig_lost(oc_callsig *cs, oc_queue *to_ui, uint64_t now_ms) {
    cs->now_ms = now_ms;
    if (cs->in_call) {
        rejoin_begin(cs, to_ui, 0);
    } else if (cs->rj.on) {
        cs->rj.authed_ms = 0;       /* lost again on the way back: wait for the next */
        cs->rj.sent = 0;
    }
}

void oc_callsig_authed(oc_callsig *cs, uint64_t now_ms) {
    cs->now_ms = now_ms;
    if (cs->rj.on) { cs->rj.authed_ms = now_ms ? now_ms : 1; cs->rj.sent = 0; }
}

void oc_callsig_quit(oc_callsig *cs, oc_callsig_write write, void *wctx, oc_queue *to_ui) {
    if (cs->in_call) {
        oc_call_leave cl = { cs->call_id };
        uint8_t buf[32]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
        if (oc_encode_call_leave(&w, OC_PROTOCOL_VERSION, &cl) == OC_OK) write(wctx, buf, w.len);
        leave_local(cs, to_ui);
    }
    if (cs->rj.on) rejoin_end(cs, to_ui, OC_EV_CALL_LEFT);
}

void oc_callsig_refused(oc_callsig *cs, oc_queue *to_ui) {
    if (cs->rj.on && cs->rj.sent) rejoin_end(cs, to_ui, OC_EV_CALL_LOST);
}

void oc_callsig_tick(oc_callsig *cs, uint64_t now_ms, oc_callsig_write write, void *wctx, oc_queue *to_ui) {
    cs->now_ms = now_ms;
    if (cs->in_call && write) {
        oc_mutex_lock(&cs->mu);
        int lost = cs->media && cs->media->lost && cs->media->lost(cs->mctx);
        oc_mutex_unlock(&cs->mu);
        if (lost) {
            rejoin_begin(cs, to_ui, 1);
            rejoin_send(cs, write, wctx, to_ui);
        }
    }
    if (!cs->rj.on) return;
    if (now_ms - cs->rj.since_ms >= rejoin_window(cs)) { rejoin_end(cs, to_ui, OC_EV_CALL_LOST); return; }
    /* Signed in, and the daemon has not reported the call: it is over. */
    uint64_t wait = cs->state_wait_ms ? cs->state_wait_ms : OC_CALLSIG_STATE_WAIT_MS;
    if (write && cs->rj.authed_ms && !cs->rj.sent && now_ms - cs->rj.authed_ms >= wait)
        rejoin_end(cs, to_ui, OC_EV_CALL_LOST);
}

static int find_slot(const oc_callsig *cs, uint8_t slot) {
    for (uint16_t i = 0; i < cs->n; i++) if (cs->parts[i].slot == slot) return i;
    return -1;
}

/* --- commands ---------------------------------------------------------------- */

int oc_callsig_command(oc_callsig *cs, const oc_cmd *c, oc_store *store, const char *workspace,
                       oc_callsig_write write, void *wctx, oc_queue *to_ui) {
    uint8_t buf[512];
    oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
    oc_result rc = OC_E_MALFORMED;
    switch (c->type) {
    case OC_CMD_CALL_START:
    case OC_CMD_CALL_JOIN: {
        if (!cs->have_key) {
            if (oc_store_device_key(store, workspace, cs->sk, cs->pk) < 0) return -1;
            cs->have_key = 1;
        }
        oc_mutex_lock(&cs->mu);
        uint8_t codecs = cs->media ? cs->media->codecs : 0;
        oc_mutex_unlock(&cs->mu);
        if (c->type == OC_CMD_CALL_JOIN) {
            oc_call_join cj = { c->call_id, {0}, codecs };
            memcpy(cj.device_key, cs->pk, OC_CALL_DEVICE_KEY_LEN);
            rc = oc_encode_call_join(&w, OC_PROTOCOL_VERSION, &cj);
        } else {
            oc_call_start st = { c->channel_id, oc_slice_str(c->body ? c->body : ""), {0}, codecs,
                                 c->n_uids, c->uids };
            memcpy(st.device_key, cs->pk, OC_CALL_DEVICE_KEY_LEN);
            rc = oc_encode_call_start(&w, OC_PROTOCOL_VERSION, &st);
        }
        break;
    }
    case OC_CMD_CALL_RECORD: {
        oc_call_record rec = { c->call_id, c->op ? 1 : 0 };
        rc = oc_encode_call_record(&w, OC_PROTOCOL_VERSION, &rec);
        break;
    }
    case OC_CMD_CALL_HISTORY: {
        /* A page at a time; development mode may make it smaller, so a test
         * reaches the next page with a handful of calls. */
        const char *dev = oc_env_dev("OPENCHIME_DEV_CALL_HISTORY_PAGE");
        int lim = dev ? atoi(dev) : 0;
        oc_call_history h = { c->start_ms, (uint16_t)(lim > 0 && lim <= (int)OC_CALL_HISTORY_PAGE ? (unsigned)lim : OC_CALL_HISTORY_PAGE) };
        rc = oc_encode_call_history(&w, OC_PROTOCOL_VERSION, &h);
        break;
    }
    case OC_CMD_CALL_DETAIL: {
        oc_call_detail d = { c->call_id, c->req_id };
        rc = oc_encode_call_detail(&w, OC_PROTOCOL_VERSION, &d);
        break;
    }
    case OC_CMD_CALL_DELETE: {
        oc_call_delete d = { c->call_id, c->op, c->message_id };
        rc = oc_encode_call_delete(&w, OC_PROTOCOL_VERSION, OC_MSG_CALL_DELETE, &d);
        break;
    }
    case OC_CMD_CALL_SUBJECT: {
        oc_call_subject su = { c->call_id, oc_slice_str(c->body ? c->body : "") };
        rc = oc_encode_call_subject(&w, OC_PROTOCOL_VERSION, &su);
        break;
    }
    case OC_CMD_CALL_INVITE: {
        oc_call_invite ci = { c->call_id, c->n_uids, c->uids };
        rc = oc_encode_call_invite(&w, OC_PROTOCOL_VERSION, &ci);
        break;
    }
    case OC_CMD_CALL_LEAVE: {
        oc_call_leave cl = { c->call_id };
        rc = oc_encode_call_leave(&w, OC_PROTOCOL_VERSION, &cl);
        break;
    }
    case OC_CMD_CALL_DECLINE: {
        oc_call_decline cd = { c->call_id };
        rc = oc_encode_call_decline(&w, OC_PROTOCOL_VERSION, &cd);
        break;
    }
    case OC_CMD_CALL_END: {
        oc_call_end ce = { c->call_id };
        rc = oc_encode_call_end(&w, OC_PROTOCOL_VERSION, &ce);
        break;
    }
    case OC_CMD_CALL_SHARE: {
        oc_call_share sh = { c->call_id, c->op ? 1 : 0 };
        rc = oc_encode_call_share(&w, OC_PROTOCOL_VERSION, &sh);
        break;
    }
    case OC_CMD_CALL_CAMERA: {
        oc_call_camera cm = { c->call_id, c->op ? 1 : 0 };
        rc = oc_encode_call_camera(&w, OC_PROTOCOL_VERSION, &cm);
        break;
    }
    case OC_CMD_CALL_VIDEO_SUB: {
        oc_call_video_sub vs = { c->call_id, c->op ? 1 : 0, c->op ? 0 : c->n_uids, c->uids };
        rc = oc_encode_call_video_sub(&w, OC_PROTOCOL_VERSION, &vs);
        break;
    }
    default:
        return -1;
    }
    if (rc != OC_OK) return -1;
    int wr = write(wctx, buf, w.len);
    /* Leaving is this device's own act: it is out now, whatever the daemon says
     * next -- which to a leaver is nothing about the call's participants. A
     * leave while getting back in stops that; a join of its own takes over. */
    if (c->type == OC_CMD_CALL_LEAVE && cs->in_call && cs->call_id == c->call_id) leave_local(cs, to_ui);
    if (c->type == OC_CMD_CALL_LEAVE && cs->rj.on && cs->rj.call_id == c->call_id)
        rejoin_end(cs, to_ui, OC_EV_CALL_LEFT);
    if ((c->type == OC_CMD_CALL_JOIN || c->type == OC_CMD_CALL_START) && cs->rj.on) rejoin_end(cs, to_ui, 0);
    return wr;
}

/* --- frames ------------------------------------------------------------------ */

static void copy_str(char *dst, size_t cap, oc_slice s) {
    size_t n = s.len < cap - 1 ? s.len : cap - 1;
    if (n) memcpy(dst, s.ptr, n);
    dst[n] = '\0';
}

/* CALL_HISTORY_PAGE: the rows, as the model keeps them. */
static int history_page(oc_rbuf *p, oc_queue *to_ui) {
    oc_call_hist_entry *ent = calloc(OC_CALL_HISTORY_PAGE, sizeof *ent);
    oc_call_history_page pg;
    if (!ent || oc_decode_call_history_page(p, &pg, ent, OC_CALL_HISTORY_PAGE) != OC_OK) { free(ent); return -1; }
    oc_ev *e = oc_ev_new(OC_EV_CALL_HISTORY);
    oc_call_history_view *v = e ? calloc(1, sizeof *v) : NULL;
    if (!v) { oc_ev_free(e); free(ent); return 1; }
    v->count = pg.count;
    v->more = pg.more;
    for (uint16_t i = 0; i < pg.count; i++) {
        const oc_call_hist_entry *x = &ent[i];
        oc_call_hist_view *r = &v->rows[i];
        r->call_id = x->call_id; r->starter = x->starter; r->started_at = x->started_at; r->ended_at = x->ended_at;
        r->from_channel = x->from_channel; r->chat_channel = x->chat_channel;
        r->end_reason = x->end_reason; r->flags = x->flags; r->n_people = x->n_people;
        r->n_faces = x->n_faces <= OC_CALL_HIST_FACES ? x->n_faces : (uint8_t)OC_CALL_HIST_FACES;
        memcpy(r->faces, x->faces, sizeof r->faces);
        copy_str(r->subject, sizeof r->subject, x->subject);
    }
    free(ent);
    e->call_hist = v;
    oc_queue_push(to_ui, e);
    return 1;
}

/* CALL_PAGE: the call's page, its text copied out of the frame. */
static int call_page(oc_rbuf *p, oc_queue *to_ui) {
    oc_call_stay *stays = calloc(OC_CALL_PAGE_STAYS, sizeof *stays);
    oc_call_invitee *inv = calloc(OC_CALL_PAGE_INVITEES, sizeof *inv);
    oc_call_event *evs = calloc(OC_CALL_PAGE_EVENTS, sizeof *evs);
    oc_call_recording *recs = calloc(OC_CALL_PAGE_RECORDINGS, sizeof *recs);
    oc_call_tline *lines = calloc(OC_CALL_PAGE_LINES, sizeof *lines);
    oc_call_page pg;
    oc_call_page_view *v = calloc(1, sizeof *v);
    int rc = 1;
    if (!stays || !inv || !evs || !recs || !lines || !v) goto out;
    if (oc_decode_call_page(p, &pg, stays, inv, evs, recs, lines) != OC_OK) { rc = -1; goto out; }
    v->call_id = pg.call_id; v->starter = pg.starter; v->started_at = pg.started_at; v->ended_at = pg.ended_at;
    v->end_reason = pg.end_reason; v->from_channel = pg.from_channel; v->chat_channel = pg.chat_channel;
    copy_str(v->subject, sizeof v->subject, pg.subject);
    v->n_stays = pg.n_stays;           memcpy(v->stays, stays, pg.n_stays * sizeof *stays);
    v->n_invitees = pg.n_invitees;     memcpy(v->invitees, inv, pg.n_invitees * sizeof *inv);
    v->n_recordings = pg.n_recordings; memcpy(v->recordings, recs, pg.n_recordings * sizeof *recs);
    v->n_events = pg.n_events;
    for (uint16_t i = 0; i < pg.n_events; i++) {
        v->events[i].at = evs[i].at; v->events[i].user_id = evs[i].user_id; v->events[i].kind = evs[i].kind;
        copy_str(v->events[i].detail, sizeof v->events[i].detail, evs[i].detail);
    }
    v->has_transcript = pg.has_transcript;
    copy_str(v->transcript_lang, sizeof v->transcript_lang, pg.transcript_lang);
    v->n_lines_total = pg.n_lines_total;
    v->line_from = pg.line_from;
    if (pg.n_lines && !(v->lines = calloc(pg.n_lines, sizeof *v->lines))) goto out;
    for (uint16_t i = 0; i < pg.n_lines; i++) {
        oc_call_line_view *l = &v->lines[i];
        l->id = lines[i].id; l->speaker = lines[i].speaker; l->start_ms = lines[i].start_ms; l->end_ms = lines[i].end_ms;
        if (!(l->text = malloc(lines[i].text.len + 1))) goto out;
        memcpy(l->text, lines[i].text.ptr, lines[i].text.len);
        l->text[lines[i].text.len] = '\0';
        v->n_lines = i + 1u;
    }
    oc_ev *e = oc_ev_new(OC_EV_CALL_PAGE);
    if (e) { e->call_page = v; e->call_id = v->call_id; v = NULL; oc_queue_push(to_ui, e); }
out:
    oc_call_page_view_free(v);
    free(stays); free(inv); free(evs); free(recs); free(lines);
    return rc;
}

int oc_callsig_frame(oc_callsig *cs, uint16_t type, oc_rbuf *p, const char *host,
                     oc_callsig_write write, void *wctx, oc_queue *to_ui) {
    switch (type) {
    case OC_MSG_CALL_JOINED: {
        oc_call_part parts[OC_MAX_CALL_PARTICIPANTS];
        oc_call_joined jd;
        if (oc_decode_call_joined(p, &jd, parts, OC_MAX_CALL_PARTICIPANTS) != OC_OK) return -1;
        /* In another call a moment ago: that one is over for this device. */
        if (cs->in_call) leave_local(cs, to_ui);
        if (cs->rj.on) rejoin_end(cs, to_ui, 0);   /* back in: JOINED says so */
        cs->in_call = 1;
        cs->call_id = jd.call_id;
        cs->epoch = jd.epoch;
        cs->slot = jd.slot;
        cs->n = jd.count;
        memcpy(cs->parts, parts, jd.count * sizeof parts[0]);
        int me = find_slot(cs, jd.slot);
        cs->self_user = me >= 0 ? cs->parts[me].user_id : 0;
        snprintf(cs->host, sizeof cs->host, "%s", host ? host : "");
        oc_mutex_lock(&cs->mu);
        if (cs->media && cs->media->tcp) cs->media->tcp(cs->mctx, cs->tcp_ok ? tcp_enqueue : NULL, cs);
        int started = cs->media && cs->media->start &&
                      cs->media->start(cs->mctx, cs->host, jd.udp_port, jd.token.ptr, jd.token.len,
                                       cs->self_user, cs->slot) == 0;
        oc_mutex_unlock(&cs->mu);
        push_view(to_ui, OC_EV_CALL_JOINED, cs, jd.starter, jd.started_at);
        if (!started) {
            /* Nothing here can carry the audio: leave rather than sit in the
             * roster, silent, as someone the others think can hear them. */
            oc_call_leave cl = { cs->call_id };
            uint8_t buf[32]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
            if (oc_encode_call_leave(&w, OC_PROTOCOL_VERSION, &cl) == OC_OK) write(wctx, buf, w.len);
            leave_local(cs, to_ui);
            return 1;
        }
        media_roster(cs);
        rekey(cs, write, wctx);
        return 1;
    }
    case OC_MSG_CALL_MEDIA: {
        oc_call_media_pkt m;
        if (oc_decode_call_media_down(p, &m) != OC_OK) return -1;
        oc_mutex_lock(&cs->mu);
        if (cs->in_call && cs->media && cs->media->rx_tcp)
            cs->media->rx_tcp(cs->mctx, m.sender, m.seq, m.stream, m.ct.ptr, m.ct.len);
        oc_mutex_unlock(&cs->mu);
        return 1;
    }
    case OC_MSG_CALL_ROSTER: {
        oc_call_part parts[OC_MAX_CALL_PARTICIPANTS];
        oc_call_roster ro;
        if (oc_decode_call_roster(p, &ro, parts, OC_MAX_CALL_PARTICIPANTS) != OC_OK) return -1;
        if (!cs->in_call || ro.call_id != cs->call_id) return 1;
        int still = 0, elsewhere = 0;
        for (uint16_t i = 0; i < ro.count; i++) {
            if (parts[i].slot == cs->slot && parts[i].user_id == cs->self_user &&
                !memcmp(parts[i].device_key, cs->pk, OC_CALL_DEVICE_KEY_LEN)) still = 1;
            else if (parts[i].user_id == cs->self_user) elsewhere = 1;
        }
        /* Not here any more. On another device of this user's, it moved there:
         * out. Otherwise the relay swept it -- its path went -- and it gets
         * back in. */
        if (!still && elsewhere) { leave_local(cs, to_ui); return 1; }
        if (!still) { rejoin_begin(cs, to_ui, 1); rejoin_send(cs, write, wctx, to_ui); return 1; }
        /* A roster in the same epoch says only that someone's camera went on
         * or off (REQ-160): nobody came or went, so the keys stand -- a second
         * key under one KID would be one nobody installs. */
        int new_epoch = ro.epoch != cs->epoch;
        cs->epoch = ro.epoch;
        cs->n = ro.count;
        memcpy(cs->parts, parts, ro.count * sizeof parts[0]);
        push_view(to_ui, OC_EV_CALL_ROSTER, cs, 0, 0);
        media_roster(cs);
        if (new_epoch) rekey(cs, write, wctx);
        return 1;
    }
    case OC_MSG_CALL_KEY_FOR: {
        oc_call_key_for kf;
        if (oc_decode_call_key_for(p, &kf) != OC_OK) return -1;
        /* This call, this epoch or the one before (a key can cross a roster on
         * the way), from someone in it, and the right size -- else ignored. */
        if (!cs->in_call || kf.call_id != cs->call_id || kf.sealed.len != OC_CALL_SEALED_LEN ||
            kf.epoch > cs->epoch || kf.epoch + 1 < cs->epoch) return 1;
        const oc_call_part *from = NULL;
        for (uint16_t i = 0; i < cs->n; i++) if (cs->parts[i].user_id == kf.sender) from = &cs->parts[i];
        if (!from || kf.sender == cs->self_user) return 1;
        uint8_t info[OC_CALLSIG_INFO_LEN], key[OC_CALL_KEY_LEN];
        oc_callsig_info(cs->call_id, kf.epoch, kf.sender, cs->self_user, info);
        if (oc_hpke_open_auth(kf.sealed.ptr, cs->sk, from->device_key, info, sizeof info, NULL, 0,
                              kf.sealed.ptr + OC_X25519_LEN, OC_CALL_SEALED_LEN - OC_X25519_LEN, key) == 0) {
            oc_mutex_lock(&cs->mu);
            if (cs->media && cs->media->rx_key) cs->media->rx_key(cs->mctx, from->user_id, from->slot, kf.epoch, key);
            oc_mutex_unlock(&cs->mu);
        }
        oc_e2e_wipe(key, sizeof key);
        return 1;
    }
    case OC_MSG_CALL_HISTORY_PAGE:
        return history_page(p, to_ui);
    case OC_MSG_CALL_PAGE:
        return call_page(p, to_ui);
    case OC_MSG_CALL_DELETED: {
        oc_call_delete d;
        if (oc_decode_call_delete(p, &d) != OC_OK) return -1;
        oc_ev *e = oc_ev_new(OC_EV_CALL_DELETED);
        if (e) { e->call_id = d.call_id; e->op = d.what; e->message_id = d.id; oc_queue_push(to_ui, e); }
        return 1;
    }
    case OC_MSG_CALL_STATE: {
        uint64_t parts[OC_MAX_CALL_PARTICIPANTS], inv[OC_MAX_CALL_INVITES];
        oc_call_state st;
        if (oc_decode_call_state(p, &st, parts, OC_MAX_CALL_PARTICIPANTS, inv, OC_MAX_CALL_INVITES) != OC_OK)
            return -1;
        oc_ev *e = oc_ev_new(OC_EV_CALL_STATE);
        if (e && (e->call = calloc(1, sizeof *e->call))) {
            oc_call_view *v = e->call;
            e->call_id = st.call_id;
            v->from_channel = st.from_channel;
            v->chat_channel = st.chat_channel;
            v->recording = (st.flags & OC_CALL_STATE_RECORDING) != 0;
            v->rec_by = v->recording ? st.rec_by : 0;
            v->call_id = st.call_id;
            size_t sl = st.subject.len < OC_CALL_SUBJECT_MAX ? st.subject.len : OC_CALL_SUBJECT_MAX;
            if (sl) memcpy(v->subject, st.subject.ptr, sl);
            v->subject[sl] = '\0';
            v->starter = st.starter;
            v->started_at = st.started_at;
            v->ended = st.ended;
            v->n_parts = st.n_parts;
            v->n_invited = st.n_invited;
            v->sharer = st.ended ? 0 : st.sharer;
            memcpy(v->parts, parts, st.n_parts * sizeof parts[0]);
            memcpy(v->invited, inv, st.n_invited * sizeof inv[0]);
            oc_queue_push(to_ui, e);
        } else {
            oc_ev_free(e);
        }
        if (cs->in_call && st.call_id == cs->call_id && !st.ended) {
            oc_mutex_lock(&cs->mu);
            if (cs->media && cs->media->sharer) cs->media->sharer(cs->mctx, st.sharer);
            oc_mutex_unlock(&cs->mu);
        }
        /* Ended for everyone -- by its starter, or its last participant left. */
        if (st.ended && cs->in_call && st.call_id == cs->call_id) leave_local(cs, to_ui);
        /* The call being got back into: still there, so join it; over, so it
         * cannot be. */
        if (cs->rj.on && st.call_id == cs->rj.call_id) {
            if (st.ended) rejoin_end(cs, to_ui, OC_EV_CALL_LOST);
            else if (cs->rj.authed_ms && !cs->rj.sent) rejoin_send(cs, write, wctx, to_ui);
        }
        return 1;
    }
    default:
        return 0;
    }
}


