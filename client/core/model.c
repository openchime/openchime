/*
 * OpenChime client — the view-model + reducers (ARCH-74). See model.h.
 */

#include "model.h"

#include "protocol.h"   /* OC_PRESENCE_OFFLINE */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

#define OC_TYPING_TIMEOUT 6   /* seconds a typing mark stays live without refresh */

void oc_model_init(oc_model *m) {
    memset(m, 0, sizeof *m);
}

static void msg_free(oc_msg *m) { free(m->body); free(m->reactions); free(m->attach); }

static void channel_free(oc_channel *c) {
    for (size_t i = 0; i < c->n_msgs; i++) msg_free(&c->msgs[i]);
    free(c->msgs);
    free(c->name);
    free(c->topic);
    free(c->readers);
}

/* Turn a message into a tombstone (REQ-052). The body is not the only thing that
 * goes: reactions, attachments and any pin hung off a body that no longer
 * exists. The daemon drops the same state server-side, so leaving it here left
 * the client showing rows the server had already deleted — a live view that
 * disagreed with what a reload would produce. */
static void msg_tombstone(oc_msg *msg) {
    msg->deleted = 1;
    free(msg->reactions);
    msg->reactions = NULL;
    msg->n_reactions = msg->cap_reactions = 0;
    free(msg->attach);
    msg->attach = NULL;
    msg->n_attach = msg->cap_attach = 0;
    msg->pinned = 0;
    msg->pinned_by = msg->pinned_at = 0;
    msg->saved = 0;
    msg->saved_at = 0;
}

/* Apply a PIN event to one message, wherever it lives (REQ-230). */
/* Both message lists, always — the channel's and any open thread's. Keeping this
 * beside msg_set_pinned because it is the same shape and the same trap: applying
 * a pin to only c->msgs left a pinned REPLY looking unpinned in the thread pane. */
static void msg_set_saved(oc_msg *msg, const oc_ev *e) {
    msg->saved    = (e->op == 1);
    msg->saved_at = (e->op == 1) ? e->pinned_at : 0;   /* the ev's timestamp slot */
}

static void msg_set_pinned(oc_msg *msg, const oc_ev *e) {
    msg->pinned    = (e->op == 1);
    msg->pinned_by = (e->op == 1) ? e->user_id : 0;
    msg->pinned_at = (e->op == 1) ? e->server_time : 0;
}

/* Fold a reaction aggregate (emoji now has `count` reactors) into a message. */
static void msg_apply_reaction(oc_msg *msg, const char *emoji, uint32_t count,
                               uint8_t op, int is_me) {
    if (!emoji || !emoji[0]) return;
    uint8_t idx = msg->n_reactions;
    for (uint8_t i = 0; i < msg->n_reactions; i++)
        if (strcmp(msg->reactions[i].emoji, emoji) == 0) { idx = i; break; }

    if (count == 0) {                         /* emoji fully removed: drop the row */
        if (idx < msg->n_reactions) {
            msg->reactions[idx] = msg->reactions[msg->n_reactions - 1];
            msg->n_reactions--;
        }
        return;
    }
    if (idx == msg->n_reactions) {            /* new emoji: append (grow if needed) */
        if (msg->n_reactions == msg->cap_reactions) {
            uint8_t cap = msg->cap_reactions ? (uint8_t)(msg->cap_reactions * 2) : 4;
            oc_reaction *nr = realloc(msg->reactions, (size_t)cap * sizeof *nr);
            if (!nr) return;
            msg->reactions = nr; msg->cap_reactions = cap;
        }
        oc_reaction *r = &msg->reactions[idx];
        memset(r, 0, sizeof *r);
        snprintf(r->emoji, sizeof r->emoji, "%s", emoji);
        msg->n_reactions++;
    }
    msg->reactions[idx].count = count;
    if (is_me) msg->reactions[idx].mine = (op == 1 /* OC_REACT_ADD */) ? 1 : 0;
}

/* One monotonic millisecond clock, owned by the core so a frontend and the net
 * thread cannot read different ones. mingw has no clock_gettime without linking
 * winpthread, and GetTickCount64 is the platform's monotonic tick anyway. */
uint64_t oc_model_now_ms(void) {
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
    return (uint64_t)time(NULL) * 1000u;
#endif
}

uint64_t oc_model_reconnect_in(const oc_model *m, uint64_t now_ms) {
    if (!m || !m->reconnect_at_ms || m->reconnect_at_ms <= now_ms) return 0;
    return m->reconnect_at_ms - now_ms;
}

uint8_t *oc_model_take_attachment(oc_model *m, uint64_t *attachment_id, size_t *len) {
    if (!m || !m->fetched_data) return NULL;
    uint8_t *d = m->fetched_data;
    if (attachment_id) *attachment_id = m->fetched_attachment;
    if (len) *len = m->fetched_len;
    m->fetched_data = NULL;
    m->fetched_len = 0;
    m->fetched_attachment = 0;
    return d;
}

void oc_model_free(oc_model *m) {
    free(m->fetched_data);
    for (size_t i = 0; i < m->n_channels; i++) channel_free(&m->channels[i]);
    free(m->channels);
    free(m->presence);
    free(m->typing);
    for (size_t i = 0; i < m->n_thread_msgs; i++) msg_free(&m->thread_msgs[i]);
    free(m->thread_msgs);
    for (size_t i = 0; i < m->n_search; i++) free(m->search_results[i].snippet);
    free(m->search_results);
    free(m->reactors);
    for (size_t i = 0; i < m->n_pins; i++) free(m->pins[i].body);
    free(m->pins);
    free(m->chanmem);
    for (size_t i = 0; i < m->n_saved; i++) free(m->saved[i].body);
    free(m->saved);
    for (size_t i = 0; i < m->n_activity; i++) free(m->activity[i].text);
    free(m->activity);
    free(m->files);
    free(m->users);
    free(m->webhooks);
    free(m->settings);
    free(m->audit);
    memset(m, 0, sizeof *m);
}

/* Upsert a synced setting keyed on `key` (grow the bucket as needed). */
static void setting_upsert(oc_model *m, const char *key, const char *value) {
    if (!key || !key[0]) return;
    for (size_t i = 0; i < m->n_settings; i++)
        if (strcmp(m->settings[i].key, key) == 0) {
            snprintf(m->settings[i].value, sizeof m->settings[i].value, "%s", value ? value : "");
            return;
        }
    if (m->n_settings == m->cap_settings) {
        size_t cap = m->cap_settings ? m->cap_settings * 2 : 8;
        oc_setting *ns = realloc(m->settings, cap * sizeof *ns);
        if (!ns) return;
        m->settings = ns; m->cap_settings = cap;
    }
    oc_setting *s = &m->settings[m->n_settings++];
    snprintf(s->key, sizeof s->key, "%s", key);
    snprintf(s->value, sizeof s->value, "%s", value ? value : "");
}

const char *oc_model_setting(const oc_model *m, const char *key) {
    if (!key) return NULL;
    for (size_t i = 0; i < m->n_settings; i++)
        if (strcmp(m->settings[i].key, key) == 0) return m->settings[i].value;
    return NULL;
}

/* Advance a member's read cursor in a channel (grow the list as needed). */
static void reader_advance(oc_channel *c, uint64_t user_id, uint64_t message_id) {
    for (size_t i = 0; i < c->n_readers; i++)
        if (c->readers[i].user_id == user_id) {
            if (message_id > c->readers[i].message_id) c->readers[i].message_id = message_id;
            return;
        }
    if (c->n_readers == c->cap_readers) {
        size_t cap = c->cap_readers ? c->cap_readers * 2 : 8;
        oc_read_cursor_view *nr = realloc(c->readers, cap * sizeof *nr);
        if (!nr) return;
        c->readers = nr; c->cap_readers = cap;
    }
    c->readers[c->n_readers].user_id = user_id;
    c->readers[c->n_readers].message_id = message_id;
    c->n_readers++;
}

size_t oc_model_seen_by(const oc_model *m, uint64_t channel_id, uint64_t message_id,
                        uint64_t exclude, uint64_t *out, size_t cap) {
    oc_channel *c = oc_model_channel((oc_model *)m, channel_id);
    if (!c || message_id == 0) return 0;
    size_t n = 0;
    for (size_t i = 0; i < c->n_readers && n < cap; i++)
        if (c->readers[i].user_id != exclude && c->readers[i].message_id >= message_id)
            out[n++] = c->readers[i].user_id;
    return n;
}

/* Upsert a roster entry keyed on user_id. */
static void user_upsert(oc_model *m, uint64_t user_id, const char *name, uint8_t role,
                        uint8_t disabled, uint64_t avatar_id) {
    for (size_t i = 0; i < m->n_users; i++)
        if (m->users[i].user_id == user_id) {
            snprintf(m->users[i].name, sizeof m->users[i].name, "%s", name ? name : "");
            m->users[i].role = role; m->users[i].disabled = disabled;
            m->users[i].avatar_id = avatar_id;                    /* WIN-47 */
            return;
        }
    if (m->n_users == m->cap_users) {
        size_t cap = m->cap_users ? m->cap_users * 2 : 16;
        oc_member *nu = realloc(m->users, cap * sizeof *nu);
        if (!nu) return;
        m->users = nu; m->cap_users = cap;
    }
    oc_member *u = &m->users[m->n_users++];
    memset(u, 0, sizeof *u);
    u->user_id = user_id; u->role = role; u->disabled = disabled;
    u->avatar_id = avatar_id;
    snprintf(u->name, sizeof u->name, "%s", name ? name : "");
}

/* Update a roster member's role/disabled without disturbing its name (a
 * USER_UPDATED carries no name); upsert with an empty name if unseen. */
