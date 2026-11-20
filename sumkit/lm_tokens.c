/* The summary model's tokenizer: byte-level BPE as Qwen's tokenizer.json
 * describes it (sumkit.h, "the tokenizer"). Text is split first at the added
 * tokens (the chat format's), each other piece NFC-normalised and cut by Qwen's
 * pre-tokenizer pattern, written out here by hand:
 *
 *   (?i:'s|'t|'re|'ve|'m|'ll|'d) | [^\r\n\p{L}\p{N}]?[\p{L}\p{M}]+ | \p{N} |
 *    ?[^\s\p{L}\p{M}\p{N}]+[\r\n]* | \s*[\r\n]+ | \s+(?!\S) | \s+
 *
 * tried in that order at each position, as the regex does. Each piece's bytes
 * start as one token a byte and are merged by rank, lowest first. */
#include "sk_priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utf8proc.h"

struct sk_tokenizer {
    const uint8_t *bytes;
    const int32_t *offs;
    uint32_t       n_tok;
    int32_t        byte_tok[256];
    /* merges: (left, right) -> rank and result, open addressing */
    struct mslot { int32_t l, r, rank, res; } *m;
    size_t         mcap;
    const int32_t *added;
    uint32_t       n_added;
};

static size_t mhash(int32_t l, int32_t r, size_t cap) {
    uint64_t h = (uint64_t)(uint32_t)l * 0x9E3779B97F4A7C15ull ^ (uint64_t)(uint32_t)r * 0xC2B2AE3D27D4EB4Full;
    return (size_t)(h ^ h >> 29) & (cap - 1);
}

sk_tokenizer *sk_tokenizer_new(const sk_file *f, char *err, size_t errcap) {
    const sk_config *c = sk_config_of(f);
    const sk_tensor *tb = sk_find(f, "tok.bytes"), *to = sk_find(f, "tok.offs"), *tm = sk_find(f, "tok.merges"),
                    *ta = sk_find(f, "tok.added");
    if (!tb || !to || !tm) { sk_seterr(err, errcap, "the model has no tokenizer"); return NULL; }
    sk_tokenizer *t = calloc(1, sizeof *t);
    if (!t) { sk_seterr(err, errcap, "out of memory"); return NULL; }
    t->bytes = tb->data;
    t->offs = to->data;
    t->n_tok = c->n_tok;
    t->added = ta ? ta->data : NULL;
    t->n_added = ta ? ta->dims[0] : 0;
    for (int b = 0; b < 256; b++) t->byte_tok[b] = -1;
    for (uint32_t i = 0; i < t->n_tok; i++)
        if (t->offs[i + 1] - t->offs[i] == 1 && t->byte_tok[t->bytes[t->offs[i]]] < 0) {
            int is_added = 0;
            for (uint32_t a = 0; a < t->n_added; a++) is_added |= t->added[a] == (int32_t)i;
            if (!is_added) t->byte_tok[t->bytes[t->offs[i]]] = (int32_t)i;
        }
    for (int b = 0; b < 256; b++)
        if (t->byte_tok[b] < 0) { free(t); sk_seterr(err, errcap, "the tokenizer has no token for byte %d", b); return NULL; }
    t->mcap = 1;
    while (t->mcap < 2 * (size_t)c->n_merges + 16) t->mcap <<= 1;
    t->m = malloc(t->mcap * sizeof *t->m);
    if (!t->m) { free(t); sk_seterr(err, errcap, "out of memory"); return NULL; }
    for (size_t i = 0; i < t->mcap; i++) t->m[i].l = -1;
    const int32_t *mv = tm->data;
    for (uint32_t k = 0; k < c->n_merges; k++) {
        int32_t l = mv[3 * k], r = mv[3 * k + 1];
        size_t i = mhash(l, r, t->mcap);
        while (t->m[i].l >= 0 && !(t->m[i].l == l && t->m[i].r == r)) i = (i + 1) & (t->mcap - 1);
        if (t->m[i].l < 0) t->m[i] = (struct mslot){ l, r, (int32_t)k, mv[3 * k + 2] };   /* the first rank wins */
    }
    return t;
}

void sk_tokenizer_free(sk_tokenizer *t) {
    if (!t) return;
    free(t->m);
    free(t);
}

static const struct mslot *merge_of(const sk_tokenizer *t, int32_t l, int32_t r) {
    size_t i = mhash(l, r, t->mcap);
    while (t->m[i].l >= 0) {
        if (t->m[i].l == l && t->m[i].r == r) return &t->m[i];
        i = (i + 1) & (t->mcap - 1);
    }
    return NULL;
}

