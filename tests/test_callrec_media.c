/* A call's recording through the real codecs (daemon/callrec_engine.c,
 * CALLS.md §8): a synthetic two-person call -- alice's voice and camera, bob's
 * voice, bob sharing a screen a while -- spooled as the Recorder spools it,
 * made into the recording, and the recording parsed and decoded: VP9 at
 * 1280×720 and Opus that both play, the voices where they were said, and the
 * picture following them -- alice's camera large while she speaks, the screen
 * while it is shared, bob once he speaks. Run with OC_TEST_ONLY=callrec_media. */

#include "callrec.h"
#include "callrec_render.h"
#include "oc_codec.h"
#include "oc_mp4.h"
#include "check.h"
#include "testenv.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define T0  1700000000000ull
#define UA  11u
#define UB  22u
#define SECS 6

static void w64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (56 - 8 * i)); }
static void w32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void w16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

static FILE *spool_file(const char *dir, const char *name) {
    char path[700];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    return fopen(path, "wb");
}

/* A voice as a call's device sends it: 16 kHz Opus with DTX, a tone from
 * `from_ms` to `to_ms` and silence otherwise, the silence not sent. */
static void voice(const char *dir, uint64_t user, double hz, int from_ms, int to_ms) {
    char name[32]; snprintf(name, sizeof name, "audio.%llu", (unsigned long long)user);
    FILE *f = spool_file(dir, name);
    oc_opusenc *e = oc_opusenc_open_voice(24);
    double ph = 0;
    for (int i = 0; f && e && i < SECS * 50; i++) {
        int16_t pcm[OC_VOICE_FRAME];
        int on = i * 20 >= from_ms && i * 20 < to_ms;
        for (int k = 0; k < OC_VOICE_FRAME; k++) {
            pcm[k] = on ? (int16_t)(8000 * sin(ph)) : 0;
            ph += 2 * M_PI * hz / OC_VOICE_RATE;
        }
        uint8_t pkt[1500];
        int n = oc_opusenc_encode_voice(e, pcm, pkt, sizeof pkt);
        if (n <= 2) continue;                       /* DTX: not sent */
        uint8_t h[14];
        w64(h, T0 + (uint64_t)i * 20); w32(h + 8, 5000u + (uint32_t)i); w16(h + 12, (uint32_t)n);
        fwrite(h, 1, sizeof h, f); fwrite(pkt, 1, (size_t)n, f);
    }
    if (e) oc_opusenc_close(e);
    if (f) fclose(f);
}

typedef struct { FILE *f; uint64_t sharer; int64_t at_ms; int bad; } vctx;
static void vemit(void *ctx, const oc_packet *p) {
    vctx *v = ctx;
    uint8_t h[25];
    size_t n;
    w64(h, T0 + (uint64_t)v->at_ms);
    if (v->sharer) { w64(h + 8, v->sharer); h[16] = (uint8_t)p->keyframe; w16(h + 17, 320); w16(h + 19, 240); w32(h + 21, (uint32_t)p->len); n = 25; }
    else           { w32(h + 8, 0); h[12] = (uint8_t)p->keyframe; w16(h + 13, 320); w16(h + 15, 240); w32(h + 17, (uint32_t)p->len); n = 21; }
    if (fwrite(h, 1, n, v->f) != n || fwrite(p->data, 1, p->len, v->f) != p->len) v->bad = 1;
}

/* A picture stream of flat luma `y`, 320×240 at 15 fps, from `from_ms` to `to_ms`. */
static int picture(const char *dir, const char *name, uint64_t sharer, uint8_t y, int from_ms, int to_ms) {
    vctx v = { spool_file(dir, name), sharer, 0, 0 };
    oc_vp9enc *e = oc_vp9enc_open(320, 240, 15);
    oc_frame f;
    if (!v.f || !e || oc_frame_alloc(&f, 320, 240) != 0) { if (v.f) fclose(v.f); if (e) oc_vp9enc_close(e); return -1; }
    for (int t = from_ms, i = 0; t < to_ms; t += 66, i++) {
        for (int r = 0; r < 240; r++) memset(f.plane[0] + (size_t)r * f.stride[0], y, 320);
        memset(f.plane[1], 128, (size_t)f.stride[1] * 120);
        memset(f.plane[2], 128, (size_t)f.stride[2] * 120);
        f.pts_us = (int64_t)i * 66666;
        v.at_ms = t;
        if (oc_vp9enc_encode(e, &f, 0, vemit, &v) != 0) v.bad = 1;
    }
    v.at_ms = to_ms;
    oc_vp9enc_encode(e, NULL, 0, vemit, &v);
    oc_vp9enc_close(e);
    oc_frame_free(&f);
    fclose(v.f);
    return v.bad ? -1 : 0;
}

static uint8_t *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = n > 0 ? malloc((size_t)n) : NULL;
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    *len = b ? (size_t)n : 0;
    return b;
}

static double goertzel48(const int16_t *x, int n, double f) {
    double w = 2 * M_PI * f / 48000.0, c = 2 * cos(w), s1 = 0, s2 = 0;
    for (int i = 0; i < n; i++) { double s = x[i] + c * s1 - s2; s2 = s1; s1 = s; }
    double p = s1 * s1 + s2 * s2 - c * s1 * s2;
    return sqrt(p > 0 ? p : 0) / n;
}

