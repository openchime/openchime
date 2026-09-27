/*
 * How many connections each peer address holds, for the per-address cap
 * (ARCH-22): a fixed-capacity map from an address, as text, to a count. Open
 * addressing on the address itself, with linear probing and backward-shift
 * deletion, as in idmap.h: two addresses whose hashes collide are simply two
 * keys in one cluster, and a lookup never walks past the end of its cluster.
 * The loop owns it and it takes no locks.
 *
 * The capacity is fixed at creation and must be a power of two; an increment
 * that would add a key beyond half of it is refused. The owner sizes it at
 * twice the most connections it can hold, so that never happens.
 */

#ifndef OPENCHIME_SRCCOUNT_H
#define OPENCHIME_SRCCOUNT_H

#include <stddef.h>

#define OC_SRC_LEN 46   /* an IPv6 address as text, with its terminator */

typedef struct {
    char  (*keys)[OC_SRC_LEN];   /* "" = empty */
    int    *counts;
    size_t  cap, n;
} oc_srccount;

/* 0, or -1 if `cap` is not a power of two or allocation failed. */
int  oc_srccount_init(oc_srccount *m, size_t cap);
void oc_srccount_free(oc_srccount *m);
/* The count for `src`; 0 for an address not held, and for "". */
int  oc_srccount_get(const oc_srccount *m, const char *src);
/* One more for `src`: the new count, or -1 for "" or a new address in a map
 * already holding cap/2 of them. */
int  oc_srccount_inc(oc_srccount *m, const char *src);
/* One fewer; at zero the address is removed. A missing address is not an error. */
void oc_srccount_dec(oc_srccount *m, const char *src);

#endif /* OPENCHIME_SRCCOUNT_H */
