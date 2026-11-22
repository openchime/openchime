/* Tests for voice input's daemon half that need no model (ARCH-112,
 * docs/details/VOICE-INPUT.md §6): spoken mentions, and the recognition worker with a
 * stub engine -- order, a full queue, the eventfd, opening only when needed and
 * closing when idle. The recognizer itself is tests/test_sttkit.c's. */
#include "check.h"
#include "config.h"
#include "stt_mentions.h"
#include "stt_worker.h"

#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ---- spoken mentions ---- */

static void test_mentions(void) {
    int failures_before = failures;
    const char *names[] = { "dana", "Bob", "jo.smith", "Sam Lee", "twin", "Twin" };
    char out[256];
    oc_stt_mentions("Send the report to at Dana.", names, 6, out, sizeof out);
    CHECK(strcmp(out, "Send the report to @dana.") == 0);   /* contract: the transcript as rewritten, exact */
    oc_stt_mentions("At bob, look at this.", names, 6, out, sizeof out);
    CHECK(strcmp(out, "@Bob, look at this.") == 0);                /* contract: the transcript as rewritten; "at this" is nobody */
    oc_stt_mentions("ping at jo.smith now", names, 6, out, sizeof out);
    CHECK(strcmp(out, "ping @jo.smith now") == 0);   /* contract: the transcript as rewritten, exact */
    oc_stt_mentions("ask at Sam about it", names, 6, out, sizeof out);
    CHECK(strcmp(out, "ask at Sam about it") == 0);                /* contract: the transcript as rewritten; "Sam Lee" cannot be a mention */
    oc_stt_mentions("tell at twin", names, 6, out, sizeof out);
    CHECK(strcmp(out, "tell at twin") == 0);                       /* contract: the transcript as rewritten; two people: stays words */
    oc_stt_mentions("meet at noon", names, 6, out, sizeof out);
    CHECK(strcmp(out, "meet at noon") == 0);   /* contract: the transcript as rewritten, exact */
    oc_stt_mentions("that danasaur", names, 6, out, sizeof out);
    CHECK(strcmp(out, "that danasaur") == 0);   /* contract: the transcript as rewritten, exact */
    oc_stt_mentions("chat at dana", NULL, 0, out, sizeof out);
    CHECK(strcmp(out, "chat at dana") == 0);   /* contract: the transcript as rewritten, exact */
    if (failures == failures_before) printf("  spoken mentions ok\n");
}

/* ---- the worker, with a stub engine ---- */

static pthread_mutex_t stub_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  stub_cv = PTHREAD_COND_INITIALIZER;
static int stub_hold, stub_held, stub_opens, stub_closes;   /* stub_held: waiting in hold */

static void *stub_open(void *ctx, char *err, size_t cap) {
    (void)ctx; (void)err; (void)cap;
    pthread_mutex_lock(&stub_mu); stub_opens++; pthread_mutex_unlock(&stub_mu);
    static int token;
    return &token;
}
static void stub_close(void *e) {
    (void)e;
    pthread_mutex_lock(&stub_mu); stub_closes++; pthread_mutex_unlock(&stub_mu);
}
/* The text is the first sample as a number; a first sample of -1 fails, with
 * this reason: a token of the test's own, which the result must carry. */
#define STUB_REASON "stub-refusal-7f3a"
static int stub_hear(void *e, const int16_t *pcm, size_t n, char **text, char *err, size_t cap) {
    (void)e;
    pthread_mutex_lock(&stub_mu);
    while (stub_hold) {
        if (!stub_held) { stub_held = 1; pthread_cond_broadcast(&stub_cv); }
        pthread_cond_wait(&stub_cv, &stub_mu);
    }
    stub_held = 0;
    pthread_mutex_unlock(&stub_mu);
    if (n && pcm[0] == -1) { snprintf(err, cap, "%s", STUB_REASON); return -1; }
    char buf[32];
    snprintf(buf, sizeof buf, "%d", n ? pcm[0] : 0);
    *text = strdup(buf);
    return 0;
}
static const oc_stt_engine STUB = {
    .version = "stub-stt-1", .lang = "en-US", .ctx = NULL,
    .open = stub_open, .close = stub_close, .hear = stub_hear,
};

static int16_t *seg(int16_t first) {
    int16_t *p = calloc(160, sizeof *p);
    if (p) {   /* as the wire carries it: little-endian */
        uint8_t *b = (uint8_t *)p;
        b[0] = (uint8_t)((uint16_t)first & 0xFF);
        b[1] = (uint8_t)((uint16_t)first >> 8);
    }
    return p;
}

