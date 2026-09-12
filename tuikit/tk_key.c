/*
 * tuikit — tk_key. See tk_key.h.
 */

#include "tk_key.h"
#include "tk_theme.h"
#include "tk_draw.h"
#include "tk_style.h"

#include <stdio.h>
#include <string.h>

int tk_key_match(const struct tb_event *ev, const tk_binding *b) {
    if (!b->enabled) return 0;
    if (b->key && ev->key == (uint16_t)b->key) return 1;
    if (b->ch  && ev->ch  == b->ch)            return 1;
    return 0;
}

int tk_help_footer(int y, int x0, int x1, const tk_binding *b, int n,
                   uintattr_t fg, uintattr_t bg) {
    int x = x0;
    tk_fill(y, x0, x1, bg);
    x = tk_text(x, y, x1, " ", fg, bg);
    int first = 1;
    for (int i = 0; i < n && x < x1; i++) {
        if (!b[i].enabled) continue;
        if (!first) x = tk_text(x, y, x1, " \xc2\xb7 ", fg, bg);   /* " · " */
        first = 0;
        char item[96];
        if (b[i].help_desc && b[i].help_desc[0])
            snprintf(item, sizeof item, "%s %s", b[i].help_key ? b[i].help_key : "", b[i].help_desc);
        else
            snprintf(item, sizeof item, "%s", b[i].help_key ? b[i].help_key : "");
        x = tk_text(x, y, x1, item, fg, bg);
    }
    return x;
}

void tk_help_full(int W, int H, const char *title, const tk_binding *b, int n) {
    int enabled = 0, keyw = 0;
    for (int i = 0; i < n; i++) {
        if (!b[i].enabled) continue;
        enabled++;
        int kw = b[i].help_key ? tk_str_width(b[i].help_key) : 0;
        if (kw > keyw) keyw = kw;
    }
    int bw = 48; if (bw > W - 2) bw = W - 2; if (bw < 20) bw = 20;
    int bh = enabled + 4; if (bh > H - 2) bh = H - 2; if (bh < 5) bh = 5;
    int x = (W - bw) / 2, y = (H - bh) / 2; if (x < 0) x = 0; if (y < 0) y = 0;
    const tk_theme *th = tk_theme_active();
    tk_box(x, y, bw, bh);
    char t[64]; snprintf(t, sizeof t, " %s ", title ? title : "Help");
    tk_text(x + 2, y, x + bw - 1, t, th->accent | TB_BOLD, th->bg);

    int row = y + 2;
    for (int i = 0; i < n && row < y + bh - 1; i++) {
        if (!b[i].enabled) continue;
        tk_text(x + 3, row, x + 3 + keyw + 1, b[i].help_key ? b[i].help_key : "",
                th->accent | TB_BOLD, th->bg);
        tk_text(x + 3 + keyw + 2, row, x + bw - 2, b[i].help_desc ? b[i].help_desc : "",
                th->fg, th->bg);
        row++;
    }
    tk_text(x + 2, y + bh - 1, x + bw - 1, " any key to close ", th->muted, th->bg);
}
