/* The export worker (export_worker.h). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "export_worker.h"

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

#define TMP_PREFIX "export-"
#define TMP_SUFFIX ".part"
/* How often a waiting worker looks at the gate again. */
#define GATE_POLL_MS 50
/* The worker thread's niceness: below the net loop, which serves people. */
#define WORKER_NICE 10

typedef struct req { uint64_t req_id, user_id; struct req *next; } req;

typedef struct job {
    uint64_t    attachment_id;
    char       *src_key;
    uint64_t    src_size;
    req        *reqs;           /* every request waiting on this export */
    size_t      n_reqs;
    struct job *next;
} job;

typedef struct result_node {
    oc_export_result    r;
    struct result_node *next;
} result_node;

struct oc_export_worker {
    const oc_export_codecs *codecs;     /* borrowed */
    oc_blobstore           *bs;         /* borrowed */
    char                   *tmp_dir;
    oc_export_gate_fn       gate;
    void                   *gate_ctx;
    pthread_t               thread;

    pthread_mutex_t         mu;
    pthread_cond_t          cv;         /* on CLOCK_MONOTONIC */
    job                    *in_head, *in_tail;
    size_t                  queued;
    job                    *running;    /* the export in progress, which requests may join */
    result_node            *out_head, *out_tail;
    volatile int            stopping;
    uint64_t                started;
    uint64_t                seq;        /* names temporary files */

    int                     evfd;
};

static uint64_t fnv64(const char *s) {
    uint64_t h = 1469598103934665603ull;
    while (*s) { h ^= (uint8_t)*s++; h *= 1099511628211ull; }
    return h;
}

void oc_export_copy_key(const oc_export_codecs *codecs, uint64_t attachment_id, char *key, size_t cap) {
    snprintf(key, cap, "%016llx_%016llx", (unsigned long long)attachment_id,
             (unsigned long long)fnv64(codecs && codecs->version ? codecs->version : ""));
}

static void free_job(job *j) {
    if (!j) return;
    for (req *r = j->reqs, *n; r; r = n) { n = r->next; free(r); }
    free(j->src_key);
    free(j);
}

/* Requests `user_id` has queued or running, under mu. */
static size_t user_load(oc_export_worker *w, uint64_t user_id) {
    size_t n = 0;
    for (job *j = w->running; j; j = NULL)
        for (req *r = j->reqs; r; r = r->next) n += r->user_id == user_id;
    for (job *j = w->in_head; j; j = j->next)
        for (req *r = j->reqs; r; r = r->next) n += r->user_id == user_id;
    return n;
}

/* ---- the source, from the blob store ----------------------------------------------- */

static long blob_read(void *ctx, void *buf, size_t cap) { return oc_blob_get_chunk(ctx, buf, cap); }

/* The finished file into the blob store, in chunks as uploads go, so the S3
 * backend streams it rather than holding it. 0, or -1. */
static int store_file(oc_blobstore *bs, const char *path, const char *key, uint64_t size) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    oc_blob_writer *bw = oc_blob_put_begin(bs, key, size);
    int ok = bw != NULL;
    uint8_t buf[65536];
    size_t n;
    while (ok && (n = fread(buf, 1, sizeof buf, f)) > 0)
        if (oc_blob_put_chunk(bw, buf, n) != 0) ok = 0;
    if (ferror(f)) ok = 0;
    fclose(f);
    if (bw && !ok) oc_blob_put_abort(bw);
    else if (bw && oc_blob_put_commit(bw) != 0) ok = 0;
    return ok ? 0 : -1;
}

