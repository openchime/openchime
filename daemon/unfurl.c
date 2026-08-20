/* Link-unfurl fetch worker — see unfurl.h and ARCH-105. One worker thread, a
 * bounded in-queue, and the push emitter's outbound-HTTP idiom (push.c): a
 * blocking socket with receive/send timeouts, TLS through shared/tls.c's
 * CA-verifying client. What is new relative to push/enroll is the SSRF gate —
 * those dial one operator-configured endpoint; this dials whatever a user
 * pasted into a message. */

#include "unfurl.h"

#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "tls.h"

/* Bounds. All compile-time: nothing here has needed tuning yet, and a knob
 * nobody turns is a liability (the REQ-190 reasoning). */
#define UF_MAX_URL       2048          /* longer is a probe, not a link        */
#define UF_MAX_BODY      (128 * 1024)  /* bytes of response we will look at    */
#define UF_MAX_REDIRECTS 3
#define UF_IO_TIMEOUT_S  5             /* per read/write                       */
#define UF_DEADLINE_MS   10000         /* whole fetch, redirects included      */
#define UF_QUEUE_MAX     64            /* pending fetches; beyond this, drop   */
#define UF_TITLE_CAP     256
#define UF_DESCR_CAP     512

/* --- The SSRF gate ------------------------------------------------------- */

/* v4 in host order. The list is the well-known non-routable set: unspecified,
 * RFC-1918 private, loopback, link-local, CGNAT, the documentation and
 * benchmark nets, multicast, and everything at or above 240/4. */
static int v4_public(uint32_t a) {
    unsigned o1 = (a >> 24) & 0xff, o2 = (a >> 16) & 0xff, o3 = (a >> 8) & 0xff;
    if (o1 == 0 || o1 == 10 || o1 == 127)            return 0; /* 0/8 10/8 127/8 */
    if (o1 == 100 && (o2 & 0xc0) == 64)              return 0; /* 100.64/10 CGNAT */
    if (o1 == 169 && o2 == 254)                      return 0; /* 169.254/16      */
    if (o1 == 172 && (o2 & 0xf0) == 16)              return 0; /* 172.16/12       */
    if (o1 == 192 && o2 == 168)                      return 0; /* 192.168/16      */
    if (o1 == 192 && o2 == 0 && (o3 == 0 || o3 == 2)) return 0; /* 192.0.0/24 + 192.0.2/24 */
    if (o1 == 198 && (o2 == 18 || o2 == 19))         return 0; /* 198.18/15 bench */
    if (o1 == 198 && o2 == 51 && o3 == 100)          return 0; /* 198.51.100/24   */
    if (o1 == 203 && o2 == 0 && o3 == 113)           return 0; /* 203.0.113/24    */
    if (o1 >= 224)                                   return 0; /* 224/4 + 240/4   */
    return 1;
}

int oc_unfurl_addr_public(const struct sockaddr *sa) {
    if (!sa) return 0;
    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *s4 = (const struct sockaddr_in *)sa;
        return v4_public(ntohl(s4->sin_addr.s_addr));
    }
    if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *s6 = (const struct sockaddr_in6 *)sa;
        const uint8_t *b = s6->sin6_addr.s6_addr;
        static const uint8_t zeros[15] = { 0 };
        /* :: and ::1 */
        if (memcmp(b, zeros, 15) == 0 && (b[15] == 0 || b[15] == 1)) return 0;
        /* ::ffff:a.b.c.d — judge the embedded v4. */
        if (memcmp(b, zeros, 10) == 0 && b[10] == 0xff && b[11] == 0xff) {
            uint32_t a = ((uint32_t)b[12] << 24) | ((uint32_t)b[13] << 16) |
                         ((uint32_t)b[14] << 8)  |  (uint32_t)b[15];
            return v4_public(a);
        }
        /* 64:ff9b::/96 NAT64 — an indirection layer; refuse rather than guess. */
        if (b[0] == 0x00 && b[1] == 0x64 && b[2] == 0xff && b[3] == 0x9b) return 0;
        if ((b[0] & 0xfe) == 0xfc) return 0;                 /* fc00::/7 ULA    */
        if (b[0] == 0xfe && (b[1] & 0xc0) == 0x80) return 0; /* fe80::/10 link  */
        if (b[0] == 0xfe && (b[1] & 0xc0) == 0xc0) return 0; /* fec0::/10 site  */
        if (b[0] == 0xff) return 0;                          /* ff00::/8 mcast  */
        if (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x0d && b[3] == 0xb8)
            return 0;                                        /* 2001:db8::/32   */
        return 1;
    }
    return 0;   /* an address family we cannot judge is one we do not dial */
}

