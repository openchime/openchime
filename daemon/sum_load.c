/* Whether the machine is quiet enough to summarize (sum_load.h). */
#define _POSIX_C_SOURCE 200809L
#include "sum_load.h"

#include <stdio.h>
#include <string.h>
#include <time.h>


static int sys_cpu(void *ctx, uint64_t *busy, uint64_t *total) {
    (void)ctx;
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return -1;
    unsigned long long v[10] = {0};
    int n = fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9]);
    fclose(f);
    if (n < 4) return -1;
    uint64_t t = 0;
    for (int i = 0; i < 8 && i < n; i++) t += v[i];
    /* idle and iowait are not busy */
    *total = t;
    *busy = t - v[3] - (n > 4 ? v[4] : 0);
    return 0;
}

static int sys_mem(void *ctx, uint64_t *avail_mb) {
    (void)ctx;
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return -1;
    char line[128];
    int rc = -1;
    while (fgets(line, sizeof line, f)) {
        unsigned long long kb;
        if (sscanf(line, "MemAvailable: %llu kB", &kb) == 1) { *avail_mb = kb / 1024; rc = 0; break; }
    }
    fclose(f);
    return rc;
}

static int (*g_net_reader)(uint64_t *bytes);

void oc_sum_probe_set_net(int (*reader)(uint64_t *bytes)) { g_net_reader = reader; }

static int sys_net(void *ctx, uint64_t *bytes) {
    (void)ctx;
    if (!g_net_reader) return -1;
    return g_net_reader(bytes);
}

static uint64_t sys_now(void *ctx) {
    (void)ctx;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

const oc_sum_probe *oc_sum_probe_system(void) {
    static const oc_sum_probe p = { sys_cpu, sys_mem, sys_net, sys_now, NULL };
    return &p;
}

void oc_sum_load_init(oc_sum_load *g, const oc_sum_probe *probe) {
    memset(g, 0, sizeof *g);
    g->probe = probe;
}

int oc_sum_load_busy(oc_sum_load *g) {
    const oc_sum_probe *p = g->probe;
    uint64_t now = p->now_ms(p->ctx);
    if (g->have_prev && now - g->last_sample_ms < SUM_LOAD_SAMPLE_MS) return g->busy;
    uint64_t busy = 0, total = 0, avail = 0, bytes = 0;
    int cpu_ok = p->cpu(p->ctx, &busy, &total) == 0;
    int mem_ok = p->mem(p->ctx, &avail) == 0;
    int net_ok = p->net(p->ctx, &bytes) == 0;
    if (!g->have_prev) {
        /* The first reading only sets the baseline; it is not yet known to be
         * quiet. */
        g->prev_busy = busy;
        g->prev_total = total;
        g->prev_bytes = bytes;
        g->last_sample_ms = now;
        g->have_prev = 1;
        g->busy = 1;
        return 1;
    }
    uint64_t dt = now - g->last_sample_ms;
    unsigned pct = 0;
    if (cpu_ok && total > g->prev_total) pct = (unsigned)((busy - g->prev_busy) * 100u / (total - g->prev_total));
    uint64_t bps = net_ok && dt ? (bytes - g->prev_bytes) * 1000u / dt : 0;
    g->prev_busy = busy;
    g->prev_total = total;
    g->prev_bytes = bytes;
    g->last_sample_ms = now;
    g->cpu_pct = pct;
    g->avail_mb = avail;
    g->net_bps = bps;

    int mem_low = mem_ok && avail < SUM_MEM_MIN_MB;
    if (mem_low && !g->mem_low_since_ms) g->mem_low_since_ms = now;
    if (!mem_low) g->mem_low_since_ms = 0;

    /* Hysteresis on CPU, so the worker does not flap at the line. */
    int cpu_busy = g->busy ? pct >= SUM_CPU_RESUME_PCT : pct > SUM_CPU_BUSY_PCT;
    int was = g->busy;
    g->busy = cpu_busy || mem_low || bps > SUM_NET_BUSY_BPS;
    if (g->busy) g->quiet_since_ms = 0;
    else if (was || !g->quiet_since_ms) g->quiet_since_ms = now;
    return g->busy;
}

int oc_sum_load_settled(const oc_sum_load *g) {
    return !g->busy && g->quiet_since_ms &&
           g->probe->now_ms(g->probe->ctx) - g->quiet_since_ms >= SUM_IDLE_SETTLE_MS;
}

int oc_sum_load_should_unload(const oc_sum_load *g) {
    return g->mem_low_since_ms && g->probe->now_ms(g->probe->ctx) - g->mem_low_since_ms >= SUM_MEM_UNLOAD_MS;
}