static void user_update_role(oc_model *m, uint64_t user_id, uint8_t role, uint8_t disabled) {
    for (size_t i = 0; i < m->n_users; i++)
        if (m->users[i].user_id == user_id) {
            m->users[i].role = role; m->users[i].disabled = disabled; return;
        }
    user_upsert(m, user_id, "", role, disabled, 0);
}

const char *oc_model_user_name(const oc_model *m, uint64_t user_id) {
    for (size_t i = 0; i < m->n_users; i++)
        if (m->users[i].user_id == user_id) return m->users[i].name;
    return "";
}

uint8_t     oc_model_deployment_mode(const oc_model *m) { return m->deployment_mode; }
uint32_t    oc_model_max_users(const oc_model *m)       { return m->max_users; }
const char *oc_model_workspace_name(const oc_model *m)  { return m->workspace_name; }
const char *oc_model_deployment_name(const oc_model *m) {
    switch (m->deployment_mode) {
        case 1:  return "federated";
        case 2:  return "managed";
        default: return "standalone";
    }
}

uint64_t oc_model_user_id(const oc_model *m, const char *name) {
    if (!name) return 0;
    for (size_t i = 0; i < m->n_users; i++)
        if (strcmp(m->users[i].name, name) == 0) return m->users[i].user_id;
    return 0;
}

void oc_model_close_search(oc_model *m) {
    for (size_t i = 0; i < m->n_search; i++) free(m->search_results[i].snippet);
    free(m->search_results);
    m->search_results = NULL;
    m->n_search = m->cap_search = 0;
    m->search_open = 0;
    m->search_truncated = 0;
    m->search_query[0] = '\0';
}

void oc_model_search_begin(oc_model *m, const char *query) {
    oc_model_close_search(m);      /* drop any prior hits */
    m->search_open = 1;
    snprintf(m->search_query, sizeof m->search_query, "%s", query ? query : "");
}

void oc_model_close_reactlist(oc_model *m) {
    free(m->reactors);
    m->reactors = NULL;
    m->n_reactors = m->cap_reactors = 0;
    m->reactlist_open = 0;
    m->reactlist_message = 0;
}

void oc_model_reactlist_begin(oc_model *m, uint64_t message_id) {
    oc_model_close_reactlist(m);   /* drop any prior reactors */
    m->reactlist_open = 1;
    m->reactlist_message = message_id;
}

void oc_model_close_pinlist(oc_model *m) {
    for (size_t i = 0; i < m->n_pins; i++) free(m->pins[i].body);
    free(m->pins);
    m->pins = NULL;
    m->n_pins = m->cap_pins = 0;
    m->pinlist_open = 0;
    m->pinlist_loading = 0;
    m->pinlist_channel = 0;
}

void oc_model_pinlist_begin(oc_model *m, uint64_t channel_id) {
    oc_model_close_pinlist(m);     /* drop any prior list */
    m->pinlist_open = 1;
    m->pinlist_loading = 1;
    m->pinlist_channel = channel_id;
}

void oc_model_chanmem_begin(oc_model *m, uint64_t channel_id) {
    m->n_chanmem = 0;                 /* keep the allocation, drop the rows */
    m->chanmem_channel = channel_id;
    m->chanmem_loading = 1;
}

void oc_model_close_filelist(oc_model *m) {
    free(m->files);
    m->files = NULL;
    m->n_files = m->cap_files = 0;
    m->filelist_open = m->filelist_loading = 0;
    m->filelist_channel = 0;
}

void oc_model_filelist_begin(oc_model *m, uint64_t channel_id) {
    oc_model_close_filelist(m);
    m->filelist_open = 1;
    m->filelist_loading = 1;
    m->filelist_channel = channel_id;
}

void oc_model_close_saved(oc_model *m) {
    for (size_t i = 0; i < m->n_saved; i++) free(m->saved[i].body);
    free(m->saved);
    m->saved = NULL; m->n_saved = m->cap_saved = 0;
    m->saved_open = m->saved_loading = 0;
}
void oc_model_saved_begin(oc_model *m) {
    oc_model_close_saved(m);
    m->saved_open = 1; m->saved_loading = 1;
}
void oc_model_close_activity(oc_model *m) {
    for (size_t i = 0; i < m->n_activity; i++) free(m->activity[i].text);
    free(m->activity);
    m->activity = NULL; m->n_activity = m->cap_activity = 0;
    m->activity_open = m->activity_loading = 0;
}
void oc_model_activity_begin(oc_model *m) {
    oc_model_close_activity(m);
    m->activity_open = 1; m->activity_loading = 1;
}

void oc_model_close_weblist(oc_model *m) {
    free(m->webhooks);
    m->webhooks = NULL;
    m->n_webhooks = m->cap_webhooks = 0;
    m->weblist_open = 0;
    m->weblist_channel = 0;
}

void oc_model_close_invites(oc_model *m) {
    free(m->invites);
    m->invites = NULL;
    m->n_invites = m->cap_invites = 0;
    m->invites_open = m->invites_loading = 0;
}

void oc_model_close_sessions(oc_model *m) {
    free(m->sessions);
    m->sessions = NULL;
    m->n_sessions = m->cap_sessions = 0;
    m->sessions_open = m->sessions_loading = 0;
}

void oc_model_sessions_begin(oc_model *m) {
    oc_model_close_sessions(m);
    m->sessions_open = 1;
    m->sessions_loading = 1;
}

void oc_model_invites_begin(oc_model *m) {
    oc_model_close_invites(m);
    m->invites_open = 1;
    m->invites_loading = 1;
}

void oc_model_weblist_begin(oc_model *m, uint64_t channel_id) {
    oc_model_close_weblist(m);     /* drop any prior list */
    m->weblist_open = 1;
    m->weblist_channel = channel_id;
    m->webhook_token[0] = '\0';    /* clear any prior shown-once token */
    m->webhook_new_id = 0;
}

void oc_model_set_prefs_open(oc_model *m, int open) {
    m->prefs_open = open ? 1 : 0;
}

void oc_model_close_thread(oc_model *m) {
    for (size_t i = 0; i < m->n_thread_msgs; i++) msg_free(&m->thread_msgs[i]);
    free(m->thread_msgs);
    m->thread_msgs = NULL;
    m->n_thread_msgs = m->cap_thread_msgs = 0;
    m->thread_open = 0;
    m->thread_parent = m->thread_channel = 0;
}

void oc_model_open_thread(oc_model *m, uint64_t channel_id, uint64_t parent_id) {
    oc_model_close_thread(m);      /* drop any prior thread's replies */
    m->thread_open = 1;
    m->thread_channel = channel_id;
    m->thread_parent = parent_id;
}

/* Append a thread reply, stealing ownership of `*body` (NULL on success). */
static void thread_append(oc_model *m, uint64_t message_id, uint64_t author_id,
                          uint64_t server_time, char **body) {
    for (size_t i = 0; i < m->n_thread_msgs; i++)
        if (m->thread_msgs[i].message_id == message_id) return;   /* dedup */
    if (m->n_thread_msgs == m->cap_thread_msgs) {
        size_t cap = m->cap_thread_msgs ? m->cap_thread_msgs * 2 : 16;
        oc_msg *nm = realloc(m->thread_msgs, cap * sizeof *nm);
        if (!nm) return;
        m->thread_msgs = nm; m->cap_thread_msgs = cap;
    }
    oc_msg *msg = &m->thread_msgs[m->n_thread_msgs++];
    memset(msg, 0, sizeof *msg);
    msg->body = *body;
    msg->author_id = author_id;
    msg->message_id = message_id;
    msg->server_time = server_time;
    *body = NULL;
}

/* Append a search hit, stealing ownership of `*snippet` (NULL on success). */
static void search_append(oc_model *m, uint64_t message_id, uint64_t channel_id,
                          uint64_t author_id, uint64_t server_time, char **snippet) {
    for (size_t i = 0; i < m->n_search; i++)
        if (m->search_results[i].message_id == message_id) return;   /* dedup */
    if (m->n_search == m->cap_search) {
        size_t cap = m->cap_search ? m->cap_search * 2 : 16;
        oc_search_result *ns = realloc(m->search_results, cap * sizeof *ns);
        if (!ns) return;
        m->search_results = ns; m->cap_search = cap;
    }
    oc_search_result *sr = &m->search_results[m->n_search++];
    sr->message_id = message_id; sr->channel_id = channel_id;
    sr->author_id = author_id; sr->server_time = server_time;
    sr->snippet = *snippet; *snippet = NULL;
}

/* Append one reactor (user_id + emoji) to the open who-reacted list. */
static void reactor_append(oc_model *m, uint64_t user_id, const char *emoji) {
    if (m->n_reactors == m->cap_reactors) {
        size_t cap = m->cap_reactors ? m->cap_reactors * 2 : 16;
        oc_reactor_row *nr = realloc(m->reactors, cap * sizeof *nr);
        if (!nr) return;
        m->reactors = nr; m->cap_reactors = cap;
    }
    oc_reactor_row *r = &m->reactors[m->n_reactors++];
    r->user_id = user_id;
    snprintf(r->emoji, sizeof r->emoji, "%s", emoji ? emoji : "");
}

