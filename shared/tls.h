/*
 * OpenChime TLS (ARCH-10, ARCH-51) — a thin wrapper over vendored mbedTLS.
 *
 * The daemon terminates TLS on every client connection (REQ-180, no plaintext
 * fallback). It presents a CA-issued certificate, or on first run generates its
 * own self-signed one, which a client trusts only by its fingerprint (ARCH-10). The wrapper is written for a
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
#include <mbedtls/ssl_ticket.h>
#include <mbedtls/x509_crt.h>

#include "oc_thread.h"

#define OC_TLS_FINGERPRINT_LEN 32 /* SHA-256 of the certificate DER */

/* ALPN of an ACME TLS-ALPN-01 validation (RFC 8737): the CA connects with only
 * this, and is answered with the challenge certificate for the name it asks. */
#define OC_TLS_ALPN_ACME "acme-tls/1"
#define OC_TLS_CHALLENGES 8          /* names being validated at once */

typedef enum {
    OC_TLS_OK        =  0,
    OC_TLS_WANT_READ =  1,
    OC_TLS_WANT_WRITE=  2,
    OC_TLS_CLOSED    =  3,
    OC_TLS_ERROR     = -1
} oc_tls_status;

/* How long a session ticket the daemon issues can be used to resume. Its key
 * is made at start, so a restart ends every ticket issued before it. */
#define OC_TLS_TICKET_LIFETIME_S 86400

/* A certificate chain and its key, as a handshake presents them. Shared by the
 * handshakes that chose it and freed when the last lets go of a retired one. */
typedef struct oc_tls_bundle {
    mbedtls_x509_crt chain;
    mbedtls_pk_context key;
    int              refs;
} oc_tls_bundle;

/* Server-side TLS state: RNG, the daemon's cert+key, the shared config, and the
 * key its session tickets are sealed with. A client that returns with one
 * resumes the session without the certificate exchange and signature a full
 * handshake costs; `resumed` counts those (read with __atomic loads).
 *
 * `cert`/`key` are what the daemon started with (its own, self-signed, or the
 * operator's). `current` is what it presents now: a CA-issued certificate once
 * one has been obtained, swapped in whole by oc_tls_server_use without touching
 * a handshake under way. `challenge` holds the ACME TLS-ALPN-01 certificates
 * (RFC 8737) for names being validated: presented only on an OC_TLS_ALPN_ACME
 * handshake that names one of them. */
typedef struct {
    mbedtls_entropy_context  entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_pk_context       key;
    mbedtls_x509_crt         cert;
    mbedtls_ssl_config       conf;
    mbedtls_ssl_ticket_context ticket;
    unsigned long            resumed;
    oc_mutex_t               mu;             /* current, challenge */
    oc_tls_bundle           *current;        /* NULL: cert/key */
    struct { char name[256]; oc_tls_bundle *b; } challenge[OC_TLS_CHALLENGES];
} oc_tls_server;

/* Client-side TLS state, with three mutually exclusive trust modes:
 *   - **Pinning** (`oc_tls_client_init` with a non-NULL `pin`): the peer
 *     leaf's SHA-256 must equal the pin -- a fingerprint known in advance.
 *   - **No verification** (`oc_tls_client_init` with `pin == NULL`): any
 *     certificate is accepted. Only for callers that pin out-of-band.
 *   - **CA-chain verification** (`oc_tls_client_init_ca`): an ordinary public
 *     PKI check against built-in roots, plus hostname verification. Used where the
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
    void                    *roots;   /* oc_tls_client_init_verify's shared roots, held */
} oc_tls_client;

/* A session a client can resume: kept from a connection the daemon gave a
 * ticket on, offered on the next. */
typedef struct {
    mbedtls_ssl_session s;
    int                 have;
} oc_tls_session;

