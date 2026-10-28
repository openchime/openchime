/* The emoji catalogue and skin tones (client/core/complete.c).
 *
 * The catalogue had no test at all while it was 179 entries; expanding it to
 * 800-odd is exactly the change that can silently break a shortcode somebody
 * already sent, so the invariants are written down here rather than assumed. */

#include "complete.h"
#include "check.h"

#include <string.h>
#include <ctype.h>
#include <stdio.h>

/* Is `s` well-formed UTF-8? A mis-typed hex escape in the table produces a
 * plausible-looking C string that renders as a replacement character, which no
 * other check would catch. */
static int valid_utf8(const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        int n;
        if (*p < 0x80) n = 0;
        else if ((*p & 0xE0) == 0xC0) n = 1;
        else if ((*p & 0xF0) == 0xE0) n = 2;
        else if ((*p & 0xF8) == 0xF0) n = 3;
        else return 0;
        for (int i = 0; i < n; i++)
            if ((p[1 + i] & 0xC0) != 0x80) return 0;
        p += n + 1;
    }
    return 1;
}

static void test_catalogue_integrity(void) {
    size_t n = 0;
    const oc_emoji *all = oc_emoji_all(&n);
    CHECK(all != NULL);

    /* The item this expansion closes said "179 entries"; the point of the work
     * is that it is now a catalogue rather than a sample. */
    CHECK(n > 700);
    CHECK(n <= OC_EMOJI_MAX);   /* the bound every caller sizes a buffer by */

    int bad_name = 0, bad_emoji = 0, bad_cat = 0, dup = 0, toned = 0;
    for (size_t i = 0; i < n; i++) {
        const char *nm = all[i].name;
        if (!nm || !nm[0]) { bad_name++; continue; }
        for (const char *p = nm; *p; p++)
            if (!(islower((unsigned char)*p) || isdigit((unsigned char)*p) || *p == '_'))
                { bad_name++; break; }
        if (!all[i].emoji || !all[i].emoji[0] || !valid_utf8(all[i].emoji)) bad_emoji++;
        if (!all[i].keywords || !valid_utf8(all[i].keywords)) bad_emoji++;
        if (all[i].category >= OC_EMOJI_CAT_COUNT) bad_cat++;
        if (all[i].tonable) toned++;
        /* Shortcodes are the identity: two rows with one name means one of them
         * is unreachable through oc_emoji_by_name. */
        for (size_t j = i + 1; j < n; j++)
            if (strcmp(all[i].name, all[j].name) == 0) { dup++; break; }
    }
    CHECK(bad_name == 0);
    CHECK(bad_emoji == 0);
    CHECK(bad_cat == 0);
    CHECK(dup == 0);
    CHECK(toned > 40);          /* the gestures/people that take a tone */

    /* Categories are contiguous and in order, which is what lets a picker emit
     * section headers by walking the table once (the reason for the ordering). */
    int out_of_order = 0;
    for (size_t i = 1; i < n; i++)
        if (all[i].category < all[i - 1].category) out_of_order++;
    CHECK(out_of_order == 0);
    for (uint8_t c = 0; c < OC_EMOJI_CAT_COUNT; c++)
        CHECK(oc_emoji_category_name(c)[0] != '\0');
    CHECK(oc_emoji_category_name(OC_EMOJI_CAT_COUNT)[0] == '\0');
}

/* Every shortcode that shipped in the 179-entry catalogue must still resolve.
 * A message stored last week with `:sweat_smile:` renders from the same table,
 * so dropping or renaming a shortcode silently breaks existing history. These
 * are the ones the expansion renamed or nearly lost. */
/* Both names resolve, to the same character. NULL-safe on purpose (see below). */
static int same_emoji(const char *a, const char *b) {
    const char *ea = oc_emoji_by_name(a), *eb = oc_emoji_by_name(b);
    return ea && eb && strcmp(ea, eb) == 0;
}

