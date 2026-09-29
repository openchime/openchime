/*
 * OpenChime — tiny socket-portability shim (POSIX vs Winsock).
 *
 * shared/tls.c's non-blocking BIO callbacks and the client's network thread
 * (client/net.c) use it so the same wire/TLS code compiles for the Linux daemon
 * and the Windows client. Only the pieces both need are abstracted: would-block
 * detection, close, non-blocking, a one-shot poll, Winsock startup, and a wake
 * another thread can use to end a poll early. Address resolution (getaddrinfo)
 * is standard on both and used directly.
 *
 * On Windows, include this before anything pulling in <windows.h> so winsock2
 * wins the include race.
 */

#ifndef OC_SOCK_H
#define OC_SOCK_H

#include <string.h>

#ifdef _WIN32

#include <winsock2.h>
#include <ws2tcpip.h>

static inline void oc_sock_startup(void) {
    static int done = 0;
    if (!done) { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); done = 1; }
}
static inline int  oc_sock_wouldblock(void) {
    int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEINTR;
}
static inline void oc_closesock(int fd)        { closesocket((SOCKET)fd); }
static inline int  oc_sock_setnonblock(int fd) { u_long m = 1; return ioctlsocket((SOCKET)fd, FIONBIO, &m); }
static inline int  oc_sock_setblock(int fd)    { u_long m = 0; return ioctlsocket((SOCKET)fd, FIONBIO, &m); }
static inline int  oc_sock_inprogress(void)    { return WSAGetLastError() == WSAEWOULDBLOCK; }
static inline int  oc_poll(int fd, int want_write, int timeout_ms) {
    WSAPOLLFD p;
    p.fd = (SOCKET)fd;
    p.events = (short)(want_write ? POLLWRNORM : POLLRDNORM);
    p.revents = 0;
    return WSAPoll(&p, 1, timeout_ms);
}

/* A wake: two loopback UDP sockets, one connected to the other, because WSAPoll
 * takes only sockets. */
typedef struct { int rd, wr; } oc_wake;
/* One family's attempt: IPv4 loopback, or IPv6 loopback on a host without it. */
static inline int oc_wake_open_family(oc_wake *w, int family) {
    SOCKET r = socket(family, SOCK_DGRAM, IPPROTO_UDP), s = socket(family, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_storage a; int al = sizeof a;
    memset(&a, 0, sizeof a);
    int alen;
    if (family == AF_INET6) {
        struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)&a;
        a6->sin6_family = AF_INET6; a6->sin6_addr = in6addr_loopback;
        alen = (int)sizeof *a6;
    } else {
        struct sockaddr_in *a4 = (struct sockaddr_in *)&a;
        a4->sin_family = AF_INET; a4->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        alen = (int)sizeof *a4;
    }
    if (r == INVALID_SOCKET || s == INVALID_SOCKET ||
        bind(r, (struct sockaddr *)&a, alen) != 0 ||
        getsockname(r, (struct sockaddr *)&a, &al) != 0 ||
        connect(s, (struct sockaddr *)&a, al) != 0) {
        if (r != INVALID_SOCKET) closesocket(r);
        if (s != INVALID_SOCKET) closesocket(s);
        return -1;
    }
    w->rd = (int)r; w->wr = (int)s;
    oc_sock_setnonblock(w->rd); oc_sock_setnonblock(w->wr);
    return 0;
}
static inline int oc_wake_open(oc_wake *w) {
    oc_sock_startup();
    w->rd = w->wr = -1;
    return oc_wake_open_family(w, AF_INET) == 0 ? 0 : oc_wake_open_family(w, AF_INET6);
}
static inline void oc_wake_signal(oc_wake *w) { char b = 1; send((SOCKET)w->wr, &b, 1, 0); }
static inline void oc_wake_drain(oc_wake *w) {
    char b[64];
    while (recv((SOCKET)w->rd, b, sizeof b, 0) > 0) {}
}
static inline void oc_wake_close(oc_wake *w) {
    if (w->rd >= 0) closesocket((SOCKET)w->rd);
    if (w->wr >= 0) closesocket((SOCKET)w->wr);
    w->rd = w->wr = -1;
}
/* Poll `fd` and a wake together: bit 1 `fd` is ready, bit 2 the wake was
 * signalled; 0 on timeout, -1 on error. */
static inline int oc_poll_wake(int fd, int want_write, const oc_wake *w, int timeout_ms) {
    WSAPOLLFD p[2];
    p[0].fd = (SOCKET)fd; p[0].events = (short)(want_write ? POLLWRNORM : POLLRDNORM); p[0].revents = 0;
    p[1].fd = (SOCKET)w->rd; p[1].events = POLLRDNORM; p[1].revents = 0;
    int n = WSAPoll(p, 2, timeout_ms);
    if (n <= 0) return n;
    return (p[0].revents ? 1 : 0) | (p[1].revents ? 2 : 0);
}

