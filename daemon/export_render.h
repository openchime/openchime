/*
 * OpenChime — one export of a video message: its VP9/Opus MP4 to an H.264 + MP3
 * copy (ARCH-135, docs/details/VIDEO-MESSAGES.md "Export").
 *
 * The render reads the source once, front to back, as the blob store gives it:
 * the index (ftyp, moov) first, then each sample in the order it lies in mdat,
 * handed to the codecs as it arrives. Nothing holds the whole source or the
 * whole copy; the copy is written to a file as it is made.
 *
 * The codecs are behind a small interface so the render, the worker and the
 * daemon above them are tested with a stub that writes a file of the right
 * shape, and only a daemon built with EXPORT=1 carries the real ones
 * (export_engine.c: libvpx and libopus to decode, libavc and LAME to encode).
 */
#ifndef OC_EXPORT_RENDER_H
#define OC_EXPORT_RENDER_H

#include <stddef.h>
#include <stdint.h>

/* The longest index a source may have: moov for five minutes at 30 fps and 50
 * Opus packets a second is under 300 KB. */
#define OC_EXPORT_MAX_HEAD (4u * 1024u * 1024u)

typedef struct {
    int      width, height;        /* the source's, kept */
    unsigned fps;                  /* its average frame rate, rounded, 1..60 */
    int      has_audio;
    int      opus_channels;        /* 1 or 2 */
    unsigned opus_preskip;         /* samples at 48 kHz the decoder discards */
} oc_export_params;

typedef struct oc_export_codecs {
    /* Names the encoders and their settings together: a copy made by one version
     * is never served as another's (the cache key). */
    const char *version;
    /* Start a copy written to `out_path`. NULL with a reason on failure. */
    void *(*open)(const char *out_path, const oc_export_params *p, char *err, size_t errcap);
    /* One VP9 sample, presented at `pts_us`; one Opus packet. 0, or -1 with a reason. */
    int   (*video)(void *s, const uint8_t *data, size_t len, int64_t pts_us, char *err, size_t errcap);
    int   (*audio)(void *s, const uint8_t *data, size_t len, char *err, size_t errcap);
    /* Flush, write the index and close the file: 0 with its size and duration,
     * or -1 with the file removed. Frees `s` either way. */
    int   (*finish)(void *s, uint64_t *bytes, uint32_t *duration_ms, char *err, size_t errcap);
    /* Stop: remove the file, free `s`. */
    void  (*abort)(void *s);
} oc_export_codecs;

/* The source, read front to back: fill up to `cap` bytes, return how many
 * (>0), 0 at its end, <0 on an error. The blob store's reader is one. */
typedef long (*oc_export_read_fn)(void *ctx, void *buf, size_t cap);

typedef enum {
    OC_EXPORT_OK = 0,
    OC_EXPORT_NOT_VIDEO,       /* not a video message's file: no VP9 track, or not the profile */
    OC_EXPORT_CANCELLED,       /* `*cancel` was set: the daemon is stopping */
    OC_EXPORT_FAILED,          /* reading, decoding, encoding or writing failed */
} oc_export_status;

/* Export the `size`-byte source `read` gives to `out_path` with `c`. On
 * OC_EXPORT_OK the file is there and `*bytes`, `*duration_ms` describe it; on
 * anything else no file is left. `cancel` (if non-NULL) is checked between
 * samples. */
oc_export_status oc_export_render(const oc_export_codecs *c, oc_export_read_fn read, void *rctx,
                                  uint64_t size, const char *out_path, const volatile int *cancel,
                                  uint64_t *bytes, uint32_t *duration_ms, char *err, size_t errcap);

/* A stand-in for the codecs, for tests: it writes, in order, one line per
 * sample it was handed ("v <pts_us> <len>" or "a <len>"), and finish reports the
 * file's size and the last video timestamp as the duration. */
extern const oc_export_codecs oc_export_stub_codecs;

/* The real codecs (export_engine.c), or NULL in a daemon built without them. */
const oc_export_codecs *oc_export_codecs_builtin(void);
/* The real codecs themselves, wherever export_engine.c is linked: the daemon
 * built with EXPORT=1, and the test binary's media suite. */
const oc_export_codecs *oc_export_codecs_real(void);

#endif