static void test_shipped_shortcodes_still_resolve(void) {
    static const char *const shipped[] = {
        /* renamed, kept as aliases */
        "slightly_smiling", "neutral", "zipper_mouth", "nerd", "hot", "cold",
        "woozy", "upside_down", "melting", "shushing", "salute", "party_face",
        "hand", "person", "sun", "wine", "confetti", "medal", "chart_down",
        "inbox", "outbox", "phone", "floppy", "alarm", "money", "check",
        "heavy_check", "eyes_symbol",
        /* nearly dropped outright */
        "sweat_smile", "brain", "popcorn", "pushpin",
        /* a sample of the ones that never moved */
        "smile", "joy", "fire", "tada", "eyes", "rocket", "thumbsup", "heart",
    };
    for (size_t i = 0; i < sizeof shipped / sizeof *shipped; i++) {
        const char *e = oc_emoji_by_name(shipped[i]);
        if (!e) printf("  shortcode stopped resolving: %s\n", shipped[i]);
        CHECK(e != NULL && e[0] != '\0');
    }

    /* An alias resolves to exactly what its modern spelling does. Compared
     * through a helper because CHECK records and continues: a bare
     * strcmp(oc_emoji_by_name(...), ...) turns the very failure this test exists
     * to catch into a segfault that takes the whole suite with it. */
    CHECK(same_emoji("nerd", "nerd_face"));
    CHECK(same_emoji("check", "white_check_mark"));
    CHECK(same_emoji("hand", "raised_hand"));

    /* Aliases are NOT in the catalogue: a picker must not show one glyph twice. */
    size_t n = 0;
    const oc_emoji *all = oc_emoji_all(&n);
    int alias_in_table = 0;
    for (size_t i = 0; i < n; i++)
        if (strcmp(all[i].name, "nerd") == 0 || strcmp(all[i].name, "check") == 0 ||
            strcmp(all[i].name, "hand") == 0) alias_in_table++;
    CHECK(alias_in_table == 0);

    CHECK(oc_emoji_by_name("no_such_emoji_at_all") == NULL);
    CHECK(oc_emoji_by_name("") == NULL);
    CHECK(oc_emoji_by_name(NULL) == NULL);
}

/* Find a catalogue entry by name (the struct, not just the character). */
static const oc_emoji *entry(const char *name) {
    size_t n = 0;
    const oc_emoji *all = oc_emoji_all(&n);
    for (size_t i = 0; i < n; i++)
        if (strcmp(all[i].name, name) == 0) return &all[i];
    return NULL;
}

