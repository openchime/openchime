/* Bare-address detection — see url.h and MARKDOWN.md §4. Lifted verbatim from
 * the client formatting parser (client/core/richtext.c), which now calls this
 * instead of carrying its own copy: the daemon's unfurl fetcher (ARCH-105)
 * needs the same rules, and shared/ is the directory for code the daemon and
 * the client must agree on (mention.c, searchq.c, notify.c). */

#include "url.h"

static int u_space(char ch) { return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r'; }
static int u_delim(char ch) { return ch == '*' || ch == '_' || ch == '~' || ch == '`'; }
static char u_lower(char ch) { return (ch >= 'A' && ch <= 'Z') ? (char)(ch + 32) : ch; }

/* May an address START at `i`? Preceded by a line start, whitespace or an
 * opening bracket — looking THROUGH any run of formatting delimiters
 * immediately before it, so `*https://x.com*` links while
 * `xhttps://x.com` (an identifier) does not. The same look-through rule the
 * formatting parser applies to its own openers, for the same reason. */
static int u_boundary_ok(const char *b, size_t i) {
    char p;
    while (i > 0 && u_delim(b[i - 1])) i--;
    if (i == 0) return 1;
    p = b[i - 1];
    return u_space(p) || p == '(' || p == '[' || p == '{' || p == '<';
}

static size_t u_scheme_len(const char *b, size_t end, size_t i) {
    static const char *https = "https://", *http = "http://";
    size_t k;
    for (k = 0; k < 8; k++) if (i + k >= end || u_lower(b[i + k]) != https[k]) break;
    if (k == 8) return 8;                      /* https first: http is its prefix */
    for (k = 0; k < 7; k++) if (i + k >= end || u_lower(b[i + k]) != http[k]) break;
    return k == 7 ? 7 : 0;
}

/* A byte that cannot be inside a URL. Space ends it; `<`/`>`/`"` cannot appear
 * literally in one; a backtick would let a link straddle a code span. */
static int u_stop(char ch) {
    unsigned char u = (unsigned char)ch;
    return u <= 0x20 || u == 0x7f || ch == '<' || ch == '>' || ch == '"' || ch == '`';
}

/* Trailing bytes that belong to the sentence rather than the address.
 * `_` is deliberately NOT here — underscores are ordinary inside real
 * addresses, and trimming one the address owns changes where the link GOES
 * (MARKDOWN.md §4 records the trade in full). */
static int u_trim(char ch) {
    return ch == '.' || ch == ',' || ch == ';' || ch == ':' || ch == '!' ||
           ch == '?' || ch == '\'' || ch == '*' || ch == '~';
}

size_t oc_url_len(const char *b, size_t end, size_t i) {
    size_t s = u_scheme_len(b, end, i), j, n;
    if (!s || !u_boundary_ok(b, i)) return 0;
    for (j = i + s; j < end && !u_stop(b[j]); j++) { }
    n = j - i;
    for (;;) {
        char last;
        if (n <= s) return 0;                  /* a scheme with no address */
        last = b[i + n - 1];
        if (u_trim(last)) { n--; continue; }
        /* A closing bracket is part of the address only if the address opened
         * it: Wikipedia's `..._(disambiguation)` keeps its `)`, while a URL
         * written inside `(parentheses)` does not take the one that closes
         * them. */
        if (last == ')' || last == ']' || last == '}') {
            char op = last == ')' ? '(' : (last == ']' ? '[' : '{');
            size_t k, no = 0, nc = 0;
            for (k = 0; k < n; k++) {
                if (b[i + k] == op)        no++;
                else if (b[i + k] == last) nc++;
            }
            if (nc > no) { n--; continue; }
        }
        break;
    }
    return n;
}

/* An inline `code` span opening at `i`: its total length including both
 * backticks, or 0. The same acceptance rules as the formatting parser's, so
 * both walks skip the same bytes: not a run of backticks, opens at a word
 * boundary, no space right after the opener, closes after a non-space. */
static size_t u_code_span_len(const char *b, size_t end, size_t i) {
    size_t j;
    if (b[i] != '`') return 0;
    if (i + 1 < end && b[i + 1] == '`') return 0;
    if (!u_boundary_ok(b, i)) return 0;
    if (i + 1 >= end || u_space(b[i + 1])) return 0;
    for (j = i + 2; j < end; j++)
        if (b[j] == '`' && !u_space(b[j - 1])) return j - i + 1;
    return 0;
}

size_t oc_url_extract(const char *b, size_t len, oc_url_span *out, size_t max) {
    size_t i = 0, n = 0;
    if (!b || !out || !max) return 0;
    while (i < len && n < max) {
        size_t L;
        /* A closed ```fenced block``` is literal down to its closing fence;
         * an unclosed one is literal text and is walked normally. */
        if (b[i] == '`' && i + 2 < len && b[i + 1] == '`' && b[i + 2] == '`'
            && u_boundary_ok(b, i)) {
            size_t j;
            for (j = i + 3; j + 3 <= len; j++)
                if (b[j] == '`' && b[j + 1] == '`' && b[j + 2] == '`') break;
            if (j + 3 <= len) { i = j + 3; continue; }
        }
        L = u_code_span_len(b, len, i);
        if (L) { i += L; continue; }
        /* A backslash escape is markup; the escaped byte cannot open anything. */
        if (b[i] == '\\' && i + 1 < len) { i += 2; continue; }
        L = oc_url_len(b, len, i);
        if (L) {
            out[n].start = i;
            out[n].len   = L;
            n++;
            i += L;
            continue;
        }
        i++;
    }
    return n;
}