int run_callrec_media_tests(void) {
    printf("test_callrec_media: a call's recording through libvpx and libopus\n");
    char dir[256], spool[400], out[400];
    CHECK(oc_test_mkdtemp("callrec-media", dir, sizeof dir) == 0);
    oc_test_path(spool, sizeof spool, dir, "spool");
    oc_test_path(out, sizeof out, dir, "rec.mp4");
    mkdir(spool, 0700);
    FILE *m = spool_file(spool, "meta");
    uint8_t h[30]; memcpy(h, "OCREC1", 6); w64(h + 6, 7); w64(h + 14, 3); w64(h + 22, T0);
    if (m) { fwrite(h, 1, sizeof h, m); fclose(m); }
    FILE *t = spool_file(spool, "timeline");
    if (t) {
        uint8_t r[17];
        uint64_t ev[5][3] = { { T0, OC_CALLREC_TL_JOIN, UA }, { T0, OC_CALLREC_TL_JOIN, UB },
                              { T0, OC_CALLREC_TL_CAMERA_ON, UA },
                              { T0 + 2000, OC_CALLREC_TL_SHARE_ON, UB }, { T0 + 4000, OC_CALLREC_TL_SHARE_OFF, UB } };
        for (int i = 0; i < 5; i++) { w64(r, ev[i][0]); r[8] = (uint8_t)ev[i][1]; w64(r + 9, ev[i][2]); fwrite(r, 1, sizeof r, t); }
        fclose(t);
    }
    voice(spool, UA, 440, 0, 3000);
    voice(spool, UB, 660, 3000, 6000);
    char cam[32]; snprintf(cam, sizeof cam, "camera.%u", UA);
    CHECK(picture(spool, cam, 0, 200, 0, 6000) == 0);
    CHECK(picture(spool, "share", UB, 60, 2000, 4000) == 0);

    uint64_t bytes = 0; uint32_t dur = 0; char err[200] = "";
    CHECK(oc_callrec_render(oc_callrec_codecs_real(), spool, out, NULL, &bytes, &dur, err, sizeof err) == OC_CALLREC_OK);
    if (err[0]) printf("  %s\n", err);
    size_t len = 0;
    uint8_t *file = slurp(out, &len);
    oc_mp4_info in; memset(&in, 0, sizeof in);
    CHECK(file && oc_mp4_parse(file, len, &in) == 0);
    CHECK(in.width == 1280 && in.height == 720 && dur >= 5800);
    printf("  %llu bytes, %u ms, %u video frames, %u audio packets\n", (unsigned long long)bytes, dur,
           in.video.n_samples, in.audio.n_samples);

    /* Every frame decodes; the centre of the large area at three moments. */
    oc_vp9dec *vd = oc_vp9dec_open_threads(1);
    int decoded = 0, y1 = -1, y3 = -1, y5 = -1;
    for (uint32_t i = 0; file && vd && i < in.video.n_samples; i++) {
        oc_frame f;
        if (oc_vp9dec_decode(vd, file + in.video.samples[i].offset, in.video.samples[i].size, 0, &f) != 0) continue;
        decoded++;
        uint64_t ms = in.video.samples[i].dts * 1000 / OC_MP4_VIDEO_TIMESCALE;
        int y = f.plane[0][(size_t)288 * f.stride[0] + 640];
        if (ms <= 1000) y1 = y;
        if (ms <= 3000) y3 = y;
        if (ms <= 5500) y5 = y;
    }
    if (vd) oc_vp9dec_close(vd);
    CHECK(decoded == (int)in.video.n_samples && decoded > 80);
    uint64_t hb = (uint64_t)UB * 0x9E3779B97F4A7C15ull;
    int bob_y = 70 + (int)((hb >> 56) % 100);
    printf("  the large area: %d at 1 s (alice's camera, 200), %d at 3 s (the screen, 60), %d at 5.5 s (bob, %d)\n",
           y1, y3, y5, bob_y);
    CHECK(abs(y1 - 200) <= 12 && abs(y3 - 60) <= 12 && abs(y5 - bob_y) <= 12);

    /* The voices, where they were said. */
    oc_opusdec *ad = oc_opusdec_open_rate(48000);
    double a440 = 0, a660 = 0, b440 = 0, b660 = 0;
    for (uint32_t i = 0; file && ad && i < in.audio.n_samples; i++) {
        int16_t pcm[960];
        int n = oc_opusdec_decode_frame(ad, file + in.audio.samples[i].offset, in.audio.samples[i].size, 0, pcm, 960);
        if (n != 960) continue;
        double ms = i * 20.0;
        if (ms >= 500 && ms < 2500) { a440 += goertzel48(pcm, n, 440); a660 += goertzel48(pcm, n, 660); }
        if (ms >= 3500 && ms < 5500) { b440 += goertzel48(pcm, n, 440); b660 += goertzel48(pcm, n, 660); }
    }
    if (ad) oc_opusdec_close(ad);
    printf("  the mix: 440 Hz %.0f against 660 Hz %.0f while alice speaks; %.0f against %.0f while bob does\n",
           a440, a660, b440, b660);
    CHECK(a440 > 4 * a660 && b660 > 4 * b440);
    oc_mp4_info_free(&in);
    free(file);
    oc_test_rmtree(dir);
    return failures;
}
