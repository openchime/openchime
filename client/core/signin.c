/* Browser sign-in, client side — see signin.h. */

#include "sock.h"       /* first: winsock2 must win the include race */
#include "signin.h"

#include "model.h"      /* oc_model_now_ms: the one clock */
#include "protocol.h"   /* OC_ALPN_HTTP11 */
#include "tls.h"        /* the tunnel's connection to the daemon */

#include <mbedtls/sha256.h>

#ifdef _WIN32
#include <bcrypt.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- randomness, verifier, challenge ------------------------------------- */

static int os_random(uint8_t *out, size_t n) {
#ifdef _WIN32
    return BCryptGenRandom(NULL, out, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0 ? 0 : -1;
#else
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f) return -1;
    size_t r = fread(out, 1, n, f);
    fclose(f);
    return r == n ? 0 : -1;
#endif
}

static const char B64URL[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static size_t b64url(const uint8_t *in, size_t n, char *out) {
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        size_t rem = n - i;
        uint32_t v = (uint32_t)in[i] << 16;
        if (rem > 1) v |= (uint32_t)in[i + 1] << 8;
        if (rem > 2) v |= (uint32_t)in[i + 2];
        out[o++] = B64URL[(v >> 18) & 63];
        out[o++] = B64URL[(v >> 12) & 63];
        if (rem > 1) out[o++] = B64URL[(v >> 6) & 63];
        if (rem > 2) out[o++] = B64URL[v & 63];
    }
    out[o] = '\0';
    return o;
}

void oc_signin_wipe(void *p, size_t n) {
    volatile unsigned char *v = (volatile unsigned char *)p;
    while (n--) *v++ = 0;
}

int oc_signin_verifier(char verifier[OC_SIGNIN_VERIFIER_LEN + 1],
                       char challenge[OC_SIGNIN_CHALLENGE_LEN + 1]) {
    uint8_t raw[32], hash[32];
    if (os_random(raw, sizeof raw) != 0) return -1;
    b64url(raw, sizeof raw, verifier);
    oc_signin_wipe(raw, sizeof raw);
    /* The challenge is over the verifier AS SENT — its base64url text. */
    if (mbedtls_sha256((const unsigned char *)verifier, OC_SIGNIN_VERIFIER_LEN, hash, 0) != 0)
        return -1;
    b64url(hash, sizeof hash, challenge);
    return 0;
}

/* --- the loopback listener ------------------------------------------------ */

struct oc_loopback {
    int  fd;
    unsigned port;
    int  v6;
    char secret[32];
    char path[64];     /* "/cb/<secret>" */
    char tpath[64];    /* "/p/<secret>": the tunnel's pages, when there is one */
    int  tunnel;
    oc_tunnel_target t;
};

/* A test's knob: skip IPv4 loopback, as a host without it would. */
static int g_loopback_v6_only;
void oc_loopback_force_v6(int on) { g_loopback_v6_only = on; }

/* A listening socket on this machine's loopback in `family`, never the wildcard
 * address, and the port it was given. -1 if the family has no loopback. */
static int listen_loopback(int family, unsigned *port) {
    int fd = (int)socket(family, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_storage a;
    memset(&a, 0, sizeof a);
    socklen_t alen;
    if (family == AF_INET6) {
        struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)&a;
        a6->sin6_family = AF_INET6; a6->sin6_addr = in6addr_loopback;
        alen = (socklen_t)sizeof *a6;
    } else {
        struct sockaddr_in *a4 = (struct sockaddr_in *)&a;
        a4->sin_family = AF_INET; a4->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        alen = (socklen_t)sizeof *a4;
    }
    if (bind(fd, (struct sockaddr *)&a, alen) != 0 || listen(fd, 4) != 0 ||
        getsockname(fd, (struct sockaddr *)&a, &alen) != 0) {
        oc_closesock(fd);
        return -1;
    }
    *port = family == AF_INET6 ? ntohs(((struct sockaddr_in6 *)&a)->sin6_port)
                               : ntohs(((struct sockaddr_in *)&a)->sin_port);
    return fd;
}

