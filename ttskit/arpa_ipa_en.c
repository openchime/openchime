/* CMUdict ARPAbet to espeak-style IPA, letter names, weak forms, and the small
 * helpers ttskit shares (TTSKIT.md §3). */
#define _POSIX_C_SOURCE 200809L
#include "tts_priv.h"

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

void tts_seterr(char *err, size_t cap, const char *fmt, ...) {
    if (!err || !cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

int tts_map_open(const char *path, tts_map *m) {
    m->p = NULL; m->n = 0; m->owned = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) { close(fd); return -1; }
    void *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return -1;
    m->p = p; m->n = (size_t)st.st_size; m->owned = 1;
    return 0;
}

void tts_map_close(tts_map *m) {
    if (m->p && m->owned) munmap((void *)m->p, m->n);
    m->p = NULL; m->n = 0; m->owned = 0;
}

/* ---- the table ---------------------------------------------------------------------
 * Vowels take a stressed and an unstressed form; espeak writes a stress mark
 * immediately before the vowel it stresses. Secondary stress is left unmarked:
 * espeak omits it far more often than it writes it, and measured against espeak
 * on common words the table is closer without it. */

typedef struct { const char *arpa, *stressed, *unstressed; } vowel;
static const vowel VOWELS[] = {
    { "AA", "ɑː", "ɑː" }, { "AE", "æ", "æ" },   { "AH", "ʌ", "ə" },   { "AO", "ɔː", "ɔː" },
    { "AW", "aʊ", "aʊ" }, { "AY", "aɪ", "aɪ" }, { "EH", "ɛ", "ɛ" },   { "ER", "ɜː", "ɚ" },
    { "EY", "eɪ", "eɪ" }, { "IH", "ɪ", "ɪ" },   { "IY", "iː", "i" },  { "OW", "oʊ", "oʊ" },
    { "OY", "ɔɪ", "ɔɪ" }, { "UH", "ʊ", "ʊ" },   { "UW", "uː", "uː" },
};
typedef struct { const char *arpa, *ipa; } consonant;
static const consonant CONSONANTS[] = {
    { "B", "b" },  { "CH", "tʃ" }, { "D", "d" },  { "DH", "ð" }, { "F", "f" },  { "G", "ɡ" },
    { "HH", "h" }, { "JH", "dʒ" }, { "K", "k" },  { "L", "l" },  { "M", "m" },  { "N", "n" },
    { "NG", "ŋ" }, { "P", "p" },   { "R", "ɹ" },  { "S", "s" },  { "SH", "ʃ" }, { "T", "t" },
    { "TH", "θ" }, { "V", "v" },   { "W", "w" },  { "Y", "j" },  { "Z", "z" },  { "ZH", "ʒ" },
};

typedef struct { char base[3]; int stress; int vowel; } phone;   /* stress -1: consonant */

static int is(const phone *p, const char *base) { return strcmp(p->base, base) == 0; }

static const vowel *find_vowel(const char *b) {
    for (size_t i = 0; i < sizeof VOWELS / sizeof VOWELS[0]; i++) if (!strcmp(VOWELS[i].arpa, b)) return &VOWELS[i];
    return NULL;
}
static const consonant *find_consonant(const char *b) {
    for (size_t i = 0; i < sizeof CONSONANTS / sizeof CONSONANTS[0]; i++) if (!strcmp(CONSONANTS[i].arpa, b)) return &CONSONANTS[i];
    return NULL;
}

typedef struct { char *p; size_t n, cap; int units; int bad; } ob;
static void emit(ob *o, const char *s) {
    size_t n = strlen(s);
    if (o->units && o->n) n += 1;
    if (o->n + n + 1 > o->cap) { o->bad = 1; return; }
    if (o->units && o->n) o->p[o->n++] = ' ';
    memcpy(o->p + o->n, s, strlen(s));
    o->n += strlen(s);
    o->p[o->n] = '\0';
}
static void emit_joined(ob *o, const char *mark, const char *s) {
    char u[16];
    snprintf(u, sizeof u, "%s%s", mark, s);
    emit(o, u);
}

size_t tts_arpa_to_ipa(const char *arpa, int units, char *out, size_t cap) {
    if (!out || cap == 0) return 0;
    out[0] = '\0';
    phone ph[64];
    int n = 0;
    for (const char *s = arpa; *s && n < 64;) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s || *s == '#') break;
        size_t k = 0;
        char tok[8];
        while (s[k] && s[k] != ' ' && s[k] != '\t' && k < sizeof tok - 1) { tok[k] = s[k]; k++; }
        tok[k] = '\0';
        s += k;
        int stress = -1;
        if (k && tok[k - 1] >= '0' && tok[k - 1] <= '2') { stress = tok[k - 1] - '0'; tok[k - 1] = '\0'; }
        if (strlen(tok) > 2) return 0;
        memcpy(ph[n].base, tok, strlen(tok) + 1);
        ph[n].vowel = find_vowel(tok) != NULL;
        if (!ph[n].vowel && !find_consonant(tok)) return 0;
        if (ph[n].vowel && stress < 0) return 0;
        ph[n].stress = ph[n].vowel ? stress : -1;
        n++;
    }
    if (n == 0) return 0;
    ob o = { out, 0, cap, units, 0 };
    for (int i = 0; i < n; i++) {
        const phone *p = &ph[i], *prv = i > 0 ? &ph[i - 1] : NULL, *nxt = i + 1 < n ? &ph[i + 1] : NULL;
        if (p->vowel) {
            const vowel *v = find_vowel(p->base);
            int stressed = p->stress >= 1;
            const char *u = stressed ? v->stressed : v->unstressed;
            if (is(p, "AO") && nxt && (is(nxt, "L") || is(nxt, "NG"))) u = "ɔ";
            if (is(p, "IY") && p->stress == 2 && i == n - 1) u = "i";
            if (is(p, "IY") && p->stress == 0 && nxt && is(nxt, "OW")) u = "ɪ";
            if ((is(p, "IH") || is(p, "AH")) && p->stress == 0) {
                /* A reduced vowel in a prefix (de-, re-, be-, pre-) or an -ed/-es ending. */
                if (i == 1 && (is(prv, "D") || is(prv, "R") || is(prv, "B"))) u = "ᵻ";
                else if (i == 2 && is(&ph[0], "P") && is(&ph[1], "R")) u = "ᵻ";
                else if (i == n - 2 && nxt && (is(nxt, "D") || is(nxt, "Z")) && prv &&
                         (is(prv, "T") || is(prv, "D") || is(prv, "S") || is(prv, "Z") ||
                          is(prv, "SH") || is(prv, "JH") || is(prv, "CH") || is(prv, "ZH"))) u = "ᵻ";
            }
            emit_joined(&o, p->stress == 1 ? "ˈ" : "", u);
            if (is(p, "ER") && p->stress == 0 && nxt && nxt->vowel) emit(&o, "ɹ");   /* linking r */
        } else {
            const char *u = find_consonant(p->base)->ipa;
            /* The flap: T between a vowel (or R) and an unstressed vowel. */
            if (is(p, "T") && prv && nxt && (prv->vowel || is(prv, "R")) && nxt->vowel && nxt->stress == 0) u = "ɾ";
            else if (is(p, "T") && prv && nxt && prv->vowel && is(nxt, "IY") && nxt->stress == 2 && i + 1 == n - 1) u = "ɾ";
            emit(&o, u);
        }
    }
    if (o.bad) { out[0] = '\0'; return 0; }
    return o.n;
}

