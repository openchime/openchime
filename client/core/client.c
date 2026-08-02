/*
 * OpenChime client — the app-core facade (ARCH-74). See client.h.
 */

#include "client.h"

#include "event.h"
#include "net.h"
#include "queue.h"
#include "protocol.h"   /* the OC_NOTIFY_* levels are wire constants */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

struct oc_client {
    oc_net  *net;
    oc_queue events;   /* net -> UI (oc_ev) */
    oc_queue cmds;     /* UI -> net (oc_cmd) */
    oc_model model;
};

oc_client *oc_client_start(const char *host, int port, const char *cred) {
    return oc_client_start_secure(host, port, cred, NULL, NULL);
}

oc_client *oc_client_start_stored(const char *host, int port, const char *cred,
                                  const char *store_path) {
    return oc_client_start_secure(host, port, cred, store_path, NULL);
}

oc_client *oc_client_start_secure(const char *host, int port, const char *cred,
                                  const char *store_path, oc_secret *secret) {
    oc_client *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    oc_queue_init(&c->events);
    oc_queue_init(&c->cmds);
    oc_model_init(&c->model);
    c->net = oc_net_start(host, port, cred, store_path, secret, &c->events, &c->cmds);
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
    oc_channel *ch = oc_model_channel(&c->model, channel_id);
    uint64_t before = ch ? ch->read_marker : 0;
    oc_model_mark_read(&c->model, channel_id);
    ch = oc_model_channel(&c->model, channel_id);
    if (ch && ch->read_marker > before) {   /* advanced: ack the server (drives seen-by, REQ-090) */
        oc_cmd *cmd = oc_cmd_new(OC_CMD_MARK_READ);
        if (cmd) { cmd->channel_id = channel_id; cmd->message_id = ch->read_marker; oc_queue_push(&c->cmds, cmd); }
    }
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

/* Open the search view with no query. The overlay owns the input box (WIN-4),
 * so it needs the pane on screen before there is anything to search for. */
void oc_client_open_search(oc_client *c) {
    if (!c) return;
    oc_model_search_begin(&c->model, "");
}

void oc_client_search(oc_client *c, const char *query) {
    if (!c || !query || !query[0]) return;
    oc_model_search_begin(&c->model, query);      /* clears prior hits + records it */
    oc_cmd *cmd = oc_cmd_new(OC_CMD_SEARCH);
    if (!cmd) return;
    cmd->body = strdup(query);
    oc_queue_push(&c->cmds, cmd);
}

/* WIN-38: the next page of the CURRENT search. `before_id` is the oldest id already
 * shown — a keyset cursor, so a message posted while you page cannot make a row
 * repeat or vanish the way an OFFSET would. The query is re-sent verbatim because the
 * server holds no search state; that is deliberate (a stateless server cannot leak a
 * stale cursor).
 *
 * Appends rather than replacing: oc_model_search_begin is NOT called here. */
void oc_client_search_more(oc_client *c, uint64_t before_id) {
    if (!c || !before_id || !c->model.search_query[0]) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_SEARCH);
    if (!cmd) return;
    cmd->body = strdup(c->model.search_query);
    cmd->message_id = before_id;
    oc_queue_push(&c->cmds, cmd);
}

void oc_client_close_search(oc_client *c) {
    if (!c) return;
    oc_model_close_search(&c->model);
}

void oc_client_create_channel_ex(oc_client *c, const char *name, int is_public) {
    if (!c || !name || !name[0]) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_CREATE_CHANNEL);
    if (!cmd) return;
    cmd->body = strdup(name);
    cmd->op = (uint8_t)(is_public ? 1 : 0);
    oc_queue_push(&c->cmds, cmd);
}

/* CREATE_CHANNEL has always carried is_public on the wire; this facade dropped
 * it and hardcoded public (WIN-30). Kept as the public-channel shorthand. */
void oc_client_create_channel(oc_client *c, const char *name) {
    oc_client_create_channel_ex(c, name, 1);
}

void oc_client_history(oc_client *c, uint64_t channel_id, uint64_t before_message_id) {
    if (!c || !channel_id) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_HISTORY);
    if (!cmd) return;
    cmd->channel_id = channel_id;
    cmd->message_id = before_message_id;
    oc_queue_push(&c->cmds, cmd);
}

