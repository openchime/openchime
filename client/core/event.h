/*
 * OpenChime client — the events/commands crossing the UI↔net queues (ARCH-62).
 * net→UI events report connection + protocol state; UI→net commands carry user
 * actions. Variable-length bodies are heap-owned by the struct.
 */

#ifndef OC_EVENT_H
#define OC_EVENT_H

#include <stdint.h>

/* net thread -> UI thread */
enum {
    OC_EV_CONNECTED = 1,   /* TLS + handshake up */
    OC_EV_AUTH_OK,         /* authenticated; user_id set */
    OC_EV_MESSAGE,         /* a BROADCAST: channel/author/message_id/time + body */
    OC_EV_CHANNEL,         /* a CHANNEL_LIST entry: channel_id + name(body) + status */
    OC_EV_PRESENCE,        /* a PRESENCE_UPDATE: user_id + status */
    OC_EV_REACTION,        /* a REACTION_UPDATED: channel/message/user + emoji/op/count */
    OC_EV_EDIT,            /* a MSG_EDITED: channel/message + new body */
    OC_EV_DELETE,          /* a MSG_DELETED: channel/message tombstone */
    OC_EV_TYPING,          /* a TYPING_UPDATE: user_id is typing in channel_id */
    OC_EV_THREAD_REPLY,    /* a THREAD_REPLY: parent_id/message + body + count */
    OC_EV_THREAD_META,     /* a THREAD_META: message_id + reply count (backfill) */
    OC_EV_SEARCH_RESULT,   /* a SEARCH_RESULTS entry: channel/message/author + snippet(body) */
    OC_EV_USER,            /* a USER_LIST entry: user_id + name(body) + role(status) + disabled(op) */
    OC_EV_DISCONNECTED,    /* connection dropped/closed */
    OC_EV_ERROR            /* protocol/transport error; body = human message */
};

typedef struct {
    int      type;
    uint64_t user_id;
    uint64_t channel_id;
    uint64_t author_id;
    uint64_t message_id;
    uint64_t parent_id;    /* THREAD_REPLY: the parent message this replies to */
    uint64_t server_time;
    uint8_t  status;       /* PRESENCE: online/away/offline; CHANNEL: joined flag */
    uint8_t  op;           /* REACTION: add/remove */
    uint32_t count;        /* REACTION: running aggregate count for the emoji */
    char     emoji[40];    /* REACTION: the emoji */
    char     author_name[64]; /* MESSAGE: author display name ("" = fall back to id) */
    char    *body;         /* heap; MESSAGE/ERROR/CHANNEL(name) only, else NULL */
} oc_ev;

oc_ev *oc_ev_new(int type);
void   oc_ev_free(oc_ev *e);

/* UI thread -> net thread */
enum {
    OC_CMD_SEND = 1,       /* send `body` to `channel_id` */
    OC_CMD_BACKFILL,       /* request history for `channel_id` (from id 0) */
    OC_CMD_REACT,          /* react to `message_id` in `channel_id`: body=emoji, op */
    OC_CMD_EDIT,           /* edit `message_id` in `channel_id`: body=new text */
    OC_CMD_DELETE,         /* delete `message_id` in `channel_id` */
    OC_CMD_TYPING,         /* signal "I am typing" in `channel_id` */
    OC_CMD_OPEN_THREAD,    /* request a thread's replies: `message_id` = parent */
    OC_CMD_REPLY,          /* reply in a thread: `message_id` = parent, body=text */
    OC_CMD_SEARCH,         /* full-text search: body = query */
    OC_CMD_CREATE_CHANNEL,  /* create a public channel: body = name */
    OC_CMD_JOIN_CHANNEL,    /* join `channel_id` */
    OC_CMD_LEAVE_CHANNEL,   /* leave `channel_id` */
    OC_CMD_LIST_CHANNELS,   /* refresh the channel list (discover new channels) */
    OC_CMD_LIST_USERS,      /* request the tenant roster */
    OC_CMD_SET_PRESENCE,    /* set own presence: op = OC_PRESENCE_ONLINE / _AWAY */
    OC_CMD_OPEN_DM,         /* open/get a 1:1 DM with `channel_id` (reused as target user id) */
    OC_CMD_LOGOUT,          /* revoke this session (op = scope) and close the connection */
    OC_CMD_QUIT
};

typedef struct {
    int      type;
    uint64_t channel_id;
    uint64_t message_id;   /* REACT */
    uint8_t  op;           /* REACT: add/remove */
    char    *body;         /* heap; SEND body / REACT emoji */
} oc_cmd;

oc_cmd *oc_cmd_new(int type);
void    oc_cmd_free(oc_cmd *c);

#endif /* OC_EVENT_H */
