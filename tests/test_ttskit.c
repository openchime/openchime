/* Tests for ttskit, read-aloud's pronunciation (ttskit/, ARCH-111, TTSKIT.md):
 * the ARPAbet->IPA table, number and abbreviation expansion, dictionary, guesser
 * and spelling, whole sentences, the committed data against its pins, the C
 * guesser against Phonetisaurus's own decoder, damaged data refused, and the
 * IPA turned into the voice model's token ids (daemon/tts_kitten_tokens.c). */
#include "check.h"
#include "ttskit.h"
#include "tts_priv.h"
#include "tts_kitten.h"

#include <mbedtls/sha256.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DATA "ttskit/data"

/* The committed data files. Regenerating them (scripts/build_ttskit_data.sh)
 * changes these, the reference output below and TTSKIT.md §8 together. */
static const char *LEXICON_SHA256 = "d518a2b696a4000fc82b47d56f01028b9463c104e65f4495bfc689856d472ee9";
static const char *GUESSES_SHA256 = "efd55988df04bda24622daee8282c3ec39a8fe8d655d6f64d294cfd22318b757";

static int ipa_is(const char *arpa, int units, const char *want) {
    char out[256];
    tts_arpa_to_ipa(arpa, units, out, sizeof out);
    if (strcmp(out, want) != 0) printf("    arpa   \"%s\"\n    ipa    \"%s\"\n    wanted \"%s\"\n", arpa, out, want);
    return strcmp(out, want) == 0;
}

static int norm_is(const char *text, const char *want) {
    char out[1024];
    tts_normalize(text, out, sizeof out);
    if (strcmp(out, want) != 0) printf("    text   \"%s\"\n    got    \"%s\"\n    wanted \"%s\"\n", text, out, want);
    return strcmp(out, want) == 0;
}

static int word_is(tts *t, const char *word, int how, const char *want) {
    char out[256];
    int r = tts_word(t, word, out, sizeof out);
    if (r != how || strcmp(out, want) != 0)
        printf("    word   \"%s\"\n    got    %d \"%s\"\n    wanted %d \"%s\"\n", word, r, out, how, want);
    return r == how && strcmp(out, want) == 0;
}

static int text_is(tts *t, const char *text, const char *want) {
    char out[1024];
    tts_text(t, text, out, sizeof out);
    if (strcmp(out, want) != 0) printf("    text   \"%s\"\n    got    \"%s\"\n    wanted \"%s\"\n", text, out, want);
    return strcmp(out, want) == 0;
}

static int sha256_is(const char *path, const char *want) {
    FILE *f = fopen(path, "rb");
    if (!f) { printf("    cannot open %s\n", path); return 0; }
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts(&c, 0);
    unsigned char buf[65536], sum[32];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) mbedtls_sha256_update(&c, buf, n);
    fclose(f);
    mbedtls_sha256_finish(&c, sum);
    mbedtls_sha256_free(&c);
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", sum[i]);
    if (strcmp(hex, want) != 0) printf("    %s\n    sha256 %s\n    pinned %s\n", path, hex, want);
    return strcmp(hex, want) == 0;
}

/* tests/data/ttskit_guess_ref.txt is Phonetisaurus's decoder (phonetisaurus-g2pfst,
 * best path) on 3,010 words with the model guesses.bin was packed from. Returns
 * how many of them the C decoder pronounces identically. */
static int guess_agreement(int *total) {
    tts_guesser g;
    char err[256];
    *total = 0;
    if (tts_guesser_open(DATA "/guesses.bin", NULL, &g, err, sizeof err) != 0) { printf("    %s\n", err); return 0; }
    FILE *f = fopen("tests/data/ttskit_guess_ref.txt", "r");
    if (!f) { tts_guesser_close(&g); return 0; }
    char line[512], out[256];
    int same = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = '\0';
        (*total)++;
        tts_guess(&g, line, out, sizeof out);
        if (strcmp(out, tab + 1) == 0) same++;
    }
    fclose(f);
    tts_guesser_close(&g);
    return same;
}

static void put(const char *path, const void *p, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(p, 1, n, f);
    fclose(f);
}

static unsigned char *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *p = len > 0 ? malloc((size_t)len) : NULL;
    if (p && fread(p, 1, (size_t)len, f) != (size_t)len) { free(p); p = NULL; }
    fclose(f);
    *n = p ? (size_t)len : 0;
    return p;
}

/* Damaged data: a file that is present but wrong refuses to load, so a bad
 * install fails loudly instead of guessing every word. */
