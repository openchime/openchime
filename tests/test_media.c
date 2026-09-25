/* Tests for the video message media core (REQ-162–165, ARCH-110,
 * docs/VIDEO-MESSAGES.md §11): the MP4 writer and reader (including a mutation
 * fuzz of the reader), the VP9 and Opus round trips, the recorder against the
 * synthetic camera and tone, ffprobe's view of a recorded file, and the player
 * with the real-time test sink. */
#define _POSIX_C_SOURCE 200809L
#include "check.h"
#include "oc_capture.h"
#include "oc_codec.h"
#include "oc_mp4.h"
#include "oc_player.h"
#include "oc_recorder.h"
#include "audio_dev.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void msleep(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* ---- MP4 --------------------------------------------------------------------------- */

/* A file of recognisable fake samples: video packet i is i+1 bytes of (i & 0xFF)
 * at 30 fps with a keyframe every 10; audio packet j is 20 bytes of 0xA0|j&15. */
static int build_fake(uint8_t **out, size_t *len, uint32_t *dur, int nv, int na) {
    oc_mp4_writer *w = oc_mp4_writer_open(640, 360);
    if (!w) return -1;
    uint8_t buf[512];
    int vi = 0, ai = 0;
    while (vi < nv || ai < na) {
        /* Interleave by time, as the recorder does. */
        int64_t vt = (int64_t)vi * 1000000 / 30, at = (int64_t)ai * 20000;
        if (vi < nv && (ai >= na || vt <= at)) {
            memset(buf, vi & 0xFF, (size_t)vi % 400 + 1);
            if (oc_mp4_write_video(w, buf, (size_t)vi % 400 + 1, 5000000 + vt, vi % 10 == 0) != 0) { oc_mp4_writer_abort(w); return -1; }
            vi++;
        } else {
            memset(buf, 0xA0 | (ai & 15), 20);
            if (oc_mp4_write_audio(w, buf, 20, 960) != 0) { oc_mp4_writer_abort(w); return -1; }
            ai++;
        }
    }
    return oc_mp4_writer_finish(w, out, len, dur);
}

static void test_mp4_roundtrip(void) {
    uint8_t *file = NULL; size_t len = 0; uint32_t dur = 0;
    CHECK(build_fake(&file, &len, &dur, 90, 150) == 0);
    if (!file) return;
    /* moov before mdat: ftyp, then moov at offset 28. */
    CHECK(len > 64 && memcmp(file + 4, "ftyp", 4) == 0 && memcmp(file + 28 + 4, "moov", 4) == 0);

    oc_mp4_info info;
    CHECK(oc_mp4_parse(file, len, &info) == 0);
    CHECK(info.width == 640 && info.height == 360);
    CHECK(info.video.present && info.video.n_samples == 90 && info.video.timescale == 90000);
    CHECK(info.audio.present && info.audio.n_samples == 150 && info.audio.timescale == 48000);
    CHECK(info.opus_channels == 1 && info.opus_preskip == 312);
    int ok = 1;
    for (uint32_t i = 0; i < info.video.n_samples; i++) {
        const oc_mp4_sample *s = &info.video.samples[i];
        if (s->size != i % 400 + 1 || s->dts != (uint64_t)i * 3000 || s->sync != (i % 10 == 0)) ok = 0;
        for (uint32_t k = 0; k < s->size; k++) if (file[s->offset + k] != (i & 0xFF)) ok = 0;
    }
    CHECK(ok);
    ok = 1;
    for (uint32_t j = 0; j < info.audio.n_samples; j++) {
        const oc_mp4_sample *s = &info.audio.samples[j];
        if (s->size != 20 || s->dts != (uint64_t)j * 960 || file[s->offset] != (0xA0 | (j & 15))) ok = 0;
    }
    CHECK(ok);
    /* Video lasts 90 frames = 3000 ms; audio 150×20 ms less the pre-skip. */
    CHECK(info.duration_ms == 3000 && dur == 3000);
    CHECK(oc_mp4_keyframe_before(&info, 45 * 3000) == 40);
    CHECK(oc_mp4_keyframe_before(&info, 0) == 0);
    oc_mp4_info_free(&info);

    /* A first frame that is not a keyframe, and time going backwards, are refused. */
    oc_mp4_writer *w = oc_mp4_writer_open(64, 64);
    uint8_t b[4] = {1, 2, 3, 4};
    CHECK(oc_mp4_write_video(w, b, 4, 0, 0) != 0);
    CHECK(oc_mp4_write_video(w, b, 4, 100, 1) == 0);
    CHECK(oc_mp4_write_video(w, b, 4, 50, 0) != 0);
    oc_mp4_writer_abort(w);
    free(file);
}

/* Audio only, as a read-aloud render is: one Opus track, no video, accepted by
 * the same reader; a video writer still needs its video, an audio writer its audio. */
static void test_mp4_audio_only(void) {
    oc_mp4_writer *w = oc_mp4_writer_open_audio(24000);
    CHECK(w != NULL);
    if (!w) return;
    uint8_t pkt[24];
    int ok = 1;
    for (unsigned j = 0; j < 100; j++) {
        memset(pkt, (int)(0xB0 | (j & 15)), sizeof pkt);
        if (oc_mp4_write_audio(w, pkt, 10 + j % 14, 960) != 0) ok = 0;
    }
    CHECK(ok);
    uint8_t b[4] = {1, 2, 3, 4};
    CHECK(oc_mp4_write_video(w, b, 4, 0, 1) != 0);
    uint8_t *file = NULL; size_t len = 0; uint32_t dur = 0;
    CHECK(oc_mp4_writer_finish(w, &file, &len, &dur) == 0);
    if (!file) return;
    oc_mp4_info info;
    CHECK(oc_mp4_parse(file, len, &info) == 0);
    CHECK(!info.video.present && info.width == 0);
    CHECK(info.audio.present && info.audio.n_samples == 100 && info.audio.timescale == 48000);
    CHECK(info.opus_channels == 1 && info.opus_preskip == 312);
    ok = 1;
    for (uint32_t j = 0; j < info.audio.n_samples; j++) {
        const oc_mp4_sample *s = &info.audio.samples[j];
        if (s->size != 10 + j % 14 || s->dts != (uint64_t)j * 960 || file[s->offset] != (0xB0 | (j & 15))) ok = 0;
    }
    CHECK(ok);
    /* 100 × 20 ms less the 312-sample pre-skip (6.5 ms). */
    CHECK(info.duration_ms == 1993 && dur == 1993);
    CHECK(oc_mp4_keyframe_before(&info, 0) == -1);
    oc_mp4_info_free(&info);
    free(file);

    w = oc_mp4_writer_open_audio(24000);
    CHECK(oc_mp4_writer_finish(w, &file, &len, &dur) != 0);         /* nothing written */
    w = oc_mp4_writer_open(64, 64);
    CHECK(oc_mp4_write_audio(w, pkt, 20, 960) == 0);
    CHECK(oc_mp4_writer_finish(w, &file, &len, &dur) != 0);         /* a video file with no video */
    CHECK(oc_mp4_writer_open_audio(0) == NULL);
}

