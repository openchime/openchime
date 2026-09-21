/* PROXY protocol v2, receiving side (daemon/proxyproto.c): the header parser and
 * the list of peers it is believed from. */

#include "check.h"
#include "../daemon/proxyproto.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>

static const uint8_t SIG[12] = { 0x0D,0x0A,0x0D,0x0A,0x00,0x0D,0x0A,0x51,0x55,0x49,0x54,0x0A };

/* A v2 header: TCP over IPv4 from `src`, with `extra` bytes of TLV after it. */
static size_t v4_header(uint8_t *out, const char *src, size_t extra) {
    memcpy(out, SIG, 12);
    out[12] = 0x21; out[13] = 0x11;
    size_t body = 12 + extra;
    out[14] = (uint8_t)(body >> 8); out[15] = (uint8_t)body;
    inet_pton(AF_INET, src, out + 16);
    inet_pton(AF_INET, "10.0.0.1", out + 20);
    out[24] = 0xC0; out[25] = 0x01; out[26] = 0x20; out[27] = 0xFB;
    memset(out + 28, 0xEE, extra);
    return 16 + body;
}

static struct sockaddr_storage peer4(const char *ip) {
    struct sockaddr_storage ss; memset(&ss, 0, sizeof ss);
    struct sockaddr_in *a = (struct sockaddr_in *)&ss;
    a->sin_family = AF_INET; inet_pton(AF_INET, ip, &a->sin_addr);
    return ss;
}
static struct sockaddr_storage peer6(const char *ip) {
    struct sockaddr_storage ss; memset(&ss, 0, sizeof ss);
    struct sockaddr_in6 *a = (struct sockaddr_in6 *)&ss;
    a->sin6_family = AF_INET6; inet_pton(AF_INET6, ip, &a->sin6_addr);
    return ss;
}

int run_proxyproto_tests(void) {
    printf("test_proxyproto: v2 header (IPv4, IPv6, LOCAL, TLVs, partial, refused), "
           "trusted peers (addresses, CIDR, v4-mapped, strict parse)\n");
    uint8_t h[OC_PROXY_V2_MAX + 64];
    char src[46];

    /* The client's address comes out, and the length says where TLS begins. */
    size_t n = v4_header(h, "203.0.113.7", 0);
    CHECK(oc_proxy_v2_parse(h, n, src) == 28 && strcmp(src, "203.0.113.7") == 0);
    /* Bytes after the header are TLS's, and are not counted. */
    CHECK(oc_proxy_v2_parse(h, n + 30, src) == 28);
    /* TLVs a forwarder adds are skipped, not read. */
    n = v4_header(h, "203.0.113.7", 40);
    CHECK(oc_proxy_v2_parse(h, n, src) == 68 && strcmp(src, "203.0.113.7") == 0);

    /* Arriving in pieces: every prefix of a good header asks for more, none is refused. */
    n = v4_header(h, "198.51.100.9", 0);
    for (size_t i = 1; i < n; i++) CHECK(oc_proxy_v2_parse(h, i, src) == 0);

    /* IPv6. */
    memcpy(h, SIG, 12); h[12] = 0x21; h[13] = 0x21; h[14] = 0; h[15] = 36;
    inet_pton(AF_INET6, "2001:db8::7", h + 16);
    inet_pton(AF_INET6, "2001:db8::1", h + 32);
    memset(h + 48, 0, 4);
    CHECK(oc_proxy_v2_parse(h, 52, src) == 52 && strcmp(src, "2001:db8::7") == 0);

    /* LOCAL — the forwarder's own health check: consumed, and names nobody. */
    memcpy(h, SIG, 12); h[12] = 0x20; h[13] = 0x00; h[14] = 0; h[15] = 0;
    CHECK(oc_proxy_v2_parse(h, 16, src) == 16 && src[0] == '\0');

    /* Refused: a TLS ClientHello (what an untrusted peer would be sending), v1's
     * text form, another version, an unknown command, UDP, a body too short for
     * its family, and a length past what we will buffer. */
    static const uint8_t HELLO[] = { 0x16, 0x03, 0x01, 0x02, 0x00 };
    CHECK(oc_proxy_v2_parse(HELLO, sizeof HELLO, src) == -1);
    CHECK(oc_proxy_v2_parse((const uint8_t *)"PROXY TCP4 1.2.3.4 5.6.7.8 1 2\r\n", 32, src) == -1);
    n = v4_header(h, "203.0.113.7", 0);
    h[12] = 0x11; CHECK(oc_proxy_v2_parse(h, n, src) == -1);
    h[12] = 0x22; CHECK(oc_proxy_v2_parse(h, n, src) == -1);
    h[12] = 0x21; h[13] = 0x12; CHECK(oc_proxy_v2_parse(h, n, src) == -1);
    h[13] = 0x11; h[15] = 8; CHECK(oc_proxy_v2_parse(h, 24, src) == -1);
    h[14] = 0x03; h[15] = 0x00; CHECK(oc_proxy_v2_parse(h, 16, src) == -1);

    /* Who is believed. */
    char err[160];
    oc_trusted_proxies *none = oc_trusted_proxies_parse(NULL, err, sizeof err);
    struct sockaddr_storage p = peer4("127.0.0.1");
    CHECK(none != NULL && oc_trusted_proxies_match(none, &p) == 0);
    oc_trusted_proxies_free(none);

    oc_trusted_proxies *t = oc_trusted_proxies_parse(" 172.16.0.0/12, 192.0.2.7 ,fdaa::/16,", err, sizeof err);
    CHECK(t != NULL);
    if (t) {
        p = peer4("172.19.4.2");   CHECK(oc_trusted_proxies_match(t, &p) == 1);
        p = peer4("172.32.0.1");   CHECK(oc_trusted_proxies_match(t, &p) == 0);
        p = peer4("192.0.2.7");    CHECK(oc_trusted_proxies_match(t, &p) == 1);
        p = peer4("192.0.2.8");    CHECK(oc_trusted_proxies_match(t, &p) == 0);
        p = peer6("fdaa:0:1::3");  CHECK(oc_trusted_proxies_match(t, &p) == 1);
        p = peer6("fdab::1");      CHECK(oc_trusted_proxies_match(t, &p) == 0);
        /* An IPv4 peer seen through a dual-stack socket is still that IPv4 peer. */
        p = peer6("::ffff:172.19.4.2"); CHECK(oc_trusted_proxies_match(t, &p) == 1);
        p = peer6("::ffff:8.8.8.8");    CHECK(oc_trusted_proxies_match(t, &p) == 0);
        oc_trusted_proxies_free(t);
    }
    /* /0 is everybody, and is what somebody means when they write it. */
    t = oc_trusted_proxies_parse("0.0.0.0/0", err, sizeof err);
    p = peer4("8.8.8.8");
    CHECK(t && oc_trusted_proxies_match(t, &p) == 1);
    oc_trusted_proxies_free(t);

    static const char *const BAD[] = { "fly", "10.0.0.0/33", "fdaa::/129", "10.0.0.0/", "10.0.0.0/x",
                                       "10.0.0.0/-1", "1.2.3", "10.0.0.1,nonsense" };
    for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
        err[0] = '\0';
        oc_trusted_proxies *b = oc_trusted_proxies_parse(BAD[i], err, sizeof err);
        if (b) printf("  accepted: %s\n", BAD[i]);
        CHECK(b == NULL && err[0] != '\0');
        oc_trusted_proxies_free(b);
    }
    return failures;
}
