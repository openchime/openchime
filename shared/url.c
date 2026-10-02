/* Bare-address detection — see url.h and MARKDOWN.md §4. Lifted verbatim from
 * the client formatting parser (shared/richtext.c), which now calls this
 * instead of carrying its own copy: the daemon's unfurl fetcher (ARCH-105)
 * needs the same rules, and shared/ is the directory for code the daemon and
 * the client must agree on (mention.c, searchq.c, notify.c). */

#include "url.h"
#include <stdio.h>
#include <string.h>

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

int oc_url_transport_ok(const char *url) {
    if (!url) return 0;
    int tls;
    if (strncmp(url, "https://", 8) == 0)     { tls = 1; url += 8; }
    else if (strncmp(url, "http://", 7) == 0) { tls = 0; url += 7; }
    else return 0;
    char host[256], port[8] = "";
    if (oc_url_authority(url, strcspn(url, "/?#"), host, sizeof host, port, sizeof port) != 0) return 0;
    return tls || strcmp(host, "127.0.0.1") == 0 || strcmp(host, "::1") == 0 || strcmp(host, "localhost") == 0;
}

int oc_url_authority(const char *auth, size_t len, char *host, size_t hcap, char *port, size_t pcap) {
    if (!auth || !host || !port || hcap == 0 || pcap == 0) return -1;
    const char *e = auth + len;
    const char *h = auth, *he, *pp = NULL;
    if (len && *auth == '[') {                       /* [v6] or [v6]:port */
        const char *rb = memchr(auth, ']', len);
        if (!rb || rb == auth + 1) return -1;
        h = auth + 1; he = rb;
        if (rb + 1 < e) {
            if (rb[1] != ':') return -1;             /* junk after the bracket */
            pp = rb + 2;
        }
    } else {
        const char *c = memchr(auth, ':', len);
        if (c && memchr(c + 1, ':', (size_t)(e - c - 1))) return -1;   /* a bare v6 needs brackets */
        he = c ? c : e;
        if (c) pp = c + 1;
    }
    size_t hn = (size_t)(he - h);
    if (hn == 0 || hn >= hcap) return -1;
    memcpy(host, h, hn); host[hn] = '\0';
    if (pp) {
        size_t pn = (size_t)(e - pp);
        if (pn == 0 || pn >= pcap || pn > 5) return -1;
        long v = 0;
        for (size_t k = 0; k < pn; k++) {
            if (pp[k] < '0' || pp[k] > '9') return -1;
            v = v * 10 + (pp[k] - '0');
        }
        if (v < 1 || v > 65535) return -1;
        memcpy(port, pp, pn); port[pn] = '\0';
    }
    return 0;
}

int oc_url_hostheader(const char *host, const char *port, char *out, size_t cap) {
    if (!host || !out || cap == 0) return -1;
    int v6 = strchr(host, ':') != NULL;
    int n = (port && *port) ? snprintf(out, cap, v6 ? "[%s]:%s" : "%s:%s", host, port)
                            : snprintf(out, cap, v6 ? "[%s]" : "%s", host);
    return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}

/* --- query strings and form bodies ------------------------------------------ */

static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int oc_query_get(const char *query, const char *key, char *out, size_t cap) {
    if (!query || !key || !out || cap == 0) return -1;
    out[0] = '\0';
    size_t kl = strlen(key);
    const char *p = query;
    while (*p) {
        const char *amp = strchr(p, '&');
        const char *end = amp ? amp : p + strlen(p);
        const char *eq = memchr(p, '=', (size_t)(end - p));
        if (eq && (size_t)(eq - p) == kl && strncmp(p, key, kl) == 0) {
            size_t o = 0;
            for (const char *v = eq + 1; v < end; v++) {
                int c = (unsigned char)*v;
                if (c == '+') c = ' ';
                else if (c == '%') {
                    if (end - v < 3) return -1;
                    int hi = hexv(v[1]), lo = hexv(v[2]);
                    if (hi < 0 || lo < 0) return -1;
                    c = hi * 16 + lo;
                    v += 2;
                }
                if (c == 0 || o + 1 >= cap) { out[0] = '\0'; return -1; }
                out[o++] = (char)c;
            }
            out[o] = '\0';
            return 1;
        }
        p = amp ? amp + 1 : end;
    }
    return 0;
}
