/* Image previews (ARCH-137). See preview.h for what is made and the four
 * bounds every decode is held to, and docs/details/IMAGE-PREVIEWS.md for the
 * values and the reasons. */

#include "preview.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <jpeglib.h>
#include <spng.h>
#include <webp/decode.h>
#include <webp/encode.h>

/* The quality the lossy rungs are encoded at: a preview is looked at, not
 * edited, and these are the values where the artefacts stop being visible
 * at the size shown. */
#define JPEG_QUALITY 82
#define WEBP_QUALITY 80.0f

/* libspng's own bounds, beside ours: no chunk larger than this, and no more
 * than this held of the ancillary chunks it caches. */
#define PNG_CHUNK_LIMIT (8u << 20)
#define PNG_CACHE_LIMIT (16u << 20)

/* How much of a WebP is handed to its incremental decoder between two looks
 * at the clock. */
#define WEBP_FEED 65536

/* --- the clock ------------------------------------------------------------ */

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

typedef struct {
    uint64_t   deadline;    /* ms, on `now` */
    uint64_t (*now)(void *ctx);
    void      *ctx;
} clock_t_;

static uint64_t clock_read(const clock_t_ *c) { return c->now ? c->now(c->ctx) : now_ms(); }
static int late(const clock_t_ *c) { return clock_read(c) > c->deadline; }

/* --- a decoded image ------------------------------------------------------ */

typedef struct {
    uint8_t *px;            /* rows of w * ch bytes, top first */
    uint32_t w, h;
    int      ch;            /* 3 (RGB) or 4 (RGBA, straight alpha) */
} image;

static void image_free(image *im) { free(im->px); im->px = NULL; }

/* --- public helpers ------------------------------------------------------- */

void oc_preview_limits_default(oc_preview_limits *l) {
    l->inline_px   = OC_PREVIEW_INLINE_DEFAULT;
    l->expanded_px = OC_PREVIEW_EXPANDED_DEFAULT;
    l->max_bytes   = OC_PREVIEW_MAX_BYTES_DEFAULT;
    l->max_pixels  = OC_PREVIEW_MAX_PIXELS_DEFAULT;
    l->timeout_ms  = OC_PREVIEW_TIMEOUT_DEFAULT;
    l->clock_ms    = NULL;
    l->clock_ctx   = NULL;
}

const char *oc_preview_status_name(oc_preview_status s) {
    switch (s) {
    case OC_PREVIEW_MADE:        return "made";
    case OC_PREVIEW_SMALL:       return "small";
    case OC_PREVIEW_PASSTHROUGH: return "passthrough";
    case OC_PREVIEW_TOO_BIG:     return "over the byte cap";
    case OC_PREVIEW_TOO_MANY_PX: return "over the pixel cap";
    case OC_PREVIEW_TIMED_OUT:   return "over the time limit";
    case OC_PREVIEW_MALFORMED:   return "malformed";
    case OC_PREVIEW_NO_MEMORY:   return "out of memory";
    }
    return "?";
}

void oc_preview_fit(uint32_t w, uint32_t h, uint32_t box, uint32_t *ow, uint32_t *oh) {
    if (box == 0 || w == 0 || h == 0) { *ow = w; *oh = h; return; }
    uint32_t longer = w > h ? w : h;
    if (longer <= box) { *ow = w; *oh = h; return; }
    uint64_t nw = ((uint64_t)w * box + longer / 2) / longer;
    uint64_t nh = ((uint64_t)h * box + longer / 2) / longer;
    *ow = nw ? (uint32_t)nw : 1;
    *oh = nh ? (uint32_t)nh : 1;
}

