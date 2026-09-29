/* Tests for the call media relay (daemon/relay.c, REQ-150/151). The relay is a
 * library the net loop drives, so these drive it the same way, on this thread:
 * a real UDP socket, mock UDP clients, and oc_relay_on_readable called when the
 * socket is readable. They check that an authorized participant's audio reaches
 * its call-mates (tagged with the sender's user id) and no one else; that a
 * call is isolated; that a token used from any address but the one it was first
 * used from neither speaks nor redirects; that a revoke stops forwarding; that a
 * keepalive is answered to its sender alone; that a full call's fan-out reaches
 * every participant; that two tokens sharing an index key are not confused; and
 * that the silence sweep reports whom it dropped. Then, on the daemon's own
 * dual-stack socket: a token led by a routing prefix, an answer from the address
 * the client sent to, and IPv4 and IPv6 participants in one call. */

#include "relay.h"
#include "check.h"
#include "listen.h"
#include "protocol.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static uint64_t rd_u64(const uint8_t *p) { uint64_t v = 0; for (int i = 0; i < 8; i++) v = (v << 8) | p[i]; return v; }

/* Let the relay handle what has arrived: wait (briefly) for the socket to be
 * readable, then relay it all. */
static void pump(oc_relay *r, int fd) {
    struct pollfd pfd = { fd, POLLIN, 0 };
    if (poll(&pfd, 1, 200) > 0) while (oc_relay_on_readable(r, 64)) {}
}

/* The relay here is pumped on the test's own thread, so anything it forwards is
 * on the receiving socket before the test reads: a read that finds nothing in
 * 50 ms has found nothing, and a read that must find something finds it at
 * once. */
