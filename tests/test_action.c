/* Tests for what makes a message an action (shared/action.c, REQ-058,
 * ARCH-115): every edge of the rule, stated as the body and the answer. */
#include "check.h"
#include "action.h"

#include <stdlib.h>
#include <string.h>

/* 1 when `body` is an action whose text is exactly `text`; for a non-action,
 * pass NULL and it must be refused. */
static int is(const char *body, const char *text) {
    uint32_t st = 0xFFFFFFFFu, tl = 0xFFFFFFFFu;
    int got = oc_action_parse(body, strlen(body), &st, &tl);
    if (!text) {
        if (got || st != 0xFFFFFFFFu || tl != 0xFFFFFFFFu)
            printf("    \"%s\" was an action, wanted none\n", body);
        return !got && st == 0xFFFFFFFFu && tl == 0xFFFFFFFFu;
    }
    int ok = got && tl == strlen(text) && memcmp(body + st, text, tl) == 0;
    if (!ok) printf("    \"%s\" gave %d \"%.*s\", wanted \"%s\"\n", body, got, got ? (int)tl : 0,
                    got ? body + st : "", text);
    return ok;
}

int run_action_tests(void) {
    printf("test_action: the rule, spaces, what is refused, later lines, bytes, bounds\n");
    CHECK(is("/me is away", "is away"));
    CHECK(is("/me waves", "waves"));
    CHECK(is("/me    waves", "waves"));                 /* every space skipped */
    CHECK(is("/me waves  ", "waves  "));                /* trailing bytes are the author's */
    CHECK(is("/me waves\nand leaves", "waves\nand leaves"));
    CHECK(is("/me *waves* at @dana", "*waves* at @dana"));
    CHECK(is("/me /me", "/me"));
    CHECK(is("/me \xC3\xA9tudie", "\xC3\xA9tudie"));     /* text starts on a multi-byte character */

    CHECK(is("/me", NULL));
    CHECK(is("/me ", NULL));
    CHECK(is("/me    ", NULL));
    CHECK(is("/me \nwaves", NULL));
    CHECK(is("/me \twaves", NULL));
    CHECK(is("/me \r\nwaves", NULL));
    CHECK(is("/me\twaves", NULL));
    CHECK(is("/mewaves", NULL));
    CHECK(is("/mex waves", NULL));
    CHECK(is(" /me waves", NULL));
    CHECK(is("/ME waves", NULL));
    CHECK(is("/Me waves", NULL));
    CHECK(is("me waves", NULL));
    CHECK(is("", NULL));

    /* The length bounds the scan: a body is never read past `len`. */
    uint32_t st = 0, tl = 0;
    CHECK(oc_action_parse("/me waves", 4, &st, &tl) == 0);
    CHECK(oc_action_parse("/me waves", 5, &st, &tl) == 1 && st == 4 && tl == 1);
    CHECK(oc_action_parse(NULL, 9, &st, &tl) == 0);

    /* A long body: the text runs to its end. */
    size_t n = 64 * 1024;
    char *big = malloc(n);
    CHECK(big != NULL);
    if (big) {
        memcpy(big, "/me ", 4);
        memset(big + 4, 'x', n - 4);
        CHECK(oc_action_parse(big, n, &st, &tl) == 1 && st == 4 && tl == n - 4);
        free(big);
    }
    return failures;
}
