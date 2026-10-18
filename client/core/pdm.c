/* Encrypted DMs on the client: see pdm.h and PRIVATE-DM.md. */

#include "pdm.h"

#include "e2e_hpke.h"
#include "model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mbedtls/base64.h>
#include <mbedtls/sha256.h>

enum {
    S_OPENING = 1,   /* this device opened it: handshake 1 sent, waiting for 2 */
    S_INVITED,       /* the other person opened it: waiting for this person to decide */
    S_ANSWERED,      /* accepted here: handshake 2 sent, waiting for 3 */
    S_HELD,          /* done, but the other device's key changed: nothing is sent */
    S_OPEN,
    S_STARTING,      /* accepted here, handshake done: waiting for the opener's first message,
                        before which this side's ratchet has nothing to send with */
};

/* A sealed body: this, then the ratchet's header and the ciphertext in base64,
 * so it is text wherever a body is taken for text. */
static const char SEAL_PREFIX[] = "oc-e2e:1:";
#define SEAL_PREFIX_LEN (sizeof SEAL_PREFIX - 1)

static const char PROLOGUE[] = "OpenChime private DM v1";

/* Both sides give the same prologue: the name, then the opener and the peer. */
static size_t prologue(uint64_t opener, uint64_t peer, uint8_t out[64]) {
    size_t n = sizeof PROLOGUE - 1;
    memcpy(out, PROLOGUE, n);
    for (int i = 0; i < 8; i++) out[n + i] = (uint8_t)(opener >> (56 - 8 * i));
    for (int i = 0; i < 8; i++) out[n + 8 + i] = (uint8_t)(peer >> (56 - 8 * i));
    return n + 16;
}

static void sess_free(oc_pdm_sess *s) {
    if (s->r) { oc_ratchet_wipe(s->r); free(s->r); }
    for (size_t i = 0; i < s->n_plain; i++) {
        oc_e2e_wipe(s->plain[i].text, strlen(s->plain[i].text));
        free(s->plain[i].text);
    }
    free(s->plain);
    oc_e2e_wipe(s, sizeof *s);
}

static void digest_of(const uint8_t *body, size_t len, uint8_t out[16]) {
    uint8_t dg[32];
    mbedtls_sha256(body, len, dg, 0);
    memcpy(out, dg, 16);
}

static const char *plain_find(const oc_pdm_sess *s, const uint8_t dg[16]) {
    for (size_t i = s->n_plain; i-- > 0; )
        if (memcmp(s->plain[i].digest, dg, 16) == 0) return s->plain[i].text;
    return NULL;
}

static void plain_keep(oc_pdm_sess *s, const uint8_t dg[16], const char *text, size_t len) {
    if (s->n_plain == s->cap_plain) {
        size_t nc = s->cap_plain ? s->cap_plain * 2 : 64;
        oc_pdm_plain *g = realloc(s->plain, nc * sizeof *g);
        if (!g) return;
        s->plain = g; s->cap_plain = nc;
    }
    char *t = malloc(len + 1);
    if (!t) return;
    memcpy(t, text, len); t[len] = '\0';
    memcpy(s->plain[s->n_plain].digest, dg, 16);
    s->plain[s->n_plain++].text = t;
}

static oc_pdm_sess *by_id(oc_pdm *d, uint64_t id) {
    if (!id) return NULL;
    for (int i = 0; i < OC_PDM_CLIENT_MAX; i++) if (d->s[i].used && d->s[i].id == id) return &d->s[i];
    return NULL;
}

static oc_pdm_sess *new_sess(oc_pdm *d) {
    for (int i = 0; i < OC_PDM_CLIENT_MAX; i++)
        if (!d->s[i].used) { memset(&d->s[i], 0, sizeof d->s[i]); d->s[i].used = 1; return &d->s[i]; }
    return NULL;
}

static int ensure_key(oc_pdm *d) {
    if (d->have_key) return 0;
    if (oc_store_device_key(d->store, d->workspace[0] ? d->workspace : NULL, d->sk, d->pk) < 0) return -1;
    d->have_key = 1;
    return 0;
}

