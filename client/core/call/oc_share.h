/* Screen sharing's packet logic (REQ-161, ARCH-86/87, docs/VIDEO.md §4-§6): what
 * the call engine does with a share's frames between the encoder and SFrame, and
 * between SFrame and the decoder. No sockets, no threads, no codec -- the engine
 * owns those -- so every rule here can be driven by a test with a simulated
 * network.
 *
 * A call packet's plaintext starts with its type (OC_CALLPKT_*). A share's frame
 * is split into fragments of at most OC_SHARE_FRAG bytes, each its own packet:
 *
 *   2 ‖ frame(u32) ‖ frag(u16) ‖ nfrags(u16) ‖ flags(u8) ‖ width(u16) ‖ height(u16) ‖ bytes
 *
 * and a viewer answers the sharer with control packets, relayed to everyone and
 * acted on only by the one they name:
 *
 *   3 ‖ target(u64) ‖ kind(u8) ‖ body
 *
 * NACK names a frame and the fragments missing from it (none: the whole frame);
 * PLI asks for a keyframe; REPORT says what arrived. The sharer keeps a second of
 * what it sent to answer NACKs, sends a keyframe for a PLI at most once a second
 * and every ten seconds regardless, and sets its bitrate from the reports: halved
 * when any viewer loses more than 5%, grown by a tenth a second otherwise, within
 * a floor and the built-in ceiling. */
#ifndef OC_SHARE_H
#define OC_SHARE_H

#include <stddef.h>
#include <stdint.h>

/* The plaintext's first byte. */
enum {
    OC_CALLPKT_AUDIO   = 0,   /* frame(u32) ‖ opus */
    OC_CALLPKT_STATE   = 1,   /* flags(u8): bit 0 muted */
    OC_CALLPKT_VIDEO   = 2,   /* a fragment of a shared screen's frame */
    OC_CALLPKT_CONTROL = 3,   /* a viewer to the sharer */
};

enum { OC_SHARE_NACK = 1, OC_SHARE_PLI = 2, OC_SHARE_REPORT = 3 };

#define OC_SHARE_FRAG        1100u   /* bytes of a frame per packet */
#define OC_SHARE_VIDEO_HDR   14u
#define OC_SHARE_MAX_FRAGS   1024u   /* a frame of at most 1.1 MB */
#define OC_SHARE_MAX_PT      (OC_SHARE_VIDEO_HDR + OC_SHARE_FRAG)
#define OC_SHARE_CTL_MAX     (1 + 8 + 1 + 4 + 2 + 2 * 64)
#define OC_SHARE_MAX_DIM     4096

#define OC_SHARE_KBPS_MAX    2500    /* the ceiling on a share's bitrate: what the relay carries */
#define OC_SHARE_KBPS_MIN    150
#define OC_SHARE_KBPS_START  1200
#define OC_SHARE_FPS         15      /* while the screen changes */
#define OC_SHARE_STILL_MS    500     /* while it does not: a frame this often (2 fps) */
#define OC_SHARE_HISTORY_MS  1000
#define OC_SHARE_KEY_MIN_MS  1000    /* keyframes on request, at most this often */
#define OC_SHARE_KEY_MAX_MS  10000   /* and at least this often */
#define OC_SHARE_NACK_MS     60      /* a fragment missing this long is asked for again */
#define OC_SHARE_GIVEUP_MS   300     /* a frame still incomplete this long is given up */
#define OC_SHARE_PLI_MS      500     /* a viewer's keyframe requests, at most this often */
#define OC_SHARE_JOIN_MS     250     /* a new viewer waits this long for a keyframe before asking */
#define OC_SHARE_REPORT_MS   500

/* Where a packet goes: the engine seals and sends it. */
typedef void (*oc_share_emit)(void *ctx, const uint8_t *pt, size_t len);

/* ---- the sharer -------------------------------------------------------------- */

#define OC_SHARE_HISTORY 1024   /* fragments kept for NACKs */

typedef struct {
    uint32_t frame;
    uint16_t frag;
    uint16_t len;              /* 0 = empty */
    int64_t  at_ms;
    uint8_t  pt[OC_SHARE_MAX_PT];
} oc_share_sent;

typedef struct { uint64_t user; int loss_permille; int64_t at_ms; } oc_share_viewer;

typedef struct {
    uint64_t        self;          /* the sharer's user id: control naming anyone else is ignored */
    uint32_t        next_frame;
    oc_share_sent  *hist;          /* OC_SHARE_HISTORY, heap */
    int             hist_at;
    int             kbps;
    int64_t         rate_at_ms;    /* when the bitrate last moved */
    int64_t         cut_at_ms;     /* when it was last halved */
    int64_t         key_at_ms;     /* the last keyframe sent */
    int             have_key;      /* ...if there has been one in this share */
    int             key_wanted;
    oc_share_viewer viewers[32];
    uint32_t        frames, keyframes, frags, resent, nacks, plis, reports;
    uint64_t        bytes;
} oc_share_tx;