/* Upsert one webhook (id + label + disabled) into the open webhook list. */
static void webhook_upsert(oc_model *m, uint64_t webhook_id, const char *label, uint8_t disabled) {
    for (size_t i = 0; i < m->n_webhooks; i++)
        if (m->webhooks[i].webhook_id == webhook_id) {
            snprintf(m->webhooks[i].label, sizeof m->webhooks[i].label, "%s", label ? label : "");
            m->webhooks[i].disabled = disabled; return;
        }
    if (m->n_webhooks == m->cap_webhooks) {
        size_t cap = m->cap_webhooks ? m->cap_webhooks * 2 : 16;
        oc_webhook_view *nw = realloc(m->webhooks, cap * sizeof *nw);
        if (!nw) return;
        m->webhooks = nw; m->cap_webhooks = cap;
    }
    oc_webhook_view *w = &m->webhooks[m->n_webhooks++];
    w->webhook_id = webhook_id;
    w->disabled = disabled;
    snprintf(w->label, sizeof w->label, "%s", label ? label : "");
}

/* Drop a webhook from the open list by id (a WEBHOOK_DELETED ack). */
static void webhook_remove(oc_model *m, uint64_t webhook_id) {
    for (size_t i = 0; i < m->n_webhooks; i++)
        if (m->webhooks[i].webhook_id == webhook_id) {
            m->webhooks[i] = m->webhooks[--m->n_webhooks];
            return;
        }
}

/* Raise a message's thread reply count, finding it in any channel (message ids
 * are globally unique, and THREAD_META carries no channel_id). */
static void bump_reply_count(oc_model *m, uint64_t message_id, uint32_t count) {
    for (size_t ci = 0; ci < m->n_channels; ci++)
        for (size_t i = 0; i < m->channels[ci].n_msgs; i++)
            if (m->channels[ci].msgs[i].message_id == message_id) {
                if (count > m->channels[ci].msgs[i].reply_count)
                    m->channels[ci].msgs[i].reply_count = count;
                return;
            }
}

/* Append an attachment to a message, deduped by id. Frees nothing; the caller
 * owns the source strings (copied in). */
static void msg_add_attach(oc_msg *msg, uint64_t id, const char *filename,
                           const char *mime, uint64_t size, uint8_t reclaimed) {
    for (uint8_t i = 0; i < msg->n_attach; i++)
        if (msg->attach[i].id == id) return;   /* dedup (backfill re-delivery) */
    if (msg->n_attach >= OC_MAX_ATTACH) return;
    if (msg->n_attach == msg->cap_attach) {
        uint8_t cap = msg->cap_attach ? (uint8_t)(msg->cap_attach * 2) : 2;
        oc_attachment *na = realloc(msg->attach, (size_t)cap * sizeof *na);
        if (!na) return;
        msg->attach = na; msg->cap_attach = cap;
    }
    oc_attachment *a = &msg->attach[msg->n_attach++];
    a->id = id; a->size = size; a->reclaimed = reclaimed;
    snprintf(a->filename, sizeof a->filename, "%s", filename ? filename : "");
    snprintf(a->mime, sizeof a->mime, "%s", mime ? mime : "");
}

/* Attach an attachment to the message with `message_id`, searching every channel
 * buffer and the open thread's replies (message ids are globally unique). */
static void attach_to_msg(oc_model *m, uint64_t message_id, uint64_t id,
                          const char *filename, const char *mime, uint64_t size,
                          uint8_t reclaimed) {
    for (size_t ci = 0; ci < m->n_channels; ci++)
        for (size_t i = 0; i < m->channels[ci].n_msgs; i++)
            if (m->channels[ci].msgs[i].message_id == message_id) {
                msg_add_attach(&m->channels[ci].msgs[i], id, filename, mime, size, reclaimed);
                return;
            }
    for (size_t i = 0; i < m->n_thread_msgs; i++)
        if (m->thread_msgs[i].message_id == message_id) {
            msg_add_attach(&m->thread_msgs[i], id, filename, mime, size, reclaimed);
            return;
        }
}

/* Record that `user_id` is typing in `channel_id` now, pruning stale marks. */
static void typing_touch(oc_model *m, uint64_t channel_id, uint64_t user_id) {
    long long now = (long long)time(NULL);
    size_t w = 0, found = (size_t)-1;
    for (size_t i = 0; i < m->n_typing; i++) {
        if (now - m->typing[i].seen > OC_TYPING_TIMEOUT) continue;   /* drop expired */
        if (m->typing[i].channel_id == channel_id && m->typing[i].user_id == user_id) found = w;
        m->typing[w++] = m->typing[i];
    }
    m->n_typing = w;
    if (found != (size_t)-1) { m->typing[found].seen = now; return; }
    if (m->n_typing == m->cap_typing) {
        size_t cap = m->cap_typing ? m->cap_typing * 2 : 8;
        oc_typing_row *nt = realloc(m->typing, cap * sizeof *nt);
        if (!nt) return;
        m->typing = nt; m->cap_typing = cap;
    }
    m->typing[m->n_typing].channel_id = channel_id;
    m->typing[m->n_typing].user_id = user_id;
    m->typing[m->n_typing].seen = now;
    m->n_typing++;
}

size_t oc_model_typing(const oc_model *m, uint64_t channel_id, uint64_t exclude,
                       uint64_t *out, size_t cap) {
    long long now = (long long)time(NULL);
    size_t n = 0;
    for (size_t i = 0; i < m->n_typing && n < cap; i++) {
        if (m->typing[i].channel_id != channel_id) continue;
        if (m->typing[i].user_id == exclude) continue;
        if (now - m->typing[i].seen > OC_TYPING_TIMEOUT) continue;
        out[n++] = m->typing[i].user_id;
    }
    return n;
}

oc_channel *oc_model_channel(oc_model *m, uint64_t channel_id) {
    for (size_t i = 0; i < m->n_channels; i++)
        if (m->channels[i].channel_id == channel_id) return &m->channels[i];
    return NULL;
}

void oc_model_mark_read(oc_model *m, uint64_t channel_id) {
    oc_channel *c = oc_model_channel(m, channel_id);
    if (!c) return;
    c->read_marker = c->high_water;
    c->unread = 0;
}

static oc_channel *channel_ensure(oc_model *m, uint64_t channel_id) {
    oc_channel *c = oc_model_channel(m, channel_id);
    if (c) return c;
    if (m->n_channels == m->cap_channels) {
        size_t cap = m->cap_channels ? m->cap_channels * 2 : 8;
        oc_channel *nc = realloc(m->channels, cap * sizeof *nc);
        if (!nc) return NULL;
        m->channels = nc;
        m->cap_channels = cap;
    }
    c = &m->channels[m->n_channels++];
    memset(c, 0, sizeof *c);
    c->channel_id = channel_id;
    return c;
}

/* Append a message, stealing ownership of `*body` (set to NULL on success).
 * Returns 1 if a message was appended, 0 if it was a dedup/alloc no-op. */
static int channel_append(oc_channel *c, uint64_t author_id, const char *author_name,
                          uint64_t message_id, uint64_t server_time, char **body) {
    /* Dedup on the per-channel high-water mark (ARCH-45). message_id 0 means the
     * server did not assign one (shouldn't happen for a BROADCAST) — keep it.
     *
     * An id at or below the mark is USUALLY a redelivery, but it is also what a
     * backwards history page looks like (WIN-16). So instead of rejecting it
     * outright, check whether we actually hold it: if not, it is older history
     * and belongs at its sorted position. Rejecting on the mark alone silently
     * discarded every paged-in message. */
    int older = (message_id && message_id <= c->high_water);
    if (older) {
        for (size_t i = 0; i < c->n_msgs; i++)
            if (c->msgs[i].message_id == message_id) return 0;   /* genuine redelivery */
    }
    if (c->n_msgs == c->cap_msgs) {
        size_t cap = c->cap_msgs ? c->cap_msgs * 2 : 32;
        oc_msg *nm = realloc(c->msgs, cap * sizeof *nm);
        if (!nm) return 0;
        c->msgs = nm;
        c->cap_msgs = cap;
    }
    /* Keep the array ordered by id: the renderer walks it as the transcript, and
     * grouping/date dividers assume ascending time. */
    size_t at = c->n_msgs;
    if (older) {
        while (at > 0 && c->msgs[at - 1].message_id > message_id) at--;
        memmove(&c->msgs[at + 1], &c->msgs[at], (c->n_msgs - at) * sizeof *c->msgs);
    }
    c->n_msgs++;
    oc_msg *msg = &c->msgs[at];
    memset(msg, 0, sizeof *msg);   /* clear reactions et al. before we populate */
    msg->body = *body;
    snprintf(msg->author_name, sizeof msg->author_name, "%s", author_name ? author_name : "");
    msg->author_id = author_id;
    msg->message_id = message_id;
    msg->server_time = server_time;
    *body = NULL;
    if (message_id > c->high_water) c->high_water = message_id;
    return 1;
}

static void presence_set(oc_model *m, uint64_t user_id, uint8_t status) {
    for (size_t i = 0; i < m->n_presence; i++) {
        if (m->presence[i].user_id == user_id) { m->presence[i].status = status; return; }
    }
    if (m->n_presence == m->cap_presence) {
        size_t cap = m->cap_presence ? m->cap_presence * 2 : 16;
        oc_presence_row *np = realloc(m->presence, cap * sizeof *np);
        if (!np) return;
        m->presence = np;
        m->cap_presence = cap;
    }
    m->presence[m->n_presence].user_id = user_id;
    m->presence[m->n_presence].status = status;
    m->n_presence++;
}

void oc_model_note_presence(oc_model *m, uint64_t user_id, uint8_t status) {
    presence_set(m, user_id, status);
}

uint8_t oc_model_presence_of(const oc_model *m, uint64_t user_id) {
    for (size_t i = 0; i < m->n_presence; i++)
        if (m->presence[i].user_id == user_id) return m->presence[i].status;
    return OC_PRESENCE_OFFLINE;
}

static void set_status(oc_model *m, const char *s) {
    snprintf(m->status, sizeof m->status, "%s", s ? s : "");
}

