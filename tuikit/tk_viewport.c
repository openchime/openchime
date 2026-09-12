/*
 * tuikit — tk_viewport. See tk_viewport.h.
 */

#include "tk_viewport.h"
#include "tk_theme.h"
#include "tk_draw.h"

void tk_viewport_init(tk_viewport *v, int follow) {
    v->lines = NULL; v->fg = NULL; v->n = 0; v->top = 0; v->follow = follow;
}

void tk_viewport_set(tk_viewport *v, const char *const *lines,
                     const uintattr_t *fg, int n) {
    v->lines = lines; v->fg = fg; v->n = n;
}

static void clamp(tk_viewport *v, int height) {
    int max_top = v->n - height; if (max_top < 0) max_top = 0;
    if (v->follow) v->top = max_top;
    if (v->top > max_top) v->top = max_top;
    if (v->top < 0) v->top = 0;
}

tk_result tk_viewport_handle(tk_viewport *v, const struct tb_event *ev, int height) {
    if (ev->type != TB_EVENT_KEY && ev->type != TB_EVENT_MOUSE) return TK_NONE;
    int step = 0;
    switch (ev->key) {
        case TB_KEY_ARROW_UP:          step = -1; break;
        case TB_KEY_ARROW_DOWN:        step =  1; break;
        case TB_KEY_PGUP:              step = -(height - 1); break;
        case TB_KEY_PGDN:              step =  (height - 1); break;
        case TB_KEY_MOUSE_WHEEL_UP:    step = -3; break;
        case TB_KEY_MOUSE_WHEEL_DOWN:  step =  3; break;
        default: return TK_NONE;
    }
    v->top += step;
    if (step < 0) v->follow = 0;                 /* user scrolled up: stop following */
    int max_top = v->n - height; if (max_top < 0) max_top = 0;
    if (v->top >= max_top) { v->top = max_top; v->follow = 1; }  /* back at bottom: resume */
    if (v->top < 0) v->top = 0;
    return TK_CHANGED;
}

void tk_viewport_draw(tk_viewport *v, tk_rect r) {
    clamp(v, r.h);
    for (int j = 0; j < r.h; j++) {
        int idx = v->top + j;
        tk_fill(r.y + j, r.x, r.x + r.w, TB_DEFAULT);
        if (idx < 0 || idx >= v->n || !v->lines[idx]) continue;
        uintattr_t fg = v->fg ? v->fg[idx] : TB_DEFAULT;
        tk_text(r.x, r.y + j, r.x + r.w, v->lines[idx], fg, TB_DEFAULT);
    }
}
