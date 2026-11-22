/*
 * OpenChime — making a call's recording from its spool (CALLS.md §8).
 *
 * The Recorder kept what it heard still encoded (callrec.h): each person's
 * Opus frames, each camera's and the shared screen's VP9 frames, and the
 * timeline, each stamped with when it was had. Processing decodes them and
 * makes one recording, as a video message is (ARCH-110): VP9 at 1280×720,
 * 15 fps, and one mono Opus track at 48 kHz, in the restricted MP4 the clients
 * play, written to a file as it goes (oc_mp4_writer_open_file).
 *
 *   - the audio: everyone's voice, each put back on the call's clock by its
 *     frame numbers, a lost frame concealed, mixed and softly limited;
 *   - the picture: the call view's speaker layout -- whoever is speaking large,
 *     staying until someone else speaks, everyone else in a strip of tiles
 *     below; a shared screen large while it is shared, everyone in the strip;
 *     each person their camera's picture while it is on, else a tile of their
 *     own colour.
 *
 * The codecs are behind a small interface, as the export's are (export_render.h):
 * the render and the worker are in every daemon and `make test` drives them with
 * a stub; only a daemon built with CALLREC=1 carries the real ones
 * (callrec_engine.c: libvpx and libopus).
 */
#ifndef OC_CALLREC_RENDER_H
#define OC_CALLREC_RENDER_H

#include <stddef.h>
#include <stdint.h>

#include "oc_frame.h"

#define OC_CALLREC_WIDTH   1280
#define OC_CALLREC_HEIGHT  720
#define OC_CALLREC_FPS     15
#define OC_CALLREC_KBPS    450      /* what keeps an hour within the profile (oc_mp4.h) */
#define OC_CALLREC_RATE    48000    /* the mix's rate, and Opus's */
#define OC_CALLREC_FRAME   960      /* 20 ms at 48 kHz */

typedef struct oc_callrec_codecs {
    /* Names the encoders and their settings together. */
    const char *version;
    /* A decoder of one person's Opus at OC_CALLREC_RATE: `pkt` NULL conceals a
     * lost frame. Writes OC_CALLREC_FRAME samples; 0, or -1. */
    void *(*audio_dec_open)(void);
    int   (*audio_decode)(void *d, const uint8_t *pkt, size_t len, int16_t *pcm);
    void  (*audio_dec_close)(void *d);
    /* A VP9 decoder: 0 and `out` the decoder's own frame (until the next call),
     * 1 no frame this time, -1 an error. */
    void *(*video_dec_open)(void);
    int   (*video_decode)(void *d, const uint8_t *data, size_t len, oc_frame *out);
    void  (*video_dec_close)(void *d);
    /* The recording's encoders: VP9 at OC_CALLREC_WIDTH×HEIGHT, OC_CALLREC_FPS,
     * OC_CALLREC_KBPS; Opus, mono, OC_CALLREC_RATE, OC_CALLREC_FRAME a packet. A
     * packet is handed to `emit`; 0, or -1. */
    void *(*video_enc_open)(void);
    int   (*video_encode)(void *e, const oc_frame *f, int force_key,
                          void (*emit)(void *ctx, const uint8_t *data, size_t len, int keyframe), void *ctx);
    void  (*video_enc_close)(void *e);
    void *(*audio_enc_open)(void);
    int   (*audio_encode)(void *e, const int16_t *pcm, uint8_t *out, size_t cap);
    void  (*audio_enc_close)(void *e);
} oc_callrec_codecs;

typedef enum {
    OC_CALLREC_OK = 0,
    OC_CALLREC_EMPTY,          /* nothing in the spool to make a recording of */
    OC_CALLREC_CANCELLED,      /* `*cancel` was set: the daemon is stopping */
    OC_CALLREC_FAILED,         /* the spool is damaged, or decoding, encoding or writing failed */
} oc_callrec_status;

/* Where the recording's picture finds people's avatars (CALLS.md §8): `load`
 * gives the bytes stored under `key` (heap, freed by the caller), 0, or -1.
 * NULL: no avatars, each person their initials on their colour. */
typedef struct {
    int  (*load)(void *ctx, const char *key, uint8_t **data, size_t *len);
    void *ctx;
} oc_callrec_assets;

/* Make the recording of the spool in `spool_dir` at `out_path` with `c`. On
 * OC_CALLREC_OK the file is there and `*bytes`, `*duration_ms` describe it; on
 * anything else no file is left. `cancel` (if non-NULL) is checked between
 * frames. */
oc_callrec_status oc_callrec_render(const oc_callrec_codecs *c, const oc_callrec_assets *assets,
                                    const char *spool_dir, const char *out_path,
                                    const volatile int *cancel, uint64_t *bytes, uint32_t *duration_ms,
                                    char *err, size_t errcap);

/* How a person is drawn without a camera (CALLS.md §8): in their box, a circle
 * holding their avatar -- or their initials on their own colour -- with their
 * name under it; the speaker in the large area, the rest in the strip; a ring
 * round the box of whoever is speaking. The layout of a box, `tile` -1 the
 * large one and 0.. the strip's: */
typedef struct { int x, y, w, h, cx, cy, r, name_y, name_px; } oc_callrec_layout;
void oc_callrec_face_layout(int tile, oc_callrec_layout *out);
#define OC_CALLREC_RING_Y 95       /* the speaking ring: the clients' accent blue */
#define OC_CALLREC_RING_U 192
#define OC_CALLREC_RING_V 75
#define OC_CALLREC_RING_W 6
/* One frame as it draws people with no camera and no share, for a test:
 * faces[0] large, the rest in the strip, into an OC_CALLREC_WIDTH ×
 * OC_CALLREC_HEIGHT I420 buffer. An avatar is a PNG or JPEG's bytes. */
typedef struct { uint64_t user; const char *name; const uint8_t *avatar; size_t avatar_len; int speaking; } oc_callrec_face;
int oc_callrec_compose(uint8_t *i420, const oc_callrec_face *faces, int n);
/* The colour a person without an avatar is drawn in. */
void oc_callrec_person_colour(uint64_t user, uint8_t *y, uint8_t *u, uint8_t *v);

/* A stand-in for the codecs, for tests (callrec_render.c). An Opus "packet" is
 * its frame's sample value, one byte, decoded as a frame of that value; a VP9
 * "frame" is its picture's luma, one byte, decoded as a 64×48 picture of it,
 * and the keyframe byte set. The encoders write what a test reads back: a video
 * packet is the recording's picture sampled -- the large area's luma at its
 * centre, then each strip tile's, OC_CALLREC_STUB_TILES of them, 0 where there
 * is none -- and an audio packet is the mean of the frame's samples' absolute
 * values, a 16-bit number. */
#define OC_CALLREC_STUB_TILES 6
extern const oc_callrec_codecs oc_callrec_stub_codecs;

/* The real codecs (callrec_engine.c), or NULL in a daemon built without them. */
const oc_callrec_codecs *oc_callrec_codecs_builtin(void);
/* The real codecs themselves, wherever callrec_engine.c is linked: the daemon
 * built with CALLREC=1, and the test binary's media suite. */
const oc_callrec_codecs *oc_callrec_codecs_real(void);

#endif
