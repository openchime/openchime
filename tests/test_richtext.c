/* Message formatting (REQ-220, ARCH-100).
 *
 * The parser both frontends render from, so the edge cases ARE the suite. The
 * single most common failure of an in-band dialect is eating text somebody
 * meant literally — `2 * 3 * 4`, `a_variable_name`, a half-typed asterisk — so
 * §2 of docs/MARKDOWN.md gets more checks here than the happy path does. */

#include "check.h"
#include "richtext.h"

#include <stdio.h>
#include <stdlib.h>
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

/* The address a labelled link's label opens, as a string ("" when none). */
static const char *target_of(const char *body, const char *label) {
    static char out[256];
    const char *p = strstr(body, label);
    size_t n = scan(body), i, off, tl;
    out[0] = 0;
    if (!p) return out;
    if (n > OC_RT_MAX) n = OC_RT_MAX;
    for (i = 0; i < n; i++)
        if (g_sp[i].style == OC_RT_LABELLED && g_sp[i].start == (size_t)(p - body) &&
            g_sp[i].len == strlen(label) &&
            oc_rt_target(body, strlen(body), &g_sp[i], &off, &tl) && tl < sizeof out) {
            memcpy(out, body + off, tl); out[tl] = 0;
        }
    return out;
}

static void test_labelled_links(void) {
    const char *b = "read [the guide](https://example.com/guide) first";
    CHECK(span_over(b, "the guide", OC_RT_LABELLED));
    CHECK(span_over(b, "[", OC_RT_LABELLED | OC_RT_DELIM));
    CHECK(span_over(b, "](https://example.com/guide)", OC_RT_LABELLED | OC_RT_DELIM));
    CHECK(strcmp(target_of(b, "the guide"), "https://example.com/guide") == 0);
    CHECK(scan(b) == 3);                          /* open, label, close: no bare LINK */
    CHECK(content_spans(b, OC_RT_LINK) == 0);

    /* The address follows the autolink rules exactly: a bracket it owns is kept,
     * and the one that closes the link is not part of it. */
    b = "[Foo](https://en.wikipedia.org/wiki/Foo_(bar))";
    CHECK(strcmp(target_of(b, "Foo"), "https://en.wikipedia.org/wiki/Foo_(bar)") == 0);

    /* Emphasis inside the label; the label beside ordinary markup. */
    CHECK(span_over("[*bold* label](https://a.com)", "bold", OC_RT_BOLD));
    CHECK(span_over("*see [x](https://a.com/a_b)*", "see [x](https://a.com/a_b)", OC_RT_BOLD));

    /* Only http(s): anything else is literal text, as a bare one would be. */
    CHECK(content_spans("[click](javascript:alert(1))", OC_RT_LABELLED) == 0);
    CHECK(content_spans("[f](file:///etc/passwd)", OC_RT_LABELLED) == 0);
    CHECK(content_spans("[m](mailto:a@b.com)", OC_RT_LABELLED) == 0);

    /* Not quite a link: literal, and a bare address inside still autolinks. */
    CHECK(content_spans("[x] (https://a.com)", OC_RT_LABELLED) == 0);
    CHECK(content_spans("[](https://a.com)", OC_RT_LABELLED) == 0);
    CHECK(content_spans("[x](https://a.com", OC_RT_LABELLED) == 0);
    CHECK(content_spans("[x](https://a.com).", OC_RT_LABELLED) == 1);
    CHECK(content_spans("[x](https://a.com. )", OC_RT_LABELLED) == 0);
    CHECK(content_spans("[a [b] c](https://a.com)", OC_RT_LABELLED) == 0);
    CHECK(content_spans("[x\ny](https://a.com)", OC_RT_LABELLED) == 0);
    CHECK(span_over("[x](https://a.com", "https://a.com", OC_RT_LINK));

    /* Code suppresses it. */
    CHECK(content_spans("`[x](https://a.com)`", OC_RT_LABELLED) == 0);
    CHECK(content_spans("```\n[x](https://a.com)\n```", OC_RT_LABELLED) == 0);

    /* A label that reads as another address still gets a label span; the
     * frontend's job is to show where it really goes (MARKDOWN.md §4). */
    b = "[https://bank.com](https://evil.example)";
    CHECK(strcmp(target_of(b, "https://bank.com"), "https://evil.example") == 0);

    /* Only a label span has a target. */
    {
        oc_rt_span sp = { 0, 3, OC_RT_LINK };
        size_t o, l;
        CHECK(!oc_rt_target("abc", 3, &sp, &o, &l));
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

/* ---- tables (MARKDOWN.md §1a) -------------------------------------------- */

static oc_rt_table g_tb[8];

static size_t tables(const char *body) {
    memset(g_tb, 0, sizeof g_tb);
    return oc_rt_tables(body, strlen(body), g_tb, 8);
}

/* Cell (row, col) of the first table, as a string -- bounded. */
static const char *cell(const char *body, uint32_t want_row, uint8_t col) {
    static char buf[256];
    oc_rt_rowit it;
    oc_rt_cell cells[OC_RT_TABLE_MAX_COLS];
    uint32_t row;
    buf[0] = '\0';
    oc_rt_rows_begin(&it, body, strlen(body), &g_tb[0]);
    while (oc_rt_row_next(&it, cells, &row))
        if (row == want_row && col < g_tb[0].cols) {
            size_t n = cells[col].len < sizeof buf - 1 ? cells[col].len : sizeof buf - 1;
            memcpy(buf, body + cells[col].start, n);
            buf[n] = '\0';
            return buf;
        }
    return "(none)";
}

static void test_tables(void) {
    /* The ordinary case: header, separator, rows; leading and trailing pipes. */
    const char *t1 = "Numbers:\n| Region | Q1 | Q2 |\n|---|:-:|--:|\n| East | 120 | 140 |\n| West | 90 | 110 |\nThoughts?";
    CHECK(tables(t1) == 1);
    CHECK(g_tb[0].cols == 3 && g_tb[0].rows == 2 && g_tb[0].header == 1);
    CHECK(g_tb[0].align[0] == OC_RT_ALIGN_NONE && g_tb[0].align[1] == OC_RT_ALIGN_CENTER &&
          g_tb[0].align[2] == OC_RT_ALIGN_RIGHT);
    CHECK(strncmp(t1 + g_tb[0].start, "| Region", 8) == 0);
    CHECK(t1[g_tb[0].start + g_tb[0].len] == '\n');               /* the last row, not its newline */
    CHECK(strcmp(cell(t1, 0, 0), "Region") == 0);
    CHECK(strcmp(cell(t1, 2, 2), "110") == 0);
    CHECK(strcmp(cell(t1, 1, 1), "120") == 0);
    /* oc_rt_scan: one span over the whole table, the text either side scanned. */
    CHECK(span_over(t1, "| Region | Q1 | Q2 |\n|---|:-:|--:|\n| East | 120 | 140 |\n| West | 90 | 110 |", OC_RT_TABLE));

    /* No outer pipes, and :--- is left. */
    CHECK(tables("a | b\n:--- | ---\n1 | 2") == 1 && g_tb[0].cols == 2 && g_tb[0].rows == 1);
    CHECK(g_tb[0].align[0] == OC_RT_ALIGN_LEFT);

    /* An all-empty header is no header. */
    CHECK(tables("| | |\n|---|---|\n| x | y |") == 1 && g_tb[0].header == 0 && g_tb[0].rows == 1);

    /* A short row has empty cells; extra cells are ignored. */
    const char *t2 = "| a | b | c |\n|---|---|---|\n| 1 |\n| 1 | 2 | 3 | 4 |";
    CHECK(tables(t2) == 1 && g_tb[0].rows == 2);
    CHECK(strcmp(cell(t2, 1, 2), "") == 0);
    CHECK(strcmp(cell(t2, 2, 2), "3") == 0);

    /* \| is a literal pipe, not a column. */
    const char *t3 = "| a | b |\n|---|---|\n| x \\| y | z |";
    CHECK(tables(t3) == 1 && g_tb[0].cols == 2);
    CHECK(strcmp(cell(t3, 1, 0), "x \\| y") == 0);

    /* A blank line ends it; so does a line with no pipe. */
    CHECK(tables("| a |\n|---|\n| 1 |\n\n| 2 |") == 1 && g_tb[0].rows == 1);
    CHECK(tables("| a |\n|---|\n| 1 |\nafter") == 1 && g_tb[0].rows == 1);
    /* A header alone, with no body rows, is still a table. */
    CHECK(tables("| a | b |\n|---|---|") == 1 && g_tb[0].rows == 0);

    /* Not tables, so scanned as text: no separator; separator cell count
     * differs; a separator cell that is not dashes; no pipe in the header;
     * inside a fenced block; behind a block marker; past the column limit. */
    CHECK(tables("| a | b |\n| 1 | 2 |") == 0);
    CHECK(tables("| a | b |\n|---|\n| 1 | 2 |") == 0);
    CHECK(tables("| a | b |\n|---|-x-|") == 0);
    CHECK(tables("a\n---") == 0);
    CHECK(tables("```\n| a | b |\n|---|---|\n```") == 0);
    CHECK(tables("> | a | b |\n> |---|---|") == 0);
    CHECK(tables("- | a | b |\n|---|---|") == 0);
    {
        char wide[512] = "", sep[512] = "";
        for (int i = 0; i <= OC_RT_TABLE_MAX_COLS; i++) { strcat(wide, "| c "); strcat(sep, "|---"); }
        strcat(wide, "|\n"); strcat(wide, sep); strcat(wide, "|");
        CHECK(tables(wide) == 0);
    }
    /* Malformed: the text is still text, emphasis and all. */
    CHECK(span_over("| *a* | b |\n| 1 | 2 |", "a", OC_RT_BOLD));

    /* CRLF line ends. */
    CHECK(tables("| a | b |\r\n|---|---|\r\n| 1 | 2 |\r\n") == 1 && g_tb[0].rows == 1);

    /* Two tables, apart. */
    CHECK(tables("| a |\n|---|\n| 1 |\n\ntext\n\n| b |\n|---|\n| 2 |\n| 3 |") == 2 && g_tb[1].rows == 2);

    /* No inline span reaches across cells: the asterisks are in different cells. */
    CHECK(content_spans("| *a | b* |\n|---|---|", OC_RT_BOLD) == 0);
}

static void test_table_cells(void) {
    oc_rt_span sp[16];
    /* A cell carries inline markup, with offsets into the whole body. */
    const char *b = "x | *bold* and <br/>next | `c|d`\n";
    size_t off = 4, len = 20, n = oc_rt_scan_cell(b, off, len, sp, 16);
    int bold = 0, brk = 0;
    for (size_t i = 0; i < n && i < 16; i++) {
        if (sp[i].style == OC_RT_BOLD && sp[i].start == 5 && sp[i].len == 4) bold = 1;
        if (sp[i].style == OC_RT_BREAK && strncmp(b + sp[i].start, "<br/>", 5) == 0) brk = 1;
    }
    CHECK(bold && brk);
    /* Every spelling of the break, and none outside a cell. */
    CHECK(oc_rt_scan_cell("<BR>", 0, 4, sp, 16) == 1 && sp[0].style == OC_RT_BREAK);
    CHECK(oc_rt_scan_cell("a<br />b", 0, 8, sp, 16) == 1 && sp[0].len == 6);
    CHECK(content_spans("a<br>b", OC_RT_BREAK) == 0);
    CHECK(oc_rt_scan_cell("<b>", 0, 3, sp, 16) == 0);
    /* \| in a cell: the backslash is markup to hide. */
    CHECK(oc_rt_scan_cell("x \\| y", 0, 6, sp, 16) == 1 && sp[0].style == OC_RT_DELIM && sp[0].start == 2);
    /* No block markers in a cell: a leading "- " is text. */
    CHECK(oc_rt_scan_cell("- item", 0, 6, sp, 16) == 0);
}

static void test_table_flatten(void) {
    char out[128];
    const char *b = "See:\n| a | b |\n|---|---|\n| 1 | 2 |\n| 3 | 4 |\nok";
    size_t n = oc_rt_flatten(b, strlen(b), out, sizeof out);
    CHECK(n == strlen(out));
    /* contract: the excerpt wording is what MARKDOWN.md §1a specifies */
    CHECK(strcmp(out, "See:\nTable: 2 rows \xC3\x97 2 columns\nok") == 0);
    const char *one = "| a |\n|---|\n| 1 |";
    oc_rt_flatten(one, strlen(one), out, sizeof out);
    /* contract: singular forms, as specified */
    CHECK(strcmp(out, "Table: 1 row \xC3\x97 1 column") == 0);
    /* No table: every byte kept. */
    oc_rt_flatten("plain | text", 12, out, sizeof out);
    CHECK(strcmp(out, "plain | text") == 0);
    /* Cut short, never inside a character: the multiplication sign is two
     * bytes, so a cut through it drops it whole. */
    char small[16];
    n = oc_rt_flatten(one, strlen(one), small, 16);
    CHECK(n <= 15 && small[n] == '\0');
    for (size_t i = 0; i < n; i++) CHECK((unsigned char)small[i] != 0xC3 || i + 1 < n);
}

/* Time tokens (MARKDOWN.md §1c): what is one, and what is not. */
static void test_time_tokens(void) {
    const char *b = "Meet at <time:2026-10-30T15:00Z> please";
    int64_t ms = 0;
    CHECK(span_over(b, "<time:2026-10-30T15:00Z>", OC_RT_TIME));
    CHECK(content_spans(b, OC_RT_TIME) == 1);
    {
        size_t n = scan(b), i;
        for (i = 0; i < n && g_sp[i].style != OC_RT_TIME; i++) {}
        CHECK(i < n && oc_rt_time_value(b, strlen(b), &g_sp[i], &ms) && ms == 1793372400000LL);
    }
    /* Seconds, and a leap day that exists. */
    CHECK(span_over("<time:2028-02-29T00:00:30Z>", "<time:2028-02-29T00:00:30Z>", OC_RT_TIME));
    /* Not a token: a day that does not exist, an hour past 23, no Z, an
     * offset instead of Z, a space for the T, a leap day in a common year. */
    CHECK(content_spans("<time:2026-02-30T10:00Z>", OC_RT_TIME) == 0);
    CHECK(content_spans("<time:2026-10-30T24:00Z>", OC_RT_TIME) == 0);
    CHECK(content_spans("<time:2026-10-30T15:00>", OC_RT_TIME) == 0);
    CHECK(content_spans("<time:2026-10-30T15:00+01:00>", OC_RT_TIME) == 0);
    CHECK(content_spans("<time:2026-10-30 15:00Z>", OC_RT_TIME) == 0);
    CHECK(content_spans("<time:2026-02-29T10:00Z>", OC_RT_TIME) == 0);
    CHECK(content_spans("<time:2026-10-30T15:00Z", OC_RT_TIME) == 0);
    /* Code is literal. */
    CHECK(content_spans("`<time:2026-10-30T15:00Z>`", OC_RT_TIME) == 0);
    CHECK(content_spans("```\n<time:2026-10-30T15:00Z>\n```", OC_RT_TIME) == 0);
    /* Inside emphasis it is still one. */
    CHECK(span_over("*by <time:2026-10-30T15:00Z>*", "<time:2026-10-30T15:00Z>", OC_RT_TIME));
    /* The token for a moment, and back. */
    {
        char t[48];
        CHECK(oc_rt_time_token(1793372400000LL, t, sizeof t) > 0 && strcmp(t, "<time:2026-10-30T15:00Z>") == 0);
        CHECK(oc_rt_time_token(1835395230000LL, t, sizeof t) > 0 && strcmp(t, "<time:2028-02-29T00:00:30Z>") == 0);
        CHECK(oc_rt_time_token(-60000LL, t, sizeof t) > 0 && strcmp(t, "<time:1969-12-31T23:59Z>") == 0);
        CHECK(oc_rt_time_token(1793372400000LL, t, 8) == 0);
    }
}

/* A moment as a reader is shown it: their zone, their clock, "today". */
static void test_time_text(void) {
    const int64_t at = 1793372400000LL;                 /* Friday 30 October 2026, 15:00 UTC */
    char out[96];
    /* The same day for the reader: the time alone. */
    oc_rt_clock utc0 = { 0, 1, 0, at };
    oc_rt_time_text(at, &utc0, OC_RT_TIME_SHORT, out, sizeof out);
    CHECK(strcmp(out, "15:00") == 0);
    /* UTC-8, same day there: 07:00. */
    oc_rt_clock la = { -480, 1, 0, at };
    oc_rt_time_text(at, &la, OC_RT_TIME_SHORT, out, sizeof out);
    CHECK(strcmp(out, "07:00") == 0);
    /* UTC+5:30, and the 12-hour clock. */
    oc_rt_clock in = { 330, 0, 0, at };
    oc_rt_time_text(at, &in, OC_RT_TIME_SHORT, out, sizeof out);
    CHECK(strcmp(out, "8:30 PM") == 0);
    /* UTC+14: across the date line it is Saturday there. "Now" is the same
     * instant, so it is today -- the reader's today, not UTC's. */
    oc_rt_clock kiri = { 840, 1, 0, at };
    oc_rt_time_text(at, &kiri, OC_RT_TIME_SHORT, out, sizeof out);
    CHECK(strcmp(out, "05:00") == 0);
    oc_rt_time_text(at, &kiri, OC_RT_TIME_FULL, out, sizeof out);
    /* contract: the form MARKDOWN.md §1c specifies */
    CHECK(strcmp(out, "Saturday 31 October 2026, 05:00 (UTC+14:00)") == 0);
    /* A day earlier than now, in the reader's zone: with the date. */
    oc_rt_clock later = { -480, 1, 0, at + 86400000LL };
    oc_rt_time_text(at, &later, OC_RT_TIME_SHORT, out, sizeof out);
    /* contract: the form MARKDOWN.md §1c specifies */
    CHECK(strcmp(out, "Fri 30 Oct, 07:00") == 0);
    /* Another year: with the year. */
    oc_rt_clock nextyear = { 0, 1, 0, at + 400LL * 86400000LL };
    oc_rt_time_text(at, &nextyear, OC_RT_TIME_SHORT, out, sizeof out);
    /* contract: the form MARKDOWN.md §1c specifies */
    CHECK(strcmp(out, "Fri 30 Oct 2026, 15:00") == 0);
    /* The full form names the zone used. */
    oc_rt_time_text(at, &in, OC_RT_TIME_FULL, out, sizeof out);
    /* contract: the form MARKDOWN.md §1c specifies */
    CHECK(strcmp(out, "Friday 30 October 2026, 8:30 PM (UTC+05:30)") == 0);
    oc_rt_time_text(at, &la, OC_RT_TIME_FULL, out, sizeof out);
    /* contract: the form MARKDOWN.md §1c specifies */
    CHECK(strcmp(out, "Friday 30 October 2026, 07:00 (UTC-08:00)") == 0);
    oc_rt_time_text(at, &utc0, OC_RT_TIME_FULL, out, sizeof out);
    /* contract: the form MARKDOWN.md §1c specifies */
    CHECK(strcmp(out, "Friday 30 October 2026, 15:00 (UTC)") == 0);
    /* Midnight and noon on the 12-hour clock. */
    oc_rt_clock h12 = { 0, 0, 0, at };
    oc_rt_time_text(at - 15LL * 3600000LL, &h12, OC_RT_TIME_SHORT, out, sizeof out);
    CHECK(strcmp(out, "12:00 AM") == 0);
    oc_rt_time_text(at - 3LL * 3600000LL, &h12, OC_RT_TIME_SHORT, out, sizeof out);
    CHECK(strcmp(out, "12:00 PM") == 0);
    /* No reader -- the daemon -- says UTC. */
    oc_rt_clock daemon = { 330, 1, 1, at };
    oc_rt_time_text(at, &daemon, OC_RT_TIME_SHORT, out, sizeof out);
    CHECK(strcmp(out, "15:00 UTC") == 0);
    oc_rt_time_text(at, &daemon, OC_RT_TIME_FULL, out, sizeof out);
    /* contract: the form MARKDOWN.md §1c specifies */
    CHECK(strcmp(out, "Friday 30 October 2026, 15:00 UTC") == 0);
}

/* An excerpt carries the time as text, in the reader's zone or UTC. */
static void test_time_flatten(void) {
    const int64_t at = 1793372400000LL;
    char out[160];
    const char *b = "Standup <time:2026-10-30T15:00Z>, and `<time:2026-10-30T15:00Z>` stays";
    oc_rt_clock in = { 330, 1, 0, at };
    oc_rt_flatten_at(b, strlen(b), &in, out, sizeof out);
    /* contract: the form MARKDOWN.md §1c specifies */
    CHECK(strcmp(out, "Standup 20:30, and `<time:2026-10-30T15:00Z>` stays") == 0);
    /* With a table before it, both are replaced, in order. */
    const char *t = "| a |\n|---|\n| 1 |\nthen <time:2026-10-30T15:00Z>";
    oc_rt_clock utc = { 0, 1, 1, at };
    oc_rt_flatten_at(t, strlen(t), &utc, out, sizeof out);
    /* contract: the excerpt wording is what MARKDOWN.md §1a and §1b specify */
    CHECK(strcmp(out, "Table: 1 row \xC3\x97 1 column\nthen 15:00 UTC") == 0);
    /* An invalid token is text, as written. */
    oc_rt_flatten_at("<time:2026-13-01T00:00Z>", 24, &in, out, sizeof out);
    CHECK(strcmp(out, "<time:2026-13-01T00:00Z>") == 0);
}

/* Spoilers (MARKDOWN.md §1d): both forms, and what is not one. */
static void test_spoilers(void) {
    oc_rt_spoiler sp[4];
    const char *b = "The killer is >!the *butler*!< obviously";
    CHECK(span_over(b, "the *butler*", OC_RT_SPOILER));
    CHECK(span_over(b, "butler", OC_RT_BOLD));                 /* emphasis inside */
    CHECK(oc_rt_spoilers(b, strlen(b), sp, 4) == 1);
    CHECK(sp[0].start == 14 && sp[0].len == 16 && sp[0].hidden == 16 && sp[0].hidden_len == 12 &&
          !sp[0].block && !sp[0].cell && sp[0].label_len == 0);
    /* Not a spoiler: empty, white space alone, across lines, unclosed, inside code. */
    CHECK(content_spans("a >!!< b", OC_RT_SPOILER) == 0);
    CHECK(content_spans("a >!   !< b", OC_RT_SPOILER) == 0);
    CHECK(content_spans(">!one\ntwo!<", OC_RT_SPOILER) == 0);
    CHECK(content_spans("a >!never closed", OC_RT_SPOILER) == 0);
    CHECK(content_spans("`>!code!<`", OC_RT_SPOILER) == 0);
    CHECK(content_spans("```\n>!code!<\n```", OC_RT_SPOILER) == 0);
    CHECK(oc_rt_spoilers("x >! y", 6, sp, 4) == 0);
    /* The old inline form is ordinary text, with no span at all. */
    CHECK(scan("||not hidden||") == 0);
    CHECK(scan("a || b || c") == 0);
    CHECK(oc_rt_spoilers("||not hidden||", 14, sp, 4) == 0);
    /* The block form, with a label and lines of text (not code) in it. */
    const char *k = "Before\n```spoiler Ending\nShe *lives*.\nThe end.\n```\nAfter";
    CHECK(span_over(k, "She *lives*.\nThe end.", OC_RT_SPOILER_BLOCK));
    CHECK(span_over(k, "lives", OC_RT_BOLD));
    CHECK(content_spans(k, OC_RT_CODEBLOCK) == 0);
    CHECK(oc_rt_spoilers(k, strlen(k), sp, 4) == 1 && sp[0].block);
    CHECK(sp[0].label_len == 6 && memcmp(k + sp[0].label, "Ending", 6) == 0);
    CHECK(sp[0].start == 7 && k[sp[0].start + sp[0].len] == '\n');
    /* No label; and a fence whose word is not "spoiler" stays code. */
    const char *nl = "```spoiler\nhidden\n```";
    CHECK(oc_rt_spoilers(nl, strlen(nl), sp, 4) == 1 && sp[0].block && sp[0].label_len == 0);
    CHECK(content_spans("```spoilers\nx\n```", OC_RT_SPOILER_BLOCK) == 0);
    CHECK(content_spans("```spoilers\nx\n```", OC_RT_CODEBLOCK) == 1);
    /* A body is `len` bytes, not a C string: nothing past it is read. Here
     * the bytes after the body would make it a spoiler if they were. */
    const char buf[] = "plain text```spoiler\nx\n```";
    CHECK(oc_rt_spoilers(buf, 10, sp, 4) == 0);
}

/* `>!` at the start of a line, where a `>` is a quote everywhere else. */
static void test_spoiler_line_start(void) {
    oc_rt_spoiler sp[4];
    /* Closed, it is a spoiler and there is no quote. */
    CHECK(span_over(">!hidden!<", "hidden", OC_RT_SPOILER));
    CHECK(content_spans(">!hidden!<", OC_RT_QUOTE) == 0);
    CHECK(span_over(">!a!< and more", "a", OC_RT_SPOILER));
    CHECK(content_spans(">!a!< and more", OC_RT_QUOTE) == 0);
    CHECK(content_spans("  >!a!<", OC_RT_QUOTE) == 0 && content_spans("  >!a!<", OC_RT_SPOILER) == 1);
    CHECK(content_spans("first\n>!a!<\nlast", OC_RT_SPOILER) == 1 && content_spans("first\n>!a!<\nlast", OC_RT_QUOTE) == 0);
    /* Unclosed or empty, it is the quote it always was, and hides nothing. */
    CHECK(span_over(">!text", ">!text", OC_RT_QUOTE) == 0);
    CHECK(span_over(">!text", "!text", OC_RT_QUOTE));
    CHECK(content_spans(">!text", OC_RT_SPOILER) == 0);
    CHECK(span_over(">!!<", "!!<", OC_RT_QUOTE));
    CHECK(content_spans(">!!<", OC_RT_SPOILER) == 0);
    CHECK(content_spans(">!a\nb!<", OC_RT_QUOTE) == 1 && content_spans(">!a\nb!<", OC_RT_SPOILER) == 0);
    /* A quote that holds one, and a list that holds one. */
    CHECK(span_over("> >!text!<", ">!text!<", OC_RT_QUOTE));
    CHECK(span_over("> >!text!<", "text", OC_RT_SPOILER));
    CHECK(span_over(">>!text!<", "text", OC_RT_SPOILER));
    CHECK(content_spans(">>!text!<", OC_RT_QUOTE) == 1);
    CHECK(span_over("> say >!text!< now", "text", OC_RT_SPOILER));
    CHECK(span_over("- >!text!<", "text", OC_RT_SPOILER));
    /* A plain quote is untouched, and a `!` only opens straight after the `>`. */
    CHECK(span_over("> quoted", "quoted", OC_RT_QUOTE));
    CHECK(span_over(">quoted!", "quoted!", OC_RT_QUOTE));
    CHECK(content_spans("> !x!<", OC_RT_QUOTE) == 1 && content_spans("> !x!<", OC_RT_SPOILER) == 0);
    /* Escaped, either half: no spoiler; `\>` is no quote either. */
    CHECK(content_spans("\\>!x!<", OC_RT_SPOILER) == 0 && content_spans("\\>!x!<", OC_RT_QUOTE) == 0);
    CHECK(content_spans(">\\!x!<", OC_RT_SPOILER) == 0 && content_spans(">\\!x!<", OC_RT_QUOTE) == 1);
    CHECK(oc_rt_spoilers("\\>!x!<", 6, sp, 4) == 0);
}

/* What was left open by the syntax, each the reading that shows least. */
static void test_spoiler_edges(void) {
    oc_rt_spoiler sp[4];
    /* Any place in a word, and with spaces inside. */
    CHECK(span_over("a>!b!<c", "b", OC_RT_SPOILER));
    CHECK(span_over("say >! both !< now", " both ", OC_RT_SPOILER));
    /* Nested: the whole of it, and no second spoiler inside. */
    const char *n = ">!a >!b!< c!< d";
    CHECK(oc_rt_spoilers(n, strlen(n), sp, 4) == 1);
    CHECK(sp[0].hidden == 2 && sp[0].hidden_len == 9);
    CHECK(content_spans(n, OC_RT_SPOILER) == 1);
    /* More openers than closers: out to the last closer. */
    const char *u = "x >!a >!b!< d";
    CHECK(oc_rt_spoilers(u, strlen(u), sp, 4) == 1 && sp[0].hidden_len == strlen("a >!b"));
    /* Two side by side stay two. */
    CHECK(oc_rt_spoilers(">!a!< x >!b!<", 13, sp, 4) == 2);
    /* An escaped closer does not close. */
    CHECK(span_over(">!a\\!<b!<", "a\\!<b", OC_RT_SPOILER));
    /* Emphasis cannot cut one in two, whichever starts first. */
    const char *e = "*a >!b* c!< d*";
    CHECK(span_over(e, "b* c", OC_RT_SPOILER));
    CHECK(span_over(e, "a >!b* c!< d", OC_RT_BOLD));
    CHECK(span_over(">!*bold*!<", "bold", OC_RT_BOLD));
    CHECK(span_over(">!`code`!<", "code", OC_RT_CODE));
    CHECK(span_over("*>!x!<*", "x", OC_RT_SPOILER));
    /* Nor can a link's label; a whole link inside is a link. */
    const char *l = "[a >!b](https://example.com/x) c!<";
    CHECK(span_over(l, "b](https://example.com/x) c", OC_RT_SPOILER));
    CHECK(content_spans(l, OC_RT_LABELLED) == 0);
    CHECK(span_over(">![a](https://example.com/x)!<", "a", OC_RT_LABELLED));
    CHECK(span_over(">![a](https://example.com/x)!<", "[a](https://example.com/x)", OC_RT_SPOILER));
    /* An address ends at the closer, which is neither part of it nor missed. */
    CHECK(span_over(">!https://example.com/x!<", "https://example.com/x", OC_RT_LINK));
    CHECK(span_over(">!https://example.com/x!<", "https://example.com/x", OC_RT_SPOILER));
    CHECK(span_over("see >!at https://example.com/x!!< ok", "https://example.com/x", OC_RT_LINK));
    /* A time token's `>` before the closer does not read as an opener. */
    const char *t = ">!at <time:2026-10-30T15:00Z>!<";
    CHECK(oc_rt_spoilers(t, strlen(t), sp, 4) == 1 && sp[0].hidden_len == strlen("at <time:2026-10-30T15:00Z>"));
    CHECK(content_spans(t, OC_RT_TIME) == 1);
    /* Code wins when it starts first, and hides a closer inside it. */
    CHECK(content_spans("`a >!b` c!<", OC_RT_SPOILER) == 0);
    CHECK(span_over(">!a `b!<` c!<", "a `b!<` c", OC_RT_SPOILER));
    /* A fence inside one on its line is its text, not a block that shows. */
    const char *f = ">! ```a``` !<";
    CHECK(oc_rt_spoilers(f, strlen(f), sp, 4) == 1 && content_spans(f, OC_RT_CODEBLOCK) == 0);
    /* Bounded like every delimiter: a line of openers, none closed, is text. */
    {
        static char many[8001];
        for (size_t i = 0; i < 8000; i += 2) memcpy(many + i, ">!", 2);
        many[8000] = 0;
        CHECK(oc_rt_spoilers(many, 8000, NULL, 0) == 0);
    }
}

/* A spoiler in a table cell: the row splits round it, and it is listed. */
static void test_spoiler_cells(void) {
    oc_rt_span cs[16];
    oc_rt_spoiler sp[8];
    oc_rt_table t[2];
    oc_rt_cell cells[OC_RT_TABLE_MAX_COLS];
    oc_rt_rowit it;
    uint32_t row;
    const char *b = "| Who | What |\n|---|---|\n| >!Ada!< | plain |\n| x | >!y!< and >!z!< |";
    CHECK(oc_rt_tables(b, strlen(b), t, 2) == 1 && t[0].cols == 2 && t[0].rows == 2);
    /* oc_rt_scan still reports the table as one span. */
    CHECK(content_spans(b, OC_RT_TABLE) == 1 && content_spans(b, OC_RT_SPOILER) == 0);
    CHECK(oc_rt_spoilers(b, strlen(b), NULL, 0) == 3);
    CHECK(oc_rt_spoilers(b, strlen(b), sp, 8) == 3);
    CHECK(sp[0].cell && sp[1].cell && sp[2].cell);
    CHECK(memcmp(b + sp[0].hidden, "Ada", 3) == 0 && sp[0].hidden_len == 3);
    /* The cell's own spans say the same. */
    oc_rt_rows_begin(&it, b, strlen(b), &t[0]);
    CHECK(oc_rt_row_next(&it, cells, &row));
    CHECK(oc_rt_row_next(&it, cells, &row) && row == 1);
    size_t ns = oc_rt_scan_cell(b, cells[0].start, cells[0].len, cs, 16);
    CHECK(ns == 3 && cs[1].style == OC_RT_SPOILER && cs[1].start == sp[0].hidden && cs[1].len == 3);

    /* A pipe inside one separates nothing, either way round, escaped or not. */
    const char *p = "| a | b |\n|---|---|\n| >!x|y!< | c |\n| >!u\\|v!< | w |";
    CHECK(oc_rt_tables(p, strlen(p), t, 2) == 1 && t[0].cols == 2);
    CHECK(oc_rt_spoilers(p, strlen(p), sp, 8) == 2);
    CHECK(sp[0].hidden_len == 3 && memcmp(p + sp[0].hidden, "x|y", 3) == 0);
    oc_rt_rows_begin(&it, p, strlen(p), &t[0]);
    oc_rt_row_next(&it, cells, &row);
    oc_rt_row_next(&it, cells, &row);
    CHECK(cells[0].len == strlen(">!x|y!<") && cells[1].len == 1 && p[cells[1].start] == 'c');
    /* In a header too. */
    const char *h = "| >!x|y!< | c |\n|---|---|\n| 1 | 2 |";
    CHECK(oc_rt_tables(h, strlen(h), t, 2) == 1 && t[0].cols == 2);
    CHECK(oc_rt_spoilers(h, strlen(h), sp, 8) == 1 && sp[0].cell);
    /* A `>!` that does not close leaves the pipes alone, and hides nothing. */
    const char *o = "| a | b |\n|---|---|\n| >!x | y |";
    CHECK(oc_rt_tables(o, strlen(o), t, 2) == 1 && oc_rt_spoilers(o, strlen(o), sp, 8) == 0);
    /* `>!` in a cell's code opens nothing, and takes no pipe with it. */
    const char *k = "| a | b |\n|---|---|\n| `>!x` | y!< |";
    CHECK(oc_rt_tables(k, strlen(k), t, 2) == 1 && oc_rt_spoilers(k, strlen(k), sp, 8) == 0);
    oc_rt_rows_begin(&it, k, strlen(k), &t[0]);
    oc_rt_row_next(&it, cells, &row);
    oc_rt_row_next(&it, cells, &row);
    CHECK(cells[1].len == 3 && memcmp(k + cells[1].start, "y!<", 3) == 0);
    /* A row may begin with one, with no leading pipe, and so may the header:
     * it is no block marker. A row's quote still stops a table starting. */
    const char *n = ">!x!< | b\n---|---\n>!y!< | d";
    CHECK(oc_rt_tables(n, strlen(n), t, 2) == 1 && t[0].rows == 1);
    CHECK(oc_rt_spoilers(n, strlen(n), sp, 8) == 2 && sp[0].cell && sp[0].start == 0);
    CHECK(oc_rt_tables("> x | b\n---|---\n", 16, t, 2) == 0);

    /* A cell past the columns says so, for a frontend whose grid never draws it. */
    const char *xc = "| a | b |\n|---|---|\n| >!p!< | y | >!q!< |";
    CHECK(oc_rt_spoilers(xc, strlen(xc), sp, 8) == 2 && sp[0].cell && !sp[0].extra && sp[1].cell && sp[1].extra);

    /* The old form in a row is cells and ordinary text. */
    const char *leak = "| a | b |\n|---|---|\n| ||TABLELEAK|| | c |";
    CHECK(oc_rt_tables(leak, strlen(leak), t, 2) == 1);
    CHECK(oc_rt_spoilers(leak, strlen(leak), sp, 8) == 0);
    oc_rt_rows_begin(&it, leak, strlen(leak), &t[0]);
    oc_rt_row_next(&it, cells, &row);
    oc_rt_row_next(&it, cells, &row);
    CHECK(cells[0].len == 0 && cells[1].len == 0);

    /* In a table inside a block, it goes with the block. */
    const char *in = "```spoiler\n| a |\n|---|\n| >!x!< |\n```";
    CHECK(oc_rt_spoilers(in, strlen(in), sp, 8) == 1 && sp[0].block && !sp[0].cell);
    /* Beside the block, the text and a table, each is counted in order. */
    const char *mix = ">!a!<\n\n| h |\n|---|\n| >!b!< |\n\n```spoiler\nc\n```\n\nand >!d!<";
    CHECK(oc_rt_spoilers(mix, strlen(mix), sp, 8) == 4);
    CHECK(!sp[0].cell && sp[1].cell && sp[2].block && !sp[2].cell && !sp[3].cell);
}

/* A cell's spoiler in every excerpt, including the tables past the 32 an
 * excerpt summarises and a cut past the 32 spoilers it lists. */
static void test_spoiler_cell_excerpt(void) {
    char out[4096];
    const char *b = "| a | b |\n|---|---|\n| >!SECRET!< | c |";
    oc_rt_plain_excerpt(b, strlen(b), out, sizeof out);
    CHECK(strstr(out, "Table: 1 row") == out && !strstr(out, "SECRET"));
    oc_rt_flatten(b, strlen(b), out, sizeof out);
    CHECK(!strstr(out, "SECRET"));
    oc_rt_clock k = { 0, 1, 1, 0 };
    oc_rt_flatten_at(b, strlen(b), &k, out, sizeof out);
    CHECK(!strstr(out, "SECRET"));
    /* A first cell with no leading pipe starts where its table does. */
    const char *f = ">!SECRET!< | b\n---|---\n1 | 2";
    oc_rt_plain_excerpt(f, strlen(f), out, sizeof out);
    CHECK(strstr(out, "Table: 1 row") == out && !strstr(out, "SECRET"));
    /* Past the 32 tables an excerpt sums up, nothing is shown: not a spoiler
     * in a declared cell, an extra one, or the text after them. */
    {
        static char many[4096];
        size_t o = 0;
        for (int i = 0; i < 32; i++) o += (size_t)snprintf(many + o, sizeof many - o, "| a |\n|---|\n| %d |\n\n", i);
        o += (size_t)snprintf(many + o, sizeof many - o, "| a |\n|---|\n| b | >!SECRET!< | raw |\n\nAFTER");
        oc_rt_plain_excerpt(many, o, out, sizeof out);
        CHECK(!strstr(out, "SECRET") && !strstr(out, "raw") && !strstr(out, "AFTER"));
    }
    /* The limit comes after the list: a block holding the table the limit falls
     * on, or past it, is still hidden whole. */
    {
        static char blk[4096];
        size_t o = (size_t)snprintf(blk, sizeof blk, "```spoiler\nSECRET\n");
        for (int i = 0; i < 33; i++) o += (size_t)snprintf(blk + o, sizeof blk - o, "| a |\n|---|\n| CELL%d |\n\n", i);
        o += (size_t)snprintf(blk + o, sizeof blk - o, "```");
        oc_rt_plain_excerpt(blk, o, out, sizeof out);
        /* contract: the excerpt wording MARKDOWN.md §1d specifies */
        CHECK(!strstr(out, "SECRET") && !strstr(out, "CELL") && strncmp(out, "[spoiler]", 9) == 0);
    }
    /* A cell past the table's columns is a cell: it is hidden in every excerpt. */
    {
        const char *x = "| a | b |\n|---|---|\n| x | y | >!SECRET!< |";
        oc_rt_spoiler sv[2];
        CHECK(oc_rt_spoilers(x, strlen(x), sv, 2) == 1 && sv[0].cell);
        oc_rt_plain_excerpt(x, strlen(x), out, sizeof out);
        CHECK(!strstr(out, "SECRET"));
        char *cov = oc_rt_spoilers_covered(x, strlen(x));
        /* contract: the wording a frontend that shows the source shows */
        CHECK(cov && !strstr(cov, "SECRET") && strstr(cov, "| x | y | [spoiler] |"));
        free(cov);
        /* And with a pipe in it, and in a later table. */
        const char *y = "| a |\n|---|\n| b | c | >!S1|S2!< | d |\n\n| a |\n|---|\n| e | >!S3!< |";
        CHECK(oc_rt_spoilers(y, strlen(y), NULL, 0) == 2);
        cov = oc_rt_spoilers_covered(y, strlen(y));
        CHECK(cov && !strstr(cov, "S1") && !strstr(cov, "S2") && !strstr(cov, "S3"));
        free(cov);
        /* The source form: none, none to cover; past 32, nothing after the 32nd. */
        CHECK(oc_rt_spoilers_covered("plain | text", 12) == NULL);
        static char lots[1024];
        size_t o = 0;
        for (int i = 0; i < 33; i++) o += (size_t)snprintf(lots + o, sizeof lots - o, ">!s%d!< ", i);
        o += (size_t)snprintf(lots + o, sizeof lots - o, "AFTER");
        cov = oc_rt_spoilers_covered(lots, o);
        CHECK(cov && !strstr(cov, "s32") && !strstr(cov, "AFTER"));
        free(cov);
    }
    /* An address in a spoiler is not one to fetch, wherever the spoiler is. */
    {
        oc_url_span u[4];
        const char *v = "https://a.example/1 >!https://b.example/2!< >!see https://c.example/3!<\n"
                        "| h |\n|---|\n| x | >!https://d.example/4!< |\n\n"
                        "```spoiler\nhttps://e.example/5\n```\nhttps://f.example/6";
        size_t n = oc_rt_visible_urls(v, strlen(v), u, 4);
        CHECK(n == 2);
        if (n == 2) {
            CHECK(strncmp(v + u[0].start, "https://a.example/1", u[0].len) == 0);
            CHECK(strncmp(v + u[1].start, "https://f.example/6", u[1].len) == 0);
        }
        /* The hidden do not use up `max`. */
        CHECK(oc_rt_visible_urls(v, strlen(v), u, 1) == 1);
    }
    /* More than 32 in one table: the cut falls inside it, and nothing runs on
     * past the end of what was written. */
    {
        static char big[4096];
        size_t o = (size_t)snprintf(big, sizeof big, "| a |\n|---|\n");
        for (int i = 0; i < 40; i++) o += (size_t)snprintf(big + o, sizeof big - o, "| >!S%02d!< |\n", i);
        size_t w = oc_rt_plain_excerpt(big, o, out, sizeof out);
        CHECK(!strstr(out, "S0") && !strstr(out, "S3"));
        /* contract: the excerpt wording MARKDOWN.md §1d specifies */
        CHECK(strstr(out, "Table: 40 rows") == out && w == strlen(out) && w < 64);
    }
    /* The old form in a row was drawn as text, and still is: it is no spoiler,
     * and its excerpt is the table's line. */
    const char *leak = "| a | b |\n|---|---|\n| ||TABLELEAK|| | c |";
    oc_rt_plain_excerpt(leak, strlen(leak), out, sizeof out);
    CHECK(strstr(out, "Table: 1 row") == out && !strstr(out, "TABLELEAK"));
}

/* Every excerpt hides what a spoiler hides. */
static void test_spoiler_excerpt(void) {
    char out[160];
    const char *b = "The killer is >!the butler!< obviously";
    oc_rt_plain_excerpt(b, strlen(b), out, sizeof out);
    /* contract: the excerpt wording MARKDOWN.md §1d specifies */
    CHECK(strcmp(out, "The killer is [spoiler] obviously") == 0);
    const char *k = "Before\n```spoiler Ending\nShe lives.\n```\nAfter";
    oc_rt_plain_excerpt(k, strlen(k), out, sizeof out);
    /* contract: the excerpt wording MARKDOWN.md §1d specifies */
    CHECK(strcmp(out, "Before\n[spoiler: Ending]\nAfter") == 0);
    /* A table inside the block goes with it; one outside stays a table line. */
    const char *t = "| a |\n|---|\n| 1 |\n\n```spoiler\n| b |\n|---|\n| 2 |\n```";
    oc_rt_plain_excerpt(t, strlen(t), out, sizeof out);
    CHECK(strstr(out, "Table: 1 row") == out && strstr(out, "[spoiler]") && !strstr(out, "| 2 |"));
    /* oc_rt_flatten, the older name, hides them too. */
    oc_rt_flatten(b, strlen(b), out, sizeof out);
    CHECK(!strstr(out, "butler"));
    /* Cut short, the hidden text is never what is left. */
    char small[20];
    oc_rt_plain_excerpt(b, strlen(b), small, sizeof small);
    CHECK(!strstr(small, "butler"));
    /* A time inside a spoiler is hidden with it, and one outside still reads
     * as its time. */
    {
        char ex[128];
        const char *tb = "at <time:2026-10-30T15:00Z> >!then <time:2026-10-30T16:00Z>!<";
        oc_rt_clock k = { 0, 1, 1, 0 };
        oc_rt_flatten_at(tb, strlen(tb), &k, ex, sizeof ex);
        CHECK(strstr(ex, "[spoiler]") != NULL && strstr(ex, "16:00") == NULL && strstr(ex, "<time:") == NULL);
    }
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
    test_labelled_links();
    test_truncation();
    test_tables();
    test_table_cells();
    test_table_flatten();
    test_time_tokens();
    test_time_text();
    test_time_flatten();
    test_spoilers();
    test_spoiler_line_start();
    test_spoiler_edges();
    test_spoiler_cells();
    test_spoiler_excerpt();
    test_spoiler_cell_excerpt();
    printf("test_richtext: emphasis, literal delimiters, escapes, code, fenced blocks, labelled links, "
           "quotes and lists, nesting, byte offsets, autolinking, truncation, tables and their cells and excerpts, "
           "time tokens in every zone, spoilers\n");
    return failures;
}
