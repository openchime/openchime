/* @mention scanning (REQ-221, ARCH-89).
 *
 * This scanner decides two things in two processes — whether the daemon
 * notifies you and whether your client highlights the text. The rules are only
 * safe to share if they are pinned down, so the edge cases are the point of
 * this suite, not an afterthought. */

#include "check.h"
#include "mention.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define S(lit) (lit), strlen(lit)

/* How many mentions does `body` contain? */
static size_t count(const char *body) {
    oc_mention m[16];
    return oc_mention_scan(body, strlen(body), m, 16);
}

/* The first mention's name, or "" if none. */
static const char *first(const char *body, uint8_t *kind) {
    static oc_mention m[16];
    size_t n = oc_mention_scan(body, strlen(body), m, 16);
    if (n == 0) { if (kind) *kind = 255; return ""; }
    if (kind) *kind = m[0].kind;
    return m[0].name;
}

static void test_basic(void) {
    uint8_t k;
    CHECK(count("hello @alice") == 1);
    CHECK(strcmp(first("hello @alice", &k), "alice") == 0 && k == OC_MENTION_USER);

    CHECK(count("@alice") == 1);                 /* at the very start */
    CHECK(count("@alice @bob @carol") == 3);
    CHECK(count("no mentions here") == 0);
    CHECK(count("") == 0);
    CHECK(count("@") == 0);                      /* bare sigil is not a mention */
    CHECK(count("@ alice") == 0);
}

static void test_word_boundary(void) {
    /* The case that makes a naive scanner notify half the workspace. */
    CHECK(count("mail me at danny@example.com") == 0);
    CHECK(count("a@b") == 0);
    CHECK(count("foo@alice") == 0);
    /* But punctuation before it is still a boundary. */
    CHECK(count("(@alice)") == 1);
    CHECK(count("hey,@alice") == 1);
    CHECK(count("\"@alice\"") == 1);
    CHECK(count("line\n@alice") == 1);
}

static void test_trailing_punctuation(void) {
    /* '.' and '-' are legal *inside* a username, so they cannot simply be
     * excluded — they have to be trimmed only at the end. */
    CHECK(strcmp(first("thanks @alice.", NULL), "alice") == 0);
    CHECK(strcmp(first("@alice, hi", NULL), "alice") == 0);
    CHECK(strcmp(first("@alice...", NULL), "alice") == 0);
    CHECK(strcmp(first("@first.last says", NULL), "first.last") == 0);
    CHECK(strcmp(first("@a-b_c!", NULL), "a-b_c") == 0);
}

static void test_broadcasts(void) {
    uint8_t k;
    first("@here now", &k);      CHECK(k == OC_MENTION_HERE);
    first("@channel now", &k);   CHECK(k == OC_MENTION_CHANNEL);
    first("@everyone now", &k);  CHECK(k == OC_MENTION_EVERYONE);
    /* Case-insensitive, and reserved even if a user is called that. */
    first("@HERE", &k);          CHECK(k == OC_MENTION_HERE);
    first("@Channel", &k);       CHECK(k == OC_MENTION_CHANNEL);
    first("@alice", &k);         CHECK(k == OC_MENTION_USER);
}

static void test_spans(void) {
    /* The span has to cover the '@' and stop before trailing punctuation, or a
     * client highlights the wrong bytes. */
    oc_mention m[4];
    const char *b = "hi @alice.";
    CHECK(oc_mention_scan(b, strlen(b), m, 4) == 1);
    CHECK(m[0].start == 3);
    CHECK(m[0].len == 6);                                  /* "@alice" */
    CHECK(memcmp(b + m[0].start, "@alice", 6) == 0);

    const char *c = "@a @bb";
    CHECK(oc_mention_scan(c, strlen(c), m, 4) == 2);
    CHECK(m[0].start == 0 && m[0].len == 2);
    CHECK(m[1].start == 3 && m[1].len == 3);
}

