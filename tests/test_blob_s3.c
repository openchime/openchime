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
 * tests/manual/s3_smoke.c run against a live provider. */

#include "blobstore.h"
#include "check.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* --- the fake S3 endpoint -------------------------------------------------- */

#define FAKE_MAX_OBJ  (512 * 1024)

typedef struct {
    int      listen_fd;
    int      port;
    pthread_t th;
    volatile int stop;

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

static void *fake_thread(void *arg) {
    fake_s3 *f = arg;
    for (;;) {
        int fd = accept(f->listen_fd, NULL, NULL);
        if (fd < 0) { if (f->stop) break; continue; }
        if (f->stop) { close(fd); break; }

        char buf[8192];
        size_t total = 0;
        long hlen = read_headers(fd, buf, sizeof buf, &total);
        if (hlen < 0) { close(fd); continue; }

        sscanf(buf, "%15s %511s", f->last_method, f->last_uri);
        header_value(buf, "Authorization", f->last_auth, sizeof f->last_auth);
        header_value(buf, "x-amz-content-sha256", f->last_sha_hdr, sizeof f->last_sha_hdr);
        f->saw_request = 1;

        /* The object key is the path after /<bucket>/. */
        const char *k = f->last_uri;
        const char *slash = strchr(k + 1, '/');
        snprintf(f->key, sizeof f->key, "%.255s", slash ? slash + 1 : k);

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
            const char *resp = f->have_obj
                ? "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
                : "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            send_all(fd, resp, strlen(resp));
        } else if (strcmp(f->last_method, "GET") == 0) {
            if (!f->have_obj) {
                const char *resp = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                send_all(fd, resp, strlen(resp));
            } else {
                char head[256];
                int n = snprintf(head, sizeof head,
                                 "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n"
                                 "Connection: close\r\n\r\n", f->body_len);
                send_all(fd, head, (size_t)n);
                send_all(fd, (const char *)f->body, f->body_len);
            }
        } else if (strcmp(f->last_method, "DELETE") == 0) {
            f->have_obj = 0;
            f->body_len = 0;
            const char *resp = "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            send_all(fd, resp, strlen(resp));
        } else {
            const char *resp = "HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            send_all(fd, resp, strlen(resp));
        }
        close(fd);
    }
    return NULL;
}

static int fake_start(fake_s3 *f) {
    memset(f, 0, sizeof *f);
    f->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (f->listen_fd < 0) return -1;
    int yes = 1;
    setsockopt(f->listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;                                  /* ephemeral: no port clash */
    if (bind(f->listen_fd, (struct sockaddr *)&a, sizeof a) != 0) return -1;
    if (listen(f->listen_fd, 8) != 0) return -1;
    socklen_t al = sizeof a;
    if (getsockname(f->listen_fd, (struct sockaddr *)&a, &al) != 0) return -1;
    f->port = ntohs(a.sin_port);
    return pthread_create(&f->th, NULL, fake_thread, f) == 0 ? 0 : -1;
}

static void fake_stop(fake_s3 *f) {
    f->stop = 1;
    /* Poke the accept() so the thread notices the stop flag. */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
        struct sockaddr_in a;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons((uint16_t)f->port);
        if (connect(fd, (struct sockaddr *)&a, sizeof a) == 0) { /* handled */ }
        close(fd);
    }
    pthread_join(f->th, NULL);
    close(f->listen_fd);
}

/* --- the test -------------------------------------------------------------- */

static void set_env_for(const fake_s3 *f) {
    char ep[64];
    /* Explicit http:// keeps this plaintext: a bare host would now default to
     * HTTPS, which is the right production default but untestable hermetically. */
    snprintf(ep, sizeof ep, "http://127.0.0.1:%d", f->port);
    setenv("OPENCHIME_S3_ENDPOINT", ep, 1);
    setenv("OPENCHIME_S3_BUCKET", "testbucket", 1);
    setenv("OPENCHIME_S3_ACCESS_KEY", "AKIDEXAMPLE", 1);
    setenv("OPENCHIME_S3_SECRET_KEY", "SECRETEXAMPLEKEY", 1);
    setenv("OPENCHIME_S3_REGION", "us-east-1", 1);
}

