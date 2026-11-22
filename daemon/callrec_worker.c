/* The recordings' worker (callrec_worker.h). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <mbedtls/sha256.h>
#include "callrec_worker.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define TMP_PREFIX   "render-"
#define TMP_SUFFIX   ".mp4"
#define GATE_POLL_MS 50
#define WORKER_NICE  10

typedef struct job { uint64_t rec_id; char *spool; struct job *next; } job;
typedef struct result_node { oc_callrec_result r; struct result_node *next; } result_node;

struct oc_callrec_worker {
    const oc_callrec_codecs *codecs;
    oc_blobstore            *bs;
    char                    *tmp_dir;
    oc_callrec_gate_fn       gate;
    void                    *gate_ctx;
    pthread_t                thread;
    pthread_mutex_t          mu;
    pthread_cond_t           cv;
    job                     *in_head, *in_tail;
    size_t                   queued;
    uint64_t                 running;
    result_node             *out_head, *out_tail;
    volatile int             stopping;
    int                      held;
    int                      evfd;
};

void oc_callrec_blob_key(uint64_t rec_id, char *key, size_t cap) {
    snprintf(key, cap, "ca11_%016llx", (unsigned long long)rec_id);
}

/* A spool, and everything in it, gone. */
static void remove_spool(const char *dir) {
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        char path[4200];
        while ((e = readdir(d)) != NULL) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
            unlink(path);
        }
        closedir(d);
    }
    rmdir(dir);
}

/* The file into the store under `key`, its SHA-256 taken on the way: an
 * attachment is served only with its digest (REQ-141). */
static int store_file(oc_blobstore *bs, const char *path, const char *key, uint64_t size, uint8_t sha[32]) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    oc_blob_writer *bw = oc_blob_put_begin(bs, key, size);
    int ok = bw != NULL;
    mbedtls_sha256_context h;
    mbedtls_sha256_init(&h);
    if (mbedtls_sha256_starts(&h, 0) != 0) ok = 0;
    uint8_t buf[65536];
    size_t n;
    while (ok && (n = fread(buf, 1, sizeof buf, f)) > 0)
        if (oc_blob_put_chunk(bw, buf, n) != 0 || mbedtls_sha256_update(&h, buf, n) != 0) ok = 0;
    if (ferror(f)) ok = 0;
    if (ok && mbedtls_sha256_finish(&h, sha) != 0) ok = 0;
    mbedtls_sha256_free(&h);
    fclose(f);
    if (bw && !ok) oc_blob_put_abort(bw);
    else if (bw && oc_blob_put_commit(bw) != 0) ok = 0;
    return ok ? 0 : -1;
}

static void run_one(oc_callrec_worker *w, job *j, oc_callrec_result *r) {
    char path[4200], err[200] = "";
    snprintf(path, sizeof path, "%s/" TMP_PREFIX "%016llx" TMP_SUFFIX, w->tmp_dir, (unsigned long long)j->rec_id);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    r->status = oc_callrec_render(w->codecs, j->spool, path, &w->stopping, &r->bytes, &r->duration_ms, err, sizeof err);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if (r->status == OC_CALLREC_OK) {
        oc_callrec_blob_key(j->rec_id, r->key, sizeof r->key);
        if (store_file(w->bs, path, r->key, r->bytes, r->sha256) != 0) {
            r->status = OC_CALLREC_FAILED;
            r->key[0] = '\0';
            snprintf(r->reason, sizeof r->reason, "storing the recording failed");
        }
    } else {
        snprintf(r->reason, sizeof r->reason, "%.150s", err);
    }
    unlink(path);
    /* Made, or never to be: the spool goes. Cancelled: it stays for the next start. */
    if (r->status == OC_CALLREC_OK || r->status == OC_CALLREC_FAILED || r->status == OC_CALLREC_EMPTY)
        remove_spool(j->spool);
    double ms = (double)(t1.tv_sec - t0.tv_sec) * 1000.0 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
    if (r->status == OC_CALLREC_OK)
        fprintf(stderr, "callrec: recording %llu, %u ms, made in %.0f ms, %llu bytes\n",
                (unsigned long long)j->rec_id, r->duration_ms, ms, (unsigned long long)r->bytes);
    else if (r->status != OC_CALLREC_CANCELLED)
        fprintf(stderr, "callrec: recording %llu not made: %s\n", (unsigned long long)j->rec_id, r->reason);
}

static void wait_ms(oc_callrec_worker *w, unsigned ms) {
    struct timespec until;
    clock_gettime(CLOCK_MONOTONIC, &until);
    until.tv_nsec += (long)ms * 1000000L;
    while (until.tv_nsec >= 1000000000L) { until.tv_sec++; until.tv_nsec -= 1000000000L; }
    pthread_cond_timedwait(&w->cv, &w->mu, &until);
}

