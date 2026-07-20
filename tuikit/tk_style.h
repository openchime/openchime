/*
 * tuikit — the formatting/style layer (tk_style): the Lip Gloss subset that
 * survives when the renderer is a cell grid instead of ANSI strings — bordered
 * boxes with an optional title. Padding/alignment helpers grow here as widgets
 * need them.
 */

#ifndef TK_STYLE_H
#define TK_STYLE_H

#include "tk_term.h"

/* A bordered, optionally-titled panel at (x,y) sized w×h. `active` draws the
 * border cyan+bold (focused) instead of white. Does not clear the interior. */
void tk_panel(int x, int y, int w, int h, const char *title, int active);

/* A single-line white box border that also clears its interior with spaces —
 * for centered modals/dialogs drawn over the main frame. */
void tk_box(int x, int y, int w, int h);

#endif /* TK_STYLE_H */
