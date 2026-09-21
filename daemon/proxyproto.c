/* PROXY protocol v2, receiving side — see proxyproto.h. */

#include "proxyproto.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int     family;        /* AF_INET or AF_INET6 */
    uint8_t addr[16];
    int     bits;
} net_block;

struct oc_trusted_proxies {
    net_block *blocks;
    size_t     n;
};

static int bits_match(const uint8_t *a, const uint8_t *b, int bits) {
    int full = bits / 8, rest = bits % 8;
    if (memcmp(a, b, (size_t)full) != 0) return 0;
    if (!rest) return 1;
    uint8_t mask = (uint8_t)(0xFFu << (8 - rest));
    return (a[full] & mask) == (b[full] & mask);
}

void oc_trusted_proxies_free(oc_trusted_proxies *t) {
    if (!t) return;
    free(t->blocks);
    free(t);
}

oc_trusted_proxies *oc_trusted_proxies_parse(const char *spec, char *err, size_t errcap) {
    if (err && errcap) err[0] = '\0';
    oc_trusted_proxies *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    if (!spec || !*spec) return t;

    size_t cap = 1;
    for (const char *p = spec; *p; p++) if (*p == ',') cap++;
    t->blocks = calloc(cap, sizeof *t->blocks);
    if (!t->blocks) { free(t); return NULL; }

    const char *p = spec;
    while (*p) {
        const char *end = strchr(p, ',');
        if (!end) end = p + strlen(p);
        const char *s = p, *e = end;
        while (s < e && (*s == ' ' || *s == '\t')) s++;
        while (e > s && (e[-1] == ' ' || e[-1] == '\t')) e--;
        p = *end ? end + 1 : end;
        if (s == e) continue;

        char item[80];
        size_t n = (size_t)(e - s);
        if (n >= sizeof item) goto bad;
        memcpy(item, s, n);
        item[n] = '\0';

        int bits = -1;
        char *slash = strchr(item, '/');
        if (slash) {
            *slash = '\0';
            char *endp = NULL;
            long v = strtol(slash + 1, &endp, 10);
            if (!slash[1] || *endp || v < 0) { *slash = '/'; goto bad; }
            bits = (int)v;
        }
        net_block *b = &t->blocks[t->n];
        if (inet_pton(AF_INET, item, b->addr) == 1) {
            b->family = AF_INET;
            if (bits < 0) bits = 32;
            if (bits > 32) goto bad;
        } else if (inet_pton(AF_INET6, item, b->addr) == 1) {
            b->family = AF_INET6;
            if (bits < 0) bits = 128;
            if (bits > 128) goto bad;
        } else {
            if (slash) *slash = '/';
            goto bad;
        }
        b->bits = bits;
        t->n++;
        continue;
    bad:
        if (err && errcap) snprintf(err, errcap, "not an address or a CIDR block: \"%.*s\"", (int)n, s);
        oc_trusted_proxies_free(t);
        return NULL;
    }
    return t;
}

int oc_trusted_proxies_match(const oc_trusted_proxies *t, const struct sockaddr_storage *peer) {
    if (!t || !peer || t->n == 0) return 0;
    static const uint8_t V4MAPPED[12] = { 0,0,0,0,0,0,0,0,0,0,0xFF,0xFF };
    const uint8_t *v4 = NULL, *v6 = NULL;
    if (peer->ss_family == AF_INET) {
        v4 = (const uint8_t *)&((const struct sockaddr_in *)peer)->sin_addr;
    } else if (peer->ss_family == AF_INET6) {
        v6 = (const uint8_t *)&((const struct sockaddr_in6 *)peer)->sin6_addr;
        if (memcmp(v6, V4MAPPED, sizeof V4MAPPED) == 0) v4 = v6 + 12;
    } else {
        return 0;
    }
    for (size_t i = 0; i < t->n; i++) {
        const net_block *b = &t->blocks[i];
        if (b->family == AF_INET && v4 && bits_match(v4, b->addr, b->bits)) return 1;
        if (b->family == AF_INET6 && v6 && bits_match(v6, b->addr, b->bits)) return 1;
    }
    return 0;
}

long oc_proxy_v2_parse(const uint8_t *buf, size_t len, char src[46]) {
    static const uint8_t SIG[12] = { 0x0D,0x0A,0x0D,0x0A,0x00,0x0D,0x0A,0x51,0x55,0x49,0x54,0x0A };
    src[0] = '\0';
    /* Refuse as soon as the bytes cannot be the signature, rather than wait for
     * sixteen of something else. */
    size_t cmp = len < sizeof SIG ? len : sizeof SIG;
    if (memcmp(buf, SIG, cmp) != 0) return -1;
    if (len < 16) return 0;

    uint8_t ver = buf[12] >> 4, cmd = buf[12] & 0x0F;
    uint8_t fam = buf[13];
    size_t body = ((size_t)buf[14] << 8) | buf[15];
    if (ver != 2 || (cmd != 0 && cmd != 1)) return -1;
    if (16 + body > OC_PROXY_V2_MAX) return -1;
    if (len < 16 + body) return 0;

    if (cmd == 0) return (long)(16 + body);    /* LOCAL: the forwarder speaking for itself */

    if (fam == 0x11) {                          /* TCP over IPv4: src, dst, ports */
        if (body < 12 || !inet_ntop(AF_INET, buf + 16, src, 46)) return -1;
    } else if (fam == 0x21) {                   /* TCP over IPv6 */
        if (body < 36 || !inet_ntop(AF_INET6, buf + 16, src, 46)) return -1;
    } else {
        return -1;                              /* UDP, UNIX, unspecified: not a client of ours */
    }
    return (long)(16 + body);
}
