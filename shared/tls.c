/*
 * OpenChime TLS wrapper over vendored mbedTLS. See tls.h and docs/TLS.md.
 */

#include "tls.h"
#include "protocol.h"   /* OC_ALPN_PROTO */
#include "sock.h"       /* POSIX/Winsock shim for the BIO callbacks */

#include <stdio.h>
#if !defined(_WIN32)
#include <pthread.h>
#endif
#include <stdlib.h>
#include <string.h>

#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pem.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/sha256.h>

/* A plain connection's send (oc_tls_conn_init_plain): no SIGPIPE where the flag exists. */
#ifdef MSG_NOSIGNAL
#  define OC_SEND_FLAGS MSG_NOSIGNAL
#else
#  define OC_SEND_FLAGS 0
#endif

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
/* acme-tls/1 last: a CA validating a name offers it alone, so it is chosen only
 * then, and never over oc/1 or http/1.1 for anyone offering those. */
static const char *oc_tls_alpn_server[] = { OC_ALPN_PROTO, OC_ALPN_HTTP11, OC_TLS_ALPN_ACME, NULL };

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

/* The one critical extension a certificate here may carry that mbedTLS does not
 * know: ACME's acmeIdentifier (RFC 8737, 1.3.6.1.5.5.7.1.31), on the
 * TLS-ALPN-01 challenge certificate. Any other unknown critical extension is
 * refused, as RFC 5280 says. */
static int acme_ext_cb(void *ctx, mbedtls_x509_crt const *crt, mbedtls_x509_buf const *oid,
                       int critical, const unsigned char *p, const unsigned char *end) {
    (void)ctx; (void)crt; (void)critical; (void)p; (void)end;
    static const unsigned char ACME_ID[] = { 0x2b, 0x06, 0x01, 0x05, 0x05, 0x07, 0x01, 0x1f };
    return oid->len == sizeof ACME_ID && !memcmp(oid->p, ACME_ID, sizeof ACME_ID) ? 0
         : MBEDTLS_ERR_X509_FEATURE_UNAVAILABLE;
}

/* Every certificate in the PEM `z` (NUL-terminated), through acme_ext_cb. */
static int parse_chain(mbedtls_x509_crt *chain, const char *z) {
    int n = 0;
    for (const char *p = z; (p = strstr(p, "-----BEGIN CERTIFICATE-----")) != NULL; ) {
        mbedtls_pem_context pem;
        mbedtls_pem_init(&pem);
        size_t used = 0;
        int rc = mbedtls_pem_read_buffer(&pem, "-----BEGIN CERTIFICATE-----", "-----END CERTIFICATE-----",
                                         (const unsigned char *)p, NULL, 0, &used);
        if (rc == 0) {
            size_t dl = 0;
            const unsigned char *der = mbedtls_pem_get_buffer(&pem, &dl);
            rc = mbedtls_x509_crt_parse_der_with_ext_cb(chain, der, dl, 1, acme_ext_cb, NULL);
        }
        mbedtls_pem_free(&pem);
        if (rc != 0 || !used) return -1;
        p += used;
        n++;
    }
    return n ? 0 : -1;
}

/* A chain and key, parsed from PEM, held once. NULL if either does not parse. */
static oc_tls_bundle *bundle_new(oc_tls_server *s, const char *chain_pem, size_t chain_len,
                                 const char *key_pem, size_t key_len) {
    if (!chain_pem || !key_pem) return NULL;
    oc_tls_bundle *b = calloc(1, sizeof *b);
    if (!b) return NULL;
    mbedtls_x509_crt_init(&b->chain);
    mbedtls_pk_init(&b->key);
    /* mbedtls wants PEM NUL-terminated and counted with the NUL. */
    char *c = malloc(chain_len + 1), *k = malloc(key_len + 1);
    int ok = c && k;
    if (ok) {
        memcpy(c, chain_pem, chain_len); c[chain_len] = '\0';
        memcpy(k, key_pem, key_len);     k[key_len] = '\0';
        ok = parse_chain(&b->chain, c) == 0 &&
             mbedtls_pk_parse_key(&b->key, (const unsigned char *)k, key_len + 1, NULL, 0,
                                  mbedtls_ctr_drbg_random, &s->ctr_drbg) == 0;
    }
    if (k) { mbedtls_platform_zeroize(k, key_len + 1); free(k); }
    free(c);
    if (!ok) { mbedtls_x509_crt_free(&b->chain); mbedtls_pk_free(&b->key); free(b); return NULL; }
    b->refs = 1;
    return b;
}

