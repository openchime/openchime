/*
 * tuikit — tk_viewport: a scrollable region over pre-rendered lines. The app
 * builds an array of line strings (+ optional per-line colours) each frame; the
 * viewport owns the scroll offset and draws the visible slice. Backs the message
 * pane and any content taller than its area.
 */

#ifndef TK_VIEWPORT_H
#define TK_VIEWPORT_H

#include "tk_term.h"
#include "tk_common.h"

typedef struct {
    const char *const *lines;   /* borrowed; valid through draw */
    const uintattr_t  *fg;      /* borrowed per-line fg, or NULL for TB_DEFAULT */
    int   n;                    /* line count */
    int   top;                  /* first visible line */
    int   follow;               /* 1 = stick to the bottom (chat) */
} tk_viewport;

void        tk_viewport_init(tk_viewport *v, int follow);
void        tk_viewport_set(tk_viewport *v, const char *const *lines,
                            const uintattr_t *fg, int n);
/* PgUp/PgDn/arrows/wheel scroll; scrolling up drops follow. `height` = visible rows. */
tk_result   tk_viewport_handle(tk_viewport *v, const struct tb_event *ev, int height);
void        tk_viewport_draw(tk_viewport *v, tk_rect r);

#endif /* TK_VIEWPORT_H */
