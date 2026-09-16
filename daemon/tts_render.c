/* One read-aloud render (tts_render.h). */
#define _POSIX_C_SOURCE 200809L
#include "tts_render.h"

#include "oc_mp4.h"
#include "speakable.h"

#include <opus/opus.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void seterr(char *err, size_t cap, const char *fmt, ...) {
    if (!err || !cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

#define MAX_SEGMENTS 256

oc_tts_status oc_tts_render(const oc_tts_engine *e, void *engine, const char *text, int voice,
                            uint8_t **mp4, size_t *len, uint32_t *duration_ms,
                            char *err, size_t errcap) {
    *mp4 = NULL;
    *len = 0;
    *duration_ms = 0;
    if (!e || !engine || !text || voice < 0 || voice >= e->voices) { seterr(err, errcap, "bad render request"); return OC_TTS_FAILED; }
    unsigned rate = e->rate;
    if (rate != 8000 && rate != 12000 && rate != 16000 && rate != 24000 && rate != 48000) {
        seterr(err, errcap, "engine rate %u is not an Opus rate", rate);
        return OC_TTS_FAILED;
    }
    /* A 20 ms packet is `frame` input samples and always 960 at the file's 48 kHz. */
    int frame = (int)(rate / 50);

    int oerr;
    OpusEncoder *enc = opus_encoder_create((opus_int32)rate, 1, OPUS_APPLICATION_AUDIO, &oerr);
    if (oerr != OPUS_OK || !enc) { seterr(err, errcap, "opus encoder: %s", opus_strerror(oerr)); return OC_TTS_FAILED; }
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(OC_TTS_BITRATE));
    opus_encoder_ctl(enc, OPUS_SET_VBR(1));
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(10));
    opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    /* The file's pre-skip is fixed; the encoder's look-ahead, scaled to 48 kHz,
     * must be what it covers or the start would be clipped or delayed. */
    opus_int32 lookahead = 0;
    opus_encoder_ctl(enc, OPUS_GET_LOOKAHEAD(&lookahead));
    if ((uint32_t)lookahead * (48000u / rate) != OC_MP4_OPUS_PRESKIP) {
        seterr(err, errcap, "opus look-ahead %d does not match the file's pre-skip", (int)lookahead);
        opus_encoder_destroy(enc);
        return OC_TTS_FAILED;
    }
    oc_mp4_writer *w = oc_mp4_writer_open_audio(rate);
    if (!w) { opus_encoder_destroy(enc); seterr(err, errcap, "out of memory"); return OC_TTS_FAILED; }

    size_t tlen = strlen(text), starts[MAX_SEGMENTS], lens[MAX_SEGMENTS];
    int segs = oc_speakable_segments(text, tlen, OC_TTS_SEGMENT_CHARS, starts, lens, MAX_SEGMENTS);
    float *pending = malloc((size_t)frame * sizeof *pending);   /* samples short of a whole packet */
    opus_int16 *pcm16 = malloc((size_t)frame * sizeof *pcm16);
    unsigned char packet[1500];
    size_t npend = 0;
    int said = 0;
    oc_tts_status st = OC_TTS_OK;
    if (!pending || !pcm16) { seterr(err, errcap, "out of memory"); st = OC_TTS_FAILED; }

    for (int s = 0; s < segs && st == OC_TTS_OK; s++) {
        char *seg = malloc(lens[s] + 1);
        if (!seg) { seterr(err, errcap, "out of memory"); st = OC_TTS_FAILED; break; }
        memcpy(seg, text + starts[s], lens[s]);
        seg[lens[s]] = '\0';
        float *pcm = NULL;
        size_t n = 0;
        int r = e->say(engine, seg, voice, &pcm, &n, err, errcap);
        free(seg);
        if (r < 0) { st = OC_TTS_FAILED; free(pcm); break; }
        if (r > 0 || n == 0) { free(pcm); continue; }
        said = 1;
        /* Feed the segment's samples into whole packets, carrying the remainder. */
        for (size_t i = 0; i < n && st == OC_TTS_OK;) {
            size_t take = (size_t)frame - npend;
            if (take > n - i) take = n - i;
            memcpy(pending + npend, pcm + i, take * sizeof *pcm);
            npend += take;
            i += take;
            if (npend == (size_t)frame) {
                for (int k = 0; k < frame; k++) {
                    float v = pending[k] * 32767.0f;
                    pcm16[k] = (opus_int16)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : v);
                }
                opus_int32 bytes = opus_encode(enc, pcm16, frame, packet, sizeof packet);
                if (bytes < 0 || oc_mp4_write_audio(w, packet, (size_t)bytes, 960) != 0) {
                    seterr(err, errcap, "encoding failed");
                    st = OC_TTS_FAILED;
                }
                npend = 0;
            }
        }
        free(pcm);
    }
    /* The last partial packet, padded with silence, then enough silence to push
     * the look-ahead's worth of real audio out of the encoder. */
    if (st == OC_TTS_OK && said) {
        int flush_packets = (npend ? 1 : 0) + 1;
        for (int f = 0; f < flush_packets && st == OC_TTS_OK; f++) {
            for (int k = 0; k < frame; k++) {
                float v = (size_t)k < npend ? pending[k] * 32767.0f : 0.0f;
                pcm16[k] = (opus_int16)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : v);
            }
            npend = 0;
            opus_int32 bytes = opus_encode(enc, pcm16, frame, packet, sizeof packet);
            if (bytes < 0 || oc_mp4_write_audio(w, packet, (size_t)bytes, 960) != 0) {
                seterr(err, errcap, "encoding failed");
                st = OC_TTS_FAILED;
            }
        }
    }
    free(pending);
    free(pcm16);
    opus_encoder_destroy(enc);

    if (st == OC_TTS_OK && !said) st = OC_TTS_NOTHING;
    if (st != OC_TTS_OK) { oc_mp4_writer_abort(w); return st; }
    if (oc_mp4_writer_finish(w, mp4, len, duration_ms) != 0) { seterr(err, errcap, "writing the file failed"); return OC_TTS_FAILED; }
    return OC_TTS_OK;
}
