/* The call media relay, run by the net loop. See relay.h. */

#include "relay.h"
#include "idmap.h"
#include "protocol.h"   /* OC_MAX_CALL_PARTICIPANTS */

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#define OC_RELAY_MAX_PARTS 2048

typedef struct participant {
    int      used;
    uint8_t  token[OC_AUDIO_TOKEN_MAX];
    size_t   token_len;
    uint64_t token_key;              /* its key in the token index */
    uint64_t call_id;
    uint64_t user_id;
    uint64_t conn_id;                /* the connection it joined from */
    int      tcp;                    /* its latest packet came by the connection */
    struct participant *c_next, *c_prev;   /* the call's other participants */
    struct sockaddr_storage addr;    /* learned from the first UDP packet */
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

struct oc_relay {
    int            fd;
    int            family;           /* the socket's, which decides the control messages */
    uint64_t       silence_ms;
    oc_relay_hooks hooks;
    void          *ctx;
    size_t         token_len;        /* every token one daemon issues has the one length */
    oc_idmap       by_token;         /* token key -> participant */
    oc_idmap       by_call;          /* call id -> first participant */
    oc_idmap       by_conn;          /* connection id -> participant */
    size_t         n;
    participant    parts[OC_RELAY_MAX_PARTS];
};

static uint64_t mono_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void wr_u64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (56 - 8 * i));
}

/* A token's index key: eight of its random bytes, which is as good a hash as
 * there is. 0 means "empty" to the index, so it is moved. A lookup always
 * compares the whole token as well. */
static uint64_t token_key(const uint8_t *token, size_t len) {
    uint64_t k = 0;
    memcpy(&k, token + len - OC_AUDIO_TOKEN_RAND, sizeof k);
    return k ? k : 1;
}

static participant *find_token(oc_relay *r, const uint8_t *token, size_t len) {
    if (len != r->token_len || len < OC_AUDIO_TOKEN_RAND) return NULL;
    participant *p = oc_idmap_get(&r->by_token, token_key(token, len));
    return p && p->token_len == len && memcmp(p->token, token, len) == 0 ? p : NULL;
}

static void call_unlink(oc_relay *r, participant *p) {
    if (p->c_prev) p->c_prev->c_next = p->c_next;
    else if (p->c_next) oc_idmap_put(&r->by_call, p->call_id, p->c_next);
    else oc_idmap_del(&r->by_call, p->call_id);
    if (p->c_next) p->c_next->c_prev = p->c_prev;
    p->c_next = p->c_prev = NULL;
}

static int call_link(oc_relay *r, participant *p) {
    participant *head = oc_idmap_get(&r->by_call, p->call_id);
    if (oc_idmap_put(&r->by_call, p->call_id, p) != 0) return -1;
    p->c_prev = NULL;
    p->c_next = head;
    if (head) head->c_prev = p;
    return 0;
}

static void drop(oc_relay *r, participant *p) {
    call_unlink(r, p);
    if (oc_idmap_get(&r->by_conn, p->conn_id) == p) oc_idmap_del(&r->by_conn, p->conn_id);
    oc_idmap_del(&r->by_token, p->token_key);
    p->used = 0;
    r->n--;
    if (!r->n) r->token_len = 0;
}

oc_relay *oc_relay_open(int udp_fd, const oc_relay_hooks *hooks, void *ctx) {
    oc_relay *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    /* Twice the table, so neither index passes half full (idmap.h). */
    if (oc_idmap_init(&r->by_token, 2 * OC_RELAY_MAX_PARTS) != 0 ||
        oc_idmap_init(&r->by_call, 2 * OC_RELAY_MAX_PARTS) != 0 ||
        oc_idmap_init(&r->by_conn, 2 * OC_RELAY_MAX_PARTS) != 0) {
        oc_idmap_free(&r->by_token); oc_idmap_free(&r->by_call); oc_idmap_free(&r->by_conn); free(r);
        return NULL;
    }
    r->fd = udp_fd;
    r->silence_ms = OC_AUDIO_SILENCE_MS;
    if (hooks) r->hooks = *hooks;
    r->ctx = ctx;
    int fl = fcntl(udp_fd, F_GETFL, 0);
    if (fl >= 0) fcntl(udp_fd, F_SETFL, fl | O_NONBLOCK);
    /* A shared screen's keyframe arrives as a burst of a hundred packets or more
     * (VIDEO.md §5): room for several, both ways. The FORCE forms pass the
     * system's ceiling where the daemon may; elsewhere the plain request gets
     * what the ceiling allows. */
    int bufsz = 4 << 20;
    if (setsockopt(udp_fd, SOL_SOCKET, SO_RCVBUFFORCE, &bufsz, sizeof bufsz) != 0)
        setsockopt(udp_fd, SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof bufsz);
    if (setsockopt(udp_fd, SOL_SOCKET, SO_SNDBUFFORCE, &bufsz, sizeof bufsz) != 0)
        setsockopt(udp_fd, SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof bufsz);
    struct sockaddr_storage self; socklen_t self_len = sizeof self;
    r->family = getsockname(udp_fd, (struct sockaddr *)&self, &self_len) == 0 ? self.ss_family : AF_INET;
    int on = 1;
    if (r->family == AF_INET6) setsockopt(udp_fd, IPPROTO_IPV6, IPV6_RECVPKTINFO, &on, sizeof on);
    else setsockopt(udp_fd, IPPROTO_IP, IP_PKTINFO, &on, sizeof on);
    return r;
}

