/*
 * tuikit — width-correct drawing primitives (tk_draw). Every higher-level widget
 * and the style layer draw through these. They wrap utf8proc so a glyph's display
 * width is always correct — the one thing that, gotten wrong, shifts a whole row
 * and corrupts the layout.
 */

#ifndef TK_DRAW_H
#define TK_DRAW_H

#include <stdint.h>
#include "tk_term.h"   /* uintattr_t, tb_set_cell, TB_* colors */

/* Display width of a codepoint (0 for control/combining, 1 or 2 otherwise). */
int  tk_cp_width(int32_t cp);

/* Display width (columns) of a whole UTF-8 string. */
int  tk_str_width(const char *s);

/* Draw a UTF-8 string at (x,y), clipped to columns [x, xmax); advances by each
 * glyph's display width; won't split a wide glyph at the edge. Returns the next
 * free column. */
int  tk_text(int x, int y, int xmax, const char *s, uintattr_t fg, uintattr_t bg);

/* Fill columns [x0, x1) on row y with spaces in the given background. */
void tk_fill(int y, int x0, int x1, uintattr_t bg);

/* Draw `s` right-aligned within [x0, x1) on row y: its last glyph sits at x1-1.
 * Clipped to x0 on the left. Returns the starting column. */
int  tk_text_right(int y, int x0, int x1, const char *s, uintattr_t fg, uintattr_t bg);

#endif /* TK_DRAW_H */
