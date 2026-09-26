/* Integration test for the TLS wrapper (src/tls.c): the daemon generates a
 * self-signed cert (ARCH-10), a client connects over real TCP with that cert's
 * fingerprint pinned (TOFU), and a byte round-trips through the tunnel. Uses
 * blocking loopback sockets and a server thread; hermetic and non-interactive,
 * so it runs under `make test`. Includes tls.c directly per the openblocks
 * convention; links vendored mbedTLS + pthread. */

#include "tls.h"
#include "protocol.h"
#include "config.h"
#include "check.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <mbedtls/x509_crt.h>

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

/* What the client named in its handshake, as the server saw it. */
static char g_seen_sni[256];
static int seen_sni_cb(void *ctx, mbedtls_ssl_context *ssl, const unsigned char *name, size_t len) {
    (void)ctx; (void)ssl;
    if (len >= sizeof g_seen_sni) len = sizeof g_seen_sni - 1;
    memcpy(g_seen_sni, name, len);
    g_seen_sni[len] = '\0';
    return 0;
}

/* One handshake against a fresh self-signed server, the client naming `sni` (NULL =
 * none) and pinning `pin` (NULL = first use). Returns the client's result. */
static oc_tls_status handshake_naming(const char *sni, int wrong_pin, int first_use) {
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(lfd, (struct sockaddr *)&addr, sizeof addr);
    listen(lfd, 1);
    socklen_t alen = sizeof addr;
    getsockname(lfd, (struct sockaddr *)&addr, &alen);

    oc_tls_server srv;
    CHECK(oc_tls_server_init(&srv, NULL, NULL) == 0);
    g_seen_sni[0] = '\0';
    mbedtls_ssl_conf_sni(&srv.conf, seen_sni_cb, NULL);
    uint8_t fp[OC_TLS_FINGERPRINT_LEN];
    CHECK(oc_tls_server_fingerprint(&srv, fp) == 0);
    if (wrong_pin) fp[0] ^= 0xFF;

    pthread_t th;
    struct server_arg arg = { lfd, &srv, 0 };
    pthread_create(&th, NULL, server_thread, &arg);

    int cfd = socket(AF_INET, SOCK_STREAM, 0);
    connect(cfd, (struct sockaddr *)&addr, sizeof addr);
    oc_tls_client cli;
    CHECK(oc_tls_client_init(&cli, first_use ? NULL : fp) == 0);
    oc_tls_conn c;
    CHECK(oc_tls_conn_init(&c, &cli.conf, cfd) == 0);
    if (sni) CHECK(oc_tls_conn_set_hostname(&c, sni) == 0);
    oc_tls_status st = handshake_blocking(&c);

    oc_tls_conn_free(&c);
    close(cfd);
    pthread_join(th, NULL);
    oc_tls_client_free(&cli);
    oc_tls_server_free(&srv);
    close(lfd);
    return st;
}

/* The client names the workspace in its handshake, and trust is STILL the pin, not
 * the name: the daemon's self-signed certificate carries no such name, and that
 * must not turn every pinned workspace into "certificate changed". */
static void test_tls_sni_does_not_replace_the_pin(void) {
    /* A name the certificate does not carry: pinned, it completes... */
    CHECK(handshake_naming("acme.workspace.openchime.test", 0, 0) == OC_TLS_OK);
    CHECK(strcmp(g_seen_sni, "acme.workspace.openchime.test") == 0);
    /* ...on first use too, before there is a pin... */
    CHECK(handshake_naming("acme.workspace.openchime.test", 0, 1) == OC_TLS_OK);
    /* ...and a WRONG pin still fails, name or no name. */
    CHECK(handshake_naming("acme.workspace.openchime.test", 1, 0) == OC_TLS_ERROR);
    CHECK(handshake_naming(NULL, 1, 0) == OC_TLS_ERROR);
    /* No name given, none sent. */
    CHECK(handshake_naming(NULL, 0, 0) == OC_TLS_OK);
    CHECK(g_seen_sni[0] == '\0');
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

/* --- CA verification: the built-in roots and OPENCHIME_EXTRA_CA ------------- */

extern const char oc_ca_roots_pem[];
extern const size_t oc_ca_roots_pem_size;

/* Every root compiled in parses with the mbedTLS we ship. The loader tolerates a
 * root it cannot read, so without this a refresh could drop one silently. */
static void test_tls_builtin_roots_parse(void) {
    size_t blocks = 0;
    for (const char *p = oc_ca_roots_pem; (p = strstr(p, "-----BEGIN CERTIFICATE-----")); p++)
        blocks++;
    CHECK(blocks >= 100);

    mbedtls_x509_crt roots;
    mbedtls_x509_crt_init(&roots);
    CHECK(mbedtls_x509_crt_parse(&roots, (const unsigned char *)oc_ca_roots_pem,
                                 oc_ca_roots_pem_size) == 0);
    size_t parsed = 0;
    for (const mbedtls_x509_crt *c = &roots; c && c->raw.len; c = c->next) parsed++;
    CHECK(parsed == blocks);
    mbedtls_x509_crt_free(&roots);
}

/* A private CA and a server certificate it issued, as PEM files in `dir`. */
struct private_pki {
    char ca[128], cert[128], key[128];
};

static int write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    int ok = fputs(text, f) >= 0;
    return (fclose(f) == 0 && ok) ? 0 : -1;
}

