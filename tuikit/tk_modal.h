/*
 * tuikit — tk_modal: a centered overlay. Draws a bordered, titled box over the
 * current frame and returns the inner content rect; the app then draws widgets
 * (a list, a form) inside it. The container for menus, dialogs, and confirms.
 * Focus capture is the app's job (route events to the modal's widgets while open).
 */

#ifndef TK_MODAL_H
#define TK_MODAL_H

#include "tk_common.h"

/* Draw a centered w×h box titled `title` over a W×H screen, clear its interior,
 * and return the inner content rect (inside the border + 1 col padding). */
tk_rect tk_modal_begin(int W, int H, int w, int h, const char *title);

/* Convenience: a confirm prompt box asking `question`; the app reads y/n itself.
 * Returns the inner rect where the question was drawn. */
tk_rect tk_modal_confirm(int W, int H, const char *question);

#endif /* TK_MODAL_H */
