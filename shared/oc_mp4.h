/* The restricted MP4 a video message is stored in (ARCH-110,
 * docs/VIDEO-MESSAGES.md §5): one VP9 track and one Opus track, progressive,
 * `moov` before `mdat` — or, for read-aloud (ARCH-111, docs/READ-ALOUD.md §4),
 * the same file with the Opus track alone. The writer produces exactly those
 * layouts and the reader accepts exactly those — which is what makes an in-tree
 * implementation reasonable, and what lets the reader reject everything else
 * cleanly. Shared: clients write and play video messages, the daemon writes
 * read-aloud renders. */
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
/* Start an audio-only file: an Opus track and no video. `input_rate` is the
 * sample rate the audio was produced at (recorded in the file for information;
 * Opus itself always decodes at 48 kHz). */
oc_mp4_writer *oc_mp4_writer_open_audio(unsigned input_rate);

/* Sample bytes written so far: roughly the file's size, less a small moov. */
uint64_t oc_mp4_writer_bytes(const oc_mp4_writer *w);

/* Append one encoded VP9 frame. `pts_us` must not go backwards, and the first
 * frame must be a keyframe. Refused by an audio-only writer. */
int oc_mp4_write_video(oc_mp4_writer *w, const uint8_t *data, size_t len,
                       int64_t pts_us, int keyframe);
/* Append one Opus packet of `samples` samples at 48 kHz (960 for 20 ms). */
int oc_mp4_write_audio(oc_mp4_writer *w, const uint8_t *data, size_t len, unsigned samples);

/* Assemble the file (ftyp, moov, mdat) into `*out` (malloc'd; the caller frees
 * it) and free `w`. `*duration_ms` receives the longer track's duration. A video
 * writer needs at least one video frame, an audio-only one at least one packet.
 * Returns 0, or -1 with `w` freed and nothing allocated. */
int  oc_mp4_writer_finish(oc_mp4_writer *w, uint8_t **out, size_t *len, uint32_t *duration_ms);
/* Stop without a file. */
void oc_mp4_writer_abort(oc_mp4_writer *w);

/* ---- the export writer (ARCH-135, docs/VIDEO-MESSAGES.md "Export") --------------
 *
 * A video message re-encoded for players that do not open VP9: one H.264 track
 * and one MP3 track, written to a FILE as it goes -- the daemon's export never
 * holds the whole copy in memory -- as ftyp · mdat · moov, the index last because
 * a file saved for playing elsewhere is never streamed progressively. Not the
 * restricted profile above: oc_mp4_parse refuses it. Not built for Windows,
 * where nothing exports. */
#if !defined(_WIN32)

#define OC_MP4_EXPORT_MAX_FILE  0xF0000000ull      /* stco's 32-bit offsets, with room for moov */

typedef struct oc_mp4_export oc_mp4_export;

/* Create `path` for a `width`×`height` H.264 track and a mono MP3 track at
 * `audio_rate` Hz. NULL on failure, with nothing left at `path`. */
oc_mp4_export *oc_mp4_export_open(const char *path, int width, int height, unsigned audio_rate);
/* Append one H.264 access unit in Annex B form (start codes), as an encoder
 * emits it. Its SPS and PPS go into the sample entry, the first of each kept and
 * later copies dropped, as are access unit delimiters; every other NAL unit is
 * written length-prefixed. A unit holding an IDR slice is a sync sample, and the
 * first unit must hold one. `pts_us` must not go backwards. */
int oc_mp4_export_video(oc_mp4_export *x, const uint8_t *au, size_t len, int64_t pts_us);
/* Append one MP3 frame of `samples` samples. */
int oc_mp4_export_audio(oc_mp4_export *x, const uint8_t *frame, size_t len, unsigned samples);
/* Write the index and close the file. `audio_delay` is the encoder's and
 * decoder's delay in samples and `audio_samples` the real audio's length: an
 * edit list plays exactly that, so the sound lines up with the picture. On 0,
 * `*bytes` is the file's size and `*duration_ms` the longer track's; on -1 the
 * file is removed. Frees `x` either way. */
int oc_mp4_export_finish(oc_mp4_export *x, uint32_t audio_delay, uint64_t audio_samples,
                         uint64_t *bytes, uint32_t *duration_ms);
/* Stop: close and remove the file, free `x`. */
void oc_mp4_export_abort(oc_mp4_export *x);

#endif

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
 * profile. A file with no video track is accepted when it has an audio track
 * (`info->video.present` is 0). Never reads outside `data[0..len)`. */
int  oc_mp4_parse(const uint8_t *data, size_t len, oc_mp4_info *info);
void oc_mp4_info_free(oc_mp4_info *info);
/* The same from the file's head alone -- ftyp, moov and mdat's 8-byte header,
 * `head_len` bytes of a file of `file_len` -- for a reader that streams the
 * samples rather than holding them (the daemon's export, ARCH-135). The
 * samples are checked against `file_len`; `*mdat_payload` (if non-NULL)
 * receives where mdat's samples start. */
int  oc_mp4_parse_head(const uint8_t *head, size_t head_len, uint64_t file_len, oc_mp4_info *info,
                       uint64_t *mdat_payload);

/* The index of the keyframe at or before `dts` in the video track, or -1. */
int oc_mp4_keyframe_before(const oc_mp4_info *info, uint64_t dts);

#endif
