/* S3 backend for the oc_blobstore seam (ARCH-70). Speaks the S3 REST API
 * (path-style `/<bucket>/<key>`), signing each request with SigV4
 * (daemon/sigv4.c) and streaming the body with
 * `x-amz-content-sha256: UNSIGNED-PAYLOAD` so an upload/download need not be
 * buffered whole. Selected by the presence of S3 credentials (blobstore.c);
 * configured via OPENCHIME_S3_ENDPOINT, _BUCKET, _ACCESS_KEY, _SECRET_KEY,
 * _REGION.
 *
 * **Transport.** HTTPS by default, with real CA-chain + hostname verification
 * (oc_tls_client_init_ca) — every public S3 provider is HTTPS-only, and sending
 * SigV4 credentials and attachment bytes over an unauthenticated channel would
 * be trivially interceptable. Note this is the *opposite* trust model from
 * ARCH-10: there the client pins a daemon's self-signed cert, whereas here the
 * daemon is an ordinary client of someone else's public service, with no
 * fingerprint to pin and a provider that rotates certs freely. Plaintext is
 * still reachable with an explicit `http://` scheme or a non-443 port, for a
 * local S3 implementation on a private network.
 *
 * NOTE: this backend does blocking network I/O; the daemon currently drives it
 * from the net thread, which is fine for the local-FS default but means a slow
 * S3 endpoint can stall the event loop. Moving blob I/O to a dedicated transfer
 * worker (ARCH-69) is the prerequisite for S3 under concurrent load. */

#include "blob_backend.h"
#include "sigv4.h"
#include "tls.h"

#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    char endpoint[256];   /* host[:port], as sent in the Host header */
    char host[256];       /* host only, for connect() + SNI/cert check */
    char port[16];
    char bucket[256];
    char region[64];
    char ak[256];
    char sk[256];
    int  use_tls;
    oc_tls_client tls;    /* valid iff use_tls; CA-verifying config */
} s3_store;

/* One request's transport: a socket, optionally wrapped in TLS. */
typedef struct {
    int           fd;
    int           tls;
    oc_tls_conn   conn;
} s3_conn;

typedef struct {
    s3_conn  c;
    uint64_t remaining;   /* body bytes still to send */
    int      failed;
} s3_writer;

/* Spill must be >= read_response_head's header buffer, so all body bytes that
 * arrive alongside the header block are kept, never truncated. */
#define S3_SPILL 8192
typedef struct {
    s3_conn  c;
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
    /* Scheme, if given, decides the transport outright; otherwise the port does,
     * and a bare host defaults to HTTPS/443 because every public provider is
     * HTTPS-only. A local S3 implementation on a private network is reached with
     * an explicit http:// or a non-443 port. */
    int explicit_scheme = 0;
    if (strncmp(ep, "https://", 8) == 0) { s->use_tls = 1; explicit_scheme = 1; ep += 8; }
    else if (strncmp(ep, "http://", 7) == 0) { s->use_tls = 0; explicit_scheme = 1; ep += 7; }

    if (snprintf(s->endpoint, sizeof s->endpoint, "%s", ep) >= (int)sizeof s->endpoint) { free(s); return NULL; }
    /* Trim any trailing path — the endpoint is an authority, not a URL. */
    char *slash = strchr(s->endpoint, '/');
    if (slash) *slash = '\0';

    const char *colon = strrchr(s->endpoint, ':');
    if (colon) {
        size_t hn = (size_t)(colon - s->endpoint);
        if (hn >= sizeof s->host) { free(s); return NULL; }
        memcpy(s->host, s->endpoint, hn); s->host[hn] = '\0';
        snprintf(s->port, sizeof s->port, "%s", colon + 1);
        if (!explicit_scheme) s->use_tls = (strcmp(s->port, "443") == 0);
    } else {
        snprintf(s->host, sizeof s->host, "%s", s->endpoint);
        if (!explicit_scheme) s->use_tls = 1;
        snprintf(s->port, sizeof s->port, "%s", s->use_tls ? "443" : "80");
    }

    if (s->use_tls && oc_tls_client_init_ca(&s->tls, getenv("OPENCHIME_S3_CA_BUNDLE")) != 0) {
        fprintf(stderr, "openchimed: S3 endpoint is HTTPS but no CA bundle could be "
                        "loaded; install ca-certificates or set OPENCHIME_S3_CA_BUNDLE\n");
        free(s);
        return NULL;
    }
    snprintf(s->bucket, sizeof s->bucket, "%s", bucket);
    snprintf(s->region, sizeof s->region, "%s", env_or("OPENCHIME_S3_REGION", "us-east-1"));
    snprintf(s->ak, sizeof s->ak, "%s", ak);
    snprintf(s->sk, sizeof s->sk, "%s", sk);
    return s;
}

