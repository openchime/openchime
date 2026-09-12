/* Tests for the attachment transfer worker pool (ARCH-69, daemon/xferpool.c).
 *
 * Uses the local-filesystem blob backend against a temp directory, so this is
 * hermetic — the point is the pool's threading and queue behavior, not the
 * storage backend (covered by test_blob_s3.c).
 *
 * The properties that matter, and that a single-threaded test would miss:
 *   - a job's result comes back with the right handle and status;
 *   - chunks submitted one-at-a-time per transfer land in order, byte-exact;
 *   - several transfers run concurrently across workers without interfering;
 *   - a fire-and-forget job (conn_id 0) is executed and freed, yielding no
 *     result — that is how an abandoned transfer gets cleaned up;
 *   - stopping the pool with jobs still queued releases their blob handles. */

#include "xferpool.h"
#include "check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define TMPDIR "build/oc-xferpool-test"

/* Collect exactly one result, spinning briefly since workers are async. */
static oc_xfer_job *await_result(oc_xferpool *p) {
    for (int i = 0; i < 2000; i++) {          /* ~10s worst case */
        oc_xfer_job *j = oc_xferpool_next_result(p);
        if (j) return j;
        struct timespec ts = { 0, 5 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    return NULL;
}

static void fill_pattern(uint8_t *b, size_t n, uint32_t seed) {
    uint32_t x = seed;
    for (size_t i = 0; i < n; i++) { x = x * 1664525u + 1013904223u; b[i] = (uint8_t)(x >> 24); }
}

/* Write `nchunks` chunks through the pool, honoring the one-op-per-transfer
 * rule, then commit. Returns 0 on success. */
static int upload_via_pool(oc_xferpool *p, const char *key,
                           const uint8_t *src, size_t total, size_t chunk) {
    oc_xfer_job *j = oc_xfer_job_new(OC_XFER_OPEN_W, 1);
    if (!j) return -1;
    j->key = strdup(key);
    j->size_hint = total;
    oc_xferpool_submit(p, j);
    j = await_result(p);
    if (!j || j->rc != 0 || !j->bw) { oc_xfer_job_free(j); return -1; }

    oc_blob_writer *bw = j->bw;
    oc_xfer_job_free(j);

    for (size_t off = 0; off < total; off += chunk) {
        size_t n = (total - off < chunk) ? total - off : chunk;
        oc_xfer_job *w = oc_xfer_job_new(OC_XFER_WRITE, 1);
        if (!w) return -1;
        w->bw = bw;
        w->data = malloc(n);
        if (!w->data) { oc_xfer_job_free(w); return -1; }
        memcpy(w->data, src + off, n);
        w->len = n;
        oc_xferpool_submit(p, w);
        w = await_result(p);                  /* one op in flight at a time */
        if (!w || w->rc != 0) { oc_xfer_job_free(w); return -1; }
        oc_xfer_job_free(w);
    }

    oc_xfer_job *cm = oc_xfer_job_new(OC_XFER_COMMIT, 1);
    if (!cm) return -1;
    cm->bw = bw;
    oc_xferpool_submit(p, cm);
    cm = await_result(p);
    int rc = (cm && cm->rc == 0) ? 0 : -1;
    oc_xfer_job_free(cm);
    return rc;
}

/* Read a whole blob back through the pool into `out`. Returns bytes read. */
static size_t download_via_pool(oc_xferpool *p, const char *key,
                                uint8_t *out, size_t cap, uint64_t *size_out) {
    oc_xfer_job *j = oc_xfer_job_new(OC_XFER_OPEN_R, 1);
    if (!j) return 0;
    j->key = strdup(key);
    oc_xferpool_submit(p, j);
    j = await_result(p);
    if (!j || j->rc != 0 || !j->br) { oc_xfer_job_free(j); return 0; }
    oc_blob_reader *br = j->br;
    if (size_out) *size_out = j->blob_size;
    oc_xfer_job_free(j);

    size_t total = 0;
    for (;;) {
        oc_xfer_job *rd = oc_xfer_job_new(OC_XFER_READ, 1);
        if (!rd) break;
        rd->br = br;
        rd->len = (cap - total) < 8192 ? (cap - total) : 8192;
        if (rd->len == 0) { oc_xfer_job_free(rd); break; }
        rd->data = malloc(rd->len);
        if (!rd->data) { oc_xfer_job_free(rd); break; }
        oc_xferpool_submit(p, rd);
        rd = await_result(p);
        if (!rd || rd->rc != 0 || rd->len == 0) { oc_xfer_job_free(rd); break; }
        memcpy(out + total, rd->data, rd->len);
        total += rd->len;
        oc_xfer_job_free(rd);
        if (total >= cap) break;
    }

    oc_xfer_job *cl = oc_xfer_job_new(OC_XFER_CLOSE, 1);
    if (cl) { cl->br = br; oc_xferpool_submit(p, cl); oc_xfer_job_free(await_result(p)); }
    return total;
}

int run_xferpool_tests(void) {
    printf("test_xferpool: worker round-trip, chunk ordering, concurrent transfers, "
           "fire-and-forget cleanup, stop-with-queued-jobs\n");

    mkdir("build", 0755);
    mkdir(TMPDIR, 0700);

    oc_blobstore *bs = oc_blobstore_open(TMPDIR);
    CHECK(bs != NULL);
    if (!bs) return failures;

    oc_xferpool *p = oc_xferpool_start(bs, 4);
    CHECK(p != NULL);
    if (!p) { oc_blobstore_close(bs); return failures; }
    CHECK(oc_xferpool_eventfd(p) >= 0);

    /* --- ordering: many small chunks must land in submission order --------- */
    enum { N = 64 * 1024 };
    uint8_t *src = malloc(N), *got = malloc(N);
    CHECK(src && got);
    if (src && got) {
        fill_pattern(src, N, 7);
        CHECK(upload_via_pool(p, "abc00001", src, N, 1000) == 0);
        uint64_t size = 0;
        size_t n = download_via_pool(p, "abc00001", got, N, &size);
        CHECK(size == (uint64_t)N);
        CHECK(n == (size_t)N);
        CHECK(n == (size_t)N && memcmp(src, got, N) == 0);
    }
    free(src);
    free(got);

    /* --- several transfers interleaved across the pool --------------------- */
    /* Open three writers first, so their subsequent writes are genuinely
     * concurrent across workers rather than serialized by construction. */
    oc_blob_writer *bws[3] = { NULL, NULL, NULL };
    const char *keys[3] = { "bbb00001", "bbb00002", "bbb00003" };
    for (int i = 0; i < 3; i++) {
        oc_xfer_job *j = oc_xfer_job_new(OC_XFER_OPEN_W, 1);
        CHECK(j != NULL);
        if (!j) continue;
        j->key = strdup(keys[i]);
        j->size_hint = 4;
        oc_xferpool_submit(p, j);
    }
    for (int i = 0; i < 3; i++) {
        oc_xfer_job *j = await_result(p);
        CHECK(j != NULL && j->rc == 0 && j->bw != NULL);
        if (j && j->bw) bws[i] = j->bw;        /* order across transfers is free */
        oc_xfer_job_free(j);
    }
    for (int i = 0; i < 3; i++) {
        if (!bws[i]) continue;
        oc_xfer_job *w = oc_xfer_job_new(OC_XFER_WRITE, 1);
        if (!w) continue;
        w->bw = bws[i];
        w->data = malloc(4);
        memcpy(w->data, "wxyz", 4);
        w->len = 4;
        oc_xferpool_submit(p, w);
    }
    for (int i = 0; i < 3; i++) {
        oc_xfer_job *j = await_result(p);
        CHECK(j != NULL && j->rc == 0);
        oc_xfer_job_free(j);
    }
    for (int i = 0; i < 3; i++) {
        if (!bws[i]) continue;
        oc_xfer_job *cm = oc_xfer_job_new(OC_XFER_COMMIT, 1);
        if (!cm) continue;
        cm->bw = bws[i];
        oc_xferpool_submit(p, cm);
    }
    for (int i = 0; i < 3; i++) {
        oc_xfer_job *j = await_result(p);
        CHECK(j != NULL && j->rc == 0);
        oc_xfer_job_free(j);
    }
    /* All three landed independently. */
    for (int i = 0; i < 3; i++) {
        uint8_t buf[8]; uint64_t sz = 0;
        size_t n = download_via_pool(p, keys[i], buf, sizeof buf, &sz);
        CHECK(n == 4 && sz == 4);
        CHECK(n == 4 && memcmp(buf, "wxyz", 4) == 0);
    }

    /* --- fire-and-forget: conn_id 0 runs but yields no result -------------- */
    {
        oc_xfer_job *j = oc_xfer_job_new(OC_XFER_OPEN_W, 1);
        CHECK(j != NULL);
        if (j) {
            j->key = strdup("ccc00001");
            j->size_hint = 4;
            oc_xferpool_submit(p, j);
            j = await_result(p);
            CHECK(j != NULL && j->bw != NULL);
            if (j && j->bw) {
                /* Abandon it the way a closing connection would: hand the
                 * handle to a conn_id-0 abort and expect no completion back. */
                oc_xfer_job *ab = oc_xfer_job_new(OC_XFER_ABORT, 0);
                CHECK(ab != NULL);
                if (ab) { ab->bw = j->bw; oc_xferpool_submit(p, ab); }
            }
            oc_xfer_job_free(j);
            /* Nothing should arrive; a short wait is enough to catch a wrong
             * implementation that pushes results for conn_id 0. */
            struct timespec ts = { 0, 200 * 1000 * 1000 };
            nanosleep(&ts, NULL);
            oc_xfer_job *stray = oc_xferpool_next_result(p);
            CHECK(stray == NULL);
            oc_xfer_job_free(stray);
            /* The aborted upload left no object behind. */
            oc_blob_reader *r = oc_blob_get_begin(bs, "ccc00001", NULL);
            CHECK(r == NULL);
            if (r) oc_blob_get_close(r);
        }
    }

    oc_xferpool_stop(p);

    /* --- stopping with work still queued must not leak the handles --------- */
    {
        oc_xferpool *p2 = oc_xferpool_start(bs, 2);
        CHECK(p2 != NULL);
        if (p2) {
            for (int i = 0; i < 8; i++) {
                oc_xfer_job *j = oc_xfer_job_new(OC_XFER_OPEN_W, 1);
                if (!j) continue;
                char k[32]; snprintf(k, sizeof k, "ddd0000%d", i);
                j->key = strdup(k);
                j->size_hint = 4;
                oc_xferpool_submit(p2, j);
            }
            /* Stop immediately: some jobs may still be queued, others completed
             * and uncollected. Either way their writers must be released
             * (ASan/LeakSanitizer is what actually enforces this). */
            oc_xferpool_stop(p2);
        }
    }

    oc_blobstore_close(bs);
    return failures;
}