void oc_model_apply(oc_model *m, oc_ev *e) {
    switch (e->type) {
    case OC_EV_CONNECTED:
        m->connected = true;
        m->user_id = e->user_id;
        m->last_error[0] = '\0';        /* a live connection clears any prior error */
        set_status(m, "connected");
        break;
    case OC_EV_AUTH_OK:
        m->authed = true;
        m->user_id = e->user_id;
        m->last_error[0] = '\0';
        presence_set(m, e->user_id, OC_PRESENCE_ONLINE);   /* self: the server won't tell us */
        set_status(m, "authenticated");
        break;
    case OC_EV_WORKSPACE_INFO:
        m->deployment_mode = e->status;
        m->max_users = e->count;
        snprintf(m->workspace_name, sizeof m->workspace_name, "%s", e->body ? e->body : "");
        break;
    case OC_EV_CHANNEL: {
        oc_channel *c = channel_ensure(m, e->channel_id);
        if (c) {
            c->joined = e->status;
            c->kind = e->op;                     /* channel vs DM */
            c->is_public = e->is_public;         /* public vs private (REQ-031) */
            if (e->user_id) c->peer_id = e->user_id;   /* DM peer (CHANNEL_INFO only) */
            /* A group DM's participants (REQ-056). Assigned, not merged: the server's
             * set is the truth, and a group can lose a member. */
            c->n_peers = e->n_peers > 9 ? 9 : e->n_peers;
            for (uint16_t k = 0; k < c->n_peers; k++) c->peers[k] = e->peers[k];
            if (e->server_time) c->last_message_at = e->server_time;
            if (e->count) {
                c->srv_unread = e->count;
                /* Before any backfill the server's count is all we know, so let
                 * it seed the badge; live BROADCASTs take over from there. */
                if (c->unread == 0) c->unread = (int)e->count;
            }
            if (e->body) { free(c->name); c->name = e->body; e->body = NULL; }
            /* Topic and archived ride on the same frame (ARCH-93): a rename, a
             * topic change and an archive all arrive as CHANNEL_INFO, so this
             * one arm keeps the sidebar and the header honest for all three. */
            c->archived = e->archived;
            if (e->created_at) c->created_at = e->created_at;
            if (e->preview) {
                snprintf(c->preview, sizeof c->preview, "%s", e->preview);
                c->preview_author = e->preview_author;
            }
            free(c->topic);
            c->topic = (e->topic && e->topic[0]) ? strdup(e->topic) : NULL;
        }
        break;
    }
    case OC_EV_MESSAGE: {
        oc_channel *c = channel_ensure(m, e->channel_id);
        /* Keep the list preview live. Taken BEFORE channel_append, which takes
         * ownership of the body — after it, e->body is gone. Only for messages
         * newer than what we last previewed, so a backfill replaying older
         * history cannot rewind the sidebar to an old line. */
        if (c && e->body && e->message_id >= c->high_water) {
            snprintf(c->preview, sizeof c->preview, "%s", e->body);
            c->preview_author = e->author_id;
        }
        if (c && channel_append(c, e->author_id, e->author_name, e->message_id, e->server_time, &e->body)) {
            /* Count as unread only messages from others past the read marker; a
             * frontend clears this by marking the focused channel read. */
            if (e->author_id != m->user_id && e->message_id > c->read_marker)
                c->unread++;
        }
        break;
    }
    case OC_EV_PRESENCE:
        presence_set(m, e->user_id, e->status);
        break;
    case OC_EV_TYPING:
        typing_touch(m, e->channel_id, e->user_id);
        break;
    case OC_EV_CHAN_MEMBER: {
        /* Only for the roster currently being loaded — a late frame from a
         * previous channel must not land in this one's list. */
        if (m->chanmem_channel != e->channel_id) break;
        if (m->n_chanmem == m->cap_chanmem) {
            size_t nc = m->cap_chanmem ? m->cap_chanmem * 2 : 16;
            oc_chan_member *g = realloc(m->chanmem, nc * sizeof *g);
            if (!g) break;
            m->chanmem = g; m->cap_chanmem = nc;
        }
        oc_chan_member *cm = &m->chanmem[m->n_chanmem++];
        cm->user_id   = e->user_id;
        cm->role      = e->status;
        cm->joined_at = e->server_time;
        break;
    }
    case OC_EV_CHAN_MEMBERS_END:
        if (m->chanmem_channel == e->channel_id) m->chanmem_loading = 0;
        break;
    case OC_EV_FILE: {
        /* Matched on "a list is in flight", not on the channel: in the
         * workspace-wide view (channel 0) each entry names its OWN channel, so
         * comparing against the requested one would drop every row. The
         * terminator clears `loading`, which is what fences a late frame from a
         * previous request. */
        if (!m->filelist_open || !m->filelist_loading) break;
        if (m->n_files == m->cap_files) {
            size_t nc = m->cap_files ? m->cap_files * 2 : 32;
            oc_file_view *g = realloc(m->files, nc * sizeof *g);
            if (!g) break;
            m->files = g; m->cap_files = nc;
        }
        oc_file_view *f = &m->files[m->n_files++];
        f->id          = e->attach_id;
        f->channel_id  = e->channel_id;      /* the file's own channel */
        f->message_id  = e->message_id;
        f->uploader_id = e->author_id;
        f->size        = e->size;
        f->created_at  = e->server_time;
        f->reclaimed   = e->reclaimed;
        snprintf(f->filename, sizeof f->filename, "%s", e->body ? e->body : "");
        snprintf(f->mime, sizeof f->mime, "%s", e->emoji);
        break;
    }
    case OC_EV_FILES_END:
        if (m->filelist_open && m->filelist_channel == e->channel_id) m->filelist_loading = 0;
        break;
    case OC_EV_SAVED_MSG: {
        if (!m->saved_open || !m->saved_loading) break;
        if (m->n_saved == m->cap_saved) {
            size_t nc = m->cap_saved ? m->cap_saved * 2 : 16;
            oc_saved_view *g = realloc(m->saved, nc * sizeof *g);
            if (!g) break;
            m->saved = g; m->cap_saved = nc;
        }
        oc_saved_view *sv = &m->saved[m->n_saved++];
        sv->message_id = e->message_id; sv->channel_id = e->channel_id;
        sv->author_id  = e->author_id;  sv->server_time = e->server_time;
        sv->saved_at   = e->pinned_at;
        sv->body = e->body ? strdup(e->body) : NULL;
        snprintf(sv->attach_name, sizeof sv->attach_name, "%s", e->author_name);
        break;
    }
    case OC_EV_SAVED_END:
        if (m->saved_open) m->saved_loading = 0;
        break;
    case OC_EV_SESSION_ROW: {
        if (!m->sessions_open) break;
        if (m->n_sessions == m->cap_sessions) {
            size_t nc = m->cap_sessions ? m->cap_sessions * 2 : 8;
            oc_session_row *g = realloc(m->sessions, nc * sizeof *g);
            if (!g) break;
            m->sessions = g; m->cap_sessions = nc;
        }
        oc_session_row *sr = &m->sessions[m->n_sessions++];
        sr->session_id = e->message_id;
        sr->created_at = e->server_time;
        sr->last_seen  = e->pinned_at;      /* the ev's spare timestamp slot */
        sr->expires_at = e->channel_id;    /* 64-bit slot; see net.c */
        sr->current    = e->op;
        snprintf(sr->device, sizeof sr->device, "%s", e->body ? e->body : "");
        break;
    }
    case OC_EV_SESSIONS_END:
        if (m->sessions_open) m->sessions_loading = 0;
        break;
    case OC_EV_FILE_CHANNELS_BEGIN:
        m->n_fchans = 0;
        break;
    case OC_EV_FILE_CHANNEL: {
        if (m->n_fchans == m->cap_fchans) {
            size_t nc = m->cap_fchans ? m->cap_fchans * 2 : 16;
            oc_chan_count *g = realloc(m->fchans, nc * sizeof *g);
            if (!g) break;
            m->fchans = g; m->cap_fchans = nc;
        }
        m->fchans[m->n_fchans].channel_id = e->channel_id;
        m->fchans[m->n_fchans].count      = (uint32_t)e->count;
        m->n_fchans++;
        break;
    }
    case OC_EV_PROFILE_INFO: {
        /* Unpack the \x1f-separated fields packed in net.c — kept beside the struct
         * being filled, so the two halves are read together. */
        oc_member *mem = NULL;
        for (size_t i = 0; i < m->n_users; i++)
            if (m->users[i].user_id == e->user_id) { mem = &m->users[i]; break; }
        if (!mem) break;                 /* not in the roster: nothing to attach to */
        const char *f[5] = { "", "", "", "", "" };
        char buf[512];
        snprintf(buf, sizeof buf, "%s", e->body ? e->body : "");
        int nf = 0;
        f[nf++] = buf;
        for (char *q = buf; *q && nf < 5; q++)
            if (*q == '\x1f') { *q = '\0'; f[nf++] = q + 1; }
        if (f[0][0]) snprintf(mem->name, sizeof mem->name, "%s", f[0]);
        snprintf(mem->status_emoji, sizeof mem->status_emoji, "%s", f[1]);
        snprintf(mem->status_text,  sizeof mem->status_text,  "%s", f[2]);
        snprintf(mem->title,        sizeof mem->title,        "%s", f[3]);
        snprintf(mem->timezone,     sizeof mem->timezone,     "%s", f[4]);
        mem->status_expires = e->server_time;
        mem->avatar_id      = e->message_id;
        if (e->op) mem->role = e->op;
        break;
    }
    case OC_EV_INVITE_ROW: {
        if (!m->invites_open) break;
        if (m->n_invites == m->cap_invites) {
            size_t nc = m->cap_invites ? m->cap_invites * 2 : 16;
            oc_invite_row *g = realloc(m->invites, nc * sizeof *g);
            if (!g) break;
            m->invites = g; m->cap_invites = nc;
        }
        oc_invite_row *iv = &m->invites[m->n_invites++];
        iv->invite_id  = e->message_id;
        iv->role       = e->op;
        iv->expires_at = e->server_time;
        iv->created_by = e->user_id;
        break;
    }
    case OC_EV_INVITE_END:
        if (m->invites_open) m->invites_loading = 0;
        break;
    case OC_EV_INVITE_REVOKED:
        /* Drop the row locally so the list does not need a second round trip. */
        for (size_t i = 0; i < m->n_invites; i++)
            if (m->invites[i].invite_id == e->message_id) {
                memmove(&m->invites[i], &m->invites[i + 1],
                        (m->n_invites - i - 1) * sizeof *m->invites);
                m->n_invites--;
                break;
            }
        break;
    case OC_EV_SAVED_UPDATED:
        /* Mark the message itself, in every list that can hold it, so the
         * transcript can show a bookmark without the Later view ever being
         * opened. Both lists for the reason msg_set_saved states. */
        for (size_t ci = 0; ci < m->n_channels; ci++) {
            oc_channel *c = &m->channels[ci];
            for (size_t i = 0; i < c->n_msgs; i++)
                if (c->msgs[i].message_id == e->message_id) { msg_set_saved(&c->msgs[i], e); break; }
        }
        for (size_t i = 0; i < m->n_thread_msgs; i++)
            if (m->thread_msgs[i].message_id == e->message_id) {
                msg_set_saved(&m->thread_msgs[i], e);
                break;
            }
        /* Drop it from an open list on unsave, so the view cannot show a row
         * the server no longer has. */
        if (m->saved_open && e->op == 0)
            for (size_t i = 0; i < m->n_saved; i++)
                if (m->saved[i].message_id == e->message_id) {
                    free(m->saved[i].body);
                    memmove(&m->saved[i], &m->saved[i + 1],
                            (m->n_saved - i - 1) * sizeof *m->saved);
                    m->n_saved--;
                    break;
                }
        break;
    case OC_EV_ACTIVITY: {
        if (!m->activity_open || !m->activity_loading) break;
        if (m->n_activity == m->cap_activity) {
            size_t nc = m->cap_activity ? m->cap_activity * 2 : 32;
            oc_activity_view *g = realloc(m->activity, nc * sizeof *g);
            if (!g) break;
            m->activity = g; m->cap_activity = nc;
        }
        oc_activity_view *av = &m->activity[m->n_activity++];
        av->kind = e->status; av->message_id = e->message_id;
        av->channel_id = e->channel_id; av->actor_id = e->user_id;
        av->at = e->server_time;
        av->text = e->body ? strdup(e->body) : NULL;
        break;
    }
    case OC_EV_ACTIVITY_END:
        if (m->activity_open) { m->activity_loading = 0; m->activity_seen = e->pinned_at; }
        break;
    case OC_EV_PIN: {
        /* Mark the message in the transcript. Arriving for a message we have not
         * loaded is normal (backfill order, or another client's pin far up the
         * scroll) and is simply dropped — the flag rides along when that message
         * is eventually replayed. */
        oc_channel *c = oc_model_channel(m, e->channel_id);
        if (c)
            for (size_t i = 0; i < c->n_msgs; i++)
                if (c->msgs[i].message_id == e->message_id) {
                    msg_set_pinned(&c->msgs[i], e);
                    break;
                }
        /* A thread reply is NOT in the channel's message list, so marking only
         * that list left a pinned reply looking unpinned in the thread pane —
         * and its menu still offering "Pin", which made unpinning it from there
         * impossible (a re-pin is a no-op). Same omission as WIN-15. */
        for (size_t i = 0; i < m->n_thread_msgs; i++)
            if (m->thread_msgs[i].message_id == e->message_id) {
                msg_set_pinned(&m->thread_msgs[i], e);
                break;
            }
        /* Keep an open pins overlay honest: an unpin from anywhere removes the
         * row rather than leaving a stale entry that 404s when clicked. */
        if (m->pinlist_open && m->pinlist_channel == e->channel_id && e->op != 1) {
            for (size_t i = 0; i < m->n_pins; i++) {
                if (m->pins[i].message_id == e->message_id) {
                    free(m->pins[i].body);
                    memmove(&m->pins[i], &m->pins[i + 1],
                            (m->n_pins - i - 1) * sizeof *m->pins);
                    m->n_pins--;
                    break;
                }
            }
        }
        break;
    }
    case OC_EV_PINNED_MSG: {
        if (!m->pinlist_open || m->pinlist_channel != e->channel_id) break;
        if (m->n_pins == m->cap_pins) {
            size_t nc = m->cap_pins ? m->cap_pins * 2 : 16;
            oc_pinned_row *g = realloc(m->pins, nc * sizeof *g);
            if (!g) break;
            m->pins = g; m->cap_pins = nc;
        }
        oc_pinned_row *pr = &m->pins[m->n_pins++];
        pr->message_id  = e->message_id;
        pr->author_id   = e->author_id;
        pr->server_time = e->server_time;
        pr->pinned_by   = e->user_id;
        pr->pinned_at   = e->pinned_at;
        pr->body        = e->body ? strdup(e->body) : NULL;
        snprintf(pr->attach_name, sizeof pr->attach_name, "%s", e->author_name);
        break;
    }
    case OC_EV_PINS_END:
        if (m->pinlist_open && m->pinlist_channel == e->channel_id)
            m->pinlist_loading = 0;
        break;
    case OC_EV_REACTION: {
        oc_channel *c = oc_model_channel(m, e->channel_id);
        if (c) {
            for (size_t i = 0; i < c->n_msgs; i++) {
                if (c->msgs[i].message_id == e->message_id) {
                    msg_apply_reaction(&c->msgs[i], e->emoji, e->count, e->op,
                                       e->user_id == m->user_id);
                    break;
                }
            }
        }
        break;
    }
    case OC_EV_EDIT: {
        oc_channel *c = oc_model_channel(m, e->channel_id);
        if (c) {
            for (size_t i = 0; i < c->n_msgs; i++) {
                if (c->msgs[i].message_id == e->message_id) {
                    free(c->msgs[i].body);
                    c->msgs[i].body = e->body;   /* steal the new text */
                    e->body = NULL;
                    c->msgs[i].edited = 1;
                    break;
                }
            }
        }
        break;
    }
    case OC_EV_DELETE: {
        oc_channel *c = oc_model_channel(m, e->channel_id);
        if (c) {
            for (size_t i = 0; i < c->n_msgs; i++) {
                if (c->msgs[i].message_id == e->message_id) { msg_tombstone(&c->msgs[i]); break; }
            }
        }
        /* A deleted thread reply lives in the open thread, not the channel list
         * — the same omission that hid a pinned reply (WIN-15's lesson, twice). */
        for (size_t i = 0; i < m->n_thread_msgs; i++)
            if (m->thread_msgs[i].message_id == e->message_id) {
                msg_tombstone(&m->thread_msgs[i]);
                break;
            }
        break;
    }
    case OC_EV_THREAD_REPLY:
        bump_reply_count(m, e->parent_id, e->count);   /* mark the parent in scroll */
        if (m->thread_open && e->parent_id == m->thread_parent)
            thread_append(m, e->message_id, e->author_id, e->server_time, &e->body);
        break;
    case OC_EV_THREAD_META:
        bump_reply_count(m, e->message_id, e->count);
        break;
    case OC_EV_SEARCH_RESULT:
        if (m->search_open) {
            search_append(m, e->message_id, e->channel_id, e->author_id, e->server_time, &e->body);
            if (e->status) m->search_truncated = 1;
        }
        break;
    case OC_EV_REACTIONS:
        if (m->reactlist_open && e->message_id == m->reactlist_message)
            reactor_append(m, e->user_id, e->emoji);
        break;
    case OC_EV_DND:
        /* A NOTIFY_PREFS frame's header: it always precedes the per-channel
         * entries, so treat it as the sync boundary and reset every channel to
         * the default level; the following OC_EV_NOTIFY_PREF events set the
         * non-default ones. Works for both solicited and pushed syncs. */
        m->notify_default = e->op;                 /* REQ-134 */
        for (size_t i = 0; i < m->n_channels; i++)
            m->channels[i].notify_level = m->notify_default;
        m->dnd_enabled   = e->status;
        m->dnd_start_min = (uint16_t)(e->count >> 16);
        m->dnd_end_min   = (uint16_t)(e->count & 0xFFFF);
        break;
    case OC_EV_NOTIFY_PREF: {
        oc_channel *c = oc_model_channel(m, e->channel_id);
        if (c) { c->notify_level = e->op; c->muted = e->status; }
        break;
    }
    case OC_EV_EMOJI_BEGIN:
        m->n_cemoji = 0;                      /* the server's catalogue is the truth */
        break;
    case OC_EV_EMOJI: {
        if (m->n_cemoji == m->cap_cemoji) {
            size_t cap = m->cap_cemoji ? m->cap_cemoji * 2 : 32;
            oc_custom_emoji *ne = realloc(m->cemoji, cap * sizeof *ne);
            if (!ne) break;
            m->cemoji = ne; m->cap_cemoji = cap;
        }
        oc_custom_emoji *ce = &m->cemoji[m->n_cemoji++];
        memset(ce, 0, sizeof *ce);
        snprintf(ce->name, sizeof ce->name, "%s", e->body ? e->body : "");
        ce->attachment_id = e->message_id;
        ce->created_by = e->user_id;
        break;
    }
    case OC_EV_SETTINGS_BEGIN:
        /* A CLIENT_SETTINGS frame start (solicited or a device-sync push): the
         * snapshot replaces the bucket wholesale, so clear it before the entries. */
        m->n_settings = 0;
        m->settings_synced = 1;
        break;
    case OC_EV_SETTING:
        setting_upsert(m, e->author_name, e->body ? e->body : "");
        break;
    case OC_EV_AUDIT_BEGIN:
        m->n_audit = 0;              /* a page replaces whatever was shown */
        break;
    case OC_EV_AUDIT:
        if (m->n_audit == m->cap_audit) {
            size_t nc = m->cap_audit ? m->cap_audit * 2 : 64;
            oc_audit_view *na = realloc(m->audit, nc * sizeof *na);
            if (!na) break;
            m->audit = na; m->cap_audit = nc;
        }
        m->audit[m->n_audit++] = e->audit;
        break;
    case OC_EV_STORAGE:
        m->storage = e->storage;
        m->storage_have = 1;
        set_status(m, "storage report updated");
        break;
    case OC_EV_PROFILE:
        /* A display-name change (REQ-020): update the roster entry's name in place
         * (role/disabled untouched). For our own id it doubles as the change ack. */
        for (size_t i = 0; i < m->n_users; i++)
            if (m->users[i].user_id == e->user_id) {
                snprintf(m->users[i].name, sizeof m->users[i].name, "%s", e->body ? e->body : "");
                break;
            }
        if (e->user_id == m->user_id) set_status(m, "profile updated");
        break;
    case OC_EV_READ_CURSOR: {
        /* Seen-by (REQ-090): a member read `channel_id` up to `message_id`. */
        oc_channel *c = oc_model_channel(m, e->channel_id);
        if (c) reader_advance(c, e->user_id, e->message_id);
        break;
    }
    case OC_EV_USER_UPDATED: {
        user_update_role(m, e->user_id, e->status, e->op);
        const char *nm = oc_model_user_name(m, e->user_id);
        const char *what = e->op ? "removed"
                         : e->status == OC_ROLE_OWNER ? "now owner"
                         : e->status == OC_ROLE_ADMIN ? "now admin" : "now member";
        char buf[160];
        snprintf(buf, sizeof buf, "%s %s", nm[0] ? nm : "user", what);
        set_status(m, buf);
        break;
    }
    case OC_EV_INVITE:
        snprintf(m->invite_token, sizeof m->invite_token, "%s", e->body ? e->body : "");
        m->invite_role = e->op;
        m->invite_expires = e->server_time;
        {
            char buf[160];
            snprintf(buf, sizeof buf, "invite (%s): %s",
                     e->op == OC_ROLE_ADMIN ? "admin" : "member", m->invite_token);
            set_status(m, buf);
        }
        break;
    case OC_EV_USER:
        user_upsert(m, e->user_id, e->body ? e->body : "", e->status, e->op, e->message_id);
        break;
    case OC_EV_WEBHOOK_INFO:
        /* A freshly-minted webhook: the token is shown once. Record it and, if the
         * list overlay is open for this channel, add the new row (label unknown
         * until the next list refresh). */
        snprintf(m->webhook_token, sizeof m->webhook_token, "%s", e->body ? e->body : "");
        m->webhook_new_id = e->message_id;
        if (m->weblist_open && e->channel_id == m->weblist_channel)
            webhook_upsert(m, e->message_id, "", 0);
        {
            char buf[160];
            snprintf(buf, sizeof buf, "webhook #%llu token: %s",
                     (unsigned long long)e->message_id, m->webhook_token);
            set_status(m, buf);
        }
        break;
    case OC_EV_WEBHOOK:
        if (m->weblist_open && e->channel_id == m->weblist_channel)
            webhook_upsert(m, e->message_id, e->body ? e->body : "", e->op);
        break;
    case OC_EV_WEBHOOK_DELETED:
        webhook_remove(m, e->message_id);
        if (m->webhook_new_id == e->message_id) { m->webhook_token[0] = '\0'; m->webhook_new_id = 0; }
        set_status(m, "webhook deleted");
        break;
    case OC_EV_ATTACHMENT_DATA:
        /* Only one in flight; a second replaces the first rather than queueing,
         * because a frontend asks for what it is about to draw. */
        free(m->fetched_data);
        m->fetched_attachment = e->message_id;
        m->fetched_data = (uint8_t *)e->body;
        m->fetched_len = e->count;
        e->body = NULL;                      /* the model owns the bytes now */
        break;
    case OC_EV_ATTACHMENT:
        /* parent_id = attachment id, server_time = size, body = filename,
         * author_name = mime. Arrives right after its OC_EV_MESSAGE. */
        attach_to_msg(m, e->message_id, e->parent_id, e->body ? e->body : "",
                      e->author_name, e->server_time, e->status);
        break;
    case OC_EV_XFER:
        if (e->body) set_status(m, e->body);
        break;
    case OC_EV_READ_STATE:
        /* Cached history restored at startup is not "new": mark it read so a
         * relaunch doesn't show every replayed message as unread. Genuinely new
         * messages (backfilled past this mark) still count. */
        oc_model_mark_read(m, e->channel_id);
        break;
    case OC_EV_BACKOFF:
        m->reconnect_at_ms = e->server_time;
        break;
    case OC_EV_DISCONNECTED:
        m->connected = false;
        m->authed = false;
        set_status(m, "disconnected");
        break;
    case OC_EV_ERROR:
        if (e->body) {
            set_status(m, e->body);
            snprintf(m->last_error, sizeof m->last_error, "%s", e->body);
            m->error_seq++;
        }
        break;
    default:
        break;
    }
}

