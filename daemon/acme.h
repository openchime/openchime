/*
 * ACME (RFC 8555) with the TLS-ALPN-01 challenge (RFC 8737): a certificate for
 * the daemon's own names from any ACME CA -- Let's Encrypt, or an internal CA
 * behind an air gap (step-ca, Vault PKI, AD CS through ACME) -- validated on the
 * port the daemon already serves (TLS.md, "Certificates").
 *
 * One call is one whole issuance, blocking: the account (made on first use and
 * kept), the order, each name's challenge answered through the daemon's own TLS
 * listener (oc_tls_server_set_challenge), the finalization with a fresh key's
 * CSR, and the chain. certs.c runs it on its worker and renews with it.
 */
#ifndef OPENCHIME_ACME_H
#define OPENCHIME_ACME_H

#include <stddef.h>
#include <stdint.h>

#include "tls.h"

typedef struct {
    const char    *directory;        /* the CA's directory URL */
    const char    *names;            /* the DNS names, comma-separated */
    const char    *email;            /* contact, or NULL */
    oc_tls_server *tls;              /* answers the challenges */
    /* The account, if one is kept: its key (PEM) and URL. A new one is made when
     * the key is absent, and handed to store_account. */
    const char    *account_key_pem;
    const char    *account_url;
    void         (*store_account)(void *ctx, const char *key_pem, const char *url);
    void          *ctx;
    int            poll_ms;          /* between polls of a pending object; 0 = 2000 */
    /* The certificate this one replaces, by its ARI identifier (RFC 9773 §5),
     * or NULL: the CA links the two, and may favour a replacement it asked for. */
    const char    *replaces;
    const int     *stop;             /* nonzero: give up at the next poll (shutdown) */
} oc_acme_opts;

/* A certificate, as issued: the chain and its key in PEM, and its validity. */
typedef struct {
    char    *chain_pem;
    char    *key_pem;
    char    *names;                  /* comma-separated, as issued */
    uint64_t not_before_ms, not_after_ms;
} oc_cert_issued;

void oc_cert_issued_free(oc_cert_issued *c);

/* Obtain a certificate for `o->names`. 0 with `out` filled, or -1 with `err`
 * saying what failed (an ACME problem's type and detail where the CA gave one). */
int oc_acme_issue(const oc_acme_opts *o, oc_cert_issued *out, char *err, size_t errcap);

/* The TLS-ALPN-01 certificate for `name` and key authorization `keyauth`
 * (RFC 8737 §3): self-signed, naming `name`, carrying the critical
 * acmeIdentifier extension with SHA-256(keyauth). PEM chain and key, malloc'd.
 * Exposed for tests. Returns 0. */
int oc_acme_challenge_cert(const char *name, const char *keyauth, char **cert_pem, char **key_pem);

/* A CSR for `names` (comma-separated) over a new P-256 key: its DER into `*der`
 * and the key's PEM into `*key_pem`, both malloc'd. Returns 0. */
int oc_acme_csr(const char *names, uint8_t **der, size_t *der_len, char **key_pem);

/* ACME Renewal Information (RFC 9773): the CA's own word on when a certificate
 * should be renewed -- earlier than usual after an incident or a revocation.
 *
 * The certificate's identifier (§4.1): base64url of its authority key
 * identifier, a dot, base64url of its serial number's content octets -- of the
 * first certificate in `chain_pem`. 0, or -1 if it has no authority key
 * identifier or `cap` is too small. The raw form, for tests. */
#define OC_ACME_CERT_ID_MAX 200
int oc_acme_cert_id(const char *chain_pem, char *out, size_t cap);
int oc_acme_cert_id_raw(const uint8_t *aki, size_t aki_len, const uint8_t *serial, size_t serial_len,
                        char *out, size_t cap);

/* The window the CA suggests for renewing `chain_pem`'s certificate, from the
 * directory's renewalInfo (unauthenticated GET, §4.2), in ms since the epoch,
 * and how long to wait before asking again (its Retry-After; 0 if none). 0; or
 * -1 with `err` -- including where the directory offers no renewalInfo, as an
 * internal CA may not. */
int oc_acme_renewal_info(const char *directory, const char *chain_pem, uint64_t *start_ms, uint64_t *end_ms,
                         uint64_t *retry_after_ms, char *err, size_t errcap);

/* An RFC 3339 time -- "2025-01-02T04:00:00Z", with or without fractions and a
 * numeric offset -- in ms since the epoch. 0, or -1 if it is not one. */
int oc_rfc3339_ms(const char *s, uint64_t *out);

/* A certificate's validity window (the first in `pem`) in ms since the epoch.
 * Returns 0. */
int oc_cert_validity(const char *pem, size_t len, uint64_t *not_before_ms, uint64_t *not_after_ms);

#endif /* OPENCHIME_ACME_H */
