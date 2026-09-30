/* The CA-issued certificate (TLS.md, "Certificates"): ACME with TLS-ALPN-01
 * against a fake CA that does what a real one does -- checks every request's JWS
 * signature and nonce, validates a name by connecting to the daemon's TLS
 * listener with ALPN acme-tls/1 and reading the acmeIdentifier extension, and
 * signs the CSR with a test root -- and certificates through a fake central,
 * which checks the machine signature (AUTH.md §8.7). The TLS listener is the
 * daemon's own oc_tls_server, so what is proven is the certificate choice the
 * daemon makes per handshake, the swap on renewal, and the refusals. */

#include "acme.h"
#include "certs.h"
#include "enroll.h"
#include "jwt.h"
#include "push.h"
#include "tls.h"
#include "check.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <mbedtls/base64.h>
#include <mbedtls/pem.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/entropy.h>
#include <mbedtls/sha256.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/x509_csr.h>

#define JSMN_HEADER
#include "jsmn.h"

static void msleep(int ms) { struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L }; nanosleep(&ts, NULL); }

static int listen_loopback(int *port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t al = sizeof a;
    if (bind(fd, (struct sockaddr *)&a, sizeof a) || listen(fd, 16) || getsockname(fd, (struct sockaddr *)&a, &al)) { close(fd); return -1; }
    *port = ntohs(a.sin_port);
    return fd;
}

static int connect_loopback(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons((uint16_t)port);
    struct timeval tv = { 5, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) { close(fd); return -1; }
    return fd;
}

static int handshake(oc_tls_conn *c) {
    for (;;) {
        oc_tls_status st = oc_tls_handshake(c);
        if (st == OC_TLS_OK) return 0;
        if (st != OC_TLS_WANT_READ && st != OC_TLS_WANT_WRITE) return -1;
    }
}

/* --- a test root --------------------------------------------------------- */

static struct {
    mbedtls_entropy_context  ent;
    mbedtls_ctr_drbg_context rng;
    mbedtls_pk_context       key;
    char                     pem[4096];
    char                     path[128];
} g_ca;

static int ca_init(void) {
    mbedtls_entropy_init(&g_ca.ent); mbedtls_ctr_drbg_init(&g_ca.rng); mbedtls_pk_init(&g_ca.key);
    if (mbedtls_ctr_drbg_seed(&g_ca.rng, mbedtls_entropy_func, &g_ca.ent, NULL, 0) ||
        mbedtls_pk_setup(&g_ca.key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) ||
        mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(g_ca.key), mbedtls_ctr_drbg_random, &g_ca.rng)) return -1;
    mbedtls_x509write_cert w; mbedtls_x509write_crt_init(&w);
    unsigned char serial = 1;
    mbedtls_x509write_crt_set_subject_key(&w, &g_ca.key); mbedtls_x509write_crt_set_issuer_key(&w, &g_ca.key);
    mbedtls_x509write_crt_set_version(&w, MBEDTLS_X509_CRT_VERSION_3); mbedtls_x509write_crt_set_md_alg(&w, MBEDTLS_MD_SHA256);
    int rc = mbedtls_x509write_crt_set_subject_name(&w, "CN=ACME Test Root") ||
             mbedtls_x509write_crt_set_issuer_name(&w, "CN=ACME Test Root") ||
             mbedtls_x509write_crt_set_serial_raw(&w, &serial, 1) ||
             mbedtls_x509write_crt_set_validity(&w, "20200101000000", "20500101000000") ||
             mbedtls_x509write_crt_set_basic_constraints(&w, 1, -1) ||
             mbedtls_x509write_crt_set_key_usage(&w, MBEDTLS_X509_KU_KEY_CERT_SIGN) ||
             mbedtls_x509write_crt_pem(&w, (unsigned char *)g_ca.pem, sizeof g_ca.pem, mbedtls_ctr_drbg_random, &g_ca.rng) ? -1 : 0;
    mbedtls_x509write_crt_free(&w);
    snprintf(g_ca.path, sizeof g_ca.path, "build/test_acme_ca.pem");
    FILE *f = fopen(g_ca.path, "w");
    if (!f) return -1;
    fputs(g_ca.pem, f); fclose(f);
    return rc;
}

/* How far back what the CA issues is dated: a minute, as a CA allows for clocks;
 * none where a test counts the worker's renewals, which fall at two thirds of a
 * certificate's whole validity -- so a minute's backdate on a life of seconds
 * would make every certificate due the moment it arrived. */
static int g_ca_backdate_s = 60;

/* A leaf for `subject_key`, naming `names` (comma-separated), valid for
 * `life_s` from g_ca_backdate_s ago, signed by the test root. The chain (leaf
 * then root) into `out`. */
