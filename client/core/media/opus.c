/* Opus encode over libopus (oc_codec.h); the decoder is shared (shared/oc_opusdec.c). */
#include "oc_codec.h"

#include <stdlib.h>

#include <opus/opus.h>

struct oc_opusenc { OpusEncoder *enc; int frame; };

oc_opusenc *oc_opusenc_open(void) {
    oc_opusenc *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    int err;
    e->enc = opus_encoder_create(OC_OPUS_RATE, 1, OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK || !e->enc) { free(e); return NULL; }
    opus_encoder_ctl(e->enc, OPUS_SET_BITRATE(48000));
    opus_encoder_ctl(e->enc, OPUS_SET_VBR(1));
    opus_encoder_ctl(e->enc, OPUS_SET_COMPLEXITY(8));
    opus_encoder_ctl(e->enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    e->frame = OC_OPUS_FRAME;
    return e;
}

oc_opusenc *oc_opusenc_open_voice(int kbps) {
    oc_opusenc *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    int err;
    e->enc = opus_encoder_create(OC_VOICE_RATE, 1, OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK || !e->enc) { free(e); return NULL; }
    opus_encoder_ctl(e->enc, OPUS_SET_BITRATE(kbps * 1000));
    opus_encoder_ctl(e->enc, OPUS_SET_VBR(1));
    opus_encoder_ctl(e->enc, OPUS_SET_COMPLEXITY(8));
    opus_encoder_ctl(e->enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(e->enc, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(e->enc, OPUS_SET_PACKET_LOSS_PERC(5));   /* until the network says otherwise */
    opus_encoder_ctl(e->enc, OPUS_SET_DTX(1));
    e->frame = OC_VOICE_FRAME;
    return e;
}

void oc_opusenc_set_loss(oc_opusenc *e, int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    opus_encoder_ctl(e->enc, OPUS_SET_PACKET_LOSS_PERC(percent));
}

int oc_opusenc_encode_voice(oc_opusenc *e, const int16_t *pcm, uint8_t *out, size_t cap) {
    if (cap > 0x7FFFFFFF) cap = 0x7FFFFFFF;
    int n = opus_encode(e->enc, pcm, e->frame, out, (opus_int32)cap);
    return n < 0 ? -1 : n;
}

int oc_opusenc_lookahead(oc_opusenc *e) {
    opus_int32 la = 0;
    opus_encoder_ctl(e->enc, OPUS_GET_LOOKAHEAD(&la));
    return (int)la;
}

int oc_opusenc_encode(oc_opusenc *e, const int16_t *pcm, uint8_t *out, size_t cap) {
    if (cap > 0x7FFFFFFF) cap = 0x7FFFFFFF;
    int n = opus_encode(e->enc, pcm, OC_OPUS_FRAME, out, (opus_int32)cap);
    return n < 0 ? -1 : n;
}

void oc_opusenc_close(oc_opusenc *e) {
    if (!e) return;
    opus_encoder_destroy(e->enc);
    free(e);
}