/* A read-aloud rendering plays: an audio-only file has no picture, so the player
 * runs on the audio clock alone and ends where the audio does (ARCH-111). */
static void test_player_audio_only(void) {
    setenv("OPENCHIME_TEST_AUDIO", "synthetic", 1);
    oc_opusenc *enc = oc_opusenc_open();
    CHECK(enc != NULL);
    if (!enc) return;
    oc_mp4_writer *w = oc_mp4_writer_open_audio(OC_OPUS_RATE);
    CHECK(w != NULL);
    int16_t pcm[OC_OPUS_FRAME];
    uint8_t pkt[OC_OPUS_MAX_PACKET];
    int ok = 1;
    for (int f = 0; f < 50; f++) {                        /* one second */
        for (int i = 0; i < OC_OPUS_FRAME; i++)
            pcm[i] = (int16_t)(8000 * sin(2 * M_PI * 440.0 * (f * OC_OPUS_FRAME + i) / OC_OPUS_RATE));
        int n = oc_opusenc_encode(enc, pcm, pkt, sizeof pkt);
        if (n <= 0 || oc_mp4_write_audio(w, pkt, (size_t)n, OC_OPUS_FRAME) != 0) ok = 0;
    }
    oc_opusenc_close(enc);
    CHECK(ok);
    uint8_t *file = NULL; size_t len = 0; uint32_t dur = 0;
    CHECK(oc_mp4_writer_finish(w, &file, &len, &dur) == 0);
    if (!file) return;

    oc_player *p = oc_player_open(file, len);
    CHECK(p != NULL);
    if (p) {
        oc_player_play(p);
        oc_player_status st;
        /* It must REACH THE END, not merely start: the clock of an audio-only
         * file is the speaker's own count of what it has consumed, so any frame
         * handed over and not counted leaves the clock short of the duration for
         * ever. The player then stays PLAYING on a file that is long finished,
         * which is silent to every other assertion here -- and to a listener it
         * is a channel that reads one message aloud and then nothing, ever
         * (REQ-291). Two seconds for one second of audio. */
        int ended = 0;
        for (int i = 0; i < 200 && !ended; i++) {
            oc_player_status_get(p, &st);
            if (st.state == OC_PLAYER_ENDED) ended = 1;
            else msleep(10);
        }
        oc_player_status_get(p, &st);
        CHECK(ended && st.has_audio && st.duration_ms == dur);
        CHECK(st.position_ms == dur);
        CHECK(st.state != OC_PLAYER_ERROR);
        /* No picture, ever: a frame request is answered with nothing rather than
         * with a stale or invented one. */
        CHECK(oc_player_frame(p, NULL, 0, NULL, NULL, NULL) == 0);
        oc_player_close(p);
    }
    free(file);
    unsetenv("OPENCHIME_TEST_AUDIO");
}

static uint32_t rng_state = 12345;
static uint32_t rnd(void) { rng_state = rng_state * 1103515245u + 12345u; return rng_state >> 8; }

static void test_mp4_fuzz(void) {
    uint8_t *file = NULL; size_t len = 0; uint32_t dur;
    CHECK(build_fake(&file, &len, &dur, 60, 100) == 0);
    if (!file) return;
    uint8_t *m = malloc(len);
    int accepted = 0, rejected = 0;
    for (int iter = 0; iter < 3000; iter++) {
        memcpy(m, file, len);
        size_t n = len;
        int kind = iter % 4;
        if (kind == 0) {                          /* flip bytes in the header region */
            int flips = 1 + (int)(rnd() % 8);
            for (int f = 0; f < flips; f++) m[rnd() % (len < 4096 ? len : 4096)] ^= (uint8_t)(1 + rnd() % 255);
        } else if (kind == 1) {                   /* truncate */
            n = rnd() % len;
        } else if (kind == 2) {                   /* a box size set to something hostile */
            size_t at = rnd() % (len < 2048 ? len - 4 : 2044);
            uint32_t v = (rnd() % 3 == 0) ? 0xFFFFFFFFu : rnd();
            m[at] = (uint8_t)(v >> 24); m[at + 1] = (uint8_t)(v >> 16); m[at + 2] = (uint8_t)(v >> 8); m[at + 3] = (uint8_t)v;
        } else {                                  /* random bytes throughout */
            for (size_t k = 0; k < 64; k++) m[rnd() % len] = (uint8_t)rnd();
        }
        oc_mp4_info info;
        if (oc_mp4_parse(m, n, &info) == 0) {
            /* Whatever is accepted must be internally consistent. */
            int ok = 1;
            for (uint32_t i = 0; i < info.video.n_samples; i++)
                if (info.video.samples[i].offset + info.video.samples[i].size > n) ok = 0;
            for (uint32_t i = 0; i < info.audio.n_samples; i++)
                if (info.audio.samples[i].offset + info.audio.samples[i].size > n) ok = 0;
            CHECK(ok);
            accepted++;
            oc_mp4_info_free(&info);
        } else {
            rejected++;
        }
    }
    CHECK(rejected > 0);
    printf("  mp4 fuzz: %d accepted, %d rejected\n", accepted, rejected);
    /* Pure noise is refused. */
    for (size_t k = 0; k < len; k++) m[k] = (uint8_t)rnd();
    oc_mp4_info info;
    CHECK(oc_mp4_parse(m, len, &info) != 0);
    CHECK(oc_mp4_parse(NULL, 0, &info) != 0);
    free(m);
    free(file);
}

/* ---- codecs ------------------------------------------------------------------------ */

typedef struct { uint8_t *d[64]; size_t n[64]; int64_t pts[64]; int key[64]; int count; } pkts;

static void keep_packet(void *ctx, const oc_packet *p) {
    pkts *k = ctx;
    if (k->count >= 64) return;
    k->d[k->count] = malloc(p->len);
    memcpy(k->d[k->count], p->data, p->len);
    k->n[k->count] = p->len; k->pts[k->count] = p->pts_us; k->key[k->count] = p->keyframe;
    k->count++;
}