static void chan_member_op(oc_client *c, int type, uint64_t channel_id, uint64_t user_id) {
    if (!c || !channel_id || !user_id) return;
    oc_cmd *cmd = oc_cmd_new(type);
    if (!cmd) return;
    cmd->channel_id = channel_id;
    cmd->message_id = user_id;
    oc_queue_push(&c->cmds, cmd);
}

void oc_client_channel_invite(oc_client *c, uint64_t channel_id, uint64_t user_id) {
    chan_member_op(c, OC_CMD_CHANNEL_INVITE, channel_id, user_id);
}

void oc_client_channel_kick(oc_client *c, uint64_t channel_id, uint64_t user_id) {
    chan_member_op(c, OC_CMD_CHANNEL_KICK, channel_id, user_id);
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

void oc_client_pin(oc_client *c, uint64_t channel_id, uint64_t message_id, uint8_t op) {
    if (!c || !message_id) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_PIN);
    if (!cmd) return;
    cmd->channel_id = channel_id;
    cmd->message_id = message_id;
    cmd->op = op;
    oc_queue_push(&c->cmds, cmd);
}

void oc_client_list_pins(oc_client *c, uint64_t channel_id) {
    if (!c || !channel_id) return;
    oc_model_pinlist_begin(&c->model, channel_id);   /* clears prior + marks loading */
    oc_cmd *cmd = oc_cmd_new(OC_CMD_LIST_PINS);
    if (!cmd) return;
    cmd->channel_id = channel_id;
    oc_queue_push(&c->cmds, cmd);
}

void oc_client_update_channel(oc_client *c, uint64_t channel_id, uint8_t op, const char *value) {
    if (!c || !channel_id) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_UPDATE_CHANNEL);
    if (!cmd) return;
    cmd->channel_id = channel_id;
    cmd->op = op;
    cmd->body = strdup(value ? value : "");
    oc_queue_push(&c->cmds, cmd);
}

void oc_client_history_around(oc_client *c, uint64_t channel_id, uint64_t message_id, uint16_t limit) {
    if (!c || !channel_id || !message_id) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_HISTORY_AROUND);
    if (!cmd) return;
    cmd->channel_id = channel_id;
    cmd->message_id = message_id;
    cmd->op = (uint8_t)(limit > 255 ? 255 : limit);
    oc_queue_push(&c->cmds, cmd);
}

void oc_client_save_item(oc_client *c, uint64_t message_id, uint8_t op) {
    if (!c || !message_id) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_SAVE_ITEM);
    if (!cmd) return;
    cmd->message_id = message_id; cmd->op = op;
    oc_queue_push(&c->cmds, cmd);
}
void oc_client_list_saved(oc_client *c) {
    if (!c) return;
    oc_model_saved_begin(&c->model);
    oc_cmd *cmd = oc_cmd_new(OC_CMD_LIST_SAVED);
    if (cmd) oc_queue_push(&c->cmds, cmd);
}
void oc_client_close_saved(oc_client *c) { if (c) oc_model_close_saved(&c->model); }
void oc_client_list_activity(oc_client *c, uint8_t filter) {
    if (!c) return;
    /* Open the list BEFORE asking, as every other list command does: the fold
     * drops an entry that arrives while the list is closed, so without this the
     * feed was silently empty however many rows the server sent. */
    oc_model_activity_begin(&c->model);
    oc_cmd *cmd = oc_cmd_new(OC_CMD_LIST_ACTIVITY);
    if (!cmd) return;
    cmd->op = filter;
    oc_queue_push(&c->cmds, cmd);
}
void oc_client_close_activity(oc_client *c) { if (c) oc_model_close_activity(&c->model); }

void oc_client_list_members(oc_client *c, uint64_t channel_id) {
    if (!c || !channel_id) return;
    oc_model_chanmem_begin(&c->model, channel_id);
    oc_cmd *cmd = oc_cmd_new(OC_CMD_LIST_MEMBERS);
    if (!cmd) return;
    cmd->channel_id = channel_id;
    oc_queue_push(&c->cmds, cmd);
}

