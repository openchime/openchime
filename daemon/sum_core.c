/* Channel and DM summaries: the engine-free core (sum_core.h). */
#define _POSIX_C_SOURCE 200809L
#include "sum_core.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "json.h"

/* --- the growing string ------------------------------------------------------ */

static int buf_grow(oc_sum_buf *b, size_t more) {
    if (b->oom) return -1;
    if (b->n + more + 1 <= b->cap) return 0;
    size_t nc = b->cap ? b->cap : 256;
    while (nc < b->n + more + 1) nc *= 2;
    char *np = realloc(b->p, nc);
    if (!np) { b->oom = 1; return -1; }
    b->p = np;
    b->cap = nc;
    return 0;
}

void oc_sum_buf_add(oc_sum_buf *b, const char *s, size_t n) {
    if (buf_grow(b, n) != 0) return;
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = '\0';
}

void oc_sum_buf_puts(oc_sum_buf *b, const char *s) { oc_sum_buf_add(b, s, strlen(s)); }

void oc_sum_buf_printf(oc_sum_buf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char small[256];
    int n = vsnprintf(small, sizeof small, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof small) { oc_sum_buf_add(b, small, (size_t)n); return; }
    if (buf_grow(b, (size_t)n) != 0) return;
    va_start(ap, fmt);
    vsnprintf(b->p + b->n, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b->n += (size_t)n;
}

void oc_sum_buf_json(oc_sum_buf *b, const char *s) {
    oc_sum_buf_add(b, "\"", 1);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"':  oc_sum_buf_add(b, "\\\"", 2); break;
        case '\\': oc_sum_buf_add(b, "\\\\", 2); break;
        case '\n': oc_sum_buf_add(b, "\\n", 2); break;
        case '\r': oc_sum_buf_add(b, "\\r", 2); break;
        case '\t': oc_sum_buf_add(b, "\\t", 2); break;
        default:
            if (*p < 0x20) oc_sum_buf_printf(b, "\\u%04x", *p);
            else oc_sum_buf_add(b, (const char *)p, 1);
        }
    }
    oc_sum_buf_add(b, "\"", 1);
}

void oc_sum_buf_free(oc_sum_buf *b) { free(b->p); memset(b, 0, sizeof *b); }

/* --- cutting a channel into pieces ------------------------------------------- */

size_t oc_sum_msg_tokens(const oc_sum_msg *m) {
    size_t bytes = 24 + (m->author ? strlen(m->author) : 0) + (m->text ? strlen(m->text) : 0);
    return bytes / SUM_BYTES_PER_TOKEN + 1;
}

typedef struct {
    int64_t root;
    int    *idx;
    int     n, cap;
    int64_t end_ms;
    size_t  tokens;
} unit;

static int cmp_unit(const void *a, const void *b) {
    const unit *x = a, *y = b;
    if (x->end_ms != y->end_ms) return x->end_ms < y->end_ms ? -1 : 1;
    return x->root < y->root ? -1 : x->root > y->root;
}

static int add_idx(int **arr, int *n, int *cap, int v) {
    if (*n == *cap) {
        int nc = *cap ? *cap * 2 : 8;
        int *na = realloc(*arr, (size_t)nc * sizeof *na);
        if (!na) return -1;
        *arr = na;
        *cap = nc;
    }
    (*arr)[(*n)++] = v;
    return 0;
}

static int push_piece(oc_sum_cut *c, int *cap, oc_sum_piece p) {
    if (c->n == *cap) {
        int nc = *cap ? *cap * 2 : 16;
        oc_sum_piece *np = realloc(c->pieces, (size_t)nc * sizeof *np);
        if (!np) return -1;
        c->pieces = np;
        *cap = nc;
    }
    c->pieces[c->n++] = p;
    return 0;
}

static int chunk_from(const int *idx, int n, int64_t root, int64_t end_ms, oc_sum_chunk *out) {
    out->idx = malloc((size_t)(n ? n : 1) * sizeof *out->idx);
    if (!out->idx) return -1;
    memcpy(out->idx, idx, (size_t)n * sizeof *idx);
    out->n = n;
    out->root_id = root;
    out->end_ms = end_ms;
    return 0;
}

int oc_sum_cut_build(const oc_sum_msg *msgs, int n, size_t threshold, uint64_t gap_ms, oc_sum_cut *out) {
    memset(out, 0, sizeof *out);
    unit *units = NULL;
    int nu = 0, cu = 0, rc = -1;
    int pcap = 0;
    int *cur = NULL, ncur = 0, ccur = 0;
    size_t cur_tokens = 0;
    int64_t cur_end = 0, cur_root = 0;
    int cur_units = 0;
    /* Group by thread root, in message order. The roots are few per channel
     * window, so a linear search for each message's unit is fine here. */
    for (int i = 0; i < n; i++) {
        int64_t root = msgs[i].parent_id ? msgs[i].parent_id : msgs[i].id;
        int u = -1;
        for (int k = nu - 1; k >= 0; k--) if (units[k].root == root) { u = k; break; }
        if (u < 0) {
            if (nu == cu) {
                int nc = cu ? cu * 2 : 32;
                unit *nn = realloc(units, (size_t)nc * sizeof *nn);
                if (!nn) goto done;
                units = nn;
                cu = nc;
            }
            memset(&units[nu], 0, sizeof units[nu]);
            units[nu].root = root;
            u = nu++;
        }
        if (add_idx(&units[u].idx, &units[u].n, &units[u].cap, i) != 0) goto done;
        if (msgs[i].created_ms > units[u].end_ms) units[u].end_ms = msgs[i].created_ms;
        units[u].tokens += oc_sum_msg_tokens(&msgs[i]);
    }
    /* A thread is placed at its last activity. */
    qsort(units, (size_t)nu, sizeof *units, cmp_unit);

#define FLUSH()                                                                     \
    do {                                                                            \
        if (ncur) {                                                                 \
            oc_sum_piece p; memset(&p, 0, sizeof p);                                \
            p.chunks = malloc(sizeof *p.chunks);                                    \
            if (!p.chunks) goto done;                                               \
            p.n_chunks = 1;                                                         \
            p.root_id = cur_units == 1 ? cur_root : 0;                              \
            p.end_ms = cur_end;                                                     \
            if (chunk_from(cur, ncur, p.root_id, cur_end, &p.chunks[0]) != 0) { free(p.chunks); goto done; } \
            if (push_piece(out, &pcap, p) != 0) { free(p.chunks[0].idx); free(p.chunks); goto done; } \
            ncur = 0; cur_tokens = 0; cur_units = 0; cur_root = 0;                  \
        }                                                                           \
    } while (0)

    for (int u = 0; u < nu; u++) {
        unit *x = &units[u];
        int is_thread = x->n > 1;
        if (is_thread && x->tokens > threshold) {
            /* Too big for one piece: its own chunks of whole messages. */
            FLUSH();
            oc_sum_piece p;
            memset(&p, 0, sizeof p);
            p.is_big_thread = 1;
            p.root_id = x->root;
            p.end_ms = x->end_ms;
            int ccap = 0, s = 0;
            while (s < x->n) {
                size_t t = 0;
                int e = s;
                while (e < x->n && (e == s || t + oc_sum_msg_tokens(&msgs[x->idx[e]]) <= threshold))
                    t += oc_sum_msg_tokens(&msgs[x->idx[e++]]);
                if (p.n_chunks == ccap) {
                    int nc = ccap ? ccap * 2 : 4;
                    oc_sum_chunk *nch = realloc(p.chunks, (size_t)nc * sizeof *nch);
                    if (!nch) goto big_fail;
                    p.chunks = nch;
                    ccap = nc;
                }
                int64_t last = 0;
                for (int k = s; k < e; k++) if (msgs[x->idx[k]].created_ms > last) last = msgs[x->idx[k]].created_ms;
                if (chunk_from(x->idx + s, e - s, x->root, last, &p.chunks[p.n_chunks]) != 0) goto big_fail;
                p.n_chunks++;
                s = e;
            }
            if (push_piece(out, &pcap, p) != 0) goto big_fail;
            continue;
        big_fail:
            for (int k = 0; k < p.n_chunks; k++) free(p.chunks[k].idx);
            free(p.chunks);
            goto done;
        }
        if (ncur && ((uint64_t)(x->end_ms - cur_end) > gap_ms || cur_tokens + x->tokens > threshold))
            FLUSH();
        for (int k = 0; k < x->n; k++)
            if (add_idx(&cur, &ncur, &ccur, x->idx[k]) != 0) goto done;
        cur_tokens += x->tokens;
        cur_end = x->end_ms;
        cur_root = is_thread ? x->root : 0;
        cur_units++;
    }
    FLUSH();
#undef FLUSH
    rc = 0;
done:
    for (int u = 0; u < nu; u++) free(units[u].idx);
    free(units);
    free(cur);
    if (rc != 0) oc_sum_cut_free(out);
    return rc;
}

