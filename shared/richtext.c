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
 * An inline spoiler is the one construct the other passes have to ask about:
 * `>!` looks like a quote to scan_line, a table row splits on any pipe, and a
 * fence or an emphasis run may start inside one. Each asks spoiler_close, so
 * what is hidden is decided once, and before any of them can show it.
 *
 * No allocation and no rewriting: every span points into the caller's bytes. */

#include "richtext.h"

#include "url.h"   /* shared/: the address rules the daemon must agree with */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
    int         in_spoiler;  /* inside an inline spoiler, where `>!` is only text */
    /* oc_rt_spoilers: what emit is told goes straight into this list, not spans. */
    oc_rt_spoiler *sv;
    size_t      sv_max, sv_n;
    uint16_t    sv_kind;     /* the spoiler open now, or 0 */
    size_t      sv_start, sv_hidden, sv_table_end;
    int         sv_cell, sv_extra;
    uint32_t    cols;        /* scan_cells: the table's columns */
    int         cell_extra;  /* scanning a cell past them */
} rt_ctx;

/* A span, as oc_rt_spoilers reads it: each opener and the next closer of its kind
 * make one spoiler, and a closer of the other kind inside a block goes with it.
 * Nothing is kept but the list, so this cannot run out of memory: "none" from
 * oc_rt_spoilers always means none. */
static void note_spoiler(rt_ctx *c, size_t start, size_t len, uint16_t style) {
    uint16_t kind = style & (OC_RT_SPOILER | OC_RT_SPOILER_BLOCK);
    if (style == OC_RT_TABLE) c->sv_table_end = start + len;
    if (!kind || style != (OC_RT_DELIM | kind)) return;
    if (!c->sv_kind) {
        c->sv_kind = kind; c->sv_start = start; c->sv_hidden = start + len;
        c->sv_cell = start < c->sv_table_end;
        c->sv_extra = c->sv_cell && c->cell_extra;
        return;
    }
    if (kind != c->sv_kind) return;
    if (c->sv_n < c->sv_max) {
        oc_rt_spoiler *o = &c->sv[c->sv_n];
        memset(o, 0, sizeof *o);
        o->start = c->sv_start;
        o->len = start + len - c->sv_start;
        o->hidden = c->sv_hidden;
        o->hidden_len = start - c->sv_hidden;
        o->block = kind == OC_RT_SPOILER_BLOCK;
        o->cell = c->sv_cell;
        o->extra = c->sv_extra;
        if (o->block) {
            size_t a = c->sv_start + 10, b = c->sv_hidden;
            while (a < b && (c->b[a] == ' ' || c->b[a] == '\t')) a++;
            while (b > a && (c->b[b - 1] == '\n' || c->b[b - 1] == '\r' || c->b[b - 1] == ' ' || c->b[b - 1] == '\t')) b--;
            o->label = a; o->label_len = b - a;
        }
    }
    c->sv_n++;
    c->sv_kind = 0;
}

