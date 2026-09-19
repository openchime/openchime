/* The call's media engine — see oc_call_engine.h and docs/CALLS.md §3-§5. */
#include "sock.h"       /* first: winsock2 before anything pulls in windows.h */

#include "oc_call_engine.h"

#include "audio_dev.h"
#include "e2e_sframe.h"
#include "oc_codec.h"
#include "oc_jitter.h"
#include "oc_media.h"
#include "oc_processor.h"
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
/* A frame number no audio frame has: the packet is this sender's STATE -- one
 * byte, bit 0 muted -- sent every second while muted, inside the encryption,
 * so the others can show it and the daemon cannot see it. */
#define STATE_FRAME  0xFFFFFFFFu
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
};

/* --- the seam ------------------------------------------------------------------ */

static int  m_start(void *ctx, const char *host, uint16_t port, const uint8_t *token, size_t token_len,
                    uint64_t self_user, uint8_t self_slot);
static void m_roster(void *ctx, uint32_t epoch, const oc_call_part *parts, int n);
static void m_tx_key(void *ctx, uint32_t epoch, uint8_t slot, const uint8_t key[OC_CALL_KEY_LEN]);
static void m_rx_key(void *ctx, uint64_t user, uint8_t slot, uint32_t epoch, const uint8_t key[OC_CALL_KEY_LEN]);
static void m_stop(void *ctx);

static const oc_call_media MEDIA = { m_start, m_roster, m_tx_key, m_rx_key, m_stop };
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

/* One Opus frame, as frame number ‖ packet, sealed under the sending key. The
 * counter restarts with every key, and a key is never reused (CALLS.md §5.4). */
static void send_frame(oc_call_engine *e, uint32_t frame, const uint8_t *opus, int n) {
    uint8_t pt[4 + OC_OPUS_MAX_PACKET], ct[4 + OC_OPUS_MAX_PACKET + OC_SFRAME_OVERHEAD];
    size_t ctlen = 0;
    pt[0] = (uint8_t)(frame >> 24); pt[1] = (uint8_t)(frame >> 16);
    pt[2] = (uint8_t)(frame >> 8);  pt[3] = (uint8_t)frame;
    memcpy(pt + 4, opus, (size_t)n);
    oc_mutex_lock(&e->mu);
    int64_t t = now_ms();
    if (e->tx_next.ready && t >= e->tx_next_at) {
        oc_sframe_key_wipe(&e->tx);
        e->tx = e->tx_next;
        e->tx_epoch = e->tx_next_epoch;
        e->tx_ctr = 0;
        memset(&e->tx_next, 0, sizeof e->tx_next);
    }
    int ok = e->tx.ready && oc_sframe_encrypt(&e->tx, e->tx_ctr, NULL, 0, pt, 4 + (size_t)n,
                                              ct, sizeof ct, &ctlen) == 0;
    if (ok) e->tx_ctr++;
    if (ok) send_raw(e, ct, ctlen);
    oc_mutex_unlock(&e->mu);
    if (ok) atomic_fetch_add(&e->sent, 1);
}

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
    uint8_t pt[4 + OC_JB_MAX_PACKET];
    size_t ptlen = 0;
    uint64_t got_ctr = 0;
    if (oc_sframe_decrypt(&k->key, NULL, 0, ct, ctlen, pt, sizeof pt, &ptlen, &got_ctr) != 0 || ptlen < 4) {
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
    uint32_t frame = ((uint32_t)pt[0] << 24) | ((uint32_t)pt[1] << 16) | ((uint32_t)pt[2] << 8) | pt[3];
    if (frame == STATE_FRAME) {
        p->muted_until = ptlen > 4 && (pt[4] & 1) ? now_ms() + MUTED_HOLD : 0;
    } else {
        p->muted_until = 0;                          /* audio: not muted, whatever came before */
        oc_jb_put(&p->jb, frame, pt + 4, ptlen - 4, now_ms());
    }
    oc_mutex_unlock(&e->mu);
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
                uint8_t muted = 1;
                send_frame(e, STATE_FRAME, &muted, 1);
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

/* --- the seam's calls ------------------------------------------------------------- */

static void stop_threads(oc_call_engine *e) {
    if (!e->active) return;
    atomic_store(&e->stop, 1);
    oc_thread_join(e->th_io);
    oc_thread_join(e->th_cap);
    oc_thread_join(e->th_play);
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

static int m_start(void *ctx, const char *host, uint16_t port, const uint8_t *token, size_t token_len,
                   uint64_t self_user, uint8_t self_slot) {
    oc_call_engine *e = ctx;
    stop_threads(e);
    oc_mutex_lock(&e->mu);
    forget_call(e);
    oc_mutex_unlock(&e->mu);
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
    e->sock = s;
    memcpy(e->token, token, token_len);
    e->token_len = token_len;
    e->self_user = self_user;
    e->slot = self_slot;
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
    e->active = 1;
    return 0;
}

static void m_roster(void *ctx, uint32_t epoch, const oc_call_part *parts, int n) {
    oc_call_engine *e = ctx;
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
    return e;
}

void oc_call_engine_free(oc_call_engine *e) {
    if (!e) return;
    m_stop(e);
    oc_mutex_destroy(&e->mu);
    free(e);
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
}