void oc_sum_cut_free(oc_sum_cut *c) {
    for (int i = 0; i < c->n; i++) {
        for (int k = 0; k < c->pieces[i].n_chunks; k++) free(c->pieces[i].chunks[k].idx);
        free(c->pieces[i].chunks);
    }
    free(c->pieces);
    memset(c, 0, sizeof *c);
}

/* --- lines ----------------------------------------------------------------- */

static int64_t *ids_dup(const int64_t *v, int n) {
    int64_t *o = malloc((size_t)(n > 0 ? n : 1) * sizeof *o);
    if (o && n > 0) memcpy(o, v, (size_t)n * sizeof *v);
    return o;
}

int oc_sum_lines_add_full(oc_sum_lines *l, const char *text, int indent, const char *label,
                          const int64_t *refs, int n_refs, const int64_t *all, int n_all,
                          const char *by, char kind) {
    if (l->n == l->cap) {
        int nc = l->cap ? l->cap * 2 : 32;
        oc_sum_line *nv = realloc(l->v, (size_t)nc * sizeof *nv);
        if (!nv) return -1;
        l->v = nv;
        l->cap = nc;
    }
    oc_sum_line *x = &l->v[l->n];
    memset(x, 0, sizeof *x);
    x->text = strdup(text ? text : "");
    x->label = label ? strdup(label) : NULL;
    x->by = strdup(by ? by : "");
    x->refs = ids_dup(refs, n_refs);
    x->all = ids_dup(all, n_all);
    if (!x->text || (label && !x->label) || !x->by || !x->refs || !x->all) {
        free(x->text); free(x->label); free(x->by); free(x->refs); free(x->all);
        return -1;
    }
    x->n_refs = n_refs > 0 ? n_refs : 0;
    x->n_all = n_all > 0 ? n_all : 0;
    x->indent = indent;
    x->kind = kind;
    l->n++;
    return 0;
}

int oc_sum_lines_add(oc_sum_lines *l, const char *text, int indent, const char *label,
                     const int64_t *refs, int n_refs) {
    return oc_sum_lines_add_full(l, text, indent, label, refs, n_refs, refs, n_refs, NULL, 0);
}

void oc_sum_lines_free(oc_sum_lines *l) {
    for (int i = 0; i < l->n; i++) {
        free(l->v[i].text); free(l->v[i].label); free(l->v[i].refs); free(l->v[i].all); free(l->v[i].by);
    }
    free(l->v);
    memset(l, 0, sizeof *l);
}

size_t oc_sum_lines_tokens(const oc_sum_lines *l) {
    size_t bytes = 0;
    for (int i = 0; i < l->n; i++)
        bytes += 8 + strlen(l->v[i].text) + (l->v[i].label ? strlen(l->v[i].label) + 2 : 0);
    return bytes / SUM_BYTES_PER_TOKEN + 1;
}

size_t oc_sum_words(const char *s) {
    size_t n = 0;
    int in = 0;
    for (; s && *s; s++) {
        int sp = *s == ' ' || *s == '\n' || *s == '\t' || *s == '\r';
        if (!sp && !in) n++;
        in = !sp;
    }
    return n;
}

size_t oc_sum_lines_words(const oc_sum_lines *l) {
    size_t w = 0;
    for (int i = 0; i < l->n; i++) w += oc_sum_words(l->v[i].text);
    return w;
}

/* `s` as a repeat key: lower case, every digit one '#', runs of space one. Two
 * messages with the same key say the same thing but for their numbers. */
static void same_key(const char *s, oc_sum_buf *k) {
    k->n = 0;
    int sp = 0, dig = 0;
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (isspace(c)) { if (!sp && k->n) oc_sum_buf_add(k, " ", 1); sp = 1; dig = 0; continue; }
        sp = 0;
        if (isdigit(c)) { if (!dig) oc_sum_buf_add(k, "#", 1); dig = 1; continue; }
        dig = 0;
        char lc = (char)tolower(c);
        oc_sum_buf_add(k, &lc, 1);
    }
}

/* Add `name` to the newline-separated list `by`, once. */
static void by_add(oc_sum_buf *by, const char *name) {
    if (!name || !*name) return;
    size_t n = strlen(name);
    for (const char *p = by->p; p && *p; ) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len == n && !strncmp(p, name, n)) return;
        p = e ? e + 1 : p + len;
    }
    if (by->n) oc_sum_buf_add(by, "\n", 1);
    oc_sum_buf_add(by, name, n);
}

int oc_sum_chunk_lines(const oc_sum_msg *msgs, const oc_sum_chunk *c, oc_sum_lines *l) {
    oc_sum_buf t = {0}, k0 = {0}, k1 = {0}, by = {0};
    int rc = -1;
    int64_t *run = malloc((size_t)(c->n ? c->n : 1) * sizeof *run);
    if (!run) return -1;
    for (int k = 0; k < c->n; ) {
        const oc_sum_msg *m = &msgs[c->idx[k]];
        /* A run of messages saying the same but for their numbers, whoever
         * wrote them, is one line with how many (SUMMARIES.md §2). */
        same_key(m->text, &k0);
        int nr = 0, e = k;
        by.n = 0;
        if (by.p) by.p[0] = '\0';
        while (e < c->n) {
            const oc_sum_msg *x = &msgs[c->idx[e]];
            if (e > k) {
                same_key(x->text, &k1);
                if ((x->parent_id != 0) != (m->parent_id != 0) || k0.oom || k1.oom || strcmp(k0.p ? k0.p : "", k1.p ? k1.p : ""))
                    break;
            }
            run[nr++] = x->id;
            by_add(&by, x->author && *x->author ? x->author : "someone");
            e++;
        }
        const char *who = m->author && *m->author ? m->author : "someone";
        t.n = 0;
        oc_sum_buf_printf(&t, "%s: ", who);
        oc_sum_buf_puts(&t, m->text ? m->text : "");
        if (nr > 1) oc_sum_buf_printf(&t, " (x%d)", nr);
        if (t.oom || by.oom ||
            oc_sum_lines_add_full(l, t.p, m->parent_id != 0, NULL, run, nr < SUM_CITES_MAX ? nr : SUM_CITES_MAX,
                                  run, nr, by.p ? by.p : "", 0) != 0)
            goto out;
        k = e;
    }
    rc = 0;
out:
    free(run);
    oc_sum_buf_free(&t);
    oc_sum_buf_free(&k0);
    oc_sum_buf_free(&k1);
    oc_sum_buf_free(&by);
    return rc;
}

/* --- splitting one message too big for a chunk ------------------------------------ */

typedef struct { char **v; int n, cap; } parts;

static int part_add(parts *p, const char *s, size_t n) {
    if (p->n == p->cap) {
        int nc = p->cap ? p->cap * 2 : 8;
        char **nv = realloc(p->v, (size_t)nc * sizeof *nv);
        if (!nv) return -1;
        p->v = nv;
        p->cap = nc;
    }
    char *x = malloc(n + 1);
    if (!x) return -1;
    memcpy(x, s, n);
    x[n] = '\0';
    p->v[p->n++] = x;
    return 0;
}

/* Paragraphs, lines, sentences, words: the largest unit that fits wins. */
static const char *const SEPS[] = { "\n\n", "\n", ". ", " " };
#define N_SEPS ((int)(sizeof SEPS / sizeof *SEPS))

static int split_at(const char *s, size_t n, size_t max, int level, parts *out) {
    if (n <= max) return part_add(out, s, n);
    if (level == N_SEPS) {
        /* No separator left: cut at `max`, backed off to a character's start. */
        size_t at = max;
        while (at > 1 && ((unsigned char)s[at] & 0xC0) == 0x80) at--;
        if (part_add(out, s, at) != 0) return -1;
        return split_at(s + at, n - at, max, level, out);
    }
    const char *sep = SEPS[level];
    size_t sl = strlen(sep);
    /* Segments end just after each separator; adjacent ones are joined while
     * they fit, and one that does not fit alone is split at the next level. */
    size_t start = 0, run = 0;
    while (start + run < n) {
        size_t e = start + run, cut = n;
        for (size_t k = e; k + sl <= n; k++)
            if (!memcmp(s + k, sep, sl)) { cut = k + sl; break; }
        if (cut - start <= max) { run = cut - start; continue; }
        if (run) {
            if (part_add(out, s + start, run) != 0) return -1;
            start += run;
            run = 0;
            continue;
        }
        if (split_at(s + start, cut - start, max, level + 1, out) != 0) return -1;
        start = cut;
    }
    if (run) return part_add(out, s + start, run);
    return 0;
}

