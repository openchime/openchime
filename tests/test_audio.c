/* Tests for the audio relay sidecar (daemon/audio_sidecar.c, REQ-150/151). Runs
 * the relay on a thread with a socketpair for the daemon IPC and a real UDP
 * socket, plus mock UDP "clients", and checks: an authorized participant's audio
 * is forwarded to its call-mates (tagged with the sender's user id) and to no
 * one else; a different call is isolated; a participant's token used from any
 * address but the one it was first used from neither speaks nor redirects their
 * audio; and a REVOKE stops forwarding. Then, on the daemon's own dual-stack
 * socket: a token led by a routing prefix; an answer that comes from the address
 * the client sent to, not whichever one the kernel would pick; and IPv4 and IPv6
 * participants in one call. */

#include "audio.h"
#include "check.h"
#include "listen.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop;
static int g_ipc, g_udp;

static void *sidecar_thread(void *arg) {
    (void)arg;
    oc_audio_sidecar_run(g_ipc, g_udp, &g_stop);
    return NULL;
}

static void wr_u64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (56 - 8 * i)); }
static uint64_t rd_u64(const uint8_t *p) { uint64_t v = 0; for (int i = 0; i < 8; i++) v = (v << 8) | p[i]; return v; }

/* Send a length-prefixed IPC message (type + payload) to the sidecar. */
static void ipc_send(int fd, uint8_t type, const uint8_t *payload, size_t plen) {
    uint8_t buf[64]; uint32_t mlen = (uint32_t)(1 + plen);
    buf[0] = (uint8_t)(mlen >> 24); buf[1] = (uint8_t)(mlen >> 16);
    buf[2] = (uint8_t)(mlen >> 8);  buf[3] = (uint8_t)mlen;
    buf[4] = type;
    memcpy(buf + 5, payload, plen);
    ssize_t n = write(fd, buf, 5 + plen); (void)n;
}

static void authorize_n(int fd, uint64_t call, uint64_t user, const uint8_t *tok, size_t tlen) {
    uint8_t p[16 + OC_AUDIO_TOKEN_MAX]; wr_u64(p, call); wr_u64(p + 8, user); memcpy(p + 16, tok, tlen);
    ipc_send(fd, OC_AUDIO_IPC_AUTHORIZE, p, 16 + tlen);
}
static void authorize(int fd, uint64_t call, uint64_t user, const uint8_t *tok) {
    authorize_n(fd, call, user, tok, OC_AUDIO_TOKEN_RAND);
}

/* A mock client UDP socket with a short receive timeout. */
static int mk_client(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct timeval tv = { 1, 0 };   /* 1 s — generous ceiling so a loaded CI
                                     * runner doesn't miss a forwarded datagram
                                     * (loopback UDP doesn't drop; this only waits
                                     * longer for a delayed one). */
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    return fd;
}

/* token + seq + payload -> sidecar; `to` NULL on a connected socket. */
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

/* Receive one forwarded datagram; returns payload len or -1 on timeout. Fills
 * *sender + *seq. */
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
    for (;;) { if (recv(fd, b, sizeof b, 0) < 0) break; }
}

static int addressing_tests(void);

