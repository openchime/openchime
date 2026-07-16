/* S3/MinIO backend for the oc_blobstore seam (ARCH-70). Speaks the S3 REST API
 * (path-style `/<bucket>/<key>`) over plain HTTP to the configured endpoint,
 * signing each request with SigV4 (daemon/sigv4.c) and streaming the body with
 * `x-amz-content-sha256: UNSIGNED-PAYLOAD` so an upload/download need not be
 * buffered whole. Selected with OPENCHIME_BLOB_BACKEND=s3; configured via
 * OPENCHIME_S3_ENDPOINT (host:port), _BUCKET, _ACCESS_KEY, _SECRET_KEY, _REGION.
 *
 * NOTE: this backend does blocking network I/O; the daemon currently drives it
 * from the net thread, which is fine for the local-FS default but means a slow
 * S3 endpoint can stall the event loop. Moving blob I/O to a dedicated transfer
 * worker (ARCH-69) is the prerequisite for S3 under concurrent load. */

#include "blob_backend.h"
#include "sigv4.h"

#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    char endpoint[256];   /* host:port, as sent in the Host header */
    char host[256];       /* host only, for connect() */
    char port[16];
    char bucket[256];
    char region[64];
    char ak[256];
    char sk[256];
} s3_store;

typedef struct {
    int      fd;
    uint64_t remaining;   /* body bytes still to send */
    int      failed;
} s3_writer;

/* Spill must be >= read_response_head's header buffer, so all body bytes that
 * arrive alongside the header block are kept, never truncated. */
#define S3_SPILL 8192
typedef struct {
    int      fd;
    uint64_t remaining;   /* body bytes still to read */
    uint8_t  buf[S3_SPILL]; /* body bytes read while parsing headers */
    size_t   buf_off, buf_len;
} s3_reader;

static const char *env_or(const char *k, const char *d) {
    const char *v = getenv(k);
    return (v && *v) ? v : d;
}

static void *s3_open(const char *cfg) {
    (void)cfg;
    const char *ep = getenv("OPENCHIME_S3_ENDPOINT");
    const char *bucket = getenv("OPENCHIME_S3_BUCKET");
    const char *ak = getenv("OPENCHIME_S3_ACCESS_KEY");
    const char *sk = getenv("OPENCHIME_S3_SECRET_KEY");
    if (!ep || !*ep || !bucket || !*bucket || !ak || !*ak || !sk || !*sk) return NULL;

    s3_store *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    if (snprintf(s->endpoint, sizeof s->endpoint, "%s", ep) >= (int)sizeof s->endpoint) { free(s); return NULL; }
    /* Split host:port; default port 80. */
    const char *colon = strrchr(ep, ':');
    if (colon) {
        size_t hn = (size_t)(colon - ep);
        if (hn >= sizeof s->host) { free(s); return NULL; }
        memcpy(s->host, ep, hn); s->host[hn] = '\0';
        snprintf(s->port, sizeof s->port, "%s", colon + 1);
    } else {
        snprintf(s->host, sizeof s->host, "%s", ep);
        snprintf(s->port, sizeof s->port, "80");
    }
    snprintf(s->bucket, sizeof s->bucket, "%s", bucket);
    snprintf(s->region, sizeof s->region, "%s", env_or("OPENCHIME_S3_REGION", "us-east-1"));
    snprintf(s->ak, sizeof s->ak, "%s", ak);
    snprintf(s->sk, sizeof s->sk, "%s", sk);
    return s;
}

static void s3_close(void *store) { free(store); }

/* --- small blocking HTTP helpers ------------------------------------------ */