static int ca_issue(mbedtls_pk_context *subject_key, const char *names, int life_s, char *out, size_t cap) {
    mbedtls_x509write_cert w; mbedtls_x509write_crt_init(&w);
    char store[512]; snprintf(store, sizeof store, "%s", names);
    mbedtls_x509_san_list nodes[8]; int n = 0; char first[256] = "";
    for (char *t = strtok(store, ","); t && n < 8; t = strtok(NULL, ",")) {
        memset(&nodes[n], 0, sizeof nodes[n]);
        nodes[n].node.type = MBEDTLS_X509_SAN_DNS_NAME;
        nodes[n].node.san.unstructured_name.p = (unsigned char *)t;
        nodes[n].node.san.unstructured_name.len = strlen(t);
        if (n) nodes[n - 1].next = &nodes[n];
        if (!first[0]) snprintf(first, sizeof first, "%s", t);
        n++;
    }
    char subj[300]; snprintf(subj, sizeof subj, "CN=%s", first);
    static unsigned char serial = 10; serial++;
    time_t a = time(NULL) - g_ca_backdate_s, b = time(NULL) + life_s;
    struct tm ta, tb; gmtime_r(&a, &ta); gmtime_r(&b, &tb);
    char nb[16], na[16]; strftime(nb, sizeof nb, "%Y%m%d%H%M%S", &ta); strftime(na, sizeof na, "%Y%m%d%H%M%S", &tb);
    mbedtls_x509write_crt_set_subject_key(&w, subject_key); mbedtls_x509write_crt_set_issuer_key(&w, &g_ca.key);
    mbedtls_x509write_crt_set_version(&w, MBEDTLS_X509_CRT_VERSION_3); mbedtls_x509write_crt_set_md_alg(&w, MBEDTLS_MD_SHA256);
    unsigned char leaf[4096];
    int rc = mbedtls_x509write_crt_set_subject_name(&w, subj) ||
             mbedtls_x509write_crt_set_issuer_name(&w, "CN=ACME Test Root") ||
             mbedtls_x509write_crt_set_serial_raw(&w, &serial, 1) ||
             mbedtls_x509write_crt_set_validity(&w, nb, na) ||
             mbedtls_x509write_crt_set_subject_alternative_name(&w, &nodes[0]) ||
             mbedtls_x509write_crt_pem(&w, leaf, sizeof leaf, mbedtls_ctr_drbg_random, &g_ca.rng) ? -1 : 0;
    mbedtls_x509write_crt_free(&w);
    if (!rc) snprintf(out, cap, "%s%s", (char *)leaf, g_ca.pem);
    return rc;
}

/* --- the daemon's TLS listener -------------------------------------------- */

static oc_tls_server g_srv;
static int g_srv_fd = -1, g_srv_port;
static int g_srv_stop;

static void *serve_one(void *p) {
    int fd = (int)(intptr_t)p;
    oc_tls_conn c;
    if (oc_tls_conn_init(&c, &g_srv.conf, fd) == 0 && handshake(&c) == 0) {
        const char *alpn = oc_tls_alpn_selected(&c);
        if (!alpn || strcmp(alpn, OC_TLS_ALPN_ACME) != 0) {
            for (;;) {                                   /* echo, for a connection that must outlive a swap */
                char b[64]; size_t n = 0;
                oc_tls_status st = oc_tls_read(&c, b, sizeof b, &n);
                if (st == OC_TLS_WANT_READ || st == OC_TLS_WANT_WRITE) continue;
                if (st != OC_TLS_OK || !n) break;
                size_t w = 0;
                if (oc_tls_write(&c, b, n, &w) != OC_TLS_OK) break;
            }
        }
    }
    oc_tls_conn_free(&c);
    close(fd);
    return NULL;
}

