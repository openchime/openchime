/*
 * OpenChime — whether the machine is quiet enough to summarize (REQ-310,
 * ARCH-116, docs/SUMMARIES.md §5).
 *
 * Summaries are made in idle time only. The worker asks this gate before each
 * piece and every few tokens; while the machine's CPU is busy, its available
 * memory low, or the daemon itself busy with people's traffic, the gate holds the
 * worker (the generation keeps its place) and lets it go once things are quiet
 * again. Memory that stays low makes the worker give the model's memory back.
 *
 * The readings come through a table of functions, so the tests drive the gate
 * with made-up numbers.
 */
#ifndef OC_SUM_LOAD_H
#define OC_SUM_LOAD_H

#include <stdint.h>

/* The gate's thresholds: code constants, tuned while summaries are evaluated. */
#define SUM_LOAD_SAMPLE_MS   1000u   /* how often the readings are taken */
#define SUM_CPU_BUSY_PCT     70u     /* whole-machine CPU above this: pause */
#define SUM_CPU_RESUME_PCT   50u     /* ...and below this again: carry on */
#define SUM_MEM_MIN_MB       256u    /* available memory under this: pause */
#define SUM_MEM_UNLOAD_MS    30000u  /* ...for this long: unload the model too */
#define SUM_NET_BUSY_BPS     65536u  /* the daemon reading more than this a second: pause */
#define SUM_IDLE_SETTLE_MS   15000u  /* quiet this long before background work starts */

typedef struct {
    /* Cumulative CPU jiffies of the whole machine: busy and total. 0 or -1. */
    int (*cpu)(void *ctx, uint64_t *busy, uint64_t *total);
    /* Available memory in MB. 0 or -1. */
    int (*mem)(void *ctx, uint64_t *avail_mb);
    /* Cumulative bytes the daemon has read from people. 0 or -1. */
    int (*net)(void *ctx, uint64_t *bytes);
    /* Milliseconds on a monotonic clock. */
    uint64_t (*now_ms)(void *ctx);
    void *ctx;
} oc_sum_probe;

/* The real readings: /proc/stat, /proc/meminfo, and the daemon's own traffic
 * through `oc_sum_probe_set_net` (the daemon passes the net loop's counter of
 * bytes read; unset, traffic is not counted). */
const oc_sum_probe *oc_sum_probe_system(void);
void oc_sum_probe_set_net(int (*reader)(uint64_t *bytes));

typedef struct {
    const oc_sum_probe *probe;
    uint64_t last_sample_ms;
    uint64_t prev_busy, prev_total, prev_bytes;
    int      have_prev;
    int      busy;              /* the last verdict */
    uint64_t quiet_since_ms;    /* when the machine last became quiet (0: busy) */
    uint64_t mem_low_since_ms;  /* when available memory went low (0: it is not) */
    unsigned cpu_pct;           /* the last readings, for the log and the tests */
    uint64_t avail_mb, net_bps;
} oc_sum_load;

void oc_sum_load_init(oc_sum_load *g, const oc_sum_probe *probe);

/* Take a reading if one is due; 1 if the machine is busy (pause), 0 if quiet. */
int oc_sum_load_busy(oc_sum_load *g);

/* 1 once the machine has been quiet for SUM_IDLE_SETTLE_MS. */
int oc_sum_load_settled(const oc_sum_load *g);

/* 1 once available memory has been low for SUM_MEM_UNLOAD_MS. */
int oc_sum_load_should_unload(const oc_sum_load *g);

#endif