static int refused(const char *dir, const char *name, const unsigned char *p, size_t n) {
    char path[512], err[256] = "";
    snprintf(path, sizeof path, "%s/%s", dir, name);
    put(path, p, n);
    tts *t = tts_load(dir, err, sizeof err);
    unlink(path);
    if (t) { tts_free(t); return 0; }
    return err[0] != '\0';
}

int run_ttskit_tests(void) {
    printf("test_ttskit: IPA table, normalization, lexicon, guesser, spelling, sentences, pins, agreement, damaged data\n");

    /* The ARPAbet->IPA table and its context rules (TTSKIT.md §3). */
    CHECK(ipa_is("M AY0 G R EY1 SH AH0 N", 0, "maɪɡɹˈeɪʃən"));
    CHECK(ipa_is("M AY0 G R EY1 SH AH0 N", 1, "m aɪ ɡ ɹ ˈeɪ ʃ ə n"));
    CHECK(ipa_is("W AO1 T ER0", 0, "wˈɔːɾɚ"));              /* flap between vowels; unstressed ER */
    CHECK(ipa_is("B AO1 L", 0, "bˈɔl"));                   /* AO before L is short */
    CHECK(ipa_is("HH AE1 P IY0", 0, "hˈæpi"));
    CHECK(ipa_is("S IH1 T IY2", 0, "sˈɪɾi"));              /* flap before a final IY2 */
    CHECK(ipa_is("R IY0 T ER1 N", 0, "ɹitˈɜːn"));
    CHECK(ipa_is("W AA1 N T IH0 D", 0, "wˈɑːntᵻd"));       /* -ed reduced vowel */
    CHECK(ipa_is("K AA1 N T ER0 AE2 K T", 0, "kˈɑːntɚɹækt")); /* linking r; no secondary stress */
    CHECK(ipa_is("M IY0 OW1", 0, "mɪˈoʊ"));
    CHECK(ipa_is("Q Q1", 0, ""));                          /* unknown symbol */

    /* IPA to Kitten's token ids, against ids from Kitten's own Python tokenizer:
     * punctuation split off with spaces, the end marker and pads around. */
    {
        static const long long hello[] = { 0, 50, 83, 54, 156, 57, 135, 16, 3, 16, 65, 156, 87, 158, 54, 46, 16, 5, 16,
                                           50, 156, 43, 135, 16, 69, 158, 123, 16, 52, 63, 158, 16, 6, 10, 0 };
        static const long long clock[] = { 0, 156, 102, 62, 61, 16, 48, 156, 43, 102, 64, 16, 83, 53, 54, 156, 69, 158, 53, 10, 0 };
        long long ids[128];
        size_t n = kitten_tokens("həlˈoʊ, wˈɜːld! hˈaʊ ɑːɹ juː?", ids, 128);
        CHECK(n == sizeof hello / sizeof hello[0] && memcmp(ids, hello, sizeof hello) == 0);
        n = kitten_tokens("ˈɪts   fˈaɪv əklˈɑːk", ids, 128);
        CHECK(n == sizeof clock / sizeof clock[0] && memcmp(ids, clock, sizeof clock) == 0);
        n = kitten_tokens("", ids, 128);
        CHECK(n == 3 && ids[0] == 0 && ids[1] == 10 && ids[2] == 0);
        n = kitten_tokens("həlˈoʊ wˈɜːld", ids, 8);                      /* cut, still closed */
        CHECK(n == 8 && ids[6] == 10 && ids[7] == 0);
    }

    /* Numbers, times, money, ordinals, years, abbreviations (TTSKIT.md §4). */
    CHECK(norm_is("Meet at 3:15 on the 2nd, $40", "Meet at three fifteen on the second, forty dollars"));
    CHECK(norm_is("$3.50 and $1", "three dollars and fifty cents and one dollar"));
    CHECK(norm_is("21st 100th 12th 40th", "twenty first one hundredth twelfth fortieth"));
    CHECK(norm_is("in 2026 and 2005 and 1900", "in twenty twenty six and two thousand five and nineteen hundred"));
    CHECK(norm_is("3.5 and 1,200,000", "three point five and one million two hundred thousand"));
    CHECK(norm_is("at 3:00 or 3:00pm or 3:05 am", "at three o'clock or three p m or three oh five a m"));
    CHECK(norm_is("50% off", "fifty percent off"));
    CHECK(norm_is("Dr. Who & co, e.g. this", "doctor Who and co, for example this"));
    CHECK(norm_is("10x", "ten x"));
    CHECK(norm_is("1234567890123456789012", "one two three four five six seven eight nine zero one two three four five six seven eight nine zero one two"));
    CHECK(norm_is("", ""));

    char err[256] = "";
    tts *t = tts_load(DATA, err, sizeof err);
    CHECK(t != NULL);
    if (!t) { printf("    %s\n", err); return failures; }

    /* Dictionary, guesser, spelling. */
    CHECK(word_is(t, "migration", 1, "maɪɡɹˈeɪʃən"));
    CHECK(word_is(t, "Hello", 1, "həlˈoʊ"));
    CHECK(word_is(t, "don't", 1, "dˈoʊnt"));
    CHECK(word_is(t, "NASA", 1, "nˈæsə"));                 /* capitals in the dictionary are a word */
    CHECK(word_is(t, "openchime", 2, "ˈoʊpəntʃaɪm"));
    CHECK(word_is(t, "Kubernetes", 2, "kjˈuːbɚnˈɛtiːz"));
    CHECK(word_is(t, "API", 3, "ˈeɪ pˈiː ˈaɪ"));            /* short capitals are always spelled */
    CHECK(word_is(t, "HTTPS", 3, "ˈeɪtʃ tˈiː tˈiː pˈiː ˈɛs"));
    CHECK(word_is(t, "", 0, ""));
    CHECK(word_is(t, "42", 0, ""));

    /* Sentences: function words weak, initialisms spelled, pauses kept. */
    CHECK(text_is(t, "I can see US troops.", "aɪ kæn sˈiː jˈuː ˈɛs tɹˈuːps."));
    CHECK(text_is(t, "Hello, world!  How are you?", "həlˈoʊ, wˈɜːld! hˈaʊ ɑːɹ juː?"));
    CHECK(text_is(t, "It's 5 o'clock", "ˈɪts fˈaɪv əklˈɑːk"));
    CHECK(text_is(t, "\xF0\x9F\x98\x80 --- !!!", ""));
    tts_free(t);

    /* Without data, words are still said: spelled. */
    t = tts_load("/nonexistent-ttskit-data", err, sizeof err);
    CHECK(t != NULL);
    CHECK(word_is(t, "hello", 3, "ˈeɪtʃ ˈiː ˈɛl ˈɛl ˈoʊ"));
    tts_free(t);

    /* The committed data is the data that was measured. */
    CHECK(sha256_is(DATA "/lexicon.bin", LEXICON_SHA256));
    CHECK(sha256_is(DATA "/guesses.bin", GUESSES_SHA256));

    /* The C decoder finds Phonetisaurus's best path. One word in the reference
     * ("doman") has two paths within rounding of each other; allow one in 1,000. */
    int total, same = guess_agreement(&total);
    CHECK(total == 3010);
    CHECK(total > 0 && same * 1000 >= total * 999);
    if (same != total) printf("    guesser agrees on %d of %d words\n", same, total);

    /* Damaged files are refused, each in a directory of its own. */
    char dir[] = "/tmp/oc-ttskit-XXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    size_t ln = 0, gn = 0;
    unsigned char *lex = slurp(DATA "/lexicon.bin", &ln), *gs = slurp(DATA "/guesses.bin", &gn);
    CHECK(lex && gs && ln > 64 && gn > 64);
    if (lex && gs && ln > 64 && gn > 64) {
        CHECK(refused(dir, "lexicon.bin", lex, ln / 2));                       /* truncated */
        CHECK(refused(dir, "guesses.bin", gs, gn / 2));
        CHECK(refused(dir, "lexicon.bin", (const unsigned char *)"short", 5));
        memcpy(lex, "OCTTSGS1", 8);                                             /* wrong magic */
        CHECK(refused(dir, "lexicon.bin", lex, ln));
        memcpy(lex, TTS_LEX_MAGIC, 8);
        lex[8] = 2;                                                             /* a later version */
        CHECK(refused(dir, "lexicon.bin", lex, ln));
        gs[8] = 2;
        CHECK(refused(dir, "guesses.bin", gs, gn));
    }
    /* The same data lent from memory says the same thing; damaged bytes are refused. */
    if (lex && gs) {
        memcpy(lex, TTS_LEX_MAGIC, 8);
        lex[8] = 1;
        gs[8] = 1;
        t = tts_load_mem(lex, ln, gs, gn, err, sizeof err);
        CHECK(t != NULL);
        CHECK(word_is(t, "migration", 1, "maɪɡɹˈeɪʃən"));
        CHECK(word_is(t, "Kubernetes", 2, "kjˈuːbɚnˈɛtiːz"));
        tts_free(t);
        err[0] = '\0';
        CHECK(tts_load_mem(lex, ln / 2, gs, gn, err, sizeof err) == NULL && err[0]);
        CHECK(tts_load_mem(lex, ln, NULL, 0, err, sizeof err) == NULL);
    }
    free(lex);
    free(gs);
    rmdir(dir);
    return failures;
}
