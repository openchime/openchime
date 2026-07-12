/* Shared assertion macro for the OpenChime test binary. Each test translation
 * unit includes this, accumulates into its own file-local `failures`, and
 * returns that count from its run_<suite>_tests() entry point; tests/main.c
 * sums them. A non-zero total fails `make test` and CI. */

#ifndef OC_TEST_CHECK_H
#define OC_TEST_CHECK_H

#include <stdio.h>

static int failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
            failures++;                                                      \
        }                                                                    \
    } while (0)

#endif /* OC_TEST_CHECK_H */