int oc_sum_split_text(const char *text, size_t max_tokens, char ***out, int *n) {
    parts p = {0};
    size_t max = max_tokens * SUM_BYTES_PER_TOKEN;
    if (max < 16) max = 16;
    if (split_at(text ? text : "", text ? strlen(text) : 0, max, 0, &p) != 0) {
        oc_sum_parts_free(p.v, p.n);
        return -1;
    }
    *out = p.v;
    *n = p.n;
    return 0;
}

void oc_sum_parts_free(char **v, int n) {
    for (int i = 0; i < n; i++) free(v[i]);
    free(v);
}

/* --- reading what the model wrote ------------------------------------------------ */

/* The string at token `i`, unescaped, on the heap; NULL if not a string. */
static char *json_dup(const oc_json *d, int i) {
    if (i < 0 || d->t[i].type != JSMN_STRING) return NULL;
    size_t cap = (size_t)(d->t[i].end - d->t[i].start) + 1;
    char *s = malloc(cap);
    if (s && oc_json_str(d, i, s, cap) != 0) { free(s); s = NULL; }
    return s;
}

/* The integers of the array at token `arr`, on the heap (*out), and how many. */
static int json_ids(const oc_json *d, int arr, int64_t **out) {
    *out = NULL;
    if (arr < 0 || d->t[arr].type != JSMN_ARRAY) return 0;
    int64_t *v = malloc((size_t)(d->t[arr].size ? d->t[arr].size : 1) * sizeof *v);
    if (!v) return -1;
    int n = 0, i = arr + 1;
    for (int k = 0; k < d->t[arr].size; k++, i = oc_json_skip(d, i)) {
        uint64_t x;
        if (oc_json_u64(d, i, &x) == 0) v[n++] = (int64_t)x;
    }
    *out = v;
    return n;
}

typedef struct { int64_t *v; int n, cap; } idset;

static int idset_add(idset *s, const int64_t *add, int na) {
    for (int a = 0; a < na; a++) {
        int dup = 0;
        for (int k = 0; k < s->n && !dup; k++) dup = s->v[k] == add[a];
        if (dup) continue;
        if (s->n == s->cap) {
            int nc = s->cap ? s->cap * 2 : 16;
            int64_t *nv = realloc(s->v, (size_t)nc * sizeof *nv);
            if (!nv) return -1;
            s->v = nv;
            s->cap = nc;
        }
        s->v[s->n++] = add[a];
    }
    return 0;
}

typedef struct { int *v; int n, cap; } ints;

static int ints_add(ints *a, int v) {
    for (int i = 0; i < a->n; i++) if (a->v[i] == v) return 0;
    if (a->n == a->cap) {
        int nc = a->cap ? a->cap * 2 : 8;
        int *nv = realloc(a->v, (size_t)nc * sizeof *nv);
        if (!nv) return -1;
        a->v = nv;
        a->cap = nc;
    }
    a->v[a->n++] = v;
    return 0;
}

/* Take the citations out of `s`, in place, wherever they are: "[3]", "[3][7]",
 * "[3, 7]", "[1-4]". Into `cited`: 0-based lines, once each, in the order
 * written. A bracketed number that is not a line is taken out and ignored. 0,
 * or -1 when out of memory. */
/* Record the lines `a` to `z` cite. */
static int cite_range(long a, long z, const oc_sum_lines *l, ints *cited) {
    for (long v = a; v <= z && v - a < 1000; v++)
        if (v >= 1 && v <= l->n && ints_add(cited, (int)v - 1) != 0) return -1;
    return 0;
}

/* Citations written bare at the start of `s` (after a bullet mark): "12",
 * "12-15", "12, 15", with a colon after, as a model not held to the brackets
 * writes them. They are taken out; the text after them is moved to the front.
 * A line that opens with a number and no brackets is far likelier a citation
 * than a sentence, since every line is told to open with one. */
static int take_bare_cites(char *s, const oc_sum_lines *l, ints *cited) {
    char *p = s;
    while (*p == ' ') p++;
    if (*p == '-' || *p == '*') { p++; while (*p == ' ') p++; }
    if (!isdigit((unsigned char)*p)) return 0;
    ints c = {0};
    char *q = p;
    for (;;) {
        long a = strtol(q, &q, 10), z = a;
        /* "12-15", "12 - 15", or with an en or em dash */
        char *r = q;
        while (*r == ' ') r++;
        if (*r == '-' || !strncmp(r, "\xe2\x80\x93", 3) || !strncmp(r, "\xe2\x80\x94", 3)) {
            r += *r == '-' ? 1 : 3;
            while (*r == ' ') r++;
            if (isdigit((unsigned char)*r)) z = strtol(r, &q, 10);
        }
        if (cite_range(a, z, l, &c) != 0) { free(c.v); return -1; }
        /* "12, 15" */
        r = q;
        if (*r == ',') {
            r++;
            while (*r == ' ') r++;
            if (isdigit((unsigned char)*r)) { q = r; continue; }
        }
        break;
    }
    if (*q == ':') q++;
    if (*q && *q != ' ') { free(c.v); return 0; }   /* "12th", "3pm": not a citation */
    while (*q == ' ') q++;
    for (int i = 0; i < c.n; i++)
        if (ints_add(cited, c.v[i]) != 0) { free(c.v); return -1; }
    free(c.v);
    memmove(p, q, strlen(q) + 1);
    return 0;
}

static int take_cites(char *s, const oc_sum_lines *l, ints *cited) {
    if (take_bare_cites(s, l, cited) != 0) return -1;
    char *w = s;
    for (char *p = s; *p; ) {
        if (*p == '[') {
            char *e = p + 1;
            int digits = 0;
            while (*e && (isdigit((unsigned char)*e) || *e == ' ' || *e == ',' || *e == '-')) {
                if (isdigit((unsigned char)*e)) digits = 1;
                e++;
            }
            if (digits && *e == ']') {
                for (char *q = p + 1; q < e; ) {
                    if (!isdigit((unsigned char)*q)) { q++; continue; }
                    long a = strtol(q, &q, 10), z = a;
                    while (*q == ' ') q++;
                    if (*q == '-') {
                        q++;
                        while (*q == ' ') q++;
                        if (isdigit((unsigned char)*q)) z = strtol(q, &q, 10);
                    }
                    if (cite_range(a, z, l, cited) != 0) return -1;
                }
                p = e + 1;
                continue;
            }
        }
        *w++ = *p++;
    }
    *w = '\0';
    return 0;
}

/* `s` with runs of spaces made one, no space before punctuation (where a
 * citation was taken out), and bullet marks and stray punctuation taken off
 * either end. */
static char *tidy(char *s) {
    char *w = s;
    int sp = 0;
    for (char *p = s; *p; p++) {
        if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
            if (!sp) *w++ = ' ';
            sp = 1;
        } else {
            if (sp && strchr(".,;:!?)", *p) && w > s) w--;
            *w++ = *p;
            sp = 0;
        }
    }
    *w = '\0';
    for (;;) {
        while (*s == ' ') s++;
        if ((*s == '-' || *s == '*' || *s == '#') && !isdigit((unsigned char)s[1])) { s++; continue; }
        if (!strncmp(s, "\xe2\x80\xa2", 3)) { s += 3; continue; }
        /* List numbering: "3. " or "3) ". */
        const char *d = s;
        while (isdigit((unsigned char)*d)) d++;
        if (d > s && (*d == '.' || *d == ')') && d[1] == ' ') { s = (char *)d + 2; continue; }
        break;
    }
    size_t n = strlen(s);
    while (n && strchr(" -*#,;:|", s[n - 1])) s[--n] = '\0';
    return s;
}

/* Every run of digits in `text` occurs in one of the lines cited. */
static int numbers_ok(const char *text, const oc_sum_lines *l, const int *cited, int nc) {
    for (const char *p = text; *p; ) {
        if (*p < '0' || *p > '9') { p++; continue; }
        const char *s = p;
        while (*p >= '0' && *p <= '9') p++;
        size_t len = (size_t)(p - s);
        int found = 0;
        for (int k = 0; k < nc && !found; k++)
            for (const char *q = l->v[cited[k]].text; *q && !found; q++)
                if (!strncmp(q, s, len)) found = 1;
        if (!found) return 0;
    }
    return 1;
}

/* `name` stands in `text` as a whole name, not inside a word. */
static int has_name(const char *text, const char *name, size_t n) {
    for (const char *p = text; (p = strstr(p, name)) != NULL; p++) {
        int before = p == text || !isalnum((unsigned char)p[-1]);
        int after = !isalnum((unsigned char)p[n]);
        if (before && after) return 1;
    }
    return 0;
}

