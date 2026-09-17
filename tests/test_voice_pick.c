/* Tests for choosing a default read-aloud voice (daemon/voice_pick.c; REQ-292,
 * READ-ALOUD.md §3). What they prove: pronouns naming a presentation prefer the
 * matching voices, pronouns naming neither -- they/them among them -- are treated
 * exactly like no pronouns at all, an existing voice stands while it is offered,
 * a non-English engine ignores the field, and the choice is stable per user. */
#include "check.h"
#include "voice_pick.h"

#include <string.h>

#define VOICES "a-m,b-f,c-m,d-f,e-m,f-f"

static const char *id_at(int i) {
    static const char *ids[] = { "a-m", "b-f", "c-m", "d-f", "e-m", "f-f" };
    return i >= 0 && i < 6 ? ids[i] : "";
}
static char ending(int i) { const char *s = id_at(i); return s[strlen(s) - 1]; }

static int pick(const char *lang, const char *have, const char *pronouns, uint64_t uid, int *persist) {
    int p = -1;
    int r = oc_voice_pick(lang, VOICES, have, pronouns, uid, &p);
    if (persist) *persist = p;
    return r;
}

/* True when, for every user id tried, `pronouns` chooses exactly what no pronouns
 * would. Asserted across many ids because a single id can agree by coincidence:
 * a hash over half the list and a hash over all of it sometimes land on the same
 * voice, and one agreeing id proves nothing. */
static int same_as_no_pronouns(const char *lang, const char *pronouns) {
    for (uint64_t uid = 1; uid <= 500; uid++)
        if (pick(lang, NULL, pronouns, uid, NULL) != pick(lang, NULL, "", uid, NULL)) return 0;
    return 1;
}

/* True when every user id is given a voice ending `want`. */
static int always_ends(const char *lang, const char *pronouns, char want) {
    for (uint64_t uid = 1; uid <= 500; uid++)
        if (ending(pick(lang, NULL, pronouns, uid, NULL)) != want) return 0;
    return 1;
}

int run_voice_pick_tests(void) {
    printf("test_voice_pick: presentation from pronouns, they/them neutral, whole words, existing voice, language gate, fallback, stability\n");

    /* A presentation named picks from the matching half. */
    CHECK(always_ends("en-US", "she/her", 'f'));
    CHECK(always_ends("en-US", "he/him", 'm'));
    CHECK(always_ends("en-US", "She/Her", 'f'));              /* case is not meaning */

    /* they/them names no presentation, so it is the same as saying nothing. A
     * substring search failed this: "they" contains "he". */
    CHECK(same_as_no_pronouns("en-US", "they/them"));
    CHECK(same_as_no_pronouns("en-US", "ze/hir"));
    CHECK(same_as_no_pronouns("en-US", "any pronouns"));

    /* Whole words only: a word merely containing a pronoun is not one. */
    CHECK(same_as_no_pronouns("en-US", "Shelby"));
    CHECK(same_as_no_pronouns("en-US", "theirs"));

    /* The first presentation named decides. */
    CHECK(always_ends("en-US", "she/they", 'f'));
    CHECK(always_ends("en-US", "he/they", 'm'));
    CHECK(always_ends("en-US", "they/she", 'f'));

    /* A voice the user already has stands, and is not written again. */
    int persist = -1;
    CHECK(pick("en-US", "c-m", "she/her", 7, &persist) == 2 && persist == 0);
    /* One the engine no longer offers is replaced, and the new choice is kept. */
    persist = -1;
    int r = pick("en-US", "gone-f", "she/her", 7, &persist);
    CHECK(ending(r) == 'f' && persist == 1);

    /* A non-English engine cannot read the field, so it does not try. */
    CHECK(same_as_no_pronouns("de-DE", "she/her"));
    CHECK(same_as_no_pronouns(NULL, "she/her"));

    /* No voice with the wanted ending: every voice is a candidate, not a failure. */
    for (uint64_t uid = 1; uid <= 50; uid++) {
        int p = -1;
        int i = oc_voice_pick("en-US", "x-m,y-m", NULL, "she/her", uid, &p);
        CHECK(i >= 0 && i <= 1 && p == 1);
    }

    /* Stable: the same person gets the same voice every time. */
    CHECK(pick("en-US", NULL, "", 42, NULL) == pick("en-US", NULL, "", 42, NULL));

    /* Nothing offered: index 0 and nothing to persist. */
    persist = -1;
    CHECK(oc_voice_pick("en-US", "", NULL, "she/her", 1, &persist) == 0 && persist == 0);

    return failures;
}