/* The next result, waited for on the worker's eventfd until a deadline on the
 * monotonic clock. */
static int wait_result(oc_stt_worker *w, oc_stt_result *r) {
    struct pollfd pf = { oc_stt_worker_eventfd(w), POLLIN, 0 };
    uint64_t end = oc_test_deadline(4000);
    for (;;) {
        if (oc_stt_worker_next_result(w, r)) return 1;
        uint64_t now = oc_test_mono_ms();
        if (now >= end) return oc_stt_worker_next_result(w, r);
        poll(&pf, 1, (int)(end - now < 50 ? end - now : 50));
        uint64_t cnt;
        while (read(pf.fd, &cnt, sizeof cnt) > 0) {}
    }
}

static void test_worker(void) {
    int failures_before = failures;
    stub_opens = stub_closes = 0;
    oc_stt_worker *w = oc_stt_worker_start(&STUB, 2, 100);
    CHECK(w != NULL);
    CHECK(!oc_stt_worker_engine_open(w));                 /* nothing loaded until needed */

    /* In order, one result per segment. */
    CHECK(oc_stt_worker_submit(w, 1, seg(11), 160) == 0);
    CHECK(oc_stt_worker_submit(w, 2, seg(22), 160) == 0);
    oc_stt_result r;
    CHECK(wait_result(w, &r) && r.req_id == 1 && r.ok && strcmp(r.text, "11") == 0);
    free(r.text);
    CHECK(wait_result(w, &r) && r.req_id == 2 && r.ok && strcmp(r.text, "22") == 0);
    free(r.text);
    pthread_mutex_lock(&stub_mu);
    CHECK(stub_opens == 1);
    pthread_mutex_unlock(&stub_mu);

    /* A failure is a result, with a reason. */
    CHECK(oc_stt_worker_submit(w, 3, seg(-1), 160) == 0);
    CHECK(wait_result(w, &r) && r.req_id == 3 && !r.ok && r.text == NULL && strstr(r.reason, STUB_REASON));

    /* A full queue refuses rather than grows; the audio is freed either way. */
    pthread_mutex_lock(&stub_mu); stub_hold = 1; pthread_mutex_unlock(&stub_mu);
    CHECK(oc_stt_worker_submit(w, 4, seg(4), 160) == 0);   /* taken by the worker, held: */
    pthread_mutex_lock(&stub_mu);                            /* ...waited for, not slept on */
    for (uint64_t end = oc_test_deadline(4000); !stub_held && oc_test_mono_ms() < end; ) {
        struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl);   /* the condition variable's own clock */
        dl.tv_nsec += 10 * 1000000L; if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&stub_cv, &stub_mu, &dl);
    }
    CHECK(stub_held);
    pthread_mutex_unlock(&stub_mu);
    CHECK(oc_stt_worker_submit(w, 5, seg(5), 160) == 0);
    CHECK(oc_stt_worker_submit(w, 6, seg(6), 160) == 0);
    CHECK(oc_stt_worker_submit(w, 7, seg(7), 160) != 0);   /* two waiting already */
    pthread_mutex_lock(&stub_mu); stub_hold = 0; pthread_cond_broadcast(&stub_cv); pthread_mutex_unlock(&stub_mu);
    for (uint64_t want = 4; want <= 6; want++) {
        CHECK(wait_result(w, &r) && r.req_id == want && r.ok);
        free(r.text);
    }

    /* Idle: the engine closes, and opens again for the next segment. */
    CHECK(WAIT_UNTIL(!oc_stt_worker_engine_open(w), 4000));
    pthread_mutex_lock(&stub_mu);
    CHECK(stub_closes == 1);
    pthread_mutex_unlock(&stub_mu);
    CHECK(oc_stt_worker_submit(w, 8, seg(8), 160) == 0);
    CHECK(wait_result(w, &r) && r.req_id == 8 && r.ok);
    free(r.text);
    pthread_mutex_lock(&stub_mu);
    CHECK(stub_opens == 2);
    pthread_mutex_unlock(&stub_mu);
    oc_stt_worker_stop(w);
    if (failures == failures_before) printf("  recognition worker ok\n");
}

/* ---- long segments, heard in windows ---- */

/* An engine that records each piece it is handed and answers by its own count:
 * "w1", "w2", ...; the piece numbered `win_blank` answers only spaces, and the
 * one numbered `win_fail` fails. */
