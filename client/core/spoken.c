/* What a message says to a screen reader (REQ-269, ARCH-99). See spoken.h. */
#include "spoken.h"

#include <stdio.h>
#include <string.h>

oc_spoken_opts oc_spoken_defaults(void) {
    oc_spoken_opts o = { 1, 1, 0, 0, 0 };
    return o;
}

unsigned oc_spoken_bits(const oc_spoken_opts *o) {
    return (o->sender ? 1u : 0u) | (o->time ? 2u : 0u) | (o->reactions ? 4u : 0u) |
           (o->replies ? 8u : 0u) | (o->text_first ? 16u : 0u);
}

oc_spoken_opts oc_spoken_from_bits(unsigned bits) {
    oc_spoken_opts o = { (bits & 1u) != 0, (bits & 2u) != 0, (bits & 4u) != 0,
                         (bits & 8u) != 0, (bits & 16u) != 0 };
    return o;
}

/* An output buffer that keeps whole UTF-8 characters: once something does not
 * fit, what of it did is cut back to the last character boundary and nothing
 * more is written. */
typedef struct { char *p; size_t cap, len; int full; } sbuf;

static void put(sbuf *b, const char *s) {
    if (b->full || !s) return;
    size_t n = strlen(s), room = b->cap - 1 - b->len;
    if (n <= room) { memcpy(b->p + b->len, s, n); b->len += n; b->p[b->len] = '\0'; return; }
    /* Back off to a boundary: never end on a lead byte or inside a sequence. */
    size_t k = room;
    while (k > 0 && ((unsigned char)s[k] & 0xC0) == 0x80) k--;
    memcpy(b->p + b->len, s, k);
    b->len += k;
    b->p[b->len] = '\0';
    b->full = 1;
}

size_t oc_spoken_msg(const oc_spoken_opts *o, const char *who, const char *when,
                     const char *text, const oc_reaction *r, int n_r,
                     uint32_t replies, int deleted, char *out, size_t cap) {
    if (!out || cap == 0) return 0;
    out[0] = '\0';
    oc_spoken_opts d = oc_spoken_defaults();
    if (!o) o = &d;
    sbuf b = { out, cap, 0, 0 };
    const char *name = (who && who[0]) ? who : "someone";
    const char *body = deleted ? "message deleted" : (text ? text : "");

    /* The sender and the time, as one run: "<who>, <when>". */
    char head[256];
    head[0] = '\0';
    if (o->sender && o->time) snprintf(head, sizeof head, "%s, %s", name, when ? when : "");
    else if (o->sender)       snprintf(head, sizeof head, "%s", name);
    else if (o->time)         snprintf(head, sizeof head, "%s", when ? when : "");
    int has_head = o->sender || o->time;

    if (o->text_first) {
        put(&b, body);
        if (has_head) { put(&b, ", "); put(&b, head); }
    } else if (has_head) {
        put(&b, head);
        put(&b, deleted ? ", " : ": ");
        put(&b, body);
    } else {
        put(&b, body);
    }

    if (o->reactions && r && n_r > 0) {
        put(&b, ", reactions: ");
        for (int i = 0; i < n_r; i++) {
            char one[64];
            snprintf(one, sizeof one, "%s%s %u", i ? ", " : "", r[i].emoji, (unsigned)r[i].count);
            put(&b, one);
        }
    }
    if (o->replies && replies > 0) {
        char rp[40];
        if (replies == 1) snprintf(rp, sizeof rp, ", 1 reply");
        else              snprintf(rp, sizeof rp, ", %u replies", (unsigned)replies);
        put(&b, rp);
    }
    return b.len;
}