void oc_client_list_files(oc_client *c, uint64_t channel_id) {
    if (!c) return;
    oc_model_filelist_begin(&c->model, channel_id);
    oc_cmd *cmd = oc_cmd_new(OC_CMD_LIST_FILES);
    if (!cmd) return;
    cmd->channel_id = channel_id;      /* 0 = every channel I can read */
    oc_queue_push(&c->cmds, cmd);
}

void oc_client_close_files(oc_client *c) {
    if (c) oc_model_close_filelist(&c->model);
}

void oc_client_close_pins(oc_client *c) {
    if (c) oc_model_close_pinlist(&c->model);
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

/* The recurring schedule (REQ-136). `start_min`/`end_min` are the hours
 * notifications are ALLOWED; `days` applies only to OC_DND_CUSTOM. The timezone
 * offset rides along because only this side knows it — the daemon would have to
 * carry tzdata into the thread that answers every push (ARCH-103). */
void oc_client_set_schedule(oc_client *c, uint8_t mode, int16_t tz_offset_min,
                            uint16_t start_min, uint16_t end_min,
                            const oc_schedule_day *days, uint8_t n_days) {
    if (!c) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_SET_SCHEDULE);
    if (!cmd) return;
    cmd->sched_mode = mode;
    cmd->tz_offset_min = tz_offset_min;
    cmd->sched_start_min = start_min;
    cmd->sched_end_min = end_min;
    if (n_days > OC_SCHEDULE_DAYS) n_days = OC_SCHEDULE_DAYS;
    for (uint8_t i = 0; i < n_days && days; i++) cmd->sched_days[i] = days[i];
    cmd->n_sched_days = days ? n_days : 0;
    oc_queue_push(&c->cmds, cmd);
}

/* Both lists are REPLACEMENTS (REQ-135): short, edited whole, and their terms
 * are their own identity, so there is nothing an add/remove pair could name. */
void oc_client_set_keywords(oc_client *c, const char terms[][OC_KEYWORD_MAX], uint8_t n) {
    if (!c) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_SET_KEYWORDS);
    if (!cmd) return;
    if (n > OC_MAX_KEYWORDS) n = OC_MAX_KEYWORDS;
    for (uint8_t i = 0; i < n && terms; i++)
        snprintf(cmd->kw_terms[i], OC_KEYWORD_MAX, "%s", terms[i]);
    cmd->n_kw_terms = terms ? n : 0;
    oc_queue_push(&c->cmds, cmd);
}

void oc_client_set_priority(oc_client *c, const uint64_t *people, uint8_t n) {
    if (!c) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_SET_PRIORITY);
    if (!cmd) return;
    if (n > OC_MAX_PRIORITY) n = OC_MAX_PRIORITY;
    for (uint8_t i = 0; i < n && people; i++) cmd->pri_people[i] = people[i];
    cmd->n_pri_people = people ? n : 0;
    oc_queue_push(&c->cmds, cmd);
}

/* Pause notifications for `minutes` from now; 0 ends a pause early (REQ-278).
 * Minutes rather than an instant because that is what the presets ARE, and the
 * daemon resolving them once is what keeps a pause off everybody's clock. */
void oc_client_set_snooze(oc_client *c, uint32_t minutes) {
    if (!c) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_SET_SNOOZE);
    if (!cmd) return;
    cmd->message_id = minutes;
    oc_queue_push(&c->cmds, cmd);
}

/* Threads I am in, across channels (REQ-062). `filter` is OC_THREADF_*. Opens
 * the list first, as every other list command does — the fold drops entries that
 * arrive while it is closed, which is the bug that made the activity feed look
 * permanently empty. */
void oc_client_list_threads(oc_client *c, uint8_t filter) {
    if (!c) return;
    oc_model_threads_begin(&c->model);
    oc_cmd *cmd = oc_cmd_new(OC_CMD_LIST_THREADS);
    if (!cmd) return;
    cmd->op = filter;
    oc_queue_push(&c->cmds, cmd);
}

void oc_client_thread_follow(oc_client *c, uint64_t channel_id, uint64_t root_id, uint8_t on) {
    if (!c || !root_id) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_THREAD_FOLLOW);
    if (!cmd) return;
    cmd->channel_id = channel_id; cmd->message_id = root_id; cmd->op = on;
    oc_queue_push(&c->cmds, cmd);
}

