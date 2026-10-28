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

#include "url.h"   /* shared/: the address rules the daemon must agree with */

#include <stdio.h>
#include <string.h>

#define RT_NONE ((size_t)-1)

typedef struct {
    const char *b;
    size_t      len;
    oc_rt_span *out;
    size_t      max;
    size_t      n;      /* found so far — may pass `max`, see oc_rt_scan */
    /* oc_rt_tables runs the same walk collecting tables and nothing else, so
     * what is a table cannot differ between the two. */
    oc_rt_table *tabs;
    size_t      tmax, tn;
    int         tables_only;
    int         in_cell;  /* scanning one table cell: `<br>` is a break */
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
    return rt_delim(ch) || ch == '>' || ch == '-' || ch == '\\' || ch == '|';
}

/* `<br>`, `<br/>` or `<br />` at `i`, in any case: its length, else 0. The one
 * piece of HTML the dialect takes, and only in a table cell (MARKDOWN.md §1a),
 * because a cell has no other way to hold a second line. */
static size_t br_len(rt_ctx *c, size_t i, size_t e) {
    const char *b = c->b;
    size_t j;
    if (i + 4 > e || b[i] != '<' || (b[i + 1] | 0x20) != 'b' || (b[i + 2] | 0x20) != 'r') return 0;
    j = i + 3;
    if (j < e && b[j] == ' ') j++;
    if (j < e && b[j] == '/') j++;
    if (j < e && b[j] == '>') return j + 1 - i;
    return 0;
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

/* ---- autolinking (MARKDOWN.md §4) ----------------------------------------
 *
 * A bare URL is the only construct with no delimiters: the address is its own
 * label, which is why there is no authoring syntax to go with this and no
 * `<url|label>` form (that one is Slack's API artifact and is declined).
 *
 * The rules themselves — the http/https scheme list, where an address ends,
 * the trailing trim, the bracket balance, the opening boundary — live in
 * shared/url.c (ARCH-105): the daemon's unfurl fetcher must agree with the
 * spans this parser links, and one implementation is what makes disagreement
 * impossible rather than merely unlikely. url_len below is the thin adapter
 * onto it. */
static size_t url_len(rt_ctx *c, size_t i, size_t e) {
    return oc_url_len(c->b, e, i);
}

/* A labelled link `[label](https://address)` opening at `i` and ending before
 * `e`: its total length, with the label's end (the `]`) in *close; 0 if not one.
 * One line (scan_inline's range already is), a non-empty label with no brackets
 * of its own, and an address the autolinker would link in full, closed by `)`. */
static size_t labelled_len(rt_ctx *c, size_t i, size_t e, size_t *close) {
    size_t j, L;
    if (c->b[i] != '[') return 0;
    for (j = i + 1; j < e && c->b[j] != ']'; j++)
        if (c->b[j] == '[' || c->b[j] == '\n') return 0;
    if (j >= e || j == i + 1 || j + 2 >= e || c->b[j + 1] != '(') return 0;
    L = url_len(c, j + 2, e);
    if (!L || j + 2 + L >= e || c->b[j + 2 + L] != ')') return 0;
    *close = j;
    return j + 2 + L + 1 - i;
}

/* The first valid closer for a `d`-byte run of `ch` in [from, e), or RT_NONE.
 * Escapes, code spans and URLs are stepped over rather than looked inside, so
 * the `*` in "*a `b*c` d*" closes at the end and not in the middle — and an
 * underscore inside a URL cannot close an emphasis run that opened before it. */
static size_t find_close(rt_ctx *c, size_t from, size_t e, char ch, size_t d) {
    size_t i = from;
    while (i < e) {
        size_t cs;
        if (c->b[i] == '\\' && i + 1 < e && rt_escapable(c->b[i + 1])) { i += 2; continue; }
        cs = code_span_len(c, i, e);
        if (cs) { i += cs; continue; }
        cs = url_len(c, i, e);
        if (cs) { i += cs; continue; }
        {
            size_t k;
            cs = labelled_len(c, i, e, &k);
            if (cs) { i += cs; continue; }
        }
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
        if (c->in_cell && ch == '<') {
            size_t L = br_len(c, i, e);
            if (L) { emit(c, i, L, OC_RT_BREAK); i += L; continue; }
        }
        /* Before the delimiter forms, so a URL is one span rather than a
         * scattering of whatever punctuation it happens to contain. It carries
         * no OC_RT_DELIM: there is no markup around it to hide.
         *
         * Asked at every byte rather than behind a cheap `ch == 'h'` gate. That
         * gate was here and was deleted: it made the SCHEME LIST a lie, since a
         * scheme added to the shared matcher (shared/url.c) would never be
         * reached, and a test asserting `file://` does not autolink passed for
         * that reason instead of the intended one. The matcher rejects on its
         * first byte anyway, so the gate bought a comparison and cost the
         * invariant. */
        {
            size_t L = url_len(c, i, e);
            if (L) { emit(c, i, L, OC_RT_LINK); i += L; continue; }
        }
        if (ch == '[') {
            size_t k, L = labelled_len(c, i, e, &k);
            if (L) {
                emit(c, i, 1, OC_RT_DELIM | OC_RT_LABELLED);
                emit(c, i + 1, k - i - 1, OC_RT_LABELLED);
                scan_inline(c, i + 1, k, depth + 1);
                emit(c, k, i + L - k, OC_RT_DELIM | OC_RT_LABELLED);
                i += L;
                continue;
            }
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

/* ---- tables (MARKDOWN.md §1a) ------------------------------------------- */

/* The end of the line starting at `i` (its `\n`, or `e`), and in *trim the end
 * of its content, a `\r` before the newline excluded. */
static size_t line_at(rt_ctx *c, size_t i, size_t e, size_t *trim) {
    size_t nl = i;
    while (nl < e && c->b[nl] != '\n') nl++;
    *trim = (nl > i && c->b[nl - 1] == '\r') ? nl - 1 : nl;
    return nl;
}

/* Split the line [s, e) into cells on its unescaped pipes, after dropping a
 * leading and a trailing one, each cell trimmed of spaces. Writes up to `max`
 * to `cells` and returns how many there are; *pipes says whether the line had
 * an unescaped pipe at all. */
static size_t split_row(const char *b, size_t s, size_t e, oc_rt_cell *cells, size_t max, int *pipes) {
    size_t n = 0, k, cs;
    *pipes = 0;
    while (s < e && (b[s] == ' ' || b[s] == '\t')) s++;
    while (e > s && (b[e - 1] == ' ' || b[e - 1] == '\t')) e--;
    if (s < e && b[s] == '|') { s++; *pipes = 1; }
    /* A trailing pipe counts only if it is not escaped. */
    if (e > s && b[e - 1] == '|') {
        size_t bs = 0;
        for (k = e - 1; k > s && b[k - 1] == '\\'; k--) bs++;
        if (bs % 2 == 0) { e--; *pipes = 1; }
    }
    cs = s;
    for (k = s; k <= e; k++) {
        if (k < e && b[k] == '\\' && k + 1 < e) { k++; continue; }
        if (k == e || b[k] == '|') {
            size_t a = cs, z = k;
            if (k < e) *pipes = 1;
            while (a < z && (b[a] == ' ' || b[a] == '\t')) a++;
            while (z > a && (b[z - 1] == ' ' || b[z - 1] == '\t')) z--;
            if (n < max) { cells[n].start = a; cells[n].len = z - a; }
            n++;
            cs = k + 1;
        }
    }
    return n;
}

/* A separator cell: dashes, with an optional colon at either end. */
static int sep_cell(const char *b, const oc_rt_cell *cell, uint8_t *align) {
    size_t s = cell->start, e = cell->start + cell->len, d = 0;
    int l = 0, r = 0;
    if (s < e && b[s] == ':') { l = 1; s++; }
    if (e > s && b[e - 1] == ':') { r = 1; e--; }
    for (; s < e; s++, d++) if (b[s] != '-') return 0;
    if (!d) return 0;
    *align = l && r ? OC_RT_ALIGN_CENTER : r ? OC_RT_ALIGN_RIGHT : l ? OC_RT_ALIGN_LEFT : OC_RT_ALIGN_NONE;
    return 1;
}

/* Does a block marker begin the line [s, e)? Such a line belongs to a quote or a
 * list and does not start a table. */
static int block_marker(const char *b, size_t s, size_t e) {
    size_t q;
    while (s < e && (b[s] == ' ' || b[s] == '\t')) s++;
    if (s < e && b[s] == '>') return 1;
    if (s + 1 < e && b[s] == '-' && b[s + 1] == ' ') return 1;
    for (q = s; q < e && b[q] >= '0' && b[q] <= '9'; q++) {}
    return q > s && q + 1 < e && b[q] == '.' && b[q + 1] == ' ';
}

/* If a table starts at the line beginning at `i`, fill *t and return the offset
 * just past its last line (its newline included); else 0. */
static size_t table_at(rt_ctx *c, size_t i, size_t e, oc_rt_table *t) {
    const char *b = c->b;
    oc_rt_cell head[OC_RT_TABLE_MAX_COLS + 1], sep[OC_RT_TABLE_MAX_COLS + 1];
    size_t ht, hn, st, sn, nh, ns, k, last, p;
    int hp, sp;
    hn = line_at(c, i, e, &ht);
    if (hn >= e || ht == i || block_marker(b, i, ht)) return 0;
    nh = split_row(b, i, ht, head, OC_RT_TABLE_MAX_COLS + 1, &hp);
    if (!hp || nh == 0 || nh > OC_RT_TABLE_MAX_COLS) return 0;
    sn = line_at(c, hn + 1, e, &st);
    ns = split_row(b, hn + 1, st, sep, OC_RT_TABLE_MAX_COLS + 1, &sp);
    if (!sp || ns != nh) return 0;
    memset(t, 0, sizeof *t);
    for (k = 0; k < ns; k++)
        if (!sep_cell(b, &sep[k], &t->align[k])) return 0;
    t->cols = (uint8_t)nh;
    for (k = 0; k < nh; k++) if (head[k].len) t->header = 1;
    t->start = i;
    last = st;
    p = sn < e ? sn + 1 : e;
    while (p < e) {
        size_t rt, rn = line_at(c, p, e, &rt);
        oc_rt_cell tmp[1];
        int rp;
        if (rt == p) break;                       /* a blank line ends it */
        split_row(b, p, rt, tmp, 0, &rp);
        if (!rp) break;                           /* so does a line with no pipe */
        t->rows++;
        last = rt;
        p = rn < e ? rn + 1 : e;
    }
    t->len = last - i;
    return p;
}

/* Everything between two fenced blocks, split into lines; a table where one
 * starts. */
static void scan_text(rt_ctx *c, size_t s, size_t e) {
    size_t i = s;
    while (i < e) {
        size_t nl, end, after;
        oc_rt_table t;
        if ((after = table_at(c, i, e, &t)) != 0) {
            if (c->tables_only) {
                if (c->tn < c->tmax) c->tabs[c->tn] = t;
                c->tn++;
            } else {
                emit(c, t.start, t.len, OC_RT_TABLE);
            }
            i = after;
            continue;
        }
        nl = line_at(c, i, e, &end);
        if (end > i && !c->tables_only) scan_line(c, i, end, 0);
        i = nl + 1;
    }
}

/* The walk both oc_rt_scan and oc_rt_tables make: fenced code blocks first,
 * the text between them line by line. */
static void scan_body(rt_ctx *c) {
    const char *body = c->b;
    size_t len = c->len, i = 0, seg = 0;
    while (i < len) {
        size_t close = RT_NONE, j, cs, ce;
        if (!(body[i] == '`' && i + 2 < len && body[i + 1] == '`' && body[i + 2] == '`'
              && open_ok(c, i))) { i++; continue; }
        for (j = i + 3; j + 3 <= len; j++)
            if (body[j] == '`' && body[j + 1] == '`' && body[j + 2] == '`') { close = j; break; }
        if (close == RT_NONE) { i++; continue; }   /* unclosed fence: literal text */

        scan_text(c, seg, i);
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
        emit(c, i, cs - i, OC_RT_DELIM | OC_RT_CODEBLOCK);
        emit(c, cs, ce - cs, OC_RT_CODEBLOCK);
        emit(c, ce, close + 3 - ce, OC_RT_DELIM | OC_RT_CODEBLOCK);
        i = seg = close + 3;
    }
    scan_text(c, seg, len);
}

size_t oc_rt_scan(const char *body, size_t len, oc_rt_span *out, size_t max) {
    rt_ctx c;
    memset(&c, 0, sizeof c);
    c.b = body; c.len = len; c.out = out; c.max = out ? max : 0;
    if (!body || len == 0) return 0;
    scan_body(&c);
    return c.n;
}

size_t oc_rt_tables(const char *body, size_t len, oc_rt_table *out, size_t max) {
    rt_ctx c;
    memset(&c, 0, sizeof c);
    c.b = body; c.len = len; c.tabs = out; c.tmax = out ? max : 0; c.tables_only = 1;
    if (!body || len == 0) return 0;
    scan_body(&c);
    return c.tn;
}

void oc_rt_rows_begin(oc_rt_rowit *it, const char *body, size_t len, const oc_rt_table *t) {
    memset(it, 0, sizeof *it);
    if (!body || !t || t->start + t->len > len) return;
    it->b = body;
    it->pos = t->start;
    it->end = t->start + t->len;
    it->cols = t->cols;
}

int oc_rt_row_next(oc_rt_rowit *it, oc_rt_cell cells[OC_RT_TABLE_MAX_COLS], uint32_t *row) {
    rt_ctx c;
    size_t trim, nl, n, k;
    int pipes;
    if (!it->b || it->pos >= it->end) return 0;
    memset(&c, 0, sizeof c);
    c.b = it->b;
    if (it->row == 1) {                       /* the separator is not a row */
        nl = line_at(&c, it->pos, it->end, &trim);
        it->pos = nl + 1;
        if (it->pos >= it->end) return 0;
    }
    nl = line_at(&c, it->pos, it->end, &trim);
    n = split_row(it->b, it->pos, trim, cells, it->cols, &pipes);
    for (k = n; k < it->cols; k++) { cells[k].start = trim; cells[k].len = 0; }
    *row = it->row++;
    it->pos = nl + 1;
    return 1;
}

size_t oc_rt_scan_cell(const char *body, size_t off, size_t len, oc_rt_span *out, size_t max) {
    rt_ctx c;
    memset(&c, 0, sizeof c);
    c.b = body; c.len = off + len; c.out = out; c.max = out ? max : 0; c.in_cell = 1;
    if (!body || len == 0) return 0;
    scan_inline(&c, off, off + len, 0);
    return c.n;
}

size_t oc_rt_flatten(const char *body, size_t len, char *out, size_t cap) {
    oc_rt_table t[32];
    size_t nt, k, i = 0, o = 0;
    if (!out || cap == 0) return 0;
    out[0] = '\0';
    if (!body) return 0;
    nt = oc_rt_tables(body, len, t, 32);
    if (nt > 32) nt = 32;                     /* past that, the rest stay as written */
    for (k = 0; k <= nt && o + 1 < cap; k++) {
        size_t upto = k < nt ? t[k].start : len, n = upto - i;
        if (n > cap - 1 - o) n = cap - 1 - o;
        memcpy(out + o, body + i, n);
        o += n;
        if (k < nt && o + 1 < cap) {
            int w = snprintf(out + o, cap - o, "Table: %u row%s \xC3\x97 %u column%s",
                             (unsigned)t[k].rows, t[k].rows == 1 ? "" : "s",
                             (unsigned)t[k].cols, t[k].cols == 1 ? "" : "s");
            if (w > 0) o += (size_t)w < cap - o ? (size_t)w : cap - 1 - o;
            i = t[k].start + t[k].len;
        }
    }
    /* Never end inside a character: if the last one is short of the bytes its
     * lead byte promises, it was cut, and goes. */
    {
        size_t j = o, want;
        unsigned char lead;
        while (j > 0 && ((unsigned char)out[j - 1] & 0xC0) == 0x80) j--;
        if (j > 0) {
            lead = (unsigned char)out[j - 1];
            want = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
            if (o - (j - 1) < want) o = j - 1;
        }
    }
    out[o] = '\0';
    return o;
}

int oc_rt_target(const char *body, size_t len, const oc_rt_span *span,
                 size_t *off, size_t *tlen) {
    size_t o, L;
    if (!body || !span || span->style != OC_RT_LABELLED) return 0;
    o = span->start + span->len + 2;                  /* past "](" */
    if (o > len || body[o - 2] != ']' || body[o - 1] != '(') return 0;
    L = oc_url_len(body, len, o);
    if (!L) return 0;
    *off = o; *tlen = L;
    return 1;
}
