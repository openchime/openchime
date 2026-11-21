/* The export's real codecs (export_render.h), in a daemon built with EXPORT=1:
 * VP9 decoded by libvpx and re-encoded as H.264 Constrained Baseline by libavc,
 * Opus decoded by libopus and re-encoded as MP3 by LAME, written by oc_mp4's
 * export writer (ARCH-135, docs/VIDEO-MESSAGES.md "Export").
 *
 * Memory is what the managed box allows (ARCH-135): one decoder thread, one
 * encoder core, libavc's reference and search buffers sized to this video
 * rather than its 1920×1920 default, and a handful of frames in flight. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "export_render.h"

#include "oc_mp4.h"
#include "oc_opusdec.h"
#include "oc_vp9dec.h"

#include <ih264_typedefs.h>   /* the types ih264e.h uses, not included by it */
#include <ih264e.h>
#include <lame/lame.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* What the copy is made with. A change to any of it is a new version, which the
 * cache is keyed by, so an old copy is never served as a new one. */
#define EXPORT_VERSION      "h264-cbp-libavc-1.10.0/mp3-lame-3.100-96k/1"
#define MP3_KBPS            96
#define MP3_FRAME           1152            /* MPEG-1 Layer III, samples a frame */
#define MP3_DECODER_DELAY   529             /* what an MP3 decoder adds (LAME's own figure) */
#define INPUT_BUFS          4               /* frames the encoder may hold at once */
#define OPUS_MAX_FRAME      5760            /* 120 ms at 48 kHz */