static void run_one(oc_export_worker *w, job *j, oc_export_result *r, uint64_t seq) {
    char path[4096];
    snprintf(path, sizeof path, "%s/" TMP_PREFIX "%016llx-%llu" TMP_SUFFIX, w->tmp_dir,
             (unsigned long long)j->attachment_id, (unsigned long long)seq);
    uint64_t size = 0;
    oc_blob_reader *rd = oc_blob_get_begin(w->bs, j->src_key, &size);
    if (!rd) {
        r->status = OC_EXPORT_FAILED;
        snprintf(r->reason, sizeof r->reason, "the video's bytes could not be opened");
        return;
    }
    char err[200] = "";
    struct timespec t0, t1, t2;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    r->status = oc_export_render(w->codecs, blob_read, rd, size ? size : j->src_size, path, &w->stopping,
                                 &r->bytes, &r->duration_ms, err, sizeof err);
    oc_blob_get_close(rd);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if (r->status == OC_EXPORT_OK) {
        oc_export_copy_key(w->codecs, j->attachment_id, r->key, sizeof r->key);
        if (store_file(w->bs, path, r->key, r->bytes) != 0) {
            r->status = OC_EXPORT_FAILED;
            r->key[0] = '\0';
            snprintf(r->reason, sizeof r->reason, "storing the copy failed");
        }
    } else {
        snprintf(r->reason, sizeof r->reason, "%.150s", err);
    }
    unlink(path);                                          /* whatever happened: no file stays */
    clock_gettime(CLOCK_MONOTONIC, &t2);
    if (r->status == OC_EXPORT_OK) {
        double ms_x = (double)(t1.tv_sec - t0.tv_sec) * 1000.0 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
        double ms_s = (double)(t2.tv_sec - t1.tv_sec) * 1000.0 + (double)(t2.tv_nsec - t1.tv_nsec) / 1e6;
        fprintf(stderr, "export: %u ms of video into %llu bytes in %.0f ms, stored in %.0f ms\n",
                r->duration_ms, (unsigned long long)r->bytes, ms_x, ms_s);
    }
}

static void post(oc_export_worker *w, result_node *n) {
    pthread_mutex_lock(&w->mu);
    n->next = NULL;
    if (w->out_tail) w->out_tail->next = n; else w->out_head = n;
    w->out_tail = n;
    pthread_mutex_unlock(&w->mu);
    uint64_t one = 1;
    ssize_t rc = write(w->evfd, &one, sizeof one);         /* wake the net loop */
    (void)rc;
}

static void wait_ms(oc_export_worker *w, unsigned ms) {
    struct timespec until;
    clock_gettime(CLOCK_MONOTONIC, &until);
    until.tv_nsec += (long)ms * 1000000L;
    while (until.tv_nsec >= 1000000000L) { until.tv_sec++; until.tv_nsec -= 1000000000L; }
    pthread_cond_timedwait(&w->cv, &w->mu, &until);
}

static void *run(void *arg) {
    oc_export_worker *w = arg;
    /* This thread alone, below everything else the daemon does. */
    if (setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), WORKER_NICE) != 0)
        fprintf(stderr, "export: could not lower the worker's priority: %s\n", strerror(errno));
    for (;;) {
        pthread_mutex_lock(&w->mu);
        for (;;) {
            if (w->stopping) break;
            if (!w->in_head) { pthread_cond_wait(&w->cv, &w->mu); continue; }
            /* Work is waiting: not while a read-aloud rendering holds the memory. */
            if (w->gate && w->gate(w->gate_ctx)) { wait_ms(w, GATE_POLL_MS); continue; }
            break;
        }
        if (w->stopping) { pthread_mutex_unlock(&w->mu); break; }
        job *j = w->in_head;
        w->in_head = j->next;
        if (!w->in_head) w->in_tail = NULL;
        w->queued--;
        j->next = NULL;
        w->running = j;
        w->started++;
        uint64_t seq = ++w->seq;
        pthread_mutex_unlock(&w->mu);

        result_node *n = calloc(1, sizeof *n);
        if (n) {
            n->r.attachment_id = j->attachment_id;
            run_one(w, j, &n->r, seq);
        }

        pthread_mutex_lock(&w->mu);
        w->running = NULL;
        /* Everyone who joined while it ran is answered by it too. */
        if (n) {
            n->r.req_ids = malloc((j->n_reqs ? j->n_reqs : 1) * sizeof *n->r.req_ids);
            for (req *r = j->reqs; n->r.req_ids && r; r = r->next) n->r.req_ids[n->r.n_req++] = r->req_id;
        }
        pthread_mutex_unlock(&w->mu);
        free_job(j);
        if (n && w->stopping) { oc_export_result_free(&n->r); free(n); continue; }
        if (n) post(w, n);
    }
    return NULL;
}

