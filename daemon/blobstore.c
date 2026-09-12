/* oc_blobstore seam (ARCH-70): a thin dispatcher over a backend vtable, plus the
 * local-filesystem backend. Each blob is a file `<base>/<key>`; writes stage to
 * `<base>/<key>.tmp` and atomically rename into place on commit, so a reader
 * never sees a partial blob and an aborted upload leaves nothing behind. The
 * S3/MinIO backend (blob_s3.c) implements the same vtable and is selected with
 * OPENCHIME_BLOB_BACKEND=s3. */

#include "blobstore.h"
#include "blob_backend.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* --- Public handles: a backend vtable + its private handle ----------------- */

struct oc_blobstore   { const oc_blob_backend *be; void *store; };
struct oc_blob_writer { const oc_blob_backend *be; void *w; };
struct oc_blob_reader { const oc_blob_backend *be; void *r; };

/* --- Local filesystem backend --------------------------------------------- */

typedef struct { char base[1024]; } fs_store;

typedef struct {
    FILE *fp;
    char tmp_path[1200];
    char final_path[1200];
} fs_writer;

typedef struct { FILE *fp; } fs_reader;

/* Reject a key that could escape the base directory. Keys are daemon-minted (hex
 * of the attachment id), so this is defense-in-depth, not the primary guard. */
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

static void *fs_open(const char *base_dir) {
    if (!base_dir || !*base_dir) return NULL;
    if (mkdir(base_dir, 0700) != 0 && errno != EEXIST) return NULL;
    fs_store *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    size_t n = strlen(base_dir);
    if (n >= sizeof s->base) { free(s); return NULL; }
    memcpy(s->base, base_dir, n + 1);
    return s;
}

static void fs_close(void *store) { free(store); }

static void *fs_put_begin(void *store, const char *key, uint64_t size_hint) {
    (void)size_hint;
    fs_store *s = store;
    if (!s || !key_ok(key)) return NULL;
    fs_writer *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    if (snprintf(w->final_path, sizeof w->final_path, "%s/%s", s->base, key)
            >= (int)sizeof w->final_path ||
        snprintf(w->tmp_path, sizeof w->tmp_path, "%s/%s.tmp", s->base, key)
            >= (int)sizeof w->tmp_path) {
        free(w);
        return NULL;
    }
    w->fp = fopen(w->tmp_path, "wb");
    if (!w->fp) { free(w); return NULL; }
    return w;
}

static int fs_put_chunk(void *wv, const void *data, size_t len) {
    fs_writer *w = wv;
    if (!w || !w->fp) return -1;
    if (len == 0) return 0;
    if (fwrite(data, 1, len, w->fp) != len) return -1;
    return 0;
}

