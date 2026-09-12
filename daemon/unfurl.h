/* Link-unfurl fetch worker (REQ-222, ARCH-105). The net thread hands it the
 * URLs of a just-committed message; it fetches each one off the hot path,
 * extracts a title + description from the HTML, and submits the result back to
 * the DB writer as an OC_JOB_UNFURL_STORE — the same worker-owns-the-outbound
 * shape as the push emitter (ARCH-85). Fire-and-forget throughout: a fetch
 * that fails, times out, or is refused leaves nothing behind.
 *
 * The daemon dialling a USER-SUPPLIED destination is what is new here — no
 * other outbound client takes an attacker-chosen address — so every fetch
 * passes an SSRF gate first: the host is resolved and the whole fetch is
 * refused if ANY resolved address is loopback, private, link-local, multicast
 * or otherwise reserved; a redirect re-enters the same gate; responses are
 * capped in bytes, in redirects, and in time. HTTPS is CA-verified with
 * hostname checking (ARCH-10's daemon-as-client rule). */

#ifndef OPENCHIME_UNFURL_H
#define OPENCHIME_UNFURL_H

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#include "dbwriter.h"

/* At most this many URLs of a message are unfurled (ARCH-105). */
#define OC_UNFURL_MAX_URLS 3

typedef struct oc_unfurler oc_unfurler;

/* Start the worker. `dbw` receives the completed fetches; `ca_bundle` NULL uses
 * the system store; `allow_private` disables the SSRF gate and exists ONLY so a
 * test can fetch from a loopback fixture (OPENCHIME_UNFURL_ALLOW_PRIVATE) —
 * never set it in a deployment. Returns NULL on failure. */
oc_unfurler *oc_unfurler_start(oc_dbwriter *dbw, const char *ca_bundle,
                               int allow_private);

/* Queue one URL of one message for fetching. Non-blocking; drops silently when
 * the queue is full (an unfurl is best-effort, a message is not). `url` need
 * not be NUL-terminated. No-op if u is NULL. */
void oc_unfurler_fetch(oc_unfurler *u, uint64_t channel_id, uint64_t message_id,
                       const char *url, size_t url_len);

void oc_unfurler_stop(oc_unfurler *u);

/* ---- exposed for testing ---- */

/* The SSRF gate's per-address verdict: 1 when `sa` is publicly routable, 0 for
 * loopback / private / link-local / CGNAT / multicast / reserved / documentation
 * ranges, in both families (a v4-mapped v6 address is judged as its v4). */
int oc_unfurl_addr_public(const struct sockaddr *sa);

/* Scan a fetched HTML buffer for og:title / og:description, falling back to
 * <title> and <meta name="description">. Decodes the basic entities, collapses
 * whitespace, and truncates on a UTF-8 boundary. Returns 0 when a non-empty
 * title was found (descr may still be empty), -1 otherwise. */
int oc_unfurl_extract_html(const char *html, size_t len,
                           char *title, size_t title_cap,
                           char *descr, size_t descr_cap);

/* 1 when the extracted title/descr reads as a BLOCK PAGE — a bot wall's
 * access-denied, a verification challenge, a soft not-found — served with a
 * 200 the status check cannot catch. A preview of the refusal is worse than
 * no preview, so these fetches produce nothing. A false positive costs only
 * a missing card, which is this feature's normal outcome (ARCH-105). */
int oc_unfurl_blocked(const char *title, const char *descr);

#endif /* OPENCHIME_UNFURL_H */
