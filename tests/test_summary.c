/* Channel and DM summaries (REQ-310, ARCH-116): the pieces, the checks, the
 * store and its purge, the load gate, and the worker's whole build over a stub
 * model -- everything but a real model, which make test never loads. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>

#include "check.h"
#include "dbwriter.h"
#include "migrate.h"
#include "sum_core.h"
#include "sum_engine.h"
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
    /* A run of messages saying the same but for their numbers is one line,
     * whoever wrote them; the line stands for all of them and shows three. */
    oc_sum_msg m[] = {
        { 100, 0, 10, 0, 0, "Ann", "Ticket 6701 is open; Bob please look.", },
        { 101, 100, 11, MIN, 0, "Bob", "On it.", },
        { 102, 0, 12, 2 * MIN, 0, "Bot", "Order 55 cancelled.", },
        { 103, 0, 12, 3 * MIN, 0, "Bot", "Order 56 cancelled.", },
        { 104, 0, 13, 4 * MIN, 0, "Cara", "Order 57 cancelled.", },
        { 105, 0, 12, 5 * MIN, 0, "Bot", "Order 58 cancelled.", },
    };
    oc_sum_cut c;
    CHECK(oc_sum_cut_build(m, 6, 1000, 45 * MIN, &c) == 0);
    oc_sum_lines l = {0};
    CHECK(c.n == 1 && oc_sum_chunk_lines(m, &c.pieces[0].chunks[0], &l) == 0);
    CHECK(l.n == 3);
    if (l.n == 3) {
        CHECK(!strcmp(l.v[0].text, "Ann: Ticket 6701 is open; Bob please look.") && !strcmp(l.v[0].by, "Ann"));
        CHECK(l.v[1].indent == 1 && l.v[1].n_all == 1 && l.v[1].all[0] == 101);
        CHECK(!strcmp(l.v[2].text, "Bot: Order 55 cancelled. (x4)") && l.v[2].n_all == 4 && l.v[2].n_refs == 3 &&
              l.v[2].refs[2] == 104 && !strcmp(l.v[2].by, "Bot\nCara"));
    }
    CHECK(oc_sum_lines_words(&l) == 16);

    /* How many notes a call may write: one to every SUM_TOKENS_PER_NOTE tokens
     * read, at least SUM_NOTES_MIN, at most the answer room -- and never more
     * than half the lines, which is what makes every merge smaller. */
    CHECK(oc_sum_notes_for(100, 40) == SUM_NOTES_MIN);
    CHECK(oc_sum_notes_for(2500, 40) == 10);
    CHECK(oc_sum_notes_for(100000, 1000) == SUM_NOTES_ROOM);
    CHECK(oc_sum_notes_for(2500, 6) == 3 && oc_sum_notes_for(2500, 1) == 1);
    /* Never fewer than SUM_NOTES_FLOOR when there are that many lines: a few
     * unrelated lines are not forced into one note. */
    CHECK(oc_sum_notes_for(2500, 3) == 3 && oc_sum_notes_for(2500, 2) == 2 && oc_sum_notes_for(2500, 4) == 3);
    /* Notes: the lines numbered, at most as many as asked, held to a grammar
     * of notes citing lines that exist. */
    oc_sum_buf prompt = {0}, g = {0}, out = {0};
    CHECK(oc_sum_notes_prompt("Messages from #support.", &l, 8, &prompt, &g) == 0);
    CHECK(strstr(prompt.p, "[1] Ann: Ticket") && strstr(prompt.p, "  [2] Bob: On it.") &&
          strstr(prompt.p, "- [line number] kind | topic | what happened") && strstr(prompt.p, "at most 8,"));
    CHECK(strstr(g.p, "root ::= note{1,8}\n") && strstr(g.p, "id ::= \"1\" | \"2\" | \"3\"\n"));
    oc_sum_buf_free(&prompt);
    oc_sum_buf_free(&g);
    const char *notes =
        "- [1][2] action | Ticket 6701 | Ann asked Bob to look at ticket 6701, and he took it.\n"
        "- [1] action | ticket | ann asked bob to look at ticket 6701.\n"
        "- [3] event | Orders | Four orders were cancelled. Order 9999 was refunded.\n"
        "- [2] decision | Ticket | Cara closed the ticket.\n"
        "- [3] event | Orders | None\n";
    int dropped = 0;
    CHECK(oc_sum_parse_notes(notes, &l, &out, &dropped, 0) == 2);
    /* Dropped: the repeat, the sentence with a number no line has, the note
     * naming someone its line does not, and the one saying nothing. */
    CHECK(dropped == 3);
    CHECK(out.p && !strcmp(out.p,
        "{\"notes\":[{\"kind\":\"action\",\"topic\":\"Ticket 6701\","
        "\"text\":\"Ann asked Bob to look at ticket 6701, and he took it.\",\"refs\":[100,101],\"all\":[100,101],"
        "\"by\":[\"Ann\",\"Bob\"]},"
        "{\"kind\":\"event\",\"topic\":\"Orders\",\"text\":\"Four orders were cancelled.\",\"refs\":[102,103,104],"
        "\"all\":[102,103,104,105],\"by\":[\"Bot\",\"Cara\"]}]}"));
    /* Read back as lines for the next level: each stands for what its note does. */
    oc_sum_lines up = {0};
    CHECK(oc_sum_notes_lines(out.p, "On Fri 04 Sep:", &up) == 0 && up.n == 2);
    if (up.n == 2) {
        CHECK(!strcmp(up.v[0].text, "(action) Ticket 6701: Ann asked Bob to look at ticket 6701, and he took it.") &&
              up.v[0].kind == 'a' && up.v[0].label && up.v[0].n_all == 2 && !strcmp(up.v[0].by, "Ann\nBob"));
        CHECK(up.v[1].kind == 'e' && up.v[1].n_all == 4 && up.v[1].n_refs == 3 && !up.v[1].label);
    }
    CHECK(oc_sum_body_empty(out.p) == 0 && oc_sum_body_empty("{\"notes\":[]}") == 1);
    oc_sum_buf_free(&out);
    /* Not notes: nothing cited, no "|". */
    CHECK(oc_sum_parse_notes("Here are some notes.\n", &l, &out, &dropped, 0) == 0);
    /* One more note than asked for: the first `notes_max` kept are taken. */
    oc_sum_buf_free(&out);
    memset(&out, 0, sizeof out);
    CHECK(oc_sum_parse_notes("- [1] action | Ticket | Ann asked Bob to look at the ticket.\n"
                             "- [3] event | Orders | Four orders were cancelled.\n"
                             "- [2] decision | Ticket | Cara closed the ticket.\n", &l, &out, &dropped, 2) == 2);
    CHECK(strstr(out.p, "Four orders") && !strstr(out.p, "Cara closed"));
    oc_sum_buf_free(&out);

    /* The summary: sized to its span -- a word to every ten, between the floor
     * and the ceiling, never longer than the span; a topic to every hundred. */
    oc_sum_caps cs, cf, tiny, huge;
    oc_sum_caps_for(300, &cs);
    oc_sum_caps_for(4000, &cf);
    oc_sum_caps_for(12, &tiny);
    oc_sum_caps_for(20000, &huge);
    CHECK(cs.total == SUM_MIN_WORDS && cs.topics == 2 && cs.topics_min == 0 && cs.details_min == 0 && cs.more == 0);
    CHECK(cf.total == 400 && cf.topics == 4 && cf.topics_min == 2 && cf.details_min == 2 && cf.more == SUM_MORE_TOPICS);
    CHECK(tiny.total == 12);
    CHECK(huge.total == SUM_MAX_WORDS && huge.topics == SUM_MAX_TOPICS && huge.topics_min == 4);
    /* The room: what the shape holds at most, well above the total; the
     * answer's rail is sized to it, not to the total. */
    CHECK(cs.room == 60 + 2 * (6 + 80 + 3 * 40) + 5 * 40 && cs.room > 6 * cs.total);
    CHECK(cf.room == 60 + 4 * (6 + 80 + 3 * 40) + 5 * 40 + 10 * 6 && cf.room > cf.total);
    /* The length is asked for in sentences a part and a ceiling on the whole. */
    CHECK(oc_sum_final_prompt("Notes on #support.", &up, &cf, &prompt, &g) == 0);
    CHECK(strstr(prompt.p, "Overview: [line number]") && strstr(prompt.p, "More topics: <topic>; <topic>") &&
          strstr(prompt.p, "Write 2 to 4 topics") && strstr(prompt.p, "2 to 3 details of one sentence each") &&
          strstr(prompt.p, "one to three sentences") && strstr(prompt.p, "The overview is one sentence") &&
          !strstr(prompt.p, "words in all") && strstr(prompt.p, "Do not comment on"));
    CHECK(strstr(g.p, "root ::= ov topic{2,4} attn? more?\n") && strstr(g.p, "aitem ::=") && strstr(g.p, "det{2,3}"));
    oc_sum_buf_free(&prompt);
    oc_sum_buf_free(&g);
    CHECK(oc_sum_final_prompt("Messages.", &l, &cs, &prompt, &g) == 0);
    CHECK(strstr(g.p, "root ::= ov topic{0,2} attn?\n") && !strstr(prompt.p, "More topics"));
    oc_sum_buf_free(&prompt);
    oc_sum_buf_free(&g);

    /* Read back: the overview, topics ranked by weight (the decision first),
     * details, what needs attention, more topics; checked as notes are. */
    oc_sum_lines both = {0};
    CHECK(oc_sum_notes_lines("{\"notes\":[{\"kind\":\"event\",\"topic\":\"Orders\",\"text\":\"Four orders were "
                             "cancelled.\",\"refs\":[102],\"all\":[102,103,104,105],\"by\":[\"Bot\"]},"
                             "{\"kind\":\"decision\",\"topic\":\"Ticket\",\"text\":\"Bob took ticket 6701.\","
                             "\"refs\":[101],\"all\":[100,101],\"by\":[\"Ann\",\"Bob\"]}]}", NULL, &both) == 0);
    const char *ans =
        "Overview: [2] Bob took ticket 6701, and four orders were cancelled.\n"
        "## Orders\n"
        "[1] Four orders were cancelled.\n"
        "- [1] Four orders were cancelled.\n"
        "- [1] Order 77 was lost.\n"
        "## Ticket 6701\n"
        "[2] Bob took the ticket.\n"
        "- [2] Ann asked; Bob took it.\n"
        "Needs attention:\n"
        "- [2] action: Bob to fix ticket 6701\n"
        "- [2] question: Did Bot agree?\n"
        "More topics: 1. Lunch; 2. Parking [1][2]; orders; Lunch\n";
    oc_sum_final f;
    CHECK(oc_sum_parse_final(ans, &both, &cf, &f, &dropped) == 0);
    oc_sum_final_rank(&f);
    CHECK(f.ov.text && !strcmp(f.ov.text, "Bob took ticket 6701, and four orders were cancelled.") && f.ov.refs[0] == 101);
    CHECK(f.nt == 2);
    if (f.nt == 2) {
        CHECK(!strcmp(f.top[0].title, "Ticket 6701") && f.top[0].weight == 3 && f.top[0].n_all == 2);
        CHECK(!strcmp(f.top[1].title, "Orders") && f.top[1].nd == 0 && f.top[1].n_all == 4);   /* repeat, invented number */
    }
    /* Bot wrote none of the lines the question cites, nor is named in them. */
    CHECK(f.na == 1 && f.att_kind[0] == 'a' && !strcmp(f.att[0].text, "Bob to fix ticket 6701"));
    CHECK(f.nm == 2 && !strcmp(f.more[1], "Parking"));
    CHECK(dropped == 3);
    oc_sum_buf_free(&out);
    CHECK(oc_sum_final_json(&f, &out) == 0);
    CHECK(out.p && strstr(out.p, "{\"overview\":{\"text\":\"Bob took ticket 6701, and four orders were cancelled.\","
                                 "\"refs\":[101]},\"topics\":[{\"title\":\"Ticket 6701\",\"text\":\"Bob took the ticket.\","
                                 "\"refs\":[101],\"count\":2,\"people\":[\"Ann\",\"Bob\"],\"details\":[{\"text\":\"Ann "
                                 "asked; Bob took it.\",\"refs\":[101]}]}") &&
          strstr(out.p, "\"attention\":[{\"kind\":\"action\",\"text\":\"Bob to fix ticket 6701\",\"refs\":[101]}],"
                        "\"more\":[\"Lunch\",\"Parking\"],\"words\":0}"));
    CHECK(oc_sum_body_readable(out.p) && !oc_sum_body_readable("{\"refs\":[],\"topics\":[]}"));
    oc_sum_buf_free(&out);
    /* Held to a whole of 12 words: the lowest topic goes to "More topics", then
     * details and attention, then the account is cut at a sentence. */
    oc_sum_caps small = cf;
    small.total = 12;
    CHECK(oc_sum_final_fit(&f, &small) == 1);
    CHECK(f.nt <= 1 && f.na == 0 && !strcmp(f.more[0], "Orders"));
    oc_sum_final_free(&f);
    /* Not a summary. */
    CHECK(oc_sum_parse_final("I cannot help.\n", &both, &cf, &f, NULL) == -1);
    /* Citations written bare, as a model not held to the brackets writes them:
     * before the word, after it, a range, a list, with a colon; a number that
     * is part of a word or a time is not one. */
    CHECK(oc_sum_parse_final("1-2 Overview: Bob took the ticket.\n"
                             "## Ticket\n"
                             "2: Bob took the ticket.\n"
                             "- 1, 2 Four orders were cancelled.\n"
                             "- 2 Bob took it at noon.\n"
                             "Needs attention:\n"
                             "- 2 action: Bob to fix ticket 6701\n", &both, &cf, &f, &dropped) == 0);
    CHECK(f.ov.text && !strcmp(f.ov.text, "Bob took the ticket.") && f.ov.n_cited == 2);
    CHECK(f.nt == 1 && f.top[0].para.n_cited == 1 && f.top[0].para.cited[0] == 1);
    CHECK(f.nt == 1 && f.top[0].nd == 2 && f.top[0].det[0].n_cited == 2 && !strcmp(f.top[0].det[1].text, "Bob took it at noon."));
    CHECK(f.na == 1 && f.att_kind[0] == 'a');
    oc_sum_final_free(&f);
    CHECK(oc_sum_parse_final("Overview: 12th was the day.\n", &both, &cf, &f, NULL) == -1);
    /* Bracketed citations before the word, and topic titles without "##". */
    CHECK(oc_sum_parse_final("[1][2] Overview: Bob took the ticket.\n"
                             "[1][2] Ticket 6701\n"
                             "[2] Bob took the ticket.\n"
                             "- [1] Four orders were cancelled.\n"
                             "[1] Orders\n"
                             "[1] Four orders were cancelled.\n", &both, &cf, &f, &dropped) == 0);
    CHECK(f.ov.text && f.ov.n_cited == 2);
    CHECK(f.nt == 2 && !strcmp(f.top[0].title, "Ticket 6701") && f.top[0].nd == 1 && !strcmp(f.top[1].title, "Orders"));
    CHECK(f.nt == 2 && f.top[1].para.text && !strcmp(f.top[1].para.text, "Four orders were cancelled."));
    oc_sum_final_free(&f);

    /* The headings written as titles are the sections they name, not topics. */
    CHECK(oc_sum_parse_final("Overview: [1] Bob took the ticket.\n"
                             "## Ticket 6701\n"
                             "[2] Bob took the ticket.\n"
                             "## Needs attention\n"
                             "- [2] question: Ann asked who has the ticket.\n"
                             "## More topics: Orders; Returns\n", &both, &cf, &f, &dropped) == 0);
    CHECK(f.nt == 1 && !strcmp(f.top[0].title, "Ticket 6701"));
    CHECK(f.na == 1 && f.nm == 2 && !strcmp(f.more[1], "Returns"));
    oc_sum_final_free(&f);

    /* Cutting a part: at the last whole sentence that fits, else at a word. */
    char t1[] = "One two three. Four five six seven.";
    CHECK(oc_sum_cut_words(t1, 5) == 1 && !strcmp(t1, "One two three."));
    char t2[] = "One two three four five six.";
    CHECK(oc_sum_cut_words(t2, 3) == 1 && !strcmp(t2, "One two three"));
    CHECK(oc_sum_cut_words(t2, 9) == 0);
    CHECK(oc_sum_rewrite_prompt("Too long a part.", 3, &prompt, &g) == 0);
    CHECK(strstr(prompt.p, "This is 4 words:") && strstr(prompt.p, "at most 3 words") && strstr(g.p, "{3,24}"));
    oc_sum_buf_free(&prompt);
    oc_sum_buf_free(&g);

    oc_sum_lines_free(&both);
    oc_sum_lines_free(&up);
    oc_sum_lines_free(&l);
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
        n[i].body = strdup("{\"notes\":[{\"kind\":\"event\",\"topic\":\"x\",\"text\":\"x\",\"refs\":[],\"all\":[],\"by\":[]}]}");
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