/* Everyone the lines name as an author, as one newline-separated list. */
static void all_names(const oc_sum_lines *l, oc_sum_buf *names) {
    for (int i = 0; i < l->n; i++)
        for (const char *p = l->v[i].by; p && *p; ) {
            const char *e = strchr(p, '\n');
            size_t len = e ? (size_t)(e - p) : strlen(p);
            char nm[256];
            snprintf(nm, sizeof nm, "%.*s", (int)(len < 255 ? len : 255), p);
            by_add(names, nm);
            p = e ? e + 1 : p + len;
        }
}

/* Every person `text` names is the author of, or named in, a line it cites
 * (SUMMARIES.md §2: the commonest error in dialogue summaries is the wrong
 * person). */
static int people_ok(const char *text, const char *names, const oc_sum_lines *l, const int *cited, int nc) {
    for (const char *p = names; p && *p; ) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        char nm[256];
        snprintf(nm, sizeof nm, "%.*s", (int)(len < 255 ? len : 255), p);
        p = e ? e + 1 : p + len;
        if (strlen(nm) < 2 || !has_name(text, nm, strlen(nm))) continue;
        int found = 0;
        for (int k = 0; k < nc && !found; k++)
            found = has_name(l->v[cited[k]].text, nm, strlen(nm)) || has_name(l->v[cited[k]].by, nm, strlen(nm));
        if (!found) return 0;
    }
    return 1;
}

/* Keep, of `text`, the sentences that pass both checks against the lines they
 * cite; how many were dropped. In place. */
static int keep_checked(char *text, const char *names, const oc_sum_lines *l, const int *cited, int nc) {
    oc_sum_buf out = {0};
    int dropped = 0;
    for (char *s = text; *s; ) {
        char *e = s;
        while (*e && !((*e == '.' || *e == '!' || *e == '?') && (e[1] == ' ' || !e[1]))) e++;
        if (*e) e++;
        char save = *e;
        *e = '\0';
        if (numbers_ok(s, l, cited, nc) && people_ok(s, names, l, cited, nc)) {
            if (out.n) oc_sum_buf_add(&out, " ", 1);
            oc_sum_buf_puts(&out, s);
        } else {
            dropped++;
        }
        *e = save;
        s = e;
        while (*s == ' ') s++;
    }
    snprintf(text, strlen(text) + 1, "%s", out.p ? out.p : "");
    oc_sum_buf_free(&out);
    return dropped;
}

/* A repeat: every word of `t` is in `kept` (lower case, letters and digits). */
static int words_within(const char *t, const char *kept) {
    char a[1024], b[1024];
    size_t na = 0, nb = 0;
    for (const char *p = t; *p && na + 2 < sizeof a; p++) a[na++] = isalnum((unsigned char)*p) || (*p & 0x80) ? (char)tolower((unsigned char)*p) : ' ';
    for (const char *p = kept; *p && nb + 2 < sizeof b; p++) b[nb++] = isalnum((unsigned char)*p) || (*p & 0x80) ? (char)tolower((unsigned char)*p) : ' ';
    a[na] = b[nb] = '\0';
    char padded[1100];
    snprintf(padded, sizeof padded, " %s ", b);
    char *save = NULL;
    for (char *w = strtok_r(a, " ", &save); w; w = strtok_r(NULL, " ", &save)) {
        char word[300];
        snprintf(word, sizeof word, " %s ", w);
        if (!strstr(padded, word)) return 0;
    }
    return 1;
}

/* An item that says there is nothing: "None", "N/A", ... */
static int says_nothing(const char *t) {
    static const char *const NOTHING[] = { "none", "n/a", "na", "nothing", "none reported", "no",
                                           "not applicable", "none noted", "none mentioned" };
    size_t n = strlen(t);
    while (n && (t[n - 1] == '.' || t[n - 1] == '!')) n--;
    for (size_t i = 0; i < sizeof NOTHING / sizeof *NOTHING; i++)
        if (strlen(NOTHING[i]) == n && !strncasecmp(t, NOTHING[i], n)) return 1;
    return 0;
}

/* What the cited lines stand for, put together: the messages to show (each
 * line's first in turn, up to SUM_CITES_MAX), everything behind them, and their
 * authors. */
static int gather(const oc_sum_lines *l, const ints *c, idset *refs, idset *all, oc_sum_buf *by) {
    for (int round = 0; refs && refs->n < SUM_CITES_MAX; round++) {
        int any = 0;
        for (int k = 0; k < c->n && refs->n < SUM_CITES_MAX; k++) {
            const oc_sum_line *x = &l->v[c->v[k]];
            if (round < x->n_refs) {
                any = 1;
                if (idset_add(refs, &x->refs[round], 1) != 0) return -1;
            }
        }
        if (!any) break;
    }
    for (int k = 0; k < c->n; k++) {
        const oc_sum_line *x = &l->v[c->v[k]];
        if (all && idset_add(all, x->all, x->n_all) != 0) return -1;
        if (by)
            for (const char *p = x->by; p && *p; ) {
                const char *e = strchr(p, '\n');
                size_t len = e ? (size_t)(e - p) : strlen(p);
                char nm[256];
                snprintf(nm, sizeof nm, "%.*s", (int)(len < 255 ? len : 255), p);
                by_add(by, nm);
                p = e ? e + 1 : p + len;
            }
    }
    return 0;
}

static void ids_json(oc_sum_buf *b, const int64_t *v, int n) {
    oc_sum_buf_puts(b, "[");
    for (int i = 0; i < n; i++) oc_sum_buf_printf(b, "%s%lld", i ? "," : "", (long long)v[i]);
    oc_sum_buf_puts(b, "]");
}

static void names_json(oc_sum_buf *b, const char *by) {
    oc_sum_buf_puts(b, "[");
    int first = 1;
    for (const char *p = by; p && *p; ) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        char nm[256];
        snprintf(nm, sizeof nm, "%.*s", (int)(len < 255 ? len : 255), p);
        if (!first) oc_sum_buf_puts(b, ",");
        oc_sum_buf_json(b, nm);
        first = 0;
        p = e ? e + 1 : p + len;
    }
    oc_sum_buf_puts(b, "]");
}

/* The grammar's alternatives for a line number, 1..n. */
static void ids_rule(oc_sum_buf *g, int n) {
    oc_sum_buf_puts(g, "id ::= ");
    for (int i = 1; i <= (n < 1 ? 1 : n); i++) oc_sum_buf_printf(g, "%s\"%d\"", i > 1 ? " | " : "", i);
    oc_sum_buf_puts(g, "\n");
}

static void lines_text(const oc_sum_lines *l, oc_sum_buf *out) {
    for (int i = 0; i < l->n; i++) {
        if (l->v[i].label) oc_sum_buf_printf(out, "%s\n", l->v[i].label);
        oc_sum_buf_printf(out, "%s[%d] ", l->v[i].indent ? "  " : "", i + 1);
        oc_sum_buf_puts(out, l->v[i].text);
        oc_sum_buf_puts(out, "\n");
    }
}

/* --- notes --------------------------------------------------------------------------- */

const char *const OC_SUM_SYSTEM =
    "You take notes on a team's chat for someone catching up on it. You write only what the lines "
    "you are given say.";

static const char *const KIND_WORD[4] = { "event", "decision", "action", "question" };
static const char KIND_TAG[4] = { 'e', 'd', 'a', 'q' };

int oc_sum_notes_for(size_t input_tokens, int input_lines) {
    int n = (int)(input_tokens / SUM_TOKENS_PER_NOTE);
    if (n < SUM_NOTES_MIN) n = SUM_NOTES_MIN;
    if (n > SUM_NOTES_ROOM) n = SUM_NOTES_ROOM;
    int half = input_lines / 2;
    int floor = input_lines < SUM_NOTES_FLOOR ? input_lines : SUM_NOTES_FLOOR;
    if (half < floor) half = floor;
    if (half < 1) half = 1;
    return n < half ? n : half;
}

