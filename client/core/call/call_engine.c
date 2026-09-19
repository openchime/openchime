/* The call's media engine — see oc_call_engine.h and docs/CALLS.md §3-§5. */
#include "sock.h"       /* first: winsock2 before anything pulls in windows.h */

#include "oc_call_engine.h"

#include "audio_dev.h"
#include "oc_capture.h"
#include "e2e_sframe.h"
#include "oc_codec.h"
#include "oc_jitter.h"
#include "oc_media.h"
#include "oc_processor.h"
#include "oc_share.h"
#include "oc_thread.h"

#include <speex/speex_preprocess.h>

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FRAME        OC_VOICE_FRAME
#define KBPS         24
#define KEEPALIVE_MS 5000
#define GRACE_MS     1000      /* a new key is used this long after it is made */
#define RETIRE_MS    5000      /* an old key is kept this long after its successor is used */
#define MAX_PEERS    32
#define MAX_RXKEYS   (MAX_PEERS * 3)
#define PACKET_MAX   1400
#define SPEAK_LEVEL  900       /* peak above which a frame is someone talking */
#define SPEAK_HOLD   300
#define S2C_HDR      10        /* sender(u64) seq(u16): the relay's framing (AUDIO.md §1.1) */
/* A sender's STATE packet (OC_CALLPKT_STATE) -- one byte, bit 0 muted -- goes
 * every second while muted, inside the encryption, so the others can show it
 * and the daemon cannot see it. */
#define MUTED_HOLD   2500

static void nap(int ms) {
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
#endif
}

static int64_t now_ms(void) { return oc_media_clock_us() / 1000; }

typedef struct {
    int         used;
    uint64_t    user;
    uint8_t     slot;
    oc_jitter   jb;
    oc_opusdec *dec;
    int         level, quiet;
    int64_t     speaking_until, muted_until;
    uint64_t    last_ctr, last_kid;
    int         have_ctr;
    uint32_t    packets, lost, undecryptable;
    uint32_t    rep_packets, rep_lost;   /* at the last share REPORT */
    uint64_t    video_bytes;
} peer;

typedef struct {
    int              used;
    uint64_t         kid, user;
    uint32_t         epoch;
    oc_sframe_key    key;
    oc_sframe_replay rw;
    int64_t          expire_ms;   /* 0 = keep */
} rxkey;

typedef struct { uint64_t user; float gain; } volume;

struct oc_call_engine {
    oc_call_engine_opts opts;
    char        mic_id[520], spk_id[520];
    oc_mutex_t  mu;

    /* The call. */
    int         active;
    atomic_int  stop;
    oc_thread_t th_io, th_cap, th_play;
    int         sock;
    struct sockaddr_in relay;
    uint8_t     token[32];
    size_t      token_len;
    uint64_t    self_user;
    uint8_t     slot;
    uint16_t    seq;

    /* Sending keys: the one in use and the one waiting out its grace. */
    oc_sframe_key tx, tx_next;
    uint32_t    tx_epoch, tx_next_epoch;
    uint64_t    tx_ctr;
    int64_t     tx_next_at;

    rxkey       rx[MAX_RXKEYS];
    peer        peers[MAX_PEERS];
    uint32_t    epoch;

    volume      vols[64];
    int         n_vols;

    atomic_int  muted, ptt, ns, reopen_mic, reopen_spk;
    atomic_int  mic_level, speaking, mic_error, spk_error, loss_pct;
    atomic_uint sent, keepalives;
    int64_t     last_send;

    /* Screen sharing (REQ-161, VIDEO.md). share_mu guards all of it and is taken
     * before mu, never after. */
    oc_mutex_t  share_mu;
    uint64_t    sharer;            /* who the daemon says is sharing, 0 nobody */
    int         share_want;        /* this device asked to share */
    int         share_confirmed;   /* ...and the daemon named it */
    int         share_taken;       /* someone else took over */
    int         share_error;       /* OC_CAP_* opening or reading the source */
    char        share_dev[256];
    int         share_max_w, share_max_h;
    int         share_thread;      /* th_share was started and not yet joined */
    atomic_int  share_stop;
    oc_thread_t th_share, th_view;
    oc_share_tx stx;
    oc_share_rx srx;
    uint32_t    rx_gen;            /* bumped when the sharer changes: the decoder starts over */
    int         share_w, share_h, share_fps_now;
    int64_t     report_ms;
    uint32_t    view_errors;

    /* The newest decoded frame of someone else's share, for the frontend. */
    oc_mutex_t  view_mu;
    oc_frame    view;
    uint32_t    view_seq, view_frame_no;
};

/* --- the seam ------------------------------------------------------------------ */

static int  m_start(void *ctx, const char *host, uint16_t port, const uint8_t *token, size_t token_len,
                    uint64_t self_user, uint8_t self_slot);
static void m_roster(void *ctx, uint32_t epoch, const oc_call_part *parts, int n);
static void m_tx_key(void *ctx, uint32_t epoch, uint8_t slot, const uint8_t key[OC_CALL_KEY_LEN]);
static void m_rx_key(void *ctx, uint64_t user, uint8_t slot, uint32_t epoch, const uint8_t key[OC_CALL_KEY_LEN]);
static void m_stop(void *ctx);
static void m_sharer(void *ctx, uint64_t user);

static const oc_call_media MEDIA = { m_start, m_roster, m_tx_key, m_rx_key, m_stop, m_sharer, OC_CALL_CODEC_VP9 };
const oc_call_media *oc_call_engine_media(void) { return &MEDIA; }

static uint64_t kid_of(uint32_t epoch, uint8_t slot) { return ((uint64_t)epoch << 8) | slot; }

/* --- sending -------------------------------------------------------------------- */

static void send_raw(oc_call_engine *e, const uint8_t *payload, size_t len) {
    uint8_t pkt[PACKET_MAX];
    if (e->token_len + 2 + len > sizeof pkt) return;
    memcpy(pkt, e->token, e->token_len);
    pkt[e->token_len] = (uint8_t)(e->seq >> 8);
    pkt[e->token_len + 1] = (uint8_t)e->seq;
    e->seq++;
    if (len) memcpy(pkt + e->token_len + 2, payload, len);
    sendto(e->sock, (const char *)pkt, (int)(e->token_len + 2 + len), 0,
           (const struct sockaddr *)&e->relay, sizeof e->relay);
    e->last_send = now_ms();
}