/* A growing array of token ids. */
typedef struct { int32_t *v; size_t n, cap; int oom; } ids;
static void push(ids *o, int32_t id) {
    if (o->n == o->cap) {
        size_t nc = o->cap ? 2 * o->cap : 256;
        int32_t *nv = realloc(o->v, nc * sizeof *nv);
        if (!nv) { o->oom = 1; return; }
        o->v = nv;
        o->cap = nc;
    }
    o->v[o->n++] = id;
}

/* One pre-tokenizer piece: bytes to tokens, merged by rank. */
static void bpe(const sk_tokenizer *t, const uint8_t *s, size_t n, ids *o) {
    int32_t stack[64], *sym = n <= 64 ? stack : malloc(n * sizeof *sym);
    if (!sym) { o->oom = 1; return; }
    for (size_t i = 0; i < n; i++) sym[i] = t->byte_tok[s[i]];
    size_t len = n;
    for (;;) {
        int32_t best = -1;
        size_t at = 0;
        for (size_t i = 0; i + 1 < len; i++) {
            const struct mslot *m = merge_of(t, sym[i], sym[i + 1]);
            if (m && (best < 0 || m->rank < best)) { best = m->rank; at = i; }
        }
        if (best < 0) break;
        /* every occurrence of that pair, left to right */
        int32_t l = sym[at], r = sym[at + 1], res = merge_of(t, l, r)->res;
        size_t w = 0;
        for (size_t i = 0; i < len; i++) {
            if (i + 1 < len && sym[i] == l && sym[i + 1] == r) { sym[w++] = res; i++; }
            else sym[w++] = sym[i];
        }
        len = w;
    }
    for (size_t i = 0; i < len; i++) push(o, sym[i]);
    if (sym != stack) free(sym);
}

/* ---- the pre-tokenizer's classes ---------------------------------------------- */

static int is_letter(int32_t c) {
    utf8proc_category_t k = utf8proc_category(c);
    return k >= UTF8PROC_CATEGORY_LU && k <= UTF8PROC_CATEGORY_LO;
}
static int is_mark(int32_t c) {
    utf8proc_category_t k = utf8proc_category(c);
    return k >= UTF8PROC_CATEGORY_MN && k <= UTF8PROC_CATEGORY_ME;
}
static int is_number(int32_t c) {
    utf8proc_category_t k = utf8proc_category(c);
    return k >= UTF8PROC_CATEGORY_ND && k <= UTF8PROC_CATEGORY_NO;
}
/* \s: Unicode's White_Space. */
static int is_space(int32_t c) {
    return (c >= 9 && c <= 13) || c == 32 || c == 0x85 || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) ||
           c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}
static int is_nl(int32_t c) { return c == '\r' || c == '\n'; }

/* The text as code points with each one's byte offset (n + 1 offsets). */
typedef struct { int32_t *cp; size_t *at; size_t n; } cps;

static int decode(const uint8_t *s, size_t n, cps *o) {
    o->cp = malloc((n + 1) * sizeof *o->cp);
    o->at = malloc((n + 1) * sizeof *o->at);
    if (!o->cp || !o->at) return -1;
    o->n = 0;
    size_t i = 0;
    while (i < n) {
        utf8proc_int32_t c;
        utf8proc_ssize_t k = utf8proc_iterate(s + i, (utf8proc_ssize_t)(n - i), &c);
        if (k <= 0) { c = s[i]; k = 1; }          /* a stray byte stands for itself */
        o->cp[o->n] = c;
        o->at[o->n++] = i;
        i += (size_t)k;
    }
    o->at[o->n] = n;
    return 0;
}

