/* Shared assertion macro for the OpenChime test binary. Each test translation
 * unit includes this, accumulates into its own file-local `failures`, and
 * returns that count from its run_<suite>_tests() entry point; tests/main.c
 * sums them. A non-zero total fails `make test` and CI. */

#ifndef OC_TEST_CHECK_H
#define OC_TEST_CHECK_H

#include <stdio.h>

/* GCC and clang both spell it this way; a compiler without it loses only the
 * suppression, which is why this is a fallback rather than a hard error. */
#if defined(__GNUC__)
#define OC_UNUSED __attribute__((unused))
#else
#define OC_UNUSED
#endif

/* Each translation unit keeps its own counter, so a unit that includes this
 * header for the macro without ever running a CHECK has one that is never
 * touched. That is intended, not an oversight -- the alternative is a shared
 * symbol and a link order to reason about -- so it is marked as such rather
 * than left for -Wunused-variable to report in every such unit forever. */
static int failures OC_UNUSED = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
            failures++;                                                      \
        }                                                                    \
    } while (0)

#endif /* OC_TEST_CHECK_H */
