/*
 * tuikit — tk_style. See tk_style.h.
 */

#include "tk_style.h"
#include "tk_theme.h"
#include "tk_draw.h"

#include <stdio.h>

void tk_panel(int x, int y, int w, int h, const char *title, int active) {
    if (w < 2 || h < 2) return;
    const tk_theme *th = tk_theme_active();
    uintattr_t bc = active ? th->border_active : th->border;
    for (int i = 1; i < w - 1; i++) {
        tb_set_cell(x + i, y, 0x2500, bc, TB_DEFAULT);
        tb_set_cell(x + i, y + h - 1, 0x2500, bc, TB_DEFAULT);
    }
    for (int j = 1; j < h - 1; j++) {
        tb_set_cell(x, y + j, 0x2502, bc, TB_DEFAULT);
        tb_set_cell(x + w - 1, y + j, 0x2502, bc, TB_DEFAULT);
    }
    tb_set_cell(x, y, 0x250c, bc, TB_DEFAULT);
    tb_set_cell(x + w - 1, y, 0x2510, bc, TB_DEFAULT);
    tb_set_cell(x, y + h - 1, 0x2514, bc, TB_DEFAULT);
    tb_set_cell(x + w - 1, y + h - 1, 0x2518, bc, TB_DEFAULT);
    if (title && title[0]) {
        char t[96]; snprintf(t, sizeof t, " %s ", title);
        tk_text(x + 2, y, x + w - 1, t, active ? (th->accent | TB_BOLD) : (th->fg | TB_BOLD), th->bg);
    }
}

void tk_box(int x, int y, int w, int h) {
    const tk_theme *th = tk_theme_active();
    uintattr_t bc = th->border_active;   /* modals use the accent border, uniformly */
    for (int j = 1; j < h - 1; j++)
        for (int i = 1; i < w - 1; i++) tb_set_cell(x + i, y + j, ' ', TB_DEFAULT, th->bg);
    for (int i = 1; i < w - 1; i++) {
        tb_set_cell(x + i, y,         0x2500, bc, th->bg);
        tb_set_cell(x + i, y + h - 1, 0x2500, bc, th->bg);
    }
    for (int j = 1; j < h - 1; j++) {
        tb_set_cell(x,         y + j, 0x2502, bc, th->bg);
        tb_set_cell(x + w - 1, y + j, 0x2502, bc, th->bg);
    }
    tb_set_cell(x,         y,         0x250c, bc, th->bg);
    tb_set_cell(x + w - 1, y,         0x2510, bc, th->bg);
    tb_set_cell(x,         y + h - 1, 0x2514, bc, th->bg);
    tb_set_cell(x + w - 1, y + h - 1, 0x2518, bc, th->bg);
}
