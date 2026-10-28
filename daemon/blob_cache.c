/* The blob directory as a disk cache in front of an S3 store (ARCH-70).
 *
 * With OPENCHIME_S3 the store is the real copy and the blob directory holds
 * what the box has used lately, and what it has not yet sent:
 *
 *   - a write lands in the blob directory, as on the local backend, and is
 *     accepted there; a queue entry `<dir>/.queue/<key>.put` is made before the
 *     blob is published, and the uploader thread below copies it to the store
 *     and removes the entry. A delete removes the local copy and queues
 *     `<key>.del` the same way, so a store that is down when an attachment is
 *     reclaimed still loses it once it is back.
 *   - a read is served from the blob directory when the blob is there, and
 *     otherwise fetched from the store into it and served from there.
 *   - the directory keeps to a budget: `share_pct` of the free space above the
 *     floor (the storage-pressure point), re-measured on every pass, so it
 *     grows into space nothing else wants and gives it back as soon as
 *     something does. Least recently read goes first. A blob still queued for
 *     the store is never evicted: it is the only copy.
 *
 * Nobody has to tend it. A failed upload is retried after base x failures^2
 * (capped, with jitter), for as long as it takes; one failure pauses the whole
 * queue, the next attempt being the probe, and the entry that failed goes to
 * the back so one bad blob cannot hold up the rest. The queue is files, so it
 * survives a restart, and a blob directory first given a store (or a different
 * one) has every blob it holds queued, which is how attachments kept on disk
 * before S3 was turned on reach it.
 *
 * When the disk is too short to stage a write, it goes straight to the store,
 * as the S3 backend alone would; a read the disk has no room for streams from
 * the store uncached.
 *
 * On a managed box the uploader also sweeps the store: an object older than the
 * restore window that no database row names is deleted (blobstore.h). Keys are
 * never reused, so an object a restored database no longer names will never be
 * named again. */

#include "blobstore.h"
#include "blob_backend.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define DEF_RETRY_BASE_MS  2000ull
#define DEF_RETRY_CAP_MS   (15ull * 60 * 1000)
#define DEF_PASS_MS        (60ull * 1000)
#define DEF_SWEEP_EVERY_MS (24ull * 3600 * 1000)
#define SWEEP_BATCH        100

typedef struct cache {
    void *fs, *s3;               /* the two backends' stores */
    char  base[1024];
    char  qdir[1100];
    char  ident[900];            /* which store: endpoint/bucket/prefix */

    pthread_t       th;
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    int             started, stop, kick;

    oc_blob_cache_policy pol;    /* under mu */
    oc_blob_cache_stats  st;     /* under mu */
    uint64_t             last_sweep_ms;
    unsigned             seq;    /* fetch staging names */
} cache;

typedef struct { const oc_blob_backend *be; void *h; cache *c; char key[256]; uint64_t bytes; } cache_writer;
typedef struct { const oc_blob_backend *be; void *h; } cache_reader;

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static uint64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static uint64_t pol_or(uint64_t v, uint64_t def) { return v ? v : def; }

/* Free bytes on the blob directory's filesystem; UINT64_MAX if unmeasurable,
 * so a measuring error never turns writes away. */
static uint64_t avail_bytes(const cache *c) {
    struct statvfs sv;
    if (statvfs(c->base, &sv) != 0) return UINT64_MAX;
    return (uint64_t)sv.f_bavail * (uint64_t)sv.f_frsize;
}

static int path_of(char *out, size_t cap, const char *dir, const char *key, const char *suffix) {
    return snprintf(out, cap, "%s/%s%s", dir, key, suffix) < (int)cap ? 0 : -1;
}

/* Queue `key` for `op` ("put" or "del"), stamped with when. Written beside and
 * renamed into place, so a later put of the same key is a new entry (inode)
 * that the uploader's removal of the earlier one cannot take with it. */