static int tcp_connect(const s3_store *s) {
    struct addrinfo hints, *res = NULL, *rp;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(s->host, s->port, &hints, &res) != 0) return -1;
    int fd = -1;
    for (rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    if (fd >= 0) {
        /* Bound every blocking op so a stuck/slow endpoint can't hang the caller
         * forever (the I/O is on the net thread until ARCH-69's worker lands). */
        struct timeval tv = { 30, 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    }
    return fd;
}

static int write_all(int fd, const void *buf, size_t len) {
    const char *p = buf; size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, p + sent, len - sent);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

static void iso8601(char amzdate[20], char datestamp[12]) {
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(amzdate, 20, "%Y%m%dT%H%M%SZ", &tm);
    strftime(datestamp, 12, "%Y%m%d", &tm);
}

/* Build + send the request line and signed headers. For PUT, `content_length` is
 * the body size that follows; for GET/DELETE pass -1 (no body). Returns 0 ok. */
static int send_request(const s3_store *s, int fd, const char *method,
                        const char *key, long long content_length) {
    char uri[600];
    if (snprintf(uri, sizeof uri, "/%s/%s", s->bucket, key) >= (int)sizeof uri) return -1;
    char amzdate[20], datestamp[12];
    iso8601(amzdate, datestamp);
    char auth[600];
    if (oc_sigv4_authorization(method, uri, s->endpoint, "UNSIGNED-PAYLOAD",
                               amzdate, datestamp, s->region, "s3",
                               s->ak, s->sk, auth, sizeof auth) != 0) return -1;
    char req[1400];
    int n;
    if (content_length >= 0) {
        n = snprintf(req, sizeof req,
            "%s %s HTTP/1.1\r\nHost: %s\r\nx-amz-content-sha256: UNSIGNED-PAYLOAD\r\n"
            "x-amz-date: %s\r\nAuthorization: %s\r\nContent-Length: %lld\r\n"
            "Connection: close\r\n\r\n",
            method, uri, s->endpoint, amzdate, auth, content_length);
    } else {
        n = snprintf(req, sizeof req,
            "%s %s HTTP/1.1\r\nHost: %s\r\nx-amz-content-sha256: UNSIGNED-PAYLOAD\r\n"
            "x-amz-date: %s\r\nAuthorization: %s\r\nConnection: close\r\n\r\n",
            method, uri, s->endpoint, amzdate, auth);
    }
    if (n < 0 || n >= (int)sizeof req) return -1;
    return write_all(fd, req, (size_t)n);
}

/* Read the response status line + headers. Returns the HTTP status code, fills
 * *content_length (or -1 if absent), and copies any body bytes already read into
 * `spill`. Returns -1 on I/O/parse error. */
static int read_response_head(int fd, long long *content_length,
                              uint8_t *spill, size_t spill_cap, size_t *spill_len) {
    char hdr[8192];
    size_t len = 0;
    const char *term = NULL;
    while (len < sizeof hdr - 1) {
        ssize_t n = read(fd, hdr + len, sizeof hdr - 1 - len);
        if (n <= 0) return -1;
        len += (size_t)n;
        hdr[len] = '\0';
        term = strstr(hdr, "\r\n\r\n");
        if (term) break;
    }
    if (!term) return -1;
    if (strncmp(hdr, "HTTP/1.", 7) != 0) return -1;
    int status = atoi(hdr + 9);

    *content_length = -1;
    for (const char *p = hdr; p && p < term; ) {
        const char *eol = strstr(p, "\r\n");
        if (!eol || eol > term) break;
        if (strncasecmp(p, "Content-Length:", 15) == 0) {
            *content_length = atoll(p + 15);
        }
        p = eol + 2;
    }
    /* Body bytes that arrived with the header block. */
    const char *body = term + 4;
    size_t have = len - (size_t)(body - hdr);
    if (have > spill_cap) have = spill_cap;
    if (have) memcpy(spill, body, have);
    *spill_len = have;
    return status;
}

/* --- backend: upload ------------------------------------------------------ */

static void *s3_put_begin(void *store, const char *key, uint64_t size_hint) {
    s3_store *s = store;
    int fd = tcp_connect(s);
    if (fd < 0) return NULL;
    if (send_request(s, fd, "PUT", key, (long long)size_hint) != 0) { close(fd); return NULL; }
    s3_writer *w = calloc(1, sizeof *w);
    if (!w) { close(fd); return NULL; }
    w->fd = fd;
    w->remaining = size_hint;
    return w;
}

static int s3_put_chunk(void *wv, const void *data, size_t len) {
    s3_writer *w = wv;
    if (!w || w->failed || len == 0) return w && w->failed ? -1 : 0;
    if (len > w->remaining) { w->failed = 1; return -1; }
    if (write_all(w->fd, data, len) != 0) { w->failed = 1; return -1; }
    w->remaining -= len;
    return 0;
}

static int s3_put_commit(void *wv) {
    s3_writer *w = wv;
    int rc = -1;
    if (w && !w->failed && w->remaining == 0) {
        long long cl; uint8_t spill[64]; size_t sl;
        int status = read_response_head(w->fd, &cl, spill, sizeof spill, &sl);
        if (status >= 200 && status < 300) rc = 0;
    }
    if (w) { close(w->fd); free(w); }
    return rc;
}

static void s3_put_abort(void *wv) {
    s3_writer *w = wv;
    if (!w) return;
    close(w->fd);   /* incomplete body -> S3 does not create the object */
    free(w);
}

/* --- backend: download ---------------------------------------------------- */

static void *s3_get_begin(void *store, const char *key, uint64_t *size_out) {
    s3_store *s = store;
    int fd = tcp_connect(s);
    if (fd < 0) return NULL;
    if (send_request(s, fd, "GET", key, -1) != 0) { close(fd); return NULL; }
    s3_reader *r = calloc(1, sizeof *r);
    if (!r) { close(fd); return NULL; }
    r->fd = fd;
    long long cl;
    int status = read_response_head(fd, &cl, r->buf, sizeof r->buf, &r->buf_len);
    if (status < 200 || status >= 300 || cl < 0) { close(fd); free(r); return NULL; }
    r->remaining = (uint64_t)cl;
    if (size_out) *size_out = (uint64_t)cl;
    return r;
}

static long s3_get_chunk(void *rv, void *buf, size_t cap) {
    s3_reader *r = rv;
    if (!r) return -1;
    /* Drain any body bytes captured with the header block first. */
    if (r->buf_off < r->buf_len) {
        size_t avail = r->buf_len - r->buf_off;
        size_t n = avail < cap ? avail : cap;
        memcpy(buf, r->buf + r->buf_off, n);
        r->buf_off += n;
        if (r->remaining >= n) r->remaining -= n;
        return (long)n;
    }
    if (r->remaining == 0) return 0;
    size_t want = cap < r->remaining ? cap : (size_t)r->remaining;
    ssize_t n = read(r->fd, buf, want);
    if (n < 0) return -1;
    if (n == 0) return 0;
    r->remaining -= (uint64_t)n;
    return (long)n;
}

static void s3_get_close(void *rv) {
    s3_reader *r = rv;
    if (!r) return;
    close(r->fd);
    free(r);
}

static int s3_del(void *store, const char *key) {
    s3_store *s = store;
    int fd = tcp_connect(s);
    if (fd < 0) return -1;
    int rc = -1;
    if (send_request(s, fd, "DELETE", key, -1) == 0) {
        long long cl; uint8_t spill[64]; size_t sl;
        int status = read_response_head(fd, &cl, spill, sizeof spill, &sl);
        if ((status >= 200 && status < 300) || status == 404) rc = 0;
    }
    close(fd);
    return rc;
}

const oc_blob_backend oc_blob_backend_s3 = {
    s3_open, s3_close, s3_put_begin, s3_put_chunk, s3_put_commit, s3_put_abort,
    s3_get_begin, s3_get_chunk, s3_get_close, s3_del,
};
