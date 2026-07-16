/* Tests for the audio relay sidecar (daemon/audio_sidecar.c, REQ-150/151). Runs
 * the relay on a thread with a socketpair for the daemon IPC and a real UDP
 * socket, plus mock UDP "clients", and checks: an authorized participant's audio
 * is forwarded to its call-mates (tagged with the sender's user id) and to no
 * one else; a different call is isolated; and a REVOKE stops forwarding. */

#include "audio.h"
#include "check.h"

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

static void authorize(int fd, uint64_t call, uint64_t user, const uint8_t *tok) {
    uint8_t p[32]; wr_u64(p, call); wr_u64(p + 8, user); memcpy(p + 16, tok, OC_AUDIO_TOKEN_LEN);
    ipc_send(fd, OC_AUDIO_IPC_AUTHORIZE, p, sizeof p);
}

/* A mock client UDP socket with a short receive timeout. */
static int mk_client(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct timeval tv = { 0, 300000 };   /* 300 ms */
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    return fd;
}

/* token + seq + payload -> sidecar. */
static void udp_send(int fd, const struct sockaddr_in *to, const uint8_t *tok, uint16_t seq,
                     const char *payload) {
    uint8_t pkt[128]; size_t pl = payload ? strlen(payload) : 0;
    memcpy(pkt, tok, OC_AUDIO_TOKEN_LEN);
    pkt[OC_AUDIO_TOKEN_LEN] = (uint8_t)(seq >> 8); pkt[OC_AUDIO_TOKEN_LEN + 1] = (uint8_t)seq;
    if (pl) memcpy(pkt + OC_AUDIO_C2S_HDR, payload, pl);
    sendto(fd, pkt, OC_AUDIO_C2S_HDR + pl, 0, (const struct sockaddr *)to, sizeof *to);
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
    return failures;
}