static int mk_client(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct timeval tv = { 0, 50000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    return fd;
}

/* token + seq + payload -> relay; `to` NULL on a connected socket. */
static void udp_send_n(int fd, const struct sockaddr *to, socklen_t tolen, const uint8_t *tok,
                       size_t tlen, uint16_t seq, const char *payload) {
    uint8_t pkt[128]; size_t pl = payload ? strlen(payload) : 0;
    memcpy(pkt, tok, tlen);
    pkt[tlen] = (uint8_t)(seq >> 8); pkt[tlen + 1] = (uint8_t)seq;
    if (pl) memcpy(pkt + tlen + 2, payload, pl);
    sendto(fd, pkt, tlen + 2 + pl, 0, to, to ? tolen : 0);
}
static void udp_send(int fd, const struct sockaddr_in *to, const uint8_t *tok, uint16_t seq,
                     const char *payload) {
    udp_send_n(fd, (const struct sockaddr *)to, sizeof *to, tok, OC_AUDIO_TOKEN_RAND, seq, payload);
}

/* One forwarded datagram: payload length, or -1 on timeout. */
static int udp_recv(int fd, uint64_t *sender, uint16_t *seq, char *out, size_t cap) {
    uint8_t pkt[256];
    ssize_t n = recv(fd, pkt, sizeof pkt, 0);
    if (n < (ssize_t)OC_AUDIO_S2C_HDR) return -1;
    *sender = rd_u64(pkt);
    *seq = (uint16_t)((pkt[8] << 8) | pkt[9]);
    size_t pl = (size_t)n - OC_AUDIO_S2C_HDR;
    if (pl > cap) pl = cap;
    memcpy(out, pkt + OC_AUDIO_S2C_HDR, pl);
    return (int)pl;
}

static void drain(int fd) {
    uint8_t b[256];
    while (recv(fd, b, sizeof b, MSG_DONTWAIT) >= 0) {}
}

static struct { uint8_t t[OC_AUDIO_TOKEN_MAX]; size_t len; int n; } g_gone;
static void on_gone(void *ctx, const uint8_t *token, size_t len) {
    (void)ctx;
    memcpy(g_gone.t, token, len);
    g_gone.len = len;
    g_gone.n++;
}

/* The last packet the relay handed back for a participant on the connection. */
static struct { uint64_t conn, sender; uint16_t seq; uint8_t ct[64]; size_t len; int n; } g_tcp;
static void on_tcp(void *ctx, uint64_t conn_id, uint64_t sender, uint16_t seq,
                   const uint8_t *ct, size_t len) {
    (void)ctx;
    g_tcp.conn = conn_id; g_tcp.sender = sender; g_tcp.seq = seq;
    g_tcp.len = len < sizeof g_tcp.ct ? len : sizeof g_tcp.ct;
    memcpy(g_tcp.ct, ct, g_tcp.len);
    g_tcp.n++;
}

static int addressing_tests(void);

int run_audio_tests(void) {
    printf("test_audio: relay -- authorized fan-out, call isolation, address binding, revoke, "
           "keepalive echo, a full call, index collisions, the silence sweep\n");

    int udp = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in relay_addr; memset(&relay_addr, 0, sizeof relay_addr);
    relay_addr.sin_family = AF_INET; relay_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(udp, (struct sockaddr *)&relay_addr, sizeof relay_addr) == 0);
    socklen_t sl = sizeof relay_addr; getsockname(udp, (struct sockaddr *)&relay_addr, &sl);
    oc_relay_hooks hooks = { on_gone, on_tcp };
    oc_relay *r = oc_relay_open(udp, &hooks, NULL);
    CHECK(r != NULL);
    if (!r) return failures;

    uint8_t tA[16], tB[16], tC[16], tD[16];
    memset(tA, 0xA1, 16); memset(tB, 0xB2, 16); memset(tC, 0xC3, 16); memset(tD, 0xD4, 16);
    /* A, B, C in call 100; D in call 200. */
    CHECK(oc_relay_authorize(r, 100, 1, 0, tA, 16) == 0);
    CHECK(oc_relay_authorize(r, 100, 2, 0, tB, 16) == 0);
    CHECK(oc_relay_authorize(r, 100, 3, 0, tC, 16) == 0);
    CHECK(oc_relay_authorize(r, 200, 4, 0, tD, 16) == 0);
    CHECK(oc_relay_count(r) == 4);
    CHECK(oc_relay_authorize(r, 100, 5, 0, tA, 8) == -1);   /* not a usable token length */

    int cA = mk_client(), cB = mk_client(), cC = mk_client(), cD = mk_client();
    /* Each client's first packet teaches the relay its address. A keepalive is
     * answered to its sender alone: each hears its own, and nobody else's. */
    uint64_t sender; uint16_t seq; char body[64];
    int cl[4] = { cA, cB, cC, cD };
    const uint8_t *tk[4] = { tA, tB, tC, tD };
    for (int i = 0; i < 4; i++) {
        udp_send(cl[i], &relay_addr, tk[i], 0, NULL);
        pump(r, udp);
        CHECK(udp_recv(cl[i], &sender, &seq, body, sizeof body) == 0 && sender == (uint64_t)(i + 1));
    }
    for (int i = 0; i < 4; i++) CHECK(udp_recv(cl[i], &sender, &seq, body, sizeof body) == -1);

    /* A speaks: B and C hear it as user 1; D (another call) and A do not. */
    udp_send(cA, &relay_addr, tA, 42, "hello-audio");
    pump(r, udp);
    int nb = udp_recv(cB, &sender, &seq, body, sizeof body);
    CHECK(nb == 11 && sender == 1 && seq == 42 && memcmp(body, "hello-audio", 11) == 0);
    int nc = udp_recv(cC, &sender, &seq, body, sizeof body);
    CHECK(nc == 11 && sender == 1 && seq == 42);
    CHECK(udp_recv(cD, &sender, &seq, body, sizeof body) == -1);   /* call isolation */
    CHECK(udp_recv(cA, &sender, &seq, body, sizeof body) == -1);   /* not to the speaker */

    /* A's token from somewhere else: neither speaks as A nor takes A's audio. */
    int cX = mk_client();
    udp_send(cX, &relay_addr, tA, 50, "spoofed");
    pump(r, udp);
    CHECK(udp_recv(cB, &sender, &seq, body, sizeof body) == -1);
    udp_send(cB, &relay_addr, tB, 51, "for-A");
    pump(r, udp);
    int na = udp_recv(cA, &sender, &seq, body, sizeof body);
    CHECK(na == 5 && sender == 2 && seq == 51 && memcmp(body, "for-A", 5) == 0);
    CHECK(udp_recv(cX, &sender, &seq, body, sizeof body) == -1);
    close(cX);
    drain(cC);

    /* Revoke C: A speaks, B hears, C does not. */
    oc_relay_revoke(r, tC, 16);
    CHECK(oc_relay_count(r) == 3);
    udp_send(cA, &relay_addr, tA, 43, "again");
    pump(r, udp);
    nb = udp_recv(cB, &sender, &seq, body, sizeof body);
    CHECK(nb == 5 && sender == 1 && seq == 43);
    CHECK(udp_recv(cC, &sender, &seq, body, sizeof body) == -1);

    /* Re-authorizing a token moves it: D joins call 100 and now hears A. */
    CHECK(oc_relay_authorize(r, 100, 4, 0, tD, 16) == 0);
    udp_send(cA, &relay_addr, tA, 44, "moved");
    pump(r, udp);
    CHECK(udp_recv(cD, &sender, &seq, body, sizeof body) == 5 && sender == 1);
    drain(cB);

    /* Two tokens that share the index key (the same eight random bytes) are two
     * tokens, not one: the second is refused rather than taking the first's
     * place. */
    uint8_t tK[16]; memcpy(tK, tA, 16); tK[15] ^= 0x55;
    CHECK(oc_relay_authorize(r, 100, 9, 0, tK, 16) == -1);
    udp_send(cB, &relay_addr, tB, 45, "still-A");
    pump(r, udp);
    CHECK(udp_recv(cA, &sender, &seq, body, sizeof body) == 7 && sender == 2);

    /* A full call: thirty-two participants, one speaks, thirty-one hear it. */
    {
        enum { N = OC_MAX_CALL_PARTICIPANTS };
        int cs[N]; uint8_t ts[N][16];
        for (int i = 0; i < N; i++) {
            memset(ts[i], 0x40 + i, 16);
            ts[i][0] = 0xEE;
            CHECK(oc_relay_authorize(r, 900, 1000 + (uint64_t)i, 0, ts[i], 16) == 0);
            cs[i] = mk_client();
            udp_send(cs[i], &relay_addr, ts[i], 0, NULL);
            pump(r, udp);
            drain(cs[i]);
        }
        udp_send(cs[0], &relay_addr, ts[0], 7, "all-of-you");
        pump(r, udp);
        int heard = 0;
        for (int i = 1; i < N; i++)
            if (udp_recv(cs[i], &sender, &seq, body, sizeof body) == 10 && sender == 1000) heard++;
        CHECK(heard == N - 1);
        CHECK(udp_recv(cs[0], &sender, &seq, body, sizeof body) == -1);
        for (int i = 0; i < N; i++) { oc_relay_revoke(r, ts[i], 16); close(cs[i]); }
    }

    /* A participant on the connection (E, connection 77, user 5) and the rest
     * on UDP hear each other, each by its own transport; a keepalive over the
     * connection comes back over it alone; and one UDP packet from E moves it
     * back to UDP. */
    {
        uint8_t tE[16]; memset(tE, 0xE5, 16);
        CHECK(oc_relay_authorize(r, 100, 5, 77, tE, 16) == 0);
        drain(cA); drain(cB); drain(cD);
        CHECK(oc_relay_from_tcp(r, 77, 5, (const uint8_t *)"tcp-hi", 6) == 0);
        nb = udp_recv(cB, &sender, &seq, body, sizeof body);
        CHECK(nb == 6 && sender == 5 && seq == 5 && memcmp(body, "tcp-hi", 6) == 0);
        CHECK(udp_recv(cA, &sender, &seq, body, sizeof body) == 6 && sender == 5);
        g_tcp.n = 0;
        udp_send(cA, &relay_addr, tA, 46, "to-tcp");
        pump(r, udp);
        CHECK(g_tcp.n == 1 && g_tcp.conn == 77 && g_tcp.sender == 1 && g_tcp.seq == 46);
        CHECK(g_tcp.len == 6 && memcmp(g_tcp.ct, "to-tcp", 6) == 0);
        drain(cB); drain(cD);
        g_tcp.n = 0;
        CHECK(oc_relay_from_tcp(r, 77, 6, NULL, 0) == 0);   /* a keepalive */
        CHECK(g_tcp.n == 1 && g_tcp.conn == 77 && g_tcp.sender == 5 && g_tcp.len == 0);
        CHECK(udp_recv(cB, &sender, &seq, body, sizeof body) == -1);
        CHECK(oc_relay_from_tcp(r, 78, 1, (const uint8_t *)"x", 1) == -1);   /* no such participant */
        int cE = mk_client();
        udp_send(cE, &relay_addr, tE, 7, NULL);
        pump(r, udp);
        drain(cE);
        g_tcp.n = 0;
        udp_send(cA, &relay_addr, tA, 47, "back-to-udp");
        pump(r, udp);
        CHECK(udp_recv(cE, &sender, &seq, body, sizeof body) == 11 && sender == 1 && seq == 47);
        CHECK(g_tcp.n == 0);
        oc_relay_revoke(r, tE, 16);
        close(cE);
        drain(cB); drain(cD);
    }

    /* The sweep: B falls silent while A keeps talking; B is dropped and reported
     * whole, A is not. */
    oc_relay_set_silence_ms(r, 60);
    g_gone.n = 0;
    for (int i = 0; i < 40 && !g_gone.n; i++) {
        udp_send(cA, &relay_addr, tA, (uint16_t)(80 + i), NULL);
        udp_send(cD, &relay_addr, tD, (uint16_t)(80 + i), NULL);
        pump(r, udp);
        usleep(20000);
        oc_relay_sweep(r);
    }
    CHECK(g_gone.n == 1 && g_gone.len == 16 && memcmp(g_gone.t, tB, 16) == 0);
    CHECK(oc_relay_count(r) == 2);
    oc_relay_set_silence_ms(r, 0);

    oc_relay_close(r);
    close(cA); close(cB); close(cC); close(cD); close(udp);
    return addressing_tests();
}