static void emit(rt_ctx *c, size_t start, size_t len, uint16_t style) {
    if (len == 0) return;             /* an empty span is not a rendering */
    if (c->sv) { note_spoiler(c, start, len, style); return; }
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
 * no way to write a literal asterisk at a word boundary (MARKDOWN.md §2, rule 3). */
static int rt_escapable(char ch) {
    return rt_delim(ch) || ch == '>' || ch == '-' || ch == '\\' || ch == '|' || ch == '!';
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

/* open_ok for what may follow a spoiler's opening, so that what it hides can
 * start with emphasis or code. Not for a fence: a block cannot open in a line's
 * spoiler. */
static int inner_open_ok(rt_ctx *c, size_t i) {
    if (open_ok(c, i)) return 1;
    while (i > 0 && rt_delim(c->b[i - 1])) i--;
    return i >= 2 && c->b[i - 1] == '!' && c->b[i - 2] == '>';
}

/* If a complete `code` span opens at `i` and closes before `e`, its total
 * length including both backticks; otherwise 0. A RUN of backticks is never an
 * inline code opener — the fenced form is handled a pass above, and a leftover
 * run is literal text. */
static size_t code_span_len(rt_ctx *c, size_t i, size_t e) {
    size_t j;
    if (c->b[i] != '`') return 0;
    if (i + 1 < e && c->b[i + 1] == '`') return 0;
    if (!inner_open_ok(c, i)) return 0;
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

/* ---- time tokens (MARKDOWN.md §1c) ----------------------------------------
 *
 * `<time:YYYY-MM-DDTHH:MM[:SS]Z>`: a moment, stored in UTC and readable as
 * written in a client that does not render it. The calendar is the proleptic
 * Gregorian one, by Howard Hinnant's days-from-civil, so no C library time zone
 * or range limit is involved. */
static int64_t floor_div(int64_t a, int64_t b) { int64_t q = a / b; return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q; }

static int64_t days_from_civil(int64_t y, int m, int d) {
    y -= m <= 2;
    int64_t era = floor_div(y, 400);
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days(int64_t z, int64_t *y, int *m, int *d) {
    z += 719468;
    int64_t era = floor_div(z, 146097);
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = yoe + era * 400 + (*m <= 2);
}

static int rt_digits(const char *b, size_t i, int n, int *v) {
    int x = 0;
    for (int k = 0; k < n; k++) {
        char ch = b[i + (size_t)k];
        if (ch < '0' || ch > '9') return 0;
        x = x * 10 + (ch - '0');
    }
    *v = x;
    return 1;
}

/* A time token at `i`, ending before `e`: its length, with the moment in *ms;
 * else 0. A date that does not exist (30 February) is not a token. */
static size_t time_len(const char *b, size_t e, size_t i, int64_t *ms) {
    static const int MDAYS[12] = { 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int Y, M, D, h, mi, sec = 0;
    size_t j = i + 6;
    if (i + 6 + 16 + 2 > e || memcmp(b + i, "<time:", 6) != 0) return 0;
    if (!rt_digits(b, j, 4, &Y) || b[j + 4] != '-' || !rt_digits(b, j + 5, 2, &M) || b[j + 7] != '-' ||
        !rt_digits(b, j + 8, 2, &D) || b[j + 10] != 'T' || !rt_digits(b, j + 11, 2, &h) ||
        b[j + 13] != ':' || !rt_digits(b, j + 14, 2, &mi)) return 0;
    j += 16;
    if (b[j] == ':') {
        if (j + 3 + 2 > e || !rt_digits(b, j + 1, 2, &sec) || sec > 59) return 0;
        j += 3;
    }
    if (j + 2 > e || b[j] != 'Z' || b[j + 1] != '>') return 0;
    if (Y < 1 || M < 1 || M > 12 || D < 1 || D > MDAYS[M - 1] || h > 23 || mi > 59) return 0;
    if (M == 2 && D == 29 && !((Y % 4 == 0 && Y % 100 != 0) || Y % 400 == 0)) return 0;
    if (ms) *ms = ((days_from_civil(Y, M, D) * 86400) + h * 3600 + mi * 60 + sec) * 1000;
    return j + 2 - i;
}

/* ---- inline spoilers (MARKDOWN.md §1d) ------------------------------------ */

static int at_spoiler_open(rt_ctx *c, size_t i, size_t e) {
    return i + 1 < e && c->b[i] == '>' && c->b[i + 1] == '!';
}

/* The `!<` that closes the `>!` at `i`, before `e`, or RT_NONE when there is no
 * closer at all. Escapes, code, addresses and time tokens are stepped over. An
 * opener inside counts, so `>!a >!b!< c!<` hides all of it, and when the
 * openers outnumber the closers the last closer is the one: the wider reading
 * is the one that leaves nothing out. `>!<` can open nothing, so `a>!<` closes
 * after the `a>`. */
static size_t spoiler_close(rt_ctx *c, size_t i, size_t e) {
    size_t j = i + 2, last = RT_NONE, depth = 1, L, k;
    while (j < e) {
        char ch = c->b[j];
        if (ch == '\\' && j + 1 < e && rt_escapable(c->b[j + 1])) { j += 2; continue; }
        if ((L = code_span_len(c, j, e)) != 0 || (L = url_len(c, j, e)) != 0 ||
            (L = labelled_len(c, j, e, &k)) != 0 ||
            (ch == '<' && (L = time_len(c->b, e, j, NULL)) != 0)) { j += L; continue; }
        if (ch == '`') { while (j < e && c->b[j] == '`') j++; continue; }   /* a bare run is literal */
        if (ch == '!' && j + 1 < e && c->b[j + 1] == '<') {
            last = j;
            if (--depth == 0) break;
            j += 2;
            continue;
        }
        if (at_spoiler_open(c, j, e) && !(j + 2 < e && c->b[j + 2] == '<')) { depth++; j += 2; continue; }
        j++;
    }
    return last;
}

/* Is there nothing but white space in [s, e)? An empty spoiler is no spoiler. */
static int all_space(const char *b, size_t s, size_t e) {
    for (; s < e; s++) if (!rt_space(b[s])) return 0;
    return 1;
}

/* Does a spoiler, closed and not empty, open at the `>!` at `i`? */
static int spoiler_opens(rt_ctx *c, size_t i, size_t e) {
    size_t j;
    if (!at_spoiler_open(c, i, e)) return 0;
    j = spoiler_close(c, i, e);
    return j != RT_NONE && !all_space(c->b, i + 2, j);
}

/* The first valid closer for a `d`-byte run of `ch` in [from, e), or RT_NONE.
 * Escapes, code spans and URLs are stepped over rather than looked inside, so
 * the `*` in "*a `b*c` d*" closes at the end and not in the middle — and an
 * underscore inside a URL cannot close an emphasis run that opened before it. */
static size_t find_close(rt_ctx *c, size_t from, size_t e, char ch, size_t d) {
    size_t i = from;
    int no_spoiler = c->in_spoiler;
    while (i < e) {
        size_t cs;
        if (c->b[i] == '\\' && i + 1 < e && rt_escapable(c->b[i + 1])) { i += 2; continue; }
        /* A spoiler is stepped over whole: emphasis closing inside one would
         * cut its range short of the closer, and show what it hides. */
        if (!no_spoiler && at_spoiler_open(c, i, e)) {
            cs = spoiler_close(c, i, e);
            if (cs == RT_NONE) no_spoiler = 1;
            else { i = cs + 2; continue; }
        }
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

/* Does a spoiler that opens in [s, z) close at or past `z`, before `e`? A link's
 * label is such a range: taken as a link it would cut the spoiler in two. */
static int spoiler_leaves(rt_ctx *c, size_t s, size_t z, size_t e) {
    if (c->in_spoiler) return 0;
    while (s < z) {
        size_t j;
        if (c->b[s] == '\\' && s + 1 < z && rt_escapable(c->b[s + 1])) { s += 2; continue; }
        if ((j = code_span_len(c, s, z)) != 0) { s += j; continue; }
        if (at_spoiler_open(c, s, z)) {
            j = spoiler_close(c, s, e);
            if (j == RT_NONE) return 0;
            if (j >= z) return 1;
            s = j + 2;
            continue;
        }
        s++;
    }
    return 0;
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
    int hopeless_spoiler = 0;
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
        /* A time token is one span, like a URL: nothing inside it is markup. */
        if (ch == '<') {
            size_t L = time_len(c->b, e, i, NULL);
            if (L) { emit(c, i, L, OC_RT_TIME); i += L; continue; }
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
            if (L && !spoiler_leaves(c, i + 1, k, e)) {
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
        /* A spoiler (MARKDOWN.md §1d): `>!` opens anywhere and `!<` closes it,
         * on the same line, with something other than white space between.
         * What it hides may carry emphasis, but not another spoiler. */
        if (ch == '>' && i + 1 < e && c->b[i + 1] == '!' && !c->in_spoiler && !hopeless_spoiler) {
            size_t j = spoiler_close(c, i, e);
            if (j == RT_NONE) hopeless_spoiler = 1;
            else if (all_space(c->b, i + 2, j)) { i = j + 2; continue; }
            else {
                emit(c, i, 2, OC_RT_DELIM | OC_RT_SPOILER);
                emit(c, i + 2, j - (i + 2), OC_RT_SPOILER);
                c->in_spoiler++;
                scan_inline(c, i + 2, j, depth + 1);
                c->in_spoiler--;
                emit(c, j, 2, OC_RT_DELIM | OC_RT_SPOILER);
                i = j + 2;
                continue;
            }
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

        if (inner_open_ok(c, i) && i + d < e && !rt_space(c->b[i + d]) && !hopeless[slot][d]) {
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
        i += d;                                /* unclosed: literal, MARKDOWN.md §2, rule 2 */
    }
}

/* One line, `\n` already excluded. Lifts at most one block marker off the front
 * and recurses, so `> - item` is a bulleted line inside a quote. */
static void scan_line(rt_ctx *c, size_t s, size_t e, int depth) {
    size_t p = s, q;
    if (depth > 3) { scan_inline(c, s, e, 0); return; }
    while (p < e && (c->b[p] == ' ' || c->b[p] == '\t')) p++;   /* indentation */

    /* `>!` that closes on this line is a spoiler, not a quote whose text begins
     * with a `!` (MARKDOWN.md §1d). One that does not close is the quote it
     * always was. */
    if (spoiler_opens(c, p, e)) { scan_inline(c, s, e, 0); return; }
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
 * to `cells` and returns how many there are, however many that is; *pipes says
 * whether the line had an unescaped pipe at all. With `scan`, each cell's inline
 * spans are reported too, past `max` as well. A pipe inside a spoiler separates nothing: split
 * there, the spoiler's closer would be in the next cell and neither would be
 * hidden (MARKDOWN.md §1d). */
static size_t split_row(rt_ctx *c, size_t s, size_t e, oc_rt_cell *cells, size_t max, int *pipes, int scan) {
    const char *b = c->b;
    size_t n = 0, k, cs;
    int spoilers = 0;
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
    for (k = s; k + 1 < e && !spoilers; k++) spoilers = b[k] == '>' && b[k + 1] == '!';
    cs = s;
    for (k = s; k <= e; k++) {
        if (k < e && b[k] == '\\' && k + 1 < e) { k++; continue; }
        if (spoilers && k < e && b[k] == '`') {
            /* Code that stays in one cell: a `>!` in it opens nothing. */
            size_t L = code_span_len(c, k, e);
            if (L && !memchr(b + k, '|', L)) { k += L - 1; continue; }
        }
        if (spoilers && at_spoiler_open(c, k, e)) {
            size_t j = spoiler_close(c, k, e);
            if (j == RT_NONE) spoilers = 0;   /* nor will a later one close */
            else if (!all_space(b, k + 2, j)) { k = j + 1; continue; }
        }
        if (k == e || b[k] == '|') {
            size_t a = cs, z = k;
            if (k < e) *pipes = 1;
            while (a < z && (b[a] == ' ' || b[a] == '\t')) a++;
            while (z > a && (b[z - 1] == ' ' || b[z - 1] == '\t')) z--;
            if (n < max) { cells[n].start = a; cells[n].len = z - a; }
            if (scan) {
                c->cell_extra = n >= c->cols;
                scan_inline(c, a, z, 0);
            }
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
 * list and does not start a table. A spoiler's `>!` is no marker. */
static int block_marker(rt_ctx *c, size_t s, size_t e) {
    const char *b = c->b;
    size_t q;
    while (s < e && (b[s] == ' ' || b[s] == '\t')) s++;
    if (s < e && b[s] == '>') return !spoiler_opens(c, s, e);
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
    if (hn >= e || ht == i || block_marker(c, i, ht)) return 0;
    nh = split_row(c, i, ht, head, OC_RT_TABLE_MAX_COLS + 1, &hp, 0);
    if (!hp || nh == 0 || nh > OC_RT_TABLE_MAX_COLS) return 0;
    sn = line_at(c, hn + 1, e, &st);
    ns = split_row(c, hn + 1, st, sep, OC_RT_TABLE_MAX_COLS + 1, &sp, 0);
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
        split_row(c, p, rt, tmp, 0, &rp, 0);
        if (!rp) break;                           /* so does a line with no pipe */
        t->rows++;
        last = rt;
        p = rn < e ? rn + 1 : e;
    }
    t->len = last - i;
    return p;
}

/* The inline spans of every cell of table `t`, for oc_rt_spoilers, in order: the
 * cells a row has past the table's columns too. No one draws those, but a frontend
 * that shows the source, or a table as text, shows them. */
static void scan_cells(rt_ctx *c, const oc_rt_table *t) {
    size_t p = t->start, end = t->start + t->len, row = 0;
    c->in_cell = 1;
    c->cols = t->cols;
    while (p < end) {
        size_t trim, nl = line_at(c, p, end, &trim);
        int pipes;
        if (row++ != 1) split_row(c, p, trim, NULL, 0, &pipes, 1);   /* row 1 is the separator */
        p = nl + 1;
    }
    c->in_cell = 0;
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
                if (c->sv) scan_cells(c, &t);
            }
            i = after;
            continue;
        }
        nl = line_at(c, i, e, &end);
        if (end > i && !c->tables_only) scan_line(c, i, end, 0);
        i = nl + 1;
    }
}

/* Is `i` inside an inline spoiler on its own line? Then the fence there opens
 * nothing: it would show what the spoiler around it hides. */
static int in_line_spoiler(rt_ctx *c, size_t i) {
    size_t s = i, e = i, p;
    while (s > 0 && c->b[s - 1] != '\n') s--;
    while (e < c->len && c->b[e] != '\n') e++;
    for (p = s; p < i; p++) {
        size_t j;
        if (c->b[p] == '\\' && p + 1 < e && rt_escapable(c->b[p + 1])) { p++; continue; }
        if (!at_spoiler_open(c, p, e)) continue;
        j = spoiler_close(c, p, e);
        if (j == RT_NONE) return 0;
        if (j >= i && !all_space(c->b, p + 2, j)) return 1;
        p = j + 1;
    }
    return 0;
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
        if (close == RT_NONE || in_line_spoiler(c, i)) { i++; continue; }   /* unclosed fence: literal text */

        scan_text(c, seg, i);
        /* ```spoiler Label (MARKDOWN.md §1d): the opening line, label and all,
         * is the delimiter, and the lines up to the closing fence are TEXT,
         * hidden, rather than code. It needs a line of its own to open. */
        if (close >= i + 10 && memcmp(body + i + 3, "spoiler", 7) == 0 &&
            (body[i + 10] == ' ' || body[i + 10] == '\t' || body[i + 10] == '\r' || body[i + 10] == '\n')) {
            size_t eol = i + 10;
            while (eol < close && body[eol] != '\n') eol++;
            if (eol < close) {
                cs = eol + 1; ce = close;
                if (ce > cs && body[ce - 1] == '\n') {
                    ce--;
                    if (ce > cs && body[ce - 1] == '\r') ce--;
                }
                emit(c, i, cs - i, OC_RT_DELIM | OC_RT_SPOILER_BLOCK);
                emit(c, cs, ce - cs, OC_RT_SPOILER_BLOCK);
                scan_text(c, cs, ce);
                emit(c, ce, close + 3 - ce, OC_RT_DELIM | OC_RT_SPOILER_BLOCK);
                i = seg = close + 3;
                continue;
            }
        }
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
    n = split_row(&c, it->pos, trim, cells, it->cols, &pipes, 0);
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

int oc_rt_time_value(const char *body, size_t len, const oc_rt_span *span, int64_t *ms) {
    if (!body || !span || span->style != OC_RT_TIME || span->start + span->len > len) return 0;
    return time_len(body, span->start + span->len, span->start, ms) == span->len;
}

size_t oc_rt_time_token(int64_t ms, char *out, size_t cap) {
    int64_t s = floor_div(ms, 1000), days = floor_div(s, 86400), y;
    int64_t sod = s - days * 86400;
    int m, d, w;
    civil_from_days(days, &y, &m, &d);
    if (y < 1 || y > 9999) return 0;
    if (sod % 60)
        w = snprintf(out, cap, "<time:%04d-%02d-%02dT%02d:%02d:%02dZ>", (int)y, m, d,
                     (int)(sod / 3600), (int)(sod / 60 % 60), (int)(sod % 60));
    else
        w = snprintf(out, cap, "<time:%04d-%02d-%02dT%02d:%02dZ>", (int)y, m, d,
                     (int)(sod / 3600), (int)(sod / 60 % 60));
    if (w < 0 || (size_t)w >= cap) { if (cap) out[0] = '\0'; return 0; }
    return (size_t)w;
}

size_t oc_rt_time_text(int64_t ms, const oc_rt_clock *clock, int form, char *out, size_t cap) {
    static const char *const WD[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
    static const char *const MO[12] = { "January", "February", "March", "April", "May", "June", "July",
                                        "August", "September", "October", "November", "December" };
    oc_rt_clock utc = { 0, 1, 1, 0 };
    const oc_rt_clock *k = clock ? clock : &utc;
    int tz = k->utc ? 0 : k->tz_min;
    int64_t now = k->now_ms ? k->now_ms : (int64_t)time(NULL) * 1000;
    int64_t ls = floor_div(ms, 1000) + (int64_t)tz * 60, days = floor_div(ls, 86400), sod = ls - days * 86400;
    int64_t ndays = floor_div(floor_div(now, 1000) + (int64_t)tz * 60, 86400), y, ny;
    int m, d, nm, nd, hh = (int)(sod / 3600), mi = (int)(sod / 60 % 60);
    int wd = (int)(((days % 7) + 11) % 7);           /* 1970-01-01 was a Thursday */
    char clk[16];
    int w;
    if (!out || !cap) return 0;
    civil_from_days(days, &y, &m, &d);
    civil_from_days(ndays, &ny, &nm, &nd);
    if (k->time24) snprintf(clk, sizeof clk, "%02d:%02d", hh, mi);
    else           snprintf(clk, sizeof clk, "%d:%02d %s", hh % 12 ? hh % 12 : 12, mi, hh < 12 ? "AM" : "PM");
    if (form == OC_RT_TIME_FULL) {
        char zone[24];
        if (k->utc)   snprintf(zone, sizeof zone, " UTC");
        else if (!tz) snprintf(zone, sizeof zone, " (UTC)");
        else          snprintf(zone, sizeof zone, " (UTC%c%02d:%02d)", tz < 0 ? '-' : '+',
                               (tz < 0 ? -tz : tz) / 60, (tz < 0 ? -tz : tz) % 60);
        w = snprintf(out, cap, "%s %d %s %lld, %s%s", WD[wd], d, MO[m - 1], (long long)y, clk, zone);
    } else if (days == ndays) {
        w = snprintf(out, cap, "%s%s", clk, k->utc ? " UTC" : "");
    } else if (y == ny) {
        w = snprintf(out, cap, "%.3s %d %.3s, %s%s", WD[wd], d, MO[m - 1], clk, k->utc ? " UTC" : "");
    } else {
        w = snprintf(out, cap, "%.3s %d %.3s %lld, %s%s", WD[wd], d, MO[m - 1], (long long)y, clk,
                     k->utc ? " UTC" : "");
    }
    if (w < 0) { out[0] = '\0'; return 0; }
    return (size_t)w < cap ? (size_t)w : cap - 1;
}

/* Whether `body` holds what a spoiler opens with anywhere in its `len` bytes:
 * bounded, since a body need not end in a NUL. */
static int has_spoiler_open(const char *body, size_t len) {
    for (size_t i = 0; i + 1 < len; i++) {
        if (body[i] == '>' && body[i + 1] == '!') return 1;
        if (body[i] == '`' && len - i >= 10 && memcmp(body + i, "```spoiler", 10) == 0) return 1;
    }
    return 0;
}

size_t oc_rt_spoilers(const char *body, size_t len, oc_rt_spoiler *out, size_t max) {
    oc_rt_spoiler none;
    rt_ctx c;
    if (!body || len == 0 || !has_spoiler_open(body, len)) return 0;
    /* The scan oc_rt_scan makes, with what is in each table's cells as well. */
    memset(&c, 0, sizeof c);
    c.b = body; c.len = len;
    c.sv = out ? out : &none; c.sv_max = out ? max : 0;
    scan_body(&c);
    return c.sv_n;
}

char *oc_rt_spoilers_covered(const char *body, size_t len) {
    oc_rt_spoiler sp[32];
    size_t n = oc_rt_spoilers(body, len, sp, 32), i = 0, o = 0;
    char *out;
    if (!n) return NULL;
    if (n > 32) { len = sp[31].start + sp[31].len; n = 32; }
    out = malloc(len + n * 80 + 1);
    if (!out) return NULL;
    for (size_t k = 0; k < n; k++) {
        memcpy(out + o, body + i, sp[k].start - i);
        o += sp[k].start - i;
        if (sp[k].label_len) o += (size_t)sprintf(out + o, "[spoiler: %.*s]", (int)(sp[k].label_len > 64 ? 64 : sp[k].label_len), body + sp[k].label);
        else                 o += (size_t)sprintf(out + o, "[spoiler]");
        i = sp[k].start + sp[k].len;
    }
    memcpy(out + o, body + i, len - i);
    o += len - i;
    out[o] = '\0';
    return out;
}

size_t oc_rt_visible_urls(const char *body, size_t len, oc_url_span *out, size_t max) {
    oc_url_span all[64];
    size_t n = oc_url_extract(body, len, all, 64), nh, w = 0;
    oc_rt_spoiler *hid;
    if (!n) return 0;
    nh = oc_rt_spoilers(body, len, NULL, 0);
    hid = nh ? malloc(nh * sizeof *hid) : NULL;
    if (nh && !hid) return 0;
    if (nh) nh = oc_rt_spoilers(body, len, hid, nh);
    for (size_t i = 0; i < n && w < max; i++) {
        int hidden = 0;
        for (size_t k = 0; k < nh && !hidden; k++)
            hidden = all[i].start >= hid[k].start && all[i].start < hid[k].start + hid[k].len;
        if (!hidden) out[w++] = all[i];
    }
    free(hid);
    return w;
}

/* What an excerpt puts in place of a construct: a table's one line, a time
 * token's text, or a spoiler's word. */
enum { RT_R_TABLE, RT_R_TIME, RT_R_SPOILER };
typedef struct { size_t start, len; int kind; uint32_t rows, cols; int64_t ms; size_t label, label_len; } rt_repl;

size_t oc_rt_flatten_at(const char *body, size_t len, const oc_rt_clock *clock, char *out, size_t cap) {
    oc_rt_table t[32];
    oc_rt_spoiler sv[32];
    oc_rt_span sp[OC_RT_MAX];
    rt_repl r[32 + 32 + OC_RT_MAX];
    size_t nt, nv, ns, nr = 0, k, i = 0, o = 0, lim;
    if (!out || cap == 0) return 0;
    out[0] = '\0';
    if (!body) return 0;
    /* Everything from the whole body, and only then any limit: a limit that fell
     * inside a spoiler before it was listed would cut off its closer, and with it
     * the fact that it was one. What starts before the limit goes whole, and past
     * the limit nothing is shown, rather than show something hidden. */
    lim = len;
    nt = oc_rt_tables(body, len, t, 32);
    if (nt > 32) { lim = t[31].start + t[31].len; nt = 32; }
    nv = oc_rt_spoilers(body, len, sv, 32);
    if (nv > 32) {
        if (sv[31].start + sv[31].len < lim) lim = sv[31].start + sv[31].len;
        nv = 32;
    }
    ns = memchr(body, '<', len) ? oc_rt_scan(body, len, sp, OC_RT_MAX) : 0;
    if (ns > OC_RT_MAX) ns = OC_RT_MAX;
    for (k = 0; k < nt; k++) {
        if (t[k].start >= lim) continue;
        r[nr].start = t[k].start; r[nr].len = t[k].len; r[nr].kind = RT_R_TABLE;
        r[nr].rows = t[k].rows; r[nr].cols = t[k].cols; nr++;
    }
    for (k = 0; k < nv; k++) {
        if (sv[k].start >= lim) continue;
        r[nr].start = sv[k].start; r[nr].len = sv[k].len; r[nr].kind = RT_R_SPOILER;
        r[nr].label = sv[k].label; r[nr].label_len = sv[k].label_len; nr++;
    }
    for (k = 0; k < ns; k++) {
        if (sp[k].style != OC_RT_TIME || sp[k].start >= lim || !oc_rt_time_value(body, len, &sp[k], &r[nr].ms)) continue;
        r[nr].start = sp[k].start; r[nr].len = sp[k].len; r[nr].kind = RT_R_TIME; nr++;
    }
    len = lim;
    for (k = 1; k < nr; k++) {
        rt_repl x = r[k];
        size_t q = k;
        while (q > 0 && (r[q - 1].start > x.start ||
                         (r[q - 1].start == x.start && r[q - 1].len < x.len))) { r[q] = r[q - 1]; q--; }
        r[q] = x;
    }
    {
        size_t m = 0;
        for (k = 0; k < nr; k++)
            if (!m || r[k].start >= r[m - 1].start + r[m - 1].len) r[m++] = r[k];
        nr = m;
    }
    for (k = 0; k <= nr && o + 1 < cap; k++) {
        /* A limit inside a table or a spoiler leaves `i` past `len`: it went whole. */
        size_t upto = k < nr ? r[k].start : len, n = upto > i ? upto - i : 0;
        if (n > cap - 1 - o) n = cap - 1 - o;
        memcpy(out + o, body + i, n);
        o += n;
        if (k < nr && o + 1 < cap) {
            int w;
            if (r[k].kind == RT_R_TABLE)
                w = snprintf(out + o, cap - o, "Table: %u row%s \xC3\x97 %u column%s",
                             (unsigned)r[k].rows, r[k].rows == 1 ? "" : "s",
                             (unsigned)r[k].cols, r[k].cols == 1 ? "" : "s");
            else if (r[k].kind == RT_R_TIME)
                w = (int)oc_rt_time_text(r[k].ms, clock, OC_RT_TIME_SHORT, out + o, cap - o);
            else if (r[k].label_len)
                w = snprintf(out + o, cap - o, "[spoiler: %.*s]", (int)r[k].label_len, body + r[k].label);
            else
                w = snprintf(out + o, cap - o, "[spoiler]");
            if (w > 0) o += (size_t)w < cap - o ? (size_t)w : cap - 1 - o;
            i = r[k].start + r[k].len;
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

char *oc_rt_local_times(const char *body, size_t len, const oc_rt_clock *clock) {
    oc_rt_span sp[OC_RT_MAX];
    size_t ns, k, i = 0, o = 0, cap;
    char *out;
    if (!body || !memchr(body, '<', len)) return NULL;
    ns = oc_rt_scan(body, len, sp, OC_RT_MAX);
    if (ns > OC_RT_MAX) ns = OC_RT_MAX;
    for (k = 0; k < ns && sp[k].style != OC_RT_TIME; k++) {}
    if (k == ns) return NULL;
    /* A token is at least 24 bytes and its text at most 40: room for each to grow. */
    cap = len + ns * 48 + 1;
    out = malloc(cap);
    if (!out) return NULL;
    for (k = 0; k < ns; k++) {
        int64_t ms;
        if (sp[k].style != OC_RT_TIME || !oc_rt_time_value(body, len, &sp[k], &ms)) continue;
        memcpy(out + o, body + i, sp[k].start - i);
        o += sp[k].start - i;
        o += oc_rt_time_text(ms, clock, OC_RT_TIME_SHORT, out + o, cap - o);
        i = sp[k].start + sp[k].len;
    }
    memcpy(out + o, body + i, len - i);
    o += len - i;
    out[o] = '\0';
    return out;
}

size_t oc_rt_flatten(const char *body, size_t len, char *out, size_t cap) {
    return oc_rt_flatten_at(body, len, NULL, out, cap);
}

size_t oc_rt_plain_excerpt(const char *body, size_t len, char *out, size_t cap) {
    return oc_rt_flatten_at(body, len, NULL, out, cap);
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