/* The stub: every call is checked to carry the one system prompt, and answers
 * by its grammar -- notes, a summary or a rewrite -- citing line 1. In "fat"
 * mode its notes are the most it may write, each long, so merging is needed. */
typedef struct {
    int calls, leaf, merge, part, final, rewrite, shorten, other_system, fat;
    int long_finals;   /* the first this many summaries are written far over their total */
    int fail_finals;   /* the first this many summary calls fail outright */
    int runaway_finals; /* the first this many summary calls run away (hit their rail) */
} stub;
static void *s_open(void *ctx, char *err, size_t cap) { (void)err; (void)cap; return ctx; }
static void s_close(void *h) { (void)h; }
static int s_run(void *h, const char *system, const char *user, const char *grammar, int max_out,
                 oc_sum_gate_fn gate, void *gctx, char **out, oc_sum_run_stats *st, char *err, size_t cap) {
    (void)max_out; (void)err; (void)cap;
    stub *s = h;
    if (gate && gate(gctx)) return -1;
    if (st) memset(st, 0, sizeof *st);
    s->calls++;
    if (system != OC_SUM_SYSTEM) s->other_system++;
    oc_sum_buf b = {0};
    if (grammar && strstr(grammar, "note ::=")) {
        if (strstr(user, "Messages from")) s->leaf++;
        else if (strstr(user, "Notes on consecutive parts")) s->merge++;
        else if (strstr(user, "One long message")) s->part++;
        /* As many notes as the grammar allows ("note{1,N}"), when fat. */
        const char *brace = strstr(grammar, "note{1,");
        int n = s->fat && brace ? atoi(brace + 7) : 1;
        for (int i = 0; i < n && i < 26; i++)
            oc_sum_buf_printf(&b, "- [1] decision | plan %c | They agreed to ship the %c release on the planned day, "
                                  "as discussed at length by everyone involved in the thread, after the usual "
                                  "back and forth about what was ready and what was not and who would do it.\n",
                              'a' + i, 'a' + i);
    } else if (grammar && strstr(grammar, "ov ::=") && strstr(user, "This summary is")) {
        /* Asked to write it again shorter: shorter it is, unless still told
         * to write long. */
        s->shorten++;
        if (s->long_finals > 0) {
            s->long_finals--;
            oc_sum_buf_puts(&b, "Overview: [1] They agreed to ship.\n## Plan\n[1] They agreed to ship.\n");
            for (int i = 0; i < 40; i++)
                oc_sum_buf_puts(&b, "- [1] They agreed to ship it on the day they had planned, after the usual discussion.\n");
        } else {
            oc_sum_buf_puts(&b, "Overview: [1] They agreed to ship.\n## Plan\n[1] They agreed to ship.\n- [1] agreed\n");
        }
    } else if (grammar && strstr(grammar, "ov ::=")) {
        s->final++;
        if (s->fail_finals > 0) { s->fail_finals--; snprintf(err, cap, "the server is down"); return -1; }
        if (s->runaway_finals > 0) {
            s->runaway_finals--;
            *out = strdup("!!!!!!!!!!!!");
            snprintf(err, cap, "the answer reached its %d tokens without ending", max_out);
            return -1;
        }
        if (s->long_finals > 0) {
            s->long_finals--;
            oc_sum_buf_puts(&b, "Overview: [1] They agreed to ship.\n## Plan\n[1] They agreed to ship.\n");
            for (int i = 0; i < 40; i++)
                oc_sum_buf_puts(&b, "- [1] They agreed to ship it on the day they had planned, after the usual discussion.\n");
        } else {
            oc_sum_buf_puts(&b, "Overview: [1] They agreed to ship.\n## Plan\n[1] They agreed to ship.\n- [1] agreed\n");
        }
    } else {
        s->rewrite++;
        oc_sum_buf_puts(&b, "Shorter.\n");
    }
    *out = b.p;
    return 0;
}