static int queue_add(cache *c, const char *key, const char *op) {
    char path[1400], tmp[1500];
    if (snprintf(path, sizeof path, "%s/%s.%s", c->qdir, key, op) >= (int)sizeof path) return -1;
    if (snprintf(tmp, sizeof tmp, "%s.new", path) >= (int)sizeof tmp) return -1;
    FILE *fp = fopen(tmp, "w");
    if (!fp) return -1;
    fprintf(fp, "%llu\n", (unsigned long long)now_ms());
    int ok = fflush(fp) == 0 && fsync(fileno(fp)) == 0;
    ok = (fclose(fp) == 0) && ok;
    if (!ok || rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}

static int queued(const cache *c, const char *key, const char *op) {
    char path[1400];
    if (snprintf(path, sizeof path, "%s/%s.%s", c->qdir, key, op) >= (int)sizeof path) return 0;
    return access(path, F_OK) == 0;
}

static void queue_drop(cache *c, const char *key, const char *op) {
    char path[1400];
    if (snprintf(path, sizeof path, "%s/%s.%s", c->qdir, key, op) < (int)sizeof path) unlink(path);
}

static void wake(cache *c) {
    pthread_mutex_lock(&c->mu);
    c->kick = 1;
    pthread_cond_signal(&c->cv);
    pthread_mutex_unlock(&c->mu);
}

static int stopping(cache *c) {
    pthread_mutex_lock(&c->mu);
    int s = c->stop;
    pthread_mutex_unlock(&c->mu);
    return s;
}

/* --- the backend: writes ---------------------------------------------------- */

static void *cache_put_begin(void *store, const char *key, uint64_t size_hint) {
    cache *c = store;
    if (!c || !oc_blob_key_ok(key) || strlen(key) >= sizeof ((cache_writer *)0)->key) return NULL;
    cache_writer *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    w->c = c;
    snprintf(w->key, sizeof w->key, "%s", key);
    pthread_mutex_lock(&c->mu);
    uint64_t floor = c->pol.floor_bytes;
    pthread_mutex_unlock(&c->mu);
    /* Staging it here needs the room to; without, it goes to the store as the
     * S3 backend alone would send it, and the client waits for that. */
    uint64_t avail = avail_bytes(c);
    if (avail != UINT64_MAX && avail < floor + size_hint) {
        w->be = &oc_blob_backend_s3;
        w->h = w->be->put_begin(c->s3, key, size_hint);
    } else {
        w->be = &oc_blob_backend_fs;
        w->h = w->be->put_begin(c->fs, key, size_hint);
    }
    if (!w->h) { free(w); return NULL; }
    return w;
}

static int cache_put_chunk(void *wv, const void *data, size_t len) {
    cache_writer *w = wv;
    if (!w) return -1;
    int rc = w->be->put_chunk(w->h, data, len);
    if (rc == 0) w->bytes += len;
    return rc;
}

/* Counted the moment it is queued, so the figures never read nothing waiting
 * while something is; each pass of the uploader counts them afresh. */
static void count_queued(cache *c, uint64_t bytes, int pending) {
    pthread_mutex_lock(&c->mu);
    c->st.queued++;
    if (pending) { c->st.pending_count++; c->st.pending_bytes += bytes; }
    if (!c->st.oldest_queued_ms) c->st.oldest_queued_ms = now_ms();
    pthread_mutex_unlock(&c->mu);
}

static int cache_put_commit(void *wv) {
    cache_writer *w = wv;
    if (!w) return -1;
    int rc;
    if (w->be == &oc_blob_backend_fs) {
        /* Queued before it is published: a crash between the two leaves an
         * entry with no blob, which the uploader drops, never a blob the store
         * will not be sent. */
        if (queue_add(w->c, w->key, "put") != 0) { w->be->put_abort(w->h); free(w); return -1; }
        rc = w->be->put_commit(w->h);
        if (rc != 0) queue_drop(w->c, w->key, "put");
        else {
            queue_drop(w->c, w->key, "del");
            count_queued(w->c, w->bytes, 1);
            wake(w->c);
        }
    } else {
        rc = w->be->put_commit(w->h);
    }
    free(w);
    return rc;
}

static void cache_put_abort(void *wv) {
    cache_writer *w = wv;
    if (!w) return;
    w->be->put_abort(w->h);
    free(w);
}

/* --- the backend: reads ----------------------------------------------------- */

static void touch_read(const char *path) {
    struct timespec t[2] = { { 0, UTIME_NOW }, { 0, UTIME_OMIT } };
    utimensat(AT_FDCWD, path, t, 0);
}

static void count(cache *c, uint64_t *field) {
    pthread_mutex_lock(&c->mu);
    (*field)++;
    pthread_mutex_unlock(&c->mu);
}

static void *wrap_reader(const oc_blob_backend *be, void *h) {
    cache_reader *r = calloc(1, sizeof *r);
    if (!r) { be->get_close(h); return NULL; }
    r->be = be; r->h = h;
    return r;
}

/* Copy the store's `key` into the blob directory. 0 once it is there; -1 with
 * errno ENOENT when the store has no such object, another errno otherwise. */
static int fetch(cache *c, const char *key, void **direct, uint64_t *size_out) {
    uint64_t size = 0;
    void *src = oc_blob_backend_s3.get_begin(c->s3, key, &size);
    if (!src) return -1;
    pthread_mutex_lock(&c->mu);
    uint64_t floor = c->pol.floor_bytes;
    unsigned seq = ++c->seq;
    pthread_mutex_unlock(&c->mu);
    uint64_t avail = avail_bytes(c);
    if (avail != UINT64_MAX && avail < floor + size) {
        /* No room to keep it: serve it straight from the store. */
        *direct = src;
        if (size_out) *size_out = size;
        return 0;
    }
    char tmp[1400], fin[1400];
    if (snprintf(tmp, sizeof tmp, "%s/%s.fetch-%ld-%u", c->base, key, (long)getpid(), seq) >= (int)sizeof tmp ||
        path_of(fin, sizeof fin, c->base, key, "") != 0) {
        oc_blob_backend_s3.get_close(src); errno = EIO; return -1;
    }
    FILE *fp = fopen(tmp, "wb");
    if (!fp) { oc_blob_backend_s3.get_close(src); errno = EIO; return -1; }
    char buf[64 * 1024];
    uint64_t got = 0;
    long n;
    int ok = 1;
    while ((n = oc_blob_backend_s3.get_chunk(src, buf, sizeof buf)) > 0) {
        if (fwrite(buf, 1, (size_t)n, fp) != (size_t)n) { ok = 0; break; }
        got += (uint64_t)n;
    }
    oc_blob_backend_s3.get_close(src);
    ok = ok && n == 0 && got == size;
    ok = (fclose(fp) == 0) && ok;
    if (!ok || rename(tmp, fin) != 0) { unlink(tmp); errno = EIO; return -1; }
    return 0;
}

static void *cache_get_begin(void *store, const char *key, uint64_t *size_out) {
    cache *c = store;
    if (!c || !oc_blob_key_ok(key)) { errno = EINVAL; return NULL; }
    char path[1400];
    if (path_of(path, sizeof path, c->base, key, "") != 0) { errno = EINVAL; return NULL; }
    void *h = oc_blob_backend_fs.get_begin(c->fs, key, size_out);
    if (h) {
        touch_read(path);
        count(c, &c->st.hits);
        return wrap_reader(&oc_blob_backend_fs, h);
    }
    /* Queued for the store means it is only here; not here means gone. */
    if (queued(c, key, "put")) { errno = ENOENT; return NULL; }
    count(c, &c->st.misses);
    void *direct = NULL;
    if (fetch(c, key, &direct, size_out) != 0) return NULL;
    if (direct) return wrap_reader(&oc_blob_backend_s3, direct);
    h = oc_blob_backend_fs.get_begin(c->fs, key, size_out);
    if (!h) { errno = EIO; return NULL; }
    touch_read(path);
    return wrap_reader(&oc_blob_backend_fs, h);
}

static long cache_get_chunk(void *rv, void *buf, size_t cap) {
    cache_reader *r = rv;
    return r ? r->be->get_chunk(r->h, buf, cap) : -1;
}

static void cache_get_close(void *rv) {
    cache_reader *r = rv;
    if (!r) return;
    r->be->get_close(r->h);
    free(r);
}

static int cache_del(void *store, const char *key) {
    cache *c = store;
    if (!c || !oc_blob_key_ok(key)) return -1;
    if (queue_add(c, key, "del") != 0) return -1;
    queue_drop(c, key, "put");
    int rc = oc_blob_backend_fs.del(c->fs, key);
    count_queued(c, 0, 0);
    wake(c);
    return rc;
}

/* --- the uploader ----------------------------------------------------------- */

typedef struct { char name[300]; uint64_t mtime; } qent;

static int qent_cmp(const void *a, const void *b) {
    const qent *x = a, *y = b;
    return x->mtime < y->mtime ? -1 : x->mtime > y->mtime;
}

/* The queue, oldest first. The caller frees *out. */
static size_t queue_list(cache *c, qent **out) {
    *out = NULL;
    DIR *d = opendir(c->qdir);
    if (!d) return 0;
    size_t n = 0, cap = 0;
    qent *v = NULL;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        size_t l = strlen(de->d_name);
        if (l < 5 || l >= sizeof v->name) continue;
        const char *ext = de->d_name + l - 4;
        if (strcmp(ext, ".put") != 0 && strcmp(ext, ".del") != 0) continue;
        char p[1400];
        struct stat sb;
        if (snprintf(p, sizeof p, "%s/%s", c->qdir, de->d_name) >= (int)sizeof p || stat(p, &sb) != 0) continue;
        if (n == cap) {
            size_t nc = cap ? cap * 2 : 64;
            qent *nv = realloc(v, nc * sizeof *v);
            if (!nv) break;
            v = nv; cap = nc;
        }
        snprintf(v[n].name, sizeof v[n].name, "%s", de->d_name);
        v[n].mtime = (uint64_t)sb.st_mtim.tv_sec * 1000 + (uint64_t)sb.st_mtim.tv_nsec / 1000000;
        n++;
    }
    closedir(d);
    if (n > 1) qsort(v, n, sizeof *v, qent_cmp);
    *out = v;
    return n;
}

