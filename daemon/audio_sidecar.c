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
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#define OC_AUDIO_MAX_PARTS 2048

typedef struct {
    int      used;
    uint8_t  token[OC_AUDIO_TOKEN_MAX];
    size_t   token_len;
    uint64_t call_id;
    uint64_t user_id;
    struct sockaddr_storage addr;   /* learned from the first UDP packet */
    socklen_t addr_len;
    int      addr_known;
    /* The address that first packet was sent TO, which every packet to this
     * participant is sent FROM. A host can have several, and a reply from any
     * other is one the client's NAT, or a platform's UDP edge, discards: Fly
     * delivers public UDP to a `fly-global-services` address and drops a reply
     * from the machine's own. */
    struct in6_pktinfo local6;
    struct in_pktinfo  local4;
    int      local_known;
    uint64_t last_seen_ms;
} participant;

static participant g_parts[OC_AUDIO_MAX_PARTS];
static uint64_t g_silence_ms = OC_AUDIO_SILENCE_MS;
static int g_family;   /* the UDP socket's, which decides the control messages */

void oc_audio_sidecar_set_silence_ms(uint64_t ms) { g_silence_ms = ms ? ms : OC_AUDIO_SILENCE_MS; }

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

/* The participant whose token is `token` exactly (IPC), or leads `pkt` (UDP:
 * the token is followed by at least a seq). */
static participant *find_by_token(const uint8_t *token, size_t len) {
    for (int i = 0; i < OC_AUDIO_MAX_PARTS; i++)
        if (g_parts[i].used && g_parts[i].token_len == len && memcmp(g_parts[i].token, token, len) == 0)
            return &g_parts[i];
    return NULL;
}
static participant *find_by_packet(const uint8_t *pkt, size_t n) {
    for (int i = 0; i < OC_AUDIO_MAX_PARTS; i++)
        if (g_parts[i].used && n >= g_parts[i].token_len + 2 &&
            memcmp(g_parts[i].token, pkt, g_parts[i].token_len) == 0)
            return &g_parts[i];
    return NULL;
}

static void authorize(uint64_t call_id, uint64_t user_id, const uint8_t *token, size_t len) {
    participant *e = find_by_token(token, len);
    if (!e) {
        for (int i = 0; i < OC_AUDIO_MAX_PARTS; i++)
            if (!g_parts[i].used) { e = &g_parts[i]; break; }
        if (!e) return;   /* table full */
        memset(e, 0, sizeof *e);
        e->used = 1;
        memcpy(e->token, token, len);
        e->token_len = len;
    }
    e->call_id = call_id;
    e->user_id = user_id;
    e->last_seen_ms = now_ms();
}

static void revoke_token(const uint8_t *token, size_t len) {
    participant *e = find_by_token(token, len);
    if (e) e->used = 0;
}

/* --- daemon IPC: buffer bytes, apply complete length-prefixed messages ----- */