static int lower_ascii(int32_t c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* The length in code points of the piece the pattern matches at p (always >= 1). */
static size_t piece(const cps *t, size_t p) {
    const int32_t *c = t->cp;
    size_t n = t->n;
    /* (?i:'s|'t|'re|'ve|'m|'ll|'d) */
    if (c[p] == '\'' && p + 1 < n) {
        int a = lower_ascii(c[p + 1]), b = p + 2 < n ? lower_ascii(c[p + 2]) : 0;
        if ((a == 'r' && b == 'e') || (a == 'v' && b == 'e') || (a == 'l' && b == 'l')) return 3;
        if (a == 's' || a == 't' || a == 'm' || a == 'd') return 2;
    }
    /* [^\r\n\p{L}\p{N}]?[\p{L}\p{M}]+ */
    {
        size_t q = p;
        if (!is_nl(c[q]) && !is_letter(c[q]) && !is_number(c[q]) && q + 1 < n && (is_letter(c[q + 1]) || is_mark(c[q + 1]))) q++;
        if (is_letter(c[q]) || is_mark(c[q])) {
            while (q < n && (is_letter(c[q]) || is_mark(c[q]))) q++;
            return q - p;
        }
    }
    /* \p{N} */
    if (is_number(c[p])) return 1;
    /*  ?[^\s\p{L}\p{M}\p{N}]+[\r\n]* */
    {
        size_t q = p + (c[p] == ' ');
        size_t r = q;
        while (r < n && !is_space(c[r]) && !is_letter(c[r]) && !is_mark(c[r]) && !is_number(c[r])) r++;
        if (r > q) {
            while (r < n && is_nl(c[r])) r++;
            return r - p;
        }
    }
    /* \s*[\r\n]+ : through the last newline of the run of white space */
    {
        size_t r = p, last = 0;
        int seen = 0;
        while (r < n && is_space(c[r])) {
            if (is_nl(c[r])) { last = r; seen = 1; }
            r++;
        }
        if (seen) return last + 1 - p;
        /* \s+(?!\S) : the run, less its last character when one follows */
        if (r > p) {
            if (r == n) return r - p;
            if (r - p > 1) return r - p - 1;
            return 1;                               /* \s+ */
        }
    }
    return 1;   /* nothing else matches a lone character: it is its own piece */
}

static void encode_plain(const sk_tokenizer *t, const uint8_t *s, size_t n, ids *o) {
    if (!n) return;
    /* utf8proc_NFC wants a NUL-terminated string: a copy of the piece. */
    char *tmp = malloc(n + 1);
    if (!tmp) { o->oom = 1; return; }
    memcpy(tmp, s, n);
    tmp[n] = 0;
    uint8_t *norm = utf8proc_NFC((const utf8proc_uint8_t *)tmp);
    free(tmp);
    const uint8_t *src = norm ? norm : s;
    size_t len = norm ? strlen((const char *)norm) : n;
    cps c;
    if (decode(src, len, &c)) { o->oom = 1; free(norm); free(c.cp); free(c.at); return; }
    for (size_t p = 0; p < c.n;) {
        size_t k = piece(&c, p);
        bpe(t, src + c.at[p], c.at[p + k] - c.at[p], o);
        p += k;
    }
    free(c.cp);
    free(c.at);
    free(norm);
}

long sk_tokenize(const sk_tokenizer *t, const char *text, int32_t **out) {
    ids o = { 0 };
    const uint8_t *s = (const uint8_t *)text;
    size_t n = strlen(text), start = 0, i = 0;
    while (i < n) {
        /* the longest added token that starts here */
        int32_t best = -1;
        size_t blen = 0;
        for (uint32_t a = 0; a < t->n_added; a++) {
            int32_t id = t->added[a];
            size_t l = (size_t)(t->offs[id + 1] - t->offs[id]);
            if (l > blen && l <= n - i && !memcmp(s + i, t->bytes + t->offs[id], l)) { best = id; blen = l; }
        }
        if (best < 0) { i++; continue; }
        encode_plain(t, s + start, i - start, &o);
        push(&o, best);
        i += blen;
        start = i;
    }
    encode_plain(t, s + start, n - start, &o);
    if (o.oom) { free(o.v); *out = NULL; return -1; }
    *out = o.v;
    return (long)o.n;
}

long sk_detok(const sk_tokenizer *t, const int32_t *tok, size_t n, char *out, size_t cap) {
    size_t w = 0;
    for (size_t i = 0; i < n; i++) {
        if (tok[i] < 0 || (uint32_t)tok[i] >= t->n_tok) continue;
        size_t l = (size_t)(t->offs[tok[i] + 1] - t->offs[tok[i]]);
        if (w + l + 1 > cap) return -1;
        memcpy(out + w, t->bytes + t->offs[tok[i]], l);
        w += l;
    }
    if (cap) out[w] = 0;
    return (long)w;
}

const char *sk_token_bytes(const sk_tokenizer *t, int32_t id, size_t *len) {
    if (id < 0 || (uint32_t)id >= t->n_tok) { *len = 0; return ""; }
    *len = (size_t)(t->offs[id + 1] - t->offs[id]);
    return (const char *)t->bytes + t->offs[id];
}

char *sk_chat_prompt(const char *system, const char *user) {
    static const char a[] = "<|im_start|>system\n", b[] = "<|im_end|>\n<|im_start|>user\n",
                      c[] = "<|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n";
    size_t n = strlen(a) + strlen(system) + strlen(b) + strlen(user) + strlen(c) + 1;
    char *p = malloc(n);
    if (!p) return NULL;
    snprintf(p, n, "%s%s%s%s%s", a, system, b, user, c);
    return p;
}
