/* What a test needs from its surroundings, so that it shares none of them
 * (docs/TESTING.md §2.3): a directory of its own, a port the kernel chose, and
 * the tree's files found from the test binary rather than the working
 * directory. Header-only; every function is static inline. */
#ifndef OC_TEST_ENV_H
#define OC_TEST_ENV_H

#include <arpa/inet.h>
#include <errno.h>
#include <ftw.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* A fresh directory under $TMPDIR (else /tmp), named for `who`, into `out`:
 * nothing another suite, another run or another checkout can collide with.
 * 0, or -1. */
static inline int oc_test_mkdtemp(const char *who, char *out, size_t cap) {
    const char *base = getenv("TMPDIR");
    if (!base || !base[0]) base = P_tmpdir;
    int n = snprintf(out, cap, "%s/oc-%s-XXXXXX", base, who);
    if (n < 0 || (size_t)n >= cap) return -1;
    return mkdtemp(out) ? 0 : -1;
}

static inline int oc_test_rm_one_(const char *p, const struct stat *st, int flag, struct FTW *f) {
    (void)st; (void)flag; (void)f;
    return remove(p);
}

/* `dir` and everything under it, gone. Quiet about what is already gone. */
static inline void oc_test_rmtree(const char *dir) {
    if (dir && dir[0]) nftw(dir, oc_test_rm_one_, 16, FTW_DEPTH | FTW_PHYS);
}

/* `dir`/`name` into `out`; `out`, or "" if it does not fit. */
static inline const char *oc_test_path(char *out, size_t cap, const char *dir, const char *name) {
    int n = snprintf(out, cap, "%s/%s", dir, name);
    if (n < 0 || (size_t)n >= cap) out[0] = '\0';
    return out;
}

/* A loopback TCP port nothing is listening on, chosen by the kernel (bind
 * port 0, getsockname). Between this and the caller's own bind another
 * process could take it; a caller that can listen on port 0 itself and read
 * the port back should, and one that cannot retries on EADDRINUSE. -1 if
 * none. */
static inline int oc_test_free_port(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    socklen_t al = sizeof a;
    int port = -1;
    if (bind(fd, (struct sockaddr *)&a, sizeof a) == 0 && getsockname(fd, (struct sockaddr *)&a, &al) == 0)
        port = ntohs(a.sin_port);
    close(fd);
    return port;
}

/* The tree's `rel` (e.g. "tests/data/x.wav"), found from the test binary --
 * build/tests sits one directory below the tree's root -- so the test runs
 * from any working directory. Falls back to `rel` itself, relative to the
 * working directory, where /proc/self/exe cannot be read. */
static inline const char *oc_test_tree_path(const char *rel, char *out, size_t cap) {
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n > 0) {
        exe[n] = '\0';
        char *slash = strrchr(exe, '/');                  /* .../build/tests -> .../build */
        if (slash) *slash = '\0';
        slash = strrchr(exe, '/');                        /* .../build -> ... */
        if (slash) *slash = '\0';
        int k = snprintf(out, cap, "%s/%s", exe, rel);
        if (k > 0 && (size_t)k < cap && access(out, F_OK) == 0) return out;
    }
    snprintf(out, cap, "%s", rel);
    return out;
}

#endif /* OC_TEST_ENV_H */
