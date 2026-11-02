/* Hermetic test for the S3 blob backend (ARCH-70, daemon/blob_s3.c).
 *
 * Stands up a minimal fake S3 endpoint on loopback in a thread and points the
 * real backend at it over plain HTTP, so the whole put/get/delete vertical runs
 * with **no network, no credentials, and no external service** — the committed
 * suite must never need any of those.
 *
 * What this can and cannot prove. It covers our side of the exchange: request
 * framing, the streaming body, response-header parsing (including body bytes
 * that arrive in the same read as the headers), size reporting, error statuses,
 * and that a well-formed SigV4 Authorization header is actually sent. It
 * CANNOT prove a real provider accepts our signatures — a fake validates
 * whatever we produce by construction. That gap is closed by the manual
 * tests/manual/s3_smoke.c run against a live provider.
 *
 * The fake keeps a connection open across requests, as S3 does, one thread per
 * connection, and counts the connections it accepts: that count is how the
 * reuse of a kept connection (and the cases that must not reuse one) is seen. */

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
#include <time.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <unistd.h>

/* --- the fake S3 endpoint -------------------------------------------------- */

#define FAKE_MAX_OBJ  (512 * 1024)

typedef struct {
    int      listen_fd;
    int      port;
    int      v6;         /* listening on [::1] rather than 127.0.0.1 */
    pthread_t th;
    volatile int stop;

    /* One thread per accepted connection, each serving requests on it until
     * the client closes it. */
    pthread_mutex_t mu;               /* the object, the observations, the counts */
    pthread_t conn_th[64];
    int       conn_fd[64];
    int       n_conn;
    int       accepts;                /* connections accepted, for the reuse tests */
    /* Behaviours a test asks for. */
    int      close_after;            /* close after each response, saying nothing */
    int      say_close;              /* answer `Connection: close`, but keep the socket */
    int      drop_second;            /* once: read a connection's second request, close unanswered */

    /* Exactly one object, which is all the backend ever handles per request. */
    char     key[256];
    uint8_t  body[FAKE_MAX_OBJ];
    size_t   body_len;
    int      have_obj;

    /* Observations the test asserts on. */
    char     last_method[16];
    char     last_uri[512];
    char     last_auth[512];
    char     last_sha_hdr[128];
    int      saw_request;
} fake_s3;

static fake_s3 g_fake;

/* Read until the end of the header block; returns header length, or -1. Any
 * body bytes already read are left in buf beyond the returned length. */
static long read_headers(int fd, char *buf, size_t cap, size_t *total_out) {
    size_t total = 0;
    while (total < cap - 1) {
        ssize_t n = read(fd, buf + total, cap - 1 - total);
        if (n <= 0) return -1;
        total += (size_t)n;
        buf[total] = '\0';
        char *end = strstr(buf, "\r\n\r\n");
        if (end) { *total_out = total; return (long)(end - buf) + 4; }
    }
    return -1;
}

/* Case-insensitive header lookup into `out`. */
static void header_value(const char *hdrs, const char *name, char *out, size_t cap) {
    out[0] = '\0';
    size_t nlen = strlen(name);
    for (const char *p = hdrs; *p; p++) {
        if ((p == hdrs || p[-1] == '\n') && strncasecmp(p, name, nlen) == 0 && p[nlen] == ':') {
            const char *v = p + nlen + 1;
            while (*v == ' ') v++;
            const char *e = strpbrk(v, "\r\n");
            size_t len = e ? (size_t)(e - v) : strlen(v);
            if (len >= cap) len = cap - 1;
            memcpy(out, v, len);
            out[len] = '\0';
            return;
        }
    }
}

static void send_all(int fd, const char *s, size_t n) {
    size_t sent = 0;
    while (sent < n) {
        ssize_t w = write(fd, s + sent, n - sent);
        if (w <= 0) return;
        sent += (size_t)w;
    }
}

typedef struct { fake_s3 *f; int fd; } fake_conn;

/* One request on `fd`, answered. 0 to go on serving the connection, -1 to stop
 * (the client closed it, or the test asked for it closed). */