oc_loopback *oc_loopback_open(char *redirect_uri, size_t cap) {
    oc_sock_startup();
    uint8_t raw[18];
    if (os_random(raw, sizeof raw) != 0) return NULL;

    oc_loopback *lb = calloc(1, sizeof *lb);
    if (!lb) return NULL;
    b64url(raw, sizeof raw, lb->secret);
    /* IPv4 loopback, which every host has; IPv6 loopback on one that does not.
     * The daemon accepts either as a redirect (AUTH.md §8.1). */
    lb->fd = g_loopback_v6_only ? -1 : listen_loopback(AF_INET, &lb->port);
    if (lb->fd < 0) { lb->fd = listen_loopback(AF_INET6, &lb->port); lb->v6 = 1; }
    if (lb->fd < 0) { free(lb); return NULL; }
    oc_sock_setnonblock(lb->fd);
    snprintf(lb->path, sizeof lb->path, "/cb/%s", lb->secret);
    snprintf(lb->tpath, sizeof lb->tpath, "/p/%s", lb->secret);
    if (redirect_uri) {
        int n = snprintf(redirect_uri, cap, lb->v6 ? "http://[::1]:%u%s" : "http://127.0.0.1:%u%s",
                         lb->port, lb->path);
        if (n < 0 || (size_t)n >= cap) { oc_loopback_close(lb); return NULL; }
    }
    return lb;
}

void oc_loopback_close(oc_loopback *lb) {
    if (!lb) return;
    if (lb->fd >= 0) oc_closesock(lb->fd);
    oc_signin_wipe(lb, sizeof *lb);
    free(lb);
}

void oc_loopback_set_tunnel(oc_loopback *lb, const oc_tunnel_target *t) {
    if (!lb || !t) return;
    lb->t = *t;
    lb->tunnel = 1;
}

int oc_loopback_tunnel_base(const oc_loopback *lb, char *out, size_t cap) {
    if (!lb) return -1;
    int n = snprintf(out, cap, lb->v6 ? "http://[::1]:%u%s" : "http://127.0.0.1:%u%s", lb->port, lb->tpath);
    return n < 0 || (size_t)n >= cap ? -1 : 0;
}

static int send_all(int fd, const char *s, size_t n) {
    size_t off = 0;
    while (off < n) {
        int w = (int)send(fd, s + off, (int)(n - off), 0);
        if (w > 0) { off += (size_t)w; continue; }
        if (w < 0 && oc_sock_wouldblock() && oc_poll(fd, 1, 1000) > 0) continue;
        return -1;
    }
    return 0;
}

static const char PAGE_OK[] =
    "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\n"
    "Referrer-Policy: no-referrer\r\nConnection: close\r\n\r\n"
    "<!doctype html><meta charset=utf-8><title>OpenChime</title>"
    "<body style=\"font:16px system-ui,sans-serif;margin:4em auto;max-width:28em\">"
    "<h1 style=\"font-size:1.3em\">You are signed in</h1>"
    "<p>You can close this tab and go back to OpenChime.</p>";
static const char PAGE_404[] =
    "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
static const char PAGE_502[] =
    "HTTP/1.1 502 Bad Gateway\r\nContent-Type: text/plain\r\nContent-Length: 38\r\nConnection: close\r\n\r\n"
    "OpenChime could not reach the server.\n";

/* --- one request from the browser ----------------------------------------- */

#define REQ_MAX (24u * 1024u)   /* head and body: the pages' forms are small */

typedef struct {
    char   buf[REQ_MAX + 1];
    size_t len, head_len, body_len;
    char   method[8], target[2048];
    const char *host;   size_t host_len;
    const char *origin; size_t origin_len;
    const char *ctype;  size_t ctype_len;
} breq;

static int ci_prefix(const char *a, const char *b) {
    for (; *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (x != y) return 0;
    }
    return 1;
}

/* Read one request -- its head, and a body up to Content-Length -- within a
 * few seconds. 0, or -1 for one that is malformed, too large or too slow. */
