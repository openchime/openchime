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

/* --- lines and people ----------------------------------------------------------- */

int oc_sum_lines_add(oc_sum_lines *l, const char *text, int indent, const char *label,
                     const int64_t *refs, int n_refs) {
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
    x->refs = malloc((size_t)(n_refs > 0 ? n_refs : 1) * sizeof *x->refs);
    if (!x->text || (label && !x->label) || !x->refs) {
        free(x->text); free(x->label); free(x->refs);
        return -1;
    }
    if (n_refs > 0) memcpy(x->refs, refs, (size_t)n_refs * sizeof *refs);
    x->n_refs = n_refs > 0 ? n_refs : 0;
    x->indent = indent;
    l->n++;
    return 0;
}

void oc_sum_lines_free(oc_sum_lines *l) {
    for (int i = 0; i < l->n; i++) { free(l->v[i].text); free(l->v[i].label); free(l->v[i].refs); }
    free(l->v);
    memset(l, 0, sizeof *l);
}

size_t oc_sum_lines_tokens(const oc_sum_lines *l) {
    size_t bytes = 0;
    for (int i = 0; i < l->n; i++)
        bytes += 8 + strlen(l->v[i].text) + (l->v[i].label ? strlen(l->v[i].label) + 2 : 0);
    return bytes / SUM_BYTES_PER_TOKEN + 1;
}

int oc_sum_people_add(oc_sum_people *p, int64_t id, const char *name) {
    for (int i = 0; i < p->n; i++) if (p->id[i] == id) return 0;
    int64_t *ni = realloc(p->id, (size_t)(p->n + 1) * sizeof *ni);
    if (!ni) return -1;
    p->id = ni;
    char **nn = realloc(p->name, (size_t)(p->n + 1) * sizeof *nn);
    if (!nn) return -1;
    p->name = nn;
    p->name[p->n] = strdup(name && *name ? name : "someone");
    if (!p->name[p->n]) return -1;
    p->id[p->n++] = id;
    return 0;
}