static int fs_put_commit(void *wv) {
    fs_writer *w = wv;
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

static void fs_put_abort(void *wv) {
    fs_writer *w = wv;
    if (!w) return;
    if (w->fp) fclose(w->fp);
    unlink(w->tmp_path);
    free(w);
}

static void *fs_get_begin(void *store, const char *key, uint64_t *size_out) {
    fs_store *s = store;
    if (!s || !key_ok(key)) return NULL;
    char path[1200];
    if (snprintf(path, sizeof path, "%s/%s", s->base, key) >= (int)sizeof path)
        return NULL;
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (size_out) {
        struct stat st;
        if (fstat(fileno(fp), &st) != 0) { fclose(fp); return NULL; }
        *size_out = (uint64_t)st.st_size;
    }
    fs_reader *r = calloc(1, sizeof *r);
    if (!r) { fclose(fp); return NULL; }
    r->fp = fp;
    return r;
}

static long fs_get_chunk(void *rv, void *buf, size_t cap) {
    fs_reader *r = rv;
    if (!r || !r->fp) return -1;
    size_t n = fread(buf, 1, cap, r->fp);
    if (n == 0 && ferror(r->fp)) return -1;
    return (long)n;
}

static void fs_get_close(void *rv) {
    fs_reader *r = rv;
    if (!r) return;
    if (r->fp) fclose(r->fp);
    free(r);
}

static int fs_del(void *store, const char *key) {
    fs_store *s = store;
    if (!s || !key_ok(key)) return -1;
    char path[1200];
    if (snprintf(path, sizeof path, "%s/%s", s->base, key) >= (int)sizeof path)
        return -1;
    if (unlink(path) != 0 && errno != ENOENT) return -1;
    return 0;
}

const oc_blob_backend oc_blob_backend_fs = {
    fs_open, fs_close, fs_put_begin, fs_put_chunk, fs_put_commit, fs_put_abort,
    fs_get_begin, fs_get_chunk, fs_get_close, fs_del,
};

/* --- Public dispatch ------------------------------------------------------- */

/* Is a usable S3 configuration present? Selection is by *configuration*, not a
 * mode flag (ARCH-70): an operator who supplies S3 credentials gets the S3
 * backend, and one who doesn't gets local disk. All four values are required —
 * a partial configuration is an operator error, and silently falling back to
 * local disk would put attachments somewhere they didn't intend, so we say so
 * and let the caller fail rather than guess. */
static int s3_configured(void) {
    static const char *const REQUIRED[] = {
        "OPENCHIME_S3_ENDPOINT", "OPENCHIME_S3_BUCKET",
        "OPENCHIME_S3_ACCESS_KEY", "OPENCHIME_S3_SECRET_KEY",
    };
    int set = 0;
    for (size_t i = 0; i < sizeof REQUIRED / sizeof REQUIRED[0]; i++) {
        const char *v = getenv(REQUIRED[i]);
        if (v && *v) set++;
    }
    if (set == 0) return 0;                     /* none: local disk */
    if (set == (int)(sizeof REQUIRED / sizeof REQUIRED[0])) return 1;   /* all: S3 */
    fprintf(stderr, "openchimed: incomplete S3 configuration (%d of 4 of "
                    "OPENCHIME_S3_ENDPOINT/_BUCKET/_ACCESS_KEY/_SECRET_KEY set); "
                    "set all four for S3 or none for local disk\n", set);
    return -1;
}

oc_blobstore *oc_blobstore_open(const char *base_dir) {
    int s3 = s3_configured();
    if (s3 < 0) return NULL;                    /* misconfigured: refuse to guess */
    /* OPENCHIME_BLOB_BACKEND is the legacy explicit selector, still honored so
     * an existing deployment keeps working; the credential check above is the
     * documented path. */
    const char *sel = getenv("OPENCHIME_BLOB_BACKEND");
    if (sel && strcmp(sel, "s3") == 0) s3 = 1;
    else if (sel && strcmp(sel, "fs") == 0) s3 = 0;

    const oc_blob_backend *be = s3 ? &oc_blob_backend_s3 : &oc_blob_backend_fs;
    fprintf(stderr, "openchimed: blob storage = %s\n",
            s3 ? "s3 (operator-supplied endpoint)" : "local disk");
    void *store = be->open(base_dir);
    if (!store) return NULL;
    oc_blobstore *bs = calloc(1, sizeof *bs);
    if (!bs) { be->close(store); return NULL; }
    bs->be = be;
    bs->store = store;
    return bs;
}

void oc_blobstore_close(oc_blobstore *bs) {
    if (!bs) return;
    bs->be->close(bs->store);
    free(bs);
}

oc_blob_writer *oc_blob_put_begin(oc_blobstore *bs, const char *key, uint64_t size_hint) {
    if (!bs) return NULL;
    void *w = bs->be->put_begin(bs->store, key, size_hint);
    if (!w) return NULL;
    oc_blob_writer *pw = calloc(1, sizeof *pw);
    if (!pw) { bs->be->put_abort(w); return NULL; }
    pw->be = bs->be;
    pw->w = w;
    return pw;
}

int oc_blob_put_chunk(oc_blob_writer *w, const void *data, size_t len) {
    return (w && w->be) ? w->be->put_chunk(w->w, data, len) : -1;
}

int oc_blob_put_commit(oc_blob_writer *w) {
    if (!w) return -1;
    int rc = w->be->put_commit(w->w);
    free(w);
    return rc;
}

void oc_blob_put_abort(oc_blob_writer *w) {
    if (!w) return;
    w->be->put_abort(w->w);
    free(w);
}

oc_blob_reader *oc_blob_get_begin(oc_blobstore *bs, const char *key, uint64_t *size_out) {
    if (!bs) return NULL;
    void *r = bs->be->get_begin(bs->store, key, size_out);
    if (!r) return NULL;
    oc_blob_reader *pr = calloc(1, sizeof *pr);
    if (!pr) { bs->be->get_close(r); return NULL; }
    pr->be = bs->be;
    pr->r = r;
    return pr;
}

long oc_blob_get_chunk(oc_blob_reader *r, void *buf, size_t cap) {
    return (r && r->be) ? r->be->get_chunk(r->r, buf, cap) : -1;
}

void oc_blob_get_close(oc_blob_reader *r) {
    if (!r) return;
    r->be->get_close(r->r);
    free(r);
}

int oc_blob_delete(oc_blobstore *bs, const char *key) {
    return (bs && bs->be) ? bs->be->del(bs->store, key) : -1;
}
