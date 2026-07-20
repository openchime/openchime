/*
 * tuikit — tk_modal. See tk_modal.h.
 */

#include "tk_modal.h"
#include "tk_draw.h"
#include "tk_style.h"
#include "tk_term.h"

#include <string.h>

tk_rect tk_modal_begin(int W, int H, int w, int h, const char *title) {
    if (w > W - 2) w = W - 2; if (w < 8) w = 8;
    if (h > H - 2) h = H - 2; if (h < 3) h = 3;
    int x = (W - w) / 2, y = (H - h) / 2; if (x < 0) x = 0; if (y < 0) y = 0;
    tk_box(x, y, w, h);
    if (title && title[0]) {
        char t[96]; snprintf(t, sizeof t, " %s ", title);
        tk_text(x + 2, y, x + w - 1, t, TB_CYAN | TB_BOLD, TB_DEFAULT);
    }
    tk_rect inner = { x + 2, y + 1, w - 4, h - 2 };
    return inner;
}

tk_rect tk_modal_confirm(int W, int H, const char *question) {
    int w = (int)strlen(question) + 8; if (w < 30) w = 30;
    tk_rect in = tk_modal_begin(W, H, w, 5, "Confirm");
    tk_text(in.x, in.y, in.x + in.w, question, TB_DEFAULT, TB_DEFAULT);
    tk_text(in.x, in.y + in.h - 1, in.x + in.w, "y confirm  ·  n / Esc cancel", TB_BLACK | TB_BOLD, TB_DEFAULT);
    return in;
}
