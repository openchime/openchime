/* The net loop's id -> pointer map (daemon/idmap.c): every key put is found,
 * every key deleted is gone, and deleting from the middle of a cluster leaves
 * the rest reachable -- checked against a plain array as the reference, through
 * a long random run of puts and deletes that wraps the table's end. */

#include "idmap.h"
#include "check.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_basics(void) {
    oc_idmap m;
    CHECK(oc_idmap_init(&m, 100) == -1);           /* not a power of two */
    CHECK(oc_idmap_init(&m, 16) == 0);
    int a, b;
    CHECK(oc_idmap_put(&m, 0, &a) == -1);          /* 0 is "empty" */
    CHECK(oc_idmap_get(&m, 0) == NULL);
    CHECK(oc_idmap_put(&m, 7, &a) == 0);
    CHECK(oc_idmap_get(&m, 7) == &a);
    CHECK(oc_idmap_put(&m, 7, &b) == 0);           /* replace, not a second entry */
    CHECK(oc_idmap_get(&m, 7) == &b && m.n == 1);
    oc_idmap_del(&m, 99);                          /* missing: no effect */
    CHECK(m.n == 1);
    oc_idmap_del(&m, 7);
    CHECK(oc_idmap_get(&m, 7) == NULL && m.n == 0);
    /* Half full is the bound: the ninth distinct key into 16 slots is refused. */
    for (uint64_t k = 1; k <= 8; k++) CHECK(oc_idmap_put(&m, k, &a) == 0);
    CHECK(oc_idmap_put(&m, 9, &a) == -1);
    CHECK(oc_idmap_put(&m, 8, &b) == 0);           /* a replace still is not */
    oc_idmap_free(&m);
}

/* Against a reference: keys 1..R, present or not, with a value each. Small
 * table, many operations, so clusters form, wrap and are cut repeatedly. */
static void test_random_against_reference(void) {
    enum { CAP = 64, R = 200, OPS = 200000 };
    oc_idmap m;
    CHECK(oc_idmap_init(&m, CAP) == 0);
    static int vals[R + 1];
    static int present[R + 1];
    memset(present, 0, sizeof present);
    size_t n = 0;
    srand(4422);
    int bad = 0;
    for (int op = 0; op < OPS && !bad; op++) {
        uint64_t k = 1 + (uint64_t)(rand() % R);
        if (rand() % 2) {
            int rc = oc_idmap_put(&m, k, &vals[k]);
            if (present[k]) bad |= rc != 0;
            else if (n < CAP / 2) { bad |= rc != 0; if (!rc) { present[k] = 1; n++; } }
            else bad |= rc != -1;
        } else {
            oc_idmap_del(&m, k);
            if (present[k]) { present[k] = 0; n--; }
        }
        if (op % 997 == 0)
            for (uint64_t q = 1; q <= R; q++)
                bad |= oc_idmap_get(&m, q) != (present[q] ? (void *)&vals[q] : NULL);
    }
    CHECK(!bad);
    CHECK(m.n == n);
    for (uint64_t q = 1; q <= R; q++)
        CHECK(oc_idmap_get(&m, q) == (present[q] ? (void *)&vals[q] : NULL));
    oc_idmap_free(&m);
}

int run_idmap_tests(void) {
    printf("test_idmap: put/get/replace/delete, the half-full bound, and a random run against a reference\n");
    test_basics();
    test_random_against_reference();
    return failures;
}