/* `up_to` 0 means every reply in it, which is what opening one means. */
void oc_client_mark_thread_read(oc_client *c, uint64_t root_id, uint64_t up_to) {
    if (!c || !root_id) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_MARK_THREAD_READ);
    if (!cmd) return;
    cmd->message_id = root_id; cmd->server_time = up_to;
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

void oc_client_redeem_invite(oc_client *c, const char *invite_token) {
    if (c) oc_net_set_invite(c->net, invite_token);
}

void oc_client_set_client_type(oc_client *c, const char *client_type) {
    if (c) oc_net_set_client_type(c->net, client_type);
}

void oc_client_set_setting(oc_client *c, const char *key, const char *value) {
    if (!c || !key || !key[0]) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_SET_SETTING);
    if (!cmd) return;
    cmd->body  = strdup(key);
    cmd->body2 = strdup(value ? value : "");   /* empty value deletes the key */
    oc_queue_push(&c->cmds, cmd);
}

void oc_client_set_draft(oc_client *c, uint64_t channel_id, uint64_t thread_root,
                         const char *body) {
    if (!c || !channel_id) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_SET_DRAFT);
    if (!cmd) return;
    cmd->channel_id = channel_id;
    cmd->message_id = thread_root;
    cmd->body = strdup(body ? body : "");   /* empty deletes */
    oc_queue_push(&c->cmds, cmd);
}

void oc_client_schedule(oc_client *c, uint64_t channel_id, uint64_t thread_root,
                        uint64_t send_at_ms, const char *body) {
    if (!c || !channel_id || !body || !body[0]) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_SCHEDULE);
    if (!cmd) return;
    cmd->channel_id = channel_id;
    cmd->message_id = thread_root;
    cmd->server_time = send_at_ms;
    cmd->body = strdup(body);
    oc_queue_push(&c->cmds, cmd);
}

void oc_client_list_scheduled(oc_client *c) {
    if (!c) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_LIST_SCHEDULED);
    if (cmd) oc_queue_push(&c->cmds, cmd);
}

void oc_client_cancel_scheduled(oc_client *c, uint64_t id) {
    if (!c || !id) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_CANCEL_SCHEDULED);
    if (!cmd) return;
    cmd->message_id = id;
    oc_queue_push(&c->cmds, cmd);
}

void oc_client_update_scheduled(oc_client *c, uint64_t id, uint64_t send_at_ms,
                                const char *body) {
    if (!c || !id) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_UPDATE_SCHEDULED);
    if (!cmd) return;
    cmd->message_id = id;
    cmd->server_time = send_at_ms;
    cmd->body = strdup(body ? body : "");
    oc_queue_push(&c->cmds, cmd);
}

void oc_client_set_draft_to(oc_client *c, const char *recipients, const char *body) {
    if (!c) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_SET_DRAFT);
    if (!cmd) return;
    cmd->channel_id = 0;                  /* not addressed yet */
    cmd->message_id = 0;
    cmd->body  = strdup(body ? body : "");
    cmd->body2 = strdup(recipients ? recipients : "");
    oc_queue_push(&c->cmds, cmd);
}

void oc_client_list_drafts(oc_client *c) {
    if (!c) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_LIST_DRAFTS);
    if (cmd) oc_queue_push(&c->cmds, cmd);
}

void oc_client_list_settings(oc_client *c) {
    if (!c) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_LIST_SETTINGS);
    if (cmd) oc_queue_push(&c->cmds, cmd);
}

void oc_client_storage_status(oc_client *c) {
    if (!c) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_STORAGE_STATUS);
    if (cmd) oc_queue_push(&c->cmds, cmd);
}

void oc_client_toggle_storage(oc_client *c, int open) {
    if (c) c->model.storage_open = open ? 1 : 0;
}

void oc_client_audit_query(oc_client *c, uint64_t before_ms) {
    if (!c) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_AUDIT_QUERY);
    if (cmd) { cmd->message_id = before_ms; oc_queue_push(&c->cmds, cmd); }
}

void oc_client_toggle_audit(oc_client *c, int open) {
    if (c) c->model.audit_open = open ? 1 : 0;
}

