/* VP9 encode and decode over libvpx (oc_codec.h). */
#include "oc_codec.h"

#include <stdlib.h>
#include <string.h>

#include <vpx/vp8cx.h>
#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>
#include <vpx/vpx_encoder.h>

#ifdef _WIN32
#include <windows.h>
static int cores(void) { SYSTEM_INFO si; GetSystemInfo(&si); return (int)si.dwNumberOfProcessors; }
#else
#include <unistd.h>
static int cores(void) { long n = sysconf(_SC_NPROCESSORS_ONLN); return n > 0 ? (int)n : 1; }
#endif

struct oc_vp9enc {
    vpx_codec_ctx_t ctx;
    vpx_image_t     img;
    int             width, height, fps;
    int64_t         first_pts_us;
    int             started;
};

unsigned oc_vp9enc_bitrate_kbps(int width, int height) {
    /* Enough that a face keeps its detail at the fastest real-time speed, and
     * small enough that five minutes at 1080p (4 Mbps plus audio, about 150 MB)
     * fits the default video cap. */
    long px = (long)width * height;
    if (px >= 1920L * 1080) return 4000;
    if (px >= 1280L * 720)  return 2500;
    if (px >= 854L * 480)   return 1200;
    return 800;
}

oc_vp9enc *oc_vp9enc_open(int width, int height, int fps) {
    if (width <= 0 || height <= 0 || (width & 1) || (height & 1) || fps <= 0) return NULL;
    oc_vp9enc *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    vpx_codec_enc_cfg_t cfg;
    if (vpx_codec_enc_config_default(vpx_codec_vp9_cx(), &cfg, 0) != VPX_CODEC_OK) { free(e); return NULL; }
    int threads = cores() - 1;
    cfg.g_w = (unsigned)width;
    cfg.g_h = (unsigned)height;
    cfg.g_timebase.num = 1;
    cfg.g_timebase.den = 1000000;                          /* microseconds */
    cfg.g_threads = (unsigned)(threads < 1 ? 1 : threads > 8 ? 8 : threads);
    cfg.g_lag_in_frames = 0;
    cfg.g_error_resilient = 0;
    cfg.g_pass = VPX_RC_ONE_PASS;
    cfg.rc_end_usage = VPX_CBR;
    cfg.rc_target_bitrate = oc_vp9enc_bitrate_kbps(width, height);
    cfg.rc_min_quantizer = 4;
    cfg.rc_max_quantizer = 56;
    cfg.rc_undershoot_pct = 50;
    cfg.rc_overshoot_pct = 50;
    cfg.rc_buf_initial_sz = 500;
    cfg.rc_buf_optimal_sz = 600;
    cfg.rc_buf_sz = 1000;
    cfg.kf_mode = VPX_KF_AUTO;
    cfg.kf_min_dist = 0;
    cfg.kf_max_dist = (unsigned)(2 * fps);
    if (vpx_codec_enc_init(&e->ctx, vpx_codec_vp9_cx(), &cfg, 0) != VPX_CODEC_OK) { free(e); return NULL; }
    vpx_codec_control(&e->ctx, VP8E_SET_CPUUSED, 8);
    vpx_codec_control(&e->ctx, VP9E_SET_ROW_MT, 1);
    vpx_codec_control(&e->ctx, VP9E_SET_TILE_COLUMNS, width >= 1280 ? 2 : 1);
    vpx_codec_control(&e->ctx, VP9E_SET_AQ_MODE, 3);
    vpx_codec_control(&e->ctx, VP9E_SET_COLOR_SPACE, VPX_CS_BT_709);
    vpx_codec_control(&e->ctx, VP9E_SET_COLOR_RANGE, VPX_CR_STUDIO_RANGE);
    e->width = width; e->height = height; e->fps = fps;
    return e;
}

int oc_vp9enc_encode(oc_vp9enc *e, const oc_frame *f, int force_keyframe,
                     void (*emit)(void *ctx, const oc_packet *p), void *ctx) {
    vpx_image_t *img = NULL;
    vpx_codec_pts_t pts = 0;
    unsigned long dur = (unsigned long)(1000000 / e->fps);
    if (f) {
        if (f->width != e->width || f->height != e->height) return -1;
        if (!e->started) { e->first_pts_us = f->pts_us; e->started = 1; }
        vpx_img_wrap(&e->img, VPX_IMG_FMT_I420, (unsigned)f->width, (unsigned)f->height, 1, f->plane[0]);
        for (int p = 0; p < 3; p++) { e->img.planes[p] = f->plane[p]; e->img.stride[p] = f->stride[p]; }
        img = &e->img;
        pts = f->pts_us - e->first_pts_us;
    }
    if (vpx_codec_encode(&e->ctx, img, pts, dur, force_keyframe ? VPX_EFLAG_FORCE_KF : 0,
                         VPX_DL_REALTIME) != VPX_CODEC_OK)
        return -1;
    vpx_codec_iter_t it = NULL;
    const vpx_codec_cx_pkt_t *pkt;
    while ((pkt = vpx_codec_get_cx_data(&e->ctx, &it)) != NULL) {
        if (pkt->kind != VPX_CODEC_CX_FRAME_PKT) continue;
        oc_packet out = { pkt->data.frame.buf, pkt->data.frame.sz,
                          pkt->data.frame.pts + e->first_pts_us,
                          (pkt->data.frame.flags & VPX_FRAME_IS_KEY) != 0 };
        emit(ctx, &out);
    }
    return 0;
}

void oc_vp9enc_close(oc_vp9enc *e) {
    if (!e) return;
    vpx_codec_destroy(&e->ctx);
    free(e);
}

struct oc_vp9dec { vpx_codec_ctx_t ctx; };

oc_vp9dec *oc_vp9dec_open(void) {
    oc_vp9dec *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    int threads = cores() - 1;
    vpx_codec_dec_cfg_t cfg = { (unsigned)(threads < 1 ? 1 : threads > 4 ? 4 : threads), 0, 0 };
    if (vpx_codec_dec_init(&d->ctx, vpx_codec_vp9_dx(), &cfg, 0) != VPX_CODEC_OK) { free(d); return NULL; }
    return d;
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