/* A client socket connected to `to`: the kernel then delivers only datagrams
 * whose source is exactly `to`, which is what a NAT, or a platform's UDP edge,
 * does with an answer from anywhere else. */
static int mk_connected(int family, const struct sockaddr *to, socklen_t tolen) {
    int fd = socket(family, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    struct timeval tv = { 0, 50000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (connect(fd, to, tolen) != 0) { close(fd); return -1; }
    return fd;
}

static int addressing_tests(void) {
    printf("test_audio: relay on the daemon's socket -- prefixed tokens, answers from the address sent to, both families\n");

    const char *op = NULL;
    int udp = oc_listen_bind(SOCK_DGRAM, 0, &op);
    CHECK(udp >= 0);
    if (udp < 0) return failures;
    struct sockaddr_storage self; socklen_t sl = sizeof self;
    getsockname(udp, (struct sockaddr *)&self, &sl);
    int dual = self.ss_family == AF_INET6;
    uint16_t port = ntohs(dual ? ((struct sockaddr_in6 *)&self)->sin6_port
                               : ((struct sockaddr_in *)&self)->sin_port);
    oc_relay *r = oc_relay_open(udp, NULL, NULL);
    CHECK(r != NULL);
    if (!r) { close(udp); return failures; }

    /* Tokens as a daemon with OPENCHIME_AUDIO_TOKEN_PREFIX set issues them: the
     * longest there is, sixteen bytes of prefix and sixteen random. */
    uint8_t tA[OC_AUDIO_TOKEN_MAX], tB[OC_AUDIO_TOKEN_MAX], tC[OC_AUDIO_TOKEN_MAX];
    memset(tA, 0x7E, OC_AUDIO_PREFIX_MAX); memset(tB, 0x7E, OC_AUDIO_PREFIX_MAX);
    memset(tC, 0x7E, OC_AUDIO_PREFIX_MAX);
    memset(tA + OC_AUDIO_PREFIX_MAX, 0xA1, OC_AUDIO_TOKEN_RAND);
    memset(tB + OC_AUDIO_PREFIX_MAX, 0xB2, OC_AUDIO_TOKEN_RAND);
    memset(tC + OC_AUDIO_PREFIX_MAX, 0xC3, OC_AUDIO_TOKEN_RAND);
    CHECK(oc_relay_authorize(r, 300, 11, 0, tA, sizeof tA) == 0);
    CHECK(oc_relay_authorize(r, 300, 12, 0, tB, sizeof tB) == 0);
    CHECK(oc_relay_authorize(r, 300, 13, 0, tC, sizeof tC) == 0);

    /* A and B reach the relay at 127.0.0.5, an address the host has but would
     * never choose as the source of a packet to 127.0.0.1 -- so each hears the
     * other only if the relay answers from where it was sent to. This is Fly's
     * `fly-global-services` in miniature. */
    struct sockaddr_in alt; memset(&alt, 0, sizeof alt);
    alt.sin_family = AF_INET; alt.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.5", &alt.sin_addr);
    int cA = mk_connected(AF_INET, (struct sockaddr *)&alt, sizeof alt);
    int cB = mk_connected(AF_INET, (struct sockaddr *)&alt, sizeof alt);
    CHECK(cA >= 0 && cB >= 0);
    udp_send_n(cA, NULL, 0, tA, sizeof tA, 0, NULL); pump(r, udp);
    udp_send_n(cB, NULL, 0, tB, sizeof tB, 0, NULL); pump(r, udp);
    uint64_t sender; uint16_t seq; char body[64];
    /* The keepalive echo arrives: the answer came from 127.0.0.5. */
    CHECK(udp_recv(cA, &sender, &seq, body, sizeof body) == 0 && sender == 11);
    CHECK(udp_recv(cB, &sender, &seq, body, sizeof body) == 0 && sender == 12);

    udp_send_n(cA, NULL, 0, tA, sizeof tA, 61, "from-a"); pump(r, udp);
    int nb = udp_recv(cB, &sender, &seq, body, sizeof body);
    CHECK(nb == 6 && sender == 11 && seq == 61 && memcmp(body, "from-a", 6) == 0);
    udp_send_n(cB, NULL, 0, tB, sizeof tB, 62, "from-b"); pump(r, udp);
    int na = udp_recv(cA, &sender, &seq, body, sizeof body);
    CHECK(na == 6 && sender == 12 && seq == 62 && memcmp(body, "from-b", 6) == 0);

    /* The prefix and a wrong rest is nobody. */
    uint8_t tX[OC_AUDIO_TOKEN_MAX]; memcpy(tX, tA, sizeof tX); tX[sizeof tX - 1] ^= 1;
    udp_send_n(cA, NULL, 0, tX, sizeof tX, 63, "forged"); pump(r, udp);
    CHECK(udp_recv(cB, &sender, &seq, body, sizeof body) == -1);

    /* C over IPv6, in the same call. A host with no IPv6 has a relay without it,
     * and nothing to check. */
    if (dual) {
        struct sockaddr_in6 v6; memset(&v6, 0, sizeof v6);
        v6.sin6_family = AF_INET6; v6.sin6_port = htons(port); v6.sin6_addr = in6addr_loopback;
        int cC = mk_connected(AF_INET6, (struct sockaddr *)&v6, sizeof v6);
        CHECK(cC >= 0);
        udp_send_n(cC, NULL, 0, tC, sizeof tC, 0, NULL); pump(r, udp);
        drain(cA); drain(cB); drain(cC);
        udp_send_n(cC, NULL, 0, tC, sizeof tC, 64, "over-v6"); pump(r, udp);
        nb = udp_recv(cB, &sender, &seq, body, sizeof body);
        CHECK(nb == 7 && sender == 13 && seq == 64 && memcmp(body, "over-v6", 7) == 0);
        udp_send_n(cA, NULL, 0, tA, sizeof tA, 65, "to-v6"); pump(r, udp);
        int nc = udp_recv(cC, &sender, &seq, body, sizeof body);
        CHECK(nc == 5 && sender == 11 && seq == 65 && memcmp(body, "to-v6", 5) == 0);
        close(cC);
    } else {
        printf("test_audio: no IPv6 on this host; the relay is IPv4 alone\n");
    }

    oc_relay_close(r);
    close(cA); close(cB); close(udp);
    return failures;
}
