/* Opus decode over libopus (oc_opusdec.h). */
#include "oc_opusdec.h"

#include <stdlib.h>

#include <opus/opus.h>

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

oc_opusdec *oc_opusdec_open_rate(int rate) {
    oc_opusdec *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    int err;
    d->dec = opus_decoder_create(rate, 1, &err);
    if (err != OPUS_OK || !d->dec) { free(d); return NULL; }
    d->channels = 1;
    return d;
}

int oc_opusdec_decode_frame(oc_opusdec *d, const uint8_t *data, size_t len, int fec,
                            int16_t *pcm, int frame) {
    if (frame <= 0 || frame > 5760 || len > 0x7FFFFFFF) return -1;
    /* A NULL packet asks libopus to conceal; a packet with decode_fec set asks
     * for the frame before it, from the redundancy it carries. Either way the
     * frame size must be the one that was lost. */
    int n = opus_decode(d->dec, data && len ? data : NULL, data && len ? (opus_int32)len : 0,
                        pcm, frame, data && len ? fec : 0);
    return n < 0 ? -1 : n;
}

void oc_opusdec_close(oc_opusdec *d) {
    if (!d) return;
    opus_decoder_destroy(d->dec);
    free(d);
}
