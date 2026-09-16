/* A message as it is read aloud (speakable.h). */
#include "speakable.h"
#include "mention.h"
#include "richtext.h"
#include "url.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct { char *p; size_t n, cap; } sbuf;

static void put(sbuf *b, const char *s, size_t n) {
    if (b->n >= b->cap) return;
    if (n > b->cap - 1 - b->n) n = b->cap - 1 - b->n;
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = '\0';
}
static void puts_(sbuf *b, const char *s) { put(b, s, strlen(s)); }

static int ends_sentence(const sbuf *b) {
    for (size_t i = b->n; i > 0; i--) {
        char c = b->p[i - 1];
        if (c == ' ') continue;
        return c == '.' || c == '!' || c == '?' || c == ':' || c == ';';
    }
    return 1;                          /* nothing yet: no full stop needed */
}

/* A space, unless there is one already or nothing has been said. */
static void space(sbuf *b) {
    if (b->n && b->p[b->n - 1] != ' ') put(b, " ", 1);
}

/* End a sentence at a line break or before an announcement. */
static void full_stop(sbuf *b) {
    while (b->n && b->p[b->n - 1] == ' ') b->p[--b->n] = '\0';
    if (b->n && !ends_sentence(b)) put(b, ".", 1);
    space(b);
}

/* Bytes that begin an emoji (or a pictograph) are dropped with their sequence:
 * U+2190–U+2BFF (arrows, symbols, dingbats), U+1F000 and up, the variation
 * selector U+FE0F and the zero-width joiner. Decoded just enough to say so. */
static size_t emoji_len(const unsigned char *s, size_t n) {
    uint32_t cp; size_t len;
    if (s[0] < 0x80) return 0;
    if ((s[0] & 0xE0) == 0xC0 && n >= 2) { cp = (uint32_t)(s[0] & 0x1F) << 6 | (s[1] & 0x3F); len = 2; }
    else if ((s[0] & 0xF0) == 0xE0 && n >= 3) { cp = (uint32_t)(s[0] & 0x0F) << 12 | (uint32_t)(s[1] & 0x3F) << 6 | (s[2] & 0x3F); len = 3; }
    else if ((s[0] & 0xF8) == 0xF0 && n >= 4) { cp = (uint32_t)(s[0] & 0x07) << 18 | (uint32_t)(s[1] & 0x3F) << 12 | (uint32_t)(s[2] & 0x3F) << 6 | (s[3] & 0x3F); len = 4; }
    else return 0;
    if ((cp >= 0x2190 && cp <= 0x2BFF) || cp >= 0x1F000 || cp == 0xFE0F || cp == 0x200D) return len;
    return 0;
}

static int is_word_byte(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '+';
}

/* The host of a URL span: after "scheme://", up to the first '/', ':', '?' or '#',
 * without a leading "www.". */
static void say_host(sbuf *b, const char *u, size_t n) {
    const char *p = u, *end = u + n;
    const char *sep = NULL;
    for (const char *q = u; q + 2 < end; q++) if (q[0] == ':' && q[1] == '/' && q[2] == '/') { sep = q + 3; break; }
    if (sep) p = sep;
    if (end - p > 4 && strncmp(p, "www.", 4) == 0) p += 4;
    const char *h = p;
    while (h < end && *h != '/' && *h != ':' && *h != '?' && *h != '#') h++;
    space(b);
    put(b, p, (size_t)(h - p));
    space(b);
}

