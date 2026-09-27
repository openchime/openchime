/* Fixed-capacity address -> count map. See srccount.h. */

#include "srccount.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static size_t slot_of(const oc_srccount *m, const char *src) {
    uint64_t h = 1469598103934665603ull;   /* FNV-1a */
    for (; *src; src++) { h ^= (uint8_t)*src; h *= 1099511628211ull; }
    return (size_t)h & (m->cap - 1);
}

int oc_srccount_init(oc_srccount *m, size_t cap) {
    m->keys = NULL; m->counts = NULL; m->cap = 0; m->n = 0;
    if (cap < 2 || (cap & (cap - 1))) return -1;
    m->keys = calloc(cap, sizeof *m->keys);
    m->counts = calloc(cap, sizeof *m->counts);
    if (!m->keys || !m->counts) { oc_srccount_free(m); return -1; }
    m->cap = cap;
    return 0;
}

void oc_srccount_free(oc_srccount *m) {
    free(m->keys); free(m->counts);
    m->keys = NULL; m->counts = NULL; m->cap = 0; m->n = 0;
}

/* The slot holding `src`, or the empty slot that ends its cluster. */
static size_t find(const oc_srccount *m, const char *src) {
    size_t i = slot_of(m, src);
    while (m->keys[i][0] && strcmp(m->keys[i], src) != 0) i = (i + 1) & (m->cap - 1);
    return i;
}

int oc_srccount_get(const oc_srccount *m, const char *src) {
    if (!src[0] || !m->cap) return 0;
    size_t i = find(m, src);
    return m->keys[i][0] ? m->counts[i] : 0;
}

int oc_srccount_inc(oc_srccount *m, const char *src) {
    if (!src[0] || !m->cap || strlen(src) >= OC_SRC_LEN) return -1;
    size_t i = find(m, src);
    if (!m->keys[i][0]) {
        if (m->n >= m->cap / 2) return -1;
        memcpy(m->keys[i], src, strlen(src) + 1);
        m->counts[i] = 0;
        m->n++;
    }
    return ++m->counts[i];
}

void oc_srccount_dec(oc_srccount *m, const char *src) {
    if (!src[0] || !m->cap) return;
    size_t mask = m->cap - 1, i = find(m, src);
    if (!m->keys[i][0]) return;
    if (--m->counts[i] > 0) return;
    /* Backward shift, as in idmap.c: pull later members of the cluster into the
     * hole when their home slot does not lie strictly between the hole and where
     * they sit, so every remaining key is still reachable from its home. */
    size_t hole = i;
    for (size_t j = (i + 1) & mask; m->keys[j][0]; j = (j + 1) & mask) {
        size_t home = slot_of(m, m->keys[j]);
        int between = hole <= j ? (hole < home && home <= j) : (hole < home || home <= j);
        if (between) continue;
        memcpy(m->keys[hole], m->keys[j], OC_SRC_LEN);
        m->counts[hole] = m->counts[j];
        hole = j;
    }
    m->keys[hole][0] = '\0';
    m->counts[hole] = 0;
    m->n--;
}
