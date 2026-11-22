/* Tests for screen sharing's packet logic (REQ-161, docs/details/CALLS.md §6.4-§6.6), with no
 * network or device: a sharer and a viewer joined by a simulated network that
 * loses, delays, reorders and duplicates. Fragmenting and reassembling; NACKs
 * recovering lost fragments and whole lost frames; a PLI bringing a keyframe when
 * a frame cannot be recovered and for a late joiner; the rate following the
 * viewers' reports; the size stepping down and back; malformed fragments refused;
 * and the synthetic screen through VP9, the lossy network and back. */
#include "check.h"
#include "oc_capture.h"
#include "oc_codec.h"
#include "oc_share.h"

#include <stdlib.h>
#include <string.h>

static uint32_t rng_state;
static uint32_t rnd(void) { rng_state = rng_state * 1664525u + 1013904223u; return rng_state >> 8; }
static int chance(int permille) { return (int)(rnd() % 1000) < permille; }

#define VIEWER 77u
#define SHARER 55u

/* ---- the simulated network ------------------------------------------------------ */

typedef struct { int64_t at; int to_rx; uint16_t len; uint8_t d[OC_SHARE_MAX_PT]; } flight;

typedef struct {
    flight *fl;
    int     n, cap;
    int64_t now;
    int     loss, dup, jitter;        /* per mille, per mille, ms */
    int     drop_nacks;               /* the sharer never hears a NACK */
    int     drop_frame;               /* the first sending of this frame is lost (-1 none) */
    int     drop_frame_all;           /* ...and its resends too */
    int     first_send;               /* oc_share_tx_frame is sending, not a resend */
    int     viewer_on;                /* the viewer is there */
    oc_share_tx tx;
    oc_share_rx rx;
    uint32_t sent_len[4096];
    int      sent_key[4096];
    uint32_t pkts_to_rx, pkts_to_tx;
} sim;

static void queue(sim *s, int to_rx, const uint8_t *pt, size_t len) {
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 1024;
        s->fl = realloc(s->fl, (size_t)s->cap * sizeof *s->fl);
    }
    flight *f = &s->fl[s->n++];
    f->at = s->now + 5 + (s->jitter ? (int)(rnd() % (unsigned)s->jitter) : 0);
    f->to_rx = to_rx;
    f->len = (uint16_t)len;
    memcpy(f->d, pt, len);
}

static void send_net(sim *s, int to_rx, const uint8_t *pt, size_t len) {
    if (to_rx) s->pkts_to_rx++; else s->pkts_to_tx++;
    if (chance(s->loss)) return;
    queue(s, to_rx, pt, len);
    if (chance(s->dup)) queue(s, to_rx, pt, len);
}

static void emit_tx(void *ctx, const uint8_t *pt, size_t len) {
    sim *s = ctx;
    if (s->drop_frame >= 0 && pt[0] == OC_CALLPKT_VIDEO && (s->first_send || s->drop_frame_all)) {
        uint32_t fr = ((uint32_t)pt[1] << 24) | ((uint32_t)pt[2] << 16) | ((uint32_t)pt[3] << 8) | pt[4];
        if ((int)fr == s->drop_frame) return;
    }
    send_net(s, 1, pt, len);
}

static void emit_rx(void *ctx, const uint8_t *pt, size_t len) {
    sim *s = ctx;
    if (s->drop_nacks && len > 9 && pt[9] == OC_SHARE_NACK) return;
    send_net(s, 0, pt, len);
}

static void deliver(sim *s) {
    for (int i = 0; i < s->n; ) {
        if (s->fl[i].at > s->now) { i++; continue; }
        flight f = s->fl[i];
        s->fl[i] = s->fl[--s->n];
        if (f.to_rx) { if (s->viewer_on) oc_share_rx_put(&s->rx, f.d + 1, f.len - 1u, s->now); }
        else oc_share_tx_control(&s->tx, VIEWER, f.d + 1, f.len - 1u, s->now, emit_tx, s);
    }
}

static void sim_init(sim *s, uint32_t seed) {
    memset(s, 0, sizeof *s);
    rng_state = seed;
    s->drop_frame = -1;
    s->viewer_on = 1;
    CHECK(oc_share_tx_init(&s->tx, SHARER) == 0);
    oc_share_tx_restart(&s->tx, 0);
    oc_share_rx_init(&s->rx, SHARER);
}

static void sim_free(sim *s) {
    oc_share_tx_free(&s->tx);
    oc_share_rx_free(&s->rx);
    free(s->fl);
}

/* The bytes of frame `f`: its number in the first four, then a pattern of it. */
static void blob(uint32_t f, uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) b[i] = (uint8_t)(f * 31u + i * 7u);
    if (n >= 4) { b[0] = (uint8_t)(f >> 24); b[1] = (uint8_t)(f >> 16); b[2] = (uint8_t)(f >> 8); b[3] = (uint8_t)f; }
}

