/* Spoken mentions (stt_mentions.h). */
#include "stt_mentions.h"

#include <string.h>
#include <strings.h>

/* The characters a mention's name may hold (shared/mention.h). */
static int name_char(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '.' || c == '_' || c == '-';
}

static int word_char(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c >= 0x80;
}

/* The one member called `w` (len bytes, case-insensitively), or NULL if none is
 * or more than one is. A name holding a character a mention cannot is skipped:
 * written as "@name" it would not be found by the scanner. */
static const char *only_member(const char *w, size_t len, const char *const *names, size_t n) {
    const char *hit = NULL;
    for (size_t i = 0; i < n; i++) {
        const char *nm = names[i];
        if (!nm || strlen(nm) != len || strncasecmp(nm, w, len)) continue;
        int ok = 1;
        for (size_t k = 0; k < len; k++) if (!name_char((unsigned char)nm[k])) ok = 0;
        if (!ok) continue;
        if (hit && strcasecmp(hit, nm)) return NULL;   /* two different people */
        if (hit) return NULL;                          /* the same name twice */
        hit = nm;
    }
    return hit;
}

size_t oc_stt_mentions(const char *text, const char *const *names, size_t n_names, char *out, size_t cap) {
    size_t w = 0, len = strlen(text);
    if (!cap) return 0;
#define PUT(p, n) do { size_t _n = (n); if (w + _n >= cap) _n = cap - 1 - w; memcpy(out + w, (p), _n); w += _n; } while (0)
    for (size_t i = 0; i < len;) {
        int boundary = i == 0 || !word_char((unsigned char)text[i - 1]);
        if (boundary && i + 3 < len && (text[i] == 'a' || text[i] == 'A') && (text[i + 1] == 't' || text[i + 1] == 'T') &&
            text[i + 2] == ' ') {
            size_t s = i + 3;
            while (s < len && text[s] == ' ') s++;
            size_t e = s;
            while (e < len && name_char((unsigned char)text[e])) e++;
            /* Trailing '.', '-' and '_' end the sentence, not the name. */
            while (e > s && (text[e - 1] == '.' || text[e - 1] == '-' || text[e - 1] == '_')) e--;
            const char *who = e > s && (e == len || !word_char((unsigned char)text[e]))
                                  ? only_member(text + s, e - s, names, n_names) : NULL;
            if (who) {
                PUT("@", 1);
                PUT(who, strlen(who));
                i = e;
                continue;
            }
        }
        PUT(text + i, 1);
        i++;
    }
#undef PUT
    out[w] = '\0';
    return w;
}
