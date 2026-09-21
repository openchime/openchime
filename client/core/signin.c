/* Browser sign-in, client side — see signin.h. */

#include "sock.h"       /* first: winsock2 must win the include race */
#include "signin.h"

#include "model.h"      /* oc_model_now_ms: the one clock */

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
    char path[64];   /* "/cb/<secret>" */
};

oc_loopback *oc_loopback_open(char *redirect_uri, size_t cap) {
    oc_sock_startup();
    uint8_t raw[18];
    char secret[32];
    if (os_random(raw, sizeof raw) != 0) return NULL;
    b64url(raw, sizeof raw, secret);

    oc_loopback *lb = calloc(1, sizeof *lb);
    if (!lb) return NULL;
    lb->fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (lb->fd < 0) { free(lb); return NULL; }

    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   /* never the wildcard address */
    a.sin_port = 0;
    socklen_t alen = sizeof a;
    if (bind(lb->fd, (struct sockaddr *)&a, sizeof a) != 0 ||
        listen(lb->fd, 4) != 0 ||
        getsockname(lb->fd, (struct sockaddr *)&a, &alen) != 0) {
        oc_loopback_close(lb);
        return NULL;
    }
    oc_sock_setnonblock(lb->fd);
    snprintf(lb->path, sizeof lb->path, "/cb/%s", secret);
    int n = snprintf(redirect_uri, cap, "http://127.0.0.1:%u%s",
                     (unsigned)ntohs(a.sin_port), lb->path);
    if (n < 0 || (size_t)n >= cap) { oc_loopback_close(lb); return NULL; }
    return lb;
}

void oc_loopback_close(oc_loopback *lb) {
    if (!lb) return;
    if (lb->fd >= 0) oc_closesock(lb->fd);
    oc_signin_wipe(lb->path, sizeof lb->path);
    free(lb);
}

static void send_all(int fd, const char *s) {
    size_t n = strlen(s), off = 0;
    while (off < n) {
        int w = (int)send(fd, s + off, (int)(n - off), 0);
        if (w > 0) { off += (size_t)w; continue; }
        if (w < 0 && oc_sock_wouldblock() && oc_poll(fd, 1, 1000) > 0) continue;
        break;
    }
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

/* Read one request's first line from `fd` (up to 2 s) and, if it is a GET on
 * `path`, copy its query out. 1 ours, 0 not ours, -1 the query does not fit. */
static int serve_one(int fd, const char *path, char *query, size_t qcap) {
    char buf[8192];
    size_t got = 0;
    uint64_t until = oc_model_now_ms() + 2000;
    while (got < sizeof buf - 1 && !memchr(buf, '\n', got)) {
        uint64_t now = oc_model_now_ms();
        if (now >= until || oc_poll(fd, 0, (int)(until - now)) <= 0) break;
        int r = (int)recv(fd, buf + got, (int)(sizeof buf - 1 - got), 0);
        if (r > 0) { got += (size_t)r; continue; }
        if (r < 0 && oc_sock_wouldblock()) continue;
        break;
    }
    buf[got] = '\0';
    char *eol = strpbrk(buf, "\r\n");
    if (!eol || strncmp(buf, "GET ", 4) != 0) return 0;
    *eol = '\0';
    char *target = buf + 4;
    char *sp = strchr(target, ' ');
    if (!sp) return 0;
    *sp = '\0';
    size_t pl = strlen(path);
    if (strncmp(target, path, pl) != 0) return 0;
    const char *q = target + pl;
    if (*q == '\0') q = "";
    else if (*q == '?') q++;
    else return 0;                         /* "/cb/<secret>extra" is not the path */
    if (strlen(q) >= qcap) return -1;
    memcpy(query, q, strlen(q) + 1);
    return 1;
}

oc_loopback_result oc_loopback_wait(oc_loopback *lb, int timeout_ms, const volatile int *cancel,
                                    char *query, size_t qcap) {
    if (!lb || !query || qcap == 0) return OC_LOOPBACK_ERROR;
    query[0] = '\0';
    uint64_t until = oc_model_now_ms() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0);
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
        int ours = serve_one(c, lb->path, query, qcap);
        send_all(c, ours == 1 ? PAGE_OK : PAGE_404);
        oc_closesock(c);
        if (ours == 1) return OC_LOOPBACK_OK;
        if (ours < 0) return OC_LOOPBACK_ERROR;
    }
}

/* --- query strings -------------------------------------------------------- */

static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int oc_query_get(const char *query, const char *key, char *out, size_t cap) {
    if (!query || !key || !out || cap == 0) return -1;
    out[0] = '\0';
    size_t kl = strlen(key);
    const char *p = query;
    while (*p) {
        const char *amp = strchr(p, '&');
        const char *end = amp ? amp : p + strlen(p);
        const char *eq = memchr(p, '=', (size_t)(end - p));
        if (eq && (size_t)(eq - p) == kl && strncmp(p, key, kl) == 0) {
            size_t o = 0;
            for (const char *v = eq + 1; v < end; v++) {
                int c = (unsigned char)*v;
                if (c == '+') c = ' ';
                else if (c == '%') {
                    if (end - v < 3) return -1;
                    int hi = hexv(v[1]), lo = hexv(v[2]);
                    if (hi < 0 || lo < 0) return -1;
                    c = hi * 16 + lo;
                    v += 2;
                }
                if (c == 0 || o + 1 >= cap) { out[0] = '\0'; return -1; }
                out[o++] = (char)c;
            }
            out[o] = '\0';
            return 1;
        }
        p = amp ? amp + 1 : end;
    }
    return 0;
}
