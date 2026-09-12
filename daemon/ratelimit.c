/* Fixed-window rate limiter — see ratelimit.h. A linear-scan slot table; for the
 * single-box daemon's modest key counts this is simpler and cache-friendlier
 * than a hash table, and eviction keeps it bounded. */

#include "ratelimit.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    char     key[OC_RL_KEYMAX];
    uint64_t window_start;   /* ms of the current window's start */
    uint64_t last_ms;        /* last touch, for LRU eviction */
    uint32_t count;          /* events in the current window */
    int      used;
} slot;

struct oc_ratelimit {
    uint32_t max_events;
    uint64_t window_ms;
    size_t   cap;
    slot    *slots;
};

oc_ratelimit *oc_ratelimit_new(uint32_t max_events, uint64_t window_ms, size_t capacity) {
    if (capacity == 0) return NULL;
    oc_ratelimit *rl = calloc(1, sizeof *rl);
    if (!rl) return NULL;
    rl->slots = calloc(capacity, sizeof *rl->slots);
    if (!rl->slots) { free(rl); return NULL; }
    rl->max_events = max_events;
    rl->window_ms = window_ms;
    rl->cap = capacity;
    return rl;
}

void oc_ratelimit_free(oc_ratelimit *rl) {
    if (!rl) return;
    free(rl->slots);
    free(rl);
}

static void key_copy(char *dst, const char *key) {
    size_t n = strlen(key);
    if (n >= OC_RL_KEYMAX) n = OC_RL_KEYMAX - 1;
    memcpy(dst, key, n);
    dst[n] = '\0';
}

/* Find the slot for `key`, or NULL. Rolls the window if it has elapsed. */
static slot *find(oc_ratelimit *rl, const char *key, uint64_t now_ms) {
    char k[OC_RL_KEYMAX];
    key_copy(k, key);
    for (size_t i = 0; i < rl->cap; i++) {
        slot *s = &rl->slots[i];
        if (s->used && strcmp(s->key, k) == 0) {
            if (now_ms - s->window_start >= rl->window_ms) {
                s->window_start = now_ms;
                s->count = 0;
            }
            return s;
        }
    }
    return NULL;
}

/* Find, or claim a free/LRU slot for `key` and initialise its window. */
static slot *find_or_alloc(oc_ratelimit *rl, const char *key, uint64_t now_ms) {
    slot *s = find(rl, key, now_ms);
    if (s) return s;

    slot *victim = NULL;
    for (size_t i = 0; i < rl->cap; i++) {
        slot *c = &rl->slots[i];
        if (!c->used) { victim = c; break; }
        if (!victim || c->last_ms < victim->last_ms) victim = c;
    }
    key_copy(victim->key, key);
    victim->window_start = now_ms;
    victim->last_ms = now_ms;
    victim->count = 0;
    victim->used = 1;
    return victim;
}

int oc_ratelimit_blocked(oc_ratelimit *rl, const char *key, uint64_t now_ms) {
    slot *s = find(rl, key, now_ms);
    if (!s) return 0;
    s->last_ms = now_ms;
    return s->count >= rl->max_events;
}

void oc_ratelimit_record(oc_ratelimit *rl, const char *key, uint64_t now_ms) {
    slot *s = find_or_alloc(rl, key, now_ms);
    if (s->count < UINT32_MAX) s->count++;
    s->last_ms = now_ms;
}

void oc_ratelimit_reset(oc_ratelimit *rl, const char *key) {
    char k[OC_RL_KEYMAX];
    key_copy(k, key);
    for (size_t i = 0; i < rl->cap; i++) {
        if (rl->slots[i].used && strcmp(rl->slots[i].key, k) == 0) {
            rl->slots[i].used = 0;
            return;
        }
    }
}
