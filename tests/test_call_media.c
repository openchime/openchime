/* Tests for the call's media pieces that need no network or device (docs/CALLS.md
 * §3): the jitter buffer against a simulated network -- steady, jittery,
 * reordered, lossy, bursty, duplicated, silent between talk spurts -- and Opus
 * at 16 kHz with FEC, concealment and DTX. */
#include "check.h"
#include "oc_codec.h"
#include "oc_jitter.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* A tiny deterministic generator, so a failure repeats. */
static uint32_t rng_state;
static uint32_t rnd(void) { rng_state = rng_state * 1664525u + 1013904223u; return rng_state >> 8; }
static int chance(int pct) { return (int)(rnd() % 100) < pct; }

typedef struct { uint32_t frame; int64_t at; int dropped; } sim_pkt;

typedef struct {
    int played, packet, fec, plc, none_mid;   /* none_mid: silence inside a talk spurt */
    int plc_mid;                              /* concealment before the stream's end */
    int sent;                                 /* distinct frames that arrived */
    int max_target;
} sim_out;

/* Send `n` frames every 20 ms; each arrives after `base` plus up to `jitter` ms,
 * `loss`% never arrive (in bursts of `burst` when set), `dup`% arrive twice.
 * Frames in [gap_from, gap_to) are not sent at all (DTX). Play one frame every
 * 20 ms of simulated time and count what came out. */
static sim_out simulate(int n, int base, int jitter, int loss, int burst, int dup,
                        int gap_from, int gap_to, uint32_t seed) {
    rng_state = seed;
    sim_pkt *p = calloc((size_t)n * 2, sizeof *p);
    int np = 0, o_sent = 0;
    for (int f = 0; f < n; f++) {
        if (f >= gap_from && f < gap_to) continue;
        int lost = 0;
        if (burst) { if (chance(loss)) { for (int k = 0; k < burst && f < n; k++) if (k) f++; lost = 1; } }
        else lost = chance(loss);
        if (lost) continue;
        o_sent++;
        p[np++] = (sim_pkt){ (uint32_t)f, (int64_t)f * 20 + base + (jitter ? (int64_t)(rnd() % (unsigned)(jitter + 1)) : 0), 0 };
        if (dup && chance(dup)) p[np++] = (sim_pkt){ (uint32_t)f, (int64_t)f * 20 + base + jitter, 0 };
    }
    oc_jitter *j = calloc(1, sizeof *j);
    oc_jb_init(j);
    sim_out o;
    memset(&o, 0, sizeof o);
    o.sent = o_sent;
    uint8_t payload[3] = { 1, 2, 3 };
    int quiet = 1;
    int64_t end = (int64_t)n * 20 + base + jitter + 600;
    for (int64_t t = 0; t < end; t += 20) {
        for (int i = 0; i < np; i++)
            if (!p[i].dropped && p[i].at <= t) { oc_jb_put(j, p[i].frame, payload, sizeof payload, p[i].at); p[i].dropped = 1; }
        const uint8_t *d; size_t len;
        int w = oc_jb_get(j, quiet, &d, &len);
        if (w == OC_JB_PACKET) { o.packet++; quiet = 0; }
        else if (w == OC_JB_FEC) { o.fec++; quiet = 0; }
        else if (w == OC_JB_PLC) { o.plc++; if (t < (int64_t)n * 20 - 400) o.plc_mid++; }
        else { quiet = 1; if (o.packet > 0 && t < (int64_t)n * 20 - 400) o.none_mid++; }
        if (w != OC_JB_NONE) o.played++;
        if (oc_jb_target_ms(j) > o.max_target) o.max_target = oc_jb_target_ms(j);
    }
    free(j);
    free(p);
    return o;
}

