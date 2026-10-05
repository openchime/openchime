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
#include "sum_fetch.h"
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

/* --- lines, the prompt and reading an answer ----------------------------------- */

static void test_parse(void) {
    oc_sum_msg m[] = {
        { 100, 0, 10, 0, 0, "Ann", "Ticket 6701 is open; Bob please look.", },
        { 101, 100, 11, MIN, 0, "Bob", "On it.", },
    };
    oc_sum_cut c;
    CHECK(oc_sum_cut_build(m, 2, 1000, 45 * MIN, &c) == 0);
    oc_sum_lines l = {0};
    oc_sum_people pp = {0};
    CHECK(oc_sum_chunk_lines(m, &c.pieces[0].chunks[0], &l, &pp) == 0);
    CHECK(l.n == 2 && !strcmp(l.v[0].text, "Ann: Ticket 6701 is open; Bob please look.") && l.v[1].indent == 1);
    CHECK(pp.n == 2 && pp.id[1] == 11 && !strcmp(pp.name[1], "Bob"));

    /* The prompt numbers the lines, carries no clock times, and asks for 30% of
     * their words (11 words: at most 4). */
    oc_sum_buf prompt = {0}, out = {0};
    CHECK(oc_sum_prompt("Messages from #support.", &l, &prompt) == 0);
    CHECK(strstr(prompt.p, "[1] Ann: Ticket") && strstr(prompt.p, "  [2] Bob: On it.") && strstr(prompt.p, "at most 4 words"));
    CHECK(!strstr(prompt.p, "00:"));
    const char *ans =
        "Overview: Ann asked Bob about ticket 6701.\n"
        "Decisions:\n"
        "Actions:\n"
        "- Bob: look at ticket 6701 (open) [1][2]\n"
        "- Bob: look at ticket 6701 (open) [1]\n"
        "- Carol: someone invented (open) [1]\n"
        "Problems:\n"
        "- ticket 9999 is broken (open) [1]\n"
        "- cites nothing real (open) [7]\n"
        "Facts:\n"
        "- Ticket 6701 [1]\n";
    int dropped = 0;
    int kept = oc_sum_parse(ans, &l, &pp, &out, &dropped);
    /* Kept: the first action and the fact. Dropped: the repeat, the person not
     * there, the invented number, the line not there. */
    CHECK(kept == 2);
    CHECK(dropped == 4);
    CHECK(out.p && strstr(out.p, "\"who\":11,\"what\":\"look at ticket 6701\",\"refs\":[100,101],\"status\":\"open\""));
    CHECK(out.p && strstr(out.p, "\"overview\":\"Ann asked Bob about ticket 6701.\""));
    CHECK(out.p && strstr(out.p, "\"facts\":[{\"text\":\"Ticket 6701\",\"refs\":[100]}]"));
    CHECK(out.p && strstr(out.p, "\"decisions\":[]"));
    oc_sum_buf_free(&out);
    /* An overview stating a number no line has is dropped, the bullets kept. */
    CHECK(oc_sum_parse("Overview: 42 tickets.\nDecisions:\n- look [1]\nActions:\nProblems:\nFacts:\n", &l, &pp, &out,
                       &dropped) == 1);
    CHECK(out.p && strstr(out.p, "\"overview\":\"\"") && dropped == 1);
    oc_sum_buf_free(&out);
    CHECK(oc_sum_parse("not a summary", &l, &pp, &out, NULL) == -1);
    oc_sum_buf_free(&out);

    /* Written freely, it is read as written: headings inside a line and in
     * markdown, citations in parentheses, ranges and lists, a status anywhere,
     * a bullet with no mark. Kept: the decision, the action and the fact.
     * Dropped: a line citing nothing, an action with no status, a problem that
     * runs on into numbers its line does not hold. */
    const char *free_ans =
        "Here is the summary.\n"
        "**Overview:** Ann asked Bob about ticket 6701 [1]. Decisions: - Bob looks first (1-2)\n"
        "Actions:\n"
        "- bob: (open) look at ticket 6701 (1, 2)\n"
        "- Ann: wait\n"
        "- Ann: wait [1]\n"
        "Problems: - nothing (open) [1] - (3) - (4) - (5)\n"
        "## Facts:\n"
        "1. Ticket 6701 [1, 2]\n";
    kept = oc_sum_parse(free_ans, &l, &pp, &out, &dropped);
    CHECK(kept == 3);
    CHECK(dropped == 3);
    CHECK(out.p && strstr(out.p, "\"overview\":\"Ann asked Bob about ticket 6701.\""));
    CHECK(out.p && strstr(out.p, "\"decisions\":[{\"text\":\"Bob looks first\",\"by\":[],\"refs\":[100,101]}]"));
    CHECK(out.p && strstr(out.p, "\"who\":11,\"what\":\"look at ticket 6701\",\"refs\":[100,101],\"status\":\"open\""));
    CHECK(out.p && strstr(out.p, "\"facts\":[{\"text\":\"Ticket 6701\",\"refs\":[100,101]}]"));
    oc_sum_buf_free(&out);

    /* A stored summary reads back as lines, each standing for its messages;
     * the overview for all of them. */
    oc_sum_lines up = {0};
    oc_sum_people upp = {0};
    const char *body =
        "{\"overview\":\"Bob took the ticket.\",\"decisions\":[],"
        "\"actions\":[{\"who\":11,\"what\":\"look at ticket 6701\",\"refs\":[100,101],\"status\":\"open\"}],"
        "\"problems\":[],\"facts\":[{\"text\":\"Ticket 6701\",\"refs\":[100]}]}";
    CHECK(oc_sum_body_lines(body, NULL, NULL, "On Fri 04 Sep:", &up, &upp) == 0);
    CHECK(up.n == 3);
    if (up.n == 3) {
        CHECK(!strcmp(up.v[0].text, "Overview: Bob took the ticket.") && up.v[0].n_refs == 2 && up.v[0].label);
        CHECK(!strcmp(up.v[1].text, "Action (open): someone: look at ticket 6701") && !up.v[1].label);
        CHECK(!strcmp(up.v[2].text, "Fact: Ticket 6701") && up.v[2].n_refs == 1 && up.v[2].refs[0] == 100);
    }
    CHECK(upp.n == 1 && upp.id[0] == 11);
    oc_sum_lines_free(&up);
    oc_sum_people_free(&upp);

    oc_sum_buf_free(&prompt);
    oc_sum_lines_free(&l);
    oc_sum_people_free(&pp);
    oc_sum_cut_free(&c);
}