/* 1 sent, 0 nothing to send (the blob is gone), 2 not yet published (try on
 * the next pass), -1 failed. */
static int upload(cache *c, const char *key) {
    char path[1400], staged[1400];
    if (path_of(path, sizeof path, c->base, key, "") != 0 ||
        path_of(staged, sizeof staged, c->base, key, ".tmp") != 0) return 0;
    FILE *fp = fopen(path, "rb");
    if (!fp && errno == ENOENT) {
        /* The entry is made before the blob is renamed into place, so a blob
         * not there yet may be one still being published: its staging file
         * says so. The rename is atomic, so with the staging file gone a blob
         * that was published is there to be opened. */
        if (access(staged, F_OK) == 0) return 2;
        fp = fopen(path, "rb");
    }
    if (!fp) return errno == ENOENT ? 0 : -1;   /* nothing left to send */
    struct stat sb;
    if (fstat(fileno(fp), &sb) != 0) { fclose(fp); return -1; }
    void *w = oc_blob_backend_s3.put_begin(c->s3, key, (uint64_t)sb.st_size);
    if (!w) { fclose(fp); return -1; }
    char buf[64 * 1024];
    size_t n;
    int ok = 1;
    while (ok && (n = fread(buf, 1, sizeof buf, fp)) > 0) {
        if (stopping(c)) { ok = 0; break; }   /* shutting down: it stays queued */
        ok = oc_blob_backend_s3.put_chunk(w, buf, n) == 0;
    }
    ok = ok && !ferror(fp);
    fclose(fp);
    if (!ok) { oc_blob_backend_s3.put_abort(w); return -1; }
    return oc_blob_backend_s3.put_commit(w) == 0 ? 1 : -1;
}

