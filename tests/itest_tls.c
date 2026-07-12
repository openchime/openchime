/* Integration test for the TLS wrapper (src/tls.c): the daemon generates a
 * self-signed cert (ARCH-10), a client connects over real TCP with that cert's
 * fingerprint pinned (TOFU), and a byte round-trips through the tunnel. Uses
 * blocking loopback sockets and a server thread; hermetic and non-interactive,
 * so it runs under `make test`. Includes tls.c directly per the openblocks
 * convention; links vendored mbedTLS + pthread. */

#include "tls.h"
#include "check.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>

static oc_tls_status handshake_blocking(oc_tls_conn *c) {
    for (;;) {
        oc_tls_status st = oc_tls_handshake(c);
        if (st == OC_TLS_WANT_READ || st == OC_TLS_WANT_WRITE) continue;
        return st;
    }
}

struct server_arg {
    int            listen_fd;
    oc_tls_server *srv;
    int            ok;      /* handshake + echo completed */
};

/* Accept one connection, TLS-handshake as server, echo one message back. */
static void *server_thread(void *p) {
    struct server_arg *a = (struct server_arg *)p;
    a->ok = 0;

    int conn_fd = accept(a->listen_fd, NULL, NULL);
    if (conn_fd < 0) return NULL;

    oc_tls_conn c;
    if (oc_tls_conn_init(&c, &a->srv->conf, conn_fd) != 0) { close(conn_fd); return NULL; }
    if (handshake_blocking(&c) != OC_TLS_OK) goto out;

    char buf[128];
    size_t n = 0;
    if (oc_tls_read(&c, buf, sizeof buf, &n) != OC_TLS_OK || n == 0) goto out;

    size_t wrote = 0;
    if (oc_tls_write(&c, buf, n, &wrote) != OC_TLS_OK || wrote != n) goto out;
    a->ok = 1;

out:
    oc_tls_conn_free(&c);
    close(conn_fd);
    return NULL;
}

static void test_tls_handshake_and_echo(void) {
    /* Listen on an ephemeral loopback port. */
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(lfd >= 0);
    int yes = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    CHECK(bind(lfd, (struct sockaddr *)&addr, sizeof addr) == 0);
    CHECK(listen(lfd, 1) == 0);
    socklen_t alen = sizeof addr;
    CHECK(getsockname(lfd, (struct sockaddr *)&addr, &alen) == 0);

    /* Server generates a self-signed cert (in-memory: no paths). */
    oc_tls_server srv;
    CHECK(oc_tls_server_init(&srv, NULL, NULL) == 0);
    uint8_t srv_fp[OC_TLS_FINGERPRINT_LEN];
    CHECK(oc_tls_server_fingerprint(&srv, srv_fp) == 0);

    pthread_t th;
    struct server_arg arg = { lfd, &srv, 0 };
    CHECK(pthread_create(&th, NULL, server_thread, &arg) == 0);

    /* Client connects and pins the server's fingerprint (TOFU). */
    int cfd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(cfd >= 0);
    CHECK(connect(cfd, (struct sockaddr *)&addr, sizeof addr) == 0);

    oc_tls_client cli;
    CHECK(oc_tls_client_init(&cli, srv_fp) == 0);
    oc_tls_conn c;
    CHECK(oc_tls_conn_init(&c, &cli.conf, cfd) == 0);
    CHECK(handshake_blocking(&c) == OC_TLS_OK);

    /* The pinned peer cert is exactly the server's cert. */
    uint8_t peer_fp[OC_TLS_FINGERPRINT_LEN];
    CHECK(oc_tls_peer_fingerprint(&c, peer_fp) == 0);
    CHECK(memcmp(peer_fp, srv_fp, sizeof peer_fp) == 0);

    /* Round-trip a message through the tunnel. */
    const char *msg = "ping openchime";
    size_t sent = 0;
    CHECK(oc_tls_write(&c, msg, strlen(msg), &sent) == OC_TLS_OK && sent == strlen(msg));
    char got[128];
    size_t n = 0;
    CHECK(oc_tls_read(&c, got, sizeof got, &n) == OC_TLS_OK);
    CHECK(n == strlen(msg) && memcmp(got, msg, n) == 0);

    pthread_join(th, NULL);
    CHECK(arg.ok == 1);

    oc_tls_conn_free(&c);
    oc_tls_client_free(&cli);
    oc_tls_server_free(&srv);
    close(cfd);
    close(lfd);
}

/* A wrong pin must make the handshake fail (no silent trust). */
static void test_tls_pin_mismatch(void) {
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    bind(lfd, (struct sockaddr *)&addr, sizeof addr);
    listen(lfd, 1);
    socklen_t alen = sizeof addr;
    getsockname(lfd, (struct sockaddr *)&addr, &alen);

    oc_tls_server srv;
    CHECK(oc_tls_server_init(&srv, NULL, NULL) == 0);

    pthread_t th;
    struct server_arg arg = { lfd, &srv, 0 };
    pthread_create(&th, NULL, server_thread, &arg);

    int cfd = socket(AF_INET, SOCK_STREAM, 0);
    connect(cfd, (struct sockaddr *)&addr, sizeof addr);

    uint8_t bogus[OC_TLS_FINGERPRINT_LEN];
    memset(bogus, 0xEE, sizeof bogus);           /* not the server's fingerprint */
    oc_tls_client cli;
    CHECK(oc_tls_client_init(&cli, bogus) == 0);
    oc_tls_conn c;
    CHECK(oc_tls_conn_init(&c, &cli.conf, cfd) == 0);
    CHECK(handshake_blocking(&c) == OC_TLS_ERROR); /* pin mismatch rejects */

    /* Close the client first so the server's blocking read unblocks: its
     * handshake completed at the TLS layer; only our app-level pin rejected. */
    oc_tls_conn_free(&c);
    close(cfd);
    pthread_join(th, NULL);
    oc_tls_client_free(&cli);
    oc_tls_server_free(&srv);
    close(lfd);
}

int run_tls_tests(void) {
    printf("itest_tls: self-signed cert generation, TOFU-pinned handshake,\n");
    printf("           byte round-trip, pin-mismatch rejection\n");
    test_tls_handshake_and_echo();
    test_tls_pin_mismatch();
    return failures;
}