/* A message too big for a chunk splits at paragraphs, then lines, sentences and
 * words, and loses nothing. */
static void test_split(void) {
    static char text[6000];
    text[0] = '\0';
    for (int p = 0; p < 6; p++) {
        for (int s = 0; s < 8; s++) strcat(text, "This sentence is about twenty-five bytes. ");
        strcat(text, "\n\n");
    }
    char **parts = NULL;
    int n = 0;
    CHECK(oc_sum_split_text(text, 100, &parts, &n) == 0);
    CHECK(n >= 4);
    size_t total = 0;
    oc_sum_buf joined = {0};
    for (int i = 0; i < n; i++) {
        CHECK(strlen(parts[i]) <= 100 * SUM_BYTES_PER_TOKEN);
        total += strlen(parts[i]);
        oc_sum_buf_puts(&joined, parts[i]);
    }
    CHECK(total == strlen(text) && !strcmp(joined.p, text));
    /* Whole paragraphs where they fit. */
    CHECK(n > 0 && strstr(parts[0], "\n\n") != NULL);
    oc_sum_buf_free(&joined);
    oc_sum_parts_free(parts, n);
    /* One word longer than a part: cut, never inside a character. */
    static char word[900];
    memset(word, 0, sizeof word);
    for (int i = 0; i + 2 < (int)sizeof word - 1; i += 2) { word[i] = (char)0xC3; word[i + 1] = (char)0xA9; }
    CHECK(oc_sum_split_text(word, 50, &parts, &n) == 0);
    for (int i = 0; i < n; i++) CHECK(((unsigned char)parts[i][0] & 0xC0) != 0x80);
    oc_sum_parts_free(parts, n);
}