/* One TLS connection over an already-connected, non-blocking socket `fd`. */
typedef struct {
    mbedtls_ssl_context ssl;
    int                 fd;
    oc_tls_session     *keep;    /* client: where a ticket the server gives goes */
    /* client: an IP address the peer's certificate must name (an iPAddress SAN),
     * checked after the handshake -- set by oc_tls_conn_set_hostname for an
     * address, which is never sent as SNI (RFC 6066 §3). */
    unsigned char       expect_ip[16];
    size_t              expect_ip_len;
    /* server: the certificate this handshake presents, held until it is freed */
    oc_tls_bundle      *bundle;
    void               *server;
    /* client: verification is the caller's to judge after the handshake
     * (oc_tls_conn_defer_verify); `ip_ok` is the address check's answer. */
    int                 defer, ip_ok;
} oc_tls_conn;

/* Load the cert+key from the given PEM paths, generating a self-signed pair
 * (and writing it to those paths, if non-NULL) when they are absent. Returns 0
 * on success or a negative mbedTLS error code. */
int  oc_tls_server_init(oc_tls_server *s, const char *cert_path, const char *key_path);
void oc_tls_server_free(oc_tls_server *s);

/* SHA-256 fingerprint of the certificate the server presents now, for
 * out-of-band publication (the .well-known metadata, ARCH-10/14). Returns 0 on
 * success. */
int  oc_tls_server_fingerprint(oc_tls_server *s, uint8_t out[OC_TLS_FINGERPRINT_LEN]);

/* Present this chain and key from the next handshake on (a CA-issued
 * certificate, or its renewal). Handshakes under way, and connections already
 * up, keep what they had. The PEM is copied. Returns 0, or negative if either
 * does not parse -- in which case nothing changes. */
int  oc_tls_server_use(oc_tls_server *s, const char *chain_pem, size_t chain_len,
                       const char *key_pem, size_t key_len);

/* The ACME TLS-ALPN-01 certificate for `name` (RFC 8737), presented only to a
 * handshake offering OC_TLS_ALPN_ACME for that name; NULL `chain_pem` removes
 * it. Returns 0, or negative (unparsable, or every slot taken). */
int  oc_tls_server_set_challenge(oc_tls_server *s, const char *name,
                                 const char *chain_pem, size_t chain_len,
                                 const char *key_pem, size_t key_len);

/* Initialize a client. If `pin` is non-NULL it must point to
 * OC_TLS_FINGERPRINT_LEN bytes and enables pinning. Returns 0 on success. */
int  oc_tls_client_init(oc_tls_client *c, const uint8_t *pin);
/* As above, but with an explicit NULL-terminated ALPN list (which must outlive
 * the client): pass an HTTP list, or NULL to offer no ALPN at all, for a client
 * the daemon should route to its HTTP handler (ARCH-32/54). */
int  oc_tls_client_init_ex(oc_tls_client *c, const uint8_t *pin, const char **alpn);

/* Initialize a client that verifies the peer against **CA roots** with
 * hostname checking (MBEDTLS_SSL_VERIFY_REQUIRED), for talking to a public
 * HTTPS service rather than to an OpenChime daemon. The roots are Mozilla's,
 * built into the binary, plus any set by oc_tls_set_extra_ca; the host's store
 * is never read. No ALPN is offered. The caller MUST call
 * oc_tls_conn_set_hostname() before the handshake, or the hostname is not
 * checked. Returns 0 on success, negative on failure. */
int  oc_tls_client_init_ca(oc_tls_client *c);

/* Initialize a client that verifies an OpenChime daemon (TLS.md, "Trust"): the
 * built-in roots, any set by oc_tls_set_extra_ca, and the operating system's
 * own trusted roots -- where an organisation's internal CA is -- with ALPN
 * `alpn` (NULL: oc/1). The result is judged by the caller after the handshake:
 * call oc_tls_conn_defer_verify on each connection, then oc_tls_conn_ca_trusted.
 * Returns 0. */
int  oc_tls_client_init_verify(oc_tls_client *c, const char **alpn);

/* Complete the handshake without refusing an untrusted certificate, so the
 * caller can judge it (oc_tls_conn_ca_trusted, oc_tls_peer_fingerprint). */
