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
    free(e);
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
    free(c);
}