static void *run(void *arg) {
    oc_callrec_worker *w = arg;
    if (setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), WORKER_NICE) != 0)
        fprintf(stderr, "callrec: could not lower the worker's priority: %s\n", strerror(errno));
    for (;;) {
        pthread_mutex_lock(&w->mu);
        for (;;) {
            if (w->stopping) break;
            if (!w->in_head || w->held) { pthread_cond_wait(&w->cv, &w->mu); continue; }
            if (w->gate && w->gate(w->gate_ctx)) { wait_ms(w, GATE_POLL_MS); continue; }
            break;
        }
        if (w->stopping) { pthread_mutex_unlock(&w->mu); break; }
        job *j = w->in_head;
        w->in_head = j->next;
        if (!w->in_head) w->in_tail = NULL;
        w->queued--;
        w->running = j->rec_id;
        pthread_mutex_unlock(&w->mu);

        result_node *n = calloc(1, sizeof *n);
        if (n) { n->r.rec_id = j->rec_id; run_one(w, j, &n->r); }
        free(j->spool);
        free(j);
        pthread_mutex_lock(&w->mu);
        w->running = 0;
        if (n && (w->stopping || n->r.status == OC_CALLREC_CANCELLED)) { free(n); n = NULL; }
        if (n) {
            if (w->out_tail) w->out_tail->next = n; else w->out_head = n;
            w->out_tail = n;
        }
        pthread_mutex_unlock(&w->mu);
        if (n) { uint64_t one = 1; ssize_t rc = write(w->evfd, &one, sizeof one); (void)rc; }
    }
    return NULL;
}

static void sweep_tmp(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    char path[4200];
    size_t pl = strlen(TMP_PREFIX);
    while ((e = readdir(d)) != NULL)
        if (strncmp(e->d_name, TMP_PREFIX, pl) == 0) {
            snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
            unlink(path);
        }
    closedir(d);
}

oc_callrec_worker *oc_callrec_worker_start(const oc_callrec_codecs *codecs, oc_blobstore *bs, const char *tmp_dir,
                                           oc_callrec_gate_fn gate, void *gate_ctx) {
    if (!codecs || !bs || !tmp_dir || !*tmp_dir) return NULL;
    oc_callrec_worker *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    w->codecs = codecs; w->bs = bs; w->gate = gate; w->gate_ctx = gate_ctx;
    w->tmp_dir = strdup(tmp_dir);
    w->evfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    int ok = w->tmp_dir && w->evfd >= 0 && pthread_mutex_init(&w->mu, NULL) == 0;
    if (ok) {
        pthread_condattr_t ca;
        pthread_condattr_init(&ca);
        pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
        ok = pthread_cond_init(&w->cv, &ca) == 0;
        pthread_condattr_destroy(&ca);
        if (!ok) pthread_mutex_destroy(&w->mu);
    }
    if (ok) sweep_tmp(w->tmp_dir);
    if (ok && pthread_create(&w->thread, NULL, run, w) != 0) {
        pthread_cond_destroy(&w->cv);
        pthread_mutex_destroy(&w->mu);
        ok = 0;
    }
    if (!ok) {
        if (w->evfd >= 0) close(w->evfd);
        free(w->tmp_dir);
        free(w);
        return NULL;
    }
    return w;
}

void oc_callrec_worker_stop(oc_callrec_worker *w) {
    if (!w) return;
    pthread_mutex_lock(&w->mu);
    w->stopping = 1;
    pthread_cond_broadcast(&w->cv);
    pthread_mutex_unlock(&w->mu);
    pthread_join(w->thread, NULL);
    for (job *j = w->in_head, *next; j; j = next) { next = j->next; free(j->spool); free(j); }
    for (result_node *n = w->out_head, *next; n; n = next) { next = n->next; free(n); }
    pthread_cond_destroy(&w->cv);
    pthread_mutex_destroy(&w->mu);
    close(w->evfd);
    free(w->tmp_dir);
    free(w);
}

int oc_callrec_worker_eventfd(oc_callrec_worker *w) { return w ? w->evfd : -1; }

int oc_callrec_worker_submit(oc_callrec_worker *w, uint64_t rec_id, const char *spool_dir) {
    if (!w || !spool_dir) return -1;
    pthread_mutex_lock(&w->mu);
    int dup = w->running == rec_id;
    for (job *j = w->in_head; !dup && j; j = j->next) dup = j->rec_id == rec_id;
    if (dup || w->stopping || w->queued >= OC_CALLREC_QUEUE) {
        pthread_mutex_unlock(&w->mu);
        return dup ? 0 : -1;
    }
    job *j = calloc(1, sizeof *j);
    if (!j || !(j->spool = strdup(spool_dir))) { pthread_mutex_unlock(&w->mu); free(j); return -1; }
    j->rec_id = rec_id;
    if (w->in_tail) w->in_tail->next = j; else w->in_head = j;
    w->in_tail = j;
    w->queued++;
    pthread_cond_signal(&w->cv);
    pthread_mutex_unlock(&w->mu);
    return 0;
}

int oc_callrec_worker_next_result(oc_callrec_worker *w, oc_callrec_result *out) {
    pthread_mutex_lock(&w->mu);
    result_node *n = w->out_head;
    if (n) { w->out_head = n->next; if (!w->out_head) w->out_tail = NULL; }
    pthread_mutex_unlock(&w->mu);
    if (!n) return 0;
    *out = n->r;
    free(n);
    return 1;
}

void oc_callrec_worker_hold(oc_callrec_worker *w, int on) {
    if (!w) return;
    pthread_mutex_lock(&w->mu);
    w->held = on ? 1 : 0;
    pthread_cond_broadcast(&w->cv);
    pthread_mutex_unlock(&w->mu);
}
