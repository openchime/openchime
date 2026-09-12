/*
 * OpenChime — tiny socket-portability shim (POSIX vs Winsock).
 *
 * shared/tls.c's non-blocking BIO callbacks and the client's network thread
 * (client/net.c) use it so the same wire/TLS code compiles for the Linux daemon
 * and the Windows client. Only the pieces both need are abstracted: would-block
 * detection, close, non-blocking, a one-shot poll, and Winsock startup. Address
 * resolution (getaddrinfo) is standard on both and used directly.
 *
 * On Windows, include this before anything pulling in <windows.h> so winsock2
 * wins the include race.
 */

#ifndef OC_SOCK_H
#define OC_SOCK_H

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

#endif

#endif /* OC_SOCK_H */