static void emit(oc_queue *to_ui, int kind, uint64_t id, uint64_t peer, const char *body, size_t body_len,
                 uint8_t reason, const char *safety) {
    oc_ev *e = oc_ev_new(OC_EV_PDM);
    if (!e) return;
    e->pdm_kind = (uint8_t)kind;
    e->channel_id = id;
    e->user_id = peer;
    e->pdm_reason = reason;
    if (safety) snprintf(e->pdm_safety, sizeof e->pdm_safety, "%s", safety);
    if (body) {
        e->body = malloc(body_len + 1);
        if (e->body) { memcpy(e->body, body, body_len); e->body[body_len] = '\0'; }
    }
    oc_queue_push(to_ui, e);
}

static void send_end(uint64_t id, uint8_t reason, oc_pdm_write write, void *wctx) {
    uint8_t buf[64]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
    oc_pdm_end e = { id, reason };
    if (write && oc_encode_pdm_end(&w, OC_PROTOCOL_VERSION, &e) == OC_OK) write(wctx, buf, w.len);
}

/* End a session here: the daemon and the other side are told `wire` (0: they
 * already know), the UI `reason`. */
static void finish(oc_pdm_sess *s, uint8_t wire, uint8_t reason, oc_pdm_write write, void *wctx, oc_queue *to_ui) {
    if (wire && s->id) send_end(s->id, wire, write, wctx);
    emit(to_ui, OC_PDM_EV_ENDED, s->id, s->peer, NULL, 0, reason, NULL);
    sess_free(s);
}

void oc_pdm_safety_number(uint64_t user_a, const uint8_t pk_a[32], uint64_t user_b, const uint8_t pk_b[32],
                          char out[OC_PDM_SAFETY_LEN]) {
    /* In a fixed order, so both devices compute the same: the lower user id first. */
    uint8_t in[1 + 2 * (8 + 32)], dg[64];
    const uint8_t *k1 = pk_a, *k2 = pk_b;
    uint64_t u1 = user_a, u2 = user_b;
    if (user_b < user_a) { u1 = user_b; u2 = user_a; k1 = pk_b; k2 = pk_a; }
    for (int i = 0; i < 8; i++) { in[1 + i] = (uint8_t)(u1 >> (56 - 8 * i)); in[41 + i] = (uint8_t)(u2 >> (56 - 8 * i)); }
    memcpy(in + 9, k1, 32);
    memcpy(in + 49, k2, 32);
    in[0] = 0; mbedtls_sha256(in, sizeof in, dg, 0);
    in[0] = 1; mbedtls_sha256(in, sizeof in, dg + 32, 0);
    size_t n = 0;
    for (int g = 0; g < 12; g++) {
        uint64_t v = 0;
        for (int i = 0; i < 5; i++) v = v << 8 | dg[g * 5 + i];
        n += (size_t)snprintf(out + n, OC_PDM_SAFETY_LEN - n, g ? " %05u" : "%05u", (unsigned)(v % 100000));
    }
}

void oc_pdm_authed(oc_pdm *d, uint64_t self, oc_store *store, const char *workspace) {
    d->self = self;
    d->store = store;
    snprintf(d->workspace, sizeof d->workspace, "%s", workspace ? workspace : "");
}

/* The handshake is done: start the ratchet, and hold the other device to the key
 * remembered for that person. */
static void announce(oc_pdm *d, oc_pdm_sess *s, oc_queue *to_ui);
static int open_session(oc_pdm *d, oc_pdm_sess *s, oc_pdm_write write, void *wctx, oc_queue *to_ui) {
    oc_noise_result res;
    if (oc_noise_xx_split(&s->hs, &res) != 0) return -1;
    s->r = calloc(1, sizeof *s->r);
    int rc = !s->r ? -1
           : s->initiator ? oc_ratchet_init_alice(s->r, res.rk, res.bob_pk, res.hh)
                          : oc_ratchet_init_bob(s->r, res.rk, res.bob_sk, res.bob_pk, res.hh);
    memcpy(s->peer_pk, res.rs, sizeof s->peer_pk);
    oc_e2e_wipe(&res, sizeof res);
    if (rc != 0) return -1;
    s->opened_ms = oc_model_now_ms();

    /* The opener speaks first, with nothing: the ratchet's first message, which
     * the other side needs before it can answer. */
    if (s->initiator) {
        uint8_t hdr[OC_RATCHET_HEADERLEN], ct[OC_RATCHET_TAGLEN];
        if (oc_ratchet_encrypt(s->r, NULL, 0, hdr, ct) != 0) return -1;
        uint8_t buf[256]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
        oc_pdm_msg m = { s->id, { hdr, sizeof hdr }, { ct, sizeof ct } };
        if (oc_encode_pdm_msg(&w, OC_PROTOCOL_VERSION, OC_MSG_PDM_SEND, &m) != OC_OK || write(wctx, buf, w.len) != 0) return -1;
    }

    if (!s->initiator) { s->state = S_STARTING; return 0; }
    announce(d, s, to_ui);
    return 0;
}


