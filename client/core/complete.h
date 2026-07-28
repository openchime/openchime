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

/* The whole catalogue, and the display name of a category. */
const oc_emoji *oc_emoji_all(size_t *count);
const char     *oc_emoji_category_name(uint8_t category);

/* The emoji for a shortcode ("fire", no colons), or NULL. */
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

#endif /* OC_COMPLETE_H */
