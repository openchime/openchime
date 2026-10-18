/* The bounded connect (shared/sock.h, oc_connect_any): every address a name has,
 * in the system's order, each given at most its bound -- so a route that goes
 * nowhere costs that bound, not the kernel's minutes, before the next address
 * (the next family) is tried. Checked against a listener whose accept queue is
 * full, where a SYN is dropped and a plain connect would wait on the kernel;
 * and against a name with two addresses, only one of them listening. */

#include "sock.h"
#include "check.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

static long ms_since(const struct timeval *t0) {
    struct timeval t; gettimeofday(&t, NULL);
    return (t.tv_sec - t0->tv_sec) * 1000 + (t.tv_usec - t0->tv_usec) / 1000;
}

/* A listener on `family`'s loopback; its port in *port. -1 if the family has none. */
static int loopback_listener(int family, int backlog, int *port) {
    int fd = socket(family, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_storage a; memset(&a, 0, sizeof a);
    socklen_t al;
    if (family == AF_INET6) {
        struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)&a;
        a6->sin6_family = AF_INET6; a6->sin6_addr = in6addr_loopback; al = sizeof *a6;
    } else {
        struct sockaddr_in *a4 = (struct sockaddr_in *)&a;
        a4->sin_family = AF_INET; a4->sin_addr.s_addr = htonl(INADDR_LOOPBACK); al = sizeof *a4;
    }
    if (bind(fd, (struct sockaddr *)&a, al) != 0 || listen(fd, backlog) != 0 ||
        getsockname(fd, (struct sockaddr *)&a, &al) != 0) { close(fd); return -1; }
    *port = family == AF_INET6 ? ntohs(((struct sockaddr_in6 *)&a)->sin6_port)
                               : ntohs(((struct sockaddr_in *)&a)->sin_port);
    return fd;
}

/* A dropped SYN costs the bound, not the kernel's retry schedule. */
static void test_bound(void) {
    int port = 0;
    int lfd = loopback_listener(AF_INET, 0, &port);
    CHECK(lfd >= 0);
    if (lfd < 0) return;
    /* Fill the accept queue, never accepting, until a connect stays pending:
     * from then on this listener drops every SYN. */
    int fill[32], nfill = 0, pending = 0;
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons((uint16_t)port);
    while (nfill < 32 && !pending) {
        int s = socket(AF_INET, SOCK_STREAM, 0);
        if (s < 0) break;
        oc_sock_setnonblock(s);
        fill[nfill++] = s;
        if (connect(s, (struct sockaddr *)&a, sizeof a) == 0) continue;
        if (oc_poll(s, 1, 200) == 0) pending = 1;      /* not answered: the queue is full */
    }
    if (!pending) {
        printf("  (this kernel answered every SYN: the bound is not observable here)\n");
    } else {
        struct timeval t0; gettimeofday(&t0, NULL);
        int fd = oc_connect_any("127.0.0.1", port, 300);
        long took = ms_since(&t0);
        if (fd >= 0) close(fd);
        CHECK(fd < 0);
        if (took >= 1500) printf("  connect to a full queue took %ld ms\n", took);
        CHECK(took < 1500);                            /* one address, a 300 ms bound */
    }
    for (int i = 0; i < nfill; i++) close(fill[i]);
    close(lfd);
}

/* A name with two addresses, only one listening: the other is passed over and
 * the one that listens is reached, whichever the system lists first. */
static void test_moves_on(void) {
    int port = 0;
    int lfd = loopback_listener(AF_INET, 4, &port);
    CHECK(lfd >= 0);
    if (lfd < 0) return;
    int fd = oc_connect_any("localhost", port, 1000);
    CHECK(fd >= 0);
    if (fd >= 0) close(fd);
    close(lfd);
    /* And over IPv6 alone, where the host has it. */
    lfd = loopback_listener(AF_INET6, 4, &port);
    if (lfd >= 0) {
        fd = oc_connect_any("::1", port, 1000);
        CHECK(fd >= 0);
        if (fd >= 0) close(fd);
        close(lfd);
    }
    /* Nothing listening anywhere: -1, promptly. */
    CHECK(oc_connect_any("127.0.0.1", 1, 1000) < 0);
}

int run_sock_tests(void) {
    printf("test_sock: a bounded connect per address, and moving on to the next\n");
    test_bound();
    test_moves_on();
    return failures;
}
