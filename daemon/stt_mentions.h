/*
 * OpenChime — spoken mentions (ARCH-112, docs/VOICE-INPUT.md §6).
 *
 * A recognizer writes what it hears: "send it to at Dana" rather than
 * "@dana". Before the text is posted or returned, "at" followed by a name that
 * belongs to exactly one member of the conversation becomes a mention in the
 * form the shared scanner (shared/mention.h) finds and the daemon resolves
 * (ARCH-89). A name nobody has, or that two people share, stays words.
 */
#ifndef OC_STT_MENTIONS_H
#define OC_STT_MENTIONS_H

#include <stddef.h>

/* Rewrite `text` into `out` (NUL-terminated within `cap`). `names` are the
 * members' display names. Returns the length written. */
size_t oc_stt_mentions(const char *text, const char *const *names, size_t n_names, char *out, size_t cap);

#endif
