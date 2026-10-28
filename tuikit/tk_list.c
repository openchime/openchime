/*
 * tuikit — tk_list. See tk_list.h.
 */

#include "tk_list.h"
#include "tk_theme.h"
#include "tk_draw.h"

#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>      /* snprintf */
#include <string.h>

/* Case-insensitive subsequence ("fuzzy") match: do needle's chars appear in
 * order within hay? Empty needle matches everything. */
static int fuzzy(const char *hay, const char *needle) {
    if (!*needle) return 1;
    for (; *hay; hay++)
        if (tolower((unsigned char)*hay) == tolower((unsigned char)*needle))
            if (!*++needle) return 1;
    return 0;
}

void tk_list_init(tk_list *l, const tk_list_opts *o) {
    memset(l, 0, sizeof *l);
    l->opts = *o;
}

void tk_list_free(tk_list *l) {
    free(l->view);
    l->view = NULL; l->vcap = l->nview = 0;
}

/* Rebuild the filtered view map from the live item count + current filter, then
 * clamp the selection. Cheap enough to run every handle/draw. */
static void rebuild(tk_list *l) {
    int cnt = l->opts.count ? l->opts.count(l->opts.ud) : 0;
    if (cnt > l->vcap) {
        int cap = l->vcap ? l->vcap * 2 : 32;
        while (cap < cnt) cap *= 2;
        l->view = (int *)realloc(l->view, (size_t)cap * sizeof(int));
        l->vcap = cap;
    }
    l->nview = 0;
    for (int i = 0; i < cnt; i++) {
        if (l->filter[0]) {
            char fb[256];
            if (l->opts.item_filter) l->opts.item_filter(i, l->opts.ud, fb, sizeof fb);
            else { uintattr_t fg; l->opts.item_text(i, l->opts.ud, fb, sizeof fb, &fg); }
            if (!fuzzy(fb, l->filter)) continue;
        }
        l->view[l->nview++] = i;
    }
    if (l->sel >= l->nview) l->sel = l->nview - 1;
    if (l->sel < 0) l->sel = 0;
}

int tk_list_selected(tk_list *l) {
    rebuild(l);
    if (l->nview == 0) return -1;
    return l->view[l->sel];
}

void tk_list_reset_filter(tk_list *l) {
    l->filter[0] = '\0'; l->filtering = 0;
}

tk_result tk_list_handle(tk_list *l, const struct tb_event *ev) {
    rebuild(l);
    if (ev->type != TB_EVENT_KEY) return TK_NONE;

    /* Single-mode: arrows navigate, typing filters live (no j/k, no '/' gate). */
    switch (ev->key) {
        case TB_KEY_ARROW_UP:   case TB_KEY_MOUSE_WHEEL_UP:   l->sel--; break;
        case TB_KEY_ARROW_DOWN: case TB_KEY_MOUSE_WHEEL_DOWN: l->sel++; break;
        case TB_KEY_ENTER:      return l->nview ? TK_SELECT : TK_USED;
        case TB_KEY_ESC:
            if (l->opts.filterable && l->filter[0]) { l->filter[0] = '\0'; l->sel = 0; return TK_CHANGED; }
            return TK_CANCEL;
        case TB_KEY_BACKSPACE: case TB_KEY_BACKSPACE2:
            if (l->opts.filterable) {
                size_t n = strlen(l->filter); if (n) { l->filter[n - 1] = '\0'; l->sel = 0; }
                return TK_CHANGED;
            }
            return TK_NONE;
        default:
            if (l->opts.filterable && ev->ch >= 0x20 && ev->ch < 0x7f) {
                size_t n = strlen(l->filter);
                if (n + 1 < sizeof l->filter) { l->filter[n] = (char)ev->ch; l->filter[n + 1] = '\0'; l->sel = 0; }
                return TK_CHANGED;
            }
            return TK_NONE;
    }
    if (l->sel < 0) l->sel = 0;
    if (l->sel >= l->nview) l->sel = l->nview ? l->nview - 1 : 0;
    return TK_CHANGED;
}

void tk_list_draw(tk_list *l, tk_rect r) {
    const tk_theme *th = tk_theme_active();
    rebuild(l);
    int y = r.y, rows = r.h;
    /* live filter query line at the top once the user starts typing */
    if (l->opts.filterable && l->filter[0] && rows > 0) {
        char q[80]; snprintf(q, sizeof q, "%s", l->filter);
        tk_fill(y, r.x, r.x + r.w, th->bg);
        int cx = tk_text(r.x, y, r.x + r.w, q, th->accent | TB_BOLD, th->bg);
        if (cx < r.x + r.w) tb_set_cell(cx, y, ' ', TB_DEFAULT, TB_REVERSE);
        y++; rows--;
    }
    /* keep selection visible */
    if (l->sel < l->top) l->top = l->sel;
    if (l->sel >= l->top + rows) l->top = l->sel - rows + 1;
    if (l->top < 0) l->top = 0;

    for (int k = l->top; k < l->nview && k < l->top + rows; k++, y++) {
        int sel = (k == l->sel);
        uintattr_t fg = th->fg, bg = sel ? th->sel_bg : th->bg;
        char buf[256];
        l->opts.item_text(l->view[k], l->opts.ud, buf, sizeof buf, &fg);
        if (sel) { fg = th->sel_fg | TB_BOLD; tk_fill(y, r.x, r.x + r.w, bg); }
        tk_text(r.x, y, r.x + r.w, buf, fg, bg);
    }
}
