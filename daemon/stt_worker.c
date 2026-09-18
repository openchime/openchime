/* The voice-input recognition worker (stt_worker.h). */
#define _POSIX_C_SOURCE 200809L
#include "stt_worker.h"

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
    int16_t    *pcm;
    size_t      samples;
    struct job *next;
} job;

typedef struct result_node {
    oc_stt_result       r;
    struct result_node *next;
} result_node;

struct oc_stt_worker {
    const oc_stt_engine *engine;         /* borrowed */
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

static double ms_between(const struct timespec *a, const struct timespec *b) {
    return (double)(b->tv_sec - a->tv_sec) * 1000.0 + (double)(b->tv_nsec - a->tv_nsec) / 1e6;
}

static void post(oc_stt_worker *w, result_node *n) {
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
    oc_stt_worker *w = arg;
    void *engine = NULL;
    for (;;) {
        pthread_mutex_lock(&w->mu);
        while (!w->in_head && !w->stopping) {
            if (!engine) { pthread_cond_wait(&w->cv, &w->mu); continue; }
            struct timespec until;
            clock_gettime(CLOCK_MONOTONIC, &until);
            until.tv_sec += w->idle_ms / 1000;
            until.tv_nsec += (long)(w->idle_ms % 1000) * 1000000L;
            if (until.tv_nsec >= 1000000000L) { until.tv_sec++; until.tv_nsec -= 1000000000L; }
            int rc = 0;
            while (!w->in_head && !w->stopping && rc != ETIMEDOUT)
                rc = pthread_cond_timedwait(&w->cv, &w->mu, &until);
            if (!w->in_head && !w->stopping) {
                /* Nothing came: give the model's memory back, without the lock. */
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
        if (!n) { free(j->pcm); free(j); continue; }
        n->r.req_id = j->req_id;

        char err[256] = "";
        if (!engine) {
            struct timespec o0, o1;
            clock_gettime(CLOCK_MONOTONIC, &o0);
            engine = w->engine->open(w->engine->ctx, err, sizeof err);
            clock_gettime(CLOCK_MONOTONIC, &o1);
            if (engine) {
                fprintf(stderr, "stt: model loaded in %.0f ms\n", ms_between(&o0, &o1));
                pthread_mutex_lock(&w->mu);
                w->engine_is_open = 1;
                pthread_mutex_unlock(&w->mu);
            }
        }
        if (!engine) {
            snprintf(n->r.reason, sizeof n->r.reason, "opening the engine: %.120s", err);
        } else {
            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            int rc = w->engine->hear(engine, j->pcm, j->samples, &n->r.text, err, sizeof err);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            if (rc == 0 && n->r.text) {
                n->r.ok = 1;
                double audio_ms = (double)j->samples * 1000.0 / 16000.0;
                double took = ms_between(&t0, &t1);
                fprintf(stderr, "stt: heard %.0f ms of audio in %.0f ms (%.2fx)\n", audio_ms, took,
                        audio_ms > 0 ? took / audio_ms : 0.0);
            } else {
                free(n->r.text);
                n->r.text = NULL;
                snprintf(n->r.reason, sizeof n->r.reason, "%.150s", err[0] ? err : "recognition failed");
            }
        }
        /* The audio is done with: freed here, never kept. */
        free(j->pcm);
        free(j);
        post(w, n);
    }
    if (engine) w->engine->close(engine);
    return NULL;
}

oc_stt_worker *oc_stt_worker_start(const oc_stt_engine *engine, size_t queue_cap, unsigned idle_ms) {
    if (!engine) return NULL;
    oc_stt_worker *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    w->engine = engine;
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

void oc_stt_worker_stop(oc_stt_worker *w) {
    if (!w) return;
    pthread_mutex_lock(&w->mu);
    w->stopping = 1;
    pthread_cond_broadcast(&w->cv);
    pthread_mutex_unlock(&w->mu);
    pthread_join(w->thread, NULL);
    for (job *j = w->in_head, *next; j; j = next) { next = j->next; free(j->pcm); free(j); }
    for (result_node *n = w->out_head, *next; n; n = next) { next = n->next; free(n->r.text); free(n); }
    pthread_cond_destroy(&w->cv);
    pthread_mutex_destroy(&w->mu);
    close(w->evfd);
    free(w);
}

int oc_stt_worker_eventfd(oc_stt_worker *w) { return w ? w->evfd : -1; }

int oc_stt_worker_submit(oc_stt_worker *w, uint64_t req_id, int16_t *pcm, size_t samples) {
    if (!w) { free(pcm); return -1; }
    job *j = calloc(1, sizeof *j);
    if (!j) { free(pcm); return -1; }
    j->req_id = req_id;
    j->pcm = pcm;
    j->samples = samples;
    pthread_mutex_lock(&w->mu);
    if (w->queued >= w->queue_cap || w->stopping) {
        pthread_mutex_unlock(&w->mu);
        free(j->pcm);
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

int oc_stt_worker_next_result(oc_stt_worker *w, oc_stt_result *out) {
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

int oc_stt_worker_engine_open(oc_stt_worker *w) {
    pthread_mutex_lock(&w->mu);
    int open = w->engine_is_open;
    pthread_mutex_unlock(&w->mu);
    return open;
}
