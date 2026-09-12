/*
 * Attachment transfer worker pool (ARCH-69).
 *
 * Blob I/O used to run inline on the epoll network thread. With the local
 * filesystem backend that is cheap, but with an S3 backend every call is a
 * network round trip — a connect + TLS handshake to open, a socket write per
 * chunk — so one slow endpoint froze the event loop and with it *every* user on
 * the tenant. This pool moves that work off the net thread.
 *
 * The shape deliberately mirrors the existing net thread <-> DB writer hand-off
 * (ARCH-52): a submit queue, a result queue, and an **eventfd** the net loop
 * registers in epoll, so collecting completions is just another readable fd in
 * the same loop. Nothing here touches a socket; the worker only calls the
 * oc_blobstore vtable.
 *
 * ## The ordering invariant
 *
 * Blob chunks must reach the store in the order the client sent them, and two
 * operations must never touch the same blob handle at once. Both fall out of a
 * single rule the *caller* maintains:
 *
 *     at most one job per transfer is in flight at a time.
 *
 * The net thread submits the next chunk only when the previous one's result
 * arrives. That makes ordering automatic, needs no pinning of a transfer to a
 * particular worker, and means a pool of N threads is safe even though several
 * jobs run concurrently — they always belong to *different* transfers.
 *
 * It also supplies backpressure for free: while a write is in flight the net
 * thread stops reading that connection (drops EPOLLIN), so TCP flow control
 * throttles a client that streams faster than the store can absorb.
 *
 * ## Ownership across a closing connection
 *
 * A job carries a `conn_id`, never a `conn *` — the connection may be gone by
 * the time a job finishes, exactly as with DB results. The blob handle travels
 * *with the job*, so when a result comes back for a connection that no longer
 * exists, the completion path still owns a valid handle and can abort or close
 * it rather than leaking it. A caller tearing down a transfer that currently has
 * a job in flight must therefore not abort the handle itself; it marks the
 * transfer abandoned and lets the completion do the cleanup, which is what
 * preserves the one-op-per-transfer rule.
 */

#ifndef OC_XFERPOOL_H
#define OC_XFERPOOL_H

#include <stddef.h>
#include <stdint.h>

#include "blobstore.h"

typedef struct oc_xferpool oc_xferpool;

typedef enum {
    OC_XFER_OPEN_W = 1,  /* oc_blob_put_begin(key, size_hint) -> bw            */
    OC_XFER_WRITE,       /* oc_blob_put_chunk(bw, data, len)                   */
    OC_XFER_COMMIT,      /* oc_blob_put_commit(bw) — frees bw either way       */
    OC_XFER_ABORT,       /* oc_blob_put_abort(bw)  — frees bw                  */
    OC_XFER_OPEN_R,      /* oc_blob_get_begin(key) -> br + size                */
    OC_XFER_READ,        /* oc_blob_get_chunk(br, buf, cap)                    */
    OC_XFER_CLOSE,       /* oc_blob_get_close(br)  — frees br                  */
    OC_XFER_DELETE,      /* oc_blob_delete(key) — storage reclamation (ARCH-78) */
} oc_xfer_op;

typedef struct oc_xfer_job {
    oc_xfer_op op;
    uint64_t   conn_id;        /* 0 = fire-and-forget cleanup, no result wanted */
    uint64_t   attachment_id;  /* echoed back so the caller can match it        */

    char      *key;            /* OPEN_*: heap, freed with the job              */
    uint64_t   size_hint;      /* OPEN_W: declared object size                  */

    /* WRITE: bytes to store (heap, owned by the job — the caller's frame buffer
     * is reused before the worker runs). READ: the buffer the worker fills. */
    uint8_t   *data;
    size_t     len;            /* WRITE: byte count. READ: capacity in, count out. */

    /* The handle this job operates on, and which it hands back in the result. */
    oc_blob_writer *bw;
    oc_blob_reader *br;

    /* Filled by the worker. */
    int        rc;             /* 0 on success, <0 on failure                   */
    uint64_t   blob_size;      /* OPEN_R: total object size                     */

    struct oc_xfer_job *next;  /* queue linkage; not for callers                */
} oc_xfer_job;

/* Start `nthreads` workers over `bs` (borrowed; must outlive the pool). A small
 * pool, not one thread: a single worker would serialize every transfer on the
 * tenant behind the slowest one. Returns NULL on failure. */
oc_xferpool *oc_xferpool_start(oc_blobstore *bs, int nthreads);

/* Stop the workers and drain both queues. Safe with jobs still queued. */
void oc_xferpool_stop(oc_xferpool *p);

/* Readable whenever at least one result is waiting; register it in epoll. */
int oc_xferpool_eventfd(oc_xferpool *p);

/* Allocate a zeroed job, or NULL. Free with oc_xfer_job_free (which also frees
 * `key` and `data`). */
oc_xfer_job *oc_xfer_job_new(oc_xfer_op op, uint64_t conn_id);
void         oc_xfer_job_free(oc_xfer_job *j);

/* Hand a job to the pool; the pool owns it from here until it comes back out of
 * oc_xferpool_next_result (or is freed at stop). */
void oc_xferpool_submit(oc_xferpool *p, oc_xfer_job *j);

/* Next completed job, or NULL if none are ready. The caller owns it and must
 * free it with oc_xfer_job_free. */
oc_xfer_job *oc_xferpool_next_result(oc_xferpool *p);

#endif /* OC_XFERPOOL_H */
