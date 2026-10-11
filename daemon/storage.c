/* Storage pressure policy + measurement (ARCH-77/78). See storage.h. */

#include "storage.h"

#include <string.h>
#include <sys/statvfs.h>

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
