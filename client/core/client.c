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

void oc_client_react(oc_client *c, uint64_t channel_id, uint64_t message_id,
                     const char *emoji, uint8_t op) {
    if (!c || !emoji || !emoji[0]) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_REACT);
    if (!cmd) return;
    cmd->channel_id = channel_id;
    cmd->message_id = message_id;
    cmd->op = op;
    cmd->body = strdup(emoji);
    oc_queue_push(&c->cmds, cmd);
}

void oc_client_edit(oc_client *c, uint64_t channel_id, uint64_t message_id, const char *body) {
    if (!c || !body || !body[0]) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_EDIT);
    if (!cmd) return;
    cmd->channel_id = channel_id;
    cmd->message_id = message_id;
    cmd->body = strdup(body);
    oc_queue_push(&c->cmds, cmd);
}

void oc_client_delete(oc_client *c, uint64_t channel_id, uint64_t message_id) {
    if (!c) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_DELETE);
    if (!cmd) return;
    cmd->channel_id = channel_id;
    cmd->message_id = message_id;
    oc_queue_push(&c->cmds, cmd);
}

void oc_client_typing(oc_client *c, uint64_t channel_id) {
    if (!c) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_TYPING);
    if (!cmd) return;
    cmd->channel_id = channel_id;
    oc_queue_push(&c->cmds, cmd);
}

void oc_client_open_thread(oc_client *c, uint64_t channel_id, uint64_t parent_id) {
    if (!c) return;
    oc_model_open_thread(&c->model, channel_id, parent_id);   /* frontend view state */
    oc_cmd *cmd = oc_cmd_new(OC_CMD_OPEN_THREAD);
    if (!cmd) return;
    cmd->channel_id = channel_id;
    cmd->message_id = parent_id;
    oc_queue_push(&c->cmds, cmd);
}

void oc_client_reply(oc_client *c, uint64_t channel_id, uint64_t parent_id, const char *body) {
    if (!c || !body || !body[0]) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_REPLY);
    if (!cmd) return;
    cmd->channel_id = channel_id;
    cmd->message_id = parent_id;
    cmd->body = strdup(body);
    oc_queue_push(&c->cmds, cmd);
}

void oc_client_close_thread(oc_client *c) {
    if (!c) return;
    oc_model_close_thread(&c->model);
}

void oc_client_search(oc_client *c, const char *query) {
    if (!c || !query || !query[0]) return;
    oc_model_search_begin(&c->model, query);      /* clears prior hits + records it */
    oc_cmd *cmd = oc_cmd_new(OC_CMD_SEARCH);
    if (!cmd) return;
    cmd->body = strdup(query);
    oc_queue_push(&c->cmds, cmd);
}

void oc_client_close_search(oc_client *c) {
    if (!c) return;
    oc_model_close_search(&c->model);
}

void oc_client_create_channel(oc_client *c, const char *name) {
    if (!c || !name || !name[0]) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_CREATE_CHANNEL);
    if (!cmd) return;
    cmd->body = strdup(name);
    oc_queue_push(&c->cmds, cmd);
}

static void channel_op(oc_client *c, int type, uint64_t channel_id) {
    if (!c || !channel_id) return;
    oc_cmd *cmd = oc_cmd_new(type);
    if (!cmd) return;
    cmd->channel_id = channel_id;
    oc_queue_push(&c->cmds, cmd);
}
void oc_client_join_channel(oc_client *c, uint64_t channel_id)  { channel_op(c, OC_CMD_JOIN_CHANNEL, channel_id); }
void oc_client_leave_channel(oc_client *c, uint64_t channel_id) { channel_op(c, OC_CMD_LEAVE_CHANNEL, channel_id); }

void oc_client_list_channels(oc_client *c) {
    if (!c) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_LIST_CHANNELS);
    if (cmd) oc_queue_push(&c->cmds, cmd);
}

void oc_client_list_users(oc_client *c) {
    if (!c) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_LIST_USERS);
    if (cmd) oc_queue_push(&c->cmds, cmd);
}

void oc_client_set_presence(oc_client *c, uint8_t status) {
    if (!c) return;
    oc_model_note_presence(&c->model, c->model.user_id, status);   /* reflect locally */
    oc_cmd *cmd = oc_cmd_new(OC_CMD_SET_PRESENCE);
    if (!cmd) return;
    cmd->op = status;
    oc_queue_push(&c->cmds, cmd);
}

void oc_client_toggle_roster(oc_client *c, int open) {
    if (!c) return;
    c->model.roster_open = open ? 1 : 0;
    if (open) oc_client_list_users(c);   /* refresh on open */
}

void oc_client_open_dm(oc_client *c, uint64_t user_id) {
    if (!c || !user_id) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_OPEN_DM);
    if (!cmd) return;
    cmd->channel_id = user_id;   /* reused as the target user id */
    oc_queue_push(&c->cmds, cmd);
}