int oc_sum_notes_prompt(const char *intro, const oc_sum_lines *l, int notes_max, oc_sum_buf *prompt,
                        oc_sum_buf *grammar) {
    if (notes_max < 1) notes_max = 1;
    oc_sum_buf_printf(prompt, "%s\n\n", intro);
    lines_text(l, prompt);
    oc_sum_buf_printf(prompt,
        "\nWrite notes on these lines: at most %d, the most important first. Each note is one line in "
        "exactly this form:\n"
        "- [line number] kind | topic | what happened\n"
        "\n"
        "kind is event, decision, action or question. topic names what the note is about in 2 to 5 "
        "words, and notes about the same thing share it. what happened is one or two sentences saying who "
        "did what, with the particulars the lines give. Put what is about the same thing into one note, citing the lines "
        "it comes from (at most %d), each line number in square brackets, as [12] or [12][15]. Leave out "
        "greetings and small talk.\n",
        notes_max, SUM_CITES_MAX);
    oc_sum_buf_printf(grammar,
        "root ::= note{1,%d}\n"
        "note ::= \"- \" cites \" \" kind \" | \" tag \" | \" fact \"\\n\"\n"
        "cites ::= cite cite? cite?\n"
        "cite ::= \"[\" id \"]\"\n"
        "kind ::= \"event\" | \"decision\" | \"action\" | \"question\"\n"
        "tag ::= [^|\\n]{2,%d}\n"
        "fact ::= [^\\n]{3,%d}\n",
        notes_max, SUM_TAG_CHARS, SUM_FACT_CHARS);
    ids_rule(grammar, l->n);
    return prompt->oom || grammar->oom ? -1 : 0;
}

int oc_sum_parse_notes(const char *answer, const oc_sum_lines *l, oc_sum_buf *out, int *dropped, int notes_max) {
    if (dropped) *dropped = 0;
    if (!answer) return -1;
    char *a = strdup(answer);
    if (!a) return -1;
    oc_sum_buf names = {0}, by = {0};
    all_names(l, &names);
    char **kept = NULL;
    int nk = 0, drop = 0, rc = -1;
    oc_sum_buf_puts(out, "{\"notes\":[");
    for (char *line = a; line && (notes_max < 1 || nk < notes_max); ) {
        char *e = strchr(line, '\n');
        if (e) *e = '\0';
        char *next = e ? e + 1 : NULL;
        ints c = {0};
        if (take_cites(line, l, &c) != 0) { free(c.v); goto out; }
        /* kind | topic | fact */
        char *p1 = strchr(line, '|'), *p2 = p1 ? strchr(p1 + 1, '|') : NULL;
        if (!p1 || !p2 || !c.n) { if (*tidy(line)) drop++; free(c.v); line = next; continue; }
        *p1 = *p2 = '\0';
        char *kw = tidy(line), *topic = tidy(p1 + 1), *fact = tidy(p2 + 1);
        int kind = 0;
        for (int k = 0; k < 4; k++) if (!strcasecmp(kw, KIND_WORD[k])) kind = k;
        drop += keep_checked(fact, names.p, l, c.v, c.n);
        int rep = 0;
        for (int k = 0; *fact && k < nk && !rep; k++) rep = words_within(fact, kept[k]);
        if (!*fact || says_nothing(fact) || rep) { drop += rep; free(c.v); line = next; continue; }
        char **nkept = realloc(kept, (size_t)(nk + 1) * sizeof *kept);
        if (!nkept) { free(c.v); goto out; }
        kept = nkept;
        if (!(kept[nk] = strdup(fact))) { free(c.v); goto out; }
        nk++;
        idset refs = {0}, all = {0};
        by.n = 0;
        if (by.p) by.p[0] = '\0';
        if (gather(l, &c, &refs, &all, &by) != 0) { free(c.v); free(refs.v); free(all.v); goto out; }
        oc_sum_buf_printf(out, "%s{\"kind\":\"%s\",\"topic\":", nk > 1 ? "," : "", KIND_WORD[kind]);
        oc_sum_buf_json(out, topic);
        oc_sum_buf_puts(out, ",\"text\":");
        oc_sum_buf_json(out, fact);
        oc_sum_buf_puts(out, ",\"refs\":");
        ids_json(out, refs.v, refs.n);
        oc_sum_buf_puts(out, ",\"all\":");
        ids_json(out, all.v, all.n);
        oc_sum_buf_puts(out, ",\"by\":");
        names_json(out, by.p);
        oc_sum_buf_puts(out, "}");
        free(refs.v);
        free(all.v);
        free(c.v);
        line = next;
    }
    oc_sum_buf_puts(out, "]}");
    rc = out->oom ? -1 : nk;
out:
    for (int k = 0; k < nk; k++) free(kept[k]);
    free(kept);
    oc_sum_buf_free(&names);
    oc_sum_buf_free(&by);
    free(a);
    if (dropped) *dropped = drop;
    return rc;
}

/* `by` as JSON names into a newline-separated list. */
static void names_from_json(const oc_json *d, int arr, oc_sum_buf *by) {
    if (arr < 0 || d->t[arr].type != JSMN_ARRAY) return;
    int i = arr + 1;
    for (int k = 0; k < d->t[arr].size; k++, i = oc_json_skip(d, i)) {
        char *s = json_dup(d, i);
        if (s) by_add(by, s);
        free(s);
    }
}

int oc_sum_notes_lines(const char *body, const char *label, oc_sum_lines *l) {
    oc_json d;
    if (!body || oc_json_parse(&d, body, strlen(body)) != 0) return -1;
    int arr = d.t[0].type == JSMN_OBJECT ? oc_json_get(&d, 0, "notes") : -1;
    if (arr < 0 || d.t[arr].type != JSMN_ARRAY) { oc_json_free(&d); return -1; }
    int rc = -1, first = l->n;
    oc_sum_buf t = {0}, by = {0};
    int i = arr + 1;
    for (int k = 0; k < d.t[arr].size; k++, i = oc_json_skip(&d, i)) {
        char kw[16] = "event";
        oc_json_get_str(&d, i, "kind", kw, sizeof kw);
        char *topic = json_dup(&d, oc_json_get(&d, i, "topic"));
        char *text = json_dup(&d, oc_json_get(&d, i, "text"));
        int64_t *r = NULL, *a = NULL;
        int nr = json_ids(&d, oc_json_get(&d, i, "refs"), &r);
        int na = json_ids(&d, oc_json_get(&d, i, "all"), &a);
        by.n = 0;
        if (by.p) by.p[0] = '\0';
        names_from_json(&d, oc_json_get(&d, i, "by"), &by);
        char kind = 'e';
        for (int q = 0; q < 4; q++) if (!strcmp(kw, KIND_WORD[q])) kind = KIND_TAG[q];
        t.n = 0;
        oc_sum_buf_printf(&t, "(%s) %s: %s", kw, topic ? topic : "", text ? text : "");
        int add = nr < 0 || na < 0 || t.oom || by.oom ? -1
                : oc_sum_lines_add_full(l, t.p, 0, l->n == first ? label : NULL, r, nr, a, na, by.p ? by.p : "", kind);
        free(topic);
        free(text);
        free(r);
        free(a);
        if (add != 0) goto out;
    }
    rc = 0;
out:
    oc_sum_buf_free(&t);
    oc_sum_buf_free(&by);
    oc_json_free(&d);
    return rc;
}

/* --- the summary a reader is shown ---------------------------------------------------- */

void oc_sum_caps_for(size_t input_words, oc_sum_caps *c) {
    memset(c, 0, sizeof *c);
    size_t total = input_words / SUM_WORDS_PER_WORD;
    if (total < SUM_MIN_WORDS) total = SUM_MIN_WORDS;
    if (total > SUM_MAX_WORDS) total = SUM_MAX_WORDS;
    /* Never longer than what it summarizes. */
    if (total > input_words) total = input_words ? input_words : 1;
    c->total = (int)total;
    c->overview = SUM_OVERVIEW_WORDS;
    c->para = SUM_PARA_WORDS;
    c->detail = SUM_DETAIL_WORDS;
    int topics = c->total / 100;
    if (topics < 2) topics = 2;
    if (topics > SUM_MAX_TOPICS) topics = SUM_MAX_TOPICS;
    c->topics = topics;
    c->topics_min = c->total >= 200 ? (topics > 2 ? topics - 2 : 1) : 0;
    c->details = SUM_FULL_DETAILS;
    c->details_min = c->total >= 300 ? 2 : (c->topics_min ? 1 : 0);
    c->attention = SUM_FULL_ATTENTION;
    c->more = c->topics_min ? SUM_MORE_TOPICS : 0;
    /* The most the shape can hold, written to every cap: what the answer's
     * rail is sized to, so an answer that keeps the form always ends. */
    c->room = c->overview + c->topics * (6 + c->para + c->details * c->detail) + c->attention * c->detail +
              c->more * 6;
}

