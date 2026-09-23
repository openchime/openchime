/* The daemon's listening sockets — see listen.h. */

#include "listen.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int bind_family(int family, int type, int port, const char **op) {
    int fd = socket(family, type, 0);
    if (fd < 0) { *op = "socket"; return -1; }
    int yes = 1, no = 0;
    if (type == SOCK_STREAM) setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    struct sockaddr_storage ss;
    socklen_t sl;
    memset(&ss, 0, sizeof ss);
    if (family == AF_INET6) {
        /* Explicitly off: the default comes from net.ipv6.bindv6only, and a host
         * that set it would otherwise lose every IPv4 client without a word. */
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &no, sizeof no);
        struct sockaddr_in6 *a = (struct sockaddr_in6 *)&ss;
        a->sin6_family = AF_INET6;
        a->sin6_addr = in6addr_any;
        a->sin6_port = htons((uint16_t)port);
        sl = sizeof *a;
    } else {
        struct sockaddr_in *a = (struct sockaddr_in *)&ss;
        a->sin_family = AF_INET;
        a->sin_addr.s_addr = htonl(INADDR_ANY);
        a->sin_port = htons((uint16_t)port);
        sl = sizeof *a;
    }
    if (bind(fd, (struct sockaddr *)&ss, sl) < 0) {
        int e = errno;
        close(fd);
        errno = e;
        *op = "bind";
        return -1;
    }
    return fd;
}

int oc_listen_bind(int type, int port, const char **op) {
    const char *ignored;
    if (!op) op = &ignored;
    int fd = bind_family(AF_INET6, type, port, op);
    if (fd >= 0) return fd;
    /* No IPv6 on this host — a container started without it, or a kernel built
     * without it — says so as one of these, and IPv4 alone is then all there is.
     * Anything else (the port is taken, it is privileged) is the same failure
     * for IPv4 and is reported as it is. */
    if (errno == EAFNOSUPPORT || errno == EADDRNOTAVAIL || errno == EPROTONOSUPPORT)
        return bind_family(AF_INET, type, port, op);
    return -1;
}

void oc_listen_peer_text(const void *peer, char *out, size_t cap) {
    static const uint8_t V4MAPPED[12] = { 0,0,0,0,0,0,0,0,0,0,0xFF,0xFF };
    const struct sockaddr_storage *ss = peer;
    if (cap) out[0] = '\0';
    if (ss->ss_family == AF_INET) {
        inet_ntop(AF_INET, &((const struct sockaddr_in *)ss)->sin_addr, out, (socklen_t)cap);
    } else if (ss->ss_family == AF_INET6) {
        const uint8_t *a = ((const struct sockaddr_in6 *)ss)->sin6_addr.s6_addr;
        if (memcmp(a, V4MAPPED, sizeof V4MAPPED) == 0) inet_ntop(AF_INET, a + 12, out, (socklen_t)cap);
        else inet_ntop(AF_INET6, a, out, (socklen_t)cap);
    }
}