static size_t win_len[64], win_off[64], win_calls;
static int win_fail, win_blank;
static const int16_t *win_base;
static int win_hear(void *e, const int16_t *pcm, size_t n, char **text, char *err, size_t cap) {
    (void)e;
    if (win_calls < 64) { win_len[win_calls] = n; win_off[win_calls] = (size_t)(pcm - win_base); }
    win_calls++;
    if ((int)win_calls == win_fail) { snprintf(err, cap, "%s", STUB_REASON); return -1; }
    char buf[16];
    if ((int)win_calls == win_blank) snprintf(buf, sizeof buf, "  ");
    else snprintf(buf, sizeof buf, " w%zu ", win_calls);
    *text = strdup(buf);
    return *text ? 0 : -1;
}
static const oc_stt_engine WIN = { .version = "stub-win", .lang = "en-US", .hear = win_hear };

/* `secs` of loud signal with 100 ms of silence starting at each of `quiet`. */
static int16_t *speech(double secs, const double *quiet, int nquiet, size_t *n) {
    *n = (size_t)(secs * 16000);
    int16_t *p = malloc(*n * sizeof *p);
    if (!p) return NULL;
    for (size_t i = 0; i < *n; i++) p[i] = (int16_t)((i & 1) ? 12000 : -12000);
    for (int q = 0; q < nquiet; q++)
        for (size_t i = (size_t)(quiet[q] * 16000); i < (size_t)((quiet[q] + 0.1) * 16000) && i < *n; i++) p[i] = 0;
    return p;
}

static void test_windows(void) {
    int failures_before = failures;
    const size_t max = (size_t)OC_STT_WINDOW_SECS * 16000, search = (size_t)OC_STT_CUT_SEARCH_MS * 16;
    char err[160], *text = NULL;

    /* A window ends in the quiet, short of the limit, never past it. */
    const double one_gap[] = { 5.0 };
    size_t n;
    int16_t *p = speech(30, one_gap, 1, &n);
    CHECK(p != NULL);
    size_t cut = p ? oc_stt_window(p, n, max, search) : 0;
    CHECK(cut >= 80000 && cut <= 81600);                          /* inside 5.0-5.1 s */
    CHECK(p && oc_stt_window(p, 50000, max, search) == 50000);   /* fits: all of it */
    free(p);

    /* No quiet in reach: the window is cut at a 20 ms step no later than the limit. */
    p = speech(30, NULL, 0, &n);
    cut = p ? oc_stt_window(p, n, max, search) : 0;
    CHECK(cut > max - search && cut <= max);
    free(p);

    /* 30 s of speech with a pause every five and a half seconds: every piece is at
     * most the window, the pieces cover the clip once in order, and the words come
     * back joined by one space; a piece that hears only spaces adds nothing. */
    const double gaps[] = { 5.5, 11.0, 16.5, 22.0, 27.5 };
    p = speech(30, gaps, 5, &n);
    win_base = p; win_calls = 0; win_fail = 0; win_blank = 3;
    err[0] = '\0';
    CHECK(p && oc_stt_hear_windowed(&WIN, NULL, p, n, &text, err, sizeof err) == 0);
    CHECK(win_calls == 6);
    size_t covered = 0;
    for (size_t i = 0; i < win_calls && i < 64; i++) {
        CHECK(win_len[i] > 0 && win_len[i] <= max);
        CHECK(win_off[i] == covered);
        covered += win_len[i];
    }
    CHECK(covered == n);
    CHECK(text && strcmp(text, "w1 w2 w4 w5 w6") == 0);
    free(text); text = NULL;

    /* A segment that fits is heard in one piece, as the engine said it. */
    win_calls = 0; win_blank = 0;
    CHECK(p && oc_stt_hear_windowed(&WIN, NULL, p, max, &text, err, sizeof err) == 0);
    CHECK(win_calls == 1 && text && strcmp(text, " w1 ") == 0);
    free(text); text = NULL;

    /* A window that fails fails the segment, with its reason. */
    win_calls = 0; win_fail = 2; err[0] = '\0';
    CHECK(p && oc_stt_hear_windowed(&WIN, NULL, p, n, &text, err, sizeof err) == -1);
    CHECK(text == NULL && strstr(err, STUB_REASON) && win_calls == 2);
    free(p);
    if (failures == failures_before) printf("  long segments heard in windows ok\n");
}

int run_stt_tests(void) {
    printf("test_stt: tokenizer, token ceiling, spoken mentions, recognition worker order, full queue, eventfd, lazy open, idle close, long segments in windows\n");
    test_mentions();
    test_worker();
    test_windows();
    return failures;
}
