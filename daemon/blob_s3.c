/* S3 backend for the oc_blobstore seam (ARCH-70). Speaks the S3 REST API
 * (path-style `/<bucket>/[<prefix>/]<key>`), signing each request with SigV4
 * (daemon/sigv4.c) and streaming the body with
 * `x-amz-content-sha256: UNSIGNED-PAYLOAD` so an upload/download need not be
 * buffered whole. Selected by the presence of S3 credentials (blobstore.c);
 * configured via OPENCHIME_S3 (config.h: endpoint, bucket, access key, secret key,
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
 * This backend does blocking network I/O, and runs where every blob operation
 * runs: on the transfer workers (daemon/xferpool.c, ARCH-69) -- opens, reads,
 * writes, commits and deletes alike -- so a slow endpoint holds a worker, never
 * the event loop. The blob store counts any operation made on the loop's thread
 * (oc_blobstore_loop_ops), and a test holds that count to zero through a slow
 * transfer.
 *
 * **Connections are kept.** A request takes an idle connection to the store when
 * there is one and opens one only when there is not; after a response read to
 * its end that did not close the connection, it goes back. So the uploader and
 * each transfer worker pay one handshake for a run of requests, not one each.
 * The idle set is small (S3_IDLE_MAX), shared under a mutex, and freed with the
 * store. A kept connection the server has since closed is found before use, and
 * one that fails before any byte of its response is retried once on a fresh
 * connection: a stale connection is not a store failure, so the caller (and the
 * uploader's backoff) never sees it. */

#include "blob_backend.h"
#include "config.h"   /* oc_s3_config */
#include "sigv4.h"
#include "tls.h"
#include "sock.h"   /* oc_connect_any */
#include "url.h"    /* oc_url_authority, oc_url_hostheader */

#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* One request's transport: a socket, optionally wrapped in TLS. On the heap,
 * and never copied: the TLS context points at its own fd and at itself. */
typedef struct {
    int           fd;
    int           tls;
    int           reused;   /* taken from the idle set, not opened for this request */
    oc_tls_conn   conn;
} s3_conn;

#define S3_IDLE_MAX 4       /* idle connections kept per store */

typedef struct {
    char endpoint[256];   /* host[:port], as sent in the Host header */
    char host[256];       /* host only, for connect() + SNI/cert check */
    char port[16];
    char bucket[256];
    char prefix[256];     /* keys go under it; "" for the bucket's root */
    char region[64];
    char ak[256];
    char sk[256];
    int  use_tls;
    oc_tls_client tls;    /* valid iff use_tls; CA-verifying config */
    pthread_mutex_t idle_mu;          /* guards idle[] and n_idle */
    s3_conn *idle[S3_IDLE_MAX];       /* connections between requests */
    int     n_idle;
} s3_store;


typedef struct {
    s3_store *s;
    s3_conn  *c;
    uint64_t remaining;   /* body bytes still to send */
    int      failed;
} s3_writer;

/* Spill must be >= read_response_head's header buffer, so all body bytes that
 * arrive alongside the header block are kept, never truncated. */
#define S3_SPILL 8192
typedef struct {
    s3_store *s;
    s3_conn  *c;
    int      keep;        /* the response left the connection open */
    uint64_t remaining;   /* body bytes still to read */
    uint8_t  buf[S3_SPILL]; /* body bytes read while parsing headers */
    size_t   buf_off, buf_len;
} s3_reader;

static void conn_close(s3_conn *c);

