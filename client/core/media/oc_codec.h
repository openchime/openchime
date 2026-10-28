/* Thin wrappers over libvpx (VP9) and libopus for video messages (ARCH-110,
 * docs/VIDEO-MESSAGES.md §4). The settings live here and nowhere else, so the
 * recorder and the player never touch a library API directly. */
#ifndef OC_CODEC_H
#define OC_CODEC_H

#include "oc_media.h"

#include <stddef.h>
#include <stdint.h>

/* ---- VP9 ------------------------------------------------------------------------ */

typedef struct oc_vp9enc oc_vp9enc;

/* Real-time VP9 at `fps`, CBR at a bitrate chosen for the resolution, a keyframe
 * at least every two seconds. NULL on failure. */
oc_vp9enc *oc_vp9enc_open(int width, int height, int fps);
/* The same, tuned for a screen (VP9E_CONTENT_SCREEN): sharp text and flat areas
 * over motion, for a recorded screen or window (REQ-162). */
oc_vp9enc *oc_vp9enc_open_screen(int width, int height, int fps);
/* For sharing a screen in a call (REQ-161, ARCH-87): screen-tuned, at `kbps`,
 * with frames as often or as seldom as the screen changes -- each frame's
 * duration is the time since the one before -- and keyframes only when asked
 * for, since the call's loss recovery decides when one is needed. */
oc_vp9enc *oc_vp9enc_open_share(int width, int height, unsigned kbps);
/* Change the bitrate of an open encoder, from the next frame. 0, or -1. */
int        oc_vp9enc_set_bitrate(oc_vp9enc *e, unsigned kbps);
/* The bitrate (kbit/s) oc_vp9enc_open picks for a resolution. */
unsigned   oc_vp9enc_bitrate_kbps(int width, int height);

/* One encoded frame, valid until the next call on the encoder. */
typedef struct { const uint8_t *data; size_t len; int64_t pts_us; int keyframe; } oc_packet;

/* Encode `f` (NULL flushes). `emit` is called for each packet the encoder
 * produces. Returns 0, or -1 on an encoder error. */
int  oc_vp9enc_encode(oc_vp9enc *e, const oc_frame *f, int force_keyframe,
                      void (*emit)(void *ctx, const oc_packet *p), void *ctx);
void oc_vp9enc_close(oc_vp9enc *e);

typedef struct oc_vp9dec oc_vp9dec;

oc_vp9dec *oc_vp9dec_open(void);
/* Decode one sample. On success `out` points at the decoder's own frame
 * (valid until the next call) and 0 is returned; 1 means no frame this time;
 * -1 an error. `out->pts_us` is set to `pts_us`. */
int  oc_vp9dec_decode(oc_vp9dec *d, const uint8_t *data, size_t len, int64_t pts_us, oc_frame *out);
void oc_vp9dec_close(oc_vp9dec *d);

/* ---- Opus ----------------------------------------------------------------------- */

#define OC_OPUS_RATE        48000
#define OC_OPUS_FRAME       960          /* 20 ms at 48 kHz */
#define OC_OPUS_MAX_PACKET  1500

typedef struct oc_opusenc oc_opusenc;

/* Mono, 48 kHz, VOIP, 48 kbit/s VBR. */
oc_opusenc *oc_opusenc_open(void);
/* The encoder's look-ahead in samples; the MP4's PreSkip must cover it. */
int  oc_opusenc_lookahead(oc_opusenc *e);
/* Encode exactly OC_OPUS_FRAME samples. Returns the packet length or -1. */
int  oc_opusenc_encode(oc_opusenc *e, const int16_t *pcm, uint8_t *out, size_t cap);
void oc_opusenc_close(oc_opusenc *e);

typedef struct oc_opusdec oc_opusdec;

oc_opusdec *oc_opusdec_open(int channels);
/* Decode one packet into mono PCM (a stereo stream is downmixed). Returns the
 * number of samples written (at most `cap`) or -1. */
int  oc_opusdec_decode(oc_opusdec *d, const uint8_t *data, size_t len, int16_t *pcm, int cap);
void oc_opusdec_close(oc_opusdec *d);

/* ---- Opus for calls (docs/CALLS.md §3) --------------------------------------- */

#define OC_VOICE_RATE   16000
#define OC_VOICE_FRAME  320              /* 20 ms at 16 kHz */

/* Mono, 16 kHz, VOIP, `kbps` VBR, with in-band FEC and DTX. The encoder frames
 * are OC_VOICE_FRAME samples. */
oc_opusenc *oc_opusenc_open_voice(int kbps);
/* Tell the encoder how much loss the network shows (0-100), which is how much
 * FEC it puts in each packet. */
void oc_opusenc_set_loss(oc_opusenc *e, int percent);
/* Encode OC_VOICE_FRAME samples. Returns the packet length, or -1. With DTX a
 * packet of 2 bytes or fewer is silence the encoder need not have sent: the
 * caller does not send it. */
int  oc_opusenc_encode_voice(oc_opusenc *e, const int16_t *pcm, uint8_t *out, size_t cap);

/* A mono decoder at `rate`. */
oc_opusdec *oc_opusdec_open_rate(int rate);
/* One frame of `frame` samples: `data` decoded; or, with `fec`, the frame
 * BEFORE `data` rebuilt from the redundancy `data` carries; or, with `data`
 * NULL, a frame concealed from what came before (PLC). Returns samples or -1. */
int  oc_opusdec_decode_frame(oc_opusdec *d, const uint8_t *data, size_t len, int fec,
                             int16_t *pcm, int frame);

#endif