static void clear_env(void) {
    unsetenv("OPENCHIME_S3_ENDPOINT");
    unsetenv("OPENCHIME_S3_BUCKET");
    unsetenv("OPENCHIME_S3_ACCESS_KEY");
    unsetenv("OPENCHIME_S3_SECRET_KEY");
    unsetenv("OPENCHIME_S3_REGION");
    unsetenv("OPENCHIME_BLOB_BACKEND");
}

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
    /* The fake reassembled exactly the bytes we streamed. */
    CHECK(f->body_len == size);
    CHECK(memcmp(f->body, src, size) == 0);

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

int run_blob_s3_tests(void) {
    printf("test_blob_s3: fake S3 endpoint — streaming put/get round-trip, "
           "sizes, SigV4 header shape, delete, 404, config selection\n");

    fake_s3 *f = &g_fake;
    CHECK(fake_start(f) == 0);
    if (f->port == 0) return failures;
    set_env_for(f);

    oc_blobstore *bs = oc_blobstore_open("/tmp/oc-blob-s3-unused");
    CHECK(bs != NULL);
    if (!bs) { fake_stop(f); clear_env(); return failures; }

    /* Single-chunk, multi-chunk, and a chunk size that leaves a partial final
     * write — the last is what catches off-by-one errors in the body loop. */
    round_trip(bs, f, "aaaa0001", 11, 64 * 1024);
    round_trip(bs, f, "aaaa0002", 200 * 1024, 64 * 1024);
    round_trip(bs, f, "aaaa0003", 100000, 7777);

    /* The request the backend actually sent: path-style URI, SigV4 header with
     * the right algorithm + credential scope, and the unsigned-payload marker
     * that lets the body stream. */
    CHECK(f->saw_request == 1);
    CHECK(strncmp(f->last_uri, "/testbucket/", 12) == 0);
    CHECK(strncmp(f->last_auth, "AWS4-HMAC-SHA256 ", 17) == 0);
    CHECK(strstr(f->last_auth, "Credential=AKIDEXAMPLE/") != NULL);
    CHECK(strstr(f->last_auth, "/us-east-1/s3/aws4_request") != NULL);
    CHECK(strstr(f->last_auth, "SignedHeaders=") != NULL);
    CHECK(strstr(f->last_auth, "Signature=") != NULL);
    CHECK(strcmp(f->last_sha_hdr, "UNSIGNED-PAYLOAD") == 0);

    /* Delete, then confirm the object is really gone (the fake 404s). */
    CHECK(oc_blob_delete(bs, "aaaa0003") == 0);
    oc_blob_reader *gone = oc_blob_get_begin(bs, "aaaa0003", NULL);
    CHECK(gone == NULL);
    if (gone) oc_blob_get_close(gone);
    /* Deleting an absent key is documented as success, not an error. */
    CHECK(oc_blob_delete(bs, "aaaa0009") == 0);

    oc_blobstore_close(bs);

    /* --- backend selection is by configuration (ARCH-70) ------------------- */
    clear_env();
    /* No S3 credentials -> the local-filesystem backend, which works against a
     * real directory rather than the fake endpoint. */
    oc_blobstore *local = oc_blobstore_open("build/oc-blob-fs-test");
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

    /* A partial S3 configuration is refused outright rather than silently
     * falling back to local disk, which would put attachments somewhere the
     * operator did not intend. */
    setenv("OPENCHIME_S3_ENDPOINT", "http://127.0.0.1:1", 1);
    setenv("OPENCHIME_S3_BUCKET", "testbucket", 1);
    /* access key + secret deliberately absent */
    CHECK(oc_blobstore_open("build/oc-blob-fs-test") == NULL);
    clear_env();

    fake_stop(f);
    return failures;
}
