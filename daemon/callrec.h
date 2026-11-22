/*
 * OpenChime — the Recorder (CALLS.md §8, ARCH-113).
 *
 * A call stays end to end encrypted unless a participant records it. Then the
 * daemon runs a Recorder, which joins the call as a participant of its own: on
 * the roster marked RECORDER, with a slot and an X25519 key pair made for this
 * recording and held in memory only. Its join and its leave are epochs as
 * anyone's are, so the devices seal their keys to it only for the epochs it is
 * in, and it can open nothing said before it came or after it went.
 *
 * The net loop drives it on its own thread: the roster as it changes, the keys
 * sealed to it (a CALL_KEY entry naming it), every packet of the call (the
 * relay's record hook), and the clock. The Recorder opens each packet with the
 * sender's key, reassembles cameras and the shared screen as a viewer does --
 * asking the sender again for a fragment or a keyframe, its own control
 * packets sealed under a key it gives the others in turn -- and writes what it
 * hears to its spool, still encoded: each person's audio frames, each camera's
 * frames, the shared screen's frames, and the timeline. Processing (callrec
 * render) makes the recording from the spool later.
 *
 * The spool is a directory of files, each a run of records, numbers big-endian:
 *
 *   meta               "OCREC1" ‖ call_id(u64) ‖ recording_id(u64) ‖ started_ms(u64)
 *   audio.<user>       at_ms(u64) ‖ frame(u32) ‖ len(u16) ‖ opus
 *   camera.<user>      at_ms(u64) ‖ aframe(u32) ‖ key(u8) ‖ w(u16) ‖ h(u16) ‖ len(u32) ‖ vp9
 *   share              at_ms(u64) ‖ sharer(u64) ‖ key(u8) ‖ w(u16) ‖ h(u16) ‖ len(u32) ‖ vp9
 *   timeline           at_ms(u64) ‖ kind(u8) ‖ user(u64)        kind OC_CALLREC_TL_*
 *
 * where <user> is the user id in decimal and at_ms is when the Recorder had the
 * frame, on the daemon's wall clock.
 */
#ifndef OC_CALLREC_H
#define OC_CALLREC_H

#include <stddef.h>
#include <stdint.h>

#include "protocol.h"

typedef struct oc_callrec oc_callrec;

/* A participant as the Recorder sees the roster: who, at which slot, with
 * which device key -- itself among them. */
typedef struct { uint64_t user; uint8_t slot; uint8_t key[OC_CALL_DEVICE_KEY_LEN]; } oc_callrec_part;

/* What the Recorder says, through the net loop: a key it seals to one
 * participant, sent to that participant as CALL_KEY_FOR from the Recorder; and
 * a packet it puts into the call (oc_relay_inject). */
typedef struct {
    void (*key_for)(void *ctx, uint64_t call_id, uint32_t epoch, uint64_t recipient,
                    const uint8_t sealed[OC_CALL_SEALED_LEN]);
    void (*inject)(void *ctx, uint64_t call_id, uint16_t seq, uint8_t stream, const uint8_t *ct, size_t len);
    void *ctx;
} oc_callrec_io;

/* The timeline's kinds, as the spool keeps them. */
enum { OC_CALLREC_TL_JOIN = 1, OC_CALLREC_TL_LEAVE = 2, OC_CALLREC_TL_SHARE_ON = 3, OC_CALLREC_TL_SHARE_OFF = 4,
       OC_CALLREC_TL_CAMERA_ON = 5, OC_CALLREC_TL_CAMERA_OFF = 6 };

/* What a Recorder did: packets handed to it; opened; not opened, under a key
 * of an epoch before it joined (`before`, which it is never given) or of one
 * since (`unopened`: no key for the KID, a key of another sender, failed
 * authentication, a replay); frames written to the spool; control packets it
 * sent. */
typedef struct {
    uint64_t call_id;
    uint32_t packets, opened, before, unopened;
    uint32_t audio_frames, camera_frames, share_frames;
    uint32_t nacks, plis;
    uint32_t first_epoch;      /* the epoch it joined at */
} oc_callrec_stats;

/* A Recorder for call `call_id`, recording `recording_id`, spooling into
 * `spool_dir` (made here, 0700). Its key pair is made now. NULL on failure. */
oc_callrec *oc_callrec_start(uint64_t call_id, uint64_t recording_id, const char *spool_dir, uint64_t now_ms);
/* Its public key, for the roster, and its user id there (OC_CALL_RECORDER_ID). */
const uint8_t *oc_callrec_public_key(const oc_callrec *r);

/* The roster now, in `epoch`, the Recorder at `my_slot` among `parts`. A new
 * epoch: keys older than the one before are wiped, and the Recorder makes its
 * own key for it and seals a copy to every other participant through io. */
void oc_callrec_roster(oc_callrec *r, uint32_t epoch, uint8_t my_slot,
                       const oc_callrec_part *parts, int n, const oc_callrec_io *io);
/* A media key `sender` sealed to the Recorder for `epoch` (CALL_KEY). Opened
 * with the sender's device key from the roster; installed for this epoch or the
 * one before only, once. */
void oc_callrec_key(oc_callrec *r, uint64_t sender, uint32_t epoch, const uint8_t *sealed, size_t len);
/* Who is sharing a screen now, 0 for nobody: a new sharer is a new share. */
void oc_callrec_sharer(oc_callrec *r, uint64_t sharer, uint64_t now_ms);
/* A camera turned on or off: a timeline entry, and a fresh reassembly. */
void oc_callrec_camera(oc_callrec *r, uint64_t user, int on, uint64_t now_ms);
/* A participant came or went: a timeline entry. */
void oc_callrec_presence(oc_callrec *r, uint64_t user, int joined, uint64_t now_ms);
/* One packet of the call, as the relay hands it over. */
void oc_callrec_packet(oc_callrec *r, uint64_t sender, uint16_t seq, uint8_t stream,
                       const uint8_t *ct, size_t len, uint64_t now_ms, const oc_callrec_io *io);
/* Time passing: what is missing is asked for again, a frame given up asks for
 * a keyframe (the viewer's rules, oc_share.h). */
void oc_callrec_tick(oc_callrec *r, uint64_t now_ms, const oc_callrec_io *io);
void oc_callrec_get_stats(const oc_callrec *r, oc_callrec_stats *out);
/* Stop: the spool closed and kept, every key wiped, the Recorder freed. Its
 * stats are kept for oc_callrec_last_stats. */
void oc_callrec_stop(oc_callrec *r);

/* The stats of the last Recorder stopped, from any thread: for the tests. */
void oc_callrec_last_stats(oc_callrec_stats *out);

#endif /* OC_CALLREC_H */
