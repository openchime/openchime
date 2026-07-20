/*
 * tuikit — keymap + help (tk_key). Keybindings are data: each carries the key it
 * matches AND its own help label + description. The footer and the full-help
 * overlay are rendered from the same binding arrays that the input code matches
 * against, so the on-screen hints can never drift from what the keys actually do
 * — the standing bug of hand-written hint strings.
 *
 * A screen/widget declares a static tk_binding[] once, matches events with
 * tk_key_match(), and draws its hints with tk_help_footer()/tk_help_full().
 */

#ifndef TK_KEY_H
#define TK_KEY_H

#include <stdint.h>
#include "tk_term.h"

typedef struct {
    int         key;        /* TB_KEY_* to match (0 = match on ch instead) */
    uint32_t    ch;         /* character to match (0 = match on key instead) */
    const char *help_key;   /* footer label, e.g. "j/k", "Enter", "Tab" */
    const char *help_desc;  /* what it does, e.g. "select", "open" */
    int         enabled;    /* 0 = not matched and hidden from help */
} tk_binding;

/* True if event `ev` matches binding `b` (and b is enabled). */
int tk_key_match(const struct tb_event *ev, const tk_binding *b);

/* Draw a one-line hint footer of the enabled bindings — "key desc · key desc · …"
 * — on row `y` across columns [x0, x1). Returns the next free column. */
int tk_help_footer(int y, int x0, int x1, const tk_binding *b, int n,
                   uintattr_t fg, uintattr_t bg);

/* Draw a centered full-help modal listing every enabled binding (key + desc),
 * titled `title`, over a W×H screen. Any key closes it (caller's loop). */
void tk_help_full(int W, int H, const char *title, const tk_binding *b, int n);

#endif /* TK_KEY_H */
