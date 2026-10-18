/* Numbers, times, money, percentages, ordinals, years and abbreviations written
 * out as words (ttskit.h, TTSKIT.md §4). English only. */
#include "tts_priv.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

typedef struct { char *p; size_t n, cap; } nb;

static void add(nb *b, const char *s) {
    size_t n = strlen(s);
    if (b->n + n + 1 > b->cap) n = b->cap > b->n + 1 ? b->cap - b->n - 1 : 0;
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = '\0';
}
static void addc(nb *b, char c) { char s[2] = { c, 0 }; add(b, s); }
static void sp(nb *b) { if (b->n && b->p[b->n - 1] != ' ') addc(b, ' '); }

static const char *ONES[] = { "zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine",
    "ten", "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen", "seventeen", "eighteen", "nineteen" };
static const char *TENS[] = { "", "", "twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety" };

static void below_thousand(nb *b, unsigned n) {
    if (n >= 100) {
        sp(b); add(b, ONES[n / 100]); add(b, " hundred");
        n %= 100;
        if (!n) return;
    }
    sp(b);
    if (n < 20) { add(b, ONES[n]); return; }
    add(b, TENS[n / 10]);
    if (n % 10) { addc(b, ' '); add(b, ONES[n % 10]); }
}

static void cardinal(nb *b, unsigned long long n) {
    if (n == 0) { sp(b); add(b, "zero"); return; }
    static const struct { unsigned long long v; const char *name; } SCALE[] = {
        { 1000000000000ULL, "trillion" }, { 1000000000ULL, "billion" }, { 1000000ULL, "million" }, { 1000ULL, "thousand" },
    };
    for (size_t i = 0; i < sizeof SCALE / sizeof SCALE[0]; i++) {
        if (n >= SCALE[i].v) {
            cardinal(b, n / SCALE[i].v);
            sp(b); add(b, SCALE[i].name);
            n %= SCALE[i].v;
            if (!n) return;
        }
    }
    below_thousand(b, (unsigned)n);
}

/* Ordinal: the cardinal with its last word in ordinal form ("twenty one" ->
 * "twenty first", "one hundred" -> "one hundredth"). */
static void ordinal(nb *b, unsigned long long n) {
    char tmp[256];
    nb t = { tmp, 0, sizeof tmp };
    tmp[0] = '\0';
    cardinal(&t, n);
    char *last = strrchr(tmp, ' ');
    last = last ? last + 1 : tmp;
    static const struct { const char *card, *ord; } IRR[] = {
        { "one", "first" }, { "two", "second" }, { "three", "third" }, { "five", "fifth" },
        { "eight", "eighth" }, { "nine", "ninth" }, { "twelve", "twelfth" },
    };
    char word[256];
    snprintf(word, sizeof word, "%s", last);
    *last = '\0';
    sp(b);
    add(b, tmp);
    for (size_t k = 0; k < sizeof IRR / sizeof IRR[0]; k++)
        if (!strcmp(word, IRR[k].card)) { add(b, IRR[k].ord); return; }
    size_t wl = strlen(word);
    if (wl && word[wl - 1] == 'y') { word[wl - 1] = '\0'; add(b, word); add(b, "ieth"); return; }
    add(b, word);
    add(b, "th");
}

/* A year said the usual way: 1984 -> nineteen eighty-four, 2026 -> twenty twenty-six,
 * 2000 -> two thousand, 2005 -> two thousand five. */
static void year(nb *b, unsigned n) {
    if (n >= 2000 && n < 2010) { cardinal(b, n); return; }
    unsigned hi = n / 100, lo = n % 100;
    below_thousand(b, hi);
    if (lo == 0) { sp(b); add(b, "hundred"); return; }
    if (lo < 10) { sp(b); add(b, "oh"); }
    below_thousand(b, lo);
}

static int is_word_char(unsigned char c) { return isalnum(c) || c == '\'' || c >= 0x80; }

typedef struct { const char *abbr, *said; } abbr;
static const abbr ABBR[] = {
    { "dr.", "doctor" }, { "mr.", "mister" }, { "mrs.", "missus" }, { "ms.", "miz" }, { "st.", "street" },
    { "e.g.", "for example" }, { "i.e.", "that is" }, { "etc.", "et cetera" }, { "vs.", "versus" },
    { "approx.", "approximately" }, { "jan.", "january" }, { "feb.", "february" }, { "aug.", "august" },
    { "sept.", "september" }, { "oct.", "october" }, { "nov.", "november" }, { "dec.", "december" },
};

