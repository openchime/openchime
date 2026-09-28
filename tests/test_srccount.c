/* The per-address connection count (daemon/srccount.c): an address counted n
 * times reads n, reads 0 once each count is given back, and removing one
 * address from the middle of a cluster leaves the rest reachable -- checked
 * against a plain array as the reference, through a long random run in a table
 * small enough that addresses share home slots and clusters wrap its end. */

#include "srccount.h"
#include "check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_basics(void) {
    oc_srccount m;
    CHECK(oc_srccount_init(&m, 12) == -1);                /* not a power of two */
    CHECK(oc_srccount_init(&m, 8) == 0);
    CHECK(oc_srccount_inc(&m, "") == -1);                 /* "" is "empty" */
    CHECK(oc_srccount_get(&m, "") == 0);
    CHECK(oc_srccount_get(&m, "10.0.0.1") == 0);
    CHECK(oc_srccount_inc(&m, "10.0.0.1") == 1);
    CHECK(oc_srccount_inc(&m, "10.0.0.1") == 2);          /* the same key, counted up */
    CHECK(oc_srccount_get(&m, "10.0.0.1") == 2 && m.n == 1);
    CHECK(oc_srccount_get(&m, "10.0.0.10") == 0);         /* a prefix is another address */
    oc_srccount_dec(&m, "192.0.2.9");                     /* missing: no effect */
    CHECK(m.n == 1);
    oc_srccount_dec(&m, "10.0.0.1");
    CHECK(oc_srccount_get(&m, "10.0.0.1") == 1 && m.n == 1);
    oc_srccount_dec(&m, "10.0.0.1");
    CHECK(oc_srccount_get(&m, "10.0.0.1") == 0 && m.n == 0);
    /* Half full is the bound: a fifth address into 8 slots is refused, while
     * another connection from one already held is not. */
    CHECK(oc_srccount_inc(&m, "a") == 1 && oc_srccount_inc(&m, "b") == 1);
    CHECK(oc_srccount_inc(&m, "c") == 1 && oc_srccount_inc(&m, "d") == 1);
    CHECK(oc_srccount_inc(&m, "e") == -1);
    CHECK(oc_srccount_inc(&m, "d") == 2);
    char big[OC_SRC_LEN + 4];
    memset(big, '1', sizeof big - 1); big[sizeof big - 1] = '\0';
    CHECK(oc_srccount_inc(&m, big) == -1);                /* longer than any address */
    oc_srccount_free(&m);
}

/* Against a reference: R addresses, each with a count. With CAP 32 and up to
 * 16 addresses held, most of them share a cluster with another. */
static void test_random_against_reference(void) {
    enum { CAP = 32, R = 60, OPS = 200000 };
    oc_srccount m;
    CHECK(oc_srccount_init(&m, CAP) == 0);
    static char names[R][OC_SRC_LEN];
    static int count[R];
    memset(count, 0, sizeof count);
    /* Each in its own /64: an IPv6 source is counted by its /64 (oc_source_key). */
    for (int k = 0; k < R; k++) snprintf(names[k], sizeof names[k], "2001:db8:%x::1", k * 7919);
    size_t n = 0;
    srand(4422);
    int bad = 0;
    for (int op = 0; op < OPS && !bad; op++) {
        int k = rand() % R;
        if (rand() % 2) {
            int rc = oc_srccount_inc(&m, names[k]);
            if (count[k]) bad |= rc != ++count[k];
            else if (n < CAP / 2) { bad |= rc != 1; count[k] = 1; n++; }
            else bad |= rc != -1;
        } else {
            oc_srccount_dec(&m, names[k]);
            if (count[k] && --count[k] == 0) n--;
        }
        if (op % 997 == 0)
            for (int q = 0; q < R; q++) bad |= oc_srccount_get(&m, names[q]) != count[q];
    }
    CHECK(!bad);
    CHECK(m.n == n);
    for (int q = 0; q < R; q++) CHECK(oc_srccount_get(&m, names[q]) == count[q]);
    oc_srccount_free(&m);
}

/* The key a source is counted under: IPv4 as it is, IPv6 by its /64 -- one host
 * can vary its address across a /64 -- and an IPv4-mapped address as its IPv4
 * address, so a client has one key however it arrived. */
static void test_source_key(void) {
    static const struct { const char *in, *want; } T[] = {
        { "10.0.0.1",                    "10.0.0.1" },
        { "2001:db8:1:2:aaaa::1",        "2001:db8:1:2::/64" },
        { "2001:db8:1:2:ffff:ffff:ffff:ffff", "2001:db8:1:2::/64" },
        { "2001:db8:1:3::1",             "2001:db8:1:3::/64" },
        { "::ffff:192.0.2.7",            "192.0.2.7" },
        { "::1",                         "::/64" },
        { "not-an-address",              "not-an-address" },
        { "",                            "" },
    };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) {
        char k[OC_SRC_LEN];
        oc_source_key(T[i].in, k, sizeof k);
        if (strcmp(k, T[i].want) != 0) printf("  key(%s) = %s, want %s\n", T[i].in, k, T[i].want);
        CHECK(strcmp(k, T[i].want) == 0);
    }
    /* In the map: two addresses in one /64 are one source; another /64 is not. */
    oc_srccount m;
    CHECK(oc_srccount_init(&m, 8) == 0);
    CHECK(oc_srccount_inc(&m, "2001:db8:1:2::a") == 1);
    CHECK(oc_srccount_inc(&m, "2001:db8:1:2::b") == 2);
    CHECK(oc_srccount_get(&m, "2001:db8:1:2::c") == 2);
    CHECK(oc_srccount_get(&m, "2001:db8:1:3::a") == 0);
    CHECK(oc_srccount_inc(&m, "::ffff:10.0.0.9") == 1 && oc_srccount_get(&m, "10.0.0.9") == 1);
    oc_srccount_dec(&m, "2001:db8:1:2::a");
    CHECK(oc_srccount_get(&m, "2001:db8:1:2::b") == 1);
    oc_srccount_free(&m);
}

int run_srccount_tests(void) {
    printf("test_srccount: count up and down, the half-full bound, a random run against a reference, keys by /64\n");
    test_basics();
    test_source_key();
    test_random_against_reference();
    return failures;
}