static void apply_ipc(const uint8_t *msg, size_t len) {
    if (len < 1) return;
    uint8_t type = msg[0];
    const uint8_t *b = msg + 1; size_t n = len - 1;
    if (type == OC_AUDIO_IPC_AUTHORIZE && n >= 16 + OC_AUDIO_TOKEN_RAND && n - 16 <= OC_AUDIO_TOKEN_MAX) {
        authorize(rd_u64(b), rd_u64(b + 8), b + 16, n - 16);
    } else if (type == OC_AUDIO_IPC_REVOKE && n > 0 && n <= OC_AUDIO_TOKEN_MAX) {
        revoke_token(b, n);
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

static int same_peer(const participant *e, const struct sockaddr_storage *src, socklen_t sl) {
    if (e->addr_len != sl || e->addr.ss_family != src->ss_family) return 0;
    if (src->ss_family == AF_INET6) {
        const struct sockaddr_in6 *a = (const void *)&e->addr, *b = (const void *)src;
        return a->sin6_port == b->sin6_port &&
               memcmp(&a->sin6_addr, &b->sin6_addr, sizeof a->sin6_addr) == 0;
    }
    const struct sockaddr_in *a = (const void *)&e->addr, *b = (const void *)src;
    return a->sin_port == b->sin_port && a->sin_addr.s_addr == b->sin_addr.s_addr;
}

/* Send `len` bytes to `to` from the address its packets arrive at. */
static void send_to(int udp_fd, const participant *to, const uint8_t *buf, size_t len) {
    struct iovec iov = { (void *)buf, len };
    struct msghdr mh;
    memset(&mh, 0, sizeof mh);
    mh.msg_name = (void *)&to->addr;
    mh.msg_namelen = to->addr_len;
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    union { struct cmsghdr h; uint8_t b[CMSG_SPACE(sizeof(struct in6_pktinfo))]; } ctl;
    if (to->local_known) {
        memset(&ctl, 0, sizeof ctl);
        mh.msg_control = ctl.b;
        struct cmsghdr *cm = &ctl.h;
        if (g_family == AF_INET6) {
            /* On a dual-stack socket this carries an IPv4-mapped address for an
             * IPv4 peer, which Linux applies as the IPv4 source. The interface
             * is left to routing; only the address is pinned. */
            struct in6_pktinfo pi = to->local6;
            pi.ipi6_ifindex = 0;
            mh.msg_controllen = CMSG_SPACE(sizeof pi);
            cm->cmsg_level = IPPROTO_IPV6; cm->cmsg_type = IPV6_PKTINFO;
            cm->cmsg_len = CMSG_LEN(sizeof pi);
            memcpy(CMSG_DATA(cm), &pi, sizeof pi);
        } else {
            struct in_pktinfo pi;
            memset(&pi, 0, sizeof pi);
            pi.ipi_spec_dst = to->local4.ipi_addr;
            mh.msg_controllen = CMSG_SPACE(sizeof pi);
            cm->cmsg_level = IPPROTO_IP; cm->cmsg_type = IP_PKTINFO;
            cm->cmsg_len = CMSG_LEN(sizeof pi);
            memcpy(CMSG_DATA(cm), &pi, sizeof pi);
        }
    }
    sendmsg(udp_fd, &mh, 0);
}

/* Returns 0 once the socket is drained. */
static int on_udp(int udp_fd) {
    uint8_t pkt[OC_AUDIO_MAX_PACKET];
    struct sockaddr_storage src;
    struct iovec iov = { pkt, sizeof pkt };
    union { struct cmsghdr h; uint8_t b[CMSG_SPACE(sizeof(struct in6_pktinfo)) +
                                        CMSG_SPACE(sizeof(struct in_pktinfo))]; } ctl;
    struct msghdr mh;
    memset(&mh, 0, sizeof mh);
    mh.msg_name = &src; mh.msg_namelen = sizeof src;
    mh.msg_iov = &iov; mh.msg_iovlen = 1;
    mh.msg_control = ctl.b; mh.msg_controllen = sizeof ctl.b;
    ssize_t n = recvmsg(udp_fd, &mh, 0);
    if (n < 0) return 0;
    socklen_t sl = mh.msg_namelen;

    participant *me = find_by_packet(pkt, (size_t)n);   /* token leads the packet */
    if (!me) return 1;                           /* unknown/revoked token, or too short */
    /* The address is bound on first use and never re-learned. The token is not a
     * secret on the wire: it leads every packet in the clear,
     * so re-learning the return address from whichever packet arrived last let
     * anyone who saw one datagram redirect that participant's audio to
     * themselves with one forged packet -- the participant goes silent and the
     * forger hears the call. A valid token from any other address is dropped.
     *
     * A participant whose address really does change (NAT rebinding, a network
     * switch) is not relayed from the new one, so their packets stop counting,
     * the silence sweep drops them, and they rejoin with CALL_JOIN, which issues
     * a fresh token over the authenticated TCP connection (REQ-152). */
    if (!me->addr_known) {
        memcpy(&me->addr, &src, sl);
        me->addr_len = sl;
        me->addr_known = 1;
        for (struct cmsghdr *cm = CMSG_FIRSTHDR(&mh); cm; cm = CMSG_NXTHDR(&mh, cm)) {
            if (cm->cmsg_level == IPPROTO_IPV6 && cm->cmsg_type == IPV6_PKTINFO) {
                memcpy(&me->local6, CMSG_DATA(cm), sizeof me->local6);
                me->local_known = 1;
            } else if (cm->cmsg_level == IPPROTO_IP && cm->cmsg_type == IP_PKTINFO) {
                memcpy(&me->local4, CMSG_DATA(cm), sizeof me->local4);
                me->local_known = 1;
            }
        }
    } else if (!same_peer(me, &src, sl)) {
        return 1;
    }
    me->last_seen_ms = now_ms();

    const uint8_t *seq = pkt + me->token_len;               /* 2 bytes */
    const uint8_t *payload = seq + 2;
    size_t plen = (size_t)n - me->token_len - 2;

    /* Build the forwarded datagram: sender_user_id + seq + payload. It is never
     * longer than what came in, since a token is longer than a user id. */
    uint8_t out[OC_AUDIO_MAX_PACKET];
    wr_u64(out, me->user_id);
    out[8] = seq[0]; out[9] = seq[1];
    memcpy(out + OC_AUDIO_S2C_HDR, payload, plen);
    size_t olen = OC_AUDIO_S2C_HDR + plen;

    for (int i = 0; i < OC_AUDIO_MAX_PARTS; i++) {
        participant *e = &g_parts[i];
        if (!e->used || e == me || e->call_id != me->call_id || !e->addr_known) continue;
        send_to(udp_fd, e, out, olen);
    }
    return 1;
}

/* Drop the silent and tell the daemon, which takes them out of their call. The
 * report is best-effort, as every IPC write is: a daemon too busy to read it
 * still drops the participant when its connection closes. */
static void sweep_silent(int ipc_fd) {
    uint64_t now = now_ms();
    for (int i = 0; i < OC_AUDIO_MAX_PARTS; i++)
        if (g_parts[i].used && now - g_parts[i].last_seen_ms > g_silence_ms) {
            g_parts[i].used = 0;
            uint8_t m[5 + OC_AUDIO_TOKEN_MAX];
            uint32_t mlen = (uint32_t)(1 + g_parts[i].token_len);
            m[0] = (uint8_t)(mlen >> 24); m[1] = (uint8_t)(mlen >> 16);
            m[2] = (uint8_t)(mlen >> 8);  m[3] = (uint8_t)mlen;
            m[4] = OC_AUDIO_IPC_GONE;
            memcpy(m + 5, g_parts[i].token, g_parts[i].token_len);
            ssize_t n = write(ipc_fd, m, 4 + mlen); (void)n;
        }
}

int oc_audio_sidecar_run(int ipc_fd, int udp_fd, volatile sig_atomic_t *stop) {
    memset(g_parts, 0, sizeof g_parts);
    /* Non-blocking so the drain loops (read/recvfrom until EAGAIN) never block. */
    for (int fd = 0; fd < 2; fd++) {
        int f = fd == 0 ? ipc_fd : udp_fd;
        int fl = fcntl(f, F_GETFL, 0); if (fl >= 0) fcntl(f, F_SETFL, fl | O_NONBLOCK);
    }
    /* A shared screen's keyframe arrives as a burst of a hundred packets or more
     * (VIDEO.md §5): room for several, both ways, rather than the default's few. */
    int bufsz = 4 << 20;
    setsockopt(udp_fd, SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof bufsz);
    setsockopt(udp_fd, SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof bufsz);
    /* Learn the address each packet was sent to, so the answer comes from it. */
    struct sockaddr_storage self; socklen_t self_len = sizeof self;
    g_family = getsockname(udp_fd, (struct sockaddr *)&self, &self_len) == 0 ? self.ss_family : AF_INET;
    int on = 1;
    if (g_family == AF_INET6) setsockopt(udp_fd, IPPROTO_IPV6, IPV6_RECVPKTINFO, &on, sizeof on);
    else setsockopt(udp_fd, IPPROTO_IP, IP_PKTINFO, &on, sizeof on);
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
            if (fd == udp_fd) { for (int k = 0; k < 256 && on_udp(udp_fd); k++) {} }
            else if (fd == ipc_fd) {
                if (on_ipc(ipc_fd, ipc_buf, &ipc_have, sizeof ipc_buf) < 0) { close(ep); return 0; }
            }
        }
        sweep_silent(ipc_fd);
    }
    close(ep);
    return 0;
}
