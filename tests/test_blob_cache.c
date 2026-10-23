/* The blob directory as a disk cache in front of S3 (daemon/blob_cache.c).
 *
 * A fake S3 endpoint on loopback holds any number of objects, lists them a
 * page at a time, and fails every request on command, so the whole life of a
 * cached store runs here with no network and no credentials: a write accepted
 * while the store is down and sent once it is back, retries that wait longer
 * each time, reads served from disk or fetched into it, a delete that reaches
 * the store late, eviction that never takes the only copy, the blobs already
 * on disk when a store is first given, writes that go straight to the store
 * when the disk is short, and the sweep of objects no row names. */

#include "blobstore.h"
#include "check.h"
#include "testenv.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

/* --- the fake S3 endpoint -------------------------------------------------- */

#define FK_MAX 64
#define FK_BODY (256 * 1024)

typedef struct {
    char    key[300];          /* the full object name, prefix included */
    uint8_t *body;
    size_t  len;
    char    modified[40];      /* as a listing gives it */
    int     used;
} fk_obj;

typedef struct {
    int listen_fd, port;
    pthread_t th;
    pthread_mutex_t mu;
    int stop;
    int fail;                  /* every request answers 503 */
    int page;                  /* keys per listing page */
    fk_obj obj[FK_MAX];
    unsigned puts, gets, dels, lists;
} fake;

static fake g_fk;

static void fk_lock(fake *f) { pthread_mutex_lock(&f->mu); }
static void fk_unlock(fake *f) { pthread_mutex_unlock(&f->mu); }

static fk_obj *fk_find(fake *f, const char *key) {
    for (int i = 0; i < FK_MAX; i++)
        if (f->obj[i].used && strcmp(f->obj[i].key, key) == 0) return &f->obj[i];
    return NULL;
}

/* Put an object straight into the fake, as if another writer had. */
static void fk_seed(fake *f, const char *key, const char *body, const char *modified) {
    fk_lock(f);
    fk_obj *o = fk_find(f, key);
    for (int i = 0; !o && i < FK_MAX; i++) if (!f->obj[i].used) o = &f->obj[i];
    if (o) {
        free(o->body);
        o->used = 1;
        snprintf(o->key, sizeof o->key, "%s", key);
        o->len = strlen(body);
        o->body = malloc(o->len + 1);
        if (o->body) memcpy(o->body, body, o->len);
        snprintf(o->modified, sizeof o->modified, "%s", modified);
    }
    fk_unlock(f);
}

static int fk_has(fake *f, const char *key) {
    fk_lock(f);
    int h = fk_find(f, key) != NULL;
    fk_unlock(f);
    return h;
}

static int fk_body_is(fake *f, const char *key, const char *want) {
    fk_lock(f);
    fk_obj *o = fk_find(f, key);
    int ok = o && o->len == strlen(want) && memcmp(o->body, want, o->len) == 0;
    fk_unlock(f);
    return ok;
}

static void fk_set_fail(fake *f, int on) { fk_lock(f); f->fail = on; fk_unlock(f); }

static unsigned fk_puts(fake *f) { fk_lock(f); unsigned n = f->puts; fk_unlock(f); return n; }

static void send_all(int fd, const void *b, size_t n) {
    const char *p = b;
    while (n) { ssize_t w = write(fd, p, n); if (w <= 0) return; p += w; n -= (size_t)w; }
}

static void respond(int fd, const char *status, const void *body, size_t len) {
    char head[256];
    int n = snprintf(head, sizeof head, "HTTP/1.1 %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                     status, len);
    send_all(fd, head, (size_t)n);
    if (len) send_all(fd, body, len);
}

/* %XX back to bytes, in place. */
static void url_decode(char *s) {
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '%' && p[1] && p[2]) {
            char h[3] = { p[1], p[2], 0 };
            *o++ = (char)strtol(h, NULL, 16);
            p += 2;
        } else *o++ = *p;
    }
    *o = '\0';
}