/* ---- the sidebar (WIN-5/6, see model.h) ------------------------------------ */

void oc_sidebar_opts_defaults(oc_sidebar_opts *o) {
    memset(o, 0, sizeof *o);
    for (int i = 0; i < OC_SB_SECTIONS; i++) {
        o->sort[i] = OC_SB_SORT_AZ;
        o->filter[i] = OC_SB_FILTER_ALL;
        o->collapsed[i] = 0;
    }
}

void oc_sidebar_opts_encode(const oc_sidebar_opts *o, char *out, size_t cap) {
    /* Appended, not inserted: an older client parses the "c:...;d:..." prefix with
     * sscanf and ignores the rest, so it keeps working against a bucket written by
     * this one. The starred section's own sort/filter/collapsed ride the same
     * scheme once anyone needs them; today only its collapse matters and it
     * defaults open. */
    int used = snprintf(out, cap, "c:%u,%u,%u;d:%u,%u,%u",
             o->sort[OC_SB_CHANNELS], o->filter[OC_SB_CHANNELS], o->collapsed[OC_SB_CHANNELS],
             o->sort[OC_SB_DMS],      o->filter[OC_SB_DMS],      o->collapsed[OC_SB_DMS]);
    if (used < 0 || (size_t)used >= cap) return;
    if (o->n_starred) {
        size_t at = (size_t)used;
        at += (size_t)snprintf(out + at, cap - at, ";s:");
        for (uint8_t i = 0; i < o->n_starred && at + 24 < cap; i++)
            at += (size_t)snprintf(out + at, cap - at, "%s%llu", i ? "," : "",
                                   (unsigned long long)o->starred[i]);
    }
    if ((size_t)used < cap) {
        size_t at2 = strlen(out);
        if (at2 + 8 < cap) snprintf(out + at2, cap - at2, ";sc:%u", o->collapsed[OC_SB_STARRED]);
    }
    /* Custom sections (WIN-83), one ";u:" run each: name|sort,filter,collapsed|ids.
     * Appended for the same reason as everything above — an older client parses the
     * prefix it knows and ignores this. */
    for (int i = 0; i < o->n_custom; i++) {
        size_t at = strlen(out);
        if (at + strlen(o->custom[i].name) + 24 >= cap) break;
        at += (size_t)snprintf(out + at, cap - at, ";u:%s|%u,%u,%u|", o->custom[i].name,
                               o->custom[i].sort, o->custom[i].filter, o->custom[i].collapsed);
        for (uint8_t k = 0; k < o->custom[i].n_ids && at + 24 < cap; k++)
            at += (size_t)snprintf(out + at, cap - at, "%s%llu", k ? "," : "",
                                   (unsigned long long)o->custom[i].ids[k]);
    }
}

