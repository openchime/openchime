/* Audio relay sidecar — see audio.h. A small epoll loop over one UDP socket and
 * the daemon IPC socket. It keeps a table of authorized participants (token ->
 * call + user + learned UDP address) and, for each incoming audio datagram,
 * forwards the opaque payload to the other participants of the same call. */

#include "audio.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define OC_AUDIO_MAX_PARTS 2048

typedef struct {
    int      used;
    uint8_t  token[OC_AUDIO_TOKEN_LEN];
    uint64_t call_id;
    uint64_t user_id;
    struct sockaddr_in addr;   /* learned from the first UDP packet */
    int      addr_known;
    uint64_t last_seen_ms;
} participant;

static participant g_parts[OC_AUDIO_MAX_PARTS];

static uint64_t now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static uint64_t rd_u64(const uint8_t *p) {
    uint64_t v = 0; for (int i = 0; i < 8; i++) v = (v << 8) | p[i]; return v;
}
static void wr_u64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (56 - 8 * i));
}

static participant *find_by_token(const uint8_t *token) {
    for (int i = 0; i < OC_AUDIO_MAX_PARTS; i++)
        if (g_parts[i].used && memcmp(g_parts[i].token, token, OC_AUDIO_TOKEN_LEN) == 0)
            return &g_parts[i];
    return NULL;
}

static void authorize(uint64_t call_id, uint64_t user_id, const uint8_t *token) {
    participant *e = find_by_token(token);
    if (!e) {
        for (int i = 0; i < OC_AUDIO_MAX_PARTS; i++)
            if (!g_parts[i].used) { e = &g_parts[i]; break; }
        if (!e) return;   /* table full */
        memset(e, 0, sizeof *e);
        e->used = 1;
        memcpy(e->token, token, OC_AUDIO_TOKEN_LEN);
    }
    e->call_id = call_id;
    e->user_id = user_id;
    e->last_seen_ms = now_ms();
}

static void revoke_token(const uint8_t *token) {
    participant *e = find_by_token(token);
    if (e) e->used = 0;
}

/* --- daemon IPC: buffer bytes, apply complete length-prefixed messages ----- */

static void apply_ipc(const uint8_t *msg, size_t len) {
    if (len < 1) return;
    uint8_t type = msg[0];
    const uint8_t *b = msg + 1; size_t n = len - 1;
    if (type == OC_AUDIO_IPC_AUTHORIZE && n == 16 + OC_AUDIO_TOKEN_LEN) {
        authorize(rd_u64(b), rd_u64(b + 8), b + 16);
    } else if (type == OC_AUDIO_IPC_REVOKE && n == OC_AUDIO_TOKEN_LEN) {
        revoke_token(b);
    }
}

/* Returns 0 to continue, -1 if the IPC peer (daemon) closed. */
static int on_ipc(int ipc_fd, uint8_t *buf, size_t *have, size_t cap) {
    for (;;) {
        ssize_t r = read(ipc_fd, buf + *have, cap - *have);
        if (r == 0) return -1;                  /* daemon gone */
        if (r < 0) return 0;                    /* drained (would block) */
        *have += (size_t)r;
        /* Parse as many complete frames as buffered: u32 len + (type+payload). */
        size_t off = 0;
        while (*have - off >= 4) {
            uint32_t mlen = ((uint32_t)buf[off] << 24) | ((uint32_t)buf[off+1] << 16) |
                            ((uint32_t)buf[off+2] << 8) | buf[off+3];
            if (mlen == 0 || mlen > cap) { off = *have; break; }   /* bad framing: drop */
            if (*have - off - 4 < mlen) break;                     /* need more */
            apply_ipc(buf + off + 4, mlen);
            off += 4 + mlen;
        }
        if (off) { memmove(buf, buf + off, *have - off); *have -= off; }
        if (*have == cap) *have = 0;            /* overlong garbage: reset */
    }
}

/* --- UDP: relay one datagram to the sender's call-mates ------------------- */

static void on_udp(int udp_fd) {
    uint8_t pkt[OC_AUDIO_MAX_PACKET];
    struct sockaddr_in src; socklen_t sl = sizeof src;
    ssize_t n = recvfrom(udp_fd, pkt, sizeof pkt, 0, (struct sockaddr *)&src, &sl);
    if (n < (ssize_t)OC_AUDIO_C2S_HDR) return;   /* too short to be a valid packet */

    participant *me = find_by_token(pkt);        /* token is the first 16 bytes */
    if (!me) return;                             /* unknown/revoked token -> ignore */
    me->addr = src; me->addr_known = 1;
    me->last_seen_ms = now_ms();

    const uint8_t *seq = pkt + OC_AUDIO_TOKEN_LEN;         /* 2 bytes */
    const uint8_t *payload = pkt + OC_AUDIO_C2S_HDR;
    size_t plen = (size_t)n - OC_AUDIO_C2S_HDR;

    /* Build the forwarded datagram: sender_user_id + seq + payload. */
    uint8_t out[OC_AUDIO_MAX_PACKET];
    wr_u64(out, me->user_id);
    out[8] = seq[0]; out[9] = seq[1];
    memcpy(out + OC_AUDIO_S2C_HDR, payload, plen);
    size_t olen = OC_AUDIO_S2C_HDR + plen;

    for (int i = 0; i < OC_AUDIO_MAX_PARTS; i++) {
        participant *e = &g_parts[i];
        if (!e->used || e == me || e->call_id != me->call_id || !e->addr_known) continue;
        sendto(udp_fd, out, olen, 0, (struct sockaddr *)&e->addr, sizeof e->addr);
    }
}

static void sweep_silent(void) {
    uint64_t now = now_ms();
    for (int i = 0; i < OC_AUDIO_MAX_PARTS; i++)
        if (g_parts[i].used && now - g_parts[i].last_seen_ms > OC_AUDIO_SILENCE_MS)
            g_parts[i].used = 0;
}

int oc_audio_sidecar_run(int ipc_fd, int udp_fd, volatile sig_atomic_t *stop) {
    memset(g_parts, 0, sizeof g_parts);
    /* Non-blocking so the drain loops (read/recvfrom until EAGAIN) never block. */
    for (int fd = 0; fd < 2; fd++) {
        int f = fd == 0 ? ipc_fd : udp_fd;
        int fl = fcntl(f, F_GETFL, 0); if (fl >= 0) fcntl(f, F_SETFL, fl | O_NONBLOCK);
    }
    int ep = epoll_create1(0);
    if (ep < 0) return -1;
    struct epoll_event ev;
    ev.events = EPOLLIN; ev.data.fd = ipc_fd; epoll_ctl(ep, EPOLL_CTL_ADD, ipc_fd, &ev);
    ev.events = EPOLLIN; ev.data.fd = udp_fd; epoll_ctl(ep, EPOLL_CTL_ADD, udp_fd, &ev);

    uint8_t ipc_buf[4096]; size_t ipc_have = 0;
    struct epoll_event events[16];
    while (!*stop) {
        int nfds = epoll_wait(ep, events, 16, 1000);   /* 1s tick drives the sweep */
        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;
            if (fd == udp_fd) on_udp(udp_fd);
            else if (fd == ipc_fd) {
                if (on_ipc(ipc_fd, ipc_buf, &ipc_have, sizeof ipc_buf) < 0) { close(ep); return 0; }
            }
        }
        sweep_silent();
    }
    close(ep);
    return 0;
}
