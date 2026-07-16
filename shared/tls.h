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

/* Client-side TLS state. `pin` selects trust: if it is non-NULL, the peer
 * certificate's SHA-256 must equal it (TOFU pinning, ARCH-10); if NULL, any
 * certificate is accepted (used only where the caller pins out-of-band). */
typedef struct {
    mbedtls_entropy_context  entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_ssl_config       conf;
    uint8_t                  pin[OC_TLS_FINGERPRINT_LEN];
    int                      have_pin;
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
/* As above, but `with_alpn == 0` omits the oc/1 ALPN — for an HTTP/webhook
 * client that the daemon should route to its HTTP handler (ARCH-32/54). */
int  oc_tls_client_init_ex(oc_tls_client *c, const uint8_t *pin, int with_alpn);
void oc_tls_client_free(oc_tls_client *c);

/* Set up a connection object. `endpoint` is MBEDTLS_SSL_IS_SERVER or
 * MBEDTLS_SSL_IS_CLIENT and must match `conf`. Returns 0 on success. */
int  oc_tls_conn_init(oc_tls_conn *c, mbedtls_ssl_config *conf, int fd);
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

/* The ALPN protocol negotiated on this connection, or NULL if none. The daemon
 * routes port 443 by this: OC_ALPN_PROTO is the binary protocol (PROTOCOL.md §1). */
const char *oc_tls_alpn_selected(const oc_tls_conn *c);

#endif /* OPENCHIME_TLS_H */