static void note_failure(cache *c, const char *what, const char *key) {
    pthread_mutex_lock(&c->mu);
    uint32_t f = ++c->st.failures;
    uint64_t base = pol_or(c->pol.retry_base_ms, DEF_RETRY_BASE_MS);
    uint64_t cap  = pol_or(c->pol.retry_cap_ms, DEF_RETRY_CAP_MS);
    uint64_t d = base * (uint64_t)f * (uint64_t)f;
    if (d > cap || d / f / f != base) d = cap;
    /* Up to a fifth either way, so many daemons that lost the store together
     * do not come back to it together. */
    uint64_t j = d / 5;
    if (j) d = d - j + (uint64_t)rand() % (2 * j + 1);
    c->st.next_try_ms = now_ms() + d;
    c->st.last_error_ms = now_ms();
    snprintf(c->st.last_error, sizeof c->st.last_error, "could not %s %.64s", what, key);
    pthread_mutex_unlock(&c->mu);
    if (f == 1 || f % 10 == 0)
        fprintf(stderr, "openchimed: blob store: could not %s %s (%u in a row); retrying in %llus\n",
                what, key, f, (unsigned long long)(d / 1000));
}

static void note_success(cache *c) {
    pthread_mutex_lock(&c->mu);
    uint32_t was = c->st.failures;
    c->st.failures = 0;
    c->st.next_try_ms = 0;
    pthread_mutex_unlock(&c->mu);
    if (was) fprintf(stderr, "openchimed: blob store: reachable again after %u failed attempt%s\n",
                     was, was == 1 ? "" : "s");
}