static void *s3_open(const char *base_dir, const void *backend_cfg) {
    (void)base_dir;
    const oc_s3_config *c3 = backend_cfg;
    if (!c3) return NULL;
    const char *ep = c3->endpoint, *bucket = c3->bucket, *ak = c3->access_key, *sk = c3->secret_key;
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

    /* host, host:port, [v6] or [v6]:port (url.h). The endpoint keeps its
     * brackets: it is the Host header and what SigV4 signs. */
    s->port[0] = '\0';
    if (oc_url_authority(s->endpoint, strlen(s->endpoint), s->host, sizeof s->host,
                         s->port, sizeof s->port) != 0) { free(s); return NULL; }
    if (s->port[0]) {
        if (!explicit_scheme) s->use_tls = (strcmp(s->port, "443") == 0);
    } else {
        if (!explicit_scheme) s->use_tls = 1;
        snprintf(s->port, sizeof s->port, "%s", s->use_tls ? "443" : "80");
    }

    if (s->use_tls && oc_tls_client_init_ca(&s->tls) != 0) {
        fprintf(stderr, "openchimed: S3 endpoint is HTTPS but its TLS client could not be set up\n");
        free(s);
        return NULL;
    }
    snprintf(s->bucket, sizeof s->bucket, "%s", bucket);
    if (c3->prefix && snprintf(s->prefix, sizeof s->prefix, "%s", c3->prefix) >= (int)sizeof s->prefix) {
        if (s->use_tls) oc_tls_client_free(&s->tls);
        free(s); return NULL;
    }
    snprintf(s->region, sizeof s->region, "%s", c3->region && *c3->region ? c3->region : "us-east-1");
    snprintf(s->ak, sizeof s->ak, "%s", ak);
    snprintf(s->sk, sizeof s->sk, "%s", sk);
    pthread_mutex_init(&s->idle_mu, NULL);
    return s;
}

static void s3_close(void *store) {
    s3_store *s = store;
    if (!s) return;
    for (int i = 0; i < s->n_idle; i++) conn_close(s->idle[i]);
    pthread_mutex_destroy(&s->idle_mu);
    if (s->use_tls) oc_tls_client_free(&s->tls);
    free(s);
}

/* --- small blocking HTTP helpers ------------------------------------------ */

