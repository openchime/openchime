/* The recording's real codecs (callrec_render.h): libopus and libvpx, through
 * the wrappers the clients use (oc_opusdec, oc_vp9dec, oc_codec.h). In a daemon
 * built with CALLREC=1, and in the test binary's media suite. */
#include "callrec_render.h"
#include "oc_codec.h"

#include <stdlib.h>
#include <string.h>

static void *adec_open(void) { return oc_opusdec_open_rate(OC_CALLREC_RATE); }
static int adec(void *d, const uint8_t *pkt, size_t len, int16_t *pcm) {
    int n = oc_opusdec_decode_frame(d, pkt, pkt ? len : 0, 0, pcm, OC_CALLREC_FRAME);
    if (n != OC_CALLREC_FRAME) { memset(pcm, 0, OC_CALLREC_FRAME * sizeof *pcm); return n < 0 && pkt ? -1 : 0; }
    return 0;
}
static void adec_close(void *d) { oc_opusdec_close(d); }

static void *vdec_open(void) { return oc_vp9dec_open_threads(1); }
static int vdec(void *d, const uint8_t *data, size_t len, oc_frame *out) {
    return oc_vp9dec_decode(d, data, len, 0, out);
}
static void vdec_close(void *d) { oc_vp9dec_close(d); }

static void *venc_open(void) {
    oc_vp9enc *e = oc_vp9enc_open(OC_CALLREC_WIDTH, OC_CALLREC_HEIGHT, OC_CALLREC_FPS);
    if (e && oc_vp9enc_set_bitrate(e, OC_CALLREC_KBPS) != 0) { oc_vp9enc_close(e); e = NULL; }
    return e;
}
typedef struct { void (*emit)(void *, const uint8_t *, size_t, int); void *ctx; } venc_ctx;
static void venc_emit(void *ctx, const oc_packet *p) {
    venc_ctx *v = ctx;
    v->emit(v->ctx, p->data, p->len, p->keyframe);
}
static int venc(void *e, const oc_frame *f, int force_key,
                void (*emit)(void *ctx, const uint8_t *data, size_t len, int keyframe), void *ctx) {
    venc_ctx v = { emit, ctx };
    return oc_vp9enc_encode(e, f, force_key, venc_emit, &v);
}
static void venc_close(void *e) { oc_vp9enc_close(e); }

static void *aenc_open(void) { return oc_opusenc_open(); }
static int aenc(void *e, const int16_t *pcm, uint8_t *out, size_t cap) { return oc_opusenc_encode(e, pcm, out, cap); }
static void aenc_close(void *e) { oc_opusenc_close(e); }

static const oc_callrec_codecs REAL = {
    "libvpx-vp9-1280x720@15-450k+libopus-48k-mono-1",
    adec_open, adec, adec_close,
    vdec_open, vdec, vdec_close,
    venc_open, venc, venc_close,
    aenc_open, aenc, aenc_close,
};

const oc_callrec_codecs *oc_callrec_codecs_real(void) { return &REAL; }

#ifdef OC_CALLREC
const oc_callrec_codecs *oc_callrec_codecs_builtin(void) { return &REAL; }
#endif