/* Send what is queued, oldest first, until it is done or something fails. */
static void drain(cache *c) {
    qent *q;
    size_t n = queue_list(c, &q);
    for (size_t i = 0; i < n && !stopping(c); i++) {
        size_t l = strlen(q[i].name);
        char key[300];
        snprintf(key, sizeof key, "%.*s", (int)(l - 4), q[i].name);
        int is_put = strcmp(q[i].name + l - 4, ".put") == 0;
        char ent[1400];
        snprintf(ent, sizeof ent, "%s/%s", c->qdir, q[i].name);
        if (!oc_blob_key_ok(key)) { unlink(ent); continue; }
        struct stat before;
        if (stat(ent, &before) != 0) continue;              /* taken since */
        int rc;
        if (is_put) {
            if (queued(c, key, "del")) { unlink(ent); continue; }
            rc = upload(c, key);
            if (rc == 2) continue;   /* still being published: the next pass */
            if (rc == 1) { pthread_mutex_lock(&c->mu); c->st.uploaded++; pthread_mutex_unlock(&c->mu); }
        } else {
            rc = oc_blob_backend_s3.del(c->s3, key) == 0 ? 1 : -1;
        }
        if (rc < 0) {
            if (stopping(c)) break;
            /* To the back of the queue, and the queue waits. */
            struct timespec t[2] = { { 0, UTIME_OMIT }, { 0, UTIME_NOW } };
            utimensat(AT_FDCWD, ent, t, 0);
            note_failure(c, is_put ? "store" : "delete", key);
            break;
        }
        /* Removed only if it is still the entry that was sent. */
        struct stat after;
        if (stat(ent, &after) == 0 && after.st_ino == before.st_ino) unlink(ent);
        note_success(c);
    }
    free(q);
}

typedef struct { char name[300]; uint64_t size, atime; } cent;

static int cent_cmp(const void *a, const void *b) {
    const cent *x = a, *y = b;
    return x->atime < y->atime ? -1 : x->atime > y->atime;
}

/* Bring the blob directory within its budget, and count what is in it and in
 * the queue. */
static void tend(cache *c) {
    pthread_mutex_lock(&c->mu);
    uint64_t floor = c->pol.floor_bytes;
    unsigned share = c->pol.share_pct;
    pthread_mutex_unlock(&c->mu);

    DIR *d = opendir(c->base);
    if (!d) return;
    size_t n = 0, cap = 0;
    cent *v = NULL;
    uint64_t total = 0, pend_n = 0, pend_b = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!oc_blob_key_ok(de->d_name) || strlen(de->d_name) >= sizeof v->name) continue;
        char p[1400];
        struct stat sb;
        if (path_of(p, sizeof p, c->base, de->d_name, "") != 0 || stat(p, &sb) != 0 || !S_ISREG(sb.st_mode)) continue;
        total += (uint64_t)sb.st_size;
        if (queued(c, de->d_name, "put")) { pend_n++; pend_b += (uint64_t)sb.st_size; continue; }
        if (n == cap) {
            size_t nc = cap ? cap * 2 : 256;
            cent *nv = realloc(v, nc * sizeof *v);
            if (!nv) break;
            v = nv; cap = nc;
        }
        snprintf(v[n].name, sizeof v[n].name, "%s", de->d_name);
        v[n].size = (uint64_t)sb.st_size;
        v[n].atime = (uint64_t)sb.st_atim.tv_sec * 1000 + (uint64_t)sb.st_atim.tv_nsec / 1000000;
        n++;
    }
    closedir(d);

    /* The budget is a share of the room the cache could have: what it holds
     * plus what is free, above the floor. Counting its own bytes in keeps the
     * budget from shrinking just because the cache grew. */
    uint64_t avail = avail_bytes(c);
    uint64_t budget = UINT64_MAX;
    if (avail != UINT64_MAX) {
        uint64_t room = avail + total > floor ? avail + total - floor : 0;
        budget = room / 100 * share;
    }
    uint64_t evicted = 0;
    if (total > budget && n) {
        qsort(v, n, sizeof *v, cent_cmp);
        for (size_t i = 0; i < n && total > budget; i++) {
            char p[1400];
            if (path_of(p, sizeof p, c->base, v[i].name, "") != 0) continue;
            if (queued(c, v[i].name, "put")) continue;   /* written since the scan */
            if (unlink(p) == 0) { total -= v[i].size; evicted++; }
        }
    }
    free(v);

    uint64_t oldest = 0;
    qent *q;
    size_t qn = queue_list(c, &q);
    for (size_t i = 0; i < qn; i++) {
        char p[1400];
        snprintf(p, sizeof p, "%s/%s", c->qdir, q[i].name);
        FILE *fp = fopen(p, "r");
        unsigned long long at = 0;
        if (fp) { if (fscanf(fp, "%llu", &at) != 1) at = 0; fclose(fp); }
        if (at && (!oldest || at < oldest)) oldest = at;
    }
    free(q);

    pthread_mutex_lock(&c->mu);
    c->st.pending_count = pend_n;
    c->st.pending_bytes = pend_b;
    c->st.queued = qn;
    c->st.oldest_queued_ms = oldest;
    c->st.cached_bytes = total;
    c->st.budget_bytes = budget;
    c->st.evictions += evicted;
    pthread_mutex_unlock(&c->mu);
}

