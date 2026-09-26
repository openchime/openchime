/*
 * OpenChime TLS wrapper over vendored mbedTLS. See tls.h and docs/TLS.md.
 */

#include "tls.h"
#include "protocol.h"   /* OC_ALPN_PROTO */
#include "sock.h"       /* POSIX/Winsock shim for the BIO callbacks */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/sha256.h>

/* --- Non-blocking socket BIO ------------------------------------------- */

/* Suppress SIGPIPE at the syscall so a peer vanishing mid-write can't kill the
 * whole process. Absent on Winsock (which never raises SIGPIPE) and on some BSDs
 * (which use SO_NOSIGPIPE); on those it degrades to 0 and hosts fall back to a
 * SIG_IGN handler. */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static int bio_send(void *ctx, const unsigned char *buf, size_t len) {
    int fd = *(int *)ctx;
    /* send() takes (const char *, int) on Winsock and (const void *, size_t) on
     * POSIX; our frames are well under INT_MAX so the casts are safe on both. */
    int n = (int)send(fd, (const char *)buf, (int)len, MSG_NOSIGNAL);
    if (n >= 0) return n;
    if (oc_sock_wouldblock()) return MBEDTLS_ERR_SSL_WANT_WRITE;
    return MBEDTLS_ERR_NET_SEND_FAILED;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len) {
    int fd = *(int *)ctx;
    int n = (int)recv(fd, (char *)buf, (int)len, 0);
    if (n > 0) return n;
    /* A recv() of 0 is EOF. Returning 0 to mbedTLS would spin its input loop
     * forever (it keeps asking for the same bytes), so surface a real error. */
    if (n == 0) return MBEDTLS_ERR_NET_CONN_RESET;
    if (oc_sock_wouldblock()) return MBEDTLS_ERR_SSL_WANT_READ;
    return MBEDTLS_ERR_NET_RECV_FAILED;
}

/* ALPN lists, so port 443 can be demultiplexed between the binary protocol and
 * the HTTP/webhook surface (PROTOCOL.md §1, ARCH-54). NULL-terminated; each
 * array must outlive the config that points at it.
 *
 * A binary-protocol client offers oc/1 alone. The server advertises oc/1 *and*
 * http/1.1: mbedTLS picks the first entry of the server's list that the client
 * also offers, so an oc/1 peer still gets the binary protocol, while a webhook
 * sender — which offers a normal list such as h2,http/1.1 and would otherwise
 * be aborted with `no_application_protocol` — negotiates http/1.1 and reaches
 * the HTTP handler. */
static const char *oc_tls_alpn[]        = { OC_ALPN_PROTO, NULL };
static const char *oc_tls_alpn_server[] = { OC_ALPN_PROTO, OC_ALPN_HTTP11, NULL };

static oc_tls_status status_of(int rc) {
    if (rc == MBEDTLS_ERR_SSL_WANT_READ)  return OC_TLS_WANT_READ;
    if (rc == MBEDTLS_ERR_SSL_WANT_WRITE) return OC_TLS_WANT_WRITE;
    if (rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return OC_TLS_CLOSED;
    return OC_TLS_ERROR;
}

static int sha256_der(const mbedtls_x509_crt *crt, uint8_t out[OC_TLS_FINGERPRINT_LEN]) {
    if (crt == NULL || crt->raw.p == NULL) return -1;
    return mbedtls_sha256(crt->raw.p, crt->raw.len, out, 0);
}

/* --- Self-signed certificate generation (ARCH-10) ---------------------- */

/* Generate a P-256 self-signed cert into `s->cert`/`s->key`. When cert_path and
 * key_path are non-NULL, also persist PEM to disk so a restart reuses it. */
static int gen_selfsigned(oc_tls_server *s, const char *cert_path, const char *key_path) {
    int rc;
    mbedtls_x509write_cert crt;
    unsigned char cert_pem[4096];
    unsigned char key_pem[2048];
    static const unsigned char serial[] = { 0x01 };

    mbedtls_x509write_crt_init(&crt);

    if ((rc = mbedtls_pk_setup(&s->key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY))) != 0)
        goto done;
    if ((rc = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(s->key),
                                  mbedtls_ctr_drbg_random, &s->ctr_drbg)) != 0)
        goto done;

    mbedtls_x509write_crt_set_subject_key(&crt, &s->key);
    mbedtls_x509write_crt_set_issuer_key(&crt, &s->key); /* self-signed */
    if ((rc = mbedtls_x509write_crt_set_subject_name(&crt, "CN=openchime")) != 0) goto done;
    if ((rc = mbedtls_x509write_crt_set_issuer_name(&crt, "CN=openchime")) != 0) goto done;
    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    if ((rc = mbedtls_x509write_crt_set_serial_raw(&crt, (unsigned char *)serial,
                                                   sizeof serial)) != 0) goto done;
    if ((rc = mbedtls_x509write_crt_set_validity(&crt, "20200101000000",
                                                 "20500101000000")) != 0) goto done;
    if ((rc = mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1)) != 0) goto done;

    if ((rc = mbedtls_x509write_crt_pem(&crt, cert_pem, sizeof cert_pem,
                                        mbedtls_ctr_drbg_random, &s->ctr_drbg)) != 0)
        goto done;
    if ((rc = mbedtls_pk_write_key_pem(&s->key, key_pem, sizeof key_pem)) != 0)
        goto done;

    /* Parse our freshly-minted cert back in for use by the SSL config. */
    if ((rc = mbedtls_x509_crt_parse(&s->cert, cert_pem,
                                     strlen((char *)cert_pem) + 1)) != 0)
        goto done;

    if (cert_path && key_path) {
        FILE *f;
        if ((f = fopen(cert_path, "w")) != NULL) {
            fputs((char *)cert_pem, f);
            fclose(f);
        }
        if ((f = fopen(key_path, "w")) != NULL) {
            fputs((char *)key_pem, f);
            fclose(f);
        }
    }
    rc = 0;

