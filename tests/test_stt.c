/* Tests for voice input's daemon half that need no model (ARCH-112,
 * docs/VOICE-INPUT.md §6): Moonshine's tokenizer decoding, the token ceiling,
 * spoken mentions, and the recognition worker with a stub engine -- order, a full
 * queue, the eventfd, opening only when needed and closing when idle. The real
 * model is exercised by the build job (openchimed --stt-hear). */
#include "check.h"
#include "stt_mentions.h"
#include "stt_moonshine.h"
#include "stt_worker.h"

#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ---- the tokenizer ---- */

static size_t put_token(uint8_t *out, const char *s) {
    size_t n = strlen(s), w = 0;
    if (n < 128) out[w++] = (uint8_t)n;
    else { out[w++] = (uint8_t)(n % 128 + 128); out[w++] = (uint8_t)(n / 128); }
    memcpy(out + w, s, n);
    return w + n;
}

static void test_tokens(void) {
    int failures_before = failures;
    uint8_t table[1024];
    size_t n = 0;
    n += put_token(table + n, "<unk>");                 /* 0 */
    n += put_token(table + n, "<s>");                   /* 1 */
    n += put_token(table + n, "</s>");                  /* 2 */
    n += put_token(table + n, "\xE2\x96\x81Send");      /* 3 */
    n += put_token(table + n, "\xE2\x96\x81the");       /* 4 */
    n += put_token(table + n, "\xE2\x96\x81rep");       /* 5 */
    n += put_token(table + n, "ort.");                  /* 6 */
    table[n++] = 0;                                     /* 7: unused id */
    n += put_token(table + n, "\xE2\x96\x81" "caf\xC3\xA9"); /* 8 */
    char long_tok[200];
    memset(long_tok, 'x', sizeof long_tok - 1);
    long_tok[sizeof long_tok - 1] = 0;
    n += put_token(table + n, long_tok);                /* 9: a two-byte length */

    moonshine_tokens t;
    CHECK(moonshine_tokens_load(&t, table, n) == 0);
    CHECK(t.count == 10 && t.len[7] == 0 && t.len[9] == 199);

    char out[512];
    int64_t ids[] = { 1, 3, 4, 5, 6, 2 };
    CHECK(moonshine_detok(&t, ids, 6, out, sizeof out) == (long)strlen("Send the report."));
    CHECK(strcmp(out, "Send the report.") == 0);        /* specials skipped, marker is a space, trimmed */
    int64_t odd[] = { 3, 7, 999, -1, 8 };
    moonshine_detok(&t, odd, 5, out, sizeof out);
    CHECK(strcmp(out, "Send caf\xC3\xA9") == 0);        /* unused and out-of-range ids skipped */
    /* A cap that splits a character never leaves half of it. */
    moonshine_detok(&t, odd, 5, out, 10);
    CHECK(strcmp(out, "Send caf") == 0);

    /* A truncated table is refused, not read past. */
    moonshine_tokens bad;
    CHECK(moonshine_tokens_load(&bad, table, 5) != 0);
    moonshine_tokens_free(&t);

    /* The ceiling: 6.5 tokens a second of audio, at most 256. */
    CHECK(moonshine_max_tokens(16000) == 7);
    CHECK(moonshine_max_tokens(8000) == 4);
    CHECK(moonshine_max_tokens(16000 * 60) == 256);
    if (failures == failures_before) printf("  tokenizer ok\n");
}

/* ---- spoken mentions ---- */

static void test_mentions(void) {
    int failures_before = failures;
    const char *names[] = { "dana", "Bob", "jo.smith", "Sam Lee", "twin", "Twin" };
    char out[256];
    oc_stt_mentions("Send the report to at Dana.", names, 6, out, sizeof out);
    CHECK(strcmp(out, "Send the report to @dana.") == 0);
    oc_stt_mentions("At bob, look at this.", names, 6, out, sizeof out);
    CHECK(strcmp(out, "@Bob, look at this.") == 0);                /* "at this" is nobody */
    oc_stt_mentions("ping at jo.smith now", names, 6, out, sizeof out);
    CHECK(strcmp(out, "ping @jo.smith now") == 0);
    oc_stt_mentions("ask at Sam about it", names, 6, out, sizeof out);
    CHECK(strcmp(out, "ask at Sam about it") == 0);                /* "Sam Lee" cannot be a mention */
    oc_stt_mentions("tell at twin", names, 6, out, sizeof out);
    CHECK(strcmp(out, "tell at twin") == 0);                       /* two people: stays words */
    oc_stt_mentions("meet at noon", names, 6, out, sizeof out);
    CHECK(strcmp(out, "meet at noon") == 0);
    oc_stt_mentions("that danasaur", names, 6, out, sizeof out);
    CHECK(strcmp(out, "that danasaur") == 0);
    oc_stt_mentions("chat at dana", NULL, 0, out, sizeof out);
    CHECK(strcmp(out, "chat at dana") == 0);
    if (failures == failures_before) printf("  spoken mentions ok\n");
}