/* --- HTML scan ------------------------------------------------------------ */

static char uf_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

/* Case-insensitive substring search; returns offset or (size_t)-1. */
static size_t ci_find(const char *h, size_t n, size_t from, const char *needle) {
    size_t nl = strlen(needle);
    if (nl == 0 || from >= n || nl > n - from) return (size_t)-1;
    for (size_t i = from; i + nl <= n; i++) {
        size_t k = 0;
        while (k < nl && uf_lower(h[i + k]) == uf_lower(needle[k])) k++;
        if (k == nl) return i;
    }
    return (size_t)-1;
}

/* The value of attribute `attr` inside tag bytes [t, t+tn): a pointer into the
 * buffer plus its length, or NULL. Accepts single or double quotes. */
static const char *attr_value(const char *t, size_t tn, const char *attr, size_t *out_len) {
    size_t at = ci_find(t, tn, 0, attr);
    while (at != (size_t)-1) {
        size_t i = at + strlen(attr);
        /* Must be a whole attribute name: preceded by space/start-ish byte. */
        int ok = (at == 0) || t[at - 1] == ' ' || t[at - 1] == '\t' ||
                 t[at - 1] == '\n' || t[at - 1] == '\r' || t[at - 1] == '"' || t[at - 1] == '\'';
        while (ok && i < tn && (t[i] == ' ' || t[i] == '\t')) i++;
        if (ok && i < tn && t[i] == '=') {
            i++;
            while (i < tn && (t[i] == ' ' || t[i] == '\t')) i++;
            if (i < tn && (t[i] == '"' || t[i] == '\'')) {
                char q = t[i++];
                size_t s = i;
                while (i < tn && t[i] != q) i++;
                if (i <= tn) { *out_len = i - s; return t + s; }
            }
        }
        at = ci_find(t, tn, at + 1, attr);
    }
    return NULL;
}

/* Decode the basic entities and numeric ASCII references, collapse whitespace,
 * trim, and copy into `out` truncating on a UTF-8 boundary. */