static void test_truncation_and_limits(void) {
    /* The return value counts everything found, not what fitted, so a caller
     * can tell it lost some. */
    oc_mention m[2];
    const char *b = "@a @b @c @d";
    CHECK(oc_mention_scan(b, strlen(b), m, 2) == 4);
    CHECK(strcmp(m[0].name, "a") == 0 && strcmp(m[1].name, "b") == 0);

    /* An absurdly long run is not a mention rather than a buffer overrun. */
    char big[300];
    big[0] = '@';
    memset(big + 1, 'x', sizeof big - 2);
    big[sizeof big - 1] = '\0';
    CHECK(oc_mention_scan(big, strlen(big), m, 2) == 0);

    /* Not NUL-terminated: the length is authoritative. */
    const char *raw = "@alicexxxx";
    CHECK(oc_mention_scan(raw, 6, m, 2) == 1);
    CHECK(strcmp(m[0].name, "alice") == 0);
}

static void test_targets(void) {
    CHECK(oc_mention_targets(S("hey @alice"), "alice", 0) == 1);
    CHECK(oc_mention_targets(S("hey @ALICE"), "alice", 0) == 1);   /* case-insensitive */
    CHECK(oc_mention_targets(S("hey @alice"), "bob", 0) == 0);
    CHECK(oc_mention_targets(S("hey @here"), "bob", 0) == 1);      /* broadcast hits all */
    CHECK(oc_mention_targets(S("hey @here"), NULL, 0) == 1);
    CHECK(oc_mention_targets(S("hey @alice"), NULL, 0) == 0);      /* NULL asks only about broadcasts */
    CHECK(oc_mention_targets(S("nothing"), "alice", 0) == 0);
    CHECK(oc_mention_targets(S("danny@example.com"), "example.com", 0) == 0);
}

/* The id form, <@U42> and <@U42|label>: what apps written for other chat
 * platforms post. A broken one is no mention at all -- not even of someone
 * called "U42" -- or a malformed token would notify by accident. */
static void test_id_form(void) {
    oc_mention m[8];
    const char *b = "hi <@U42> there";
    CHECK(oc_mention_scan(b, strlen(b), m, 8) == 1);
    CHECK(m[0].start == 3 && m[0].len == 6 && m[0].kind == OC_MENTION_USER);
    CHECK(m[0].user_id == 42 && m[0].name[0] == '\0');

    const char *l = "<@U7|Ada Lovelace>, look";
    CHECK(oc_mention_scan(l, strlen(l), m, 8) == 1);
    CHECK(m[0].start == 0 && m[0].len == strlen("<@U7|Ada Lovelace>"));
    CHECK(m[0].user_id == 7 && strcmp(m[0].name, "Ada Lovelace") == 0);

    /* Glued to a word is still the id form: the brackets delimit it. */
    CHECK(oc_mention_scan(S("ping:<@U9>!"), m, 8) == 1 && m[0].user_id == 9);
    /* 20 digits is the most an id is written with. */
    CHECK(oc_mention_scan(S("<@U18446744073709551615>"), m, 8) == 1 &&
          m[0].user_id == 18446744073709551615ULL);

    /* Malformed: not a mention. */
    CHECK(count("<@U42") == 0);                     /* no '>' */
    CHECK(count("<@U42 and more") == 0);
    CHECK(count("<@U>") == 0);                      /* no digits */
    CHECK(count("<@U|bob>") == 0);
    CHECK(count("<@U123456789012345678901>") == 0); /* 21 digits */
    CHECK(count("<@U18446744073709551616>") == 0);  /* past what an id can be */
    CHECK(count("<@U0>") == 0);                     /* no user has id 0 */
    CHECK(count("<@U42|first\nsecond>") == 0);      /* newline in the label */
    CHECK(count("<@U42|a>b>") == 1);                /* '>' ends the label: "a" */
    CHECK(count("<@U42|unclosed") == 0);
    {
        char big[OC_MENTION_NAME_MAX + 16];
        int o = snprintf(big, sizeof big, "<@U42|");
        memset(big + o, 'x', OC_MENTION_NAME_MAX);
        big[o + OC_MENTION_NAME_MAX] = '>';
        big[o + OC_MENTION_NAME_MAX + 1] = '\0';
        CHECK(count(big) == 0);                     /* label too long */
        big[o + OC_MENTION_NAME_MAX - 1] = '>';
        big[o + OC_MENTION_NAME_MAX] = '\0';
        CHECK(count(big) == 1);                     /* one shorter is a label */
    }
    /* Not the id form at all: the '@' rule as before. */
    CHECK(strcmp(first("<@Ursula>", NULL), "Ursula") == 0);
    CHECK(count("<@u42>") == 1 && first("<@u42>", NULL)[0] == 'u');

    /* Mixed with names and broadcasts, in order. */
    const char *x = "@alice <@U42|bob> @here <@U3> ok";
    CHECK(oc_mention_scan(x, strlen(x), m, 8) == 4);
    CHECK(m[0].user_id == 0 && strcmp(m[0].name, "alice") == 0);
    CHECK(m[1].user_id == 42 && strcmp(m[1].name, "bob") == 0 && m[1].start == 7);
    CHECK(m[2].kind == OC_MENTION_HERE && m[2].user_id == 0);
    CHECK(m[3].user_id == 3 && memcmp(x + m[3].start, "<@U3>", m[3].len) == 0);
}