/* ---- letter names, for spelling acronyms and unguessable words ---------------------- */

const char *tts_letter_ipa(char c) {
    static const char *L[26] = {
        "ˈeɪ", "bˈiː", "sˈiː", "dˈiː", "ˈiː", "ˈɛf", "dʒˈiː", "ˈeɪtʃ", "ˈaɪ", "dʒˈeɪ", "kˈeɪ", "ˈɛl", "ˈɛm",
        "ˈɛn", "ˈoʊ", "pˈiː", "kjˈuː", "ˈɑːɹ", "ˈɛs", "tˈiː", "jˈuː", "vˈiː", "dˈʌbəljuː", "ˈɛks", "wˈaɪ", "zˈiː",
    };
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return (c >= 'a' && c <= 'z') ? L[c - 'a'] : NULL;
}

/* ---- function words in their unstressed forms ----------------------------------------
 * CMUdict stresses every monosyllable, which reads each "the" and "to" as if
 * emphasised; espeak, and speech, do not. Sorted for bsearch. */

typedef struct { const char *word, *ipa; } weak;
static const weak WEAK[] = {
    { "a", "ɐ" }, { "an", "ɐn" }, { "and", "ænd" }, { "are", "ɑːɹ" }, { "as", "æz" }, { "at", "æt" },
    { "be", "biː" }, { "been", "bɪn" }, { "but", "bʌt" }, { "by", "baɪ" }, { "can", "kæn" },
    { "could", "kʊd" }, { "did", "dɪd" }, { "do", "duː" }, { "does", "dʌz" }, { "for", "fɔːɹ" },
    { "from", "fɹʌm" }, { "had", "hæd" }, { "has", "hæz" }, { "have", "hæv" }, { "he", "hiː" },
    { "her", "hɜː" }, { "him", "hɪm" }, { "i", "aɪ" }, { "if", "ɪf" }, { "in", "ɪn" }, { "is", "ɪz" },
    { "it", "ɪt" }, { "its", "ɪts" }, { "just", "dʒʌst" }, { "me", "miː" }, { "my", "maɪ" },
    { "not", "nɑːt" }, { "of", "ʌv" }, { "on", "ɔn" }, { "or", "ɔːɹ" }, { "our", "aʊɚ" },
    { "she", "ʃiː" }, { "should", "ʃʊd" }, { "so", "soʊ" }, { "some", "sʌm" }, { "than", "ðæn" },
    { "that", "ðæt" }, { "the", "ðə" }, { "their", "ðɛɹ" }, { "them", "ðɛm" }, { "then", "ðɛn" },
    { "there", "ðɛɹ" }, { "they", "ðeɪ" }, { "this", "ðɪs" }, { "to", "tə" }, { "us", "ʌs" },
    { "was", "wʌz" }, { "we", "wiː" }, { "were", "wɜː" }, { "will", "wɪl" }, { "with", "wɪð" },
    { "would", "wʊd" }, { "you", "juː" }, { "your", "jʊɹ" },
};

const char *tts_weak_form(const char *word) {
    size_t lo = 0, hi = sizeof WEAK / sizeof WEAK[0];
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = strcmp(word, WEAK[mid].word);
        if (c == 0) return WEAK[mid].ipa;
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return NULL;
}