/* Let go of a bundle; the last to let go frees it. */
static void bundle_put(oc_tls_bundle *b) {
    if (!b || __atomic_sub_fetch(&b->refs, 1, __ATOMIC_ACQ_REL) != 0) return;
    mbedtls_x509_crt_free(&b->chain);
    mbedtls_pk_free(&b->key);
    free(b);
}

static int name_eq(const char *a, const unsigned char *b, size_t blen) {
    if (strlen(a) != blen) return 0;
    for (size_t i = 0; i < blen; i++) {
        unsigned char x = (unsigned char)a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return 1;
}

/* Chooses what this handshake presents, once the whole ClientHello is read and
 * both the name asked for and the ALPN chosen are known (mbedtls_ssl_conf_cert_cb).
 * An ACME validation gets the challenge certificate for its name, or nothing:
 * a CA must never be shown a certificate it did not ask about, and nobody else
 * may be shown a challenge one. Everyone else gets the current certificate. */
static int cert_cb(mbedtls_ssl_context *ssl) {
    oc_tls_conn *c = mbedtls_ssl_get_user_data_p(ssl);
    oc_tls_server *s = c ? c->server : NULL;
    if (!s) return 0;
    const char *alpn = mbedtls_ssl_get_alpn_protocol(ssl);
    oc_tls_bundle *b = NULL;
    oc_mutex_lock(&s->mu);
    if (alpn && strcmp(alpn, OC_TLS_ALPN_ACME) == 0) {
        size_t n = 0;
        const unsigned char *sni = mbedtls_ssl_get_hs_sni(ssl, &n);
        for (int i = 0; sni && i < OC_TLS_CHALLENGES && !b; i++)
            if (s->challenge[i].b && name_eq(s->challenge[i].name, sni, n)) b = s->challenge[i].b;
        if (!b) { oc_mutex_unlock(&s->mu); return MBEDTLS_ERR_SSL_HANDSHAKE_FAILURE; }
    } else {
        b = s->current;
    }
    if (b) __atomic_add_fetch(&b->refs, 1, __ATOMIC_ACQ_REL);
    oc_mutex_unlock(&s->mu);
    if (!b) return 0;                                  /* what the daemon started with */
    bundle_put(c->bundle);
    c->bundle = b;
    return mbedtls_ssl_set_hs_own_cert(ssl, &b->chain, &b->key);
}

int oc_tls_server_use(oc_tls_server *s, const char *chain_pem, size_t chain_len,
                      const char *key_pem, size_t key_len) {
    oc_tls_bundle *b = bundle_new(s, chain_pem, chain_len, key_pem, key_len);
    if (!b) return -1;
    oc_mutex_lock(&s->mu);
    oc_tls_bundle *old = s->current;
    s->current = b;
    oc_mutex_unlock(&s->mu);
    bundle_put(old);
    return 0;
}

int oc_tls_server_set_challenge(oc_tls_server *s, const char *name,
                                const char *chain_pem, size_t chain_len,
                                const char *key_pem, size_t key_len) {
    if (!name || strlen(name) >= sizeof s->challenge[0].name) return -1;
    oc_tls_bundle *b = NULL;
    if (chain_pem && !(b = bundle_new(s, chain_pem, chain_len, key_pem, key_len))) return -1;
    oc_tls_bundle *old = NULL;
    int rc = 0;
    oc_mutex_lock(&s->mu);
    int slot = -1, free_slot = -1;
    for (int i = 0; i < OC_TLS_CHALLENGES; i++) {
        if (s->challenge[i].b && name_eq(s->challenge[i].name, (const unsigned char *)name, strlen(name))) slot = i;
        else if (!s->challenge[i].b && free_slot < 0) free_slot = i;
    }
    if (slot >= 0) { old = s->challenge[slot].b; s->challenge[slot].b = NULL; }
    if (b) {
        int at = slot >= 0 ? slot : free_slot;
        if (at < 0) rc = -1;
        else { snprintf(s->challenge[at].name, sizeof s->challenge[at].name, "%s", name); s->challenge[at].b = b; b = NULL; }
    }
    oc_mutex_unlock(&s->mu);
    bundle_put(old);
    bundle_put(b);                                     /* not placed */
    return rc;
}

/* The ticket callbacks share one context (mbedtls_ssl_conf_session_tickets_cb):
 * the server, whose ticket key seals a ticket and opens one a client offers --
 * counted when it resumes a session. */
static int ticket_write(void *p, const mbedtls_ssl_session *session, unsigned char *start,
                        const unsigned char *end, size_t *tlen, uint32_t *lifetime) {
    oc_tls_server *s = p;
    return mbedtls_ssl_ticket_write(&s->ticket, session, start, end, tlen, lifetime);
}

static int ticket_parse(void *p, mbedtls_ssl_session *session, unsigned char *buf, size_t len) {
    oc_tls_server *s = p;
    int rc = mbedtls_ssl_ticket_parse(&s->ticket, session, buf, len);
    if (rc == 0) __atomic_add_fetch(&s->resumed, 1, __ATOMIC_RELAXED);
    return rc;
}

int oc_tls_server_init(oc_tls_server *s, const char *cert_path, const char *key_path) {
    int rc;
    static const char *pers = "openchimed-tls-server";

    mbedtls_entropy_init(&s->entropy);
    mbedtls_ctr_drbg_init(&s->ctr_drbg);
    mbedtls_pk_init(&s->key);
    mbedtls_x509_crt_init(&s->cert);
    mbedtls_ssl_config_init(&s->conf);
    mbedtls_ssl_ticket_init(&s->ticket);
    s->resumed = 0;
    oc_mutex_init(&s->mu);
    s->current = NULL;
    memset(s->challenge, 0, sizeof s->challenge);

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
    /* Session tickets (ARCH-22): the key is the daemon's and lives only in
     * memory; the ticket context locks for itself, so the I/O threads share it. */
    if ((rc = mbedtls_ssl_ticket_setup(&s->ticket, mbedtls_ctr_drbg_random, &s->ctr_drbg,
                                       MBEDTLS_CIPHER_AES_256_GCM, OC_TLS_TICKET_LIFETIME_S)) != 0)
        return rc;
    mbedtls_ssl_conf_session_tickets_cb(&s->conf, ticket_write, ticket_parse, s);
    /* The certificate each handshake presents is chosen per handshake (cert_cb),
     * which finds the server from the connection. */
    mbedtls_ssl_conf_set_user_data_p(&s->conf, s);
    mbedtls_ssl_conf_cert_cb(&s->conf, cert_cb);
    return 0;
}

void oc_tls_server_free(oc_tls_server *s) {
    bundle_put(s->current);
    s->current = NULL;
    for (int i = 0; i < OC_TLS_CHALLENGES; i++) { bundle_put(s->challenge[i].b); s->challenge[i].b = NULL; }
    oc_mutex_destroy(&s->mu);
    mbedtls_ssl_ticket_free(&s->ticket);
    mbedtls_ssl_config_free(&s->conf);
    mbedtls_x509_crt_free(&s->cert);
    mbedtls_pk_free(&s->key);
    mbedtls_ctr_drbg_free(&s->ctr_drbg);
    mbedtls_entropy_free(&s->entropy);
}

int oc_tls_server_fingerprint(oc_tls_server *s, uint8_t out[OC_TLS_FINGERPRINT_LEN]) {
    oc_mutex_lock(&s->mu);
    int rc = sha256_der(s->current ? &s->current->chain : &s->cert, out);
    oc_mutex_unlock(&s->mu);
    return rc;
}

/* --- Client ------------------------------------------------------------- */

/* Verify callback: when pinning, accept the leaf iff its SHA-256 matches the
 * pin, overriding the usual CA-chain result (ARCH-10). */
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
    c->roots = NULL;
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
     * mismatch still fails the connection (ARCH-10). */
    mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
    mbedtls_ssl_conf_verify(&c->conf, client_verify, c);
    /* `alpn` selects which side of the daemon's ALPN demux (ARCH-54) this client
     * lands on: the oc/1 list for the binary protocol, an HTTP list (or NULL,
     * offering nothing) for the HTTP handler. */
    if (alpn && (rc = mbedtls_ssl_conf_alpn_protocols(&c->conf, alpn)) != 0)
        return rc;
    /* Hear about the tickets a TLS 1.3 server sends after the handshake, so a
     * connection that keeps them (oc_tls_conn_resume) can; one that does not
     * lets them go (oc_tls_read). */
    mbedtls_ssl_conf_tls13_enable_signal_new_session_tickets(
        &c->conf, MBEDTLS_SSL_TLS1_3_SIGNAL_NEW_SESSION_TICKETS_ENABLED);
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

/* The roots a daemon is verified against, shared, read-only, by the clients
 * given them (mbedTLS only reads a CA chain): the built-in, the extra, and the
 * operating system's trusted roots. */

#if defined(_WIN32)
#include <wincrypt.h>
/* Windows' ROOT store: where group policy puts an organisation's internal CA. */
static int add_os_roots(mbedtls_x509_crt *chain) {
    HCERTSTORE st = CertOpenSystemStoreW(0, L"ROOT");
    if (!st) return 0;
    int n = 0;
    for (PCCERT_CONTEXT cc = NULL; (cc = CertEnumCertificatesInStore(st, cc)) != NULL; )
        if (mbedtls_x509_crt_parse_der(chain, cc->pbCertEncoded, cc->cbCertEncoded) == 0) n++;
    CertCloseStore(st, 0);
    return n;
}
#else
/* The distribution's bundle (Debian/Ubuntu, then Fedora/RHEL), where an
 * organisation installs its internal CA; OC_TLS_OS_ROOTS overrides, for tests. */
static int add_os_roots(mbedtls_x509_crt *chain) {
    const char *env = getenv("OC_TLS_OS_ROOTS");
    const char *files[] = { env && *env ? env : "/etc/ssl/certs/ca-certificates.crt",
                            env && *env ? NULL : "/etc/pki/tls/certs/ca-bundle.crt", NULL };
    for (int i = 0; files[i]; i++) {
        FILE *f = fopen(files[i], "rb");
        if (!f) continue;
        fclose(f);
        int rc = mbedtls_x509_crt_parse_file(chain, files[i]);   /* bad blocks skipped: rc > 0 */
        if (rc >= 0) return 1;
    }
    return 0;
}
#endif

/* The roots a set of clients was given: built from the extra roots as they
 * were, and freed when the last client holding them is. A new extra-roots file
 * makes later clients get a new set; clients already up keep theirs. */
typedef struct { mbedtls_x509_crt crt; int refs; } roots_set;
static roots_set *g_roots;
static oc_mutex_t g_roots_mu;

static void roots_mu_init(void) { oc_mutex_init(&g_roots_mu); }
#if defined(_WIN32)
static BOOL CALLBACK roots_mu_cb(PINIT_ONCE o, PVOID p, PVOID *ctx) {
    (void)o; (void)p; (void)ctx; roots_mu_init(); return TRUE;
}
static void roots_mu_once(void) {
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&once, roots_mu_cb, NULL, NULL);
}
#else
static void roots_mu_once(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, roots_mu_init);
}
#endif

