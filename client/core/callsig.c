/* Call signaling and media keys — see callsig.h and docs/CALLS.md §5. */

#include "callsig.h"

#include "e2e_hpke.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void oc_callsig_init(oc_callsig *cs) {
    memset(cs, 0, sizeof *cs);
    oc_mutex_init(&cs->mu);
}

void oc_callsig_destroy(oc_callsig *cs) {
    oc_e2e_wipe(cs->sk, sizeof cs->sk);
    oc_mutex_destroy(&cs->mu);
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
    e->channel_id = cs->channel_id;
    if (!(e->call = calloc(1, sizeof *e->call))) { oc_ev_free(e); return; }
    oc_call_view *v = e->call;
    v->channel_id = cs->channel_id;
    v->call_id = cs->call_id;
    v->starter = starter;
    v->started_at = started;
    v->epoch = cs->epoch;
    v->my_slot = cs->slot;
    v->n_parts = cs->n;
    for (uint16_t i = 0; i < cs->n; i++) { v->parts[i] = cs->parts[i].user_id; v->slots[i] = cs->parts[i].slot; }
    oc_queue_push(to_ui, e);
}

static void push_left(oc_queue *to_ui, uint64_t channel_id) {
    oc_ev *e = oc_ev_new(OC_EV_CALL_LEFT);
    if (!e) return;
    e->channel_id = channel_id;
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
        oc_call_key ck = { cs->channel_id, cs->call_id, cs->epoch, ne, ents };
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
    uint64_t ch = cs->channel_id;
    media_stop(cs);
    cs->in_call = 0;
    cs->n = 0;
    cs->channel_id = cs->call_id = 0;
    push_left(to_ui, ch);
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
    case OC_CMD_CALL_JOIN: {
        if (!cs->have_key) {
            if (oc_store_device_key(store, workspace, cs->sk, cs->pk) < 0) return -1;
            cs->have_key = 1;
        }
        oc_call_join cj = { c->channel_id, {0}, c->n_uids, c->uids };
        memcpy(cj.device_key, cs->pk, OC_CALL_DEVICE_KEY_LEN);
        rc = oc_encode_call_join(&w, OC_PROTOCOL_VERSION, &cj);
        break;
    }
    case OC_CMD_CALL_INVITE: {
        oc_call_invite ci = { c->channel_id, c->n_uids, c->uids };
        rc = oc_encode_call_invite(&w, OC_PROTOCOL_VERSION, &ci);
        break;
    }
    case OC_CMD_CALL_LEAVE: {
        oc_call_leave cl = { c->channel_id };
        rc = oc_encode_call_leave(&w, OC_PROTOCOL_VERSION, &cl);
        break;
    }
    case OC_CMD_CALL_DECLINE: {
        oc_call_decline cd = { c->channel_id };
        rc = oc_encode_call_decline(&w, OC_PROTOCOL_VERSION, &cd);
        break;
    }
    case OC_CMD_CALL_END: {
        oc_call_end ce = { c->channel_id };
        rc = oc_encode_call_end(&w, OC_PROTOCOL_VERSION, &ce);
        break;
    }
    default:
        return -1;
    }
    if (rc != OC_OK) return -1;
    int wr = write(wctx, buf, w.len);
    /* Leaving is this device's own act: it is out now, whatever the daemon says
     * next -- which to a leaver is nothing about the call's participants. */
    if (c->type == OC_CMD_CALL_LEAVE && cs->in_call && cs->channel_id == c->channel_id) leave_local(cs, to_ui);
    return wr;
}

/* --- frames ------------------------------------------------------------------ */