/* A keep-alive is an empty payload: the relay needs only the token to know the
 * sender is still there, and an empty packet says nothing to anyone. */
static void send_keepalive(oc_call_engine *e) {
    send_raw(e, NULL, 0);
    atomic_fetch_add(&e->keepalives, 1);
}

/* One packet's plaintext -- its type first -- sealed under the sending key. One
 * counter for every type: it restarts with every key, and a key is never reused
 * (CALLS.md §5.4). */
static int send_pt(oc_call_engine *e, const uint8_t *pt, size_t n) {
    uint8_t ct[PACKET_MAX];
    size_t ctlen = 0;
    oc_mutex_lock(&e->mu);
    int64_t t = now_ms();
    if (e->tx_next.ready && t >= e->tx_next_at) {
        oc_sframe_key_wipe(&e->tx);
        e->tx = e->tx_next;
        e->tx_epoch = e->tx_next_epoch;
        e->tx_ctr = 0;
        memset(&e->tx_next, 0, sizeof e->tx_next);
    }
    int ok = e->tx.ready && e->sock >= 0 &&
             oc_sframe_encrypt(&e->tx, e->tx_ctr, NULL, 0, pt, n, ct, sizeof ct, &ctlen) == 0;
    if (ok) e->tx_ctr++;
    if (ok) send_raw(e, ct, ctlen);
    oc_mutex_unlock(&e->mu);
    return ok;
}

/* One Opus frame: type ‖ frame number ‖ packet. */
static void send_frame(oc_call_engine *e, uint32_t frame, const uint8_t *opus, int n) {
    uint8_t pt[5 + OC_OPUS_MAX_PACKET];
    pt[0] = OC_CALLPKT_AUDIO;
    pt[1] = (uint8_t)(frame >> 24); pt[2] = (uint8_t)(frame >> 16);
    pt[3] = (uint8_t)(frame >> 8);  pt[4] = (uint8_t)frame;
    memcpy(pt + 5, opus, (size_t)n);
    if (send_pt(e, pt, 5 + (size_t)n)) atomic_fetch_add(&e->sent, 1);
}

static void send_state(oc_call_engine *e, uint8_t flags) {
    uint8_t pt[2] = { OC_CALLPKT_STATE, flags };
    send_pt(e, pt, sizeof pt);
}

/* The share code's packets, sealed and sent like any other. */
static void share_emit(void *ctx, const uint8_t *pt, size_t len) { send_pt(ctx, pt, len); }

/* --- receiving ------------------------------------------------------------------ */

static peer *peer_of(oc_call_engine *e, uint64_t user) {
    for (int i = 0; i < MAX_PEERS; i++) if (e->peers[i].used && e->peers[i].user == user) return &e->peers[i];
    return NULL;
}

static rxkey *rxkey_of(oc_call_engine *e, uint64_t kid) {
    for (int i = 0; i < MAX_RXKEYS; i++) if (e->rx[i].used && e->rx[i].kid == kid) return &e->rx[i];
    return NULL;
}

static void rxkey_drop(rxkey *k) {
    oc_sframe_key_wipe(&k->key);
    memset(k, 0, sizeof *k);
}

static void on_packet(oc_call_engine *e, const uint8_t *pkt, size_t n) {
    if (n <= S2C_HDR) return;               /* a keep-alive, or nothing */
    uint64_t sender = 0;
    for (int i = 0; i < 8; i++) sender = (sender << 8) | pkt[i];
    const uint8_t *ct = pkt + S2C_HDR;
    size_t ctlen = n - S2C_HDR;
    uint64_t kid, ctr;
    size_t hl;
    if (oc_sframe_header_decode(ct, ctlen, &kid, &ctr, &hl) != 0) return;
    oc_mutex_lock(&e->mu);
    peer *p = peer_of(e, sender);
    rxkey *k = rxkey_of(e, kid);
    /* The KID must name a key this sender gave us: the relay says who sent it,
     * and a key belongs to one sender (CALLS.md §5.6). */
    if (!p || !k || k->user != sender || !oc_sframe_replay_ok(&k->rw, ctr)) {
        if (p) p->undecryptable++;
        oc_mutex_unlock(&e->mu);
        return;
    }
    uint8_t pt[PACKET_MAX];
    size_t ptlen = 0;
    uint64_t got_ctr = 0;
    if (oc_sframe_decrypt(&k->key, NULL, 0, ct, ctlen, pt, sizeof pt, &ptlen, &got_ctr) != 0 || ptlen < 2) {
        p->undecryptable++;
        oc_mutex_unlock(&e->mu);
        return;
    }
    oc_sframe_replay_mark(&k->rw, got_ctr);
    /* Loss, from the authenticated counter: a gap in it is packets that never
     * came (a gap in frame numbers alone may be DTX). */
    if (p->have_ctr && p->last_kid == kid && got_ctr > p->last_ctr + 1) p->lost += (uint32_t)(got_ctr - p->last_ctr - 1);
    if (!p->have_ctr || p->last_kid != kid || got_ctr > p->last_ctr) { p->last_ctr = got_ctr; p->last_kid = kid; }
    p->have_ctr = 1;
    p->packets++;
    /* This sender has moved to this key: its older ones go once what was in
     * flight has had time to land. */
    for (int i = 0; i < MAX_RXKEYS; i++)
        if (e->rx[i].used && e->rx[i].user == sender && e->rx[i].epoch < k->epoch && !e->rx[i].expire_ms)
            e->rx[i].expire_ms = now_ms() + RETIRE_MS;
    if (pt[0] == OC_CALLPKT_STATE) {
        p->muted_until = pt[1] & 1 ? now_ms() + MUTED_HOLD : 0;
    } else if (pt[0] == OC_CALLPKT_AUDIO && ptlen > 5) {
        uint32_t frame = ((uint32_t)pt[1] << 24) | ((uint32_t)pt[2] << 16) | ((uint32_t)pt[3] << 8) | pt[4];
        p->muted_until = 0;                          /* audio: not muted, whatever came before */
        oc_jb_put(&p->jb, frame, pt + 5, ptlen - 5, now_ms());
    } else if (pt[0] == OC_CALLPKT_VIDEO) {
        p->video_bytes += ptlen;
    }
    oc_mutex_unlock(&e->mu);
    /* A share's packets are handled outside mu: share_mu comes first. */
    if (pt[0] == OC_CALLPKT_VIDEO) {
        oc_mutex_lock(&e->share_mu);
        if (sender == e->sharer && sender != e->self_user) oc_share_rx_put(&e->srx, pt + 1, ptlen - 1, now_ms());
        oc_mutex_unlock(&e->share_mu);
    } else if (pt[0] == OC_CALLPKT_CONTROL) {
        oc_mutex_lock(&e->share_mu);
        if (e->share_confirmed && e->sharer == e->self_user)
            oc_share_tx_control(&e->stx, sender, pt + 1, ptlen - 1, now_ms(), share_emit, e);
        oc_mutex_unlock(&e->share_mu);
    }
}