oc_img_format oc_preview_sniff(const uint8_t *b, size_t n) {
    if (n >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) return OC_IMG_JPEG;
    if (n >= 8 && memcmp(b, "\x89PNG\r\n\x1a\n", 8) == 0) return OC_IMG_PNG;
    if (n >= 12 && memcmp(b, "RIFF", 4) == 0 && memcmp(b + 8, "WEBP", 4) == 0) return OC_IMG_WEBP;
    if (n >= 6 && (memcmp(b, "GIF87a", 6) == 0 || memcmp(b, "GIF89a", 6) == 0)) return OC_IMG_GIF;
    if (n >= 12 && memcmp(b + 4, "ftyp", 4) == 0) {
        static const char *const brands[] = { "heic", "heix", "hevc", "hevx", "heim", "heis",
                                              "hevm", "hevs", "mif1", "msf1" };
        for (size_t i = 0; i < sizeof brands / sizeof brands[0]; i++)
            if (memcmp(b + 8, brands[i], 4) == 0) return OC_IMG_HEIC;
    }
    return OC_IMG_UNKNOWN;
}

static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* An animated PNG carries an acTL chunk before its first IDAT. libspng reads
 * the first frame only, which as a still would misrepresent the image, so an
 * APNG is passed through as a GIF is. A walk of the chunk headers alone. */
static int png_animated(const uint8_t *b, size_t n) {
    size_t off = 8;
    while (off + 8 <= n) {
        uint32_t len = be32(b + off);
        const uint8_t *type = b + off + 4;
        if (memcmp(type, "acTL", 4) == 0) return 1;
        if (memcmp(type, "IDAT", 4) == 0 || memcmp(type, "IEND", 4) == 0) return 0;
        if ((uint64_t)len + 12 > n - off) return 0;
        off += (size_t)len + 12;
    }
    return 0;
}

/* --- JPEG (libjpeg-turbo) ------------------------------------------------- */

typedef struct {
    struct jpeg_error_mgr       pub;
    struct jpeg_progress_mgr    progress;
    jmp_buf                     jmp;
    const clock_t_             *clock;
    int                         timed_out;
} jpeg_guard;

static void jpeg_fail(j_common_ptr cinfo) {
    jpeg_guard *g = (jpeg_guard *)cinfo->err;
    longjmp(g->jmp, 1);
}

/* Warnings are corrupt-data notices libjpeg recovers from; a preview of a
 * damaged file is not worth making, so they stop the decode too. */
static void jpeg_warn(j_common_ptr cinfo, int level) {
    if (level < 0) jpeg_fail(cinfo);
}

static void jpeg_quiet(j_common_ptr cinfo) { (void)cinfo; }

/* Called during the long passes (a progressive file's input, and every
 * scanline pass): the time limit, inside the library. */
static void jpeg_tick(j_common_ptr cinfo) {
    jpeg_guard *g = (jpeg_guard *)cinfo->err;
    if (late(g->clock)) { g->timed_out = 1; longjmp(g->jmp, 1); }
}

static void jpeg_guard_init(jpeg_guard *g, const clock_t_ *c) {
    memset(g, 0, sizeof *g);
    jpeg_std_error(&g->pub);
    g->pub.error_exit = jpeg_fail;
    g->pub.emit_message = jpeg_warn;
    g->pub.output_message = jpeg_quiet;
    g->progress.progress_monitor = jpeg_tick;
    g->clock = c;
}

static int jpeg_dims(const uint8_t *buf, size_t len, uint32_t *w, uint32_t *h) {
    struct jpeg_decompress_struct cinfo;
    jpeg_guard g;
    clock_t_ c = { UINT64_MAX, NULL, NULL };
    jpeg_guard_init(&g, &c);
    cinfo.err = &g.pub;
    if (setjmp(g.jmp)) { jpeg_destroy_decompress(&cinfo); return -1; }
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, buf, (unsigned long)len);
    jpeg_read_header(&cinfo, TRUE);
    *w = cinfo.image_width;
    *h = cinfo.image_height;
    jpeg_destroy_decompress(&cinfo);
    return 0;
}

/* Decode, scaled down by libjpeg's own 1/2, 1/4 or 1/8 where the result is
 * still at least `want` on its longer side: the cheapest resize there is. */
