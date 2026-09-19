/* The call's media engine (docs/CALLS.md §3, AUDIO.md §2-§4): the microphone
 * through the echo canceller and speexdsp's preprocessor, Opus at 16 kHz with
 * FEC and DTX, SFrame (shared/e2e_sframe.h) under keys the core hands it, the
 * relay's UDP socket, a jitter buffer and decoder per sender, and the mixer that
 * plays them. It plugs into the core through oc_call_media (callsig.h): the core
 * says when a call starts, who is in it and which keys to use; the frontend says
 * mute, push to talk, volumes and devices, and reads what is going on. */
#ifndef OC_CALL_ENGINE_H
#define OC_CALL_ENGINE_H

#include <stddef.h>
#include <stdint.h>

#include "callsig.h"
#include "oc_media.h"

typedef struct oc_call_engine oc_call_engine;

typedef struct {
    const char *mic_id, *speaker_id;   /* NULL or "" for the defaults */
    int         noise_suppression;     /* noise suppression and automatic gain, 1 on */
    /* Tests: in place of the devices, a source the microphone reads and a sink
     * the speaker writes, each called every 20 ms of real time with
     * OC_VOICE_FRAME samples at 16 kHz. */
    void (*source)(void *ctx, int16_t *pcm, int n);
    void (*sink)(void *ctx, const int16_t *pcm, int n);
    void  *io_ctx;
} oc_call_engine_opts;

oc_call_engine *oc_call_engine_new(const oc_call_engine_opts *o);
/* Stops whatever is running. Remove it from the client first
 * (oc_client_set_call_media(c, NULL, NULL)). */
void oc_call_engine_free(oc_call_engine *e);
/* The seam the core drives; the context is the engine. */
const oc_call_media *oc_call_engine_media(void);

/* Muted sends no audio -- only, once a second, a packet saying it is muted,
 * encrypted like the audio -- and keep-alives. Push to talk held sends while
 * muted. */
void  oc_call_engine_set_mute(oc_call_engine *e, int muted);
int   oc_call_engine_muted(const oc_call_engine *e);
void  oc_call_engine_set_ptt(oc_call_engine *e, int held);
/* One person's volume, 0 (silent) to 2 (twice as loud); 1 is as sent. Kept for
 * that person across calls for the life of the engine. */
void  oc_call_engine_set_volume(oc_call_engine *e, uint64_t user_id, float gain);
float oc_call_engine_volume(const oc_call_engine *e, uint64_t user_id);
/* The devices: used from the next call, and at once in this one. */
void  oc_call_engine_set_devices(oc_call_engine *e, const char *mic_id, const char *speaker_id);
void  oc_call_engine_set_noise_suppression(oc_call_engine *e, int on);

/* Share a screen or window (REQ-161): `device_id` from oc_capture_list_screens,
 * fitted inside max_w×max_h (0: 1920×1080). The source opens now; frames go out
 * once the daemon names this device the sharer, so the frontend also sends
 * oc_client_call_share(c, channel, 1). Someone else starting to share stops this
 * one (share_taken in the stats). 0, or -1 when not in a call. */
int   oc_call_engine_share_start(oc_call_engine *e, const char *device_id, int max_w, int max_h);
/* Stop sharing; the frontend also sends oc_client_call_share(c, channel, 0). */
void  oc_call_engine_share_stop(oc_call_engine *e);
/* The newest frame of someone else's share, copied into `dst` (allocated or
 * resized here; oc_frame_free it) when it is newer than `*seq`: 1 with `*seq`
 * and `*frame_no` (the sharer's frame number) updated; 0 if nothing new; -1 once
 * the share has gone, so the frontend stops showing it. */
int   oc_call_engine_share_frame(oc_call_engine *e, oc_frame *dst, uint32_t *seq, uint32_t *frame_no);

/* What is happening, for the call view and the test harness. */
typedef struct {
    uint64_t user_id;
    uint8_t  slot;
    int      speaking;          /* heard in the last 300 ms */
    int      muted;             /* has said, inside the encryption, that it is muted */
    int      level;             /* peak of the last frame played, 0-32767 */
    int      keyed;             /* a key for this person's current epoch is held */
    uint32_t packets, lost, late, fec, plc, undecryptable;
} oc_call_peer_stats;

typedef struct {
    int      active;            /* in a call */
    uint64_t self_user;         /* who this device is in it, and its slot */
    uint8_t  slot;
    int      mic_error;         /* OC_AUDIO_* opening the microphone, 0 fine */
    int      speaker_error;
    int      muted, ptt;
    int      speaking;          /* this device's voice is going out */
    int      mic_level;         /* peak of the last frame captured, 0-32767 */
    uint32_t sent, keepalives;
    uint32_t epoch;             /* of the key being sent with, 0 = none yet */
    int      loss_pct;          /* the loss told to the encoder */
    int      target_ms;         /* the largest jitter-buffer target */
    int      n_peers;
    oc_call_peer_stats peers[32];

    /* Screen sharing. */
    uint64_t sharer;            /* who the daemon says is sharing, 0 nobody */
    int      share_state;       /* this device: 0 not sharing, 1 waiting for the daemon, 2 sharing */
    int      share_taken;       /* someone took over from this device */
    int      share_error;       /* OC_CAP_* from the source, 0 fine */
    int      share_width, share_height, share_fps, share_kbps;
    uint32_t share_frames, share_keyframes, share_resent, share_nacks, share_plis, share_reports;
    int      view_width, view_height;   /* someone else's share, as decoded */
    uint32_t view_frames, view_frame_no, view_nacks, view_plis, view_skipped, view_errors;
} oc_call_stats;

void oc_call_engine_stats(oc_call_engine *e, oc_call_stats *out);

#endif
