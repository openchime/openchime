/* Fixed-capacity id -> pointer map. See idmap.h. */

#include "idmap.h"

#include <stdlib.h>

/* Fibonacci hashing: ids are sequential, and the multiply spreads neighbours
 * across the table instead of packing them into one cluster. */
static size_t slot_of(const oc_idmap *m, uint64_t key) {
    return (size_t)((key * 0x9E3779B97F4A7C15ull) >> 32) & (m->cap - 1);
}

int oc_idmap_init(oc_idmap *m, size_t cap) {
    m->keys = NULL; m->vals = NULL; m->cap = 0; m->n = 0;
    if (cap < 2 || (cap & (cap - 1))) return -1;
    m->keys = calloc(cap, sizeof *m->keys);
    m->vals = calloc(cap, sizeof *m->vals);
    if (!m->keys || !m->vals) { oc_idmap_free(m); return -1; }
    m->cap = cap;
    return 0;
}

void oc_idmap_free(oc_idmap *m) {
    free(m->keys); free(m->vals);
    m->keys = NULL; m->vals = NULL; m->cap = 0; m->n = 0;
}

void *oc_idmap_get(const oc_idmap *m, uint64_t key) {
    if (!key || !m->cap) return NULL;
    for (size_t i = slot_of(m, key);; i = (i + 1) & (m->cap - 1)) {
        if (m->keys[i] == key) return m->vals[i];
        if (m->keys[i] == 0) return NULL;
    }
}

int oc_idmap_put(oc_idmap *m, uint64_t key, void *val) {
    if (!key || !m->cap) return -1;
    size_t i = slot_of(m, key);
    for (;; i = (i + 1) & (m->cap - 1)) {
        if (m->keys[i] == key) { m->vals[i] = val; return 0; }
        if (m->keys[i] == 0) break;
    }
    if (m->n >= m->cap / 2) return -1;
    m->keys[i] = key;
    m->vals[i] = val;
    m->n++;
    return 0;
}

void oc_idmap_del(oc_idmap *m, uint64_t key) {
    if (!key || !m->cap) return;
    size_t mask = m->cap - 1, i = slot_of(m, key);
    for (;; i = (i + 1) & mask) {
        if (m->keys[i] == 0) return;
        if (m->keys[i] == key) break;
    }
    /* Backward shift: pull later members of the cluster into the hole when their
     * home slot does not lie strictly between the hole and where they sit, so
     * every remaining key is still reachable from its home without a gap. */
    size_t hole = i;
    for (size_t j = (i + 1) & mask; m->keys[j]; j = (j + 1) & mask) {
        size_t home = slot_of(m, m->keys[j]);
        int between = hole <= j ? (hole < home && home <= j) : (hole < home || home <= j);
        if (between) continue;
        m->keys[hole] = m->keys[j];
        m->vals[hole] = m->vals[j];
        hole = j;
    }
    m->keys[hole] = 0;
    m->vals[hole] = NULL;
    m->n--;
}
