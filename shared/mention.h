/*
 * @mention scanning (REQ-221, ARCH-89).
 *
 * Deliberately in shared/ and linked by BOTH the daemon and the client core.
 * "What counts as a mention" decides two different things in two different
 * processes — whether the server notifies you, and whether your client
 * highlights the text — and if those two ever disagree the product is broken in
 * a way nobody can see from either side alone. One implementation makes the
 * disagreement impossible rather than unlikely.
 *
 * This is pure text scanning: it finds candidate mentions and their byte spans.
 * Resolving a name to a user id is the caller's job, because only the caller
 * knows the roster — the daemon queries it, a client looks in its model.
 */

#ifndef OC_MENTION_H
#define OC_MENTION_H

#include <stddef.h>
#include <stdint.h>

enum {
    OC_MENTION_USER = 0,    /* @someone — resolve `name` against the roster */
    OC_MENTION_HERE,        /* @here     — members currently online */
    OC_MENTION_CHANNEL,     /* @channel  — every member */
    OC_MENTION_EVERYONE     /* @everyone — every member */
};

#define OC_MENTION_NAME_MAX 64
/* Per message. Generous for real use; a bound so a pathological body cannot
 * make the daemon do unbounded work per send. */
#define OC_MENTION_MAX 32

typedef struct {
    size_t  start;                      /* byte offset of the '@' in the body */
    size_t  len;                        /* bytes of the whole mention, '@' included */
    uint8_t kind;                       /* OC_MENTION_* */
    char    name[OC_MENTION_NAME_MAX];  /* text after '@', as written */
} oc_mention;

/* Scan `body` (`len` bytes, not necessarily NUL-terminated) and write up to
 * `max` mentions to `out`. Returns how many were found — which may exceed
 * `max`, so a caller that cares can tell it was truncated.
 *
 * The rules, stated once so both callers inherit them:
 *   - '@' counts only at a word boundary: start of input, or preceded by a byte
 *     that is not a name character. "e@mail" is an address, not a mention.
 *   - A name is [A-Za-z0-9._-], at least one byte, up to OC_MENTION_NAME_MAX-1.
 *   - Trailing '.', '-' and '_' are excluded, so "@alice." mentions alice and
 *     ends the sentence.
 *   - @here / @channel / @everyone are reserved, matched case-insensitively,
 *     and never resolve to a user even if somebody is called that.
 */
size_t oc_mention_scan(const char *body, size_t len, oc_mention *out, size_t max);

/* Does `body` mention `name` (case-insensitive), or carry any broadcast
 * audience? A convenience for the common "is this for me" question; `name` may
 * be NULL to ask only about broadcasts. */
int oc_mention_targets(const char *body, size_t len, const char *name);

#endif /* OC_MENTION_H */
