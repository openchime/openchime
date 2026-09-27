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
static inline int oc_wake_open(oc_wake *w) {
    oc_sock_startup();
    w->rd = w->wr = -1;
    SOCKET r = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP), s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in a; int al = sizeof a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
    if (r == INVALID_SOCKET || s == INVALID_SOCKET ||
        bind(r, (struct sockaddr *)&a, sizeof a) != 0 ||
        getsockname(r, (struct sockaddr *)&a, &al) != 0 ||
        connect(s, (struct sockaddr *)&a, sizeof a) != 0) {
        if (r != INVALID_SOCKET) closesocket(r);
        if (s != INVALID_SOCKET) closesocket(s);
        return -1;
    }
    w->rd = (int)r; w->wr = (int)s;
    oc_sock_setnonblock(w->rd); oc_sock_setnonblock(w->wr);
    return 0;
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

#endif /* OC_SOCK_H */
