/* Attachment transfer worker pool (ARCH-69). See xferpool.h for the design and
 * the one-op-per-transfer ordering invariant the caller maintains. */

#include "xferpool.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

struct oc_xferpool {
    oc_blobstore   *bs;             /* borrowed */
    pthread_t      *threads;
    int             nthreads;

    pthread_mutex_t mu;
    pthread_cond_t  cv;
    oc_xfer_job    *in_head, *in_tail;    /* submitted, awaiting a worker */
    oc_xfer_job    *out_head, *out_tail;  /* completed, awaiting the net thread */
    int             stopping;

    int             evfd;           /* counts pending results; net loop polls it */
};

/* --- jobs ----------------------------------------------------------------- */

oc_xfer_job *oc_xfer_job_new(oc_xfer_op op, uint64_t conn_id) {
    oc_xfer_job *j = calloc(1, sizeof *j);
    if (!j) return NULL;
    j->op = op;
    j->conn_id = conn_id;
    return j;
}

void oc_xfer_job_free(oc_xfer_job *j) {
    if (!j) return;
    free(j->key);
    free(j->data);
    if (j->sha) { mbedtls_sha256_free(j->sha); free(j->sha); }
    if (j->pv) {
        for (int i = 0; i < OC_PREVIEW_RUNGS; i++) free(j->pv->keys[i]);
        free(j->pv);
    }
    free(j);
}

/* --- queues (both guarded by p->mu) --------------------------------------- */

static void q_push(oc_xfer_job **head, oc_xfer_job **tail, oc_xfer_job *j) {
    j->next = NULL;
    if (*tail) (*tail)->next = j; else *head = j;
    *tail = j;
}

static oc_xfer_job *q_pop(oc_xfer_job **head, oc_xfer_job **tail) {
    oc_xfer_job *j = *head;
    if (!j) return NULL;
    *head = j->next;
    if (!*head) *tail = NULL;
    j->next = NULL;
    return j;
}

/* --- the work ------------------------------------------------------------- */

/* Store `len` bytes under `key`, whole. */
static int put_all(oc_blobstore *bs, const char *key, const uint8_t *data, size_t len) {
    oc_blob_writer *bw = oc_blob_put_begin(bs, key, len);
    if (!bw) return -1;
    size_t off = 0;
    while (off < len) {
        size_t n = len - off < 1u << 20 ? len - off : 1u << 20;
        if (oc_blob_put_chunk(bw, data + off, n) != 0) { oc_blob_put_abort(bw); return -1; }
        off += n;
    }
    return oc_blob_put_commit(bw);
}

/* An attachment's previews (ARCH-137): decode, resize and encode are CPU work
 * of the same kind as the blob I/O around them, which is why they run here and
 * never on the network loop. Whatever goes wrong, the outcome is only "no
 * preview": nothing half-stored is left behind. */
