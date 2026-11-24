/* What a message says to a screen reader (REQ-269, ARCH-99): its accessible
 * name, built from its parts under the person's own settings.
 *
 * A message is read as its sender, its time and its text by default -- "Ada,
 * 2m: lunch?" -- and a person who uses a screen reader all day can switch each
 * part on or off, add the reactions and the thread's reply count, and hear the
 * text first. Every graphical client names a message through this one function,
 * so the settings mean the same thing wherever they are read.
 *
 * No allocation; the result is written into the caller's buffer. */
#ifndef OC_SPOKEN_H
#define OC_SPOKEN_H

#include <stddef.h>
#include <stdint.h>

#include "model.h"   /* oc_reaction */

/* The five switches. Each is 0 or 1. */
typedef struct {
    int sender;      /* the sender's name */
    int time;        /* when it was sent, as the transcript shows it */
    int reactions;   /* "reactions: <emoji> <count>, ..." */
    int replies;     /* "1 reply", "<n> replies" */
    int text_first;  /* the text before the sender and the time */
} oc_spoken_opts;

/* The sender and the time on, everything else off: a message then reads exactly
 * as it always has. */
oc_spoken_opts oc_spoken_defaults(void);

/* The spoken name of one message, into `out` (`cap` bytes, NUL-terminated
 * whenever cap > 0). `who` NULL or empty reads "someone"; `when` NULL reads as
 * empty; `text` is what the message says, and a deleted one reads "message
 * deleted" in its place. `r` is its `n_r` reactions in the message's order,
 * `replies` its thread's reply count.
 *
 * The rules, stated once:
 *   - each part appears only when its switch is on; the text always does;
 *   - sender first: "<who>, <when>: <text>", or "<who>, <when>, message deleted";
 *   - text first: "<text>, <who>, <when>";
 *   - then ", reactions: <emoji> <count>, ..." when there are any;
 *   - then ", 1 reply" or ", <n> replies" when there are any;
 *   - cut at `cap` on a whole UTF-8 character, never through one.
 * Returns the length written. */
size_t oc_spoken_msg(const oc_spoken_opts *o, const char *who, const char *when,
                     const char *text, const oc_reaction *r, int n_r,
                     uint32_t replies, int deleted, char *out, size_t cap);

/* The five switches as one number and back, for a setting: bit 0 sender, 1 time,
 * 2 reactions, 3 replies, 4 text_first. */
unsigned oc_spoken_bits(const oc_spoken_opts *o);
oc_spoken_opts oc_spoken_from_bits(unsigned bits);

#endif