static void test_skin_tones(void) {
    char buf[64];

    const oc_emoji *wave = entry("wave");
    CHECK(wave != NULL && wave->tonable);
    if (!wave) return;

    /* The default leaves the base alone — so a caller may apply the user's tone
     * to everything without first asking whether it applies. */
    CHECK(oc_emoji_with_tone(wave, OC_SKIN_DEFAULT, buf, sizeof buf) > 0);
    CHECK(strcmp(buf, wave->emoji) == 0);

    /* A real tone appends the Fitzpatrick modifier: U+1F3FD is F0 9F 8F BD. */
    size_t len = oc_emoji_with_tone(wave, OC_SKIN_MEDIUM, buf, sizeof buf);
    CHECK(len == strlen(wave->emoji) + 4);
    CHECK(memcmp(buf, wave->emoji, strlen(wave->emoji)) == 0);
    CHECK(memcmp(buf + strlen(wave->emoji), "\xf0\x9f\x8f\xbd", 4) == 0);
    CHECK(valid_utf8(buf));

    /* Each of the five is distinct, and none equals the base. */
    char seen[OC_SKIN_COUNT][64];
    for (uint8_t t = 0; t < OC_SKIN_COUNT; t++) {
        CHECK(oc_emoji_with_tone(wave, t, seen[t], sizeof seen[t]) > 0);
        CHECK(oc_emoji_skin_name(t)[0] != '\0');
    }
    int clash = 0;
    for (uint8_t a = 0; a < OC_SKIN_COUNT; a++)
        for (uint8_t b = (uint8_t)(a + 1); b < OC_SKIN_COUNT; b++)
            if (strcmp(seen[a], seen[b]) == 0) clash++;
    CHECK(clash == 0);
    CHECK(oc_emoji_skin_name(OC_SKIN_COUNT)[0] == '\0');

    /* A variation selector is replaced, not kept: U+270C U+FE0F + a modifier is
     * not a well-formed sequence, and the modifier already implies the emoji
     * presentation the selector was asking for. */
    const oc_emoji *v = entry("v");
    CHECK(v != NULL && v->tonable);
    if (v) {
        size_t blen = strlen(v->emoji);
        CHECK(blen >= 3 && memcmp(v->emoji + blen - 3, "\xef\xb8\x8f", 3) == 0);
        len = oc_emoji_with_tone(v, OC_SKIN_DARK, buf, sizeof buf);
        CHECK(len == blen - 3 + 4);
        CHECK(memcmp(buf + blen - 3, "\xf0\x9f\x8f\xbf", 4) == 0);
        CHECK(valid_utf8(buf));
    }

    /* Something that is not a person keeps its own colour whatever is asked. */
    const oc_emoji *fire = entry("fire");
    CHECK(fire != NULL && !fire->tonable);
    if (fire) {
        CHECK(oc_emoji_with_tone(fire, OC_SKIN_DARK, buf, sizeof buf) > 0);
        CHECK(strcmp(buf, fire->emoji) == 0);
    }

    /* A ZWJ sequence is deliberately not tonable: appending a modifier after
     * the whole sequence is a different operation and produces nonsense. */
    const oc_emoji *tech = entry("technologist");
    CHECK(tech != NULL && !tech->tonable);

    /* Out of range behaves as default rather than reading past the table. */
    CHECK(oc_emoji_with_tone(wave, 99, buf, sizeof buf) > 0);
    CHECK(strcmp(buf, wave->emoji) == 0);

    /* No room is reported, not truncated into invalid UTF-8. */
    char tiny[3];
    CHECK(oc_emoji_with_tone(wave, OC_SKIN_LIGHT, tiny, sizeof tiny) == 0);
    CHECK(oc_emoji_with_tone(wave, OC_SKIN_LIGHT, buf, 0) == 0);
    CHECK(oc_emoji_with_tone(NULL, OC_SKIN_LIGHT, buf, sizeof buf) == 0);
}

static void test_search(void) {
    const oc_emoji *hits[OC_EMOJI_MAX];

    /* Browsing returns the whole catalogue — the case a fixed 256-entry hit
     * buffer used to truncate, dropping entire trailing categories. */
    size_t n = 0;
    oc_emoji_all(&n);
    CHECK(oc_emoji_search(NULL, hits, OC_EMOJI_MAX) == n);
    CHECK(oc_emoji_search("", hits, OC_EMOJI_MAX) == n);

    /* `max` is honoured. */
    CHECK(oc_emoji_search(NULL, hits, 10) == 10);

    /* Word-prefix, not substring: "art" finds :art: but not :heart:. */
    size_t k = oc_emoji_search("art", hits, OC_EMOJI_MAX);
    CHECK(k > 0);
    int saw_art = 0, saw_heart = 0;
    for (size_t i = 0; i < k; i++) {
        if (strcmp(hits[i]->name, "art") == 0) saw_art = 1;
        if (strcmp(hits[i]->name, "heart") == 0) saw_heart = 1;
    }
    CHECK(saw_art);
    CHECK(!saw_heart);

    /* Keywords are searched, so a name nobody guesses is still reachable. */
    k = oc_emoji_search("celebrate", hits, OC_EMOJI_MAX);
    int saw_tada = 0;
    for (size_t i = 0; i < k; i++) if (strcmp(hits[i]->name, "tada") == 0) saw_tada = 1;
    CHECK(saw_tada);

    CHECK(oc_emoji_search("zzzzznope", hits, OC_EMOJI_MAX) == 0);
}

int run_emoji_tests(void) {
    printf("test_emoji: catalogue integrity (names, UTF-8, categories, duplicates),\n");
    printf("            shipped shortcodes still resolve, skin tones, search\n");
    test_catalogue_integrity();
    test_shipped_shortcodes_still_resolve();
    test_skin_tones();
    test_search();
    return failures;
}
