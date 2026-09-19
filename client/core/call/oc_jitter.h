/* One sender's jitter buffer (docs/CALLS.md §3): packets in by frame number,
 * one frame out every 20 ms, and a decision for each -- decode the packet, rebuild
 * it from the next packet's FEC, conceal it, or play nothing. No codec here, so
 * it can be tested on a simulated network.
 *
 * The target delay adapts: it is the 95th percentile of how late packets arrive
 * relative to the earliest over the last two seconds, plus a frame, bounded to
 * 40-240 ms. It is reached by waiting -- at the start of a talk spurt, or by
 * concealing a frame when a packet came too late -- and given back only by
 * skipping a frame while nothing is being said, so speech is never cut to catch
 * up. */
#ifndef OC_JITTER_H
#define OC_JITTER_H

#include <stddef.h>
#include <stdint.h>

#define OC_JB_FRAME_MS   20
#define OC_JB_SLOTS      64          /* 1.28 s of frames */
#define OC_JB_MAX_PACKET 512
#define OC_JB_HIST       100         /* two seconds of arrivals */
#define OC_JB_MIN_MS     40
#define OC_JB_MAX_MS     240
#define OC_JB_PLC_MAX    3           /* frames concealed before it is silence */

enum { OC_JB_NONE = 0, OC_JB_PACKET, OC_JB_FEC, OC_JB_PLC };

typedef struct {
    int      used;
    uint32_t frame;
    uint16_t len;
    uint8_t  data[OC_JB_MAX_PACKET];
} oc_jb_slot;

typedef struct {
    oc_jb_slot slot[OC_JB_SLOTS];
    int      started;
    int      idle;                   /* played past the newest frame: the sender is quiet */
    uint32_t play;                   /* the frame the next tick plays */
    uint32_t newest;
    int      target;                 /* frames */
    int      grow;                   /* frames still to wait to reach the target */
    int      concealed;              /* frames concealed in a row */
    int64_t  delay[OC_JB_HIST];      /* arrival - frame time, ms */
    int      n_delay, i_delay;
    /* What happened, for the call's statistics. */
    uint32_t received, late, dup, fec, plc, skipped;
} oc_jitter;

void oc_jb_init(oc_jitter *j);
/* A packet for `frame` arrived at `now_ms`. Returns 0 kept, 1 a duplicate, 2 too
 * late to play. */
int  oc_jb_put(oc_jitter *j, uint32_t frame, const uint8_t *data, size_t len, int64_t now_ms);
/* The next 20 ms: OC_JB_PACKET or OC_JB_FEC with the packet to decode (for FEC,
 * the packet after the lost one), OC_JB_PLC to conceal, OC_JB_NONE for nothing.
 * `quiet` says the last frame played was silence, which is when the buffer may
 * skip a frame to give back delay. */
int  oc_jb_get(oc_jitter *j, int quiet, const uint8_t **data, size_t *len);
/* The target, in ms. */
int  oc_jb_target_ms(const oc_jitter *j);

#endif
