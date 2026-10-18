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

/* A limit on how fast code runs -- wall-clock time, or one timing against
 * another -- is asserted in the ordinary build, where it measures the code. Under
 * ThreadSanitizer, which slows code five to fifteen times and unevenly, it would
 * measure the sanitizer, so it is not: the code still runs there, for the races
 * the sanitizer is for. A timeout that must fire, checked with a wide margin,
 * is not a speed limit and stays a CHECK. */
#if defined(__SANITIZE_THREAD__)
#define OC_UNDER_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define OC_UNDER_TSAN 1
#endif
#endif
#ifndef OC_UNDER_TSAN
#define OC_UNDER_TSAN 0
#endif

#define CHECK_SPEED(cond)                                                    \
    do {                                                                     \
        if (!OC_UNDER_TSAN) CHECK(cond);                                     \
    } while (0)

/* How long to wait for work to be done -- frames encoded and decoded, say --
 * before calling it not done. Not a speed limit: what is checked is that it is
 * done. But the sanitizer slows the work as much as fifteen times, so under it
 * the wait is that much longer. */
#define WAIT_MS(ms) ((ms) * (OC_UNDER_TSAN ? 15 : 1))

#endif /* OC_TEST_CHECK_H */
