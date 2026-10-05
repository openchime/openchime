/* Channel and DM summaries: the engine-free core (sum_core.h). */
#define _POSIX_C_SOURCE 200809L
#include "sum_core.h"

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
    /* The overview stands for everything its items cite. */
    for (int kind = 0; kind < 4; kind++) {
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
                uint64_t who = 0;
                oc_json_u64(&d, oc_json_get(&d, i, "who"), &who);
                const char *nm = who && name ? name(ctx, (int64_t)who) : NULL;
                if (who && (!nm || !*nm)) nm = "someone";
                if (who && oc_sum_people_add(people, (int64_t)who, nm) != 0) { free(text); free(r); goto out; }
                oc_sum_buf_printf(&t, "Action (%s): %s: %s", strcmp(st, "done") ? "open" : "done",
                                  who ? nm : "Team", text);
                break;
            }
            case 2: oc_sum_buf_printf(&t, "Problem (%s): %s", strcmp(st, "resolved") ? "open" : "resolved", text); break;
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

/* --- the prompt and the grammar ----------------------------------------------------- */

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
        "\nSummarize these lines in at most %zu words. Begin with \"Overview:\" and what happened. "
        "Then write the headings Decisions:, Actions:, Problems: and Facts:. Under each, write one "
        "line per item that starts with \"- \" and ends with the numbers of the lines it comes "
        "from, each in brackets. An action gives who does it (or Team), a colon, what they do, "
        "then (open) or (done). A problem ends with (open) or (resolved). Facts are numbers, "
        "dates, tickets, customers and releases. Leave a heading empty when nothing belongs "
        "under it.\n",
        budget);
    return out->oom ? -1 : 0;
}

/* A GBNF string literal of `s`. */
static void gbnf_lit(oc_sum_buf *g, const char *s) {
    oc_sum_buf_puts(g, "\"");
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') oc_sum_buf_printf(g, "\\%c", *s);
        else if (*s == '\n' || *s == '\r') oc_sum_buf_puts(g, " ");
        else oc_sum_buf_add(g, s, 1);
    }
    oc_sum_buf_puts(g, "\"");
}

