/*
 * Storage pressure policy + measurement (REQ-212..218, ARCH-77/78).
 *
 * The daemon assumes an **unmanaged box**: nobody is watching the disk, and
 * nobody will intervene before it fills. A full disk is worse than it sounds —
 * SQLite stops writing and WAL stops checkpointing, so messaging dies, not just
 * attachments. This module holds the thresholds that keep that from happening
 * and the `statvfs` sampling behind them.
 *
 * The policy is three tiers, cheapest and least destructive first (ARCH-77):
 *
 *   1. reclaim   — orphaned/aborted uploads; nothing was promised to keep them
 *   2. expire    — attachments past `max_age_ms`, and under pressure the oldest
 *                  attachments regardless of age (eviction)
 *   3. refuse    — reject new uploads so the database keeps its reserve
 *
 * All of it applies to the LOCAL blob backend. With external S3 the blobs are
 * off-box, so only database growth is local and only the reserve matters.
 */

#ifndef OC_STORAGE_H
#define OC_STORAGE_H

#include <stdint.h>

typedef struct {
    /* Sampled from statvfs; 0 if the path could not be measured. */
    uint64_t total_bytes;
    uint64_t avail_bytes;
    uint64_t sampled_ms;      /* when, so a stale sample is recognizable */
    int      valid;
} oc_storage_stats;

typedef struct {
    uint64_t interval_ms;     /* how often the maintenance pass runs           */
    uint64_t max_age_ms;      /* attachments older than this expire; 0 = never */
    uint64_t grace_ms;        /* never evict anything younger than this        */
    uint64_t reserve_bytes;   /* free space belonging to SQLite, never to blobs */
    uint64_t pressure_bytes;  /* below this free, start reclaiming             */
    uint64_t recover_bytes;   /* reclaim until free is back above this         */
    uint32_t batch;           /* max blobs reclaimed per pass                  */
    int      evict_enabled;   /* pressure eviction (REQ-215); default on       */
} oc_storage_policy;

/* Load the policy from the environment, applying documented defaults for
 * anything unset:
 *   OPENCHIME_MAINT_INTERVAL_MS   (default 300000 — five minutes)
 *   OPENCHIME_ATTACH_MAX_AGE_DAYS (default 0 — keep forever)
 *   OPENCHIME_EVICT_GRACE_HOURS   (default 24)
 *   OPENCHIME_DB_RESERVE_MB       (default 256)
 *   OPENCHIME_PRESSURE_MB         (default 512)
 *   OPENCHIME_RECOVER_MB          (default 1024)
 *   OPENCHIME_MAINT_BATCH         (default 64)
 *   OPENCHIME_EVICT               (default on; "off"/"0" disables)
 * The two watermarks are deliberately distinct: reclaiming down to the same
 * threshold that triggered it would leave the daemon oscillating at the
 * boundary, running a pass on nearly every tick. */
void oc_storage_policy_load(oc_storage_policy *p);

/* Sample free space on the filesystem holding `path`. Never fails loudly: an
 * unmeasurable path yields `valid = 0`, and the caller then treats storage as
 * unconstrained rather than refusing service on a measurement error. */
void oc_storage_sample(const char *path, uint64_t now_ms, oc_storage_stats *out);

/* Is free space below the point where reclamation should run? */
int oc_storage_under_pressure(const oc_storage_stats *s, const oc_storage_policy *p);

/* Is free space so low that new uploads must be refused (REQ-216)? This is the
 * database's reserve: past here, accepting more bytes risks the outage that
 * REQ-212 exists to prevent. */
int oc_storage_must_refuse(const oc_storage_stats *s, const oc_storage_policy *p);

#endif /* OC_STORAGE_H */