static int fake_serve(fake_s3 *f, int fd, int nth) {
    char buf[8192];
    size_t total = 0;
    long hlen = read_headers(fd, buf, sizeof buf, &total);
    if (hlen < 0) return -1;
    pthread_mutex_lock(&f->mu);
    if (nth == 2 && f->drop_second) {          /* the stale-connection case */
        f->drop_second = 0;
        pthread_mutex_unlock(&f->mu);
        return -1;
    }
    sscanf(buf, "%15s %511s", f->last_method, f->last_uri);
    header_value(buf, "Authorization", f->last_auth, sizeof f->last_auth);
    header_value(buf, "x-amz-content-sha256", f->last_sha_hdr, sizeof f->last_sha_hdr);
    f->saw_request = 1;

    /* The object key is the path after /<bucket>/. */
    const char *k = f->last_uri;
    const char *slash = strchr(k + 1, '/');
    snprintf(f->key, sizeof f->key, "%.255s", slash ? slash + 1 : k);
    const char *conn_hdr = f->say_close ? "Connection: close\r\n" : "";
    int close_after = f->close_after;

    char head[256];
    if (strcmp(f->last_method, "PUT") == 0) {
        char clv[32];
        header_value(buf, "Content-Length", clv, sizeof clv);
        size_t want = (size_t)atoll(clv);
        /* Body bytes that arrived with the headers. */
        size_t have = total - (size_t)hlen;
        if (have > want) have = want;
        if (have > sizeof f->body) have = sizeof f->body;
        memcpy(f->body, buf + hlen, have);
        size_t got = have;
        while (got < want && got < sizeof f->body) {
            ssize_t n = read(fd, f->body + got, want - got);
            if (n <= 0) break;
            got += (size_t)n;
        }
        f->body_len = got;
        f->have_obj = (got == want);
        int n = snprintf(head, sizeof head, "HTTP/1.1 %s\r\nContent-Length: 0\r\n%s\r\n",
                         f->have_obj ? "200 OK" : "400 Bad Request", conn_hdr);
        send_all(fd, head, (size_t)n);
    } else if (strcmp(f->last_method, "GET") == 0) {
        if (!f->have_obj) {
            int n = snprintf(head, sizeof head, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n%s\r\n", conn_hdr);
            send_all(fd, head, (size_t)n);
        } else {
            int n = snprintf(head, sizeof head, "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n%s\r\n",
                             f->body_len, conn_hdr);
            send_all(fd, head, (size_t)n);
            send_all(fd, (const char *)f->body, f->body_len);
        }
    } else if (strcmp(f->last_method, "DELETE") == 0) {
        f->have_obj = 0;
        f->body_len = 0;
        int n = snprintf(head, sizeof head, "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n%s\r\n", conn_hdr);
        send_all(fd, head, (size_t)n);
    } else {
        int n = snprintf(head, sizeof head, "HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\n%s\r\n", conn_hdr);
        send_all(fd, head, (size_t)n);
    }
    pthread_mutex_unlock(&f->mu);
    return close_after ? -1 : 0;
}

static void *fake_conn_thread(void *arg) {
    fake_conn fc = *(fake_conn *)arg;
    free(arg);
    /* Until the client closes it, or fake_stop shuts it down under it. */
    for (int nth = 1; fake_serve(fc.f, fc.fd, nth) == 0; nth++) {}
    shutdown(fc.fd, SHUT_RDWR);
    return NULL;
}

static void *fake_thread(void *arg) {
    fake_s3 *f = arg;
    for (;;) {
        int fd = accept(f->listen_fd, NULL, NULL);
        if (fd < 0) { if (f->stop) break; continue; }
        if (f->stop) { close(fd); break; }
        fake_conn *fc = malloc(sizeof *fc);
        pthread_mutex_lock(&f->mu);
        int slot = f->n_conn < 64 && fc ? f->n_conn++ : -1;
        if (slot >= 0) { f->accepts++; f->conn_fd[slot] = fd; }
        pthread_mutex_unlock(&f->mu);
        if (slot < 0) { free(fc); close(fd); continue; }
        fc->f = f; fc->fd = fd;
        if (pthread_create(&f->conn_th[slot], NULL, fake_conn_thread, fc) != 0) {
            free(fc); shutdown(fd, SHUT_RDWR); f->conn_th[slot] = 0;
        }
    }
    return NULL;
}

static int fake_accepts(fake_s3 *f) {
    pthread_mutex_lock(&f->mu);
    int n = f->accepts;
    pthread_mutex_unlock(&f->mu);
    return n;
}

static void fake_set(fake_s3 *f, int *field, int v) {
    pthread_mutex_lock(&f->mu);
    *field = v;
    pthread_mutex_unlock(&f->mu);
}

/* The loopback of `v6`'s family, at `port` (0: the kernel picks). */
static socklen_t loop_addr(struct sockaddr_storage *a, int v6, int port) {
    memset(a, 0, sizeof *a);
    if (v6) {
        struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)a;
        a6->sin6_family = AF_INET6; a6->sin6_addr = in6addr_loopback; a6->sin6_port = htons((uint16_t)port);
        return sizeof *a6;
    }
    struct sockaddr_in *a4 = (struct sockaddr_in *)a;
    a4->sin_family = AF_INET; a4->sin_addr.s_addr = htonl(INADDR_LOOPBACK); a4->sin_port = htons((uint16_t)port);
    return sizeof *a4;
}