void oc_relay_close(oc_relay *r) {
    if (!r) return;
    oc_idmap_free(&r->by_token);
    oc_idmap_free(&r->by_call);
    oc_idmap_free(&r->by_conn);
    free(r);
}

void oc_relay_set_silence_ms(oc_relay *r, uint64_t ms) {
    if (r) r->silence_ms = ms ? ms : OC_AUDIO_SILENCE_MS;
}

size_t oc_relay_count(const oc_relay *r) { return r ? r->n : 0; }

int oc_relay_authorize(oc_relay *r, uint64_t call_id, uint64_t user_id, uint64_t conn_id,
                       const uint8_t *token, size_t len) {
    if (!r || len < OC_AUDIO_TOKEN_RAND || len > OC_AUDIO_TOKEN_MAX || !call_id) return -1;
    if (r->n && len != r->token_len) return -1;
    if (!r->n) r->token_len = len;
    participant *p = find_token(r, token, len);
    if (p) {
        call_unlink(r, p);
        if (oc_idmap_get(&r->by_conn, p->conn_id) == p) oc_idmap_del(&r->by_conn, p->conn_id);
    } else {
        uint64_t key = token_key(token, len);
        if (oc_idmap_get(&r->by_token, key)) return -1;   /* another token with its key */
        for (int i = 0; i < OC_RELAY_MAX_PARTS && !p; i++)
            if (!r->parts[i].used) p = &r->parts[i];
        if (!p) return -1;   /* table full */
        memset(p, 0, sizeof *p);
        memcpy(p->token, token, len);
        p->token_len = len;
        p->token_key = key;
        if (oc_idmap_put(&r->by_token, key, p) != 0) return -1;
        p->used = 1;
        r->n++;
    }
    p->call_id = call_id;
    p->user_id = user_id;
    p->conn_id = conn_id;
    p->last_seen_ms = mono_ms();
    if (call_link(r, p) != 0) { drop(r, p); return -1; }
    if (conn_id) oc_idmap_put(&r->by_conn, conn_id, p);   /* one call per connection */
    return 0;
}

void oc_relay_revoke(oc_relay *r, const uint8_t *token, size_t len) {
    if (!r) return;
    participant *p = find_token(r, token, len);
    if (p) drop(r, p);
}

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

/* One outgoing datagram's header: to `to`, from the address its packets
 * arrive at. `ctl` must outlive the send. */
typedef union { struct cmsghdr h; uint8_t b[CMSG_SPACE(sizeof(struct in6_pktinfo))]; } send_ctl;

static void prepare(const oc_relay *r, const participant *to, struct msghdr *mh,
                    struct iovec *iov, send_ctl *ctl) {
    memset(mh, 0, sizeof *mh);
    mh->msg_name = (void *)&to->addr;
    mh->msg_namelen = to->addr_len;
    mh->msg_iov = iov;
    mh->msg_iovlen = 1;
    if (!to->local_known) return;
    memset(ctl, 0, sizeof *ctl);
    mh->msg_control = ctl->b;
    struct cmsghdr *cm = &ctl->h;
    if (r->family == AF_INET6) {
        /* On a dual-stack socket this carries an IPv4-mapped address for an IPv4
         * peer, which Linux applies as the IPv4 source. The interface is left to
         * routing; only the address is pinned. */
        struct in6_pktinfo pi = to->local6;
        pi.ipi6_ifindex = 0;
        mh->msg_controllen = CMSG_SPACE(sizeof pi);
        cm->cmsg_level = IPPROTO_IPV6; cm->cmsg_type = IPV6_PKTINFO;
        cm->cmsg_len = CMSG_LEN(sizeof pi);
        memcpy(CMSG_DATA(cm), &pi, sizeof pi);
    } else {
        struct in_pktinfo pi;
        memset(&pi, 0, sizeof pi);
        pi.ipi_spec_dst = to->local4.ipi_addr;
        mh->msg_controllen = CMSG_SPACE(sizeof pi);
        cm->cmsg_level = IPPROTO_IP; cm->cmsg_type = IP_PKTINFO;
        cm->cmsg_len = CMSG_LEN(sizeof pi);
        memcpy(CMSG_DATA(cm), &pi, sizeof pi);
    }
}

/* Relay `me`'s packet to the rest of its call, each by its own transport: one
 * sendmmsg for those on UDP, the hook for those on the connection. A keepalive
 * (no payload) is answered to its sender alone: it is how a client learns the
 * relay can hear it, and the others have no use for it. */
