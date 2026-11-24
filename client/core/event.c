#include "event.h"

#include <stdlib.h>

oc_ev *oc_ev_new(int type) {
    oc_ev *e = calloc(1, sizeof *e);
    if (e) e->type = type;
    return e;
}
void oc_ev_free(oc_ev *e) {
    if (!e) return;
    free(e->body);
    free(e->topic);
    free(e->preview);
    free(e->call);
    free(e->call_hist);
    oc_call_page_view_free(e->call_page);
    free(e->group);
    free(e->poll);
    oc_todo_view_free(e->todo);
    free(e->ids);
    free(e->der);
    free(e->srvalert);
    free(e->usage);
    free(e->pfs);
    free(e);
}

void oc_call_page_view_free(oc_call_page_view *p) {
    if (!p) return;
    for (uint32_t i = 0; p->lines && i < p->n_lines; i++) free(p->lines[i].text);
    free(p->lines);
    free(p);
}

void oc_todo_view_free(oc_todo_view *t) {
    if (!t) return;
    for (uint16_t i = 0; i < t->cap && t->tasks; i++) { free(t->tasks[i].name); free(t->tasks[i].desc); }
    free(t->tasks);
    free(t->title);
    free(t);
}

oc_cmd *oc_cmd_new(int type) {
    oc_cmd *c = calloc(1, sizeof *c);
    if (c) c->type = type;
    return c;
}
void oc_cmd_free(oc_cmd *c) {
    if (!c) return;
    free(c->body);
    free(c->body2);
    free(c->body3);
    free(c->blob);
    free(c->blob2);
    for (int i = 0; i < c->n_paths; i++) { free(c->paths[i]); free(c->mem[i]); }
    free(c);
}