static const char *roster(void *ctx, uint64_t id) {
    (void)ctx;
    return id == 42 ? "ada" : id == 7 ? "Grace Hopper" : NULL;
}

static int shows(const char *body, const char *want) {
    char *d = oc_mention_display(body, strlen(body), roster, NULL);
    const char *got = d ? d : body;
    int ok = strcmp(got, want) == 0;
    if (!ok) printf("    body \"%s\"\n    shown \"%s\"\n    wanted \"%s\"\n", body, got, want);
    free(d);
    return ok;
}

static void test_display(void) {
    CHECK(shows("hi <@U42>", "hi @ada"));
    CHECK(shows("<@U42|someone else> and <@U7>!", "@ada and @Grace Hopper!"));   /* the name, not the label */
    CHECK(shows("who is <@U99>?", "who is <@U99>?"));                              /* unknown: as written */
    CHECK(shows("<@U99|old name>", "<@U99|old name>"));
    CHECK(shows("@alice and <@U42> and <@U42", "@alice and @ada and <@U42"));
    CHECK(shows("nothing here", "nothing here"));
    /* Nothing replaced is NULL: the body shows as it is. */
    CHECK(oc_mention_display(S("plain @alice"), roster, NULL) == NULL);
    CHECK(oc_mention_display(S("<@U99>"), roster, NULL) == NULL);
    CHECK(oc_mention_display(S("<@U42>"), NULL, NULL) == NULL);
    /* What is shown scans as a mention by name. */
    {
        char *d = oc_mention_display(S("x <@U42> y"), roster, NULL);
        oc_mention m[4];
        CHECK(d && oc_mention_scan(d, strlen(d), m, 4) == 1 && strcmp(m[0].name, "ada") == 0);
        free(d);
    }
}

static void test_targets_by_id(void) {
    CHECK(oc_mention_targets(S("hey <@U42>"), "ada", 42) == 1);
    CHECK(oc_mention_targets(S("hey <@U42>"), NULL, 42) == 1);
    CHECK(oc_mention_targets(S("hey <@U42>"), "ada", 43) == 0);
    CHECK(oc_mention_targets(S("hey <@U42>"), "ada", 0) == 0);
    /* A label is not a name: it names whoever the id is. */
    CHECK(oc_mention_targets(S("hey <@U42|bob>"), "bob", 7) == 0);
    CHECK(oc_mention_targets(S("hey <@U42|bob>"), "bob", 42) == 1);
    CHECK(oc_mention_targets(S("<@U5> @here"), "bob", 7) == 1);   /* broadcast still hits */
}

/* Keyword matching (REQ-135, ARCH-103). The rules that matter are the two ends:
 * a term must not match inside a longer word, in either direction, or a
 * highlight-word becomes a word you turn off. */