static void *io_main(void *arg) {
    oc_call_engine *e = arg;
    uint8_t pkt[PACKET_MAX + 64];
    int64_t last_sweep = now_ms();
    send_keepalive(e);          /* the relay learns this address before anyone speaks */
    while (!atomic_load(&e->stop)) {
        if (oc_poll(e->sock, 0, 20) > 0) {
            for (;;) {
                int n = (int)recv(e->sock, (char *)pkt, (int)sizeof pkt, 0);
                if (n <= 0) break;
                on_packet(e, pkt, (size_t)n);
            }
        }
        int64_t t = now_ms();
        if (t - e->last_send >= KEEPALIVE_MS) send_keepalive(e);
        if (t - last_sweep >= 1000) {
            last_sweep = t;
            oc_mutex_lock(&e->mu);
            uint32_t pk = 0, lost = 0;
            for (int i = 0; i < MAX_RXKEYS; i++)
                if (e->rx[i].used && e->rx[i].expire_ms && t >= e->rx[i].expire_ms) rxkey_drop(&e->rx[i]);
            for (int i = 0; i < MAX_PEERS; i++) if (e->peers[i].used) { pk += e->peers[i].packets; lost += e->peers[i].lost; }
            oc_mutex_unlock(&e->mu);
            /* The encoder's FEC follows the loss the others' packets show on the
             * way here -- the nearest measure this end has of the way there. */
            int pct = pk + lost ? (int)((lost * 100u) / (pk + lost)) : 0;
            atomic_store(&e->loss_pct, pct);
        }
    }
    return NULL;
}

/* --- capture: microphone -> canceller -> preprocessor -> gate -> Opus ---------- */

static int peak(const int16_t *s, int n) {
    int p = 0;
    for (int i = 0; i < n; i++) { int v = s[i] < 0 ? -s[i] : s[i]; if (v > p) p = v; }
    return p > 32767 ? 32767 : p;
}

static void *capture_main(void *arg) {
    oc_call_engine *e = arg;
    oc_audio_dev *mic = NULL;
    const oc_audio_processor *proc = &OC_PROCESSOR_SPEEX;
    void *ps = proc->open(OC_VOICE_RATE, FRAME);
    if (!ps) { proc = &OC_PROCESSOR_NONE; ps = proc->open(OC_VOICE_RATE, FRAME); }
    SpeexPreprocessState *pre = speex_preprocess_state_init(FRAME, OC_VOICE_RATE);
    if (pre) {
        int on = 1, off = 0, ns_db = -25;
        float agc_level = 8000.0f;          /* about -12 dBFS peak for speech */
        speex_preprocess_ctl(pre, SPEEX_PREPROCESS_SET_DENOISE, &on);
        speex_preprocess_ctl(pre, SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &ns_db);
        speex_preprocess_ctl(pre, SPEEX_PREPROCESS_SET_AGC, &on);
        speex_preprocess_ctl(pre, SPEEX_PREPROCESS_SET_AGC_LEVEL, &agc_level);
        /* Not its voice-activity detector: speexdsp's own warning calls it "a
         * hack pending a complete rewrite". Speaking is judged by level below. */
        /* The residual-echo suppressor stays off, as it does for voice input: in
         * the ERLE harness's double-talk it took the voice with the echo. */
        speex_preprocess_ctl(pre, SPEEX_PREPROCESS_SET_DEREVERB, &off);
    }
    oc_opusenc *enc = oc_opusenc_open_voice(KBPS);
    int16_t frame[FRAME], ref[FRAME];
    uint8_t opus[OC_OPUS_MAX_PACKET];
    size_t have = 0;
    int64_t frame_pts = 0, next_tick = now_ms();
    uint32_t frame_no = 0;
    int loss_told = -1;
    int64_t last_state = 0;
    atomic_store(&e->reopen_mic, 1);
    while (!atomic_load(&e->stop)) {
        if (!e->opts.source && atomic_exchange(&e->reopen_mic, 0)) {
            if (mic) oc_audio_close(mic);
            int err = 0;
            mic = oc_audio_capture_open(e->mic_id[0] ? e->mic_id : NULL, OC_VOICE_RATE, 1, &err);
            atomic_store(&e->mic_error, mic ? 0 : err);
            have = 0;
        }
        if (e->opts.source) {
            int64_t t = now_ms();
            if (t < next_tick) { nap((int)(next_tick - t)); continue; }
            next_tick += 20;
            e->opts.source(e->opts.io_ctx, frame, FRAME);
            frame_pts = oc_media_clock_us();
        } else if (mic) {
            int64_t pts = 0;
            size_t got = oc_audio_capture_read(mic, frame + have, FRAME - have, &pts);
            if (got == 0) { nap(5); continue; }
            if (have == 0) frame_pts = pts;
            have += got;
            if (have < FRAME) continue;
            have = 0;
        } else {
            nap(20);                          /* no microphone: listening only */
            continue;
        }
        /* The call's own sound out first -- it plays through the device layer,
         * so the reference holds it -- then noise and level. */
        oc_audio_reference(frame_pts, ref, FRAME);
        proc->process(ps, frame, ref, FRAME);
        if (pre && atomic_load(&e->ns)) speex_preprocess_run(pre, frame);
        int lvl = peak(frame, FRAME);
        atomic_store(&e->mic_level, lvl);
        uint32_t f = frame_no++;
        int open = !atomic_load(&e->muted) || atomic_load(&e->ptt);
        atomic_store(&e->speaking, open && lvl > SPEAK_LEVEL);
        if (!open) {
            int64_t t = now_ms();
            if (t - last_state >= 1000) {
                send_state(e, 1);
                last_state = t;
            }
            continue;
        }
        if (!enc) continue;
        int pct = atomic_load(&e->loss_pct);
        if (pct != loss_told) { oc_opusenc_set_loss(enc, pct < 5 ? 5 : pct); loss_told = pct; }
        int n = oc_opusenc_encode_voice(enc, frame, opus, sizeof opus);
        if (n > 2) send_frame(e, f, opus, n);    /* 2 bytes or fewer: DTX silence */
    }
    if (mic) oc_audio_close(mic);
    oc_opusenc_close(enc);
    if (pre) speex_preprocess_state_destroy(pre);
    proc->close(ps);
    return NULL;
}

