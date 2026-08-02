/* Integration test for the TLS wrapper (src/tls.c): the daemon generates a
 * self-signed cert (ARCH-10), a client connects over real TCP with that cert's
 * fingerprint pinned (TOFU), and a byte round-trips through the tunnel. Uses
 * blocking loopback sockets and a server thread; hermetic and non-interactive,
 * so it runs under `make test`. Includes tls.c directly per the openblocks
 * convention; links vendored mbedTLS + pthread. */

#include "tls.h"
#include "protocol.h"
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

    /* Both sides negotiated the binary-protocol ALPN (443 demux, PROTOCOL.md §1). */
    const char *alpn = oc_tls_alpn_selected(&c);
    CHECK(alpn != NULL && strcmp(alpn, OC_ALPN_PROTO) == 0);

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

/* Accept one connection and handshake as server, nothing more: the ALPN cases
 * below assert on the negotiated protocol, not on any traffic. */
static void *handshake_only_server(void *p) {
    struct server_arg *a = (struct server_arg *)p;
    a->ok = 0;
    int conn_fd = accept(a->listen_fd, NULL, NULL);
    if (conn_fd < 0) return NULL;
    oc_tls_conn c;
    if (oc_tls_conn_init(&c, &a->srv->conf, conn_fd) != 0) { close(conn_fd); return NULL; }
    if (handshake_blocking(&c) == OC_TLS_OK) a->ok = 1;
    oc_tls_conn_free(&c);
    close(conn_fd);
    return NULL;
}

/* Handshake one client offering `alpn` against a fresh server. Returns the
 * client's handshake status; on OK, *selected is the negotiated protocol copied
 * into `sel_buf` (mbedTLS returns a pointer into the config, freed on return). */
static oc_tls_status alpn_handshake(const char **alpn, char *sel_buf, size_t sel_cap) {
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

    oc_tls_server srv;
    CHECK(oc_tls_server_init(&srv, NULL, NULL) == 0);
    pthread_t th;
    struct server_arg arg = { lfd, &srv, 0 };
    CHECK(pthread_create(&th, NULL, handshake_only_server, &arg) == 0);

    int cfd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(cfd >= 0);
    CHECK(connect(cfd, (struct sockaddr *)&addr, sizeof addr) == 0);

    oc_tls_client cli;
    CHECK(oc_tls_client_init_ex(&cli, NULL, alpn) == 0);
    oc_tls_conn c;
    CHECK(oc_tls_conn_init(&c, &cli.conf, cfd) == 0);
    oc_tls_status st = handshake_blocking(&c);

    sel_buf[0] = '\0';
    if (st == OC_TLS_OK) {
        const char *sel = oc_tls_alpn_selected(&c);
        if (sel) { strncpy(sel_buf, sel, sel_cap - 1); sel_buf[sel_cap - 1] = '\0'; }
    }

    oc_tls_conn_free(&c);
    close(cfd);
    pthread_join(th, NULL);
    oc_tls_client_free(&cli);
    oc_tls_server_free(&srv);
    close(lfd);
    return st;
}

/* The 443 ALPN demux (ARCH-54) as the two real callers exercise it: an app
 * client offering oc/1, and a webhook sender that is an ordinary HTTPS client.
 * The sender is the case that used to be refused at the handshake — the server
 * advertised oc/1 alone, so mbedTLS answered any other list with
 * `no_application_protocol` and the HTTP handler was unreachable (REQ-170). */
static void test_tls_alpn_demux(void) {
    char sel[32];

    /* An ordinary HTTPS client's list (curl's default) negotiates http/1.1. */
    static const char *https_alpn[] = { "h2", OC_ALPN_HTTP11, NULL };
    CHECK(alpn_handshake(https_alpn, sel, sizeof sel) == OC_TLS_OK);
    CHECK(strcmp(sel, OC_ALPN_HTTP11) == 0);

    /* http/1.1 does not displace the binary protocol: the server's preference
     * order decides, so a peer offering both still gets oc/1. */
    static const char *both_alpn[] = { OC_ALPN_HTTP11, OC_ALPN_PROTO, NULL };
    CHECK(alpn_handshake(both_alpn, sel, sizeof sel) == OC_TLS_OK);
    CHECK(strcmp(sel, OC_ALPN_PROTO) == 0);

    /* A client offering no ALPN at all still connects, selecting nothing; the
     * demux reads that as HTTP. */
    CHECK(alpn_handshake(NULL, sel, sizeof sel) == OC_TLS_OK);
    CHECK(sel[0] == '\0');

    /* A protocol the daemon does not speak is still refused, rather than being
     * silently handed HTTP bytes an h2-only peer cannot parse. */
    static const char *h2_only[] = { "h2", NULL };
    CHECK(alpn_handshake(h2_only, sel, sizeof sel) == OC_TLS_ERROR);
}

int run_tls_tests(void) {
    printf("itest_tls: self-signed cert generation, TOFU-pinned handshake,\n");
    printf("           byte round-trip, pin-mismatch rejection, ALPN demux\n");
    test_tls_handshake_and_echo();
    test_tls_pin_mismatch();
    test_tls_alpn_demux();
    return failures;
}
