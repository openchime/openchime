/*
 * What makes a message an action (REQ-058, ARCH-115).
 *
 * A body that begins "/me", then one or more spaces, then text, describes what
 * its author is doing: "/me is away" reads "Ada Starr is away". The body stays
 * exactly what was typed (REQ-054); the daemon records the action as its own
 * object beside it, and every surface reads that object.
 *
 * The daemon is this function's only caller. A client never asks whether a body
 * is an action — it reads the answer the daemon sent — so there is one place
 * that decides, and the migration that backfilled existing messages is tested
 * against it.
 *
 * No allocation; the result is offsets into the caller's bytes. */

#ifndef OC_ACTION_H
#define OC_ACTION_H

#include <stddef.h>
#include <stdint.h>

/* Returns 1 when `body` (`len` bytes, not necessarily NUL-terminated) is an
 * action, setting `*start` to the byte offset of the action text and `*tlen` to
 * its length, which runs to the end of the body so later lines stay part of it.
 * Returns 0 otherwise, leaving both untouched.
 *
 * The rule, stated once:
 *   - the body begins with exactly the three bytes "/me" — case matters, and
 *     nothing may precede them;
 *   - then one or more ASCII spaces (0x20);
 *   - then a byte that is not whitespace (space, tab, CR or LF).
 * So "/me", "/me ", "/me \nx", "/mex", " /me x" and "/ME x" are not actions. */
int oc_action_parse(const char *body, size_t len, uint32_t *start, uint32_t *tlen);

#endif