typedef struct { int got, bad, order, after_skip_not_key; uint32_t last; int have_last; int64_t first_at; } seen;

/* Take every frame the viewer can decode, checking each is what was sent. */
static void drain(sim *s, seen *v) {
    const uint8_t *d; size_t len; int key, w, h; uint32_t fr;
    while (oc_share_rx_next(&s->rx, &d, &len, &key, &w, &h, &fr)) {
        static uint8_t want[1 << 20];
        if (fr >= 4096 || len != s->sent_len[fr] || key != s->sent_key[fr] || w != 1280 || h != 720) { v->bad++; continue; }
        blob(fr, want, len);
        if (memcmp(d, want, len) != 0) v->bad++;
        if (v->have_last && fr <= v->last) v->order++;
        if (v->have_last && fr != v->last + 1 && !key) v->after_skip_not_key++;
        if (!v->have_last) v->first_at = s->now;
        v->last = fr; v->have_last = 1;
        v->got++;
    }
}

/* Run `frames` frames at 15 fps, a keyframe when the sharer's rules want one and
 * otherwise deltas of `size` bytes (keyframes 5×), with the viewer polling every
 * 5 ms. */
static void run(sim *s, int frames, size_t size, seen *v) {
    static uint8_t buf[1 << 20];
    memset(v, 0, sizeof *v);
    int64_t next = s->now;
    int sent = 0;
    while (sent < frames || s->n) {
        if (sent < frames && s->now >= next) {
            int key = oc_share_tx_want_keyframe(&s->tx, s->now);
            size_t n = key ? size * 5 : size + (rnd() % 100);
            uint32_t fr = s->tx.next_frame;
            blob(fr, buf, n);
            s->sent_len[fr] = (uint32_t)n;
            s->sent_key[fr] = key;
            s->first_send = 1;
            CHECK(oc_share_tx_frame(&s->tx, buf, n, key, 1280, 720, s->now, emit_tx, s) == 0);
            s->first_send = 0;
            sent++;
            next += 66;
        }
        deliver(s);
        if (s->viewer_on) {
            oc_share_rx_poll(&s->rx, s->now, emit_rx, s);
            drain(s, v);
        }
        s->now += 5;
        if (s->now > 600000) break;
    }
}

static void test_fragments_clean(void) {
    sim s; sim_init(&s, 1);
    seen v;
    run(&s, 100, 5000, &v);
    CHECK(v.got == 100 && v.bad == 0 && v.order == 0);
    CHECK(s.tx.keyframes == 1 && s.rx.nacks == 0 && s.rx.plis == 0);
    /* Edge sizes: one byte, exactly a fragment, a fragment and one. */
    static const size_t sizes[] = { 1, OC_SHARE_FRAG, OC_SHARE_FRAG + 1, 3 * OC_SHARE_FRAG };
    for (int i = 0; i < 4; i++) {
        uint8_t b[4 * OC_SHARE_FRAG];
        uint32_t fr = s.tx.next_frame;
        blob(fr, b, sizes[i]);
        s.sent_len[fr] = (uint32_t)sizes[i]; s.sent_key[fr] = 0;
        CHECK(oc_share_tx_frame(&s.tx, b, sizes[i], 0, 1280, 720, s.now, emit_tx, &s) == 0);
        s.now += 20; deliver(&s);
        seen w = v; drain(&s, &w);
        CHECK(w.got == v.got + 1 && w.bad == 0);
        v = w;
    }
    /* Too large to send: refused, and a keyframe wanted after it. */
    size_t big = (size_t)(OC_SHARE_MAX_FRAGS + 1) * OC_SHARE_FRAG;
    uint8_t *b = calloc(1, big);
    s.tx.key_wanted = 0;
    CHECK(oc_share_tx_frame(&s.tx, b, big, 0, 1280, 720, s.now, emit_tx, &s) == -1 && s.tx.key_wanted);
    free(b);
    sim_free(&s);
}

static void test_loss_recovered(void) {
    /* 5% loss both ways, 0-40 ms of jitter (so reordering), 2% duplicates. */
    sim s; sim_init(&s, 7);
    s.loss = 50; s.jitter = 40; s.dup = 20;
    seen v;
    run(&s, 300, 8000, &v);
    printf("  5%% loss: %d/300 frames, %u NACKs, %u fragments resent, %u PLIs, %u given up\n",
           v.got, s.rx.nacks, s.tx.resent, s.rx.plis, s.rx.skipped);
    CHECK(v.bad == 0 && v.order == 0 && v.after_skip_not_key == 0);
    CHECK(v.got >= 280);                        /* NACKs bring back nearly everything */
    CHECK(s.rx.nacks > 0 && s.tx.resent > 0 && s.rx.dups > 0);
    sim_free(&s);
}

