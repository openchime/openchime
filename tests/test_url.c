/* shared/url.c — the address rules of MARKDOWN.md §4, linked by the client
 * (autolink rendering) and the daemon (the unfurl fetcher, ARCH-105). Its own
 * suite for the same reason mention.c has one: both sides link this single
 * implementation, so its edge cases are where the two products would otherwise
 * disagree about what an address is. */

#include <stdio.h>
#include <string.h>

#include "check.h"
#include "url.h"

/* oc_url_len over a C string at offset `at`, expecting `want` bytes. */
static int len_at(const char *s, size_t at) {
    return (int)oc_url_len(s, strlen(s), at);
}

static int test_extent(void) {
    int failures = 0;

    /* The plain cases. */
    CHECK(len_at("https://example.com", 0) == 19);
    CHECK(len_at("http://example.com", 0) == 18);
    CHECK(len_at("HTTPS://EXAMPLE.COM", 0) == 19);      /* scheme is case-blind */

    /* Trailing punctuation belongs to the sentence, not the address. */
    CHECK(len_at("https://example.com.", 0) == 19);
    CHECK(len_at("https://example.com,", 0) == 19);
    CHECK(len_at("https://example.com!?", 0) == 19);

    /* Bracket balance: a paren the address opened stays; one that closes the
     * sentence's does not. */
    CHECK(len_at("https://en.wikipedia.org/wiki/Foo_(bar)", 0) == 39);
    {
        const char *s = "(see https://example.com/a)";
        CHECK(len_at(s, 5) == 21);                      /* drops the ')' */
    }

    /* `*`/`~` trim so an emphasised URL links the address; `_` deliberately
     * does not, because underscores are ordinary inside real addresses. */
    CHECK(len_at("*https://example.com*", 1) == 19);
    CHECK(len_at("_https://example.com_", 1) == 20);    /* keeps the '_' */

    /* A scheme with no address is text. */
    CHECK(len_at("https://", 0) == 0);
    CHECK(len_at("https:// then words", 0) == 0);

    /* Only http and https; anything else stays ordinary text. */
    CHECK(len_at("file:///etc/passwd", 0) == 0);
    CHECK(len_at("javascript:alert(1)", 0) == 0);
    CHECK(len_at("mailto:a@b.com", 0) == 0);

    /* The opening boundary: start, whitespace, opening bracket, or a run of
     * formatting delimiters looked through to one of those — never a word. */
    CHECK(len_at("xhttps://example.com", 1) == 0);
    CHECK(len_at("see https://example.com", 4) == 19);
    CHECK(len_at("(https://example.com)", 1) == 19);
    {
        const char *s = "a*https://example.com*";        /* run traces to 'a' */
        CHECK(len_at(s, 2) == 0);
    }

    /* A stop byte ends the address outright. */
    CHECK(len_at("https://a.com>rest", 0) == 13);
    CHECK(len_at("https://a.com\"quoted", 0) == 13);

    return failures;
}

static int test_extract(void) {
    int failures = 0;
    oc_url_span sp[4];

    /* Two addresses, in order. */
    {
        const char *s = "see https://a.com and http://b.org/x today";
        size_t n = oc_url_extract(s, strlen(s), sp, 4);
        CHECK(n == 2);
        CHECK(sp[0].start == 4 && sp[0].len == 13);
        CHECK(sp[1].start == 22 && sp[1].len == 14);
    }

    /* The cap is a cap, not a crash. */
    {
        const char *s = "https://a.com https://b.com https://c.com";
        size_t n = oc_url_extract(s, strlen(s), sp, 2);
        CHECK(n == 2);
    }

    /* What a client renders literally is not extracted: inline code and a
     * closed fenced block suppress addresses, exactly as they suppress
     * emphasis in the formatting parser. */
    {
        const char *s = "`https://a.com`";
        CHECK(oc_url_extract(s, strlen(s), sp, 4) == 0);
    }
    {
        const char *s = "```\nhttps://a.com\n``` then https://b.org";
        size_t n = oc_url_extract(s, strlen(s), sp, 4);
        CHECK(n == 1);
        CHECK(strncmp(s + sp[0].start, "https://b.org", sp[0].len) == 0);
    }
    /* An UNCLOSED fence is literal text, so its address counts. */
    {
        const char *s = "``` https://a.com";
        CHECK(oc_url_extract(s, strlen(s), sp, 4) == 1);
    }

    /* Degenerate inputs. */
    CHECK(oc_url_extract(NULL, 0, sp, 4) == 0);
    CHECK(oc_url_extract("x", 1, sp, 0) == 0);
    {
        const char *s = "no links here";
        CHECK(oc_url_extract(s, strlen(s), sp, 4) == 0);
    }

    return failures;
}

int run_url_tests(void) {
    int failures = 0;
    printf("url: extent\n");
    failures += test_extent();
    printf("url: extract\n");
    failures += test_extract();
    return failures;
}