static int fake_start(fake_s3 *f, int v6) {
    memset(f, 0, sizeof *f);
    pthread_mutex_init(&f->mu, NULL);
    f->v6 = v6;
    f->listen_fd = socket(v6 ? AF_INET6 : AF_INET, SOCK_STREAM, 0);
    if (f->listen_fd < 0) return -1;
    int yes = 1;
    setsockopt(f->listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    struct sockaddr_storage a;
    socklen_t al = loop_addr(&a, v6, 0);             /* ephemeral: no port clash */
    if (bind(f->listen_fd, (struct sockaddr *)&a, al) != 0) { close(f->listen_fd); return -1; }
    if (listen(f->listen_fd, 8) != 0) return -1;
    if (getsockname(f->listen_fd, (struct sockaddr *)&a, &al) != 0) return -1;
    f->port = v6 ? ntohs(((struct sockaddr_in6 *)&a)->sin6_port) : ntohs(((struct sockaddr_in *)&a)->sin_port);
    return pthread_create(&f->th, NULL, fake_thread, f) == 0 ? 0 : -1;
}

static void fake_stop(fake_s3 *f) {
    f->stop = 1;
    /* Poke the accept() so the thread notices the stop flag. */
    int fd = socket(f->v6 ? AF_INET6 : AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
        struct sockaddr_storage a;
        socklen_t al = loop_addr(&a, f->v6, f->port);
        if (connect(fd, (struct sockaddr *)&a, al) == 0) { /* handled */ }
        close(fd);
    }
    pthread_join(f->th, NULL);
    close(f->listen_fd);
    /* Every connection's thread: its client has closed it (each store is
     * closed first), or this wakes it. */
    for (int i = 0; i < f->n_conn; i++) {
        shutdown(f->conn_fd[i], SHUT_RDWR);
        if (f->conn_th[i]) pthread_join(f->conn_th[i], NULL);
        close(f->conn_fd[i]);
    }
    f->n_conn = 0;
    pthread_mutex_destroy(&f->mu);
}

/* --- the test -------------------------------------------------------------- */

/* The store's configuration as the loader would hand it (config.h). */
static char g_ep[64];
static oc_s3_config g_s3cfg;
static const oc_s3_config *set_env_for(const fake_s3 *f) {
    /* Explicit http:// keeps this plaintext: a bare host would now default to
     * HTTPS, which is the right production default but untestable hermetically. */
    snprintf(g_ep, sizeof g_ep, f->v6 ? "http://[::1]:%d" : "http://127.0.0.1:%d", f->port);
    g_s3cfg.endpoint = g_ep; g_s3cfg.bucket = "testbucket";
    g_s3cfg.access_key = "AKIDEXAMPLE"; g_s3cfg.secret_key = "SECRETEXAMPLEKEY"; g_s3cfg.region = "us-east-1";
    return &g_s3cfg;
}

static void clear_env(void) { memset(&g_s3cfg, 0, sizeof g_s3cfg); }

static void fill_pattern(uint8_t *b, size_t n, uint32_t seed) {
    uint32_t x = seed;
    for (size_t i = 0; i < n; i++) { x = x * 1664525u + 1013904223u; b[i] = (uint8_t)(x >> 24); }
}

/* A full streaming round-trip at `size`, written in `chunk`-sized pieces. */
static void round_trip(oc_blobstore *bs, fake_s3 *f, const char *key,
                       size_t size, size_t chunk) {
    uint8_t *src = malloc(size), *got = malloc(size);
    CHECK(src && got);
    if (!src || !got) { free(src); free(got); return; }
    fill_pattern(src, size, (uint32_t)size);

    oc_blob_writer *w = oc_blob_put_begin(bs, key, size);
    CHECK(w != NULL);
    if (!w) { free(src); free(got); return; }
    for (size_t off = 0; off < size; off += chunk) {
        size_t n = (size - off < chunk) ? size - off : chunk;
        CHECK(oc_blob_put_chunk(w, src + off, n) == 0);
    }
    CHECK(oc_blob_put_commit(w) == 0);
    /* The fake reassembled exactly the bytes we streamed. Read under its lock:
     * its connection threads write these, and a socket is no ordering a race
     * detector can see. */
    pthread_mutex_lock(&f->mu);
    CHECK(f->body_len == size);
    CHECK(memcmp(f->body, src, size) == 0);
    pthread_mutex_unlock(&f->mu);

    uint64_t reported = 0;
    oc_blob_reader *r = oc_blob_get_begin(bs, key, &reported);
    CHECK(r != NULL);
    if (r) {
        CHECK(reported == (uint64_t)size);
        size_t total = 0;
        while (total < size) {
            long n = oc_blob_get_chunk(r, got + total, size - total);
            if (n <= 0) break;
            total += (size_t)n;
        }
        oc_blob_get_close(r);
        CHECK(total == size);
        CHECK(total == size && memcmp(src, got, size) == 0);
    }
    free(src); free(got);
}

/* A whole object fetched; 1 when it is `len` bytes equal to `want`. */
static int get_all(oc_blobstore *bs, const char *key, const void *want, size_t len) {
    uint8_t got[256];
    uint64_t sz = 0;
    oc_blob_reader *r = oc_blob_get_begin(bs, key, &sz);
    if (!r) return 0;
    size_t total = 0;
    for (long n; total < sizeof got && (n = oc_blob_get_chunk(r, got + total, sizeof got - total)) > 0; )
        total += (size_t)n;
    oc_blob_get_close(r);
    return sz == len && total == len && memcmp(got, want, len) == 0;
}

static int put_all(oc_blobstore *bs, const char *key, const void *data, size_t len) {
    oc_blob_writer *w = oc_blob_put_begin(bs, key, len);
    if (!w) return -1;
    if (oc_blob_put_chunk(w, data, len) != 0) { oc_blob_put_abort(w); return -1; }
    return oc_blob_put_commit(w);
}

/* Connections are kept between requests (blob_s3.c, "Connections are kept"):
 * successive requests share one; one the server closed while it was idle is
 * left for a new one without an error; one that fails before any byte of its
 * response is retried once on a new one; and a response that says
 * `Connection: close` is not kept. Each on a store of its own, so the counts
 * start from nothing kept. */
static void keepalive_tests(fake_s3 *f) {
    /* Successive requests: one connection. */
    {
        oc_blobstore *bs = oc_blobstore_open_remote(&g_s3cfg);
        CHECK(bs != NULL);
        if (bs) {
            int a = fake_accepts(f);
            CHECK(put_all(bs, "kkkk0001", "kept", 4) == 0);
            CHECK(get_all(bs, "kkkk0001", "kept", 4));
            CHECK(oc_blob_delete(bs, "kkkk0001") == 0);
            CHECK(fake_accepts(f) == a + 1);
            oc_blobstore_close(bs);
        }
    }
    /* The server closes the kept connection while it is idle: the next request
     * goes on a new one, and succeeds. */
    {
        oc_blobstore *bs = oc_blobstore_open_remote(&g_s3cfg);
        CHECK(bs != NULL);
        if (bs) {
            fake_set(f, &f->close_after, 1);
            CHECK(put_all(bs, "kkkk0002", "idle", 4) == 0);
            fake_set(f, &f->close_after, 0);
            int a = fake_accepts(f);
            CHECK(get_all(bs, "kkkk0002", "idle", 4));
            CHECK(fake_accepts(f) == a + 1);
            oc_blobstore_close(bs);
        }
    }
    /* A kept connection that takes the request and answers nothing (closed as
     * the request went out): retried once on a new one, and no error. */
    {
        oc_blobstore *bs = oc_blobstore_open_remote(&g_s3cfg);
        CHECK(bs != NULL);
        if (bs) {
            int a = fake_accepts(f);
            fake_set(f, &f->drop_second, 1);
            CHECK(put_all(bs, "kkkk0003", "late", 4) == 0);
            CHECK(get_all(bs, "kkkk0003", "late", 4));
            CHECK(fake_accepts(f) == a + 2);
            fake_set(f, &f->drop_second, 0);
            oc_blobstore_close(bs);
        }
    }
    /* `Connection: close` is believed: the socket is left open by the server,
     * and still each request opens its own. */
    {
        oc_blobstore *bs = oc_blobstore_open_remote(&g_s3cfg);
        CHECK(bs != NULL);
        if (bs) {
            fake_set(f, &f->say_close, 1);
            int a = fake_accepts(f);
            CHECK(oc_blob_delete(bs, "kkkk0002") == 0);
            CHECK(oc_blob_delete(bs, "kkkk0003") == 0);
            CHECK(fake_accepts(f) == a + 2);
            fake_set(f, &f->say_close, 0);
            oc_blobstore_close(bs);
        }
    }
}

/* An upload on a kept connection that goes stale before the store answers
 * (the server takes the request and closes it unanswered): the backend says
 * EAGAIN, since the body cannot be sent twice from there; the cache's
 * uploader, which has the blob, sends it once more on a new connection, and
 * does not count it as the store failing. */
static int uploaded(oc_blobstore *bs, uint64_t n, oc_blob_cache_stats *st) {
    oc_blobstore_cache_kick(bs);
    return oc_blobstore_cache_stats(bs, st) == 0 && st->uploaded >= n && st->pending_count == 0;
}
static int wait_uploaded(oc_blobstore *bs, uint64_t n, oc_blob_cache_stats *st) {
    return WAIT_UNTIL(uploaded(bs, n, st), 5000);
}
static void stale_put_tests(fake_s3 *f, const char *dir) {
    {
        oc_blobstore *bs = oc_blobstore_open_remote(&g_s3cfg);
        CHECK(bs != NULL);
        if (bs) {
            int a = fake_accepts(f);
            CHECK(put_all(bs, "kkkk0010", "kept", 4) == 0);
            fake_set(f, &f->drop_second, 1);
            errno = 0;
            CHECK(put_all(bs, "kkkk0011", "late", 4) == -1 && errno == EAGAIN);
            CHECK(put_all(bs, "kkkk0011", "late", 4) == 0);      /* a new connection */
            CHECK(get_all(bs, "kkkk0011", "late", 4));
            CHECK(fake_accepts(f) == a + 2);
            fake_set(f, &f->drop_second, 0);
            oc_blobstore_close(bs);
        }
    }
    /* Two kept connections idle; one goes stale under a PUT: the other is
     * dropped with it, so the retry reaches the store on a new connection
     * rather than on another socket kept as long. */
    {
        oc_blobstore *bs = oc_blobstore_open_remote(&g_s3cfg);
        CHECK(bs != NULL);
        if (bs) {
            oc_blob_writer *w1 = oc_blob_put_begin(bs, "abcd0020", 2);
            oc_blob_writer *w2 = oc_blob_put_begin(bs, "abcd0021", 2);
            CHECK(w1 && w2);
            CHECK(w1 && oc_blob_put_chunk(w1, "w1", 2) == 0 && oc_blob_put_commit(w1) == 0);
            CHECK(w2 && oc_blob_put_chunk(w2, "w2", 2) == 0 && oc_blob_put_commit(w2) == 0);
            fake_set(f, &f->drop_second, 1);
            errno = 0;
            CHECK(put_all(bs, "abcd0022", "late", 4) == -1 && errno == EAGAIN);
            int a = fake_accepts(f);
            CHECK(put_all(bs, "abcd0022", "late", 4) == 0);
            CHECK(fake_accepts(f) == a + 1);                       /* a new connection */
            fake_set(f, &f->drop_second, 0);
            oc_blobstore_close(bs);
        }
    }
    {
        char cdir[700];
        oc_test_path(cdir, sizeof cdir, dir, "stale-cache");
        mkdir(cdir, 0700);
        oc_blobstore *bs = oc_blobstore_open(cdir, &g_s3cfg);
        CHECK(bs != NULL);
        if (bs) {
            oc_blob_cache_policy p;
            memset(&p, 0, sizeof p);
            p.retry_base_ms = 10000; p.retry_cap_ms = 10000; p.pass_ms = 20;   /* a backoff would show */
            oc_blobstore_cache_policy(bs, &p);
            oc_blob_cache_stats st;
            CHECK(put_all(bs, "abcd0012", "first", 5) == 0);
            CHECK(wait_uploaded(bs, 1, &st));
            fake_set(f, &f->drop_second, 1);
            CHECK(put_all(bs, "abcd0013", "second", 6) == 0);
            CHECK(wait_uploaded(bs, 2, &st));
            CHECK(st.failures == 0 && st.next_try_ms == 0 && st.last_error[0] == '\0');
            fake_set(f, &f->drop_second, 0);
            oc_blobstore_close(bs);
        }
    }
}

int run_blob_s3_tests(void) {
    printf("test_blob_s3: fake S3 endpoint: streaming put/get round-trip, "
           "sizes, SigV4 header shape, delete, 404, config selection, kept connections\n");

    fake_s3 *f = &g_fake;
    CHECK(fake_start(f, 0) == 0);
    if (f->port == 0) return failures;
    set_env_for(f);
    /* The suite's own directory: the S3 backend ignores it, the local one
     * stores under it. */
    char dir[512], fsdir[700];
    CHECK(oc_test_mkdtemp("blob-s3", dir, sizeof dir) == 0);
    oc_test_path(fsdir, sizeof fsdir, dir, "fs");

    /* The backend itself, uncached: every call is a request the fake sees.
     * The cache in front of it has its own suite (test_blob_cache.c). */
    stale_put_tests(f, dir);
    oc_blobstore *bs = oc_blobstore_open_remote(&g_s3cfg);
    CHECK(bs != NULL);
    CHECK(!oc_blobstore_is_local(bs));
    if (!bs) { fake_stop(f); clear_env(); oc_test_rmtree(dir); return failures; }

    /* Single-chunk, multi-chunk, and a chunk size that leaves a partial final
     * write — the last is what catches off-by-one errors in the body loop. */
    round_trip(bs, f, "aaaa0001", 11, 64 * 1024);
    round_trip(bs, f, "aaaa0002", 200 * 1024, 64 * 1024);
    round_trip(bs, f, "aaaa0003", 100000, 7777);

    /* The request the backend actually sent: path-style URI, SigV4 header with
     * the right algorithm + credential scope, and the unsigned-payload marker
     * that lets the body stream. */
    pthread_mutex_lock(&f->mu);
    CHECK(f->saw_request == 1);
    CHECK(strncmp(f->last_uri, "/testbucket/", 12) == 0);
    CHECK(strncmp(f->last_auth, "AWS4-HMAC-SHA256 ", 17) == 0);
    CHECK(strstr(f->last_auth, "Credential=AKIDEXAMPLE/") != NULL);
    CHECK(strstr(f->last_auth, "/us-east-1/s3/aws4_request") != NULL);
    CHECK(strstr(f->last_auth, "SignedHeaders=") != NULL);
    CHECK(strstr(f->last_auth, "Signature=") != NULL);
    CHECK(strcmp(f->last_sha_hdr, "UNSIGNED-PAYLOAD") == 0);
    pthread_mutex_unlock(&f->mu);

    /* With a prefix, the keys go under it, and still round-trip. */
    {
        g_s3cfg.prefix = "p/q/2";
        oc_blobstore *pb = oc_blobstore_open_remote(&g_s3cfg);
        CHECK(pb != NULL);
        if (pb) {
            round_trip(pb, f, "cccc0001", 1000, 4096);
            pthread_mutex_lock(&f->mu);
            CHECK(strncmp(f->last_uri, "/testbucket/p/q/2/cccc0001", 26) == 0);
            pthread_mutex_unlock(&f->mu);
            oc_blobstore_close(pb);
        }
        g_s3cfg.prefix = NULL;
    }

    /* Delete, then confirm the object is really gone (the fake 404s), and
     * that the caller can tell gone from unreachable. */
    CHECK(oc_blob_delete(bs, "aaaa0003") == 0);
    errno = 0;
    oc_blob_reader *gone = oc_blob_get_begin(bs, "aaaa0003", NULL);
    CHECK(gone == NULL);
    CHECK(errno == ENOENT);
    if (gone) oc_blob_get_close(gone);
    /* Deleting an absent key is documented as success, not an error. */
    CHECK(oc_blob_delete(bs, "aaaa0009") == 0);

    oc_blobstore_close(bs);

    keepalive_tests(f);

    /* --- backend selection is by configuration (ARCH-70) ------------------- */
    clear_env();
    /* No S3 credentials -> the local-filesystem backend, which works against a
     * real directory rather than the fake endpoint. */
    oc_blobstore *local = oc_blobstore_open(fsdir, NULL);
    CHECK(oc_blobstore_is_local(local));
    CHECK(local != NULL);
    if (local) {
        oc_blob_writer *w = oc_blob_put_begin(local, "bbbb0001", 4);
        CHECK(w != NULL);
        if (w) {
            CHECK(oc_blob_put_chunk(w, "abcd", 4) == 0);
            CHECK(oc_blob_put_commit(w) == 0);
        }
        uint8_t got[8];
        oc_blob_reader *r = oc_blob_get_begin(local, "bbbb0001", NULL);
        CHECK(r != NULL);
        if (r) {
            CHECK(oc_blob_get_chunk(r, got, sizeof got) == 4);
            CHECK(memcmp(got, "abcd", 4) == 0);
            oc_blob_get_close(r);
        }
        oc_blob_delete(local, "bbbb0001");
        oc_blobstore_close(local);
    }

    /* A partial S3 configuration is refused by the loader (test_config.c),
     * never reaching the store; a backend configuration with no keys does not
     * open either, rather than falling back to local disk. */
    { oc_s3_config part = { "http://127.0.0.1:1", "testbucket", NULL, NULL, NULL, NULL };
      CHECK(oc_blobstore_open(fsdir, &part) == NULL); }
    clear_env();

    fake_stop(f);

    /* An endpoint at an IPv6 address, in brackets: split into host and
     * port, reached over IPv6, and signed and sent with its brackets. */
    if (fake_start(f, 1) != 0) {
        SKIP("no IPv6 loopback on this host: the [::1] endpoint");
        oc_test_rmtree(dir);
        return failures;
    }
    set_env_for(f);
    oc_blobstore *bs6 = oc_blobstore_open_remote(&g_s3cfg);
    CHECK(bs6 != NULL);
    if (bs6) {
        round_trip(bs6, f, "cccc0001", 5000, 4096);
        pthread_mutex_lock(&f->mu);
        CHECK(f->saw_request == 1);
        pthread_mutex_unlock(&f->mu);
        oc_blobstore_close(bs6);
    }
    clear_env();
    fake_stop(f);
    oc_test_rmtree(dir);
    return failures;
}