static void query_param(const char *q, const char *name, char *out, size_t cap) {
    out[0] = '\0';
    size_t nl = strlen(name);
    for (const char *p = q; p && *p; ) {
        if (strncmp(p, name, nl) == 0 && p[nl] == '=') {
            const char *v = p + nl + 1, *e = strchr(v, '&');
            size_t l = e ? (size_t)(e - v) : strlen(v);
            if (l >= cap) l = cap - 1;
            memcpy(out, v, l); out[l] = '\0';
            url_decode(out);
            return;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
}

static int by_key(const void *a, const void *b) {
    return strcmp((*(const fk_obj *const *)a)->key, (*(const fk_obj *const *)b)->key);
}

/* A page of the keys under `prefix`, in key order, continuing after the key a
 * token names -- as S3 does, so deleting what one page listed does not shift
 * the next. */
static void fk_list(fake *f, int fd, const char *query) {
    char prefix[300], token[300];
    query_param(query, "prefix", prefix, sizeof prefix);
    query_param(query, "continuation-token", token, sizeof token);
    fk_obj *sorted[FK_MAX];
    int n = 0;
    for (int i = 0; i < FK_MAX; i++)
        if (f->obj[i].used && strncmp(f->obj[i].key, prefix, strlen(prefix)) == 0) sorted[n++] = &f->obj[i];
    qsort(sorted, (size_t)n, sizeof sorted[0], by_key);
    size_t cap = 64 * 1024, len = 0;
    char *x = malloc(cap);
    if (!x) { respond(fd, "500 Internal Server Error", NULL, 0); return; }
    len += (size_t)snprintf(x + len, cap - len, "<?xml version=\"1.0\"?><ListBucketResult>");
    int shown = 0, more = 0;
    const char *last = NULL;
    for (int i = 0; i < n; i++) {
        fk_obj *o = sorted[i];
        if (token[0] && strcmp(o->key, token) <= 0) continue;
        if (shown == f->page) { more = 1; break; }
        shown++;
        last = o->key;
        len += (size_t)snprintf(x + len, cap - len,
                                "<Contents><Key>%s</Key><LastModified>%s</LastModified><Size>%zu</Size></Contents>",
                                o->key, o->modified, o->len);
    }
    if (more && last) len += (size_t)snprintf(x + len, cap - len,
                                              "<IsTruncated>true</IsTruncated><NextContinuationToken>%s</NextContinuationToken>",
                                              last);
    else len += (size_t)snprintf(x + len, cap - len, "<IsTruncated>false</IsTruncated>");
    len += (size_t)snprintf(x + len, cap - len, "</ListBucketResult>");
    respond(fd, "200 OK", x, len);
    free(x);
}

static void *fk_thread(void *arg) {
    fake *f = arg;
    for (;;) {
        int fd = accept(f->listen_fd, NULL, NULL);
        fk_lock(f);
        int stop = f->stop;
        fk_unlock(f);
        if (fd < 0) { if (stop) break; continue; }
        if (stop) { close(fd); break; }

        char buf[16384];
        size_t total = 0;
        char *end = NULL;
        while (total < sizeof buf - 1) {
            ssize_t n = read(fd, buf + total, sizeof buf - 1 - total);
            if (n <= 0) break;
            total += (size_t)n;
            buf[total] = '\0';
            if ((end = strstr(buf, "\r\n\r\n")) != NULL) break;
        }
        if (!end) { close(fd); continue; }
        size_t hlen = (size_t)(end - buf) + 4;
        char method[16], target[2048];
        if (sscanf(buf, "%15s %2047s", method, target) != 2) { close(fd); continue; }
        char *q = strchr(target, '?');
        if (q) *q++ = '\0';
        /* /<bucket>/<key>: the key is everything after the bucket. */
        const char *slash = strchr(target + 1, '/');
        const char *key = slash ? slash + 1 : "";

        fk_lock(f);
        if (f->fail) {
            fk_unlock(f);
            respond(fd, "503 Service Unavailable", NULL, 0);
            close(fd);
            continue;
        }
        if (strcmp(method, "PUT") == 0) {
            const char *cl = strcasestr(buf, "\r\nContent-Length:");
            size_t want = cl ? (size_t)atoll(cl + 17) : 0;
            uint8_t *body = malloc(want ? want : 1);
            size_t have = total - hlen;
            if (have > want) have = want;
            if (body) memcpy(body, buf + hlen, have);
            fk_unlock(f);
            while (body && have < want) {
                ssize_t n = read(fd, body + have, want - have);
                if (n <= 0) break;
                have += (size_t)n;
            }
            fk_lock(f);
            if (body && have == want) {
                fk_obj *o = fk_find(f, key);
                for (int i = 0; !o && i < FK_MAX; i++) if (!f->obj[i].used) o = &f->obj[i];
                if (o) {
                    free(o->body);
                    o->used = 1;
                    snprintf(o->key, sizeof o->key, "%s", key);
                    o->body = body; o->len = want; body = NULL;
                    snprintf(o->modified, sizeof o->modified, "2099-01-01T00:00:00.000Z");
                }
                f->puts++;
                fk_unlock(f);
                respond(fd, "200 OK", NULL, 0);
            } else {
                fk_unlock(f);
                respond(fd, "400 Bad Request", NULL, 0);
            }
            free(body);
        } else if (strcmp(method, "GET") == 0 && q && strstr(q, "list-type=2")) {
            f->lists++;
            fk_list(f, fd, q);
            fk_unlock(f);
        } else if (strcmp(method, "GET") == 0) {
            f->gets++;
            fk_obj *o = fk_find(f, key);
            if (!o) { fk_unlock(f); respond(fd, "404 Not Found", NULL, 0); }
            else {
                size_t n = o->len;
                uint8_t *copy = malloc(n ? n : 1);
                if (copy) memcpy(copy, o->body, n);
                fk_unlock(f);
                if (copy) respond(fd, "200 OK", copy, n);
                free(copy);
            }
        } else if (strcmp(method, "DELETE") == 0) {
            f->dels++;
            fk_obj *o = fk_find(f, key);
            if (o) { free(o->body); o->body = NULL; o->used = 0; }
            fk_unlock(f);
            respond(fd, "204 No Content", NULL, 0);
        } else {
            fk_unlock(f);
            respond(fd, "405 Method Not Allowed", NULL, 0);
        }
        close(fd);
    }
    return NULL;
}

static int fk_start(fake *f) {
    memset(f, 0, sizeof *f);
    pthread_mutex_init(&f->mu, NULL);
    f->page = 1000;
    f->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (f->listen_fd < 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t al = sizeof a;
    if (bind(f->listen_fd, (struct sockaddr *)&a, al) != 0 || listen(f->listen_fd, 16) != 0 ||
        getsockname(f->listen_fd, (struct sockaddr *)&a, &al) != 0) { close(f->listen_fd); return -1; }
    f->port = ntohs(a.sin_port);
    return pthread_create(&f->th, NULL, fk_thread, f) == 0 ? 0 : -1;
}

static void fk_stop(fake *f) {
    fk_lock(f); f->stop = 1; fk_unlock(f);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
        struct sockaddr_in a;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons((uint16_t)f->port);
        if (connect(fd, (struct sockaddr *)&a, sizeof a) == 0) { /* woken */ }
        close(fd);
    }
    pthread_join(f->th, NULL);
    close(f->listen_fd);
    for (int i = 0; i < FK_MAX; i++) free(f->obj[i].body);
    pthread_mutex_destroy(&f->mu);
}

/* --- helpers --------------------------------------------------------------- */

static char g_ep[64];
static oc_s3_config g_cfg;

static void cfg_for(const fake *f, const char *prefix) {
    snprintf(g_ep, sizeof g_ep, "http://127.0.0.1:%d", f->port);
    memset(&g_cfg, 0, sizeof g_cfg);
    g_cfg.endpoint = g_ep; g_cfg.bucket = "bkt";
    g_cfg.access_key = "AKIDEXAMPLE"; g_cfg.secret_key = "SECRETEXAMPLEKEY"; g_cfg.region = "us-east-1";
    g_cfg.prefix = prefix;
}

/* Quick timing, so retries and passes happen within a test's patience. */
static oc_blob_cache_policy quick(void) {
    oc_blob_cache_policy p;
    memset(&p, 0, sizeof p);
    p.retry_base_ms = 10; p.retry_cap_ms = 80; p.pass_ms = 20;
    return p;
}

static int put(oc_blobstore *bs, const char *key, const char *body) {
    size_t n = strlen(body);
    oc_blob_writer *w = oc_blob_put_begin(bs, key, n);
    if (!w) return -1;
    if (oc_blob_put_chunk(w, body, n) != 0) { oc_blob_put_abort(w); return -1; }
    return oc_blob_put_commit(w);
}

/* The blob's bytes as the store serves them; 1 when they are `want`. */
static int reads_as(oc_blobstore *bs, const char *key, const char *want) {
    uint64_t size = 0;
    oc_blob_reader *r = oc_blob_get_begin(bs, key, &size);
    if (!r) return 0;
    char buf[1024];
    size_t got = 0;
    long n;
    while ((n = oc_blob_get_chunk(r, buf + got, sizeof buf - got)) > 0) got += (size_t)n;
    oc_blob_get_close(r);
    return size == strlen(want) && got == size && memcmp(buf, want, got) == 0;
}

static oc_blob_cache_stats stats(oc_blobstore *bs) {
    oc_blob_cache_stats s;
    memset(&s, 0, sizeof s);
    oc_blobstore_cache_stats(bs, &s);
    return s;
}

static int on_disk(const char *dir, const char *key) {
    char p[800];
    snprintf(p, sizeof p, "%s/%s", dir, key);
    return access(p, F_OK) == 0;
}

static int settled(oc_blobstore *bs) {
    oc_blobstore_cache_kick(bs);
    oc_blob_cache_stats s = stats(bs);
    return s.queued == 0 && s.pending_count == 0;
}

/* The sweep's question, answered from a fixed list. */
static int refs_fixed(void *ctx, const char *const *keys, size_t n, uint8_t *ref) {
    const char *const *keep = ctx;
    for (size_t i = 0; i < n; i++) {
        ref[i] = 0;
        for (const char *const *k = keep; *k; k++) if (strcmp(*k, keys[i]) == 0) ref[i] = 1;
    }
    return 0;
}

/* --- the tests ------------------------------------------------------------- */

static void writes_survive_a_store_that_is_down(fake *f, const char *dir) {
    cfg_for(f, "ws");
    oc_blobstore *bs = oc_blobstore_open(dir, &g_cfg);
    CHECK(bs != NULL);
    if (!bs) return;
    CHECK(!oc_blobstore_is_local(bs));
    oc_blob_cache_policy p = quick();
    oc_blobstore_cache_policy(bs, &p);

    fk_set_fail(f, 1);
    /* Accepted at once, though the store is down, and readable from disk. */
    CHECK(put(bs, "a1", "first attachment") == 0);
    CHECK(on_disk(dir, "a1"));
    CHECK(reads_as(bs, "a1", "first attachment"));
    CHECK(!fk_has(f, "ws/a1"));
    oc_blob_cache_stats s;
    CHECK(WAIT_UNTIL((oc_blobstore_cache_kick(bs), s = stats(bs), s.pending_count == 1 && s.failures >= 2), 5000));
    CHECK(s.pending_bytes == strlen("first attachment"));
    CHECK(s.oldest_queued_ms > 0);
    CHECK(s.next_try_ms > 0);
    CHECK(s.last_error[0] != '\0');

    /* The store comes back: the queue drains by itself. */
    fk_set_fail(f, 0);
    CHECK(WAIT_UNTIL(fk_has(f, "ws/a1") && settled(bs), 5000));
    CHECK(fk_body_is(f, "ws/a1", "first attachment"));
    s = stats(bs);
    CHECK(s.failures == 0 && s.next_try_ms == 0);
    CHECK(s.uploaded >= 1);
    /* Still on disk: it is the cache now. */
    CHECK(on_disk(dir, "a1"));
    CHECK(reads_as(bs, "a1", "first attachment"));
    CHECK(stats(bs).hits >= 2);

    /* A delete while the store is down reaches it once it is back. */
    fk_set_fail(f, 1);
    CHECK(oc_blob_delete(bs, "a1") == 0);
    CHECK(!on_disk(dir, "a1"));
    CHECK(fk_has(f, "ws/a1"));
    fk_set_fail(f, 0);
    CHECK(WAIT_UNTIL(!fk_has(f, "ws/a1") && settled(bs), 5000));
    oc_blobstore_close(bs);
}

static void reads_fetch_into_the_directory(fake *f, const char *dir) {
    cfg_for(f, "ws");
    fk_seed(f, "ws/b1", "only in the store", "2026-01-01T00:00:00.000Z");
    oc_blobstore *bs = oc_blobstore_open(dir, &g_cfg);
    CHECK(bs != NULL);
    if (!bs) return;
    oc_blob_cache_policy p = quick();
    oc_blobstore_cache_policy(bs, &p);

    CHECK(!on_disk(dir, "b1"));
    CHECK(reads_as(bs, "b1", "only in the store"));
    CHECK(on_disk(dir, "b1"));
    CHECK(stats(bs).misses == 1);
    CHECK(reads_as(bs, "b1", "only in the store"));
    CHECK(stats(bs).hits == 1 && stats(bs).misses == 1);

    /* Gone from both: the caller is told it is gone, not that it failed. */
    errno = 0;
    CHECK(oc_blob_get_begin(bs, "b404", NULL) == NULL);
    CHECK(errno == ENOENT);
    /* Unreachable is something else. */
    fk_set_fail(f, 1);
    errno = 0;
    CHECK(oc_blob_get_begin(bs, "b405", NULL) == NULL);
    CHECK(errno != ENOENT && errno != 0);
    fk_set_fail(f, 0);
    oc_blobstore_close(bs);
}

static void eviction_keeps_the_only_copy(fake *f, const char *dir) {
    cfg_for(f, "ws");
    oc_blobstore *bs = oc_blobstore_open(dir, &g_cfg);
    CHECK(bs != NULL);
    if (!bs) return;
    oc_blob_cache_policy p = quick();
    oc_blobstore_cache_policy(bs, &p);
    CHECK(put(bs, "c1", "sent one") == 0);
    CHECK(put(bs, "c2", "sent two") == 0);
    CHECK(WAIT_UNTIL(fk_has(f, "ws/c1") && fk_has(f, "ws/c2") && settled(bs), 5000));
    fk_set_fail(f, 1);
    CHECK(put(bs, "c3", "not sent yet") == 0);

    /* No room at all: the budget is nothing, and what the store has goes. */
    p.floor_bytes = UINT64_MAX / 4;
    oc_blobstore_cache_policy(bs, &p);
    CHECK(WAIT_UNTIL((oc_blobstore_cache_kick(bs), !on_disk(dir, "c1") && !on_disk(dir, "c2")), 5000));
    CHECK(stats(bs).evictions >= 2);
    CHECK(stats(bs).budget_bytes == 0);
    /* What it does not have stays: it is the only copy. */
    CHECK(on_disk(dir, "c3"));
    CHECK(reads_as(bs, "c3", "not sent yet"));

    /* A read with no room to keep it streams from the store, uncached. */
    fk_set_fail(f, 0);
    CHECK(reads_as(bs, "c1", "sent one"));
    CHECK(!on_disk(dir, "c1"));
    CHECK(WAIT_UNTIL(fk_has(f, "ws/c3") && settled(bs), 5000));

    /* A write with no room to stage it goes straight to the store. */
    unsigned before = fk_puts(f);
    CHECK(put(bs, "c4", "straight through") == 0);
    CHECK(fk_puts(f) == before + 1);
    CHECK(fk_body_is(f, "ws/c4", "straight through"));
    CHECK(!on_disk(dir, "c4"));
    oc_blobstore_close(bs);
}

static void blobs_on_disk_are_adopted(fake *f, const char *dir) {
    /* A daemon that kept its attachments on disk... */
    oc_blobstore *local = oc_blobstore_open(dir, NULL);
    CHECK(local != NULL);
    if (!local) return;
    CHECK(put(local, "d1", "kept on disk before") == 0);
    CHECK(put(local, "d2", "and another") == 0);
    oc_blobstore_close(local);

    /* ...is given a store: both go to it, in the background, and stay. A blob
     * written the moment the store opens goes too, once, like the others. */
    cfg_for(f, "other");
    unsigned puts_before = fk_puts(f);
    oc_blobstore *bs = oc_blobstore_open(dir, &g_cfg);
    CHECK(bs != NULL);
    if (!bs) return;
    CHECK(put(bs, "d3", "written as it opened") == 0);
    oc_blob_cache_policy p = quick();
    oc_blobstore_cache_policy(bs, &p);
    CHECK(WAIT_UNTIL(fk_has(f, "other/d1") && fk_has(f, "other/d2") && fk_has(f, "other/d3") && settled(bs), 5000));
    CHECK(fk_body_is(f, "other/d1", "kept on disk before"));
    CHECK(on_disk(dir, "d1") && on_disk(dir, "d2"));
    CHECK(fk_puts(f) - puts_before == 3);
    oc_blobstore_close(bs);

    /* The same store again: nothing is sent twice. */
    unsigned before = fk_puts(f);
    bs = oc_blobstore_open(dir, &g_cfg);
    CHECK(bs != NULL);
    if (!bs) return;
    oc_blobstore_cache_policy(bs, &p);
    CHECK(WAIT_UNTIL(settled(bs), 5000));
    CHECK(fk_puts(f) == before);
    oc_blobstore_close(bs);
}

static void queue_outlives_a_restart(fake *f, const char *dir) {
    cfg_for(f, "ws");
    fk_set_fail(f, 1);
    oc_blobstore *bs = oc_blobstore_open(dir, &g_cfg);
    CHECK(bs != NULL);
    if (!bs) return;
    oc_blob_cache_policy p = quick();
    oc_blobstore_cache_policy(bs, &p);
    CHECK(put(bs, "e1", "written before the restart") == 0);
    oc_blobstore_close(bs);

    fk_set_fail(f, 0);
    bs = oc_blobstore_open(dir, &g_cfg);
    CHECK(bs != NULL);
    if (!bs) return;
    oc_blobstore_cache_policy(bs, &p);
    CHECK(WAIT_UNTIL(fk_has(f, "ws/e1") && settled(bs), 5000));
    CHECK(fk_body_is(f, "ws/e1", "written before the restart"));
    oc_blobstore_close(bs);
}

static void sweep_takes_what_no_row_names(fake *f, const char *dir) {
    cfg_for(f, "sw");
    fk_lock(f); f->page = 2; fk_unlock(f);   /* several pages */
    fk_seed(f, "sw/aa01", "nobody names this", "2020-01-01T00:00:00.000Z");
    fk_seed(f, "sw/aa02", "a row names this", "2020-01-01T00:00:00.000Z");
    fk_seed(f, "sw/aa03", "nobody names this either", "2020-01-02T00:00:00.000Z");
    fk_seed(f, "sw/bb01", "too young to judge", "2099-01-01T00:00:00.000Z");
    fk_seed(f, "elsewhere/aa09", "not this store's", "2020-01-01T00:00:00.000Z");
    static const char *const keep[] = { "aa02", NULL };
    oc_blobstore *bs = oc_blobstore_open(dir, &g_cfg);
    CHECK(bs != NULL);
    if (!bs) return;
    oc_blob_cache_policy p = quick();
    p.referenced = refs_fixed;
    p.ref_ctx = (void *)keep;
    p.sweep_age_ms = 24ull * 3600 * 1000;
    oc_blobstore_cache_policy(bs, &p);
    CHECK(WAIT_UNTIL((oc_blobstore_cache_kick(bs), !fk_has(f, "sw/aa01") && !fk_has(f, "sw/aa03")), 5000));
    CHECK(stats(bs).swept == 2);
    CHECK(fk_has(f, "sw/aa02"));
    CHECK(fk_has(f, "sw/bb01"));
    CHECK(fk_has(f, "elsewhere/aa09"));
    oc_blobstore_close(bs);
    fk_lock(f); f->page = 1000; fk_unlock(f);

    /* With no prefix there is no sweep: the bucket's root is not ours. */
    cfg_for(f, NULL);
    fk_seed(f, "cc01", "at the root", "2020-01-01T00:00:00.000Z");
    bs = oc_blobstore_open(dir, &g_cfg);
    CHECK(bs != NULL);
    if (!bs) return;
    oc_blobstore_cache_policy(bs, &p);
    CHECK(WAIT_UNTIL(settled(bs), 5000));
    oc_blobstore_cache_kick(bs);
    CHECK(WAIT_UNTIL(settled(bs), 5000));
    CHECK(fk_has(f, "cc01"));
    CHECK(stats(bs).swept == 0);
    oc_blobstore_close(bs);
}

int run_blob_cache_tests(void) {
    printf("test_blob_cache: the blob directory cached in front of a fake S3 store: "
           "writes while it is down, retries, reads, deletes, eviction, adoption, the sweep\n");
    fake *f = &g_fk;
    CHECK(fk_start(f) == 0);
    if (!f->port) return failures;
    const char *names[] = { "down", "reads", "evict", "adopt", "restart", "sweep" };
    void (*tests[])(fake *, const char *) = {
        writes_survive_a_store_that_is_down, reads_fetch_into_the_directory,
        eviction_keeps_the_only_copy, blobs_on_disk_are_adopted,
        queue_outlives_a_restart, sweep_takes_what_no_row_names,
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        char dir[512];
        CHECK(oc_test_mkdtemp(names[i], dir, sizeof dir) == 0);
        tests[i](f, dir);
        oc_test_rmtree(dir);
    }
    fk_stop(f);
    return failures;
}