int oc_sum_final_prompt(const char *intro, const oc_sum_lines *l, const oc_sum_caps *c, oc_sum_buf *prompt,
                        oc_sum_buf *grammar) {
    oc_sum_buf_printf(prompt, "%s\n\n", intro);
    lines_text(l, prompt);
    oc_sum_buf_puts(prompt,
        "\nSummarize these lines for someone catching up, in exactly this form:\n"
        "\n"
        "Overview: [line number] <the most important outcome or change>\n"
        "## <what a topic is about>\n"
        "[line number] <what happened in it, and where it stands now>\n"
        "- [line number] <one detail: who did or said what>\n"
        "Needs attention:\n"
        "- [line number] action: <who has to do what, and by when if it was said>\n"
        "- [line number] question: <a question that was asked and not answered>\n");
    if (c->more) oc_sum_buf_puts(prompt, "More topics: <topic>; <topic>\n");
    oc_sum_buf_printf(prompt,
        "\nWrite %d to %d topics, the most important first, each titled in 2 to 6 words, with what happened "
        "in it in one to three sentences and %d to %d details of one sentence each. The overview is one "
        "sentence. Write \"Needs attention\" only for actions and unanswered questions, at most %d, one "
        "sentence each, and leave it out when there are none.",
        c->topics_min, c->topics, c->details_min, c->details, c->attention);
    if (c->more) oc_sum_buf_printf(prompt, " Name any further topics, at most %d, after \"More topics\".", c->more);
    oc_sum_buf_printf(prompt,
        " Every line starts with the numbers of the lines it comes from, at most %d, "
        "each in square brackets, as [12] or [12][15]. Write only what the lines say, with their particulars, "
        "and name a person only for what their own lines say. Do not comment on, interpret or characterize "
        "what happened.\n", SUM_CITES_MAX);

    /* The shape, bounded: how many of each part, and how long each line can be
     * in characters (about 8 to a word). The words are free. */
    oc_sum_buf_printf(grammar, "root ::= ov topic{%d,%d} attn?%s\n", c->topics_min, c->topics, c->more ? " more?" : "");
    oc_sum_buf_printf(grammar, "ov ::= \"Overview: \" cites \" \" [^\\n]{3,%d} \"\\n\"\n", c->overview * 8);
    oc_sum_buf_printf(grammar, "topic ::= \"## \" title \"\\n\" cites \" \" [^\\n]{3,%d} \"\\n\" det{%d,%d}\n",
                      c->para * 8, c->details_min, c->details);
    oc_sum_buf_printf(grammar, "det ::= \"- \" cites \" \" [^\\n]{3,%d} \"\\n\"\n", c->detail * 8);
    oc_sum_buf_printf(grammar, "attn ::= \"Needs attention:\\n\" aitem{1,%d}\n", c->attention ? c->attention : 1);
    oc_sum_buf_printf(grammar, "aitem ::= \"- \" cites \" \" (\"action\" | \"question\") \": \" [^\\n]{3,%d} \"\\n\"\n",
                      c->detail * 8);
    if (c->more) oc_sum_buf_printf(grammar, "more ::= \"More topics: \" title (\"; \" title){0,%d} \"\\n\"\n", c->more - 1);
    oc_sum_buf_puts(grammar,
        "title ::= [^\\n#;]{2,60}\n"
        "cites ::= cite cite? cite?\n"
        "cite ::= \"[\" id \"]\"\n");
    ids_rule(grammar, l->n);
    return prompt->oom || grammar->oom ? -1 : 0;
}

static void part_free(oc_sum_part *p) {
    free(p->text);
    free(p->cited);
    memset(p, 0, sizeof *p);
}

void oc_sum_final_free(oc_sum_final *f) {
    part_free(&f->ov);
    for (int t = 0; t < f->nt; t++) {
        free(f->top[t].title);
        part_free(&f->top[t].para);
        for (int d = 0; d < f->top[t].nd; d++) part_free(&f->top[t].det[d]);
        free(f->top[t].all);
        free(f->top[t].by);
    }
    for (int a = 0; a < f->na; a++) part_free(&f->att[a]);
    for (int m = 0; m < f->nm; m++) free(f->more[m]);
    memset(f, 0, sizeof *f);
}

/* `raw` (with its citations) into `p`: the cited lines, the messages to show,
 * and the text that passes the checks. 0, 1 when nothing of it is left, or -1. */
static int part_take(oc_sum_part *p, char *raw, const oc_sum_lines *l, const char *names, int *dropped) {
    memset(p, 0, sizeof *p);
    ints c = {0};
    if (take_cites(raw, l, &c) != 0) { free(c.v); return -1; }
    char *t = tidy(raw);
    if (!c.n || !*t || says_nothing(t)) { if (*t && !c.n) (*dropped)++; free(c.v); return 1; }
    *dropped += keep_checked(t, names, l, c.v, c.n);
    if (!*t) { free(c.v); return 1; }
    idset refs = {0};
    if (gather(l, &c, &refs, NULL, NULL) != 0) { free(c.v); free(refs.v); return -1; }
    p->n_refs = refs.n;
    memcpy(p->refs, refs.v, (size_t)refs.n * sizeof *refs.v);
    free(refs.v);
    p->cited = c.v;
    p->n_cited = c.n;
    p->text = strdup(t);
    return p->text ? 0 : -1;
}

static int part_repeats(const oc_sum_part *p, const oc_sum_part *v, int n) {
    for (int i = 0; i < n; i++) if (v[i].text && words_within(p->text, v[i].text)) return 1;
    return 0;
}

int oc_sum_part_check(oc_sum_part *p, const oc_sum_lines *l) {
    oc_sum_buf names = {0};
    all_names(l, &names);
    int d = keep_checked(p->text, names.p, l, p->cited, p->n_cited);
    oc_sum_buf_free(&names);
    return d;
}