/* Sign a P-256 certificate for `subject_key`, named `subject`, with the issuer's
 * key and name, into `pem`. `is_ca` makes it a CA that may sign others. */
static int mint(mbedtls_pk_context *subject_key, const char *subject,
                mbedtls_pk_context *issuer_key, const char *issuer, int is_ca,
                unsigned char serial, mbedtls_ctr_drbg_context *rng,
                unsigned char *pem, size_t cap) {
    mbedtls_x509write_cert w;
    mbedtls_x509write_crt_init(&w);
    int rc;
    mbedtls_x509write_crt_set_subject_key(&w, subject_key);
    mbedtls_x509write_crt_set_issuer_key(&w, issuer_key);
    mbedtls_x509write_crt_set_version(&w, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&w, MBEDTLS_MD_SHA256);
    if ((rc = mbedtls_x509write_crt_set_subject_name(&w, subject)) != 0 ||
        (rc = mbedtls_x509write_crt_set_issuer_name(&w, issuer)) != 0 ||
        (rc = mbedtls_x509write_crt_set_serial_raw(&w, &serial, 1)) != 0 ||
        (rc = mbedtls_x509write_crt_set_validity(&w, "20200101000000", "20500101000000")) != 0 ||
        (rc = mbedtls_x509write_crt_set_basic_constraints(&w, is_ca, -1)) != 0 ||
        (is_ca && (rc = mbedtls_x509write_crt_set_key_usage(&w, MBEDTLS_X509_KU_KEY_CERT_SIGN)) != 0))
        goto done;
    rc = mbedtls_x509write_crt_pem(&w, pem, cap, mbedtls_ctr_drbg_random, rng);
done:
    mbedtls_x509write_crt_free(&w);
    return rc;
}

static int make_private_pki(const char *dir, struct private_pki *out) {
    mbedtls_entropy_context ent;
    mbedtls_ctr_drbg_context rng;
    mbedtls_pk_context ca_key, leaf_key;
    mbedtls_entropy_init(&ent);
    mbedtls_ctr_drbg_init(&rng);
    mbedtls_pk_init(&ca_key);
    mbedtls_pk_init(&leaf_key);
    unsigned char ca_pem[4096], leaf_pem[4096], key_pem[2048];
    int rc = -1;

    if (mbedtls_ctr_drbg_seed(&rng, mbedtls_entropy_func, &ent, NULL, 0) != 0) goto done;
    mbedtls_pk_context *keys[2] = { &ca_key, &leaf_key };
    for (int i = 0; i < 2; i++)
        if (mbedtls_pk_setup(keys[i], mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0 ||
            mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(*keys[i]),
                                mbedtls_ctr_drbg_random, &rng) != 0)
            goto done;
    if (mint(&ca_key, "CN=Private Test Root", &ca_key, "CN=Private Test Root", 1, 1,
             &rng, ca_pem, sizeof ca_pem) != 0 ||
        mint(&leaf_key, "CN=localhost", &ca_key, "CN=Private Test Root", 0, 2,
             &rng, leaf_pem, sizeof leaf_pem) != 0 ||
        mbedtls_pk_write_key_pem(&leaf_key, key_pem, sizeof key_pem) != 0)
        goto done;

    snprintf(out->ca, sizeof out->ca, "%s/ca.pem", dir);
    snprintf(out->cert, sizeof out->cert, "%s/cert.pem", dir);
    snprintf(out->key, sizeof out->key, "%s/key.pem", dir);
    if (write_file(out->ca, (char *)ca_pem) == 0 &&
        write_file(out->cert, (char *)leaf_pem) == 0 &&
        write_file(out->key, (char *)key_pem) == 0)
        rc = 0;
done:
    mbedtls_pk_free(&ca_key);
    mbedtls_pk_free(&leaf_key);
    mbedtls_ctr_drbg_free(&rng);
    mbedtls_entropy_free(&ent);
    return rc;
}

/* Handshake a CA-verifying client, expecting `host`, against a server presenting
 * the private PKI's certificate. Returns the client's handshake status. */
