/*
 * OpenChime client — the view-model + reducers (ARCH-74). See model.h.
 */

#include "model.h"

#include "protocol.h"   /* OC_PRESENCE_OFFLINE */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void oc_model_init(oc_model *m) {
    memset(m, 0, sizeof *m);
}

static void channel_free(oc_channel *c) {
    for (size_t i = 0; i < c->n_msgs; i++) { free(c->msgs[i].body); free(c->msgs[i].reactions); }
    free(c->msgs);
    free(c->name);
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
    memset(m, 0, sizeof *m);
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
        set_status(m, "connected");
        break;
    case OC_EV_AUTH_OK:
        m->authed = true;
        m->user_id = e->user_id;
        set_status(m, "authenticated");
        break;
    case OC_EV_CHANNEL: {
        oc_channel *c = channel_ensure(m, e->channel_id);
        if (c) {
            c->joined = e->status;
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
    case OC_EV_DISCONNECTED:
        m->connected = false;
        m->authed = false;
        set_status(m, "disconnected");
        break;
    case OC_EV_ERROR:
        if (e->body) set_status(m, e->body);
        break;
    default:
        break;
    }
}
