/* The text core's Unicode layer (sdltext/st_priv.h): grapheme clusters, line
 * break opportunities and bidi levels, as the canvas backend uses them. The
 * bidi cases are lines of the Unicode BidiCharacterTest.txt kind, hand-picked
 * and written out here rather than vendoring the file. */
#include "check.h"
#include "../sdltext/st_priv.h"

#include <stdlib.h>
#include <string.h>

static uint16_t *w16(const char *s, int *n, st_map *m) {
    uint16_t *w = NULL;
    CHECK(st__map_build(s, strlen(s), m, &w, n));
    return w;
}

/* Cluster starts as a string of 1/0 per unit. */
static void cluster_str(const char *s, char *out, size_t cap) {
    st_map m; int n; uint16_t *w = w16(s, &n, &m);
    uint8_t *cb = NULL; CHECK(st__clusters_build(w, n, &cb));
    size_t o = 0;
    for (int u = 0; u < n && o + 1 < cap; u++) out[o++] = cb[u] ? '1' : '0';
    out[o] = 0;
    free(cb); free(w); st__map_free(&m);
}

static void brk_str(const char *s, char *out, size_t cap) {
    st_map m; int n; uint16_t *w = w16(s, &n, &m);
    uint8_t *cb = NULL, *brk = NULL;
    CHECK(st__clusters_build(w, n, &cb));
    CHECK(st__break_opps(w, n, cb, &brk));
    size_t o = 0;
    for (int u = 0; u < n && o + 1 < cap; u++) out[o++] = brk[u] == ST_BRK_MANDATORY ? '!' : brk[u] ? '|' : '.';
    out[o] = 0;
    free(brk); free(cb); free(w); st__map_free(&m);
}

/* Levels as digits per unit, and the visual order as indices. */
static void bidi_str(const char *s, int para, char *levels_out, char *order_out, size_t cap, int *rtl) {
    st_map m; int n; uint16_t *w = w16(s, &n, &m);
    uint8_t *lv = NULL; int r = 0;
    CHECK(st__bidi_levels(w, n, para, &lv, &r));
    if (rtl) *rtl = r;
    size_t o = 0;
    for (int u = 0; u < n && o + 1 < cap; u++) levels_out[o++] = (char)('0' + lv[u]);
    levels_out[o] = 0;
    int *order = malloc((size_t)n * sizeof *order);
    st__bidi_reorder(lv, n, order);
    o = 0;
    for (int i = 0; i < n && o + 3 < cap; i++) { if (i) order_out[o++] = ' '; o += (size_t)snprintf(order_out + o, cap - o, "%d", order[i]); }
    order_out[o] = 0;
    free(order); free(lv); free(w); st__map_free(&m);
}

