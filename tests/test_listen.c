/* Tests for the daemon's listening sockets (daemon/listen.c): with `loopback`,
 * a stream and a datagram socket are bound to 127.0.0.1 alone, as a
 * development daemon with OPENCHIME_DEV_LOOPBACK on listens; without it, to the
 * wildcard address, as every deployment does. */

#include "check.h"
#include "listen.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* The address `fd` is bound to, as text ("127.0.0.1", "::", "0.0.0.0"). */
static void bound_addr(int fd, char *out, size_t cap) {
    struct sockaddr_storage ss; socklen_t sl = sizeof ss;
    out[0] = '\0';
    if (getsockname(fd, (struct sockaddr *)&ss, &sl) != 0) return;
    if (ss.ss_family == AF_INET)
        inet_ntop(AF_INET, &((struct sockaddr_in *)&ss)->sin_addr, out, (socklen_t)cap);
    else if (ss.ss_family == AF_INET6)
        inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&ss)->sin6_addr, out, (socklen_t)cap);
}

int run_listen_tests(void) {
    printf("test_listen: loopback binds 127.0.0.1 alone, stream and datagram; otherwise the wildcard\n");
    static const int TYPES[2] = { SOCK_STREAM, SOCK_DGRAM };
    for (int i = 0; i < 2; i++) {
        char addr[64];
        const char *op = NULL;
        int fd = oc_listen_bind(TYPES[i], 0, 1, &op);
        CHECK(fd >= 0);
        if (fd >= 0) {
            bound_addr(fd, addr, sizeof addr);
            CHECK(strcmp(addr, "127.0.0.1") == 0);
            close(fd);
        }
        fd = oc_listen_bind(TYPES[i], 0, 0, &op);
        CHECK(fd >= 0);
        if (fd >= 0) {
            bound_addr(fd, addr, sizeof addr);
            CHECK(strcmp(addr, "::") == 0 || strcmp(addr, "0.0.0.0") == 0);
            close(fd);
        }
    }
    return failures;
}