/* --- the sweep -------------------------------------------------------------- */

typedef struct {
    cache   *c;
    uint64_t older_than;
    char     keys[SWEEP_BATCH][300];
    size_t   n;
    int      failed;
} sweep_ctx;

static void sweep_flush(sweep_ctx *s) {
    if (!s->n || s->failed) { s->n = 0; return; }
    const char *keys[SWEEP_BATCH];
    uint8_t ref[SWEEP_BATCH];
    for (size_t i = 0; i < s->n; i++) keys[i] = s->keys[i];
    pthread_mutex_lock(&s->c->mu);
    oc_blob_ref_fn fn = s->c->pol.referenced;
    void *ctx = s->c->pol.ref_ctx;
    pthread_mutex_unlock(&s->c->mu);
    /* Could not tell: delete nothing. */
    if (!fn || fn(ctx, keys, s->n, ref) != 0) { s->failed = 1; s->n = 0; return; }
    for (size_t i = 0; i < s->n; i++) {
        if (ref[i] || queued(s->c, keys[i], "put")) continue;
        if (oc_blob_backend_s3.del(s->c->s3, keys[i]) != 0) { s->failed = 1; break; }
        oc_blob_backend_fs.del(s->c->fs, keys[i]);
        pthread_mutex_lock(&s->c->mu);
        s->c->st.swept++;
        pthread_mutex_unlock(&s->c->mu);
    }
    s->n = 0;
}

static int sweep_one(void *ctx, const char *key, uint64_t size, uint64_t modified_ms) {
    (void)size;
    sweep_ctx *s = ctx;
    if (stopping(s->c)) return 1;
    if (!oc_blob_key_ok(key) || !modified_ms || modified_ms >= s->older_than) return 0;
    snprintf(s->keys[s->n++], sizeof s->keys[0], "%s", key);
    if (s->n == SWEEP_BATCH) sweep_flush(s);
    return s->failed;
}

static int sweep_on(cache *c) {
    pthread_mutex_lock(&c->mu);
    int on = c->pol.sweep_age_ms && c->pol.referenced;
    pthread_mutex_unlock(&c->mu);
    return on;
}