int oc_sum_parse_final(const char *answer, const oc_sum_lines *l, const oc_sum_caps *c, oc_sum_final *f,
                       int *dropped) {
    memset(f, 0, sizeof *f);
    int drop = 0, rc = -1, mode = 0;   /* 0 overview/topics, 1 needs attention */
    if (dropped) *dropped = 0;
    if (!answer) return -1;
    char *a = strdup(answer);
    if (!a) return -1;
    oc_sum_buf names = {0};
    all_names(l, &names);
    oc_sum_topic *cur = NULL;
    for (char *line = a; line; ) {
        char *e = strchr(line, '\n');
        if (e) *e = '\0';
        char *next = e ? e + 1 : NULL;
        char *s = line;
        while (*s == ' ') s++;
        /* Citations written before the word: "1-4 Overview: ..." -- the word
         * goes first, the citations after it, as the parser reads them. */
        if (isdigit((unsigned char)*s) || *s == '[') {
            char *o = strstr(s, "Overview:");
            if (!o) o = strstr(s, "overview:");
            if (o) {
                char *c = o;
                while (c > s && c[-1] == ' ') c--;
                int ok = 1;
                for (char *k = s; k < c; k++)
                    if (!isdigit((unsigned char)*k) && !strchr(" ,-:[]", *k) && (unsigned char)*k < 0x80) ok = 0;
                if (ok && c - s < 128) {
                    char cites[128];
                    memcpy(cites, s, (size_t)(c - s));
                    cites[c - s] = '\0';
                    memmove(s, o, 9);
                    s[9] = ' ';
                    memcpy(s + 10, cites, (size_t)(c - s));
                    /* what followed "Overview:" is already in place after */
                }
            }
        }
        if (!strncasecmp(s, "Overview:", 9)) {
            if (!f->ov.text && part_take(&f->ov, s + 9, l, names.p, &drop) < 0) goto out;
        } else if (!strncmp(s, "##", 2) && !strncasecmp(tidy(s + 2), "Needs attention", 15)) {
            /* The heading written as a title: the section it names. */
            mode = 1;
            cur = NULL;
        } else if (!strncmp(s, "##", 2) && !strncasecmp(tidy(s + 2), "More topics:", 12)) {
            memmove(s, tidy(s + 2), strlen(tidy(s + 2)) + 1);
            goto more;
        } else if (!strncmp(s, "##", 2)) {
            mode = 0;
            cur = NULL;
            if (f->nt < c->topics && f->nt < SUM_MAX_TOPICS) {
                cur = &f->top[f->nt++];
                memset(cur, 0, sizeof *cur);
                char *t = tidy(s + 2);
                size_t tn = strlen(t);
                while (tn && t[tn - 1] == '.') t[--tn] = '\0';   /* a title, not a sentence */
                if (!(cur->title = strdup(t))) goto out;
            }
        } else if (!strncasecmp(s, "Needs attention", 15)) {
            mode = 1;
            cur = NULL;
        } else if (!strncasecmp(s, "More topics:", 12)) {
        more:;
            char *save = NULL;
            for (char *t = strtok_r(s + 12, ";", &save); t && f->nm < c->more && f->nm < SUM_MORE_TOPICS;
                 t = strtok_r(NULL, ";", &save)) {
                char *x = tidy(t);
                /* A title carries no citations: "[12]" written into one is dropped. */
                for (char *r = x, *w = x;; r++) {
                    if (*r == '[' && isdigit((unsigned char)r[1])) {
                        char *e = r + 1;
                        while (isdigit((unsigned char)*e)) e++;
                        if (*e == ']') { r = e; continue; }
                    }
                    *w++ = *r;
                    if (!*r) break;
                }
                x = tidy(x);
                size_t xn = strlen(x);
                while (xn && x[xn - 1] == '.') x[--xn] = '\0';
                if (*x && !(f->more[f->nm++] = strdup(x))) goto out;
            }
        } else if (mode == 1 && *s == '-') {
            if (f->na < c->attention && f->na < SUM_FULL_ATTENTION) {
                oc_sum_part p;
                char *body = s + 1;
                /* "[n] action: ..." -- the kind after the citations. */
                char *colon = strchr(body, ':');
                char kind = 'a';
                if (colon) {
                    char *k = colon;
                    while (k > body && isalpha((unsigned char)k[-1])) k--;
                    if (!strncasecmp(k, "question", 8)) kind = 'q';
                    if (!strncasecmp(k, "question", 8) || !strncasecmp(k, "action", 6)) memmove(k, colon + 1, strlen(colon + 1) + 1);
                }
                int r = part_take(&p, body, l, names.p, &drop);
                if (r < 0) goto out;
                if (r == 0 && part_repeats(&p, f->att, f->na)) { part_free(&p); drop++; r = 1; }
                if (r == 0) { f->att_kind[f->na] = kind; f->att[f->na++] = p; }
            }
        } else if (cur && *s == '-') {
            if (cur->nd < c->details && cur->nd < SUM_FULL_DETAILS) {
                oc_sum_part p;
                int r = part_take(&p, s + 1, l, names.p, &drop);
                if (r < 0) goto out;
                if (r == 0 && (part_repeats(&p, cur->det, cur->nd) || (cur->para.text && words_within(p.text, cur->para.text))))
                    { part_free(&p); drop++; r = 1; }
                if (r == 0) cur->det[cur->nd++] = p;
            }
        } else if (cur && *s && !cur->para.text) {
            if (part_take(&cur->para, s, l, names.p, &drop) < 0) goto out;
        } else if (*s && (f->ov.text || f->nt)) {
            /* A topic's title without its "##": the line after the overview, or
             * after a topic's account and details, in the order the shape
             * prescribes. Its citations, if any, are the account's to give. */
            cur = NULL;
            if (f->nt < c->topics && f->nt < SUM_MAX_TOPICS) {
                ints cs = {0};
                if (take_cites(s, l, &cs) != 0) { free(cs.v); goto out; }
                free(cs.v);
                char *t = tidy(s);
                size_t tn = strlen(t);
                while (tn && t[tn - 1] == '.') t[--tn] = '\0';
                if (*t) {
                    cur = &f->top[f->nt++];
                    memset(cur, 0, sizeof *cur);
                    if (!(cur->title = strdup(t))) goto out;
                }
            }
        }
        line = next;
    }
    /* What each topic stands for, who is in it, and how much it weighs. */
    for (int t = 0; t < f->nt; t++) {
        oc_sum_topic *tp = &f->top[t];
        ints all_c = {0};
        for (int k = 0; k < tp->para.n_cited; k++) if (ints_add(&all_c, tp->para.cited[k]) != 0) goto out;
        for (int d = 0; d < tp->nd; d++)
            for (int k = 0; k < tp->det[d].n_cited; k++) if (ints_add(&all_c, tp->det[d].cited[k]) != 0) goto out;
        idset all = {0};
        oc_sum_buf by = {0};
        int g = gather(l, &all_c, NULL, &all, &by);
        tp->weight = 1;
        for (int k = 0; k < all_c.n; k++) {
            char kd = l->v[all_c.v[k]].kind;
            int w = kd == 'd' || kd == 'a' ? 3 : kd == 'q' ? 2 : 1;
            if (w > tp->weight) tp->weight = w;
        }
        free(all_c.v);
        tp->all = all.v;
        tp->n_all = all.n;
        tp->by = by.p ? by.p : strdup("");
        for (int k = 0; k < all.n; k++) if (all.v[k] > tp->last) tp->last = all.v[k];
        if (g != 0 || !tp->by) goto out;
    }
    /* A topic with nothing left in it is none. */
    for (int t = 0; t < f->nt; ) {
        if (!f->top[t].para.text && !f->top[t].nd) {
            oc_sum_topic dead = f->top[t];
            memmove(&f->top[t], &f->top[t + 1], (size_t)(f->nt - t - 1) * sizeof *f->top);
            f->nt--;
            free(dead.title); free(dead.all); free(dead.by);
            drop++;
        } else t++;
    }
    /* "More topics" names only topics not shown, each once. */
    for (int m = 0; m < f->nm; ) {
        int dup = 0;
        for (int t = 0; t < f->nt && !dup; t++) dup = f->top[t].title && !strcasecmp(f->more[m], f->top[t].title);
        for (int q = 0; q < m && !dup; q++) dup = !strcasecmp(f->more[m], f->more[q]);
        if (dup) {
            free(f->more[m]);
            memmove(&f->more[m], &f->more[m + 1], (size_t)(f->nm - m - 1) * sizeof *f->more);
            f->nm--;
        } else m++;
    }
    /* No overview: the first topic's account stands in for it. */
    if (!f->ov.text && f->nt && f->top[0].para.text) {
        f->ov = f->top[0].para;
        f->ov.text = strdup(f->top[0].para.text);
        f->ov.cited = malloc((size_t)(f->ov.n_cited ? f->ov.n_cited : 1) * sizeof *f->ov.cited);
        if (!f->ov.text || !f->ov.cited) goto out;
        memcpy(f->ov.cited, f->top[0].para.cited, (size_t)f->ov.n_cited * sizeof *f->ov.cited);
    }
    rc = f->ov.text || f->nt ? 0 : -1;
out:
    oc_sum_buf_free(&names);
    free(a);
    if (dropped) *dropped = drop;
    if (rc != 0) oc_sum_final_free(f);
    return rc;
}

static int topic_order(const void *x, const void *y) {
    const oc_sum_topic *a = x, *b = y;
    if (a->weight != b->weight) return b->weight - a->weight;
    if (a->n_all != b->n_all) return b->n_all - a->n_all;
    return a->last < b->last ? 1 : a->last > b->last ? -1 : 0;
}

void oc_sum_final_rank(oc_sum_final *f) {
    qsort(f->top, (size_t)f->nt, sizeof *f->top, topic_order);
}

int oc_sum_cut_words(char *text, int max_words) {
    if ((int)oc_sum_words(text) <= max_words) return 0;
    /* The last whole sentence that fits; else the words that fit. */
    char *best = NULL;
    int words = 0, in = 0;
    for (char *p = text; *p; p++) {
        int sp = *p == ' ';
        if (!sp && !in) words++;
        in = !sp;
        if (words > max_words) break;
        if ((*p == '.' || *p == '!' || *p == '?') && (p[1] == ' ' || !p[1])) best = p + 1;
    }
    if (best) { *best = '\0'; return 1; }
    words = 0;
    in = 0;
    for (char *p = text; *p; p++) {
        int sp = *p == ' ';
        if (!sp && !in && ++words > max_words) { while (p > text && p[-1] == ' ') p--; *p = '\0'; break; }
        in = !sp;
    }
    return 1;
}

int oc_sum_final_words(const oc_sum_final *f) {
    int w = f->ov.text ? (int)oc_sum_words(f->ov.text) : 0;
    for (int t = 0; t < f->nt; t++) {
        w += (int)oc_sum_words(f->top[t].title) + (f->top[t].para.text ? (int)oc_sum_words(f->top[t].para.text) : 0);
        for (int d = 0; d < f->top[t].nd; d++) w += (int)oc_sum_words(f->top[t].det[d].text);
    }
    for (int a = 0; a < f->na; a++) w += (int)oc_sum_words(f->att[a].text);
    return w;
}