static void test_whole_frame_lost(void) {
    /* Every fragment of frame 10 lost the first time: the viewer sees the gap
     * when frame 11 arrives and asks for the whole frame. */
    sim s; sim_init(&s, 3);
    s.drop_frame = 10;
    seen v;
    run(&s, 30, 3000, &v);
    CHECK(v.got == 30 && v.bad == 0 && s.rx.skipped == 0 && s.tx.resent > 0);
    sim_free(&s);
}

static void test_pli_when_unrecoverable(void) {
    /* Frame 5 never gets through and NACKs are lost: the viewer gives it up and
     * asks for a keyframe, which the sharer sends; what follows decodes. */
    sim s; sim_init(&s, 4);
    s.drop_frame = 5; s.drop_frame_all = 1; s.drop_nacks = 1;
    seen v;
    run(&s, 60, 3000, &v);
    CHECK(s.rx.plis >= 1 && s.tx.plis >= 1 && s.tx.keyframes >= 2);
    CHECK(s.rx.skipped >= 1 && v.bad == 0 && v.after_skip_not_key == 0);
    CHECK(v.have_last && v.last == 59);         /* back in step by the end */
    sim_free(&s);
}

static void test_first_keyframe_lost(void) {
    /* The share's first keyframe lost whole on the way, and every resend of it:
     * a viewer cannot show a delta without it, so it asks for a keyframe,
     * gets one, and from there shows ten frames and more in a row, each what was
     * sent. On the simulated clock the picture starts once the sharer may send
     * another keyframe (one a second at most) and the request is in. */
    sim s; sim_init(&s, 6);
    s.drop_frame = 0; s.drop_frame_all = 1;
    seen v;
    run(&s, 60, 3000, &v);
    CHECK(s.rx.plis >= 1 && s.tx.plis >= 1 && s.tx.keyframes >= 2);
    CHECK(v.bad == 0 && v.order == 0 && v.after_skip_not_key == 0);
    CHECK(v.got >= 10 && v.have_last && v.last == 59);
    CHECK(v.first_at <= OC_SHARE_KEY_MIN_MS + OC_SHARE_PLI_MS);
    sim_free(&s);
}

static void test_late_joiner(void) {
    /* The viewer arrives after 3 seconds of deltas: its first poll asks for a
     * keyframe, and it is watching within a second and a half. */
    sim s; sim_init(&s, 5);
    s.viewer_on = 0;
    seen v;
    run(&s, 45, 2000, &v);
    CHECK(v.got == 0 && s.tx.keyframes == 1);
    s.viewer_on = 1;
    int64_t joined = s.now;
    run(&s, 45, 2000, &v);
    CHECK(s.rx.plis >= 1 && s.tx.keyframes == 2);
    CHECK(v.got > 0 && v.first_at - joined <= 1500 && v.bad == 0);
    sim_free(&s);
}

static void test_rate_control(void) {
    oc_share_tx t;
    CHECK(oc_share_tx_init(&t, SHARER) == 0);
    oc_share_tx_restart(&t, 0);
    uint8_t pkt[OC_SHARE_CTL_MAX];
    int64_t now = 0;
    CHECK(oc_share_tx_kbps(&t, now) == OC_SHARE_KBPS_START);
    /* Clean reports: a tenth more a second, up to the ceiling. */
    int k1 = oc_share_tx_kbps(&t, now += 1000);
    CHECK(k1 > OC_SHARE_KBPS_START && k1 <= OC_SHARE_KBPS_START * 11 / 10 + 1);
    for (int i = 0; i < 30; i++) oc_share_tx_kbps(&t, now += 1000);
    CHECK(oc_share_tx_kbps(&t, now) == OC_SHARE_KBPS_MAX);
    /* One viewer losing 10%: halved, and not again within the second. */
    size_t n = oc_share_ctl_report(pkt, OC_CALLPKT_VIDEO, SHARER, 100, 2000);
    oc_share_tx_control(&t, VIEWER, pkt + 1, n - 1, now += 10, NULL, NULL);
    CHECK(t.kbps == OC_SHARE_KBPS_MAX / 2 && t.reports == 1);
    oc_share_tx_control(&t, VIEWER, pkt + 1, n - 1, now += 400, NULL, NULL);
    CHECK(t.kbps == OC_SHARE_KBPS_MAX / 2);
    /* Down to the floor, never below. */
    for (int i = 0; i < 20; i++) oc_share_tx_control(&t, VIEWER, pkt + 1, n - 1, now += 1000, NULL, NULL);
    CHECK(t.kbps == OC_SHARE_KBPS_MIN);
    /* 3% holds growth; a report for someone else is not ours. */
    n = oc_share_ctl_report(pkt, OC_CALLPKT_VIDEO, SHARER, 30, 100);
    oc_share_tx_control(&t, VIEWER, pkt + 1, n - 1, now += 1000, NULL, NULL);
    CHECK(oc_share_tx_kbps(&t, now + 500) == OC_SHARE_KBPS_MIN);
    n = oc_share_ctl_report(pkt, OC_CALLPKT_VIDEO, 999, 900, 100);
    oc_share_tx_control(&t, VIEWER, pkt + 1, n - 1, now += 1000, NULL, NULL);
    CHECK(oc_share_tx_kbps(&t, now) > OC_SHARE_KBPS_MIN);
    /* Keyframes: on request once a second at most, and every ten seconds. */
    t.key_at_ms = now; t.have_key = 1; t.key_wanted = 0;
    CHECK(!oc_share_tx_want_keyframe(&t, now + 500));
    n = oc_share_ctl_pli(pkt, OC_CALLPKT_VIDEO, SHARER);
    oc_share_tx_control(&t, VIEWER, pkt + 1, n - 1, now + 500, NULL, NULL);
    CHECK(!oc_share_tx_want_keyframe(&t, now + 900) && oc_share_tx_want_keyframe(&t, now + 1000));
    t.key_wanted = 0;
    CHECK(!oc_share_tx_want_keyframe(&t, now + 9999) && oc_share_tx_want_keyframe(&t, now + 10000));
    oc_share_tx_free(&t);
}

