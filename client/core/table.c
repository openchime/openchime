/* A table as people edit it (REQ-220, MARKDOWN.md §1a) -- see table.h. */
#include "table.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ---- a growing string --------------------------------------------------- */

typedef struct { char *p; size_t n, cap; int bad; } tb_buf;

static void tb_put(tb_buf *b, const char *s, size_t n) {
    if (b->bad || !n) return;
    if (b->n + n + 1 > b->cap) {
        size_t c = b->cap ? b->cap : 64;
        while (c < b->n + n + 1) c *= 2;
        char *g = realloc(b->p, c);
        if (!g) { b->bad = 1; return; }
        b->p = g; b->cap = c;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = '\0';
}
static void tb_puts(tb_buf *b, const char *s) { tb_put(b, s, strlen(s)); }
static void tb_putc(tb_buf *b, char c) { tb_put(b, &c, 1); }
/* The text so far, owned by the caller ("" when nothing was written). */
static char *tb_take(tb_buf *b) {
    if (b->bad) { free(b->p); return NULL; }
    if (!b->p) return calloc(1, 1);
    return b->p;
}

/* ---- the grid ----------------------------------------------------------- */

static size_t at(const oc_table *t, uint32_t row, uint8_t col) { return (size_t)row * t->cols + col; }

oc_table *oc_table_new(uint8_t cols, uint32_t rows) {
    if (cols < 1 || cols > OC_TABLE_MAX_COLS || rows > OC_TABLE_MAX_ROWS) return NULL;
    oc_table *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->cols = cols;
    t->rows = rows;
    t->header = 1;
    size_t n = (size_t)(rows + 1) * cols;
    t->cells = calloc(n, sizeof *t->cells);
    if (!t->cells) { free(t); return NULL; }
    for (size_t i = 0; i < n; i++)
        if (!(t->cells[i] = calloc(1, 1))) { oc_table_free(t); return NULL; }
    return t;
}

void oc_table_free(oc_table *t) {
    if (!t) return;
    if (t->cells) {
        size_t n = (size_t)(t->rows + 1) * t->cols;
        for (size_t i = 0; i < n; i++) free(t->cells[i]);
        free(t->cells);
    }
    free(t);
}

oc_table *oc_table_copy(const oc_table *t) {
    if (!t) return NULL;
    oc_table *c = oc_table_new(t->cols, t->rows);
    if (!c) return NULL;
    c->header = t->header;
    memcpy(c->align, t->align, sizeof c->align);
    for (uint32_t r = 0; r <= t->rows; r++)
        for (uint8_t k = 0; k < t->cols; k++)
            if (oc_table_set(c, r, k, t->cells[at(t, r, k)]) != 0) { oc_table_free(c); return NULL; }
    return c;
}

const char *oc_table_cell(const oc_table *t, uint32_t row, uint8_t col) {
    if (!t || row > t->rows || col >= t->cols) return "";
    return t->cells[at(t, row, col)];
}

int oc_table_set(oc_table *t, uint32_t row, uint8_t col, const char *text) {
    if (!t || row > t->rows || col >= t->cols) return -1;
    char *d = strdup(text ? text : "");
    if (!d) return -1;
    free(t->cells[at(t, row, col)]);
    t->cells[at(t, row, col)] = d;
    return 0;
}

/* Rebuild the grid as `cols` x `rows`, each new cell taken from the old one
 * `map` names, or empty. */
static int regrid(oc_table *t, uint8_t cols, uint32_t rows,
                  int (*map)(const oc_table *t, uint32_t r, uint8_t k, uint32_t *or_, uint8_t *ok, uint32_t arg),
                  uint32_t arg) {
    size_t n = (size_t)(rows + 1) * cols;
    char **g = calloc(n, sizeof *g);
    if (!g) return -1;
    for (uint32_t r = 0; r <= rows; r++)
        for (uint8_t k = 0; k < cols; k++) {
            uint32_t orow; uint8_t ocol;
            const char *src = map(t, r, k, &orow, &ocol, arg) ? t->cells[at(t, orow, ocol)] : "";
            if (!(g[(size_t)r * cols + k] = strdup(src))) {
                for (size_t i = 0; i < n; i++) free(g[i]);
                free(g);
                return -1;
            }
        }
    size_t old = (size_t)(t->rows + 1) * t->cols;
    for (size_t i = 0; i < old; i++) free(t->cells[i]);
    free(t->cells);
    t->cells = g;
    t->cols = cols;
    t->rows = rows;
    return 0;
}

static int map_row_ins(const oc_table *t, uint32_t r, uint8_t k, uint32_t *or_, uint8_t *ok, uint32_t a) {
    (void)t;
    if (r == a) return 0;
    *or_ = r > a ? r - 1 : r; *ok = k;
    return 1;
}
static int map_row_del(const oc_table *t, uint32_t r, uint8_t k, uint32_t *or_, uint8_t *ok, uint32_t a) {
    (void)t;
    *or_ = r >= a ? r + 1 : r; *ok = k;
    return 1;
}
static int map_col_ins(const oc_table *t, uint32_t r, uint8_t k, uint32_t *or_, uint8_t *ok, uint32_t a) {
    (void)t;
    if (k == a) return 0;
    *or_ = r; *ok = (uint8_t)(k > a ? k - 1 : k);
    return 1;
}
static int map_col_del(const oc_table *t, uint32_t r, uint8_t k, uint32_t *or_, uint8_t *ok, uint32_t a) {
    (void)t;
    *or_ = r; *ok = (uint8_t)(k >= a ? k + 1 : k);
    return 1;
}

int oc_table_insert_row(oc_table *t, uint32_t a) {
    if (!t || a < 1 || a > t->rows + 1 || t->rows >= OC_TABLE_MAX_ROWS) return -1;
    return regrid(t, t->cols, t->rows + 1, map_row_ins, a);
}

int oc_table_remove_row(oc_table *t, uint32_t a) {
    if (!t || a < 1 || a > t->rows || t->rows <= 1) return -1;
    return regrid(t, t->cols, t->rows - 1, map_row_del, a);
}

int oc_table_insert_col(oc_table *t, uint8_t a) {
    if (!t || a > t->cols || t->cols >= OC_TABLE_MAX_COLS) return -1;
    if (regrid(t, (uint8_t)(t->cols + 1), t->rows, map_col_ins, a) != 0) return -1;
    memmove(t->align + a + 1, t->align + a, (size_t)(t->cols - 1 - a));
    t->align[a] = OC_RT_ALIGN_NONE;
    return 0;
}

int oc_table_remove_col(oc_table *t, uint8_t a) {
    if (!t || a >= t->cols || t->cols <= 1) return -1;
    if (regrid(t, (uint8_t)(t->cols - 1), t->rows, map_col_del, a) != 0) return -1;
    memmove(t->align + a, t->align + a + 1, (size_t)(t->cols - a));
    t->align[t->cols] = OC_RT_ALIGN_NONE;
    return 0;
}

/* ---- to and from Markdown ----------------------------------------------- */

/* A cell in the stored form: `|` escaped, a line break as <br>, a carriage
 * return dropped. Leading and trailing spaces would be trimmed by the parser,
 * so they are not written. */
static void put_cell(tb_buf *b, const char *s) {
    size_t n = strlen(s), a = 0;
    while (a < n && (s[a] == ' ' || s[a] == '\t')) a++;
    while (n > a && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\n' || s[n - 1] == '\r')) n--;
    for (size_t i = a; i < n; i++) {
        if (s[i] == '|') tb_puts(b, "\\|");
        else if (s[i] == '\n') tb_puts(b, "<br>");
        else if (s[i] == '\r') {}
        else if (s[i] == '\t') tb_putc(b, ' ');
        else tb_putc(b, s[i]);
    }
}

char *oc_table_to_markdown(const oc_table *t) {
    if (!t || !t->cols) return NULL;
    tb_buf b = { 0 };
    for (uint32_t r = 0; r <= t->rows; r++) {
        if (r) tb_putc(&b, '\n');
        for (uint8_t k = 0; k < t->cols; k++) {
            tb_puts(&b, "| ");
            if (r || t->header) put_cell(&b, t->cells[at(t, r, k)]);
            tb_putc(&b, ' ');
        }
        tb_putc(&b, '|');
        if (r == 0) {
            tb_putc(&b, '\n');
            for (uint8_t k = 0; k < t->cols; k++) {
                uint8_t al = t->align[k];
                tb_puts(&b, al == OC_RT_ALIGN_LEFT ? "| :--- " : al == OC_RT_ALIGN_CENTER ? "| :---: "
                          : al == OC_RT_ALIGN_RIGHT ? "| ---: " : "| --- ");
            }
            tb_putc(&b, '|');
        }
    }
    return tb_take(&b);
}

/* `<br>`, `<br/>` or `<br />` at s[i] in any case: its length, else 0 -- the
 * same spellings the scanner reads (oc_rt_scan_cell). */
static size_t md_br(const char *s, size_t i, size_t n) {
    size_t j;
    if (i + 4 > n || s[i] != '<' || (s[i + 1] | 0x20) != 'b' || (s[i + 2] | 0x20) != 'r') return 0;
    j = i + 3;
    if (j < n && s[j] == ' ') j++;
    if (j < n && s[j] == '/') j++;
    return j < n && s[j] == '>' ? j + 1 - i : 0;
}

oc_table *oc_table_from_markdown(const char *body, size_t len, const oc_rt_table *rt) {
    if (!body || !rt || !rt->cols || rt->rows > OC_TABLE_MAX_ROWS) return NULL;
    oc_table *t = oc_table_new(rt->cols, rt->rows);
    if (!t) return NULL;
    t->header = rt->header;
    memcpy(t->align, rt->align, rt->cols);
    oc_rt_rowit it;
    oc_rt_cell cells[OC_RT_TABLE_MAX_COLS];
    uint32_t row;
    oc_rt_rows_begin(&it, body, len, rt);
    while (oc_rt_row_next(&it, cells, &row) && row <= t->rows) {
        for (uint8_t k = 0; k < rt->cols; k++) {
            tb_buf b = { 0 };
            const char *s = body + cells[k].start;
            size_t n = cells[k].len;
            for (size_t i = 0; i < n; i++) {
                size_t L;
                if (s[i] == '\\' && i + 1 < n && s[i + 1] == '|') { tb_putc(&b, '|'); i++; }
                else if ((L = md_br(s, i, n)) != 0) { tb_putc(&b, '\n'); i += L - 1; }
                else tb_putc(&b, s[i]);
            }
            char *v = tb_take(&b);
            if (!v) { oc_table_free(t); return NULL; }
            free(t->cells[at(t, row, k)]);
            t->cells[at(t, row, k)] = v;
        }
    }
    return t;
}

/* ---- from a spreadsheet's text ------------------------------------------ */

typedef struct { char **v; size_t n, cap; } tb_row;
typedef struct { tb_row *r; size_t n, cap; } tb_rows;

static void rows_free(tb_rows *rs) {
    for (size_t i = 0; i < rs->n; i++) {
        for (size_t k = 0; k < rs->r[i].n; k++) free(rs->r[i].v[k]);
        free(rs->r[i].v);
    }
    free(rs->r);
    memset(rs, 0, sizeof *rs);
}

static int row_add(tb_rows *rs) {
    if (rs->n == rs->cap) {
        size_t c = rs->cap ? rs->cap * 2 : 8;
        tb_row *g = realloc(rs->r, c * sizeof *g);
        if (!g) return -1;
        rs->r = g; rs->cap = c;
    }
    memset(&rs->r[rs->n++], 0, sizeof(tb_row));
    return 0;
}

/* Append a cell (taken over) to the last row. */
static int cell_add(tb_rows *rs, char *v) {
    if (!v || !rs->n) { free(v); return -1; }
    tb_row *r = &rs->r[rs->n - 1];
    if (r->n == r->cap) {
        size_t c = r->cap ? r->cap * 2 : 8;
        char **g = realloc(r->v, c * sizeof *g);
        if (!g) { free(v); return -1; }
        r->v = g; r->cap = c;
    }
    r->v[r->n++] = v;
    return 0;
}

/* The rows as a table, the first one its header, ragged rows padded. NULL when
 * there is nothing, or too much. */
static oc_table *rows_to_table(tb_rows *rs) {
    size_t cols = 0;
    for (size_t i = 0; i < rs->n; i++) if (rs->r[i].n > cols) cols = rs->r[i].n;
    if (!rs->n || !cols || cols > OC_TABLE_MAX_COLS || rs->n - 1 > OC_TABLE_MAX_ROWS) return NULL;
    oc_table *t = oc_table_new((uint8_t)cols, (uint32_t)(rs->n - 1));
    if (!t) return NULL;
    for (size_t i = 0; i < rs->n; i++)
        for (size_t k = 0; k < rs->r[i].n; k++) {
            free(t->cells[at(t, (uint32_t)i, (uint8_t)k)]);
            t->cells[at(t, (uint32_t)i, (uint8_t)k)] = rs->r[i].v[k];
            rs->r[i].v[k] = NULL;
        }
    return t;
}

oc_table *oc_table_from_tsv(const char *text, size_t len) {
    if (!text) return NULL;
    /* The newline after the last row is not one more, empty row. */
    while (len && (text[len - 1] == '\n' || text[len - 1] == '\r')) len--;
    if (!len || !memchr(text, '\t', len)) return NULL;
    tb_rows rs = { 0 };
    size_t i = 0;
    if (row_add(&rs) != 0) return NULL;
    while (i <= len) {
        tb_buf b = { 0 };
        if (i < len && text[i] == '"') {
            /* Quoted: "" is a quote, and tabs and newlines are the cell's own. */
            i++;
            while (i < len) {
                if (text[i] == '"') {
                    if (i + 1 < len && text[i + 1] == '"') { tb_putc(&b, '"'); i += 2; continue; }
                    i++;
                    break;
                }
                if (text[i] == '\r' && i + 1 < len && text[i + 1] == '\n') { i++; continue; }
                tb_putc(&b, text[i++]);
            }
        }
        while (i < len && text[i] != '\t' && text[i] != '\n') {
            if (text[i] != '\r') tb_putc(&b, text[i]);
            i++;
        }
        if (cell_add(&rs, tb_take(&b)) != 0) { rows_free(&rs); return NULL; }
        if (i >= len) break;
        if (text[i] == '\n' && row_add(&rs) != 0) { rows_free(&rs); return NULL; }
        i++;
    }
    /* Tabular means a spreadsheet's rectangle: two rows, two columns, every row
     * the same width, and no column empty from top to bottom -- which is what
     * tells it from code indented with tabs. */
    int ok = rs.n >= 2 && rs.r[0].n >= 2;
    for (size_t r = 1; ok && r < rs.n; r++) if (rs.r[r].n != rs.r[0].n) ok = 0;
    for (size_t k = 0; ok && k < rs.r[0].n; k++) {
        int any = 0;
        for (size_t r = 0; r < rs.n && !any; r++) if (rs.r[r].v[k][0]) any = 1;
        if (!any) ok = 0;
    }
    oc_table *t = ok ? rows_to_table(&rs) : NULL;
    rows_free(&rs);
    return t;
}

/* ---- from a clipboard's HTML -------------------------------------------- */

/* The value of attribute `name` in the tag text [s, e), heap; NULL if absent. */
static char *attr(const char *s, const char *e, const char *name) {
    size_t nl = strlen(name);
    for (const char *p = s; p + nl < e; p++) {
        if (strncasecmp(p, name, nl) != 0 || (p > s && !isspace((unsigned char)p[-1]))) continue;
        const char *q = p + nl;
        while (q < e && isspace((unsigned char)*q)) q++;
        if (q >= e || *q != '=') continue;
        q++;
        while (q < e && isspace((unsigned char)*q)) q++;
        char quote = (q < e && (*q == '"' || *q == '\'')) ? *q++ : 0;
        const char *v = q;
        while (q < e && (quote ? *q != quote : !isspace((unsigned char)*q) && *q != '>')) q++;
        char *out = malloc((size_t)(q - v) + 1);
        if (!out) return NULL;
        memcpy(out, v, (size_t)(q - v));
        out[q - v] = '\0';
        return out;
    }
    return NULL;
}

/* Does a style attribute make text bold, or italic? */
static int style_has(const char *style, int italic) {
    if (!style) return 0;
    char low[512];
    size_t n = 0;
    for (const char *p = style; *p && n + 1 < sizeof low; p++) if (!isspace((unsigned char)*p)) low[n++] = (char)tolower((unsigned char)*p);
    low[n] = '\0';
    if (italic) return strstr(low, "font-style:italic") != NULL;
    const char *w = strstr(low, "font-weight:");
    if (!w) return 0;
    w += 12;
    return !strncmp(w, "bold", 4) || !strncmp(w, "bolder", 6) || (isdigit((unsigned char)*w) && atoi(w) >= 600);
}

/* One HTML entity at s (after the '&'), decoded into `b`: how many bytes it
 * took, or 0 if it is not one -- then the '&' is literal. */
static size_t entity(const char *s, const char *e, tb_buf *b) {
    static const struct { const char *n; const char *v; } E[] = {
        { "amp;", "&" }, { "lt;", "<" }, { "gt;", ">" }, { "quot;", "\"" }, { "apos;", "'" },
        { "nbsp;", " " },
    };
    for (size_t i = 0; i < sizeof E / sizeof E[0]; i++) {
        size_t n = strlen(E[i].n);
        if ((size_t)(e - s) >= n && !strncmp(s, E[i].n, n)) { tb_puts(b, E[i].v); return n; }
    }
    if (s < e && *s == '#') {
        const char *p = s + 1;
        unsigned long cp = 0;
        int hex = p < e && (*p == 'x' || *p == 'X');
        if (hex) p++;
        const char *d = p;
        while (p < e && (hex ? isxdigit((unsigned char)*p) : isdigit((unsigned char)*p))) {
            cp = cp * (hex ? 16 : 10) + (unsigned long)(isdigit((unsigned char)*p) ? *p - '0' : (tolower((unsigned char)*p) - 'a' + 10));
            if (cp > 0x10FFFF) return 0;
            p++;
        }
        if (p == d || p >= e || *p != ';' || cp == 0) return 0;
        char u[4];
        size_t n;
        if (cp < 0x80) { u[0] = (char)cp; n = 1; }
        else if (cp < 0x800) { u[0] = (char)(0xC0 | (cp >> 6)); u[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
        else if (cp < 0x10000) { u[0] = (char)(0xE0 | (cp >> 12)); u[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); u[2] = (char)(0x80 | (cp & 0x3F)); n = 3; }
        else { u[0] = (char)(0xF0 | (cp >> 18)); u[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); u[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); u[3] = (char)(0x80 | (cp & 0x3F)); n = 4; }
        tb_put(b, u, n);
        return (size_t)(p + 1 - s);
    }
    return 0;
}

/* Wrap what was written to `b` since `from` in `open`..`close`, around its
 * trimmed content so the delimiters touch the text (MARKDOWN.md §2). Nothing
 * when it is only whitespace. */
static void wrap(tb_buf *b, size_t from, const char *open, const char *close) {
    if (b->bad || from > b->n) return;
    size_t a = from, z = b->n;
    while (a < z && b->p[a] == ' ') a++;
    while (z > a && b->p[z - 1] == ' ') z--;
    if (a == z) return;
    size_t tail = b->n - z, mid = z - a;
    char *seg = malloc(mid + tail + 1);
    if (!seg) { b->bad = 1; return; }
    memcpy(seg, b->p + a, mid + tail);
    b->n = a;
    tb_puts(b, open);
    tb_put(b, seg, mid);
    tb_puts(b, close);
    tb_put(b, seg + mid, tail);
    free(seg);
}

/* An open inline element: where its text began, and how it closes. */
typedef struct { int kind; size_t from; char *href; } tb_open;
enum { TO_NONE, TO_BOLD, TO_ITALIC, TO_LINK, TO_SPAN };

oc_table *oc_table_from_html(const char *html, size_t len) {
    if (!html) return NULL;
    const char *e = html + len, *p = html;
    /* The first table, opening tag and all. */
    for (; p + 6 <= e; p++) if (*p == '<' && !strncasecmp(p, "<table", 6)) break;
    if (p + 6 > e) return NULL;
    tb_rows rs = { 0 };
    tb_buf cell = { 0 };
    int in_cell = 0, cell_bold = 0, cell_italic = 0, depth = 0, done = 0;
    tb_open st[32];
    int nst = 0;
    while (p < e && !done) {
        if (*p == '<') {
            const char *q = p + 1;
            if (q + 3 <= e && !strncmp(q, "!--", 3)) {           /* a comment */
                const char *c = q + 3;
                while (c + 3 <= e && strncmp(c, "-->", 3)) c++;
                p = c + 3 <= e ? c + 3 : e;
                continue;
            }
            int closing = q < e && *q == '/';
            if (closing) q++;
            const char *ns = q;
            while (q < e && (isalnum((unsigned char)*q))) q++;
            size_t nl = (size_t)(q - ns);
            const char *te = q;
            while (te < e && *te != '>') te++;
            char name[12] = "";
            if (nl < sizeof name) { for (size_t i = 0; i < nl; i++) name[i] = (char)tolower((unsigned char)ns[i]); name[nl] = '\0'; }
            if (!strcmp(name, "style") || !strcmp(name, "script")) {
                if (!closing) {                                     /* skip to its end */
                    const char *c = te;
                    while (c + nl + 2 < e && !(c[0] == '<' && c[1] == '/' && !strncasecmp(c + 2, name, nl))) c++;
                    te = c;
                    while (te < e && *te != '>') te++;
                }
            } else if (!strcmp(name, "table")) {
                if (closing) { if (--depth == 0) done = 1; } else depth++;
            } else if (depth == 1 && !strcmp(name, "tr")) {
                if (!closing && row_add(&rs) != 0) { rows_free(&rs); free(cell.p); return NULL; }
            } else if (depth == 1 && (!strcmp(name, "td") || !strcmp(name, "th"))) {
                if (!closing) {
                    if (!rs.n && row_add(&rs) != 0) { rows_free(&rs); free(cell.p); return NULL; }
                    free(cell.p); memset(&cell, 0, sizeof cell);
                    char *style = attr(ns, te, "style");
                    cell_bold = style_has(style, 0);
                    cell_italic = style_has(style, 1);
                    free(style);
                    char *span = attr(ns, te, "colspan");
                    int extra = span ? atoi(span) - 1 : 0;
                    free(span);
                    for (int k = 0; k < extra && k < OC_TABLE_MAX_COLS; k++)
                        if (cell_add(&rs, calloc(1, 1)) != 0) { rows_free(&rs); return NULL; }
                    in_cell = 1;
                    nst = 0;
                } else if (in_cell) {
                    while (nst > 0) free(st[--nst].href);
                    /* The cell's own text, its spaces trimmed, styled whole. */
                    size_t a = 0, z = cell.n;
                    while (a < z && cell.p[a] == ' ') a++;
                    while (z > a && (cell.p[z - 1] == ' ' || cell.p[z - 1] == '\n')) z--;
                    tb_buf v = { 0 };
                    if (z > a) tb_put(&v, cell.p + a, z - a);
                    if (cell_italic) wrap(&v, 0, "_", "_");
                    if (cell_bold) wrap(&v, 0, "*", "*");
                    if (cell_add(&rs, tb_take(&v)) != 0) { rows_free(&rs); free(cell.p); return NULL; }
                    free(cell.p); memset(&cell, 0, sizeof cell);
                    in_cell = 0;
                }
            } else if (in_cell && !strcmp(name, "br")) {
                tb_putc(&cell, '\n');
            } else if (in_cell && nst < 32 && !closing &&
                       (!strcmp(name, "b") || !strcmp(name, "strong") || !strcmp(name, "i") ||
                        !strcmp(name, "em") || !strcmp(name, "a") || !strcmp(name, "span"))) {
                tb_open *o = &st[nst++];
                o->from = cell.n;
                o->href = NULL;
                if (name[0] == 'a') { o->kind = TO_LINK; o->href = attr(ns, te, "href"); }
                else if (!strcmp(name, "b") || !strcmp(name, "strong")) o->kind = TO_BOLD;
                else if (!strcmp(name, "i") || !strcmp(name, "em")) o->kind = TO_ITALIC;
                else {
                    char *style = attr(ns, te, "style");
                    o->kind = style_has(style, 0) ? TO_BOLD : style_has(style, 1) ? TO_ITALIC : TO_SPAN;
                    free(style);
                }
            } else if (in_cell && closing && nst > 0 &&
                       (!strcmp(name, "b") || !strcmp(name, "strong") || !strcmp(name, "i") ||
                        !strcmp(name, "em") || !strcmp(name, "a") || !strcmp(name, "span"))) {
                tb_open *o = &st[--nst];
                if (o->kind == TO_BOLD) wrap(&cell, o->from, "*", "*");
                else if (o->kind == TO_ITALIC) wrap(&cell, o->from, "_", "_");
                else if (o->kind == TO_LINK && o->href &&
                         (!strncasecmp(o->href, "https://", 8) || !strncasecmp(o->href, "http://", 7))) {
                    /* A label the dialect can carry: one line, no brackets. */
                    int ok = cell.n > o->from;
                    for (size_t i = o->from; ok && i < cell.n; i++)
                        if (cell.p[i] == '[' || cell.p[i] == ']' || cell.p[i] == '\n') ok = 0;
                    if (ok) {
                        tb_buf close = { 0 };
                        tb_puts(&close, "](");
                        tb_puts(&close, o->href);
                        tb_putc(&close, ')');
                        if (!close.bad) wrap(&cell, o->from, "[", close.p);
                        free(close.p);
                    }
                }
                free(o->href);
            }
            p = te < e ? te + 1 : e;
            continue;
        }
        if (in_cell) {
            if (*p == '&') {
                size_t n = entity(p + 1, e, &cell);
                if (n) { p += 1 + n; continue; }
            }
            /* HTML whitespace: a run is one space. */
            if (isspace((unsigned char)*p)) {
                if (cell.n && cell.p[cell.n - 1] != ' ' && cell.p[cell.n - 1] != '\n') tb_putc(&cell, ' ');
            } else {
                tb_putc(&cell, *p);
            }
        }
        p++;
    }
    while (nst > 0) free(st[--nst].href);
    free(cell.p);
    /* Drop rows with no cells (a <tr> that held nothing). */
    size_t w = 0;
    for (size_t r = 0; r < rs.n; r++) {
        if (rs.r[r].n) rs.r[w++] = rs.r[r];
        else free(rs.r[r].v);
    }
    rs.n = w;
    oc_table *t = rows_to_table(&rs);
    rows_free(&rs);
    return t;
}
