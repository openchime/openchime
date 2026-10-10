/*
 * sumeval -- run the daemon's summary pipeline (ARCH-116) outside the daemon, for
 * evaluating models and the two constants (docs/SUMMARIES.md §7). Development
 * only: never installed.
 *
 *   sumeval init <db>
 *       an empty database at the daemon's schema, for scripts/slack_to_db.py
 *   sumeval run <db> <model> <channel> <start_ms> <end_ms> [threshold] [gap_minutes] [tree.md]
 *       summarize [start, end) of the channel named, print the summary as
 *       clients read it, then one line of measurements: nodes stored, model
 *       calls, CPU time, wall time and peak memory. With tree.md, every model
 *       call -- the prompt, the answer as written, and the summary it became --
 *       is written there, in the order made, to see what each level did
 *   sumeval batch <db> <model> <set.tsv> <outdir> [threshold] [eval|held|all]
 *       every span of an evaluation set (scripts/sumeval_set.py) on one load of
 *       the model: for each, <id>.json (the summary as clients read it) and
 *       <id>.tree.md, and a line of measurements in measure.tsv -- class,
 *       channel, model calls, CPU and wall seconds, nodes stored, the peak
 *       memory so far, how many notes and sentences the checks dropped, how
 *       many calls wrote something again shorter, how many requests were
 *       sent again after a 429 or 5xx, with the seconds waited, and how many
 *       answers ran away and were asked for once more.
 *       A span already summarized there is skipped
 *
 * <model> is a GGUF file, run here by the local engine, or an http(s) URL of a
 * chat-completions API, asked by the hosted engine for the model named in
 * OPENCHIME_SUMMARY_MODEL with the key in OPENCHIME_SUMMARY_API_KEY (as the
 * daemon is configured). Nothing is run on this machine for a URL.
 *
 * The real worker code builds the summary (oc_sum_build_now): the same
 * pieces, prompt, checks and recursion as the daemon, stored in the
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
#include "sum_engine.h"

/* The engine for <model>: hosted for a URL, local for a file. NULL with a
 * message printed when a URL lacks its model name. */
static const oc_sum_engine *engine_for(const char *model, int n_ctx) {
    if (!strncmp(model, "http://", 7) || !strncmp(model, "https://", 8)) {
        const char *name = getenv("OPENCHIME_SUMMARY_MODEL"), *key = getenv("OPENCHIME_SUMMARY_API_KEY");
        if (!name || !*name) {
            fprintf(stderr, "sumeval: a URL needs OPENCHIME_SUMMARY_MODEL in the environment\n");
            return NULL;
        }
        return oc_sum_cloud_engine(model, name, key ? key : "");
    }
    const char *base = strrchr(model, '/');
    static char version[128];
    snprintf(version, sizeof version, "%s", base ? base + 1 : model);
    return oc_sum_llama_engine(model, version, n_ctx);
}

typedef struct {
    sqlite3 *db;
    int      stored;
    int      dropped;
    int      rewrites, reasks, retries;
    uint32_t wait_ms;
    char    *answer;
} sink_ctx;

static int sink_store(void *ctx, const oc_sum_answer *a, oc_sum_new *nodes, int n) {
    sink_ctx *s = ctx;
    s->dropped = a->dropped;
    s->rewrites = a->rewrites;
    s->reasks = a->reasks;
    s->retries = a->retries;
    s->wait_ms = a->wait_ms;
    /* What a build made is stored whether or not it finished (SUMMARIES.md §4). */
    if (n) {
        int rc = oc_sum_store(s->db, a->channel, a->version, nodes, n);
        if (rc != 0) return rc;
        s->stored += n;
    }
    if (!a->ok) { fprintf(stderr, "sumeval: %s\n", a->err ? a->err : "failed"); return 0; }
    free(s->answer);
    s->answer = strdup(a->body ? a->body : "");
    return 0;
}

typedef struct { FILE *f; int n; } tree;

static void trace(void *ctx, const char *prompt, const char *answer, const char *body) {
    tree *t = ctx;
    t->n++;
    if (!t->f) return;
    fprintf(t->f, "## Call %d\n\n### Prompt\n\n```\n%s```\n\n### Answer\n\n```\n%s\n```\n\n### Stored\n\n```\n%s\n```\n\n",
            t->n, prompt, answer, body ? body : "(failed)");
    fflush(t->f);
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static double cpu_s(void) {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return (double)ru.ru_utime.tv_sec + (double)ru.ru_utime.tv_usec / 1e6;
}

/* The channel named, or the one with that id when `name` is a number. */
static int64_t channel_id(sqlite3 *db, const char *name) {
    sqlite3_stmt *st;
    int64_t channel = 0;
    char *end;
    long long id = strtoll(name, &end, 10);
    if (*name && !*end) {
        sqlite3_prepare_v2(db, "SELECT id FROM channels WHERE id=?1;", -1, &st, NULL);
        sqlite3_bind_int64(st, 1, id);
        if (sqlite3_step(st) == SQLITE_ROW) channel = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
        return channel;
    }
    sqlite3_prepare_v2(db, "SELECT id FROM channels WHERE name=?1;", -1, &st, NULL);
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) channel = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return channel;
}

