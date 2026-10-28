/* Shared assertion macro for the OpenChime test binary. Each test translation
 * unit includes this, accumulates into its own file-local `failures`, and
 * returns that count from its run_<suite>_tests() entry point; tests/main.c
 * sums them. A non-zero total fails `make test` and CI. */

#ifndef OC_TEST_CHECK_H
#define OC_TEST_CHECK_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

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

/* A verdict that races the wall clock -- work that must land inside a short
 * real-time window, or a count that depends on where a window's edge falls --
 * can fail on a loaded machine with nothing wrong. It is asserted unless
 * OC_TEST_TIMING is 0, which the build pipeline sets (make test-ci) so that a
 * busy runner cannot fail a build; `make test` asserts it. The code around it
 * still runs everywhere, under ThreadSanitizer too. A speed limit is such a
 * verdict as well. Each one is to be made deterministic -- an injected clock,
 * an explicit sweep, a wait on the outcome -- and become a CHECK again. */
static inline int oc_test_timing(void) {
    const char *v = getenv("OC_TEST_TIMING");
    return !(v && v[0] == '0' && v[1] == '\0');
}

#define CHECK_TIMING(cond)                                                   \
    do {                                                                     \
        if (oc_test_timing()) CHECK(cond);                                   \
    } while (0)

#define CHECK_SPEED(cond)                                                    \
    do {                                                                     \
        if (!OC_UNDER_TSAN) CHECK_TIMING(cond);                              \
    } while (0)

/* How long to wait for work to be done -- frames encoded and decoded, say --
 * before calling it not done. Not a speed limit: what is checked is that it is
 * done. But the sanitizer slows the work as much as fifteen times, so under it
 * the wait is that much longer. */
#define WAIT_MS(ms) ((ms) * (OC_UNDER_TSAN ? 15 : 1))

/* ---- Waiting: deadlines on the monotonic clock, never counts or sleeps -------
 *
 * A test waits for an outcome it can observe -- a callback's counter, a model's
 * field, a stat -- and gives up at a deadline on CLOCK_MONOTONIC. It never
 * sleeps a fixed time and then asserts (the work may simply not be done yet on
 * a loaded machine, or the assertion passes for a reason that is not the one
 * meant), and never counts iterations (a loop of 500 x 10 ms is not 5 s when
 * each turn does work). This is the one place in tests/ that sleeps
 * (scripts/check/tests_lint.sh holds every other file to it). */

/* Milliseconds on CLOCK_MONOTONIC: for deadlines and for measuring, never for
 * a date. */
static inline uint64_t oc_test_mono_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}

/* A deadline `ms` from now, stretched under the sanitizer as WAIT_MS is. */
static inline uint64_t oc_test_deadline(uint64_t ms) { return oc_test_mono_ms() + WAIT_MS(ms); }

/* One pause in a poll toward `deadline`: 0 once it has passed, else sleeps a
 * moment (at most a few milliseconds, never past the deadline) and returns 1. */
static inline int oc_test_pause(uint64_t deadline) {
    uint64_t now = oc_test_mono_ms();
    if (now >= deadline) return 0;
    uint64_t step = deadline - now < 5 ? deadline - now : 5;
    struct timespec ts = { 0, (long)(step * 1000000u) };
    nanosleep(&ts, NULL);
    return 1;
}

/* Time itself passing, where the behaviour under test is about elapsed time
 * (a timestamp that must move on, a window that must close) and there is no
 * event to wait for. Never a stand-in for waiting on work: that is WAIT_UNTIL. */
static inline void oc_test_let_time_pass(uint64_t ms) {
    uint64_t end = oc_test_mono_ms() + ms;
    while (oc_test_pause(end)) {}
}

/* Poll `cond` until it holds or WAIT_MS(ms) pass; the value is whether it held.
 * `cond` is evaluated at least once and once more after the deadline, so a
 * slow last turn is not mistaken for a timeout. */
#define WAIT_UNTIL(cond, ms)                                                 \
    __extension__({                                                          \
        uint64_t oc_wu_end_ = oc_test_deadline((uint64_t)(ms));              \
        int oc_wu_ok_;                                                       \
        while (!(oc_wu_ok_ = !!(cond)) && oc_test_pause(oc_wu_end_)) {}      \
        if (!oc_wu_ok_) oc_wu_ok_ = !!(cond);                                \
        oc_wu_ok_;                                                           \
    })

/* The same, running `step` before each look -- ticking a client, pumping a
 * relay -- for a wait whose outcome only appears when something is driven. */
#define WAIT_UNTIL_DO(step, cond, ms)                                        \
    __extension__({                                                          \
        uint64_t oc_wu_end_ = oc_test_deadline((uint64_t)(ms));              \
        int oc_wu_ok_;                                                       \
        do { step; } while (!(oc_wu_ok_ = !!(cond)) && oc_test_pause(oc_wu_end_)); \
        oc_wu_ok_;                                                           \
    })

/* ---- Skips: counted, printed, and failures in CI -------------------------------
 *
 * A check that cannot run here -- a tool not installed, IPv6 not configured --
 * says so on a SKIP line and is counted (tests/main.c prints the total). Where
 * CI is set, as GitHub Actions sets it, the runner is supposed to have
 * everything, so a skip there is a failure: a check that silently stops running
 * is a check that passes while what it covered regresses. */
extern int oc_test_skips;   /* tests/main.c */

static inline int oc_test_in_ci(void) {
    const char *ci = getenv("CI");
    return ci && ci[0] && ci[0] != '0';
}

#define SKIP(why)                                                            \
    do {                                                                     \
        oc_test_skips++;                                                     \
        printf("  SKIP %s:%d  %s\n", __FILE__, __LINE__, (why));             \
        if (oc_test_in_ci()) {                                               \
            printf("  FAIL %s:%d  skipped in CI: %s\n", __FILE__, __LINE__, (why)); \
            failures++;                                                      \
        }                                                                    \
    } while (0)

#endif /* OC_TEST_CHECK_H */