void oc_pdm_channel(oc_pdm *d, uint64_t channel_id, int e2e) {
    if (!channel_id || !e2e || oc_pdm_is_e2e(d, channel_id)) return;
    if (d->n_e2e == OC_PDM_E2E_MAX) {   /* the oldest goes: it is closed long since */
        memmove(d->e2e, d->e2e + 1, (OC_PDM_E2E_MAX - 1) * sizeof d->e2e[0]);
        d->n_e2e--;
    }
    d->e2e[d->n_e2e++] = channel_id;
}

int oc_pdm_is_e2e(const oc_pdm *d, uint64_t channel_id) {
    for (size_t i = 0; channel_id && i < d->n_e2e; i++) if (d->e2e[i] == channel_id) return 1;
    return 0;
}

int oc_pdm_holds(const oc_pdm *d, uint64_t channel_id) {
    for (int i = 0; channel_id && i < OC_PDM_CLIENT_MAX; i++)
        if (d->s[i].used && d->s[i].id == channel_id && d->s[i].state != S_INVITED) return 1;
    return 0;
}

char *oc_pdm_seal(oc_pdm *d, uint64_t channel_id, const char *text) {
    oc_pdm_sess *s = by_id(d, channel_id);
    size_t len = text ? strlen(text) : 0;
    if (!s || s->state != S_OPEN || !s->r || len == 0 || len > OC_PDM_MAX_TEXT) return NULL;
    size_t raw_len = OC_RATCHET_HEADERLEN + len + OC_RATCHET_TAGLEN;
    uint8_t *raw = malloc(raw_len);
    size_t cap = SEAL_PREFIX_LEN + 4 * ((raw_len + 2) / 3) + 1, olen = 0;
    char *out = malloc(cap);
    int ok = raw && out &&
             oc_ratchet_encrypt(s->r, (const uint8_t *)text, len, raw, raw + OC_RATCHET_HEADERLEN) == 0 &&
             mbedtls_base64_encode((unsigned char *)out + SEAL_PREFIX_LEN, cap - SEAL_PREFIX_LEN, &olen, raw, raw_len) == 0;
    if (raw) { oc_e2e_wipe(raw, raw_len); free(raw); }
    if (!ok) { free(out); return NULL; }
    memcpy(out, SEAL_PREFIX, SEAL_PREFIX_LEN);
    out[SEAL_PREFIX_LEN + olen] = '\0';
    uint8_t dg[16];
    digest_of((const uint8_t *)out, SEAL_PREFIX_LEN + olen, dg);
    plain_keep(s, dg, text, len);   /* for when it comes back: it cannot be opened here */
    s->msgs++;
    return out;
}

