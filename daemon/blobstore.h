#ifndef OC_BLOBSTORE_H
#define OC_BLOBSTORE_H

#include <stddef.h>
#include <stdint.h>

/* Attachment blob storage (ARCH-70). Attachment *bytes* live here, never in
 * SQLite — the daemon streams them to/from this store in bounded chunks as it
 * proxies a transfer (ARCH-69). The interface is a narrow streaming seam so the
 * backend is swappable: this build ships a local-filesystem implementation; an
 * S3/MinIO (SigV4) implementation is selected with OPENCHIME_BLOB_BACKEND=s3.
 *
 * `key` is an opaque storage key the daemon assigns (never exposed on the wire).
 * All calls are thread-safe with respect to *distinct* keys (each transfer owns
 * its key); concurrent writers to the same key are not supported (and never
 * happen — a key is minted per upload). */

typedef struct oc_blobstore oc_blobstore;
typedef struct oc_blob_writer oc_blob_writer;
typedef struct oc_blob_reader oc_blob_reader;

/* Open the store. `base_dir` is the local-filesystem root (created if absent);
 * with OPENCHIME_BLOB_BACKEND=s3 the S3/MinIO backend is used instead and
 * `base_dir` is ignored (it reads OPENCHIME_S3_* from the environment). Returns
 * NULL on failure. */
oc_blobstore *oc_blobstore_open(const char *base_dir);
void oc_blobstore_close(oc_blobstore *bs);

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

#endif /* OC_BLOBSTORE_H */
