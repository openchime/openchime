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

/* A certificate's validity window (the first in `pem`) in ms since the epoch.
 * Returns 0. */
int oc_cert_validity(const char *pem, size_t len, uint64_t *not_before_ms, uint64_t *not_after_ms);

#endif /* OPENCHIME_ACME_H */