int run_sdltext_unicode_tests(void)
{
    failures = 0;
    char a[256], b[256];

    /* ---- clusters ---- */
    cluster_str("abc", a, sizeof a);                   CHECK(strcmp(a, "111") == 0);
    cluster_str("e\xCC\x81x", a, sizeof a);            CHECK(strcmp(a, "101") == 0);        /* e + combining acute */
    cluster_str("\xF0\x9F\x98\x80", a, sizeof a);      CHECK(strcmp(a, "10") == 0);         /* one emoji: a surrogate pair */
    /* a family: man ZWJ woman ZWJ girl -- one cluster of eight units */
    cluster_str("\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7", a, sizeof a);
    CHECK(strcmp(a, "10000000") == 0);
    /* a flag: two regional indicators, one cluster of four units; two flags, two clusters */
    cluster_str("\xF0\x9F\x87\xAC\xF0\x9F\x87\xA7\xF0\x9F\x87\xAB\xF0\x9F\x87\xB7", a, sizeof a);
    CHECK(strcmp(a, "10001000") == 0);
    /* Devanagari: ka + virama + ssa (a conjunct) is one cluster */
    cluster_str("\xE0\xA4\x95\xE0\xA5\x8D\xE0\xA4\xB7", a, sizeof a);   CHECK(strcmp(a, "100") == 0);
    /* Hangul jamo L V T: one cluster; a precomposed syllable: one unit */
    cluster_str("\xE1\x84\x92\xE1\x85\xA1\xE1\x86\xAB", a, sizeof a);   CHECK(strcmp(a, "100") == 0);
    cluster_str("\xED\x95\x9C", a, sizeof a);                           CHECK(strcmp(a, "1") == 0);
    cluster_str("\r\n", a, sizeof a);                                   CHECK(strcmp(a, "10") == 0);
    {
        static const uint8_t cb[] = { 1, 0, 0, 1, 1, 0, 1 };
        CHECK(st__cluster_prev(cb, 6, 2) == 0 && st__cluster_prev(cb, 6, 3) == 3 && st__cluster_prev(cb, 6, 5) == 4);
        CHECK(st__cluster_next(cb, 6, 0) == 3 && st__cluster_next(cb, 6, 3) == 4 && st__cluster_next(cb, 6, 5) == 6);
    }

    /* ---- line breaks ---- */
    brk_str("ab cd", a, sizeof a);            CHECK(strcmp(a, "...|.") == 0);        /* after the space */
    brk_str("ab  cd", a, sizeof a);           CHECK(strcmp(a, "....|.") == 0);       /* after the run of spaces */
    brk_str("ab\ncd", a, sizeof a);           CHECK(strcmp(a, "...!.") == 0);        /* mandatory after the newline */
    brk_str("ab-cd", a, sizeof a);            CHECK(strcmp(a, "...|.") == 0);        /* after a hyphen */
    brk_str("ab\xC2\xA0" "cd", a, sizeof a);  CHECK(strcmp(a, ".....") == 0);        /* not at a no-break space */
    brk_str("3.14", a, sizeof a);             CHECK(strcmp(a, "....") == 0);         /* not inside a number */
    brk_str("ab, cd", a, sizeof a);           CHECK(strcmp(a, "....|.") == 0);       /* not before the comma */
    brk_str("(ab) cd", a, sizeof a);          CHECK(strcmp(a, ".....|.") == 0);      /* not after ( nor before ) */
    brk_str("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E", a, sizeof a);  CHECK(strcmp(a, ".||") == 0);   /* between ideographs */
    brk_str("ab\xE6\x97\xA5", a, sizeof a);   CHECK(strcmp(a, "..|") == 0);          /* before an ideograph */
    brk_str("e\xCC\x81 x", a, sizeof a);      CHECK(strcmp(a, "...|") == 0);         /* never inside a cluster */
    brk_str("-5", a, sizeof a);               CHECK(strcmp(a, "..") == 0);           /* a signed number */
    brk_str("a\tb", a, sizeof a);             CHECK(strcmp(a, "..|") == 0);          /* after a tab */

    /* ---- bidi ---- */
    int rtl;
    bidi_str("abc", -1, a, b, sizeof a, &rtl);  CHECK(!rtl && strcmp(a, "000") == 0 && strcmp(b, "0 1 2") == 0);
    /* Hebrew alone: an RTL paragraph, every unit at level 1, reversed */
    bidi_str("\xD7\x90\xD7\x91\xD7\x92", -1, a, b, sizeof a, &rtl);
    CHECK(rtl && strcmp(a, "111") == 0 && strcmp(b, "2 1 0") == 0);
    /* Latin, then Hebrew, then Latin: the Hebrew run reversed in place */
    bidi_str("ab \xD7\x90\xD7\x91 cd", -1, a, b, sizeof a, &rtl);
    CHECK(!rtl && strcmp(a, "00011000") == 0 && strcmp(b, "0 1 2 4 3 5 6 7") == 0);
    /* Hebrew with a number: the digits keep their order inside the RTL run (level 2) */
    bidi_str("\xD7\x90\xD7\x91 12 \xD7\x92", -1, a, b, sizeof a, &rtl);
    CHECK(rtl && strcmp(a, "1112211") == 0 && strcmp(b, "6 5 3 4 2 1 0") == 0);
    /* An LTR paragraph forced RTL: the Latin keeps its order at level 2 */
    bidi_str("ab", 1, a, b, sizeof a, &rtl);   CHECK(rtl && strcmp(a, "22") == 0 && strcmp(b, "0 1") == 0);
    /* Arabic with Latin digits: EN after AL becomes AN (W2) at level 2 */
    bidi_str("\xD8\xB9\xD8\xB1 12", -1, a, b, sizeof a, &rtl);
    CHECK(rtl && strcmp(a, "11122") == 0 && strcmp(b, "3 4 2 1 0") == 0);
    /* Neutrals between two RTL runs take RTL (N1); trailing space takes the paragraph's level (L1) */
    bidi_str("\xD7\x90 \xD7\x91 ", 0, a, b, sizeof a, &rtl);
    CHECK(!rtl && strcmp(a, "1110") == 0 && strcmp(b, "2 1 0 3") == 0);
    /* An embedding: RLE ... PDF around Latin inside Latin */
    bidi_str("a\xE2\x80\xAB" "bc\xE2\x80\xAC" "d", -1, a, b, sizeof a, &rtl);
    CHECK(!rtl && a[0] == '0' && a[2] == '2' && a[3] == '2' && a[5] == '0');
    /* An isolate: RLI ... PDI with Hebrew, in Latin */
    bidi_str("a\xE2\x81\xA7\xD7\x90\xD7\x91\xE2\x81\xA9" "b", -1, a, b, sizeof a, &rtl);
    CHECK(!rtl && a[0] == '0' && a[2] == '1' && a[3] == '1' && a[5] == '0');
    /* Mirrored brackets are not resolved (N0 is out): the parenthesis after Hebrew is a neutral between R and L -> paragraph level */
    bidi_str("\xD7\x90(a)", 0, a, b, sizeof a, &rtl);
    CHECK(!rtl && a[0] == '1' && a[1] == '0' && a[2] == '0' && a[3] == '0');
    /* Reorder with runs as elements: a line of three runs, levels 0 1 1 -> 0, then the two RTL runs reversed */
    {
        static const uint8_t lv[] = { 0, 1, 1 }; int order[3];
        st__bidi_reorder(lv, 3, order);
        CHECK(order[0] == 0 && order[1] == 2 && order[2] == 1);
    }
    return failures;
}
