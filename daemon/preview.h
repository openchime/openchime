/*
 * Image previews (ARCH-137, docs/details/IMAGE-PREVIEWS.md).
 *
 * The daemon makes two smaller copies of an uploaded JPEG, PNG or WebP --
 * the inline rung, which a message shows in the timeline, and the expanded
 * rung, which the image viewer shows -- once, at upload, so a reader never
 * downloads a multi-megabyte original to see a thumbnail. Each copy keeps its
 * original's format: a screenshot stays lossless, a photo stays lossy.
 *
 * This is the first place the daemon parses untrusted media, so every decode
 * is bounded four ways and none of the bounds can be turned off:
 *
 *   - only the hardened decoders: libjpeg-turbo, libspng, libwebp;
 *   - a byte cap: an input larger than `max_bytes` is not decoded;
 *   - a pixel cap: the dimensions are read from the header and checked
 *     against `max_pixels` BEFORE any pixel buffer is allocated, which is what
 *     stops a decompression bomb;
 *   - a time limit: decode, resize and encode are checked against
 *     `timeout_ms` row by row and given up when it passes.
 *
 * An image refused by any of them, or one that does not decode, simply gets
 * no preview: the original is served, and nobody is told of an error.
 *
 * Pure: no I/O, no globals, no threads. The transfer workers call it
 * (daemon/xferpool.c, OC_XFER_PREVIEW), never the network loop.
 */
#ifndef OC_PREVIEW_H
#define OC_PREVIEW_H

#include <stddef.h>
#include <stdint.h>

/* The two rungs. Their numbers are the wire's (PROTOCOL.md, PREVIEW_BEGIN). */
#define OC_PREVIEW_RUNG_INLINE   1
#define OC_PREVIEW_RUNG_EXPANDED 2
#define OC_PREVIEW_RUNGS         2

/* Defaults for the settings (CONFIG.md, "Attachments"). */
#define OC_PREVIEW_INLINE_DEFAULT      480u          /* px, the longer side   */
#define OC_PREVIEW_EXPANDED_DEFAULT    1600u         /* px, the longer side   */
#define OC_PREVIEW_MAX_BYTES_DEFAULT   (32u << 20)   /* 32 MiB of input       */
#define OC_PREVIEW_MAX_PIXELS_DEFAULT  50000000u     /* 50 megapixels         */
#define OC_PREVIEW_TIMEOUT_DEFAULT     5000u         /* ms                    */
/* An image no larger than the inline box on both sides and no larger than
 * this many bytes gets no preview: the original is as cheap to send. */
#define OC_PREVIEW_SMALL_BYTES         (256u << 10)

typedef struct {
    uint32_t inline_px;     /* longer side of the inline rung's box            */
    uint32_t expanded_px;   /* longer side of the expanded rung's box          */
    uint64_t max_bytes;     /* the byte cap on the input                        */
    uint64_t max_pixels;    /* the pixel cap, width x height, from the header   */
    uint32_t timeout_ms;    /* the time limit for the whole job                 */
    /* The clock the time limit is read on, in ms; NULL is the monotonic
     * clock. A test injects one that moves, so the limit is proved by
     * mechanism rather than by how fast this machine is. */
    uint64_t (*clock_ms)(void *ctx);
    void      *clock_ctx;
} oc_preview_limits;

typedef enum {
    OC_IMG_UNKNOWN = 0,
    OC_IMG_JPEG,
    OC_IMG_PNG,
    OC_IMG_WEBP,
    OC_IMG_GIF,
    OC_IMG_HEIC,
} oc_img_format;

typedef enum {
    OC_PREVIEW_MADE = 0,     /* both rungs made                                  */
    OC_PREVIEW_SMALL,        /* fits the inline box and is small: none needed    */
    OC_PREVIEW_PASSTHROUGH,  /* GIF, HEIC, an animated PNG or WebP, or another
                              * type: never previewed, the original is served    */
    OC_PREVIEW_TOO_BIG,      /* over the byte cap                                */
    OC_PREVIEW_TOO_MANY_PX,  /* over the pixel cap, read from the header         */
    OC_PREVIEW_TIMED_OUT,    /* over the time limit                              */
    OC_PREVIEW_MALFORMED,    /* did not decode, or an unsupported variant         */
    OC_PREVIEW_NO_MEMORY,
} oc_preview_status;

typedef struct {
    uint8_t    *data;        /* the encoded preview (heap)                        */
    size_t      len;
    uint32_t    width, height;
    const char *mime;        /* static: image/jpeg, image/png or image/webp      */
} oc_preview_rung;

typedef struct {
    oc_preview_status status;
    oc_img_format     format;
    uint32_t          width, height;    /* the original's, when the header gave them */
    oc_preview_rung   rung[OC_PREVIEW_RUNGS];   /* [0] inline, [1] expanded          */
} oc_preview_set;

/* The limits with every field at its default. */
void oc_preview_limits_default(oc_preview_limits *l);

/* The format by the file's own magic bytes; the uploader's MIME type is not
 * trusted for this. */
oc_img_format oc_preview_sniff(const uint8_t *buf, size_t len);

/* The dimensions from the header alone, allocating no pixels. 0 when they
 * were read, -1 when the header does not parse (or the format has none this
 * reads: GIF, HEIC). */
int oc_preview_probe(const uint8_t *buf, size_t len, uint32_t *w, uint32_t *h);

/* Make the previews of `buf`. Always fills `out` (status, and the original's
 * format and dimensions where known); on OC_PREVIEW_MADE both rungs hold an
 * encoded image the caller frees with oc_preview_set_free. Returns
 * out->status. A NULL `lim` is the defaults. */
oc_preview_status oc_preview_make(const uint8_t *buf, size_t len, const oc_preview_limits *lim,
                                  oc_preview_set *out);

void oc_preview_set_free(oc_preview_set *s);

/* The fitted size: (w, h) scaled down, aspect kept, so the longer side is at
 * most `box`; never scaled up, never below 1. */
void oc_preview_fit(uint32_t w, uint32_t h, uint32_t box, uint32_t *ow, uint32_t *oh);

/* The status as a word for the log. */
const char *oc_preview_status_name(oc_preview_status s);

#endif /* OC_PREVIEW_H */