static double psnr_y(const oc_frame *a, const oc_frame *b) {
    double se = 0;
    for (int y = 0; y < a->height; y++)
        for (int x = 0; x < a->width; x++) {
            int d = a->plane[0][y * a->stride[0] + x] - b->plane[0][y * b->stride[0] + x];
            se += d * d;
        }
    double mse = se / (a->width * a->height);
    return mse <= 0 ? 99.0 : 10.0 * log10(255.0 * 255.0 / mse);
}

static void test_vp9_roundtrip(void) {
    setenv("OPENCHIME_TEST_CAPTURE", "synthetic", 1);
    int err;
    oc_capture *cam = oc_capture_open(NULL, 1280, 720, 30, &err);
    CHECK(cam != NULL && err == OC_CAP_OK);
    if (!cam) return;
    CHECK(oc_capture_start(cam) == OC_CAP_OK);
    oc_vp9enc *enc = oc_vp9enc_open(1280, 720, 30);
    oc_vp9dec *dec = oc_vp9dec_open();
    CHECK(enc && dec);
    oc_frame src[30];
    pkts k = {0};
    int frames = 0;
    for (int i = 0; i < 30; i++) {
        oc_frame f;
        if (oc_capture_next(cam, &f, 500) != 1) break;
        oc_frame_alloc(&src[i], f.width, f.height);
        oc_frame_copy(&src[i], &f);
        CHECK(oc_vp9enc_encode(enc, &f, 0, keep_packet, &k) == 0);
        frames++;
    }
    CHECK(oc_vp9enc_encode(enc, NULL, 0, keep_packet, &k) == 0);
    CHECK(frames == 30 && k.count == 30);
    CHECK(k.count > 0 && k.key[0]);
    double worst = 99;
    int order_ok = 1, first_num = -1;
    for (int i = 0; i < k.count && i < frames; i++) {
        oc_frame out;
        CHECK(oc_vp9dec_decode(dec, k.d[i], k.n[i], k.pts[i], &out) == 0);
        CHECK(out.width == 1280 && out.height == 720);
        double p = psnr_y(&src[i], &out);
        if (p < worst) worst = p;
        int num = oc_capture_synthetic_frame_number(&out);
        if (i == 0) first_num = num;
        else if (num != first_num + i) order_ok = 0;
        CHECK(k.pts[i] == src[i].pts_us);
    }
    printf("  vp9 720p worst luma PSNR %.1f dB\n", worst);
    CHECK(worst > 35.0);
    CHECK(order_ok && first_num >= 0);
    for (int i = 0; i < k.count; i++) free(k.d[i]);
    for (int i = 0; i < frames; i++) oc_frame_free(&src[i]);
    oc_vp9enc_close(enc);
    oc_vp9dec_close(dec);
    oc_capture_stop(cam);
    oc_capture_close(cam);
}

static void test_opus_roundtrip(void) {
    oc_opusenc *e = oc_opusenc_open();
    oc_opusdec *d = oc_opusdec_open(1);
    CHECK(e && d);
    if (!e || !d) { oc_opusenc_close(e); oc_opusdec_close(d); return; }
    int la = oc_opusenc_lookahead(e);
    CHECK(la > 0 && la <= (int)OC_MP4_OPUS_PRESKIP);
    enum { BLOCKS = 50 };
    static int16_t in[BLOCKS * OC_OPUS_FRAME], out[BLOCKS * OC_OPUS_FRAME];
    /* A chirp, 200 Hz to 1 kHz: unlike a steady tone it has only one alignment. */
    for (int i = 0; i < BLOCKS * OC_OPUS_FRAME; i++) {
        double t = (double)i / 48000;
        in[i] = (int16_t)(10000 * sin(2 * M_PI * (200.0 * t + 400.0 * t * t)));
    }
    int got = 0;
    for (int b = 0; b < BLOCKS; b++) {
        uint8_t pkt[OC_OPUS_MAX_PACKET];
        int n = oc_opusenc_encode(e, in + b * OC_OPUS_FRAME, pkt, sizeof pkt);
        CHECK(n > 0);
        int m = oc_opusdec_decode(d, pkt, (size_t)n, out + got, BLOCKS * OC_OPUS_FRAME - got);
        CHECK(m == OC_OPUS_FRAME);
        if (m > 0) got += m;
    }
    /* Compare after the codec's delay — searched for, not assumed, since the
     * decoder's alignment is what is being checked — skipping 100 ms of settling. */
    double snr = -99;
    int best_lag = -1;
    for (int lag = 0; lag <= 960; lag++) {
        double sig = 0, noise = 0;
        for (int i = 4800; i + lag < got; i++) {
            double s = in[i], r = out[i + lag];
            sig += s * s; noise += (s - r) * (s - r);
        }
        double v = 10 * log10(sig / (noise > 0 ? noise : 1));
        if (v > snr) { snr = v; best_lag = lag; }
    }
    printf("  opus lag %d (look-ahead %d)\n", best_lag, la);
    CHECK(abs(best_lag - la) <= 8);                         /* within the resampler phase */
    printf("  opus chirp SNR %.1f dB\n", snr);
    CHECK(snr > 10.0);
    oc_opusenc_close(e);
    oc_opusdec_close(d);
}

/* ---- recorder ----------------------------------------------------------------------- */

static int wait_state(oc_recorder *r, oc_rec_state want, int timeout_ms, oc_rec_status *st) {
    for (int t = 0; t < timeout_ms; t += 20) {
        oc_recorder_status(r, st);
        if (st->state == want || st->state == OC_REC_ERROR) return st->state == want;
        msleep(20);
    }
    return 0;
}

static int ffprobe_ok(const uint8_t *mp4, size_t len, uint32_t duration_ms) {
    if (system("command -v ffprobe >/dev/null 2>&1") != 0) {
        printf("  SKIP ffprobe not installed\n");
        return 1;
    }
    const char *path = "build/test_media_rec.mp4";
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fwrite(mp4, 1, len, f);
    fclose(f);
    FILE *p = popen("ffprobe -v error -show_entries stream=codec_name,width,height:format=duration "
                    "-of default=nw=1 build/test_media_rec.mp4 2>&1", "r");
    if (!p) return 0;
    char out[1024] = {0};
    size_t n = fread(out, 1, sizeof out - 1, p);
    out[n] = 0;
    int rc = pclose(p);
    remove(path);
    const char *d = strstr(out, "duration=");
    double secs = d ? atof(d + 9) : -1;
    int ok = rc == 0 && strstr(out, "codec_name=vp9") && strstr(out, "codec_name=opus") &&
             !strstr(out, "rror") && secs > 0 && fabs(secs * 1000 - duration_ms) <= 50;
    if (!ok) printf("  ffprobe said:\n%s\n", out);
    return ok;
}