static void jitter_tests(void) {
    /* A perfect network: every frame played from its own packet, once, and the
     * delay stays at the floor. */
    sim_out o = simulate(500, 30, 0, 0, 0, 0, -1, -1, 1);
    CHECK(o.packet == 500 && o.fec == 0 && o.plc_mid == 0);
    CHECK(o.max_target == 40);

    /* Up to 120 ms of jitter, which also reorders: the target grows to cover it
     * and almost everything plays from its own packet. */
    o = simulate(1500, 30, 120, 0, 0, 0, -1, -1, 2);
    printf("  jitter 0-120 ms: %d packets, %d concealed, target %d ms\n", o.packet, o.plc, o.max_target);
    CHECK(o.max_target >= 120 && o.max_target <= 240);
    CHECK(o.packet >= 1500 * 97 / 100);

    /* 10% random loss: a lost frame whose successor arrived is rebuilt from that
     * successor's FEC, so FEC recovers most losses and concealment the rest. */
    o = simulate(2000, 30, 0, 10, 0, 0, -1, -1, 3);
    printf("  10%% loss: %d packets, %d from FEC, %d concealed\n", o.packet, o.fec, o.plc);
    CHECK(o.fec > 0 && o.fec + o.plc >= 2000 - o.packet - 5);
    CHECK(o.fec >= (o.fec + o.plc) * 8 / 10);
    CHECK(o.none_mid == 0);                    /* no hole: every lost frame was filled */

    /* 20% loss with jitter. Still no holes inside the stream. */
    o = simulate(2000, 30, 60, 20, 0, 0, -1, -1, 4);
    printf("  20%% loss + 60 ms jitter: %d packets, %d FEC, %d concealed, %d silent\n",
           o.packet, o.fec, o.plc, o.none_mid);
    CHECK(o.packet + o.fec + o.plc >= 2000 * 95 / 100);

    /* Bursts of five: concealed for three frames, then silence -- a gap that
     * long looks like the sender pausing -- and the stream starts again on the
     * next packet, every time. */
    o = simulate(2000, 30, 0, 3, 5, 0, -1, -1, 5);
    printf("  bursts of 5: %d packets, %d FEC, %d concealed, %d silent\n", o.packet, o.fec, o.plc, o.none_mid);
    CHECK(o.plc_mid > 0 && o.none_mid > 0);
    CHECK(o.packet == o.sent);                 /* everything that arrived was played */

    /* Duplicates change nothing heard. */
    o = simulate(500, 30, 0, 0, 0, 30, -1, -1, 6);
    CHECK(o.packet == 500 && o.plc_mid == 0);

    /* A silence between talk spurts (DTX sends nothing): the second spurt plays
     * in full rather than being thrown away as late. */
    o = simulate(600, 30, 20, 0, 0, 0, 200, 400, 7);
    CHECK(o.packet == 400);

    /* The target never leaves its bounds, however bad the network. */
    o = simulate(1000, 30, 900, 0, 0, 0, -1, -1, 8);
    CHECK(o.max_target <= 240);
}

/* Opus at 16 kHz: a tone survives the round trip; a lost frame comes back from
 * the next packet's FEC or from concealment, at full length; silence is DTX. */
static void opus_tests(void) {
    oc_opusenc *enc = oc_opusenc_open_voice(24);
    oc_opusdec *dec = oc_opusdec_open_rate(OC_VOICE_RATE);
    CHECK(enc && dec);
    if (!enc || !dec) { oc_opusenc_close(enc); oc_opusdec_close(dec); return; }
    oc_opusenc_set_loss(enc, 20);
    int16_t pcm[OC_VOICE_FRAME], out[OC_VOICE_FRAME];
    uint8_t pk[60][OC_OPUS_MAX_PACKET];
    int len[60];
    for (int f = 0; f < 60; f++) {
        for (int i = 0; i < OC_VOICE_FRAME; i++)
            pcm[i] = (int16_t)(8000.0 * sin(2 * M_PI * 440.0 * (f * OC_VOICE_FRAME + i) / OC_VOICE_RATE));
        len[f] = oc_opusenc_encode_voice(enc, pcm, pk[f], sizeof pk[f]);
    }
    CHECK(len[10] > 2 && len[10] < 200);
    int ok = 1;
    double e_fec = 0, e_plc = 0;
    for (int f = 0; f < 60; f++) {
        int n;
        if (f == 40) n = oc_opusdec_decode_frame(dec, pk[41], (size_t)len[41], 1, out, OC_VOICE_FRAME);   /* lost: FEC */
        else if (f == 50) n = oc_opusdec_decode_frame(dec, NULL, 0, 0, out, OC_VOICE_FRAME);            /* lost: PLC */
        else n = oc_opusdec_decode_frame(dec, pk[f], (size_t)len[f], 0, out, OC_VOICE_FRAME);
        if (n != OC_VOICE_FRAME) ok = 0;
        double e = 0;
        for (int i = 0; i < OC_VOICE_FRAME; i++) e += (double)out[i] * out[i];
        if (f == 40) e_fec = sqrt(e / OC_VOICE_FRAME);
        if (f == 50) e_plc = sqrt(e / OC_VOICE_FRAME);
    }
    printf("  opus 16 kHz: FEC frame rms %.0f, concealed frame rms %.0f (tone rms 5657)\n", e_fec, e_plc);
    CHECK(ok);
    CHECK(e_fec > 2000 && e_plc > 1000);
    /* Silence: once DTX settles, the encoder says there is nothing worth sending. */
    memset(pcm, 0, sizeof pcm);
    int small = 0;
    for (int f = 0; f < 50; f++) { uint8_t b[OC_OPUS_MAX_PACKET]; if (oc_opusenc_encode_voice(enc, pcm, b, sizeof b) <= 2) small++; }
    CHECK(small > 30);
    oc_opusenc_close(enc);
    oc_opusdec_close(dec);
}

int run_call_media_tests(void) {
    printf("test_call_media: the jitter buffer on a simulated network (steady, 0-120 ms jitter, 10%% and 20%% loss, bursts, duplicates, DTX gaps, bounds); Opus 16 kHz FEC, concealment and DTX\n");
    jitter_tests();
    opus_tests();
    return failures;
}
