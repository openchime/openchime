/* Tests for the speakable form of a message (shared/speakable.c, REQ-291/294,
 * ARCH-111): what read-aloud says for each construct a body can hold. */
#include "check.h"
#include "speakable.h"

#include <string.h>

static const char *names(void *ctx, const char *name) {
    (void)ctx;
    return strcmp(name, "dana") == 0 ? "Dana Scully" : NULL;
}

static int said(const char *body, const char *want) {
    char out[OC_SPEAK_MAX + 1];
    oc_speakable(body, strlen(body), names, NULL, out, sizeof out);
    if (strcmp(out, want) != 0) printf("    body   \"%s\"\n    said   \"%s\"\n    wanted \"%s\"\n", body, out, want);
    return strcmp(out, want) == 0;
}

int run_speakable_tests(void) {
    printf("test_speakable: plain text, formatting, code, links, mentions, quotes, emoji, lines, not renderable, segments, time tokens, spoilers\n");
    CHECK(said("Can you look at the logs?", "Can you look at the logs?"));
    CHECK(said("this is *really* _important_ and ~not~ that", "this is really important and not that"));
    CHECK(said("run `make test` first", "run make test first"));
    CHECK(said("before\n```\nint x = 1;\nint y = 2;\nreturn x;\n```\nafter", "before. Code block, 3 lines. after"));
    /* A table is announced and its cells skipped, mentions in them included. */
    CHECK(said("numbers\n| who | n |\n|---|--:|\n| @alice | 1 |\n| bob | 2 |\nthanks",
               "numbers. Table, 2 rows. thanks"));
    CHECK(said("| a |\n|---|\n| 1 |", "Table, 1 row."));
    /* A time token in full, in UTC: speech has no reader's zone. Inside code
     * it is not a time, so no date is said. */
    CHECK(said("standup at <time:2026-10-30T15:00Z> sharp",
               "standup at Friday 30 October 2026, 15:00 UTC sharp"));
    {
        char out[OC_SPEAK_MAX + 1];
        oc_speakable("type `<time:2026-10-30T15:00Z>`", 32, names, NULL, out, sizeof out);
        CHECK(strstr(out, "October") == NULL);
    }
    /* A spoiler is named, never read (MARKDOWN.md §1d). */
    CHECK(said("the killer is >!the butler!< indeed", "the killer is spoiler indeed"));
    /* At the start of a line it is a spoiler and no quote; a quote can hold one. */
    CHECK(said(">!the butler!< did it", "spoiler did it"));
    CHECK(said("> >!the butler!< did it", "Quote: spoiler did it"));
    CHECK(said("> the old plan\n>!the butler!<", "Quote: the old plan. spoiler"));
    CHECK(said(">!not closed", "Quote: !not closed"));
    /* In a table a spoiler is hidden with the cells, which are never read,
     * even where it starts the table. */
    CHECK(said("| a | b |\n|---|---|\n| >!the butler!< | c |", "Table, 1 row."));
    CHECK(said(">!the butler!< | b\n---|---\n1 | 2", "Table, 1 row."));
    CHECK(said("| a | b |\n|---|---|\n| x | y | >!the butler!< |", "Table, 1 row."));
    /* However many spans a message has, ordinary text is said in full and
     * hidden text never is. */
    {
        static char many[8192], out[OC_SPEAK_MAX + 1];
        size_t o;
        /* Bold words, then a table whose cells are not read. */
        o = 0;
        for (int i = 0; i < 44; i++) o += (size_t)snprintf(many + o, sizeof many - o, "*w%d* ", i);
        o += (size_t)snprintf(many + o, sizeof many - o, "\n| a | b |\n|---|---|\n| x | y | >!THEBUTLER!< | PLAINCELL |\nLAST");
        oc_speakable(many, o, names, NULL, out, sizeof out);
        CHECK(strstr(out, "w43") && strstr(out, "LAST") && !strstr(out, "THEBUTLER") && !strstr(out, "PLAINCELL"));
        /* The 128th span is what a spoiler hides. */
        o = 0;
        for (int i = 0; i < 42; i++) o += (size_t)snprintf(many + o, sizeof many - o, "*w%d* ", i);
        o += (size_t)snprintf(many + o, sizeof many - o, ">!THEBUTLER!< LAST");
        oc_speakable(many, o, names, NULL, out, sizeof out);
        CHECK(strstr(out, "w41") && strstr(out, "LAST") && !strstr(out, "THEBUTLER"));
        /* The same text is what a summary gets. */
        static char whole[32768];
        oc_speakable_full(many, o, names, NULL, whole, sizeof whole);
        CHECK(strstr(whole, "w41") && strstr(whole, "LAST") && !strstr(whole, "THEBUTLER"));
        /* The span limit falling inside a block. */
        o = (size_t)snprintf(many, sizeof many, "```spoiler\n");
        for (int i = 0; i < 64; i++) o += (size_t)snprintf(many + o, sizeof many - o, "- HIDDEN%d\n", i);
        o += (size_t)snprintf(many + o, sizeof many - o, "```\nLAST");
        oc_speakable(many, o, names, NULL, out, sizeof out);
        CHECK(!strstr(out, "HIDDEN") && strstr(out, "LAST"));
        /* Nothing hidden: said to the end, past any number of spans. */
        o = 0;
        for (int i = 0; i < 64; i++) o += (size_t)snprintf(many + o, sizeof many - o, "- item%d\n", i);
        oc_speakable(many, o, names, NULL, out, sizeof out);
        CHECK(strstr(out, "item0") && strstr(out, "item63"));
        o = 0;
        for (int i = 0; i < 100; i++) o += (size_t)snprintf(many + o, sizeof many - o, "*b%d* ", i);
        oc_speakable(many, o, names, NULL, out, sizeof out);
        CHECK(strstr(out, "b99") != NULL);
        /* Past 32 spoilers each is named, and what follows is said. */
        o = 0;
        for (int i = 0; i < 40; i++) o += (size_t)snprintf(many + o, sizeof many - o, ">!HIDDEN%d!< ", i);
        o += (size_t)snprintf(many + o, sizeof many - o, "LAST");
        oc_speakable(many, o, names, NULL, out, sizeof out);
        CHECK(!strstr(out, "HIDDEN") && strstr(out, "LAST"));
    }
    /* The old form is said as it is written. */
    CHECK(said("a||b", "a||b"));
    CHECK(said("Before\n```spoiler Ending\nShe lives.\n```\nThe end.", "Before. spoiler: Ending. The end."));
    CHECK(said("see https://www.github.com/openchime/openchime/pull/285 for it", "see github.com for it"));
    CHECK(said("read [the release notes](https://example.com/notes) today", "read the release notes today"));
    CHECK(said("thanks @dana and @fox", "thanks Dana Scully and fox"));
    CHECK(said("@here standup in 5", "everyone here standup in 5"));
    CHECK(said("@channel heads up", "everyone heads up"));
    CHECK(said("> the old plan\n> was wrong\nagreed", "Quote: the old plan. was wrong. agreed"));
    CHECK(said("- one\n- two", "one. two"));
    CHECK(said("ship it \xF0\x9F\x9A\x80 :tada:", "ship it"));
    CHECK(said("line one\nline two", "line one. line two"));
    CHECK(said("done.\nnext", "done. next"));

    char out[64];
    CHECK(oc_speakable("", 0, NULL, NULL, out, sizeof out) == 0);
    CHECK(oc_speakable("\xF0\x9F\x91\x8D", 4, NULL, NULL, out, sizeof out) == 0 && out[0] == '\0');
    CHECK(oc_speakable(":+1: :tada:", 11, NULL, NULL, out, sizeof out) == 0);
    CHECK(oc_speakable("  \n \n", 5, NULL, NULL, out, sizeof out) == 0);

    /* Segments: at sentence ends within the limit, never empty, covering the text. */
    const char *t = "First sentence here. Second one is a little longer. Third.";
    size_t st[8], ln[8];
    int n = oc_speakable_segments(t, strlen(t), 30, st, ln, 8);
    CHECK(n == 3);
    CHECK(n == 3 && ln[0] == strlen("First sentence here.") && strncmp(t + st[1], "Second", 6) == 0);
    const char *nopunct = "aaaa bbbb cccc dddd eeee ffff gggg";
    n = oc_speakable_segments(nopunct, strlen(nopunct), 16, st, ln, 8);
    int ok = n >= 2;
    for (int i = 0; i < n; i++) if (ln[i] == 0 || ln[i] > 16 || nopunct[st[i]] == ' ') ok = 0;
    CHECK(ok);
    return failures;
}
