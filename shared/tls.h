/*
 * OpenChime TLS (ARCH-10, ARCH-51) — a thin wrapper over vendored mbedTLS.
 *
 * The daemon terminates TLS on every client connection (REQ-180, no plaintext
 * fallback). On first run it generates its own self-signed certificate; clients
 * trust it via TOFU pinning (ARCH-10). The wrapper is written for a
 * non-blocking epoll loop: handshake/read/write return WANT_READ / WANT_WRITE
 * when the socket would block, which the caller maps to epoll interest.
 *
 * mbedTLS is pinned and vendored (scripts/build_mbedtls.sh) so local, CI, and
 * the Docker image share one version; see docs/TLS.md.
 */

#ifndef OPENCHIME_TLS_H
#define OPENCHIME_TLS_H

#include <stddef.h>
#include <stdint.h>

#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#define OC_TLS_FINGERPRINT_LEN 32 /* SHA-256 of the certificate DER */

typedef enum {
    OC_TLS_OK        =  0,
    OC_TLS_WANT_READ =  1,
    OC_TLS_WANT_WRITE=  2,
    OC_TLS_CLOSED    =  3,
    OC_TLS_ERROR     = -1
} oc_tls_status;

/* Server-side TLS state: RNG, the daemon's cert+key, and the shared config. */
typedef struct {
    mbedtls_entropy_context  entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_pk_context       key;
    mbedtls_x509_crt         cert;
    mbedtls_ssl_config       conf;
} oc_tls_server;

/* Client-side TLS state, with three mutually exclusive trust modes:
 *   - **TOFU pinning** (`oc_tls_client_init` with a non-NULL `pin`): the peer
 *     leaf's SHA-256 must equal the pin. This is how clients trust a daemon's
 *     self-signed cert (ARCH-10) and is the project's normal mode.
 *   - **No verification** (`oc_tls_client_init` with `pin == NULL`): any
 *     certificate is accepted. Only for callers that pin out-of-band.
 *   - **CA-chain verification** (`oc_tls_client_init_ca`): an ordinary public
 *     PKI check against a CA bundle, plus hostname verification. Used where the
 *     daemon is a *client of someone else's public service* — an S3-compatible
 *     object store (ARCH-70) — which is the opposite trust relationship from
 *     ARCH-10's: there is no fingerprint to pin, the provider rotates certs
 *     freely, and accepting any cert would expose credentials and attachment
 *     bytes to trivial interception. */
typedef struct {
    mbedtls_entropy_context  entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_ssl_config       conf;
    mbedtls_x509_crt         ca;
    uint8_t                  pin[OC_TLS_FINGERPRINT_LEN];
    int                      have_pin;
    int                      ca_mode;
} oc_tls_client;

/* One TLS connection over an already-connected, non-blocking socket `fd`. */
typedef struct {
    mbedtls_ssl_context ssl;
    int                 fd;
} oc_tls_conn;

/* Load the cert+key from the given PEM paths, generating a self-signed pair
 * (and writing it to those paths, if non-NULL) when they are absent. Returns 0
 * on success or a negative mbedTLS error code. */
int  oc_tls_server_init(oc_tls_server *s, const char *cert_path, const char *key_path);
void oc_tls_server_free(oc_tls_server *s);

/* SHA-256 fingerprint of the server's certificate, for out-of-band publication
 * (e.g. the .well-known metadata, ARCH-10/14). Returns 0 on success. */
int  oc_tls_server_fingerprint(const oc_tls_server *s, uint8_t out[OC_TLS_FINGERPRINT_LEN]);

/* Initialize a client. If `pin` is non-NULL it must point to
 * OC_TLS_FINGERPRINT_LEN bytes and enables TOFU pinning. Returns 0 on success. */
int  oc_tls_client_init(oc_tls_client *c, const uint8_t *pin);
/* As above, but with an explicit NULL-terminated ALPN list (which must outlive
 * the client): pass an HTTP list, or NULL to offer no ALPN at all, for a client
 * the daemon should route to its HTTP handler (ARCH-32/54). */
int  oc_tls_client_init_ex(oc_tls_client *c, const uint8_t *pin, const char **alpn);

/* Initialize a client that verifies the peer against a **CA bundle** with
 * hostname checking (MBEDTLS_SSL_VERIFY_REQUIRED), for talking to a public
 * HTTPS service rather than to an OpenChime daemon. `ca_bundle` is a PEM file
 * or directory; NULL probes the usual system locations. No ALPN is offered.
 * The caller MUST call oc_tls_conn_set_hostname() before the handshake, or the
 * hostname is not checked. Returns 0 on success, negative on failure (including
 * "no CA bundle found", which is fatal rather than a silent downgrade). */
int  oc_tls_client_init_ca(oc_tls_client *c, const char *ca_bundle);
void oc_tls_client_free(oc_tls_client *c);

/* Set up a connection object. `endpoint` is MBEDTLS_SSL_IS_SERVER or
 * MBEDTLS_SSL_IS_CLIENT and must match `conf`. Returns 0 on success. */
int  oc_tls_conn_init(oc_tls_conn *c, mbedtls_ssl_config *conf, int fd);
/* Set the expected peer hostname: sends SNI and, under CA verification, makes
 * the certificate's name actually get checked. Call before the handshake. */
int  oc_tls_conn_set_hostname(oc_tls_conn *c, const char *host);
void oc_tls_conn_free(oc_tls_conn *c);

/* Drive the handshake / I/O. Each returns an oc_tls_status; on OK, read/write
 * set *n to the byte count transferred. WANT_READ/WANT_WRITE mean re-arm epoll
 * and call again. */
oc_tls_status oc_tls_handshake(oc_tls_conn *c);
oc_tls_status oc_tls_read(oc_tls_conn *c, void *buf, size_t len, size_t *n);
oc_tls_status oc_tls_write(oc_tls_conn *c, const void *buf, size_t len, size_t *n);

/* SHA-256 of the peer's certificate after a completed handshake (client side).
 * Returns 0 on success, negative if no peer cert is available. */
int  oc_tls_peer_fingerprint(const oc_tls_conn *c, uint8_t out[OC_TLS_FINGERPRINT_LEN]);

/* Nonzero if the last handshake failed the client-side peer verification (a
 * TOFU pin mismatch — the server presented a different certificate), as opposed
 * to a plain transport failure. Distinguishes "cert changed" from "unreachable". */
int  oc_tls_conn_cert_rejected(const oc_tls_conn *c);

/* The ALPN protocol negotiated on this connection, or NULL if none. The daemon
 * routes port 443 by this: OC_ALPN_PROTO is the binary protocol, and anything
 * else — OC_ALPN_HTTP11, or no ALPN — is HTTP (PROTOCOL.md §1). */
const char *oc_tls_alpn_selected(const oc_tls_conn *c);

#endif /* OPENCHIME_TLS_H */
