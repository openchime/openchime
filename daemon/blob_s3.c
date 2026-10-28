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
 * transfer. */

#include "blob_backend.h"
#include "config.h"   /* oc_s3_config */
#include "sigv4.h"
#include "tls.h"
#include "sock.h"   /* oc_connect_any */
#include "url.h"    /* oc_url_authority, oc_url_hostheader */

#include <errno.h>
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
    char prefix[256];     /* keys go under it; "" for the bucket's root */
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
            "x-amz-date: %s\r\nAuthorization: %s\r\nContent-Length: %lld\r\n"
            "Connection: close\r\n\r\n",
            method, uri, query[0] ? "?" : "", query, s->endpoint, amzdate, auth, content_length);
    } else {
        n = snprintf(req, sizeof req,
            "%s %s%s%s HTTP/1.1\r\nHost: %s\r\nx-amz-content-sha256: UNSIGNED-PAYLOAD\r\n"
            "x-amz-date: %s\r\nAuthorization: %s\r\nConnection: close\r\n\r\n",
            method, uri, query[0] ? "?" : "", query, s->endpoint, amzdate, auth);
    }
    if (n < 0 || n >= (int)sizeof req) return -1;
    return conn_write(c, req, (size_t)n);
}

static int send_request(const s3_store *s, s3_conn *c, const char *method,
                        const char *key, long long content_length) {
    char uri[600];
    if (snprintf(uri, sizeof uri, "/%s/%s%s%s", s->bucket, s->prefix, s->prefix[0] ? "/" : "", key)
        >= (int)sizeof uri) return -1;
    return send_request_uri(s, c, method, uri, "", content_length);
}

/* Read the response status line + headers. Returns the HTTP status code, fills
 * *content_length (or -1 if absent), and copies any body bytes already read into
 * `spill`. Returns -1 on I/O/parse error. */
static int read_response_head_te(s3_conn *c, long long *content_length, int *chunked,
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
    if (chunked) *chunked = 0;
    for (const char *p = hdr; p && p < term; ) {
        const char *eol = strstr(p, "\r\n");
        if (!eol || eol > term) break;
        if (strncasecmp(p, "Content-Length:", 15) == 0) {
            *content_length = atoll(p + 15);
        } else if (chunked && strncasecmp(p, "Transfer-Encoding:", 18) == 0) {
            for (const char *q = p + 18; q + 7 <= eol; q++)
                if (strncasecmp(q, "chunked", 7) == 0) { *chunked = 1; break; }
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

static int read_response_head(s3_conn *c, long long *content_length,
                              uint8_t *spill, size_t spill_cap, size_t *spill_len) {
    return read_response_head_te(c, content_length, NULL, spill, spill_cap, spill_len);
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
    if (conn_open(&r->c, s) != 0) { free(r); errno = EIO; return NULL; }
    if (send_request(s, &r->c, "GET", key, -1) != 0) {
        conn_close(&r->c); free(r); errno = EIO; return NULL;
    }
    long long cl;
    int status = read_response_head(&r->c, &cl, r->buf, sizeof r->buf, &r->buf_len);
    if (status < 200 || status >= 300 || cl < 0) {
        conn_close(&r->c); free(r);
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

    s3_conn c;
    if (conn_open(&c, s) != 0) return -1;
    int rc = -1;
    char *body = NULL;
    if (send_request_uri(s, &c, "GET", uri, query, -1) == 0) {
        long long cl; int chunked = 0; uint8_t spill[S3_SPILL]; size_t sl = 0;
        int status = read_response_head_te(&c, &cl, &chunked, spill, sizeof spill, &sl);
        size_t blen = 0;
        if (status == 200) body = read_body(&c, cl, chunked, spill, sl, &blen);
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
    conn_close(&c);
    return rc < 0 ? -1 : 0;
}

const oc_blob_backend oc_blob_backend_s3 = {
    s3_open, s3_close, s3_put_begin, s3_put_chunk, s3_put_commit, s3_put_abort,
    s3_get_begin, s3_get_chunk, s3_get_close, s3_del,
};
