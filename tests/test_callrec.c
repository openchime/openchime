/* Making a call's recording from its spool (daemon/callrec_render.c,
 * callrec_worker.c, CALLS.md §8), with the stand-in codecs: a spool written
 * here as the Recorder writes one, made into the recording, and the recording
 * read back -- the voices mixed, the speaker large and the others in the
 * strip, the speaker changing when someone else speaks, a shared screen large
 * while it is shared; the worker storing it and removing the spool; a worker
 * stopped in the middle finishing on the next start; and a damaged spool
 * making no recording. The real codecs are test_callrec_media.c's. */

#include "callrec.h"
#include "callrec_render.h"
#include "callrec_worker.h"
#include "blobstore.h"
#include "oc_mp4.h"
#include "check.h"
#include "testenv.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define T0   1700000000000ull     /* the recording began (ms) */
#define UA   11u                  /* alice: speaks first, her camera on */
#define UB   22u                  /* bob: speaks next, shares his screen a while */

static void w64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (56 - 8 * i)); }
static void w32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void w16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

static FILE *spool_file(const char *dir, const char *name) {
    char path[700];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    return fopen(path, "wb");
}

static void tl(FILE *f, uint64_t at, uint8_t kind, uint64_t user) {
    uint8_t r[17]; w64(r, at); r[8] = kind; w64(r + 9, user); fwrite(r, 1, sizeof r, f);
}

/* A voice: `secs` seconds of frames, 20 ms apart, each "packet" its level --
 * `loud` from `from_ms` to `to_ms`, silent otherwise. With `dtx`, as a device
 * sends: a few silent frames, then none until it speaks. */
static void voice(const char *dir, uint64_t user, int secs, uint8_t loud, int from_ms, int to_ms, int dtx) {
    char name[32]; snprintf(name, sizeof name, "audio.%llu", (unsigned long long)user);
    FILE *f = spool_file(dir, name);
    for (int i = 0; f && i < secs * 50; i++) {
        if (dtx && i >= 5 && !(i * 20 >= from_ms && i * 20 < to_ms)) continue;
        uint8_t h[15];
        /* With dtx, the device came back at 4 s and counts again from 0. */
        uint32_t fno = dtx && i >= 200 ? (uint32_t)(i - 200) : 1000u + (uint32_t)i;
        w64(h, T0 + (uint64_t)i * 20); w32(h + 8, fno); w16(h + 12, 1);
        h[14] = (i * 20 >= from_ms && i * 20 < to_ms) ? loud : 0;
        fwrite(h, 1, sizeof h, f);
    }
    if (f) fclose(f);
}

/* A picture stream: one "frame" of luma `y` every 1/15 s from `from_ms` to
 * `to_ms`, the first a keyframe. A share's records carry the sharer. */
static void picture(const char *dir, const char *name, uint64_t sharer, uint8_t y, int from_ms, int to_ms) {
    FILE *f = spool_file(dir, name);
    int first = 1;
    for (int t = from_ms; f && t < to_ms; t += 66, first = 0) {
        uint8_t h[26];
        size_t n;
        w64(h, T0 + (uint64_t)t);
        if (sharer) { w64(h + 8, sharer); h[16] = (uint8_t)first; w16(h + 17, 64); w16(h + 19, 48); w32(h + 21, 1); h[25] = y; n = 26; }
        else        { w32(h + 8, 0); h[12] = (uint8_t)first; w16(h + 13, 64); w16(h + 15, 48); w32(h + 17, 1); h[21] = y; n = 22; }
        fwrite(h, 1, n, f);
    }
    if (f) fclose(f);
}

/* A spool of a six-second call: alice speaks for two seconds, bob for the
 * rest; alice's camera is on throughout; bob shares his screen from 3 s to 5 s. */
