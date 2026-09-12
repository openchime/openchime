/*
 * tuikit — tk_theme. See tk_theme.h. A single dark default theme in the xterm-256
 * palette, tuned toward the opencode/Charm look: soft grays, a purple accent, a
 * subtle selection instead of a solid block.
 */

#include "tk_theme.h"

static const tk_theme g_default = {
    .bg            = 0,     /* terminal default background */
    .surface       = 236,  /* dark gray — panels, header/footer */
    .fg            = 252,   /* near-white */
    .muted         = 245,   /* mid gray — timestamps, descriptions */
    .faint         = 240,   /* dark gray — key hints, disabled */
    .accent        = 141,   /* soft purple — headers, active borders */
    .accent2       = 173,   /* peach — badges, highlights */
    .sel_bg        = 238,   /* subtle highlight bar (not a solid block) */
    .sel_fg        = 231,   /* bright white on the selection */
    .border        = 240,
    .border_active = 141,
    .ok            = 114,   /* green */
    .warn          = 179,   /* amber */
    .err           = 203,   /* red */
    .presence_on   = 114,
    .presence_away = 179,
    .presence_off  = 240,
    .header_bg     = 236,
    .footer_bg     = 236,
};

const tk_theme *tk_theme_active(void) { return &g_default; }
