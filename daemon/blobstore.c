/* Local-filesystem blob store (ARCH-70). Each blob is a file `<base>/<key>`;
 * writes stage to `<base>/<key>.tmp` and atomically rename into place on commit,
 * so a reader never sees a partial blob and an aborted upload leaves nothing
 * behind. This is the first implementation behind the oc_blobstore seam; an
 * S3/MinIO backend can replace it without touching callers. */

#include "blobstore.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

struct oc_blobstore {
    char base[1024];
};

struct oc_blob_writer {
    oc_blobstore *bs;
    FILE *fp;
    char tmp_path[1200];
    char final_path[1200];
};

struct oc_blob_reader {
    FILE *fp;
};

/* Reject a key that could escape the base directory. Keys are daemon-minted
 * (hex of the attachment id), so this is defense-in-depth, not the primary
 * guard. */
static int key_ok(const char *key) {
    if (!key || !*key) return 0;
    for (const char *p = key; *p; p++) {
        char c = *p;
        int alnum = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                    (c >= 'A' && c <= 'F');
        if (!(alnum || c == '-' || c == '_')) return 0;
    }
    return 1;
}

oc_blobstore *oc_blobstore_open(const char *base_dir) {
    if (!base_dir || !*base_dir) return NULL;
    if (mkdir(base_dir, 0700) != 0 && errno != EEXIST) return NULL;
    oc_blobstore *bs = calloc(1, sizeof *bs);
    if (!bs) return NULL;
    size_t n = strlen(base_dir);
    if (n >= sizeof bs->base) { free(bs); return NULL; }
    memcpy(bs->base, base_dir, n + 1);
    return bs;
}

void oc_blobstore_close(oc_blobstore *bs) { free(bs); }

oc_blob_writer *oc_blob_put_begin(oc_blobstore *bs, const char *key) {
    if (!bs || !key_ok(key)) return NULL;
    oc_blob_writer *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    w->bs = bs;
    if (snprintf(w->final_path, sizeof w->final_path, "%s/%s", bs->base, key)
            >= (int)sizeof w->final_path ||
        snprintf(w->tmp_path, sizeof w->tmp_path, "%s/%s.tmp", bs->base, key)
            >= (int)sizeof w->tmp_path) {
        free(w);
        return NULL;
    }
    w->fp = fopen(w->tmp_path, "wb");
    if (!w->fp) { free(w); return NULL; }
    return w;
}

int oc_blob_put_chunk(oc_blob_writer *w, const void *data, size_t len) {
    if (!w || !w->fp) return -1;
    if (len == 0) return 0;
    if (fwrite(data, 1, len, w->fp) != len) return -1;
    return 0;
}

int oc_blob_put_commit(oc_blob_writer *w) {
    int rc = -1;
    if (!w || !w->fp) goto done;
    if (fflush(w->fp) != 0) goto done;
    if (fsync(fileno(w->fp)) != 0) goto done;
    if (fclose(w->fp) != 0) { w->fp = NULL; goto done; }
    w->fp = NULL;
    if (rename(w->tmp_path, w->final_path) != 0) goto done;
    rc = 0;
done:
    if (w) {
        if (w->fp) fclose(w->fp);
        if (rc != 0) unlink(w->tmp_path);
        free(w);
    }
    return rc;
}

void oc_blob_put_abort(oc_blob_writer *w) {
    if (!w) return;
    if (w->fp) fclose(w->fp);
    unlink(w->tmp_path);
    free(w);
}

oc_blob_reader *oc_blob_get_begin(oc_blobstore *bs, const char *key, uint64_t *size_out) {
    if (!bs || !key_ok(key)) return NULL;
    char path[1200];
    if (snprintf(path, sizeof path, "%s/%s", bs->base, key) >= (int)sizeof path)
        return NULL;
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (size_out) {
        struct stat st;
        if (fstat(fileno(fp), &st) != 0) { fclose(fp); return NULL; }
        *size_out = (uint64_t)st.st_size;
    }
    oc_blob_reader *r = calloc(1, sizeof *r);
    if (!r) { fclose(fp); return NULL; }
    r->fp = fp;
    return r;
}

long oc_blob_get_chunk(oc_blob_reader *r, void *buf, size_t cap) {
    if (!r || !r->fp) return -1;
    size_t n = fread(buf, 1, cap, r->fp);
    if (n == 0 && ferror(r->fp)) return -1;
    return (long)n;
}

void oc_blob_get_close(oc_blob_reader *r) {
    if (!r) return;
    if (r->fp) fclose(r->fp);
    free(r);
}

int oc_blob_delete(oc_blobstore *bs, const char *key) {
    if (!bs || !key_ok(key)) return -1;
    char path[1200];
    if (snprintf(path, sizeof path, "%s/%s", bs->base, key) >= (int)sizeof path)
        return -1;
    if (unlink(path) != 0 && errno != ENOENT) return -1;
    return 0;
}
