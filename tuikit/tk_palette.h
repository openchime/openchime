/*
 * tuikit — tk_palette: an opencode-style command palette. Always-on incremental
 * search (type to filter, no mode), arrow-key selection, section headers,
 * right-aligned key hints, and gray descriptions. Self-centers as a modal.
 *
 * The app supplies a borrowed array of items (label + optional section / desc /
 * keyhint + an id). Enter reports the selected id; Esc cancels.
 */

#ifndef TK_PALETTE_H
#define TK_PALETTE_H

#include "tk_term.h"
#include "tk_common.h"

typedef struct {
    const char *section;   /* group label; consecutive items sharing it are grouped */
    const char *label;
    const char *desc;      /* optional gray secondary text */
    const char *keyhint;   /* optional right-aligned accelerator, e.g. "Ctrl+F" */
    int         id;        /* returned on select */
} tk_pal_item;

typedef struct {
    const tk_pal_item *items;   /* borrowed */
    int    n;
    char   query[64];
    int    sel;                 /* index into the filtered view */
    int    top;                 /* first visible view row (item scroll) */
    int   *view;                /* filtered original item indices */
    int    nview, vcap;
    const char *title;
} tk_palette;

void      tk_palette_init(tk_palette *p, const char *title);
void      tk_palette_free(tk_palette *p);
/* Set the item list (borrowed) and reset the query/selection. */
void      tk_palette_set(tk_palette *p, const tk_pal_item *items, int n);
/* Type filters live; ↑/↓ move; Enter → TK_SELECT; Esc → TK_CANCEL. */
tk_result tk_palette_handle(tk_palette *p, const struct tb_event *ev);
/* The selected item's id, or -1 if the filtered list is empty. */
int       tk_palette_selected_id(tk_palette *p);
/* Draw the palette centered over a W×H screen. */
void      tk_palette_draw(tk_palette *p, int W, int H);

#endif /* TK_PALETTE_H */
