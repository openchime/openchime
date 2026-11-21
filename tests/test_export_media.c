/* The export of a video message with the real codecs (ARCH-135,
 * docs/VIDEO-MESSAGES.md "Export"): a synthetic 1080p recording, made the way a
 * client makes one, exported by libvpx/libopus to libavc/LAME, and read back by
 * an independent reader -- ffprobe for what the file is, ffmpeg for where the
 * picture and the sound fall -- with the transcode's own peak memory measured.
 * A full-suite suite: it encodes ten seconds of 1080p twice over. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "check.h"
#include "testenv.h"
#include "oc_codec.h"
#include "oc_mp4.h"
#include "export_render.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W        1920
#define H        1080
#define FPS      30
#define SECS     10
#define MARK_S   5                          /* the flash and the click, both at 5.0 s */
#define RATE     48000
/* The bound the managed box allows the transcode (ARCH-135). */
#define PEAK_MB  96

/* ---- the recording ----------------------------------------------------------------- */

typedef struct { oc_mp4_writer *w; int bad; } rec;
static void emit(void *ctx, const oc_packet *p) {
    rec *r = ctx;
    if (oc_mp4_write_video(r->w, p->data, p->len, p->pts_us, p->keyframe) != 0) r->bad = 1;
}

/* Ten seconds of mid grey with a white flash from 5.0 s, and quiet with a
 * click at 5.0 s: the two marks that must still line up in the copy. */
static int make_recording(const char *path, uint32_t *duration_ms) {
    rec r = { oc_mp4_writer_open(W, H), 0 };
    oc_vp9enc *ve = oc_vp9enc_open(W, H, FPS);
    oc_opusenc *ae = oc_opusenc_open();
    oc_frame f;
    if (!r.w || !ve || !ae || oc_frame_alloc(&f, W, H) != 0) return -1;
    int16_t pcm[OC_OPUS_FRAME];
    uint8_t pkt[OC_OPUS_MAX_PACKET];
    long t = 0;                                            /* audio samples encoded */
    for (int i = 0; i < SECS * FPS; i++) {
        uint8_t y = i >= MARK_S * FPS && i < MARK_S * FPS + 3 ? 235 : 110;
        for (int row = 0; row < H; row++) memset(f.plane[0] + (size_t)row * (size_t)f.stride[0], y, W);
        memset(f.plane[1], 128, (size_t)f.stride[1] * (H / 2));
        memset(f.plane[2], 128, (size_t)f.stride[2] * (H / 2));
        f.pts_us = (int64_t)i * 1000000 / FPS;
        if (oc_vp9enc_encode(ve, &f, 0, emit, &r) != 0) r.bad = 1;
        /* The audio up to the next frame's time, in whole packets. */
        while (t < (long)(i + 1) * RATE / FPS) {
            for (int k = 0; k < OC_OPUS_FRAME; k++) {
                long s = t + k;
                pcm[k] = s >= (long)MARK_S * RATE && s < (long)MARK_S * RATE + 96 ? 30000 : (int16_t)(200 * sin(s * 0.05));
            }
            int n = oc_opusenc_encode(ae, pcm, pkt, sizeof pkt);
            if (n <= 0 || oc_mp4_write_audio(r.w, pkt, (size_t)n, OC_OPUS_FRAME) != 0) r.bad = 1;
            t += OC_OPUS_FRAME;
        }
    }
    if (oc_vp9enc_encode(ve, NULL, 0, emit, &r) != 0) r.bad = 1;
    oc_vp9enc_close(ve);
    oc_opusenc_close(ae);
    oc_frame_free(&f);
    uint8_t *mp4 = NULL;
    size_t len = 0;
    if (r.bad || oc_mp4_writer_finish(r.w, &mp4, &len, duration_ms) != 0) return -1;
    FILE *out = fopen(path, "wb");
    int ok = out && fwrite(mp4, 1, len, out) == len;
    if (out) fclose(out);
    free(mp4);
    return ok ? 0 : -1;
}

/* ---- memory ------------------------------------------------------------------------ */

/* A field of /proc/self/status, in kB, or -1. */
static long status_kb(const char *field) {
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return -1;
    char line[256];
    long v = -1;
    size_t n = strlen(field);
    while (fgets(line, sizeof line, f))
        if (strncmp(line, field, n) == 0 && line[n] == ':') { v = atol(line + n + 1); break; }
    fclose(f);
    return v;
}

/* Start the peak (VmHWM) again from what the process holds now. */
static int reset_peak(void) {
    FILE *f = fopen("/proc/self/clear_refs", "w");
    if (!f) return -1;
    int ok = fputs("5", f) >= 0;
    return fclose(f) == 0 && ok ? 0 : -1;
}

/* ---- reading the copy back ------------------------------------------------------------ */

static long file_read(void *ctx, void *buf, size_t cap) {
    size_t n = fread(buf, 1, cap, (FILE *)ctx);
    return ferror((FILE *)ctx) ? -1 : (long)n;
}

static int have(const char *tool) {
    char cmd[128];
    snprintf(cmd, sizeof cmd, "command -v %s >/dev/null 2>&1", tool);
    return system(cmd) == 0;
}

/* The first second at which `cmd`'s raw output, read `unit` bytes at a time,
 * shows the mark `is_mark` looks for; -1 if it never does. */