static oc_preview_status jpeg_decode(const uint8_t *buf, size_t len, const oc_preview_limits *lim,
                                     const clock_t_ *clk, uint32_t want, image *out,
                                     uint32_t *ow, uint32_t *oh) {
    struct jpeg_decompress_struct cinfo;
    jpeg_guard g;
    jpeg_guard_init(&g, clk);
    cinfo.err = &g.pub;
    out->px = NULL;
    volatile oc_preview_status st = OC_PREVIEW_MALFORMED;
    if (setjmp(g.jmp)) {
        jpeg_destroy_decompress(&cinfo);
        image_free(out);
        return g.timed_out ? OC_PREVIEW_TIMED_OUT : st;
    }
    jpeg_create_decompress(&cinfo);
    cinfo.progress = &g.progress;
    jpeg_mem_src(&cinfo, buf, (unsigned long)len);
    jpeg_read_header(&cinfo, TRUE);
    *ow = cinfo.image_width;
    *oh = cinfo.image_height;
    if ((uint64_t)cinfo.image_width * cinfo.image_height > lim->max_pixels) {
        jpeg_destroy_decompress(&cinfo);
        return OC_PREVIEW_TOO_MANY_PX;
    }
    /* RGB out of YCbCr, RGB or greyscale; CMYK and YCCK are print files no
     * chat client needs a thumbnail of, and are left to their original. */
    if (cinfo.jpeg_color_space != JCS_YCbCr && cinfo.jpeg_color_space != JCS_RGB &&
        cinfo.jpeg_color_space != JCS_GRAYSCALE) {
        jpeg_destroy_decompress(&cinfo);
        return OC_PREVIEW_MALFORMED;
    }
    cinfo.out_color_space = JCS_RGB;
    uint32_t longer = cinfo.image_width > cinfo.image_height ? cinfo.image_width : cinfo.image_height;
    unsigned denom = 1;
    while (denom < 8 && longer / (denom * 2) >= want) denom *= 2;
    cinfo.scale_num = 1;
    cinfo.scale_denom = denom;
    cinfo.dct_method = JDCT_ISLOW;
    jpeg_start_decompress(&cinfo);
    if (cinfo.output_components != 3) longjmp(g.jmp, 1);
    out->w = cinfo.output_width;
    out->h = cinfo.output_height;
    out->ch = 3;
    size_t stride = (size_t)out->w * 3;
    out->px = malloc(stride * out->h);
    if (!out->px) { st = OC_PREVIEW_NO_MEMORY; longjmp(g.jmp, 1); }
    while (cinfo.output_scanline < cinfo.output_height) {
        if (late(clk)) { g.timed_out = 1; longjmp(g.jmp, 1); }
        JSAMPROW row = out->px + stride * cinfo.output_scanline;
        if (jpeg_read_scanlines(&cinfo, &row, 1) != 1) longjmp(g.jmp, 1);
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return OC_PREVIEW_MADE;
}

static oc_preview_status jpeg_encode(const image *im, const clock_t_ *clk, oc_preview_rung *r) {
    struct jpeg_compress_struct cinfo;
    jpeg_guard g;
    jpeg_guard_init(&g, clk);
    cinfo.err = &g.pub;
    unsigned char *mem = NULL;
    unsigned long memlen = 0;
    if (setjmp(g.jmp)) {
        jpeg_destroy_compress(&cinfo);
        free(mem);
        return g.timed_out ? OC_PREVIEW_TIMED_OUT : OC_PREVIEW_MALFORMED;
    }
    jpeg_create_compress(&cinfo);
    cinfo.progress = &g.progress;
    jpeg_mem_dest(&cinfo, &mem, &memlen);
    cinfo.image_width = im->w;
    cinfo.image_height = im->h;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, JPEG_QUALITY, TRUE);
    cinfo.optimize_coding = TRUE;
    jpeg_start_compress(&cinfo, TRUE);
    size_t stride = (size_t)im->w * 3;
    while (cinfo.next_scanline < cinfo.image_height) {
        if (late(clk)) { g.timed_out = 1; longjmp(g.jmp, 1); }
        JSAMPROW row = im->px + stride * cinfo.next_scanline;
        jpeg_write_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    r->data = mem;
    r->len = memlen;
    r->width = im->w;
    r->height = im->h;
    r->mime = "image/jpeg";
    return OC_PREVIEW_MADE;
}

/* --- PNG (libspng, with miniz) -------------------------------------------- */

static int png_dims(const uint8_t *buf, size_t len, uint32_t *w, uint32_t *h) {
    /* The IHDR is the first chunk, at a fixed place: no decoder needed. */
    if (len < 24 || memcmp(buf + 12, "IHDR", 4) != 0) return -1;
    *w = be32(buf + 16);
    *h = be32(buf + 20);
    return *w && *h ? 0 : -1;
}

static oc_preview_status png_decode(const uint8_t *buf, size_t len, const oc_preview_limits *lim,
                                    const clock_t_ *clk, image *out, uint32_t *ow, uint32_t *oh) {
    out->px = NULL;
    spng_ctx *ctx = spng_ctx_new(0);
    if (!ctx) return OC_PREVIEW_NO_MEMORY;
    oc_preview_status st = OC_PREVIEW_MALFORMED;
    struct spng_ihdr ihdr;
    spng_set_chunk_limits(ctx, PNG_CHUNK_LIMIT, PNG_CACHE_LIMIT);
    if (spng_set_png_buffer(ctx, buf, len) || spng_get_ihdr(ctx, &ihdr)) goto done;
    *ow = ihdr.width;
    *oh = ihdr.height;
    if ((uint64_t)ihdr.width * ihdr.height > lim->max_pixels) { st = OC_PREVIEW_TOO_MANY_PX; goto done; }
    /* Alpha kept when the image has any: an alpha colour type, or tRNS. */
    struct spng_trns trns;
    int alpha = ihdr.color_type == SPNG_COLOR_TYPE_GRAYSCALE_ALPHA ||
                ihdr.color_type == SPNG_COLOR_TYPE_TRUECOLOR_ALPHA ||
                spng_get_trns(ctx, &trns) == 0;
    int fmt = alpha ? SPNG_FMT_RGBA8 : SPNG_FMT_RGB8;
    size_t size = 0;
    if (spng_decoded_image_size(ctx, fmt, &size)) goto done;
    out->w = ihdr.width;
    out->h = ihdr.height;
    out->ch = alpha ? 4 : 3;
    if (size != (size_t)out->w * out->h * (size_t)out->ch) goto done;
    out->px = malloc(size);
    if (!out->px) { st = OC_PREVIEW_NO_MEMORY; goto done; }
    if (spng_decode_image(ctx, NULL, 0, fmt, SPNG_DECODE_TRNS | SPNG_DECODE_PROGRESSIVE)) goto done;
    size_t stride = (size_t)out->w * out->ch;
    struct spng_row_info ri;
    for (;;) {
        if (late(clk)) { st = OC_PREVIEW_TIMED_OUT; goto done; }
        if (spng_get_row_info(ctx, &ri)) break;
        int rc = spng_decode_row(ctx, out->px + stride * ri.row_num, stride);
        if (rc == SPNG_EOI) { st = OC_PREVIEW_MADE; break; }
        if (rc) goto done;
    }
done:
    spng_ctx_free(ctx);
    if (st != OC_PREVIEW_MADE) image_free(out);
    return st;
}

static oc_preview_status png_encode(const image *im, const clock_t_ *clk, oc_preview_rung *r) {
    spng_ctx *ctx = spng_ctx_new(SPNG_CTX_ENCODER);
    if (!ctx) return OC_PREVIEW_NO_MEMORY;
    oc_preview_status st = OC_PREVIEW_MALFORMED;
    struct spng_ihdr ihdr = { 0 };
    ihdr.width = im->w;
    ihdr.height = im->h;
    ihdr.bit_depth = 8;
    ihdr.color_type = im->ch == 4 ? SPNG_COLOR_TYPE_TRUECOLOR_ALPHA : SPNG_COLOR_TYPE_TRUECOLOR;
    size_t stride = (size_t)im->w * im->ch;
    if (spng_set_option(ctx, SPNG_ENCODE_TO_BUFFER, 1) || spng_set_ihdr(ctx, &ihdr) ||
        spng_encode_image(ctx, NULL, 0, SPNG_FMT_PNG, SPNG_ENCODE_PROGRESSIVE | SPNG_ENCODE_FINALIZE))
        goto done;
    for (uint32_t y = 0; y < im->h; y++) {
        if (late(clk)) { st = OC_PREVIEW_TIMED_OUT; goto done; }
        int rc = spng_encode_row(ctx, im->px + stride * y, stride);
        if (rc == SPNG_EOI) break;
        if (rc) goto done;
    }
    size_t len = 0;
    int err = 0;
    void *png = spng_get_png_buffer(ctx, &len, &err);
    if (!png || err) { free(png); goto done; }
    r->data = png;
    r->len = len;
    r->width = im->w;
    r->height = im->h;
    r->mime = "image/png";
    st = OC_PREVIEW_MADE;
done:
    spng_ctx_free(ctx);
    return st;
}

/* --- WebP (libwebp) ------------------------------------------------------- */

static int webp_dims(const uint8_t *buf, size_t len, uint32_t *w, uint32_t *h) {
    int iw = 0, ih = 0;
    if (!WebPGetInfo(buf, len, &iw, &ih) || iw <= 0 || ih <= 0) return -1;
    *w = (uint32_t)iw;
    *h = (uint32_t)ih;
    return 0;
}

/* Decode with libwebp's own scaler straight to `want`'s box, fed a slice at a
 * time so the clock is looked at between slices. */
static oc_preview_status webp_decode(const uint8_t *buf, size_t len, const oc_preview_limits *lim,
                                     const clock_t_ *clk, uint32_t want, image *out,
                                     uint32_t *ow, uint32_t *oh, int *lossless, int *animated) {
    out->px = NULL;
    WebPDecoderConfig cfg;
    if (!WebPInitDecoderConfig(&cfg)) return OC_PREVIEW_MALFORMED;
    if (WebPGetFeatures(buf, len, &cfg.input) != VP8_STATUS_OK) return OC_PREVIEW_MALFORMED;
    *ow = (uint32_t)cfg.input.width;
    *oh = (uint32_t)cfg.input.height;
    *lossless = cfg.input.format == 2;
    *animated = cfg.input.has_animation;
    if (*animated) return OC_PREVIEW_PASSTHROUGH;
    if ((uint64_t)*ow * *oh > lim->max_pixels) return OC_PREVIEW_TOO_MANY_PX;
    uint32_t dw, dh;
    oc_preview_fit(*ow, *oh, want, &dw, &dh);
    if (dw != *ow || dh != *oh) {
        cfg.options.use_scaling = 1;
        cfg.options.scaled_width = (int)dw;
        cfg.options.scaled_height = (int)dh;
    }
    int alpha = cfg.input.has_alpha;
    out->w = dw;
    out->h = dh;
    out->ch = alpha ? 4 : 3;
    size_t stride = (size_t)dw * out->ch;
    out->px = malloc(stride * dh);
    if (!out->px) return OC_PREVIEW_NO_MEMORY;
    cfg.output.colorspace = alpha ? MODE_RGBA : MODE_RGB;
    cfg.output.is_external_memory = 1;
    cfg.output.u.RGBA.rgba = out->px;
    cfg.output.u.RGBA.stride = (int)stride;
    cfg.output.u.RGBA.size = stride * dh;
    WebPIDecoder *idec = WebPIDecode(NULL, 0, &cfg);
    if (!idec) { image_free(out); return OC_PREVIEW_NO_MEMORY; }
    oc_preview_status st = OC_PREVIEW_MALFORMED;
    size_t off = 0;
    for (;;) {
        if (late(clk)) { st = OC_PREVIEW_TIMED_OUT; break; }
        size_t n = len - off < WEBP_FEED ? len - off : WEBP_FEED;
        VP8StatusCode rc = WebPIAppend(idec, buf + off, n);
        off += n;
        if (rc == VP8_STATUS_OK) { st = OC_PREVIEW_MADE; break; }
        if (rc != VP8_STATUS_SUSPENDED || off >= len) break;
    }
    WebPIDelete(idec);
    WebPFreeDecBuffer(&cfg.output);
    if (st != OC_PREVIEW_MADE) image_free(out);
    return st;
}

static int webp_progress(int percent, const WebPPicture *pic) {
    (void)percent;
    return !late((const clock_t_ *)pic->user_data);
}

static oc_preview_status webp_encode(const image *im, const clock_t_ *clk, int lossless, oc_preview_rung *r) {
    WebPConfig cfg;
    WebPPicture pic;
    WebPMemoryWriter wr;
    if (!WebPConfigInit(&cfg) || !WebPPictureInit(&pic)) return OC_PREVIEW_MALFORMED;
    cfg.lossless = lossless;
    cfg.quality = lossless ? 70.0f : WEBP_QUALITY;   /* lossless: effort, not loss */
    cfg.method = 4;
    pic.use_argb = lossless;
    pic.width = (int)im->w;
    pic.height = (int)im->h;
    int stride = (int)(im->w * (uint32_t)im->ch);
    int ok = im->ch == 4 ? WebPPictureImportRGBA(&pic, im->px, stride)
                         : WebPPictureImportRGB(&pic, im->px, stride);
    if (!ok) { WebPPictureFree(&pic); return OC_PREVIEW_NO_MEMORY; }
    WebPMemoryWriterInit(&wr);
    pic.writer = WebPMemoryWrite;
    pic.custom_ptr = &wr;
    pic.progress_hook = webp_progress;
    pic.user_data = (void *)clk;
    ok = WebPEncode(&cfg, &pic);
    WebPEncodingError err = pic.error_code;
    WebPPictureFree(&pic);
    if (!ok) {
        WebPMemoryWriterClear(&wr);
        return err == VP8_ENC_ERROR_USER_ABORT ? OC_PREVIEW_TIMED_OUT : OC_PREVIEW_MALFORMED;
    }
    /* Copied, so every rung is freed with free(). */
    r->data = malloc(wr.size ? wr.size : 1);
    if (!r->data) { WebPMemoryWriterClear(&wr); return OC_PREVIEW_NO_MEMORY; }
    memcpy(r->data, wr.mem, wr.size);
    r->len = wr.size;
    WebPMemoryWriterClear(&wr);
    r->width = im->w;
    r->height = im->h;
    r->mime = "image/webp";
    return OC_PREVIEW_MADE;
}

/* --- the resize ----------------------------------------------------------- */

/* Area averaging: every output pixel is the coverage-weighted mean of the
 * source pixels under it, which is the right filter for a reduction (no
 * aliasing, no ringing). Alpha is averaged premultiplied, so a transparent
 * pixel's colour cannot bleed into the edge of an opaque one. The clock is
 * looked at every output row. */
static oc_preview_status shrink(const image *src, uint32_t dw, uint32_t dh, const clock_t_ *clk, image *dst) {
    dst->px = NULL;
    dst->w = dw;
    dst->h = dh;
    dst->ch = src->ch;
    int ch = src->ch;
    if (dw == src->w && dh == src->h) {
        size_t n = (size_t)dw * dh * ch;
        dst->px = malloc(n);
        if (!dst->px) return OC_PREVIEW_NO_MEMORY;
        memcpy(dst->px, src->px, n);
        return OC_PREVIEW_MADE;
    }
    dst->px = malloc((size_t)dw * dh * ch);
    float *acc = calloc((size_t)dw * ch, sizeof *acc);      /* one output row */
    float *hrow = malloc((size_t)dw * ch * sizeof *hrow);    /* one source row, resampled across */
    if (!dst->px || !acc || !hrow) { free(acc); free(hrow); image_free(dst); return OC_PREVIEW_NO_MEMORY; }
    double sx = (double)src->w / dw, sy = (double)src->h / dh;
    size_t sstride = (size_t)src->w * ch;
    for (uint32_t oy = 0; oy < dh; oy++) {
        if (late(clk)) { free(acc); free(hrow); image_free(dst); return OC_PREVIEW_TIMED_OUT; }
        memset(acc, 0, (size_t)dw * ch * sizeof *acc);
        double y0 = oy * sy, y1 = (oy + 1) * sy;
        uint32_t iy0 = (uint32_t)y0, iy1 = (uint32_t)(y1 + 0.999999);
        if (iy1 > src->h) iy1 = src->h;
        double wsum_y = 0;
        for (uint32_t iy = iy0; iy < iy1; iy++) {
            double wy = (iy + 1 < y1 ? iy + 1 : y1) - (iy > y0 ? iy : y0);
            if (wy <= 0) continue;
            wsum_y += wy;
            const uint8_t *row = src->px + sstride * iy;
            /* Across: the same averaging, one source row into dw pixels. */
            for (uint32_t ox = 0; ox < dw; ox++) {
                double x0 = ox * sx, x1 = (ox + 1) * sx;
                uint32_t ix0 = (uint32_t)x0, ix1 = (uint32_t)(x1 + 0.999999);
                if (ix1 > src->w) ix1 = src->w;
                double s[4] = { 0, 0, 0, 0 }, wsum_x = 0;
                for (uint32_t ix = ix0; ix < ix1; ix++) {
                    double wx = (ix + 1 < x1 ? ix + 1 : x1) - (ix > x0 ? ix : x0);
                    if (wx <= 0) continue;
                    wsum_x += wx;
                    const uint8_t *p = row + (size_t)ix * ch;
                    if (ch == 4) {
                        double a = p[3] * wx;
                        s[0] += p[0] * a; s[1] += p[1] * a; s[2] += p[2] * a; s[3] += a;
                    } else {
                        s[0] += p[0] * wx; s[1] += p[1] * wx; s[2] += p[2] * wx;
                    }
                }
                float *h = hrow + (size_t)ox * ch;
                for (int k = 0; k < ch; k++) h[k] = wsum_x > 0 ? (float)(s[k] / wsum_x) : 0.0f;
            }
            for (size_t k = 0; k < (size_t)dw * ch; k++) acc[k] += (float)(hrow[k] * wy);
        }
        uint8_t *out = dst->px + (size_t)dw * ch * oy;
        for (uint32_t ox = 0; ox < dw; ox++) {
            float *a = acc + (size_t)ox * ch;
            if (ch == 4) {
                double alpha = wsum_y > 0 ? a[3] / wsum_y : 0;
                for (int k = 0; k < 3; k++) {
                    double v = alpha > 0 ? a[k] / wsum_y / alpha : 0;
                    out[(size_t)ox * 4 + k] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v + 0.5);
                }
                out[(size_t)ox * 4 + 3] = (uint8_t)(alpha > 255 ? 255 : alpha + 0.5);
            } else {
                for (int k = 0; k < 3; k++) {
                    double v = wsum_y > 0 ? a[k] / wsum_y : 0;
                    out[(size_t)ox * 3 + k] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v + 0.5);
                }
            }
        }
    }
    free(acc);
    free(hrow);
    return OC_PREVIEW_MADE;
}

