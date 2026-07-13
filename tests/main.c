/* Entry point for the single OpenChime test binary. Each suite lives in its own
 * translation unit under tests/, uses the shared CHECK macro (tests/check.h),
 * and exposes a run_<suite>_tests() returning its failure count. Built by
 * `make test`; a non-zero total exits non-zero and fails CI. In-process
 * integration suites (TLS, event loop) live here too; the black-box,
 * container-level end-to-end tests are separate (Scripts/test-integration.sh). */

#include <stdio.h>

int run_protocol_tests(void);
int run_framebuf_tests(void);
int run_migrate_tests(void);
int run_auth_tests(void);
int run_jwt_tests(void);
int run_ratelimit_tests(void);
int run_roles_tests(void);
int run_dbwriter_tests(void);
int run_tls_tests(void);
int run_netloop_tests(void);

int main(void) {
    int total = 0;
    total += run_protocol_tests();
    total += run_framebuf_tests();
    total += run_migrate_tests();
    total += run_auth_tests();
    total += run_jwt_tests();
    total += run_ratelimit_tests();
    total += run_roles_tests();
    total += run_dbwriter_tests();
    total += run_tls_tests();
    total += run_netloop_tests();

    if (total == 0) { printf("\nOK: all suites passed\n"); return 0; }
    printf("\nFAILED: %d check(s) across all suites\n", total);
    return 1;
}
