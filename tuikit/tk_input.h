/*
 * tuikit — tk_input: a single-line text field. Cursor, placeholder, password
 * echo, horizontal scroll. UTF-8 aware editing (utf8proc). The composer, search
 * queries, and every form field are tk_inputs.
 */

#ifndef TK_INPUT_H
#define TK_INPUT_H

#include "tk_term.h"
#include "tk_common.h"

#define TK_INPUT_CAP 512

typedef struct {
    char        buf[TK_INPUT_CAP];
    int         mask;            /* 1 = render as '*' (password) */
    const char *placeholder;
} tk_input;

void        tk_input_init(tk_input *in, int mask, const char *placeholder);
void        tk_input_clear(tk_input *in);
void        tk_input_set(tk_input *in, const char *text);
const char *tk_input_value(const tk_input *in);
/* Enter -> TK_SELECT, Esc -> TK_CANCEL, edit -> TK_CHANGED, else TK_NONE. */
tk_result   tk_input_handle(tk_input *in, const struct tb_event *ev);
/* Draw within rect r (single row); `focused` shows the cursor. */
void        tk_input_draw(tk_input *in, tk_rect r, int focused);

#endif /* TK_INPUT_H */