/* --- the whole job -------------------------------------------------------- */

int oc_preview_probe(const uint8_t *buf, size_t len, uint32_t *w, uint32_t *h) {
    switch (oc_preview_sniff(buf, len)) {
    case OC_IMG_JPEG: return jpeg_dims(buf, len, w, h);
    case OC_IMG_PNG:  return png_dims(buf, len, w, h);
    case OC_IMG_WEBP: return webp_dims(buf, len, w, h);
    default:          return -1;
    }
}

void oc_preview_set_free(oc_preview_set *s) {
    if (!s) return;
    for (int i = 0; i < OC_PREVIEW_RUNGS; i++) {
        free(s->rung[i].data);
        s->rung[i].data = NULL;
        s->rung[i].len = 0;
    }
}

static oc_preview_status encode_as(oc_img_format f, const image *im, const clock_t_ *clk, int lossless,
                                   oc_preview_rung *r) {
    switch (f) {
    case OC_IMG_JPEG: return jpeg_encode(im, clk, r);
    case OC_IMG_PNG:  return png_encode(im, clk, r);
    case OC_IMG_WEBP: return webp_encode(im, clk, lossless, r);
    default:          return OC_PREVIEW_MALFORMED;
    }
}

oc_preview_status oc_preview_make(const uint8_t *buf, size_t len, const oc_preview_limits *lim_in,
                                  oc_preview_set *out) {
    oc_preview_limits lim;
    if (lim_in) lim = *lim_in; else oc_preview_limits_default(&lim);
    memset(out, 0, sizeof *out);
    out->format = oc_preview_sniff(buf, len);

    /* Never previewed: GIF (shown as itself, capped in size by the client),
     * HEIC (no decoder linked, by choice) and anything not an image. */
    if (out->format != OC_IMG_JPEG && out->format != OC_IMG_PNG && out->format != OC_IMG_WEBP)
        return out->status = OC_PREVIEW_PASSTHROUGH;
    if (len > lim.max_bytes) return out->status = OC_PREVIEW_TOO_BIG;
    if (oc_preview_probe(buf, len, &out->width, &out->height) != 0)
        return out->status = OC_PREVIEW_MALFORMED;
    if ((uint64_t)out->width * out->height > lim.max_pixels)
        return out->status = OC_PREVIEW_TOO_MANY_PX;
    if (out->format == OC_IMG_PNG && png_animated(buf, len))
        return out->status = OC_PREVIEW_PASSTHROUGH;
    if (out->width <= lim.inline_px && out->height <= lim.inline_px && len <= OC_PREVIEW_SMALL_BYTES)
        return out->status = OC_PREVIEW_SMALL;

    clock_t_ clk = { 0, lim.clock_ms, lim.clock_ctx };
    clk.deadline = clock_read(&clk) + lim.timeout_ms;
    uint32_t ew, eh, iw, ih;
    oc_preview_fit(out->width, out->height, lim.expanded_px, &ew, &eh);
    uint32_t want = ew > eh ? ew : eh;

    image dec = { 0 };
    uint32_t w = 0, h = 0;
    int lossless = 0, animated = 0;
    oc_preview_status st;
    switch (out->format) {
    case OC_IMG_JPEG: st = jpeg_decode(buf, len, &lim, &clk, want, &dec, &w, &h); break;
    case OC_IMG_PNG:  st = png_decode(buf, len, &lim, &clk, &dec, &w, &h); break;
    default:          st = webp_decode(buf, len, &lim, &clk, want, &dec, &w, &h, &lossless, &animated); break;
    }
    if (st != OC_PREVIEW_MADE) return out->status = st;

    /* The expanded rung from the decoded image, the inline rung from that. */
    image exp = { 0 }, inl = { 0 };
    st = shrink(&dec, ew, eh, &clk, &exp);
    image_free(&dec);
    if (st != OC_PREVIEW_MADE) return out->status = st;
    oc_preview_fit(out->width, out->height, lim.inline_px, &iw, &ih);
    st = shrink(&exp, iw, ih, &clk, &inl);
    if (st == OC_PREVIEW_MADE) st = encode_as(out->format, &inl, &clk, lossless, &out->rung[0]);
    if (st == OC_PREVIEW_MADE) st = encode_as(out->format, &exp, &clk, lossless, &out->rung[1]);
    image_free(&inl);
    image_free(&exp);
    if (st != OC_PREVIEW_MADE) oc_preview_set_free(out);
    return out->status = st;
}