void oc_client_set_display_name(oc_client *c, const char *name) {
    if (!c || !name || !name[0]) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_SET_DISPLAY_NAME);
    if (!cmd) return;
    cmd->body = strdup(name);
    oc_queue_push(&c->cmds, cmd);
}

void oc_client_change_password(oc_client *c, const char *old_pw, const char *new_pw) {
    if (!c || !new_pw || !new_pw[0]) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_CHANGE_PASSWORD);
    if (!cmd) return;
    cmd->body  = strdup(old_pw ? old_pw : "");
    cmd->body2 = strdup(new_pw);
    oc_queue_push(&c->cmds, cmd);
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

/* WIN-46: outstanding invites. No arguments — the server decides what "outstanding"
 * means (not consumed, not expired) so two clients cannot disagree about it. */
void oc_client_list_invites(oc_client *c) {
    if (!c) return;
    oc_model_invites_begin(&c->model);   /* clears any prior list, marks it loading */
    oc_cmd *cmd = oc_cmd_new(OC_CMD_LIST_INVITES);
    if (cmd) oc_queue_push(&c->cmds, cmd);
}

void oc_client_revoke_invite(oc_client *c, uint64_t invite_id) {
    if (!c || !invite_id) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_REVOKE_INVITE);
    if (!cmd) return;
    cmd->message_id = invite_id;
    oc_queue_push(&c->cmds, cmd);
}

/* WIN-48. Disable is reversible and rotate is not — the old token dies the moment
 * the new one is minted, which is the point of rotating a leaked one. */
void oc_client_set_webhook_state(oc_client *c, uint64_t webhook_id, int disabled) {
    if (!c || !webhook_id) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_SET_WEBHOOK_STATE);
    if (!cmd) return;
    cmd->message_id = webhook_id;
    cmd->op = disabled ? 1 : 0;
    oc_queue_push(&c->cmds, cmd);
}

void oc_client_rotate_webhook(oc_client *c, uint64_t webhook_id) {
    if (!c || !webhook_id) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_ROTATE_WEBHOOK);
    if (!cmd) return;
    cmd->message_id = webhook_id;
    oc_queue_push(&c->cmds, cmd);
}

/* Mute (REQ-137, WIN-40). Independent of the notification level: the daemon keeps
 * them in separate columns for the same reason. */
void oc_client_set_mute(oc_client *c, uint64_t channel_id, int muted) {
    if (!c || !channel_id) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_SET_MUTE);
    if (!cmd) return;
    cmd->channel_id = channel_id;
    cmd->op = muted ? 1 : 0;
    oc_queue_push(&c->cmds, cmd);
}

/* The global notification default (REQ-134). */
void oc_client_set_notify_default(oc_client *c, uint8_t level) {
    if (!c || level > OC_NOTIFY_NONE) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_SET_NOTIFY_DEFAULT);
    if (!cmd) return;
    cmd->op = level;
    oc_queue_push(&c->cmds, cmd);
}

/* Upload an image and make it my avatar (WIN-47). Same transfer machinery as any
 * upload — chunking, window, size cap — but the finished attachment is claimed with
 * SET_AVATAR rather than posted. `channel_id` is where the bytes are uploaded (the
 * wire requires one); nothing is posted there. */
void oc_client_upload_avatar(oc_client *c, uint64_t channel_id, const char *path) {
    if (!c || !path || !path[0]) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_UPLOAD);
    if (!cmd) return;
    cmd->channel_id = channel_id;
    cmd->op = 1;                       /* purpose: avatar */
    cmd->body = strdup(path);
    oc_queue_push(&c->cmds, cmd);
}

/* Custom emoji (REQ-072). */
void oc_client_list_emoji(oc_client *c) {
    if (!c) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_LIST_EMOJI);
    if (cmd) oc_queue_push(&c->cmds, cmd);
}

void oc_client_add_emoji(oc_client *c, const char *name, uint64_t attachment_id) {
    if (!c || !name || !name[0] || !attachment_id) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_ADD_EMOJI);
    if (!cmd) return;
    cmd->body = strdup(name);
    cmd->message_id = attachment_id;
    oc_queue_push(&c->cmds, cmd);
}

void oc_client_delete_emoji(oc_client *c, const char *name) {
    if (!c || !name || !name[0]) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_DELETE_EMOJI);
    if (!cmd) return;
    cmd->body = strdup(name);
    oc_queue_push(&c->cmds, cmd);
}