/* ---- the worker, with a stub engine ---- */

static pthread_mutex_t stub_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  stub_cv = PTHREAD_COND_INITIALIZER;
static int stub_hold, stub_opens, stub_closes;

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
/* The text is the first sample as a number; a first sample of -1 fails. */
static int stub_hear(void *e, const int16_t *pcm, size_t n, char **text, char *err, size_t cap) {
    (void)e;
    pthread_mutex_lock(&stub_mu);
    while (stub_hold) pthread_cond_wait(&stub_cv, &stub_mu);
    pthread_mutex_unlock(&stub_mu);
    if (n && pcm[0] == -1) { snprintf(err, cap, "stub says no"); return -1; }
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
    if (p) p[0] = first;
    return p;
}

static int wait_result(oc_stt_worker *w, oc_stt_result *r) {
    struct pollfd pf = { oc_stt_worker_eventfd(w), POLLIN, 0 };
    for (int i = 0; i < 200; i++) {
        if (oc_stt_worker_next_result(w, r)) return 1;
        poll(&pf, 1, 20);
        uint64_t cnt;
        while (read(pf.fd, &cnt, sizeof cnt) > 0) {}
    }
    return 0;
}

static void test_worker(void) {
    int failures_before = failures;
    stub_opens = stub_closes = 0;
    oc_stt_worker *w = oc_stt_worker_start(&STUB, 2, 200);
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
    CHECK(stub_opens == 1);

    /* A failure is a result, with a reason. */
    CHECK(oc_stt_worker_submit(w, 3, seg(-1), 160) == 0);
    CHECK(wait_result(w, &r) && r.req_id == 3 && !r.ok && r.text == NULL && strstr(r.reason, "stub says no"));

    /* A full queue refuses rather than grows; the audio is freed either way. */
    pthread_mutex_lock(&stub_mu); stub_hold = 1; pthread_mutex_unlock(&stub_mu);
    CHECK(oc_stt_worker_submit(w, 4, seg(4), 160) == 0);   /* taken by the worker, held */
    struct timespec ts = { 0, 50 * 1000000L };
    nanosleep(&ts, NULL);
    CHECK(oc_stt_worker_submit(w, 5, seg(5), 160) == 0);
    CHECK(oc_stt_worker_submit(w, 6, seg(6), 160) == 0);
    CHECK(oc_stt_worker_submit(w, 7, seg(7), 160) != 0);   /* two waiting already */
    pthread_mutex_lock(&stub_mu); stub_hold = 0; pthread_cond_broadcast(&stub_cv); pthread_mutex_unlock(&stub_mu);
    for (uint64_t want = 4; want <= 6; want++) {
        CHECK(wait_result(w, &r) && r.req_id == want && r.ok);
        free(r.text);
    }

    /* Idle: the engine closes, and opens again for the next segment. */
    for (int i = 0; i < 100 && oc_stt_worker_engine_open(w); i++) {
        struct timespec t = { 0, 20 * 1000000L };
        nanosleep(&t, NULL);
    }
    CHECK(!oc_stt_worker_engine_open(w) && stub_closes == 1);
    CHECK(oc_stt_worker_submit(w, 8, seg(8), 160) == 0);
    CHECK(wait_result(w, &r) && r.req_id == 8 && r.ok);
    free(r.text);
    CHECK(stub_opens == 2);
    oc_stt_worker_stop(w);
    if (failures == failures_before) printf("  recognition worker ok\n");
}

int run_stt_tests(void) {
    printf("test_stt: tokenizer, token ceiling, spoken mentions, recognition worker order, full queue, eventfd, lazy open, idle close\n");
    test_tokens();
    test_mentions();
    test_worker();
    return failures;
}