static void test_size_and_hash(void) {
    int small = 0, w, h;
    oc_share_pick_size(1920, 1080, 2000, &small, &w, &h);
    CHECK(!small && w == 1920 && h == 1080);
    oc_share_pick_size(1920, 1080, 800, &small, &w, &h);    /* between: stays */
    CHECK(!small && w == 1920);
    oc_share_pick_size(1920, 1200, 500, &small, &w, &h);
    CHECK(small && w == 1152 && h == 720);
    oc_share_pick_size(1920, 1080, 900, &small, &w, &h);    /* between: stays small */
    CHECK(small && w == 1280 && h == 720);
    oc_share_pick_size(1920, 1080, 1100, &small, &w, &h);
    CHECK(!small && w == 1920);
    oc_share_pick_size(1000, 600, 200, &small, &w, &h);     /* already small enough */
    CHECK(small && w == 1000 && h == 600);

    oc_frame a, b;
    CHECK(oc_frame_alloc(&a, 64, 32) == 0 && oc_frame_alloc(&b, 64, 32) == 0);
    oc_i420_fill(&a, 100, 128, 128);
    oc_i420_fill(&b, 100, 128, 128);
    uint64_t ha = oc_share_frame_hash((const uint8_t *const *)a.plane, a.stride, a.width, a.height);
    CHECK(ha == oc_share_frame_hash((const uint8_t *const *)b.plane, b.stride, b.width, b.height));
    b.plane[0][33 * 1 + 5] = 101;
    CHECK(ha != oc_share_frame_hash((const uint8_t *const *)b.plane, b.stride, b.width, b.height));
    oc_frame_free(&a); oc_frame_free(&b);
}

/* A fragment's header, after the type byte. */
static void hdr(uint8_t *p, uint32_t frame, int frag, int nfrags, int key, int w, int h) {
    p[0] = (uint8_t)(frame >> 24); p[1] = (uint8_t)(frame >> 16); p[2] = (uint8_t)(frame >> 8); p[3] = (uint8_t)frame;
    p[4] = (uint8_t)(frag >> 8); p[5] = (uint8_t)frag;
    p[6] = (uint8_t)(nfrags >> 8); p[7] = (uint8_t)nfrags;
    p[8] = (uint8_t)key;
    p[9] = (uint8_t)(w >> 8); p[10] = (uint8_t)w;
    p[11] = (uint8_t)(h >> 8); p[12] = (uint8_t)h;
}

