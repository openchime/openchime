/*
 * Fixed-window rate limiter (REQ-190/191) — a small, dependency-free counter
 * keyed by an opaque string. Used to throttle failed local-auth attempts per
 * account (AUTH.md §2); the same primitive can later key on source IP or SEND.
 *
 * Single-threaded: the DB-writer thread owns its limiters, so there is no
 * internal locking. Bounded memory — a fixed slot table with LRU eviction, so a
 * flood of distinct keys can never grow it without bound.
 */

#ifndef OPENCHIME_RATELIMIT_H
#define OPENCHIME_RATELIMIT_H

#include <stddef.h>
#include <stdint.h>

#define OC_RL_KEYMAX 128   /* keys longer than this are truncated */

typedef struct oc_ratelimit oc_ratelimit;

/* A limiter allowing up to `max_events` recorded events per `window_ms`, over at
 * most `capacity` tracked keys (LRU-evicted past that). NULL on alloc failure. */
oc_ratelimit *oc_ratelimit_new(uint32_t max_events, uint64_t window_ms, size_t capacity);
void          oc_ratelimit_free(oc_ratelimit *rl);

/* 1 if `key` has reached the limit within the current window (reject), else 0.
 * Does not record an event. Rolls the window if it has elapsed. */
int  oc_ratelimit_blocked(oc_ratelimit *rl, const char *key, uint64_t now_ms);

/* Record one event for `key` (e.g. a failed attempt). Allocates/evicts a slot
 * as needed and rolls the window. */
void oc_ratelimit_record(oc_ratelimit *rl, const char *key, uint64_t now_ms);

/* Forget `key`'s counter entirely (e.g. after a successful auth). */
void oc_ratelimit_reset(oc_ratelimit *rl, const char *key);

#endif /* OPENCHIME_RATELIMIT_H */