static oc_rec_result g_rec;          /* shared with the player tests */

static void test_recorder(void) {
    setenv("OPENCHIME_TEST_CAPTURE", "synthetic", 1);
    setenv("OPENCHIME_TEST_AUDIO", "synthetic", 1);
    setenv("OPENCHIME_TEST_VIDEO_CAP_MS", "3000", 1);
    oc_recorder_opts o = { .width = 640, .height = 360, .fps = 30 };
    int err;
    oc_recorder *r = oc_recorder_open(&o, &err);
    CHECK(r != NULL && err == OC_REC_OK);
    if (!r) return;

    oc_rec_status st;
    oc_recorder_status(r, &st);
    CHECK(st.state == OC_REC_PREVIEW && st.cap_ms == 3000 && st.has_audio);
    msleep(300);                                            /* preview only: nothing recorded */
    static uint8_t bgra[640 * 360 * 4];
    int w, h; uint64_t seq = 0;
    CHECK(oc_recorder_preview(r, bgra, sizeof bgra, &w, &h, &seq) == 1 && w == 640 && h == 360);
    CHECK(oc_recorder_preview(r, bgra, 16, &w, &h, &(uint64_t){0}) == -1);

    CHECK(oc_recorder_start(r) == 0);
    CHECK(oc_recorder_start(r) != 0);
    /* The cap stops it without a stop call. */
    CHECK(wait_state(r, OC_REC_DONE, 8000, &st));
    CHECK(oc_recorder_take(r, &g_rec) == 0);
    oc_recorder_close(r);

    printf("  recorded %u ms, %zu bytes, poster %zu bytes, dropped %llu\n", g_rec.duration_ms,
           g_rec.video_len, g_rec.poster_len, (unsigned long long)st.dropped_frames);
    CHECK(g_rec.video && g_rec.poster);
    CHECK(g_rec.width == 640 && g_rec.height == 360);
    CHECK(g_rec.duration_ms >= 2950 && g_rec.duration_ms <= 3100);
    CHECK(g_rec.poster_len > 2 && g_rec.poster[0] == 0xFF && g_rec.poster[1] == 0xD8);

    oc_mp4_info info;
    CHECK(g_rec.video && oc_mp4_parse(g_rec.video, g_rec.video_len, &info) == 0);
    if (g_rec.video && info.video.present) {
        uint64_t vms = info.video.duration * 1000 / info.video.timescale;
        uint64_t ams = info.audio.present ? (info.audio.duration - info.opus_preskip) * 1000 / 48000 : 0;
        printf("  video track %llu ms, audio track %llu ms\n", (unsigned long long)vms, (unsigned long long)ams);
        CHECK(info.audio.present);
        CHECK(llabs((long long)vms - (long long)ams) < 40 + 20);   /* within a frame and an Opus block */
        CHECK(info.video.n_samples >= 80 && info.video.n_samples <= 92);
        CHECK(info.video.samples[0].sync);
        int k = oc_recorder_poster_index(&info);
        CHECK(k > 0 && info.video.samples[k].sync && info.video.samples[k].dts >= info.video.timescale);
        oc_mp4_info_free(&info);
    }
    CHECK(ffprobe_ok(g_rec.video, g_rec.video_len, g_rec.duration_ms));

    /* Stopping early. */
    setenv("OPENCHIME_TEST_VIDEO_CAP_MS", "300000", 1);
    r = oc_recorder_open(&o, &err);
    CHECK(r != NULL);
    if (r) {
        CHECK(oc_recorder_start(r) == 0);
        msleep(1200);
        oc_recorder_stop(r);
        CHECK(wait_state(r, OC_REC_DONE, 5000, &st));
        oc_rec_result res;
        CHECK(oc_recorder_take(r, &res) == 0);
        CHECK(res.duration_ms >= 900 && res.duration_ms <= 1400);
        CHECK(oc_recorder_take(r, &res) != 0 || (oc_rec_result_free(&res), 0));
        oc_rec_result_free(&res);
        oc_recorder_close(r);
    }
    /* The byte budget stops a recording before the length cap does, so a take
     * can never outgrow the daemon's video cap. */
    {
        oc_recorder_opts ob = o;
        ob.max_bytes = 150000;
        r = oc_recorder_open(&ob, &err);
        CHECK(r != NULL);
        if (r) {
            CHECK(oc_recorder_start(r) == 0);
            CHECK(wait_state(r, OC_REC_DONE, 20000, &st));
            oc_rec_result res;
            CHECK(oc_recorder_take(r, &res) == 0);
            printf("  byte budget: %u ms, %zu bytes\n", res.duration_ms, res.video_len);
            CHECK(res.duration_ms < 60000 && res.video_len >= 150000 && res.video_len < 150000 + 60000);
            oc_rec_result_free(&res);
            oc_recorder_close(r);
        }
    }
    /* Closing mid-recording leaves nothing behind (ASan checks the leak). */
    r = oc_recorder_open(&o, &err);
    if (r) { oc_recorder_start(r); msleep(300); oc_recorder_close(r); }

    /* The OS blocks the camera. */
    setenv("OPENCHIME_TEST_CAPTURE", "denied", 1);
    r = oc_recorder_open(&o, &err);
    CHECK(r == NULL && err == OC_REC_CAMERA_DENIED);
    setenv("OPENCHIME_TEST_CAPTURE", "synthetic", 1);
    unsetenv("OPENCHIME_TEST_VIDEO_CAP_MS");
}

/* ---- player ------------------------------------------------------------------------- */