int run_audio_tests(void) {
    printf("test_audio: sidecar relay — authorized fan-out, call isolation, revoke\n");

    /* Bound UDP socket for the sidecar + a socketpair for the daemon IPC. */
    g_udp = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET; sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK); sa.sin_port = 0;
    CHECK(bind(g_udp, (struct sockaddr *)&sa, sizeof sa) == 0);
    socklen_t sl = sizeof sa; getsockname(g_udp, (struct sockaddr *)&sa, &sl);
    struct sockaddr_in relay = sa;   /* where clients send */

    int sv[2]; CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    g_ipc = sv[1];
    g_stop = 0;
    pthread_t th; CHECK(pthread_create(&th, NULL, sidecar_thread, NULL) == 0);

    uint8_t tA[16], tB[16], tC[16], tD[16];
    memset(tA, 0xA1, 16); memset(tB, 0xB2, 16); memset(tC, 0xC3, 16); memset(tD, 0xD4, 16);
    /* A, B, C in call 100; D in a different call 200. */
    authorize(sv[0], 100, 1, tA);
    authorize(sv[0], 100, 2, tB);
    authorize(sv[0], 100, 3, tC);
    authorize(sv[0], 200, 4, tD);
    usleep(100000);   /* let the IPC apply */

    int cA = mk_client(), cB = mk_client(), cC = mk_client(), cD = mk_client();
    /* Each client sends a hello so the sidecar learns its address. */
    udp_send(cA, &relay, tA, 0, NULL); udp_send(cB, &relay, tB, 0, NULL);
    udp_send(cC, &relay, tC, 0, NULL); udp_send(cD, &relay, tD, 0, NULL);
    usleep(100000);
    drain(cA); drain(cB); drain(cC); drain(cD);   /* discard any forwarded helloes */

    /* A speaks -> B and C receive it tagged sender=1; D (other call) does not. */
    udp_send(cA, &relay, tA, 42, "hello-audio");
    usleep(50000);
    uint64_t sender; uint16_t seq; char body[64];
    int nb = udp_recv(cB, &sender, &seq, body, sizeof body);
    CHECK(nb == 11 && sender == 1 && seq == 42 && memcmp(body, "hello-audio", 11) == 0);
    int nc = udp_recv(cC, &sender, &seq, body, sizeof body);
    CHECK(nc == 11 && sender == 1 && seq == 42);
    int nd = udp_recv(cD, &sender, &seq, body, sizeof body);
    CHECK(nd == -1);                               /* call isolation */
    int na = udp_recv(cA, &sender, &seq, body, sizeof body);
    CHECK(na == -1);                               /* sender doesn't get its own */

    /* A's token from somewhere else: the address is bound to where A first sent
     * from, so a stranger holding A's token -- anyone who saw one of A's packets
     * -- can neither speak as A nor take A's audio. */
    int cX = mk_client();
    drain(cA); drain(cB); drain(cC);
    udp_send(cX, &relay, tA, 50, "spoofed");
    usleep(50000);
    nb = udp_recv(cB, &sender, &seq, body, sizeof body);
    CHECK(nb == -1);                               /* the forged packet is not relayed */
    udp_send(cB, &relay, tB, 51, "for-A");        /* and A's audio still goes to A */
    usleep(50000);
    na = udp_recv(cA, &sender, &seq, body, sizeof body);
    CHECK(na == 5 && sender == 2 && seq == 51 && memcmp(body, "for-A", 5) == 0);
    int nx = udp_recv(cX, &sender, &seq, body, sizeof body);
    CHECK(nx == -1);                               /* nothing reaches the stranger */
    close(cX);
    drain(cC);

    /* Revoke C; A speaks again -> B still hears, C does not. */
    ipc_send(sv[0], OC_AUDIO_IPC_REVOKE, tC, 16);
    usleep(100000);
    drain(cB); drain(cC);
    udp_send(cA, &relay, tA, 43, "again");
    usleep(50000);
    nb = udp_recv(cB, &sender, &seq, body, sizeof body);
    CHECK(nb == 5 && sender == 1 && seq == 43);
    nc = udp_recv(cC, &sender, &seq, body, sizeof body);
    CHECK(nc == -1);                               /* revoked participant is silent */

    g_stop = 1;
    /* Nudge the loop so epoll_wait returns promptly, then join. */
    shutdown(sv[0], SHUT_RDWR);
    pthread_join(th, NULL);
    close(cA); close(cB); close(cC); close(cD);
    close(sv[0]); close(sv[1]); close(g_udp);
    return addressing_tests();
}

/* A client socket connected to `to`: the kernel then delivers only datagrams
 * whose source is exactly `to`, which is what a NAT, or a platform's UDP edge,
 * does with an answer from anywhere else. */
