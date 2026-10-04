/* The client's half of a browser sign-in (client/core/signin.c): verifier and
 * challenge, the loopback listener against a scripted "browser", query reading. */

#include "check.h"
#include "../client/core/signin.h"
#include "../daemon/jwt.h"     /* the daemon's own check of the pair */
#include "tls.h"               /* a fake daemon for the tunnel */
#include "protocol.h"
#include <sys/time.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* One HTTP request to 127.0.0.1:<port>; the status line comes back in `status`. */
static void http_get(int port, const char *request, char *status, size_t cap) {
    status[0] = '\0';
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((uint16_t)port);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) == 0) {
        ssize_t w = write(fd, request, strlen(request)); (void)w;
        char buf[512];
        ssize_t n = read(fd, buf, sizeof buf - 1);
        if (n > 0) {
            buf[n] = '\0';
            char *eol = strstr(buf, "\r\n");
            if (eol) *eol = '\0';
            snprintf(status, cap, "%s", buf);
        }
    }
    close(fd);
}

struct browser { int port; char path[96]; char first[64], second[64], third[64]; };

/* A port scanner, then a wrong secret, then the real redirect. */
static void *browser_thread(void *arg) {
    struct browser *b = arg;
    char req[512];
    usleep(50000);
    http_get(b->port, "POST /cb/whatever HTTP/1.1\r\nHost: x\r\n\r\n", b->first, sizeof b->first);
    http_get(b->port, "GET /cb/not-the-secret?token=evil HTTP/1.1\r\nHost: x\r\n\r\n",
             b->second, sizeof b->second);
    snprintf(req, sizeof req, "GET %s?token=aaa.bbb.ccc&x=a%%20b+c HTTP/1.1\r\nHost: x\r\n\r\n", b->path);
    http_get(b->port, req, b->third, sizeof b->third);
    return NULL;
}

static atomic_int g_cancel;
static void *cancel_thread(void *arg) { (void)arg; usleep(150000); g_cancel = 1; return NULL; }

/* --- the tunnel ------------------------------------------------------------ */

/* A fake daemon on TLS: every request is answered 303 to one of its own paths,
 * with the request's head echoed back as the body -- what the tunnel sent. */
static oc_tls_server g_fd_srv;
static int g_fd_listen = -1, g_fd_port;
static atomic_int g_fd_stop;

static void *fake_daemon(void *arg) {
    (void)arg;
    while (!atomic_load(&g_fd_stop)) {
        struct timeval tv = { 0, 200000 };
        fd_set rs; FD_ZERO(&rs); FD_SET(g_fd_listen, &rs);
        if (select(g_fd_listen + 1, &rs, NULL, NULL, &tv) <= 0) continue;
        int fd = accept(g_fd_listen, NULL, NULL);
        if (fd < 0) continue;
        struct timeval to = { 5, 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof to);
        oc_tls_conn c;
        if (oc_tls_conn_init(&c, &g_fd_srv.conf, fd) == 0) {
            oc_tls_status st;
            while ((st = oc_tls_handshake(&c)) == OC_TLS_WANT_READ || st == OC_TLS_WANT_WRITE) {}
            char req[8192]; size_t got = 0;
            req[0] = '\0';
            /* The head, then the body its Content-Length declares. */
            for (;;) {
                char *hend = strstr(req, "\r\n\r\n");
                if (hend) {
                    const char *cl = strstr(req, "Content-Length: ");
                    size_t want = (size_t)(hend + 4 - req) + (cl && cl < hend ? (size_t)atoi(cl + 16) : 0);
                    if (got >= want) break;
                }
                if (st != OC_TLS_OK || got >= sizeof req - 1) break;
                size_t n = 0;
                oc_tls_status rs2 = oc_tls_read(&c, req + got, sizeof req - 1 - got, &n);
                got += n; req[got] = '\0';
                if (rs2 != OC_TLS_OK && rs2 != OC_TLS_WANT_READ) break;
                if (n == 0 && rs2 == OC_TLS_OK) break;
            }
            if (st == OC_TLS_OK) {
                char resp[9000];
                int n = snprintf(resp, sizeof resp, "HTTP/1.1 303 See Other\r\nLocation: /signup?x=1\r\n"
                                 "Content-Length: %zu\r\nConnection: close\r\n\r\n%s", strlen(req), req);
                size_t off = 0, w = 0;
                while (off < (size_t)n && oc_tls_write(&c, resp + off, (size_t)n - off, &w) != OC_TLS_ERROR) off += w;
            }
            oc_tls_conn_free(&c);
        }
        close(fd);
    }
    return NULL;
}