static void sweep(cache *c) {
    pthread_mutex_lock(&c->mu);
    uint64_t age = c->pol.sweep_age_ms;
    pthread_mutex_unlock(&c->mu);
    sweep_ctx *s = calloc(1, sizeof *s);
    if (!s) return;
    s->c = c;
    s->older_than = now_ms() > age ? now_ms() - age : 0;
    char token[3000] = "", next[3000];
    pthread_mutex_lock(&c->mu);
    uint64_t before = c->st.swept;
    pthread_mutex_unlock(&c->mu);
    int listed = 1;
    do {
        if (oc_blob_s3_list(c->s3, token, sweep_one, s, next, sizeof next) != 0) { s->failed = 1; listed = 0; break; }
        sweep_flush(s);
        snprintf(token, sizeof token, "%s", next);
    } while (token[0] && !s->failed && !stopping(c));
    if (!s->failed) sweep_flush(s);
    pthread_mutex_lock(&c->mu);
    uint64_t took = c->st.swept - before;
    pthread_mutex_unlock(&c->mu);
    /* Once a day at most, so always said: what it took, or that it could not. */
    if (s->failed && !stopping(c))
        fprintf(stderr, "openchimed: blob store: the sweep stopped: %s; it tries again tomorrow\n",
                listed ? "the database or the store did not answer" : "the store could not be listed");
    else if (!stopping(c))
        fprintf(stderr, "openchimed: blob store: the sweep deleted %llu object%s no row names\n",
                (unsigned long long)took, took == 1 ? "" : "s");
    free(s);
}

/* --- the thread ------------------------------------------------------------- */

/* A blob directory first given this store, or given a different one than
 * before, has everything in it queued for the store. */
static void adopt(cache *c) {
    char path[1200], have[900] = "";
    snprintf(path, sizeof path, "%s/.store", c->base);
    FILE *fp = fopen(path, "r");
    if (fp) {
        if (!fgets(have, sizeof have, fp)) have[0] = '\0';
        fclose(fp);
        have[strcspn(have, "\n")] = '\0';
    }
    if (strcmp(have, c->ident) == 0) return;
    DIR *d = opendir(c->base);
    unsigned queued_n = 0;
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (!oc_blob_key_ok(de->d_name)) continue;
            char p[1400];
            struct stat sb;
            if (path_of(p, sizeof p, c->base, de->d_name, "") != 0 || stat(p, &sb) != 0 || !S_ISREG(sb.st_mode)) continue;
            if (queue_add(c, de->d_name, "put") == 0) queued_n++;
        }
        closedir(d);
    }
    if (queued_n)
        fprintf(stderr, "openchimed: blob store: %u blob%s already on disk queued for the store\n",
                queued_n, queued_n == 1 ? "" : "s");
    fp = fopen(path, "w");
    if (fp) { fprintf(fp, "%s\n", c->ident); fclose(fp); }
}

static void *uploader(void *arg) {
    cache *c = arg;
    for (;;) {
        uint64_t mnow = mono_ms();
        uint64_t rnow = now_ms();
        pthread_mutex_lock(&c->mu);
        int backing_off = c->st.next_try_ms && rnow < c->st.next_try_ms;
        int clean = !c->st.failures;
        uint64_t sweep_every = pol_or(c->pol.sweep_every_ms, DEF_SWEEP_EVERY_MS);
        pthread_mutex_unlock(&c->mu);

        if (!backing_off) drain(c);
        tend(c);
        pthread_mutex_lock(&c->mu);
        int idle = c->st.queued == 0;
        pthread_mutex_unlock(&c->mu);
        if (clean && idle && sweep_on(c) && (!c->last_sweep_ms || mnow - c->last_sweep_ms >= sweep_every)) {
            c->last_sweep_ms = mnow ? mnow : 1;
            sweep(c);
        }

        pthread_mutex_lock(&c->mu);
        uint64_t wait = pol_or(c->pol.pass_ms, DEF_PASS_MS);
        if (c->st.next_try_ms) {
            uint64_t t = now_ms();
            uint64_t until = c->st.next_try_ms > t ? c->st.next_try_ms - t : 0;
            if (until < wait) wait = until;
        }
        struct timespec dl;
        clock_gettime(CLOCK_REALTIME, &dl);
        dl.tv_sec += (time_t)(wait / 1000);
        dl.tv_nsec += (long)(wait % 1000) * 1000000;
        if (dl.tv_nsec >= 1000000000) { dl.tv_sec++; dl.tv_nsec -= 1000000000; }
        while (!c->stop && !c->kick) {
            if (pthread_cond_timedwait(&c->cv, &c->mu, &dl) != 0) break;
        }
        int stop = c->stop;
        /* A kick while backing off only recounts; the wait is the store's. */
        c->kick = 0;
        pthread_mutex_unlock(&c->mu);
        if (stop) break;
    }
    return NULL;
}