static void roots_put(roots_set *r) {
    if (!r) return;
    oc_mutex_lock(&g_roots_mu);
    int last = --r->refs == 0;
    oc_mutex_unlock(&g_roots_mu);
    if (last) { mbedtls_x509_crt_free(&r->crt); free(r); }
}

static roots_set *roots_get(void) {
    roots_mu_once();
    oc_mutex_lock(&g_roots_mu);
    if (!g_roots && (g_roots = calloc(1, sizeof *g_roots)) != NULL) {
        mbedtls_x509_crt_init(&g_roots->crt);
        mbedtls_x509_crt_parse(&g_roots->crt, (const unsigned char *)oc_ca_roots_pem, oc_ca_roots_pem_size);
        if (g_extra_ca) mbedtls_x509_crt_parse(&g_roots->crt, (const unsigned char *)g_extra_ca, g_extra_ca_size);
        add_os_roots(&g_roots->crt);
        g_roots->refs = 1;                               /* the global's own */
    }
    roots_set *r = g_roots;
    if (r) r->refs++;
    oc_mutex_unlock(&g_roots_mu);
    return r;
}

/* Later clients build from the extra roots now set. */
static void roots_reset(void) {
    roots_mu_once();
    oc_mutex_lock(&g_roots_mu);
    roots_set *old = g_roots;
    g_roots = NULL;
    oc_mutex_unlock(&g_roots_mu);
    roots_put(old);
}