static void test_malformed(void) {
    oc_share_rx r; oc_share_rx_init(&r, SHARER);
    uint8_t p[OC_SHARE_MAX_PT];
    memset(p, 0, sizeof p);
    /* frame 0, frag 0 of 2, key, 1280x720, a full fragment: fine. */
    hdr(p, 0, 0, 2, 1, 1280, 720);
    CHECK(oc_share_rx_put(&r, p, 13 + OC_SHARE_FRAG, 0) == 0);
    CHECK(oc_share_rx_put(&r, p, 13 + 10, 0) == -1);                  /* a short middle fragment */
    CHECK(oc_share_rx_put(&r, p, 13, 0) == -1);                       /* no bytes at all */
    hdr(p, 0, 0, 0, 1, 1280, 720);
    CHECK(oc_share_rx_put(&r, p, 13 + 10, 0) == -1);                  /* nfrags 0 */
    hdr(p, 0, 3, 2, 1, 1280, 720);
    CHECK(oc_share_rx_put(&r, p, 13 + 10, 0) == -1);                  /* frag past the end */
    hdr(p, 0, 1, 2, 1, 1281, 720);
    CHECK(oc_share_rx_put(&r, p, 13 + 10, 0) == -1);                  /* odd width */
    hdr(p, 0, 1, 2, 1, 1296, 720);
    CHECK(oc_share_rx_put(&r, p, 13 + 10, 0) == -1);                  /* not the size frame 0 said */
    hdr(p, 0, 1, 2, 1, 1280, 720);
    CHECK(oc_share_rx_put(&r, p, 13 + 10, 0) == 0);                   /* the last fragment */
    const uint8_t *d; size_t len; int key, w, h; uint32_t fr;
    CHECK(oc_share_rx_next(&r, &d, &len, &key, &w, &h, &fr) == 1);
    CHECK(len == OC_SHARE_FRAG + 10 && key && w == 1280 && h == 720 && fr == 0);
    /* A control packet cut short does nothing. */
    oc_share_tx t; CHECK(oc_share_tx_init(&t, SHARER) == 0);
    uint8_t c[OC_SHARE_CTL_MAX];
    uint16_t frags[2] = { 1, 2 };
    size_t n = oc_share_ctl_nack(c, OC_CALLPKT_VIDEO, SHARER, 0, frags, 2);
    oc_share_tx_control(&t, VIEWER, c + 1, n - 3, 0, NULL, NULL);
    CHECK(t.nacks == 0);
    oc_share_tx_free(&t);
    oc_share_rx_free(&r);
}

/* ---- the synthetic screen through VP9 and a lossy network ----------------------- */

typedef struct { sim *s; int w, h; } venc;

static void on_vpacket(void *ctx, const oc_packet *p) {
    venc *v = ctx;
    CHECK(oc_share_tx_frame(&v->s->tx, p->data, p->len, p->keyframe, v->w, v->h, v->s->now, emit_tx, v->s) == 0);
}

static void test_vp9_through_loss(void) {
    sim s; sim_init(&s, 11);
    s.loss = 30; s.jitter = 20;
    const oc_capture_backend *b = &oc_capture_synthetic_screen;
    int err = 0;
    void *cap = b->open("screen:synthetic", 1280, 800, 15, &err);
    CHECK(cap != NULL && b->start(cap) == OC_CAP_OK);
    oc_vp9enc *enc = oc_vp9enc_open_share(1280, 800, 1500);
    oc_vp9dec *dec = oc_vp9dec_open();
    CHECK(enc && dec);
    venc ve = { &s, 1280, 800 };
    int decoded = 0, bad = 0, last = -1, rising = 1, errors = 0;
    for (int i = 0; i < 10 && cap && enc && dec; i++) {
        oc_frame f;
        if (b->next(cap, &f, 1000) != 1) { CHECK(0); break; }
        f.pts_us = s.now * 1000;
        int key = oc_share_tx_want_keyframe(&s.tx, s.now);
        CHECK(oc_vp9enc_encode(enc, &f, key, on_vpacket, &ve) == 0);
        for (int k = 0; k < 40; k++) {                 /* 200 ms of network per frame */
            deliver(&s);
            oc_share_rx_poll(&s.rx, s.now, emit_rx, &s);
            const uint8_t *d; size_t len; int kf, w, h; uint32_t fr;
            while (oc_share_rx_next(&s.rx, &d, &len, &kf, &w, &h, &fr)) {
                oc_frame out;
                int rc = oc_vp9dec_decode(dec, d, len, 0, &out);
                if (rc < 0) { errors++; oc_share_rx_need_key(&s.rx); break; }
                if (rc == 1) continue;
                decoded++;
                if (out.width != 1280 || out.height != 800) bad++;
                int n = oc_capture_synthetic_frame_number(&out);
                if (n < 0) bad++;
                if (n <= last) rising = 0;
                last = n;
            }
            s.now += 5;
        }
    }
    printf("  VP9 1280x800 at 3%% loss: %d/10 decoded, %u keyframes, %u NACKs, %u resent\n",
           decoded, s.tx.keyframes, s.rx.nacks, s.tx.resent);
    CHECK(decoded >= 9 && bad == 0 && rising && errors == 0);
    CHECK(s.tx.resent > 0 || s.rx.nacks == 0);
    oc_vp9enc_close(enc);
    oc_vp9dec_close(dec);
    if (cap) b->close(cap);
    sim_free(&s);
}

/* What was emitted: fragments by type (camera, share, other), and the stream
 * the last control packet was about. */