typedef struct { sqlite3 *db; int stores; char last[4096]; char err[600]; int rewrites; } sink;
static int k_store(void *ctx, const oc_sum_answer *a, oc_sum_new *nodes, int n) {
    sink *k = ctx;
    k->stores++;
    k->rewrites = a->rewrites;
    int rc = n ? oc_sum_store(k->db, a->channel, a->version, nodes, n) : 0;
    if (!a->ok) { snprintf(k->err, sizeof k->err, "%s", a->err ? a->err : ""); return 0; }
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
    oc_sum_engine e = { &s, "stub", s_open, s_close, s_run, 0 };
    sink k = { db, 0, "", "", 0 };
    oc_sum_worker_cfg cfg;
    setup(&cfg, path, &e, &k, SUM_THRESHOLD_TOKENS);
    char err[600] = "";
    /* A span that fits one prompt is summarized from its messages, in one call. */
    CHECK(oc_sum_build_now(&cfg, 1, 0, 2 * 86400000ll, 0, err, sizeof err) == 0);
    CHECK(s.calls == 1 && s.final == 1 && s.leaf == 0 && s.other_system == 0);
    CHECK(strstr(k.last, "{\"summary\":{\"overview\":{\"text\":\"They agreed to ship.\",\"refs\":[1]}") != NULL);
    CHECK(strstr(k.last, "\"count\":1,\"people\":[\"Ann\"]") != NULL);
    /* Again: found, nothing asked. */
    int before = s.calls;
    CHECK(oc_sum_build_now(&cfg, 1, 0, 2 * 86400000ll, 0, err, sizeof err) == 0);
    CHECK(s.calls == before);
    /* Another span over some of the same messages is its own summary. */
    CHECK(oc_sum_build_now(&cfg, 1, 0, 86400000ll, 0, err, sizeof err) == 0);
    CHECK(s.calls == before + 1);
    /* An edit purges what was built on the message; the next request builds again. */
    sqlite3_exec(db, "UPDATE messages SET edited_at_ms=7, body=CAST('Shipped late.' AS BLOB) WHERE id=4;", NULL, NULL, NULL);
    CHECK(oc_sum_purge(db, 1, 4, 4, 30 * 3600000) >= 1);
    before = s.calls;
    CHECK(oc_sum_build_now(&cfg, 1, 0, 2 * 86400000ll, 0, err, sizeof err) == 0);
    CHECK(s.calls == before + 1);
    /* Idle work takes notes on the chunks ahead of time. */
    add_msg(db, 5, 0, 10, 50 * 3600000, "Next week.");
    add_msg(db, 6, 0, 11, 55 * 3600000, "Sure.");
    before = s.calls;
    CHECK(oc_sum_chunks_now(&cfg, 1, 2 * 86400000ll, 3 * 86400000ll, err, sizeof err) == 0);
    CHECK(s.calls == before + 2 && s.leaf == 2);
    CHECK(count_where(db, "SELECT COUNT(*) FROM summary_nodes WHERE kind=0 AND json_extract(body,'$.notes[0].kind')='decision';") == 2);
    /* An empty span: no call, an empty summary. */
    before = s.calls;
    CHECK(oc_sum_build_now(&cfg, 1, 10 * 86400000ll, 11 * 86400000ll, 0, err, sizeof err) == 0);
    CHECK(s.calls == before && strstr(k.last, "\"topics\":[]") != NULL);

    /* A summary far over its total is handed back once to be written shorter;
     * the shorter answer is the one stored, and the call is counted. */
    add_msg(db, 7, 0, 10, 60 * 3600000, "Shipping Friday.");
    s.long_finals = 1;
    before = s.calls;
    CHECK(oc_sum_build_now(&cfg, 1, 2 * 86400000ll + 12 * 3600000, 3 * 86400000ll, 0, err, sizeof err) == 0);
    CHECK(s.calls == before + 2 && s.shorten == 1 && k.rewrites == 1);
    CHECK(strstr(k.last, "after the usual discussion") == NULL);
    /* Written again no shorter: the first answer stands, trimmed to its
     * total, and there is still only the one extra call. */
    add_msg(db, 8, 0, 11, 61 * 3600000, "Noted.");
    s.long_finals = 2;
    before = s.calls;
    s.shorten = 0;
    CHECK(oc_sum_build_now(&cfg, 1, 2 * 86400000ll + 12 * 3600000, 3 * 86400000ll + 1, 0, err, sizeof err) == 0);
    CHECK(s.calls == before + 2 && s.shorten == 1 && s.long_finals == 0);
    {
        const char *b = strstr(k.last, "\"summary\":");
        int words = 0, in = 0;
        for (const char *q = b ? b : ""; *q; q++) { int sp = *q == ' ' || *q == '\n'; if (!sp && !in) words++; in = !sp; }
        CHECK(words < 200);
    }
    s.long_finals = 0;

    /* An answer that runs away is asked for once more, the same request; a
     * second runaway fails the build. */
    /* Enough words that the stub's nine-word answer is within the total. */
    add_msg(db, 9, 0, 10, 62 * 3600000, "Done with the shipping plan for this week, thanks everyone.");
    s.runaway_finals = 1;
    before = s.calls;
    CHECK(oc_sum_build_now(&cfg, 1, 2 * 86400000ll + 12 * 3600000, 3 * 86400000ll + 2, 0, err, sizeof err) == 0);
    CHECK(s.calls == before + 2 && s.runaway_finals == 0);
    add_msg(db, 11, 0, 11, 63 * 3600000, "Yes.");
    s.runaway_finals = 2;
    before = s.calls;
    CHECK(oc_sum_build_now(&cfg, 1, 2 * 86400000ll + 12 * 3600000, 3 * 86400000ll + 3, 0, err, sizeof err) == -1);
    CHECK(s.calls == before + 2 && strstr(err, "without ending") != NULL);
    s.runaway_finals = 0;

    /* A build whose final call fails keeps the chunk notes it made: the next
     * build of the same span starts from them and makes only the final call. */
    for (int i = 0; i < 400; i++) {
        /* Each worded differently, so none is folded into another as a repeat. */
        char text[200];
        snprintf(text, sizeof text, "A message about the plan for %c%c day, long enough that four hundred of them "
                 "need notes before a summary can be written.", 'a' + i % 26, 'a' + (i / 26) % 26);
        add_msg(db, 100 + i, 0, 10 + (i & 1), 6 * 86400000ll + i * 3 * MIN, text);
    }
    s.fail_finals = 1;
    before = s.calls;
    int leaf_before = s.leaf;
    CHECK(oc_sum_build_now(&cfg, 1, 6 * 86400000ll, 7 * 86400000ll, 0, err, sizeof err) == -1);
    CHECK(strstr(err, "the server is down") != NULL);
    int made = s.leaf - leaf_before;
    CHECK(made >= 2);
    CHECK(count_where(db, "SELECT COUNT(*) FROM summary_nodes WHERE kind=0 AND start_ms>=518400000 AND start_ms<604800000;") == made);
    CHECK(count_where(db, "SELECT COUNT(*) FROM summary_nodes WHERE kind=3 AND start_ms=518400000;") == 0);
    before = s.calls;
    CHECK(oc_sum_build_now(&cfg, 1, 6 * 86400000ll, 7 * 86400000ll, 0, err, sizeof err) == 0);
    CHECK(s.calls == before + 1 && s.leaf == leaf_before + made);
    sqlite3_close(db);
    drop_db(path);
}