void oc_client_upload_emoji(oc_client *c, uint64_t channel_id, const char *name, const char *path) {
    if (!c || !name || !name[0] || !path || !path[0]) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_UPLOAD);
    if (!cmd) return;
    cmd->channel_id = channel_id;
    cmd->op = 2;                       /* purpose: custom emoji */
    cmd->body = strdup(path);
    cmd->body2 = strdup(name);         /* claimed with this name when it lands */
    oc_queue_push(&c->cmds, cmd);
}

/* A group DM (REQ-056). */
void oc_client_open_group_dm(oc_client *c, const uint64_t *user_ids, int n) {
    if (!c || !user_ids || n < 2) return;
    if (n > 8) n = 8;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_OPEN_GROUP_DM);
    if (!cmd) return;
    for (int i = 0; i < n; i++) cmd->gids[i] = user_ids[i];
    cmd->n_gids = n;
    oc_queue_push(&c->cmds, cmd);
}

/* The avatar (WIN-47). */
void oc_client_set_avatar(oc_client *c, uint64_t attachment_id) {
    if (!c) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_SET_AVATAR);
    if (!cmd) return;
    cmd->message_id = attachment_id;
    oc_queue_push(&c->cmds, cmd);
}

/* Mark unread (REQ-235, WIN-52). `message_id` is where reading resumes; 0 marks the
 * whole conversation unread. Deliberately not oc_client_mark_read's path, which may
 * only ever advance the cursor. */
void oc_client_set_read_cursor(oc_client *c, uint64_t channel_id, uint64_t message_id) {
    if (!c || !channel_id) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_SET_READ_CURSOR);
    if (!cmd) return;
    cmd->channel_id = channel_id;
    cmd->message_id = message_id;
    oc_queue_push(&c->cmds, cmd);
}

/* Custom status (REQ-241/122, WIN-53). Empty text clears it; `expires_at` 0 means
 * "until I change it". The DAEMON enforces expiry — a client that is not running
 * cannot clear its own status, so it must not be the thing that decides. */
/* REQ-182: my own live sessions. */
void oc_client_list_sessions(oc_client *c) {
    if (!c) return;
    oc_model_sessions_begin(&c->model);
    oc_cmd *cmd = oc_cmd_new(OC_CMD_LIST_SESSIONS);
    if (cmd) oc_queue_push(&c->cmds, cmd);
}

/* WIN-82: the exact channel census for the Files view. */
void oc_client_list_file_channels(oc_client *c) {
    if (!c) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_LIST_FILE_CHANNELS);
    if (cmd) oc_queue_push(&c->cmds, cmd);
}

void oc_client_set_status(oc_client *c, const char *emoji, const char *text,
                          uint64_t expires_at) {
    if (!c) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_SET_STATUS);
    if (!cmd) return;
    cmd->body  = strdup(emoji ? emoji : "");
    cmd->body2 = strdup(text ? text : "");
    cmd->message_id = expires_at;
    oc_queue_push(&c->cmds, cmd);
}

/* Profile fields (REQ-240, WIN-47). */
void oc_client_set_profile(oc_client *c, const char *title, const char *timezone) {
    if (!c) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_SET_PROFILE);
    if (!cmd) return;
    cmd->body  = strdup(title ? title : "");
    cmd->body2 = strdup(timezone ? timezone : "");
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

void oc_client_fetch_attachment(oc_client *c, uint64_t attachment_id) {
    if (!c || !attachment_id) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_FETCH);
    if (!cmd) return;
    cmd->message_id = attachment_id;
    oc_queue_push(&c->cmds, cmd);
}

void oc_client_logout(oc_client *c, uint8_t scope) {
    if (!c) return;
    oc_cmd *cmd = oc_cmd_new(OC_CMD_LOGOUT);
    if (!cmd) return;
    cmd->op = scope;
    oc_queue_push(&c->cmds, cmd);
}

int oc_client_outbox_pending(oc_client *c) {
    return c ? oc_net_outbox_pending(c->net) : 0;
}

void oc_client_reconnect(oc_client *c) {
    if (c) oc_net_reconnect(c->net);
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