static void *srv_thread(void *p) {
    (void)p;
    while (!__atomic_load_n(&g_srv_stop, __ATOMIC_ACQUIRE)) {
        int fd = accept(g_srv_fd, NULL, NULL);
        if (fd < 0) continue;
        if (__atomic_load_n(&g_srv_stop, __ATOMIC_ACQUIRE)) { close(fd); break; }
        struct timeval tv = { 5, 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        pthread_t th;
        if (pthread_create(&th, NULL, serve_one, (void *)(intptr_t)fd) == 0) pthread_detach(th);
        else close(fd);
    }
    return NULL;
}

/* A handshake with the listener offering `alpn` and naming `sni`: its status,
 * and the certificate presented (DER) into `der` if asked. With `ca` the
 * client verifies against the test root (and the name). */
static int probe(const char *alpn, const char *sni, int ca, unsigned char *der, size_t *der_len) {
    int fd = connect_loopback(g_srv_port);
    if (fd < 0) return -1;
    static const char *list[2];
    list[0] = alpn; list[1] = NULL;
    oc_tls_client cli; oc_tls_conn c;
    int rc = -1;
    if ((ca ? oc_tls_client_init_ca(&cli) : oc_tls_client_init_ex(&cli, NULL, alpn ? list : NULL)) != 0) { close(fd); return -1; }
    if (ca && alpn) mbedtls_ssl_conf_alpn_protocols(&cli.conf, list);
    if (oc_tls_conn_init(&c, &cli.conf, fd) == 0 && (!sni || oc_tls_conn_set_hostname(&c, sni) == 0) && handshake(&c) == 0) {
        rc = 0;
        const mbedtls_x509_crt *peer = mbedtls_ssl_get_peer_cert(&c.ssl);
        if (der && peer) { memcpy(der, peer->raw.p, peer->raw.len); *der_len = peer->raw.len; }
    }
    oc_tls_conn_free(&c); oc_tls_client_free(&cli); close(fd);
    return rc;
}

/* The acmeIdentifier extension, critical, holding SHA-256(keyauth), in `der`. */
static int has_acme_id(const unsigned char *der, size_t len, const char *keyauth) {
    unsigned char want[8 + 3 + 4 + 32];
    static const unsigned char head[] = { 0x2b, 0x06, 0x01, 0x05, 0x05, 0x07, 0x01, 0x1f, 0x01, 0x01, 0xff, 0x04, 0x22, 0x04, 0x20 };
    memcpy(want, head, sizeof head);
    mbedtls_sha256((const unsigned char *)keyauth, strlen(keyauth), want + sizeof head, 0);
    for (size_t i = 0; i + sizeof want <= len; i++)
        if (!memcmp(der + i, want, sizeof want)) return 1;
    return 0;
}

/* The first certificate in PEM `pem` as DER. */
static int der_of(const char *pem, unsigned char *out, size_t cap, size_t *len) {
    mbedtls_pem_context p; mbedtls_pem_init(&p);
    size_t used = 0, dl = 0;
    int rc = mbedtls_pem_read_buffer(&p, "-----BEGIN CERTIFICATE-----", "-----END CERTIFICATE-----",
                                     (const unsigned char *)pem, NULL, 0, &used);
    const unsigned char *d = rc == 0 ? mbedtls_pem_get_buffer(&p, &dl) : NULL;
    if (d && dl <= cap) { memcpy(out, d, dl); *len = dl; } else rc = -1;
    mbedtls_pem_free(&p);
    return rc;
}

/* A dNSName SAN [2] IA5String holding `name`, found in the DER: the challenge
 * certificate's critical extension is one mbedTLS will not parse, so it is
 * looked for, as the extension is. */
static int names_raw(const unsigned char *der, size_t len, const char *name) {
    size_t nl = strlen(name);
    for (size_t i = 0; i + 2 + nl <= len; i++)
        if (der[i] == 0x82 && der[i + 1] == nl && !memcmp(der + i + 2, name, nl)) return 1;
    return 0;
}

/* What the listener shows an ACME validation for `name`, as a CA sees it: by an
 * independent TLS client (OpenSSL's), since mbedTLS's refuses the critical
 * acmeIdentifier extension before anything can look at it -- as it should for
 * any certificate but this one. 0 with the DER in `der`, -1 if refused. */
static int acme_view(const char *name, unsigned char *der, size_t cap, size_t *len) {
    char cmd[512];
    snprintf(cmd, sizeof cmd, "openssl s_client -connect 127.0.0.1:%d -servername %s -alpn acme-tls/1 "
                              "</dev/null 2>/dev/null", g_srv_port, name);
    FILE *f = popen(cmd, "r");
    if (!f) return -1;
    static char out[65536]; size_t n = fread(out, 1, sizeof out - 1, f); out[n] = '\0';
    pclose(f);
    if (!strstr(out, "ALPN protocol: acme-tls/1")) return -1;
    return der_of(out, der, cap, len);
}

/* An ACME validation for `name` is refused by the listener. */
static int acme_refused(const char *name) {
    unsigned char der[4096]; size_t dl = 0;
    return acme_view(name, der, sizeof der, &dl) != 0;
}

/* --- a fake CA and central, over plain HTTP on loopback -------------------- */

static struct {
    int      fd, port;
    int stop;
    pthread_mutex_t mu;
    /* ACME */
    char     nonces[64][32]; int n_nonces;
    char     jwk[200];                      /* the account's */
    mbedtls_pk_context acct;                /* ...as a key, once known */
    int      have_acct, accounts_made;
    int      bad_nonce_once, bad_sigs, reused_nonces, wrong_urls;
    char     order_names[256];
    char     token[64];
    int      authz_state;                   /* 0 pending, 1 valid, 2 invalid */
    int      order_valid;
    char     chain[8192];
    int      life_s;                        /* of what it issues */
    int      validate_wrong;                /* validate as if the key authorization were different */
    int      validations;
    /* central */
    char     central_aud[128];
    char     central_pub[1024];             /* the enrollment key, to check signatures with */
    int      central_pending;               /* 202s still to answer */
    int      central_refuse;                /* answer 403 */
    int      central_bad_sigs;
} g_fake;

static int jtok(const char *js, jsmntok_t *t, int n, const char *key, char *out, size_t cap) {
    size_t kl = strlen(key);
    for (int i = 1; i + 1 < n; i++)
        if (t[i].type == JSMN_STRING && (size_t)(t[i].end - t[i].start) == kl && !memcmp(js + t[i].start, key, kl)) {
            int l = t[i + 1].end - t[i + 1].start;
            if ((size_t)l >= cap) l = (int)cap - 1;
            memcpy(out, js + t[i + 1].start, (size_t)l); out[l] = '\0';
            return 0;
        }
    return -1;
}

static void reply(int fd, int status, const char *extra, const char *ctype, const char *body) {
    char head[1024];
    size_t bl = body ? strlen(body) : 0;
    int n = snprintf(head, sizeof head, "HTTP/1.1 %d X\r\nConnection: close\r\nContent-Length: %zu\r\n%s%s%s%s\r\n",
                     status, bl, ctype ? "Content-Type: " : "", ctype ? ctype : "", ctype ? "\r\n" : "", extra ? extra : "");
    (void)!write(fd, head, (size_t)n);
    if (bl) (void)!write(fd, body, bl);
}

static void new_nonce(char *out) {
    static unsigned k = 0;
    snprintf(out, 32, "n%08x%u", (unsigned)rand(), ++k);
    pthread_mutex_lock(&g_fake.mu);
    snprintf(g_fake.nonces[g_fake.n_nonces++ % 64], 32, "%s", out);
    pthread_mutex_unlock(&g_fake.mu);
}

static int take_nonce(const char *n) {
    pthread_mutex_lock(&g_fake.mu);
    int ok = 0;
    for (int i = 0; i < 64 && !ok; i++) if (g_fake.nonces[i][0] && !strcmp(g_fake.nonces[i], n)) { g_fake.nonces[i][0] = '\0'; ok = 1; }
    pthread_mutex_unlock(&g_fake.mu);
    return ok;
}

/* The key the JWK names. */
static int pk_of_jwk(const char *jwk, mbedtls_pk_context *pk) {
    jsmn_parser p; jsmntok_t t[16]; jsmn_init(&p);
    int n = jsmn_parse(&p, jwk, strlen(jwk), t, 16);
    char x[64], y[64];
    if (n <= 0 || jtok(jwk, t, n, "x", x, sizeof x) || jtok(jwk, t, n, "y", y, sizeof y)) return -1;
    unsigned char pt[65]; pt[0] = 4;
    if (oc_base64url_decode(x, strlen(x), pt + 1, 32) != 32 || oc_base64url_decode(y, strlen(y), pt + 33, 32) != 32) return -1;
    mbedtls_pk_init(pk);
    if (mbedtls_pk_setup(pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY))) return -1;
    mbedtls_ecp_keypair *ec = mbedtls_pk_ec(*pk);
    return mbedtls_ecp_group_load(&ec->MBEDTLS_PRIVATE(grp), MBEDTLS_ECP_DP_SECP256R1) ||
           mbedtls_ecp_point_read_binary(&ec->MBEDTLS_PRIVATE(grp), &ec->MBEDTLS_PRIVATE(Q), pt, sizeof pt) ? -1 : 0;
}

/* Check a JWS body posted to `url`: its signature (by the jwk it carries, or
 * the account's), its nonce (issued, unused), its url. The payload into `pay`.
 * 0 ok, 1 bad nonce, -1 otherwise. */