/* --- playout: jitter buffers -> decoders -> volumes -> mix -> speaker ----------- */

static float gain_of(const oc_call_engine *e, uint64_t user) {
    for (int i = 0; i < e->n_vols; i++) if (e->vols[i].user == user) return e->vols[i].gain;
    return 1.0f;
}

/* Sum with headroom, then a soft knee above -3 dBFS instead of clipping, so a
 * room full of people talking at once gets quieter rather than distorted. */
static int16_t limit(int32_t x) {
    const int32_t knee = 23000;
    int32_t a = x < 0 ? -x : x;
    if (a > knee) {
        int32_t over = a - knee, room = 32767 - knee;
        a = knee + (int32_t)((int64_t)room * over / (over + room));
    }
    return (int16_t)(x < 0 ? -a : a);
}

static void mix_frame(oc_call_engine *e, int16_t *out) {
    int32_t acc[FRAME];
    int16_t pcm[FRAME];
    memset(acc, 0, sizeof acc);
    oc_mutex_lock(&e->mu);
    int64_t t = now_ms();
    for (int i = 0; i < MAX_PEERS; i++) {
        peer *p = &e->peers[i];
        if (!p->used || !p->dec) continue;
        const uint8_t *d; size_t len;
        int what = oc_jb_get(&p->jb, p->quiet, &d, &len);
        int n = -1;
        if (what == OC_JB_PACKET)   n = oc_opusdec_decode_frame(p->dec, d, len, 0, pcm, FRAME);
        else if (what == OC_JB_FEC) n = oc_opusdec_decode_frame(p->dec, d, len, 1, pcm, FRAME);
        else if (what == OC_JB_PLC) n = oc_opusdec_decode_frame(p->dec, NULL, 0, 0, pcm, FRAME);
        if (n != FRAME) { p->level = 0; p->quiet = 1; continue; }
        int lvl = peak(pcm, FRAME);
        p->level = lvl;
        p->quiet = lvl < SPEAK_LEVEL / 3;
        if (lvl > SPEAK_LEVEL && what == OC_JB_PACKET) p->speaking_until = t + SPEAK_HOLD;
        float g = gain_of(e, p->user);
        for (int k = 0; k < FRAME; k++) acc[k] += (int32_t)(pcm[k] * g);
    }
    oc_mutex_unlock(&e->mu);
    for (int k = 0; k < FRAME; k++) out[k] = limit(acc[k]);
}

static void *play_main(void *arg) {
    oc_call_engine *e = arg;
    oc_audio_dev *spk = NULL;
    int16_t out[FRAME];
    int64_t next_tick = now_ms();
    atomic_store(&e->reopen_spk, 1);
    while (!atomic_load(&e->stop)) {
        if (!e->opts.sink && atomic_exchange(&e->reopen_spk, 0)) {
            if (spk) oc_audio_close(spk);
            int err = 0;
            spk = oc_audio_playback_open(e->spk_id[0] ? e->spk_id : NULL, OC_VOICE_RATE, 1, &err);
            atomic_store(&e->spk_error, spk ? 0 : err);
        }
        if (e->opts.sink) {
            int64_t t = now_ms();
            if (t < next_tick) { nap((int)(next_tick - t)); continue; }
            next_tick += 20;
            mix_frame(e, out);
            e->opts.sink(e->opts.io_ctx, out, FRAME);
            continue;
        }
        /* The device's consumption is the clock: keep 60 ms queued ahead of it
         * and make a frame whenever it drops below. */
        if (!spk) { mix_frame(e, out); nap(20); continue; }
        if (oc_audio_playback_queued(spk) >= 3 * FRAME) { nap(5); continue; }
        mix_frame(e, out);
        oc_audio_playback_write(spk, out, FRAME);
    }
    if (spk) oc_audio_close(spk);
    return NULL;
}

/* --- sharing a screen: capture -> still? -> fit -> VP9 -> fragments --------------- */

typedef struct { oc_call_engine *e; int w, h; } share_ctx;

static void share_packet(void *ctx, const oc_packet *pk) {
    share_ctx *sc = ctx;
    oc_call_engine *e = sc->e;
    oc_mutex_lock(&e->share_mu);
    oc_share_tx_frame(&e->stx, pk->data, pk->len, pk->keyframe, sc->w, sc->h, now_ms(), share_emit, e);
    oc_mutex_unlock(&e->share_mu);
}

