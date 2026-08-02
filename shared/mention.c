/*
 * @mention scanning (REQ-221, ARCH-89). See mention.h.
 */

#include "mention.h"

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

size_t oc_mention_scan(const char *body, size_t len, oc_mention *out, size_t max) {
    size_t found = 0;
    if (!body) return 0;

    for (size_t i = 0; i < len; i++) {
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

int oc_mention_targets(const char *body, size_t len, const char *name) {
    oc_mention m[32];
    size_t n = oc_mention_scan(body, len, m, 32);
    if (n > 32) n = 32;
    for (size_t i = 0; i < n; i++) {
        if (m[i].kind != OC_MENTION_USER) return 1;      /* any broadcast hits */
        if (name && name[0] && ieq(m[i].name, name)) return 1;
    }
    return 0;
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