static int g_types[3], g_ctl_pkt;
static void count_type(void *ctx, const uint8_t *pt, size_t len) {
    (void)ctx; (void)len;
    g_types[pt[0] == OC_CALLPKT_CAMERA ? 0 : pt[0] == OC_CALLPKT_VIDEO ? 1 : 2]++;
}
static void last_ctl(void *ctx, const uint8_t *pt, size_t len) {
    (void)ctx;
    if (pt[0] == OC_CALLPKT_CONTROL) g_ctl_pkt = oc_share_ctl_pkt(pt + 1, len - 1);
}

/* A camera's stream (REQ-160): the same transport on its own packet type --
 * type 4 fragments, control kinds marked as a camera's -- so a camera and a
 * share from one sender never take each other's packets; recovering 5% loss as
 * a share does; and its rate between its own floor and the 600 kbit/s ceiling. */
static void test_camera_stream(void) {
    sim s; sim_init(&s, 21);
    oc_share_tx_free(&s.tx); oc_share_rx_free(&s.rx);
    CHECK(oc_share_tx_init_camera(&s.tx, SHARER) == 0);
    oc_share_tx_restart(&s.tx, 0);
    oc_share_rx_init_camera(&s.rx, SHARER);
    CHECK(s.tx.kbps == OC_CAM_KBPS_START);
    s.loss = 50; s.jitter = 30;
    seen v;
    run(&s, 200, 1500, &v);
    printf("  camera at 5%% loss: %d/200 frames, %u NACKs, %u resent\n", v.got, s.rx.nacks, s.tx.resent);
    CHECK(v.bad == 0 && v.order == 0 && v.got >= 185 && s.rx.nacks > 0 && s.tx.resent > 0);

    /* Its fragments are type 4, a share's type 2: the engine hands each type to
     * its own receiver. */
    uint8_t b[2500]; memset(b, 7, sizeof b);
    g_types[0] = g_types[1] = g_types[2] = 0;
    CHECK(oc_share_tx_frame(&s.tx, b, sizeof b, 1, 640, 480, s.now, count_type, NULL) == 0);
    oc_share_tx share; CHECK(oc_share_tx_init(&share, SHARER) == 0);
    CHECK(oc_share_tx_frame(&share, b, 100, 1, 640, 480, s.now, count_type, NULL) == 0);
    CHECK(g_types[0] == 3 && g_types[1] == 1 && g_types[2] == 0);
    uint8_t pkt[OC_SHARE_CTL_MAX];
    /* Control: a camera's PLI is the camera's, not the share's. */
    share.key_wanted = 0; share.have_key = 1;
    s.tx.key_wanted = 0;
    size_t n = oc_share_ctl_pli(pkt, OC_CALLPKT_CAMERA, SHARER);
    CHECK(oc_share_ctl_pkt(pkt + 1, n - 1) == OC_CALLPKT_CAMERA);
    oc_share_tx_control(&share, VIEWER, pkt + 1, n - 1, s.now, NULL, NULL);
    oc_share_tx_control(&s.tx, VIEWER, pkt + 1, n - 1, s.now, NULL, NULL);
    CHECK(!share.key_wanted && share.plis == 0 && s.tx.key_wanted && s.tx.plis == 1);
    n = oc_share_ctl_pli(pkt, OC_CALLPKT_VIDEO, SHARER);
    CHECK(oc_share_ctl_pkt(pkt + 1, n - 1) == OC_CALLPKT_VIDEO);
    oc_share_tx_control(&s.tx, VIEWER, pkt + 1, n - 1, s.now, NULL, NULL);
    oc_share_tx_control(&share, VIEWER, pkt + 1, n - 1, s.now, NULL, NULL);
    CHECK(s.tx.plis == 1 && share.plis == 1);

    /* A camera's receiver asks the camera for a keyframe, a share's the share. */
    oc_share_rx srx; oc_share_rx_init(&srx, SHARER);
    oc_share_rx crx; oc_share_rx_init_camera(&crx, SHARER);
    g_ctl_pkt = -1; oc_share_rx_poll(&crx, 0, last_ctl, NULL); oc_share_rx_poll(&crx, OC_SHARE_JOIN_MS, last_ctl, NULL);
    CHECK(g_ctl_pkt == OC_CALLPKT_CAMERA);
    g_ctl_pkt = -1; oc_share_rx_poll(&srx, 0, last_ctl, NULL); oc_share_rx_poll(&srx, OC_SHARE_JOIN_MS, last_ctl, NULL);
    CHECK(g_ctl_pkt == OC_CALLPKT_VIDEO);
    oc_share_rx_free(&srx); oc_share_rx_free(&crx);

    /* The rate: a camera never goes above 600 or below its floor. */
    oc_share_tx_restart(&s.tx, 0);
    int64_t now = 0;
    for (int i = 0; i < 40; i++) oc_share_tx_kbps(&s.tx, now += 1000);
    CHECK(oc_share_tx_kbps(&s.tx, now) == OC_CAM_KBPS_MAX);
    n = oc_share_ctl_report(pkt, OC_CALLPKT_CAMERA, SHARER, 200, 100);
    for (int i = 0; i < 20; i++) oc_share_tx_control(&s.tx, VIEWER, pkt + 1, n - 1, now += 1000, NULL, NULL);
    CHECK(s.tx.kbps == OC_CAM_KBPS_MIN);
    /* A share's report does not move a camera's rate. */
    oc_share_tx_restart(&s.tx, now);
    uint32_t reports = s.tx.reports;
    n = oc_share_ctl_report(pkt, OC_CALLPKT_VIDEO, SHARER, 900, 100);
    oc_share_tx_control(&s.tx, VIEWER, pkt + 1, n - 1, now += 1000, NULL, NULL);
    CHECK(s.tx.kbps == OC_CAM_KBPS_START && s.tx.reports == reports);
    oc_share_tx_free(&share);
    sim_free(&s);
}

