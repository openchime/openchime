/*
 * Keeping the daemon's certificate CA-issued (TLS.md, "Certificates"): one
 * worker thread obtains a certificate -- by ACME (acme.h) or through central
 * (AUTH.md §8.9) -- presents it (oc_tls_server_use), keeps it (store_cert), and
 * renews it at a random moment in a window: the CA's, where ACME Renewal
 * Information (RFC 9773) gives one -- which is how an early renewal after an
 * incident reaches the daemon -- and otherwise its own, from 60% to two-thirds
 * of the life. A failure is retried after a minute, ten, a hundred, then daily
 * (Let's Encrypt's integration guide); until the first success the daemon
 * presents what it started with.
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
    /* The certificate already presented, from the database: renewed when due,
     * and asked about by ARI (its chain, for its identifier), or NULL. */
    uint64_t        issued_ms, not_after_ms;
    const char     *chain_pem;
    void          (*store_cert)(void *ctx, const oc_cert_issued *c);
    void           *ctx;
    /* Tests: the first wait after a failure (0 = a minute) and the last (0 = a
     * day), the poll interval handed to ACME (0 = its default), how often a due
     * renewal is looked for (0 = hourly), and how often ARI is asked (0 = as its
     * Retry-After says, within an hour to twelve). */
    int             retry_ms, retry_max_ms, poll_ms, check_ms, ari_check_ms;
} oc_certs_opts;

typedef struct oc_certs oc_certs;

oc_certs *oc_certs_start(const oc_certs_opts *o);
void      oc_certs_stop(oc_certs *c);

/* What the worker last did, for tests and the log: certificates obtained, the
 * last one's expiry, and the last failure ("" if none). */
void oc_certs_status(oc_certs *c, int *obtained, uint64_t *not_after_ms, char *err, size_t cap);

/* When a certificate is renewed, for `rnd` in [0, 1): the daemon's own window,
 * 60% to two-thirds of the way through its life (a third left, as the CA asks);
 * or a window the CA gave. Chosen once per certificate, so daemons issued
 * together renew apart. */
uint64_t oc_certs_renew_pick(uint64_t issued_ms, uint64_t not_after_ms, double rnd);
uint64_t oc_certs_window_pick(uint64_t start_ms, uint64_t end_ms, double rnd);

/* The wait before retry `n` (0 the first): `base`, ten times it, a hundred
 * times it, then `max` -- a minute, ten, a hundred, a day. */
uint64_t oc_certs_retry_ms(int n, uint64_t base, uint64_t max);

/* One certificate through central (AUTH.md §8.9): a CSR for a new key, sent as
 * a signed machine request; the names are central's to say. 0 with `out`
 * filled; 1 if central is still working on it (`*retry_ms` how long to wait);
 * -1 with `err`, and `*final` set when central refused for good (a 4xx). */
int oc_central_issue(const char *central_url, const char *audience, const char *enroll_key_pem,
                     oc_cert_issued *out, int *retry_ms, int *final, char *err, size_t errcap);

#endif /* OPENCHIME_CERTS_H */