static void run_preview(oc_blobstore *bs, oc_xfer_job *j) {
    oc_xfer_preview *pv = j->pv;
    j->rc = 0;
    if (!pv) { j->rc = -1; return; }
    pv->status = OC_PREVIEW_MALFORMED;
    uint64_t size = 0;
    oc_blob_reader *br = oc_blob_get_begin(bs, j->key, &size);
    if (!br) { j->rc = -1; return; }
    if (size > pv->lim.max_bytes) {                 /* the byte cap, before any read */
        oc_blob_get_close(br);
        pv->status = OC_PREVIEW_TOO_BIG;
        return;
    }
    uint8_t *buf = malloc(size ? (size_t)size : 1);
    if (!buf) { oc_blob_get_close(br); pv->status = OC_PREVIEW_NO_MEMORY; return; }
    size_t got = 0;
    while (got < size) {
        long n = oc_blob_get_chunk(br, buf + got, (size_t)size - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    oc_blob_get_close(br);
    if (got != size) { free(buf); j->rc = -1; return; }

    oc_preview_set set;
    pv->status = oc_preview_make(buf, got, &pv->lim, &set);
    free(buf);
    pv->width = set.width;
    pv->height = set.height;
    if (pv->status != OC_PREVIEW_MADE) return;
    int ok = 1;
    for (int i = 0; i < OC_PREVIEW_RUNGS && ok; i++) {
        ok = pv->keys[i] && put_all(bs, pv->keys[i], set.rung[i].data, set.rung[i].len) == 0;
        pv->rung_w[i] = set.rung[i].width;
        pv->rung_h[i] = set.rung[i].height;
        pv->rung_size[i] = set.rung[i].len;
        pv->rung_mime[i] = set.rung[i].mime;
    }
    oc_preview_set_free(&set);
    if (!ok) {
        for (int i = 0; i < OC_PREVIEW_RUNGS; i++) if (pv->keys[i]) oc_blob_delete(bs, pv->keys[i]);
        j->rc = -1;
        return;
    }
    pv->stored = 1;
}

/* Run one job against the blob store. Called on a worker thread with no lock
 * held: the handle is reachable only through this job, and the caller's
 * one-op-per-transfer rule guarantees no other worker holds the same one. */
static void run_job(oc_blobstore *bs, oc_xfer_job *j) {
    switch (j->op) {
    case OC_XFER_OPEN_W:
        j->bw = oc_blob_put_begin(bs, j->key, j->size_hint);
        j->rc = j->bw ? 0 : -1;
        break;
    case OC_XFER_WRITE:
        j->rc = oc_blob_put_chunk(j->bw, j->data, j->len);
        /* The digest covers exactly what was stored, in order: one write per
         * transfer is out at a time, and the context travels with it. */
        if (j->rc == 0 && j->sha && j->len) mbedtls_sha256_update(j->sha, j->data, j->len);
        break;
    case OC_XFER_COMMIT:
        j->rc = oc_blob_put_commit(j->bw);
        j->bw = NULL;                    /* commit frees the writer either way */
        break;
    case OC_XFER_ABORT:
        oc_blob_put_abort(j->bw);
        j->bw = NULL;
        j->rc = 0;
        break;
    case OC_XFER_OPEN_R: {
        uint64_t size = 0;
        j->br = oc_blob_get_begin(bs, j->key, &size);
        j->blob_size = size;
        j->rc = j->br ? 0 : errno == ENOENT ? OC_XFER_GONE : -1;
        break;
    }
    case OC_XFER_READ: {
        long n = oc_blob_get_chunk(j->br, j->data, j->len);
        if (n < 0) { j->rc = -1; j->len = 0; }
        else       { j->rc = 0;  j->len = (size_t)n; }
        break;
    }
    case OC_XFER_CLOSE:
        oc_blob_get_close(j->br);
        j->br = NULL;
        j->rc = 0;
        break;
    case OC_XFER_DELETE:
        /* Reclaiming bytes can be a network round trip against S3, which is why
         * it runs here rather than on the writer that decided to reclaim. */
        j->rc = oc_blob_delete(bs, j->key);
        break;
    case OC_XFER_PREVIEW:
        run_preview(bs, j);
        break;
    }
}

static void *worker(void *arg) {
    oc_xferpool *p = arg;
    for (;;) {
        pthread_mutex_lock(&p->mu);
        while (!p->in_head && !p->stopping)
            pthread_cond_wait(&p->cv, &p->mu);
        if (p->stopping && !p->in_head) { pthread_mutex_unlock(&p->mu); break; }
        oc_xfer_job *j = q_pop(&p->in_head, &p->in_tail);
        pthread_mutex_unlock(&p->mu);
        if (!j) continue;

        run_job(p->bs, j);

        /* A fire-and-forget cleanup (abort/close for a connection that already
         * went away) has nobody to hand the result to. */
        if (j->conn_id == 0 && j->op != OC_XFER_PREVIEW) { oc_xfer_job_free(j); continue; }

        pthread_mutex_lock(&p->mu);
        q_push(&p->out_head, &p->out_tail, j);
        pthread_mutex_unlock(&p->mu);
        uint64_t one = 1;
        ssize_t w = write(p->evfd, &one, sizeof one);   /* wake the net loop */
        (void)w;
    }
    return NULL;
}

/* --- lifecycle ------------------------------------------------------------ */

oc_xferpool *oc_xferpool_start(oc_blobstore *bs, int nthreads) {
    if (!bs || nthreads < 1) return NULL;
    oc_xferpool *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->bs = bs;
    p->evfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (p->evfd < 0) { free(p); return NULL; }
    if (pthread_mutex_init(&p->mu, NULL) != 0) { close(p->evfd); free(p); return NULL; }
    if (pthread_cond_init(&p->cv, NULL) != 0) {
        pthread_mutex_destroy(&p->mu); close(p->evfd); free(p); return NULL;
    }
    p->threads = calloc((size_t)nthreads, sizeof *p->threads);
    if (!p->threads) {
        pthread_cond_destroy(&p->cv); pthread_mutex_destroy(&p->mu);
        close(p->evfd); free(p); return NULL;
    }
    for (int i = 0; i < nthreads; i++) {
        if (pthread_create(&p->threads[i], NULL, worker, p) != 0) break;
        p->nthreads++;
    }
    if (p->nthreads == 0) { oc_xferpool_stop(p); return NULL; }
    return p;
}

/* Free a drained job, releasing its handle only if this op OWNS one. */
static void release_owned(oc_xfer_job *j) {
    switch (j->op) {
    case OC_XFER_OPEN_W: case OC_XFER_OPEN_R:
    case OC_XFER_COMMIT: case OC_XFER_ABORT: case OC_XFER_CLOSE:
        if (j->bw) oc_blob_put_abort(j->bw);
        if (j->br) oc_blob_get_close(j->br);
        break;
    case OC_XFER_WRITE: case OC_XFER_READ:
    case OC_XFER_DELETE: case OC_XFER_PREVIEW:
        break;                      /* holds no handle to release */
    }
    oc_xfer_job_free(j);
}

void oc_xferpool_stop(oc_xferpool *p) {
    if (!p) return;
    pthread_mutex_lock(&p->mu);
    p->stopping = 1;
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->mu);
    for (int i = 0; i < p->nthreads; i++) pthread_join(p->threads[i], NULL);

    /* Drain whatever never ran or was never collected, releasing only the
     * handles these jobs actually OWN.
     *
     * WRITE and READ merely *borrow* the handle — the transfer still owns it and
     * may already have committed or closed it. Releasing those here is a double
     * free: a completed-but-uncollected WRITE whose transfer later committed
     * would abort a freed writer, and a WRITE queued alongside a real ABORT
     * would abort the same handle twice. Only the ops that take ownership
     * (an OPEN whose result was never collected, or a pending COMMIT / ABORT /
     * CLOSE) release anything. */
    oc_xfer_job *j;
    while ((j = q_pop(&p->in_head, &p->in_tail)))  release_owned(j);
    while ((j = q_pop(&p->out_head, &p->out_tail))) release_owned(j);
    free(p->threads);
    pthread_cond_destroy(&p->cv);
    pthread_mutex_destroy(&p->mu);
    close(p->evfd);
    free(p);
}

int oc_xferpool_eventfd(oc_xferpool *p) { return p ? p->evfd : -1; }

void oc_xferpool_submit(oc_xferpool *p, oc_xfer_job *j) {
    if (!p || !j) return;
    pthread_mutex_lock(&p->mu);
    q_push(&p->in_head, &p->in_tail, j);
    pthread_cond_signal(&p->cv);
    pthread_mutex_unlock(&p->mu);
}

/* Does not touch the eventfd. The net loop drains that counter once per wakeup
 * and then calls this until it returns NULL — the same pattern it already uses
 * for DB results. Reading the eventfd here would be wrong: a non-semaphore
 * eventfd returns the whole accumulated count and resets it, so consuming it on
 * the first result would silently swallow the wakeup for the rest. */
oc_xfer_job *oc_xferpool_next_result(oc_xferpool *p) {
    if (!p) return NULL;
    pthread_mutex_lock(&p->mu);
    oc_xfer_job *j = q_pop(&p->out_head, &p->out_tail);
    pthread_mutex_unlock(&p->mu);
    return j;
}
