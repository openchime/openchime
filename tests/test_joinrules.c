/* OIDC join rules (AUTH.md §8.4): the rule table, default deny, and a parser that
 * refuses what it does not understand. */

#include "check.h"
#include "../daemon/joinrules.h"

#include <stdio.h>
#include <string.h>

int run_joinrules_tests(void) {
    printf("test_joinrules: owner/tenant/domain rules, verified-address requirement, "
           "default deny, strict parse\n");
    char err[128];

    /* No rules at all: nobody new joins. */
    {
        oc_join_rules *r = oc_join_rules_parse(NULL, err, sizeof err);
        CHECK(r != NULL);
        CHECK(oc_join_rules_eval(r, "google", "acme.example", "a@acme.example", 1) == OC_JOIN_DENY);
        oc_join_rules_free(r);
        r = oc_join_rules_parse("  ", err, sizeof err);
        CHECK(r != NULL);
        CHECK(oc_join_rules_eval(r, "google", "acme.example", "a@acme.example", 1) == OC_JOIN_DENY);
        oc_join_rules_free(r);
        CHECK(oc_join_rules_eval(NULL, "google", "acme.example", "a@acme.example", 1) == OC_JOIN_DENY);
    }

    oc_join_rules *r = oc_join_rules_parse(
        "owner:Dana@Acme.example, tenant:google:acme.example,"
        "tenant:microsoft:72F988BF-86F1-41AF-91AB-2D7CD011DB47 ,domain:partner.example,",
        err, sizeof err);
    CHECK(r != NULL);
    if (!r) return failures;

    static const struct {
        const char *idp, *tenant, *email; int verified; oc_join_verdict want;
    } T[] = {
        /* the owner rule: that verified address, in any letter case */
        { "google", "",  "dana@acme.example", 1, OC_JOIN_OWNER },
        { "google", "",  "DANA@ACME.EXAMPLE", 1, OC_JOIN_OWNER },
        /* ...and never an unverified one — it is whatever its holder typed */
        { "google", "",  "dana@acme.example", 0, OC_JOIN_DENY },
        /* the owner rule wins over a member rule that also matches */
        { "google", "acme.example", "dana@acme.example", 1, OC_JOIN_OWNER },
        /* tenant rules need no address at all */
        { "google", "acme.example", "", 0, OC_JOIN_MEMBER },
        { "google", "ACME.example", NULL, 0, OC_JOIN_MEMBER },
        { "microsoft", "72f988bf-86f1-41af-91ab-2d7cd011db47", "x@unverified.example", 0, OC_JOIN_MEMBER },
        /* a tenant is a provider's: the same string under another provider is not it */
        { "microsoft", "acme.example", "", 0, OC_JOIN_DENY },
        { "", "acme.example", "", 0, OC_JOIN_DENY },
        { "google", "", "", 0, OC_JOIN_DENY },
        { "google", "other.example", "", 0, OC_JOIN_DENY },
        /* domain rules: a verified address at exactly that domain */
        { "google", "", "pat@partner.example", 1, OC_JOIN_MEMBER },
        { "google", "", "pat@partner.example", 0, OC_JOIN_DENY },
        { "google", "", "pat@sub.partner.example", 1, OC_JOIN_DENY },
        { "google", "", "pat@notpartner.example", 1, OC_JOIN_DENY },
        { "google", "", "partner.example@evil.example", 1, OC_JOIN_DENY },
        /* the domain is what follows the LAST '@' */
        { "google", "", "\"a@partner.example\"@evil.example", 1, OC_JOIN_DENY },
        /* a personal account nobody named */
        { "google", "", "someone@gmail.example", 1, OC_JOIN_DENY },
        { NULL, NULL, NULL, 1, OC_JOIN_DENY },
    };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) {
        oc_join_verdict got = oc_join_rules_eval(r, T[i].idp, T[i].tenant, T[i].email, T[i].verified);
        if (got != T[i].want) printf("  row %zu: got %d want %d\n", i, (int)got, (int)T[i].want);
        CHECK(got == T[i].want);
    }
    oc_join_rules_free(r);

    /* A rule the parser does not understand stops the boot, with the rule named. */
    static const char *const BAD[] = {
        "everyone", "owner:", "owner:not-an-address", "owner:a@b@c", "owner:a b@c.example",
        "domain:", "domain:nodot", "domain:.example", "domain:a_b.example", "domain:*.example",
        "tenant:google", "tenant:google:", "tenant::acme.example",
        "domain:ok.example,alow:typo.example",
    };
    for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
        err[0] = '\0';
        oc_join_rules *b = oc_join_rules_parse(BAD[i], err, sizeof err);
        if (b) printf("  accepted: %s\n", BAD[i]);
        CHECK(b == NULL);
        CHECK(err[0] != '\0');
        oc_join_rules_free(b);
    }
    return failures;
}