static int read_request(int fd, breq *r) {
    uint64_t until = oc_model_now_ms() + 5000;
    size_t need = 0;
    r->len = r->head_len = r->body_len = 0;
    for (;;) {
        if (r->head_len && r->len >= need) break;
        uint64_t now = oc_model_now_ms();
        if (now >= until || r->len >= REQ_MAX) return -1;
        if (oc_poll(fd, 0, (int)(until - now)) <= 0) return -1;
        int n = (int)recv(fd, r->buf + r->len, (int)(REQ_MAX - r->len), 0);
        if (n < 0 && oc_sock_wouldblock()) continue;
        if (n <= 0) return -1;
        r->len += (size_t)n;
        r->buf[r->len] = '\0';
        if (!r->head_len) {
            char *end = strstr(r->buf, "\r\n\r\n");
            if (!end) continue;
            r->head_len = (size_t)(end - r->buf) + 4;
            /* The request line. */
            char *sp1 = memchr(r->buf, ' ', r->head_len), *sp2 = sp1 ? memchr(sp1 + 1, ' ', r->head_len) : NULL;
            if (!sp1 || !sp2 || (size_t)(sp1 - r->buf) >= sizeof r->method ||
                (size_t)(sp2 - sp1 - 1) >= sizeof r->target) return -1;
            memcpy(r->method, r->buf, (size_t)(sp1 - r->buf)); r->method[sp1 - r->buf] = '\0';
            memcpy(r->target, sp1 + 1, (size_t)(sp2 - sp1 - 1)); r->target[sp2 - sp1 - 1] = '\0';
            /* The headers that matter; a chunked body is refused. */
            size_t clen = 0;
            for (char *l = strstr(r->buf, "\r\n") + 2; l < r->buf + r->head_len - 2; ) {
                char *e = strstr(l, "\r\n");
                char *colon = memchr(l, ':', (size_t)(e - l));
                if (colon) {
                    const char *v = colon + 1;
                    while (*v == ' ') v++;
                    size_t vl = (size_t)(e - v);
                    if (ci_prefix(l, "host:"))                { if (r->host) return -1; r->host = v; r->host_len = vl; }
                    else if (ci_prefix(l, "origin:"))         { r->origin = v; r->origin_len = vl; }
                    else if (ci_prefix(l, "content-type:"))   { r->ctype = v; r->ctype_len = vl; }
                    else if (ci_prefix(l, "content-length:")) clen = (size_t)strtoul(v, NULL, 10);
                    else if (ci_prefix(l, "transfer-encoding:")) return -1;
                }
                l = e + 2;
            }
            if (clen > REQ_MAX - r->head_len) return -1;
            r->body_len = clen;
            need = r->head_len + clen;
        }
    }
    return 0;
}

/* --- the tunnel (AUTH.md §8.10) --------------------------------------------- */

/* The pages the tunnel carries, and nothing else: it is not a proxy. */
static int tunnel_page(const char *p) {
    static const char *const PAGES[] = { "/signin", "/signup", "/account/password" };
    for (size_t i = 0; i < sizeof PAGES / sizeof PAGES[0]; i++) {
        size_t n = strlen(PAGES[i]);
        if (strncmp(p, PAGES[i], n) == 0 && (p[n] == '\0' || p[n] == '?')) return 1;
    }
    return 0;
}

static int tls_io_wait(int fd, oc_tls_status st, uint64_t until) {
    uint64_t now = oc_model_now_ms();
    if (now >= until) return -1;
    return oc_poll(fd, st == OC_TLS_WANT_WRITE, (int)(until - now)) > 0 ? 0 : -1;
}

/* Carry one request to the daemon and its answer back, over TLS to the very
 * certificate the client's own connection accepted. The page sees the daemon's
 * origin -- its Host, and, for a post from the tunnel's own page, its Origin;
 * a post from anywhere else keeps its own Origin, which the daemon refuses. */