static int check_jws(const char *body, const char *url, char *pay, size_t paycap, char *jwk_out, size_t jcap) {
    jsmn_parser p; jsmntok_t t[16]; jsmn_init(&p);
    int n = jsmn_parse(&p, body, strlen(body), t, 16);
    static char prot64[2048], pay64[8192], sig64[256], prot[2048];
    if (n <= 0 || jtok(body, t, n, "protected", prot64, sizeof prot64) || jtok(body, t, n, "payload", pay64, sizeof pay64) ||
        jtok(body, t, n, "signature", sig64, sizeof sig64)) return -1;
    long pl = oc_base64url_decode(prot64, strlen(prot64), (uint8_t *)prot, sizeof prot - 1);
    if (pl < 0) return -1;
    prot[pl] = '\0';
    long yl = oc_base64url_decode(pay64, strlen(pay64), (uint8_t *)pay, paycap - 1);
    if (yl < 0) return -1;
    pay[yl] = '\0';
    jsmn_parser p2; jsmntok_t t2[32]; jsmn_init(&p2);
    int n2 = jsmn_parse(&p2, prot, (size_t)pl, t2, 32);
    char nonce[64] = "", u[512] = "", alg[16] = "";
    jtok(prot, t2, n2, "nonce", nonce, sizeof nonce);
    jtok(prot, t2, n2, "url", u, sizeof u);
    jtok(prot, t2, n2, "alg", alg, sizeof alg);
    if (strcmp(u, url)) { g_fake.wrong_urls++; return -1; }
    if (strcmp(alg, "ES256")) return -1;
    mbedtls_pk_context pk; int own = 0;
    const char *jk = strstr(prot, "\"jwk\":");
    if (jk) {
        const char *s = strchr(jk, '{'), *e = s ? strchr(s, '}') : NULL;
        if (!e) return -1;
        char jwk[200]; snprintf(jwk, sizeof jwk, "%.*s", (int)(e - s + 1), s);
        if (pk_of_jwk(jwk, &pk)) return -1;
        own = 1;
        if (jwk_out) snprintf(jwk_out, jcap, "%s", jwk);
    } else if (!g_fake.have_acct) return -1;
    /* The signature: raw r || s over the signing input. */
    unsigned char sig[64];
    if (oc_base64url_decode(sig64, strlen(sig64), sig, sizeof sig) != 64) { if (own) mbedtls_pk_free(&pk); return -1; }
    char input[10240]; int il = snprintf(input, sizeof input, "%s.%s", prot64, pay64);
    unsigned char h[32]; mbedtls_sha256((const unsigned char *)input, (size_t)il, h, 0);
    mbedtls_mpi r, s; mbedtls_mpi_init(&r); mbedtls_mpi_init(&s);
    mbedtls_mpi_read_binary(&r, sig, 32); mbedtls_mpi_read_binary(&s, sig + 32, 32);
    mbedtls_ecp_keypair *ec = mbedtls_pk_ec(own ? pk : g_fake.acct);
    int vok = mbedtls_ecdsa_verify(&ec->MBEDTLS_PRIVATE(grp), h, 32, &ec->MBEDTLS_PRIVATE(Q), &r, &s) == 0;
    mbedtls_mpi_free(&r); mbedtls_mpi_free(&s);
    if (own) mbedtls_pk_free(&pk);
    if (!vok) { g_fake.bad_sigs++; return -1; }
    if (!take_nonce(nonce)) { g_fake.reused_nonces++; return 1; }
    return 0;
}

/* The TLS-ALPN-01 validation: connect as a CA does and look at what is shown. */
static int validate(const char *name) {
    unsigned char der[4096]; size_t dl = 0;
    if (acme_view(name, der, sizeof der, &dl) != 0) return 0;
    char thumb[64];
    mbedtls_pk_context pk;
    if (pk_of_jwk(g_fake.jwk, &pk) != 0) return 0;
    oc_jwk_thumbprint(&pk, thumb);
    mbedtls_pk_free(&pk);
    char keyauth[256];
    snprintf(keyauth, sizeof keyauth, "%s.%s", g_fake.token, g_fake.validate_wrong ? "not-the-thumbprint" : thumb);
    return names_raw(der, dl, name) && has_acme_id(der, dl, keyauth);
}

