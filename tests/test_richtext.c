/* Message formatting (REQ-220, ARCH-100).
 *
 * The parser both frontends render from, so the edge cases ARE the suite. The
 * single most common failure of an in-band dialect is eating text somebody
 * meant literally — `2 * 3 * 4`, `a_variable_name`, a half-typed asterisk — so
 * §2 of docs/MARKDOWN.md gets more checks here than the happy path does. */

#include "check.h"
#include "richtext.h"

#include <string.h>

static oc_rt_span g_sp[OC_RT_MAX];

static size_t scan(const char *body) {
    memset(g_sp, 0, sizeof g_sp);
    return oc_rt_scan(body, strlen(body), g_sp, OC_RT_MAX);
}

/* Is there a span covering exactly `text` (its first occurrence in `body`) with
 * exactly `style`? Written against the text rather than an offset so a check
 * says what it means. */
static int span_over(const char *body, const char *text, uint16_t style) {
    const char *p = strstr(body, text);
    size_t start, len, i, n;
    if (!p) return 0;
    start = (size_t)(p - body);
    len   = strlen(text);
    n = scan(body);
    if (n > OC_RT_MAX) n = OC_RT_MAX;
    for (i = 0; i < n; i++)
        if (g_sp[i].start == start && g_sp[i].len == len && g_sp[i].style == style) return 1;
    return 0;
}

/* How many spans carry `style` (ignoring delimiters)? */
static int content_spans(const char *body, uint16_t style) {
    size_t n = scan(body), i;
    int k = 0;
    if (n > OC_RT_MAX) n = OC_RT_MAX;
    for (i = 0; i < n; i++)
        if (g_sp[i].style == style) k++;
    return k;
}

static void test_emphasis(void) {
    CHECK(span_over("say *bold* now", "bold", OC_RT_BOLD));
    CHECK(span_over("say *bold* now", "*", OC_RT_BOLD | OC_RT_DELIM));
    CHECK(scan("say *bold* now") == 3);          /* open, content, close */

    CHECK(span_over("an _italic_ word", "italic", OC_RT_ITALIC));
    CHECK(span_over("a ~struck~ word", "struck", OC_RT_STRIKE));
    CHECK(span_over("a `code` word", "code", OC_RT_CODE));

    /* **bold** too: everyone arriving from Markdown types two (MARKDOWN.md §4). */
    CHECK(span_over("say **bold** now", "bold", OC_RT_BOLD));
    CHECK(span_over("say **bold** now", "**", OC_RT_BOLD | OC_RT_DELIM));

    /* At the very start and the very end of the body. */
    CHECK(span_over("*x*", "x", OC_RT_BOLD));
    CHECK(span_over("(*x*)", "x", OC_RT_BOLD));  /* an opening bracket is a boundary */
}

static void test_not_markup(void) {
    /* Rule 1 — word-boundary anchoring. */
    CHECK(scan("2 * 3 * 4") == 0);               /* arithmetic, not emphasis */
    CHECK(scan("a_variable_name") == 0);         /* an identifier */
    CHECK(scan("snake_case_here too") == 0);
    CHECK(scan("5*3=15") == 0);
    CHECK(scan("* leading bullet-ish") == 0);    /* opener followed by a space */
    CHECK(scan("closing not *ok *") == 0);       /* closer preceded by a space */

    /* A run of delimiters is looked THROUGH to find the boundary, so a doubled
     * delimiter inside a word is still inside a word. */
    CHECK(scan("snake__case__here") == 0);
    CHECK(scan("a**b** c") == 0);
    /* Only `**` means anything doubled; any longer run, or a double of the
     * other delimiters, is literal rather than half-matched. */
    CHECK(scan("~~struck~~") == 0);
    CHECK(scan("__loud__") == 0);
    CHECK(scan("***x***") == 0);

    /* Rule 2 — it must close on the same line. */
    CHECK(scan("*half typed") == 0);
    CHECK(scan("*a\nb*") == 0);
    CHECK(scan("_only one_side") == 3);          /* this one DOES close */
    CHECK(scan("a * b") == 0);

    /* A run of openers with nothing to close against is entirely literal —
     * partially matching a pair of them would be worse than leaving it alone.
     * The parser stops searching after the first failure (it cannot succeed
     * later within the same range), so the second line here is the check that
     * matters: giving up is scoped to the line, not to the message. */
    CHECK(scan("*a *b *c") == 0);
    CHECK(content_spans("*a *b\n*c* ok", OC_RT_BOLD) == 1);
    CHECK(span_over("*a *b\n*c* ok", "c", OC_RT_BOLD));

    /* Nothing at all in ordinary prose. */
    CHECK(scan("hello there, how are you?") == 0);
    CHECK(scan("") == 0);
    CHECK(oc_rt_scan(NULL, 0, g_sp, OC_RT_MAX) == 0);
    CHECK(oc_rt_scan("*x*", 3, NULL, 0) == 3);   /* counting with no output */
}

