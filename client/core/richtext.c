/* Message formatting (REQ-220, ARCH-100) — see richtext.h and docs/MARKDOWN.md.
 *
 * Three passes, outermost first, because each one suppresses the next:
 *   oc_rt_scan   splits the body around fenced code blocks (the only construct
 *                that may span lines, and it makes everything inside literal),
 *   scan_line    takes what is left one line at a time and lifts a leading
 *                block marker (`>`, `- `, `1. `) off the front,
 *   scan_inline  walks the remainder emitting emphasis, code spans and escapes,
 *                recursing into the content it matched so emphasis nests.
 *
 * No allocation and no rewriting: every span points into the caller's bytes. */

#include "richtext.h"

#define RT_NONE ((size_t)-1)

typedef struct {
    const char *b;
    size_t      len;
    oc_rt_span *out;
    size_t      max;
    size_t      n;      /* found so far — may pass `max`, see oc_rt_scan */
} rt_ctx;

static void emit(rt_ctx *c, size_t start, size_t len, uint16_t style) {
    if (len == 0) return;             /* an empty span is not a rendering */
    if (c->n < c->max) {
        c->out[c->n].start = start;
        c->out[c->n].len   = len;
        c->out[c->n].style = style;
    }
    c->n++;
}

static int rt_space(char ch) { return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r'; }
static int rt_delim(char ch) { return ch == '*' || ch == '_' || ch == '~' || ch == '`'; }

/* The set a backslash may escape. Slack has none at all; without one there is
 * no way to write a literal asterisk at a word boundary (MARKDOWN.md §2.3). */
static int rt_escapable(char ch) {
    return rt_delim(ch) || ch == '>' || ch == '-' || ch == '\\';
}

/* May an inline delimiter OPEN at `i`? Preceded by a line start, whitespace or
 * an opening bracket — looking THROUGH any run of delimiters immediately before
 * it, which is what lets `*_x_*` nest with no space between the two openers.
 *
 * Looking through the run rather than simply accepting a delimiter as a
 * boundary is the difference between `*_x_*` nesting and `snake__case__here`
 * italicising "case_": there the run traces back to `e`, and an identifier is
 * not a word boundary no matter how many underscores are in it. */
static int open_ok(rt_ctx *c, size_t i) {
    char p;
    while (i > 0 && rt_delim(c->b[i - 1])) i--;
    if (i == 0) return 1;
    p = c->b[i - 1];
    return rt_space(p) || p == '(' || p == '[' || p == '{' || p == '<';
}

/* If a complete `code` span opens at `i` and closes before `e`, its total
 * length including both backticks; otherwise 0. A RUN of backticks is never an
 * inline code opener — the fenced form is handled a pass above, and a leftover
 * run is literal text. */
static size_t code_span_len(rt_ctx *c, size_t i, size_t e) {
    size_t j;
    if (c->b[i] != '`') return 0;
    if (i + 1 < e && c->b[i + 1] == '`') return 0;
    if (!open_ok(c, i)) return 0;
    if (i + 1 >= e || rt_space(c->b[i + 1])) return 0;
    for (j = i + 2; j < e; j++)
        if (c->b[j] == '`' && !rt_space(c->b[j - 1])) return j - i + 1;
    return 0;
}

/* The first valid closer for a `d`-byte run of `ch` in [from, e), or RT_NONE.
 * Escapes and code spans are stepped over rather than looked inside, so the
 * `*` in "*a `b*c` d*" closes at the end and not in the middle. */
static size_t find_close(rt_ctx *c, size_t from, size_t e, char ch, size_t d) {
    size_t i = from;
    while (i < e) {
        size_t cs;
        if (c->b[i] == '\\' && i + 1 < e && rt_escapable(c->b[i + 1])) { i += 2; continue; }
        cs = code_span_len(c, i, e);
        if (cs) { i += cs; continue; }
        if (c->b[i] == ch && i > from && i + d <= e && !rt_space(c->b[i - 1])) {
            size_t k;
            for (k = 1; k < d; k++) if (c->b[i + k] != ch) break;
            if (k == d) return i;
        }
        i++;
    }
    return RT_NONE;
}

static void scan_inline(rt_ctx *c, size_t s, size_t e, int depth) {
    size_t i = s;
    /* Once a closer search for one delimiter form has failed, every LATER opener
     * of that same form fails too: its range is a subset of the one just
     * rejected, and the two walks skip escapes and code spans identically. So
     * ask once. Without this, a line of "*a *a *a …" — every asterisk an opener,
     * none of them a closer — makes each opener rescan the rest of the line, and
     * a full 4000-character composer buffer measured 2.9 ms per keystroke
     * against 5 us for ordinary prose. With it, that case is 5 us as well.
     * Indexed [delimiter][run length]. */
    int hopeless[3][3] = { { 0 } };
    if (depth > 6) return;            /* pathological nesting; the text still renders */
    while (i < e) {
        char ch = c->b[i];
        uint16_t style = 0;
        size_t d;
        int slot = 0;

        if (ch == '\\' && i + 1 < e && rt_escapable(c->b[i + 1])) {
            emit(c, i, 1, OC_RT_DELIM);      /* the backslash is markup, not text */
            i += 2;
            continue;
        }
        if (ch == '`') {
            size_t L = code_span_len(c, i, e);
            if (L) {
                emit(c, i, 1, OC_RT_DELIM | OC_RT_CODE);
                emit(c, i + 1, L - 2, OC_RT_CODE);
                emit(c, i + L - 1, 1, OC_RT_DELIM | OC_RT_CODE);
                i += L;
            } else {
                while (i < e && c->b[i] == '`') i++;   /* a bare run is literal */
            }
            continue;
        }
        if      (ch == '*') { style = OC_RT_BOLD;   slot = 0; }
        else if (ch == '_') { style = OC_RT_ITALIC; slot = 1; }
        else if (ch == '~') { style = OC_RT_STRIKE; slot = 2; }
        if (!style) { i++; continue; }

        /* The delimiter is as long as its run, and only `**` means anything —
         * everyone arriving from Markdown types two asterisks and rendering
         * those literally would look broken to more people than it would please
         * (MARKDOWN.md §4). Any other run is literal text: `~~x~~` striking
         * "~x" would be worse than leaving what the author typed alone. */
        d = 1;
        while (i + d < e && c->b[i + d] == ch) d++;
        if (d > 2 || (d == 2 && ch != '*')) { i += d; continue; }

        if (open_ok(c, i) && i + d < e && !rt_space(c->b[i + d]) && !hopeless[slot][d]) {
            size_t j = find_close(c, i + d, e, ch, d);
            if (j == RT_NONE) hopeless[slot][d] = 1;
            else {
                emit(c, i, d, (uint16_t)(style | OC_RT_DELIM));
                emit(c, i + d, j - (i + d), style);
                scan_inline(c, i + d, j, depth + 1);
                emit(c, j, d, (uint16_t)(style | OC_RT_DELIM));
                i = j + d;
                continue;
            }
        }
        i += d;                                /* unclosed: literal, MARKDOWN.md §2.2 */
    }
}

/* One line, `\n` already excluded. Lifts at most one block marker off the front
 * and recurses, so `> - item` is a bulleted line inside a quote. */
static void scan_line(rt_ctx *c, size_t s, size_t e, int depth) {
    size_t p = s, q;
    if (depth > 3) { scan_inline(c, s, e, 0); return; }
    while (p < e && (c->b[p] == ' ' || c->b[p] == '\t')) p++;   /* indentation */

    if (p < e && c->b[p] == '>') {
        size_t d = (p + 1 < e && c->b[p + 1] == ' ') ? 2 : 1;
        emit(c, p, d, OC_RT_DELIM | OC_RT_QUOTE);
        if (p + d < e) {
            emit(c, p + d, e - (p + d), OC_RT_QUOTE);
            scan_line(c, p + d, e, depth + 1);
        }
        return;
    }
    /* A bullet REQUIRES the space: "-1 degrees" is a temperature, not a list. */
    if (p + 1 < e && c->b[p] == '-' && c->b[p + 1] == ' ') {
        emit(c, p, 2, OC_RT_DELIM | OC_RT_BULLET);
        if (p + 2 < e) {
            emit(c, p + 2, e - (p + 2), OC_RT_BULLET);
            scan_line(c, p + 2, e, depth + 1);
        }
        return;
    }
    q = p;
    while (q < e && c->b[q] >= '0' && c->b[q] <= '9') q++;
    if (q > p && q + 1 < e && c->b[q] == '.' && c->b[q + 1] == ' ') {
        emit(c, p, q + 2 - p, OC_RT_DELIM | OC_RT_ORDERED);
        if (q + 2 < e) {
            emit(c, q + 2, e - (q + 2), OC_RT_ORDERED);
            scan_line(c, q + 2, e, depth + 1);
        }
        return;
    }
    scan_inline(c, s, e, 0);
}

/* Everything between two fenced blocks, split into lines. */
static void scan_text(rt_ctx *c, size_t s, size_t e) {
    size_t i = s;
    while (i < e) {
        size_t nl = i, end;
        while (nl < e && c->b[nl] != '\n') nl++;
        end = nl;
        if (end > i && c->b[end - 1] == '\r') end--;
        if (end > i) scan_line(c, i, end, 0);
        i = nl + 1;
    }
}

size_t oc_rt_scan(const char *body, size_t len, oc_rt_span *out, size_t max) {
    rt_ctx c;
    size_t i = 0, seg = 0;

    c.b = body; c.len = len; c.out = out; c.max = out ? max : 0; c.n = 0;
    if (!body || len == 0) return 0;

    while (i < len) {
        size_t close = RT_NONE, j, cs, ce;
        if (!(body[i] == '`' && i + 2 < len && body[i + 1] == '`' && body[i + 2] == '`'
              && open_ok(&c, i))) { i++; continue; }
        for (j = i + 3; j + 3 <= len; j++)
            if (body[j] == '`' && body[j + 1] == '`' && body[j + 2] == '`') { close = j; break; }
        if (close == RT_NONE) { i++; continue; }   /* unclosed fence: literal text */

        scan_text(&c, seg, i);
        /* The newline after the opening fence and the one before the closing
         * fence belong to the fence, not to the code — otherwise every block
         * renders with a blank first line. */
        cs = i + 3; ce = close;
        if (cs + 1 < ce && body[cs] == '\r' && body[cs + 1] == '\n') cs += 2;
        else if (cs < ce && body[cs] == '\n') cs++;
        if (ce > cs && body[ce - 1] == '\n') {
            ce--;
            if (ce > cs && body[ce - 1] == '\r') ce--;
        }
        emit(&c, i, cs - i, OC_RT_DELIM | OC_RT_CODEBLOCK);
        emit(&c, cs, ce - cs, OC_RT_CODEBLOCK);
        emit(&c, ce, close + 3 - ce, OC_RT_DELIM | OC_RT_CODEBLOCK);
        i = seg = close + 3;
    }
    scan_text(&c, seg, len);
    return c.n;
}
