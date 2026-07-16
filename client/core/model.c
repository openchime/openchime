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
    for (size_t i = 0; i < c->n_msgs; i++) free(c->msgs[i].body);
    free(c->msgs);
    free(c->name);
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

/* Append a message, stealing ownership of `*body` (set to NULL on success). */
static void channel_append(oc_channel *c, uint64_t author_id, uint64_t message_id,
                           uint64_t server_time, char **body) {
    /* Dedup on the per-channel high-water mark (ARCH-45). message_id 0 means the
     * server did not assign one (shouldn't happen for a BROADCAST) — keep it. */
    if (message_id && message_id <= c->high_water) return;
    if (c->n_msgs == c->cap_msgs) {
        size_t cap = c->cap_msgs ? c->cap_msgs * 2 : 32;
        oc_msg *nm = realloc(c->msgs, cap * sizeof *nm);
        if (!nm) return;
        c->msgs = nm;
        c->cap_msgs = cap;
    }
    oc_msg *msg = &c->msgs[c->n_msgs++];
    msg->body = *body;
    msg->author_id = author_id;
    msg->message_id = message_id;
    msg->server_time = server_time;
    *body = NULL;
    if (message_id > c->high_water) c->high_water = message_id;
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
        if (c) channel_append(c, e->author_id, e->message_id, e->server_time, &e->body);
        break;
    }
    case OC_EV_PRESENCE:
        presence_set(m, e->user_id, e->status);
        break;
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