int oc_sum_final_fit(oc_sum_final *f, const oc_sum_caps *c) {
    int changed = 0;
    /* The lowest-ranked topics go to "More topics" first. */
    while (oc_sum_final_words(f) > c->total && f->nt > 1) {
        oc_sum_topic *tp = &f->top[f->nt - 1];
        if (f->nm < c->more) {
            memmove(&f->more[1], &f->more[0], (size_t)f->nm * sizeof *f->more);
            f->more[0] = tp->title;
            f->nm++;
            tp->title = NULL;
        }
        free(tp->title);
        part_free(&tp->para);
        for (int d = 0; d < tp->nd; d++) part_free(&tp->det[d]);
        free(tp->all);
        free(tp->by);
        f->nt--;
        changed = 1;
    }
    /* Then the last details, the last attention items, and the account. */
    while (oc_sum_final_words(f) > c->total && f->nt && f->top[f->nt - 1].nd) {
        part_free(&f->top[f->nt - 1].det[--f->top[f->nt - 1].nd]);
        changed = 1;
    }
    while (oc_sum_final_words(f) > c->total && f->na) { part_free(&f->att[--f->na]); changed = 1; }
    while (oc_sum_final_words(f) > c->total && f->nt && f->top[0].para.text) {
        int over = oc_sum_final_words(f) - c->total, have = (int)oc_sum_words(f->top[0].para.text);
        if (have - over > 0) oc_sum_cut_words(f->top[0].para.text, have - over);
        else { free(f->top[0].para.text); f->top[0].para.text = NULL; }
        changed = 1;
        if ((int)oc_sum_words(f->top[0].para.text ? f->top[0].para.text : "") == have) break;
    }
    if (oc_sum_final_words(f) > c->total && f->nt && !f->top[0].para.text) {
        /* The title alone over: the summary is the overview. */
        oc_sum_topic *tp = &f->top[0];
        free(tp->title); free(tp->all); free(tp->by);
        f->nt = 0;
        changed = 1;
    }
    if (oc_sum_final_words(f) > c->total && f->ov.text) {
        int over = oc_sum_final_words(f) - c->total, have = (int)oc_sum_words(f->ov.text);
        oc_sum_cut_words(f->ov.text, have - over > 0 ? have - over : 1);
        changed = 1;
    }
    return changed;
}

int oc_sum_final_json(const oc_sum_final *f, oc_sum_buf *out) {
    oc_sum_buf_puts(out, "{\"overview\":{\"text\":");
    oc_sum_buf_json(out, f->ov.text ? f->ov.text : "");
    oc_sum_buf_puts(out, ",\"refs\":");
    ids_json(out, f->ov.refs, f->ov.n_refs);
    oc_sum_buf_puts(out, "},\"topics\":[");
    for (int t = 0; t < f->nt; t++) {
        const oc_sum_topic *tp = &f->top[t];
        oc_sum_buf_puts(out, t ? ",{\"title\":" : "{\"title\":");
        oc_sum_buf_json(out, tp->title ? tp->title : "");
        oc_sum_buf_puts(out, ",\"text\":");
        oc_sum_buf_json(out, tp->para.text ? tp->para.text : "");
        oc_sum_buf_puts(out, ",\"refs\":");
        ids_json(out, tp->para.refs, tp->para.n_refs);
        oc_sum_buf_printf(out, ",\"count\":%d,\"people\":", tp->n_all);
        names_json(out, tp->by);
        oc_sum_buf_puts(out, ",\"details\":[");
        for (int d = 0; d < tp->nd; d++) {
            oc_sum_buf_puts(out, d ? ",{\"text\":" : "{\"text\":");
            oc_sum_buf_json(out, tp->det[d].text);
            oc_sum_buf_puts(out, ",\"refs\":");
            ids_json(out, tp->det[d].refs, tp->det[d].n_refs);
            oc_sum_buf_puts(out, "}");
        }
        oc_sum_buf_puts(out, "]}");
    }
    oc_sum_buf_puts(out, "],\"attention\":[");
    for (int a = 0; a < f->na; a++) {
        oc_sum_buf_printf(out, "%s{\"kind\":\"%s\",\"text\":", a ? "," : "", f->att_kind[a] == 'q' ? "question" : "action");
        oc_sum_buf_json(out, f->att[a].text);
        oc_sum_buf_puts(out, ",\"refs\":");
        ids_json(out, f->att[a].refs, f->att[a].n_refs);
        oc_sum_buf_puts(out, "}");
    }
    oc_sum_buf_puts(out, "],\"more\":[");
    for (int m = 0; m < f->nm; m++) {
        if (m) oc_sum_buf_puts(out, ",");
        oc_sum_buf_json(out, f->more[m]);
    }
    oc_sum_buf_printf(out, "],\"words\":%zu}", f->words);
    return out->oom ? -1 : 0;
}

int oc_sum_rewrite_prompt(const char *text, int cap, oc_sum_buf *prompt, oc_sum_buf *grammar) {
    oc_sum_buf_printf(prompt,
        "This is %zu words:\n\n%s\n\nRewrite it in at most %d words. Keep every name, number and fact "
        "that fits, and add nothing.\n", oc_sum_words(text), text, cap);
    oc_sum_buf_printf(grammar, "root ::= [^\\n]{3,%d} \"\\n\"\n", cap * 8);
    return prompt->oom || grammar->oom ? -1 : 0;
}

/* "[3][7]" for a part's cited lines, at most SUM_CITES_MAX of them. */
static void cites_text(const oc_sum_part *p, oc_sum_buf *out) {
    int n = p->n_cited < SUM_CITES_MAX ? p->n_cited : SUM_CITES_MAX;
    for (int i = 0; i < n; i++) oc_sum_buf_printf(out, "[%d]", p->cited[i] + 1);
    if (!n) oc_sum_buf_puts(out, "[1]");
}

int oc_sum_final_text(const oc_sum_final *f, oc_sum_buf *out) {
    if (f->ov.text) {
        oc_sum_buf_puts(out, "Overview: ");
        cites_text(&f->ov, out);
        oc_sum_buf_printf(out, " %s\n", f->ov.text);
    }
    for (int t = 0; t < f->nt; t++) {
        const oc_sum_topic *tp = &f->top[t];
        oc_sum_buf_printf(out, "## %s\n", tp->title ? tp->title : "");
        if (tp->para.text) {
            cites_text(&tp->para, out);
            oc_sum_buf_printf(out, " %s\n", tp->para.text);
        }
        for (int d = 0; d < tp->nd; d++) {
            oc_sum_buf_puts(out, "- ");
            cites_text(&tp->det[d], out);
            oc_sum_buf_printf(out, " %s\n", tp->det[d].text);
        }
    }
    if (f->na) {
        oc_sum_buf_puts(out, "Needs attention:\n");
        for (int a = 0; a < f->na; a++) {
            oc_sum_buf_puts(out, "- ");
            cites_text(&f->att[a], out);
            oc_sum_buf_printf(out, " %s: %s\n", f->att_kind[a] == 'q' ? "question" : "action", f->att[a].text);
        }
    }
    if (f->nm) {
        oc_sum_buf_puts(out, "More topics: ");
        for (int m = 0; m < f->nm; m++) oc_sum_buf_printf(out, "%s%s", m ? "; " : "", f->more[m]);
        oc_sum_buf_puts(out, "\n");
    }
    return out->oom ? -1 : 0;
}

int oc_sum_shorten_prompt(const oc_sum_final *f, const oc_sum_caps *c, oc_sum_buf *prompt) {
    oc_sum_buf_printf(prompt, "This summary is %d words:\n\n", oc_sum_final_words(f));
    if (oc_sum_final_text(f, prompt) != 0) return -1;
    oc_sum_buf_printf(prompt,
        "\nWrite it again in at most %d words, in exactly the same form, each line keeping the line numbers it "
        "starts with. Keep every name, number and fact that fits, the most important first, and add nothing.\n",
        c->total);
    return prompt->oom ? -1 : 0;
}

/* --- stored bodies ---------------------------------------------------------------------- */

int oc_sum_body_readable(const char *body) {
    oc_json d;
    if (!body || oc_json_parse(&d, body, strlen(body)) != 0) return 0;
    int ov = d.t[0].type == JSMN_OBJECT ? oc_json_get(&d, 0, "overview") : -1;
    int arr = d.t[0].type == JSMN_OBJECT ? oc_json_get(&d, 0, "topics") : -1;
    int ok = ov >= 0 && d.t[ov].type == JSMN_OBJECT && arr >= 0 && d.t[arr].type == JSMN_ARRAY;
    oc_json_free(&d);
    return ok;
}

int oc_sum_body_empty(const char *body) {
    oc_json d;
    if (!body || oc_json_parse(&d, body, strlen(body)) != 0) return 1;
    int arr = oc_json_get(&d, 0, "notes");
    if (arr < 0) arr = oc_json_get(&d, 0, "topics");
    int empty = arr < 0 || d.t[arr].type != JSMN_ARRAY || d.t[arr].size == 0;
    if (empty && oc_json_get(&d, 0, "overview") >= 0) {
        char ov[8] = "";
        int o = oc_json_get(&d, 0, "overview");
        oc_json_get_str(&d, o, "text", ov, sizeof ov);
        empty = !ov[0];
    }
    oc_json_free(&d);
    return empty;
}