void oc_sidebar_opts_parse(oc_sidebar_opts *o, const char *s) {
    unsigned cs, cf, cc, ds, df, dc;
    if (!s || !*s) return;
    if (sscanf(s, "c:%u,%u,%u;d:%u,%u,%u", &cs, &cf, &cc, &ds, &df, &dc) != 6) return;
    /* The starred list and its collapse are optional suffixes, so a bucket written
     * by an older client parses fine and simply has none. */
    o->n_starred = 0;
    const char *sp = strstr(s, ";s:");
    if (sp) {
        sp += 3;
        while (*sp && o->n_starred < OC_SB_STARRED_MAX) {
            char *end = NULL;
            unsigned long long v = strtoull(sp, &end, 10);
            if (!end || end == sp) break;
            if (v) o->starred[o->n_starred++] = (uint64_t)v;
            if (*end != ',') break;
            sp = end + 1;
        }
    }
    const char *cp = strstr(s, ";sc:");
    if (cp) o->collapsed[OC_SB_STARRED] = (uint8_t)(atoi(cp + 4) ? 1 : 0);
    /* Custom sections. Parsed positionally within each ";u:" run; a malformed run is
     * skipped rather than aborting the whole setting, so one bad section cannot cost
     * the user their sort and filter choices as well. */
    o->n_custom = 0;
    for (const char *up = strstr(s, ";u:"); up; up = strstr(up, ";u:")) {
        up += 3;
        if (o->n_custom >= OC_SB_CUSTOM_MAX) break;
        const char *bar = strchr(up, '|');
        if (!bar) break;
        int ci = o->n_custom;
        memset(&o->custom[ci], 0, sizeof o->custom[ci]);
        size_t nl = (size_t)(bar - up);
        if (nl >= sizeof o->custom[ci].name) nl = sizeof o->custom[ci].name - 1;
        memcpy(o->custom[ci].name, up, nl);
        o->custom[ci].name[nl] = '\0';
        unsigned cso = 0, cfl = 0, ccl = 0;
        if (sscanf(bar + 1, "%u,%u,%u", &cso, &cfl, &ccl) != 3) { up = bar + 1; continue; }
        o->custom[ci].sort      = (uint8_t)(cso <= OC_SB_SORT_UNREAD ? cso : OC_SB_SORT_AZ);
        o->custom[ci].filter    = (uint8_t)(cfl <= OC_SB_FILTER_ACTIVE ? cfl : OC_SB_FILTER_ALL);
        o->custom[ci].collapsed = (uint8_t)(ccl ? 1 : 0);
        const char *ids = strchr(bar + 1, '|');
        if (ids) {
            ids++;
            while (*ids && *ids != ';' && o->custom[ci].n_ids < OC_SB_CUSTOM_IDS) {
                char *end = NULL;
                unsigned long long v = strtoull(ids, &end, 10);
                if (!end || end == ids) break;
                if (v) o->custom[ci].ids[o->custom[ci].n_ids++] = (uint64_t)v;
                if (*end != ',') break;
                ids = end + 1;
            }
        }
        if (o->custom[ci].name[0]) o->n_custom++;
        up = bar + 1;
    }
    /* Clamp: a bucket value can come from a newer client that knows more modes. */
    o->sort[OC_SB_CHANNELS]      = (uint8_t)(cs <= OC_SB_SORT_UNREAD ? cs : OC_SB_SORT_AZ);
    o->filter[OC_SB_CHANNELS]    = (uint8_t)(cf <= OC_SB_FILTER_ACTIVE ? cf : OC_SB_FILTER_ALL);
    o->collapsed[OC_SB_CHANNELS] = (uint8_t)(cc ? 1 : 0);
    o->sort[OC_SB_DMS]           = (uint8_t)(ds <= OC_SB_SORT_UNREAD ? ds : OC_SB_SORT_AZ);
    o->filter[OC_SB_DMS]         = (uint8_t)(df <= OC_SB_FILTER_ACTIVE ? df : OC_SB_FILTER_ALL);
    o->collapsed[OC_SB_DMS]      = (uint8_t)(dc ? 1 : 0);
}

