/* Images made in memory for the image-preview tests; see preview_samples.h. */

#include "preview_samples.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jpeglib.h>
#include <spng.h>
#include <webp/encode.h>

/* Every pixel a function of its place, so a resize that misplaces rows shows. */
static uint8_t *pixels(uint32_t w, uint32_t h, int ch) {
    uint8_t *px = malloc((size_t)w * h * ch);
    if (!px) return NULL;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint8_t *p = px + ((size_t)y * w + x) * ch;
            p[0] = (uint8_t)(x * 255 / (w > 1 ? w - 1 : 1));
            p[1] = (uint8_t)(y * 255 / (h > 1 ? h - 1 : 1));
            p[2] = (uint8_t)((x + y) & 0xFF);
            if (ch == 4) p[3] = x < w / 2 ? 0 : 255;
        }
    return px;
}

typedef struct { struct jpeg_error_mgr pub; jmp_buf jmp; } jerr;
static void jfail(j_common_ptr c) { longjmp(((jerr *)c->err)->jmp, 1); }

uint8_t *sample_jpeg(uint32_t w, uint32_t h, size_t *len) {
    uint8_t *px = pixels(w, h, 3);
    if (!px) return NULL;
    struct jpeg_compress_struct c;
    jerr e;
    c.err = jpeg_std_error(&e.pub);
    e.pub.error_exit = jfail;
    unsigned char *mem = NULL;
    unsigned long n = 0;
    if (setjmp(e.jmp)) { jpeg_destroy_compress(&c); free(mem); free(px); return NULL; }
    jpeg_create_compress(&c);
    jpeg_mem_dest(&c, &mem, &n);
    c.image_width = w;
    c.image_height = h;
    c.input_components = 3;
    c.in_color_space = JCS_RGB;
    jpeg_set_defaults(&c);
    jpeg_set_quality(&c, 90, TRUE);
    jpeg_start_compress(&c, TRUE);
    while (c.next_scanline < h) {
        JSAMPROW row = px + (size_t)c.next_scanline * w * 3;
        jpeg_write_scanlines(&c, &row, 1);
    }
    jpeg_finish_compress(&c);
    jpeg_destroy_compress(&c);
    free(px);
    *len = n;
    return mem;
}

uint8_t *sample_png(uint32_t w, uint32_t h, int alpha, size_t *len) {
    int ch = alpha ? 4 : 3;
    uint8_t *px = pixels(w, h, ch);
    if (!px) return NULL;
    spng_ctx *ctx = spng_ctx_new(SPNG_CTX_ENCODER);
    struct spng_ihdr ihdr = { 0 };
    ihdr.width = w;
    ihdr.height = h;
    ihdr.bit_depth = 8;
    ihdr.color_type = alpha ? SPNG_COLOR_TYPE_TRUECOLOR_ALPHA : SPNG_COLOR_TYPE_TRUECOLOR;
    uint8_t *out = NULL;
    int err = 0;
    if (ctx && spng_set_option(ctx, SPNG_ENCODE_TO_BUFFER, 1) == 0 && spng_set_ihdr(ctx, &ihdr) == 0 &&
        spng_encode_image(ctx, px, (size_t)w * h * ch, SPNG_FMT_PNG, SPNG_ENCODE_FINALIZE) == 0)
        out = spng_get_png_buffer(ctx, len, &err);
    spng_ctx_free(ctx);
    free(px);
    if (err) { free(out); return NULL; }
    return out;
}

uint8_t *sample_webp(uint32_t w, uint32_t h, int lossless, size_t *len) {
    uint8_t *px = pixels(w, h, 3);
    if (!px) return NULL;
    uint8_t *enc = NULL;
    size_t n = lossless ? WebPEncodeLosslessRGB(px, (int)w, (int)h, (int)w * 3, &enc)
                        : WebPEncodeRGB(px, (int)w, (int)h, (int)w * 3, 85.0f, &enc);
    free(px);
    if (!n) return NULL;
    uint8_t *out = malloc(n);
    if (out) memcpy(out, enc, n);
    WebPFree(enc);
    *len = n;
    return out;
}