static void *share_main(void *arg) {
    oc_call_engine *e = arg;
    oc_mutex_lock(&e->share_mu);
    char dev[256];
    snprintf(dev, sizeof dev, "%s", e->share_dev);
    int mw = e->share_max_w, mh = e->share_max_h;
    oc_mutex_unlock(&e->share_mu);
    int err = 0;
    oc_capture *cap = oc_capture_open_screen(dev, mw, mh, OC_SHARE_FPS, &err);
    if (cap && (err = oc_capture_start(cap)) != OC_CAP_OK) { oc_capture_close(cap); cap = NULL; }
    if (!cap) {
        oc_mutex_lock(&e->share_mu);
        e->share_error = err ? err : OC_CAP_FAILED;
        e->share_want = 0;
        oc_mutex_unlock(&e->share_mu);
        return NULL;
    }
    oc_vp9enc *enc = NULL;
    oc_frame scaled = { 0 };
    share_ctx sc = { e, 0, 0 };
    int small = 0, was_confirmed = 0, sent_in_sec = 0;
    uint64_t last_hash = 0;
    int64_t last_sent = 0, sec_at = now_ms();
    while (!atomic_load(&e->share_stop) && !atomic_load(&e->stop)) {
        oc_frame f;
        int rc = oc_capture_next(cap, &f, 100);
        if (rc < 0) {
            oc_mutex_lock(&e->share_mu);
            e->share_error = rc;
            oc_mutex_unlock(&e->share_mu);
            break;
        }
        int64_t t = now_ms();
        if (t - sec_at >= 1000) {
            oc_mutex_lock(&e->share_mu);
            e->share_fps_now = sent_in_sec;
            oc_mutex_unlock(&e->share_mu);
            sent_in_sec = 0;
            sec_at = t;
        }
        if (rc == 0) continue;
        oc_mutex_lock(&e->share_mu);
        int confirmed = e->share_confirmed;
        if (confirmed && !was_confirmed) oc_share_tx_restart(&e->stx, t);
        int want_key = confirmed && oc_share_tx_want_keyframe(&e->stx, t);
        int kbps = confirmed ? oc_share_tx_kbps(&e->stx, t) : OC_SHARE_KBPS_START;
        oc_mutex_unlock(&e->share_mu);
        if (!confirmed) { was_confirmed = 0; continue; }   /* the daemon has not named us yet */
        /* A still screen goes twice a second -- what arrives sharpens, and a
         * frame lost to a still screen is soon replaced -- a moving one at the
         * capture's rate. */
        uint64_t h = oc_share_frame_hash((const uint8_t *const *)f.plane, f.stride, f.width, f.height);
        if (was_confirmed && !want_key && h == last_hash && t - last_sent < OC_SHARE_STILL_MS) continue;
        int w, hh;
        oc_share_pick_size(f.width, f.height, kbps, &small, &w, &hh);
        if (!enc || w != sc.w || hh != sc.h) {
            oc_vp9enc_close(enc);
            oc_frame_free(&scaled);
            enc = oc_vp9enc_open_share(w, hh, (unsigned)kbps);
            if (!enc || ((w != f.width || hh != f.height) && oc_frame_alloc(&scaled, w, hh) != 0)) {
                oc_vp9enc_close(enc);
                enc = NULL;
                break;
            }
            sc.w = w; sc.h = hh;
            want_key = 1;
            oc_mutex_lock(&e->share_mu);
            e->share_w = w; e->share_h = hh;
            oc_mutex_unlock(&e->share_mu);
        }
        const oc_frame *in = &f;
        if (w != f.width || hh != f.height) {
            oc_i420_fit(&f, &scaled);
            scaled.pts_us = f.pts_us;
            in = &scaled;
        }
        oc_vp9enc_set_bitrate(enc, (unsigned)kbps);
        if (oc_vp9enc_encode(enc, in, want_key, share_packet, &sc) != 0) break;
        was_confirmed = 1;
        last_hash = h;
        last_sent = t;
        sent_in_sec++;
    }
    oc_vp9enc_close(enc);
    oc_frame_free(&scaled);
    oc_capture_close(cap);
    oc_mutex_lock(&e->share_mu);
    e->share_fps_now = 0;
    oc_mutex_unlock(&e->share_mu);
    return NULL;
}

/* Join the share thread, if there is one. Not with share_mu held. */
static void share_join(oc_call_engine *e) {
    oc_mutex_lock(&e->share_mu);
    int running = e->share_thread;
    e->share_thread = 0;
    oc_mutex_unlock(&e->share_mu);
    if (!running) return;
    atomic_store(&e->share_stop, 1);
    oc_thread_join(e->th_share);
    atomic_store(&e->share_stop, 0);
}

/* --- watching someone's share: fragments -> frames -> VP9 -> the frontend ---------- */

static void view_clear(oc_call_engine *e) {
    oc_mutex_lock(&e->view_mu);
    oc_frame_free(&e->view);
    memset(&e->view, 0, sizeof e->view);
    e->view_seq++;
    oc_mutex_unlock(&e->view_mu);
}

static void *view_main(void *arg) {
    oc_call_engine *e = arg;
    oc_vp9dec *dec = NULL;
    uint32_t gen = 0;
    uint8_t *buf = NULL;
    size_t cap = 0;
    while (!atomic_load(&e->stop)) {
        nap(5);
        int64_t t = now_ms();
        oc_mutex_lock(&e->share_mu);
        uint64_t sharer = e->sharer;
        if (!sharer || sharer == e->self_user) {
            oc_mutex_unlock(&e->share_mu);
            if (dec) { oc_vp9dec_close(dec); dec = NULL; }
            continue;
        }
        if (dec && gen != e->rx_gen) { oc_vp9dec_close(dec); dec = NULL; }
        gen = e->rx_gen;
        oc_share_rx_poll(&e->srx, t, share_emit, e);
        if (t - e->report_ms >= OC_SHARE_REPORT_MS) {
            /* What arrived from the sharer since the last report, by its SFrame
             * counter -- every type shares it, so a gap is loss of any of them. */
            int permille = 0;
            uint32_t kbps = 0;
            oc_mutex_lock(&e->mu);
            peer *p = peer_of(e, sharer);
            if (p) {
                uint32_t got = p->packets - p->rep_packets, lost = p->lost - p->rep_lost;
                permille = got + lost ? (int)(lost * 1000u / (got + lost)) : 0;
                kbps = (uint32_t)(p->video_bytes * 8 / (uint64_t)(t - e->report_ms > 0 ? t - e->report_ms : 1));
                p->rep_packets = p->packets;
                p->rep_lost = p->lost;
                p->video_bytes = 0;
            }
            oc_mutex_unlock(&e->mu);
            uint8_t pkt[OC_SHARE_CTL_MAX];
            if (p) share_emit(e, pkt, oc_share_ctl_report(pkt, sharer, permille, kbps));
            e->report_ms = t;
        }
        for (;;) {
            const uint8_t *d; size_t len; int key, w, h; uint32_t fno;
            if (!oc_share_rx_next(&e->srx, &d, &len, &key, &w, &h, &fno)) break;
            if (len > cap) {
                uint8_t *nb = realloc(buf, len);
                if (!nb) break;
                buf = nb; cap = len;
            }
            memcpy(buf, d, len);
            oc_mutex_unlock(&e->share_mu);
            if (!dec) dec = oc_vp9dec_open();
            oc_frame out;
            int rc = dec ? oc_vp9dec_decode(dec, buf, len, 0, &out) : -1;
            if (rc == 0) {
                oc_mutex_lock(&e->view_mu);
                if (e->view.width != out.width || e->view.height != out.height) {
                    oc_frame_free(&e->view);
                    if (oc_frame_alloc(&e->view, out.width, out.height) != 0) memset(&e->view, 0, sizeof e->view);
                }
                if (e->view.width) {
                    oc_frame_copy(&e->view, &out);
                    e->view_seq++;
                    e->view_frame_no = fno;
                }
                oc_mutex_unlock(&e->view_mu);
            }
            oc_mutex_lock(&e->share_mu);
            if (gen != e->rx_gen) break;               /* someone else shares now */
            if (rc < 0) { e->view_errors++; oc_share_rx_need_key(&e->srx); break; }
        }
        oc_mutex_unlock(&e->share_mu);
    }
    if (dec) oc_vp9dec_close(dec);
    free(buf);
    return NULL;
}