/* The CPU check: local summaries only where the model's instructions exist. */
static int has_all(const char *f) { (void)f; return 1; }
static int no_avx2(const char *f) { return strcmp(f, "avx2") != 0; }
static void test_cpu(void) {
    char err[128] = "";
    CHECK(oc_sum_cpu_ok(has_all, err, sizeof err) == 1);
#if defined(__x86_64__)
    CHECK(oc_sum_cpu_ok(no_avx2, err, sizeof err) == 0 && strstr(err, "avx2"));
#endif
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

/* The stub: every call is checked to be the one summarize step -- the same
 * system prompt and the same instructions at every level -- and answers with a
 * short summary citing line 1. In "grow" mode it answers with all its input
 * again, which never gets shorter. */
typedef struct {
    int calls, leaf, rollup, part, other_system, other_prompt, grow;
} stub;
static void *s_open(void *ctx, char *err, size_t cap) { (void)err; (void)cap; return ctx; }
static void s_close(void *h) { (void)h; }
static int s_run(void *h, const char *system, const char *user, int max_out,
                 oc_sum_gate_fn gate, void *gctx, char **out, oc_sum_run_stats *st, char *err, size_t cap) {
    (void)max_out; (void)err; (void)cap;
    stub *s = h;
    if (gate && gate(gctx)) return -1;
    if (st) memset(st, 0, sizeof *st);
    s->calls++;
    if (system != OC_SUM_SYSTEM) s->other_system++;
    if (!strstr(user, "Answer in exactly this form")) s->other_prompt++;
    if (strstr(user, "Messages from")) s->leaf++;
    else if (strstr(user, "Summaries of consecutive parts")) s->rollup++;
    else if (strstr(user, "One long message")) s->part++;
    oc_sum_buf b = {0};
    if (s->grow) {
        oc_sum_buf_puts(&b, "Overview:");
        for (const char *p = user; (p = strchr(p, '[')) != NULL; p++) {
            /* Only the numbered lines, not the example in the instructions. */
            const char *b0 = p;
            while (b0 > user && b0[-1] == ' ') b0--;
            if (b0 > user && b0[-1] != '\n') continue;
            const char *e = strchr(p, '\n');
            const char *t = strchr(p, ']');
            if (t && e && t < e) { oc_sum_buf_puts(&b, " "); oc_sum_buf_add(&b, t + 2, (size_t)(e - t - 2)); oc_sum_buf_puts(&b, " and more words"); }
        }
        oc_sum_buf_puts(&b, "\nDecisions:\nActions:\nProblems:\nFacts:\n");
    } else {
        oc_sum_buf_puts(&b, "Overview: They talked.\nDecisions:\n- agreed [1]\nActions:\nProblems:\nFacts:\n");
    }
    *out = b.p;
    return 0;
}

typedef struct { sqlite3 *db; int stores; char last[4096]; char err[600]; } sink;
static int k_store(void *ctx, const oc_sum_answer *a, oc_sum_new *nodes, int n) {
    sink *k = ctx;
    k->stores++;
    if (!a->ok) { snprintf(k->err, sizeof k->err, "%s", a->err ? a->err : ""); return 0; }
    int rc = n ? oc_sum_store(k->db, a->channel, a->version, nodes, n) : 0;
    if (rc == 0 && a->body) snprintf(k->last, sizeof k->last, "%s", a->body);
    return rc;
}

static void setup(oc_sum_worker_cfg *cfg, const char *path, oc_sum_engine *e, sink *k, size_t threshold) {
    memset(cfg, 0, sizeof *cfg);
    cfg->db_path = path;
    cfg->engine = e;
    cfg->sink.store = k_store;
    cfg->sink.ctx = k;
    cfg->threshold = threshold;
    cfg->gap_ms = SUM_GAP_MS;
}

static int count_where(sqlite3 *db, const char *sql) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    int n = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int(st, 0) : -1;
    sqlite3_finalize(st);
    return n;
}