static void test_player(void) {
    if (!g_rec.video) return;
    setenv("OPENCHIME_TEST_AUDIO", "synthetic", 1);
    oc_player *p = oc_player_open(g_rec.video, g_rec.video_len);
    CHECK(p != NULL);
    if (!p) return;
    oc_player_status st;
    msleep(100);
    oc_player_status_get(p, &st);
    CHECK(st.state == OC_PLAYER_PAUSED && st.duration_ms == g_rec.duration_ms && st.has_audio);
    CHECK(st.presented == 1);                               /* the first frame, shown paused */
    static uint8_t bgra[640 * 360 * 4];
    int w, h; uint64_t seq = 0;
    CHECK(oc_player_frame(p, bgra, sizeof bgra, &w, &h, &seq) == 1 && w == 640 && h == 360);

    oc_mp4_info info;
    uint32_t n_frames = 0;
    if (oc_mp4_parse(g_rec.video, g_rec.video_len, &info) == 0) { n_frames = info.video.n_samples; }

    oc_player_play(p);
    for (int t = 0; t < 6000; t += 50) {
        oc_player_status_get(p, &st);
        if (st.state == OC_PLAYER_ENDED) break;
        msleep(50);
    }
    CHECK(st.state == OC_PLAYER_ENDED);
    printf("  played: presented %llu, dropped %llu of %u\n", (unsigned long long)st.presented,
           (unsigned long long)st.dropped, n_frames);
    CHECK(st.presented + st.dropped == n_frames);
    CHECK(st.dropped <= n_frames / 10);
    CHECK(st.position_ms == st.duration_ms);

    /* A seek lands on the keyframe at or before the target. */
    oc_player_seek(p, 2500);
    msleep(200);
    oc_player_status_get(p, &st);
    int k = oc_mp4_keyframe_before(&info, 2500 * 90);
    CHECK(k >= 0 && st.position_ms == (uint32_t)(info.video.samples[k].dts / 90));
    CHECK(st.state == OC_PLAYER_PAUSED);

    /* A decoder slower than the frame rate drops frames rather than falling
     * behind: once the clock passes the next keyframe, the frames shown jump to it. */
    oc_player_seek(p, 0);
    oc_player_test_decode_delay(p, 60);
    msleep(200);
    oc_player_status_get(p, &st);
    uint64_t before_p = st.presented, before_d = st.dropped;
    int k2 = oc_mp4_keyframe_before(&info, 2600 * 90);
    uint32_t k2_ms = k2 >= 0 ? (uint32_t)(info.video.samples[k2].dts / 90) : 0;
    oc_player_play(p);
    msleep(2700);
    oc_player_status_get(p, &st);
    printf("  slow decoder: clock %u ms, frame %u ms, keyframe %u ms, dropped %llu\n", st.position_ms,
           st.frame_ms, k2_ms, (unsigned long long)(st.dropped - before_d));
    CHECK(st.dropped > before_d);
    CHECK(st.presented > before_p);
    CHECK(st.position_ms >= 2400);                          /* the clock follows the audio */
    CHECK(k2_ms > 0 && st.frame_ms >= k2_ms);               /* and the picture caught up */
    oc_player_close(p);
    oc_mp4_info_free(&info);
}

/* ---- screen recording (REQ-162) ------------------------------------------------------ */

/* A frame's box, a view into it, the camera box's geometry. */
static void test_i420_view(void) {
    oc_frame f, v;
    CHECK(oc_frame_alloc(&f, 64, 48) == 0);
    oc_i420_fill(&f, 16, 128, 128);
    CHECK(oc_i420_view(&f, 3, 0, 8, 8, &v) != 0);          /* odd position */
    CHECK(oc_i420_view(&f, 0, 0, 7, 8, &v) != 0);          /* odd size */
    CHECK(oc_i420_view(&f, 60, 0, 8, 8, &v) != 0);         /* outside */
    CHECK(oc_i420_view(&f, 56, 40, 8, 8, &v) == 0);        /* the last whole box */
    oc_i420_fill(&v, 200, 90, 160);
    int inside = 1, outside = 1;
    for (int y = 0; y < 48; y++)
        for (int x = 0; x < 64; x++) {
            int in = x >= 56 && y >= 40, val = f.plane[0][y * f.stride[0] + x];
            if (in && val != 200) inside = 0;
            if (!in && val != 16) outside = 0;
        }
    CHECK(inside && outside);
    CHECK(f.plane[1][20 * f.stride[1] + 28] == 90 && f.plane[2][20 * f.stride[2] + 28] == 160);
    CHECK(f.plane[1][19 * f.stride[1] + 27] == 128);
    oc_frame_free(&f);

    /* A fifth of the width, the camera's shape, 2% in from the chosen corner. */
    int x, y, w, h;
    oc_inset_rect(1920, 1080, 640, 360, OC_CORNER_BR, &x, &y, &w, &h);
    CHECK(w == 384 && h == 216 && x == 1920 - 38 - 384 && y == 1080 - 38 - 216);
    oc_inset_rect(1920, 1080, 640, 360, OC_CORNER_TL, &x, &y, &w, &h);
    CHECK(x == 38 && y == 38);
    oc_inset_rect(1920, 1080, 640, 360, OC_CORNER_TR, &x, &y, &w, &h);
    CHECK(x == 1920 - 38 - 384 && y == 38);
    oc_inset_rect(1920, 1080, 640, 360, OC_CORNER_BL, &x, &y, &w, &h);
    CHECK(x == 38 && y == 1080 - 38 - 216);
    /* A tall camera on a short frame is held to half the frame's height, and
     * everything stays even (a box must be a view). */
    oc_inset_rect(1152, 720, 480, 640, OC_CORNER_BR, &x, &y, &w, &h);
    CHECK(h <= 360 && !((x | y | w | h) & 1) && x + w <= 1152 && y + h <= 720);
}

/* What a frame rate can honestly be asserted to be.
 *
 * Capture and recording are REAL TIME: how many frames a second of it yields is
 * a property of the machine, not of the code. A host that cannot encode 720p at
 * 30 fps produces fewer frames, which is the recorder behaving correctly -- so a
 * narrow band around the nominal count ("27 to 32 frames in a second") reports
 * whether the machine running the suite is busy. It failed on two pinned CPUs and
 * under ThreadSanitizer, on a tree with nothing wrong in it.
 *
 * What IS the code's own, and is asserted here:
 *
 *   - it never runs FASTER than it was asked to, in the count or in any gap
 *     between frames. A host cannot make a paced source exceed its rate; a pacing
 *     defect can, and that is what this catches. The median gap is the rate the
 *     source actually paced at, and a median is not moved by a few long gaps from
 *     a busy host. There is deliberately NO upper bound on it: a loaded host
 *     spaces frames further apart, which is the recorder keeping time correctly
 *     with fewer frames to show.
 *   - the timestamps go forwards, and the run has no hole in it -- a gap of a
 *     whole second means the source stopped, which no amount of load explains.
 *   - a shortfall is a shortfall and not a stop: a floor of a fifth of the rate,
 *     6 fps of a nominal 30. Below that there is nothing to watch, whatever the
 *     machine.
 *
 * The rate itself is PRINTED. That is the number a benchmark wants, and it says
 * which machine it expects; a unit suite cannot assert it. */
