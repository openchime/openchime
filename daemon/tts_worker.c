/* The read-aloud render worker (tts_worker.h). */
#define _POSIX_C_SOURCE 200809L
#include "tts_worker.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

typedef struct job {
    uint64_t    req_id;
    uint8_t     handle[32];
    int         voice;
    char       *text;
    struct job *next;
} job;

typedef struct result_node {
    oc_tts_result       r;
    struct result_node *next;
} result_node;

struct oc_tts_worker {
    const oc_tts_engine *engine;         /* borrowed */
    oc_blobstore        *bs;             /* borrowed */
    size_t               queue_cap;
    unsigned             idle_ms;
    pthread_t            thread;

    pthread_mutex_t      mu;
    pthread_cond_t       cv;             /* on CLOCK_MONOTONIC, for the idle timeout */
    job                 *in_head, *in_tail;
    size_t               queued;
    result_node         *out_head, *out_tail;
    int                  stopping;
    int                  engine_is_open; /* read under mu */

    int                  evfd;
};

static uint64_t fnv64(const char *s) {
    uint64_t h = 1469598103934665603ull;
    while (*s) { h ^= (uint8_t)*s++; h *= 1099511628211ull; }
    return h;
}

void oc_tts_render_key(const oc_tts_engine *engine, const uint8_t handle[32], char *key, size_t cap) {
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", handle[i]);
    snprintf(key, cap, "%s-%016llx", hex, (unsigned long long)fnv64(engine->version));
}

static void store(oc_tts_worker *w, const job *j, const uint8_t *mp4, size_t len, uint32_t ms, oc_tts_result *r) {
    oc_tts_render_key(w->engine, j->handle, r->key, sizeof r->key);
    oc_blob_writer *bw = oc_blob_put_begin(w->bs, r->key, len);
    int ok = bw != NULL;
    /* In chunks, as uploads are, so the S3 backend streams rather than holds. */
    for (size_t at = 0; ok && at < len; at += 65536) {
        size_t n = len - at < 65536 ? len - at : 65536;
        if (oc_blob_put_chunk(bw, mp4 + at, n) != 0) ok = 0;
    }
    if (bw && !ok) oc_blob_put_abort(bw);
    else if (bw && oc_blob_put_commit(bw) != 0) ok = 0;
    if (!ok) {
        r->status = OC_TTS_FAILED;
        r->key[0] = '\0';
        snprintf(r->reason, sizeof r->reason, "storing the render failed");
        return;
    }
    r->bytes = len;
    r->duration_ms = ms;
}

static void post(oc_tts_worker *w, result_node *n) {
    pthread_mutex_lock(&w->mu);
    n->next = NULL;
    if (w->out_tail) w->out_tail->next = n; else w->out_head = n;
    w->out_tail = n;
    pthread_mutex_unlock(&w->mu);
    uint64_t one = 1;
    ssize_t rc = write(w->evfd, &one, sizeof one);          /* wake the net loop */
    (void)rc;
}

static void *run(void *arg) {
    oc_tts_worker *w = arg;
    void *engine = NULL;
    for (;;) {
        pthread_mutex_lock(&w->mu);
        while (!w->in_head && !w->stopping) {
            if (!engine) { pthread_cond_wait(&w->cv, &w->mu); continue; }
            /* Open and idle: wait for work, but not beyond the idle timeout. */
            struct timespec until;
            clock_gettime(CLOCK_MONOTONIC, &until);
            until.tv_sec += w->idle_ms / 1000;
            until.tv_nsec += (long)(w->idle_ms % 1000) * 1000000L;
            if (until.tv_nsec >= 1000000000L) { until.tv_sec++; until.tv_nsec -= 1000000000L; }
            int rc = 0;
            while (!w->in_head && !w->stopping && rc != ETIMEDOUT)
                rc = pthread_cond_timedwait(&w->cv, &w->mu, &until);
            if (!w->in_head && !w->stopping) {
                /* Nothing came: give the model's memory back. Closing is done
                 * without the lock, so a submit is never kept waiting on it. */
                w->engine_is_open = 0;
                pthread_mutex_unlock(&w->mu);
                w->engine->close(engine);
                engine = NULL;
                pthread_mutex_lock(&w->mu);
            }
        }
        if (w->stopping) { pthread_mutex_unlock(&w->mu); break; }
        job *j = w->in_head;
        w->in_head = j->next;
        if (!w->in_head) w->in_tail = NULL;
        w->queued--;
        pthread_mutex_unlock(&w->mu);

        result_node *n = calloc(1, sizeof *n);
        if (!n) { free(j->text); free(j); continue; }         /* no memory to report with */
        n->r.req_id = j->req_id;
        memcpy(n->r.handle, j->handle, 32);

        char err[256] = "";
        if (!engine) {
            engine = w->engine->open(w->engine->ctx, err, sizeof err);
            if (engine) {
                pthread_mutex_lock(&w->mu);
                w->engine_is_open = 1;
                pthread_mutex_unlock(&w->mu);
            }
        }
        if (!engine) {
            n->r.status = OC_TTS_FAILED;
            snprintf(n->r.reason, sizeof n->r.reason, "opening the engine: %.120s", err);
        } else {
            uint8_t *mp4 = NULL;
            size_t len = 0;
            uint32_t ms = 0;
            n->r.status = oc_tts_render(w->engine, engine, j->text, j->voice, &mp4, &len, &ms, err, sizeof err);
            if (n->r.status == OC_TTS_OK) store(w, j, mp4, len, ms, &n->r);
            else if (n->r.status == OC_TTS_FAILED) snprintf(n->r.reason, sizeof n->r.reason, "%.150s", err);
            free(mp4);
        }
        free(j->text);
        free(j);
        post(w, n);
    }
    if (engine) w->engine->close(engine);
    return NULL;
}

