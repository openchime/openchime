/*
 * Keeping the daemon's certificate CA-issued (TLS.md, "Certificates"): one
 * worker thread obtains a certificate -- by ACME (acme.h) or through central
 * (AUTH.md §8.9) -- presents it (oc_tls_server_use), keeps it (store_cert), and
 * renews it at two-thirds of its life. A failure is retried with a growing wait,
 * a minute to six hours, and until the first success the daemon presents what
 * it started with.
 */
#ifndef OPENCHIME_CERTS_H
#define OPENCHIME_CERTS_H

#include <stddef.h>
#include <stdint.h>

#include "acme.h"
#include "tls.h"

typedef enum { OC_CERTS_ACME, OC_CERTS_CENTRAL } oc_certs_source;

typedef struct {
    oc_certs_source source;
    oc_tls_server  *tls;
    /* ACME */
    const char     *directory, *names, *email;
    const char     *account_key_pem, *account_url;
    void          (*store_account)(void *ctx, const char *key_pem, const char *url);
    /* central: the enrollment origin, audience and key (AUTH.md §8.7) */
    const char     *central_url, *audience, *enroll_key_pem;
    /* The certificate already presented, from the database: renewed when due. */
    uint64_t        issued_ms, not_after_ms;
    void          (*store_cert)(void *ctx, const oc_cert_issued *c);
    void           *ctx;
    /* Tests: the first wait after a failure (0 = a minute), the poll interval
     * handed to ACME (0 = its default), and how often a due renewal is looked
     * for (0 = hourly). */
    int             retry_ms, poll_ms, check_ms;
} oc_certs_opts;

typedef struct oc_certs oc_certs;

oc_certs *oc_certs_start(const oc_certs_opts *o);
void      oc_certs_stop(oc_certs *c);

/* What the worker last did, for tests and the log: certificates obtained, the
 * last one's expiry, and the last failure ("" if none). */
void oc_certs_status(oc_certs *c, int *obtained, uint64_t *not_after_ms, char *err, size_t cap);

/* When a certificate issued at `issued_ms` for until `not_after_ms` is renewed:
 * two-thirds of the way through its life. */
uint64_t oc_certs_renew_at(uint64_t issued_ms, uint64_t not_after_ms);

/* One certificate through central (AUTH.md §8.9): a CSR for a new key, sent as
 * a signed machine request; the names are central's to say. 0 with `out`
 * filled; 1 if central is still working on it (`*retry_ms` how long to wait);
 * -1 with `err`, and `*final` set when central refused for good (a 4xx). */
int oc_central_issue(const char *central_url, const char *audience, const char *enroll_key_pem,
                     oc_cert_issued *out, int *retry_ms, int *final, char *err, size_t errcap);

#endif /* OPENCHIME_CERTS_H */