done:
    mbedtls_x509write_crt_free(&crt);
    mbedtls_platform_zeroize(key_pem, sizeof key_pem);
    return rc;
}

/* --- Server ------------------------------------------------------------- */

int oc_tls_server_init(oc_tls_server *s, const char *cert_path, const char *key_path) {
    int rc;
    static const char *pers = "openchimed-tls-server";

    mbedtls_entropy_init(&s->entropy);
    mbedtls_ctr_drbg_init(&s->ctr_drbg);
    mbedtls_pk_init(&s->key);
    mbedtls_x509_crt_init(&s->cert);
    mbedtls_ssl_config_init(&s->conf);

    if ((rc = mbedtls_ctr_drbg_seed(&s->ctr_drbg, mbedtls_entropy_func, &s->entropy,
                                    (const unsigned char *)pers, strlen(pers))) != 0)
        return rc;

    /* Reuse an existing cert+key if both are present; otherwise generate. */
    int have = 0;
    if (cert_path && key_path &&
        mbedtls_x509_crt_parse_file(&s->cert, cert_path) == 0 &&
        mbedtls_pk_parse_keyfile(&s->key, key_path, NULL,
                                 mbedtls_ctr_drbg_random, &s->ctr_drbg) == 0) {
        have = 1;
    }
    if (!have) {
        /* Reset the two objects in case a partial parse above dirtied them. */
        mbedtls_x509_crt_free(&s->cert); mbedtls_x509_crt_init(&s->cert);
        mbedtls_pk_free(&s->key);        mbedtls_pk_init(&s->key);
        if ((rc = gen_selfsigned(s, cert_path, key_path)) != 0) return rc;
    }

    if ((rc = mbedtls_ssl_config_defaults(&s->conf, MBEDTLS_SSL_IS_SERVER,
                                          MBEDTLS_SSL_TRANSPORT_STREAM,
                                          MBEDTLS_SSL_PRESET_DEFAULT)) != 0)
        return rc;
    mbedtls_ssl_conf_rng(&s->conf, mbedtls_ctr_drbg_random, &s->ctr_drbg);
    if ((rc = mbedtls_ssl_conf_own_cert(&s->conf, &s->cert, &s->key)) != 0)
        return rc;
    if ((rc = mbedtls_ssl_conf_alpn_protocols(&s->conf, oc_tls_alpn_server)) != 0)
        return rc;
    return 0;
}

void oc_tls_server_free(oc_tls_server *s) {
    mbedtls_ssl_config_free(&s->conf);
    mbedtls_x509_crt_free(&s->cert);
    mbedtls_pk_free(&s->key);
    mbedtls_ctr_drbg_free(&s->ctr_drbg);
    mbedtls_entropy_free(&s->entropy);
}

int oc_tls_server_fingerprint(const oc_tls_server *s, uint8_t out[OC_TLS_FINGERPRINT_LEN]) {
    return sha256_der(&s->cert, out);
}

/* --- Client ------------------------------------------------------------- */

