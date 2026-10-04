/* Channel and DM summaries (REQ-310, ARCH-116): the pieces, the checks, the
 * store and its purge, the load gate, and the worker's whole build over a stub
 * model -- everything but a real model, which make test never loads. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "check.h"
#include "migrate.h"
#include "sum_core.h"
#include "sum_load.h"
#include "sum_store.h"
#include "sum_worker.h"

#define MIN 60000ll

/* --- cutting ------------------------------------------------------------------ */

static void test_cut(void) {
    oc_sum_msg m[] = {
        { 1, 0, 10, 0 * MIN, 0, "Ann", "first", },
        { 2, 0, 11, 5 * MIN, 0, "Bob", "second", },
        { 3, 1, 11, 6 * MIN, 0, "Bob", "a reply to the first", },
        { 4, 0, 10, 300 * MIN, 0, "Ann", "much later", },
    };
    oc_sum_cut c;
    CHECK(oc_sum_cut_build(m, 4, 1000, 45 * MIN, &c) == 0);
    /* The quiet gap starts a new chunk; the thread sits at its last reply. */
    CHECK(c.n == 2);
    if (c.n == 2) {
        CHECK(c.pieces[0].n_chunks == 1 && c.pieces[0].chunks[0].n == 3);
        CHECK(c.pieces[0].end_ms == 6 * MIN);
        CHECK(c.pieces[1].chunks[0].n == 1 && m[c.pieces[1].chunks[0].idx[0]].id == 4);
    }
    oc_sum_cut_free(&c);

    /* Deterministic, and appending leaves the earlier pieces as they were. */
    oc_sum_cut a, b;
    CHECK(oc_sum_cut_build(m, 3, 1000, 45 * MIN, &a) == 0);
    CHECK(oc_sum_cut_build(m, 4, 1000, 45 * MIN, &b) == 0);
    CHECK(a.n >= 1 && b.n >= 1 && a.pieces[0].chunks[0].n == b.pieces[0].chunks[0].n);
    oc_sum_cut_free(&a);
    oc_sum_cut_free(&b);

    /* A size limit cuts too; and a thread bigger than it is its own chunks. */
    static char big[2400];
    memset(big, 'x', sizeof big - 1);
    oc_sum_msg t[] = {
        { 1, 0, 10, 0, 0, "Ann", big, },
        { 2, 1, 11, MIN, 0, "Bob", big, },
        { 3, 1, 10, 2 * MIN, 0, "Ann", big, },
        { 4, 0, 11, 3 * MIN, 0, "Bob", "after", },
    };
    CHECK(oc_sum_cut_build(t, 4, 1000, 45 * MIN, &c) == 0);
    CHECK(c.n == 2);
    if (c.n == 2) {
        CHECK(c.pieces[0].is_big_thread && c.pieces[0].n_chunks == 3 && c.pieces[0].root_id == 1);
        CHECK(!c.pieces[1].is_big_thread);
    }
    oc_sum_cut_free(&c);
}

/* --- rendering, the grammar and the check -------------------------------------- */