static oc_tls_status ca_handshake(const struct private_pki *pki, const char *host) {
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(lfd >= 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(lfd, (struct sockaddr *)&addr, sizeof addr) == 0);
    CHECK(listen(lfd, 1) == 0);
    socklen_t alen = sizeof addr;
    CHECK(getsockname(lfd, (struct sockaddr *)&addr, &alen) == 0);

    oc_tls_server srv;
    CHECK(oc_tls_server_init(&srv, pki->cert, pki->key) == 0);
    pthread_t th;
    struct server_arg arg = { lfd, &srv, 0 };
    CHECK(pthread_create(&th, NULL, handshake_only_server, &arg) == 0);

    int cfd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(connect(cfd, (struct sockaddr *)&addr, sizeof addr) == 0);
    oc_tls_client cli;
    oc_tls_status st = OC_TLS_ERROR;
    if (oc_tls_client_init_ca(&cli) == 0) {
        oc_tls_conn c;
        CHECK(oc_tls_conn_init(&c, &cli.conf, cfd) == 0);
        CHECK(oc_tls_conn_set_hostname(&c, host) == 0);
        st = handshake_blocking(&c);
        oc_tls_conn_free(&c);
    }
    close(cfd);
    pthread_join(th, NULL);
    oc_tls_client_free(&cli);
    oc_tls_server_free(&srv);
    close(lfd);
    return st;
}

static void test_tls_extra_ca(void) {
    char dir[] = "/tmp/oc-extra-ca-XXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    struct private_pki pki;
    CHECK(make_private_pki(dir, &pki) == 0);

    /* The built-in roots alone do not reach a private CA: verification is real. */
    CHECK(oc_tls_set_extra_ca(NULL) == 0);
    CHECK(ca_handshake(&pki, "localhost") == OC_TLS_ERROR);

    /* Added, the private root verifies its server... */
    CHECK(oc_tls_set_extra_ca(pki.ca) == 0);
    CHECK(ca_handshake(&pki, "localhost") == OC_TLS_OK);
    /* ...under its own name only: the hostname is still checked. */
    CHECK(ca_handshake(&pki, "wrong.example") == OC_TLS_ERROR);

    /* A file that is missing, or is not certificates, is refused -- and a
     * refusal leaves no extra roots behind, rather than the previous ones. */
    char missing[160], junk[160], half[160];
    snprintf(missing, sizeof missing, "%s/absent.pem", dir);
    snprintf(junk, sizeof junk, "%s/junk.pem", dir);
    snprintf(half, sizeof half, "%s/half.pem", dir);
    CHECK(oc_tls_set_extra_ca(missing) == -1);
    CHECK(ca_handshake(&pki, "localhost") == OC_TLS_ERROR);
    CHECK(write_file(junk, "not a certificate\n") == 0);
    CHECK(oc_tls_set_extra_ca(junk) == -1);
    CHECK(write_file(junk, "") == 0);
    CHECK(oc_tls_set_extra_ca(junk) == -1);

    /* One good certificate beside one that does not parse is refused whole: the
     * operator would otherwise trust less than they wrote. */
    FILE *f = fopen(pki.ca, "r");
    char ca_text[4096] = "";
    size_t n = f ? fread(ca_text, 1, sizeof ca_text - 1, f) : 0;
    if (f) fclose(f);
    ca_text[n] = '\0';
    char both[8192];
    snprintf(both, sizeof both, "%s-----BEGIN CERTIFICATE-----\nAAAA\n-----END CERTIFICATE-----\n",
             ca_text);
    CHECK(write_file(half, both) == 0);
    CHECK(oc_tls_set_extra_ca(half) == -1);

    /* The daemon reads it at boot, and a file it cannot use stops the boot. */
    char cfgerr[256] = "";
    setenv("OPENCHIME_EXTRA_CA", missing, 1);
    CHECK(oc_config_load(cfgerr, sizeof cfgerr) == -1);
    CHECK(strstr(cfgerr, "OPENCHIME_EXTRA_CA") != NULL);
    setenv("OPENCHIME_EXTRA_CA", pki.ca, 1);
    CHECK(oc_config_load(cfgerr, sizeof cfgerr) == 0);
    CHECK(ca_handshake(&pki, "localhost") == OC_TLS_OK);
    unsetenv("OPENCHIME_EXTRA_CA");
    CHECK(oc_config_load(cfgerr, sizeof cfgerr) == 0);
    CHECK(ca_handshake(&pki, "localhost") == OC_TLS_ERROR);

    unlink(pki.ca); unlink(pki.cert); unlink(pki.key); unlink(junk); unlink(half);
    rmdir(dir);
}

int run_tls_tests(void) {
    printf("itest_tls: self-signed cert generation, TOFU-pinned handshake,\n");
    printf("           byte round-trip, pin-mismatch rejection, ALPN demux,\n");
    printf("           built-in CA roots, OPENCHIME_EXTRA_CA\n");
    test_tls_handshake_and_echo();
    test_tls_pin_mismatch();
    test_tls_sni_does_not_replace_the_pin();
    test_tls_alpn_demux();
    test_tls_builtin_roots_parse();
    test_tls_extra_ca();
    return failures;
}