static void central_request(int fd, const char *path, const char *head, const char *body) {
    /* The machine signature (AUTH.md §8.7). */
    char aud[128] = "", ts[32] = "", sig64[256] = "";
    const char *h;
    if ((h = strcasestr(head, "X-OpenChime-Audience: "))) sscanf(h + 22, "%127[^\r]", aud);
    if ((h = strcasestr(head, "X-OpenChime-Timestamp: "))) sscanf(h + 23, "%31[^\r]", ts);
    if ((h = strcasestr(head, "X-OpenChime-Signature: "))) sscanf(h + 23, "%255[^\r]", sig64);
    uint8_t bh[32]; mbedtls_sha256((const unsigned char *)body, strlen(body), bh, 0);
    char hex[65]; for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", bh[i]);
    char canon[640]; int cn = snprintf(canon, sizeof canon, "openchime-machine-v1|%s|%s|%s", aud, ts, hex);
    uint8_t hh[32]; mbedtls_sha256((const unsigned char *)canon, (size_t)cn, hh, 0);
    uint8_t sig[160]; size_t sl = 0;
    mbedtls_pk_context kp; mbedtls_pk_init(&kp);
    int ok = !strcmp(aud, g_fake.central_aud) &&
             mbedtls_base64_decode(sig, sizeof sig, &sl, (const unsigned char *)sig64, strlen(sig64)) == 0 &&
             mbedtls_pk_parse_key(&kp, (const unsigned char *)g_fake.central_pub, strlen(g_fake.central_pub) + 1, NULL, 0, NULL, NULL) == 0 &&
             mbedtls_pk_verify(&kp, MBEDTLS_MD_SHA256, hh, 32, sig, sl) == 0;
    mbedtls_pk_free(&kp);
    if (!ok) { g_fake.central_bad_sigs++; reply(fd, 401, NULL, NULL, NULL); return; }
    if (!strcmp(path, "/api/machine/tls/names")) { reply(fd, 200, NULL, "application/json", "{\"names\":\"acme.workspace.test\"}"); return; }
    if (g_fake.central_refuse) { reply(fd, 403, NULL, NULL, NULL); return; }
    if (g_fake.central_pending > 0) { g_fake.central_pending--; reply(fd, 202, "Retry-After: 0\r\n", NULL, NULL); return; }
    jsmn_parser p; jsmntok_t t[8]; jsmn_init(&p);
    int n = jsmn_parse(&p, body, strlen(body), t, 8);
    char csr64[4096] = ""; unsigned char der[3072];
    jtok(body, t, n, "csr", csr64, sizeof csr64);
    long dl = oc_base64url_decode(csr64, strlen(csr64), der, sizeof der);
    mbedtls_x509_csr csr; mbedtls_x509_csr_init(&csr);
    static char chain[8192], resp[16384];
    if (dl <= 0 || mbedtls_x509_csr_parse_der(&csr, der, (size_t)dl) != 0 ||
        ca_issue(&csr.pk, "acme.workspace.test", 3600, chain, sizeof chain) != 0) {
        mbedtls_x509_csr_free(&csr); reply(fd, 400, NULL, NULL, NULL); return;
    }
    mbedtls_x509_csr_free(&csr);
    /* JSON-escape the PEM's newlines. */
    size_t o = (size_t)snprintf(resp, sizeof resp, "{\"names\":\"acme.workspace.test\",\"chain_pem\":\"");
    for (const char *c = chain; *c && o + 4 < sizeof resp; c++) { if (*c == '\n') { resp[o++] = '\\'; resp[o++] = 'n'; } else resp[o++] = *c; }
    snprintf(resp + o, sizeof resp - o, "\"}");
    reply(fd, 200, NULL, "application/json", resp);
}

static void acme_request(int fd, const char *method, const char *path, const char *body) {
    char base[64]; snprintf(base, sizeof base, "http://127.0.0.1:%d", g_fake.port);
    char url[512]; snprintf(url, sizeof url, "%s%s", base, path);
    char nonce[32]; new_nonce(nonce);
    char nh[64]; snprintf(nh, sizeof nh, "Replay-Nonce: %s\r\n", nonce);
    if (!strcmp(path, "/dir")) {
        char js[512];
        snprintf(js, sizeof js, "{\"newNonce\":\"%s/nonce\",\"newAccount\":\"%s/acct\",\"newOrder\":\"%s/order\"}", base, base, base);
        reply(fd, 200, NULL, "application/json", js); return;
    }
    if (!strcmp(path, "/nonce")) { reply(fd, 200, nh, NULL, NULL); return; }
    if (strcmp(method, "POST")) { reply(fd, 405, NULL, NULL, NULL); return; }
    static char pay[8192]; char jwk[200] = "";
    int v = check_jws(body, url, pay, sizeof pay, jwk, sizeof jwk);
    if (v == 1 || (!strcmp(path, "/acct") && g_fake.bad_nonce_once && g_fake.bad_nonce_once--)) {
        reply(fd, 400, nh, "application/problem+json", "{\"type\":\"urn:ietf:params:acme:error:badNonce\",\"detail\":\"stale\"}"); return;
    }
    if (v != 0) { reply(fd, 403, nh, "application/problem+json", "{\"type\":\"urn:ietf:params:acme:error:unauthorized\",\"detail\":\"bad JWS\"}"); return; }
    char loc[600], js[2048];
    if (!strcmp(path, "/acct")) {
        if (!g_fake.have_acct) {
            snprintf(g_fake.jwk, sizeof g_fake.jwk, "%s", jwk);
            pk_of_jwk(g_fake.jwk, &g_fake.acct);
            g_fake.have_acct = 1; g_fake.accounts_made++;
        }
        snprintf(loc, sizeof loc, "%sLocation: %s/acct/1\r\n", nh, base);
        reply(fd, 201, loc, "application/json", "{\"status\":\"valid\"}"); return;
    }
    if (!strcmp(path, "/order")) {
        /* The names asked for, in order. */
        char *s = pay; g_fake.order_names[0] = '\0';
        while ((s = strstr(s, "\"value\":\""))) {
            s += 9; char *e = strchr(s, '"'); if (!e) break;
            if (g_fake.order_names[0]) strcat(g_fake.order_names, ",");
            strncat(g_fake.order_names, s, (size_t)(e - s)); s = e;
        }
        snprintf(g_fake.token, sizeof g_fake.token, "tok%08x", (unsigned)rand());
        g_fake.authz_state = 0; g_fake.order_valid = 0;
        snprintf(loc, sizeof loc, "%sLocation: %s/order/1\r\n", nh, base);
        snprintf(js, sizeof js, "{\"status\":\"pending\",\"authorizations\":[\"%s/authz/0\"],\"finalize\":\"%s/finalize\"}", base, base);
        reply(fd, 201, loc, "application/json", js); return;
    }
    if (!strcmp(path, "/authz/0")) {
        char first[256]; snprintf(first, sizeof first, "%s", g_fake.order_names);
        char *c = strchr(first, ','); if (c) *c = '\0';
        const char *st = g_fake.authz_state == 1 ? "valid" : g_fake.authz_state == 2 ? "invalid" : "pending";
        snprintf(js, sizeof js, "{\"status\":\"%s\",\"identifier\":{\"type\":\"dns\",\"value\":\"%s\"},"
                                "\"challenges\":[{\"type\":\"http-01\",\"url\":\"%s/chal/http\",\"token\":\"x\"},"
                                "{\"type\":\"tls-alpn-01\",\"url\":\"%s/chal/0\",\"token\":\"%s\"%s}]}",
                 st, first, base, base, g_fake.token,
                 g_fake.authz_state == 2 ? ",\"error\":{\"detail\":\"the extension did not match\"}" : "");
        reply(fd, 200, nh, "application/json", js); return;
    }
    if (!strcmp(path, "/chal/0")) {
        char first[256]; snprintf(first, sizeof first, "%s", g_fake.order_names);
        char *c = strchr(first, ','); if (c) *c = '\0';
        g_fake.validations++;
        g_fake.authz_state = validate(first) ? 1 : 2;
        reply(fd, 200, nh, "application/json", "{\"status\":\"processing\"}"); return;
    }
    if (!strcmp(path, "/finalize")) {
        char csr64[4096] = "";
        jsmn_parser p; jsmntok_t t[8]; jsmn_init(&p);
        int n = jsmn_parse(&p, pay, strlen(pay), t, 8);
        jtok(pay, t, n, "csr", csr64, sizeof csr64);
        unsigned char der[3072];
        long dl = oc_base64url_decode(csr64, strlen(csr64), der, sizeof der);
        mbedtls_x509_csr csr; mbedtls_x509_csr_init(&csr);
        if (g_fake.authz_state != 1 || dl <= 0 || mbedtls_x509_csr_parse_der(&csr, der, (size_t)dl) != 0 ||
            ca_issue(&csr.pk, g_fake.order_names, g_fake.life_s, g_fake.chain, sizeof g_fake.chain) != 0) {
            mbedtls_x509_csr_free(&csr);
            reply(fd, 403, nh, "application/problem+json", "{\"type\":\"urn:ietf:params:acme:error:badCSR\"}"); return;
        }
        mbedtls_x509_csr_free(&csr);
        g_fake.order_valid = 1;
        snprintf(js, sizeof js, "{\"status\":\"processing\"}");
        reply(fd, 200, nh, "application/json", js); return;
    }
    if (!strcmp(path, "/order/1")) {
        snprintf(js, sizeof js, "{\"status\":\"%s\",\"certificate\":\"%s/cert/1\"}", g_fake.order_valid ? "valid" : "pending", base);
        reply(fd, 200, nh, "application/json", js); return;
    }
    if (!strcmp(path, "/cert/1")) { reply(fd, 200, nh, "application/pem-certificate-chain", g_fake.chain); return; }
    reply(fd, 404, nh, NULL, NULL);
}