size_t tts_normalize(const char *text, char *out, size_t cap) {
    if (!out || cap == 0) return 0;
    out[0] = '\0';
    if (!text) return 0;
    nb b = { out, 0, cap };
    size_t len = strlen(text);
    for (size_t i = 0; i < len && b.n + 1 < b.cap;) {
        unsigned char c = (unsigned char)text[i];
        int at_word_start = i == 0 || !is_word_char((unsigned char)text[i - 1]);

        /* Abbreviations (case-insensitive, at a word start). */
        if (at_word_start && isalpha(c)) {
            int matched = 0;
            for (size_t k = 0; k < sizeof ABBR / sizeof ABBR[0] && !matched; k++) {
                size_t al = strlen(ABBR[k].abbr);
                if (i + al > len) continue;
                size_t j = 0;
                while (j < al && tolower((unsigned char)text[i + j]) == ABBR[k].abbr[j]) j++;
                if (j == al && (i + al == len || !isalnum((unsigned char)text[i + al]))) {
                    sp(&b); add(&b, ABBR[k].said);
                    i += al;
                    matched = 1;
                }
            }
            if (matched) continue;
        }

        /* Money: $40, $3.50, $1,200. */
        if (c == '$' && i + 1 < len && isdigit((unsigned char)text[i + 1])) {
            size_t j = i + 1;
            unsigned long long whole = 0;
            while (j < len && (isdigit((unsigned char)text[j]) || (text[j] == ',' && j + 1 < len && isdigit((unsigned char)text[j + 1])))) {
                if (text[j] != ',') whole = whole * 10 + (unsigned)(text[j] - '0');
                j++;
            }
            unsigned cents = 0;
            int has_cents = 0;
            if (j + 2 < len && text[j] == '.' && isdigit((unsigned char)text[j + 1]) && isdigit((unsigned char)text[j + 2])) {
                cents = (unsigned)((text[j + 1] - '0') * 10 + (text[j + 2] - '0'));
                has_cents = 1;
                j += 3;
            }
            cardinal(&b, whole);
            add(&b, whole == 1 ? " dollar" : " dollars");
            if (has_cents && cents) { add(&b, " and"); cardinal(&b, cents); add(&b, cents == 1 ? " cent" : " cents"); }
            i = j;
            continue;
        }

        if (isdigit(c) && at_word_start) {
            size_t j = i;
            unsigned long long n = 0;
            int digits = 0, commas = 0;
            while (j < len && (isdigit((unsigned char)text[j]) ||
                               (text[j] == ',' && j + 3 < len && isdigit((unsigned char)text[j + 1]) &&
                                isdigit((unsigned char)text[j + 2]) && isdigit((unsigned char)text[j + 3])))) {
                if (text[j] == ',') commas++;
                else { if (digits < 18) n = n * 10 + (unsigned)(text[j] - '0'); digits++; }
                j++;
            }
            /* Time: 3:15, 10:05, 3:15pm. */
            if (j + 2 < len && text[j] == ':' && isdigit((unsigned char)text[j + 1]) && isdigit((unsigned char)text[j + 2]) && n <= 24 && !commas) {
                unsigned m = (unsigned)((text[j + 1] - '0') * 10 + (text[j + 2] - '0'));
                j += 3;
                cardinal(&b, n);
                if (m == 0) { if (!(j < len && (text[j] == 'a' || text[j] == 'p' || text[j] == 'A' || text[j] == 'P'))) { sp(&b); add(&b, "o'clock"); } }
                else if (m < 10) { sp(&b); add(&b, "oh"); cardinal(&b, m); }
                else cardinal(&b, m);
                size_t k = j;
                while (k < len && text[k] == ' ') k++;
                if (k + 1 < len && (tolower((unsigned char)text[k]) == 'a' || tolower((unsigned char)text[k]) == 'p') && tolower((unsigned char)text[k + 1]) == 'm' &&
                    (k + 2 == len || !isalnum((unsigned char)text[k + 2]))) {
                    sp(&b); addc(&b, tolower((unsigned char)text[k]) == 'a' ? 'a' : 'p'); add(&b, " m");
                    j = k + 2;
                }
                i = j;
                continue;
            }
            /* Ordinal: 1st, 22nd, 103rd, 4th. */
            if (j + 1 < len && !commas) {
                char s0 = (char)tolower((unsigned char)text[j]), s1 = (char)tolower((unsigned char)text[j + 1]);
                if (((s0 == 's' && s1 == 't') || (s0 == 'n' && s1 == 'd') || (s0 == 'r' && s1 == 'd') || (s0 == 't' && s1 == 'h')) &&
                    (j + 2 == len || !isalnum((unsigned char)text[j + 2]))) {
                    ordinal(&b, n);
                    i = j + 2;
                    continue;
                }
            }
            /* Decimal: 3.5 -> three point five. */
            if (j + 1 < len && text[j] == '.' && isdigit((unsigned char)text[j + 1]) && !commas) {
                cardinal(&b, n);
                sp(&b); add(&b, "point");
                j++;
                while (j < len && isdigit((unsigned char)text[j])) { sp(&b); add(&b, ONES[text[j] - '0']); j++; }
                i = j;
            } else if (digits == 4 && !commas && n >= 1100 && n <= 2099 && n != 2000) {
                year(&b, (unsigned)n);
                i = j;
            } else if (digits > 18) {
                /* Too long to be a quantity: read the digits. */
                for (size_t k = i; k < j; k++) if (isdigit((unsigned char)text[k])) { sp(&b); add(&b, ONES[text[k] - '0']); }
                i = j;
            } else {
                cardinal(&b, n);
                i = j;
            }
            if (i < len && text[i] == '%') { add(&b, " percent"); i++; }
            continue;
        }

        if (c == '%') { sp(&b); add(&b, "percent"); i++; continue; }
        if (c == '&') { sp(&b); add(&b, "and"); i++; continue; }
        if (c == '+' && (i + 1 == len || text[i + 1] == ' ')) { sp(&b); add(&b, "plus"); i++; continue; }

        /* Anything else passes through; a word glued to a number gets its space back. */
        if (isalpha(c) && i && isdigit((unsigned char)text[i - 1])) sp(&b);
        addc(&b, (char)c);
        i++;
    }
    return b.n;
}