char *oc_pdm_unseal(oc_pdm *d, uint64_t channel_id, const uint8_t *body, size_t len) {
    if (!len) return strdup("");
    oc_pdm_sess *s = by_id(d, channel_id);
    uint8_t dg[16];
    digest_of(body, len, dg);
    const char *had = s ? plain_find(s, dg) : NULL;
    if (had) return strdup(had);
    if (!s || !s->r || (s->state != S_OPEN && s->state != S_HELD) ||
        len <= SEAL_PREFIX_LEN || memcmp(body, SEAL_PREFIX, SEAL_PREFIX_LEN) != 0)
        return strdup(OC_PDM_UNREADABLE);
    size_t cap = (len - SEAL_PREFIX_LEN) / 4 * 3 + 3, rl = 0;
    uint8_t *raw = malloc(cap);
    char *pt = NULL;
    if (raw && mbedtls_base64_decode(raw, cap, &rl, body + SEAL_PREFIX_LEN, len - SEAL_PREFIX_LEN) == 0 &&
        rl >= OC_RATCHET_HEADERLEN + OC_RATCHET_TAGLEN && rl - OC_RATCHET_HEADERLEN - OC_RATCHET_TAGLEN <= OC_PDM_MAX_TEXT) {
        size_t n = rl - OC_RATCHET_HEADERLEN - OC_RATCHET_TAGLEN;
        pt = malloc(n + 1);
        if (pt && oc_ratchet_decrypt(s->r, raw, raw + OC_RATCHET_HEADERLEN, rl - OC_RATCHET_HEADERLEN, (uint8_t *)pt) == 0) {
            pt[n] = '\0';
            if (strlen(pt) != n) { oc_e2e_wipe(pt, n); free(pt); pt = NULL; }   /* text has no NUL */
            else { plain_keep(s, dg, pt, n); s->msgs++; }
        } else if (pt) { oc_e2e_wipe(pt, n); free(pt); pt = NULL; }
    }
    if (raw) { oc_e2e_wipe(raw, cap); free(raw); }
    return pt ? pt : strdup(OC_PDM_UNREADABLE);
}

/* The session can be used: said, with its safety number, or held when the other
 * device's key is not the one remembered for this person. */
static void announce(oc_pdm *d, oc_pdm_sess *s, oc_queue *to_ui) {
    uint8_t dg[32], fp[OC_STORE_PEER_FP];
    mbedtls_sha256(s->peer_pk, sizeof s->peer_pk, dg, 0);
    memcpy(fp, dg, sizeof fp);
    char safety[OC_PDM_SAFETY_LEN];
    oc_pdm_safety_number(d->self, d->pk, s->peer, s->peer_pk, safety);
    int known = oc_store_peer_check(d->store, d->workspace[0] ? d->workspace : NULL, s->peer, fp);
    s->state = known == 2 ? S_HELD : S_OPEN;
    emit(to_ui, s->state == S_HELD ? OC_PDM_EV_KEY_CHANGED : OC_PDM_EV_OPEN, s->id, s->peer, NULL, 0, 0, safety);
}