static void s3_close(void *store) {
    s3_store *s = store;
    if (!s) return;
    if (s->use_tls) oc_tls_client_free(&s->tls);
    free(s);
}

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

/* --- transport: plaintext fd, or TLS over it ------------------------------
 * The socket is blocking with send/recv timeouts (tcp_connect), so mbedTLS
 * normally completes each operation in one call; WANT_READ/WANT_WRITE are still
 * looped on because a renegotiation or a partial record can produce them. A
 * timeout surfaces as an error and fails the transfer rather than spinning. */

static int conn_open(s3_conn *c, s3_store *s) {
    memset(c, 0, sizeof *c);
    c->fd = tcp_connect(s);
    if (c->fd < 0) return -1;
    if (!s->use_tls) return 0;

    if (oc_tls_conn_init(&c->conn, &s->tls.conf, c->fd) != 0) { close(c->fd); return -1; }
    /* Without the hostname there is no SNI and, more importantly, no name check
     * against the certificate — a valid cert for any other host would pass. */
    if (oc_tls_conn_set_hostname(&c->conn, s->host) != 0) {
        oc_tls_conn_free(&c->conn); close(c->fd); return -1;
    }
    for (;;) {
        oc_tls_status st = oc_tls_handshake(&c->conn);
        if (st == OC_TLS_OK) break;
        if (st == OC_TLS_WANT_READ || st == OC_TLS_WANT_WRITE) continue;
        fprintf(stderr, "openchimed: S3 TLS handshake failed for %s "
                        "(certificate not trusted, or name mismatch)\n", s->host);
        oc_tls_conn_free(&c->conn); close(c->fd);
        return -1;
    }
    c->tls = 1;
    return 0;
}

static void conn_close(s3_conn *c) {
    if (!c) return;
    if (c->tls) oc_tls_conn_free(&c->conn);
    if (c->fd >= 0) close(c->fd);
    c->fd = -1; c->tls = 0;
}

static int conn_write(s3_conn *c, const void *buf, size_t len) {
    const char *p = buf; size_t sent = 0;
    while (sent < len) {
        if (c->tls) {
            size_t n = 0;
            oc_tls_status st = oc_tls_write(&c->conn, p + sent, len - sent, &n);
            if (st == OC_TLS_WANT_READ || st == OC_TLS_WANT_WRITE) continue;
            if (st != OC_TLS_OK || n == 0) return -1;
            sent += n;
        } else {
            ssize_t n = write(c->fd, p + sent, len - sent);
            if (n <= 0) return -1;
            sent += (size_t)n;
        }
    }
    return 0;
}