static void test_escapes(void) {
    /* Rule 3 — the backslash is reported so a frontend can hide it, and the
     * character after it is plain text with no span of its own. */
    CHECK(scan("\\*not bold\\*") == 2);
    CHECK(g_sp[0].start == 0 && g_sp[0].len == 1 && g_sp[0].style == OC_RT_DELIM);
    CHECK(g_sp[1].start == 10 && g_sp[1].style == OC_RT_DELIM);
    CHECK(scan("a \\\\ backslash") == 1);
    CHECK(scan("\\q is not an escape") == 0);    /* q is not escapable */

    /* An escaped delimiter cannot close an emphasis run. */
    CHECK(span_over("*a \\* b*", "a \\* b", OC_RT_BOLD));
}

static void test_code_suppresses(void) {
    /* Everything inside code is literal, including other delimiters. */
    CHECK(content_spans("`*x*`", OC_RT_BOLD) == 0);
    CHECK(span_over("`*x*`", "*x*", OC_RT_CODE));
    CHECK(content_spans("`a \\* b`", OC_RT_DELIM) == 0);   /* no escapes inside either */

    /* A code span inside emphasis does not end it early. */
    CHECK(span_over("*a `b*c` d*", "a `b*c` d", OC_RT_BOLD));

    /* A run of backticks is not an inline opener. */
    CHECK(content_spans("``x``", OC_RT_CODE) == 0);
}

static void test_code_block(void) {
    const char *b = "before\n```\nint x;\nmore\n```\nafter";
    CHECK(span_over(b, "int x;\nmore", OC_RT_CODEBLOCK));
    CHECK(span_over(b, "```\n", OC_RT_CODEBLOCK | OC_RT_DELIM));
    /* It is the one construct that may span lines — and it suppresses markup
     * on every one of them. */
    CHECK(content_spans("```\n*x*\n```", OC_RT_BOLD) == 0);
    /* Inline form. */
    CHECK(span_over("run ```make test``` now", "make test", OC_RT_CODEBLOCK));
    /* Unclosed: literal, and the text around it still parses. */
    CHECK(content_spans("``` *x*", OC_RT_CODEBLOCK) == 0);
    CHECK(content_spans("``` *x*", OC_RT_BOLD) == 1);
    /* Text before and after a block is parsed normally. */
    CHECK(content_spans("*a*\n```\nq\n```\n*b*", OC_RT_BOLD) == 2);
}

static void test_blocks(void) {
    CHECK(span_over("> quoted", "quoted", OC_RT_QUOTE));
    CHECK(span_over("> quoted", "> ", OC_RT_QUOTE | OC_RT_DELIM));
    CHECK(span_over("- item", "item", OC_RT_BULLET));
    CHECK(span_over("- item", "- ", OC_RT_BULLET | OC_RT_DELIM));
    CHECK(span_over("1. item", "item", OC_RT_ORDERED));
    CHECK(span_over("12. item", "12. ", OC_RT_ORDERED | OC_RT_DELIM));

    /* Markers are line-anchored, and a bullet needs its space. */
    CHECK(content_spans("a > b", OC_RT_QUOTE) == 0);
    CHECK(content_spans("-1 degrees", OC_RT_BULLET) == 0);
    CHECK(content_spans("mid - dash", OC_RT_BULLET) == 0);
    CHECK(content_spans("version 1. x", OC_RT_ORDERED) == 0);
    CHECK(content_spans("- a\n- b\n- c", OC_RT_BULLET) == 3);
    CHECK(content_spans("  - indented", OC_RT_BULLET) == 1);
    CHECK(content_spans("\\- not a bullet", OC_RT_BULLET) == 0);

    /* Inline markup still applies inside a block line, and blocks compose. */
    CHECK(span_over("> a *b* c", "b", OC_RT_BOLD));
    CHECK(span_over("> - item", "item", OC_RT_BULLET));
}

static void test_nesting(void) {
    const char *b = "*bold with _italic_ inside*";
    CHECK(span_over(b, "bold with _italic_ inside", OC_RT_BOLD));
    CHECK(span_over(b, "italic", OC_RT_ITALIC));
    /* Adjacent openers nest: a delimiter is itself a word boundary. */
    CHECK(span_over("*_x_*", "x", OC_RT_ITALIC));
    CHECK(span_over("*_x_*", "_x_", OC_RT_BOLD));

    /* Spans arrive sorted by start, enclosing first. */
    {
        size_t n = scan(b), i;
        for (i = 1; i < n && i < OC_RT_MAX; i++) CHECK(g_sp[i - 1].start <= g_sp[i].start);
    }
}

static void test_offsets_are_source_bytes(void) {
    /* The whole contract: spans address the ORIGINAL body, so a multi-byte
     * character before a span shifts it by its bytes, not by its glyphs. */
    const char *b = "caf\xc3\xa9 *x*";        /* "café *x*" */
    size_t n = scan(b);
    CHECK(n == 3);
    CHECK(g_sp[1].start == 7 && g_sp[1].len == 1);
    CHECK(memcmp(b + g_sp[1].start, "x", 1) == 0);
}

