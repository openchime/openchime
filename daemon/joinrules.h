/*
 * Who may join through an OIDC source (AUTH.md §8.4): OPENCHIME_OIDC_ALLOW, a
 * comma-separated list of rules, default deny, consulted only for an identity
 * the workspace has not seen.
 *
 *   owner:<email>                  that verified address, created as owner
 *   tenant:google:<hosted domain>  anyone the provider places in that
 *   tenant:microsoft:<tenant id>   organization, as member
 *   domain:<domain>                a verified address at that domain, as member
 *
 * Pure: no database, no clock, no allocation after parse. The caller decides
 * what a verdict does.
 */

#ifndef OPENCHIME_JOINRULES_H
#define OPENCHIME_JOINRULES_H

#include <stddef.h>

typedef enum {
    OC_JOIN_DENY   = 0,
    OC_JOIN_MEMBER = 1,
    OC_JOIN_OWNER  = 2
} oc_join_verdict;

typedef struct oc_join_rules oc_join_rules;

/* Parse `spec` (NULL or "" is the empty list: deny everyone). Returns NULL and
 * writes a reason to `err` on a rule it does not understand — a typo in who may
 * join must stop the boot, not quietly admit nobody or somebody else. */
oc_join_rules *oc_join_rules_parse(const char *spec, char *err, size_t errcap);
void oc_join_rules_free(oc_join_rules *r);

/* The best verdict any rule gives this identity. `email` counts only when
 * `email_verified` is set: an unverified address is whatever its holder typed.
 * Any string may be NULL or "". */
oc_join_verdict oc_join_rules_eval(const oc_join_rules *r, const char *idp,
                                   const char *tenant, const char *email,
                                   int email_verified);

#endif /* OPENCHIME_JOINRULES_H */