/* One raw request to the listener; the whole answer in `out`. */
static void http_raw(int port, const char *request, char *out, size_t cap) {
    out[0] = '\0';
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons((uint16_t)port);
    struct timeval to = { 10, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof to);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) == 0) {
        ssize_t w = write(fd, request, strlen(request)); (void)w;
        size_t got = 0;
        for (ssize_t r; got < cap - 1 && (r = read(fd, out + got, cap - 1 - got)) > 0; ) got += (size_t)r;
        out[got] = '\0';
    }
    close(fd);
}

struct serve_arg { oc_loopback *lb; atomic_int cancel; };
static void *serve_thread(void *arg) {
    struct serve_arg *sa = arg;
    oc_loopback_serve(sa->lb, 20000, &sa->cancel);
    return NULL;
}

/* The tunnel: it carries the sign-in pages, and nothing else, to the one
 * certificate it was given, as the daemon's origin -- rewriting Host, Origin
 * (for its own page's post only) and a Location on the daemon's pages. */
static void test_tunnel(void) {
    CHECK(oc_tls_server_init(&g_fd_srv, NULL, NULL) == 0);
    oc_tunnel_target t;
    memset(&t, 0, sizeof t);
    CHECK(oc_tls_server_fingerprint(&g_fd_srv, t.fp) == 0);
    g_fd_listen = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t al = sizeof a;
    CHECK(bind(g_fd_listen, (struct sockaddr *)&a, sizeof a) == 0 && listen(g_fd_listen, 8) == 0 &&
          getsockname(g_fd_listen, (struct sockaddr *)&a, &al) == 0);
    g_fd_port = ntohs(a.sin_port);
    atomic_store(&g_fd_stop, 0);
    pthread_t dth;
    CHECK(pthread_create(&dth, NULL, fake_daemon, NULL) == 0);

    snprintf(t.host, sizeof t.host, "127.0.0.1");
    t.port = g_fd_port;
    snprintf(t.authority, sizeof t.authority, "d.test:%d", g_fd_port);

    for (int round = 0; round < 2; round++) {
        /* Round 1: a target whose certificate is not the accepted one. */
        oc_tunnel_target tt = t;
        if (round == 1) tt.fp[0] ^= 0xff;
        oc_loopback *lb = oc_loopback_open(NULL, 0);
        CHECK(lb != NULL);
        if (!lb) break;
        oc_loopback_set_tunnel(lb, &tt);
        char base[128];
        CHECK(oc_loopback_tunnel_base(lb, base, sizeof base) == 0 && strncmp(base, "http://127.0.0.1:", 17) == 0);
        int lport = atoi(base + 17);
        const char *tp = strchr(base + 17, '/');
        struct serve_arg sa = { lb, 0 };
        pthread_t sth;
        CHECK(pthread_create(&sth, NULL, serve_thread, &sa) == 0);
        char req[1024], out[16384];
        if (round == 0) {
            snprintf(req, sizeof req, "GET %s/signin?q=1 HTTP/1.1\r\nHost: 127.0.0.1:%d\r\n\r\n", tp, lport);
            http_raw(lport, req, out, sizeof out);
            CHECK(strstr(out, "HTTP/1.1 303") == out);
            char want[160];
            snprintf(want, sizeof want, "\r\nLocation: %s/signup?x=1\r\n", tp);
            CHECK(strstr(out, want) != NULL);                             /* back into the tunnel */
            CHECK(strstr(out, "GET /signin?q=1 HTTP/1.1\r\n") != NULL);     /* the path, stripped */
            snprintf(want, sizeof want, "Host: d.test:%d\r\n", g_fd_port);
            CHECK(strstr(out, want) != NULL);                              /* the daemon's origin */
            /* Its own page's post goes as the daemon's origin... */
            snprintf(req, sizeof req, "POST %s/signin HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nOrigin: http://127.0.0.1:%d\r\n"
                     "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: 3\r\n\r\na=b", tp, lport, lport);
            http_raw(lport, req, out, sizeof out);
            snprintf(want, sizeof want, "Origin: https://d.test:%d\r\n", g_fd_port);
            CHECK(strstr(out, want) != NULL && strstr(out, "\r\n\r\na=b") != NULL);
            /* The second step's post (AUTH.md §8.6) is carried the same way. */
            snprintf(req, sizeof req, "POST %s/signin/verify HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nOrigin: http://127.0.0.1:%d\r\n"
                     "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: 3\r\n\r\nc=d", tp, lport, lport);
            http_raw(lport, req, out, sizeof out);
            CHECK(strstr(out, "POST /signin/verify HTTP/1.1\r\n") != NULL && strstr(out, "\r\n\r\nc=d") != NULL);
            /* ...another site's keeps its own, for the daemon to refuse. */
            snprintf(req, sizeof req, "POST %s/signin HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nOrigin: https://evil.example\r\n"
                     "Content-Length: 0\r\n\r\n", tp, lport);
            http_raw(lport, req, out, sizeof out);
            CHECK(strstr(out, "Origin: https://evil.example\r\n") != NULL && !strstr(out, "Origin: https://d.test"));
            /* Not a sign-in page, not this listener's name, not its secret: not carried. */
            snprintf(req, sizeof req, "GET %s/webhook/00 HTTP/1.1\r\nHost: 127.0.0.1:%d\r\n\r\n", tp, lport);
            http_raw(lport, req, out, sizeof out);
            CHECK(strstr(out, "HTTP/1.1 404") == out);
            snprintf(req, sizeof req, "GET %s/signin HTTP/1.1\r\nHost: evil.example:%d\r\n\r\n", tp, lport);
            http_raw(lport, req, out, sizeof out);
            CHECK(strstr(out, "HTTP/1.1 404") == out);
            snprintf(req, sizeof req, "GET /p/not-the-secret/signin HTTP/1.1\r\nHost: 127.0.0.1:%d\r\n\r\n", lport);
            http_raw(lport, req, out, sizeof out);
            CHECK(strstr(out, "HTTP/1.1 404") == out);
        } else {
            snprintf(req, sizeof req, "GET %s/signin HTTP/1.1\r\nHost: 127.0.0.1:%d\r\n\r\n", tp, lport);
            http_raw(lport, req, out, sizeof out);
            CHECK(strstr(out, "HTTP/1.1 502") == out);                     /* not the accepted certificate */
        }
        atomic_store(&sa.cancel, 1);
        pthread_join(sth, NULL);
        oc_loopback_close(lb);
    }
    atomic_store(&g_fd_stop, 1);
    pthread_join(dth, NULL);
    close(g_fd_listen);
    oc_tls_server_free(&g_fd_srv);
}

