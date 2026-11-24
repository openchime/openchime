/*
 * Outgoing webhooks' delivery (docs/details/WEBHOOKS.md). The writer keeps the
 * queue (webhook_deliveries, migration 0092); the net thread takes what is due
 * from it and hands each delivery here, and this worker, on a thread of its
 * own, opens the webhook's secret under the factor key, signs the envelope as
 * docs/plans/APPS.md §5.3 says and POSTs it. A 2xx within 3 s completes it;
 * anything else is a failure, which the writer schedules again. The same
 * worker makes the endpoint check a webhook's creation waits on: a signed
 * url_verification whose challenge the endpoint must echo. The net loop never
 * waits on either.
 */
#ifndef OPENCHIME_WEBHOOKS_OUT_H
#define OPENCHIME_WEBHOOKS_OUT_H

#include <stddef.h>
#include <stdint.h>

#include "dbwriter.h"   /* oc_outwh_due, OC_OUTWH_SECRET_LEN */
#include "totp.h"       /* OC_FACTOR_KEY_LEN */

/* What a request got back: `status` 0 when no HTTP answer came, and then
 * `timed_out` when time ran out rather than the connection failing. */
typedef struct {
    int    status;
    int    timed_out;
    char  *body;        /* heap, NUL-terminated; may be NULL */
    size_t body_len;
} oc_owh_reply;

/* Send one POST: `headers` are extra header lines, each ending "\r\n". 0 with
 * `out` filled (any status), or -1 when nothing came back -- `out->timed_out`
 * says whether time ran out. */
typedef int (*oc_owh_sender)(void *ctx, const char *url, const char *headers, const char *body,
                             size_t len, int timeout_ms, oc_owh_reply *out);
/* Unix time in milliseconds. */
typedef uint64_t (*oc_owh_clock)(void *ctx);

typedef struct oc_webhooks_out oc_webhooks_out;

/* What the worker finished: a delivery (`delivery`, with `ok` or the retry
 * `reason` it failed with), or a creation's check (`check_tag`, `ok`). */
typedef struct {
    uint64_t delivery;
    uint64_t check_tag;
    int      ok;
    char     reason[24];
} oc_owh_result;

#define OC_OUTWH_TIMEOUT_MS 3000
/* Deliveries handed over and not yet finished, at most. */
#define OC_OUTWH_INFLIGHT_MAX OC_OUTWH_TAKE_MAX

/* Start the worker with the factor key (copied). `send` and `clock` NULL are
 * the real ones: HTTPS (daemon/https_client.h) and the system's clock. NULL on
 * failure. */
oc_webhooks_out *oc_webhooks_out_start(const uint8_t key[OC_FACTOR_KEY_LEN], oc_owh_sender send,
                                       oc_owh_clock clock, void *ctx);
/* Readable when a result is waiting. */
int  oc_webhooks_out_eventfd(oc_webhooks_out *o);
/* How many deliveries are handed over and not finished. */
int  oc_webhooks_out_pending(oc_webhooks_out *o);
/* Hand over a delivery taken from the queue (copied). Never blocks. */
void oc_webhooks_out_deliver(oc_webhooks_out *o, const oc_outwh_due *d);
/* Check an endpoint for a creation: `tag` comes back with the outcome. */
void oc_webhooks_out_check(oc_webhooks_out *o, uint64_t tag, const char *url,
                           const uint8_t secret[OC_OUTWH_SECRET_LEN]);
/* The next finished one: 1 and `r`, or 0. */
int  oc_webhooks_out_next(oc_webhooks_out *o, oc_owh_result *r);
/* Stop: a request under way finishes, the rest are dropped -- deliveries stay
 * queued in the store and are taken again. A no-op for NULL. */
void oc_webhooks_out_stop(oc_webhooks_out *o);

/* ---- exposed for testing ---- */

/* A secret as it is shown and used: its 40 lowercase hex digits. */
void oc_owh_secret_text(const uint8_t raw[OC_OUTWH_SECRET_LEN], char out[2 * OC_OUTWH_SECRET_LEN + 1]);
/* X-Slack-Signature: "v0=" and the hex HMAC-SHA-256, keyed by the secret's
 * text, of "v0:{ts}:{body}". 0, or -1. */
int  oc_owh_sign(const char *secret_text, uint64_t ts, const char *body, size_t len, char out[3 + 64 + 1]);
/* Whether `reply` echoes `challenge`: the body is the challenge, or JSON whose
 * "challenge" is. */
int  oc_owh_echoes(const char *body, size_t len, const char *challenge);

#endif /* OPENCHIME_WEBHOOKS_OUT_H */