/* Autolinking (MARKDOWN.md §4). The span is what a frontend hands to the shell,
 * so the checks that matter are the ones about what does NOT become one. */
static void test_autolink(void) {
    CHECK(span_over("see https://example.com now", "https://example.com", OC_RT_LINK));
    CHECK(span_over("see http://example.com now", "http://example.com", OC_RT_LINK));
    CHECK(span_over("HTTPS://EXAMPLE.COM", "HTTPS://EXAMPLE.COM", OC_RT_LINK));
    CHECK(span_over("https://example.com/a/b?q=1&r=2#frag", "https://example.com/a/b?q=1&r=2#frag", OC_RT_LINK));
    /* No delimiter span: the address is its own label. */
    CHECK(scan("https://example.com") == 1);

    /* ONLY http(s). Everything else stays text, because this list is the set of
     * schemes a message can ask the OS to open. */
    CHECK(content_spans("file:///etc/passwd", OC_RT_LINK) == 0);
    CHECK(content_spans("javascript:alert(1)", OC_RT_LINK) == 0);
    CHECK(content_spans("mailto:a@b.com", OC_RT_LINK) == 0);
    CHECK(content_spans("ftp://example.com", OC_RT_LINK) == 0);
    CHECK(content_spans("openchime://host/c/1/m/2", OC_RT_LINK) == 0);
    /* A scheme with no address is not a link. */
    CHECK(content_spans("https://", OC_RT_LINK) == 0);
    /* Not at a word boundary — the guard against a scheme inside a longer token. */
    CHECK(content_spans("xhttps://example.com", OC_RT_LINK) == 0);

    /* Trailing punctuation belongs to the sentence. */
    CHECK(span_over("go to https://example.com.", "https://example.com", OC_RT_LINK));
    CHECK(span_over("https://example.com, then", "https://example.com", OC_RT_LINK));
    CHECK(span_over("really? https://example.com?!", "https://example.com", OC_RT_LINK));
    /* A bracket the address opened is kept; one it did not is not. */
    CHECK(span_over("(see https://example.com/a)", "https://example.com/a", OC_RT_LINK));
    CHECK(span_over("https://en.wikipedia.org/wiki/Foo_(bar)",
                    "https://en.wikipedia.org/wiki/Foo_(bar)", OC_RT_LINK));
    CHECK(span_over("(https://example.com/x_(y)).", "https://example.com/x_(y)", OC_RT_LINK));

    /* Code suppresses it, like everything else. */
    CHECK(content_spans("`https://example.com`", OC_RT_LINK) == 0);
    CHECK(content_spans("```\nhttps://example.com\n```", OC_RT_LINK) == 0);

    /* It composes with emphasis rather than fighting it: the delimiters stay
     * outside the address, and an underscore INSIDE one cannot close a run that
     * opened before it. */
    CHECK(span_over("*https://example.com*", "https://example.com", OC_RT_LINK));
    CHECK(span_over("*https://example.com*", "https://example.com", OC_RT_BOLD));
    /* An underscore is NOT trimmed, so an address keeps the one it owns and the
     * emphasis is what gives way — see the note on url_trim(). */
    CHECK(span_over("_a https://example.com/a_b_", "https://example.com/a_b_", OC_RT_LINK));
    CHECK(content_spans("_a https://example.com/a_b_", OC_RT_ITALIC) == 0);
    CHECK(span_over("https://en.wikipedia.org/wiki/Foo_bar_", "https://en.wikipedia.org/wiki/Foo_bar_", OC_RT_LINK));
    /* A quoted or bulleted line still autolinks. */
    CHECK(span_over("> see https://example.com", "https://example.com", OC_RT_LINK));
    CHECK(span_over("- see https://example.com", "https://example.com", OC_RT_LINK));

    /* Every occurrence, and byte offsets into the ORIGINAL body. */
    CHECK(content_spans("https://a.com and https://b.com", OC_RT_LINK) == 2);
    {
        const char *b = "caf\xc3\xa9 https://x.com";   /* "café https://x.com" */
        size_t n = scan(b);
        CHECK(n == 1);
        CHECK(g_sp[0].start == 6 && g_sp[0].len == 13);
    }
}

static void test_truncation(void) {
    /* Like the mention scanner: the count may exceed `max` so a caller can tell
     * it was truncated, and nothing is written past the bound. */
    oc_rt_span few[4];
    size_t n = oc_rt_scan("*a* *b* *c*", 11, few, 4);
    CHECK(n == 9);
    CHECK(few[3].start == 4);
}

int run_richtext_tests(void) {
    failures = 0;
    test_emphasis();
    test_not_markup();
    test_escapes();
    test_code_suppresses();
    test_code_block();
    test_blocks();
    test_nesting();
    test_offsets_are_source_bytes();
    test_autolink();
    test_truncation();
    printf("test_richtext: emphasis, literal delimiters, escapes, code, fenced blocks, "
           "quotes and lists, nesting, byte offsets, autolinking, truncation\n");
    return failures;
}