static void test_keywords(void) {
    size_t st = 0, ln = 0;
    CHECK(oc_keyword_match(S("ship the deploy now"), "deploy", &st, &ln) == 1);
    CHECK(st == 9 && ln == 6);
    CHECK(oc_keyword_match(S("Deploy at noon"), "deploy", NULL, NULL) == 1);   /* case */
    CHECK(oc_keyword_match(S("deploy"), "deploy", NULL, NULL) == 1);           /* whole body */
    CHECK(oc_keyword_match(S("the deployment plan"), "deploy", NULL, NULL) == 0);
    CHECK(oc_keyword_match(S("a redeploy happened"), "deploy", NULL, NULL) == 0);
    CHECK(oc_keyword_match(S("redeployment"), "deploy", NULL, NULL) == 0);
    /* Punctuation IS a boundary, so a word at the end of a sentence still hits. */
    CHECK(oc_keyword_match(S("deploy!"), "deploy", NULL, NULL) == 1);
    CHECK(oc_keyword_match(S("(deploy)"), "deploy", NULL, NULL) == 1);
    CHECK(oc_keyword_match(S("please deploy."), "deploy", NULL, NULL) == 1);
    /* Phrases are allowed, as Slack's are, and are matched whole. */
    CHECK(oc_keyword_match(S("the release train leaves"), "release train", NULL, NULL) == 1);
    CHECK(oc_keyword_match(S("the release trainer"), "release train", NULL, NULL) == 0);
    /* Degenerate input answers no rather than crashing or matching everything. */
    CHECK(oc_keyword_match(S("anything"), "", NULL, NULL) == 0);
    CHECK(oc_keyword_match(NULL, 0, "deploy", NULL, NULL) == 0);
    CHECK(oc_keyword_match(S("hi"), "a longer term than the body", NULL, NULL) == 0);
}

/* --- #channel references -------------------------------------------------- */

static size_t crefs(const char *body) {
    oc_chanref c[16];
    return oc_chanref_scan(body, strlen(body), c, 16);
}

static const char *cfirst(const char *body) {
    static oc_chanref c[16];
    size_t n = oc_chanref_scan(body, strlen(body), c, 16);
    return n ? c[0].name : "";
}

static void test_chanrefs(void) {
    CHECK(crefs("see #general") == 1);
    CHECK(strcmp(cfirst("see #general"), "general") == 0);
    CHECK(crefs("#general") == 1);                  /* at the very start */
    CHECK(crefs("#general and #random") == 2);
    CHECK(crefs("nothing here") == 0);
    CHECK(crefs("") == 0);
    CHECK(crefs("#") == 0);                         /* bare sigil */

    /* The same word boundary the '@' rule uses, on the cases it exists for. */
    CHECK(crefs("C#") == 0);
    CHECK(crefs("oc#dev") == 0);                    /* glued to a word */
    CHECK(crefs("oc #dev") == 1);                   /* ... but a lone one counts */

    /* Markdown headings are not references (REQ-220). */
    CHECK(crefs("# Notes") == 0);
    CHECK(crefs("### Notes") == 0);
    CHECK(crefs("#\tNotes") == 0);

    /* Trailing punctuation belongs to the sentence. */
    CHECK(strcmp(cfirst("in #general."), "general") == 0);
    CHECK(strcmp(cfirst("in #dev-ops, later"), "dev-ops") == 0);

    /* The span covers the sigil, so a caller can style what was written. */
    {
        oc_chanref c[4];
        const char *b = "go to #general now";
        CHECK(oc_chanref_scan(b, strlen(b), c, 4) == 1);
        CHECK(c[0].start == 6);
        CHECK(c[0].len == strlen("#general"));
        CHECK(memcmp(b + c[0].start, "#general", c[0].len) == 0);
    }

    /* Over the cap: the count is the truth, the array is what fitted. */
    {
        oc_chanref c[2];
        CHECK(oc_chanref_scan(S("#a #b #c #d"), c, 2) == 4);
        CHECK(strcmp(c[0].name, "a") == 0 && strcmp(c[1].name, "b") == 0);
    }

    /* A name too long to hold is not silently truncated into a shorter one. */
    {
        char big[OC_MENTION_NAME_MAX + 8];
        big[0] = '#';
        memset(big + 1, 'a', sizeof big - 2);
        big[sizeof big - 1] = '\0';
        CHECK(crefs(big) == 0);
    }
}

int run_mention_tests(void) {
    failures = 0;
    test_basic();
    test_word_boundary();
    test_trailing_punctuation();
    test_broadcasts();
    test_spans();
    test_truncation_and_limits();
    test_targets();
    test_id_form();
    test_display();
    test_targets_by_id();
    test_keywords();
    test_chanrefs();
    printf("test_mention: scan, word boundaries, trailing punctuation, broadcasts, spans, truncation, targets, id form, display, targets by id, keywords, #channel refs\n");
    return failures;
}
