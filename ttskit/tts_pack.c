/* tts_pack — building ttskit's data files (TTSKIT.md §8).
 *
 *   tts_pack ipa   CMUDICT.dict  lexicon.ipa  train.lex
 *       CMUdict (ARPAbet) to espeak-style IPA: lexicon.ipa is "word<TAB>ipa"
 *       (first pronunciation of each word); train.lex is the same with phonemes
 *       separated by spaces, the form the guesser trains on.
 *   tts_pack lexicon  lexicon.ipa  lexicon.bin
 *   tts_pack guesser  model.arpa   guesses.bin
 *   tts_pack word  DIR  WORD...     pronounce words with packed data (checking)
 *   tts_pack text  DIR  TEXT        pronounce a sentence
 */
#define _POSIX_C_SOURCE 200809L
#include "ttskit.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int usage(void) {
    fprintf(stderr,
        "usage: tts_pack ipa CMUDICT lexicon.ipa train.lex\n"
        "       tts_pack lexicon lexicon.ipa lexicon.bin LANG\n"
        "       tts_pack guesser model.arpa guesses.bin LANG\n"
        "       tts_pack word DIR WORD...\n"
        "       tts_pack text DIR TEXT\n");
    return 2;
}

static int do_ipa(const char *dict, const char *lex_path, const char *train_path) {
    FILE *in = fopen(dict, "r"), *lex = fopen(lex_path, "w"), *train = fopen(train_path, "w");
    if (!in || !lex || !train) { fprintf(stderr, "tts_pack: cannot open files\n"); return 1; }
    char line[1024], prev[1024] = "";
    long words = 0, skipped = 0;
    while (fgets(line, sizeof line, in)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = '\0';
        char *sp = strchr(line, ' ');
        if (!sp) continue;
        *sp = '\0';
        char *word = line, *arpa = sp + 1;
        char *paren = strchr(word, '(');
        if (paren) *paren = '\0';                 /* word(2): an alternative pronunciation */
        if (!strcmp(word, prev)) continue;        /* the first pronunciation wins */
        snprintf(prev, sizeof prev, "%s", word);
        char joined[256], units[512];
        if (!tts_arpa_to_ipa(arpa, 0, joined, sizeof joined) || !tts_arpa_to_ipa(arpa, 1, units, sizeof units)) { skipped++; continue; }
        fprintf(lex, "%s\t%s\n", word, joined);
        fprintf(train, "%s\t%s\n", word, units);
        words++;
    }
    fclose(in);
    int bad = ferror(lex) || ferror(train);
    if (fclose(lex) || fclose(train) || bad) { fprintf(stderr, "tts_pack: write failed\n"); return 1; }
    fprintf(stderr, "tts_pack: %ld words, %ld skipped\n", words, skipped);
    return 0;
}

int main(int argc, char **argv) {
    char err[512] = "";
    if (argc < 2) return usage();
    if (!strcmp(argv[1], "ipa") && argc == 5) return do_ipa(argv[2], argv[3], argv[4]);
    if (!strcmp(argv[1], "lexicon") && argc == 5) {
        if (tts_pack_lexicon(argv[2], argv[3], argv[4], err, sizeof err)) { fprintf(stderr, "tts_pack: %s\n", err); return 1; }
        return 0;
    }
    if (!strcmp(argv[1], "guesser") && argc == 5) {
        if (tts_pack_guesser(argv[2], argv[3], argv[4], err, sizeof err)) { fprintf(stderr, "tts_pack: %s\n", err); return 1; }
        return 0;
    }
    if ((!strcmp(argv[1], "word") || !strcmp(argv[1], "text")) && argc >= 4) {
        /* NULL: a tool says whatever the data says, rather than asserting a
         * language it was not told. */
        tts *t = tts_load(argv[2], NULL, err, sizeof err);
        if (!t) { fprintf(stderr, "tts_pack: %s\n", err); return 1; }
        char out[4096];
        if (!strcmp(argv[1], "text")) {
            tts_text(t, argv[3], out, sizeof out);
            printf("%s\n", out);
        } else {
            static const char *how[] = { "none", "lexicon", "guessed", "spelled" };
            for (int i = 3; i < argc; i++) {
                int r = tts_word(t, argv[i], out, sizeof out);
                printf("%s\t%s\t%s\n", argv[i], out, how[r]);
            }
        }
        tts_free(t);
        return 0;
    }
    return usage();
}