static void check_frame_rate(const char *what, const int64_t *pts_us, int n,
                             int fps, int span_ms) {
    int want   = span_ms * fps / 1000;
    int ceil_n = want + 2;                 /* the frames on each boundary */
    int floor_n = want / 5;
    double period = 1e6 / (double)fps;
    printf("  %s: %d frames in %d ms -- %.1f fps, asked for %d\n",
           what, n, span_ms, n * 1000.0 / (span_ms ? span_ms : 1), fps);
    CHECK(n <= ceil_n);
    CHECK(n >= floor_n);
    if (n < 3) return;
    double gaps[4096];
    int ng = 0, backwards = 0;
    double worst = 0;
    for (int i = 1; i < n && ng < (int)(sizeof gaps / sizeof gaps[0]); i++) {
        double g = (double)(pts_us[i] - pts_us[i - 1]);
        if (g <= 0) backwards++;
        if (g > worst) worst = g;
        gaps[ng++] = g;
    }
    CHECK(backwards == 0);
    for (int i = 1; i < ng; i++) {         /* insertion sort: a few hundred gaps */
        double v = gaps[i]; int j = i - 1;
        while (j >= 0 && gaps[j] > v) { gaps[j + 1] = gaps[j]; j--; }
        gaps[j + 1] = v;
    }
    double median = gaps[ng / 2];
    printf("    spacing: median %.1f ms, worst %.1f ms (period %.1f ms)\n",
           median / 1000.0, worst / 1000.0, period / 1000.0);
    CHECK(median >= period * 0.75);        /* never faster than asked; slower is the host */
    CHECK(worst < 1000000.0);              /* a whole second with no frame: a stop */
}

/* The screen front end: the synthetic screen changes five times a second, and
 * frames still come at the frame rate, all one size, fitted inside the maximum. */
static void test_screen_source(void) {
    setenv("OPENCHIME_TEST_CAPTURE", "synthetic", 1);
    oc_capture_device d[4];
    CHECK(oc_capture_list_screens(d, 4) == 2);
    CHECK(d[0].kind == OC_SOURCE_SCREEN && d[1].kind == OC_SOURCE_WINDOW);
    int err;
    oc_capture *c = oc_capture_open_screen("screen:synthetic", 1280, 720, 30, &err);
    CHECK(c != NULL && err == OC_CAP_OK);
    if (!c) return;
    CHECK(oc_capture_start(c) == OC_CAP_OK);
    int frames = 0, changes = 0, prev = -1, size_ok = 1, order_ok = 1;
    int64_t pts[256];
    int64_t t0 = oc_media_clock_us();
    while (oc_media_clock_us() - t0 < 1000000) {
        oc_frame f;
        int rc = oc_capture_next(c, &f, 100);
        CHECK(rc >= 0);
        if (rc != 1) continue;
        if (frames < (int)(sizeof pts / sizeof pts[0])) pts[frames] = f.pts_us;
        frames++;
        if (f.width != 1152 || f.height != 720) size_ok = 0;
        int n = oc_capture_synthetic_frame_number(&f);
        if (n < prev) order_ok = 0;
        if (n != prev) { changes++; prev = n; }
    }
    CHECK(size_ok);
    check_frame_rate("screen source", pts,
                     frames < (int)(sizeof pts / sizeof pts[0]) ? frames : 256, 30, 1000);
    /* The screen changes five times a second, whatever the rate frames arrive at:
     * never more than that (plus the two boundaries), never going backwards, and
     * at least one change seen however few frames the host managed. */
    printf("  screen source: %d changes in 1 s\n", changes);
    CHECK(order_ok);
    CHECK(changes >= 1 && changes <= 7);
    oc_capture_stop(c);
    oc_capture_close(c);
}

/* Decode the video frame at sample `k` (from the keyframe before it) into `out`. */
static int decode_video_at(const uint8_t *mp4, const oc_mp4_info *info, uint32_t k, oc_frame *out) {
    int key = oc_mp4_keyframe_before(info, info->video.samples[k].dts);
    if (key < 0) return -1;
    oc_vp9dec *dec = oc_vp9dec_open();
    if (!dec) return -1;
    int ok = -1;
    for (uint32_t i = (uint32_t)key; i <= k; i++) {
        oc_frame f;
        const oc_mp4_sample *s = &info->video.samples[i];
        if (oc_vp9dec_decode(dec, mp4 + s->offset, s->size, (int64_t)i, &f) != 0) break;
        if (i == k && oc_frame_alloc(out, f.width, f.height) == 0) { oc_frame_copy(out, &f); ok = 0; }
    }
    oc_vp9dec_close(dec);
    return ok;
}

/* How colourful a rectangle is: the mean distance of its chroma from grey. */
static double chroma_in(const oc_frame *f, int x, int y, int w, int h) {
    double sum = 0;
    int n = 0;
    for (int j = y / 2; j < (y + h) / 2; j++)
        for (int i = x / 2; i < (x + w) / 2; i++) {
            sum += abs(f->plane[1][j * f->stride[1] + i] - 128) + abs(f->plane[2][j * f->stride[2] + i] - 128);
            n++;
        }
    return n ? sum / n : 0;
}

/* The whole audio track, decoded, pre-skip removed. */
static int16_t *decode_audio(const uint8_t *mp4, const oc_mp4_info *info, size_t *n_out) {
    *n_out = 0;
    if (!info->audio.present) return NULL;
    size_t cap = (size_t)info->audio.n_samples * 960 + 960;
    int16_t *pcm = malloc(cap * sizeof *pcm);
    oc_opusdec *d = oc_opusdec_open(1);
    if (!pcm || !d) { free(pcm); oc_opusdec_close(d); return NULL; }
    size_t n = 0;
    for (uint32_t i = 0; i < info->audio.n_samples; i++) {
        const oc_mp4_sample *s = &info->audio.samples[i];
        int got = oc_opusdec_decode(d, mp4 + s->offset, s->size, pcm + n, (int)(cap - n));
        if (got > 0) n += (size_t)got;
    }
    oc_opusdec_close(d);
    size_t skip = info->opus_preskip < n ? info->opus_preskip : n;
    memmove(pcm, pcm + skip, (n - skip) * sizeof *pcm);
    *n_out = n - skip;
    return pcm;
}

