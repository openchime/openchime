/* Tests for read-aloud rendering and the render worker (daemon/tts_render.c,
 * daemon/tts_worker.c; ARCH-111, READ-ALOUD.md §4), with a stub engine in place
 * of the voice model: a tone ten milliseconds long per character of a segment.
 * What they prove: a render is a valid audio-only Opus MP4 of the right length;
 * silent and failing segments; the worker renders in order, stores under the
 * handle's key, wakes the net loop through its eventfd, refuses when its queue is
 * full, opens the engine only when needed and closes it when idle. */
#include "check.h"
#include "oc_mp4.h"
#include "tts_worker.h"

#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define TMPDIR "build/oc-tts-worker-test"

/* ---- the stub engine ---- */

static pthread_mutex_t stub_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  stub_cv = PTHREAD_COND_INITIALIZER;
static int stub_hold, stub_opens, stub_closes, stub_saying;

static void *stub_open(void *ctx, char *err, size_t errcap) {
    (void)ctx; (void)err; (void)errcap;
    pthread_mutex_lock(&stub_mu);
    stub_opens++;
    pthread_mutex_unlock(&stub_mu);
    static int token;
    return &token;
}

static void stub_close(void *engine) {
    (void)engine;
    pthread_mutex_lock(&stub_mu);
    stub_closes++;
    pthread_mutex_unlock(&stub_mu);
}

static int stub_say(void *engine, const char *segment, int voice, float **pcm, size_t *samples,
                    char *err, size_t errcap) {
    (void)engine;
    pthread_mutex_lock(&stub_mu);
    stub_saying = 1;
    pthread_cond_broadcast(&stub_cv);
    while (stub_hold) pthread_cond_wait(&stub_cv, &stub_mu);
    stub_saying = 0;
    pthread_mutex_unlock(&stub_mu);
    *pcm = NULL;
    *samples = 0;
    if (strstr(segment, "boom")) { snprintf(err, errcap, "the stub was told to fail"); return -1; }
    if (strstr(segment, "hush")) return 1;
    size_t n = strlen(segment) * 240;                     /* 10 ms a character at 24 kHz */
    *pcm = malloc(n * sizeof **pcm);
    if (!*pcm) return -1;
    double hz = voice == 0 ? 220.0 : 330.0;
    for (size_t i = 0; i < n; i++) (*pcm)[i] = (float)(0.5 * sin(2 * M_PI * hz * (double)i / 24000.0));
    *samples = n;
    return 0;
}

static const char *stub_voice_id(int v) { return v == 0 ? "stub-voice-m" : "stub-voice-f"; }
static const char *stub_voice_label(int v) { return v == 0 ? "Stub Low" : "Stub High"; }

static const oc_tts_engine STUB = {
    .version = "stub-1", .rate = 24000, .voices = 2, .ctx = NULL,
    .voice_id = stub_voice_id, .voice_label = stub_voice_label,
    .preview = "A stub reads this.",
    .open = stub_open, .close = stub_close, .say = stub_say,
};

static int count(int *v) {
    pthread_mutex_lock(&stub_mu);
    int n = *v;
    pthread_mutex_unlock(&stub_mu);
    return n;
}