static void drop_db(const char *path) {
    char p[160];
    unlink(path);
    snprintf(p, sizeof p, "%s-wal", path);
    unlink(p);
    snprintf(p, sizeof p, "%s-shm", path);
    unlink(p);
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
    sink k = { db, 0, "", "" };
    oc_sum_worker_cfg cfg;
    setup(&cfg, path, &e, &k, SUM_THRESHOLD_TOKENS);
    char err[600] = "";
    /* Three chunks, summarized; their summaries fit, so one more call over them
     * makes the two days' summary -- the same step every time. */
    CHECK(oc_sum_build_now(&cfg, 1, 0, 2 * 86400000ll, 0, err, sizeof err) == 0);
    CHECK(s.calls == 4 && s.leaf == 3 && s.rollup == 1);
    CHECK(s.other_system == 0 && s.other_prompt == 0);
    CHECK(strstr(k.last, "\"summary\":") && strstr(k.last, "agreed"));
    /* Again: everything is found, nothing is asked. */
    int before = s.calls;
    CHECK(oc_sum_build_now(&cfg, 1, 0, 2 * 86400000ll, 0, err, sizeof err) == 0);
    CHECK(s.calls == before);
    /* The first day alone: its two chunks are reused, one call puts them together. */
    before = s.calls;
    CHECK(oc_sum_build_now(&cfg, 1, 0, 86400000ll, 0, err, sizeof err) == 0);
    CHECK(s.calls == before + 1);
    /* An edit on day two purges its chunk and the two days; day one is reused. */
    sqlite3_exec(db, "UPDATE messages SET edited_at_ms=7, body=CAST('Shipped late.' AS BLOB) WHERE id=4;", NULL, NULL, NULL);
    CHECK(oc_sum_purge(db, 1, 4, 4, 30 * 3600000) >= 2);
    before = s.calls;
    CHECK(oc_sum_build_now(&cfg, 1, 0, 2 * 86400000ll, 0, err, sizeof err) == 0);
    CHECK(s.calls == before + 2);   /* day two's chunk, and the two days */
    /* Idle work makes the chunks; a request then only puts them together. */
    add_msg(db, 5, 0, 10, 50 * 3600000, "Next week.");
    add_msg(db, 6, 0, 11, 55 * 3600000, "Sure.");
    before = s.calls;
    CHECK(oc_sum_chunks_now(&cfg, 1, 2 * 86400000ll, 3 * 86400000ll, err, sizeof err) == 0);
    CHECK(s.calls == before + 2);
    before = s.calls;
    CHECK(oc_sum_build_now(&cfg, 1, 2 * 86400000ll, 3 * 86400000ll, 0, err, sizeof err) == 0);
    CHECK(s.calls == before + 1 && s.leaf == 6);
    sqlite3_close(db);
    drop_db(path);
}

/* The recursion: with a small threshold, a day of many chunks becomes sections,
 * sections of sections, and one summary; a thread too big for a chunk and a
 * message too big for one are summarized the same way. */
