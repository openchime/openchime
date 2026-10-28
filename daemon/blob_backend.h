#ifndef OC_BLOB_BACKEND_H
#define OC_BLOB_BACKEND_H

#include <stddef.h>
#include <stdint.h>

/* Private backend interface behind the oc_blobstore seam (ARCH-70). Each backend
 * (local filesystem, S3/MinIO) provides these; oc_blobstore_open picks one by configuration
 * and every public oc_blob_* call delegates through the selected vtable. `store`,
 * `w`, `r` are backend-private handles. */
typedef struct {
    void *(*open)(const char *base_dir, const void *backend_cfg);   /* fs: base_dir; s3: the oc_s3_config. NULL on fail */
    void  (*close)(void *store);
    void *(*put_begin)(void *store, const char *key, uint64_t size_hint);
    int   (*put_chunk)(void *w, const void *data, size_t len);
    int   (*put_commit)(void *w);          /* publishes; frees w. 0 ok, <0 err */
    void  (*put_abort)(void *w);           /* discards; frees w */
    void *(*get_begin)(void *store, const char *key, uint64_t *size_out);
    long  (*get_chunk)(void *r, void *buf, size_t cap);
    void  (*get_close)(void *r);
    int   (*del)(void *store, const char *key);
} oc_blob_backend;

/* A key a backend accepts: letters and digits of hex, '-' and '_'. Anything
 * with a '.' is the backends' own staging and never a blob. */
int oc_blob_key_ok(const char *key);

extern const oc_blob_backend oc_blob_backend_fs;
extern const oc_blob_backend oc_blob_backend_s3;
extern const oc_blob_backend oc_blob_backend_cache;

/* One page of an S3 store's keys, each named without the store's prefix, with
 * its size and when it was last written; `fn` returns non-zero to stop early.
 * `token` continues a listing ("" or NULL: from the start), and `next` gets the
 * token for the page after this one, "" after the last. A store with no prefix
 * is never listed. 0, or -1 if the store could not be asked. Blocking. */
typedef int (*oc_blob_list_fn)(void *ctx, const char *key, uint64_t size, uint64_t modified_ms);
int oc_blob_s3_list(void *store, const char *token, oc_blob_list_fn fn, void *ctx,
                    char *next, size_t next_cap);

/* The cache's own, behind blobstore.h's (blob_cache.c). */
#include "blobstore.h"
void oc_blob_cache_set_policy(void *store, const oc_blob_cache_policy *p);
void oc_blob_cache_get_stats(void *store, oc_blob_cache_stats *out);
void oc_blob_cache_kick(void *store);

#endif /* OC_BLOB_BACKEND_H */
