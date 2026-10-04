/*
 * sumeval -- run the daemon's summary pipeline (ARCH-116) outside the daemon, for
 * evaluating models and the two constants (docs/SUMMARIES.md §7). Development
 * only: never installed.
 *
 *   sumeval init <db>
 *       an empty database at the daemon's schema, for scripts/slack_to_db.py
 *   sumeval run <db> <model.gguf> <channel> <start_ms> <end_ms> [threshold] [gap_minutes]
 *       summarize [start, end) of the channel named, print the summary as
 *       clients read it, then one line of measurements: model calls, tokens,
 *       CPU time, wall time and peak memory
 *
 * The real worker code builds the summary (oc_sum_build_now): the same
 * pieces, prompts, grammar, checks and rollups as the daemon, stored in the
 * database given, so a second run reuses what the first made. Runs on one
 * thread, ungated. Built by `make build/sumeval`.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

#include "jsmn.h"       /* the tokenizer's one implementation in this program */
#include "migrate.h"
#include "sum_store.h"
#include "sum_worker.h"

typedef struct {
    sqlite3 *db;
    int      stored;
    char    *answer;
} sink_ctx;

static int sink_store(void *ctx, const oc_sum_answer *a, oc_sum_new *nodes, int n) {
    sink_ctx *s = ctx;
    if (!a->ok) { fprintf(stderr, "sumeval: %s\n", a->err ? a->err : "failed"); return 0; }
    if (n) {
        int rc = oc_sum_store(s->db, a->channel, a->version, nodes, n);
        if (rc != 0) return rc;
        s->stored += n;
    }
    free(s->answer);
    s->answer = strdup(a->body ? a->body : "");
    return 0;
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[1], "init")) {
        sqlite3 *db;
        char *err = NULL;
        if (sqlite3_open(argv[2], &db) != SQLITE_OK || oc_migrate_default(db, &err) != 0) {
            fprintf(stderr, "sumeval: %s\n", err ? err : "cannot create the database");
            return 1;
        }
        sqlite3_close(db);
        return 0;
    }
    if (argc < 7 || strcmp(argv[1], "run") != 0) {
        fprintf(stderr, "usage: sumeval init <db> | sumeval run <db> <model.gguf> <channel> <start_ms> <end_ms> [threshold] [gap_minutes]\n");
        return 2;
    }
    const char *dbp = argv[2], *model = argv[3], *chan = argv[4];
    int64_t start = atoll(argv[5]), end = atoll(argv[6]);
    size_t threshold = argc > 7 ? (size_t)atoi(argv[7]) : SUM_THRESHOLD_TOKENS;
    uint64_t gap = argc > 8 ? (uint64_t)atoi(argv[8]) * 60000u : SUM_GAP_MS;

    sink_ctx s = {0};
    if (sqlite3_open(dbp, &s.db) != SQLITE_OK) { fprintf(stderr, "sumeval: cannot open %s\n", dbp); return 1; }
    sqlite3_exec(s.db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_stmt *st;
    int64_t channel = 0;
    sqlite3_prepare_v2(s.db, "SELECT id FROM channels WHERE name=?1;", -1, &st, NULL);
    sqlite3_bind_text(st, 1, chan, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) channel = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    if (!channel) { fprintf(stderr, "sumeval: no channel %s\n", chan); return 1; }

    const char *base = strrchr(model, '/');
    char version[128];
    snprintf(version, sizeof version, "%s", base ? base + 1 : model);
    int ctx_tokens = (int)threshold + 1024 + SUM_MAX_OUT;
    oc_sum_worker_cfg cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.db_path = dbp;
    cfg.engine = oc_sum_llama_engine(model, version, ctx_tokens);
    cfg.sink.store = sink_store;
    cfg.sink.ctx = &s;
    cfg.threshold = threshold;
    cfg.gap_ms = gap;

    double t0 = now_s();
    char err[256] = "";
    int rc = oc_sum_build_now(&cfg, channel, start, end, 0, err, sizeof err);
    double t1 = now_s();
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    if (rc != 0) fprintf(stderr, "sumeval: %s\n", err);
    if (s.answer) printf("%s\n", s.answer);
    printf("MEASURE model=%s threshold=%zu gap_min=%llu nodes_stored=%d cpu_s=%.1f wall_s=%.1f maxrss_mb=%ld\n",
           version, threshold, (unsigned long long)(gap / 60000u), s.stored,
           (double)ru.ru_utime.tv_sec + (double)ru.ru_utime.tv_usec / 1e6, t1 - t0, ru.ru_maxrss / 1024);
    free(s.answer);
    sqlite3_close(s.db);
    return rc == 0 ? 0 : 1;
}
