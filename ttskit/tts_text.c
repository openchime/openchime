/* The public entry points: loading the data, a word, a sentence (ttskit.h). */
#define _POSIX_C_SOURCE 200809L
#include "tts_priv.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct tts {
    tts_lexicon lex;
    tts_guesser guess;
    int have_lex, have_guess;
    char lang[TTS_LANG_MAX];
};

/* Opens whichever of the two are given. A missing file is allowed; data that is
 * present but unreadable is an error, not an absence: silently guessing every word
 * would sound wrong with nothing saying why. */
static tts *load(const char *lex_name, const tts_map *lex_mem, const char *guess_name, const tts_map *guess_mem,
                 const char *want, char *err, size_t errcap) {
    tts *t = calloc(1, sizeof *t);
    if (!t) { tts_seterr(err, errcap, "out of memory"); return NULL; }
    char why[256] = "";
    t->have_lex = tts_lexicon_open(lex_name, lex_mem, &t->lex, why, sizeof why) == 0;
    if (!t->have_lex && (lex_mem || strstr(why, "cannot map") == NULL)) { tts_seterr(err, errcap, "%s", why); free(t); return NULL; }
    why[0] = '\0';
    t->have_guess = tts_guesser_open(guess_name, guess_mem, &t->guess, why, sizeof why) == 0;
    if (!t->have_guess && (guess_mem || strstr(why, "cannot map") == NULL)) {
        tts_seterr(err, errcap, "%s", why);
        if (t->have_lex) tts_lexicon_close(&t->lex);
        free(t);
        return NULL;
    }

    /* The language gate. Two files that disagree, or that are not the language
     * asked for, are refused here rather than mixed: a lexicon from one language
     * and a guesser from another do not fail as they are used -- they pronounce
     * fluent nonsense, and nothing downstream can tell.
     *
     * The wording matters. A message containing "cannot map" is read above as
     * "this file is simply absent", which is tolerated on the disk path, so a
     * mismatch worded that way would load as half a kit and say nothing. */
    const char *lex_lang = t->have_lex ? t->lex.lang : NULL;
    const char *gs_lang  = t->have_guess ? t->guess.lang : NULL;
    const char *got = lex_lang ? lex_lang : gs_lang;
    int bad = (lex_lang && gs_lang && strcmp(lex_lang, gs_lang) != 0) ||
              (want && *want && got && strcmp(got, want) != 0);
    if (bad) {
        if (lex_lang && gs_lang && strcmp(lex_lang, gs_lang) != 0)
            tts_seterr(err, errcap, "%s is for %s but %s is for %s", lex_name, lex_lang, guess_name, gs_lang);
        else
            tts_seterr(err, errcap, "%s is for %s, not %s", lex_name, got, want);
        if (t->have_lex) tts_lexicon_close(&t->lex);
        if (t->have_guess) tts_guesser_close(&t->guess);
        free(t);
        return NULL;
    }
    if (got) snprintf(t->lang, sizeof t->lang, "%s", got);
    return t;
}

tts *tts_load(const char *dir, const char *lang, char *err, size_t errcap) {
    char lex[1024], guess[1024];
    snprintf(lex, sizeof lex, "%s/lexicon.bin", dir ? dir : ".");
    snprintf(guess, sizeof guess, "%s/guesses.bin", dir ? dir : ".");
    return load(lex, NULL, guess, NULL, lang, err, errcap);
}

tts *tts_load_mem(const void *lexicon, size_t lexicon_len, const void *guesses, size_t guesses_len,
                  const char *lang, char *err, size_t errcap) {
    tts_map lm = { lexicon, lexicon_len, 0 }, gm = { guesses, guesses_len, 0 };
    return load("lexicon.bin", &lm, "guesses.bin", &gm, lang, err, errcap);
}

const char *tts_lang(const tts *t) { return t ? t->lang : ""; }

void tts_free(tts *t) {
    if (!t) return;
    if (t->have_lex) tts_lexicon_close(&t->lex);
    if (t->have_guess) tts_guesser_close(&t->guess);
    free(t);
}

static int spell(const char *word, char *ipa, size_t cap) {
    size_t n = 0;
    ipa[0] = '\0';
    for (const char *c = word; *c; c++) {
        const char *l = tts_letter_ipa(*c);
        if (!l) continue;
        size_t ll = strlen(l);
        if (n + ll + 2 > cap) break;
        if (n) ipa[n++] = ' ';
        memcpy(ipa + n, l, ll);
        n += ll;
        ipa[n] = '\0';
    }
    return n ? 3 : 0;
}

