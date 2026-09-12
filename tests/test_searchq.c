/* Search query parsing (REQ-081).
 *
 * This parser decides what a query MEANS in two processes — the client shows the user
 * what it understood and the daemon executes it — so the edge cases are the point of
 * this suite, exactly as with the mention scanner. */

#include "check.h"
#include "searchq.h"

#include <string.h>

static void test_plain(void) {
    oc_searchq q;
    oc_searchq_parse("deploy plan", &q);
    CHECK(strcmp(q.text, "deploy plan") == 0);
    CHECK(q.n_filters == 0 && q.from[0] == '\0' && q.in[0] == '\0' && q.has == 0);

    /* Empty and NULL are both "nothing", not a crash. */
    oc_searchq_parse("", &q);
    CHECK(q.text[0] == '\0' && q.n_filters == 0);
    oc_searchq_parse(NULL, &q);
    CHECK(q.text[0] == '\0');
}

static void test_operators(void) {
    oc_searchq q;
    oc_searchq_parse("from:alice in:design deploy", &q);
    CHECK(strcmp(q.from, "alice") == 0);
    CHECK(strcmp(q.in, "design") == 0);
    CHECK(strcmp(q.text, "deploy") == 0);
    CHECK(q.n_filters == 2);

    /* "#design" and "design" name the same channel: the sigil is stripped so the
     * caller does not have to know which the user typed. */
    oc_searchq_parse("in:#general hello", &q);
    CHECK(strcmp(q.in, "general") == 0 && strcmp(q.text, "hello") == 0);

    /* Case-insensitive operators. */
    oc_searchq_parse("FROM:bob HAS:file", &q);
    CHECK(strcmp(q.from, "bob") == 0 && (q.has & OC_SQ_HAS_FILE));

    /* Quoted values, so a display name with a space survives. */
    oc_searchq_parse("from:\"ada lovelace\" notes", &q);
    CHECK(strcmp(q.from, "ada lovelace") == 0 && strcmp(q.text, "notes") == 0);

    /* has: accumulates into a mask, and plurals are the same thing. */
    oc_searchq_parse("has:links has:images", &q);
    CHECK((q.has & OC_SQ_HAS_LINK) && (q.has & OC_SQ_HAS_IMAGE));
    CHECK(!(q.has & OC_SQ_HAS_FILE));
    CHECK(q.n_filters == 2);

    /* Dates pass through as typed: only the caller knows the timezone. */
    oc_searchq_parse("after:2026-01-01 before:2026-02-01 x", &q);
    CHECK(strcmp(q.after, "2026-01-01") == 0);
    CHECK(strcmp(q.before, "2026-02-01") == 0);
    CHECK(strcmp(q.text, "x") == 0);
}

static void test_not_swallowed(void) {
    oc_searchq q;
    /* An UNKNOWN has: value stays as text rather than vanishing — silently dropping
     * part of a query is how a search lies about what it looked for. */
    oc_searchq_parse("has:banana", &q);
    CHECK(q.has == 0 && q.n_filters == 0);
    CHECK(strcmp(q.text, "has:banana") == 0);

    /* A bare operator constrains nothing and must not be counted as understood. */
    oc_searchq_parse("from: deploy", &q);
    CHECK(q.from[0] == '\0' && q.n_filters == 0 && strcmp(q.text, "deploy") == 0);

    /* A colon inside ordinary text is not an operator. */
    oc_searchq_parse("ratio 3:1 today", &q);
    CHECK(q.n_filters == 0 && strcmp(q.text, "ratio 3:1 today") == 0);

    /* A URL is text, not an in: filter, even though it contains a colon. */
    oc_searchq_parse("https://example.com/x", &q);
    CHECK(q.n_filters == 0 && strcmp(q.text, "https://example.com/x") == 0);
}

static void test_describe(void) {
    oc_searchq q; char out[256];
    oc_searchq_parse("in:#design from:alice has:file after:2026-01-01 rollback", &q);
    oc_searchq_describe(&q, out, sizeof out);
    /* Round-trips into something a user can read AND re-type. */
    CHECK(strstr(out, "from:alice") != NULL);
    CHECK(strstr(out, "in:design") != NULL);
    CHECK(strstr(out, "has:file") != NULL);
    CHECK(strstr(out, "after:2026-01-01") != NULL);
    CHECK(strstr(out, "rollback") != NULL);

    /* Re-parsing the description yields the same filters — the property that makes it
     * safe to show as the query. */
    oc_searchq q2;
    oc_searchq_parse(out, &q2);
    CHECK(strcmp(q2.from, q.from) == 0 && strcmp(q2.in, q.in) == 0);
    CHECK(q2.has == q.has && strcmp(q2.text, q.text) == 0);

    /* A tiny buffer truncates rather than overruns. */
    char tiny[8];
    oc_searchq_describe(&q, tiny, sizeof tiny);
    CHECK(strlen(tiny) < sizeof tiny);
}

int run_searchq_tests(void) {
    failures = 0;
    test_plain();
    test_operators();
    test_not_swallowed();
    test_describe();
    printf("test_searchq: plain text, operators, quoting, unknown values kept, describe round-trip\n");
    return failures;
}
