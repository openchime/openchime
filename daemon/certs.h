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
    /* What needs a person (REQ-263): `key` raised with `message`, or cleared
     * (NULL) once it has stopped -- OC_CERTS_ALERT_*. */
    void          (*alert)(void *ctx, const char *key, const char *message);
    void           *ctx;
    /* Tests: the first wait after a failure (0 = a minute) and the last (0 = a
     * day), the poll interval handed to ACME (0 = its default), how often a due
     * renewal is looked for (0 = hourly), and how often ARI is asked (0 = as its
     * Retry-After says, within an hour to twelve). */
    int             retry_ms, retry_max_ms, poll_ms, check_ms, ari_check_ms;
    /* Tests: how close to expiry a certificate not yet renewed is raised (0 =
     * seven days). */
    int64_t         expiry_warn_ms;
    /* Tests: the worker's clock, in ms since the epoch (NULL = the system's),
     * given `ctx`. A wait for a renewal is at most `check_ms`, so a clock the
     * test moves is seen within one check. */
    uint64_t      (*now_ms)(void *ctx);
} oc_certs_opts;

/* The worker's alerts: a certificate not obtained or renewed (cleared by the next
 * one obtained); the CA asking for one to be replaced now, which is how a
 * revocation reaches the daemon (cleared by its replacement); and one within a
 * week of expiry and still not renewed (cleared likewise). */
#define OC_CERTS_ALERT_OBTAIN   "tls.obtain"
#define OC_CERTS_ALERT_REPLACE  "tls.replace"
#define OC_CERTS_ALERT_EXPIRING "tls.expiring"

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

/* Whether the comma-separated `names` a certificate was issued for include `name`,
 * without regard to case. A box bound to central that has moved to a new address
 * re-issues at boot when its kept certificate does not name it. */
int oc_certs_names_include(const char *names, const char *name);

/* The name a self-hosted daemon takes as its address from a certificate central
 * issued (TLS.md, the `central` source): the first of its comma-separated
 * `names`, lower-cased -- central lists the workspace's own name first. 0 with
 * it in `out`, or -1 for none (NULL, empty, or an `out` too small). */
int oc_certs_adopted_name(const char *names, char *out, size_t cap);

/* The names central says this workspace may have (POST /api/machine/tls/names,
 * a signed machine request, AUTH.md §8.9). 0 when central answered, with the
 * status in *status and, on a 200, the comma-separated names in `names` ("" if
 * it gave none); -1 with `err` when no answer came back. */
int oc_central_names(const char *central_url, const char *audience, const char *enroll_key_pem,
                     char *names, size_t cap, int *status, char *err, size_t errcap);

/* What central's answer to the names request means for an adopted name: a 200
 * listing it keeps it; a 200 without it, or a 404, drops it; anything else (a
 * 503, another status, no answer: `status` 0) says nothing about it. */
typedef enum { OC_NAME_KEEP, OC_NAME_DROP, OC_NAME_UNKNOWN } oc_name_listed;
oc_name_listed oc_certs_name_listed(int status, const char *names, const char *name);

/* One certificate through central (AUTH.md §8.9): a CSR for a new key, sent as
 * a signed machine request; the names are central's to say. 0 with `out`
 * filled; 1 if central is still working on it (`*retry_ms` how long to wait);
 * -1 with `err`, and `*final` set when central refused for good (a 4xx). */
int oc_central_issue(const char *central_url, const char *audience, const char *enroll_key_pem,
                     oc_cert_issued *out, int *retry_ms, int *final, char *err, size_t errcap);

#endif /* OPENCHIME_CERTS_H */
