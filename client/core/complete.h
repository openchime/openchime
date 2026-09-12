/*
 * OpenChime client core — composer completion and the emoji catalogue.
 *
 * Both live here rather than in a frontend because both frontends need exactly
 * the same answers (ARCH-74): a `:fire:` typed in the TUI and a `:fire:` typed
 * in the Win32 GUI must complete to the same character, and a mention must
 * match the same users. The TUI grew these inline first; this is that logic
 * lifted, not a second implementation.
 */

#ifndef OC_COMPLETE_H
#define OC_COMPLETE_H

#include "model.h"

#include <stddef.h>

/* ---- emoji catalogue ------------------------------------------------------
 * A curated set with shortcodes and categories — not all of Unicode. A picker
 * over every emoji is mostly unusable, and the shortcodes are what actually get
 * typed; the catalogue covers the ones people reach for. */
typedef struct {
    const char *name;      /* shortcode, without the surrounding colons */
    const char *emoji;     /* the UTF-8 character(s) */
    const char *keywords;  /* extra space-separated search terms, may be "" */
    uint8_t     category;  /* OC_EMOJI_CAT_* */
    /* Nonzero if a Fitzpatrick skin-tone modifier may be appended. Set only for
     * single-codepoint bases: appending after a ZWJ sequence is not the same
     * operation, so those are left alone rather than made subtly wrong. */
    uint8_t     tonable;
} oc_emoji;

enum {
    OC_EMOJI_CAT_SMILEYS = 0,
    OC_EMOJI_CAT_GESTURES,
    OC_EMOJI_CAT_PEOPLE,
    OC_EMOJI_CAT_NATURE,
    OC_EMOJI_CAT_FOOD,
    OC_EMOJI_CAT_ACTIVITY,
    OC_EMOJI_CAT_OBJECTS,
    OC_EMOJI_CAT_SYMBOLS,
    OC_EMOJI_CAT_COUNT
};

/* ---- skin tones -----------------------------------------------------------
 * Unicode spells a skin tone as the base emoji followed by a Fitzpatrick
 * modifier (U+1F3FB..U+1F3FF). It applies only to emoji that depict a person or
 * a body part, which is what `oc_emoji.tonable` marks. */
enum {
    OC_SKIN_DEFAULT = 0,   /* the yellow base, no modifier */
    OC_SKIN_LIGHT,
    OC_SKIN_MEDIUM_LIGHT,
    OC_SKIN_MEDIUM,
    OC_SKIN_MEDIUM_DARK,
    OC_SKIN_DARK,
    OC_SKIN_COUNT
};

/* Display name of a tone ("Medium-light"), or "" if out of range. */
const char *oc_emoji_skin_name(uint8_t tone);

/* Write `e` with `tone` applied into `out` (NUL-terminated), returning the
 * length written, or 0 if it would not fit. A tone of OC_SKIN_DEFAULT, an
 * out-of-range tone, or an emoji that is not `tonable` all yield the base
 * character unchanged — so a caller may apply a user's chosen tone blindly to
 * anything in the catalogue and get the right answer. */
size_t oc_emoji_with_tone(const oc_emoji *e, uint8_t tone, char *out, size_t cap);

/* Upper bound on the catalogue's size, so a caller can size a hit buffer that
 * cannot silently truncate. complete.c static-asserts the table against it: the
 * build breaks if the catalogue outgrows this, rather than a picker quietly
 * showing the first N and dropping whole categories off the end. */
#define OC_EMOJI_MAX 1024

/* The whole catalogue, and the display name of a category. */
const oc_emoji *oc_emoji_all(size_t *count);
const char     *oc_emoji_category_name(uint8_t category);

/* The emoji for a shortcode ("fire", no colons), or NULL. Also resolves the
 * older spellings that shipped before the catalogue was expanded, so a message
 * already stored with `:sweat_smile:` keeps rendering. */
const char     *oc_emoji_by_name(const char *name);

/* Fill `out` with catalogue entries matching `query` (matched against the
 * shortcode and the keywords; an empty or NULL query matches everything, in
 * catalogue order). Returns how many were written. */
size_t oc_emoji_search(const char *query, const oc_emoji **out, size_t max);

/* ---- composer completion --------------------------------------------------
 * `repl` is the text that replaces the trailing token; `disp` is what to show
 * in the list. */
typedef struct { char repl[80]; char disp[96]; } oc_completion;

enum {
    OC_AC_NONE = 0,
    OC_AC_EMOJI,     /* :short   */
    OC_AC_MENTION,   /* @user    */
    OC_AC_CHANNEL    /* #channel */
};

/* Completions for the trailing token of `text` (the text up to the caret).
 *
 * Returns the number written to `out`. `*repl_start` receives the byte offset in
 * `text` where the replacement begins, and `*kind` the OC_AC_* that matched — a
 * frontend uses the kind to label or ornament the list. Both out-params are
 * optional.
 *
 * A token is only a trigger at a word boundary, so an email address does not
 * turn into a mention popup mid-word. */
size_t oc_complete(const oc_model *m, const char *text,
                   oc_completion *out, size_t max, int *repl_start, int *kind);

/* ---- addressing a message (REQ-229) ---------------------------------------
 * People AND channels, ranked together, for a query with NO sigil — which is
 * what a "To:" field takes. oc_complete above dispatches on the leading `@`,
 * `#` or `:`, so it structurally cannot answer this: it has to be told which
 * kind you meant before it will look. A picker is asked the opposite question.
 *
 * Ranking: prefix matches before substring matches, people before channels
 * within each band (you address a person more often than a channel), each band
 * alphabetical so the list does not reshuffle as you type.
 *
 * `out[i].repl` carries the id as text — "u123" for a user, "c45" for a channel
 * — because the caller needs to resolve a choice, not re-parse a display name
 * that may not be unique. `disp` is what to show. */
typedef struct { uint64_t id; int is_channel; char name[80]; char sub[80]; } oc_target;
size_t oc_complete_targets(const oc_model *m, const char *query,
                           oc_target *out, size_t max);

#endif /* OC_COMPLETE_H */
