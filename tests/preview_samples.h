/* Images made in memory for the image-preview tests (tests/test_preview.c) and
 * the decoders' place in the fuzz sweep (tests/test_fuzz.c), encoded with the
 * same libraries the daemon decodes with: no image file is checked in. Each
 * returns a heap buffer the caller frees, or NULL. */
#ifndef OC_TEST_PREVIEW_SAMPLES_H
#define OC_TEST_PREVIEW_SAMPLES_H

#include <stddef.h>
#include <stdint.h>

/* A w x h JPEG of a smooth gradient. */
uint8_t *sample_jpeg(uint32_t w, uint32_t h, size_t *len);
/* A w x h PNG, RGBA with a transparent half when `alpha`, else RGB. */
uint8_t *sample_png(uint32_t w, uint32_t h, int alpha, size_t *len);
/* A w x h WebP, lossless or lossy. */
uint8_t *sample_webp(uint32_t w, uint32_t h, int lossless, size_t *len);

#endif
