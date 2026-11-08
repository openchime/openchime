/*
 * @mention scanning (REQ-221, ARCH-89). See mention.h.
 */

#include "mention.h"

#include <stdlib.h>
#include <string.h>

static int name_byte(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
}

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static int ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) if (lower(*a) != lower(*b)) return 0;
    return *a == *b;
}

/* The id form at `i`, `<@U<digits>>` or `<@U<digits>|label>` (mention.h).
 * Returns the token's length when it is one, filling `id` and the label's
 * offset and length; 0 when it is not. `*attempt` says whether it began like
 * one (`<@U` and a digit, '>' or '|'), so a broken one is no mention at all. */
static size_t id_form(const char *b, size_t len, size_t i, int *attempt,
                      uint64_t *id, size_t *lab, size_t *lab_len) {
    *attempt = 0;
    if (i + 3 >= len || b[i] != '<' || b[i + 1] != '@' || b[i + 2] != 'U') return 0;
    char c = b[i + 3];
    if (!((c >= '0' && c <= '9') || c == '>' || c == '|')) return 0;
    *attempt = 1;
    size_t j = i + 3, nd = 0;
    uint64_t v = 0;
    while (j < len && b[j] >= '0' && b[j] <= '9') {
        unsigned d = (unsigned)(b[j] - '0');
        if (++nd > 20 || v > (UINT64_MAX - d) / 10) return 0;
        v = v * 10 + d;
        j++;
    }
    if (nd == 0 || v == 0 || j >= len) return 0;
    *lab = *lab_len = 0;
    if (b[j] == '|') {
        size_t s = ++j;
        while (j < len && b[j] != '>' && b[j] != '\n') j++;
        if (j >= len || b[j] != '>' || j - s >= OC_MENTION_NAME_MAX) return 0;
        *lab = s;
        *lab_len = j - s;
    }
    if (b[j] != '>') return 0;
    *id = v;
    return j + 1 - i;
}

size_t oc_mention_scan(const char *body, size_t len, oc_mention *out, size_t max) {
    size_t found = 0;
    if (!body) return 0;

    for (size_t i = 0; i < len; i++) {
        if (body[i] == '<') {
            int attempt;
            uint64_t id;
            size_t lab, lab_len, L = id_form(body, len, i, &attempt, &id, &lab, &lab_len);
            if (L) {
                if (found < max) {
                    oc_mention *m = &out[found];
                    m->start = i;
                    m->len = L;
                    m->kind = OC_MENTION_USER;
                    m->user_id = id;
                    memcpy(m->name, body + lab, lab_len);
                    m->name[lab_len] = '\0';
                }
                found++;
                i += L - 1;
            } else if (attempt) {
                i += 2;    /* past "<@U": its '@' starts no mention by name either */
            }
            continue;
        }
        if (body[i] != '@') continue;
        /* Word boundary: an '@' glued to the end of a word is an address or a
         * handle inside a word, not a mention of anybody. */
        if (i > 0 && name_byte((unsigned char)body[i - 1])) continue;

        size_t j = i + 1;
        while (j < len && name_byte((unsigned char)body[j])) j++;
        /* Trailing punctuation belongs to the sentence, not the name. */
        while (j > i + 1) {
            char c = body[j - 1];
            if (c == '.' || c == '-' || c == '_') j--; else break;
        }
        size_t nlen = j - (i + 1);
        if (nlen == 0 || nlen >= OC_MENTION_NAME_MAX) { i = j > i ? j - 1 : i; continue; }

        if (found < max) {
            oc_mention *m = &out[found];
            m->start = i;
            m->len   = nlen + 1;
            m->user_id = 0;
            memcpy(m->name, body + i + 1, nlen);
            m->name[nlen] = '\0';
            m->kind = ieq(m->name, "here")     ? OC_MENTION_HERE
                    : ieq(m->name, "channel")  ? OC_MENTION_CHANNEL
                    : ieq(m->name, "everyone") ? OC_MENTION_EVERYONE
                                               : OC_MENTION_USER;
        }
        found++;
        i = j - 1;   /* continue after this mention */
    }
    return found;
}

