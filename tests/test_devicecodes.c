/* The device-code table (daemon/devicecodes.c, AUTH.md §8.11) on its own: a
 * request's life -- pending, slow down, approved and collected once, denied,
 * gone -- the codes' shapes, a typed code's forms, the per-source cap and the
 * expiry. The loop's use of it is itest_netloop's. */

#include "check.h"
#include "devicecodes.h"

#include <stdlib.h>
#include <string.h>

static const char CH[] = "JBbiqONGWPaAmwXk_8bT6UnlPfrn65D32eZlJS-zGG0";

int run_devicecodes_tests(void) {
    printf("test_devicecodes: pending, slow down, approve once, deny, gone, typed forms, per-source cap, expiry\n");
    oc_devcodes *d = oc_devcodes_new();
    CHECK(d != NULL);
    if (!d) return failures;
    char dc[OC_DEVICE_CODE_LEN + 1], uc[OC_USER_CODE_LEN + 1], *tok = NULL;
    unsigned iv = 0;
    uint64_t t = 1000000;
    CHECK(oc_devcodes_begin(d, "10.0.0.1", "10.0.0.1", CH, t, dc, uc) == 0);
    CHECK(strlen(dc) == OC_DEVICE_CODE_LEN && strlen(uc) == 9 && uc[4] == '-');
    /* Pending; sooner than the interval is slow, and the interval grows. */
    CHECK(oc_devcodes_poll(d, dc, t, &tok, &iv) == OC_DEV_PENDING && iv == 5);
    CHECK(oc_devcodes_poll(d, dc, t + 1000, &tok, &iv) == OC_DEV_SLOW && iv == 10);
    CHECK(oc_devcodes_poll(d, dc, t + 12000, &tok, &iv) == OC_DEV_PENDING && iv == 10);
    /* The code a person sees does not collect anything; the secret does. */
    CHECK(oc_devcodes_poll(d, uc, t + 30000, &tok, &iv) == OC_DEV_GONE && !tok);
    /* Typed in lower case, without the dash, or with spaces: the same code. */
    oc_dev_info in;
    char typed[16];
    snprintf(typed, sizeof typed, "%c%c%c%c %c%c%c%c", uc[0] + 32, uc[1] + 32, uc[2] + 32, uc[3] + 32,
             uc[5] + 32, uc[6] + 32, uc[7] + 32, uc[8] + 32);
    CHECK(oc_devcodes_find(d, typed, t + 30000, &in) == 1 && strcmp(in.user_code, uc) == 0 &&
          strcmp(in.challenge, CH) == 0 && strcmp(in.addr, "10.0.0.1") == 0);
    CHECK(oc_devcodes_find(d, "AAAA-AAAA", t, &in) == 0);                   /* vowels are never codes */
    /* Approved: collected once, then gone. */
    CHECK(oc_devcodes_approve(d, uc, "the.token", t + 31000) == 0);
    CHECK(oc_devcodes_approve(d, uc, "again", t + 31000) == -1);
    CHECK(oc_devcodes_poll(d, dc, t + 45000, &tok, &iv) == OC_DEV_TOKEN && tok && strcmp(tok, "the.token") == 0);
    free(tok); tok = NULL;
    CHECK(oc_devcodes_poll(d, dc, t + 60000, &tok, &iv) == OC_DEV_GONE);
    CHECK(oc_devcodes_find(d, uc, t + 60000, &in) == 0);
    /* Denied: said once, then gone. */
    CHECK(oc_devcodes_begin(d, "10.0.0.1", "10.0.0.1", CH, t, dc, uc) == 0);
    CHECK(oc_devcodes_deny(d, uc, t) == 0);
    CHECK(oc_devcodes_poll(d, dc, t + 10000, &tok, &iv) == OC_DEV_DENIED);
    CHECK(oc_devcodes_poll(d, dc, t + 20000, &tok, &iv) == OC_DEV_GONE);
    /* Five waiting from one source, not six; another source is its own. */
    for (int i = 0; i < OC_DEVICE_PER_SOURCE; i++) CHECK(oc_devcodes_begin(d, "10.0.0.9", "10.0.0.9", CH, t, dc, uc) == 0);
    CHECK(oc_devcodes_begin(d, "10.0.0.9", "10.0.0.9", CH, t, dc, uc) == -2);
    CHECK(oc_devcodes_begin(d, "10.0.0.10", "10.0.0.10", CH, t, dc, uc) == 0);
    /* Ten minutes, then gone -- pending, and approved but not collected. */
    CHECK(oc_devcodes_approve(d, uc, "late", t + 1000) == 0);
    CHECK(oc_devcodes_poll(d, dc, t + OC_DEVICE_TTL_MS, &tok, &iv) == OC_DEV_GONE && !tok);
    CHECK(oc_devcodes_begin(d, "10.0.0.9", "10.0.0.9", CH, t + OC_DEVICE_TTL_MS, dc, uc) == 0);   /* room again */
    char norm[OC_USER_CODE_LEN + 1];
    CHECK(oc_user_code_normalise("bcdf-ghjk", norm) == 0 && strcmp(norm, "BCDF-GHJK") == 0);
    CHECK(oc_user_code_normalise("BCDFGHJ", norm) == -1 && oc_user_code_normalise("BCDFGHJKL", norm) == -1);
    oc_devcodes_free(d);
    return failures;
}