/* The amplitude of one frequency in a stretch of samples (Goertzel). */
static double tone_amp(const int16_t *x, size_t n, double hz) {
    double w = 2 * 3.141592653589793 * hz / 48000.0, c = 2 * cos(w), s1 = 0, s2 = 0;
    for (size_t i = 0; i < n; i++) { double s0 = x[i] + c * s1 - s2; s2 = s1; s1 = s0; }
    double re = s1 - s2 * cos(w), im = s2 * sin(w);
    return 2.0 * sqrt(re * re + im * im) / (double)n;
}

static void test_screen_recording(void) {
    setenv("OPENCHIME_TEST_CAPTURE", "synthetic", 1);
    setenv("OPENCHIME_TEST_AUDIO", "synthetic", 1);
    setenv("OPENCHIME_TEST_VIDEO_CAP_MS", "3000", 1);
    /* The microphone hears the computer's sound back, 20 ms late at half strength:
     * a room, on a machine with speakers. */
    setenv("OPENCHIME_TEST_MIC_ECHO", "1", 1);
    /* ...and the narrator draws breath before speaking, as a person does: a
     * second from when the microphone opens, so the recording, which starts a
     * frame or so later, opens on the computer's sound alone. See below. */
    setenv("OPENCHIME_TEST_MIC_LEAD_MS", "1000", 1);

    for (int corner = OC_CORNER_BR; corner <= OC_CORNER_TL; corner++) {
        oc_recorder_opts o = { .height = 720, .fps = 30, .screen_id = "screen:synthetic",
                               .with_camera = 1, .corner = corner, .computer_sound = 1 };
        int err;
        oc_recorder *r = oc_recorder_open(&o, &err);
        CHECK(r != NULL && err == OC_REC_OK);
        if (!r) continue;
        oc_rec_status st;
        oc_recorder_status(r, &st);
        CHECK(st.screen && st.computer_sound && st.has_audio);
        CHECK(oc_recorder_start(r) == 0);
        CHECK(wait_state(r, OC_REC_DONE, 8000, &st));
        oc_rec_result res = {0};
        CHECK(oc_recorder_take(r, &res) == 0);
        oc_recorder_close(r);
        /* A 16:10 screen fitted inside 1280×720. */
        CHECK(res.width == 1152 && res.height == 720);
        /* It must not stop EARLY -- that would lose what was being recorded. Stopping
         * late is the same real-time story as the frame count: a host that cannot
         * encode fast enough notices the three seconds are up a little after they
         * are, so the ceiling is generous rather than narrow. */
        CHECK(res.duration_ms >= 2950 && res.duration_ms <= 6000);
        oc_mp4_info info;
        if (!res.video || oc_mp4_parse(res.video, res.video_len, &info) != 0) { CHECK(0); oc_rec_result_free(&res); continue; }
        /* Every frame at the rate, though the screen changed five times a second --
         * asserted as the rate the samples are SPACED at, since how many a real-time
         * recording yields is the machine's business (check_frame_rate). */
        {
            int64_t spts[4096];
            uint32_t ns = info.video.n_samples;
            if (ns > (uint32_t)(sizeof spts / sizeof spts[0])) ns = (uint32_t)(sizeof spts / sizeof spts[0]);
            for (uint32_t k = 0; k < ns; k++)
                spts[k] = (int64_t)(info.video.samples[k].dts * 1000000ULL / info.video.timescale);
            check_frame_rate("screen recording", spts, (int)ns, 30, (int)res.duration_ms);
        }
        oc_frame f;
        if (decode_video_at(res.video, &info, info.video.n_samples / 2, &f) == 0) {
            int x, y, w, h;
            oc_inset_rect(f.width, f.height, 640, 360, corner, &x, &y, &w, &h);
            double box = chroma_in(&f, x + 8, y + 8, w - 16, h - 16);
            double page = chroma_in(&f, f.width / 2 - 100, f.height / 2 - 60, 200, 120);
            /* The opposite corner, where no box is. */
            int ox, oy, ow, oh;
            oc_inset_rect(f.width, f.height, 640, 360, corner ^ 3, &ox, &oy, &ow, &oh);
            double away = chroma_in(&f, ox + 8, oy + 8, ow - 16, oh - 16);
            printf("  corner %d: box chroma %.1f, page %.1f, opposite corner %.1f\n", corner, box, page, away);
            CHECK(box > 40.0);                          /* the camera's colour bars */
            CHECK(page < 4.0 && away < 4.0);            /* the grey page */
            oc_frame_free(&f);
        } else {
            CHECK(0);
        }
        /* Sound: the computer's once, the voice whole, over the last second, with
         * the narrator talking over the computer throughout it.
         *
         * The canceller learns the room from the computer's sound heard back
         * while nobody speaks, then holds what it learned through the talking.
         * Made to learn it UNDER the talking instead -- the narrator speaking
         * from the first sample, which is what this test once did -- it settles
         * wherever a steady tone over another steady tone happens to leave it:
         * the same signals through the same canceller, offline, gave the
         * computer's sound 0.64-1.57 of its level over 1440 starting phases,
         * past 1.26 (no canceller at all) now and then, and past any bound worth
         * asserting 0.35% of the time. With a pause first, the same sweep gave
         * 1.001-1.002 from a pause of a fifth of a second. Speech pauses, so the
         * pause is what a recording gets; the narrator takes one here too, and the
         * computer's sound must then come out once, the voice whole.
         *
         * The pause runs from when the microphone opens, not from when the
         * recording starts (the recorder reads and discards it until then), so
         * that the voice starts after the recording does is checked rather than
         * assumed: a host stalled for most of a second before the first frame
         * would break it, and this says so instead of a level out of range. */
        size_t n = 0;
        int16_t *pcm = decode_audio(res.video, &info, &n);
        if (pcm && n > 48000) {
            size_t onset = 0;
            while (onset + 960 <= n && tone_amp(pcm + onset, 960, 440) < 8000 * 0.5) onset += 960;
            printf("  corner %d: the narrator starts %zu ms in\n", corner, onset / 48);
            CHECK(onset >= 48000 / 5);                /* a fifth of a second of the room first */
            const int16_t *tail = pcm + n - 48000;
            CHECK(onset <= (size_t)(tail - pcm));     /* ...and talks through the last second */
            double a440 = tone_amp(tail, 48000, 440), a660 = tone_amp(tail, 48000, 660);
            printf("  corner %d: 440 Hz %.0f (mic 8000), 660 Hz %.0f (computer 6000)\n", corner, a440, a660);
            CHECK(a440 > 8000 * 0.9 && a440 < 8000 * 1.1);
            CHECK(a660 > 6000 * 0.9 && a660 < 6000 * 1.1);
        } else {
            CHECK(0);
        }
        free(pcm);
        CHECK(ffprobe_ok(res.video, res.video_len, res.duration_ms));
        oc_mp4_info_free(&info);
        oc_rec_result_free(&res);
    }
    /* The microphone hears nothing but the computer's sound coming back from
     * the speakers. Cancelled, the computer's sound is in the recording once, at
     * its own level; uncancelled it would be about 1.26 times that (the echo is
     * half strength, 20 ms late). */
    unsetenv("OPENCHIME_TEST_MIC_LEAD_MS");
    setenv("OPENCHIME_TEST_MIC_ECHO", "only", 1);
    setenv("OPENCHIME_TEST_VIDEO_CAP_MS", "5000", 1);
    {
        oc_recorder_opts o = { .height = 360, .fps = 30, .screen_id = "screen:synthetic", .computer_sound = 1 };
        int err;
        oc_recorder *r = oc_recorder_open(&o, &err);
        CHECK(r != NULL);
        if (r) {
            oc_rec_status st;
            CHECK(oc_recorder_start(r) == 0);
            CHECK(wait_state(r, OC_REC_DONE, 10000, &st));
            oc_rec_result res = {0};
            CHECK(oc_recorder_take(r, &res) == 0);
            oc_recorder_close(r);
            oc_mp4_info info;
            if (res.video && oc_mp4_parse(res.video, res.video_len, &info) == 0) {
                size_t n = 0;
                int16_t *pcm = decode_audio(res.video, &info, &n);
                if (pcm && n > 96000) {
                    const int16_t *tail = pcm + n - 96000;          /* the last two seconds */
                    double a660 = tone_amp(tail, 96000, 660);
                    printf("  echo only: 660 Hz %.0f (once: 6000; twice, uncancelled: ~7560)\n", a660);
                    CHECK(a660 > 6000 * 0.9 && a660 < 6000 * 1.1);
                } else {
                    CHECK(0);
                }
                free(pcm);
                oc_mp4_info_free(&info);
            }
            oc_rec_result_free(&res);
        }
    }
    setenv("OPENCHIME_TEST_VIDEO_CAP_MS", "3000", 1);
    unsetenv("OPENCHIME_TEST_MIC_ECHO");

    /* A window, no camera, no computer sound: no box anywhere, the microphone only. */
    {
        oc_recorder_opts o = { .height = 720, .fps = 30, .screen_id = "window:synthetic" };
        int err;
        oc_recorder *r = oc_recorder_open(&o, &err);
        CHECK(r != NULL);
        if (r) {
            oc_rec_status st;
            CHECK(oc_recorder_start(r) == 0);
            CHECK(wait_state(r, OC_REC_DONE, 8000, &st));
            CHECK(!st.computer_sound);
            oc_rec_result res = {0};
            CHECK(oc_recorder_take(r, &res) == 0);
            oc_recorder_close(r);
            oc_mp4_info info;
            if (res.video && oc_mp4_parse(res.video, res.video_len, &info) == 0) {
                oc_frame f;
                if (decode_video_at(res.video, &info, info.video.n_samples / 2, &f) == 0) {
                    double all = chroma_in(&f, 0, 0, f.width, f.height);
                    printf("  window, no camera: chroma %.1f\n", all);
                    CHECK(all < 4.0);
                    oc_frame_free(&f);
                }
                size_t n = 0;
                int16_t *pcm = decode_audio(res.video, &info, &n);
                if (pcm && n > 48000) {
                    const int16_t *tail = pcm + n - 48000;
                    printf("  window, microphone only: 440 Hz %.0f, 660 Hz %.0f\n",
                           tone_amp(tail, 48000, 440), tone_amp(tail, 48000, 660));
                    CHECK(tone_amp(tail, 48000, 440) > 6000 && tone_amp(tail, 48000, 660) < 500);
                }
                free(pcm);
                oc_mp4_info_free(&info);
            }
            oc_rec_result_free(&res);
        }
    }

    /* The window closes mid-recording: what came before it is kept. */
    setenv("OPENCHIME_TEST_VIDEO_CAP_MS", "300000", 1);
    setenv("OPENCHIME_TEST_SCREEN_GONE_MS", "1500", 1);
    {
        oc_recorder_opts o = { .height = 720, .fps = 30, .screen_id = "window:synthetic" };
        int err;
        oc_recorder *r = oc_recorder_open(&o, &err);
        CHECK(r != NULL);
        if (r) {
            oc_rec_status st;
            CHECK(oc_recorder_start(r) == 0);
            CHECK(wait_state(r, OC_REC_DONE, 8000, &st));
            CHECK(st.source_gone);
            oc_rec_result res = {0};
            CHECK(oc_recorder_take(r, &res) == 0);
            printf("  window gone at 1.5 s: kept %u ms\n", res.duration_ms);
            CHECK(res.duration_ms >= 1000 && res.duration_ms <= 1600);
            oc_rec_result_free(&res);
            oc_recorder_close(r);
        }
    }
    /* Gone before anything was captured: refused as that. */
    setenv("OPENCHIME_TEST_SCREEN_GONE_MS", "0", 1);
    {
        oc_recorder_opts o = { .height = 720, .screen_id = "window:synthetic" };
        int err;
        oc_recorder *r = oc_recorder_open(&o, &err);
        CHECK(r == NULL && err == OC_REC_SCREEN_GONE);
        if (r) oc_recorder_close(r);
    }
    unsetenv("OPENCHIME_TEST_SCREEN_GONE_MS");
    /* The system refuses the capture. */
    setenv("OPENCHIME_TEST_CAPTURE", "denied", 1);
    {
        oc_recorder_opts o = { .height = 720, .screen_id = "screen:synthetic" };
        int err;
        oc_recorder *r = oc_recorder_open(&o, &err);
        CHECK(r == NULL && err == OC_REC_SCREEN_DENIED);
        if (r) oc_recorder_close(r);
    }
    setenv("OPENCHIME_TEST_CAPTURE", "synthetic", 1);
    unsetenv("OPENCHIME_TEST_VIDEO_CAP_MS");
}

int run_media_tests(void) {
    printf("media:\n");
    test_mp4_roundtrip();
    test_mp4_audio_only();
    test_player_audio_only();
    test_mp4_fuzz();
    test_vp9_roundtrip();
    test_opus_roundtrip();
    test_recorder();
    test_i420_view();
    test_screen_source();
    test_screen_recording();
    test_player();
    oc_rec_result_free(&g_rec);
    unsetenv("OPENCHIME_TEST_CAPTURE");
    unsetenv("OPENCHIME_TEST_AUDIO");
    return failures;
}