int oc_callsig_frame(oc_callsig *cs, uint16_t type, oc_rbuf *p, const char *host,
                     oc_callsig_write write, void *wctx, oc_queue *to_ui) {
    switch (type) {
    case OC_MSG_CALL_JOINED: {
        oc_call_part parts[OC_MAX_CALL_PARTICIPANTS];
        oc_call_joined jd;
        if (oc_decode_call_joined(p, &jd, parts, OC_MAX_CALL_PARTICIPANTS) != OC_OK) return -1;
        /* In another call a moment ago: that one is over for this device. */
        if (cs->in_call) leave_local(cs, to_ui);
        cs->in_call = 1;
        cs->channel_id = jd.channel_id;
        cs->call_id = jd.call_id;
        cs->epoch = jd.epoch;
        cs->slot = jd.slot;
        cs->n = jd.count;
        memcpy(cs->parts, parts, jd.count * sizeof parts[0]);
        int me = find_slot(cs, jd.slot);
        cs->self_user = me >= 0 ? cs->parts[me].user_id : 0;
        snprintf(cs->host, sizeof cs->host, "%s", host ? host : "");
        oc_mutex_lock(&cs->mu);
        int started = cs->media && cs->media->start &&
                      cs->media->start(cs->mctx, cs->host, jd.udp_port, jd.token.ptr, jd.token.len,
                                       cs->self_user, cs->slot) == 0;
        oc_mutex_unlock(&cs->mu);
        push_view(to_ui, OC_EV_CALL_JOINED, cs, jd.starter, jd.started_at);
        if (!started) {
            /* Nothing here can carry the audio: leave rather than sit in the
             * roster, silent, as someone the others think can hear them. */
            oc_call_leave cl = { cs->channel_id };
            uint8_t buf[32]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
            if (oc_encode_call_leave(&w, OC_PROTOCOL_VERSION, &cl) == OC_OK) write(wctx, buf, w.len);
            leave_local(cs, to_ui);
            return 1;
        }
        media_roster(cs);
        rekey(cs, write, wctx);
        return 1;
    }
    case OC_MSG_CALL_ROSTER: {
        oc_call_part parts[OC_MAX_CALL_PARTICIPANTS];
        oc_call_roster ro;
        if (oc_decode_call_roster(p, &ro, parts, OC_MAX_CALL_PARTICIPANTS) != OC_OK) return -1;
        if (!cs->in_call || ro.call_id != cs->call_id) return 1;
        int still = 0;
        for (uint16_t i = 0; i < ro.count; i++)
            if (parts[i].slot == cs->slot && parts[i].user_id == cs->self_user &&
                !memcmp(parts[i].device_key, cs->pk, OC_CALL_DEVICE_KEY_LEN)) still = 1;
        if (!still) { leave_local(cs, to_ui); return 1; }   /* moved to another device, or swept */
        cs->epoch = ro.epoch;
        cs->n = ro.count;
        memcpy(cs->parts, parts, ro.count * sizeof parts[0]);
        push_view(to_ui, OC_EV_CALL_ROSTER, cs, 0, 0);
        media_roster(cs);
        rekey(cs, write, wctx);
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
    case OC_MSG_CALL_STATE: {
        uint64_t parts[OC_MAX_CALL_PARTICIPANTS], inv[OC_MAX_CALL_INVITES];
        oc_call_state st;
        if (oc_decode_call_state(p, &st, parts, OC_MAX_CALL_PARTICIPANTS, inv, OC_MAX_CALL_INVITES) != OC_OK)
            return -1;
        oc_ev *e = oc_ev_new(OC_EV_CALL_STATE);
        if (e && (e->call = calloc(1, sizeof *e->call))) {
            oc_call_view *v = e->call;
            e->channel_id = st.channel_id;
            v->channel_id = st.channel_id;
            v->call_id = st.call_id;
            v->starter = st.starter;
            v->started_at = st.started_at;
            v->ended = st.ended;
            v->n_parts = st.n_parts;
            v->n_invited = st.n_invited;
            memcpy(v->parts, parts, st.n_parts * sizeof parts[0]);
            memcpy(v->invited, inv, st.n_invited * sizeof inv[0]);
            oc_queue_push(to_ui, e);
        } else {
            oc_ev_free(e);
        }
        /* Ended for everyone -- by its starter, or its last participant left. */
        if (st.ended && cs->in_call && st.call_id == cs->call_id) leave_local(cs, to_ui);
        return 1;
    }
    default:
        return 0;
    }
}

void oc_callsig_lost(oc_callsig *cs, oc_queue *to_ui) {
    leave_local(cs, to_ui);
}