static int batch(int argc, char **argv) {
    const char *dbp = argv[2], *model = argv[3], *set = argv[4], *dir = argv[5];
    size_t threshold = argc > 6 ? (size_t)atoi(argv[6]) : SUM_THRESHOLD_TOKENS;
    const char *which = argc > 7 ? argv[7] : "eval";
    FILE *in = fopen(set, "r");
    if (!in) { fprintf(stderr, "sumeval: cannot read %s\n", set); return 1; }
    sink_ctx s = {0};
    if (sqlite3_open(dbp, &s.db) != SQLITE_OK) { fprintf(stderr, "sumeval: cannot open %s\n", dbp); return 1; }
    sqlite3_exec(s.db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    tree t = { NULL, 0 };
    oc_sum_worker_cfg cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.db_path = dbp;
    cfg.engine = engine_for(model, SUM_CTX_TOKENS);
    if (!cfg.engine) return 2;
    cfg.sink.store = sink_store;
    cfg.sink.ctx = &s;
    cfg.threshold = threshold;
    cfg.gap_ms = SUM_GAP_MS;
    cfg.trace = trace;
    cfg.trace_ctx = &t;
    void *engine = NULL;
    char line[1024], path[1024];
    snprintf(path, sizeof path, "%s/measure.tsv", dir);
    FILE *meas = fopen(path, "a");
    if (!meas) { fprintf(stderr, "sumeval: cannot write %s\n", path); return 1; }
    while (fgets(line, sizeof line, in)) {
        if (line[0] == '#') continue;
        char cls[16], set_of[16], chan[256];
        int id;
        long long start, end;
        if (sscanf(line, "%d\t%15s\t%15s\t%255s\t%lld\t%lld", &id, cls, set_of, chan, &start, &end) != 6) continue;
        if (strcmp(which, "all") && strcmp(which, set_of)) continue;
        snprintf(path, sizeof path, "%s/%d.json", dir, id);
        FILE *done = fopen(path, "r");
        if (done) { fclose(done); continue; }
        int64_t channel = channel_id(s.db, chan);
        if (!channel) { fprintf(stderr, "sumeval: no channel %s\n", chan); continue; }
        snprintf(path, sizeof path, "%s/%d.tree.md", dir, id);
        t.f = fopen(path, "w");
        t.n = 0;
        s.stored = 0;
        free(s.answer);
        s.answer = NULL;
        double c0 = cpu_s(), w0 = now_s();
        char err[256] = "";
        int rc = oc_sum_build_keep(&cfg, &engine, channel, start, end, 0, err, sizeof err);
        double c1 = cpu_s(), w1 = now_s();
        if (t.f) fclose(t.f);
        t.f = NULL;
        if (rc == 0 && s.answer) {
            snprintf(path, sizeof path, "%s/%d.json", dir, id);
            FILE *o = fopen(path, "w");
            if (o) { fprintf(o, "%s\n", s.answer); fclose(o); }
        }
        struct rusage ru;
        getrusage(RUSAGE_SELF, &ru);
        fprintf(meas, "%d\t%s\t%s\t%s\t%d\t%.1f\t%.1f\t%d\t%ld\t%s\t%d\t%d\t%d\t%.1f\t%d\n", id, cls, chan,
                rc == 0 ? "ok" : "FAILED", t.n, c1 - c0, w1 - w0, s.stored, ru.ru_maxrss / 1024, rc == 0 ? "" : err,
                s.dropped, s.rewrites, s.retries, s.wait_ms / 1000.0, s.reasks);
        fflush(meas);
        fprintf(stderr, "sumeval: span %d (%s, %s): %s, %d calls, %.0f s\n", id, cls, chan, rc == 0 ? "ok" : err, t.n, w1 - w0);
    }
    if (engine) cfg.engine->close(engine);
    fclose(meas);
    fclose(in);
    free(s.answer);
    sqlite3_close(s.db);
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 6 && !strcmp(argv[1], "batch")) return batch(argc, argv);
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
        fprintf(stderr, "usage: sumeval init <db> | sumeval run <db> <model> <channel> <start_ms> <end_ms> "
                        "[threshold] [gap_minutes] [tree.md] | sumeval batch <db> <model> <set.tsv> <outdir> "
                        "[threshold] [eval|held|all]\n");
        return 2;
    }
    const char *dbp = argv[2], *model = argv[3], *chan = argv[4];
    int64_t start = atoll(argv[5]), end = atoll(argv[6]);
    size_t threshold = argc > 7 ? (size_t)atoi(argv[7]) : SUM_THRESHOLD_TOKENS;
    uint64_t gap = argc > 8 ? (uint64_t)atoi(argv[8]) * 60000u : SUM_GAP_MS;
    tree t = { argc > 9 ? fopen(argv[9], "w") : NULL, 0 };
    if (argc > 9 && !t.f) { fprintf(stderr, "sumeval: cannot write %s\n", argv[9]); return 1; }

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

    oc_sum_worker_cfg cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.db_path = dbp;
    cfg.engine = engine_for(model, SUM_CTX_TOKENS);
    if (!cfg.engine) return 2;
    cfg.sink.store = sink_store;
    cfg.sink.ctx = &s;
    cfg.threshold = threshold;
    cfg.gap_ms = gap;
    cfg.trace = trace;
    cfg.trace_ctx = &t;

    double t0 = now_s();
    char err[256] = "";
    int rc = oc_sum_build_now(&cfg, channel, start, end, 0, err, sizeof err);
    double t1 = now_s();
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    if (rc != 0) fprintf(stderr, "sumeval: %s\n", err);
    if (s.answer) printf("%s\n", s.answer);
    printf("MEASURE model=%s threshold=%zu gap_min=%llu nodes_stored=%d model_calls=%d cpu_s=%.1f wall_s=%.1f maxrss_mb=%ld dropped=%d\n",
           cfg.engine->version, threshold, (unsigned long long)(gap / 60000u), s.stored, t.n,
           (double)ru.ru_utime.tv_sec + (double)ru.ru_utime.tv_usec / 1e6, t1 - t0, ru.ru_maxrss / 1024, s.dropped);
    free(s.answer);
    if (t.f) fclose(t.f);
    sqlite3_close(s.db);
    return rc == 0 ? 0 : 1;
}
