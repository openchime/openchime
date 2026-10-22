/* What a new password must be (AUTH.md §2, NIST SP 800-63B rev 4): 15 to 128
 * characters after NFKC normalization, not a common password, and not built
 * on the account's own name, the workspace's name or the product's. Nothing
 * else: no character classes, no expiry. Every password, new or presented at
 * sign-in, is normalized the same way before it is hashed, so the same
 * characters typed on any keyboard give the same hash. */
#ifndef OC_PWPOLICY_H
#define OC_PWPOLICY_H

#include <stddef.h>

#define OC_PW_MIN_CHARS 15
#define OC_PW_MAX_CHARS 128
/* Room for the normalized form of the longest password: NFKC may expand a
 * character to several, each up to four bytes. */
#define OC_PW_NORM_MAX  2048

/* `pw` as NFKC into `out` (NUL-terminated). 0, or -1 when `pw` is not valid
 * UTF-8, holds a control character, or does not fit. */
int oc_pw_normalize(const char *pw, char *out, size_t cap);

/* Why a password is refused: a stable kind a page and a test can name, and
 * the form's sentence for it (oc_pw_verdict_text). */
typedef enum {
    OC_PW_OK = 0,
    OC_PW_BAD_CHARS,   /* not valid UTF-8, a control character, or too big to normalize */
    OC_PW_TOO_SHORT,   /* under OC_PW_MIN_CHARS */
    OC_PW_TOO_LONG,    /* over OC_PW_MAX_CHARS */
    OC_PW_COMMON,      /* on the common-password list, or one character repeated */
    OC_PW_PERSONAL     /* built on the username, the workspace's name or the product's */
} oc_pw_verdict;

/* Whether `pw` may become a password for `username` in `workspace` (either may
 * be NULL): OC_PW_OK with its normalized form in `out`, or why not. */
oc_pw_verdict oc_pw_check(const char *pw, const char *username, const char *workspace,
                          char *out, size_t cap);
/* The form's sentence for a refusal; "" for OC_PW_OK. */
const char *oc_pw_verdict_text(oc_pw_verdict v);

/* The form's own words for the rule, shown beside the field. */
#define OC_PW_RULE_TEXT "At least 15 characters. Spaces and any characters are fine; a long phrase is easiest."

#endif