/* The camera's ladder (REQ-160): down the sizes first, then the frame rate, as
 * the rate falls; back up a step at a time, each after a quiet while; a source
 * fitted into each step's box keeping its shape; and the platform's ceiling. */
static void test_camera_ladder(void) {
    oc_cam_ladder l; oc_cam_ladder_init(&l, 0);
    int w, h, fps;
    CHECK(oc_cam_ladder_pick(&l, 600, 0, 1280, 720, 0, 0, 0, &w, &h, &fps) == 0 && w == 640 && h == 360 && fps == 30);
    CHECK(oc_cam_ladder_pick(&l, 600, 0, 640, 480, 0, 0, 0, &w, &h, &fps) == 0 && w == 640 && h == 480);
    /* Falling: 640x480, 320x240, 160x120 at 30, then 15, then 7 -- size first. */
    CHECK(oc_cam_ladder_pick(&l, 300, 100, 640, 480, 0, 0, 0, &w, &h, &fps) == 1 && w == 320 && h == 240 && fps == 30);
    CHECK(oc_cam_ladder_pick(&l, 150, 200, 640, 480, 0, 0, 0, &w, &h, &fps) == 2 && w == 160 && h == 120 && fps == 30);
    CHECK(oc_cam_ladder_pick(&l, 100, 300, 640, 480, 0, 0, 0, &w, &h, &fps) == 3 && w == 160 && fps == 15);
    CHECK(oc_cam_ladder_pick(&l, 60, 400, 640, 480, 0, 0, 0, &w, &h, &fps) == 4 && w == 160 && fps == 7);
    /* A sudden fall goes all the way at once. */
    oc_cam_ladder_init(&l, 0);
    CHECK(oc_cam_ladder_pick(&l, 60, 10, 640, 480, 0, 0, 0, &w, &h, &fps) == 4);
    /* Rising: one step, and only once OC_CAM_STEP_UP_MS have passed. */
    CHECK(oc_cam_ladder_pick(&l, 600, 10 + OC_CAM_STEP_UP_MS - 1, 640, 480, 0, 0, 0, &w, &h, &fps) == 4);
    CHECK(oc_cam_ladder_pick(&l, 600, 10 + OC_CAM_STEP_UP_MS, 640, 480, 0, 0, 0, &w, &h, &fps) == 3);
    CHECK(oc_cam_ladder_pick(&l, 600, 10 + OC_CAM_STEP_UP_MS + 1, 640, 480, 0, 0, 0, &w, &h, &fps) == 3);
    int64_t t = 10 + OC_CAM_STEP_UP_MS;
    for (int i = 0; i < 3; i++) { t += OC_CAM_STEP_UP_MS; oc_cam_ladder_pick(&l, 600, t, 640, 480, 0, 0, 0, &w, &h, &fps); }
    CHECK(l.step == 0 && w == 640 && fps == 30);
    /* A rate between steps holds where it is: no flapping. */
    CHECK(oc_cam_ladder_pick(&l, 400, t + 10 * OC_CAM_STEP_UP_MS, 640, 480, 0, 0, 0, &w, &h, &fps) == 0);
    /* The browser's ceiling: 640x480 at 15. */
    CHECK(oc_cam_ladder_pick(&l, 600, t, 640, 480, 640, 480, 15, &w, &h, &fps) == 0 && w == 640 && fps == 15);
    CHECK(oc_cam_ladder_pick(&l, 600, t, 1280, 720, 320, 240, 15, &w, &h, &fps) == 0 && w == 320 && h == 180);
}

