/* The test harness's line to its driver (harness.h). */
#include "harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* jsmn's implementation, private to this file, as client/core/releasenotes.c
 * keeps its own: the test program links the daemon's copy too. */
#define jsmn_init  oc_hs_jsmn_init
#define jsmn_parse oc_hs_jsmn_parse
#include "jsmn.h"

#if defined(__EMSCRIPTEN__)
/* The browser has no socket to dial: no harness there. */
int  oc_harness_dial(const char *host, int port, const char *token) { (void)host; (void)port; (void)token; return -1; }
int  oc_harness_read_line(char *buf, size_t cap) { (void)buf; (void)cap; return -1; }
int  oc_harness_reply(const char *line, const void *bin, size_t bin_len) { (void)line; (void)bin; (void)bin_len; return -1; }
void oc_harness_close(void) {}
#else
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET hs_sock;
#define HS_BAD INVALID_SOCKET
#define hs_closesock closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int hs_sock;
#define HS_BAD (-1)
#define hs_closesock close
#endif

static hs_sock g_hs = HS_BAD;
static char    g_rbuf[65536];
static size_t  g_rlen;

static int hs_send_all(const void *p, size_t n) {
    const char *b = p;
    while (n) {
        int w = (int)send(g_hs, b, (int)(n > 1u << 20 ? 1u << 20 : n), 0);
        if (w <= 0) return -1;
        b += w; n -= (size_t)w;
    }
    return 0;
}

int oc_harness_dial(const char *host, int port, const char *token) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
#endif
    if (port <= 0 || port > 65535 || !token) return -1;
    g_hs = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_hs == HS_BAD) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (host && host[0] && inet_pton(AF_INET, host, &a.sin_addr) != 1) { oc_harness_close(); return -1; }
    if (connect(g_hs, (struct sockaddr *)&a, sizeof a) != 0) { oc_harness_close(); return -1; }
    int one = 1;
    setsockopt(g_hs, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    char hello[320];
    snprintf(hello, sizeof hello, "hello %s\n", token);
    return hs_send_all(hello, strlen(hello));
}

int oc_harness_read_line(char *buf, size_t cap) {
    for (;;) {
        char *nl = memchr(g_rbuf, '\n', g_rlen);
        if (nl) {
            size_t n = (size_t)(nl - g_rbuf);
            size_t k = n < cap - 1 ? n : cap - 1;
            memcpy(buf, g_rbuf, k);
            buf[k] = '\0';
            memmove(g_rbuf, nl + 1, g_rlen - n - 1);
            g_rlen -= n + 1;
            return (int)k;
        }
        if (g_rlen == sizeof g_rbuf) return -1;          /* a line too long to be a command */
        int r = (int)recv(g_hs, g_rbuf + g_rlen, (int)(sizeof g_rbuf - g_rlen), 0);
        if (r <= 0) return -1;
        g_rlen += (size_t)r;
    }
}

int oc_harness_reply(const char *line, const void *bin, size_t bin_len) {
    if (g_hs == HS_BAD) return -1;
    if (hs_send_all(line, strlen(line)) != 0 || hs_send_all("\n", 1) != 0) return -1;
    return bin_len ? hs_send_all(bin, bin_len) : 0;
}

void oc_harness_close(void) {
    if (g_hs != HS_BAD) hs_closesock(g_hs);
    g_hs = HS_BAD;
}
#endif

/* ---- the command's JSON ---------------------------------------------------- */

struct oc_hcmd {
    char      *js;
    jsmntok_t  t[96];
    int        n;
};

oc_hcmd *oc_hcmd_parse(const char *json) {
    oc_hcmd *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->js = strdup(json ? json : "");
    if (!c->js) { free(c); return NULL; }
    jsmn_parser p;
    jsmn_init(&p);
    c->n = jsmn_parse(&p, c->js, strlen(c->js), c->t, 96);
    if (c->n < 1 || c->t[0].type != JSMN_OBJECT) { oc_hcmd_free(c); return NULL; }
    return c;
}

void oc_hcmd_free(oc_hcmd *c) {
    if (!c) return;
    free(c->js);
    free(c);
}

/* The value token of top-level member `key`, or -1. Members are flat: every
 * command's values are strings and numbers. */
static int hc_find(const oc_hcmd *c, const char *key) {
    size_t kl = strlen(key);
    for (int i = 1; i + 1 < c->n; i += 2) {
        const jsmntok_t *k = &c->t[i];
        if (k->type == JSMN_STRING && (size_t)(k->end - k->start) == kl &&
            !memcmp(c->js + k->start, key, kl))
            return i + 1;
    }
    return -1;
}

int oc_hcmd_str(const oc_hcmd *c, const char *key, char *out, size_t cap) {
    int i = c ? hc_find(c, key) : -1;
    if (i < 0 || !cap) return 0;
    const jsmntok_t *v = &c->t[i];
    size_t o = 0;
    for (int k = v->start; k < v->end && o + 1 < cap; k++) {
        char ch = c->js[k];
        if (ch == '\\' && k + 1 < v->end) {                /* the escapes a driver writes */
            char e = c->js[++k];
            ch = e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e;
        }
        out[o++] = ch;
    }
    out[o] = '\0';
    return 1;
}

int oc_hcmd_num(const oc_hcmd *c, const char *key, double *out) {
    int i = c ? hc_find(c, key) : -1;
    if (i < 0) return 0;
    char tmp[48];
    int len = c->t[i].end - c->t[i].start;
    if (len <= 0 || len >= (int)sizeof tmp) return 0;
    memcpy(tmp, c->js + c->t[i].start, (size_t)len);
    tmp[len] = '\0';
    char *end = NULL;
    double v = strtod(tmp, &end);
    if (end == tmp) return 0;
    *out = v;
    return 1;
}

void oc_json_escape(const char *s, char *out, size_t cap) {
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p && o + 7 < cap; p++) {
        if (*p == '"' || *p == '\\') { out[o++] = '\\'; out[o++] = (char)*p; }
        else if (*p == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
        else if (*p < 0x20) o += (size_t)snprintf(out + o, cap - o, "\\u%04x", *p);
        else out[o++] = (char)*p;
    }
    out[o] = '\0';
}