static void seterr(char *err, size_t cap, const char *fmt, ...) {
    if (!err || !cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

/* The H.264 rate for a picture size: about one and a half times what the VP9
 * source was recorded at (oc_vp9enc_bitrate_kbps), since Baseline compresses
 * less, so the copy looks as the original did. */
static unsigned h264_kbps(int width, int height) {
    long px = (long)width * height;
    if (px >= 1920L * 1080) return 6000;
    if (px >= 1280L * 720)  return 3750;
    if (px >= 854L * 480)   return 1800;
    return 1200;
}

static unsigned h264_level(int width, int height) {
    long px = (long)width * height;
    return px > 1280L * 720 ? 40 : px > 720L * 576 ? 31 : 30;
}

/* The fastest path this CPU runs: libavc leaves the choice to its caller. */
static IV_ARCH_T best_arch(void) {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_cpu_init();
    if (__builtin_cpu_supports("sse4.2")) return ARCH_X86_SSE42;
    if (__builtin_cpu_supports("ssse3")) return ARCH_X86_SSSE3;
    return ARCH_X86_GENERIC;
#elif defined(__aarch64__)
    return ARCH_ARM_V8_NEON;
#else
    return ARCH_ARM_NONEON;
#endif
}

typedef struct { uint8_t *buf; int busy; } inbuf;

typedef struct {
    oc_mp4_export  *mp4;
    oc_vp9dec      *vdec;
    oc_opusdec     *adec;
    lame_global_flags *lame;

    iv_obj_t       *enc;
    iv_mem_rec_t   *recs;
    uint32_t        n_recs;
    inbuf           in[INPUT_BUFS];
    uint8_t        *out;               /* one access unit at a time */
    uint32_t        out_cap;
    int             width, height;

    unsigned        preskip_left;      /* Opus samples still to discard */
    uint64_t        pcm_samples;       /* real audio fed to the MP3 encoder */
    int16_t        *pcm;
    uint8_t        *mp3;               /* encoder output not yet split into frames */
    size_t          mp3_n, mp3_cap;
    uint32_t        mp3_delay;
} engine;

/* ---- libavc ---------------------------------------------------------------------- */

static int ctl(engine *e, void *ip, void *op) { return ih264e_api_function(e->enc, ip, op) == IV_SUCCESS ? 0 : -1; }

static void free_encoder(engine *e) {
    if (e->recs) {
        for (uint32_t i = 0; i < e->n_recs; i++) free(e->recs[i].pv_base);
        free(e->recs);
    }
    e->recs = NULL;
    e->enc = NULL;
    for (int i = 0; i < INPUT_BUFS; i++) { free(e->in[i].buf); e->in[i].buf = NULL; }
    free(e->out);
    e->out = NULL;
}

static int open_encoder(engine *e, int width, int height, unsigned fps, char *err, size_t errcap) {
    /* Allocations sized to this picture, rounded to whole macroblocks. */
    uint32_t wd = (uint32_t)((width + 15) & ~15), ht = (uint32_t)((height + 15) & ~15);
    unsigned level = h264_level(width, height);
    uint32_t srch_x = 64, srch_y = 48;
    /* The most the encoder is told it may search must be libavc's own default,
     * 256: it sizes its motion-vector cost table from this figure but fills the
     * table for 256 regardless (ih264e_init_mv_bits), so anything smaller is a
     * write past the end. The search itself is limited below (srch_x, srch_y). */
    const uint32_t max_srch = 256;

    ih264e_num_mem_rec_ip_t nip = {0};
    ih264e_num_mem_rec_op_t nop = {0};
    nip.s_ive_ip.u4_size = sizeof nip; nip.s_ive_ip.e_cmd = IV_CMD_GET_NUM_MEM_REC;
    nop.s_ive_op.u4_size = sizeof nop;
    if (ih264e_api_function(NULL, &nip, &nop) != IV_SUCCESS) { seterr(err, errcap, "h264: memory records"); return -1; }
    e->n_recs = nop.s_ive_op.u4_num_mem_rec;
    e->recs = calloc(e->n_recs, sizeof *e->recs);
    if (!e->recs) { seterr(err, errcap, "out of memory"); return -1; }
    for (uint32_t i = 0; i < e->n_recs; i++) { e->recs[i].u4_size = sizeof(iv_mem_rec_t); e->recs[i].e_mem_type = IV_NA_MEM_TYPE; }

    ih264e_fill_mem_rec_ip_t fip = {0};
    ih264e_fill_mem_rec_op_t fop = {0};
    fip.s_ive_ip.u4_size = sizeof fip; fip.s_ive_ip.e_cmd = IV_CMD_FILL_NUM_MEM_REC;
    fip.s_ive_ip.ps_mem_rec = e->recs; fip.s_ive_ip.u4_num_mem_rec = e->n_recs;
    fip.s_ive_ip.u4_max_wd = wd; fip.s_ive_ip.u4_max_ht = ht;
    fip.s_ive_ip.u4_max_ref_cnt = 1; fip.s_ive_ip.u4_max_reorder_cnt = 0;
    fip.s_ive_ip.u4_max_level = level; fip.s_ive_ip.e_color_format = IV_YUV_420P;
    fip.s_ive_ip.u4_max_srch_rng_x = max_srch; fip.s_ive_ip.u4_max_srch_rng_y = max_srch;
    fop.s_ive_op.u4_size = sizeof fop;
    if (ih264e_api_function(NULL, &fip, &fop) != IV_SUCCESS) { seterr(err, errcap, "h264: sizing memory"); return -1; }
    for (uint32_t i = 0; i < e->n_recs; i++) {
        uint32_t align = e->recs[i].u4_mem_alignment ? e->recs[i].u4_mem_alignment : 16;
        if (align < sizeof(void *)) align = sizeof(void *);
        void *p = NULL;
        if (posix_memalign(&p, align, e->recs[i].u4_mem_size ? e->recs[i].u4_mem_size : 1) != 0) {
            seterr(err, errcap, "out of memory"); return -1;
        }
        e->recs[i].pv_base = p;
    }

    e->enc = e->recs[0].pv_base;
    e->enc->u4_size = sizeof(iv_obj_t);
    e->enc->pv_fxns = (void *)ih264e_api_function;
    ih264e_init_ip_t iip = {0};
    ih264e_init_op_t iop = {0};
    iip.s_ive_ip.u4_size = sizeof iip; iip.s_ive_ip.e_cmd = IV_CMD_INIT;
    iip.s_ive_ip.u4_num_mem_rec = e->n_recs; iip.s_ive_ip.ps_mem_rec = e->recs;
    iip.s_ive_ip.u4_max_wd = wd; iip.s_ive_ip.u4_max_ht = ht;
    iip.s_ive_ip.u4_max_ref_cnt = 1; iip.s_ive_ip.u4_max_reorder_cnt = 0;
    iip.s_ive_ip.u4_max_level = level;
    iip.s_ive_ip.e_inp_color_fmt = IV_YUV_420P;
    iip.s_ive_ip.u4_enable_recon = 0; iip.s_ive_ip.e_recon_color_fmt = IV_YUV_420P;
    iip.s_ive_ip.e_rc_mode = IVE_RC_STORAGE;
    iip.s_ive_ip.u4_max_framerate = 60000;
    iip.s_ive_ip.u4_max_bitrate = h264_kbps(width, height) * 2000u;
    iip.s_ive_ip.u4_num_bframes = 0;
    iip.s_ive_ip.e_content_type = IV_PROGRESSIVE;
    iip.s_ive_ip.u4_max_srch_rng_x = max_srch; iip.s_ive_ip.u4_max_srch_rng_y = max_srch;
    iip.s_ive_ip.e_slice_mode = IVE_SLICE_MODE_NONE;
    iip.s_ive_ip.e_arch = best_arch(); iip.s_ive_ip.e_soc = SOC_GENERIC;
    iop.s_ive_op.u4_size = sizeof iop;
    if (ctl(e, &iip, &iop) != 0) { seterr(err, errcap, "h264: init 0x%x", iop.s_ive_op.u4_error_code); return -1; }

    ih264e_ctl_set_num_cores_ip_t cip = {0};
    ih264e_ctl_set_num_cores_op_t cop = {0};
    cip.s_ive_ip.u4_size = sizeof cip; cip.s_ive_ip.e_cmd = IVE_CMD_VIDEO_CTL;
    cip.s_ive_ip.e_sub_cmd = IVE_CMD_CTL_SET_NUM_CORES; cip.s_ive_ip.u4_num_cores = 1;
    cop.s_ive_op.u4_size = sizeof cop;

    ih264e_ctl_set_dimensions_ip_t dip = {0};
    ih264e_ctl_set_dimensions_op_t dop = {0};
    dip.s_ive_ip.u4_size = sizeof dip; dip.s_ive_ip.e_cmd = IVE_CMD_VIDEO_CTL;
    dip.s_ive_ip.e_sub_cmd = IVE_CMD_CTL_SET_DIMENSIONS;
    dip.s_ive_ip.u4_wd = (uint32_t)width; dip.s_ive_ip.u4_ht = (uint32_t)height;
    dop.s_ive_op.u4_size = sizeof dop;

    ih264e_ctl_set_frame_rate_ip_t rip = {0};
    ih264e_ctl_set_frame_rate_op_t rop = {0};
    rip.s_ive_ip.u4_size = sizeof rip; rip.s_ive_ip.e_cmd = IVE_CMD_VIDEO_CTL;
    rip.s_ive_ip.e_sub_cmd = IVE_CMD_CTL_SET_FRAMERATE;
    rip.s_ive_ip.u4_src_frame_rate = fps; rip.s_ive_ip.u4_tgt_frame_rate = fps;
    rop.s_ive_op.u4_size = sizeof rop;

    ih264e_ctl_set_ipe_params_ip_t pip = {0};
    ih264e_ctl_set_ipe_params_op_t pop = {0};
    pip.s_ive_ip.u4_size = sizeof pip; pip.s_ive_ip.e_cmd = IVE_CMD_VIDEO_CTL;
    pip.s_ive_ip.e_sub_cmd = IVE_CMD_CTL_SET_IPE_PARAMS;
    pip.s_ive_ip.u4_enable_intra_4x4 = 1; pip.s_ive_ip.u4_enc_speed_preset = IVE_FASTEST;
    pip.s_ive_ip.u4_constrained_intra_pred = 0;
    pop.s_ive_op.u4_size = sizeof pop;

    ih264e_ctl_set_bitrate_ip_t bip = {0};
    ih264e_ctl_set_bitrate_op_t bop = {0};
    bip.s_ive_ip.u4_size = sizeof bip; bip.s_ive_ip.e_cmd = IVE_CMD_VIDEO_CTL;
    bip.s_ive_ip.e_sub_cmd = IVE_CMD_CTL_SET_BITRATE;
    bip.s_ive_ip.u4_target_bitrate = h264_kbps(width, height) * 1000u;
    bop.s_ive_op.u4_size = sizeof bop;

    ih264e_ctl_set_qp_ip_t qip = {0};
    ih264e_ctl_set_qp_op_t qop = {0};
    qip.s_ive_ip.u4_size = sizeof qip; qip.s_ive_ip.e_cmd = IVE_CMD_VIDEO_CTL;
    qip.s_ive_ip.e_sub_cmd = IVE_CMD_CTL_SET_QP;
    qip.s_ive_ip.u4_i_qp = 25; qip.s_ive_ip.u4_i_qp_min = 10; qip.s_ive_ip.u4_i_qp_max = 45;
    qip.s_ive_ip.u4_p_qp = 28; qip.s_ive_ip.u4_p_qp_min = 10; qip.s_ive_ip.u4_p_qp_max = 45;
    qip.s_ive_ip.u4_b_qp = 28; qip.s_ive_ip.u4_b_qp_min = 10; qip.s_ive_ip.u4_b_qp_max = 45;
    qop.s_ive_op.u4_size = sizeof qop;

    ih264e_ctl_set_me_params_ip_t mip = {0};
    ih264e_ctl_set_me_params_op_t mop = {0};
    mip.s_ive_ip.u4_size = sizeof mip; mip.s_ive_ip.e_cmd = IVE_CMD_VIDEO_CTL;
    mip.s_ive_ip.e_sub_cmd = IVE_CMD_CTL_SET_ME_PARAMS;
    mip.s_ive_ip.u4_me_speed_preset = 100; mip.s_ive_ip.u4_enable_hpel = 1; mip.s_ive_ip.u4_enable_qpel = 1;
    mip.s_ive_ip.u4_enable_fast_sad = 0; mip.s_ive_ip.u4_enable_alt_ref = 0;
    mip.s_ive_ip.u4_srch_rng_x = srch_x; mip.s_ive_ip.u4_srch_rng_y = srch_y;
    mop.s_ive_op.u4_size = sizeof mop;

    /* An IDR every two seconds: what a player seeks to, as the source's keyframes. */
    ih264e_ctl_set_gop_params_ip_t gip = {0};
    ih264e_ctl_set_gop_params_op_t gop = {0};
    gip.s_ive_ip.u4_size = sizeof gip; gip.s_ive_ip.e_cmd = IVE_CMD_VIDEO_CTL;
    gip.s_ive_ip.e_sub_cmd = IVE_CMD_CTL_SET_GOP_PARAMS;
    gip.s_ive_ip.u4_i_frm_interval = 2 * fps; gip.s_ive_ip.u4_idr_frm_interval = 2 * fps;
    gop.s_ive_op.u4_size = sizeof gop;

    ih264e_ctl_set_profile_params_ip_t fpp = {0};
    ih264e_ctl_set_profile_params_op_t fpo = {0};
    fpp.s_ive_ip.u4_size = sizeof fpp; fpp.s_ive_ip.e_cmd = IVE_CMD_VIDEO_CTL;
    fpp.s_ive_ip.e_sub_cmd = IVE_CMD_CTL_SET_PROFILE_PARAMS;
    fpp.s_ive_ip.e_profile = IV_PROFILE_BASE; fpp.s_ive_ip.u4_entropy_coding_mode = 0;   /* CAVLC */
    fpo.s_ive_op.u4_size = sizeof fpo;

    ih264e_ctl_getbufinfo_ip_t uip = {0};
    ih264e_ctl_getbufinfo_op_t uop = {0};
    uip.s_ive_ip.u4_size = sizeof uip; uip.s_ive_ip.e_cmd = IVE_CMD_VIDEO_CTL;
    uip.s_ive_ip.e_sub_cmd = IVE_CMD_CTL_GETBUFINFO;
    uip.s_ive_ip.u4_max_wd = wd; uip.s_ive_ip.u4_max_ht = ht; uip.s_ive_ip.e_inp_color_fmt = IV_YUV_420P;
    uop.s_ive_op.u4_size = sizeof uop;

    if (ctl(e, &cip, &cop) || ctl(e, &dip, &dop) || ctl(e, &rip, &rop) || ctl(e, &pip, &pop) ||
        ctl(e, &bip, &bop) || ctl(e, &qip, &qop) || ctl(e, &mip, &mop) || ctl(e, &gip, &gop) ||
        ctl(e, &fpp, &fpo) || ctl(e, &uip, &uop)) {
        seterr(err, errcap, "h264: configuring the encoder");
        return -1;
    }
    e->out_cap = uop.s_ive_op.au4_min_out_buf_size[0];
    if (e->out_cap < 65536) e->out_cap = 65536;
    e->out = malloc(e->out_cap);
    if (!e->out) { seterr(err, errcap, "out of memory"); return -1; }
    size_t frame = (size_t)width * (size_t)height * 3 / 2;
    for (int i = 0; i < INPUT_BUFS; i++) {
        e->in[i].buf = malloc(frame);
        if (!e->in[i].buf) { seterr(err, errcap, "out of memory"); return -1; }
    }
    e->width = width; e->height = height;
    return 0;
}

static int set_mode(engine *e, IVE_ENC_MODE_T mode) {
    ih264e_ctl_set_enc_mode_ip_t ip = {0};
    ih264e_ctl_set_enc_mode_op_t op = {0};
    ip.s_ive_ip.u4_size = sizeof ip; ip.s_ive_ip.e_cmd = IVE_CMD_VIDEO_CTL;
    ip.s_ive_ip.e_sub_cmd = IVE_CMD_CTL_SET_ENC_MODE; ip.s_ive_ip.e_enc_mode = mode;
    op.s_ive_op.u4_size = sizeof op;
    return ctl(e, &ip, &op);
}

/* One encode call: `buf` a whole I420 frame, or NULL to drain. Whatever comes
 * out goes to the file. `*last` is set when the encoder has nothing more. */
static int encode(engine *e, inbuf *b, int64_t pts_us, int drain, int *last, char *err, size_t errcap) {
    ih264e_video_encode_ip_t ip;
    ih264e_video_encode_op_t op;
    memset(&ip, 0, sizeof ip);
    memset(&op, 0, sizeof op);
    ive_video_encode_ip_t *x = &ip.s_ive_ip;
    x->u4_size = sizeof ip; x->e_cmd = IVE_CMD_VIDEO_ENCODE;
    op.s_ive_op.u4_size = sizeof op;
    x->s_out_buf.pv_buf = e->out; x->s_out_buf.u4_bufsize = e->out_cap;
    x->s_inp_buf.e_color_fmt = IV_YUV_420P;
    if (b) {
        size_t y = (size_t)e->width * (size_t)e->height;
        x->s_inp_buf.apv_bufs[0] = b->buf;
        x->s_inp_buf.apv_bufs[1] = b->buf + y;
        x->s_inp_buf.apv_bufs[2] = b->buf + y + y / 4;
        x->s_inp_buf.au4_wd[0] = (uint32_t)e->width;   x->s_inp_buf.au4_ht[0] = (uint32_t)e->height;
        x->s_inp_buf.au4_wd[1] = x->s_inp_buf.au4_wd[2] = (uint32_t)e->width / 2;
        x->s_inp_buf.au4_ht[1] = x->s_inp_buf.au4_ht[2] = (uint32_t)e->height / 2;
        x->s_inp_buf.au4_strd[0] = (uint32_t)e->width;
        x->s_inp_buf.au4_strd[1] = x->s_inp_buf.au4_strd[2] = (uint32_t)e->width / 2;
        b->busy = 1;
    }
    x->u4_timestamp_low = (uint32_t)((uint64_t)pts_us & 0xFFFFFFFFu);
    x->u4_timestamp_high = (uint32_t)((uint64_t)pts_us >> 32);
    x->u4_is_last = drain ? 1 : 0;
    if (ih264e_api_function(e->enc, &ip, &op) != IV_SUCCESS) {
        seterr(err, errcap, "h264: encoding 0x%x", op.s_ive_op.u4_error_code);
        return -1;
    }
    /* The input buffer the encoder has finished with, which may be an earlier one. */
    for (int i = 0; i < INPUT_BUFS; i++)
        if (op.s_ive_op.s_inp_buf.apv_bufs[0] == e->in[i].buf) e->in[i].busy = 0;
    if (op.s_ive_op.output_present && op.s_ive_op.s_out_buf.u4_bytes) {
        int64_t out_pts = (int64_t)(((uint64_t)op.s_ive_op.u4_timestamp_high << 32) | op.s_ive_op.u4_timestamp_low);
        if (oc_mp4_export_video(e->mp4, op.s_ive_op.s_out_buf.pv_buf, op.s_ive_op.s_out_buf.u4_bytes, out_pts) != 0) {
            seterr(err, errcap, "writing the copy failed");
            return -1;
        }
    }
    if (last) *last = op.s_ive_op.u4_is_last != 0;
    return 0;
}

/* ---- MP3 ------------------------------------------------------------------------- */

/* The length of the MPEG-1 Layer III frame at `p`, or 0 if there is none. */
static size_t mp3_frame_len(const uint8_t *p, size_t n) {
    static const unsigned kbps[16] = { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 };
    static const unsigned rates[4] = { 44100, 48000, 32000, 0 };
    if (n < 4 || p[0] != 0xFF || (p[1] & 0xFE) != 0xFA) return 0;   /* sync, MPEG-1, Layer III */
    unsigned br = kbps[p[2] >> 4], sr = rates[(p[2] >> 2) & 3], pad = (p[2] >> 1) & 1;
    if (!br || !sr) return 0;
    return 144000u * br / sr + pad;
}

/* Hand every whole frame the encoder has produced to the file. */
static int mp3_emit(engine *e, char *err, size_t errcap) {
    size_t at = 0;
    for (;;) {
        size_t n = mp3_frame_len(e->mp3 + at, e->mp3_n - at);
        if (n == 0) {
            if (e->mp3_n - at >= 4) { seterr(err, errcap, "mp3: lost frame sync"); return -1; }
            break;
        }
        if (e->mp3_n - at < n) break;
        if (oc_mp4_export_audio(e->mp4, e->mp3 + at, n, MP3_FRAME) != 0) {
            seterr(err, errcap, "writing the copy failed");
            return -1;
        }
        at += n;
    }
    memmove(e->mp3, e->mp3 + at, e->mp3_n - at);
    e->mp3_n -= at;
    return 0;
}

static int mp3_room(engine *e, size_t more) {
    if (e->mp3_n + more <= e->mp3_cap) return 0;
    size_t cap = e->mp3_cap ? e->mp3_cap : 16384;
    while (cap < e->mp3_n + more) cap *= 2;
    uint8_t *p = realloc(e->mp3, cap);
    if (!p) return -1;
    e->mp3 = p; e->mp3_cap = cap;
    return 0;
}

/* ---- the codecs ------------------------------------------------------------------ */

static void engine_free(engine *e) {
    if (e->vdec) oc_vp9dec_close(e->vdec);
    if (e->adec) oc_opusdec_close(e->adec);
    if (e->lame) lame_close(e->lame);
    free_encoder(e);
    free(e->pcm);
    free(e->mp3);
    free(e);
}

static void eng_abort(void *v) {
    engine *e = v;
    oc_mp4_export_abort(e->mp4);
    engine_free(e);
}

static void *eng_open(const char *out_path, const oc_export_params *p, char *err, size_t errcap) {
    if ((p->width & 1) || (p->height & 1) || p->width < 16 || p->height < 16 || p->width > 1920 || p->height > 1920) {
        seterr(err, errcap, "unsupported picture size %dx%d", p->width, p->height);
        return NULL;
    }
    engine *e = calloc(1, sizeof *e);
    if (!e) { seterr(err, errcap, "out of memory"); return NULL; }
    e->mp4 = oc_mp4_export_open(out_path, p->width, p->height, 48000);
    e->vdec = oc_vp9dec_open_threads(1);
    if (!e->mp4 || !e->vdec) { seterr(err, errcap, "opening the copy or the decoder failed"); goto fail; }
    if (open_encoder(e, p->width, p->height, p->fps, err, errcap) != 0) goto fail;
    if (p->has_audio) {
        e->adec = oc_opusdec_open(p->opus_channels == 2 ? 2 : 1);
        e->pcm = malloc(OPUS_MAX_FRAME * sizeof *e->pcm);
        e->lame = lame_init();
        if (!e->adec || !e->pcm || !e->lame) { seterr(err, errcap, "opening the audio failed"); goto fail; }
        lame_set_num_channels(e->lame, 1);
        lame_set_in_samplerate(e->lame, OC_OPUS_RATE);
        lame_set_out_samplerate(e->lame, OC_OPUS_RATE);
        lame_set_mode(e->lame, MONO);
        lame_set_VBR(e->lame, vbr_off);
        lame_set_brate(e->lame, MP3_KBPS);
        lame_set_bWriteVbrTag(e->lame, 0);                 /* no Xing frame: the MP4 is the index */
        lame_set_quality(e->lame, 5);
        if (lame_init_params(e->lame) < 0) { seterr(err, errcap, "mp3: configuring the encoder"); goto fail; }
        e->mp3_delay = (uint32_t)lame_get_encoder_delay(e->lame) + MP3_DECODER_DELAY;
        e->preskip_left = p->opus_preskip;
    }
    /* The parameter sets first: the writer keeps them for the sample entry. */
    if (set_mode(e, IVE_ENC_MODE_HEADER) != 0 || encode(e, NULL, 0, 0, NULL, err, errcap) != 0 ||
        set_mode(e, IVE_ENC_MODE_PICTURE) != 0) {
        if (!err || !err[0]) seterr(err, errcap, "h264: writing the headers");
        goto fail;
    }
    return e;
fail:
    eng_abort(e);
    return NULL;
}

static int eng_video(void *v, const uint8_t *data, size_t len, int64_t pts_us, char *err, size_t errcap) {
    engine *e = v;
    oc_frame f;
    int r = oc_vp9dec_decode(e->vdec, data, len, pts_us, &f);
    if (r < 0) { seterr(err, errcap, "vp9: decoding failed"); return -1; }
    if (r > 0) return 0;                                   /* no picture this time */
    if (f.width != e->width || f.height != e->height) { seterr(err, errcap, "vp9: the picture changed size"); return -1; }
    inbuf *b = NULL;
    for (int i = 0; i < INPUT_BUFS && !b; i++) if (!e->in[i].busy) b = &e->in[i];
    if (!b) { seterr(err, errcap, "h264: the encoder kept every input"); return -1; }
    /* Into one contiguous I420 frame, as libavc takes it. */
    uint8_t *dst = b->buf;
    for (int p = 0; p < 3; p++) {
        int w = p ? e->width / 2 : e->width, h = p ? e->height / 2 : e->height;
        for (int y = 0; y < h; y++, dst += w) memcpy(dst, f.plane[p] + (size_t)y * (size_t)f.stride[p], (size_t)w);
    }
    return encode(e, b, pts_us, 0, NULL, err, errcap);
}

static int eng_audio(void *v, const uint8_t *data, size_t len, char *err, size_t errcap) {
    engine *e = v;
    if (!e->adec) return 0;
    int n = oc_opusdec_decode(e->adec, data, len, e->pcm, OPUS_MAX_FRAME);
    if (n < 0) { seterr(err, errcap, "opus: decoding failed"); return -1; }
    int16_t *pcm = e->pcm;
    if (e->preskip_left) {                                 /* the source's own pre-roll */
        unsigned skip = e->preskip_left < (unsigned)n ? e->preskip_left : (unsigned)n;
        e->preskip_left -= skip; pcm += skip; n -= (int)skip;
    }
    if (n <= 0) return 0;
    size_t room = (size_t)n * 5 / 4 + 7200;               /* LAME's own bound */
    if (mp3_room(e, room) != 0) { seterr(err, errcap, "out of memory"); return -1; }
    int out = lame_encode_buffer(e->lame, pcm, pcm, n, e->mp3 + e->mp3_n, (int)room);
    if (out < 0) { seterr(err, errcap, "mp3: encoding failed"); return -1; }
    e->mp3_n += (size_t)out;
    e->pcm_samples += (uint64_t)n;
    return mp3_emit(e, err, errcap);
}

static int eng_finish(void *v, uint64_t *bytes, uint32_t *ms, char *err, size_t errcap) {
    engine *e = v;
    /* Whatever the decoder still holds, then whatever the encoder does. */
    int last = 0;
    for (int guard = 0; !last && guard < 64; guard++)
        if (encode(e, NULL, 0, 1, &last, err, errcap) != 0) { eng_abort(e); return -1; }
    if (e->lame) {
        if (mp3_room(e, 7200) != 0) { eng_abort(e); return -1; }
        int out = lame_encode_flush(e->lame, e->mp3 + e->mp3_n, 7200);
        if (out < 0) { seterr(err, errcap, "mp3: flushing failed"); eng_abort(e); return -1; }
        e->mp3_n += (size_t)out;
        if (mp3_emit(e, err, errcap) != 0) { eng_abort(e); return -1; }
    }
    int rc = oc_mp4_export_finish(e->mp4, e->mp3_delay, e->pcm_samples, bytes, ms);
    e->mp4 = NULL;
    if (rc != 0) seterr(err, errcap, "writing the copy failed");
    engine_free(e);
    return rc;
}

static const oc_export_codecs REAL = {
    EXPORT_VERSION, eng_open, eng_video, eng_audio, eng_finish, eng_abort,
};

const oc_export_codecs *oc_export_codecs_real(void) { return &REAL; }
