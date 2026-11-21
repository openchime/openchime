/* The address report (AUTH.md §3.6). A connected self-hosted workspace asks
 * central to record the address it is reached at, so central can mail its
 * invitations and open it from a link: a signed machine request (ARCH-85's
 * scheme, push.h's oc_machine_http), POST /api/machine/address at the origin of
 * OPENCHIME_ENROLL_URL, with the body oc_enroll_address_body builds -- the
 * server's DNS name, or {} with none, when central proves the address the report
 * came from. Central records an address only once the server there signs a nonce
 * with the workspace's key (/enroll-proof).
 *
 * A worker thread reports in rounds: at start, then every repeat interval while
 * the daemon runs, so a server whose address changes is proved at the new one.
 * In a round, a 2xx ends the round; no answer, 422 (central could not check the
 * address), 429 or 5xx is tried again, the wait doubling from the first retry up
 * to the maximum, until the round's time is up; any other answer is a refusal,
 * and reporting stops until the next start. A round hook, when given, is called
 * after each round, on the worker; after a refusal the rounds go on for it alone,
 * with no request (main.c's check that central still lists an adopted name).
 */
#ifndef OPENCHIME_ADDRESS_REPORT_H
#define OPENCHIME_ADDRESS_REPORT_H

#include <stddef.h>

/* Production's timings: the first retry after 5 s, waits doubling to a minute
 * (central checks an address once a minute at most), a round of fifteen
 * minutes, and a round every two hours. */
#define OC_ADDRESS_FIRST_RETRY_SECS 5
#define OC_ADDRESS_RETRY_MAX_SECS   60
#define OC_ADDRESS_ROUND_SECS       900
#define OC_ADDRESS_REPEAT_SECS      7200

typedef struct {
    unsigned first_retry_ms;
    unsigned retry_max_ms;
    unsigned round_ms;
    unsigned repeat_ms;
} oc_address_report_timing;

typedef struct oc_address_report oc_address_report;

/* Start reporting `body`. `enroll_url` is OPENCHIME_ENROLL_URL (only its origin
 * is used); audience and privkey_pem are the enrollment identity; all three and
 * the body are copied. `timing` NULL is production's. `on_round` (NULL for none)
 * is called with `ctx` after each round. NULL on failure. */
oc_address_report *oc_address_report_start(const char *enroll_url, const char *audience,
                                           const char *privkey_pem, const char *body,
                                           const oc_address_report_timing *timing,
                                           void (*on_round)(void *ctx), void *ctx);

/* Stop: a request under way finishes, and a wait ends at once. A no-op for NULL. */
void oc_address_report_stop(oc_address_report *r);

/* ---- exposed for testing ---- */

/* What an answer means: `sent` 0 when no HTTP answer came back, else the status. */
typedef enum { OC_ADDRESS_RECORDED, OC_ADDRESS_REFUSED, OC_ADDRESS_RETRY } oc_address_next;
oc_address_next oc_address_report_disposition(int sent, int status);

/* The address central says it recorded: the `address` member of its answer.
 * 0 with it in `out`, or -1 when the answer carries none. */
int oc_address_report_recorded(const char *resp, char *out, size_t cap);

#endif /* OPENCHIME_ADDRESS_REPORT_H */