int run_signin_tests(void) {
    printf("test_signin: verifier + challenge, loopback listener (one GET, wrong path and "
           "method ignored, cancel, timeout), query reading\n");

    /* Verifier and challenge: the right shapes, never the same twice, and a pair
     * the daemon's own check accepts — and refuses with one character changed. */
    {
        char v1[OC_SIGNIN_VERIFIER_LEN + 1], c1[OC_SIGNIN_CHALLENGE_LEN + 1];
        char v2[OC_SIGNIN_VERIFIER_LEN + 1], c2[OC_SIGNIN_CHALLENGE_LEN + 1];
        CHECK(oc_signin_verifier(v1, c1) == 0 && oc_signin_verifier(v2, c2) == 0);
        CHECK(strlen(v1) == 43 && strlen(c1) == 43);
        CHECK(strcmp(v1, v2) != 0 && strcmp(c1, c2) != 0);
        CHECK(oc_jwt_nonce_matches(c1, (const uint8_t *)v1, strlen(v1)) == 1);
        CHECK(oc_jwt_nonce_matches(c1, (const uint8_t *)v2, strlen(v2)) == 0);
        oc_signin_wipe(v1, sizeof v1);
        CHECK(v1[0] == '\0' && v1[42] == '\0');
    }

    /* The listener: loopback, a secret path, and exactly one answer accepted. */
    {
        char uri[128];
        oc_loopback *lb = oc_loopback_open(uri, sizeof uri);
        CHECK(lb != NULL);
        CHECK(strncmp(uri, "http://127.0.0.1:", 17) == 0);
        struct browser b;
        memset(&b, 0, sizeof b);
        b.port = atoi(uri + 17);
        const char *path = strchr(uri + 17, '/');
        CHECK(b.port > 0 && path && strncmp(path, "/cb/", 4) == 0 && strlen(path) >= 4 + 20);
        snprintf(b.path, sizeof b.path, "%s", path ? path : "/");

        pthread_t th;
        CHECK(pthread_create(&th, NULL, browser_thread, &b) == 0);
        char query[256];
        CHECK(oc_loopback_wait(lb, 5000, NULL, query, sizeof query) == OC_LOOPBACK_OK);
        /* The tab waits for the outcome; it is told once there is one. */
        oc_loopback_answer(lb, OC_LOOPBACK_SIGNED_IN, NULL);
        oc_loopback_answer(lb, OC_LOOPBACK_REFUSED, "again");   /* answered already: nothing */
        pthread_join(th, NULL);
        CHECK(strcmp(query, "token=aaa.bbb.ccc&x=a%20b+c") == 0);
        CHECK(strstr(b.first, "404") != NULL);    /* not a GET */
        CHECK(strstr(b.second, "404") != NULL);   /* not the secret */
        CHECK(strstr(b.third, "200") != NULL);

        char val[64];
        CHECK(oc_query_get(query, "token", val, sizeof val) == 1 && strcmp(val, "aaa.bbb.ccc") == 0);
        CHECK(oc_query_get(query, "x", val, sizeof val) == 1 && strcmp(val, "a b c") == 0);
        CHECK(oc_query_get(query, "missing", val, sizeof val) == 0);
        CHECK(oc_query_get(query, "tok", val, sizeof val) == 0);          /* a prefix is not the key */
        CHECK(oc_query_get(query, "token", val, 4) == -1);                /* never truncated */
        CHECK(oc_query_get("a=%zz", "a", val, sizeof val) == -1);
        CHECK(oc_query_get("a=%00", "a", val, sizeof val) == -1);
        CHECK(oc_query_get("a=%4", "a", val, sizeof val) == -1);
        oc_loopback_close(lb);

        /* Closed means closed: nothing is listening there any more. */
        char st[64];
        http_get(b.port, "GET / HTTP/1.1\r\n\r\n", st, sizeof st);
        CHECK(st[0] == '\0');
    }

    /* On a host with no IPv4 loopback the listener takes IPv6's, and says so in
     * its redirect -- which the daemon accepts (PROTOCOL.md §4.2, `redirect_uri`). */
    {
        int probe = socket(AF_INET6, SOCK_STREAM, 0);
        struct sockaddr_in6 p6; memset(&p6, 0, sizeof p6);
        p6.sin6_family = AF_INET6; p6.sin6_addr = in6addr_loopback;
        int have6 = probe >= 0 && bind(probe, (struct sockaddr *)&p6, sizeof p6) == 0;
        if (probe >= 0) close(probe);
        if (!have6) {
            printf("  (no IPv6 loopback on this host: the [::1] listener is skipped)\n");
        } else {
            oc_loopback_force_v6(1);
            char uri[128];
            oc_loopback *lb = oc_loopback_open(uri, sizeof uri);
            oc_loopback_force_v6(0);
            CHECK(lb != NULL);
            CHECK(strncmp(uri, "http://[::1]:", 13) == 0);
            int port = atoi(uri + 13);
            const char *path = strchr(uri + 13, '/');
            CHECK(port > 0 && path && strncmp(path, "/cb/", 4) == 0);
            if (lb && port > 0 && path) {
                int fd = socket(AF_INET6, SOCK_STREAM, 0);
                p6.sin6_port = htons((uint16_t)port);
                CHECK(connect(fd, (struct sockaddr *)&p6, sizeof p6) == 0);
                char req[256];
                int rn = snprintf(req, sizeof req, "GET %s?token=v6 HTTP/1.1\r\nHost: [::1]\r\n\r\n", path);
                ssize_t w = write(fd, req, (size_t)rn); (void)w;
                char query[64];
                CHECK(oc_loopback_wait(lb, 5000, NULL, query, sizeof query) == OC_LOOPBACK_OK);
                CHECK(strcmp(query, "token=v6") == 0);
                close(fd);
            }
            oc_loopback_close(lb);
        }
    }

    /* A direct connection's provider: a redirect with no path, the callback at
     * "/", and the same port on [::1] for a redirect written as `localhost`. */
    {
        char uri[128];
        oc_loopback *lb = oc_loopback_open_provider(uri, sizeof uri);
        CHECK(lb != NULL);
        CHECK(strncmp(uri, "http://127.0.0.1:", 17) == 0 && strchr(uri + 17, '/') == NULL);
        int port = atoi(uri + 17);
        CHECK(port > 0);
        for (int six = 0; lb && port > 0 && six < 2; six++) {
            int fd;
            if (six) {
                struct sockaddr_in6 a6; memset(&a6, 0, sizeof a6);
                a6.sin6_family = AF_INET6; a6.sin6_addr = in6addr_loopback; a6.sin6_port = htons((uint16_t)port);
                /* whether this host has IPv6 loopback at all, asked apart from the listener */
                struct sockaddr_in6 p6 = a6; p6.sin6_port = 0;
                int probe = socket(AF_INET6, SOCK_STREAM, 0);
                int have6 = probe >= 0 && bind(probe, (struct sockaddr *)&p6, sizeof p6) == 0;
                if (probe >= 0) close(probe);
                if (!have6) { printf("  (no IPv6 loopback on this host: the [::1] callback is skipped)\n"); break; }
                fd = socket(AF_INET6, SOCK_STREAM, 0);
                CHECK(connect(fd, (struct sockaddr *)&a6, sizeof a6) == 0);
            } else {
                struct sockaddr_in a4; memset(&a4, 0, sizeof a4);
                a4.sin_family = AF_INET; a4.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a4.sin_port = htons((uint16_t)port);
                fd = socket(AF_INET, SOCK_STREAM, 0);
                CHECK(connect(fd, (struct sockaddr *)&a4, sizeof a4) == 0);
            }
            const char *req = six ? "GET /?code=c6&state=s6 HTTP/1.1\r\nHost: localhost\r\n\r\n"
                                  : "GET /?code=c4&state=s4 HTTP/1.1\r\nHost: localhost\r\n\r\n";
            ssize_t w = write(fd, req, strlen(req)); (void)w;
            char query[64];
            CHECK(oc_loopback_wait(lb, 5000, NULL, query, sizeof query) == OC_LOOPBACK_OK);
            CHECK(strcmp(query, six ? "code=c6&state=s6" : "code=c4&state=s4") == 0);
            close(fd);
        }
        /* Another path is not the callback. */
        if (lb && port > 0) {
            int fd = socket(AF_INET, SOCK_STREAM, 0);
            struct sockaddr_in a4; memset(&a4, 0, sizeof a4);
            a4.sin_family = AF_INET; a4.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a4.sin_port = htons((uint16_t)port);
            CHECK(connect(fd, (struct sockaddr *)&a4, sizeof a4) == 0);
            const char *req = "GET /cb/x?code=c&state=s HTTP/1.1\r\n\r\n";
            ssize_t w = write(fd, req, strlen(req)); (void)w;
            char query[64];
            CHECK(oc_loopback_wait(lb, 400, NULL, query, sizeof query) == OC_LOOPBACK_TIMEOUT);
            char st[64] = "";
            ssize_t n = read(fd, st, sizeof st - 1);
            if (n > 0) st[n] = '\0';
            CHECK(strstr(st, "404") != NULL);
            close(fd);
        }
        oc_loopback_close(lb);
    }

    /* Two attempts never share a secret or, in practice, a port. */
    {
        char u1[128], u2[128];
        oc_loopback *a = oc_loopback_open(u1, sizeof u1), *b = oc_loopback_open(u2, sizeof u2);
        CHECK(a && b && strcmp(u1, u2) != 0);
        CHECK(strcmp(strrchr(u1, '/'), strrchr(u2, '/')) != 0);
        /* Nobody comes: it gives up. */
        char q[32];
        CHECK(oc_loopback_wait(a, 300, NULL, q, sizeof q) == OC_LOOPBACK_TIMEOUT);
        /* The person presses cancel. */
        g_cancel = 0;
        pthread_t th;
        CHECK(pthread_create(&th, NULL, cancel_thread, NULL) == 0);
        CHECK(oc_loopback_wait(b, 5000, &g_cancel, q, sizeof q) == OC_LOOPBACK_CANCELLED);
        pthread_join(th, NULL);
        oc_loopback_close(a);
        oc_loopback_close(b);
        /* A buffer too small for the redirect is refused, not truncated. */
        char tiny[8];
        CHECK(oc_loopback_open(tiny, sizeof tiny) == NULL);
    }
    test_tunnel();
    return failures;
}