static void test_check(void) {
    oc_sum_msg m[] = {
        { 100, 0, 10, 0, 0, "Ann", "Ticket 6701 is open; Bob please look.", },
        { 101, 100, 11, MIN, 0, "Bob", "On it.", },
    };
    oc_sum_cut c;
    oc_sum_cut_build(m, 2, 1000, 45 * MIN, &c);
    oc_sum_buf prompt = {0}, g = {0}, out = {0};
    oc_sum_ids ids;
    CHECK(oc_sum_render_chunk("support", m, &c.pieces[0].chunks[0], &prompt, &ids) == 0);
    CHECK(prompt.p && strstr(prompt.p, "P1 Ann") && strstr(prompt.p, "[m2] P2"));
    CHECK(oc_sum_grammar(&ids, 0, SUM_MAX_ITEMS, &g) == 0 && strstr(g.p, "\"\\\"m2\\\"\"") && !strstr(g.p, "\"\\\"m3\\\"\""));

    const char *ans =
        "{\"overview\":\"P1 asked P2 about ticket 6701.\","
        "\"decisions\":[],"
        "\"actions\":[{\"who\":\"P2\",\"what\":\"look at ticket 6701\",\"refs\":[\"m1\"],\"status\":\"open\"},"
        "{\"who\":\"P2\",\"what\":\"look at ticket 6701\",\"refs\":[\"m1\"],\"status\":\"open\"},"
        "{\"who\":\"P9\",\"what\":\"someone invented\",\"refs\":[\"m1\"],\"status\":\"open\"}],"
        "\"problems\":[{\"text\":\"ticket 9999 is broken\",\"refs\":[\"m1\"],\"status\":\"open\"},"
        "{\"text\":\"cites nothing real\",\"refs\":[\"m7\"],\"status\":\"open\"}],"
        "\"facts\":[{\"text\":\"Ticket 6701\",\"refs\":[\"m1\"]}]}";
    int dropped = 0;
    int kept = oc_sum_check(ans, strlen(ans), &ids, 0, &out, &dropped);
    /* Kept: one action (the repeat dropped), the fact. Dropped: the repeat, the
     * unknown person, the invented number, the unknown ref. */
    CHECK(kept == 2);
    CHECK(dropped == 4);
    CHECK(out.p && strstr(out.p, "\"who\":11") && strstr(out.p, "\"refs\":[100]"));
    /* The model's ids read as names. */
    CHECK(out.p && strstr(out.p, "Ann asked Bob about ticket 6701."));
    CHECK(oc_sum_check("not json", 8, &ids, 0, &out, NULL) == -1);
    oc_sum_buf_free(&prompt);
    oc_sum_buf_free(&g);
    oc_sum_buf_free(&out);
    oc_sum_ids_free(&ids);
    oc_sum_cut_free(&c);
}

/* --- the store, its guard and the purge ---------------------------------------- */

static sqlite3 *fresh_db(const char *path) {
    unlink(path);
    sqlite3 *db = NULL;
    char *err = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK || oc_migrate_default(db, &err) != 0) { free(err); return NULL; }
    sqlite3_exec(db, "PRAGMA journal_mode=WAL; PRAGMA foreign_keys=ON;"
                     "INSERT INTO users(id, subject, display_name, created_at_ms) VALUES(10,'a','Ann',0),(11,'b','Bob',0);"
                     "INSERT INTO channels(id, kind, name, is_public, created_at_ms) VALUES(1,'channel','support',1,0);"
                     "INSERT INTO channel_members(channel_id, user_id, joined_at_ms) VALUES(1,10,0),(1,11,0);",
                 NULL, NULL, NULL);
    return db;
}

