/* The client's half of a browser sign-in (client/core/signin.c): verifier and
 * challenge, the loopback listener against a scripted "browser", query reading. */

#include "check.h"
#include "../client/core/signin.h"
#include "../daemon/jwt.h"     /* the daemon's own check of the pair */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
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

static volatile int g_cancel;
static void *cancel_thread(void *arg) { (void)arg; usleep(150000); g_cancel = 1; return NULL; }

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
    return failures;
}
