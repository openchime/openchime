/* Tests for what a message says to a screen reader (client/core/spoken.c,
 * REQ-269, ARCH-99): the defaults as the transcript has always read, each
 * switch on its own, reactions, replies, text first, and the cut. */
#include "check.h"
#include "spoken.h"

#include <string.h>

static char got[512];

/* 1 when the message, read under `o` into a buffer of `cap`, is exactly `want`. */
static int reads(const oc_spoken_opts *o, const char *who, const char *when, const char *text,
                 const oc_reaction *r, int n_r, uint32_t replies, int deleted, size_t cap,
                 const char *want) {
    memset(got, 'X', sizeof got);
    size_t n = oc_spoken_msg(o, who, when, text, r, n_r, replies, deleted, got, cap);
    int ok = strcmp(got, want) == 0 && n == strlen(want);
    if (!ok) printf("    read \"%s\" (%zu), wanted \"%s\"\n", got, n, want);
    return ok;
}
#define READS(o, who, when, text, r, n_r, rep, del, want) \
    reads(o, who, when, text, r, n_r, rep, del, sizeof got, want)

/* 1 when `s` is whole UTF-8: no sequence cut short at its end. */
static int whole_utf8(const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        int n = *p < 0x80 ? 1 : (*p & 0xE0) == 0xC0 ? 2 : (*p & 0xF0) == 0xE0 ? 3 : (*p & 0xF8) == 0xF0 ? 4 : 0;
        if (!n) return 0;
        for (int i = 1; i < n; i++) if ((p[i] & 0xC0) != 0x80) return 0;
        p += n;
    }
    return 1;
}