static void m_sharer(void *ctx, uint64_t user) {
    oc_call_engine *e = ctx;
    oc_mutex_lock(&e->share_mu);
    if (user != e->sharer) {
        e->sharer = user;
        oc_share_rx_free(&e->srx);
        oc_share_rx_init(&e->srx, user);
        e->rx_gen++;
        e->report_ms = 0;
    }
    if (e->share_want) {
        if (user == e->self_user) {
            e->share_confirmed = 1;
        } else if (e->share_confirmed) {
            /* Someone took over (one sharer at a time): this device stops. */
            e->share_confirmed = 0;
            e->share_want = 0;
            e->share_taken = 1;
            atomic_store(&e->share_stop, 1);
        }
    }
    oc_mutex_unlock(&e->share_mu);
    view_clear(e);
}

/* --- the seam's calls ------------------------------------------------------------- */

static void stop_threads(oc_call_engine *e) {
    share_join(e);          /* even between calls: a share started as one ended */
    if (!e->active) return;
    atomic_store(&e->stop, 1);
    oc_thread_join(e->th_io);
    oc_thread_join(e->th_cap);
    oc_thread_join(e->th_play);
    oc_thread_join(e->th_view);
    oc_closesock(e->sock);
    e->sock = -1;
    e->active = 0;
}

static void forget_call(oc_call_engine *e) {
    for (int i = 0; i < MAX_RXKEYS; i++) if (e->rx[i].used) rxkey_drop(&e->rx[i]);
    for (int i = 0; i < MAX_PEERS; i++) {
        if (e->peers[i].dec) oc_opusdec_close(e->peers[i].dec);
        memset(&e->peers[i], 0, sizeof e->peers[i]);
    }
    oc_sframe_key_wipe(&e->tx);
    oc_sframe_key_wipe(&e->tx_next);
    e->tx_epoch = e->tx_next_epoch = 0;
    e->tx_ctr = 0;
    e->epoch = 0;
    memset(e->token, 0, sizeof e->token);
}

/* Out of the call: nobody is sharing, and this device is not. Not with share_mu
 * held; the threads are stopped. */
static void forget_share(oc_call_engine *e) {
    oc_mutex_lock(&e->share_mu);
    e->sharer = 0;
    e->share_want = e->share_confirmed = 0;
    oc_share_rx_free(&e->srx);
    oc_share_rx_init(&e->srx, 0);
    e->rx_gen++;
    e->share_fps_now = 0;
    oc_mutex_unlock(&e->share_mu);
    view_clear(e);
}

static int m_start(void *ctx, const char *host, uint16_t port, const uint8_t *token, size_t token_len,
                   uint64_t self_user, uint8_t self_slot) {
    oc_call_engine *e = ctx;
    stop_threads(e);
    oc_mutex_lock(&e->mu);
    forget_call(e);
    oc_mutex_unlock(&e->mu);
    forget_share(e);
    if (!host || !token || token_len == 0 || token_len > sizeof e->token || !port) return -1;

    oc_sock_startup();
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;                  /* the relay is IPv4 (AUDIO.md §4) */
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) return -1;
    memcpy(&e->relay, res->ai_addr, sizeof e->relay);
    freeaddrinfo(res);
    e->relay.sin_port = htons(port);
    int s = (int)socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return -1;
    oc_sock_setnonblock(s);
    /* Room for a shared screen's keyframe, which comes as one burst. */
    int bufsz = 4 << 20;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char *)&bufsz, sizeof bufsz);
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char *)&bufsz, sizeof bufsz);
    e->sock = s;
    memcpy(e->token, token, token_len);
    e->token_len = token_len;
    e->self_user = self_user;
    e->slot = self_slot;
    oc_mutex_lock(&e->share_mu);
    e->stx.self = self_user;
    oc_mutex_unlock(&e->share_mu);
    e->seq = 0;
    e->last_send = 0;
    atomic_store(&e->stop, 0);
    atomic_store(&e->sent, 0);
    atomic_store(&e->keepalives, 0);
    atomic_store(&e->loss_pct, 0);
    if (oc_thread_create(&e->th_io, io_main, e) != 0) { oc_closesock(s); e->sock = -1; return -1; }
    if (oc_thread_create(&e->th_cap, capture_main, e) != 0) {
        atomic_store(&e->stop, 1); oc_thread_join(e->th_io); oc_closesock(s); e->sock = -1; return -1;
    }
    if (oc_thread_create(&e->th_play, play_main, e) != 0) {
        atomic_store(&e->stop, 1); oc_thread_join(e->th_io); oc_thread_join(e->th_cap);
        oc_closesock(s); e->sock = -1; return -1;
    }
    if (oc_thread_create(&e->th_view, view_main, e) != 0) {
        atomic_store(&e->stop, 1); oc_thread_join(e->th_io); oc_thread_join(e->th_cap);
        oc_thread_join(e->th_play); oc_closesock(s); e->sock = -1; return -1;
    }
    e->active = 1;
    return 0;
}

