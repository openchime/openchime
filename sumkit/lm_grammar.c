/* The grammars an answer is held to (docs/details/SUMKIT.md §5): the subset of GBNF
 * that daemon/sum_core.c builds -- rules ("name ::= ..."), string literals,
 * character classes ([...] and [^...], with ranges and \n \t \r \\ \" \]
 * escapes), alternation, grouping, rule references, and ?, *, + and {m},
 * {m,}, {m,n} -- read once a call, anything else refused. The root rule is
 * "root". Matching is over code points, as GBNF's is: the state is a set
 * of stacks, each the rest of what can follow, popped down to the next
 * character class; a token is allowed when its bytes take some stack through,
 * a character at a time (a token that ends inside a character is allowed when
 * its first byte could start one the stack accepts). */
#include "sk_priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { G_SEQ, G_ALT, G_REP, G_CLASS, G_RULE };

typedef struct gnode {
    int kind;
    int n, cap;                 /* SEQ, ALT: children; CLASS: ranges */
    struct gnode **kid;
    int min, max;               /* REP (max -1: no bound); RULE: rule index in min */
    int negated;                /* CLASS */
    uint32_t *lo, *hi;          /* CLASS ranges */
} gnode;

typedef struct frame {
    const gnode *node;
    int at;                     /* SEQ: next child; REP: done so far */
    const struct frame *next;   /* the rest of the stack, shared */
} frame;

#define G_MAX_STACKS 512

struct sk_grammar {
    gnode **all;                /* every node, for freeing */
    int nall, call;
    char **names;
    gnode **rules;
    int nrules;
    int root;
    /* the frames of the current state and of candidates, from an arena kept
     * across a call */
    frame *arena;
    size_t used, cap;
    const frame **cur;          /* the current stacks, each popped to a CLASS; NULL: accepting */
    int ncur;
    uint8_t part[4];            /* bytes of a character a token ended inside */
    int npart;
};

static gnode *newnode(sk_grammar *g, int kind) {
    gnode *x = calloc(1, sizeof *x);
    if (!x) return NULL;
    x->kind = kind;
    if (g->nall == g->call) {
        int nc = g->call ? 2 * g->call : 64;
        gnode **na = realloc(g->all, (size_t)nc * sizeof *na);
        if (!na) { free(x); return NULL; }
        g->all = na;
        g->call = nc;
    }
    g->all[g->nall++] = x;
    return x;
}

static int addkid(gnode *p, gnode *k) {
    if (!k) return -1;
    if (p->n == p->cap) {
        int nc = p->cap ? 2 * p->cap : 4;
        gnode **nk = realloc(p->kid, (size_t)nc * sizeof *nk);
        if (!nk) return -1;
        p->kid = nk;
        p->cap = nc;
    }
    p->kid[p->n++] = k;
    return 0;
}

static int addrange(gnode *c, uint32_t lo, uint32_t hi) {
    if (c->n == c->cap) {
        int nc = c->cap ? 2 * c->cap : 4;
        uint32_t *a = realloc(c->lo, (size_t)nc * sizeof *a), *b = a ? realloc(c->hi, (size_t)nc * sizeof *b) : NULL;
        if (!a || !b) { if (a) c->lo = a; return -1; }
        c->lo = a;
        c->hi = b;
        c->cap = nc;
    }
    c->lo[c->n] = lo;
    c->hi[c->n++] = hi;
    return 0;
}

void sk_grammar_free(sk_grammar *g) {
    if (!g) return;
    for (int i = 0; i < g->nall; i++) {
        free(g->all[i]->kid);
        free(g->all[i]->lo);
        free(g->all[i]->hi);
        free(g->all[i]);
    }
    free(g->all);
    for (int i = 0; i < g->nrules; i++) free(g->names[i]);
    free(g->names);
    free(g->rules);
    free(g->arena);
    free(g->cur);
    free(g);
}

/* ---- reading ------------------------------------------------------------------ */

