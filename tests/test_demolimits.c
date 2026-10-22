/* A demo workspace's daily limits (daemon/demolimits.c): each person's and the
 * workspace's, a refusal counting nothing, a new UTC day starting afresh, and
 * the kinds that are the workspace's only. */
#include "demolimits.h"
#include "check.h"

#include <string.h>

int run_demolimits_tests(void) {
    printf("test_demolimits: per person and per workspace, a new day, workspace-only kinds\n");
    oc_dl *d = oc_dl_new();
    CHECK(d != NULL);
    if (!d) return failures;
    const uint64_t day = 20000ull * 86400000ull;   /* a UTC midnight */
    unsigned pu, pw;
    oc_dl_limits(OC_DL_SUMMARY, &pu, &pw);
    CHECK(pu == 5 && pw == 100);
    /* A person's own limit, and the next person unaffected. */
    for (unsigned i = 0; i < pu; i++) CHECK(oc_dl_take(d, OC_DL_SUMMARY, 7, day + i) == 1);
    CHECK(oc_dl_take(d, OC_DL_SUMMARY, 7, day + 10) == 0);
    CHECK(oc_dl_take(d, OC_DL_SUMMARY, 8, day + 10) == 1);
    /* A refusal carries a reason for the person (the error's text); its words are theirs to change. */
    CHECK(oc_dl_why(OC_DL_SUMMARY)[0] != '\0');
    /* Other kinds count apart. */
    CHECK(oc_dl_take(d, OC_DL_MESSAGE, 7, day + 11) == 1);
    /* The workspace's limit stops everyone, and a refusal counts nothing. */
    oc_dl_limits(OC_DL_MAIL, &pu, &pw);
    CHECK(pu == 0 && pw == 20);
    for (unsigned i = 0; i < pw; i++) CHECK(oc_dl_take(d, OC_DL_MAIL, 0, day + 20) == 1);
    CHECK(oc_dl_take(d, OC_DL_MAIL, 0, day + 21) == 0);
    CHECK(oc_dl_take(d, OC_DL_MAIL, 9, day + 21) == 0);
    /* The next UTC day starts with nothing counted. */
    CHECK(oc_dl_take(d, OC_DL_SUMMARY, 7, day + 86400000ull) == 1);
    CHECK(oc_dl_take(d, OC_DL_MAIL, 0, day + 86400000ull) == 1);
    /* Many people: each counted, up to the workspace's limit. */
    oc_dl_limits(OC_DL_UPLOAD, &pu, &pw);
    unsigned ok = 0;
    for (uint64_t u = 100; u < 100 + pw + 50; u++) ok += (unsigned)oc_dl_take(d, OC_DL_UPLOAD, u, day + 86400000ull + 5);
    CHECK(ok == pw);
    oc_dl_free(d);
    return failures;
}
