/*
 * tuikit — tk_list: a generic, filterable, selectable list. It owns no items;
 * the app supplies a live item count and a text delegate, so the same widget
 * renders a channel sidebar, a member roster, an action menu, or a fuzzy palette.
 * tuikit never sees the app's data types.
 */

#ifndef TK_LIST_H
#define TK_LIST_H

#include <stddef.h>
#include "tk_term.h"
#include "tk_common.h"

typedef struct {
    void *ud;                                                   /* passed to callbacks */
    int  (*count)(void *ud);                                    /* live item count */
    /* Format item i's display text into buf; optionally set *fg (else TB_DEFAULT). */
    void (*item_text)(int i, void *ud, char *buf, size_t cap, uintattr_t *fg);
    /* Optional: text used for fuzzy filtering (defaults to item_text if NULL). */
    void (*item_filter)(int i, void *ud, char *buf, size_t cap);
    int  filterable;                                            /* 1 = '/' begins filtering */
} tk_list_opts;

typedef struct {
    tk_list_opts opts;
    int   sel;         /* selection, index into the filtered view */
    int   top;         /* first visible view row (scroll) */
    char  filter[64];
    int   filtering;   /* editing the filter query */
    int  *view;        /* view[k] = original item index; rebuilt on demand */
    int   nview, vcap;
} tk_list;

void        tk_list_init(tk_list *l, const tk_list_opts *o);
void        tk_list_free(tk_list *l);
tk_result   tk_list_handle(tk_list *l, const struct tb_event *ev);
void        tk_list_draw(tk_list *l, tk_rect r);
/* Original item index of the current selection, or -1 if empty. */
int         tk_list_selected(tk_list *l);
/* Clear any active filter. */
void        tk_list_reset_filter(tk_list *l);

#endif /* TK_LIST_H */
