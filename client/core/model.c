/*
 * OpenChime client — the view-model + reducers (ARCH-74). See model.h.
 */

#include "model.h"

#include "protocol.h"   /* OC_PRESENCE_OFFLINE */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define OC_TYPING_TIMEOUT 6   /* seconds a typing mark stays live without refresh */

void oc_model_init(oc_model *m) {
    memset(m, 0, sizeof *m);
}

static void msg_free(oc_msg *m) { free(m->body); free(m->reactions); free(m->attach); }

static void channel_free(oc_channel *c) {
    for (size_t i = 0; i < c->n_msgs; i++) msg_free(&c->msgs[i]);
    free(c->msgs);
    free(c->name);
    free(c->readers);
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

void oc_model_free(oc_model *m) {
    for (size_t i = 0; i < m->n_channels; i++) channel_free(&m->channels[i]);
    free(m->channels);
    free(m->presence);
    free(m->typing);
    for (size_t i = 0; i < m->n_thread_msgs; i++) msg_free(&m->thread_msgs[i]);
    free(m->thread_msgs);
    for (size_t i = 0; i < m->n_search; i++) free(m->search_results[i].snippet);
    free(m->search_results);
    free(m->reactors);
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
static void user_upsert(oc_model *m, uint64_t user_id, const char *name, uint8_t role, uint8_t disabled) {
    for (size_t i = 0; i < m->n_users; i++)
        if (m->users[i].user_id == user_id) {
            snprintf(m->users[i].name, sizeof m->users[i].name, "%s", name ? name : "");
            m->users[i].role = role; m->users[i].disabled = disabled; return;
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
    snprintf(u->name, sizeof u->name, "%s", name ? name : "");
}

/* Update a roster member's role/disabled without disturbing its name (a
 * USER_UPDATED carries no name); upsert with an empty name if unseen. */
static void user_update_role(oc_model *m, uint64_t user_id, uint8_t role, uint8_t disabled) {
    for (size_t i = 0; i < m->n_users; i++)
        if (m->users[i].user_id == user_id) {
            m->users[i].role = role; m->users[i].disabled = disabled; return;
        }
    user_upsert(m, user_id, "", role, disabled);
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

void oc_model_close_weblist(oc_model *m) {
    free(m->webhooks);
    m->webhooks = NULL;
    m->n_webhooks = m->cap_webhooks = 0;
    m->weblist_open = 0;
    m->weblist_channel = 0;
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
     * server did not assign one (shouldn't happen for a BROADCAST) — keep it. */
    if (message_id && message_id <= c->high_water) return 0;
    if (c->n_msgs == c->cap_msgs) {
        size_t cap = c->cap_msgs ? c->cap_msgs * 2 : 32;
        oc_msg *nm = realloc(c->msgs, cap * sizeof *nm);
        if (!nm) return 0;
        c->msgs = nm;
        c->cap_msgs = cap;
    }
    oc_msg *msg = &c->msgs[c->n_msgs++];
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
            if (e->server_time) c->last_message_at = e->server_time;
            if (e->count) {
                c->srv_unread = e->count;
                /* Before any backfill the server's count is all we know, so let
                 * it seed the badge; live BROADCASTs take over from there. */
                if (c->unread == 0) c->unread = (int)e->count;
            }
            if (e->body) { free(c->name); c->name = e->body; e->body = NULL; }
        }
        break;
    }
    case OC_EV_MESSAGE: {
        oc_channel *c = channel_ensure(m, e->channel_id);
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
                if (c->msgs[i].message_id == e->message_id) { c->msgs[i].deleted = 1; break; }
            }
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
        if (m->search_open)
            search_append(m, e->message_id, e->channel_id, e->author_id, e->server_time, &e->body);
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
        for (size_t i = 0; i < m->n_channels; i++) m->channels[i].notify_level = OC_NOTIFY_ALL;
        m->dnd_enabled   = e->status;
        m->dnd_start_min = (uint16_t)(e->count >> 16);
        m->dnd_end_min   = (uint16_t)(e->count & 0xFFFF);
        break;
    case OC_EV_NOTIFY_PREF: {
        oc_channel *c = oc_model_channel(m, e->channel_id);
        if (c) c->notify_level = e->op;
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
        user_upsert(m, e->user_id, e->body ? e->body : "", e->status, e->op);
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
    case OC_EV_DISCONNECTED:
        m->connected = false;
        m->authed = false;
        set_status(m, "disconnected");
        break;
    case OC_EV_ERROR:
        if (e->body) {
            set_status(m, e->body);
            snprintf(m->last_error, sizeof m->last_error, "%s", e->body);
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
    snprintf(out, cap, "c:%u,%u,%u;d:%u,%u,%u",
             o->sort[OC_SB_CHANNELS], o->filter[OC_SB_CHANNELS], o->collapsed[OC_SB_CHANNELS],
             o->sort[OC_SB_DMS],      o->filter[OC_SB_DMS],      o->collapsed[OC_SB_DMS]);
}

void oc_sidebar_opts_parse(oc_sidebar_opts *o, const char *s) {
    unsigned cs, cf, cc, ds, df, dc;
    if (!s || !*s) return;
    if (sscanf(s, "c:%u,%u,%u;d:%u,%u,%u", &cs, &cf, &cc, &ds, &df, &dc) != 6) return;
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
    if (c->kind == OC_CHANNEL_KIND_DM) {
        const char *pn = (c->peer_id == m->user_id) ? "you" : oc_model_user_name(m, c->peer_id);
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

size_t oc_model_sidebar(const oc_model *m, const oc_sidebar_opts *o,
                        oc_sidebar_row *out, size_t cap) {
    if (!m || !o || !out || cap == 0) return 0;
    size_t n = 0;

    for (int sec = 0; sec < OC_SB_SECTIONS; sec++) {
        /* Collect this section's rows, with the timestamp alongside for sorting. */
        sb_tmp *tmp = calloc(m->n_channels + 1, sizeof *tmp);
        size_t tn = 0, total = 0;
        for (size_t i = 0; tmp && i < m->n_channels; i++) {
            const oc_channel *c = &m->channels[i];
            int is_dm = (c->kind == OC_CHANNEL_KIND_DM);
            if ((sec == OC_SB_DMS) != is_dm) continue;
            /* A named channel the user has not joined is browsable, not listed. */
            if (!is_dm && (!c->name || !c->name[0])) continue;
            total++;

            char label[96];
            sb_label(m, c, label, sizeof label);

            if (o->find[0]) {                     /* match the LABEL, not the name */
                char low[96]; sb_lower(label, low, sizeof low);
                if (!strstr(low, o->find)) continue;
            }
            if (o->filter[sec] == OC_SB_FILTER_UNREAD && c->unread <= 0) continue;
            if (o->filter[sec] == OC_SB_FILTER_ACTIVE) {
                /* Channels: joined ones. DMs: a peer who is not offline. */
                if (is_dm) {
                    if (oc_model_presence_of(m, c->peer_id) == OC_PRESENCE_OFFLINE) continue;
                } else if (!c->joined) continue;
            }

            tmp[tn].at = c->last_message_at;
            tmp[tn].row.is_header  = 0;
            tmp[tn].row.section    = (uint8_t)sec;
            tmp[tn].row.channel_id = c->channel_id;
            snprintf(tmp[tn].row.label, sizeof tmp[tn].row.label, "%s", label);
            tmp[tn].row.is_private = (uint8_t)(!is_dm && !c->is_public);
            tmp[tn].row.joined     = c->joined;
            tmp[tn].row.unread     = c->unread;
            tn++;
        }

        if (tn > 1) { g_sb_sort = o->sort[sec]; qsort(tmp, tn, sizeof *tmp, sb_cmp); }

        /* Header first, always — a collapsed or empty section must stay openable. */
        if (n < cap) {
            oc_sidebar_row *h = &out[n++];
            memset(h, 0, sizeof *h);
            h->is_header = 1;
            h->section = (uint8_t)sec;
            snprintf(h->label, sizeof h->label, "%s",
                     sec == OC_SB_CHANNELS ? "Channels" : "Direct messages");
            h->section_total = (int)total;
        }
        if (!o->collapsed[sec])
            for (size_t i = 0; i < tn && n < cap; i++) out[n++] = tmp[i].row;
        free(tmp);
    }
    return n;
}