/* A span too big for one prompt: notes on every chunk, merged -- at least two
 * into one, fewer notes out than lines in -- level after level until they fit,
 * then one summary from them and the messages they cite. However much there
 * is, it ends. A thread and a message too big for a chunk are merged to one
 * node the same way. */
static void test_recursion(void) {
    const char *path = "/tmp/oc_test_summary_recursion.db";
    sqlite3 *db = fresh_db(path);
    CHECK(db != NULL);
    if (!db) return;
    int64_t id = 100000;
    for (int i = 0; i < 5000; i++)     /* far more than one prompt holds, or one level of merges */
        add_msg(db, id++, 0, 10 + (i & 1), (int64_t)i * 10000 + 60000,
                "A message about the plan for today, long enough that two hundred of them do not fit one prompt.");
    stub s = {0};
    s.fat = 1;
    oc_sum_engine e = { &s, "stub", s_open, s_close, s_run, 0 };
    sink k = { db, 0, "", "", 0 };
    oc_sum_worker_cfg cfg;
    setup(&cfg, path, &e, &k, 60);
    char err[600] = "";
    CHECK(oc_sum_build_now(&cfg, 1, 0, 86400000ll, 0, err, sizeof err) == 0);
    CHECK(s.leaf > 60 && s.merge > 4 && s.final == 1 && s.other_system == 0);
    /* Two levels at least: a section built from sections. */
    CHECK(count_where(db, "SELECT COUNT(*) FROM summary_inputs i JOIN summary_nodes p ON p.id=i.parent_id "
                          "JOIN summary_nodes c ON c.id=i.child_id WHERE i.child_kind=1 AND p.kind=2 AND c.kind=2;") > 0);
    /* Every merge took two or more. */
    CHECK(count_where(db, "SELECT COUNT(*) FROM summary_nodes n WHERE n.kind=2 AND "
                          "(SELECT COUNT(*) FROM summary_inputs i WHERE i.parent_id=n.id) < 2;") == 0);
    /* Every node holds at most the answer room's notes, every merge fewer than
     * its children together; every body is whole JSON. */
    char q[400];
    snprintf(q, sizeof q, "SELECT COUNT(*) FROM summary_nodes WHERE kind<>3 AND json_array_length(body,'$.notes')>%d;",
             SUM_NOTES_ROOM);
    CHECK(count_where(db, q) == 0);
    CHECK(count_where(db, "SELECT COUNT(*) FROM summary_nodes n WHERE n.kind=2 AND json_array_length(n.body,'$.notes') >= "
                          "(SELECT SUM(json_array_length(c.body,'$.notes')) FROM summary_inputs i JOIN summary_nodes c "
                          "ON c.id=i.child_id WHERE i.parent_id=n.id);") == 0);
    CHECK(count_where(db, "SELECT COUNT(*) FROM summary_nodes WHERE json_valid(body)=0;") == 0);
    CHECK(count_where(db, "SELECT COUNT(*) FROM summary_nodes WHERE kind=3;") == 1);

    /* A thread too big for a chunk: one node for the whole thread. */
    for (int i = 0; i < 6; i++)
        add_msg(db, 1000 + i, i ? 1000 : 0, 10 + (i & 1), 2 * 86400000ll + i * MIN,
                "A longer reply in a busy thread, with a few more words in it.");
    s.leaf = s.merge = 0;
    CHECK(oc_sum_chunks_now(&cfg, 1, 2 * 86400000ll, 3 * 86400000ll, err, sizeof err) == 0);
    CHECK(s.leaf >= 2 && s.merge >= 1);
    CHECK(count_where(db, "SELECT COUNT(*) FROM summary_nodes WHERE kind=1 AND root_id=1000;") == 1);

    /* A message too big for a chunk: notes on its parts, merged to one. */
    static char big[1200];
    big[0] = '\0';
    while (strlen(big) + 40 < sizeof big) strcat(big, "One sentence of a very long message. ");
    add_msg(db, 2000, 0, 10, 4 * 86400000ll, big);
    s.part = s.merge = 0;
    CHECK(oc_sum_chunks_now(&cfg, 1, 4 * 86400000ll, 5 * 86400000ll, err, sizeof err) == 0);
    CHECK(s.part >= 4 && s.merge >= 1);
    CHECK(count_where(db, "SELECT COUNT(*) FROM summary_nodes WHERE ikey LIKE 'q:%';") == s.part);
    /* Every part's node is built on the message, so an edit purges them all. */
    sqlite3_exec(db, "UPDATE messages SET edited_at_ms=9 WHERE id=2000;", NULL, NULL, NULL);
    CHECK(oc_sum_purge(db, 1, 2000, 2000, 4 * 86400000ll) >= s.part + 1);
    CHECK(count_where(db, "SELECT COUNT(*) FROM summary_nodes WHERE ikey LIKE 'q:%';") == 0);
    sqlite3_close(db);
    drop_db(path);
}