int oc_tls_set_extra_ca(const char *path) {
    roots_reset();
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

int oc_tls_client_init_verify(oc_tls_client *c, const char **alpn) {
    static const char *oc1[] = { OC_ALPN_PROTO, NULL };
    int rc = oc_tls_client_init_ex(c, NULL, alpn ? alpn : oc1);
    if (rc != 0) return rc;
    c->ca_mode = 1;
    roots_set *r = roots_get();
    if (!r) return -1;
    c->roots = r;
    mbedtls_ssl_conf_ca_chain(&c->conf, &r->crt, NULL);
    /* OPTIONAL: the handshake completes and mbedTLS records what it found; the
     * caller defers to oc_tls_conn_ca_trusted and its own fallback. */
    mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
    return 0;
}

void oc_tls_conn_defer_verify(oc_tls_conn *c) { c->defer = 1; }

int oc_tls_conn_init_plain(oc_tls_conn *c, int fd) {
    memset(c, 0, sizeof *c);
    c->fd = fd;
    c->plain = 1;
    return 0;
}

int oc_tls_conn_ca_trusted(const oc_tls_conn *c) {
    if (c->plain) return 0;
    uint32_t vr = mbedtls_ssl_get_verify_result(&c->ssl);
    return vr == 0 && c->ip_ok;
}

void oc_tls_client_free(oc_tls_client *c) {
    mbedtls_ssl_config_free(&c->conf);                   /* before the roots it points at */
    roots_put(c->roots);
    c->roots = NULL;
    mbedtls_x509_crt_free(&c->ca);
    mbedtls_ctr_drbg_free(&c->ctr_drbg);
    mbedtls_entropy_free(&c->entropy);
}

/* --- Connection --------------------------------------------------------- */

int oc_tls_conn_init(oc_tls_conn *c, mbedtls_ssl_config *conf, int fd) {
    int rc;
    mbedtls_ssl_init(&c->ssl);
    c->fd = fd;
    c->plain = 0;
    c->keep = NULL;
    c->expect_ip_len = 0;
    c->bundle = NULL;
    c->defer = 0; c->ip_ok = 0;
    c->server = mbedtls_ssl_conf_get_user_data_p(conf);   /* a server's conf carries it */
    if ((rc = mbedtls_ssl_setup(&c->ssl, conf)) != 0) return rc;
    mbedtls_ssl_set_user_data_p(&c->ssl, c);
    mbedtls_ssl_set_bio(&c->ssl, &c->fd, bio_send, bio_recv, NULL);
    return 0;
}

int oc_tls_conn_set_hostname(oc_tls_conn *c, const char *host) {
    c->expect_ip_len = 0;
    if (c->plain) return 0;
    if (host) {
        /* An address, bracketed or not, is matched against the certificate's
         * iPAddress SANs after the handshake rather than sent as SNI, which
         * RFC 6066 reserves for host names (mbedTLS would send it as given). */
        char a[64];
        size_t n = strlen(host);
        if (n >= 2 && host[0] == '[' && host[n - 1] == ']') { host++; n -= 2; }
        if (n < sizeof a) {
            memcpy(a, host, n); a[n] = '\0';
            size_t ipn = mbedtls_x509_crt_parse_cn_inet_pton(a, c->expect_ip);
            if (ipn == 4 || ipn == 16) {
                c->expect_ip_len = ipn;
                return mbedtls_ssl_set_hostname(&c->ssl, NULL);
            }
        }
    }
    return mbedtls_ssl_set_hostname(&c->ssl, host);
}

/* Does the peer's certificate name `c->expect_ip` among its iPAddress SANs? */
static int peer_names_ip(oc_tls_conn *c) {
    const mbedtls_x509_crt *peer = mbedtls_ssl_get_peer_cert(&c->ssl);
    if (!peer) return 0;
    int found = 0;
    for (const mbedtls_x509_sequence *seq = &peer->subject_alt_names; seq && !found; seq = seq->next) {
        if (!seq->buf.p) continue;
        mbedtls_x509_subject_alternative_name san;
        memset(&san, 0, sizeof san);
        if (mbedtls_x509_parse_subject_alt_name(&seq->buf, &san) != 0) continue;
        if (san.type == MBEDTLS_X509_SAN_IP_ADDRESS &&
            san.san.unstructured_name.len == c->expect_ip_len &&
            memcmp(san.san.unstructured_name.p, c->expect_ip, c->expect_ip_len) == 0)
            found = 1;
        mbedtls_x509_free_subject_alt_name(&san);
    }
    return found;
}

void oc_tls_conn_free(oc_tls_conn *c) {
    if (c->plain) return;
    mbedtls_ssl_free(&c->ssl);
    bundle_put(c->bundle);
    c->bundle = NULL;
}

void oc_tls_session_init(oc_tls_session *s) {
    mbedtls_ssl_session_init(&s->s);
    s->have = 0;
}

void oc_tls_session_free(oc_tls_session *s) {
    mbedtls_ssl_session_free(&s->s);
    mbedtls_ssl_session_init(&s->s);
    s->have = 0;
}

int oc_tls_conn_resume(oc_tls_conn *c, oc_tls_session *s) {
    if (c->plain) return 0;
    c->keep = s;
    if (s->have && mbedtls_ssl_set_session(&c->ssl, &s->s) != 0) oc_tls_session_free(s);
    return 0;
}

/* The server gave a ticket: keep it where the connection was told to, if
 * anywhere. The caller then goes on with what it was doing (ssl.h). */
static void take_ticket(oc_tls_conn *c) {
    if (!c->keep) return;
    oc_tls_session_free(c->keep);
    c->keep->have = mbedtls_ssl_get_session(&c->ssl, &c->keep->s) == 0;
}

int oc_tls_conn_cert_rejected(const oc_tls_conn *c) {
    uint32_t vr = mbedtls_ssl_get_verify_result(&c->ssl);
    vr &= ~(uint32_t)MBEDTLS_X509_BADCERT_SKIP_VERIFY;
    return vr != 0;
}

oc_tls_status oc_tls_handshake(oc_tls_conn *c) {
    int rc;
    if (c->plain) return OC_TLS_OK;
    while ((rc = mbedtls_ssl_handshake(&c->ssl)) == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
        take_ticket(c);
    if (rc != 0) return status_of(rc);
    /* Handshake completed at the TLS layer. Enforce the client-side peer
     * verification result: a pinned client leaves real flag bits set on a
     * mismatch (e.g. BADCERT_NOT_TRUSTED). A server connection performs no
     * peer verification, so its result is exactly BADCERT_SKIP_VERIFY, which we
     * mask off; anything remaining is a genuine failure. */
    uint32_t vr = mbedtls_ssl_get_verify_result(&c->ssl);
    vr &= ~(uint32_t)MBEDTLS_X509_BADCERT_SKIP_VERIFY;
    c->ip_ok = !c->expect_ip_len || peer_names_ip(c);
    if (c->defer) return OC_TLS_OK;              /* the caller judges it */
    if (vr != 0) return OC_TLS_ERROR;
    /* An address set as the expected name: the chain was verified above; the
     * certificate must also name the address (oc_tls_conn_set_hostname). */
    if (!c->ip_ok) return OC_TLS_ERROR;
    return OC_TLS_OK;
}

oc_tls_status oc_tls_read(oc_tls_conn *c, void *buf, size_t len, size_t *n) {
    /* A TLS 1.3 ticket arrives after the handshake. Reading its record answers
     * WANT_READ with the record held back and its processing still to run, and
     * the next call signals the ticket. The record is already read, so waiting
     * on the socket here would wait for nothing: while TLS holds data it has
     * not processed, go straight on (a few rounds at most). */
    int rc;
    if (c->plain) {
        ssize_t r = recv(c->fd, buf, len, 0);
        if (r > 0) { *n = (size_t)r; return OC_TLS_OK; }
        if (r == 0) return OC_TLS_CLOSED;
        return oc_sock_wouldblock() ? OC_TLS_WANT_READ : OC_TLS_ERROR;
    }
    for (int rounds = 0;; rounds++) {
        rc = mbedtls_ssl_read(&c->ssl, (unsigned char *)buf, len);
        if (rc == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) { take_ticket(c); continue; }
        if (rc == MBEDTLS_ERR_SSL_WANT_READ && rounds < 8 && mbedtls_ssl_check_pending(&c->ssl)) continue;
        break;
    }
    if (rc > 0) { *n = (size_t)rc; return OC_TLS_OK; }
    if (rc == 0) return OC_TLS_CLOSED;
    return status_of(rc);
}

size_t oc_tls_pending(const oc_tls_conn *c) {
    if (c->plain) return 0;
    return mbedtls_ssl_get_bytes_avail(&c->ssl);
}

oc_tls_status oc_tls_write(oc_tls_conn *c, const void *buf, size_t len, size_t *n) {
    int rc;
    if (c->plain) {
        ssize_t r = send(c->fd, buf, len, OC_SEND_FLAGS);
        if (r >= 0) { *n = (size_t)r; return OC_TLS_OK; }
        return oc_sock_wouldblock() ? OC_TLS_WANT_WRITE : OC_TLS_ERROR;
    }
    while ((rc = mbedtls_ssl_write(&c->ssl, (const unsigned char *)buf, len)) == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
        take_ticket(c);
    if (rc >= 0) { *n = (size_t)rc; return OC_TLS_OK; }
    return status_of(rc);
}

int oc_tls_peer_fingerprint(const oc_tls_conn *c, uint8_t out[OC_TLS_FINGERPRINT_LEN]) {
    if (c->plain) return -1;
    const mbedtls_x509_crt *peer = mbedtls_ssl_get_peer_cert(&c->ssl);
    return sha256_der(peer, out);
}

int oc_tls_peer_der(const oc_tls_conn *c, const uint8_t **der, size_t *len) {
    if (c->plain) return -1;
    const mbedtls_x509_crt *peer = mbedtls_ssl_get_peer_cert(&c->ssl);
    if (!peer || !peer->raw.p) return -1;
    *der = peer->raw.p;
    *len = peer->raw.len;
    return 0;
}

const char *oc_tls_alpn_selected(const oc_tls_conn *c) {
    if (c->plain) return OC_ALPN_PROTO;
    return mbedtls_ssl_get_alpn_protocol(&c->ssl);
}
