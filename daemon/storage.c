/* Storage pressure policy + measurement (ARCH-77/78). See storage.h. */

#include "storage.h"

#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>

static uint64_t env_u64(const char *name, uint64_t dflt) {
    const char *v = getenv(name);
    if (!v || !*v) return dflt;
    char *end = NULL;
    unsigned long long n = strtoull(v, &end, 10);
    if (end == v) return dflt;              /* unparseable: keep the default */
    return (uint64_t)n;
}

#define MB (1024ull * 1024)

void oc_storage_policy_load(oc_storage_policy *p) {
    memset(p, 0, sizeof *p);
    p->interval_ms    = env_u64("OPENCHIME_MAINT_INTERVAL_MS", 5 * 60 * 1000);
    p->max_age_ms     = env_u64("OPENCHIME_ATTACH_MAX_AGE_DAYS", 0) * 24ull * 60 * 60 * 1000;
    p->grace_ms       = env_u64("OPENCHIME_EVICT_GRACE_HOURS", 24) * 60ull * 60 * 1000;
    p->reserve_bytes  = env_u64("OPENCHIME_DB_RESERVE_MB", 256) * MB;
    p->pressure_bytes = env_u64("OPENCHIME_PRESSURE_MB", 512) * MB;
    p->recover_bytes  = env_u64("OPENCHIME_RECOVER_MB", 1024) * MB;
    p->batch          = (uint32_t)env_u64("OPENCHIME_MAINT_BATCH", 64);

    const char *ev = getenv("OPENCHIME_EVICT");
    p->evict_enabled = !(ev && (strcmp(ev, "off") == 0 || strcmp(ev, "0") == 0));

    /* Keep the watermarks coherent however they were configured: the recovery
     * target must sit above the pressure threshold, and both above the reserve,
     * or the pass would either never stop or never protect the database. */
    if (p->pressure_bytes < p->reserve_bytes) p->pressure_bytes = p->reserve_bytes;
    if (p->recover_bytes  < p->pressure_bytes) p->recover_bytes = p->pressure_bytes;
    if (p->interval_ms < 1000) p->interval_ms = 1000;   /* never busy-loop */
    if (p->batch == 0) p->batch = 1;
}

void oc_storage_sample(const char *path, uint64_t now_ms, oc_storage_stats *out) {
    memset(out, 0, sizeof *out);
    out->sampled_ms = now_ms;
    struct statvfs vfs;
    if (!path || !*path || statvfs(path, &vfs) != 0) return;   /* valid stays 0 */
    /* f_bavail, not f_bfree: the root-reserved blocks are not ours to spend. */
    out->avail_bytes = (uint64_t)vfs.f_bavail * (uint64_t)vfs.f_frsize;
    out->total_bytes = (uint64_t)vfs.f_blocks * (uint64_t)vfs.f_frsize;
    out->valid = 1;
}

int oc_storage_under_pressure(const oc_storage_stats *s, const oc_storage_policy *p) {
    /* An unmeasurable filesystem is treated as unconstrained. Guessing
     * "pressured" would make the daemon evict user data because statvfs failed,
     * which is a far worse outcome than doing nothing. */
    if (!s->valid) return 0;
    return s->avail_bytes < p->pressure_bytes;
}

int oc_storage_must_refuse(const oc_storage_stats *s, const oc_storage_policy *p) {
    if (!s->valid) return 0;                    /* same reasoning as above */
    return s->avail_bytes < p->reserve_bytes;
}