static int mk_connected(int family, const struct sockaddr *to, socklen_t tolen) {
    int fd = socket(family, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    struct timeval tv = { 1, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (connect(fd, to, tolen) != 0) { close(fd); return -1; }
    return fd;
}

static int addressing_tests(void) {
    printf("test_audio: relay on the daemon's socket — prefixed tokens, answers from the address sent to, both families\n");

    /* The socket the daemon binds: every address, both families. */
    const char *op = NULL;
    g_udp = oc_listen_bind(SOCK_DGRAM, 0, &op);
    CHECK(g_udp >= 0);
    if (g_udp < 0) return failures;
    struct sockaddr_storage self; socklen_t sl = sizeof self;
    getsockname(g_udp, (struct sockaddr *)&self, &sl);
    int dual = self.ss_family == AF_INET6;
    uint16_t port = ntohs(dual ? ((struct sockaddr_in6 *)&self)->sin6_port
                               : ((struct sockaddr_in *)&self)->sin_port);

    int sv[2]; CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    g_ipc = sv[1];
    g_stop = 0;
    pthread_t th; CHECK(pthread_create(&th, NULL, sidecar_thread, NULL) == 0);

    /* Tokens as a daemon with OPENCHIME_AUDIO_TOKEN_PREFIX set issues them: the
     * longest there is, sixteen bytes of prefix and sixteen random. */
    uint8_t tA[OC_AUDIO_TOKEN_MAX], tB[OC_AUDIO_TOKEN_MAX], tC[OC_AUDIO_TOKEN_MAX];
    memset(tA, 0x7E, OC_AUDIO_PREFIX_MAX); memset(tB, 0x7E, OC_AUDIO_PREFIX_MAX);
    memset(tC, 0x7E, OC_AUDIO_PREFIX_MAX);
    memset(tA + OC_AUDIO_PREFIX_MAX, 0xA1, OC_AUDIO_TOKEN_RAND);
    memset(tB + OC_AUDIO_PREFIX_MAX, 0xB2, OC_AUDIO_TOKEN_RAND);
    memset(tC + OC_AUDIO_PREFIX_MAX, 0xC3, OC_AUDIO_TOKEN_RAND);
    authorize_n(sv[0], 300, 11, tA, sizeof tA);
    authorize_n(sv[0], 300, 12, tB, sizeof tB);
    authorize_n(sv[0], 300, 13, tC, sizeof tC);
    usleep(100000);

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
    udp_send_n(cA, NULL, 0, tA, sizeof tA, 0, NULL);
    udp_send_n(cB, NULL, 0, tB, sizeof tB, 0, NULL);
    usleep(100000);
    drain(cA); drain(cB);

    udp_send_n(cA, NULL, 0, tA, sizeof tA, 61, "from-a");
    uint64_t sender; uint16_t seq; char body[64];
    int nb = udp_recv(cB, &sender, &seq, body, sizeof body);
    CHECK(nb == 6 && sender == 11 && seq == 61 && memcmp(body, "from-a", 6) == 0);
    udp_send_n(cB, NULL, 0, tB, sizeof tB, 62, "from-b");
    int na = udp_recv(cA, &sender, &seq, body, sizeof body);
    CHECK(na == 6 && sender == 12 && seq == 62 && memcmp(body, "from-b", 6) == 0);

    /* The prefix alone, or the prefix and a wrong rest, is nobody. */
    uint8_t tX[OC_AUDIO_TOKEN_MAX]; memcpy(tX, tA, sizeof tX); tX[sizeof tX - 1] ^= 1;
    udp_send_n(cA, NULL, 0, tX, sizeof tX, 63, "forged");
    CHECK(udp_recv(cB, &sender, &seq, body, sizeof body) == -1);

    /* C over IPv6, in the same call: a dual-stack relay carries both families
     * in one call. A host with no IPv6 has a relay without it, and nothing to
     * check. */
    if (dual) {
        struct sockaddr_in6 v6; memset(&v6, 0, sizeof v6);
        v6.sin6_family = AF_INET6; v6.sin6_port = htons(port); v6.sin6_addr = in6addr_loopback;
        int cC = mk_connected(AF_INET6, (struct sockaddr *)&v6, sizeof v6);
        CHECK(cC >= 0);
        udp_send_n(cC, NULL, 0, tC, sizeof tC, 0, NULL);
        usleep(100000);
        drain(cA); drain(cB); drain(cC);
        udp_send_n(cC, NULL, 0, tC, sizeof tC, 64, "over-v6");
        nb = udp_recv(cB, &sender, &seq, body, sizeof body);
        CHECK(nb == 7 && sender == 13 && seq == 64 && memcmp(body, "over-v6", 7) == 0);
        udp_send_n(cA, NULL, 0, tA, sizeof tA, 65, "to-v6");
        int nc = udp_recv(cC, &sender, &seq, body, sizeof body);
        CHECK(nc == 5 && sender == 11 && seq == 65 && memcmp(body, "to-v6", 5) == 0);
        close(cC);
    } else {
        printf("test_audio: no IPv6 on this host; the relay is IPv4 alone\n");
    }

    /* B goes quiet: the sweep's GONE names B's token whole, prefix and all,
     * which is how the daemon finds whom to take out of the call. */
    ipc_send(sv[0], OC_AUDIO_IPC_REVOKE, tC, sizeof tC);   /* B alone falls silent */
    oc_audio_sidecar_set_silence_ms(600);
    uint8_t gone[4 + 1 + OC_AUDIO_TOKEN_MAX];
    struct timeval nap = { 0, 250000 };
    setsockopt(sv[0], SOL_SOCKET, SO_RCVTIMEO, &nap, sizeof nap);
    size_t got = 0;
    for (int i = 0; i < 12 && got < sizeof gone; i++) {
        udp_send_n(cA, NULL, 0, tA, sizeof tA, (uint16_t)(70 + i), NULL);
        ssize_t r = recv(sv[0], gone + got, sizeof gone - got, 0);
        if (r > 0) got += (size_t)r;
    }
    CHECK(got == sizeof gone && gone[3] == 1 + OC_AUDIO_TOKEN_MAX && gone[4] == OC_AUDIO_IPC_GONE);
    CHECK(memcmp(gone + 5, tB, sizeof tB) == 0);
    oc_audio_sidecar_set_silence_ms(0);

    g_stop = 1;
    shutdown(sv[0], SHUT_RDWR);
    pthread_join(th, NULL);
    close(cA); close(cB);
    close(sv[0]); close(sv[1]); close(g_udp);
    return failures;
}