static double first_mark(const char *cmd, size_t unit, double per_unit, int (*is_mark)(const uint8_t *, size_t)) {
    FILE *p = popen(cmd, "r");
    if (!p) return -1;
    uint8_t *b = malloc(unit);
    double at = -1;
    for (long i = 0; b && fread(b, 1, unit, p) == unit; i++)
        if (at < 0 && is_mark(b, unit)) at = (double)i * per_unit;
    free(b);
    pclose(p);
    return at;
}
/* A loud sample: the click. */
static int loud(const uint8_t *b, size_t n) { (void)n; int16_t s; memcpy(&s, b, 2); return s > 16000 || s < -16000; }
/* A bright picture: the flash (16×16 grey, mean above 200). */
static int bright(const uint8_t *b, size_t n) {
    unsigned long sum = 0;
    for (size_t i = 0; i < n; i++) sum += b[i];
    return sum / n > 200;
}

static void test_export_1080p(void) {
    if (!have("ffprobe") || !have("ffmpeg")) {
        SKIP("ffprobe and ffmpeg not installed: the copy is not checked by an independent reader");
        return;
    }
    char dir[256], src[512], out[512];
    CHECK(oc_test_mkdtemp("export-media", dir, sizeof dir) == 0);
    oc_test_path(src, sizeof src, dir, "recording.mp4");
    oc_test_path(out, sizeof out, dir, "copy.mp4");
    uint32_t src_ms = 0;
    CHECK(make_recording(src, &src_ms) == 0);
    CHECK(src_ms >= SECS * 1000 - 40 && src_ms <= SECS * 1000 + 40);

    /* Exported from a file read front to back, as the blob store gives it, with
     * the peak measured from here. */
    FILE *in = fopen(src, "rb");
    CHECK(in != NULL);
    if (!in) { oc_test_rmtree(dir); return; }
    fseek(in, 0, SEEK_END);
    long size = ftell(in);
    fseek(in, 0, SEEK_SET);
    long rss0 = status_kb("VmRSS");
    CHECK(reset_peak() == 0);
    uint64_t bytes = 0;
    uint32_t ms = 0;
    char err[200] = "";
    oc_export_status st = oc_export_render(oc_export_codecs_real(), file_read, in, (uint64_t)size, out, NULL,
                                           &bytes, &ms, err, sizeof err);
    long peak = status_kb("VmHWM");
    fclose(in);
    if (st != OC_EXPORT_OK) printf("  export failed: %s\n", err);
    CHECK(st == OC_EXPORT_OK && bytes > 0);
    /* The transcode's own working memory: the peak over what was held before. */
    CHECK(rss0 > 0 && peak > 0);
    if (rss0 > 0 && peak > 0 && peak - rss0 >= PEAK_MB * 1024) printf("  peak %ld kB over %ld kB\n", peak, rss0);
    CHECK(rss0 > 0 && peak > 0 && peak - rss0 < PEAK_MB * 1024);

    /* What the file is: one H.264 Constrained Baseline stream at the source's
     * size and one MP3 stream, the picture's duration within a frame of the source's. */
    char cmd[1200], line[256];
    snprintf(cmd, sizeof cmd, "ffprobe -v error -show_entries stream=codec_name,profile,width,height,duration "
                              "-of compact=p=0:nk=0 '%s' 2>&1", out);
    FILE *p = popen(cmd, "r");
    int h264 = 0, mp3 = 0, streams = 0;
    double vdur = -1;
    while (p && fgets(line, sizeof line, p)) {
        streams++;
        /* contract: ffprobe's own names for the codec and the H.264 profile, which the criterion is about */
        if (strstr(line, "codec_name=h264") && strstr(line, "profile=Constrained Baseline") &&
            strstr(line, "width=1920") && strstr(line, "height=1080")) {
            h264 = 1;
            const char *d = strstr(line, "duration=");
            if (d) vdur = atof(d + 9);
        }
        if (strstr(line, "codec_name=mp3")) mp3 = 1;
    }
    if (p) pclose(p);
    CHECK(streams == 2 && h264 && mp3);
    CHECK(fabs(vdur * 1000.0 - (double)src_ms) <= 1000.0 / FPS);

    /* Where the marks fall: the flash and the click within a frame of each other,
     * which they are only if the edit list trims the MP3 encoder's delay. */
    snprintf(cmd, sizeof cmd, "ffmpeg -v error -i '%s' -map 0:a:0 -f s16le -ac 1 -ar %d - 2>/dev/null", out, RATE);
    double ta = first_mark(cmd, 2, 1.0 / RATE, loud);
    snprintf(cmd, sizeof cmd, "ffmpeg -v error -i '%s' -map 0:v:0 -fps_mode passthrough "
                              "-vf scale=16:16,format=gray -f rawvideo - 2>/dev/null", out);
    double tv = first_mark(cmd, 16 * 16, 1.0 / FPS, bright);
    if (fabs(ta - tv) > 1.0 / FPS) printf("  click at %.4f s, flash at %.4f s\n", ta, tv);
    CHECK(ta > 0 && tv > 0 && fabs(ta - MARK_S) <= 1.0 / FPS && fabs(ta - tv) <= 1.0 / FPS);
    oc_test_rmtree(dir);
}

int run_export_media_tests(void) {
    printf("test_export_media: a 1080p recording exported with the real codecs -- H.264 Constrained Baseline and MP3, the size and duration kept, picture and sound within a frame, the peak memory under %d MB\n", PEAK_MB);
    test_export_1080p();
    return failures;
}