int run_spoken_tests(void) {
    printf("test_spoken: the defaults, each switch, reactions, replies, text first, the cut\n");
    oc_spoken_opts d = oc_spoken_defaults();
    oc_reaction rx[3];
    memset(rx, 0, sizeof rx);
    snprintf(rx[0].emoji, sizeof rx[0].emoji, "\xF0\x9F\x91\x8D"); rx[0].count = 2;   /* thumbs up */
    snprintf(rx[1].emoji, sizeof rx[1].emoji, "tada");             rx[1].count = 1;
    snprintf(rx[2].emoji, sizeof rx[2].emoji, "\xE2\x9D\xA4");     rx[2].count = 5;   /* heart */

    /* The defaults: the sender and the time, exactly the two forms the
     * transcript has always said. Reactions and replies, given, say nothing. */
    CHECK(d.sender == 1 && d.time == 1 && d.reactions == 0 && d.replies == 0 && d.text_first == 0);
    CHECK(READS(&d, "Ada", "2m", "lunch?", NULL, 0, 0, 0, "Ada, 2m: lunch?"));
    CHECK(READS(&d, "Ada", "2m", "lunch?", rx, 3, 4, 0, "Ada, 2m: lunch?"));
    CHECK(READS(&d, "Ada", "2m", "lunch?", NULL, 0, 0, 1, "Ada, 2m, message deleted"));
    CHECK(READS(NULL, "Ada", "2m", "lunch?", NULL, 0, 0, 0, "Ada, 2m: lunch?"));
    /* A time with a colon in it is the time's, not a separator of its own. */
    CHECK(READS(&d, "Ada", "14:05", "hi", NULL, 0, 0, 0, "Ada, 14:05: hi"));

    /* Nobody named reads "someone", as it always has. */
    CHECK(READS(&d, NULL, "2m", "hi", NULL, 0, 0, 0, "someone, 2m: hi"));
    CHECK(READS(&d, "", "2m", "hi", NULL, 0, 0, 0, "someone, 2m: hi"));
    CHECK(READS(&d, "", "2m", NULL, NULL, 0, 0, 1, "someone, 2m, message deleted"));

    /* Each switch on its own changes only its part. */
    oc_spoken_opts o = d; o.sender = 0;
    CHECK(READS(&o, "Ada", "2m", "lunch?", NULL, 0, 0, 0, "2m: lunch?"));
    CHECK(READS(&o, "Ada", "2m", "lunch?", NULL, 0, 0, 1, "2m, message deleted"));
    o = d; o.time = 0;
    CHECK(READS(&o, "Ada", "2m", "lunch?", NULL, 0, 0, 0, "Ada: lunch?"));
    CHECK(READS(&o, "Ada", "2m", "lunch?", NULL, 0, 0, 1, "Ada, message deleted"));
    o = d; o.sender = 0; o.time = 0;
    CHECK(READS(&o, "Ada", "2m", "lunch?", rx, 3, 3, 0, "lunch?"));      /* the text alone */
    CHECK(READS(&o, "Ada", "2m", "lunch?", NULL, 0, 0, 1, "message deleted"));
    o = d; o.reactions = 1;
    CHECK(READS(&o, "Ada", "2m", "lunch?", rx, 1, 7, 0, "Ada, 2m: lunch?, reactions: \xF0\x9F\x91\x8D 2"));
    o = d; o.replies = 1;
    CHECK(READS(&o, "Ada", "2m", "lunch?", rx, 3, 1, 0, "Ada, 2m: lunch?, 1 reply"));

    /* Reactions: none says nothing; several, in the message's order. */
    o = d; o.reactions = 1;
    CHECK(READS(&o, "Ada", "2m", "lunch?", rx, 0, 0, 0, "Ada, 2m: lunch?"));
    CHECK(READS(&o, "Ada", "2m", "lunch?", NULL, 3, 0, 0, "Ada, 2m: lunch?"));
    CHECK(READS(&o, "Ada", "2m", "lunch?", rx, 3, 0, 0,
                "Ada, 2m: lunch?, reactions: \xF0\x9F\x91\x8D 2, tada 1, \xE2\x9D\xA4 5"));
    CHECK(READS(&o, "Ada", "2m", "lunch?", rx + 1, 2, 0, 0,
                "Ada, 2m: lunch?, reactions: tada 1, \xE2\x9D\xA4 5"));

    /* Replies: nothing at none, "1 reply", "<n> replies". */
    o = d; o.replies = 1;
    CHECK(READS(&o, "Ada", "2m", "lunch?", NULL, 0, 0, 0, "Ada, 2m: lunch?"));
    CHECK(READS(&o, "Ada", "2m", "lunch?", NULL, 0, 1, 0, "Ada, 2m: lunch?, 1 reply"));
    CHECK(READS(&o, "Ada", "2m", "lunch?", NULL, 0, 12, 0, "Ada, 2m: lunch?, 12 replies"));

    /* Text first, in both forms, and with everything on. */
    o = d; o.text_first = 1;
    CHECK(READS(&o, "Ada", "2m", "lunch?", NULL, 0, 0, 0, "lunch?, Ada, 2m"));
    CHECK(READS(&o, "Ada", "2m", "lunch?", NULL, 0, 0, 1, "message deleted, Ada, 2m"));
    o.time = 0;
    CHECK(READS(&o, "Ada", "2m", "lunch?", NULL, 0, 0, 0, "lunch?, Ada"));
    o = oc_spoken_from_bits(31);
    CHECK(READS(&o, "Ada", "2m", "lunch?", rx, 2, 1, 0,
                "lunch?, Ada, 2m, reactions: \xF0\x9F\x91\x8D 2, tada 1, 1 reply"));

    /* The bits a setting stores, both ways. */
    CHECK(oc_spoken_bits(&d) == 3);
    for (unsigned b = 0; b < 32; b++) {
        oc_spoken_opts x = oc_spoken_from_bits(b);
        CHECK(oc_spoken_bits(&x) == b);
    }
    o = oc_spoken_from_bits(3);
    CHECK(memcmp(&o, &d, sizeof o) == 0);

    /* The cut: at a small cap the result ends on a whole character and is
     * NUL-terminated, at every cap from 1 up. "é" is two bytes, the emoji four. */
    const char *text = "caf\xC3\xA9 \xF0\x9F\x8D\xB0 \xE2\x9D\xA4 ok";
    o = d; o.reactions = 1;
    char full[256];
    size_t fl = oc_spoken_msg(&o, "Zo\xC3\xAB", "2m", text, rx, 3, 0, 0, full, sizeof full);
    CHECK(fl == strlen(full) && fl > 20);
    for (size_t cap = 1; cap <= fl + 1; cap++) {
        char small[256];
        memset(small, 'X', sizeof small);
        size_t n = oc_spoken_msg(&o, "Zo\xC3\xAB", "2m", text, rx, 3, 0, 0, small, cap);
        int ok = n < cap && small[n] == '\0' && strlen(small) == n && whole_utf8(small) &&
                 strncmp(small, full, n) == 0 && (cap >= sizeof small || small[cap] == 'X');
        if (!ok) printf("    cap %zu: \"%s\" (%zu)\n", cap, small, n);
        CHECK(ok);
        /* Nothing past what fits is ever dropped while there is room for it:
         * at most a character's worth of bytes is left unused. */
        CHECK(cap - 1 - n <= 3);
    }
    CHECK(oc_spoken_msg(&o, "Ada", "2m", "hi", NULL, 0, 0, 0, NULL, 10) == 0);
    char one[1] = { 'X' };
    CHECK(oc_spoken_msg(&o, "Ada", "2m", "hi", NULL, 0, 0, 0, one, 1) == 0 && one[0] == '\0');
    return failures;
}