typedef struct { const char *p; sk_grammar *g; const char *err; } rd;

static void skip_ws(rd *r, int newlines) {
    for (;;) {
        while (*r->p == ' ' || *r->p == '\t' || (newlines && (*r->p == '\n' || *r->p == '\r'))) r->p++;
        if (*r->p == '#') { while (*r->p && *r->p != '\n') r->p++; continue; }
        break;
    }
}

static int is_name(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_'; }

static int rule_index(sk_grammar *g, const char *name, size_t n) {
    for (int i = 0; i < g->nrules; i++)
        if (strlen(g->names[i]) == n && !memcmp(g->names[i], name, n)) return i;
    char **nn = realloc(g->names, (size_t)(g->nrules + 1) * sizeof *nn);
    gnode **nr = nn ? realloc(g->rules, (size_t)(g->nrules + 1) * sizeof *nr) : NULL;
    if (!nn || !nr) { if (nn) g->names = nn; return -1; }
    g->names = nn;
    g->rules = nr;
    g->names[g->nrules] = malloc(n + 1);
    if (!g->names[g->nrules]) return -1;
    memcpy(g->names[g->nrules], name, n);
    g->names[g->nrules][n] = 0;
    g->rules[g->nrules] = NULL;
    return g->nrules++;
}

/* One character of a literal or class, escapes read: its code point, or -1. */
static long read_char(rd *r) {
    const unsigned char *s = (const unsigned char *)r->p;
    if (*s == '\\') {
        r->p += 2;
        switch (s[1]) {
        case 'n': return '\n';
        case 't': return '\t';
        case 'r': return '\r';
        case '\\': return '\\';
        case '"': return '"';
        case ']': return ']';
        case '[': return '[';
        case '-': return '-';
        case '^': return '^';
        default: r->err = "an escape the engine does not read"; return -1;
        }
    }
    int len = s[0] < 0x80 ? 1 : (s[0] >> 5) == 6 ? 2 : (s[0] >> 4) == 14 ? 3 : (s[0] >> 3) == 30 ? 4 : 0;
    if (!len || !s[0]) { r->err = "a character that is not UTF-8"; return -1; }
    uint32_t v = len == 1 ? s[0] : len == 2 ? s[0] & 0x1f : len == 3 ? s[0] & 0x0f : s[0] & 0x07;
    for (int k = 1; k < len; k++) {
        if ((s[k] & 0xc0) != 0x80) { r->err = "a character that is not UTF-8"; return -1; }
        v = v << 6 | (s[k] & 0x3f);
    }
    r->p += len;
    return (long)v;
}

static gnode *read_alt(rd *r);

static gnode *read_term(rd *r) {
    sk_grammar *g = r->g;
    gnode *t = NULL;
    if (*r->p == '"') {
        r->p++;
        t = newnode(g, G_SEQ);
        while (t && *r->p && *r->p != '"') {
            long c = read_char(r);
            gnode *k = c < 0 ? NULL : newnode(g, G_CLASS);
            if (!k || addrange(k, (uint32_t)c, (uint32_t)c) || addkid(t, k)) return NULL;
        }
        if (*r->p != '"') { r->err = "a string that does not end"; return NULL; }
        r->p++;
    } else if (*r->p == '[') {
        r->p++;
        t = newnode(g, G_CLASS);
        if (!t) return NULL;
        if (*r->p == '^') { t->negated = 1; r->p++; }
        while (*r->p && *r->p != ']') {
            long a = read_char(r), b = a;
            if (a < 0) return NULL;
            if (*r->p == '-' && r->p[1] != ']') { r->p++; b = read_char(r); if (b < a) { r->err = "a range that runs backwards"; return NULL; } }
            if (addrange(t, (uint32_t)a, (uint32_t)b)) return NULL;
        }
        if (*r->p != ']') { r->err = "a character class that does not end"; return NULL; }
        r->p++;
    } else if (*r->p == '(') {
        r->p++;
        skip_ws(r, 1);
        t = read_alt(r);
        skip_ws(r, 1);
        if (!t || *r->p != ')') { if (!r->err) r->err = "a group that does not end"; return NULL; }
        r->p++;
    } else if (is_name(*r->p)) {
        const char *s = r->p;
        while (is_name(*r->p)) r->p++;
        t = newnode(g, G_RULE);
        if (!t || (t->min = rule_index(g, s, (size_t)(r->p - s))) < 0) return NULL;
    } else {
        r->err = "something the engine's grammars do not have";
        return NULL;
    }
    /* repetition */
    for (;;) {
        int mn, mx;
        if (*r->p == '?') { mn = 0; mx = 1; r->p++; }
        else if (*r->p == '*') { mn = 0; mx = -1; r->p++; }
        else if (*r->p == '+') { mn = 1; mx = -1; r->p++; }
        else if (*r->p == '{') {
            char *e;
            mn = (int)strtol(r->p + 1, &e, 10);
            if (e == r->p + 1 || mn < 0) { r->err = "a repetition without a count"; return NULL; }
            mx = mn;
            if (*e == ',') {
                const char *f = e + 1;
                mx = *f == '}' ? -1 : (int)strtol(f, &e, 10);
                if (mx != -1 && (e == f || mx < mn)) { r->err = "a repetition whose counts are wrong"; return NULL; }
                if (mx == -1) e = (char *)f;
            }
            if (*e != '}') { r->err = "a repetition that does not end"; return NULL; }
            r->p = e + 1;
        } else break;
        gnode *rep = newnode(g, G_REP);
        if (!rep || addkid(rep, t)) return NULL;
        rep->min = mn;
        rep->max = mx;
        t = rep;
    }
    return t;
}

static gnode *read_seq(rd *r) {
    gnode *s = newnode(r->g, G_SEQ);
    if (!s) return NULL;
    for (;;) {
        skip_ws(r, 0);
        if (!*r->p || *r->p == '\n' || *r->p == '\r' || *r->p == '|' || *r->p == ')') break;
        if (addkid(s, read_term(r))) return NULL;
    }
    return s;
}

static gnode *read_alt(rd *r) {
    gnode *a = newnode(r->g, G_ALT);
    if (!a || addkid(a, read_seq(r))) return NULL;
    for (;;) {
        const char *save = r->p;
        skip_ws(r, 1);
        if (*r->p != '|') { r->p = save; break; }
        r->p++;
        skip_ws(r, 1);
        if (addkid(a, read_seq(r))) return NULL;
    }
    return a;
}

sk_grammar *sk_grammar_new(const char *text, char *err, size_t errcap) {
    sk_grammar *g = calloc(1, sizeof *g);
    if (!g) { sk_seterr(err, errcap, "out of memory"); return NULL; }
    rd r = { text, g, NULL };
    for (;;) {
        skip_ws(&r, 1);
        if (!*r.p) break;
        const char *s = r.p;
        while (is_name(*r.p)) r.p++;
        size_t n = (size_t)(r.p - s);
        skip_ws(&r, 0);
        if (!n || strncmp(r.p, "::=", 3)) { r.err = "a line that is not a rule"; break; }
        r.p += 3;
        int idx = rule_index(g, s, n);
        if (idx < 0) { r.err = "out of memory"; break; }
        if (g->rules[idx]) { r.err = "a rule defined twice"; break; }
        skip_ws(&r, 0);
        gnode *body = read_alt(&r);
        if (!body) { if (!r.err) r.err = "out of memory"; break; }
        g->rules[idx] = body;
    }
    if (!r.err) {
        g->root = -1;
        for (int i = 0; i < g->nrules; i++) {
            if (!g->rules[i]) { r.err = "a rule used but not defined"; break; }
            if (!strcmp(g->names[i], "root")) g->root = i;
        }
        if (!r.err && g->root < 0) r.err = "no root rule";
    }
    if (r.err) {
        sk_seterr(err, errcap, "the grammar does not parse: %s (at \"%.20s\")", r.err, r.p);
        sk_grammar_free(g);
        return NULL;
    }
    sk_grammar_reset(g);
    if (g->ncur < 0) { sk_seterr(err, errcap, "the grammar is too large to run"); sk_grammar_free(g); return NULL; }
    return g;
}

/* ---- matching ------------------------------------------------------------------ */

static const frame *push(sk_grammar *g, const gnode *n, int at, const frame *next) {
    if (g->used == g->cap) return NULL;           /* the arena is sized on reset */
    frame *f = &g->arena[g->used++];
    f->node = n;
    f->at = at;
    f->next = next;
    return f;
}

/* Every stack `s` can become with a CLASS on top (or empty: accepting),
 * appended to out. -1 when there are too many. */
static int expand(sk_grammar *g, const frame *s, const frame **out, int *n, int depth) {
    if (depth > 4096) return -1;
    if (!s) {
        for (int i = 0; i < *n; i++) if (!out[i]) return 0;      /* one accepting stack is enough */
        if (*n >= G_MAX_STACKS) return -1;
        out[(*n)++] = NULL;
        return 0;
    }
    const gnode *x = s->node;
    switch (x->kind) {
    case G_CLASS:
        if (*n >= G_MAX_STACKS) return -1;
        out[(*n)++] = s;
        return 0;
    case G_RULE:
        return expand(g, push(g, g->rules[x->min], 0, s->next), out, n, depth + 1);
    case G_SEQ:
        if (s->at >= x->n) return expand(g, s->next, out, n, depth + 1);
        {
            const frame *rest = push(g, x, s->at + 1, s->next);
            if (!rest) return -1;
            const frame *top = push(g, x->kid[s->at], 0, rest);
            return top ? expand(g, top, out, n, depth + 1) : -1;
        }
    case G_ALT:
        for (int i = 0; i < x->n; i++) {
            const frame *k = push(g, x->kid[i], 0, s->next);
            if (!k || expand(g, k, out, n, depth + 1)) return -1;
        }
        return 0;
    case G_REP:
        if (x->max < 0 || s->at < x->max) {
            const frame *rest = push(g, x, s->at + 1, s->next);
            const frame *top = rest ? push(g, x->kid[0], 0, rest) : NULL;
            if (!top || expand(g, top, out, n, depth + 1)) return -1;
        }
        if (s->at >= x->min) return expand(g, s->next, out, n, depth + 1);
        return 0;
    }
    return -1;
}

static int class_has(const gnode *c, uint32_t cp) {
    int in = 0;
    for (int i = 0; i < c->n && !in; i++) in = cp >= c->lo[i] && cp <= c->hi[i];
    return c->negated ? !in : in;
}

/* Stacks after one code point; -1 when there are too many. */
static int step(sk_grammar *g, const frame *const *in, int nin, uint32_t cp, const frame **out) {
    int n = 0;
    for (int i = 0; i < nin; i++)
        if (in[i] && class_has(in[i]->node, cp) && expand(g, in[i]->next, out, &n, 0)) return -1;
    return n;
}

/* Could a character starting with byte b be accepted by one of the stacks?
 * (A token may end inside a character.) */
static int could_start(const frame *const *in, int nin, uint8_t b) {
    uint32_t lo = b < 0xe0 ? 0x80 : b < 0xf0 ? 0x800 : 0x10000, hi = b < 0xe0 ? 0x7ff : b < 0xf0 ? 0xffff : 0x10ffff;
    for (int i = 0; i < nin; i++) {
        if (!in[i]) continue;
        const gnode *c = in[i]->node;
        if (c->negated) return 1;
        for (int k = 0; k < c->n; k++) if (c->hi[k] >= lo && c->lo[k] <= hi) return 1;
    }
    return 0;
}

void sk_grammar_reset(sk_grammar *g) {
    if (!g->arena) {
        g->cap = 1u << 20;
        g->arena = malloc(g->cap * sizeof *g->arena);
        g->cur = malloc(G_MAX_STACKS * sizeof *g->cur);
        if (!g->arena || !g->cur) { g->ncur = -1; return; }
    }
    g->used = 0;
    g->npart = 0;
    const frame *root = push(g, g->rules[g->root], 0, NULL);
    g->ncur = 0;
    if (expand(g, root, g->cur, &g->ncur, 0)) g->ncur = -1;
}

/* Feed `n` bytes to the stacks in `st` (count *ns), with the partial character
 * `part` (count *np) carried; the new stacks into `st`. 1 when every byte is
 * taken, 0 when not. `scratch` holds G_MAX_STACKS stacks. */
static int feed(sk_grammar *g, const frame **st, int *ns, uint8_t *part, int *np, const uint8_t *s, size_t n,
                const frame **scratch) {
    for (size_t i = 0; i < n; i++) {
        part[(*np)++] = s[i];
        uint8_t b0 = part[0];
        int need = b0 < 0x80 ? 1 : (b0 >> 5) == 6 ? 2 : (b0 >> 4) == 14 ? 3 : (b0 >> 3) == 30 ? 4 : 0;
        if (!need) return 0;
        if (*np < need) continue;
        uint32_t cp = need == 1 ? b0 : need == 2 ? b0 & 0x1f : need == 3 ? b0 & 0x0f : b0 & 0x07;
        for (int k = 1; k < need; k++) {
            if ((part[k] & 0xc0) != 0x80) return 0;
            cp = cp << 6 | (part[k] & 0x3f);
        }
        *np = 0;
        int k = step(g, st, *ns, cp, scratch);
        if (k <= 0) return 0;
        memcpy(st, scratch, (size_t)k * sizeof *st);
        *ns = k;
    }
    return *np == 0 || could_start(st, *ns, part[0]);
}

int sk_grammar_allows(sk_grammar *g, const char *bytes, size_t n) {
    if (g->ncur <= 0) return 0;
    const frame *st[G_MAX_STACKS], *scr[G_MAX_STACKS];
    uint8_t part[4];
    int ns = g->ncur, np = g->npart;
    memcpy(st, g->cur, (size_t)ns * sizeof *st);
    memcpy(part, g->part, sizeof part);
    size_t mark = g->used;
    int ok = feed(g, st, &ns, part, &np, (const uint8_t *)bytes, n, scr);
    g->used = mark;                                  /* a candidate leaves nothing behind */
    return ok;
}

int sk_grammar_accepting(const sk_grammar *g) {
    if (g->npart) return 0;
    for (int i = 0; i < g->ncur; i++) if (!g->cur[i]) return 1;
    return 0;
}

int sk_grammar_accept(sk_grammar *g, const char *bytes, size_t n) {
    if (g->ncur <= 0) return -1;
    const frame *scr[G_MAX_STACKS];
    if (!feed(g, g->cur, &g->ncur, g->part, &g->npart, (const uint8_t *)bytes, n, scr)) { g->ncur = 0; return -1; }
    /* keep the arena small: copy the live stacks' frames to its start when it
     * is past half full */
    if (g->used > g->cap / 2) {
        frame *fresh = malloc(g->cap * sizeof *fresh);
        if (!fresh) return -1;
        size_t at = 0;
        for (int i = 0; i < g->ncur; i++) {
            /* copy the whole chain (shared tails are copied once per stack: the
             * stacks are few and short) */
            const frame *src = g->cur[i];
            const frame *chain[4096];
            int d = 0;
            while (src && d < 4096) { chain[d++] = src; src = src->next; }
            const frame *next = NULL;
            for (int k = d - 1; k >= 0; k--) {
                fresh[at] = *chain[k];
                fresh[at].next = next;
                next = &fresh[at++];
            }
            g->cur[i] = next;
        }
        free(g->arena);
        g->arena = fresh;
        g->used = at;
    }
    return 0;
}