int tts_word(tts *t, const char *word, char *ipa, size_t cap) {
    if (!ipa || cap == 0) return 0;
    ipa[0] = '\0';
    if (!word || !word[0]) return 0;
    char w[TTS_WORD_MAX];
    size_t n = 0, upper = 0, letters = 0;
    for (const char *c = word; *c && n + 1 < sizeof w; c++) {
        unsigned char ch = (unsigned char)*c;
        if (isalpha(ch)) { letters++; if (isupper(ch)) upper++; w[n++] = (char)tolower(ch); }
        else if (ch == '\'') w[n++] = '\'';
    }
    w[n] = '\0';
    if (!letters) return 0;

    /* Short all-capital words are initialisms ("API", "CPU", "ID"), said letter
     * by letter even when the dictionary has a word spelled the same way. */
    if (letters >= 2 && letters <= 3 && upper == letters) return spell(w, ipa, cap);

    const char *hit = t && t->have_lex ? tts_lexicon_find(&t->lex, w) : NULL;
    if (hit) {
        snprintf(ipa, cap, "%s", hit);
        return 1;
    }
    if (upper == letters && letters <= 5) return spell(w, ipa, cap);
    if (t && t->have_guess) {
        /* The guesser knows letters only; apostrophes are dropped for guessing. */
        char g[TTS_WORD_MAX];
        size_t gn = 0;
        for (size_t i = 0; w[i]; i++) if (w[i] != '\'') g[gn++] = w[i];
        g[gn] = '\0';
        if (gn && tts_guess(&t->guess, g, ipa, cap)) return 2;
    }
    return spell(w, ipa, cap);
}

size_t tts_text(tts *t, const char *text, char *ipa, size_t cap) {
    if (!ipa || cap == 0) return 0;
    ipa[0] = '\0';
    if (!text) return 0;
    size_t tl = strlen(text);
    char *norm = malloc(tl * 8 + 64);                 /* numbers grow into words */
    if (!norm) return 0;
    tts_normalize(text, norm, tl * 8 + 64);

    size_t n = 0;
    int said = 0;
    for (const char *p = norm; *p;) {
        unsigned char c = (unsigned char)*p;
        if (isalpha(c) || c == '\'') {
            char w[TTS_WORD_MAX];
            size_t wn = 0;
            while (*p && (isalpha((unsigned char)*p) || *p == '\'') && wn + 1 < sizeof w) w[wn++] = *p++;
            while (*p && (isalpha((unsigned char)*p) || *p == '\'')) p++;       /* over-long: truncated */
            w[wn] = '\0';
            char lower[TTS_WORD_MAX];
            for (size_t i = 0; i <= wn; i++) lower[i] = (char)tolower((unsigned char)w[i]);
            char pron[TTS_IPA_MAX];
            const char *weak = tts_weak_form(lower);
            /* An initialism that happens to be a function word ("US", "IT") is spelled. */
            int caps = wn >= 2 && isupper((unsigned char)w[0]) && isupper((unsigned char)w[1]);
            if (weak && !caps) snprintf(pron, sizeof pron, "%s", weak);
            else if (!tts_word(t, w, pron, sizeof pron)) continue;
            size_t pl = strlen(pron);
            if (n + pl + 2 > cap) break;
            if (n && ipa[n - 1] != ' ') ipa[n++] = ' ';
            memcpy(ipa + n, pron, pl);
            n += pl;
            ipa[n] = '\0';
            said = 1;
        } else if (c == ',' || c == '.' || c == '!' || c == '?' || c == ';' || c == ':') {
            if (n && n + 2 < cap && ipa[n - 1] != ',' && ipa[n - 1] != '.' && ipa[n - 1] != '!' && ipa[n - 1] != '?' &&
                ipa[n - 1] != ';' && ipa[n - 1] != ':') {
                while (n && ipa[n - 1] == ' ') n--;
                ipa[n++] = (char)c;
                ipa[n] = '\0';
            }
            p++;
        } else {
            p++;                                          /* spaces, hyphens, symbols */
        }
    }
    free(norm);
    while (n && ipa[n - 1] == ' ') ipa[--n] = '\0';
    return said ? n : (ipa[0] = '\0', 0);
}
