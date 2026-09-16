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
    printf("test_speakable: plain text, formatting, code, links, mentions, quotes, emoji, lines, not renderable, segments\n");
    CHECK(said("Can you look at the logs?", "Can you look at the logs?"));
    CHECK(said("this is *really* _important_ and ~not~ that", "this is really important and not that"));
    CHECK(said("run `make test` first", "run make test first"));
    CHECK(said("before\n```\nint x = 1;\nint y = 2;\nreturn x;\n```\nafter", "before. Code block, 3 lines. after"));
    CHECK(said("see https://www.github.com/openchime/openchime/pull/285 for it", "see github.com for it"));
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
