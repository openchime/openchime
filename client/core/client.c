/*
 * OpenChime client — the app-core facade (ARCH-74). See client.h.
 */

#include "client.h"

#include "event.h"
#include "net.h"
#include "queue.h"

#include <stdlib.h>
#include <string.h>

struct oc_client {
    oc_net  *net;
    oc_queue events;   /* net -> UI (oc_ev) */
    oc_queue cmds;     /* UI -> net (oc_cmd) */
    oc_model model;
};

oc_client *oc_client_start(const char *host, int port, const char *cred) {
    oc_client *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    oc_queue_init(&c->events);
    oc_queue_init(&c->cmds);
    oc_model_init(&c->model);
    c->net = oc_net_start(host, port, cred, &c->events, &c->cmds);
    if (!c->net) {
        oc_queue_destroy(&c->events);
        oc_queue_destroy(&c->cmds);
        oc_model_free(&c->model);
        free(c);
        return NULL;
    }
    return c;
}

void oc_client_tick(oc_client *c) {
    if (!c) return;
    oc_ev *e;
    while ((e = oc_queue_try_pop(&c->events)) != NULL) {
        oc_model_apply(&c->model, e);
        oc_ev_free(e);
    }
}

const oc_model *oc_client_model(oc_client *c) {
    return c ? &c->model : NULL;
}

void oc_client_send(oc_client *c, uint64_t channel_id, const char *body) {
    if (!c || !body) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_SEND);
    if (!cmd) return;
    cmd->channel_id = channel_id;
    cmd->body = strdup(body);
    oc_queue_push(&c->cmds, cmd);
}

void oc_client_backfill(oc_client *c, uint64_t channel_id) {
    if (!c) return;
    /* Ask at most once per channel; a known channel gets its flag set, an
     * unknown one is requested anyway (the reducer will create it on reply). */
    oc_channel *ch = oc_model_channel(&c->model, channel_id);
    if (ch) {
        if (ch->history_requested) return;
        ch->history_requested = 1;
    }
    oc_cmd *cmd = oc_cmd_new(OC_CMD_BACKFILL);
    if (!cmd) return;
    cmd->channel_id = channel_id;
    oc_queue_push(&c->cmds, cmd);
}

void oc_client_mark_read(oc_client *c, uint64_t channel_id) {
    if (!c) return;
    oc_model_mark_read(&c->model, channel_id);
}

void oc_client_stop(oc_client *c) {
    if (!c) return;
    oc_net_stop(c->net);   /* signals QUIT, joins the thread */

    /* Drain and free any events the thread left in the queue. */
    oc_ev *e;
    while ((e = oc_queue_try_pop(&c->events)) != NULL) oc_ev_free(e);
    /* Free any unsent commands still queued. */
    oc_cmd *cmd;
    while ((cmd = oc_queue_try_pop(&c->cmds)) != NULL) oc_cmd_free(cmd);

    oc_queue_destroy(&c->events);
    oc_queue_destroy(&c->cmds);
    oc_model_free(&c->model);
    free(c);
}
