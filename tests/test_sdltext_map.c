/* sdltext's byte<->UTF-16 offset map (sdltext/st_common.c). Portable — this
 * is the half of the text layer that runs on any host, and the half where an
 * off-by-one corrupts a surrogate pair or misplaces every richtext span. */

#include "check.h"
#include "../sdltext/st_priv.h"

#include <stdlib.h>
#include <string.h>

static void map_of(const char *s, st_map *m, uint16_t **w, int *units)
{
    CHECK(st__map_build(s, strlen(s), m, w, units));
}

int run_sdltext_map_tests(void)
{
    failures = 0;

    /* ASCII: identity in both directions. */
    {
        st_map m;
        uint16_t *w;
        int n;
        map_of("hello", &m, &w, &n);
        CHECK(n == 5);
        CHECK(m.blen == 5 && m.wlen == 5);
        for (int i = 0; i <= 5; i++) {
            CHECK(st__b2w(&m, (size_t)i) == i);
            CHECK(st__w2b(&m, i) == i);
        }
        CHECK(w[0] == 'h' && w[4] == 'o' && w[5] == 0);
        free(w);
        st__map_free(&m);
    }

    /* 2- and 3-byte sequences: "é" (2 bytes, 1 unit), "€" (3 bytes, 1 unit). */
    {
        st_map m;
        uint16_t *w;
        int n;
        map_of("a\xC3\xA9z\xE2\x82\xAC", &m, &w, &n);   /* a é z € */
        CHECK(n == 4);
        CHECK(w[1] == 0x00E9 && w[3] == 0x20AC);
        CHECK(st__b2w(&m, 1) == 1);   /* start of é */
        CHECK(st__b2w(&m, 2) == 1);   /* mid-sequence maps to its character */
        CHECK(st__b2w(&m, 3) == 2);   /* z */
        CHECK(st__b2w(&m, 4) == 3);   /* start of € */
        CHECK(st__w2b(&m, 3) == 4);
        CHECK(st__w2b(&m, 4) == 7);
        free(w);
        st__map_free(&m);
    }

    /* 4-byte sequence: one emoji is TWO UTF-16 units, and both units map back
     * to the same byte — the surrogate-pair case the map exists for. */
    {
        st_map m;
        uint16_t *w;
        int n;
        map_of("x\xF0\x9F\x98\x80y", &m, &w, &n);   /* x 😀 y */
        CHECK(n == 4);
        CHECK(w[1] == 0xD83D && w[2] == 0xDE00);
        CHECK(st__b2w(&m, 1) == 1);
        CHECK(st__b2w(&m, 5) == 3);   /* y is after both units */
        CHECK(st__w2b(&m, 1) == 1 && st__w2b(&m, 2) == 1);
        CHECK(st__w2b(&m, 3) == 5);
        free(w);
        st__map_free(&m);
    }

    /* Invalid bytes degrade to U+FFFD one byte at a time; offsets stay sane. */
    {
        st_map m;
        uint16_t *w;
        int n;
        map_of("a\xFF\xFE" "b", &m, &w, &n);
        CHECK(n == 4);
        CHECK(w[1] == 0xFFFD && w[2] == 0xFFFD && w[3] == 'b');
        CHECK(st__w2b(&m, 3) == 3);
        free(w);
        st__map_free(&m);
    }

    /* A truncated sequence at the end of the buffer must not read past it. */
    {
        st_map m;
        uint16_t *w;
        int n;
        CHECK(st__map_build("a\xF0\x9F\x98", 4, &m, &w, &n));   /* cut emoji */
        CHECK(n >= 2 && w[0] == 'a');
        free(w);
        st__map_free(&m);
    }

    /* Clamping: offsets past either end land on the end, never out of it. */
    {
        st_map m;
        map_of("ab", &m, NULL, NULL);
        CHECK(st__b2w(&m, 99) == 2);
        CHECK(st__w2b(&m, 99) == 2);
        CHECK(st__w2b(&m, -5) == 0);
        st__map_free(&m);
    }

    /* Empty input is a valid map. */
    {
        st_map m;
        uint16_t *w;
        int n;
        CHECK(st__map_build("", 0, &m, &w, &n));
        CHECK(n == 0 && st__b2w(&m, 0) == 0 && st__w2b(&m, 0) == 0);
        free(w);
        st__map_free(&m);
    }

    return failures;
}