int oc_pdm_frame(oc_pdm *d, uint16_t type, oc_rbuf *p, oc_pdm_write write, void *wctx, oc_queue *to_ui) {
    if (type < OC_MSG_PDM_OPEN || type > OC_MSG_PDM_END) return 0;
    uint8_t buf[OC_PDM_MAX_HS + 64], pl[64];
    size_t n, pn;
    oc_wbuf w;

    if (type == OC_MSG_PDM_OPENED) {
        oc_pdm_opened op;
        if (oc_decode_pdm_opened(p, &op) != OC_OK) return -1;
        for (int i = 0; i < OC_PDM_CLIENT_MAX; i++) {
            oc_pdm_sess *s = &d->s[i];
            if (s->used && s->state == S_OPENING && s->id == 0 && s->peer == op.peer) {
                s->id = op.pdm_id;
                oc_pdm_channel(d, s->id, 1);
                emit(to_ui, OC_PDM_EV_OPENING, s->id, s->peer, NULL, 0, 0, NULL);
                break;
            }
        }
        return 1;
    }
    if (type == OC_MSG_PDM_INVITE) {
        oc_pdm_invite inv;
        if (oc_decode_pdm_invite(p, &inv) != OC_OK) return -1;
        if (by_id(d, inv.pdm_id)) return 1;            /* shown again after a reconnect */
        oc_pdm_sess *s = new_sess(d);
        if (!s) { send_end(inv.pdm_id, OC_PDM_END_DECLINED, write, wctx); return 1; }
        s->id = inv.pdm_id; s->peer = inv.from; s->initiator = 0; s->state = S_INVITED;
        oc_pdm_channel(d, s->id, 1);
        memcpy(s->hs1, inv.hs.ptr, inv.hs.len); s->hs1_len = inv.hs.len;
        emit(to_ui, OC_PDM_EV_INVITED, s->id, s->peer, NULL, 0, 0, NULL);
        return 1;
    }
    if (type == OC_MSG_PDM_HS_FOR) {
        oc_pdm_hs h;
        if (oc_decode_pdm_hs(p, &h) != OC_OK) return -1;
        oc_pdm_sess *s = by_id(d, h.pdm_id);
        if (!s) { send_end(h.pdm_id, OC_PDM_END_LOST, write, wctx); return 1; }
        int ok = -1;
        if (s->initiator && s->state == S_OPENING) {
            ok = oc_noise_xx_read(&s->hs, h.hs.ptr, h.hs.len, pl, sizeof pl, &pn) == 0
              && oc_noise_xx_write(&s->hs, NULL, 0, buf, sizeof buf, &n) == 0 ? 0 : -1;
            if (ok == 0) {
                oc_wbuf_init(&w, buf + n, sizeof buf - n);
                oc_pdm_hs out = { s->id, { buf, n } };
                ok = oc_encode_pdm_hs(&w, OC_PROTOCOL_VERSION, OC_MSG_PDM_HS, &out) == OC_OK
                  && write(wctx, buf + n, w.len) == 0 ? 0 : -1;
            }
        } else if (!s->initiator && s->state == S_ANSWERED) {
            ok = oc_noise_xx_read(&s->hs, h.hs.ptr, h.hs.len, pl, sizeof pl, &pn);
        }
        if (ok == 0) ok = open_session(d, s, write, wctx, to_ui);
        if (ok != 0) finish(s, OC_PDM_END_LOST, OC_PDM_END_LOST, write, wctx, to_ui);
        return 1;
    }
    if (type == OC_MSG_PDM_MSG) {
        /* The opener's first message, empty, over the handshake's own relay:
         * it starts this side's ratchet, after which it can send too. */
        oc_pdm_msg m;
        if (oc_decode_pdm_msg(p, &m) != OC_OK) return -1;
        oc_pdm_sess *s = by_id(d, m.pdm_id);
        if (!s || !s->r) { send_end(m.pdm_id, OC_PDM_END_LOST, write, wctx); if (s) finish(s, 0, OC_PDM_END_LOST, write, wctx, to_ui); return 1; }
        if (s->state != S_STARTING || m.ct.len != OC_RATCHET_TAGLEN) return 1;
        uint8_t pt[1];
        if (oc_ratchet_decrypt(s->r, m.header.ptr, m.ct.ptr, m.ct.len, pt) != 0) {
            finish(s, OC_PDM_END_LOST, OC_PDM_END_LOST, write, wctx, to_ui);
            return 1;
        }
        announce(d, s, to_ui);
        return 1;
    }
    if (type == OC_MSG_PDM_END) {
        oc_pdm_end e;
        if (oc_decode_pdm_end(p, &e) != OC_OK) return -1;
        oc_pdm_sess *s = by_id(d, e.pdm_id);
        if (!s && e.pdm_id == 0)   /* an open refused before it was named: the one waiting */
            for (int i = 0; i < OC_PDM_CLIENT_MAX && !s; i++)
                if (d->s[i].used && d->s[i].state == S_OPENING && d->s[i].id == 0) s = &d->s[i];
        if (s) finish(s, 0, e.reason, write, wctx, to_ui);
        return 1;
    }
    return 0;   /* the client-to-server types arriving here are not for it */
}

