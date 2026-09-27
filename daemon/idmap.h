/*
 * A fixed-capacity map from a non-zero 64-bit id to a pointer, for the net
 * loop's indexes (ARCH-22): connection id to connection, user id to that user's
 * connections. Open addressing with linear probing and backward-shift deletion,
 * so there are no tombstones and a lookup never walks further than the cluster
 * it lands in. The loop owns it and it takes no locks.
 *
 * The capacity is fixed at creation and must be a power of two; the owner keeps
 * the number of entries at or below half of it, which is what keeps the probes
 * short. A put into a map at that bound is refused rather than degraded.
 */

#ifndef OPENCHIME_IDMAP_H
#define OPENCHIME_IDMAP_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t *keys;   /* 0 = empty */
    void    **vals;
    size_t    cap, n;
} oc_idmap;

/* 0, or -1 if `cap` is not a power of two or allocation failed. */
int   oc_idmap_init(oc_idmap *m, size_t cap);
void  oc_idmap_free(oc_idmap *m);
void *oc_idmap_get(const oc_idmap *m, uint64_t key);
/* Insert or replace. -1 for key 0, or when the map already holds cap/2 keys. */
int   oc_idmap_put(oc_idmap *m, uint64_t key, void *val);
/* Remove; a missing key is not an error. */
void  oc_idmap_del(oc_idmap *m, uint64_t key);

#endif /* OPENCHIME_IDMAP_H */