static void tunnel_forward(oc_loopback *lb, int bfd, breq *r, const char *page) {
    const oc_tunnel_target *t = &lb->t;
    char self[64];
    snprintf(self, sizeof self, lb->v6 ? "http://[::1]:%u" : "http://127.0.0.1:%u", lb->port);
    int fd = oc_connect_any(t->host, t->port, 3000);
    if (fd < 0) { send_all(bfd, PAGE_502, sizeof PAGE_502 - 1); return; }
    oc_sock_setnonblock(fd);
    oc_tls_client cli;
    oc_tls_conn conn;
    static const char *alpn[] = { OC_ALPN_HTTP11, NULL };
    int ok = 0;
    char *resp = NULL;
    size_t rl = 0;
    uint64_t until = oc_model_now_ms() + 15000;
    if (oc_tls_client_init_verify(&cli, alpn) != 0) { oc_closesock(fd); send_all(bfd, PAGE_502, sizeof PAGE_502 - 1); return; }
    if (oc_tls_conn_init(&conn, &cli.conf, fd) != 0) goto out_cli;
    oc_tls_conn_defer_verify(&conn);
    oc_tls_conn_set_hostname(&conn, t->name[0] ? t->name : t->host);
    for (;;) {
        oc_tls_status st = oc_tls_handshake(&conn);
        if (st == OC_TLS_OK) break;
        if ((st != OC_TLS_WANT_READ && st != OC_TLS_WANT_WRITE) || tls_io_wait(fd, st, until) != 0) goto out;
    }
    {
        unsigned char fp[32];
        if (oc_tls_peer_fingerprint(&conn, fp) != 0 || memcmp(fp, t->fp, sizeof fp) != 0) goto out;
    }
    {
        char head[4096];
        int own = r->origin && r->origin_len == strlen(self) && memcmp(r->origin, self, r->origin_len) == 0;
        int n = snprintf(head, sizeof head, "%s %s HTTP/1.1\r\nHost: %s\r\n", r->method, page, t->authority);
        if (n > 0 && r->origin && (size_t)n < sizeof head) {
            if (own) n += snprintf(head + n, sizeof head - (size_t)n, "Origin: https://%s\r\n", t->authority);
            else     n += snprintf(head + n, sizeof head - (size_t)n, "Origin: %.*s\r\n", (int)r->origin_len, r->origin);
        }
        if (n > 0 && r->ctype && (size_t)n < sizeof head)
            n += snprintf(head + n, sizeof head - (size_t)n, "Content-Type: %.*s\r\n", (int)r->ctype_len, r->ctype);
        if (n > 0 && (size_t)n < sizeof head)
            n += snprintf(head + n, sizeof head - (size_t)n, "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                          r->body_len);
        if (n <= 0 || (size_t)n >= sizeof head) goto out;
        const char *parts[2] = { head, r->buf + r->head_len };
        size_t lens[2] = { (size_t)n, r->body_len };
        for (int k = 0; k < 2; k++) {
            size_t off = 0;
            while (off < lens[k]) {
                size_t w = 0;
                oc_tls_status st = oc_tls_write(&conn, parts[k] + off, lens[k] - off, &w);
                off += w;
                if (st == OC_TLS_OK) continue;
                if ((st != OC_TLS_WANT_READ && st != OC_TLS_WANT_WRITE) || tls_io_wait(fd, st, until) != 0) goto out;
            }
        }
        oc_signin_wipe(head, sizeof head);
    }
    /* The answer, to the daemon's close. */
    size_t cap = 64 * 1024;
    if (!(resp = malloc(cap + 1))) goto out;
    for (;;) {
        if (rl >= cap) goto out;
        size_t got = 0;
        oc_tls_status st = oc_tls_read(&conn, resp + rl, cap - rl, &got);
        rl += got;
        if (st == OC_TLS_OK && got) continue;
        if (st == OC_TLS_WANT_READ || st == OC_TLS_WANT_WRITE) { if (tls_io_wait(fd, st, until) != 0) goto out; continue; }
        break;
    }
    resp[rl] = '\0';
    ok = rl > 12 && strncmp(resp, "HTTP/1.1 ", 9) == 0;
    if (ok) {
        /* A Location on the daemon's own pages is written back into the tunnel. */
        char *hend = strstr(resp, "\r\n\r\n");
        char *loc = hend ? strstr(resp, "\r\nLocation: /") : NULL;
        if (loc && loc < hend) {
            char *at = loc + strlen("\r\nLocation: ");
            send_all(bfd, resp, (size_t)(at - resp));
            send_all(bfd, lb->tpath, strlen(lb->tpath));
            send_all(bfd, at, rl - (size_t)(at - resp));
        } else {
            send_all(bfd, resp, rl);
        }
    }
out:
    oc_tls_conn_free(&conn);
out_cli:
    oc_tls_client_free(&cli);
    oc_closesock(fd);
    if (resp) { oc_signin_wipe(resp, rl); free(resp); }
    if (!ok) send_all(bfd, PAGE_502, sizeof PAGE_502 - 1);
}

/* One browser connection: the callback (1, with its query), a tunnelled page
 * (0), or anything else, answered 404 (0). -1 if the callback's query does not
 * fit. The request must name this listener as its Host -- a page another site
 * points at 127.0.0.1 under its own name (DNS rebinding) is not ours. */
static int serve_one(oc_loopback *lb, int fd, char *query, size_t qcap) {
    breq *r = calloc(1, sizeof *r);
    if (!r) return 0;
    int ours = 0;
    if (read_request(fd, r) != 0) { send_all(fd, PAGE_404, sizeof PAGE_404 - 1); goto out; }
    char self[48];
    snprintf(self, sizeof self, lb->v6 ? "[::1]:%u" : "127.0.0.1:%u", lb->port);
    int host_ok = r->host && r->host_len == strlen(self) && memcmp(r->host, self, r->host_len) == 0;
    size_t pl = strlen(lb->path), tl = strlen(lb->tpath);
    if (strcmp(r->method, "GET") == 0 && strncmp(r->target, lb->path, pl) == 0 &&
        (r->target[pl] == '\0' || r->target[pl] == '?')) {
        const char *q = r->target[pl] == '?' ? r->target + pl + 1 : "";
        if (strlen(q) >= qcap) { send_all(fd, PAGE_404, sizeof PAGE_404 - 1); ours = -1; goto out; }
        memcpy(query, q, strlen(q) + 1);
        send_all(fd, PAGE_OK, sizeof PAGE_OK - 1);
        ours = 1;
    } else if (lb->tunnel && host_ok && (strcmp(r->method, "GET") == 0 || strcmp(r->method, "POST") == 0) &&
               strncmp(r->target, lb->tpath, tl) == 0 && tunnel_page(r->target + tl)) {
        tunnel_forward(lb, fd, r, r->target + tl);
    } else {
        send_all(fd, PAGE_404, sizeof PAGE_404 - 1);
    }
out:
    oc_signin_wipe(r, sizeof *r);
    free(r);
    return ours;
}

static oc_loopback_result serve(oc_loopback *lb, int timeout_ms, const atomic_int *cancel,
                                char *query, size_t qcap, int want_callback) {
    uint64_t until = oc_model_now_ms() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0);
    char scratch[8];
    for (;;) {
        if (cancel && *cancel) return OC_LOOPBACK_CANCELLED;
        uint64_t now = oc_model_now_ms();
        if (now >= until) return OC_LOOPBACK_TIMEOUT;
        uint64_t left = until - now;
        int pr = oc_poll(lb->fd, 0, left > 200 ? 200 : (int)left);   /* short, to see `cancel` */
        if (pr < 0 && !oc_sock_wouldblock()) return OC_LOOPBACK_ERROR;
        if (pr <= 0) continue;
        int c = (int)accept(lb->fd, NULL, NULL);
        if (c < 0) continue;
        oc_sock_setnonblock(c);
        int ours = want_callback ? serve_one(lb, c, query, qcap) : serve_one(lb, c, scratch, sizeof scratch);
        oc_closesock(c);
        if (want_callback && ours == 1) return OC_LOOPBACK_OK;
        if (want_callback && ours < 0) return OC_LOOPBACK_ERROR;
    }
}

oc_loopback_result oc_loopback_wait(oc_loopback *lb, int timeout_ms, const atomic_int *cancel,
                                    char *query, size_t qcap) {
    if (!lb || !query || qcap == 0) return OC_LOOPBACK_ERROR;
    query[0] = '\0';
    return serve(lb, timeout_ms, cancel, query, qcap, 1);
}

oc_loopback_result oc_loopback_serve(oc_loopback *lb, int timeout_ms, const atomic_int *cancel) {
    if (!lb) return OC_LOOPBACK_ERROR;
    return serve(lb, timeout_ms, cancel, NULL, 0, 0);
}