#else /* POSIX */

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

static inline void oc_sock_startup(void) {}
static inline int  oc_sock_wouldblock(void) {
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}
static inline void oc_closesock(int fd)        { close(fd); }
static inline int  oc_sock_setnonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    return fl < 0 ? -1 : fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}
static inline int  oc_sock_setblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    return fl < 0 ? -1 : fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
}
static inline int  oc_sock_inprogress(void)    { return errno == EINPROGRESS || errno == EINTR; }
static inline int  oc_poll(int fd, int want_write, int timeout_ms) {
    struct pollfd p;
    p.fd = fd;
    p.events = (short)(want_write ? POLLOUT : POLLIN);
    p.revents = 0;
    return poll(&p, 1, timeout_ms);
}

/* A wake: a pipe another thread writes a byte to. */
typedef struct { int rd, wr; } oc_wake;
static inline int oc_wake_open(oc_wake *w) {
    int p[2];
    w->rd = w->wr = -1;
    if (pipe(p) != 0) return -1;
    for (int i = 0; i < 2; i++) {
        oc_sock_setnonblock(p[i]);
        fcntl(p[i], F_SETFD, FD_CLOEXEC);
    }
    w->rd = p[0]; w->wr = p[1];
    return 0;
}
/* A full pipe is already a wake: the byte is not needed. */
static inline void oc_wake_signal(oc_wake *w) { char b = 1; ssize_t r = write(w->wr, &b, 1); (void)r; }
static inline void oc_wake_drain(oc_wake *w) {
    char b[64];
    while (read(w->rd, b, sizeof b) > 0) {}
}
static inline void oc_wake_close(oc_wake *w) {
    if (w->rd >= 0) close(w->rd);
    if (w->wr >= 0) close(w->wr);
    w->rd = w->wr = -1;
}
/* Poll `fd` and a wake together: bit 1 `fd` is ready, bit 2 the wake was
 * signalled; 0 on timeout, -1 on error. */
static inline int oc_poll_wake(int fd, int want_write, const oc_wake *w, int timeout_ms) {
    struct pollfd p[2];
    p[0].fd = fd; p[0].events = (short)(want_write ? POLLOUT : POLLIN); p[0].revents = 0;
    p[1].fd = w->rd; p[1].events = POLLIN; p[1].revents = 0;
    int n = poll(p, 2, timeout_ms);
    if (n <= 0) return n;
    return (p[0].revents ? 1 : 0) | (p[1].revents ? 2 : 0);
}

#endif

#include <stdio.h>

/* How long one address may take to connect before the next is tried. */
#define OC_CONNECT_PER_ADDR_MS 3000

/* Connect to `host` (a name or an IPv4/IPv6 address, unbracketed) on `port`: every
 * address getaddrinfo gives, in the system's preferred order -- IPv6 first where
 * the host has a route for it -- each given at most `per_addr_ms` before the next
 * is tried. So a broken route costs a bounded wait rather than the kernel's
 * minutes before the family that works. The socket comes back BLOCKING, as the
 * connect loops this replaces left it. -1 when none connects. */
/* One address, its connect bounded by `ms`: a connected, blocking socket, or -1. */
static inline int oc_connect_addr(const struct addrinfo *a, int ms) {
    int s = (int)socket(a->ai_family, a->ai_socktype, a->ai_protocol);
    if (s < 0) return -1;
    oc_sock_setnonblock(s);
    int ok = connect(s, a->ai_addr, (int)a->ai_addrlen) == 0;
    if (!ok && oc_sock_inprogress() && oc_poll(s, 1, ms) > 0) {
        int err = 0;
        socklen_t el = (socklen_t)sizeof err;
        ok = getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &el) == 0 && err == 0;
    }
    if (ok && oc_sock_setblock(s) == 0) return s;
    oc_closesock(s);
    return -1;
}

static inline int oc_connect_any(const char *host, int port, int per_addr_ms) {
    oc_sock_startup();
    char ps[8];
    snprintf(ps, sizeof ps, "%d", port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (!host || getaddrinfo(host, ps, &hints, &res) != 0) return -1;
    int fd = -1;
    for (struct addrinfo *a = res; a && fd < 0; a = a->ai_next)
        fd = oc_connect_addr(a, per_addr_ms);
    freeaddrinfo(res);
    return fd;
}

#endif /* OC_SOCK_H */
