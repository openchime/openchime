#ifndef OC_BLOB_BACKEND_H
#define OC_BLOB_BACKEND_H

#include <stddef.h>
#include <stdint.h>

/* Private backend interface behind the oc_blobstore seam (ARCH-70). Each backend
 * (local filesystem, S3/MinIO) provides these; oc_blobstore_open picks one by env
 * and every public oc_blob_* call delegates through the selected vtable. `store`,
 * `w`, `r` are backend-private handles. */
typedef struct {
    void *(*open)(const char *cfg);        /* cfg = base dir (fs); env (s3). NULL on fail */
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

extern const oc_blob_backend oc_blob_backend_fs;
extern const oc_blob_backend oc_blob_backend_s3;

#endif /* OC_BLOB_BACKEND_H */
