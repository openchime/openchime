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

#include <mbedtls/asn1.h>
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
    static unsigned n_issued; n_issued++;          /* two bytes, 0x40.. first: positive, minimal, distinct across repeats */
    unsigned char serial[2] = { (unsigned char)(0x40 | ((n_issued >> 8) & 0x3f)), (unsigned char)n_issued };
    time_t a = time(NULL) - g_ca_backdate_s, b = time(NULL) + life_s;
    struct tm ta, tb; gmtime_r(&a, &ta); gmtime_r(&b, &tb);
    char nb[16], na[16]; strftime(nb, sizeof nb, "%Y%m%d%H%M%S", &ta); strftime(na, sizeof na, "%Y%m%d%H%M%S", &tb);
    mbedtls_x509write_crt_set_subject_key(&w, subject_key); mbedtls_x509write_crt_set_issuer_key(&w, &g_ca.key);
    mbedtls_x509write_crt_set_version(&w, MBEDTLS_X509_CRT_VERSION_3); mbedtls_x509write_crt_set_md_alg(&w, MBEDTLS_MD_SHA256);
    unsigned char leaf[4096];
    int rc = mbedtls_x509write_crt_set_subject_name(&w, subj) ||
             mbedtls_x509write_crt_set_issuer_name(&w, "CN=ACME Test Root") ||
             mbedtls_x509write_crt_set_serial_raw(&w, serial, sizeof serial) ||
             mbedtls_x509write_crt_set_validity(&w, nb, na) ||
             mbedtls_x509write_crt_set_subject_alternative_name(&w, &nodes[0]) ||
             mbedtls_x509write_crt_set_authority_key_identifier(&w) ||   /* for ARI's identifier */
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
    /* ARI (RFC 9773): offered at all; a window already open, for the first ask
     * only (a replacement is not due at once); else a window of these absolute
     * times (ms); its Retry-After; and what was asked, and replaced. */
    int      ari, ari_open_once, ari_retry_s;
    uint64_t ari_start_ms, ari_end_ms;
    int      ari_asked;
    char     ari_last_id[200], order_replaces[200];
    /* Orders: fail every one; and when each came. */
    int      order_fail, n_orders;
    uint64_t order_ms[32];
} g_fake;