/* --- open, close ------------------------------------------------------------ */

/* Staging left by a crash: a fetch or an upload that never finished. Nothing
 * else is writing yet. */
static void clear_staging(const char *base) {
    DIR *d = opendir(base);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        size_t l = strlen(de->d_name);
        if (strstr(de->d_name, ".fetch-") || (l > 4 && strcmp(de->d_name + l - 4, ".tmp") == 0)) {
            char p[1400];
            if (snprintf(p, sizeof p, "%s/%s", base, de->d_name) < (int)sizeof p) unlink(p);
        }
    }
    closedir(d);
}

static void *cache_open(const char *base_dir, const void *backend_cfg) {
    const oc_s3_config *s3 = backend_cfg;
    if (!base_dir || !*base_dir || !s3) return NULL;
    cache *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    if (snprintf(c->base, sizeof c->base, "%s", base_dir) >= (int)sizeof c->base) { free(c); return NULL; }
    snprintf(c->qdir, sizeof c->qdir, "%s/.queue", c->base);
    snprintf(c->ident, sizeof c->ident, "%s/%s/%s", s3->endpoint, s3->bucket ? s3->bucket : "",
             s3->prefix ? s3->prefix : "");
    c->fs = oc_blob_backend_fs.open(base_dir, NULL);
    if (!c->fs) { free(c); return NULL; }
    c->s3 = oc_blob_backend_s3.open(base_dir, s3);
    if (!c->s3) { oc_blob_backend_fs.close(c->fs); free(c); return NULL; }
    if (mkdir(c->qdir, 0700) != 0 && errno != EEXIST) {
        oc_blob_backend_s3.close(c->s3); oc_blob_backend_fs.close(c->fs); free(c); return NULL;
    }
    clear_staging(c->base);
    pthread_mutex_init(&c->mu, NULL);
    pthread_cond_init(&c->cv, NULL);
    c->pol.share_pct = OC_BLOB_CACHE_SHARE_PCT;
    c->st.budget_bytes = UINT64_MAX;
    /* Before the uploader starts and before anything can be written: a blob
     * written after the scan is queued by its own write, and only once. Then
     * the figures, so what a restart found waiting reads so at once. */
    adopt(c);
    tend(c);
    if (pthread_create(&c->th, NULL, uploader, c) != 0) {
        pthread_cond_destroy(&c->cv); pthread_mutex_destroy(&c->mu);
        oc_blob_backend_s3.close(c->s3); oc_blob_backend_fs.close(c->fs); free(c); return NULL;
    }
    c->started = 1;
    return c;
}

static void cache_close(void *store) {
    cache *c = store;
    if (!c) return;
    if (c->started) {
        pthread_mutex_lock(&c->mu);
        c->stop = 1;
        pthread_cond_signal(&c->cv);
        pthread_mutex_unlock(&c->mu);
        pthread_join(c->th, NULL);
    }
    pthread_cond_destroy(&c->cv);
    pthread_mutex_destroy(&c->mu);
    oc_blob_backend_s3.close(c->s3);
    oc_blob_backend_fs.close(c->fs);
    free(c);
}

const oc_blob_backend oc_blob_backend_cache = {
    cache_open, cache_close, cache_put_begin, cache_put_chunk, cache_put_commit, cache_put_abort,
    cache_get_begin, cache_get_chunk, cache_get_close, cache_del,
};

/* --- what blobstore.c reaches through --------------------------------------- */

void oc_blob_cache_set_policy(void *store, const oc_blob_cache_policy *p) {
    cache *c = store;
    if (!c || !p) return;
    pthread_mutex_lock(&c->mu);
    c->pol = *p;
    if (!c->pol.share_pct) c->pol.share_pct = OC_BLOB_CACHE_SHARE_PCT;
    if (c->pol.share_pct > 100) c->pol.share_pct = 100;
    c->kick = 1;
    pthread_cond_signal(&c->cv);
    pthread_mutex_unlock(&c->mu);
}

void oc_blob_cache_get_stats(void *store, oc_blob_cache_stats *out) {
    cache *c = store;
    pthread_mutex_lock(&c->mu);
    *out = c->st;
    pthread_mutex_unlock(&c->mu);
}

void oc_blob_cache_kick(void *store) { wake(store); }
