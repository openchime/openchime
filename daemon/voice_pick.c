/* Choosing a default read-aloud voice (voice_pick.h). */
#include "voice_pick.h"
#include "protocol.h"   /* OC_TTS_VOICE_MAX */

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* The presentation the first pronoun naming one asks for: 'f', 'm', or 0.
 *
 * Whole words, not substrings. A substring search reads "they" as containing
 * "he", and so gave everyone who states they/them pronouns a masculine voice --
 * the opposite of what stating them asks for. A word too long to be a pronoun is
 * skipped outright rather than compared truncated. */
static char presentation(const char *pronouns) {
    char word[8];
    size_t k = 0;
    int too_long = 0;
    for (const char *c = pronouns;; c++) {
        if (*c && isalpha((unsigned char)*c)) {
            if (k + 1 < sizeof word) word[k++] = (char)tolower((unsigned char)*c);
            else too_long = 1;
            continue;
        }
        word[k] = '\0';
        if (k && !too_long) {
            if (!strcmp(word, "she") || !strcmp(word, "her") || !strcmp(word, "hers")) return 'f';
            if (!strcmp(word, "he") || !strcmp(word, "him") || !strcmp(word, "his")) return 'm';
        }
        k = 0;
        too_long = 0;
        if (!*c) return 0;
    }
}

int oc_voice_pick(const char *lang, const char *voices, const char *have,
                  const char *pronouns, uint64_t user_id, int *persist) {
    char list[512];
    snprintf(list, sizeof list, "%s", voices ? voices : "");
    char *ids[OC_TTS_VOICE_MAX];
    int n = 0;
    for (char *p = list; *p && n < OC_TTS_VOICE_MAX;) {
        ids[n++] = p;
        char *comma = strchr(p, ',');
        if (!comma) break;
        *comma = '\0';
        p = comma + 1;
    }
    *persist = 0;
    if (n == 0) return 0;
    for (int i = 0; i < n; i++)
        if (have && *have && strcmp(have, ids[i]) == 0) return i;

    /* Pronouns pick a voice's presentation only where the daemon can read them,
     * and it reads English. In another language the same field holds words this
     * knows nothing about, so the hash picks instead -- a fair choice rather than
     * a wrong one. */
    char want = 0;
    if (pronouns && *pronouns && lang && strncmp(lang, "en", 2) == 0)
        want = presentation(pronouns);

    int pick[OC_TTS_VOICE_MAX], np = 0;
    for (int i = 0; i < n; i++) {
        size_t l = strlen(ids[i]);
        if (!want || (l >= 2 && ids[i][l - 2] == '-' && ids[i][l - 1] == want)) pick[np++] = i;
    }
    if (np == 0) { for (int i = 0; i < n; i++) pick[np++] = i; }
    /* A hash of the id, not the id itself: consecutive sign-ups should not walk
     * the list in order and give a whole team the same two voices. */
    uint64_t h = 1469598103934665603ull ^ user_id;
    h *= 1099511628211ull;
    h ^= h >> 29;
    *persist = 1;
    return pick[(size_t)(h % (uint64_t)np)];
}