/* Lip sync (REQ-160): a frame waits for its sender's voice, up to a limit, is
 * dropped once the voice is more than 200 ms past it, and with no voice playing
 * is shown at once; and a camera frame's bytes carry the audio frame number. */
static void test_camera_sync(void) {
    CHECK(oc_cam_sync(100, 1, 95, 0) == OC_CAM_HOLD);                    /* 100 ms ahead */
    CHECK(oc_cam_sync(100, 1, 95, OC_CAM_HOLD_MS) == OC_CAM_SHOW);       /* held long enough */
    CHECK(oc_cam_sync(100, 1, 100, 0) == OC_CAM_SHOW);                   /* in step */
    CHECK(oc_cam_sync(100, 1, 110, 0) == OC_CAM_SHOW);                   /* 200 ms behind: still shown */
    CHECK(oc_cam_sync(100, 1, 111, 0) == OC_CAM_DROP);                   /* 220 ms behind */
    CHECK(oc_cam_sync(100, 0, 0, 0) == OC_CAM_SHOW);                     /* no voice */
    CHECK(oc_cam_sync(2, 1, 0xFFFFFFFEu, 0) == OC_CAM_HOLD);             /* across the wrap */
    uint8_t buf[64]; static const uint8_t vp9[5] = { 1, 2, 3, 4, 5 };
    size_t n = oc_cam_frame_put(buf, 0xA1B2C3D4u, vp9, sizeof vp9);
    uint32_t af = 0; const uint8_t *d = NULL; size_t dn = 0;
    CHECK(n == OC_CAM_PREFIX + 5 && oc_cam_frame_get(buf, n, &af, &d, &dn) == 0);
    CHECK(af == 0xA1B2C3D4u && dn == 5 && memcmp(d, vp9, 5) == 0);
    CHECK(oc_cam_frame_get(buf, OC_CAM_PREFIX, &af, &d, &dn) == -1);
}

/* The camera's encoder (REQ-160): the synthetic camera at 640x480 through it
 * and back, every frame decoded in order at its size; a keyframe only when asked
 * for -- the first, and one asked for midway -- and none of its own accord. */
typedef struct { oc_vp9dec *dec; int decoded, bad, keys, last, rising; } cam_out;

static void on_cam_packet(void *ctx, const oc_packet *p) {
    cam_out *o = ctx;
    if (p->keyframe) o->keys++;
    oc_frame out;
    int rc = oc_vp9dec_decode(o->dec, p->data, p->len, 0, &out);
    if (rc != 0) { o->bad++; return; }
    o->decoded++;
    if (out.width != 640 || out.height != 480) o->bad++;
    int n = oc_capture_synthetic_frame_number(&out);
    if (n <= o->last) o->rising = 0;
    o->last = n;
}

static void test_camera_encoder(void) {
    const oc_capture_backend *b = &oc_capture_synthetic;
    int err = 0;
    void *cap = b->open("synthetic", 640, 480, 30, &err);
    CHECK(cap != NULL && b->start(cap) == OC_CAP_OK);
    oc_vp9enc *enc = oc_vp9enc_open_camera(640, 480, 30, 600);
    cam_out o = { oc_vp9dec_open(), 0, 0, 0, -1, 1 };
    CHECK(enc && o.dec);
    CHECK(oc_vp9enc_open_camera(641, 480, 30, 600) == NULL);   /* odd sizes are refused */
    for (int i = 0; i < 40 && cap && enc && o.dec; i++) {
        oc_frame f;
        if (b->next(cap, &f, 1000) != 1) { CHECK(0); break; }
        if (i == 20) CHECK(oc_vp9enc_set_bitrate(enc, 300) == 0);
        CHECK(oc_vp9enc_encode(enc, &f, i == 0 || i == 30, on_cam_packet, &o) == 0);
    }
    printf("  camera 640x480: %d/40 decoded, %d keyframes\n", o.decoded, o.keys);
    CHECK(o.decoded == 40 && o.bad == 0 && o.rising && o.keys == 2);
    oc_vp9enc_close(enc);
    oc_vp9dec_close(o.dec);
    if (cap) b->close(cap);
}

int run_share_media_tests(void) {
    printf("test_share_media: fragments, loss + NACK, whole frame lost, PLI, first keyframe lost, late joiner, rate, size, malformed, VP9 through loss, a camera's stream, ladder, lip sync and encoder\n");
    test_fragments_clean();
    test_loss_recovered();
    test_whole_frame_lost();
    test_pli_when_unrecoverable();
    test_first_keyframe_lost();
    test_late_joiner();
    test_rate_control();
    test_size_and_hash();
    test_malformed();
    test_vp9_through_loss();
    test_camera_stream();
    test_camera_ladder();
    test_camera_sync();
    test_camera_encoder();
    return failures;
}
