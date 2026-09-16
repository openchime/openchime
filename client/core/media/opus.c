/* Opus encode and decode over libopus (oc_codec.h). */
#include "oc_codec.h"

#include <stdlib.h>

#include <opus/opus.h>

struct oc_opusenc { OpusEncoder *enc; };

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
    return e;
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

struct oc_opusdec { OpusDecoder *dec; int channels; int16_t buf[5760 * 2]; };

oc_opusdec *oc_opusdec_open(int channels) {
    if (channels != 1 && channels != 2) return NULL;
    oc_opusdec *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    int err;
    d->dec = opus_decoder_create(OC_OPUS_RATE, channels, &err);
    if (err != OPUS_OK || !d->dec) { free(d); return NULL; }
    d->channels = channels;
    return d;
}

int oc_opusdec_decode(oc_opusdec *d, const uint8_t *data, size_t len, int16_t *pcm, int cap) {
    if (!data || len == 0 || len > 0x7FFFFFFF || cap <= 0) return -1;
    int n = opus_decode(d->dec, data, (opus_int32)len, d->buf, 5760, 0);
    if (n < 0) return -1;
    if (n > cap) n = cap;
    if (d->channels == 1) {
        for (int i = 0; i < n; i++) pcm[i] = d->buf[i];
    } else {
        for (int i = 0; i < n; i++) pcm[i] = (int16_t)((d->buf[2 * i] + d->buf[2 * i + 1]) / 2);
    }
    return n;
}

void oc_opusdec_close(oc_opusdec *d) {
    if (!d) return;
    opus_decoder_destroy(d->dec);
    free(d);
}