void oc_sum_people_free(oc_sum_people *p) {
    for (int i = 0; i < p->n; i++) free(p->name[i]);
    free(p->id);
    free(p->name);
    memset(p, 0, sizeof *p);
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

int oc_sum_chunk_lines(const oc_sum_msg *msgs, const oc_sum_chunk *c, oc_sum_lines *l, oc_sum_people *people) {
    oc_sum_buf t = {0};
    for (int k = 0; k < c->n; k++) {
        const oc_sum_msg *m = &msgs[c->idx[k]];
        const char *who = m->author && *m->author ? m->author : "someone";
        t.n = 0;
        oc_sum_buf_printf(&t, "%s: ", who);
        oc_sum_buf_puts(&t, m->text ? m->text : "");
        if (t.oom || oc_sum_lines_add(l, t.p, m->parent_id != 0, NULL, &m->id, 1) != 0 ||
            oc_sum_people_add(people, m->author_id, who) != 0) {
            oc_sum_buf_free(&t);
            return -1;
        }
    }
    oc_sum_buf_free(&t);
    return 0;
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

/* --- a stored summary as lines ---------------------------------------------------- */

static const char *const KINDS[4] = { "decisions", "actions", "problems", "facts" };

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

static int add_unique(int64_t **v, int *n, int *cap, const int64_t *add, int na) {
    for (int a = 0; a < na; a++) {
        int dup = 0;
        for (int k = 0; k < *n && !dup; k++) dup = (*v)[k] == add[a];
        if (dup) continue;
        if (*n == *cap) {
            int nc = *cap ? *cap * 2 : 16;
            int64_t *nv = realloc(*v, (size_t)nc * sizeof *nv);
            if (!nv) return -1;
            *v = nv;
            *cap = nc;
        }
        (*v)[(*n)++] = add[a];
    }
    return 0;
}

int oc_sum_body_lines(const char *body, oc_sum_name_fn name, void *ctx, const char *label,
                      oc_sum_lines *l, oc_sum_people *people) {
    oc_json d;
    if (!body || oc_json_parse(&d, body, strlen(body)) != 0) return -1;
    if (d.t[0].type != JSMN_OBJECT) { oc_json_free(&d); return -1; }
    int rc = -1, first = l->n;
    oc_sum_buf t = {0};
    int64_t *all = NULL;
    int n_all = 0, c_all = 0;
    /* The overview stands for every message its summary's lines stood for, or
     * else for everything its items cite. */
    if ((n_all = json_ids(&d, oc_json_get(&d, 0, "refs"), &all)) < 0) { n_all = 0; goto out; }
    c_all = n_all;
    for (int kind = 0; kind < 4 && !n_all; kind++) {
        int arr = oc_json_get(&d, 0, KINDS[kind]);
        if (arr < 0 || d.t[arr].type != JSMN_ARRAY) continue;
        int i = arr + 1;
        for (int k = 0; k < d.t[arr].size; k++, i = oc_json_skip(&d, i)) {
            int64_t *r = NULL;
            int nr = json_ids(&d, oc_json_get(&d, i, "refs"), &r);
            if (nr < 0 || add_unique(&all, &n_all, &c_all, r, nr) != 0) { free(r); goto out; }
            free(r);
        }
    }
    char *ov = json_dup(&d, oc_json_get(&d, 0, "overview"));
    if (ov && *ov) {
        t.n = 0;
        oc_sum_buf_printf(&t, "Overview: %s", ov);
        if (t.oom || oc_sum_lines_add(l, t.p, 0, label, all, n_all) != 0) { free(ov); goto out; }
    }
    free(ov);
    for (int kind = 0; kind < 4; kind++) {
        int arr = oc_json_get(&d, 0, KINDS[kind]);
        if (arr < 0 || d.t[arr].type != JSMN_ARRAY) continue;
        int i = arr + 1;
        for (int k = 0; k < d.t[arr].size; k++, i = oc_json_skip(&d, i)) {
            char *text = json_dup(&d, oc_json_get(&d, i, kind == 1 ? "what" : "text"));
            char st[16] = "";
            oc_json_get_str(&d, i, "status", st, sizeof st);
            int64_t *r = NULL;
            int nr = json_ids(&d, oc_json_get(&d, i, "refs"), &r);
            if (!text || nr < 0) { free(text); free(r); if (nr < 0) goto out; continue; }
            t.n = 0;
            switch (kind) {
            case 0: oc_sum_buf_printf(&t, "Decision: %s", text); break;
            case 1: {
                /* No "who": the text names whoever it is. */
                int wi = oc_json_get(&d, i, "who");
                uint64_t who = 0;
                if (wi >= 0) oc_json_u64(&d, wi, &who);
                const char *nm = who && name ? name(ctx, (int64_t)who) : NULL;
                if (who && (!nm || !*nm)) nm = "someone";
                if (who && oc_sum_people_add(people, (int64_t)who, nm) != 0) { free(text); free(r); goto out; }
                oc_sum_buf_puts(&t, "Action");
                if (*st) oc_sum_buf_printf(&t, " (%s)", st);
                if (wi >= 0) oc_sum_buf_printf(&t, ": %s: %s", who ? nm : "Team", text);
                else oc_sum_buf_printf(&t, ": %s", text);
                break;
            }
            case 2:
                oc_sum_buf_puts(&t, "Problem");
                if (*st) oc_sum_buf_printf(&t, " (%s)", st);
                oc_sum_buf_printf(&t, ": %s", text);
                break;
            default: oc_sum_buf_printf(&t, "Fact: %s", text);
            }
            free(text);
            int add = t.oom ? -1 : oc_sum_lines_add(l, t.p, 0, l->n == first ? label : NULL, r, nr);
            free(r);
            if (add != 0) goto out;
        }
    }
    rc = 0;
out:
    free(all);
    oc_sum_buf_free(&t);
    oc_json_free(&d);
    return rc;
}

int oc_sum_body_empty(const char *body) {
    oc_json d;
    if (!body || oc_json_parse(&d, body, strlen(body)) != 0) return 1;
    int ov = oc_json_get(&d, 0, "overview");
    int empty = ov < 0 || d.t[ov].type != JSMN_STRING || d.t[ov].end == d.t[ov].start;
    for (int kind = 0; kind < 4 && empty; kind++) {
        int arr = oc_json_get(&d, 0, KINDS[kind]);
        if (arr >= 0 && d.t[arr].type == JSMN_ARRAY && d.t[arr].size > 0) empty = 0;
    }
    oc_json_free(&d);
    return empty;
}

int oc_sum_body_overview(const char *body, char *out, size_t cap) {
    oc_json d;
    if (!body || oc_json_parse(&d, body, strlen(body)) != 0) return -1;
    int rc = oc_json_get_str(&d, 0, "overview", out, cap);
    oc_json_free(&d);
    return rc;
}

/* --- the prompt ---------------------------------------------------------------------- */

const char *const OC_SUM_SYSTEM =
    "You summarize a team's chat for someone catching up on it. You write only what the lines "
    "you are given say.";

int oc_sum_prompt(const char *intro, const oc_sum_lines *l, oc_sum_buf *out) {
    size_t words = 0;
    for (int i = 0; i < l->n; i++) words += oc_sum_words(l->v[i].text);
    size_t budget = (words * SUM_WORDS_PCT + 99) / 100;
    oc_sum_buf_printf(out, "%s\n\n", intro);
    for (int i = 0; i < l->n; i++) {
        if (l->v[i].label) oc_sum_buf_printf(out, "%s\n", l->v[i].label);
        oc_sum_buf_printf(out, "%s[%d] ", l->v[i].indent ? "  " : "", i + 1);
        oc_sum_buf_puts(out, l->v[i].text);
        oc_sum_buf_puts(out, "\n");
    }
    oc_sum_buf_printf(out,
        "\nSummarize these lines. Write at most %zu words of text; headings and line numbers do "
        "not count. Answer in exactly this form, one item to a line, every item starting with the "
        "numbers of the lines it comes from:\n"
        "\n"
        "Overview: <what happened>\n"
        "Decisions:\n"
        "- [line number] <what was decided>\n"
        "Actions:\n"
        "- [line number] <who, or Team>: <what they do> (open)\n"
        "Problems:\n"
        "- [line number] <what went wrong> (open)\n"
        "Facts:\n"
        "- [line number] <a number, date, ticket, customer or release>\n"
        "\n"
        "An action is (open) or (done); a problem is (open) or (resolved). Write as many items "
        "under a heading as the lines give, or none.\n",
        budget);
    return out->oom ? -1 : 0;
}

/* --- the follow-up ------------------------------------------------------------------ */

int oc_sum_followup(const char *ask, int n_items, int n_lines, oc_sum_buf *q, oc_sum_buf *g) {
    oc_sum_buf_puts(q, "These items from your summary have no line numbers:\n\n");
    oc_sum_buf_puts(q, ask);
    oc_sum_buf_puts(q, "\nFor each item, write its number, a colon, and the numbers of the lines it comes "
                       "from, each in brackets, one item to a line, like this: "
                       "\"<item number>: [line number][line number]\".\n");
    oc_sum_buf_puts(g, "root ::= item+\nitem ::= k \":\" ( \" [\" id \"]\" )+ \"\\n\"\nk ::= ");
    for (int i = 1; i <= (n_items < 1 ? 1 : n_items); i++) oc_sum_buf_printf(g, "%s\"%d\"", i > 1 ? " | " : "", i);
    oc_sum_buf_puts(g, "\nid ::= ");
    for (int i = 1; i <= (n_lines < 1 ? 1 : n_lines); i++) oc_sum_buf_printf(g, "%s\"%d\"", i > 1 ? " | " : "", i);
    oc_sum_buf_puts(g, "\n");
    return q->oom || g->oom ? -1 : 0;
}

/* --- reading an answer --------------------------------------------------------------- */

/* Every run of digits in `text` must occur in one of the lines cited. */
static int numbers_ok(const char *text, const oc_sum_lines *l, const int *cited, int nc) {
    for (const char *p = text; *p; ) {
        if (*p < '0' || *p > '9') { p++; continue; }
        const char *s = p;
        while (*p >= '0' && *p <= '9') p++;
        size_t len = (size_t)(p - s);
        int found = 0;
        for (int k = 0; k < nc && !found; k++) {
            for (const char *q = l->v[cited[k]].text; *q && !found; q++)
                if (!strncmp(q, s, len)) found = 1;
        }
        if (!found) return 0;
    }
    return 1;
}

/* The headings, as a model may write them: on a line of their own or inside one,
 * in any case, in markdown or not. */
static const char *const HEADS[5] = { "overview", "decisions", "actions", "problems", "facts" };

/* A heading at `p`, not inside a word: its index, with *after just past its
 * colon; or -1. */
static int heading_at(const char *start, const char *p, char **after) {
    if (p > start && isalnum((unsigned char)p[-1])) return -1;
    for (int h = 0; h < 5; h++) {
        size_t n = strlen(HEADS[h]);
        if (strncasecmp(p, HEADS[h], n) != 0) continue;
        const char *q = p + n;
        while (*q == '*') q++;
        if (*q != ':') continue;
        q++;
        while (*q == '*') q++;
        *after = (char *)q;
        return h;
    }
    return -1;
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
 * "[3, 7]", "[1-4]", and the same in parentheses. Into `cited`: 0-based lines,
 * once each, in the order written. A bracketed number that is not a line is
 * taken out and ignored; a parenthesized one is text, left as it is. 0, or -1
 * when out of memory. */
static int take_cites(char *s, const oc_sum_lines *l, ints *cited) {
    char *w = s;
    for (char *p = s; *p; ) {
        if (*p == '[' || *p == '(') {
            char close = *p == '[' ? ']' : ')';
            char *e = p + 1;
            int digits = 0;
            while (*e && (isdigit((unsigned char)*e) || *e == ' ' || *e == ',' || *e == '-')) {
                if (isdigit((unsigned char)*e)) digits = 1;
                e++;
            }
            if (digits && *e == close) {
                ints got = {0};
                int ok = 1;
                for (char *q = p + 1; q < e && ok; ) {
                    if (!isdigit((unsigned char)*q)) { q++; continue; }
                    long a = strtol(q, &q, 10), z = a;
                    while (*q == ' ') q++;
                    if (*q == '-') {
                        q++;
                        while (*q == ' ') q++;
                        if (isdigit((unsigned char)*q)) z = strtol(q, &q, 10);
                    }
                    if (a < 1 || z < a || z > l->n) { ok = 0; break; }
                    for (long v = a; v <= z && ok; v++) if (ints_add(&got, (int)v - 1) != 0) ok = -1;
                }
                if (ok == 1) {
                    for (int i = 0; i < got.n; i++)
                        if (ints_add(cited, got.v[i]) != 0) { free(got.v); return -1; }
                    free(got.v);
                    p = e + 1;
                    continue;
                }
                free(got.v);
                if (ok < 0) return -1;
                if (close == ']') { p = e + 1; continue; }
            }
        }
        *w++ = *p++;
    }
    *w = '\0';
    return 0;
}

/* Take every "(open)", "(done)" and "(resolved)" out of `s`, in place, in any
 * case: the first one's index in STATUS, or -1 when there is none. */
static const char *const STATUS[3] = { "open", "done", "resolved" };
static int take_status(char *s) {
    int first = -1;
    char *w = s;
    for (char *p = s; *p; ) {
        if (*p == '(') {
            int hit = -1;
            for (int k = 0; k < 3 && hit < 0; k++) {
                size_t n = strlen(STATUS[k]);
                if (!strncasecmp(p + 1, STATUS[k], n) && p[1 + n] == ')') hit = k;
            }
            if (hit >= 0) {
                if (first < 0) first = hit;
                p += strlen(STATUS[hit]) + 2;
                continue;
            }
        }
        *w++ = *p++;
    }
    *w = '\0';
    return first;
}

/* `s` with runs of spaces made one, no space before punctuation (where a
 * citation was taken out), and the bullet marks, numbering and stray
 * punctuation left at either end taken off. */
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
        const char *d = s;
        while (isdigit((unsigned char)*d)) d++;
        if (d > s && (*d == '.' || *d == ')') && d[1] == ' ') { s = (char *)d + 2; continue; }
        break;
    }
    size_t n = strlen(s);
    while (n && strchr(" -*#,;:", s[n - 1])) s[--n] = '\0';
    return s;
}

typedef struct { char **v; int n, cap; } seen;

static int seen_has(const seen *s, const char *t) {
    for (int i = 0; i < s->n; i++) if (!strcasecmp(s->v[i], t)) return 1;
    return 0;
}

static int seen_add(seen *s, const char *t) {
    if (s->n == s->cap) {
        int nc = s->cap ? s->cap * 2 : 8;
        char **nv = realloc(s->v, (size_t)nc * sizeof *nv);
        if (!nv) return -1;
        s->v = nv;
        s->cap = nc;
    }
    s->v[s->n] = strdup(t);
    return s->v[s->n] ? (s->n++, 0) : -1;
}

static void seen_free(seen *s) {
    for (int i = 0; i < s->n; i++) free(s->v[i]);
    free(s->v);
    memset(s, 0, sizeof *s);
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

/* Who `name` is: the team (0), someone in the lines by full name, or by a first
 * name only one of them has. 1 with *who set, or 0. */
static int find_who(const char *name, const oc_sum_people *people, int64_t *who) {
    *who = 0;
    if (!strcasecmp(name, "Team")) return 1;
    for (int k = 0; k < people->n; k++)
        if (!strcasecmp(name, people->name[k])) { *who = people->id[k]; return 1; }
    int found = 0;
    size_t n = strlen(name);
    for (int k = 0; k < people->n; k++) {
        const char *nm = people->name[k];
        if (!strncasecmp(nm, name, n) && nm[n] == ' ') {
            if (found) return 0;
            found = 1;
            *who = people->id[k];
        }
    }
    return found;
}

/* The line numbers a follow-up gave each item asked about: "2: [3][5]", one
 * item to a line. */
typedef struct { ints *v; int n; } given;

static int read_given(const char *text, const oc_sum_lines *l, given *g) {
    memset(g, 0, sizeof *g);
    if (!text) return 0;
    char *t = strdup(text);
    if (!t) return -1;
    int rc = 0;
    for (char *line = t; line && rc == 0; ) {
        char *e = strchr(line, '\n');
        if (e) *e = '\0';
        char *q = line;
        while (*q == ' ' || *q == '-') q++;
        if (isdigit((unsigned char)*q)) {
            long k = strtol(q, &q, 10);
            if (*q == ':' || *q == '.') q++;
            if (k >= 1 && k <= 100000) {
                if (k > g->n) {
                    ints *nv = realloc(g->v, (size_t)k * sizeof *nv);
                    if (!nv) { rc = -1; break; }
                    memset(nv + g->n, 0, (size_t)(k - g->n) * sizeof *nv);
                    g->v = nv;
                    g->n = (int)k;
                }
                if (!g->v[k - 1].n && take_cites(q, l, &g->v[k - 1]) != 0) rc = -1;
            }
        }
        line = e ? e + 1 : NULL;
    }
    free(t);
    return rc;
}

static void given_free(given *g) {
    for (int i = 0; i < g->n; i++) free(g->v[i].v);
    free(g->v);
    memset(g, 0, sizeof *g);
}

static const char *const SECTION_NAME[4] = { "Decision", "Action", "Problem", "Fact" };

int oc_sum_parse(const char *answer, const char *cites, const oc_sum_lines *l, const oc_sum_people *people,
                 oc_sum_buf *out, oc_sum_buf *ask, int *dropped) {
    if (dropped) *dropped = 0;
    if (!answer) return -1;
    char *a = strdup(answer);
    if (!a) return -1;
    /* Where each heading is, and where what it heads begins. */
    typedef struct { int h; char *at, *body; } mark;
    mark *m = NULL;
    int nm = 0, cm = 0;
    for (char *p = a; *p; ) {
        char *after;
        int h = heading_at(a, p, &after);
        if (h < 0) { p++; continue; }
        if (nm == cm) {
            int nc = cm ? cm * 2 : 16;
            mark *nv = realloc(m, (size_t)nc * sizeof *nv);
            if (!nv) { free(m); free(a); return -1; }
            m = nv;
            cm = nc;
        }
        m[nm++] = (mark){ h, p, after };
        p = after;
    }
    if (!nm) { free(m); free(a); return -1; }
    for (int i = 1; i < nm; i++) *m[i].at = '\0';

    oc_sum_buf items[4] = { {0}, {0}, {0}, {0} };
    int count[4] = { 0, 0, 0, 0 };
    seen kept[4] = { {0}, {0}, {0}, {0} };
    int drop = 0, total = 0, rc = -1, uncited = 0;
    char *overview = NULL;
    ints all = {0};
    int64_t *all_refs = NULL;
    int n_all = 0, c_all = 0;
    given g = {0};
    for (int i = 0; i < l->n; i++)
        if (ints_add(&all, i) != 0 || add_unique(&all_refs, &n_all, &c_all, l->v[i].refs, l->v[i].n_refs) != 0)
            goto out;
    if (read_given(cites, l, &g) != 0) goto out;

    for (int i = 0; i < nm; i++) {
        if (m[i].h == 0) {
            /* The first overview with something in it. */
            if (overview && *overview) continue;
            ints c = {0};
            int bad = take_cites(m[i].body, l, &c);
            free(c.v);
            if (bad) goto out;
            take_status(m[i].body);
            char *t = tidy(m[i].body);
            free(overview);
            overview = NULL;
            if (says_nothing(t)) continue;
            if (!numbers_ok(t, l, all.v, all.n)) { drop++; continue; }
            if (!(overview = strdup(t))) goto out;
            continue;
        }
        int section = m[i].h - 1;
        for (char *line = m[i].body; line; ) {
            char *e = strchr(line, '\n');
            if (e) *e = '\0';
            char *next = e ? e + 1 : NULL;
            ints cited = {0};
            if (take_cites(line, l, &cited) != 0) { free(cited.v); goto out; }
            int status = take_status(line);
            char *text = tidy(line);
            if (!*text || says_nothing(text)) { free(cited.v); line = next; continue; }
            /* No line numbers: those a follow-up gave it, or else every line. */
            if (!cited.n) {
                uncited++;
                if (uncited <= g.n && g.v[uncited - 1].n) {
                    cited = g.v[uncited - 1];
                    g.v[uncited - 1] = (ints){0};
                } else if (ask) {
                    oc_sum_buf_printf(ask, "%d. %s: %s\n", uncited, SECTION_NAME[section], text);
                }
            }
            const int *cv = cited.n ? cited.v : all.v;
            int cn = cited.n ? cited.n : all.n;
            /* An action: "who: what", the who someone in the lines or the team;
             * anyone else stays in the text. */
            int64_t who = -1;
            if (section == 1) {
                char *colon = strchr(text, ':');
                if (colon) {
                    *colon = '\0';
                    char *name = tidy(text);
                    int64_t w;
                    if (find_who(name, people, &w)) { who = w; text = tidy(colon + 1); }
                    else *colon = ':';
                }
            }
            /* Statuses: an action's open or done, a problem's open or resolved;
             * none, or the other kind's, is no status. */
            const char *st = NULL;
            if (section == 1 && (status == 0 || status == 1)) st = STATUS[status];
            if (section == 2 && (status == 0 || status == 2)) st = STATUS[status];
            if (!*text || !numbers_ok(text, l, cv, cn)) {
                drop++;
                free(cited.v);
                line = next;
                continue;
            }
            /* A repeat of a kept item (for an action, the same person too). */
            oc_sum_buf k = {0};
            oc_sum_buf_printf(&k, "%lld|", (long long)who);
            oc_sum_buf_puts(&k, text);
            if (k.oom || seen_has(&kept[section], k.p)) {
                int oom = k.oom;
                oc_sum_buf_free(&k);
                free(cited.v);
                if (oom) goto out;
                drop++;
                line = next;
                continue;
            }
            if (seen_add(&kept[section], k.p) != 0) { oc_sum_buf_free(&k); free(cited.v); goto out; }
            oc_sum_buf_free(&k);
            /* The message ids its lines stand for. */
            int64_t *refs = NULL;
            int nr = 0, cr = 0;
            for (int c = 0; c < cn; c++)
                if (add_unique(&refs, &nr, &cr, l->v[cv[c]].refs, l->v[cv[c]].n_refs) != 0) {
                    free(refs);
                    free(cited.v);
                    goto out;
                }
            oc_sum_buf *b = &items[section];
            oc_sum_buf_puts(b, count[section] ? ",{" : "{");
            if (section == 1) {
                if (who >= 0) oc_sum_buf_printf(b, "\"who\":%lld,", (long long)who);
                oc_sum_buf_puts(b, "\"what\":");
            } else {
                oc_sum_buf_puts(b, "\"text\":");
            }
            oc_sum_buf_json(b, text);
            if (section == 0) oc_sum_buf_puts(b, ",\"by\":[]");
            oc_sum_buf_puts(b, ",\"refs\":[");
            for (int x = 0; x < nr; x++) oc_sum_buf_printf(b, "%s%lld", x ? "," : "", (long long)refs[x]);
            oc_sum_buf_puts(b, "]");
            if (st) oc_sum_buf_printf(b, ",\"status\":\"%s\"", st);
            oc_sum_buf_puts(b, "}");
            count[section]++;
            total++;
            free(refs);
            free(cited.v);
            line = next;
        }
    }
    /* The overview stands for every message the lines stand for. */
    oc_sum_buf_puts(out, "{\"overview\":");
    oc_sum_buf_json(out, overview ? overview : "");
    oc_sum_buf_puts(out, ",\"refs\":[");
    for (int x = 0; x < n_all; x++) oc_sum_buf_printf(out, "%s%lld", x ? "," : "", (long long)all_refs[x]);
    oc_sum_buf_puts(out, "]");
    for (int s = 0; s < 4; s++) {
        oc_sum_buf_printf(out, ",\"%s\":[", KINDS[s]);
        if (items[s].p) oc_sum_buf_add(out, items[s].p, items[s].n);
        oc_sum_buf_puts(out, "]");
        if (items[s].oom) out->oom = 1;
    }
    oc_sum_buf_puts(out, "}");
    if (ask && ask->oom) out->oom = 1;
    rc = out->oom ? -1 : total;
out:
    for (int s = 0; s < 4; s++) { oc_sum_buf_free(&items[s]); seen_free(&kept[s]); }
    free(overview);
    free(all.v);
    free(all_refs);
    given_free(&g);
    free(m);
    free(a);
    if (dropped) *dropped = drop;
    return rc;
}
