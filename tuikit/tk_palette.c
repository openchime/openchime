/*
 * tuikit — tk_palette. See tk_palette.h.
 */

#include "tk_palette.h"
#include "tk_theme.h"
#include "tk_draw.h"
#include "tk_style.h"

#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>      /* snprintf */
#include <string.h>

static int fuzzy(const char *hay, const char *needle) {
    if (!*needle) return 1;
    for (; *hay; hay++)
        if (tolower((unsigned char)*hay) == tolower((unsigned char)*needle))
            if (!*++needle) return 1;
    return 0;
}

void tk_palette_init(tk_palette *p, const char *title) {
    memset(p, 0, sizeof *p);
    p->title = title ? title : "Commands";
}

void tk_palette_free(tk_palette *p) { free(p->view); p->view = NULL; p->vcap = p->nview = 0; }

void tk_palette_set(tk_palette *p, const tk_pal_item *items, int n) {
    p->items = items; p->n = n;
    p->query[0] = '\0'; p->sel = 0; p->top = 0;
}

/* Rebuild the filtered view (matches on label + desc), clamp selection. */
static void rebuild(tk_palette *p) {
    if (p->n > p->vcap) {
        int cap = p->vcap ? p->vcap * 2 : 32;
        while (cap < p->n) cap *= 2;
        p->view = (int *)realloc(p->view, (size_t)cap * sizeof(int));
        p->vcap = cap;
    }
    p->nview = 0;
    for (int i = 0; i < p->n; i++) {
        const tk_pal_item *it = &p->items[i];
        if (p->query[0]) {
            int m = fuzzy(it->label ? it->label : "", p->query)
                 || (it->desc && fuzzy(it->desc, p->query));
            if (!m) continue;
        }
        p->view[p->nview++] = i;
    }
    if (p->sel >= p->nview) p->sel = p->nview - 1;
    if (p->sel < 0) p->sel = 0;
}

int tk_palette_selected_id(tk_palette *p) {
    rebuild(p);
    if (p->nview == 0) return -1;
    return p->items[p->view[p->sel]].id;
}

tk_result tk_palette_handle(tk_palette *p, const struct tb_event *ev) {
    rebuild(p);
    if (ev->type != TB_EVENT_KEY) return TK_NONE;
    if (ev->key == TB_KEY_ESC)   return TK_CANCEL;
    if (ev->key == TB_KEY_ENTER) return p->nview ? TK_SELECT : TK_USED;
    if (ev->key == TB_KEY_ARROW_UP)   { if (p->sel > 0) p->sel--; return TK_CHANGED; }
    if (ev->key == TB_KEY_ARROW_DOWN) { if (p->sel + 1 < p->nview) p->sel++; return TK_CHANGED; }
    if (ev->key == TB_KEY_BACKSPACE || ev->key == TB_KEY_BACKSPACE2) {
        size_t n = strlen(p->query); if (n) p->query[n - 1] = '\0';
        p->sel = 0; return TK_CHANGED;
    }
    uint32_t cp = ev->ch; if (ev->key == TB_KEY_SPACE) cp = ' ';
    if (cp >= 0x20 && cp < 0x7f) {                 /* incremental filter */
        size_t n = strlen(p->query);
        if (n + 1 < sizeof p->query) { p->query[n] = (char)cp; p->query[n + 1] = '\0'; p->sel = 0; }
        return TK_CHANGED;
    }
    return TK_USED;   /* modal: swallow other keys */
}

void tk_palette_draw(tk_palette *p, int W, int H) {
    const tk_theme *th = tk_theme_active();
    rebuild(p);

    /* size: title + search + a blank + up to N rows (items + section headers). */
    int sections = 0; const char *last = NULL;
    for (int k = 0; k < p->nview; k++) {
        const char *s = p->items[p->view[k]].section;
        if (s && (!last || strcmp(s, last) != 0)) { sections++; last = s; }
        last = s;
    }
    int content = p->nview + sections;
    int bw = 60; if (bw > W - 4) bw = W - 4; if (bw < 30) bw = 30;
    int bh = content + 4; if (bh > H - 2) bh = H - 2; if (bh < 6) bh = 6;
    int x = (W - bw) / 2, y = (H - bh) / 2; if (x < 0) x = 0; if (y < 0) y = 0;
    int rightpad = 2;

    tk_box(x, y, bw, bh);
    char t[64]; snprintf(t, sizeof t, " %s ", p->title);
    tk_text(x + 2, y, x + bw - 1, t, th->accent | TB_BOLD, th->bg);
    tk_text_right(y, x + 2, x + bw - 2, "esc", th->faint, th->bg);

    /* search line */
    int sy = y + 1;
    tk_fill(sy, x + 1, x + bw - 1, th->bg);
    int cx = tk_text(x + 2, sy, x + bw - 2, p->query, th->fg, th->bg);
    if (!p->query[0]) tk_text(x + 2, sy, x + bw - 2, "Search", th->muted, th->bg);
    else if (cx < x + bw - 2) tb_set_cell(cx, sy, ' ', TB_DEFAULT, TB_REVERSE);

    /* rows: interleave section headers; keep selection visible via p->top */
    int avail = bh - 3;                 /* rows for the list (below title+search) */
    if (p->sel < p->top) p->top = p->sel;
    if (p->sel >= p->top + avail) p->top = p->sel - avail + 1;   /* rough (headers cost extra) */
    if (p->top < 0) p->top = 0;

    int row = y + 3;
    const char *prev = (void *)0;
    for (int k = p->top; k < p->nview && row < y + bh - 1; k++) {
        const tk_pal_item *it = &p->items[p->view[k]];
        if (it->section && (!prev || strcmp(it->section, prev) != 0)) {
            if (row < y + bh - 1) {
                char sh[48]; snprintf(sh, sizeof sh, "%s", it->section);
                tk_fill(row, x + 1, x + bw - 1, th->bg);
                tk_text(x + 2, row, x + bw - 2, sh, th->accent | TB_BOLD, th->bg);
                row++;
            }
        }
        prev = it->section;
        if (row >= y + bh - 1) break;
        int sel = (k == p->sel);
        uintattr_t bg = sel ? th->sel_bg : th->bg;
        uintattr_t fg = sel ? (th->sel_fg | TB_BOLD) : th->fg;
        tk_fill(row, x + 1, x + bw - 1, bg);
        int lx = tk_text(x + 3, row, x + bw - 2, it->label ? it->label : "", fg, bg);
        if (it->desc && it->desc[0])
            tk_text(lx + 2, row, x + bw - 2, it->desc, th->muted, bg);
        if (it->keyhint && it->keyhint[0])
            tk_text_right(row, x + 3, x + bw - 1 - rightpad, it->keyhint, th->faint, bg);
        row++;
    }
}