static void clean_text(const char *s, size_t n, char *out, size_t cap) {
    size_t o = 0, i = 0;
    int pending_space = 0;
    if (!cap) return;
    while (i < n && o + 1 < cap) {
        unsigned char c = (unsigned char)s[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            pending_space = (o > 0);
            i++;
            continue;
        }
        if (pending_space) {
            if (o + 2 >= cap) break;
            out[o++] = ' ';
            pending_space = 0;
        }
        if (c == '&') {
            struct { const char *name; char ch; } ents[] = {
                { "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' },
                { "&quot;", '"' }, { "&apos;", '\'' }, { "&nbsp;", ' ' },
            };
            size_t e;
            for (e = 0; e < sizeof ents / sizeof ents[0]; e++) {
                size_t el = strlen(ents[e].name);
                if (i + el <= n && strncmp(s + i, ents[e].name, el) == 0) {
                    out[o++] = ents[e].ch; i += el; break;
                }
            }
            if (e < sizeof ents / sizeof ents[0]) continue;
            if (i + 2 < n && s[i + 1] == '#') {
                size_t j = i + 2; unsigned long v = 0; int hex = 0;
                if (j < n && (s[j] == 'x' || s[j] == 'X')) { hex = 1; j++; }
                size_t d0 = j;
                while (j < n && s[j] != ';' && j - d0 < 6) {
                    char dc = uf_lower(s[j]);
                    int dv = (dc >= '0' && dc <= '9') ? dc - '0'
                           : (hex && dc >= 'a' && dc <= 'f') ? dc - 'a' + 10 : -1;
                    if (dv < 0) break;
                    v = v * (hex ? 16 : 10) + (unsigned long)dv;
                    j++;
                }
                if (j < n && s[j] == ';' && j > d0 && v >= 0x20 && v < 0x7f) {
                    out[o++] = (char)v; i = j + 1; continue;
                }
            }
            out[o++] = '&'; i++;
            continue;
        }
        out[o++] = (char)c;
        i++;
    }
    /* Never cut a UTF-8 sequence: if the tail is an incomplete one — a lone
     * lead byte, or a lead short of its continuations — drop it whole. */
    if (o > 0) {
        size_t lead = o;
        while (lead > 0 && ((unsigned char)out[lead - 1] & 0xc0) == 0x80) lead--;
        if (lead > 0) {
            unsigned char lc = (unsigned char)out[lead - 1];
            size_t want = (lc & 0xf8) == 0xf0 ? 4 : (lc & 0xf0) == 0xe0 ? 3 :
                          (lc & 0xe0) == 0xc0 ? 2 : 1;
            if (lc >= 0x80 && o - (lead - 1) < want) o = lead - 1;
        }
    }
    out[o] = '\0';
}

/* Find the content= of the first <meta ...> whose property= or name= equals
 * `key` (case-insensitively), and clean it into out. Returns 1 on a hit. */
static int meta_content(const char *h, size_t n, const char *key, char *out, size_t cap) {
    size_t at = ci_find(h, n, 0, "<meta");
    while (at != (size_t)-1) {
        size_t end = at;
        while (end < n && h[end] != '>') end++;
        const char *t = h + at; size_t tn = end - at;
        size_t vl = 0;
        const char *v = attr_value(t, tn, "property", &vl);
        if (!v) v = attr_value(t, tn, "name", &vl);
        if (v && vl == strlen(key)) {
            size_t k = 0;
            while (k < vl && uf_lower(v[k]) == uf_lower(key[k])) k++;
            if (k == vl) {
                size_t cl = 0;
                const char *c = attr_value(t, tn, "content", &cl);
                if (c && cl) { clean_text(c, cl, out, cap); return out[0] != '\0'; }
            }
        }
        at = ci_find(h, n, end, "<meta");
    }
    return 0;
}

int oc_unfurl_extract_html(const char *html, size_t len,
                           char *title, size_t title_cap,
                           char *descr, size_t descr_cap) {
    if (title_cap) title[0] = '\0';
    if (descr_cap) descr[0] = '\0';
    if (!html || !len) return -1;

    /* og: first — it is what the page's author wrote FOR this use — then the
     * document's own <title> / description meta. */
    if (!meta_content(html, len, "og:title", title, title_cap)) {
        size_t t = ci_find(html, len, 0, "<title");
        if (t != (size_t)-1) {
            size_t open = t;
            while (open < len && html[open] != '>') open++;
            size_t close = ci_find(html, len, open, "</title");
            if (open < len && close != (size_t)-1 && close > open + 1)
                clean_text(html + open + 1, close - open - 1, title, title_cap);
        }
    }
    if (!meta_content(html, len, "og:description", descr, descr_cap))
        meta_content(html, len, "description", descr, descr_cap);

    return (title_cap && title[0] != '\0') ? 0 : -1;
}

/* --- The fetch ------------------------------------------------------------ */

typedef struct {
    char host[256];
    char port[16];
    char path[UF_MAX_URL];
    int  use_tls;
} uf_url;

static int uf_parse_url(const char *url, size_t len, uf_url *u) {
    memset(u, 0, sizeof *u);
    if (len >= UF_MAX_URL) return -1;
    char buf[UF_MAX_URL];
    memcpy(buf, url, len); buf[len] = '\0';

    const char *p = buf;
    if (strncasecmp(p, "https://", 8) == 0) { p += 8; u->use_tls = 1; snprintf(u->port, sizeof u->port, "443"); }
    else if (strncasecmp(p, "http://", 7) == 0) { p += 7; u->use_tls = 0; snprintf(u->port, sizeof u->port, "80"); }
    else return -1;

    char hostport[256];
    size_t i = 0;
    while (*p && *p != '/' && *p != '?' && *p != '#' && i < sizeof hostport - 1) hostport[i++] = *p++;
    hostport[i] = '\0';
    if (i == 0) return -1;
    /* A bracketed v6 literal is refused rather than parsed: rare in prose, and
     * every hard case here is a case the gate must not get wrong. */
    if (strchr(hostport, '[')) return -1;

    char *colon = strchr(hostport, ':');
    if (colon) {
        *colon = '\0';
        if (!colon[1]) return -1;
        snprintf(u->port, sizeof u->port, "%s", colon + 1);
    }
    if (!hostport[0]) return -1;
    snprintf(u->host, sizeof u->host, "%s", hostport);

    const char *frag = strchr(p, '#');            /* a fragment never goes on the wire */
    size_t plen = frag ? (size_t)(frag - p) : strlen(p);
    if (plen == 0 || p[0] != '/') snprintf(u->path, sizeof u->path, "/%.*s", (int)plen, p);
    else snprintf(u->path, sizeof u->path, "%.*s", (int)plen, p);
    return 0;
}

/* Resolve and connect, gating EVERY resolved address first: if any one of them
 * is non-public the whole fetch is refused — connecting to "one of the good
 * ones" hands a rebinding resolver a coin to flip. Connects to the vetted
 * sockaddr itself, never by re-resolving the name. */
static int uf_connect(const uf_url *u, int allow_private) {
    struct addrinfo hints, *res = NULL, *rp;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(u->host, u->port, &hints, &res) != 0) return -1;
    if (!allow_private) {
        for (rp = res; rp; rp = rp->ai_next)
            if (!oc_unfurl_addr_public(rp->ai_addr)) { freeaddrinfo(res); return -1; }
    }
    int fd = -1;
    for (rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        struct timeval tv = { UF_IO_TIMEOUT_S, 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

typedef struct {
    int         fd;
    int         tls;
    oc_tls_conn conn;
} uf_conn;

static uint64_t uf_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void uf_close(uf_conn *c) {
    if (c->tls) oc_tls_conn_free(&c->conn);
    if (c->fd >= 0) close(c->fd);
    c->fd = -1; c->tls = 0;
}

static int uf_write(uf_conn *c, const void *buf, size_t len, uint64_t deadline) {
    const char *p = buf; size_t sent = 0;
    while (sent < len) {
        if (uf_now_ms() > deadline) return -1;
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

/* One read; 0 = orderly EOF, -1 = error or past the deadline. */
static long uf_read(uf_conn *c, void *buf, size_t cap, uint64_t deadline) {
    for (;;) {
        if (uf_now_ms() > deadline) return -1;
        if (c->tls) {
            size_t n = 0;
            oc_tls_status st = oc_tls_read(&c->conn, buf, cap, &n);
            if (st == OC_TLS_WANT_READ || st == OC_TLS_WANT_WRITE) continue;
            if (st == OC_TLS_CLOSED) return 0;
            if (st != OC_TLS_OK) return -1;
            return (long)n;
        }
        ssize_t n = read(c->fd, buf, cap);
        if (n < 0) return -1;
        return (long)n;
    }
}

/* Case-insensitive header lookup in [h, h+n): the value of `name` cleaned of
 * leading space, copied NUL-terminated into out. Returns 1 on a hit. */
static int uf_header(const char *h, size_t n, const char *name, char *out, size_t cap) {
    size_t i = 0;
    size_t nl = strlen(name);
    while (i < n) {
        size_t eol = i;
        while (eol < n && h[eol] != '\n') eol++;
        if (eol - i > nl && h[i + nl] == ':' ) {
            size_t k = 0;
            while (k < nl && uf_lower(h[i + k]) == uf_lower(name[k])) k++;
            if (k == nl) {
                size_t s = i + nl + 1, e = eol;
                while (s < e && (h[s] == ' ' || h[s] == '\t')) s++;
                while (e > s && (h[e - 1] == '\r' || h[e - 1] == ' ')) e--;
                if (e - s >= cap) e = s + cap - 1;
                memcpy(out, h + s, e - s);
                out[e - s] = '\0';
                return 1;
            }
        }
        i = eol + 1;
    }
    return 0;
}

typedef struct oc_unfurler {
    oc_dbwriter    *dbw;
    oc_tls_client   tls;
    int             tls_ok;
    int             allow_private;
    pthread_t       th;
    int             started;
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    int             stop;
    int             depth;
    struct uf_job { struct uf_job *next; uint64_t channel_id, message_id; char *url; } *head, *tail;
} oc_unfurler;

/* GET one URL, following up to UF_MAX_REDIRECTS, each hop re-gated. On success
 * the response body (headers stripped) is in *out, heap, *out_len set. */
static int uf_get(oc_unfurler *u, const char *url, size_t url_len,
                  char **out, size_t *out_len) {
    char cur[UF_MAX_URL];
    if (url_len >= sizeof cur) return -1;
    memcpy(cur, url, url_len); cur[url_len] = '\0';

    uint64_t deadline = uf_now_ms() + UF_DEADLINE_MS;

    for (int hop = 0; hop <= UF_MAX_REDIRECTS; hop++) {
        uf_url loc;
        if (uf_parse_url(cur, strlen(cur), &loc) != 0) return -1;
        if (loc.use_tls && !u->tls_ok) return -1;

        uf_conn c = { .fd = -1 };
        c.fd = uf_connect(&loc, u->allow_private);
        if (c.fd < 0) return -1;
        if (loc.use_tls) {
            if (oc_tls_conn_init(&c.conn, &u->tls.conf, c.fd) != 0) { close(c.fd); return -1; }
            if (oc_tls_conn_set_hostname(&c.conn, loc.host) != 0) { oc_tls_conn_free(&c.conn); close(c.fd); return -1; }
            for (;;) {
                oc_tls_status st = oc_tls_handshake(&c.conn);
                if (st == OC_TLS_OK) break;
                if ((st == OC_TLS_WANT_READ || st == OC_TLS_WANT_WRITE)
                    && uf_now_ms() <= deadline) continue;
                oc_tls_conn_free(&c.conn); close(c.fd);
                return -1;
            }
            c.tls = 1;
        }

        /* HTTP/1.0 on purpose: it forbids chunked transfer coding, so the body
         * arrives as plain bytes until close and needs no de-chunker for
         * whatever an arbitrary server would otherwise send. */
        char req[UF_MAX_URL + 512];
        int is_default = (loc.use_tls && strcmp(loc.port, "443") == 0) ||
                         (!loc.use_tls && strcmp(loc.port, "80") == 0);
        int rn = snprintf(req, sizeof req,
                          "GET %s HTTP/1.0\r\n"
                          "Host: %s%s%s\r\n"
                          "User-Agent: openchimed-unfurl\r\n"
                          "Accept: text/html\r\n"
                          "Connection: close\r\n\r\n",
                          loc.path, loc.host,
                          is_default ? "" : ":", is_default ? "" : loc.port);
        if (rn <= 0 || (size_t)rn >= sizeof req ||
            uf_write(&c, req, (size_t)rn, deadline) != 0) { uf_close(&c); return -1; }

        char *buf = malloc(UF_MAX_BODY + 1);
        if (!buf) { uf_close(&c); return -1; }
        size_t got = 0;
        for (;;) {
            if (got >= UF_MAX_BODY) break;                     /* the cap IS the page */
            long n = uf_read(&c, buf + got, UF_MAX_BODY - got, deadline);
            if (n <= 0) break;
            got += (size_t)n;
        }
        uf_close(&c);
        buf[got] = '\0';

        /* Head: status line + headers up to the blank line. */
        size_t hdr_end = ci_find(buf, got, 0, "\r\n\r\n");
        if (hdr_end == (size_t)-1 || strncmp(buf, "HTTP/1.", 7) != 0 || got < 12) {
            free(buf); return -1;
        }
        int status = atoi(buf + 9);
        if (status == 301 || status == 302 || status == 303 ||
            status == 307 || status == 308) {
            char where[UF_MAX_URL];
            int have = uf_header(buf, hdr_end, "location", where, sizeof where);
            free(buf);
            if (!have || hop == UF_MAX_REDIRECTS) return -1;
            if (where[0] == '/') {
                /* Path-relative: same scheme, host and port. */
                char abs[UF_MAX_URL];
                int an = snprintf(abs, sizeof abs, "%s://%s:%s%s",
                                  loc.use_tls ? "https" : "http", loc.host, loc.port, where);
                if (an <= 0 || (size_t)an >= sizeof abs) return -1;
                memcpy(cur, abs, (size_t)an + 1);
            } else {
                if (strlen(where) >= sizeof cur) return -1;
                strcpy(cur, where);
            }
            continue;
        }
        if (status != 200) { free(buf); return -1; }

        char ctype[128];
        if (!uf_header(buf, hdr_end, "content-type", ctype, sizeof ctype) ||
            ci_find(ctype, strlen(ctype), 0, "text/html") == (size_t)-1) {
            free(buf); return -1;
        }

        size_t body_at = hdr_end + 4;
        memmove(buf, buf + body_at, got - body_at);
        *out = buf;
        *out_len = got - body_at;
        return 0;
    }
    return -1;
}

static void uf_do(oc_unfurler *u, struct uf_job *job) {
    char *page = NULL; size_t page_len = 0;
    if (uf_get(u, job->url, strlen(job->url), &page, &page_len) != 0) return;

    char title[UF_TITLE_CAP], descr[UF_DESCR_CAP];
    int ok = oc_unfurl_extract_html(page, page_len, title, sizeof title, descr, sizeof descr);
    free(page);
    if (ok != 0) return;

    /* Hand the store to the writer (conn_id 0: no connection owns this). The
     * writer re-validates the message — it may have been tombstoned or edited
     * while the fetch was in flight — and the net thread fans the result. */
    oc_job *j = oc_job_new(OC_JOB_UNFURL_STORE, 0);
    if (!j) return;
    j->channel_id = job->channel_id;
    j->message_id = job->message_id;
    j->unf_url    = strdup(job->url);
    j->unf_title  = strdup(title);
    j->unf_descr  = strdup(descr);
    if (!j->unf_url || !j->unf_title || !j->unf_descr) {
        /* The writer's own free path is private; a job we never submit is
         * ours to unpick by hand. */
        free(j->unf_url); free(j->unf_title); free(j->unf_descr); free(j);
        return;
    }
    oc_dbwriter_submit(u->dbw, j);
}

static void *uf_worker(void *arg) {
    oc_unfurler *u = arg;
    for (;;) {
        pthread_mutex_lock(&u->mu);
        while (!u->stop && !u->head) pthread_cond_wait(&u->cv, &u->mu);
        if (u->stop && !u->head) { pthread_mutex_unlock(&u->mu); return NULL; }
        struct uf_job *job = u->head;
        u->head = job->next;
        if (!u->head) u->tail = NULL;
        u->depth--;
        pthread_mutex_unlock(&u->mu);

        if (!u->stop) uf_do(u, job);
        free(job->url);
        free(job);
    }
}

oc_unfurler *oc_unfurler_start(oc_dbwriter *dbw, const char *ca_bundle, int allow_private) {
    if (!dbw) return NULL;
    oc_unfurler *u = calloc(1, sizeof *u);
    if (!u) return NULL;
    u->dbw = dbw;
    u->allow_private = allow_private;
    u->tls_ok = (oc_tls_client_init_ca(&u->tls, ca_bundle) == 0);
    if (!u->tls_ok)
        fprintf(stderr, "unfurl: CA store unavailable; https targets disabled\n");
    pthread_mutex_init(&u->mu, NULL);
    pthread_cond_init(&u->cv, NULL);
    if (pthread_create(&u->th, NULL, uf_worker, u) != 0) {
        if (u->tls_ok) oc_tls_client_free(&u->tls);
        pthread_mutex_destroy(&u->mu);
        pthread_cond_destroy(&u->cv);
        free(u);
        return NULL;
    }
    u->started = 1;
    return u;
}

void oc_unfurler_fetch(oc_unfurler *u, uint64_t channel_id, uint64_t message_id,
                       const char *url, size_t url_len) {
    if (!u || !url || url_len == 0 || url_len >= UF_MAX_URL) return;
    struct uf_job *j = calloc(1, sizeof *j);
    if (!j) return;
    j->channel_id = channel_id;
    j->message_id = message_id;
    j->url = malloc(url_len + 1);
    if (!j->url) { free(j); return; }
    memcpy(j->url, url, url_len);
    j->url[url_len] = '\0';

    pthread_mutex_lock(&u->mu);
    if (u->stop || u->depth >= UF_QUEUE_MAX) {   /* best-effort: drop, never block */
        pthread_mutex_unlock(&u->mu);
        free(j->url); free(j);
        return;
    }
    if (u->tail) u->tail->next = j; else u->head = j;
    u->tail = j;
    u->depth++;
    pthread_cond_signal(&u->cv);
    pthread_mutex_unlock(&u->mu);
}

void oc_unfurler_stop(oc_unfurler *u) {
    if (!u) return;
    pthread_mutex_lock(&u->mu);
    u->stop = 1;
    /* Drop what is still queued: these are best-effort fetches, and stop should
     * not wait behind a slow site. The one in flight finishes on its own. */
    struct uf_job *j = u->head;
    while (j) { struct uf_job *nx = j->next; free(j->url); free(j); j = nx; }
    u->head = u->tail = NULL;
    u->depth = 0;
    pthread_cond_broadcast(&u->cv);
    pthread_mutex_unlock(&u->mu);
    if (u->started) pthread_join(u->th, NULL);
    if (u->tls_ok) oc_tls_client_free(&u->tls);
    pthread_mutex_destroy(&u->mu);
    pthread_cond_destroy(&u->cv);
    free(u);
}
