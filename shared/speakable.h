/*
 * OpenChime — a message as it is read aloud (REQ-291–295, ARCH-111,
 * docs/READ-ALOUD.md §3).
 *
 * Synthesis turns text into sound; this decides WHICH text. It is the layer the
 * feature works or fails on: a raw body read verbatim says "asterisk" and spells
 * out URLs and code. The rules, applied over the same spans the clients render
 * (shared/richtext.c) and the same mentions the daemon resolves (shared/mention.c):
 *
 *   - a fenced code block is announced and skipped: "Code block, 12 lines.";
 *     inline code is read as written;
 *   - a URL is reduced to its host, without "www.";
 *   - @someone becomes their display name, through the caller's resolver;
 *     @here is "everyone here", @channel and @everyone "everyone";
 *   - formatting delimiters, list markers and escapes are dropped;
 *   - a quoted line is introduced once per quote with "Quote:";
 *   - emoji and :shortcodes: are dropped;
 *   - each line ends a sentence, and whitespace is collapsed.
 *
 * A message that leaves nothing to say — a bare attachment, only emoji — is
 * NOT RENDERABLE, and says so by producing zero bytes.
 */
#ifndef OC_SPEAKABLE_H
#define OC_SPEAKABLE_H

#include <stddef.h>

/* The longest speakable text produced; longer bodies are cut at a sentence. */
#define OC_SPEAK_MAX 4096

/* Resolve a mention's name (as written, no '@') to what should be said, or NULL
 * to say the name as written. */
typedef const char *(*oc_speak_name_fn)(void *ctx, const char *name);

/* Write the speakable form of `body` (`len` bytes) into `out` (`cap` bytes,
 * NUL-terminated). Returns the length written; 0 means not renderable. */
size_t oc_speakable(const char *body, size_t len, oc_speak_name_fn resolve, void *ctx,
                    char *out, size_t cap);

/* Split speakable text into segments of at most `max_chars`, at sentence ends
 * where possible, so a long message synthesizes in pieces. Writes up to `max`
 * (start, length) pairs and returns how many there are. */
int oc_speakable_segments(const char *text, size_t len, size_t max_chars,
                          size_t *starts, size_t *lens, int max);

#endif