static void msleep(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* ---- rendering ---- */

static void test_render(void) {
    void *eng = stub_open(NULL, NULL, 0);
    uint8_t *mp4 = NULL;
    size_t len = 0;
    uint32_t ms = 0;
    char err[256] = "";
    /* Two segments, 30 and 21 characters: 510 ms of tone. */
    const char *text = "The deploy logs look clean now. Ship it tonight.";
    CHECK(oc_tts_render(&STUB, eng, text, 0, &mp4, &len, &ms, err, sizeof err) == OC_TTS_OK);
    oc_mp4_info info;
    CHECK(mp4 && oc_mp4_parse(mp4, len, &info) == 0);
    if (mp4 && info.audio.present) {
        CHECK(!info.video.present && info.opus_channels == 1 && info.audio.timescale == 48000);
        /* Whole 20 ms packets: the tone rounded up, plus one of flush, less the pre-skip. */
        CHECK(ms >= 480 && ms <= 560);
        CHECK(info.duration_ms == ms);
        /* 24 kbit/s is about 3 KB a second; a tone is easy, so allow well under. */
        CHECK(len < 4000);
        oc_mp4_info_free(&info);
    }
    free(mp4);

    CHECK(oc_tts_render(&STUB, eng, "hush now.", 0, &mp4, &len, &ms, err, sizeof err) == OC_TTS_NOTHING && !mp4);
    CHECK(oc_tts_render(&STUB, eng, "", 0, &mp4, &len, &ms, err, sizeof err) == OC_TTS_NOTHING && !mp4);
    err[0] = '\0';
    CHECK(oc_tts_render(&STUB, eng, "fine. boom.", 1, &mp4, &len, &ms, err, sizeof err) == OC_TTS_FAILED && !mp4 && err[0]);
    CHECK(oc_tts_render(&STUB, eng, "fine.", 2, &mp4, &len, &ms, err, sizeof err) == OC_TTS_FAILED);   /* no voice 2 */
    /* A segment with nothing to say among ones that do is simply left out. Short
     * sentences share a segment of up to 120 characters, so the silent one is kept
     * apart by the length of the sentence after it (120 characters with its stop). */
    char mixed[256];
    snprintf(mixed, sizeof mixed, "hush. %s.", "Then this sentence, which runs long enough that it cannot share a segment with the short one before it, not at all here");
    CHECK(strlen(mixed) == 6 + 120);
    CHECK(oc_tts_render(&STUB, eng, mixed, 1, &mp4, &len, &ms, err, sizeof err) == OC_TTS_OK && ms >= 1190 && ms <= 1260);
    free(mp4);
    stub_close(eng);
}

/* ---- the worker ---- */

static int wait_result(oc_tts_worker *w, oc_tts_result *r, int timeout_ms) {
    for (int t = 0; t < timeout_ms; t += 10) {
        if (oc_tts_worker_next_result(w, r)) return 1;
        msleep(10);
    }
    return 0;
}

static void test_worker(void) {
    mkdir("build", 0755);
    mkdir(TMPDIR, 0700);
    oc_blobstore *bs = oc_blobstore_open(TMPDIR);
    CHECK(bs != NULL);
    if (!bs) return;
    stub_opens = stub_closes = 0;

    oc_tts_worker *w = oc_tts_worker_start(&STUB, bs, 2, 150);
    CHECK(w != NULL);
    if (!w) { oc_blobstore_close(bs); return; }
    CHECK(!oc_tts_worker_engine_open(w) && count(&stub_opens) == 0);   /* nothing loaded until needed */

    /* In order, stored under the handle's key, readable back as the file. */
    uint8_t h1[32], h2[32], h3[32];
    memset(h1, 1, 32); memset(h2, 2, 32); memset(h3, 3, 32);
    CHECK(oc_tts_worker_submit(w, 11, h1, 0, "First message here.") == 0);
    CHECK(oc_tts_worker_submit(w, 12, h2, 1, "hush.") == 0);
    oc_tts_result r;
    CHECK(wait_result(w, &r, 5000) && r.req_id == 11 && r.status == OC_TTS_OK);
    struct pollfd pfd = { oc_tts_worker_eventfd(w), POLLIN, 0 };
    CHECK(poll(&pfd, 1, 0) == 1);                          /* the net loop would have been woken */
    uint64_t drain;
    CHECK(read(pfd.fd, &drain, sizeof drain) == sizeof drain && drain >= 1);
    char want[OC_TTS_KEY_MAX];
    oc_tts_render_key(&STUB, h1, want, sizeof want);
    CHECK(strcmp(r.key, want) == 0 && strlen(r.key) == 81 && strncmp(r.key, "0101", 4) == 0);
    uint64_t size = 0;
    oc_blob_reader *br = oc_blob_get_begin(bs, r.key, &size);
    CHECK(br && size == r.bytes && r.duration_ms > 0);
    if (br) {
        uint8_t *buf = malloc(size);
        long got = buf ? oc_blob_get_chunk(br, buf, size) : -1;
        oc_mp4_info info;
        CHECK(got == (long)size && oc_mp4_parse(buf, size, &info) == 0 && info.duration_ms == r.duration_ms);
        oc_mp4_info_free(&info);
        free(buf);
        oc_blob_get_close(br);
    }
    CHECK(wait_result(w, &r, 5000) && r.req_id == 12 && r.status == OC_TTS_NOTHING && r.key[0] == '\0');
    /* The same handle under another engine version is another key. */
    oc_tts_engine other = STUB;
    other.version = "stub-2";
    char other_key[OC_TTS_KEY_MAX];
    oc_tts_render_key(&other, h1, other_key, sizeof other_key);
    CHECK(strcmp(other_key, want) != 0);

    /* A full queue refuses: one render held in the engine, two waiting, the next refused. */
    pthread_mutex_lock(&stub_mu);
    stub_hold = 1;
    pthread_mutex_unlock(&stub_mu);
    CHECK(oc_tts_worker_submit(w, 21, h1, 0, "Held.") == 0);
    pthread_mutex_lock(&stub_mu);
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 5;
    while (!stub_saying && pthread_cond_timedwait(&stub_cv, &stub_mu, &deadline) == 0) {}
    int saying = stub_saying;
    pthread_mutex_unlock(&stub_mu);
    CHECK(saying);
    CHECK(oc_tts_worker_submit(w, 22, h2, 0, "Waiting one.") == 0);
    CHECK(oc_tts_worker_submit(w, 23, h3, 0, "Waiting two.") == 0);
    CHECK(oc_tts_worker_submit(w, 24, h3, 0, "No room.") == -1);
    CHECK(oc_tts_worker_submit(w, 25, h3, 5, "No such voice.") == -1);
    pthread_mutex_lock(&stub_mu);
    stub_hold = 0;
    pthread_cond_broadcast(&stub_cv);
    pthread_mutex_unlock(&stub_mu);
    int order_ok = 1;
    for (uint64_t want_id = 21; want_id <= 23; want_id++)
        if (!wait_result(w, &r, 5000) || r.req_id != want_id || r.status != OC_TTS_OK) order_ok = 0;
    CHECK(order_ok);
    CHECK(count(&stub_opens) == 1);                        /* opened once for all of it */

    /* Idle: the engine is closed, and the next render opens it again. */
    int closed = 0;
    for (int t = 0; t < 3000 && !closed; t += 20) { closed = !oc_tts_worker_engine_open(w); if (!closed) msleep(20); }
    CHECK(closed && count(&stub_closes) == 1);
    CHECK(oc_tts_worker_submit(w, 31, h1, 1, "Back again.") == 0);
    CHECK(wait_result(w, &r, 5000) && r.req_id == 31 && r.status == OC_TTS_OK);
    CHECK(count(&stub_opens) == 2);

    /* A failure is a result, with a reason. */
    CHECK(oc_tts_worker_submit(w, 41, h2, 0, "boom.") == 0);
    CHECK(wait_result(w, &r, 5000) && r.req_id == 41 && r.status == OC_TTS_FAILED && r.reason[0]);

    /* Stopping with renders still queued neither hangs nor leaks. */
    pthread_mutex_lock(&stub_mu);
    stub_hold = 1;
    pthread_mutex_unlock(&stub_mu);
    CHECK(oc_tts_worker_submit(w, 51, h1, 0, "Held at stop.") == 0);
    CHECK(oc_tts_worker_submit(w, 52, h2, 0, "Dropped at stop.") == 0);
    msleep(50);
    pthread_mutex_lock(&stub_mu);
    stub_hold = 0;
    pthread_cond_broadcast(&stub_cv);
    pthread_mutex_unlock(&stub_mu);
    oc_tts_worker_stop(w);
    CHECK(count(&stub_opens) == count(&stub_closes));      /* the engine is closed at stop */

    char key[OC_TTS_KEY_MAX];
    for (int i = 1; i <= 3; i++) {
        uint8_t h[32];
        memset(h, i, 32);
        oc_tts_render_key(&STUB, h, key, sizeof key);
        oc_blob_delete(bs, key);
    }
    oc_blobstore_close(bs);
}

int run_tts_worker_tests(void) {
    printf("test_tts_worker: render to audio-only Opus MP4, silent and failing segments, order, keys, eventfd, full queue, lazy open, idle close, stop\n");
    test_render();
    test_worker();
    return failures;
}