static void forward(oc_relay *r, participant *me, uint16_t seq, const uint8_t *payload, size_t plen) {
    if (plen > OC_AUDIO_MAX_PACKET - OC_AUDIO_S2C_HDR) return;
    /* The forwarded datagram: sender_user_id + seq + payload. */
    uint8_t out[OC_AUDIO_MAX_PACKET];
    wr_u64(out, me->user_id);
    out[8] = (uint8_t)(seq >> 8); out[9] = (uint8_t)seq;
    memcpy(out + OC_AUDIO_S2C_HDR, payload, plen);
    size_t olen = OC_AUDIO_S2C_HDR + plen;

    struct mmsghdr mm[OC_MAX_CALL_PARTICIPANTS];
    struct iovec   iv[OC_MAX_CALL_PARTICIPANTS];
    send_ctl       cc[OC_MAX_CALL_PARTICIPANTS];
    unsigned k = 0;
    for (participant *e = plen ? oc_idmap_get(&r->by_call, me->call_id) : me;
         e && k < OC_MAX_CALL_PARTICIPANTS; e = plen ? e->c_next : NULL) {
        if (plen && e == me) continue;
        if (e->tcp) {
            if (r->hooks.tcp_send) r->hooks.tcp_send(r->ctx, e->conn_id, me->user_id, seq, payload, plen);
            continue;
        }
        if (!e->addr_known) continue;
        iv[k] = (struct iovec){ out, olen };
        prepare(r, e, &mm[k].msg_hdr, &iv[k], &cc[k]);
        k++;
    }
    for (unsigned sent = 0; sent < k; ) {
        int got = sendmmsg(r->fd, mm + sent, k - sent, 0);
        if (got <= 0) break;   /* best-effort, as UDP is: loss is the clients' to hide */
        sent += (unsigned)got;
    }
}

int oc_relay_from_tcp(oc_relay *r, uint64_t conn_id, uint16_t seq, const uint8_t *ct, size_t len) {
    participant *me = r && conn_id ? oc_idmap_get(&r->by_conn, conn_id) : NULL;
    if (!me) return -1;
    me->tcp = 1;
    me->last_seen_ms = mono_ms();
    forward(r, me, seq, ct, len);
    return 0;
}

/* Relay one waiting datagram. 1 if one was read (whatever became of it), 0 when
 * none was waiting. */
static int relay_one(oc_relay *r) {
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
    ssize_t n = recvmsg(r->fd, &mh, 0);
    if (n < 0) return 0;
    socklen_t sl = mh.msg_namelen;

    if (!r->token_len || (size_t)n < r->token_len + 2) return 1;
    participant *me = find_token(r, pkt, r->token_len);   /* the token leads the packet */
    if (!me) return 1;                                     /* unknown or revoked */
    /* The address is bound on first use and never re-learned. The token is not a
     * secret on the wire: it leads every packet in the clear, so re-learning the
     * return address from whichever packet arrived last let anyone who saw one
     * datagram redirect that participant's audio to themselves with one forged
     * packet. A valid token from any other address is dropped.
     *
     * A participant whose address really does change (NAT rebinding, a network
     * switch) is not relayed from the new one, so their packets stop counting,
     * the silence sweep drops them, and they rejoin with CALL_JOIN, which issues
     * a fresh token over the authenticated connection (REQ-152). */
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
    me->last_seen_ms = mono_ms();

    me->tcp = 0;
    const uint8_t *seq = pkt + me->token_len;
    forward(r, me, (uint16_t)((seq[0] << 8) | seq[1]), seq + 2, (size_t)n - me->token_len - 2);
    return 1;
}

int oc_relay_on_readable(oc_relay *r, int max_packets) {
    if (!r) return 0;
    for (int i = 0; i < max_packets; i++)
        if (!relay_one(r)) return 0;
    return 1;
}

void oc_relay_sweep(oc_relay *r) {
    if (!r || !r->n) return;
    uint64_t now = mono_ms();
    struct { uint8_t t[OC_AUDIO_TOKEN_MAX]; size_t len; } *gone = NULL;
    size_t ng = 0;
    for (int i = 0; i < OC_RELAY_MAX_PARTS; i++) {
        participant *p = &r->parts[i];
        if (!p->used || now - p->last_seen_ms <= r->silence_ms) continue;
        if (r->hooks.gone) {
            void *g = realloc(gone, (ng + 1) * sizeof *gone);
            if (g) {
                gone = g;
                memcpy(gone[ng].t, p->token, p->token_len);
                gone[ng].len = p->token_len;
                ng++;
            }
        }
        drop(r, p);
    }
    /* Reported after the walk, so a hook that authorizes or revokes (a call
     * re-keying round its lost participant) cannot disturb it. */
    for (size_t i = 0; i < ng; i++) r->hooks.gone(r->ctx, gone[i].t, gone[i].len);
    free(gone);
}
