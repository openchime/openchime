/*
 * A local account's second step on the daemon's pages (AUTH.md §8.6): the page
 * whose password was right is answered with the step page, not a token, and
 * carries a ticket to it. The ticket stands for what the password page proved
 * -- whose account, which version of its password -- and what that page was
 * going to do: a sign-in's callback and challenge, a device code to approve, or
 * a password change's new key.
 *
 * The tickets, in the event loop's memory: bounded in all and per source, five
 * minutes and five wrong codes each, dropped by a restart. The loop's alone --
 * no locks. Only a hash of a ticket is kept, so this table is no list of them.
 */
#ifndef OPENCHIME_WEBSTEP_H
#define OPENCHIME_WEBSTEP_H

#include <stddef.h>
#include <stdint.h>

#define OC_STEP_TTL_MS     (5u * 60u * 1000u)
#define OC_STEP_TRIES      5
#define OC_STEP_MAX        256
#define OC_STEP_PER_SOURCE 8
#define OC_STEP_TICKET_LEN 43    /* base64url of 32 random bytes */

/* What a ticket stands for. `page` is the page it finishes (oc_page_kind). */
typedef struct {
    int      page;
    uint64_t uid, version;
    char     redirect_uri[512], nonce[48];   /* a sign-in's */
    char     user_code[16];                  /* a device approval's */
    int      pw;                             /* a password change's new key */
    uint32_t pw_iters;
    uint8_t  pw_salt[16], pw_hash[32];
} oc_step_ticket;

typedef struct oc_websteps oc_websteps;

oc_websteps *oc_websteps_new(void);
void         oc_websteps_free(oc_websteps *s);
/* A test's knob: how long a ticket lives. 0 restores OC_STEP_TTL_MS. */
void         oc_websteps_set_ttl(oc_websteps *s, uint64_t ms);

/* Keep `t` for `source` (its limiter key): the ticket, written to `out`. 0; -1
 * when the table is full; -2 when this source already holds its share. */
int oc_websteps_put(oc_websteps *s, const char *source, const oc_step_ticket *t, uint64_t now_ms,
                    char out[OC_STEP_TICKET_LEN + 1]);
/* What a live ticket stands for, copied. 1, or 0 if there is none. */
int oc_websteps_get(oc_websteps *s, const char *ticket, uint64_t now_ms, oc_step_ticket *out);
/* A wrong code against it: the tries it has left, 0 once it is gone. */
int oc_websteps_fail(oc_websteps *s, const char *ticket, uint64_t now_ms);
/* Spent: gone. */
void oc_websteps_drop(oc_websteps *s, const char *ticket);

#endif /* OPENCHIME_WEBSTEP_H */
