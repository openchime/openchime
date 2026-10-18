#ifndef OC_BLOBSTORE_H
#define OC_BLOBSTORE_H

#include <stddef.h>
#include "config.h"   /* oc_s3_config */
#include <stdint.h>

/* Attachment blob storage (ARCH-70). Attachment *bytes* live here, never in
 * SQLite — the daemon streams them to/from this store in bounded chunks as it
 * proxies a transfer (ARCH-69). The interface is a narrow streaming seam so the
 * backend is swappable: this build ships a local-filesystem implementation; an
 * S3/MinIO (SigV4) implementation is selected by OPENCHIME_S3.
 *
 * `key` is an opaque storage key the daemon assigns (never exposed on the wire).
 * All calls are thread-safe with respect to *distinct* keys (each transfer owns
 * its key); concurrent writers to the same key are not supported (and never
 * happen — a key is minted per upload). */

typedef struct oc_blobstore oc_blobstore;
typedef struct oc_blob_writer oc_blob_writer;
typedef struct oc_blob_reader oc_blob_reader;

/* Open the store: S3 when `s3` names an endpoint (config.h: all four values,
 * which the loader has checked), local disk under `base_dir` otherwise. */
oc_blobstore *oc_blobstore_open(const char *base_dir, const oc_s3_config *s3);
void oc_blobstore_close(oc_blobstore *bs);
/* Whether the store is local disk: what the storage-pressure policy measures
 * (netloop.c); an S3 store's free space is not ours to sample. */
int oc_blobstore_is_local(const oc_blobstore *bs);

/* Streaming write: begin -> chunk* -> commit | abort. The bytes are staged and
 * only become visible under `key` on commit, so an aborted or crashed upload
 * leaves no half-written blob. `size_hint` is the expected total (0 if unknown);
 * the S3 backend needs it for the object Content-Length. */
oc_blob_writer *oc_blob_put_begin(oc_blobstore *bs, const char *key, uint64_t size_hint);
int  oc_blob_put_chunk(oc_blob_writer *w, const void *data, size_t len); /* 0 ok, <0 err */
int  oc_blob_put_commit(oc_blob_writer *w);   /* fsync + publish; 0 ok, <0 err. Frees w. */
void oc_blob_put_abort(oc_blob_writer *w);    /* discard staged bytes. Frees w. */

/* Streaming read. `*size_out` (if non-NULL) receives the total blob size. */
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
