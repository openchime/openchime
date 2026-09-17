/*
 * OpenChime — choosing the voice a person's messages are read in, the first time
 * they are read (REQ-292, ARCH-111, READ-ALOUD.md §3).
 *
 * Pure: strings and an id in, an index out. No database and no engine, so every
 * rule is testable directly.
 */
#ifndef OC_VOICE_PICK_H
#define OC_VOICE_PICK_H

#include <stdint.h>

/* The index into `voices` -- the engine's voice ids, comma separated -- of the
 * voice to read `user_id` in.
 *
 * A voice the user already has (`have`) stands while the engine still offers it,
 * and *persist is set to 0. Otherwise a new choice is made and *persist is set to
 * 1, so it is written to the profile and shown there rather than recomputed:
 * where the engine speaks English (`lang` begins "en"), the first pronoun in
 * `pronouns` that names a presentation prefers the voices whose id ends "-f" or
 * "-m"; the choice among the candidates is a stable hash of `user_id`, and if no
 * voice carries the wanted ending every voice is a candidate. Returns 0 when
 * `voices` is empty. */
int oc_voice_pick(const char *lang, const char *voices, const char *have,
                  const char *pronouns, uint64_t user_id, int *persist);

#endif