/* Returns bytes read (>0), 0 on clean EOF, <0 on error. */
static long conn_read(s3_conn *c, void *buf, size_t cap) {
    if (c->tls) {
        for (;;) {
            size_t n = 0;
            oc_tls_status st = oc_tls_read(&c->conn, buf, cap, &n);
            if (st == OC_TLS_WANT_READ || st == OC_TLS_WANT_WRITE) continue;
            if (st == OC_TLS_CLOSED) return 0;
            if (st != OC_TLS_OK) return -1;
            return (long)n;
        }
    }
    ssize_t n = read(c->fd, buf, cap);
    return (n < 0) ? -1 : (long)n;
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
static int send_request(const s3_store *s, s3_conn *c, const char *method,
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
    return conn_write(c, req, (size_t)n);
}

/* Read the response status line + headers. Returns the HTTP status code, fills
 * *content_length (or -1 if absent), and copies any body bytes already read into
 * `spill`. Returns -1 on I/O/parse error. */
static int read_response_head(s3_conn *c, long long *content_length,
                              uint8_t *spill, size_t spill_cap, size_t *spill_len) {
    char hdr[8192];
    size_t len = 0;
    const char *term = NULL;
    while (len < sizeof hdr - 1) {
        long n = conn_read(c, hdr + len, sizeof hdr - 1 - len);
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
    s3_writer *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    if (conn_open(&w->c, s) != 0) { free(w); return NULL; }
    if (send_request(s, &w->c, "PUT", key, (long long)size_hint) != 0) {
        conn_close(&w->c); free(w); return NULL;
    }
    w->remaining = size_hint;
    return w;
}

static int s3_put_chunk(void *wv, const void *data, size_t len) {
    s3_writer *w = wv;
    if (!w || w->failed || len == 0) return w && w->failed ? -1 : 0;
    if (len > w->remaining) { w->failed = 1; return -1; }
    if (conn_write(&w->c, data, len) != 0) { w->failed = 1; return -1; }
    w->remaining -= len;
    return 0;
}

static int s3_put_commit(void *wv) {
    s3_writer *w = wv;
    int rc = -1;
    if (w && !w->failed && w->remaining == 0) {
        long long cl; uint8_t spill[64]; size_t sl;
        int status = read_response_head(&w->c, &cl, spill, sizeof spill, &sl);
        if (status >= 200 && status < 300) rc = 0;
    }
    if (w) { conn_close(&w->c); free(w); }
    return rc;
}

static void s3_put_abort(void *wv) {
    s3_writer *w = wv;
    if (!w) return;
    conn_close(&w->c);   /* incomplete body -> S3 does not create the object */
    free(w);
}

/* --- backend: download ---------------------------------------------------- */

static void *s3_get_begin(void *store, const char *key, uint64_t *size_out) {
    s3_store *s = store;
    s3_reader *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    if (conn_open(&r->c, s) != 0) { free(r); return NULL; }
    if (send_request(s, &r->c, "GET", key, -1) != 0) {
        conn_close(&r->c); free(r); return NULL;
    }
    long long cl;
    int status = read_response_head(&r->c, &cl, r->buf, sizeof r->buf, &r->buf_len);
    if (status < 200 || status >= 300 || cl < 0) { conn_close(&r->c); free(r); return NULL; }
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
    long n = conn_read(&r->c, buf, want);
    if (n < 0) return -1;
    if (n == 0) return 0;
    r->remaining -= (uint64_t)n;
    return (long)n;
}

static void s3_get_close(void *rv) {
    s3_reader *r = rv;
    if (!r) return;
    conn_close(&r->c);
    free(r);
}

static int s3_del(void *store, const char *key) {
    s3_store *s = store;
    s3_conn c;
    if (conn_open(&c, s) != 0) return -1;
    int rc = -1;
    if (send_request(s, &c, "DELETE", key, -1) == 0) {
        long long cl; uint8_t spill[64]; size_t sl;
        int status = read_response_head(&c, &cl, spill, sizeof spill, &sl);
        if ((status >= 200 && status < 300) || status == 404) rc = 0;
    }
    conn_close(&c);
    return rc;
}

const oc_blob_backend oc_blob_backend_s3 = {
    s3_open, s3_close, s3_put_begin, s3_put_chunk, s3_put_commit, s3_put_abort,
    s3_get_begin, s3_get_chunk, s3_get_close, s3_del,
};
