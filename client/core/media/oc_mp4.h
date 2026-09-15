/* The restricted MP4 a video message is stored in (ARCH-110,
 * docs/VIDEO-MESSAGES.md §5): one VP9 track and one Opus track, progressive,
 * `moov` before `mdat`. The writer produces exactly that layout and the reader
 * accepts exactly that layout — which is what makes an in-tree implementation
 * reasonable, and what lets the reader reject everything else cleanly. */
#ifndef OC_MP4_H
#define OC_MP4_H

#include <stddef.h>
#include <stdint.h>

#define OC_MP4_VIDEO_TIMESCALE 90000u
#define OC_MP4_AUDIO_TIMESCALE 48000u
#define OC_MP4_OPUS_PRESKIP    312u
/* Bounds the reader enforces, generous against five minutes at 30 fps and 50
 * Opus packets a second, so a hostile file cannot make it allocate much. */
#define OC_MP4_MAX_SAMPLES     200000u
#define OC_MP4_MAX_FILE        (256u * 1024u * 1024u)

/* ---- writing --------------------------------------------------------------- */

typedef struct oc_mp4_writer oc_mp4_writer;

/* Start a recording of `width`×`height` video. Everything is held in memory —
 * a client writes no files (ARCH-88) — and OC_MP4_MAX_FILE bounds it. NULL on
 * failure. */
oc_mp4_writer *oc_mp4_writer_open(int width, int height);

/* Sample bytes written so far: roughly the file's size, less a small moov. */
uint64_t oc_mp4_writer_bytes(const oc_mp4_writer *w);

/* Append one encoded VP9 frame. `pts_us` must not go backwards, and the first
 * frame must be a keyframe. */
int oc_mp4_write_video(oc_mp4_writer *w, const uint8_t *data, size_t len,
                       int64_t pts_us, int keyframe);
/* Append one Opus packet of `samples` samples at 48 kHz (960 for 20 ms). */
int oc_mp4_write_audio(oc_mp4_writer *w, const uint8_t *data, size_t len, unsigned samples);

/* Assemble the file (ftyp, moov, mdat) into `*out` (malloc'd; the caller frees
 * it) and free `w`. `*duration_ms` receives the longer track's duration.
 * Returns 0, or -1 with `w` freed and nothing allocated. */
int  oc_mp4_writer_finish(oc_mp4_writer *w, uint8_t **out, size_t *len, uint32_t *duration_ms);
/* Stop without a file. */
void oc_mp4_writer_abort(oc_mp4_writer *w);

/* ---- reading --------------------------------------------------------------- */

typedef struct {
    uint64_t offset;         /* into the file */
    uint32_t size;
    uint64_t dts;            /* in the track's timescale */
    uint32_t duration;
    uint8_t  sync;           /* keyframe (always 1 for audio) */
} oc_mp4_sample;

typedef struct {
    int            present;
    uint32_t       timescale;
    oc_mp4_sample *samples;
    uint32_t       n_samples;
    uint64_t       duration;         /* in timescale */
} oc_mp4_track;

typedef struct oc_mp4_info {
    oc_mp4_track video, audio;
    int      width, height;
    uint8_t  vp9_profile, vp9_bit_depth;
    uint8_t  opus_channels;
    uint16_t opus_preskip;
    uint32_t duration_ms;
} oc_mp4_info;

/* Parse a whole file held in memory. Returns 0 and fills `info` (free with
 * oc_mp4_info_free), or a negative value for anything that is not exactly this
 * profile. Never reads outside `data[0..len)`. */
int  oc_mp4_parse(const uint8_t *data, size_t len, oc_mp4_info *info);
void oc_mp4_info_free(oc_mp4_info *info);

/* The index of the keyframe at or before `dts` in the video track, or -1. */
int oc_mp4_keyframe_before(const oc_mp4_info *info, uint64_t dts);

#endif
