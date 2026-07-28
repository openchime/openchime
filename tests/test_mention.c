/* @mention scanning (REQ-221, ARCH-89).
 *
 * This scanner decides two things in two processes — whether the daemon
 * notifies you and whether your client highlights the text. The rules are only
 * safe to share if they are pinned down, so the edge cases are the point of
 * this suite, not an afterthought. */

#include "check.h"
#include "mention.h"

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
    CHECK(oc_mention_targets(S("hey @alice"), "alice") == 1);
    CHECK(oc_mention_targets(S("hey @ALICE"), "alice") == 1);   /* case-insensitive */
    CHECK(oc_mention_targets(S("hey @alice"), "bob") == 0);
    CHECK(oc_mention_targets(S("hey @here"), "bob") == 1);      /* broadcast hits all */
    CHECK(oc_mention_targets(S("hey @here"), NULL) == 1);
    CHECK(oc_mention_targets(S("hey @alice"), NULL) == 0);      /* NULL asks only about broadcasts */
    CHECK(oc_mention_targets(S("nothing"), "alice") == 0);
    CHECK(oc_mention_targets(S("danny@example.com"), "example.com") == 0);
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
    printf("test_mention: scan, word boundaries, trailing punctuation, broadcasts, spans, truncation, targets\n");
    return failures;
}