size_t oc_chanref_scan(const char *body, size_t len, oc_chanref *out, size_t max) {
    size_t found = 0;
    if (!body) return 0;

    for (size_t i = 0; i < len; i++) {
        if (body[i] != '#') continue;
        /* The same word boundary the '@' rule uses, and it earns its keep on a
         * different case: "C#" and an issue number written "oc#12" are not
         * references to a channel. */
        if (i > 0 && name_byte((unsigned char)body[i - 1])) continue;
        /* A heading is not a reference either. Markdown (REQ-220) gives '#' at
         * the start of a line to the heading, and "# Notes" would otherwise be a
         * reference to a channel called nothing at all -- the space already
         * ends the name, but a run of them ("### Notes") would leave "##" as a
         * name byte for the next pass rather than a heading. */
        if (i + 1 < len && (body[i + 1] == '#' || body[i + 1] == ' ')) continue;

        size_t j = i + 1;
        while (j < len && name_byte((unsigned char)body[j])) j++;
        while (j > i + 1) {
            char c = body[j - 1];
            if (c == '.' || c == '-' || c == '_') j--; else break;
        }
        size_t nlen = j - (i + 1);
        if (nlen == 0 || nlen >= OC_MENTION_NAME_MAX) { i = j > i ? j - 1 : i; continue; }

        if (found < max) {
            oc_chanref *c = &out[found];
            c->start = i;
            c->len   = nlen + 1;
            memcpy(c->name, body + i + 1, nlen);
            c->name[nlen] = '\0';
        }
        found++;
        i = j - 1;
    }
    return found;
}

int oc_mention_targets(const char *body, size_t len, const char *name, uint64_t user_id) {
    oc_mention m[32];
    size_t n = oc_mention_scan(body, len, m, 32);
    if (n > 32) n = 32;
    for (size_t i = 0; i < n; i++) {
        if (m[i].kind != OC_MENTION_USER) return 1;      /* any broadcast hits */
        if (m[i].user_id) { if (user_id && m[i].user_id == user_id) return 1; continue; }
        if (name && name[0] && ieq(m[i].name, name)) return 1;
    }
    return 0;
}

char *oc_mention_display(const char *body, size_t len, oc_mention_name_fn resolve, void *ctx) {
    if (!body || !resolve || len < 5 || !memchr(body, '<', len)) return NULL;
    size_t n = oc_mention_scan(body, len, NULL, 0);
    if (!n) return NULL;
    oc_mention *m = malloc(n * sizeof *m);
    if (!m) return NULL;
    oc_mention_scan(body, len, m, n);
    /* What each id becomes, measured first: a name is any length. */
    size_t cap = len + 1, hits = 0;
    for (size_t k = 0; k < n; k++) {
        const char *nm = m[k].user_id ? resolve(ctx, m[k].user_id) : NULL;
        if (nm && nm[0]) { cap += 1 + strlen(nm); hits++; }
    }
    char *out = hits ? malloc(cap) : NULL;
    if (!out) { free(m); return NULL; }
    size_t i = 0, o = 0;
    for (size_t k = 0; k < n; k++) {
        const char *nm = m[k].user_id ? resolve(ctx, m[k].user_id) : NULL;
        if (!nm || !nm[0]) continue;
        size_t nl = strlen(nm);
        if (o + (m[k].start - i) + 1 + nl >= cap) break;   /* a resolver that changed its answer */
        memcpy(out + o, body + i, m[k].start - i);
        o += m[k].start - i;
        out[o++] = '@';
        memcpy(out + o, nm, nl);
        o += nl;
        i = m[k].start + m[k].len;
    }
    size_t rest = len - i;
    if (o + rest >= cap) rest = cap - 1 - o;
    memcpy(out + o, body + i, rest);
    o += rest;
    out[o] = '\0';
    free(m);
    return out;
}

/* --- keyword matching (REQ-135, ARCH-103) --------------------------------- */

static int kw_wordish(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

int oc_keyword_match(const char *body, size_t len, const char *term,
                     size_t *span_start, size_t *span_len) {
    if (!body || !term || !*term) return 0;
    size_t tl = strlen(term);
    if (!tl || tl > len) return 0;
    for (size_t i = 0; i + tl <= len; i++) {
        /* Boundary BEFORE: the byte to the left must not be word-ish, or the
         * term is the tail of a longer word ("redeploy" must not match). */
        if (i && kw_wordish((unsigned char)body[i - 1])) continue;
        size_t k = 0;
        while (k < tl) {
            unsigned char a = (unsigned char)body[i + k], b = (unsigned char)term[k];
            if (a >= 'A' && a <= 'Z') a = (unsigned char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (unsigned char)(b - 'A' + 'a');
            if (a != b) break;
            k++;
        }
        if (k != tl) continue;
        /* ...and AFTER, which is what stops "deploy" matching "deployment". */
        if (i + tl < len && kw_wordish((unsigned char)body[i + tl])) continue;
        if (span_start) *span_start = i;
        if (span_len)   *span_len   = tl;
        return 1;
    }
    return 0;
}