/* Render a channel's sidebar label. A DM has no name on the wire, so it is
 * titled by its peer — which is exactly why the old Win32 sidebar, which skipped
 * unnamed channels, showed no DMs at all. */
static void sb_label(const oc_model *m, const oc_channel *c, char *out, size_t cap) {
    /* A GROUP DM is titled by its people (REQ-056), the way Slack does it: names
     * rather than a made-up name, because nobody named it. EVERY participant is
     * listed, yourself included: the title states who is in the conversation, and
     * a roster that silently omits the reader disagrees with the member pane and
     * with the participant count beside it. Alphabetical, so the same group reads
     * the same way in every client and does not reshuffle when somebody posts. */
    if (c->kind == OC_CHANNEL_KIND_DM && c->n_peers > 2) {
        const char *names[9]; int nn = 0;
        for (uint16_t i = 0; i < c->n_peers && nn < 9; i++) {
            const char *nm = oc_model_user_name(m, c->peers[i]);
            names[nn++] = (nm && nm[0]) ? nm : "someone";
        }
        for (int a = 0; a < nn; a++)
            for (int b = a + 1; b < nn; b++)
                if (strcmp(names[b], names[a]) < 0) { const char *t = names[a]; names[a] = names[b]; names[b] = t; }
        size_t at = 0;
        int shown = 0;
        for (int i = 0; i < nn; i++) {
            size_t need = strlen(names[i]) + (at ? 2 : 0);
            /* Leave room for ", +N" rather than truncating a name mid-word: a
             * half-written name reads as a different person. */
            if (at && at + need + 6 >= cap) break;
            at += (size_t)snprintf(out + at, cap - at, "%s%s", at ? ", " : "", names[i]);
            shown++;
        }
        if (shown < nn && at + 8 < cap) snprintf(out + at, cap - at, ", +%d", nn - shown);
        if (!at) snprintf(out, cap, "group message");
        return;
    }
    if (c->kind == OC_CHANNEL_KIND_DM) {
        /* Always the account name, never "you": a self-DM's peer_id IS the user's
         * own id, so the ordinary lookup already yields the right name and the
         * special case only served to hide it. */
        const char *pn = oc_model_user_name(m, c->peer_id);
        snprintf(out, cap, "%s", (pn && pn[0]) ? pn : "direct message");
    } else {
        snprintf(out, cap, "%s", (c->name && c->name[0]) ? c->name : "channel");
    }
}

static void sb_lower(const char *in, char *out, size_t cap) {
    size_t i = 0;
    for (; in[i] && i < cap - 1; i++)
        out[i] = (char)(in[i] >= 'A' && in[i] <= 'Z' ? in[i] + 32 : in[i]);
    out[i] = '\0';
}

/* Sorting works over (timestamp, row) pairs so Recency has the field it needs;
 * the active mode is file-static because C99 qsort passes no context. */
typedef struct { uint64_t at; oc_sidebar_row row; } sb_tmp;
static uint8_t g_sb_sort;
static int sb_cmp(const void *va, const void *vb) {
    const sb_tmp *a = va, *b = vb;
    if (g_sb_sort == OC_SB_SORT_RECENT) {
        /* Newest first; a channel the server reports no activity for (0) sinks to
         * the bottom rather than jumbling in with the rest. */
        if (a->at != b->at) return a->at > b->at ? -1 : 1;
    } else if (g_sb_sort == OC_SB_SORT_UNREAD) {
        int au = a->row.unread > 0, bu = b->row.unread > 0;
        if (au != bu) return bu - au;
    }
    return strcmp(a->row.label, b->row.label);   /* A-Z, and the tiebreak for both */
}

int oc_sidebar_is_starred(const oc_sidebar_opts *o, uint64_t channel_id) {
    if (!o || !channel_id) return 0;
    for (uint8_t i = 0; i < o->n_starred; i++)
        if (o->starred[i] == channel_id) return 1;
    return 0;
}

int oc_sidebar_toggle_star(oc_sidebar_opts *o, uint64_t channel_id) {
    if (!o || !channel_id) return 0;
    for (uint8_t i = 0; i < o->n_starred; i++)
        if (o->starred[i] == channel_id) {
            memmove(&o->starred[i], &o->starred[i + 1],
                    (size_t)(o->n_starred - i - 1) * sizeof o->starred[0]);
            o->n_starred--;
            return 1;
        }
    if (o->n_starred >= OC_SB_STARRED_MAX) return 0;   /* full: refuse, do not evict */
    o->starred[o->n_starred++] = channel_id;
    return 1;
}

int oc_sb_custom_index(int section) {
    if (section < OC_SB_CUSTOM_BASE) return -1;
    int i = section - OC_SB_CUSTOM_BASE;
    return (i < (int)OC_SB_CUSTOM_MAX) ? i : -1;
}

uint8_t oc_sb_sort_of(const oc_sidebar_opts *o, int section) {
    if (!o) return OC_SB_SORT_AZ;
    int ci = oc_sb_custom_index(section);
    if (ci >= 0) return o->custom[ci].sort;
    return (section >= 0 && section < OC_SB_SECTIONS) ? o->sort[section] : OC_SB_SORT_AZ;
}
uint8_t oc_sb_filter_of(const oc_sidebar_opts *o, int section) {
    if (!o) return OC_SB_FILTER_ALL;
    int ci = oc_sb_custom_index(section);
    if (ci >= 0) return o->custom[ci].filter;
    return (section >= 0 && section < OC_SB_SECTIONS) ? o->filter[section] : OC_SB_FILTER_ALL;
}
uint8_t oc_sb_collapsed_of(const oc_sidebar_opts *o, int section) {
    if (!o) return 0;
    int ci = oc_sb_custom_index(section);
    if (ci >= 0) return o->custom[ci].collapsed;
    return (section >= 0 && section < OC_SB_SECTIONS) ? o->collapsed[section] : 0;
}
void oc_sb_set_sort(oc_sidebar_opts *o, int section, uint8_t v) {
    if (!o || v > OC_SB_SORT_UNREAD) return;
    int ci = oc_sb_custom_index(section);
    if (ci >= 0) { if (ci < o->n_custom) o->custom[ci].sort = v; return; }
    if (section >= 0 && section < OC_SB_SECTIONS) o->sort[section] = v;
}
void oc_sb_set_filter(oc_sidebar_opts *o, int section, uint8_t v) {
    if (!o || v > OC_SB_FILTER_ACTIVE) return;
    int ci = oc_sb_custom_index(section);
    if (ci >= 0) { if (ci < o->n_custom) o->custom[ci].filter = v; return; }
    if (section >= 0 && section < OC_SB_SECTIONS) o->filter[section] = v;
}
void oc_sb_set_collapsed(oc_sidebar_opts *o, int section, uint8_t v) {
    if (!o) return;
    v = v ? 1 : 0;
    int ci = oc_sb_custom_index(section);
    if (ci >= 0) { if (ci < o->n_custom) o->custom[ci].collapsed = v; return; }
    if (section >= 0 && section < OC_SB_SECTIONS) o->collapsed[section] = v;
}

