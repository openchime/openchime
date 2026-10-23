#ifndef OC_BLOBSTORE_H
#define OC_BLOBSTORE_H

#include <stddef.h>
#include "config.h"   /* oc_s3_config */
#include <stdint.h>

/* Attachment blob storage (ARCH-70). Attachment *bytes* live here, never in
 * SQLite — the daemon streams them to/from this store in bounded chunks as it
 * proxies a transfer (ARCH-69). The interface is a narrow streaming seam so the
 * backend is swappable. Without OPENCHIME_S3 the blob directory is the store;
 * with it, the S3 store (SigV4) is, and the blob directory is its disk cache:
 * writes land there and are uploaded in the background, reads are served from
 * it and fetched into it (blob_cache.c).
 *
 * `key` is an opaque storage key the daemon assigns (never exposed on the wire).
 * All calls are thread-safe with respect to *distinct* keys (each transfer owns
 * its key); concurrent writers to the same key are not supported (and never
 * happen — a key is minted per upload). */

typedef struct oc_blobstore oc_blobstore;
typedef struct oc_blob_writer oc_blob_writer;
typedef struct oc_blob_reader oc_blob_reader;

/* Open the store: S3 when `s3` names an endpoint (config.h: all four values,
 * which the loader has checked), cached under `base_dir`; local disk under
 * `base_dir` otherwise. */
oc_blobstore *oc_blobstore_open(const char *base_dir, const oc_s3_config *s3);
/* The S3 store alone, uncached: each call a request. For tools and the
 * backend's own tests; the daemon serves through oc_blobstore_open. */
oc_blobstore *oc_blobstore_open_remote(const oc_s3_config *s3);
/* Closing a cached store stops its uploader; what it had not sent stays
 * queued on disk for the next open. */
void oc_blobstore_close(oc_blobstore *bs);
/* Whether the store is local disk: what the storage-pressure policy measures
 * (netloop.c). With S3 the attachments are the store's, and the cache looks
 * after its own share of the disk. */
int oc_blobstore_is_local(const oc_blobstore *bs);

/* --- the cache in front of S3 (blob_cache.c) -------------------------------- */

/* The cache's share of the free space above the floor. */
#define OC_BLOB_CACHE_SHARE_PCT 50

/* For each of `n` keys, whether a database row still names it: ref[i] 1 or 0.
 * 0, or -1 if it could not tell, and then nothing is deleted. Called on the
 * uploader's thread. */
typedef int (*oc_blob_ref_fn)(void *ctx, const char *const *keys, size_t n, uint8_t *ref);

typedef struct {
    uint64_t floor_bytes;        /* free space the cache never takes: the pressure point */
    unsigned share_pct;          /* of the free space above it; 0: OC_BLOB_CACHE_SHARE_PCT */
    /* The sweep: objects older than `sweep_age_ms` that `referenced` says no
     * row names are deleted from the store. Off unless both are set: only a
     * managed box, whose prefix is its own, sets them. */
    oc_blob_ref_fn referenced;
    void          *ref_ctx;
    uint64_t       sweep_age_ms;
    /* Timing; 0 takes the default (2 s, 15 min, 1 min, 1 day). Tests shorten
     * them. */
    uint64_t retry_base_ms, retry_cap_ms, pass_ms, sweep_every_ms;
} oc_blob_cache_policy;

typedef struct {
    uint64_t pending_count, pending_bytes;  /* in the directory, not yet in the store */
    uint64_t queued;                        /* queue entries, deletes included */
    uint64_t oldest_queued_ms;              /* when the oldest was queued; 0 none */
    uint64_t cached_bytes, budget_bytes;    /* the directory, and what it may hold */
    uint64_t hits, misses, evictions, uploaded, swept;
    uint32_t failures;                      /* in a row; 0 when the store answers */
    uint64_t next_try_ms;                   /* backing off until; 0 when not */
    uint64_t last_error_ms;
    char     last_error[160];
} oc_blob_cache_stats;

/* Both no-ops on a store that is not cached. */
void oc_blobstore_cache_policy(oc_blobstore *bs, const oc_blob_cache_policy *p);
void oc_blobstore_cache_kick(oc_blobstore *bs);   /* a pass now */
/* 0 with `out` filled for a cached store, -1 otherwise. */
int  oc_blobstore_cache_stats(oc_blobstore *bs, oc_blob_cache_stats *out);

/* Streaming write: begin -> chunk* -> commit | abort. The bytes are staged and
 * only become visible under `key` on commit, so an aborted or crashed upload
 * leaves no half-written blob. `size_hint` is the expected total (0 if unknown);
 * the S3 backend needs it for the object Content-Length. */
oc_blob_writer *oc_blob_put_begin(oc_blobstore *bs, const char *key, uint64_t size_hint);
int  oc_blob_put_chunk(oc_blob_writer *w, const void *data, size_t len); /* 0 ok, <0 err */
int  oc_blob_put_commit(oc_blob_writer *w);   /* fsync + publish; 0 ok, <0 err. Frees w. */
void oc_blob_put_abort(oc_blob_writer *w);    /* discard staged bytes. Frees w. */

/* Streaming read. `*size_out` (if non-NULL) receives the total blob size.
 * NULL with errno ENOENT when there is no such blob; another errno when the
 * store could not be read. */
oc_blob_reader *oc_blob_get_begin(oc_blobstore *bs, const char *key, uint64_t *size_out);
/* Fill up to `cap` bytes; returns bytes read (>0), 0 at EOF, <0 on error. */
long oc_blob_get_chunk(oc_blob_reader *r, void *buf, size_t cap);
void oc_blob_get_close(oc_blob_reader *r);

/* Remove a blob. Returns 0 on success or if it was already absent, <0 on error. */
int oc_blob_delete(oc_blobstore *bs, const char *key);

/* ARCH-69, checked rather than inferred: blob I/O never runs on the event loop.
 * A loop marks its own thread (on at start, off at exit); every operation above
 * that runs on a marked thread is counted, process-wide. The loop only opens
 * and closes the store, so while it serves the count should not move -- a test
 * reads it before and after a slow transfer and requires no change. */
void     oc_blobstore_mark_loop_thread(int on);
uint64_t oc_blobstore_loop_ops(void);

#endif /* OC_BLOBSTORE_H */