/* Remove the temporary files a daemon that stopped mid-export left. */
static void sweep_tmp(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    char path[4096];
    size_t pl = strlen(TMP_PREFIX), sl = strlen(TMP_SUFFIX);
    while ((e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);
        if (n > pl + sl && strncmp(e->d_name, TMP_PREFIX, pl) == 0 && strcmp(e->d_name + n - sl, TMP_SUFFIX) == 0) {
            snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
            unlink(path);
        }
    }
    closedir(d);
}

oc_export_worker *oc_export_worker_start(const oc_export_codecs *codecs, oc_blobstore *bs, const char *tmp_dir,
                                         oc_export_gate_fn gate, void *gate_ctx) {
    if (!codecs || !bs || !tmp_dir || !*tmp_dir) return NULL;
    oc_export_worker *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    w->codecs = codecs;
    w->bs = bs;
    w->gate = gate;
    w->gate_ctx = gate_ctx;
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

void oc_export_worker_stop(oc_export_worker *w) {
    if (!w) return;
    pthread_mutex_lock(&w->mu);
    w->stopping = 1;
    pthread_cond_broadcast(&w->cv);
    pthread_mutex_unlock(&w->mu);
    pthread_join(w->thread, NULL);
    for (job *j = w->in_head, *next; j; j = next) { next = j->next; free_job(j); }
    for (result_node *n = w->out_head, *next; n; n = next) { next = n->next; oc_export_result_free(&n->r); free(n); }
    pthread_cond_destroy(&w->cv);
    pthread_mutex_destroy(&w->mu);
    close(w->evfd);
    free(w->tmp_dir);
    free(w);
}

int oc_export_worker_eventfd(oc_export_worker *w) { return w ? w->evfd : -1; }

int oc_export_worker_submit(oc_export_worker *w, uint64_t req_id, uint64_t user_id, uint64_t attachment_id,
                            const char *src_key, uint64_t src_size) {
    if (!w || !src_key) return -1;
    req *r = calloc(1, sizeof *r);
    if (!r) return -1;
    r->req_id = req_id;
    r->user_id = user_id;
    pthread_mutex_lock(&w->mu);
    if (w->stopping) { pthread_mutex_unlock(&w->mu); free(r); return -1; }
    if (user_load(w, user_id) >= OC_EXPORT_PER_USER) { pthread_mutex_unlock(&w->mu); free(r); return -2; }
    /* The same video already on its way: one transcode answers both. */
    job *same = w->running && w->running->attachment_id == attachment_id ? w->running : NULL;
    for (job *j = w->in_head; !same && j; j = j->next) if (j->attachment_id == attachment_id) same = j;
    if (same) {
        r->next = same->reqs;
        same->reqs = r;
        same->n_reqs++;
        pthread_mutex_unlock(&w->mu);
        return 1;
    }
    if (w->queued >= OC_EXPORT_QUEUE) { pthread_mutex_unlock(&w->mu); free(r); return -1; }
    job *j = calloc(1, sizeof *j);
    if (!j || !(j->src_key = strdup(src_key))) { pthread_mutex_unlock(&w->mu); free(j); free(r); return -1; }
    j->attachment_id = attachment_id;
    j->src_size = src_size;
    j->reqs = r;
    j->n_reqs = 1;
    if (w->in_tail) w->in_tail->next = j; else w->in_head = j;
    w->in_tail = j;
    w->queued++;
    pthread_cond_signal(&w->cv);
    pthread_mutex_unlock(&w->mu);
    return 0;
}

int oc_export_worker_next_result(oc_export_worker *w, oc_export_result *out) {
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

void oc_export_result_free(oc_export_result *r) {
    if (!r) return;
    free(r->req_ids);
    r->req_ids = NULL;
    r->n_req = 0;
}

uint64_t oc_export_worker_started(oc_export_worker *w) {
    pthread_mutex_lock(&w->mu);
    uint64_t n = w->started;
    pthread_mutex_unlock(&w->mu);
    return n;
}