/* --- the queue, and idle work giving way to it -------------------------------------- */

/* A model that is busy until the gate tells it to stop, once; after that it
 * answers at once. */
static volatile int g_q_hold, g_q_stopped, g_q_calls;
static void *q_open(void *ctx, char *err, size_t cap) { (void)err; (void)cap; return ctx; }
static void q_close(void *h) { (void)h; }
static int q_run(void *h, const char *system, const char *user, const char *grammar, int max_out,
                 oc_sum_gate_fn gate, void *gctx, char **out, oc_sum_run_stats *st, char *err, size_t cap) {
    (void)h; (void)system; (void)user; (void)max_out;
    __atomic_add_fetch(&g_q_calls, 1, __ATOMIC_ACQ_REL);
    if (st) memset(st, 0, sizeof *st);
    while (__atomic_load_n(&g_q_hold, __ATOMIC_ACQUIRE)) {
        if (gate && gate(gctx)) {
            __atomic_store_n(&g_q_hold, 0, __ATOMIC_RELEASE);
            __atomic_add_fetch(&g_q_stopped, 1, __ATOMIC_ACQ_REL);
            snprintf(err, cap, "stopped");
            return -1;
        }
        struct timespec ts = { 0, 2 * 1000000L };
        nanosleep(&ts, NULL);
    }
    *out = strdup(grammar && strstr(grammar, "note ::=") ? "- [1] decision | plans | They agreed.\n"
                                                         : "Overview: [1] They agreed.\n## Plans\n[1] They agreed.\n");
    return 0;
}
/* A quiet machine whose clock runs fast, so idle work starts at once. */
static int q_cpu(void *c, uint64_t *b, uint64_t *t) { (void)c; static uint64_t n; n += 100; *b = 0; *t = n; return 0; }
static int q_mem(void *c, uint64_t *m) { (void)c; *m = 1u << 20; return 0; }
static int q_net(void *c, uint64_t *n) { (void)c; *n = 0; return 0; }
static uint64_t q_now(void *c) { (void)c; static uint64_t t; t += 20000; return t; }

typedef struct { sqlite3 *db; volatile int answered, answered_ok; } qsink;
static int q_store(void *ctx, const oc_sum_answer *a, oc_sum_new *nodes, int n) {
    qsink *k = ctx;
    int rc = n ? oc_sum_store(k->db, a->channel, a->version, nodes, n) : 0;
    if (a->request_id && rc != 1) {
        char sql[96];
        snprintf(sql, sizeof sql, "DELETE FROM summary_requests WHERE id=%lld;", (long long)a->request_id);
        sqlite3_exec(k->db, sql, NULL, NULL, NULL);
        __atomic_store_n(&k->answered_ok, a->ok && rc == 0, __ATOMIC_RELEASE);
        __atomic_store_n(&k->answered, 1, __ATOMIC_RELEASE);
    }
    return rc;
}
static int q_take(void *ctx, int64_t row) {
    qsink *k = ctx;
    char sql[128];
    snprintf(sql, sizeof sql, "UPDATE summary_requests SET state='running' WHERE id=%lld AND state='queued';",
             (long long)row);
    sqlite3_exec(k->db, sql, NULL, NULL, NULL);
    return sqlite3_changes(k->db) == 1;
}

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int wait_for(volatile int *v, int want_at_least, int seconds) {
    for (int i = 0; i < seconds * 100; i++) {
        if (__atomic_load_n(v, __ATOMIC_ACQUIRE) >= want_at_least) return 1;
        struct timespec ts = { 0, 10 * 1000000L };
        nanosleep(&ts, NULL);
    }
    return 0;
}

/* Idle work summarizes what is recent and leaves the old for when someone asks;
 * a request in the queue (summary_requests) stops idle work mid-answer and is
 * made, and the idle work is done after. */