static uint64_t wall_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void rfc3339(uint64_t ms, char *out, size_t cap) {
    time_t t = (time_t)(ms / 1000); struct tm tm; gmtime_r(&t, &tm);
    char b[32]; strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%S", &tm);
    snprintf(out, cap, "%s.%03uZ", b, (unsigned)(ms % 1000));
}

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
        char js[640], ri[160] = "";
        if (g_fake.ari) snprintf(ri, sizeof ri, ",\"renewalInfo\":\"%s/renewal-info/\"", base);
        snprintf(js, sizeof js, "{\"newNonce\":\"%s/nonce\",\"newAccount\":\"%s/acct\",\"newOrder\":\"%s/order\"%s}", base, base, base, ri);
        reply(fd, 200, NULL, "application/json", js); return;
    }
    if (!strncmp(path, "/renewal-info/", 14) && g_fake.ari) {
        pthread_mutex_lock(&g_fake.mu);
        snprintf(g_fake.ari_last_id, sizeof g_fake.ari_last_id, "%s", path + 14);
        uint64_t now = wall_ms(), st = g_fake.ari_start_ms, en = g_fake.ari_end_ms;
        if (g_fake.ari_open_once && g_fake.ari_asked == 0) { st = now - 60000; en = now + 60000; }
        g_fake.ari_asked++;
        int ra = g_fake.ari_retry_s;
        pthread_mutex_unlock(&g_fake.mu);
        char js[256], a[40], b[40], ex[64] = "";
        rfc3339(st, a, sizeof a); rfc3339(en, b, sizeof b);
        snprintf(js, sizeof js, "{\"suggestedWindow\":{\"start\":\"%s\",\"end\":\"%s\"}}", a, b);
        if (ra) snprintf(ex, sizeof ex, "Retry-After: %d\r\n", ra);
        reply(fd, 200, ex, "application/json", js); return;
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
        pthread_mutex_lock(&g_fake.mu);
        if (g_fake.n_orders < 32) g_fake.order_ms[g_fake.n_orders] = wall_ms();
        g_fake.n_orders++;
        /* What it replaces (RFC 9773 §5), if anything. */
        g_fake.order_replaces[0] = '\0';
        const char *rp = strstr(pay, "\"replaces\":\"");
        if (rp) { rp += 12; snprintf(g_fake.order_replaces, sizeof g_fake.order_replaces, "%.*s", (int)strcspn(rp, "\""), rp); }
        int failing = g_fake.order_fail;
        pthread_mutex_unlock(&g_fake.mu);
        if (failing) {
            reply(fd, 500, nh, "application/problem+json", "{\"type\":\"urn:ietf:params:acme:error:serverInternal\",\"detail\":\"down\"}");
            return;
        }
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
    /* Its serial is a DER INTEGER a CA's parser accepts: positive and minimal --
     * no leading zero byte, which OpenSSL and Go refuse. The random serial's
     * top byte is built to be 0x40-0x7f, which guarantees both; checked on
     * enough certificates that a byte left random would show. */
    int serials_ok = 1;
    for (int i = 0; i < 32; i++) {
        cp = kp = NULL;
        if (oc_acme_challenge_cert(NAME, "tok.thumb", &cp, &kp) != 0 || der_of(cp, der0, sizeof der0, &dl0) != 0) serials_ok = 0;
        unsigned char *q = der0, *e = der0 + dl0; size_t l = 0;
        if (serials_ok &&
            (mbedtls_asn1_get_tag(&q, e, &l, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE) ||   /* certificate */
             mbedtls_asn1_get_tag(&q, e, &l, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE) ||   /* tbsCertificate */
             mbedtls_asn1_get_tag(&q, e, &l, MBEDTLS_ASN1_CONTEXT_SPECIFIC | MBEDTLS_ASN1_CONSTRUCTED) ||
             (q += l, 0) ||                                                                          /* version */
             mbedtls_asn1_get_tag(&q, e, &l, MBEDTLS_ASN1_INTEGER) || l != 16 ||                     /* serial */
             (q[0] & 0x80) || (q[0] == 0 && !(q[1] & 0x80)) || (q[0] & 0xc0) != 0x40)) serials_ok = 0;
        free(cp); free(kp);
    }
    CHECK(serials_ok);
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
    /* Renewal comes at a random moment from 60% to two-thirds of the way through
     * a certificate's life, chosen per certificate; a CA's window likewise. */
    CHECK(oc_certs_renew_pick(0, 90ull * 86400000ull, 0.0) == 54ull * 86400000ull);
    uint64_t hi = oc_certs_renew_pick(0, 90ull * 86400000ull, 0.999999);
    CHECK(hi < 60ull * 86400000ull && hi > 59ull * 86400000ull);
    CHECK(oc_certs_window_pick(1000, 2000, 0.0) == 1000 && oc_certs_window_pick(1000, 2000, 0.5) == 1500);
    CHECK(oc_certs_window_pick(1000, 1000, 0.7) == 1000);
    {
        int differ = 0;
        uint64_t first = oc_certs_renew_pick(0, 7776000000ull, (double)rand() / ((double)RAND_MAX + 1));
        for (int i = 0; i < 100; i++)
            if (oc_certs_renew_pick(0, 7776000000ull, (double)rand() / ((double)RAND_MAX + 1)) != first) differ = 1;
        CHECK(differ);
    }
    /* Retries: a minute, ten, a hundred, then a day (Let's Encrypt's guide). */
    /* A moved box re-issues when its kept certificate does not name the new address. */
    CHECK(oc_certs_names_include("acme-new.workspace.openchime.io,acme.workspace.openchime.io", "ACME.workspace.openchime.io") == 1);
    CHECK(oc_certs_names_include("acme.workspace.openchime.io", "acme-new.workspace.openchime.io") == 0);
    CHECK(oc_certs_names_include("acme.workspace.openchime.io", "acme.workspace.openchime") == 0);
    CHECK(oc_certs_names_include(NULL, "acme.workspace.openchime.io") == 0);
    CHECK(oc_certs_names_include("acme.workspace.openchime.io", "") == 0);
    CHECK(oc_certs_retry_ms(0, 60000, 86400000) == 60000 && oc_certs_retry_ms(1, 60000, 86400000) == 600000 &&
          oc_certs_retry_ms(2, 60000, 86400000) == 6000000 && oc_certs_retry_ms(3, 60000, 86400000) == 86400000 &&
          oc_certs_retry_ms(9, 60000, 86400000) == 86400000);
    /* ARI's certificate identifier: RFC 9773 §4.1's own example. */
    {
        static const uint8_t aki[] = { 0x69, 0x88, 0x5B, 0x6B, 0x87, 0x46, 0x40, 0x41, 0xE1, 0xB3,
                                       0x7B, 0x84, 0x7B, 0xA0, 0xAE, 0x2C, 0xDE, 0x01, 0xC8, 0xD4 };
        static const uint8_t ser[] = { 0x00, 0x87, 0x65, 0x43, 0x21 };
        char id[OC_ACME_CERT_ID_MAX];
        CHECK(oc_acme_cert_id_raw(aki, sizeof aki, ser, sizeof ser, id, sizeof id) == 0 &&
              !strcmp(id, "aYhba4dGQEHhs3uEe6CuLN4ByNQ.AIdlQyE"));
    }
    /* RFC 3339, as ARI writes its windows. */
    {
        uint64_t t = 0;
        CHECK(oc_rfc3339_ms("2025-01-02T04:00:00Z", &t) == 0 && t == 1735790400000ull);
        CHECK(oc_rfc3339_ms("2025-01-02T04:00:00.250Z", &t) == 0 && t == 1735790400250ull);
        CHECK(oc_rfc3339_ms("2025-01-02T06:00:00+02:00", &t) == 0 && t == 1735790400000ull);
        CHECK(oc_rfc3339_ms("2025-01-02 04:00:00", &t) == -1 && oc_rfc3339_ms("2025-13-02T04:00:00Z", &t) == -1);
    }
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

/* What the worker raised and cleared (REQ-263), in order: the key, and the
 * message or "" for a clear. */
static struct { char key[32]; char msg[OC_MAX_ALERT_MESSAGE + 1]; int raise; } g_alerts[256];
static int g_n_alerts;
static pthread_mutex_t g_alerts_mu = PTHREAD_MUTEX_INITIALIZER;
static void record_alert(void *ctx, const char *key, const char *message) {
    (void)ctx;
    pthread_mutex_lock(&g_alerts_mu);
    if (g_n_alerts < 256) {
        snprintf(g_alerts[g_n_alerts].key, sizeof g_alerts[0].key, "%s", key);
        snprintf(g_alerts[g_n_alerts].msg, sizeof g_alerts[0].msg, "%s", message ? message : "");
        g_alerts[g_n_alerts].raise = message != NULL;
        g_n_alerts++;
    }
    pthread_mutex_unlock(&g_alerts_mu);
}
static void alerts_reset(void) { pthread_mutex_lock(&g_alerts_mu); g_n_alerts = 0; pthread_mutex_unlock(&g_alerts_mu); }
/* The index of the first raise (1) or clear (0) of `key` at or after `from` whose
 * message holds `has` (a raise), or -1. */
static int alert_at(const char *key, int raise, const char *has, int from) {
    int at = -1;
    pthread_mutex_lock(&g_alerts_mu);
    for (int i = from < 0 ? 0 : from; i < g_n_alerts && at < 0; i++)
        if (!strcmp(g_alerts[i].key, key) && g_alerts[i].raise == raise && (!has || strstr(g_alerts[i].msg, has)))
            at = i;
    pthread_mutex_unlock(&g_alerts_mu);
    return at;
}

/* The first chain kept, for what ARI and a replacement name. */
static char g_first_chain[8192];
static int g_first_kept;
static void keep_first(void *ctx, const oc_cert_issued *c) {
    (void)ctx;
    if (!__atomic_load_n(&g_first_kept, __ATOMIC_ACQUIRE)) {
        snprintf(g_first_chain, sizeof g_first_chain, "%s", c->chain_pem);
        __atomic_store_n(&g_first_kept, 1, __ATOMIC_RELEASE);
    }
}

static oc_certs *worker_on(const char *dir, int life_s) {
    oc_certs_opts o; memset(&o, 0, sizeof o);
    o.source = OC_CERTS_ACME; o.tls = &g_srv; o.directory = dir; o.names = NAME;
    o.account_key_pem = g_kept_key; o.account_url = g_kept_url;
    o.store_cert = keep_first; o.retry_ms = 200; o.poll_ms = 20; o.check_ms = 100; o.ari_check_ms = 100;
    o.alert = record_alert;
    alerts_reset();
    g_fake.life_s = life_s;
    __atomic_store_n(&g_first_kept, 0, __ATOMIC_RELEASE);
    return oc_certs_start(&o);
}

static int obtained(oc_certs *w) { int n = 0; oc_certs_status(w, &n, NULL, NULL, 0); return n; }

/* ARI (RFC 9773): the CA's window rules. One already open means now -- a
 * certificate with an hour to run is replaced at once, the replacement naming
 * it -- and one set later holds a renewal the daemon's own window would have
 * made. */
static void test_worker_ari(void) {
    char dir[128]; snprintf(dir, sizeof dir, "http://127.0.0.1:%d/dir", g_fake.port);
    g_ca_backdate_s = 0;
    /* Open now: renewed at once, though its own moment is forty minutes off. */
    g_fake.ari = 1; g_fake.ari_open_once = 1; g_fake.ari_asked = 0; g_fake.ari_retry_s = 0;
    g_fake.ari_start_ms = wall_ms() + 3600000; g_fake.ari_end_ms = wall_ms() + 7200000;
    oc_certs *w = worker_on(dir, 3600);
    CHECK(w != NULL);
    if (!w) return;
    int i;
    for (i = 0; i < 200 && obtained(w) < 2; i++) msleep(50);
    CHECK(obtained(w) >= 2);
    char id[OC_ACME_CERT_ID_MAX];
    CHECK(__atomic_load_n(&g_first_kept, __ATOMIC_ACQUIRE) && oc_acme_cert_id(g_first_chain, id, sizeof id) == 0);
    pthread_mutex_lock(&g_fake.mu);                               /* the fake is still answering ARI checks */
    int asked_first = g_fake.ari_asked >= 1;
    char replaced[200]; snprintf(replaced, sizeof replaced, "%s", g_fake.order_replaces);
    pthread_mutex_unlock(&g_fake.mu);
    CHECK(asked_first && strcmp(replaced, id) == 0);              /* the replacement names the replaced */
    {   /* ...and the identifier is the certificate's own: its authority key and serial. */
        mbedtls_x509_crt crt; mbedtls_x509_crt_init(&crt);
        CHECK(mbedtls_x509_crt_parse(&crt, (const unsigned char *)g_first_chain, strlen(g_first_chain) + 1) >= 0);
        char want[OC_ACME_CERT_ID_MAX];
        CHECK(crt.authority_key_id.keyIdentifier.len == 20 &&
              oc_acme_cert_id_raw(crt.authority_key_id.keyIdentifier.p, 20, crt.serial.p, crt.serial.len, want,
                                  sizeof want) == 0 && strcmp(want, id) == 0);
        mbedtls_x509_crt_free(&crt);
    }
    msleep(600);
    CHECK(obtained(w) == 2);                                      /* and the replacement is not due */
    oc_certs_stop(w);
    /* An owner or admin was told the CA asked (REQ-263), and told again when
     * the replacement answered it. */
    int asked = alert_at(OC_CERTS_ALERT_REPLACE, 1, "replaced now", 0);
    CHECK(asked >= 0 && alert_at(OC_CERTS_ALERT_REPLACE, 0, NULL, asked) > asked);

    /* Later: a two-second life would renew within 1.3 s; the CA's window,
     * three seconds on, holds it. */
    g_fake.ari_open_once = 0; g_fake.ari_asked = 0;
    g_fake.ari_start_ms = wall_ms() + 3000; g_fake.ari_end_ms = wall_ms() + 3400;
    w = worker_on(dir, 2);
    CHECK(w != NULL);
    if (!w) return;
    for (i = 0; i < 100 && obtained(w) < 1; i++) msleep(50);
    CHECK(obtained(w) == 1);
    uint64_t first_at = wall_ms();
    while (wall_ms() < g_fake.ari_start_ms - 300) { CHECK(obtained(w) == 1); msleep(100); }
    for (i = 0; i < 100 && obtained(w) < 2; i++) msleep(50);
    CHECK(obtained(w) >= 2 && wall_ms() - first_at >= 1500);
    oc_certs_stop(w);
    CHECK(g_fake.ari_asked >= 1);
    /* A certificate within a week of expiry, not renewed, is raised -- once for
     * its day, not at every look -- and its renewal clears it. */
    int near = alert_at(OC_CERTS_ALERT_EXPIRING, 1, "has not been renewed", 0);
    int cleared = alert_at(OC_CERTS_ALERT_EXPIRING, 0, NULL, near);
    CHECK(near >= 0 && cleared > near);
    int again = alert_at(OC_CERTS_ALERT_EXPIRING, 1, NULL, near + 1);
    CHECK(again < 0 || again > cleared);
    g_fake.ari = 0;
    g_ca_backdate_s = 60;
}

/* A CA that fails every order is asked again after the base wait, ten times
 * it, a hundred times it, then at the ceiling -- not a doubling. */
static void test_worker_retries(void) {
    char dir[128]; snprintf(dir, sizeof dir, "http://127.0.0.1:%d/dir", g_fake.port);
    pthread_mutex_lock(&g_fake.mu);
    g_fake.order_fail = 1; g_fake.n_orders = 0;
    pthread_mutex_unlock(&g_fake.mu);
    oc_certs_opts o; memset(&o, 0, sizeof o);
    o.source = OC_CERTS_ACME; o.tls = &g_srv; o.directory = dir; o.names = NAME;
    o.account_key_pem = g_kept_key; o.account_url = g_kept_url;
    o.retry_ms = 20; o.retry_max_ms = 400; o.poll_ms = 20;
    o.alert = record_alert;
    alerts_reset();
    oc_certs *w = oc_certs_start(&o);
    CHECK(w != NULL);
    if (!w) return;
    for (int i = 0; i < 100; i++) {
        pthread_mutex_lock(&g_fake.mu); int n = g_fake.n_orders; pthread_mutex_unlock(&g_fake.mu);
        if (n >= 5) break;
        msleep(50);
    }
    oc_certs_stop(w);
    pthread_mutex_lock(&g_fake.mu);
    int n = g_fake.n_orders;
    uint64_t t[5]; for (int i = 0; i < 5 && i < n; i++) t[i] = g_fake.order_ms[i];
    g_fake.order_fail = 0;
    pthread_mutex_unlock(&g_fake.mu);
    CHECK(n >= 5);
    /* Each failure is raised, in words a person can act on, under one key --
     * one entry, however many times (REQ-263). Never obtained, never cleared. */
    int f1 = alert_at(OC_CERTS_ALERT_OBTAIN, 1, "could not be obtained", 0);
    CHECK(f1 >= 0 && alert_at(OC_CERTS_ALERT_OBTAIN, 1, NULL, f1 + 1) > f1);
    CHECK(alert_at(OC_CERTS_ALERT_OBTAIN, 0, NULL, 0) < 0);
    if (n < 5) return;
    printf("  retries after a failed order: %llu, %llu, %llu, %llu ms\n", (unsigned long long)(t[1] - t[0]),
           (unsigned long long)(t[2] - t[1]), (unsigned long long)(t[3] - t[2]), (unsigned long long)(t[4] - t[3]));
    CHECK(t[1] - t[0] >= 20);
    CHECK(t[2] - t[1] >= 200);                                    /* ten times, not twice */
    CHECK(t[3] - t[2] >= 400 && t[4] - t[3] >= 400);              /* the ceiling (a hundred times is over it) */
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
    /* Every run starts from nothing, so the suite runs again in one process
     * (OC_TEST_REPEAT): the fake CA's state and stop flag, the listener's,
     * and what the last run kept. */
    memset(&g_fake, 0, sizeof g_fake);
    g_srv_stop = 0; g_ca_backdate_s = 60;
    g_kept_key[0] = g_kept_url[0] = '\0'; g_kept_accounts = 0;
    g_cert_stores = 0; g_stored_chain[0] = '\0';
    g_first_chain[0] = '\0'; g_first_kept = 0;
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
    test_worker_ari();
    test_worker_retries();
    test_central();

    __atomic_store_n(&g_srv_stop, 1, __ATOMIC_RELEASE); __atomic_store_n(&g_fake.stop, 1, __ATOMIC_RELEASE);
    close(connect_loopback(g_srv_port)); close(connect_loopback(g_fake.port));
    pthread_join(st, NULL); pthread_join(ft, NULL);
    close(g_srv_fd); close(g_fake.fd);
    msleep(100);                                                  /* the detached echo threads */
    oc_tls_server_free(&g_srv);
    oc_tls_set_extra_ca(NULL);
    if (g_fake.have_acct) mbedtls_pk_free(&g_fake.acct);
    pthread_mutex_destroy(&g_fake.mu);
    mbedtls_pk_free(&g_ca.key); mbedtls_ctr_drbg_free(&g_ca.rng); mbedtls_entropy_free(&g_ca.ent);
    unlink(g_ca.path); unlink("build/test_acme_cert.pem"); unlink("build/test_acme_key.pem");
    return failures;
}