static void make_spool(const char *dir) {
    mkdir(dir, 0700);
    FILE *m = spool_file(dir, "meta");
    uint8_t h[30];
    memcpy(h, "OCREC1", 6); w64(h + 6, 7); w64(h + 14, 3); w64(h + 22, T0);
    if (m) { fwrite(h, 1, sizeof h, m); fclose(m); }
    FILE *t = spool_file(dir, "timeline");
    if (t) {
        tl(t, T0, OC_CALLREC_TL_JOIN, UA);
        tl(t, T0, OC_CALLREC_TL_JOIN, UB);
        tl(t, T0, OC_CALLREC_TL_CAMERA_ON, UA);
        tl(t, T0 + 3000, OC_CALLREC_TL_SHARE_ON, UB);
        tl(t, T0 + 5000, OC_CALLREC_TL_SHARE_OFF, UB);
        fclose(t);
    }
    voice(dir, UA, 6, 50, 0, 2000, 0);
    voice(dir, UB, 6, 60, 2000, 6000, 1);   /* silent until 2 s: nothing sent */
    char cam[32]; snprintf(cam, sizeof cam, "camera.%u", UA);
    picture(dir, cam, 0, 200, 0, 6000);
    picture(dir, "share", UB, 30, 3000, 5000);
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

/* The stand-in's picture at `ms`: the large area's luma, then the strip's. */
static const uint8_t *frame_at(const uint8_t *file, const oc_mp4_info *in, uint32_t ms) {
    uint64_t dts = (uint64_t)ms * OC_MP4_VIDEO_TIMESCALE / 1000;
    uint32_t k = 0;
    for (uint32_t i = 0; i < in->video.n_samples; i++) if (in->video.samples[i].dts <= dts) k = i;
    return file + in->video.samples[k].offset;
}

static int dir_exists(const char *d) { struct stat sb; return stat(d, &sb) == 0 && S_ISDIR(sb.st_mode); }

static void test_render(void) {
    char dir[256], spool[400], out[400];
    CHECK(oc_test_mkdtemp("callrec-render", dir, sizeof dir) == 0);
    oc_test_path(spool, sizeof spool, dir, "spool");
    oc_test_path(out, sizeof out, dir, "rec.mp4");
    make_spool(spool);
    uint64_t bytes = 0; uint32_t dur = 0; char err[200] = "";
    CHECK(oc_callrec_render(&oc_callrec_stub_codecs, spool, out, NULL, &bytes, &dur, err, sizeof err) == OC_CALLREC_OK);
    size_t len = 0;
    uint8_t *file = slurp(out, &len);
    CHECK(file && len == bytes);
    oc_mp4_info in;
    memset(&in, 0, sizeof in);
    CHECK(file && oc_mp4_parse(file, len, &in) == 0);       /* the profile the clients play */
    CHECK(in.width == OC_CALLREC_WIDTH && in.height == OC_CALLREC_HEIGHT);
    CHECK(in.video.n_samples >= 88 && in.video.n_samples <= 92);   /* 6 s at 15 fps */
    CHECK(in.audio.n_samples >= 299 && in.audio.n_samples <= 300);  /* 6 s of 20 ms */
    CHECK(dur >= 5900 && dur <= 6100);
    if (file && in.video.n_samples) {
        uint8_t by, bu, bv;   /* bob's colour, as a tile without a picture shows it */
        (void)bu; (void)bv;
        {   /* person_colour, as the render picks it */
            uint64_t h = (uint64_t)UB * 0x9E3779B97F4A7C15ull;
            by = (uint8_t)(70 + (h >> 56) % 100);
        }
        /* At 1 s alice speaks: her camera large, bob in the strip. */
        const uint8_t *f = frame_at(file, &in, 1000);
        CHECK(f[0] == 200 && f[1] == by && f[2] == 24);   /* 24: the strip with no tile there */
        /* At 2.8 s bob has spoken long enough: he is large, alice's camera in the strip. */
        f = frame_at(file, &in, 2800);
        CHECK(f[0] == by && f[1] == 200 && f[2] == 24);
        /* At 4 s bob shares: the screen large, and both of them in the strip. */
        f = frame_at(file, &in, 4000);
        CHECK(f[0] == 30 && f[3] == 24);
        CHECK((f[1] == 200 && f[2] == by) || (f[1] == by && f[2] == 200));
        /* At 5.5 s the share is over: bob large again. */
        f = frame_at(file, &in, 5500);
        CHECK(f[0] == by);
        /* The mix: alice's level, then bob's (the stand-in's levels × 100). */
        int a1 = (file[in.audio.samples[25].offset] << 8) | file[in.audio.samples[25].offset + 1];
        int b1 = (file[in.audio.samples[200].offset] << 8) | file[in.audio.samples[200].offset + 1];
        CHECK(a1 == 5000 && b1 == 6000);
        /* Bob's speech after his silence plays when he said it, not early:
         * just before 2 s only alice is heard, just after only bob. */
        int a2 = (file[in.audio.samples[95].offset] << 8) | file[in.audio.samples[95].offset + 1];
        int b2 = (file[in.audio.samples[102].offset] << 8) | file[in.audio.samples[102].offset + 1];
        CHECK(a2 == 5000 && b2 == 6000);
        /* ...and after he came back at 4 s, counting afresh, he is still heard. */
        int b3 = (file[in.audio.samples[250].offset] << 8) | file[in.audio.samples[250].offset + 1];
        CHECK(b3 == 6000);
    }
    oc_mp4_info_free(&in);
    free(file);

    /* Cancelled in the middle: no file, the spool as it was. */
    unlink(out);
    volatile int cancel = 1;
    CHECK(oc_callrec_render(&oc_callrec_stub_codecs, spool, out, &cancel, &bytes, &dur, err, sizeof err) == OC_CALLREC_CANCELLED);
    CHECK(access(out, F_OK) != 0 && dir_exists(spool));

    /* A damaged spool: a voice cut off in the middle of a frame. */
    char apath[500];
    snprintf(apath, sizeof apath, "%s/audio.%u", spool, UA);
    FILE *f = fopen(apath, "ab");
    if (f) { uint8_t half[7] = { 0 }; fwrite(half, 1, sizeof half, f); fclose(f); }
    CHECK(oc_callrec_render(&oc_callrec_stub_codecs, spool, out, NULL, &bytes, &dur, err, sizeof err) == OC_CALLREC_FAILED);
    CHECK(access(out, F_OK) != 0 && err[0]);
    /* An empty one: nothing to make. */
    char empty[400];
    oc_test_path(empty, sizeof empty, dir, "empty");
    mkdir(empty, 0700);
    FILE *m = spool_file(empty, "meta");
    uint8_t h[30]; memcpy(h, "OCREC1", 6); w64(h + 6, 1); w64(h + 14, 1); w64(h + 22, T0);
    if (m) { fwrite(h, 1, sizeof h, m); fclose(m); }
    CHECK(oc_callrec_render(&oc_callrec_stub_codecs, empty, out, NULL, &bytes, &dur, err, sizeof err) == OC_CALLREC_EMPTY);
    /* Longer than the profile holds (about 51 minutes): refused, and said so,
     * before any of it is made. */
    {
        char longs[400], ap[500];
        oc_test_path(longs, sizeof longs, dir, "long");
        mkdir(longs, 0700);
        FILE *lm = spool_file(longs, "meta");
        if (lm) { fwrite(h, 1, sizeof h, lm); fclose(lm); }
        snprintf(ap, sizeof ap, "%s/audio.%u", longs, UA);
        FILE *la = fopen(ap, "wb");
        for (int i = 0; la && i < 2; i++) {
            uint8_t r[15];
            w64(r, T0 + (uint64_t)i * 52 * 60 * 1000); w32(r + 8, 7u + (uint32_t)i * 156000u); w16(r + 12, 1); r[14] = 9;
            fwrite(r, 1, sizeof r, la);
        }
        if (la) fclose(la);
        err[0] = '\0';
        CHECK(oc_callrec_render(&oc_callrec_stub_codecs, longs, out, NULL, &bytes, &dur, err, sizeof err) == OC_CALLREC_FAILED);
        /* contract: the reason the recording's row carries, as the page shows it */
        CHECK(strstr(err, "51 minutes") != NULL && access(out, F_OK) != 0);
    }
    oc_test_rmtree(dir);
}

static int next_result(oc_callrec_worker *w, oc_callrec_result *r) {
    return WAIT_UNTIL(oc_callrec_worker_next_result(w, r), 10000);
}

static void test_worker(void) {
    char dir[256], blobs[400], tmp[400], spool[400], bad[400];
    CHECK(oc_test_mkdtemp("callrec-worker", dir, sizeof dir) == 0);
    oc_test_path(blobs, sizeof blobs, dir, "blobs");
    oc_test_path(tmp, sizeof tmp, dir, "recordings");
    mkdir(tmp, 0700);
    oc_test_path(spool, sizeof spool, tmp, "3");
    make_spool(spool);
    oc_blobstore *bs = oc_blobstore_open(blobs, NULL);
    CHECK(bs != NULL);
    if (!bs) { oc_test_rmtree(dir); return; }

    /* Stopped before it is done: the spool stays for the next start. */
    oc_callrec_worker *w = oc_callrec_worker_start(&oc_callrec_stub_codecs, bs, tmp, NULL, NULL);
    CHECK(w != NULL);
    oc_callrec_worker_hold(w, 1);
    CHECK(oc_callrec_worker_submit(w, 3, spool) == 0);
    CHECK(oc_callrec_worker_submit(w, 3, spool) == 0);   /* asked twice, made once */
    oc_callrec_worker_stop(w);
    CHECK(dir_exists(spool));

    /* The next start makes it: stored under its key, the spool gone. */
    w = oc_callrec_worker_start(&oc_callrec_stub_codecs, bs, tmp, NULL, NULL);
    CHECK(w != NULL);
    CHECK(oc_callrec_worker_submit(w, 3, spool) == 0);
    oc_callrec_result r;
    CHECK(next_result(w, &r) && r.rec_id == 3 && r.status == OC_CALLREC_OK && r.bytes > 0 && r.duration_ms >= 5900);
    char key[OC_CALLREC_KEY_MAX];
    oc_callrec_blob_key(3, key, sizeof key);
    CHECK(strcmp(r.key, key) == 0 && strcmp(key, "ca11_0000000000000003") == 0);
    uint64_t size = 0;
    oc_blob_reader *rd = oc_blob_get_begin(bs, key, &size);
    CHECK(rd != NULL && size == r.bytes);
    if (rd) oc_blob_get_close(rd);
    CHECK(!dir_exists(spool));

    /* A damaged spool: failed, with why, and the spool gone too. */
    oc_test_path(bad, sizeof bad, tmp, "4");
    make_spool(bad);
    char vpath[500];
    snprintf(vpath, sizeof vpath, "%s/camera.%u", bad, UA);
    FILE *f = fopen(vpath, "ab");
    if (f) { uint8_t half[9] = { 0 }; fwrite(half, 1, sizeof half, f); fclose(f); }
    CHECK(oc_callrec_worker_submit(w, 4, bad) == 0);
    CHECK(next_result(w, &r) && r.rec_id == 4 && r.status == OC_CALLREC_FAILED && r.reason[0]);
    CHECK(!dir_exists(bad));
    oc_callrec_worker_stop(w);
    oc_blobstore_close(bs);
    oc_test_rmtree(dir);
}

/* The file-backed writer: what it makes is what the in-memory one makes. */
static void test_file_writer(void) {
    char dir[256], path[400];
    CHECK(oc_test_mkdtemp("callrec-mp4", dir, sizeof dir) == 0);
    oc_test_path(path, sizeof path, dir, "x.mp4");
    oc_mp4_writer *a = oc_mp4_writer_open(64, 48), *b = oc_mp4_writer_open_file(path, 64, 48);
    CHECK(a && b);
    static const uint8_t v[3] = { 1, 2, 3 }, s[2] = { 9, 9 };
    for (int i = 0; a && b && i < 10; i++) {
        CHECK(oc_mp4_write_video(a, v, sizeof v, i * 66666, i == 0) == 0);
        CHECK(oc_mp4_write_video(b, v, sizeof v, i * 66666, i == 0) == 0);
        CHECK(oc_mp4_write_audio(a, s, sizeof s, 960) == 0);
        CHECK(oc_mp4_write_audio(b, s, sizeof s, 960) == 0);
    }
    uint8_t *mem = NULL; size_t mlen = 0; uint32_t d1 = 0, d2 = 0; uint64_t flen = 0;
    CHECK(oc_mp4_writer_finish(a, &mem, &mlen, &d1) == 0);
    CHECK(oc_mp4_writer_finish_file(b, &flen, &d2) == 0);
    size_t got = 0;
    uint8_t *disk = slurp(path, &got);
    CHECK(disk && got == mlen && flen == mlen && d1 == d2 && memcmp(disk, mem, mlen) == 0);
    char part[500];
    snprintf(part, sizeof part, "%s.part", path);
    CHECK(access(part, F_OK) != 0);                         /* the part is gone */
    free(disk); free(mem);
    oc_test_rmtree(dir);
}

int run_callrec_tests(void) {
    printf("test_callrec: the recording from a spool -- mix, speaker layout, share; the worker; a damaged spool\n");
    test_file_writer();
    test_render();
    test_worker();
    return failures;
}