int oc_sum_grammar(int n_lines, const oc_sum_people *people, oc_sum_buf *g) {
    oc_sum_buf_puts(g,
        "root ::= \"Overview: \" ov \"\\n\" \"Decisions:\\n\" dec* \"Actions:\\n\" act* "
        "\"Problems:\\n\" prob* \"Facts:\\n\" fact*\n"
        "ov ::= [^\\n]+\n"
        "txt ::= [^\\n\\[]+\n"
        "cites ::= \" \" ( \"[\" id \"]\" )+\n"
        "dec ::= \"- \" txt cites \"\\n\"\n"
        "act ::= \"- \" who \": \" txt \" (\" ( \"open\" | \"done\" ) \")\" cites \"\\n\"\n"
        "prob ::= \"- \" txt \" (\" ( \"open\" | \"resolved\" ) \")\" cites \"\\n\"\n"
        "fact ::= \"- \" txt cites \"\\n\"\n"
        "who ::= \"Team\"");
    for (int i = 0; i < people->n; i++) {
        oc_sum_buf_puts(g, " | ");
        gbnf_lit(g, people->name[i]);
    }
    oc_sum_buf_puts(g, "\nid ::= ");
    if (n_lines < 1) n_lines = 1;
    for (int i = 1; i <= n_lines; i++) oc_sum_buf_printf(g, "%s\"%d\"", i > 1 ? " | " : "", i);
    oc_sum_buf_puts(g, "\n");
    return g->oom ? -1 : 0;
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

/* Strip the citations "[3][7]" off the end of `s` (in place), into `cited`
 * (0-based line indices). Returns how many, or -1 when one is not a line. */
static int take_cites(char *s, const oc_sum_lines *l, int **cited) {
    int n = 0, cap = 0;
    *cited = NULL;
    size_t len = strlen(s);
    while (len && s[len - 1] == ' ') s[--len] = '\0';
    while (len && s[len - 1] == ']') {
        size_t o = len - 1;
        while (o && s[o - 1] != '[') o--;
        if (!o) break;
        long v = 0;
        int digits = 0;
        for (size_t k = o; k < len - 1; k++) {
            if (s[k] < '0' || s[k] > '9') { digits = -1; break; }
            v = v * 10 + (s[k] - '0');
            digits++;
        }
        if (digits <= 0) break;
        if (v < 1 || v > l->n) { free(*cited); *cited = NULL; return -1; }
        if (n == cap) {
            int nc = cap ? cap * 2 : 4;
            int *nv = realloc(*cited, (size_t)nc * sizeof *nv);
            if (!nv) { free(*cited); *cited = NULL; return -1; }
            *cited = nv;
            cap = nc;
        }
        (*cited)[n++] = (int)v - 1;
        len = o - 1;
        s[len] = '\0';
        while (len && s[len - 1] == ' ') s[--len] = '\0';
    }
    /* Read from the end: put them back in the order written. */
    for (int a = 0, z = n - 1; a < z; a++, z--) { int t = (*cited)[a]; (*cited)[a] = (*cited)[z]; (*cited)[z] = t; }
    return n;
}

/* `s` ends with " (word)" for one of `words`: strip it, and return its index. */
static int take_status(char *s, const char *const *words, int n) {
    size_t len = strlen(s);
    for (int i = 0; i < n; i++) {
        size_t wl = strlen(words[i]);
        if (len >= wl + 3 && s[len - wl - 3] == ' ' && s[len - wl - 2] == '(' && s[len - 1] == ')' &&
            !strncmp(s + len - wl - 1, words[i], wl)) {
            s[len - wl - 3] = '\0';
            return i;
        }
    }
    return -1;
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

int oc_sum_parse(const char *answer, const oc_sum_lines *l, const oc_sum_people *people, oc_sum_buf *out,
                 int *dropped) {
    static const char *const OPEN_DONE[2] = { "open", "done" };
    static const char *const OPEN_RES[2] = { "open", "resolved" };
    static const char *const HEADS[4] = { "Decisions:", "Actions:", "Problems:", "Facts:" };
    if (dropped) *dropped = 0;
    if (!answer || strncmp(answer, "Overview:", 9) != 0) return -1;
    oc_sum_buf items[4] = { {0}, {0}, {0}, {0} };
    int count[4] = { 0, 0, 0, 0 };
    seen kept[4] = { {0}, {0}, {0}, {0} };
    int section = -1, drop = 0, total = 0, rc = -1;
    char *overview = NULL;
    int *all = malloc((size_t)(l->n ? l->n : 1) * sizeof *all);
    if (!all) return -1;
    for (int i = 0; i < l->n; i++) all[i] = i;

    const char *p = answer;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        char *line = malloc(len + 1);
        if (!line) goto out;
        memcpy(line, p, len);
        line[len] = '\0';
        p = e ? e + 1 : p + len;
        while (len && (line[len - 1] == ' ' || line[len - 1] == '\r')) line[--len] = '\0';

        int head = -1;
        for (int h = 0; h < 4; h++) if (!strcmp(line, HEADS[h])) head = h;
        if (head >= 0) { section = head; free(line); continue; }
        if (section < 0 && !strncmp(line, "Overview:", 9)) {
            char *t = line + 9;
            while (*t == ' ') t++;
            int *c = NULL;
            int nc = take_cites(t, l, &c);
            free(c);
            free(overview);
            overview = nc < 0 ? NULL : strdup(t);
            if (overview && !numbers_ok(overview, l, all, l->n)) { free(overview); overview = NULL; drop++; }
            free(line);
            continue;
        }
        if (section < 0 || strncmp(line, "- ", 2) != 0) { free(line); continue; }
        char *t = line + 2;
        int *cited = NULL;
        int nc = take_cites(t, l, &cited);
        if (nc <= 0) { drop++; free(cited); free(line); continue; }
        int status = 0;
        int64_t who = 0;
        if (section == 1 || section == 2) {
            status = take_status(t, section == 1 ? OPEN_DONE : OPEN_RES, 2);
            if (status < 0) { drop++; free(cited); free(line); continue; }
        }
        char *text = t;
        if (section == 1) {
            char *colon = strstr(t, ": ");
            if (!colon) { drop++; free(cited); free(line); continue; }
            *colon = '\0';
            int found = !strcmp(t, "Team");
            for (int k = 0; k < people->n && !found; k++)
                if (!strcmp(t, people->name[k])) { found = 1; who = people->id[k]; }
            if (!found) { drop++; free(cited); free(line); continue; }
            text = colon + 2;
        }
        while (*text == ' ') text++;
        if (!*text || !numbers_ok(text, l, cited, nc)) { drop++; free(cited); free(line); continue; }
        /* A repeat of a kept bullet (for an action, the same person too). */
        char key[64];
        snprintf(key, sizeof key, "%lld|", (long long)who);
        oc_sum_buf k = {0};
        oc_sum_buf_puts(&k, key);
        oc_sum_buf_puts(&k, text);
        if (k.oom || seen_has(&kept[section], k.p)) {
            int oom = k.oom;
            oc_sum_buf_free(&k);
            free(cited);
            free(line);
            if (oom) goto out;
            drop++;
            continue;
        }
        if (seen_add(&kept[section], k.p) != 0) { oc_sum_buf_free(&k); free(cited); free(line); goto out; }
        oc_sum_buf_free(&k);
        /* The message ids its lines stand for. */
        int64_t *refs = NULL;
        int nr = 0, cr = 0;
        for (int c = 0; c < nc; c++)
            if (add_unique(&refs, &nr, &cr, l->v[cited[c]].refs, l->v[cited[c]].n_refs) != 0) {
                free(refs); free(cited); free(line);
                goto out;
            }
        oc_sum_buf *b = &items[section];
        oc_sum_buf_puts(b, count[section] ? ",{" : "{");
        if (section == 1) {
            oc_sum_buf_printf(b, "\"who\":%lld,\"what\":", (long long)who);
        } else {
            oc_sum_buf_puts(b, "\"text\":");
        }
        oc_sum_buf_json(b, text);
        if (section == 0) oc_sum_buf_puts(b, ",\"by\":[]");
        oc_sum_buf_puts(b, ",\"refs\":[");
        for (int x = 0; x < nr; x++) oc_sum_buf_printf(b, "%s%lld", x ? "," : "", (long long)refs[x]);
        oc_sum_buf_puts(b, "]");
        if (section == 1) oc_sum_buf_printf(b, ",\"status\":\"%s\"", OPEN_DONE[status]);
        if (section == 2) oc_sum_buf_printf(b, ",\"status\":\"%s\"", OPEN_RES[status]);
        oc_sum_buf_puts(b, "}");
        count[section]++;
        total++;
        free(refs);
        free(cited);
        free(line);
    }
    oc_sum_buf_puts(out, "{\"overview\":");
    oc_sum_buf_json(out, overview ? overview : "");
    for (int s = 0; s < 4; s++) {
        oc_sum_buf_printf(out, ",\"%s\":[", KINDS[s]);
        if (items[s].p) oc_sum_buf_add(out, items[s].p, items[s].n);
        oc_sum_buf_puts(out, "]");
        if (items[s].oom) out->oom = 1;
    }
    oc_sum_buf_puts(out, "}");
    rc = out->oom ? -1 : total;
out:
    for (int s = 0; s < 4; s++) { oc_sum_buf_free(&items[s]); seen_free(&kept[s]); }
    free(overview);
    free(all);
    if (dropped) *dropped = drop;
    return rc;
}
