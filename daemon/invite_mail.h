/* Invitation mail report (ARCH-85's third outbound channel, REQ-280's carve-out).
 * With OPENCHIME_INVITE_MAIL=on and an active enrollment, each committed invite
 * bound to an address is reported to central, which sends the one fixed
 * invitation message for a workspace whose name and address it holds and keeps
 * nothing. The report is {inviteId, email, expiresAt}, POSTed to
 * /api/machine/invite/notify at the origin of OPENCHIME_ENROLL_URL and signed with
 * the enrollment key exactly as a push batch is. A worker thread sends it, off
 * the net loop; a report that fails never fails the invite. A 2xx is done, a 4xx
 * is final, and a 5xx or no answer is tried again a few times, with backoff and
 * the same inviteId, so central can tell a retry from a second invitation.
 */
#ifndef OPENCHIME_INVITE_MAIL_H
#define OPENCHIME_INVITE_MAIL_H

#include <stddef.h>
#include <stdint.h>

typedef struct oc_invite_mail oc_invite_mail;

/* Start the emitter. `enroll_url` is OPENCHIME_ENROLL_URL (only its origin is
 * used); audience + privkey_pem are the enrollment identity (copied). NULL on
 * failure. */
oc_invite_mail *oc_invite_mail_start(const char *enroll_url, const char *audience,
                                     const char *privkey_pem);

/* The same, waiting `backoff_ms` before the first retry (doubling after), for a
 * test that cannot wait seconds. */
oc_invite_mail *oc_invite_mail_start_backoff(const char *enroll_url, const char *audience,
                                             const char *privkey_pem, unsigned backoff_ms);

/* Queue one report. Fire and forget; never blocks the caller (the net loop). A
 * no-op when m is NULL or an argument is missing. `invite_id` is 32 lowercase
 * hex digits; `expires_at` is unix seconds. */
void oc_invite_mail_report(oc_invite_mail *m, const char *invite_id, const char *email,
                           uint64_t expires_at);

/* Stop: a report being sent finishes, and any retries still waiting are
 * abandoned. A no-op for NULL. */
void oc_invite_mail_stop(oc_invite_mail *m);

/* ---- exposed for testing ---- */

/* The report body, exactly {"inviteId":"..","email":"..","expiresAt":N}, the
 * address escaped as JSON requires. 0, or -1 when it does not fit. */
int oc_invite_mail_build_body(const char *invite_id, const char *email, uint64_t expires_at,
                              char *out, size_t cap);

/* What a reply means: `sent` 0 when no HTTP answer came back, else the status. */
typedef enum { OC_INVITE_MAIL_DONE, OC_INVITE_MAIL_FINAL, OC_INVITE_MAIL_RETRY } oc_invite_mail_next;
oc_invite_mail_next oc_invite_mail_disposition(int sent, int status);

/* How many attempts one report gets, the first included. */
#define OC_INVITE_MAIL_ATTEMPTS 5

#endif /* OPENCHIME_INVITE_MAIL_H */
