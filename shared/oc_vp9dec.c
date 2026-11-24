/* VP9 decode over libvpx (oc_vp9dec.h). */
#include "oc_vp9dec.h"

#include <stdlib.h>
#include <string.h>

#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>

#if defined(__EMSCRIPTEN__)
/* A page's threads are Web Workers from a pool of fixed size (WEB.md): an
 * encoder that took one per core would drain it, and a thread the pool
 * cannot give never starts while the page's thread is busy. Three in all. */
static int cores(void) { return 3; }
#elif defined(_WIN32)
#include <windows.h>
static int cores(void) { SYSTEM_INFO si; GetSystemInfo(&si); return (int)si.dwNumberOfProcessors; }
#else
#include <unistd.h>
static int cores(void) { long n = sysconf(_SC_NPROCESSORS_ONLN); return n > 0 ? (int)n : 1; }
#endif

struct oc_vp9dec { vpx_codec_ctx_t ctx; };

oc_vp9dec *oc_vp9dec_open_threads(int threads) {
    oc_vp9dec *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    vpx_codec_dec_cfg_t cfg = { (unsigned)(threads < 1 ? 1 : threads), 0, 0 };
    if (vpx_codec_dec_init(&d->ctx, vpx_codec_vp9_dx(), &cfg, 0) != VPX_CODEC_OK) { free(d); return NULL; }
    return d;
}

oc_vp9dec *oc_vp9dec_open(void) {
    int threads = cores() - 1;
    return oc_vp9dec_open_threads(threads < 1 ? 1 : threads > 4 ? 4 : threads);
}

int oc_vp9dec_decode(oc_vp9dec *d, const uint8_t *data, size_t len, int64_t pts_us, oc_frame *out) {
    if (!data || len == 0 || len > 0xFFFFFFFFu) return -1;
    if (vpx_codec_decode(&d->ctx, data, (unsigned int)len, NULL, 0) != VPX_CODEC_OK) return -1;
    vpx_codec_iter_t it = NULL;
    vpx_image_t *img = vpx_codec_get_frame(&d->ctx, &it);
    if (!img) return 1;
    if (img->fmt != VPX_IMG_FMT_I420 || (img->d_w & 1) || (img->d_h & 1)) return -1;
    out->width = (int)img->d_w; out->height = (int)img->d_h;
    out->pts_us = pts_us;
    for (int p = 0; p < 3; p++) { out->plane[p] = img->planes[p]; out->stride[p] = img->stride[p]; }
    return 0;
}

void oc_vp9dec_close(oc_vp9dec *d) {
    if (!d) return;
    vpx_codec_destroy(&d->ctx);
    free(d);
}