void oc_pdm_command(oc_pdm *d, const oc_cmd *c, oc_pdm_write write, void *wctx, oc_queue *to_ui) {
    uint8_t buf[OC_MAX_FRAME_SIZE < 1024 ? OC_MAX_FRAME_SIZE : 1024], hs[OC_PDM_MAX_HS], pl[64];
    size_t n, pn;
    oc_wbuf w;

    if (c->type == OC_CMD_PDM_OPEN) {
        uint64_t peer = c->channel_id;
        for (int i = 0; i < OC_PDM_CLIENT_MAX; i++)   /* one waiting to be named at a time */
            if (d->s[i].used && d->s[i].state == S_OPENING && d->s[i].id == 0) return;
        oc_pdm_sess *s = (d->enabled && peer && peer != d->self && ensure_key(d) == 0) ? new_sess(d) : NULL;
        if (!s) { emit(to_ui, OC_PDM_EV_ENDED, 0, peer, NULL, 0, OC_PDM_END_REFUSED, NULL); return; }
        s->peer = peer; s->initiator = 1; s->state = S_OPENING;
        uint8_t pro[64];
        size_t pro_len = prologue(d->self, peer, pro);
        oc_wbuf_init(&w, buf, sizeof buf);
        int ok = oc_noise_xx_init(&s->hs, 1, pro, pro_len, d->sk) == 0
              && oc_noise_xx_write(&s->hs, NULL, 0, hs, sizeof hs, &n) == 0;
        if (ok) {
            oc_pdm_open op = { peer, { hs, n } };
            ok = oc_encode_pdm_open(&w, OC_PROTOCOL_VERSION, &op) == OC_OK && write(wctx, buf, w.len) == 0;
        }
        if (!ok) finish(s, 0, OC_PDM_END_REFUSED, write, wctx, to_ui);
        return;
    }

    oc_pdm_sess *s = by_id(d, c->channel_id);
    if (!s) return;
    if (c->type == OC_CMD_PDM_ACCEPT && s->state == S_INVITED) {
        uint8_t pro[64];
        size_t pro_len = prologue(s->peer, d->self, pro);
        int ok = ensure_key(d) == 0
              && oc_noise_xx_init(&s->hs, 0, pro, pro_len, d->sk) == 0
              && oc_noise_xx_read(&s->hs, s->hs1, s->hs1_len, pl, sizeof pl, &pn) == 0
              && oc_noise_xx_write(&s->hs, NULL, 0, hs, sizeof hs, &n) == 0;
        if (ok) {
            oc_wbuf_init(&w, buf, sizeof buf);
            oc_pdm_hs out = { s->id, { hs, n } };
            ok = oc_encode_pdm_hs(&w, OC_PROTOCOL_VERSION, OC_MSG_PDM_HS, &out) == OC_OK && write(wctx, buf, w.len) == 0;
        }
        if (!ok) { finish(s, OC_PDM_END_DECLINED, OC_PDM_END_LOST, write, wctx, to_ui); return; }
        s->state = S_ANSWERED;
        emit(to_ui, OC_PDM_EV_OPENING, s->id, s->peer, NULL, 0, 0, NULL);
        return;
    }
    if (c->type == OC_CMD_PDM_DECLINE && s->state == S_INVITED) {
        finish(s, OC_PDM_END_DECLINED, OC_PDM_END_DECLINED, write, wctx, to_ui);
        return;
    }
    if (c->type == OC_CMD_PDM_TRUST && s->state == S_HELD) {
        uint8_t dg[32];
        mbedtls_sha256(s->peer_pk, sizeof s->peer_pk, dg, 0);
        oc_store_peer_accept(d->store, d->workspace[0] ? d->workspace : NULL, s->peer, dg);
        s->state = S_OPEN;
        char safety[OC_PDM_SAFETY_LEN];
        oc_pdm_safety_number(d->self, d->pk, s->peer, s->peer_pk, safety);
        emit(to_ui, OC_PDM_EV_OPEN, s->id, s->peer, NULL, 0, 0, safety);
        return;
    }
    if (c->type == OC_CMD_PDM_CLOSE) {
        finish(s, s->state == S_INVITED ? OC_PDM_END_DECLINED : OC_PDM_END_CLOSED, OC_PDM_END_CLOSED, write, wctx, to_ui);
        return;
    }
}

void oc_pdm_tick(oc_pdm *d, uint64_t now_ms, oc_pdm_write write, void *wctx, oc_queue *to_ui) {
    for (int i = 0; i < OC_PDM_CLIENT_MAX; i++) {
        oc_pdm_sess *s = &d->s[i];
        if (!s->used || !s->r) continue;
        if (s->msgs >= OC_PDM_LIMIT_MSGS || (s->opened_ms && now_ms - s->opened_ms >= OC_PDM_LIMIT_MS))
            finish(s, OC_PDM_END_CLOSED, OC_PDM_END_CLOSED, write, wctx, to_ui);
    }
}

void oc_pdm_quit(oc_pdm *d, oc_pdm_write write, void *wctx) {
    for (int i = 0; i < OC_PDM_CLIENT_MAX; i++)
        if (d->s[i].used && d->s[i].id)
            send_end(d->s[i].id, d->s[i].state == S_INVITED ? OC_PDM_END_DECLINED : OC_PDM_END_CLOSED, write, wctx);
}

void oc_pdm_free(oc_pdm *d) {
    for (int i = 0; i < OC_PDM_CLIENT_MAX; i++) if (d->s[i].used) sess_free(&d->s[i]);
    oc_e2e_wipe(d, sizeof *d);
}