int  oc_share_tx_init(oc_share_tx *t, uint64_t self);
void oc_share_tx_free(oc_share_tx *t);
/* Frame numbers carry on across shares, so a viewer can tell an old share's
 * stragglers from a new one's; a new share starts with a keyframe. */
void oc_share_tx_restart(oc_share_tx *t, int64_t now_ms);
/* Split one encoded frame into packets and emit them, keeping them for NACKs.
 * 0, or -1 if the frame is too large to send. */
int  oc_share_tx_frame(oc_share_tx *t, const uint8_t *data, size_t len, int keyframe,
                       int width, int height, int64_t now_ms, oc_share_emit emit, void *ctx);
/* A control packet (the plaintext after its type byte) from `from`: resends for a
 * NACK, a keyframe wanted for a PLI, the rate for a REPORT. */
void oc_share_tx_control(oc_share_tx *t, uint64_t from, const uint8_t *body, size_t len,
                         int64_t now_ms, oc_share_emit emit, void *ctx);
/* Whether the next frame should be a keyframe: asked for and a second since the
 * last, or ten seconds since the last. */
int  oc_share_tx_want_keyframe(oc_share_tx *t, int64_t now_ms);
/* The bitrate now, kbps; grows a tenth a second while nobody reports loss. */
int  oc_share_tx_kbps(oc_share_tx *t, int64_t now_ms);
/* A viewer who left no longer holds the rate down. */
void oc_share_tx_forget(oc_share_tx *t, uint64_t user);

/* ---- a viewer ------------------------------------------------------------------ */

#define OC_SHARE_SLOTS 32

typedef struct {
    int      used;
    uint32_t frame;
    uint16_t nfrags, have;       /* nfrags 0: known missing, no fragment seen yet */
    uint16_t last_len;
    uint8_t  keyframe;
    uint16_t width, height;
    uint8_t  got[OC_SHARE_MAX_FRAGS / 8];
    uint8_t *buf;                /* nfrags * OC_SHARE_FRAG, heap */
    int64_t  first_ms, nacked_ms;
    int      nacks;
} oc_share_slot;

typedef struct {
    uint64_t      sharer;        /* whose frames these are; the engine resets on a change */
    int           started;       /* a keyframe has been had */
    int           need_key;
    uint32_t      next_frame;    /* the next frame to decode */
    uint32_t      top_frame;
    int           have_top;
    int64_t       pli_ms;        /* the last PLI asked */
    int           pli_any;
    int64_t       born_ms;       /* the first poll; -1 before it */
    int           handed;        /* the slot oc_share_rx_next last returned, -1 none */
    oc_share_slot slots[OC_SHARE_SLOTS];
    uint32_t      frags, dups, frames, skipped, nacks, plis;
} oc_share_rx;

void oc_share_rx_init(oc_share_rx *r, uint64_t sharer);
void oc_share_rx_free(oc_share_rx *r);
/* A video fragment (the plaintext after its type byte). 0 kept, -1 malformed or
 * not wanted. */
int  oc_share_rx_put(oc_share_rx *r, const uint8_t *body, size_t len, int64_t now_ms);
/* The next frame to decode, in order -- or, when frames were lost or it is
 * starting, the oldest complete keyframe to come: 1 with its bytes (valid until
 * the next call), 0 if none. */
int  oc_share_rx_next(oc_share_rx *r, const uint8_t **data, size_t *len, int *keyframe,
                      int *width, int *height, uint32_t *frame);
/* Ask for what is missing: NACKs for fragments missing past OC_SHARE_NACK_MS, and
 * a PLI while there is no keyframe to start from or a frame was given up. */
void oc_share_rx_poll(oc_share_rx *r, int64_t now_ms, oc_share_emit emit, void *ctx);
/* The decoder failed: nothing more can be decoded until a keyframe. */
void oc_share_rx_need_key(oc_share_rx *r);

/* Control packets, whole (type byte first); return their length. */
size_t oc_share_ctl_pli(uint8_t *out, uint64_t target);
size_t oc_share_ctl_report(uint8_t *out, uint64_t target, int loss_permille, uint32_t kbps);
size_t oc_share_ctl_nack(uint8_t *out, uint64_t target, uint32_t frame, const uint16_t *frags, int n);

/* ---- what to send -------------------------------------------------------------- */

/* A frame's fingerprint, to skip sending a screen that has not changed. */
uint64_t oc_share_frame_hash(const uint8_t *const plane[3], const int stride[3], int width, int height);

/* The size to encode at: the source as it is, or fitted into 1280×720 while the
 * bitrate is low (below 600 kbps, back above 1000). `small` carries the state.
 * Even dimensions. */
void oc_share_pick_size(int src_w, int src_h, int kbps, int *small, int *w, int *h);

#endif