/* Verify callback: when pinning, accept the leaf iff its SHA-256 matches the
 * pin, overriding the usual CA-chain result (TOFU, ARCH-10). */
static int client_verify(void *ctx, mbedtls_x509_crt *crt, int depth, uint32_t *flags) {
    oc_tls_client *c = (oc_tls_client *)ctx;
    /* CA mode does a real chain check; leave mbedTLS's own result untouched. */
    if (c->ca_mode) return 0;
    /* Pin only the leaf; there is no CA chain, so clear chain-level flags. */
    if (depth != 0) { *flags = 0; return 0; }
    if (!c->have_pin) { *flags = 0; return 0; }
    uint8_t fp[OC_TLS_FINGERPRINT_LEN];
    if (sha256_der(crt, fp) == 0 && memcmp(fp, c->pin, sizeof fp) == 0)
        *flags = 0;                              /* pin matched: trust it */
    else
        *flags = MBEDTLS_X509_BADCERT_NOT_TRUSTED; /* mismatch: reject after handshake */
    return 0;
}

int oc_tls_client_init_ex(oc_tls_client *c, const uint8_t *pin, const char **alpn) {
    int rc;
    static const char *pers = "openchimed-tls-client";

    mbedtls_entropy_init(&c->entropy);
    mbedtls_ctr_drbg_init(&c->ctr_drbg);
    mbedtls_ssl_config_init(&c->conf);
    mbedtls_x509_crt_init(&c->ca);
    c->have_pin = 0;
    c->ca_mode = 0;
    if (pin) { memcpy(c->pin, pin, OC_TLS_FINGERPRINT_LEN); c->have_pin = 1; }

    if ((rc = mbedtls_ctr_drbg_seed(&c->ctr_drbg, mbedtls_entropy_func, &c->entropy,
                                    (const unsigned char *)pers, strlen(pers))) != 0)
        return rc;
    if ((rc = mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT,
                                          MBEDTLS_SSL_TRANSPORT_STREAM,
                                          MBEDTLS_SSL_PRESET_DEFAULT)) != 0)
        return rc;
    mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &c->ctr_drbg);
    /* OPTIONAL so the handshake runs the verify callback without a CA chain
     * (REQUIRED would refuse outright). The callback records whether the pin
     * matched; oc_tls_handshake then rejects a non-clean verify result, so a
     * mismatch still fails the connection (TOFU, ARCH-10). */
    mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
    mbedtls_ssl_conf_verify(&c->conf, client_verify, c);
    /* `alpn` selects which side of the daemon's ALPN demux (ARCH-54) this client
     * lands on: the oc/1 list for the binary protocol, an HTTP list (or NULL,
     * offering nothing) for the HTTP handler. */
    if (alpn && (rc = mbedtls_ssl_conf_alpn_protocols(&c->conf, alpn)) != 0)
        return rc;
    return 0;
}

int oc_tls_client_init(oc_tls_client *c, const uint8_t *pin) {
    return oc_tls_client_init_ex(c, pin, oc_tls_alpn);
}

/* Mozilla's roots, compiled in (third_party/ca-roots/ca_roots.c, generated by
 * scripts/update_ca_roots.sh). Never the host's store: what a build trusts is
 * the same on every distribution, in a container with none, and on Windows. */
extern const char oc_ca_roots_pem[];
extern const size_t oc_ca_roots_pem_size;

/* The operator's additional roots, PEM, NUL-terminated; NULL for none. Set once
 * at startup, before any thread can read it. */
static char  *g_extra_ca;
static size_t g_extra_ca_size;   /* including the NUL, as mbedTLS wants it */

int oc_tls_set_extra_ca(const char *path) {
    free(g_extra_ca);
    g_extra_ca = NULL;
    g_extra_ca_size = 0;
    if (!path || !*path) return 0;

    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char *buf = NULL;
    long n = -1;
    if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0 &&
        (buf = malloc((size_t)n + 1)) != NULL && fread(buf, 1, (size_t)n, f) == (size_t)n) {
        buf[n] = '\0';
    } else {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (!buf) return -1;

    /* Every certificate must parse. A file that is half read would trust less
     * than its operator believes, and they would learn it from a failed upload. */
    mbedtls_x509_crt probe;
    mbedtls_x509_crt_init(&probe);
    int rc = mbedtls_x509_crt_parse(&probe, (const unsigned char *)buf, (size_t)n + 1);
    mbedtls_x509_crt_free(&probe);
    if (rc != 0) { free(buf); return -1; }

    g_extra_ca = buf;
    g_extra_ca_size = (size_t)n + 1;
    return 0;
}

