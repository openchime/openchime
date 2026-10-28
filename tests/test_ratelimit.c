/* Unit tests for the fixed-window rate limiter (daemon/ratelimit.c). Time is
 * injected (now_ms) so window rollover and eviction are deterministic. */

#include "ratelimit.h"
#include "check.h"

static void test_window_and_reset(void) {
    /* max 3 events per 1000ms window. */
    oc_ratelimit *rl = oc_ratelimit_new(3, 1000, 8);
    CHECK(rl != NULL);

    /* Below the limit is never blocked. */
    CHECK(oc_ratelimit_blocked(rl, "alice", 0) == 0);
    oc_ratelimit_record(rl, "alice", 0);
    oc_ratelimit_record(rl, "alice", 10);
    CHECK(oc_ratelimit_blocked(rl, "alice", 20) == 0);   /* 2 < 3 */
    oc_ratelimit_record(rl, "alice", 20);
    CHECK(oc_ratelimit_blocked(rl, "alice", 30) == 1);   /* 3 >= 3 */

    /* A different account is unaffected. */
    CHECK(oc_ratelimit_blocked(rl, "bob", 30) == 0);

    /* The window rolls over: after 1000ms the counter resets. */
    CHECK(oc_ratelimit_blocked(rl, "alice", 1031) == 0);

    /* Explicit reset clears immediately (e.g. after a success). */
    oc_ratelimit_record(rl, "carol", 0);
    oc_ratelimit_record(rl, "carol", 0);
    oc_ratelimit_record(rl, "carol", 0);
    CHECK(oc_ratelimit_blocked(rl, "carol", 0) == 1);
    oc_ratelimit_reset(rl, "carol");
    CHECK(oc_ratelimit_blocked(rl, "carol", 0) == 0);

    oc_ratelimit_free(rl);
}

static void test_eviction(void) {
    /* Capacity 2: tracking a third key evicts the least-recently-used one, but
     * the table must never overflow or crash. */
    oc_ratelimit *rl = oc_ratelimit_new(1, 1000, 2);
    CHECK(rl != NULL);

    oc_ratelimit_record(rl, "a", 0);     /* a: last_ms=0 */
    oc_ratelimit_record(rl, "b", 5);     /* b: last_ms=5 */
    CHECK(oc_ratelimit_blocked(rl, "a", 6) == 1);
    CHECK(oc_ratelimit_blocked(rl, "b", 6) == 1);

    /* "c" evicts the LRU slot; both tracked keys still resolve without crash. */
    oc_ratelimit_record(rl, "c", 10);
    CHECK(oc_ratelimit_blocked(rl, "c", 11) == 1);

    oc_ratelimit_free(rl);
}

int run_ratelimit_tests(void) {
    printf("test_ratelimit: window rollover, per-key isolation, reset, LRU eviction\n");
    test_window_and_reset();
    test_eviction();
    return failures;
}