static void *fake_thread(void *p) {
    (void)p;
    while (!__atomic_load_n(&g_fake.stop, __ATOMIC_ACQUIRE)) {
        int fd = accept(g_fake.fd, NULL, NULL);
        if (fd < 0) continue;
        struct timeval tv = { 5, 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        static char buf[65536]; size_t len = 0; char *end = NULL;
        while (len < sizeof buf - 1) {
            ssize_t n = recv(fd, buf + len, sizeof buf - 1 - len, 0);
            if (n <= 0) break;
            len += (size_t)n; buf[len] = '\0';
            if ((end = strstr(buf, "\r\n\r\n"))) {
                const char *cl = strcasestr(buf, "Content-Length:");
                size_t want = (size_t)(end + 4 - buf) + (cl && cl < end ? (size_t)atoi(cl + 15) : 0);
                if (len >= want) break;
            }
        }
        if (end) {
            char method[8] = "", path[256] = "";
            sscanf(buf, "%7s %255s", method, path);
            *end = '\0';
            if (!strncmp(path, "/api/machine/", 13)) central_request(fd, path, buf, end + 4);
            else acme_request(fd, method, path, end + 4);
        }
        close(fd);
    }
    return NULL;
}

/* --- the tests ------------------------------------------------------------ */

static const char NAME[] = "chat.acme.test";

static void test_units(void) {
    /* The challenge certificate: the name, and the extension over this key
     * authorization -- not over another. */
    char *cp = NULL, *kp = NULL;
    CHECK(oc_acme_challenge_cert(NAME, "tok.thumb", &cp, &kp) == 0 && cp && kp);
    unsigned char der0[4096]; size_t dl0 = 0;
    CHECK(cp && der_of(cp, der0, sizeof der0, &dl0) == 0);
    CHECK(names_raw(der0, dl0, NAME) && has_acme_id(der0, dl0, "tok.thumb"));
    CHECK(!has_acme_id(der0, dl0, "tok.other"));
    free(cp); free(kp);
    /* The CSR names what it was asked to. */
    uint8_t *der = NULL; size_t dl = 0; char *key = NULL;
    CHECK(oc_acme_csr("a.acme.test,b.acme.test", &der, &dl, &key) == 0 && der && key);
    mbedtls_x509_csr csr; mbedtls_x509_csr_init(&csr);
    CHECK(der && mbedtls_x509_csr_parse_der(&csr, der, dl) == 0);
    int a = 0, b = 0;
    for (const mbedtls_x509_sequence *s = &csr.subject_alt_names; s; s = s->next) {
        if (s->buf.len == 11 && !memcmp(s->buf.p, "a.acme.test", 11)) a = 1;
        if (s->buf.len == 11 && !memcmp(s->buf.p, "b.acme.test", 11)) b = 1;
    }
    CHECK(a && b);
    mbedtls_x509_csr_free(&csr); free(der); free(key);
    /* An EC key's canonical JWK (RFC 7638 §3.2): members in order, nothing else. */
    mbedtls_pk_context pk;
    CHECK(pk_of_jwk("{\"kty\":\"EC\",\"crv\":\"P-256\",\"x\":\"f83OJ3D2xF1Bg8vub9tLe1gHMzV76e8Tus9uPHvRVEU\","
                    "\"y\":\"x_FEzRu9m36HLN_tue659LNpXW6pCyStikYjKIWI5a0\"}", &pk) == 0);
    char jwk[200];
    CHECK(oc_jwk_p256(&pk, jwk, sizeof jwk) > 0 &&
          !strcmp(jwk, "{\"crv\":\"P-256\",\"kty\":\"EC\",\"x\":\"f83OJ3D2xF1Bg8vub9tLe1gHMzV76e8Tus9uPHvRVEU\","
                       "\"y\":\"x_FEzRu9m36HLN_tue659LNpXW6pCyStikYjKIWI5a0\"}"));
    /* ...whose thumbprint is SHA-256 of exactly that canonical form (RFC 7638). */
    char th[64], want[64]; uint8_t h[32];
    mbedtls_sha256((const unsigned char *)jwk, strlen(jwk), h, 0);
    oc_base64url_encode(h, 32, want);
    CHECK(oc_jwk_thumbprint(&pk, th) == 0 && !strcmp(th, want));
    mbedtls_pk_free(&pk);
    /* Renewal comes two-thirds of the way through a certificate's life. */
    CHECK(oc_certs_renew_at(1000, 4000) == 3000);
    CHECK(oc_certs_renew_at(0, 90ull * 86400000ull) == 60ull * 86400000ull);
}

/* No challenge pending: an ACME handshake is refused, and everyone else is
 * shown the ordinary certificate, never a challenge one. */
static void test_listener_choice(void) {
    CHECK(acme_refused(NAME));
    char *cp = NULL, *kp = NULL;
    CHECK(oc_acme_challenge_cert(NAME, "t.k", &cp, &kp) == 0);
    CHECK(oc_tls_server_set_challenge(&g_srv, NAME, cp, strlen(cp), kp, strlen(kp)) == 0);
    unsigned char der[4096]; size_t dl = 0;
    CHECK(acme_view(NAME, der, sizeof der, &dl) == 0 && has_acme_id(der, dl, "t.k"));
    CHECK(acme_view("other.acme.test", der, sizeof der, &dl) != 0);          /* only the name asked */
    dl = 0;
    CHECK(probe(OC_ALPN_PROTO, NAME, 0, der, &dl) == 0 && dl && !has_acme_id(der, dl, "t.k"));
    CHECK(oc_tls_server_set_challenge(&g_srv, NAME, NULL, 0, NULL, 0) == 0);
    CHECK(acme_refused(NAME));                /* removed */
    free(cp); free(kp);
}

static char g_kept_key[2048], g_kept_url[512];
static int g_kept_accounts;
static void keep_account(void *ctx, const char *key_pem, const char *url) {
    (void)ctx; g_kept_accounts++;
    snprintf(g_kept_key, sizeof g_kept_key, "%s", key_pem); snprintf(g_kept_url, sizeof g_kept_url, "%s", url);
}

static void test_issue(void) {
    char dir[128]; snprintf(dir, sizeof dir, "http://127.0.0.1:%d/dir", g_fake.port);
    oc_acme_opts o; memset(&o, 0, sizeof o);
    o.directory = dir; o.names = NAME; o.tls = &g_srv; o.store_account = keep_account; o.poll_ms = 20;
    g_fake.life_s = 3600; g_fake.bad_nonce_once = 1;
    oc_cert_issued got; char err[512] = "";
    int rc = oc_acme_issue(&o, &got, err, sizeof err);
    if (rc) printf("  acme: %s\n", err);
    CHECK(rc == 0);
    CHECK(g_kept_accounts == 1 && g_kept_url[0]);                  /* kept, once, through a badNonce */
    CHECK(g_fake.bad_sigs == 0 && g_fake.wrong_urls == 0 && g_fake.reused_nonces == 0);
    if (rc != 0) return;
    CHECK(got.not_after_ms > got.not_before_ms);
    /* Presented: a client that trusts only the test root verifies it by name. */
    CHECK(probe(NULL, NAME, 1, NULL, NULL) != 0);                  /* the self-signed one, before */
    CHECK(oc_tls_server_use(&g_srv, got.chain_pem, strlen(got.chain_pem), got.key_pem, strlen(got.key_pem)) == 0);
    CHECK(probe(NULL, NAME, 1, NULL, NULL) == 0);
    CHECK(probe(NULL, "wrong.acme.test", 1, NULL, NULL) != 0);     /* ...for its name only */
    CHECK(acme_refused(NAME));      /* and the challenge is gone */
    oc_cert_issued_free(&got);

    /* Again, with the kept account: no new one is made. */
    o.account_key_pem = g_kept_key; o.account_url = g_kept_url;
    int made = g_fake.accounts_made;
    CHECK(oc_acme_issue(&o, &got, err, sizeof err) == 0);
    CHECK(g_fake.accounts_made == made && g_kept_accounts == 1);
    oc_cert_issued_free(&got);

    /* A failed validation: an error saying so, nothing issued. */
    g_fake.validate_wrong = 1;
    err[0] = '\0';
    CHECK(oc_acme_issue(&o, &got, err, sizeof err) != 0);
    CHECK(strstr(err, "validation failed") != NULL);
    CHECK(acme_refused(NAME));      /* and the challenge was taken down */
    g_fake.validate_wrong = 0;
}

/* The worker: obtains a certificate, presents it, renews it when due, and a
 * connection made before a renewal keeps working through it. */
static int g_cert_stores;
static char g_stored_chain[8192];
static void keep_cert(void *ctx, const oc_cert_issued *c) {
    (void)ctx; g_cert_stores++;
    snprintf(g_stored_chain, sizeof g_stored_chain, "%s", c->chain_pem);
}

static void test_worker(void) {
    char dir[128]; snprintf(dir, sizeof dir, "http://127.0.0.1:%d/dir", g_fake.port);
    g_fake.life_s = 3;                                             /* renewed every ~2 s */
    g_ca_backdate_s = 0;
    oc_certs_opts o; memset(&o, 0, sizeof o);
    o.source = OC_CERTS_ACME; o.tls = &g_srv; o.directory = dir; o.names = NAME;
    o.account_key_pem = g_kept_key; o.account_url = g_kept_url;
    o.store_cert = keep_cert; o.retry_ms = 200; o.poll_ms = 20; o.check_ms = 100;
    g_cert_stores = 0;
    oc_certs *w = oc_certs_start(&o);
    CHECK(w != NULL);
    if (!w) return;
    int got = 0;
    for (int i = 0; i < 100 && !got; i++) { oc_certs_status(w, &got, NULL, NULL, 0); if (!got) msleep(50); }
    CHECK(got >= 1 && g_cert_stores >= 1);
    /* A connection up before a renewal, and the certificate it was given. The
     * worker swaps a certificate in and only then counts it, so the count read
     * after the handshake may lag a swap already made: two more are waited for,
     * the second of which was begun only after the count was read -- and so
     * swapped in after this connection was made. */
    int fd = connect_loopback(g_srv_port);
    oc_tls_client cli; oc_tls_conn c;
    CHECK(fd >= 0 && oc_tls_client_init_ca(&cli) == 0);
    CHECK(oc_tls_conn_init(&c, &cli.conf, fd) == 0 && oc_tls_conn_set_hostname(&c, NAME) == 0 && handshake(&c) == 0);
    unsigned char before[32];
    CHECK(oc_tls_peer_fingerprint(&c, before) == 0);
    int base = 0;
    oc_certs_status(w, &base, NULL, NULL, 0);
    got = base;
    for (int i = 0; i < 600 && got < base + 2; i++) { oc_certs_status(w, &got, NULL, NULL, 0); if (got < base + 2) msleep(50); }
    CHECK(got >= base + 2);                                        /* renewed since the connection */
    unsigned char after[32];
    CHECK(oc_tls_server_fingerprint(&g_srv, after) == 0 && memcmp(before, after, 32) != 0);
    size_t n = 0; char echo[8] = "";
    CHECK(oc_tls_write(&c, "ping", 4, &n) == OC_TLS_OK && n == 4);
    for (int i = 0; i < 100; i++) {
        oc_tls_status st = oc_tls_read(&c, echo, sizeof echo, &n);
        if (st == OC_TLS_WANT_READ || st == OC_TLS_WANT_WRITE) continue;
        break;
    }
    CHECK(n == 4 && !memcmp(echo, "ping", 4));                     /* the old connection still works */
    CHECK(probe(NULL, NAME, 1, NULL, NULL) == 0);                  /* and a new one gets the new certificate */
    oc_tls_conn_free(&c); oc_tls_client_free(&cli); close(fd);
    oc_certs_stop(w);
    g_ca_backdate_s = 60;
}

static void test_central(void) {
    char pk[1024], aud[128];
    CHECK(oc_enroll_generate(pk, sizeof pk, aud, sizeof aud) == 0);
    snprintf(g_fake.central_aud, sizeof g_fake.central_aud, "%s", aud);
    snprintf(g_fake.central_pub, sizeof g_fake.central_pub, "%s", pk);
    char url[128]; snprintf(url, sizeof url, "http://127.0.0.1:%d/api/machine/enroll", g_fake.port);
    oc_cert_issued got; int retry = 0, final = 0; char err[256] = "";
    /* Still working on it: asked to come back. */
    g_fake.central_pending = 1;
    CHECK(oc_central_issue(url, aud, pk, &got, &retry, &final, err, sizeof err) == 1 && retry > 0);
    /* Then the certificate, for the names central says. */
    CHECK(oc_central_issue(url, aud, pk, &got, &retry, &final, err, sizeof err) == 0);
    CHECK(got.names && !strcmp(got.names, "acme.workspace.test") && got.chain_pem);
    CHECK(g_fake.central_bad_sigs == 0);
    oc_cert_issued_free(&got);
    /* Refused for good: not retried. */
    g_fake.central_refuse = 1;
    CHECK(oc_central_issue(url, aud, pk, &got, &retry, &final, err, sizeof err) == -1 && final);
    g_fake.central_refuse = 0;
    /* A request signed with another key is refused by central. */
    char pk2[1024], aud2[128];
    CHECK(oc_enroll_generate(pk2, sizeof pk2, aud2, sizeof aud2) == 0);
    int bad = g_fake.central_bad_sigs;
    CHECK(oc_central_issue(url, aud, pk2, &got, &retry, &final, err, sizeof err) == -1);
    CHECK(g_fake.central_bad_sigs > bad);
}

int run_acme_tests(void) {
    printf("test_acme: ACME TLS-ALPN-01 against a fake CA, the listener's certificate choice, renewal and swap, certificates through central\n");
    srand((unsigned)time(NULL));
    CHECK(ca_init() == 0);
    CHECK(oc_tls_set_extra_ca(g_ca.path) == 0);
    unlink("build/test_acme_cert.pem"); unlink("build/test_acme_key.pem");
    CHECK(oc_tls_server_init(&g_srv, "build/test_acme_cert.pem", "build/test_acme_key.pem") == 0);
    g_srv_fd = listen_loopback(&g_srv_port);
    pthread_mutex_init(&g_fake.mu, NULL);
    g_fake.fd = listen_loopback(&g_fake.port);
    CHECK(g_srv_fd >= 0 && g_fake.fd >= 0);
    pthread_t st, ft;
    pthread_create(&st, NULL, srv_thread, NULL);
    pthread_create(&ft, NULL, fake_thread, NULL);

    test_units();
    test_listener_choice();
    test_issue();
    test_worker();
    test_central();

    __atomic_store_n(&g_srv_stop, 1, __ATOMIC_RELEASE); __atomic_store_n(&g_fake.stop, 1, __ATOMIC_RELEASE);
    close(connect_loopback(g_srv_port)); close(connect_loopback(g_fake.port));
    pthread_join(st, NULL); pthread_join(ft, NULL);
    close(g_srv_fd); close(g_fake.fd);
    msleep(100);                                                  /* the detached echo threads */
    oc_tls_server_free(&g_srv);
    oc_tls_set_extra_ca(NULL);
    if (g_fake.have_acct) mbedtls_pk_free(&g_fake.acct);
    mbedtls_pk_free(&g_ca.key); mbedtls_ctr_drbg_free(&g_ca.rng); mbedtls_entropy_free(&g_ca.ent);
    unlink(g_ca.path); unlink("build/test_acme_cert.pem"); unlink("build/test_acme_key.pem");
    return failures;
}