int oc_tls_client_init_ca(oc_tls_client *c) {
    int rc = oc_tls_client_init_ex(c, NULL, NULL);   /* no pin, no ALPN */
    if (rc != 0) return rc;
    c->ca_mode = 1;

    /* >0 is "parsed some, skipped some": a root this mbedTLS cannot read costs
     * that one root, not every connection. Only <0 is a real failure. The test
     * holds that every built-in root parses. */
    if (mbedtls_x509_crt_parse(&c->ca, (const unsigned char *)oc_ca_roots_pem,
                               oc_ca_roots_pem_size) < 0)
        return -1;
    if (g_extra_ca &&
        mbedtls_x509_crt_parse(&c->ca, (const unsigned char *)g_extra_ca, g_extra_ca_size) != 0)
        return -1;

    mbedtls_ssl_conf_ca_chain(&c->conf, &c->ca, NULL);
    mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    return 0;
}

void oc_tls_client_free(oc_tls_client *c) {
    mbedtls_x509_crt_free(&c->ca);
    mbedtls_ssl_config_free(&c->conf);
    mbedtls_ctr_drbg_free(&c->ctr_drbg);
    mbedtls_entropy_free(&c->entropy);
}

/* --- Connection --------------------------------------------------------- */

int oc_tls_conn_init(oc_tls_conn *c, mbedtls_ssl_config *conf, int fd) {
    int rc;
    mbedtls_ssl_init(&c->ssl);
    c->fd = fd;
    if ((rc = mbedtls_ssl_setup(&c->ssl, conf)) != 0) return rc;
    mbedtls_ssl_set_bio(&c->ssl, &c->fd, bio_send, bio_recv, NULL);
    return 0;
}

int oc_tls_conn_set_hostname(oc_tls_conn *c, const char *host) {
    return mbedtls_ssl_set_hostname(&c->ssl, host);
}

void oc_tls_conn_free(oc_tls_conn *c) {
    mbedtls_ssl_free(&c->ssl);
}

int oc_tls_conn_cert_rejected(const oc_tls_conn *c) {
    uint32_t vr = mbedtls_ssl_get_verify_result(&c->ssl);
    vr &= ~(uint32_t)MBEDTLS_X509_BADCERT_SKIP_VERIFY;
    return vr != 0;
}

oc_tls_status oc_tls_handshake(oc_tls_conn *c) {
    int rc = mbedtls_ssl_handshake(&c->ssl);
    if (rc != 0) return status_of(rc);
    /* Handshake completed at the TLS layer. Enforce the client-side peer
     * verification result: a pinned client leaves real flag bits set on a
     * mismatch (e.g. BADCERT_NOT_TRUSTED). A server connection performs no
     * peer verification, so its result is exactly BADCERT_SKIP_VERIFY, which we
     * mask off; anything remaining is a genuine failure. */
    uint32_t vr = mbedtls_ssl_get_verify_result(&c->ssl);
    vr &= ~(uint32_t)MBEDTLS_X509_BADCERT_SKIP_VERIFY;
    if (vr != 0) return OC_TLS_ERROR;
    return OC_TLS_OK;
}

oc_tls_status oc_tls_read(oc_tls_conn *c, void *buf, size_t len, size_t *n) {
    int rc = mbedtls_ssl_read(&c->ssl, (unsigned char *)buf, len);
    if (rc > 0) { *n = (size_t)rc; return OC_TLS_OK; }
    if (rc == 0) return OC_TLS_CLOSED;
    return status_of(rc);
}

size_t oc_tls_pending(const oc_tls_conn *c) {
    return mbedtls_ssl_get_bytes_avail(&c->ssl);
}

oc_tls_status oc_tls_write(oc_tls_conn *c, const void *buf, size_t len, size_t *n) {
    int rc = mbedtls_ssl_write(&c->ssl, (const unsigned char *)buf, len);
    if (rc >= 0) { *n = (size_t)rc; return OC_TLS_OK; }
    return status_of(rc);
}

int oc_tls_peer_fingerprint(const oc_tls_conn *c, uint8_t out[OC_TLS_FINGERPRINT_LEN]) {
    const mbedtls_x509_crt *peer = mbedtls_ssl_get_peer_cert(&c->ssl);
    return sha256_der(peer, out);
}

const char *oc_tls_alpn_selected(const oc_tls_conn *c) {
    return mbedtls_ssl_get_alpn_protocol(&c->ssl);
}