oc_tts_worker *oc_tts_worker_start(const oc_tts_engine *engine, oc_blobstore *bs, size_t queue_cap,
                                   unsigned idle_ms) {
    if (!engine || !bs) return NULL;
    oc_tts_worker *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    w->engine = engine;
    w->bs = bs;
    w->queue_cap = queue_cap ? queue_cap : 1;
    w->idle_ms = idle_ms;
    w->evfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    pthread_condattr_t ca;
    int ok = w->evfd >= 0 && pthread_mutex_init(&w->mu, NULL) == 0;
    if (ok) {
        pthread_condattr_init(&ca);
        pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
        ok = pthread_cond_init(&w->cv, &ca) == 0;
        pthread_condattr_destroy(&ca);
        if (!ok) pthread_mutex_destroy(&w->mu);
    }
    if (ok && pthread_create(&w->thread, NULL, run, w) != 0) {
        pthread_cond_destroy(&w->cv);
        pthread_mutex_destroy(&w->mu);
        ok = 0;
    }
    if (!ok) {
        if (w->evfd >= 0) close(w->evfd);
        free(w);
        return NULL;
    }
    return w;
}

void oc_tts_worker_stop(oc_tts_worker *w) {
    if (!w) return;
    pthread_mutex_lock(&w->mu);
    w->stopping = 1;
    pthread_cond_broadcast(&w->cv);
    pthread_mutex_unlock(&w->mu);
    pthread_join(w->thread, NULL);
    for (job *j = w->in_head, *next; j; j = next) { next = j->next; free(j->text); free(j); }
    for (result_node *n = w->out_head, *next; n; n = next) { next = n->next; free(n); }
    pthread_cond_destroy(&w->cv);
    pthread_mutex_destroy(&w->mu);
    close(w->evfd);
    free(w);
}

int oc_tts_worker_eventfd(oc_tts_worker *w) { return w ? w->evfd : -1; }

int oc_tts_worker_submit(oc_tts_worker *w, uint64_t req_id, const uint8_t handle[32], int voice,
                         const char *text) {
    if (!w || !text || voice < 0 || voice >= w->engine->voices) return -1;
    job *j = calloc(1, sizeof *j);
    if (!j || !(j->text = strdup(text))) { free(j); return -1; }
    j->req_id = req_id;
    memcpy(j->handle, handle, 32);
    j->voice = voice;
    pthread_mutex_lock(&w->mu);
    if (w->queued >= w->queue_cap || w->stopping) {
        pthread_mutex_unlock(&w->mu);
        free(j->text);
        free(j);
        return -1;
    }
    if (w->in_tail) w->in_tail->next = j; else w->in_head = j;
    w->in_tail = j;
    w->queued++;
    pthread_cond_signal(&w->cv);
    pthread_mutex_unlock(&w->mu);
    return 0;
}

int oc_tts_worker_next_result(oc_tts_worker *w, oc_tts_result *out) {
    pthread_mutex_lock(&w->mu);
    result_node *n = w->out_head;
    if (n) {
        w->out_head = n->next;
        if (!w->out_head) w->out_tail = NULL;
    }
    pthread_mutex_unlock(&w->mu);
    if (!n) return 0;
    *out = n->r;
    free(n);
    return 1;
}

int oc_tts_worker_engine_open(oc_tts_worker *w) {
    pthread_mutex_lock(&w->mu);
    int open = w->engine_is_open;
    pthread_mutex_unlock(&w->mu);
    return open;
}