static void m_roster(void *ctx, uint32_t epoch, const oc_call_part *parts, int n) {
    oc_call_engine *e = ctx;
    uint64_t gone[MAX_PEERS];
    int n_gone = 0;
    oc_mutex_lock(&e->mu);
    e->epoch = epoch;
    /* Forget whoever left, with every key they gave; welcome whoever came. */
    for (int i = 0; i < MAX_PEERS; i++) {
        peer *p = &e->peers[i];
        if (!p->used) continue;
        int here = 0;
        for (int k = 0; k < n; k++) if (parts[k].user_id == p->user) here = 1;
        if (here) continue;
        for (int r = 0; r < MAX_RXKEYS; r++) if (e->rx[r].used && e->rx[r].user == p->user) rxkey_drop(&e->rx[r]);
        gone[n_gone++] = p->user;
        if (p->dec) oc_opusdec_close(p->dec);
        memset(p, 0, sizeof *p);
    }
    for (int k = 0; k < n; k++) {
        if (parts[k].user_id == e->self_user) continue;
        peer *p = peer_of(e, parts[k].user_id);
        if (!p) {
            for (int i = 0; i < MAX_PEERS && !p; i++) if (!e->peers[i].used) p = &e->peers[i];
            if (!p) continue;
            memset(p, 0, sizeof *p);
            p->used = 1;
            p->user = parts[k].user_id;
            p->quiet = 1;
            oc_jb_init(&p->jb);
            p->dec = oc_opusdec_open_rate(OC_VOICE_RATE);
        }
        p->slot = parts[k].slot;
    }
    oc_mutex_unlock(&e->mu);
    /* Whoever left no longer holds a share's bitrate down. */
    oc_mutex_lock(&e->share_mu);
    for (int i = 0; i < n_gone; i++) oc_share_tx_forget(&e->stx, gone[i]);
    oc_mutex_unlock(&e->share_mu);
}

static void m_tx_key(void *ctx, uint32_t epoch, uint8_t slot, const uint8_t key[OC_CALL_KEY_LEN]) {
    oc_call_engine *e = ctx;
    oc_mutex_lock(&e->mu);
    oc_sframe_key k;
    if (oc_sframe_key_init(&k, kid_of(epoch, slot), key, OC_CALL_KEY_LEN) == 0) {
        if (!e->tx.ready) {
            /* The first key of the call: nobody has anything of ours to decrypt
             * yet, so there is nothing to wait for. */
            e->tx = k;
            e->tx_epoch = epoch;
            e->tx_ctr = 0;
        } else {
            oc_sframe_key_wipe(&e->tx_next);
            e->tx_next = k;
            e->tx_next_epoch = epoch;
            e->tx_next_at = now_ms() + GRACE_MS;
        }
    }
    oc_mutex_unlock(&e->mu);
}

static void m_rx_key(void *ctx, uint64_t user, uint8_t slot, uint32_t epoch, const uint8_t key[OC_CALL_KEY_LEN]) {
    oc_call_engine *e = ctx;
    uint64_t kid = kid_of(epoch, slot);
    oc_mutex_lock(&e->mu);
    if (!rxkey_of(e, kid) && peer_of(e, user)) {
        rxkey *r = NULL;
        for (int i = 0; i < MAX_RXKEYS && !r; i++) if (!e->rx[i].used) r = &e->rx[i];
        if (!r) {
            /* Full: the oldest key of anyone goes first. */
            for (int i = 0; i < MAX_RXKEYS; i++) if (!r || e->rx[i].epoch < r->epoch) r = &e->rx[i];
            rxkey_drop(r);
        }
        if (oc_sframe_key_init(&r->key, kid, key, OC_CALL_KEY_LEN) == 0) {
            r->used = 1;
            r->kid = kid;
            r->user = user;
            r->epoch = epoch;
            memset(&r->rw, 0, sizeof r->rw);
            r->expire_ms = 0;
        }
    }
    oc_mutex_unlock(&e->mu);
}

static void m_stop(void *ctx) {
    oc_call_engine *e = ctx;
    stop_threads(e);
    oc_mutex_lock(&e->mu);
    forget_call(e);
    oc_mutex_unlock(&e->mu);
    forget_share(e);
}

/* --- the frontend's calls ------------------------------------------------------- */

oc_call_engine *oc_call_engine_new(const oc_call_engine_opts *o) {
    oc_call_engine *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    if (o) e->opts = *o;
    e->opts.mic_id = e->opts.speaker_id = NULL;
    if (o && o->mic_id) snprintf(e->mic_id, sizeof e->mic_id, "%s", o->mic_id);
    if (o && o->speaker_id) snprintf(e->spk_id, sizeof e->spk_id, "%s", o->speaker_id);
    e->sock = -1;
    atomic_store(&e->ns, o ? o->noise_suppression : 1);
    oc_mutex_init(&e->mu);
    oc_mutex_init(&e->share_mu);
    oc_mutex_init(&e->view_mu);
    oc_share_rx_init(&e->srx, 0);
    if (oc_share_tx_init(&e->stx, 0) != 0) {
        oc_mutex_destroy(&e->view_mu); oc_mutex_destroy(&e->share_mu); oc_mutex_destroy(&e->mu);
        free(e);
        return NULL;
    }
    return e;
}

void oc_call_engine_free(oc_call_engine *e) {
    if (!e) return;
    m_stop(e);
    share_join(e);
    oc_share_tx_free(&e->stx);
    oc_share_rx_free(&e->srx);
    oc_frame_free(&e->view);
    oc_mutex_destroy(&e->view_mu);
    oc_mutex_destroy(&e->share_mu);
    oc_mutex_destroy(&e->mu);
    free(e);
}

int oc_call_engine_share_start(oc_call_engine *e, const char *device_id, int max_w, int max_h) {
    share_join(e);
    oc_mutex_lock(&e->share_mu);
    if (!e->active) { oc_mutex_unlock(&e->share_mu); return -1; }
    snprintf(e->share_dev, sizeof e->share_dev, "%s", device_id ? device_id : "");
    e->share_max_w = max_w > 0 ? max_w : 1920;
    e->share_max_h = max_h > 0 ? max_h : 1080;
    e->share_want = 1;
    e->share_taken = 0;
    e->share_error = 0;
    e->share_confirmed = e->sharer == e->self_user && e->self_user != 0;
    e->share_w = e->share_h = 0;
    atomic_store(&e->share_stop, 0);
    int rc = oc_thread_create(&e->th_share, share_main, e);
    if (rc == 0) e->share_thread = 1;
    else e->share_want = 0;
    oc_mutex_unlock(&e->share_mu);
    return rc == 0 ? 0 : -1;
}