void oc_client_list_reactions(oc_client *c, uint64_t channel_id, uint64_t message_id) {
    if (!c || !message_id) return;
    oc_model_reactlist_begin(&c->model, message_id);   /* clears prior + records it */
    oc_cmd *cmd = oc_cmd_new(OC_CMD_LIST_REACTIONS);
    if (!cmd) return;
    cmd->channel_id = channel_id;
    cmd->message_id = message_id;
    oc_queue_push(&c->cmds, cmd);
}

void oc_client_close_reactions(oc_client *c) {
    if (!c) return;
    oc_model_close_reactlist(&c->model);
}

void oc_client_set_notify_pref(oc_client *c, uint64_t channel_id, uint8_t level) {
    if (!c || !channel_id) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_SET_NOTIFY_PREF);
    if (!cmd) return;
    cmd->channel_id = channel_id;
    cmd->op = level;
    oc_queue_push(&c->cmds, cmd);
}

void oc_client_set_dnd(oc_client *c, uint8_t enabled, uint16_t start_min, uint16_t end_min) {
    if (!c) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_SET_DND);
    if (!cmd) return;
    cmd->op = enabled;
    cmd->channel_id = start_min;   /* reused: DND window start (minutes) */
    cmd->message_id = end_min;     /* reused: DND window end (minutes) */
    oc_queue_push(&c->cmds, cmd);
}

void oc_client_list_notify_prefs(oc_client *c) {
    if (!c) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_LIST_NOTIFY_PREFS);
    if (cmd) oc_queue_push(&c->cmds, cmd);
}

void oc_client_toggle_prefs(oc_client *c, int open) {
    if (!c) return;
    oc_model_set_prefs_open(&c->model, open);
    if (open) oc_client_list_notify_prefs(c);   /* refresh on open */
}

void oc_client_set_role(oc_client *c, uint64_t user_id, uint8_t role) {
    if (!c || !user_id) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_SET_ROLE);
    if (!cmd) return;
    cmd->channel_id = user_id;   /* reused as the target user id */
    cmd->op = role;
    oc_queue_push(&c->cmds, cmd);
}

void oc_client_invite_user(oc_client *c, uint8_t role) {
    if (!c) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_INVITE_USER);
    if (!cmd) return;
    cmd->op = role;
    oc_queue_push(&c->cmds, cmd);
}

void oc_client_remove_user(oc_client *c, uint64_t user_id) {
    if (!c || !user_id) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_REMOVE_USER);
    if (!cmd) return;
    cmd->channel_id = user_id;   /* reused as the target user id */
    oc_queue_push(&c->cmds, cmd);
}

void oc_client_webhooks(oc_client *c, uint64_t channel_id) {
    if (!c || !channel_id) return;
    oc_model_weblist_begin(&c->model, channel_id);   /* open overlay + clear prior */
    oc_cmd *cmd = oc_cmd_new(OC_CMD_LIST_WEBHOOKS);
    if (!cmd) return;
    cmd->channel_id = channel_id;
    oc_queue_push(&c->cmds, cmd);
}

void oc_client_close_webhooks(oc_client *c) {
    if (!c) return;
    oc_model_close_weblist(&c->model);
}

void oc_client_create_webhook(oc_client *c, uint64_t channel_id, const char *label) {
    if (!c || !channel_id || !label || !label[0]) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_CREATE_WEBHOOK);
    if (!cmd) return;
    cmd->channel_id = channel_id;
    cmd->body = strdup(label);
    oc_queue_push(&c->cmds, cmd);
}

void oc_client_delete_webhook(oc_client *c, uint64_t webhook_id) {
    if (!c || !webhook_id) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_DELETE_WEBHOOK);
    if (!cmd) return;
    cmd->message_id = webhook_id;   /* reused as the webhook id */
    oc_queue_push(&c->cmds, cmd);
}

void oc_client_upload(oc_client *c, uint64_t channel_id, const char *path) {
    if (!c || !channel_id || !path || !path[0]) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_UPLOAD);
    if (!cmd) return;
    cmd->channel_id = channel_id;
    cmd->body = strdup(path);
    oc_queue_push(&c->cmds, cmd);
}

void oc_client_download(oc_client *c, uint64_t attachment_id, const char *dest_path) {
    if (!c || !attachment_id || !dest_path || !dest_path[0]) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_DOWNLOAD);
    if (!cmd) return;
    cmd->message_id = attachment_id;   /* reused as the attachment id */
    cmd->body = strdup(dest_path);
    oc_queue_push(&c->cmds, cmd);
}

void oc_client_logout(oc_client *c, uint8_t scope) {
    if (!c) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_LOGOUT);
    if (!cmd) return;
    cmd->op = scope;
    oc_queue_push(&c->cmds, cmd);
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