static void test_recursion(void) {
    const char *path = "/tmp/oc_test_summary_recursion.db";
    sqlite3 *db = fresh_db(path);
    CHECK(db != NULL);
    if (!db) return;
    int64_t id = 1;
    for (int i = 0; i < 20; i++)       /* twenty conversations an hour apart */
        add_msg(db, id++, 0, 10 + (i & 1), (int64_t)i * 3600000 + 60000, "A short note about the plan for today.");
    stub s = {0};
    oc_sum_engine e = { &s, "stub", s_open, s_close, s_run };
    sink k = { db, 0, "", "" };
    oc_sum_worker_cfg cfg;
    setup(&cfg, path, &e, &k, 40);
    char err[600] = "";
    CHECK(oc_sum_build_now(&cfg, 1, 0, 86400000ll, 0, err, sizeof err) == 0);
    CHECK(s.leaf == 20 && s.rollup > 4 && s.other_system == 0 && s.other_prompt == 0);
    /* Three levels at least: a section built from sections. */
    CHECK(count_where(db, "SELECT COUNT(*) FROM summary_inputs i JOIN summary_nodes p ON p.id=i.parent_id "
                          "JOIN summary_nodes c ON c.id=i.child_id WHERE i.child_kind=1 AND p.kind=2 AND c.kind=2;") > 0);
    CHECK(count_where(db, "SELECT COUNT(*) FROM summary_nodes WHERE kind=3;") == 1);
    /* Nothing was cut short: every stored body is whole JSON. */
    CHECK(count_where(db, "SELECT COUNT(*) FROM summary_nodes WHERE json_valid(body)=0;") == 0);

    /* A thread too big for a chunk. */
    for (int i = 0; i < 6; i++)
        add_msg(db, 100 + i, i ? 100 : 0, 10 + (i & 1), 2 * 86400000ll + i * MIN, "A longer reply in a busy thread, with a few more words in it.");
    s.leaf = s.rollup = 0;
    CHECK(oc_sum_build_now(&cfg, 1, 2 * 86400000ll, 3 * 86400000ll, 0, err, sizeof err) == 0);
    CHECK(s.leaf >= 2 && s.rollup >= 1);
    CHECK(count_where(db, "SELECT COUNT(*) FROM summary_nodes WHERE kind=1 AND root_id=100;") >= 1 ||
          count_where(db, "SELECT COUNT(*) FROM summary_nodes WHERE kind=2 AND root_id=100;") >= 1);

    /* A message too big for a chunk: parts, then the parts together. */
    static char big[1200];
    big[0] = '\0';
    while (strlen(big) + 40 < sizeof big) strcat(big, "One sentence of a very long message. ");
    add_msg(db, 200, 0, 10, 4 * 86400000ll, big);
    s.part = s.rollup = 0;
    CHECK(oc_sum_build_now(&cfg, 1, 4 * 86400000ll, 5 * 86400000ll, 0, err, sizeof err) == 0);
    CHECK(s.part >= 4 && s.rollup >= 1);
    CHECK(count_where(db, "SELECT COUNT(*) FROM summary_nodes WHERE ikey LIKE 'q:%';") == s.part);
    /* Every part's node is built on the message, so an edit purges them all. */
    sqlite3_exec(db, "UPDATE messages SET edited_at_ms=9 WHERE id=200;", NULL, NULL, NULL);
    CHECK(oc_sum_purge(db, 1, 200, 200, 4 * 86400000ll) >= s.part + 1);
    CHECK(count_where(db, "SELECT COUNT(*) FROM summary_nodes WHERE ikey LIKE 'q:%';") == 0);

    /* A level that does not get shorter stops the build, reported, not cut. */
    stub g = {0};
    g.grow = 1;
    oc_sum_engine ge = { &g, "grow", s_open, s_close, s_run };
    oc_sum_worker_cfg gc;
    setup(&gc, path, &ge, &k, 40);
    CHECK(oc_sum_build_now(&gc, 1, 0, 86400000ll, 0, err, sizeof err) == -1);
    CHECK(strstr(err, "did not get shorter") != NULL);
    sqlite3_close(db);
    drop_db(path);
}

int run_summary_tests(void) {
    test_cut();
    test_parse();
    test_split();
    test_cpu();
    test_store();
    test_gate();
    test_build();
    test_recursion();
    return failures;
}
