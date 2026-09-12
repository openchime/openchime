/*
 * tuikit — theme tokens (tk_theme). Semantic 256-color slots so widgets and the
 * app reference roles (accent, muted, selection, …) instead of raw TB_* / palette
 * indices. Requires 256-color output (tb_set_output_mode(TB_OUTPUT_256)); values
 * are xterm-256 indices (0 = terminal default; 232-255 = grayscale ramp; 16-231 =
 * the 6×6×6 cube). TB_BOLD is still OR'd in by callers where an *attribute* (not a
 * color) is wanted.
 */

#ifndef TK_THEME_H
#define TK_THEME_H

#include "tk_term.h"   /* uintattr_t + the original TB_* */

/* In 256-color output, the color value is a raw xterm-256 index, but termbox2's
 * named constants are offset by one (TB_DEFAULT=0 so TB_BLACK=1, TB_RED=2, …) —
 * so TB_RED(2) would emit index 2 = green. Remap the 8 names to their true xterm
 * indices: indices 1..7 ARE the standard ANSI colors, so the on-screen look is
 * identical to the old 8-color mode, while values 8..255 (the theme tokens below)
 * now render as real 256 colors. TB_DEFAULT(0) and the TB_BOLD/TB_REVERSE
 * attribute bits are unchanged. This header is included by every color-writing
 * TU (widgets + app); tk_term.c does NOT include it, so termbox2's own
 * implementation keeps the original constants and just passes numbers through. */
#undef  TB_BLACK
#undef  TB_RED
#undef  TB_GREEN
#undef  TB_YELLOW
#undef  TB_BLUE
#undef  TB_MAGENTA
#undef  TB_CYAN
#undef  TB_WHITE
#define TB_BLACK   8   /* bright-black / gray (matches the old TB_BLACK|TB_BOLD) */
#define TB_RED     1
#define TB_GREEN   2
#define TB_YELLOW  3
#define TB_BLUE    4
#define TB_MAGENTA 5
#define TB_CYAN    6
#define TB_WHITE   7

typedef struct {
    uintattr_t bg;             /* app background (0 = terminal default) */
    uintattr_t surface;        /* panels, header/footer bars */
    uintattr_t fg;             /* primary text */
    uintattr_t muted;          /* secondary text (timestamps, descriptions) */
    uintattr_t faint;          /* tertiary (key hints, disabled) */
    uintattr_t accent;         /* section headers, active borders, prompts */
    uintattr_t accent2;        /* secondary accent (badges, highlights) */
    uintattr_t sel_bg, sel_fg; /* selected row */
    uintattr_t border;         /* inactive panel/modal border */
    uintattr_t border_active;  /* focused panel / modal border */
    uintattr_t ok, warn, err;  /* status colors */
    uintattr_t presence_on, presence_away, presence_off;
    uintattr_t header_bg, footer_bg;
} tk_theme;

/* The active theme (a default is provided; swap later for user themes). */
const tk_theme *tk_theme_active(void);

#endif /* TK_THEME_H */
