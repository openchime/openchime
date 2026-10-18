/*
 * tuikit — tk_textarea: a multi-line, soft-wrapping text field (the message
 * composer). Appends/deletes at the end (a full 2-D cursor is a later
 * refinement); wraps to the draw width. Enter reports TK_SELECT so the app
 * decides send-vs-newline.
 */

#ifndef TK_TEXTAREA_H
#define TK_TEXTAREA_H

#include "tk_term.h"
#include "tk_common.h"

#define TK_TEXTAREA_CAP 2048

typedef struct {
    char        buf[TK_TEXTAREA_CAP];
    const char *placeholder;
} tk_textarea;

void        tk_textarea_init(tk_textarea *t, const char *placeholder);
void        tk_textarea_clear(tk_textarea *t);
const char *tk_textarea_value(const tk_textarea *t);
tk_result   tk_textarea_handle(tk_textarea *t, const struct tb_event *ev);
/* Draw wrapped within rect r; `focused` shows the cursor at the end. */
void        tk_textarea_draw(tk_textarea *t, tk_rect r, int focused);

#endif /* TK_TEXTAREA_H */