static int tcp_connect(const s3_store *s) {
    /* Each address bounded (sock.h): a dead IPv6 route costs seconds, not minutes. */
    int fd = oc_connect_any(s->host, atoi(s->port), OC_CONNECT_PER_ADDR_MS);
    if (fd >= 0) {
        /* Bound every blocking op so a stuck/slow endpoint can't hang the
         * worker or the uploader that called it forever. */
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

/* A new connection to the store, or NULL. */
static s3_conn *conn_open(s3_store *s) {
    s3_conn *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->fd = tcp_connect(s);
    if (c->fd < 0) { free(c); return NULL; }
    if (!s->use_tls) return c;

    if (oc_tls_conn_init(&c->conn, &s->tls.conf, c->fd) != 0) { close(c->fd); free(c); return NULL; }
    /* Without the hostname there is no SNI and, more importantly, no name check
     * against the certificate — a valid cert for any other host would pass. */
    if (oc_tls_conn_set_hostname(&c->conn, s->host) != 0) {
        oc_tls_conn_free(&c->conn); close(c->fd); free(c); return NULL;
    }
    for (;;) {
        oc_tls_status st = oc_tls_handshake(&c->conn);
        if (st == OC_TLS_OK) break;
        if (st == OC_TLS_WANT_READ || st == OC_TLS_WANT_WRITE) continue;
        fprintf(stderr, "openchimed: S3 TLS handshake failed for %s "
                        "(certificate not trusted, or name mismatch)\n", s->host);
        oc_tls_conn_free(&c->conn); close(c->fd); free(c);
        return NULL;
    }
    c->tls = 1;
    return c;
}

static void conn_close(s3_conn *c) {
    if (!c) return;
    if (c->tls) oc_tls_conn_free(&c->conn);
    if (c->fd >= 0) close(c->fd);
    free(c);
}

/* An idle connection is fit to send on when nothing is waiting to be read on it:
 * between responses the server has nothing to say, so readable means it closed
 * (EOF) or sent what nobody asked for. Either way it goes. */
static int conn_alive(const s3_conn *c) {
    struct pollfd p = { c->fd, POLLIN, 0 };
    return poll(&p, 1, 0) == 0;
}

/* A connection for one request: a kept one when there is one still alive,
 * otherwise a new one. 0, or -1 when none could be opened. */
static s3_conn *conn_take(s3_store *s) {
    for (;;) {
        s3_conn *c = NULL;
        pthread_mutex_lock(&s->idle_mu);
        if (s->n_idle > 0) c = s->idle[--s->n_idle];
        pthread_mutex_unlock(&s->idle_mu);
        if (!c) break;
        if (conn_alive(c)) { c->reused = 1; return c; }
        conn_close(c);
    }
    return conn_open(s);   /* reused = 0 */
}

/* After a request: back to the idle set when `reusable` (the response said
 * nothing of closing, and its body was read to the end) and there is room,
 * closed otherwise. */
static void conn_give(s3_store *s, s3_conn *c, int reusable) {
    if (!c) return;
    if (reusable) {
        pthread_mutex_lock(&s->idle_mu);
        if (s->n_idle < S3_IDLE_MAX) { s->idle[s->n_idle++] = c; c = NULL; }
        pthread_mutex_unlock(&s->idle_mu);
    }
    conn_close(c);
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
static int send_request_uri(const s3_store *s, s3_conn *c, const char *method,
                            const char *uri, const char *query, long long content_length) {
    char amzdate[20], datestamp[12];
    iso8601(amzdate, datestamp);
    char auth[600];
    if (oc_sigv4_authorization_query(method, uri, query, s->endpoint, "UNSIGNED-PAYLOAD",
                                     amzdate, datestamp, s->region, "s3",
                                     s->ak, s->sk, auth, sizeof auth) != 0) return -1;
    char req[2400];
    int n;
    if (content_length >= 0) {
        n = snprintf(req, sizeof req,
            "%s %s%s%s HTTP/1.1\r\nHost: %s\r\nx-amz-content-sha256: UNSIGNED-PAYLOAD\r\n"
            "x-amz-date: %s\r\nAuthorization: %s\r\nContent-Length: %lld\r\n\r\n",
            method, uri, query[0] ? "?" : "", query, s->endpoint, amzdate, auth, content_length);
    } else {
        n = snprintf(req, sizeof req,
            "%s %s%s%s HTTP/1.1\r\nHost: %s\r\nx-amz-content-sha256: UNSIGNED-PAYLOAD\r\n"
            "x-amz-date: %s\r\nAuthorization: %s\r\n\r\n",
            method, uri, query[0] ? "?" : "", query, s->endpoint, amzdate, auth);
    }
    if (n < 0 || n >= (int)sizeof req) return -1;
    return conn_write(c, req, (size_t)n);
}


/* Read the response status line + headers. Returns the HTTP status code, fills
 * *content_length (or -1 if absent), and copies any body bytes already read into
 * `spill`. *keep (when given) says whether the connection may carry another
 * request: HTTP/1.1 without `Connection: close`. Returns -1 on I/O/parse error,
 * and -2 when not one byte of a response arrived -- the case a kept connection
 * the server has dropped produces, and the one worth a retry. */
static int read_response_head_te(s3_conn *c, long long *content_length, int *chunked,
                                 int *keep, uint8_t *spill, size_t spill_cap, size_t *spill_len) {
    char hdr[8192];
    size_t len = 0;
    const char *term = NULL;
    if (keep) *keep = 0;
    while (len < sizeof hdr - 1) {
        long n = conn_read(c, hdr + len, sizeof hdr - 1 - len);
        if (n <= 0) return len == 0 ? -2 : -1;
        len += (size_t)n;
        hdr[len] = '\0';
        term = strstr(hdr, "\r\n\r\n");
        if (term) break;
    }
    if (!term) return -1;
    if (strncmp(hdr, "HTTP/1.", 7) != 0) return -1;
    int status = atoi(hdr + 9);
    int k = strncmp(hdr, "HTTP/1.1", 8) == 0;

    *content_length = -1;
    if (chunked) *chunked = 0;
    for (const char *p = hdr; p && p < term; ) {
        const char *eol = strstr(p, "\r\n");
        if (!eol || eol > term) break;
        if (strncasecmp(p, "Content-Length:", 15) == 0) {
            *content_length = atoll(p + 15);
        } else if (chunked && strncasecmp(p, "Transfer-Encoding:", 18) == 0) {
            for (const char *q = p + 18; q + 7 <= eol; q++)
                if (strncasecmp(q, "chunked", 7) == 0) { *chunked = 1; break; }
        } else if (strncasecmp(p, "Connection:", 11) == 0) {
            for (const char *q = p + 11; q + 5 <= eol; q++)
                if (strncasecmp(q, "close", 5) == 0) { k = 0; break; }
        }
        p = eol + 2;
    }
    /* Body bytes that arrived with the header block. */
    const char *body = term + 4;
    size_t have = len - (size_t)(body - hdr);
    if (have > spill_cap) have = spill_cap;
    if (have) memcpy(spill, body, have);
    *spill_len = have;
    /* More than fitted in the spill is a body nobody will finish reading. */
    if ((size_t)(len - (size_t)(body - hdr)) > spill_cap) k = 0;
    if (keep) *keep = k;
    return status;
}

/* Read and discard the rest of a body of `cl` bytes, `have` of which came with
 * the head: 1 when it ended exactly there, so the connection can carry the next
 * request; 0 when its length is unknown, it is large, or it ran over. */
#define S3_DRAIN_MAX (64 * 1024)
static int drain_body(s3_conn *c, long long cl, size_t have) {
    if (cl < 0 || (size_t)cl < have || cl > S3_DRAIN_MAX) return 0;
    size_t left = (size_t)cl - have;
    char sink[4096];
    while (left > 0) {
        long n = conn_read(c, sink, left < sizeof sink ? left : sizeof sink);
        if (n <= 0) return 0;
        left -= (size_t)n;
    }
    return 1;
}

/* Take a connection, send the request and read the response head. A kept
 * connection that fails before any byte of the response comes back (a write
 * error, or the server closed it while it waited) is closed and the request
 * sent once more on a new one; anything after that stands. The status, or -1
 * with the connection closed. */
static int request_head(s3_store *s, s3_conn **cp, const char *method, const char *uri,
                        const char *query, long long *cl, int *chunked, int *keep,
                        uint8_t *spill, size_t spill_cap, size_t *spill_len) {
    *cp = NULL;
    for (int attempt = 0; attempt < 2; attempt++) {
        s3_conn *c = attempt == 0 ? conn_take(s) : conn_open(s);
        if (!c) return -1;
        int st = send_request_uri(s, c, method, uri, query, -1) == 0
               ? read_response_head_te(c, cl, chunked, keep, spill, spill_cap, spill_len) : -2;
        if (st >= 0) { *cp = c; return st; }
        int retry = c->reused && st == -2;
        conn_close(c);
        if (!retry) return -1;
    }
    return -1;
}

/* The path-style URI of `key` in the store. */
static int key_uri(const s3_store *s, const char *key, char *uri, size_t cap) {
    return snprintf(uri, cap, "/%s/%s%s%s", s->bucket, s->prefix, s->prefix[0] ? "/" : "", key)
           >= (int)cap ? -1 : 0;
}

/* --- backend: upload ------------------------------------------------------ */

static void *s3_put_begin(void *store, const char *key, uint64_t size_hint) {
    s3_store *s = store;
    char uri[600];
    if (key_uri(s, key, uri, sizeof uri) != 0) return NULL;
    s3_writer *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    w->s = s;
    /* The body streams after this, from the caller, and cannot be sent twice:
     * the retry a kept connection gets is here, on the request head. */
    if (!(w->c = conn_take(s))) { free(w); return NULL; }
    if (send_request_uri(s, w->c, "PUT", uri, "", (long long)size_hint) != 0) {
        int retry = w->c->reused;
        conn_close(w->c);
        w->c = retry ? conn_open(s) : NULL;
        if (!w->c || send_request_uri(s, w->c, "PUT", uri, "", (long long)size_hint) != 0) {
            conn_close(w->c); free(w); return NULL;
        }
    }
    w->remaining = size_hint;
    return w;
}

/* A kept connection that failed before any byte of the response came back --
 * a write error while the body streamed, or nothing at all after it -- says
 * so with errno EAGAIN: the body cannot be sent twice from here, but the
 * caller, which has it, can send it once more on a new connection (that one
 * is fresh, and its failures are reported as they are). */
static int stale_fail(s3_writer *w) {
    w->failed = 1;
    errno = w->c && w->c->reused ? EAGAIN : EIO;
    return -1;
}

static int s3_put_chunk(void *wv, const void *data, size_t len) {
    s3_writer *w = wv;
    if (!w || w->failed || len == 0) return w && w->failed ? -1 : 0;
    if (len > w->remaining) { w->failed = 1; errno = EIO; return -1; }
    if (conn_write(w->c, data, len) != 0) return stale_fail(w);
    w->remaining -= len;
    return 0;
}

static int s3_put_commit(void *wv) {
    s3_writer *w = wv;
    int rc = -1;
    int reusable = 0;
    if (w && !w->failed && w->remaining == 0) {
        long long cl; uint8_t spill[64]; size_t sl; int keep = 0;
        int status = read_response_head_te(w->c, &cl, NULL, &keep, spill, sizeof spill, &sl);
        if (status >= 200 && status < 300) rc = 0;
        else if (status == -2) stale_fail(w);   /* no byte of a response */
        else errno = EIO;
        reusable = status > 0 && keep && drain_body(w->c, cl, sl);
    } else {
        errno = w && w->failed && errno == EAGAIN ? EAGAIN : EIO;
    }
    int keep_errno = errno;
    if (w) { conn_give(w->s, w->c, reusable); free(w); }
    errno = keep_errno;
    return rc;
}

static void s3_put_abort(void *wv) {
    s3_writer *w = wv;
    if (!w) return;
    conn_close(w->c);   /* incomplete body -> S3 does not create the object */
    free(w);
}

/* --- backend: download ---------------------------------------------------- */

static void *s3_get_begin(void *store, const char *key, uint64_t *size_out) {
    s3_store *s = store;
    char uri[600];
    if (key_uri(s, key, uri, sizeof uri) != 0) { errno = EIO; return NULL; }
    s3_reader *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->s = s;
    long long cl;
    int status = request_head(s, &r->c, "GET", uri, "", &cl, NULL, &r->keep,
                              r->buf, sizeof r->buf, &r->buf_len);
    if (status < 0) { free(r); errno = EIO; return NULL; }
    if (status < 200 || status >= 300 || cl < 0) {
        conn_give(s, r->c, r->keep && drain_body(r->c, cl, r->buf_len));
        free(r);
        /* The store said the object is not there: the caller tells that apart
         * from a store it could not reach (blobstore.h). */
        errno = status == 404 ? ENOENT : EIO;
        return NULL;
    }
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
    long n = conn_read(r->c, buf, want);
    if (n < 0) return -1;
    if (n == 0) return 0;
    r->remaining -= (uint64_t)n;
    return (long)n;
}

static void s3_get_close(void *rv) {
    s3_reader *r = rv;
    if (!r) return;
    /* Kept only when the whole body was read: a download abandoned part-way
     * leaves the rest on the wire. */
    conn_give(r->s, r->c, r->keep && r->remaining == 0 && r->buf_off >= r->buf_len);
    free(r);
}

static int s3_del(void *store, const char *key) {
    s3_store *s = store;
    char uri[600];
    if (key_uri(s, key, uri, sizeof uri) != 0) return -1;
    s3_conn *c;
    long long cl; uint8_t spill[64]; size_t sl; int keep = 0;
    int status = request_head(s, &c, "DELETE", uri, "", &cl, NULL, &keep, spill, sizeof spill, &sl);
    if (status < 0) return -1;
    conn_give(s, c, keep && drain_body(c, cl, sl));
    return ((status >= 200 && status < 300) || status == 404) ? 0 : -1;
}

/* --- listing (blob_backend.h) -------------------------------------------- */

/* URI-encode `in` as SigV4 wants a query value: unreserved characters as they
 * are, everything else (the prefix's '/', a token's '+' and '=') as %XX. */
static int uri_encode(const char *in, char *out, size_t cap) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        int keep = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                   (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.' || *p == '~';
        if (o + (keep ? 1 : 3) >= cap) return -1;
        if (keep) out[o++] = (char)*p;
        else { out[o++] = '%'; out[o++] = hex[*p >> 4]; out[o++] = hex[*p & 15]; }
    }
    out[o] = '\0';
    return 0;
}

/* The text between <tag> and </tag> at or after `from`, unescaped of the five
 * XML entities; NULL past `end` or with none. Returns where it ended. */
static const char *xml_take(const char *from, const char *end, const char *tag,
                            char *out, size_t cap) {
    char open[48], close_[48];
    snprintf(open, sizeof open, "<%s>", tag);
    snprintf(close_, sizeof close_, "</%s>", tag);
    const char *a = strstr(from, open);
    if (!a || a >= end) return NULL;
    a += strlen(open);
    const char *b = strstr(a, close_);
    if (!b || b > end) return NULL;
    size_t o = 0;
    for (const char *p = a; p < b && o + 1 < cap; ) {
        static const struct { const char *e; char c; } ents[] = {
            { "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' }, { "&quot;", '"' }, { "&apos;", '\'' } };
        int done = 0;
        if (*p == '&')
            for (size_t i = 0; i < sizeof ents / sizeof ents[0]; i++) {
                size_t l = strlen(ents[i].e);
                if ((size_t)(b - p) >= l && strncmp(p, ents[i].e, l) == 0) { out[o++] = ents[i].c; p += l; done = 1; break; }
            }
        if (!done) out[o++] = *p++;
    }
    out[o] = '\0';
    return b + strlen(close_);
}

/* "2026-10-23T03:55:00.000Z" as milliseconds since the epoch; 0 if unreadable. */
static uint64_t iso_ms(const char *t) {
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    if (sscanf(t, "%4d-%2d-%2dT%2d:%2d:%2d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
               &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6) return 0;
    tm.tm_year -= 1900; tm.tm_mon -= 1;
    time_t v = timegm(&tm);
    return v < 0 ? 0 : (uint64_t)v * 1000;
}

/* The whole body of a response whose head has been read, by its length or in
 * chunks, into a buffer of at most `cap` bytes the caller frees. */
#define S3_LIST_MAX (4u * 1024 * 1024)
static char *read_body(s3_conn *c, long long cl, int chunked,
                       const uint8_t *spill, size_t spill_len, size_t *len_out) {
    size_t cap = 64 * 1024, len = 0;
    char *raw = malloc(cap + 1);
    if (!raw) return NULL;
    memcpy(raw, spill, spill_len);
    len = spill_len;
    for (;;) {
        if (!chunked && cl >= 0 && len >= (size_t)cl) break;
        if (len == cap) {
            if (cap >= S3_LIST_MAX) { free(raw); return NULL; }
            char *n = realloc(raw, cap * 2 + 1);
            if (!n) { free(raw); return NULL; }
            raw = n; cap *= 2;
        }
        long n = conn_read(c, raw + len, cap - len);
        if (n < 0) { free(raw); return NULL; }
        if (n == 0) break;
        len += (size_t)n;
        if (chunked) {
            raw[len] = '\0';
            if (strstr(raw, "\r\n0\r\n") || (len >= 3 && strncmp(raw, "0\r\n", 3) == 0)) break;
        }
    }
    if (!chunked) {
        if (cl >= 0 && len > (size_t)cl) len = (size_t)cl;
        raw[len] = '\0';
        *len_out = len;
        return raw;
    }
    /* De-chunk in place: each chunk is <hex size>\r\n<bytes>\r\n. */
    size_t in = 0, out = 0;
    raw[len] = '\0';
    while (in < len) {
        char *e;
        unsigned long sz = strtoul(raw + in, &e, 16);
        char *crlf = strstr(raw + in, "\r\n");
        if (!crlf || e == raw + in) break;
        in = (size_t)(crlf - raw) + 2;
        if (sz == 0) break;
        if (in + sz > len) sz = len - in;
        memmove(raw + out, raw + in, sz);
        out += sz;
        in += sz + 2;
    }
    raw[out] = '\0';
    *len_out = out;
    return raw;
}

int oc_blob_s3_list(void *store, const char *token, oc_blob_list_fn fn, void *ctx,
                    char *next, size_t next_cap) {
    s3_store *s = store;
    if (next && next_cap) next[0] = '\0';
    if (!s || !fn) return -1;
    /* A listing is of this store's own keys: under the prefix, never the
     * bucket's root (a shared bucket holds other people's). */
    if (!s->prefix[0]) return -1;
    char pfx[300], epfx[900], etok[3200] = "";
    snprintf(pfx, sizeof pfx, "%s/", s->prefix);
    if (uri_encode(pfx, epfx, sizeof epfx) != 0) return -1;
    if (token && *token && uri_encode(token, etok, sizeof etok) != 0) return -1;
    char query[4300];
    if (snprintf(query, sizeof query, "%s%s%slist-type=2&prefix=%s",
                 etok[0] ? "continuation-token=" : "", etok, etok[0] ? "&" : "", epfx)
        >= (int)sizeof query) return -1;
    char uri[300];
    snprintf(uri, sizeof uri, "/%s", s->bucket);

    s3_conn *c;
    int rc = -1, reusable = 0;
    char *body = NULL;
    long long cl; int chunked = 0, keep = 0; uint8_t spill[S3_SPILL]; size_t sl = 0;
    int status = request_head(s, &c, "GET", uri, query, &cl, &chunked, &keep, spill, sizeof spill, &sl);
    if (status < 0) return -1;
    {
        size_t blen = 0;
        if (status == 200) body = read_body(c, cl, chunked, spill, sl, &blen);
        /* A listing by its length, read to exactly that, can carry the next
         * page; a chunked one's end is not tracked to the byte, so it closes. */
        reusable = keep && body && !chunked && cl >= 0 && blen == (size_t)cl;
        if (body) {
            rc = 0;
            const char *end = body + blen, *p = body;
            char key[1100], when[64], size[32];
            size_t plen = strlen(pfx);
            while ((p = strstr(p, "<Contents>")) != NULL) {
                const char *ce = strstr(p, "</Contents>");
                if (!ce) break;
                if (xml_take(p, ce, "Key", key, sizeof key) && strncmp(key, pfx, plen) == 0) {
                    when[0] = size[0] = '\0';
                    xml_take(p, ce, "LastModified", when, sizeof when);
                    xml_take(p, ce, "Size", size, sizeof size);
                    if (fn(ctx, key + plen, strtoull(size, NULL, 10), iso_ms(when)) != 0) { rc = 1; break; }
                }
                p = ce;
            }
            char trunc[16] = "";
            if (rc == 0 && xml_take(body, end, "IsTruncated", trunc, sizeof trunc) &&
                strcmp(trunc, "true") == 0 && next && next_cap)
                if (!xml_take(body, end, "NextContinuationToken", next, next_cap)) rc = -1;
        }
    }
    free(body);
    conn_give(s, c, reusable);
    return rc < 0 ? -1 : 0;
}

const oc_blob_backend oc_blob_backend_s3 = {
    s3_open, s3_close, s3_put_begin, s3_put_chunk, s3_put_commit, s3_put_abort,
    s3_get_begin, s3_get_chunk, s3_get_close, s3_del,
};