void oc_tls_conn_defer_verify(oc_tls_conn *c);
/* After a deferred handshake: 1 if the certificate chains to a trusted root and
 * names the host set by oc_tls_conn_set_hostname (or the address). */
int  oc_tls_conn_ca_trusted(const oc_tls_conn *c);

/* Add the roots in the PEM file at `path` to those every later
 * oc_tls_client_init_ca trusts: for a self-hosted service behind a private CA.
 * They are added, never substituted. The file is read now, and every
 * certificate in it must parse. NULL or "" clears. Not thread-safe: call before
 * any client is set up. Returns 0, or -1 with the previous extra roots cleared. */
int  oc_tls_set_extra_ca(const char *path);
void oc_tls_client_free(oc_tls_client *c);

/* Set up a connection object. `endpoint` is MBEDTLS_SSL_IS_SERVER or
 * MBEDTLS_SSL_IS_CLIENT and must match `conf`. Returns 0 on success. */
int  oc_tls_conn_init(oc_tls_conn *c, mbedtls_ssl_config *conf, int fd);
/* Set the expected peer hostname: sends SNI and, under CA verification, makes
 * the certificate's name actually get checked. Call before the handshake. An IP
 * address (IPv4, or IPv6 with or without brackets) is not a hostname: it is sent
 * as no SNI, and the handshake instead requires the certificate to name that
 * address among its iPAddress subject alternative names. */
int  oc_tls_conn_set_hostname(oc_tls_conn *c, const char *host);
void oc_tls_conn_free(oc_tls_conn *c);

void oc_tls_session_init(oc_tls_session *s);
/* Forget it: the next connection does a full handshake. */
void oc_tls_session_free(oc_tls_session *s);
/* Client, before the handshake: offer `s` if it holds a session, and keep in it
 * any ticket the server gives on this connection. `s` must outlive `c`. A
 * session the server no longer accepts costs nothing: the handshake is a full
 * one. Returns 0. */
int  oc_tls_conn_resume(oc_tls_conn *c, oc_tls_session *s);

/* Drive the handshake / I/O. Each returns an oc_tls_status; on OK, read/write
 * set *n to the byte count transferred. WANT_READ/WANT_WRITE mean re-arm epoll
 * and call again. */
oc_tls_status oc_tls_handshake(oc_tls_conn *c);
oc_tls_status oc_tls_read(oc_tls_conn *c, void *buf, size_t len, size_t *n);
/* Decrypted bytes TLS already holds for this connection. They are invisible to
 * poll() on the socket — the record they came in has been read off it — so a
 * loop that polls before reading must check this first or it waits on data it
 * already has. */
size_t oc_tls_pending(const oc_tls_conn *c);
oc_tls_status oc_tls_write(oc_tls_conn *c, const void *buf, size_t len, size_t *n);

/* SHA-256 of the peer's certificate after a completed handshake (client side).
 * Returns 0 on success, negative if no peer cert is available. */
int  oc_tls_peer_fingerprint(const oc_tls_conn *c, uint8_t out[OC_TLS_FINGERPRINT_LEN]);

/* The peer's certificate itself (DER), borrowed from the connection: valid until
 * it is freed. 0 on success, negative if there is none. */
int  oc_tls_peer_der(const oc_tls_conn *c, const uint8_t **der, size_t *len);

/* Nonzero if the last handshake failed the client-side peer verification (a
 * pin mismatch — the server presented a different certificate), as opposed
 * to a plain transport failure. Distinguishes "cert changed" from "unreachable". */
int  oc_tls_conn_cert_rejected(const oc_tls_conn *c);

/* The ALPN protocol negotiated on this connection, or NULL if none. The daemon
 * routes port 443 by this: OC_ALPN_PROTO is the binary protocol, and anything
 * else — OC_ALPN_HTTP11, or no ALPN — is HTTP (PROTOCOL.md §1). */
const char *oc_tls_alpn_selected(const oc_tls_conn *c);

#endif /* OPENCHIME_TLS_H */