static void add_msg(sqlite3 *db, int64_t id, int64_t parent, int64_t author, int64_t at, const char *body) {
    sqlite3_stmt *st;
    sqlite3_prepare_v2(db, "INSERT INTO messages(id, channel_id, author_id, body, created_at_ms, parent_id) "
                           "VALUES(?1,1,?2,?3,?4,?5);", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, id);
    sqlite3_bind_int64(st, 2, author);
    sqlite3_bind_blob(st, 3, body, (int)strlen(body), SQLITE_STATIC);
    sqlite3_bind_int64(st, 4, at);
    if (parent) sqlite3_bind_int64(st, 5, parent); else sqlite3_bind_null(st, 5);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

static int count_nodes(sqlite3 *db) {
    sqlite3_stmt *st;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM summary_nodes;", -1, &st, NULL);
    int n = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int(st, 0) : -1;
    sqlite3_finalize(st);
    return n;
}

static void test_store(void) {
    const char *path = "/tmp/oc_test_summary_store.db";
    sqlite3 *db = fresh_db(path);
    CHECK(db != NULL);
    if (!db) return;
    add_msg(db, 1, 0, 10, 1000, "hello");
    add_msg(db, 2, 1, 11, 2000, "a reply");
    add_msg(db, 3, 0, 10, 90000000, "another day");

    oc_sum_window w;
    CHECK(oc_sum_load_window(db, 1, 0, 86400000, &w) == 0);
    CHECK(w.n == 2 && !strcmp(w.channel, "support") && !strcmp(w.msgs[1].author, "Bob"));
    oc_sum_window_free(&w);

    /* chunk(1,2) <- period(day 0); chunk(3) <- period(day 1) */
    oc_sum_new n[4];
    memset(n, 0, sizeof n);
    oc_sum_input in0[2] = { { OC_SUM_IN_MSG, 1, 0 }, { OC_SUM_IN_MSG, 2, 0 } };
    oc_sum_input in1[1] = { { OC_SUM_IN_NEW, 0, 0 } };
    oc_sum_input in2[1] = { { OC_SUM_IN_MSG, 3, 0 } };
    oc_sum_input in3[1] = { { OC_SUM_IN_NEW, 2, 0 } };
    oc_sum_input *ins[4] = { in0, in1, in2, in3 };
    int nin[4] = { 2, 1, 1, 1 };
    int kinds[4] = { OC_SUM_KIND_CHUNK, OC_SUM_KIND_PERIOD, OC_SUM_KIND_CHUNK, OC_SUM_KIND_PERIOD };
    int64_t starts[4] = { 1000, 0, 90000000, 86400000 }, ends[4] = { 2000, 86400000, 90000000, 172800000 };
    for (int i = 0; i < 4; i++) {
        n[i].kind = kinds[i];
        n[i].in = malloc((size_t)nin[i] * sizeof *n[i].in);
        memcpy(n[i].in, ins[i], (size_t)nin[i] * sizeof *n[i].in);
        n[i].n_in = nin[i];
        oc_sum_ikey('x', n[i].in, n[i].n_in, n[i].ikey, sizeof n[i].ikey);
        n[i].body = strdup("{\"overview\":\"x\",\"decisions\":[],\"actions\":[],\"problems\":[],\"facts\":[]}");
        n[i].start_ms = starts[i];
        n[i].end_ms = ends[i];
    }
    CHECK(oc_sum_store(db, 1, "v1", n, 4) == 0);
    CHECK(count_nodes(db) == 4);
    int64_t id = 0;
    char *body = NULL;
    CHECK(oc_sum_find_period(db, 1, 0, 86400000, 0, "v1", 0, &id, &body, NULL, 0) == 1 && id == n[1].id);
    free(body);
    body = NULL;
    /* An older version is served only when asked for. */
    CHECK(oc_sum_find_period(db, 1, 0, 86400000, 0, "v2", 0, &id, &body, NULL, 0) == 0);
    char got[32] = "";
    CHECK(oc_sum_find_period(db, 1, 0, 86400000, 0, "v2", 1, &id, &body, got, sizeof got) == 1 && !strcmp(got, "v1"));
    free(body);

    /* An edit purges the chunk and the period above it, and nothing else. */
    sqlite3_exec(db, "UPDATE messages SET edited_at_ms=5 WHERE id=2;", NULL, NULL, NULL);
    CHECK(oc_sum_purge(db, 1, 2, 1, 2000) == 2);
    CHECK(count_nodes(db) == 2);
    /* The guard: a batch read before the edit is refused, and nothing is kept. */
    for (int i = 0; i < 2; i++) n[i].id = 0;
    CHECK(oc_sum_store(db, 1, "v1", n, 2) == 1);
    CHECK(count_nodes(db) == 2);
    /* A new message on day 1 purges that day's period (and only it). */
    add_msg(db, 4, 0, 11, 95000000, "new");
    CHECK(oc_sum_purge(db, 1, 4, 4, 95000000) == 1);
    CHECK(count_nodes(db) == 1);
    oc_sum_new_free(n, 4);
    sqlite3_close(db);
    unlink(path);
}

/* --- the load gate -------------------------------------------------------------- */

typedef struct { uint64_t busy, total, mem, net, now; } fake;
static int f_cpu(void *c, uint64_t *b, uint64_t *t) { fake *f = c; *b = f->busy; *t = f->total; return 0; }
static int f_mem(void *c, uint64_t *m) { *m = ((fake *)c)->mem; return 0; }
static int f_net(void *c, uint64_t *n) { *n = ((fake *)c)->net; return 0; }
static uint64_t f_now(void *c) { return ((fake *)c)->now; }

static void tick(fake *f, unsigned busy_pct, uint64_t mem, uint64_t net) {
    f->now += SUM_LOAD_SAMPLE_MS;
    f->total += 1000;
    f->busy += busy_pct * 10;
    f->mem = mem;
    f->net += net;
}

static void test_gate(void) {
    fake f = { 0, 0, 4096, 0, 1000 };
    oc_sum_probe p = { f_cpu, f_mem, f_net, f_now, &f };
    oc_sum_load g;
    oc_sum_load_init(&g, &p);
    CHECK(oc_sum_load_busy(&g) == 1);             /* the first reading is a baseline */
    tick(&f, 10, 4096, 0);
    CHECK(oc_sum_load_busy(&g) == 0);
    CHECK(!oc_sum_load_settled(&g));
    for (unsigned i = 0; i < SUM_IDLE_SETTLE_MS / SUM_LOAD_SAMPLE_MS; i++) { tick(&f, 10, 4096, 0); oc_sum_load_busy(&g); }
    CHECK(oc_sum_load_settled(&g));
    tick(&f, 90, 4096, 0);
    CHECK(oc_sum_load_busy(&g) == 1);             /* busy CPU pauses */
    CHECK(!oc_sum_load_settled(&g));
    tick(&f, 60, 4096, 0);
    CHECK(oc_sum_load_busy(&g) == 1);             /* above the resume line: still paused */
    tick(&f, 30, 4096, 0);
    CHECK(oc_sum_load_busy(&g) == 0);
    tick(&f, 10, 100, 0);
    CHECK(oc_sum_load_busy(&g) == 1);             /* low memory pauses */
    CHECK(!oc_sum_load_should_unload(&g));
    for (unsigned i = 0; i < SUM_MEM_UNLOAD_MS / SUM_LOAD_SAMPLE_MS; i++) { tick(&f, 10, 100, 0); oc_sum_load_busy(&g); }
    CHECK(oc_sum_load_should_unload(&g));         /* ...and, kept low, unloads */
    tick(&f, 10, 4096, (uint64_t)SUM_NET_BUSY_BPS * 2);
    CHECK(oc_sum_load_busy(&g) == 1);             /* people's traffic pauses */
    CHECK(!oc_sum_load_should_unload(&g));
}

/* --- the whole build, over a stub model ----------------------------------------- */

typedef struct { int calls, rollups; } stub;
static void *s_open(void *ctx, char *err, size_t cap) { (void)err; (void)cap; return ctx; }
static void s_close(void *h) { (void)h; }
static int s_run(void *h, const char *system, const char *user, const char *grammar, int max_out,
                 oc_sum_gate_fn gate, void *gctx, char **out, oc_sum_run_stats *st, char *err, size_t cap) {
    (void)user; (void)grammar; (void)max_out; (void)err; (void)cap;
    stub *s = h;
    if (gate && gate(gctx)) return -1;
    if (st) memset(st, 0, sizeof *st);
    s->calls++;
    int roll = system == OC_SUM_SYSTEM_ROLLUP;
    s->rollups += roll;
    char buf[512];
    snprintf(buf, sizeof buf,
             "{\"overview\":\"P1 talked.\",\"decisions\":[{\"text\":\"%s\",\"by\":[\"P1\"],\"refs\":[\"%s\"]}],"
             "\"actions\":[],\"problems\":[],\"facts\":[]}", roll ? "agreed overall" : "agreed", roll ? "i1" : "m1");
    *out = strdup(buf);
    return 0;
}

typedef struct { sqlite3 *db; int stores; char last[2048]; int refused; } sink;
static int k_store(void *ctx, const oc_sum_answer *a, oc_sum_new *nodes, int n) {
    sink *k = ctx;
    k->stores++;
    if (!a->ok) { k->refused++; return 0; }
    int rc = n ? oc_sum_store(k->db, a->channel, a->version, nodes, n) : 0;
    if (rc == 0) snprintf(k->last, sizeof k->last, "%s", a->body ? a->body : "");
    return rc;
}

static void test_build(void) {
    const char *path = "/tmp/oc_test_summary_build.db";
    sqlite3 *db = fresh_db(path);
    CHECK(db != NULL);
    if (!db) return;
    /* Two quiet-separated conversations on one day, one on the next. */
    add_msg(db, 1, 0, 10, 1 * 3600000, "We should ship on Friday.");
    add_msg(db, 2, 1, 11, 1 * 3600000 + MIN, "Agreed.");
    add_msg(db, 3, 0, 11, 5 * 3600000, "Lunch?");
    add_msg(db, 4, 0, 10, 30 * 3600000, "Shipped.");
    stub s = {0};
    oc_sum_engine e = { &s, "stub", s_open, s_close, s_run };
    sink k = { db, 0, "", 0 };
    oc_sum_worker_cfg cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.db_path = path;
    cfg.engine = &e;
    cfg.sink.store = k_store;
    cfg.sink.ctx = &k;
    cfg.threshold = SUM_THRESHOLD_TOKENS;
    cfg.gap_ms = SUM_GAP_MS;
    char err[256] = "";
    /* Two days: two chunks and a rollup for the first day, one chunk for the
     * second, and the rollup of the two days. */
    CHECK(oc_sum_build_now(&cfg, 1, 0, 2 * 86400000ll, 0, err, sizeof err) == 0);
    CHECK(s.calls == 5 && s.rollups == 2);
    CHECK(strstr(k.last, "\"summary\":") && strstr(k.last, "agreed overall") && strstr(k.last, "\"10\":\"Ann\""));
    int stored = count_nodes(db);
    CHECK(stored >= 6);
    /* Again: everything is found, nothing is asked. */
    int before = s.calls;
    CHECK(oc_sum_build_now(&cfg, 1, 0, 2 * 86400000ll, 0, err, sizeof err) == 0);
    CHECK(s.calls == before);
    /* An edit on day two purges its pieces and the whole; day one is reused. */
    sqlite3_exec(db, "UPDATE messages SET edited_at_ms=7, body=CAST('Shipped late.' AS BLOB) WHERE id=4;", NULL, NULL, NULL);
    CHECK(oc_sum_purge(db, 1, 4, 4, 30 * 3600000) >= 2);
    before = s.calls;
    CHECK(oc_sum_build_now(&cfg, 1, 0, 2 * 86400000ll, 0, err, sizeof err) == 0);
    CHECK(s.calls == before + 2);   /* day two's chunk, and the two days' rollup */
    sqlite3_close(db);
    unlink(path);
    char wal[128];
    snprintf(wal, sizeof wal, "%s-wal", path);
    unlink(wal);
    snprintf(wal, sizeof wal, "%s-shm", path);
    unlink(wal);
}

/* An answer that runs past its budget is asked again, with room for one item
 * of each kind, and the piece is kept. */
static int over_calls;
static int o_run(void *h, const char *system, const char *user, const char *grammar, int max_out,
                 oc_sum_gate_fn gate, void *gctx, char **out, oc_sum_run_stats *st, char *err, size_t cap) {
    (void)h; (void)system; (void)user; (void)max_out; (void)gate; (void)gctx;
    if (st) memset(st, 0, sizeof *st);
    over_calls++;
    if (strstr(grammar, "){0,2}")) {   /* the full grammar: three of a kind */
        snprintf(err, cap, "the answer ran past 2048 tokens");
        return -1;
    }
    *out = strdup("{\"overview\":\"short\",\"decisions\":[],\"actions\":[],\"problems\":[],\"facts\":[]}");
    return 0;
}

static void test_retry(void) {
    const char *path = "/tmp/oc_test_summary_retry.db";
    sqlite3 *db = fresh_db(path);
    CHECK(db != NULL);
    if (!db) return;
    add_msg(db, 1, 0, 10, 3600000, "One thing.");
    static int handle;   /* the stub's open returns its context: any non-NULL */
    oc_sum_engine e = { &handle, "over", s_open, s_close, o_run };
    sink k = { db, 0, "", 0 };
    oc_sum_worker_cfg cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.db_path = path;
    cfg.engine = &e;
    cfg.sink.store = k_store;
    cfg.sink.ctx = &k;
    cfg.threshold = SUM_THRESHOLD_TOKENS;
    cfg.gap_ms = SUM_GAP_MS;
    char err[256] = "";
    over_calls = 0;
    CHECK(oc_sum_build_now(&cfg, 1, 0, 86400000ll, 0, err, sizeof err) == 0);
    CHECK(over_calls == 2);
    CHECK(strstr(k.last, "short") != NULL);
    sqlite3_close(db);
    unlink(path);
}

int run_summary_tests(void) {
    test_cut();
    test_check();
    test_store();
    test_gate();
    test_build();
    test_retry();
    return failures;
}