/* Names round-trip through ONE flat setting string, so the separators that string
 * uses cannot survive inside a name. Stripped rather than rejected: the user asked
 * for a section called "Work; urgent" and should get one, not an error dialog about
 * an encoding they cannot see. */
static void sb_clean_name(const char *in, char *out, size_t cap) {
    size_t n = 0;
    for (; in && *in && n + 1 < cap; in++) {
        unsigned char c = (unsigned char)*in;
        if (c < 0x20 || c == ';' || c == ':' || c == ',' || c == '|') continue;
        out[n++] = (char)c;
    }
    /* Trailing spaces would make two sections look identical. */
    while (n && out[n - 1] == ' ') n--;
    out[n] = '\0';
}

int oc_sidebar_section_add(oc_sidebar_opts *o, const char *name) {
    if (!o || o->n_custom >= OC_SB_CUSTOM_MAX) return -1;
    char clean[OC_SB_NAME_MAX];
    sb_clean_name(name, clean, sizeof clean);
    if (!clean[0]) return -1;
    int i = o->n_custom++;
    memset(&o->custom[i], 0, sizeof o->custom[i]);
    snprintf(o->custom[i].name, sizeof o->custom[i].name, "%s", clean);
    return i;
}

void oc_sidebar_section_rename(oc_sidebar_opts *o, int idx, const char *name) {
    if (!o || idx < 0 || idx >= o->n_custom) return;
    char clean[OC_SB_NAME_MAX];
    sb_clean_name(name, clean, sizeof clean);
    if (!clean[0]) return;                 /* a nameless section is unclickable */
    snprintf(o->custom[idx].name, sizeof o->custom[idx].name, "%s", clean);
}

void oc_sidebar_section_remove(oc_sidebar_opts *o, int idx) {
    if (!o || idx < 0 || idx >= o->n_custom) return;
    for (int i = idx; i + 1 < o->n_custom; i++) o->custom[i] = o->custom[i + 1];
    o->n_custom--;
    memset(&o->custom[o->n_custom], 0, sizeof o->custom[0]);
}

int oc_sidebar_section_of(const oc_sidebar_opts *o, uint64_t channel_id) {
    if (!o || !channel_id) return -1;
    for (int i = 0; i < o->n_custom; i++)
        for (uint8_t k = 0; k < o->custom[i].n_ids; k++)
            if (o->custom[i].ids[k] == channel_id) return i;
    return -1;
}

int oc_sidebar_assign(oc_sidebar_opts *o, uint64_t channel_id, int idx) {
    if (!o || !channel_id) return 0;
    int changed = 0, cur = oc_sidebar_section_of(o, channel_id);
    if (cur == idx) return 0;
    if (cur >= 0) {                        /* out of the old one first: at most one */
        for (uint8_t k = 0; k < o->custom[cur].n_ids; k++)
            if (o->custom[cur].ids[k] == channel_id) {
                memmove(&o->custom[cur].ids[k], &o->custom[cur].ids[k + 1],
                        (size_t)(o->custom[cur].n_ids - k - 1) * sizeof o->custom[cur].ids[0]);
                o->custom[cur].n_ids--;
                changed = 1;
                break;
            }
    }
    if (idx < 0 || idx >= o->n_custom) return changed;
    if (o->custom[idx].n_ids >= OC_SB_CUSTOM_IDS) return changed;   /* full: refuse */
    o->custom[idx].ids[o->custom[idx].n_ids++] = channel_id;
    return 1;
}

uint64_t oc_model_custom_emoji(const oc_model *m, const char *name) {
    if (!m || !name || !name[0]) return 0;
    for (size_t i = 0; i < m->n_cemoji; i++)
        if (strcmp(m->cemoji[i].name, name) == 0) return m->cemoji[i].attachment_id;
    return 0;
}

void oc_model_dm_title(const oc_model *m, const oc_channel *c, char *out, size_t cap) {
    if (!m || !c || !out || !cap) return;
    sb_label(m, c, out, cap);
}

size_t oc_model_sidebar(const oc_model *m, const oc_sidebar_opts *o,
                        oc_sidebar_row *out, size_t cap) {
    if (!m || !o || !out || cap == 0) return 0;
    size_t n = 0;

    /* STARRED first on screen, then Channels, then DMs — Slack's order. The enum
     * numbers the sections for storage; this array decides what the eye sees, so
     * adding a section does not renumber anyone's saved preferences. */
    /* Starred, then the user's own sections in their order, then Channels, then
     * DMs. Built as a list rather than a fixed array because the middle is now
     * variable-length (WIN-83). */
    int order[OC_SB_SECTIONS + OC_SB_CUSTOM_MAX];
    int n_order = 0;
    order[n_order++] = OC_SB_STARRED;
    for (int i = 0; i < o->n_custom; i++) order[n_order++] = OC_SB_CUSTOM_BASE + i;
    order[n_order++] = OC_SB_CHANNELS;
    order[n_order++] = OC_SB_DMS;

    for (int oi = 0; oi < n_order; oi++) {
        int sec = order[oi];
        int cidx = oc_sb_custom_index(sec);
        /* Collect this section's rows, with the timestamp alongside for sorting. */
        sb_tmp *tmp = calloc(m->n_channels + 1, sizeof *tmp);
        size_t tn = 0, total = 0;
        for (size_t i = 0; tmp && i < m->n_channels; i++) {
            const oc_channel *c = &m->channels[i];
            int is_dm = (c->kind == OC_CHANNEL_KIND_DM);
            int starred = oc_sidebar_is_starred(o, c->channel_id);
            int inbox = oc_sidebar_section_of(o, c->channel_id);   /* custom, or -1 */
            /* A conversation appears ONCE. Starred lifts it out of everything; a
             * custom section lifts it out of Channels/DMs. Starred wins when it is
             * in both, because two lift-it-out rules need a precedence. */
            if (sec == OC_SB_STARRED) {
                if (!starred) continue;
            } else if (cidx >= 0) {
                if (starred || inbox != cidx) continue;
            } else {
                if (starred || inbox >= 0) continue;
                if ((sec == OC_SB_DMS) != is_dm) continue;
            }
            /* A named channel the user has not joined is browsable, not listed. */
            if (!is_dm && (!c->name || !c->name[0])) continue;
            total++;

            char label[96];
            sb_label(m, c, label, sizeof label);

            if (o->find[0]) {                     /* match the LABEL, not the name */
                char low[96]; sb_lower(label, low, sizeof low);
                if (!strstr(low, o->find)) continue;
            }
            if (oc_sb_filter_of(o, sec) == OC_SB_FILTER_UNREAD && c->unread <= 0) continue;
            if (oc_sb_filter_of(o, sec) == OC_SB_FILTER_ACTIVE) {
                /* Channels: joined ones. DMs: a peer who is not offline.
                 *
                 * A GROUP DM (REQ-056) has no single peer — peer_id is 0 — so the
                 * 1:1 test read it as "offline" and the filter hid every group
                 * conversation. "Active" means somebody in it is around, so for a
                 * group that is ANY participant but you. */
                if (is_dm && c->n_peers > 2) {
                    int any = 0;
                    for (uint16_t k = 0; k < c->n_peers && !any; k++) {
                        if (c->peers[k] == m->user_id) continue;
                        if (oc_model_presence_of(m, c->peers[k]) != OC_PRESENCE_OFFLINE) any = 1;
                    }
                    if (!any) continue;
                } else if (is_dm) {
                    if (oc_model_presence_of(m, c->peer_id) == OC_PRESENCE_OFFLINE) continue;
                } else if (!c->joined) continue;
            }

            tmp[tn].at = c->last_message_at;
            tmp[tn].row.is_header  = 0;
            tmp[tn].row.section    = (uint8_t)sec;
            tmp[tn].row.channel_id = c->channel_id;
            snprintf(tmp[tn].row.label, sizeof tmp[tn].row.label, "%s", label);
            tmp[tn].row.is_private = (uint8_t)(!is_dm && !c->is_public);
            /* Slack renders the self-DM as "Danny Heskett  you" — the account
             * name, with a dimmed tag after it. The core flags it; each frontend
             * draws the tag in its own dim style. */
            tmp[tn].row.is_self    = (uint8_t)(is_dm && c->peer_id == m->user_id);
            tmp[tn].row.peer_id    = is_dm ? c->peer_id : 0;
            tmp[tn].row.joined     = c->joined;
            tmp[tn].row.unread     = c->unread;
            tn++;
        }

        if (tn > 1) { g_sb_sort = oc_sb_sort_of(o, sec); qsort(tmp, tn, sizeof *tmp, sb_cmp); }

        /* Header first, always — a collapsed or empty section must stay openable. */
        if (n < cap) {
            oc_sidebar_row *h = &out[n++];
            memset(h, 0, sizeof *h);
            h->is_header = 1;
            h->section = (uint8_t)sec;
            snprintf(h->label, sizeof h->label, "%s",
                     cidx >= 0             ? o->custom[cidx].name :
                     sec == OC_SB_STARRED  ? "Starred" :
                     sec == OC_SB_CHANNELS ? "Channels" : "Direct messages");
            h->section_total = (int)total;
        }
        if (!oc_sb_collapsed_of(o, sec))
            for (size_t i = 0; i < tn && n < cap; i++) out[n++] = tmp[i].row;
        free(tmp);
    }
    return n;
}