size_t oc_speakable(const char *body, size_t len, oc_speak_name_fn resolve, void *ctx,
                    char *out, size_t cap) {
    if (!out || cap == 0) return 0;
    out[0] = '\0';
    if (!body || len == 0) return 0;
    if (cap > OC_SPEAK_MAX + 1) cap = OC_SPEAK_MAX + 1;
    sbuf b = { out, 0, cap };

    oc_rt_span spans[OC_RT_MAX];
    size_t nsp = oc_rt_scan(body, len, spans, OC_RT_MAX);
    if (nsp > OC_RT_MAX) nsp = OC_RT_MAX;
    oc_mention ments[OC_MENTION_MAX];
    size_t nm = oc_mention_scan(body, len, ments, OC_MENTION_MAX);
    if (nm > OC_MENTION_MAX) nm = OC_MENTION_MAX;

    int in_quote = 0;
    size_t i = 0;
    while (i < len && b.n + 1 < b.cap) {
        /* Constructs starting here, outermost first. */
        int handled = 0;
        for (size_t k = 0; k < nsp && !handled; k++) {
            const oc_rt_span *s = &spans[k];
            if (s->start != i) continue;
            uint16_t st = s->style;
            if (st & OC_RT_DELIM) {
                /* A delimiter, a list marker or an escape: not said. The quote
                 * marker introduces a quote once. */
                if ((st & OC_RT_QUOTE) && !in_quote) { full_stop(&b); puts_(&b, "Quote: "); in_quote = 1; }
                i = s->start + s->len;
                handled = 1;
            } else if (st & OC_RT_CODEBLOCK) {
                /* The span is the content between the fences, which are their
                 * own delimiter spans: its lines are its newlines plus one. */
                size_t lines = 1;
                for (size_t q = s->start; q < s->start + s->len; q++) if (body[q] == '\n') lines++;
                char ann[48];
                snprintf(ann, sizeof ann, "Code block, %zu line%s.", lines, lines == 1 ? "" : "s");
                full_stop(&b);
                puts_(&b, ann);
                space(&b);
                i = s->start + s->len;
                handled = 1;
            } else if (st & OC_RT_LINK) {
                say_host(&b, body + s->start, s->len);
                i = s->start + s->len;
                handled = 1;
            }
        }
        if (handled) continue;

        for (size_t k = 0; k < nm && !handled; k++) {
            const oc_mention *m = &ments[k];
            if (m->start != i) continue;
            space(&b);
            if (m->kind == OC_MENTION_HERE) puts_(&b, "everyone here");
            else if (m->kind == OC_MENTION_CHANNEL || m->kind == OC_MENTION_EVERYONE) puts_(&b, "everyone");
            else {
                const char *said = resolve ? resolve(ctx, m->name) : NULL;
                puts_(&b, said && said[0] ? said : m->name);
            }
            i = m->start + m->len;
            handled = 1;
        }
        if (handled) continue;

        unsigned char c = (unsigned char)body[i];
        if (c == '\n') {
            full_stop(&b);
            /* A quote lasts until a line that does not begin with the marker. */
            size_t j = i + 1;
            while (j < len && (body[j] == ' ' || body[j] == '\t')) j++;
            if (!(j < len && body[j] == '>')) in_quote = 0;
            i++;
            continue;
        }
        if (c == '\r' || c == '\t' || c == ' ') { space(&b); i++; continue; }
        /* :shortcode: */
        if (c == ':' && i + 1 < len && is_word_byte((unsigned char)body[i + 1])) {
            size_t j = i + 1;
            while (j < len && is_word_byte((unsigned char)body[j])) j++;
            if (j < len && body[j] == ':' && j - i >= 2) { i = j + 1; continue; }
        }
        size_t el = emoji_len((const unsigned char *)body + i, len - i);
        if (el) { i += el; continue; }
        put(&b, body + i, 1);
        i++;
    }
    while (b.n && b.p[b.n - 1] == ' ') b.p[--b.n] = '\0';

    /* Nothing to say unless a letter or digit survived. */
    int any = 0;
    for (size_t k = 0; k < b.n && !any; k++) {
        unsigned char c = (unsigned char)b.p[k];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c >= 0x80) any = 1;
    }
    if (!any) { out[0] = '\0'; return 0; }
    return b.n;
}

int oc_speakable_segments(const char *text, size_t len, size_t max_chars,
                          size_t *starts, size_t *lens, int max) {
    int n = 0;
    size_t at = 0;
    if (max_chars < 16) max_chars = 16;
    while (at < len && n < max) {
        while (at < len && text[at] == ' ') at++;
        if (at >= len) break;
        size_t limit = at + max_chars < len ? at + max_chars : len;
        size_t cut = limit;
        if (limit < len) {
            /* Last sentence end within the limit; else the last space; else hard. */
            size_t best = 0;
            for (size_t q = at; q < limit; q++)
                if ((text[q] == '.' || text[q] == '!' || text[q] == '?') && (q + 1 == len || text[q + 1] == ' ')) best = q + 1;
            if (!best) for (size_t q = limit; q > at; q--) if (text[q - 1] == ' ') { best = q - 1; break; }
            if (best > at) cut = best;
        }
        size_t e = cut;
        while (e > at && text[e - 1] == ' ') e--;
        if (e > at) { starts[n] = at; lens[n] = e - at; n++; }
        at = cut;
    }
    return n;
}