void oc_call_engine_share_stop(oc_call_engine *e) {
    share_join(e);
    oc_mutex_lock(&e->share_mu);
    e->share_want = e->share_confirmed = 0;
    oc_mutex_unlock(&e->share_mu);
}

int oc_call_engine_share_frame(oc_call_engine *e, oc_frame *dst, uint32_t *seq, uint32_t *frame_no) {
    oc_mutex_lock(&e->view_mu);
    int rc = 0;
    if (e->view.width && *seq != e->view_seq) {
        if (dst->width != e->view.width || dst->height != e->view.height) {
            oc_frame_free(dst);
            if (oc_frame_alloc(dst, e->view.width, e->view.height) != 0) { oc_mutex_unlock(&e->view_mu); return 0; }
        }
        oc_frame_copy(dst, &e->view);
        *seq = e->view_seq;
        if (frame_no) *frame_no = e->view_frame_no;
        rc = 1;
    } else if (!e->view.width && *seq != e->view_seq) {
        *seq = e->view_seq;
        rc = -1;                                     /* nothing to show any more */
    }
    oc_mutex_unlock(&e->view_mu);
    return rc;
}

void oc_call_engine_set_mute(oc_call_engine *e, int muted) { atomic_store(&e->muted, muted ? 1 : 0); }
int  oc_call_engine_muted(const oc_call_engine *e) { return atomic_load(&((oc_call_engine *)e)->muted); }
void oc_call_engine_set_ptt(oc_call_engine *e, int held) { atomic_store(&e->ptt, held ? 1 : 0); }
void oc_call_engine_set_noise_suppression(oc_call_engine *e, int on) { atomic_store(&e->ns, on ? 1 : 0); }

void oc_call_engine_set_volume(oc_call_engine *e, uint64_t user_id, float gain) {
    if (gain < 0.0f) gain = 0.0f;
    if (gain > 2.0f) gain = 2.0f;
    oc_mutex_lock(&e->mu);
    int i = 0;
    while (i < e->n_vols && e->vols[i].user != user_id) i++;
    if (i == e->n_vols) {
        if (e->n_vols == (int)(sizeof e->vols / sizeof e->vols[0])) i = 0;   /* full: reuse the first */
        else e->n_vols++;
    }
    e->vols[i].user = user_id;
    e->vols[i].gain = gain;
    oc_mutex_unlock(&e->mu);
}

float oc_call_engine_volume(const oc_call_engine *e, uint64_t user_id) {
    oc_call_engine *m = (oc_call_engine *)e;
    oc_mutex_lock(&m->mu);
    float g = gain_of(m, user_id);
    oc_mutex_unlock(&m->mu);
    return g;
}

void oc_call_engine_set_devices(oc_call_engine *e, const char *mic_id, const char *speaker_id) {
    oc_mutex_lock(&e->mu);
    snprintf(e->mic_id, sizeof e->mic_id, "%s", mic_id ? mic_id : "");
    snprintf(e->spk_id, sizeof e->spk_id, "%s", speaker_id ? speaker_id : "");
    oc_mutex_unlock(&e->mu);
    atomic_store(&e->reopen_mic, 1);
    atomic_store(&e->reopen_spk, 1);
}

void oc_call_engine_stats(oc_call_engine *e, oc_call_stats *out) {
    memset(out, 0, sizeof *out);
    oc_mutex_lock(&e->mu);
    out->active = e->active;
    out->self_user = e->self_user;
    out->slot = e->slot;
    out->epoch = e->tx.ready ? e->tx_epoch : 0;
    int64_t t = now_ms();
    for (int i = 0; i < MAX_PEERS && out->n_peers < 32; i++) {
        peer *p = &e->peers[i];
        if (!p->used) continue;
        oc_call_peer_stats *s = &out->peers[out->n_peers++];
        s->user_id = p->user;
        s->slot = p->slot;
        s->speaking = p->speaking_until > t;
        s->muted = p->muted_until > t;
        s->level = p->level;
        s->keyed = rxkey_of(e, kid_of(e->epoch, p->slot)) != NULL;
        s->packets = p->packets;
        s->lost = p->lost;
        s->late = p->jb.late;
        s->fec = p->jb.fec;
        s->plc = p->jb.plc;
        s->undecryptable = p->undecryptable;
        int tm = oc_jb_target_ms(&p->jb);
        if (tm > out->target_ms) out->target_ms = tm;
    }
    oc_mutex_unlock(&e->mu);
    out->mic_error = atomic_load(&e->mic_error);
    out->speaker_error = atomic_load(&e->spk_error);
    out->muted = atomic_load(&e->muted);
    out->ptt = atomic_load(&e->ptt);
    out->speaking = atomic_load(&e->speaking);
    out->mic_level = atomic_load(&e->mic_level);
    out->sent = atomic_load(&e->sent);
    out->keepalives = atomic_load(&e->keepalives);
    out->loss_pct = atomic_load(&e->loss_pct);
    oc_mutex_lock(&e->share_mu);
    out->sharer = e->sharer;
    out->share_state = !e->share_want ? 0 : e->share_confirmed ? 2 : 1;
    out->share_taken = e->share_taken;
    out->share_error = e->share_error;
    out->share_width = e->share_w;
    out->share_height = e->share_h;
    out->share_fps = e->share_fps_now;
    out->share_kbps = e->stx.kbps;
    out->share_frames = e->stx.frames;
    out->share_keyframes = e->stx.keyframes;
    out->share_resent = e->stx.resent;
    out->share_nacks = e->stx.nacks;
    out->share_plis = e->stx.plis;
    out->share_reports = e->stx.reports;
    out->view_frames = e->srx.frames;
    out->view_nacks = e->srx.nacks;
    out->view_plis = e->srx.plis;
    out->view_skipped = e->srx.skipped;
    out->view_errors = e->view_errors;
    oc_mutex_unlock(&e->share_mu);
    oc_mutex_lock(&e->view_mu);
    out->view_width = e->view.width;
    out->view_height = e->view.height;
    out->view_frame_no = e->view_frame_no;
    oc_mutex_unlock(&e->view_mu);
}