static void test_queue(void) {
    const char *path = "/tmp/oc_test_summary_queue.db";
    drop_db(path);
    sqlite3 *db = fresh_db(path);
    CHECK(db != NULL);
    if (!db) return;
    int64_t now = now_ms();
    add_msg(db, 1, 0, 10, now - 3600000, "We should ship on Friday.");
    add_msg(db, 2, 0, 11, now - 3600000 + MIN, "Agreed.");
    add_msg(db, 3, 0, 10, now - 30ll * 86400000, "An old note from last month.");
    static oc_sum_engine eng = { NULL, "queue", q_open, q_close, q_run, 0 };
    eng.ctx = &eng;
    static oc_sum_probe quiet = { q_cpu, q_mem, q_net, q_now, NULL };
    qsink k = { db, 0, 0 };
    oc_sum_worker_cfg cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.db_path = path;
    cfg.engine = &eng;
    cfg.probe = &quiet;
    cfg.sink.store = q_store;
    cfg.sink.take = q_take;
    cfg.sink.ctx = &k;
    cfg.background = 1;
    __atomic_store_n(&g_q_hold, 1, __ATOMIC_RELEASE);
    char err[256] = "";
    oc_sum_worker *w = oc_sum_worker_start(&cfg, err, sizeof err);
    CHECK(w != NULL);
    if (!w) { sqlite3_close(db); return; }
    /* Idle work starts on the recent conversation, and the model is busy. */
    CHECK(wait_for(&g_q_calls, 1, 20));
    /* Someone asks: their request joins the queue, and the worker is told. */
    char sql[256];
    snprintf(sql, sizeof sql,
             "INSERT INTO summary_requests (conn_id, req_id, user_id, channel_id, start_ms, end_ms, created_at_ms) "
             "VALUES (5, 9, 10, 1, %lld, %lld, %lld);", (long long)(now - 86400000), (long long)(now + 1),
             (long long)now);
    CHECK(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
    oc_sum_worker_wake(w);
    CHECK(wait_for(&k.answered, 1, 20));
    CHECK(g_q_stopped == 1 && k.answered_ok);
    CHECK(count_where(db, "SELECT COUNT(*) FROM summary_requests;") == 0);
    oc_sum_worker_stop(w);
    /* What idle work was doing was made for the request; the old note is left
     * for whoever asks for last month. */
    CHECK(count_where(db, "SELECT COUNT(*) FROM summary_inputs i JOIN summary_nodes n ON n.id=i.parent_id "
                          "WHERE n.kind=0 AND i.child_kind=0 AND i.child_id=1;") == 1);
    CHECK(count_where(db, "SELECT COUNT(*) FROM summary_inputs i JOIN summary_nodes n ON n.id=i.parent_id "
                          "WHERE n.kind=0 AND i.child_kind=0 AND i.child_id=3;") == 0);
    sqlite3_close(db);
    drop_db(path);
}

/* A request outlives the daemon (SUMMARIES.md §5): one left waiting or being
 * made when it stopped waits again at the next start, watched by nobody, and a
 * notice left unseen is still there. */
static void test_requeue_at_start(void) {
    const char *path = "/tmp/oc_test_summary_requeue.db";
    drop_db(path);
    oc_dbwriter *w = oc_dbwriter_start(path);
    if (w) oc_dbwriter_set_pw_iterations(w, 2048);   /* fast PBKDF2 for tests */
    CHECK(w != NULL);
    if (!w) return;
    oc_dbwriter_stop(w);
    sqlite3 *db = NULL;
    CHECK(sqlite3_open(path, &db) == SQLITE_OK);
    CHECK(sqlite3_exec(db,
        "INSERT INTO summary_requests (conn_id, req_id, user_id, channel_id, scope, start_ms, end_ms, state, "
        "created_at_ms) VALUES (7, 3, 10, 1, 1, 0, 100, 'running', 1), (8, 4, 11, 1, 4, 0, 100, 'queued', 2);"
        "INSERT INTO summary_notices (user_id, channel_id, scope, start_ms, end_ms, status, body, made_at_ms) "
        "VALUES (10, 1, 1, 0, 100, 0, '{}', 5);", NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_close(db);
    w = oc_dbwriter_start(path);
    if (w) oc_dbwriter_set_pw_iterations(w, 2048);   /* fast PBKDF2 for tests */
    CHECK(w != NULL);
    if (w) oc_dbwriter_stop(w);
    CHECK(sqlite3_open(path, &db) == SQLITE_OK);
    CHECK(count_where(db, "SELECT COUNT(*) FROM summary_requests;") == 2);
    CHECK(count_where(db, "SELECT COUNT(*) FROM summary_requests WHERE state='queued' AND conn_id=0 AND req_id=0;") == 2);
    CHECK(count_where(db, "SELECT COUNT(*) FROM summary_notices WHERE seen_at_ms IS NULL;") == 1);
    sqlite3_close(db);
    drop_db(path);
}

/* A notice seen more than a day ago is deleted by the storage sweep; one seen
 * within the day, and one never seen however old, stay (SUMMARIES.md §5). */
static void test_notice_prune(void) {
    const char *path = "/tmp/oc_test_summary_prune.db";
    drop_db(path);
    oc_dbwriter *w = oc_dbwriter_start(path);
    if (w) oc_dbwriter_set_pw_iterations(w, 2048);   /* fast PBKDF2 for tests */
    CHECK(w != NULL);
    if (!w) return;
    sqlite3 *db = NULL;
    CHECK(sqlite3_open(path, &db) == SQLITE_OK);
    long long now = (long long)now_ms(), hour = 3600000ll;
    char sql[640];
    snprintf(sql, sizeof sql,
             "INSERT INTO summary_notices (id, user_id, channel_id, scope, start_ms, end_ms, status, body, made_at_ms, "
             "seen_at_ms) VALUES (1, 10, 1, 1, 0, 100, 0, '{}', %lld, %lld), (2, 10, 1, 1, 0, 100, 0, '{}', %lld, %lld), "
             "(3, 10, 1, 1, 0, 100, 0, '{}', %lld, NULL);",
             now - 30 * hour, now - 25 * hour, now - 2 * hour, now - 1 * hour, now - 400 * hour);
    CHECK(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
    oc_job *j = oc_job_new(OC_JOB_STORAGE_MAINT, 0);
    CHECK(j != NULL);
    if (j) {
        j->maint_grace_ms = (uint64_t)hour;
        j->maint_batch = 8;
        oc_dbwriter_submit(w, j);
        oc_dbres *r = NULL;
        for (int i = 0; i < 400 && !r; i++) { r = oc_dbwriter_next_result(w); usleep(5000); }
        CHECK(r != NULL);
        if (r) oc_dbres_free(r);
    }
    CHECK(count_where(db, "SELECT COUNT(*) FROM summary_notices WHERE id=1;") == 0);   /* seen 25 h ago */
    CHECK(count_where(db, "SELECT COUNT(*) FROM summary_notices WHERE id=2;") == 1);   /* seen 1 h ago */
    CHECK(count_where(db, "SELECT COUNT(*) FROM summary_notices WHERE id=3;") == 1);   /* never seen */
    sqlite3_close(db);
    oc_dbwriter_stop(w);
    drop_db(path);
}

/* A hosted model does not run on this machine, so the load gate does not hold
 * it: on a machine kept busy, a request to a hosted model is still answered. */
static int b_cpu(void *c, uint64_t *b, uint64_t *t) { (void)c; static uint64_t n; n += 100; *b = n; *t = n; return 0; }
/* As the hosted engine: the gate asked once, before the request. */
static int r_run(void *h, const char *system, const char *user, const char *grammar, int max_out,
                 oc_sum_gate_fn gate, void *gctx, char **out, oc_sum_run_stats *st, char *err, size_t cap) {
    if (gate && gate(gctx)) { snprintf(err, cap, "stopped"); return -1; }
    return q_run(h, system, user, grammar, max_out, NULL, NULL, out, st, err, cap);
}

static void test_remote_ungated(void) {
    const char *path = "/tmp/oc_test_summary_remote.db";
    drop_db(path);
    sqlite3 *db = fresh_db(path);
    CHECK(db != NULL);
    if (!db) return;
    int64_t now = now_ms();
    add_msg(db, 1, 0, 10, now - 3600000, "We should ship on Friday.");
    static oc_sum_engine eng = { NULL, "hosted", q_open, q_close, r_run, 1 };
    eng.ctx = &eng;
    static oc_sum_probe busy = { b_cpu, q_mem, q_net, q_now, NULL };
    qsink k = { db, 0, 0 };
    oc_sum_worker_cfg cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.db_path = path;
    cfg.engine = &eng;
    cfg.probe = &busy;
    cfg.sink.store = q_store;
    cfg.sink.take = q_take;
    cfg.sink.ctx = &k;
    __atomic_store_n(&g_q_hold, 0, __ATOMIC_RELEASE);
    char err[256] = "", sql[256];
    oc_sum_worker *w = oc_sum_worker_start(&cfg, err, sizeof err);
    CHECK(w != NULL);
    if (!w) { sqlite3_close(db); return; }
    snprintf(sql, sizeof sql,
             "INSERT INTO summary_requests (conn_id, req_id, user_id, channel_id, start_ms, end_ms, created_at_ms) "
             "VALUES (5, 9, 10, 1, %lld, %lld, %lld);", (long long)(now - 86400000), (long long)(now + 1),
             (long long)now);
    CHECK(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
    oc_sum_worker_wake(w);
    CHECK(wait_for(&k.answered, 1, 10));
    CHECK(k.answered_ok);
    oc_sum_worker_stop(w);
    sqlite3_close(db);
    drop_db(path);
}

/* Integration posts are summarized under the name they sign with; what a client
 * is sent carries the span's count, who posted (most first, an integration as
 * id 0) and the messages the details cite. Call events and deleted messages are
 * neither summarized nor counted. */
static void test_client_body(void) {
    const char *path = "/tmp/oc_test_summary_client.db";
    sqlite3 *db = fresh_db(path);
    CHECK(db != NULL);
    if (!db) return;
    add_msg(db, 1, 0, 10, 1000, "Order 55 is in.");
    add_msg(db, 2, 1, 11, 2000, "Thanks.");
    add_msg(db, 3, 0, 10, 3000, "Order 56 cancelled.");
    add_msg(db, 4, 0, 10, 4000, "Order 57 cancelled.");
    add_msg(db, 5, 0, 11, 5000, "a call");
    add_msg(db, 6, 0, 11, 6000, "gone");
    sqlite3_exec(db, "UPDATE messages SET author_name='PartsFisher' WHERE id IN (3,4);"
                     "UPDATE messages SET kind=1 WHERE id=5; UPDATE messages SET deleted_at_ms=1 WHERE id=6;",
                 NULL, NULL, NULL);
    oc_sum_window w;
    CHECK(oc_sum_load_window(db, 1, 0, 10000, &w) == 0);
    CHECK(w.n == 4);
    if (w.n == 4) CHECK(!strcmp(w.msgs[2].author, "PartsFisher") && !strcmp(w.msgs[2].text, "Order 56 cancelled."));
    oc_sum_window_free(&w);
    const char *body =
        "{\"overview\":{\"text\":\"Orders came and went.\",\"refs\":[1]},\"topics\":[{\"title\":\"Orders\","
        "\"text\":\"Two were cancelled.\",\"refs\":[3],\"count\":3,\"people\":[],"
        "\"details\":[{\"text\":\"55 came in\",\"refs\":[1]},{\"text\":\"56 and 57 were cancelled\",\"refs\":[3,4]},"
        "{\"text\":\"again\",\"refs\":[3,6]}]}],\"attention\":[],\"more\":[]}";
    char *cb = oc_sum_client_body(db, body, 1, 0, 10000);
    CHECK(cb != NULL);
    if (cb) {
        CHECK(!strncmp(cb, "{\"summary\":{\"overview\":", 23));
        CHECK(strstr(cb, ",\"posters\":[{\"id\":0,\"name\":\"PartsFisher\"},{\"id\":10,\"name\":\"Ann\"},"
                         "{\"id\":11,\"name\":\"Bob\"}],\"count\":4,") != NULL);
        CHECK(strstr(cb, "\"sources\":{\"1\":{\"author\":\"Ann\",\"author_id\":10,\"at\":1000,\"parent\":0,"
                         "\"text\":\"Order 55 is in.\"},\"3\":{\"author\":\"PartsFisher\",\"author_id\":0,\"at\":3000,"
                         "\"parent\":0,\"text\":\"Order 56 cancelled.\"},\"4\":") != NULL);
        CHECK(strstr(cb, "\"6\":") == NULL);   /* deleted: not a source */
    }
    free(cb);
    sqlite3_close(db);
    drop_db(path);
}

/* --- the hosted engine --------------------------------------------------------- */

/* One HTTP exchange on loopback: the request kept, a canned answer sent. */
/* `hold`: seconds the connection stays open after the answer, as a proxy may
 * keep it. */
typedef struct {
    int fd; int hold; const char *answer; char req[16384];
    const char *answers[8]; int n;   /* a sequence, for fake_api_serve_n */
    int status;                      /* the status sent: 0 for 200 */
    int statuses[8];                 /* one a step of the sequence; 0 for 200 */
    const char *retry_after;         /* a Retry-After header, when set */
    int served;                      /* exchanges answered so far */
} fake_api;

static void *fake_api_serve(void *arg) {
    fake_api *f = arg;
    int c = accept(f->fd, NULL, NULL);
    if (c < 0) return NULL;
    size_t n = 0;
    for (;;) {
        ssize_t r = read(c, f->req + n, sizeof f->req - 1 - n);
        if (r <= 0) break;
        n += (size_t)r;
        f->req[n] = '\0';
        char *h = strstr(f->req, "\r\n\r\n");
        char *cl = strstr(f->req, "Content-Length: ");
        if (h && cl && (size_t)(h + 4 - f->req) + (size_t)atoi(cl + 16) <= n) break;
    }
    char head[320], ra[96] = "";
    if (f->retry_after) snprintf(ra, sizeof ra, "Retry-After: %s\r\n", f->retry_after);
    int hn = snprintf(head, sizeof head, "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n"
                      "%sConnection: close\r\n\r\n", f->status ? f->status : 200, f->status ? "Busy" : "OK",
                      strlen(f->answer), ra);
    f->served++;
    if (write(c, head, (size_t)hn) < 0 || write(c, f->answer, strlen(f->answer)) < 0) {}
    if (f->hold) sleep((unsigned)f->hold);
    close(c);
    return NULL;
}

static int fake_api_open(fake_api *f, int *port) {
    struct sockaddr_in a;
    socklen_t al = sizeof a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    f->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (f->fd < 0 || bind(f->fd, (struct sockaddr *)&a, sizeof a) || listen(f->fd, 1) ||
        getsockname(f->fd, (struct sockaddr *)&a, &al))
        return -1;
    *port = ntohs(a.sin_port);
    return 0;
}

/* `n` exchanges in a row, each answered with the next of `answers`; the last
 * request is kept. */
static void *fake_api_serve_n(void *arg) {
    fake_api *f = arg;
    for (int i = 0; i < f->n; i++) {
        f->answer = f->answers[i];
        f->status = f->statuses[i];
        fake_api_serve(f);
    }
    f->status = 0;
    return NULL;
}

#define PLAIN_OK "{\"choices\":[{\"message\":{\"content\":\"OK\"},\"finish_reason\":\"stop\"}]," \
                 "\"usage\":{\"completion_tokens\":1,\"completion_tokens_details\":{\"reasoning_tokens\":0}}}"
#define REASONED "{\"choices\":[{\"message\":{\"content\":\"\"},\"finish_reason\":\"length\"}]," \
                 "\"usage\":{\"completion_tokens\":16,\"completion_tokens_details\":{\"reasoning_tokens\":16}}}"

static void test_cloud(void) {
    static fake_api f;
    int port = 0;
    CHECK(fake_api_open(&f, &port) == 0);
    char url[64];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/v1/", port);
    const oc_sum_engine *e = oc_sum_cloud_engine(url, "m-1", "k-2");
    CHECK(!strcmp(e->version, "m-1"));

    /* Opening asks a one-line prompt. A model that answers it plainly is sent
     * standard fields only, and the second open asks nothing. */
    pthread_t t;
    char err[300] = "";
    f.answer = PLAIN_OK;
    pthread_create(&t, NULL, fake_api_serve, &f);
    void *h = e->open(e->ctx, err, sizeof err);
    pthread_join(t, NULL);
    CHECK(h != NULL);
    CHECK(strstr(f.req, "\"content\":\"Say OK.\"") && strstr(f.req, "\"max_tokens\":16}"));
    CHECK(strstr(f.req, "reasoning") == NULL);
    CHECK(e->open(e->ctx, err, sizeof err) == h);

    /* An answer: its text, tokens and the server's own timings. */
    f.answer = "{\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":\"- [1] a | b\\nc\"},"
               "\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":120,\"completion_tokens\":7},"
               "\"timings\":{\"prompt_n\":120,\"prompt_ms\":35.5,\"predicted_n\":7,\"predicted_ms\":410.25}}";
    f.hold = 3;
    pthread_create(&t, NULL, fake_api_serve, &f);
    char *out = NULL;
    oc_sum_run_stats st;
    int rc = e->run(h, "sys \"q\"", "line one\nline two", "root ::= \"x\"", 300, NULL, NULL, &out, &st, err, sizeof err);
    /* The whole answer is in hand before the server lets go of the connection. */
    CHECK(st.wall_ms < 2000);
    pthread_join(t, NULL);
    f.hold = 0;
    CHECK(rc == 0);
    CHECK(out && !strcmp(out, "- [1] a | b\nc"));
    CHECK(st.prompt_tokens == 120 && st.output_tokens == 7 && st.read_ms == 35 && st.write_ms == 410);
    free(out);
    /* What was sent: the endpoint, the key, both prompts, the answer cap and
     * greedy decoding; nothing a standard API would not read, and no grammar. */
    CHECK(!strncmp(f.req, "POST /v1/chat/completions HTTP/1.1\r\n", 36));
    CHECK(strstr(f.req, "Authorization: Bearer k-2\r\n") != NULL);
    CHECK(strstr(f.req, "\"model\":\"m-1\"") != NULL);
    CHECK(strstr(f.req, "{\"role\":\"system\",\"content\":\"sys \\\"q\\\"\"}") != NULL);
    CHECK(strstr(f.req, "{\"role\":\"user\",\"content\":\"line one\\nline two\"}") != NULL);
    CHECK(strstr(f.req, "\"stream\":false,\"temperature\":0,\"max_tokens\":300}") != NULL);
    CHECK(strstr(f.req, "\"grammar\"") == NULL);
    CHECK(strstr(f.req, "dry_") == NULL && strstr(f.req, "samplers") == NULL);
    CHECK(strstr(f.req, "chat_template_kwargs") == NULL);
    CHECK(strstr(f.req, "\"reasoning\"") == NULL);

    /* A thinking model's reasoning is dropped; the answer is what follows it. */
    f.answer = "{\"choices\":[{\"message\":{\"content\":\"<think>\\nhmm\\n</think>\\n\\n- [1] a | b\"},"
               "\"finish_reason\":\"stop\"}]}";
    pthread_create(&t, NULL, fake_api_serve, &f);
    out = NULL;
    rc = e->run(h, "s", "u", NULL, 0, NULL, NULL, &out, &st, err, sizeof err);
    pthread_join(t, NULL);
    CHECK(rc == 0);
    CHECK(out && !strcmp(out, "- [1] a | b"));
    free(out);

    /* An answer that ran out of tokens fails as the local engine's does, its
     * text kept for the log. */
    f.answer = "{\"choices\":[{\"message\":{\"content\":\"- [1] a | b\"},\"finish_reason\":\"length\"}]}";
    pthread_create(&t, NULL, fake_api_serve, &f);
    out = NULL;
    rc = e->run(h, "s", "u", NULL, 5, NULL, NULL, &out, &st, err, sizeof err);
    pthread_join(t, NULL);
    CHECK(rc == -1);
    CHECK(strstr(err, "reached its 5 tokens") != NULL);
    CHECK(out && !strcmp(out, "- [1] a | b"));
    CHECK(strstr(f.req, "\"grammar\"") == NULL);
    free(out);

    /* A refusal is a reason, not an answer. */
    f.answer = "{\"error\":{\"message\":\"no such model\"}}";
    pthread_create(&t, NULL, fake_api_serve, &f);
    out = NULL;
    rc = e->run(h, "s", "u", NULL, 5, NULL, NULL, &out, &st, err, sizeof err);
    pthread_join(t, NULL);
    CHECK(rc == -1 && out == NULL);
    CHECK(strstr(err, "no choices") != NULL);
    e->close(h);

    /* A model that reasons first: the one-line prompt comes back empty with
     * reasoning tokens in the usage. The fields for turning that off are tried
     * in turn, and the first the server honours is sent with every request. */
    e = oc_sum_cloud_engine(url, "m-2", "");
    f.answers[0] = REASONED;   /* standard fields */
    f.answers[1] = REASONED;   /* reasoning_effort: ignored */
    f.answers[2] = PLAIN_OK;   /* reasoning: {enabled: false}: honoured */
    f.n = 3;
    pthread_create(&t, NULL, fake_api_serve_n, &f);
    h = e->open(e->ctx, err, sizeof err);
    pthread_join(t, NULL);
    CHECK(h != NULL);
    CHECK(strstr(f.req, ",\"reasoning\":{\"enabled\":false}}") != NULL);
    f.answer = "{\"choices\":[{\"message\":{\"content\":\"- [1] a | b\"},\"finish_reason\":\"stop\"}]}";
    pthread_create(&t, NULL, fake_api_serve, &f);
    out = NULL;
    rc = e->run(h, "s", "u", NULL, 40, NULL, NULL, &out, &st, err, sizeof err);
    pthread_join(t, NULL);
    CHECK(rc == 0 && out && !strcmp(out, "- [1] a | b"));
    CHECK(strstr(f.req, "\"max_tokens\":40,\"reasoning\":{\"enabled\":false}}") != NULL);
    free(out);
    e->close(h);

    /* One that reasons only on a prompt worth it: the open's line passes, the
     * first real request shows reasoning tokens, and is asked again with each
     * field until one is honoured; that answer is the call's. */
    e = oc_sum_cloud_engine(url, "m-2b", "");
    f.answer = PLAIN_OK;
    pthread_create(&t, NULL, fake_api_serve, &f);
    h = e->open(e->ctx, err, sizeof err);
    pthread_join(t, NULL);
    CHECK(h != NULL);
    f.answers[0] = "{\"choices\":[{\"message\":{\"content\":\"\"},\"finish_reason\":\"length\"}],"
                   "\"usage\":{\"completion_tokens\":40,\"completion_tokens_details\":{\"reasoning_tokens\":40}}}";
    f.answers[1] = "{\"choices\":[{\"message\":{\"content\":\"- [1] a | b\"},\"finish_reason\":\"stop\"}],"
                   "\"usage\":{\"completion_tokens\":6,\"completion_tokens_details\":{\"reasoning_tokens\":0}}}";
    f.n = 2;
    pthread_create(&t, NULL, fake_api_serve_n, &f);
    out = NULL;
    rc = e->run(h, "s", "u", NULL, 40, NULL, NULL, &out, &st, err, sizeof err);
    pthread_join(t, NULL);
    CHECK(rc == 0 && out && !strcmp(out, "- [1] a | b"));
    CHECK(st.output_tokens == 6);
    CHECK(strstr(f.req, "\"max_tokens\":40,\"reasoning_effort\":\"none\"}") != NULL);
    free(out);
    f.answer = "{\"choices\":[{\"message\":{\"content\":\"- [2] c | d\"},\"finish_reason\":\"stop\"}]}";
    pthread_create(&t, NULL, fake_api_serve, &f);
    out = NULL;
    rc = e->run(h, "s", "u", NULL, 40, NULL, NULL, &out, &st, err, sizeof err);
    pthread_join(t, NULL);
    CHECK(rc == 0 && strstr(f.req, "\"reasoning_effort\":\"none\"}") != NULL);
    free(out);
    e->close(h);

    /* One that cannot be told not to: the open fails with that as the reason. */
    e = oc_sum_cloud_engine(url, "m-3", "");
    for (int i = 0; i < 4; i++) f.answers[i] = REASONED;
    f.n = 4;
    pthread_create(&t, NULL, fake_api_serve_n, &f);
    h = e->open(e->ctx, err, sizeof err);
    pthread_join(t, NULL);
    CHECK(h == NULL);
    CHECK(strstr(err, "reasons before answering and cannot be told not to") != NULL);

    /* A server that asks for a pause: the same request is sent again after
     * the wait it names, and the retry is counted. */
    e = oc_sum_cloud_engine(url, "m-5", "");
    f.answer = PLAIN_OK;
    f.status = 0;
    pthread_create(&t, NULL, fake_api_serve, &f);
    h = e->open(e->ctx, err, sizeof err);
    pthread_join(t, NULL);
    CHECK(h != NULL);
    f.answers[0] = "{\"error\":{\"message\":\"slow down\"}}";
    f.statuses[0] = 429;
    f.answers[1] = "{\"choices\":[{\"message\":{\"content\":\"- [1] a | b\"},\"finish_reason\":\"stop\"}]}";
    f.statuses[1] = 0;
    f.n = 2;
    f.retry_after = "1";
    f.served = 0;
    pthread_create(&t, NULL, fake_api_serve_n, &f);
    out = NULL;
    uint32_t t0 = (uint32_t)time(NULL);
    rc = e->run(h, "s", "u", NULL, 40, NULL, NULL, &out, &st, err, sizeof err);
    pthread_join(t, NULL);
    CHECK(rc == 0 && out && !strcmp(out, "- [1] a | b"));
    CHECK(st.retries == 1 && st.wait_ms == 1000 && f.served == 2);
    CHECK((uint32_t)time(NULL) - t0 >= 1);
    free(out);
    /* A 503 the same way. */
    f.statuses[0] = 503;
    f.served = 0;
    pthread_create(&t, NULL, fake_api_serve_n, &f);
    out = NULL;
    rc = e->run(h, "s", "u", NULL, 40, NULL, NULL, &out, &st, err, sizeof err);
    pthread_join(t, NULL);
    CHECK(rc == 0 && st.retries == 1 && f.served == 2);
    free(out);
    /* Still refused after every retry: the server's answer is the error, and
     * the request was sent six times in all. */
    for (int i = 0; i < 6; i++) { f.answers[i] = "{\"error\":{\"message\":\"slow down\"}}"; f.statuses[i] = 429; }
    f.n = 6;
    f.retry_after = "0";
    f.served = 0;
    pthread_create(&t, NULL, fake_api_serve_n, &f);
    out = NULL;
    rc = e->run(h, "s", "u", NULL, 40, NULL, NULL, &out, &st, err, sizeof err);
    pthread_join(t, NULL);
    CHECK(rc == -1 && strstr(err, "answered 429") != NULL);
    CHECK(st.retries == 5 && f.served == 6);
    /* A 400 is not tried again. */
    f.answers[0] = "{\"error\":{\"message\":\"bad request\"}}";
    f.statuses[0] = 400;
    f.n = 1;
    f.served = 0;
    pthread_create(&t, NULL, fake_api_serve_n, &f);
    out = NULL;
    rc = e->run(h, "s", "u", NULL, 40, NULL, NULL, &out, &st, err, sizeof err);
    pthread_join(t, NULL);
    CHECK(rc == -1 && strstr(err, "answered 400") != NULL && st.retries == 0 && f.served == 1);
    f.retry_after = NULL;
    e->close(h);

    /* An answer with no text is a failure, not an empty summary. */
    e = oc_sum_cloud_engine(url, "m-4", "");
    f.answer = PLAIN_OK;
    pthread_create(&t, NULL, fake_api_serve, &f);
    h = e->open(e->ctx, err, sizeof err);
    pthread_join(t, NULL);
    f.answer = "{\"choices\":[{\"message\":{\"content\":\"\"},\"finish_reason\":\"stop\"}]}";
    pthread_create(&t, NULL, fake_api_serve, &f);
    out = NULL;
    rc = e->run(h, "s", "u", NULL, 40, NULL, NULL, &out, &st, err, sizeof err);
    pthread_join(t, NULL);
    CHECK(rc == -1 && strstr(err, "no text") != NULL);
    free(out);
    e->close(h);
    close(f.fd);
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
    test_queue();
    test_remote_ungated();
    test_requeue_at_start();
    test_notice_prune();
    test_client_body();
    test_cloud();
    return failures;
}
