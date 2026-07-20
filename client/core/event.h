/*
 * OpenChime client — the events/commands crossing the UI↔net queues (ARCH-62).
 * net→UI events report connection + protocol state; UI→net commands carry user
 * actions. Variable-length bodies are heap-owned by the struct.
 */

#ifndef OC_EVENT_H
#define OC_EVENT_H

#include <stdint.h>

/* Storage usage, policy, and what maintenance has reclaimed (REQ-214/215),
 * carried on OC_EV_STORAGE and folded into the model for the frontend to
 * render. Owner/admin only — the daemon refuses the request otherwise. */
typedef struct {
    uint64_t total_bytes, avail_bytes, attach_bytes, attach_count;
    uint64_t rec_orphan, rec_expired, rec_evicted, last_reclaim_ms;
    uint64_t max_age_days, reserve_bytes;
    uint8_t  evict_enabled, under_pressure;
} oc_storage_view;

/* One audit entry as the frontend sees it (REQ-251). Fixed-size strings: a page
 * is bounded and short-lived, so this avoids per-entry heap churn. */
typedef struct {
    uint64_t at_ms;
    uint64_t actor_id;
    uint64_t target_id;
    char     actor_name[64];
    char     action[48];
    char     target[64];
    char     detail[96];
    uint8_t  family;    /* 1 admin, 2 account, 3 security, 4 moderation */
    uint8_t  outcome;   /* 1 ok, 0 denied/failed */
} oc_audit_view;

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
    OC_EV_REACTIONS,       /* a REACTIONS entry: message_id + one reactor (user_id + emoji) */
    OC_EV_DND,             /* a NOTIFY_PREFS header (frame start): status=enabled, count=(start<<16|end) */
    OC_EV_NOTIFY_PREF,     /* a NOTIFY_PREFS entry: channel_id + level(op) */
    OC_EV_USER_UPDATED,    /* a USER_UPDATED: user_id + role(status) + disabled(op) */
    OC_EV_INVITE,          /* an INVITE_CREATED: body=token, op=role, server_time=expires_at */
    OC_EV_USER,            /* a USER_LIST entry: user_id + name(body) + role(status) + disabled(op) */
    OC_EV_WEBHOOK_INFO,    /* a WEBHOOK_INFO: message_id=webhook_id + channel_id + body=token (shown once) */
    OC_EV_WEBHOOK,         /* a WEBHOOK_LIST entry: message_id=webhook_id + channel_id + body=label + op=disabled */
    OC_EV_WEBHOOK_DELETED, /* a WEBHOOK_DELETED: message_id=webhook_id */
    OC_EV_ATTACHMENT,      /* a message's attachment: channel_id + message_id + parent_id=attachment_id + server_time=size + body=filename + author_name=mime */
    OC_EV_XFER,            /* a transfer notice: op=phase (0 progress, 1 done, 2 error), body=status text */
    OC_EV_READ_STATE,      /* channel_id: mark its currently-loaded messages read (replayed cache is not "unread") */
    OC_EV_SETTINGS_BEGIN,  /* a CLIENT_SETTINGS frame start: clears the synced bucket before its entries */
    OC_EV_SETTING,         /* one synced setting: author_name=key, body=value */
    OC_EV_PROFILE,         /* a PROFILE_UPDATED: user_id + body=display_name (own = the change ack) */
    OC_EV_READ_CURSOR,     /* a READ_CURSOR: user_id read up to message_id in channel_id (seen-by) */
    OC_EV_STORAGE,         /* a STORAGE_STATUS: usage + policy report (REQ-214) */
    OC_EV_AUDIT_BEGIN,     /* an AUDIT_PAGE starts: clears the model's page */
    OC_EV_AUDIT,           /* one audit entry (REQ-251) */
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
    uint8_t  is_public;    /* CHANNEL: 1 public, 0 private/DM */
    oc_storage_view storage;  /* OC_EV_STORAGE */
    oc_audit_view   audit;    /* OC_EV_AUDIT */
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
    OC_CMD_LIST_REACTIONS,  /* inspect who reacted to `message_id` in `channel_id` */
    OC_CMD_SET_NOTIFY_PREF, /* set `channel_id`'s notification level (op = level) */
    OC_CMD_SET_DND,         /* set the DND window: op = enabled, channel_id = start_min, message_id = end_min */
    OC_CMD_LIST_NOTIFY_PREFS, /* request all notification settings (DND + per-channel) */
    OC_CMD_SET_SETTING,     /* upsert a synced client setting: body=key, body2=value (empty value deletes) */
    OC_CMD_LIST_SETTINGS,   /* request the synced client-settings bucket */
    OC_CMD_SET_DISPLAY_NAME, /* change your own display name: body=name */
    OC_CMD_CHANGE_PASSWORD, /* change your own password: body=old, body2=new */
    OC_CMD_MARK_READ,       /* CLIENT_ACK: read `channel_id` up to `message_id` (drives seen-by) */
    OC_CMD_STORAGE_STATUS,  /* ask for the storage usage report (owner/admin) */
    OC_CMD_AUDIT_QUERY,     /* page the audit log (owner/admin): message_id = before_ms */
    OC_CMD_SET_ROLE,        /* set a user's tenant role: channel_id = user_id, op = role */
    OC_CMD_INVITE_USER,     /* mint a tenant invite token: op = role */
    OC_CMD_REMOVE_USER,     /* remove/disable a user: channel_id = user_id */
    OC_CMD_CREATE_WEBHOOK,  /* mint an incoming webhook for `channel_id`: body = label */
    OC_CMD_LIST_WEBHOOKS,   /* list `channel_id`'s webhooks */
    OC_CMD_DELETE_WEBHOOK,  /* delete a webhook: message_id = webhook_id */
    OC_CMD_UPLOAD,          /* upload+post a file to `channel_id`: body = local path */
    OC_CMD_DOWNLOAD,        /* download an attachment: message_id = attachment_id, body = dest path */
    OC_CMD_LOGOUT,          /* revoke this session (op = scope) and close the connection */
    OC_CMD_QUIT
};

typedef struct {
    int      type;
    uint64_t channel_id;
    uint64_t message_id;   /* REACT */
    uint8_t  op;           /* REACT: add/remove */
    char    *body;         /* heap; SEND body / REACT emoji / SET_SETTING key */
    char    *body2;        /* heap; SET_SETTING value, else NULL */
} oc_cmd;

oc_cmd *oc_cmd_new(int type);
void    oc_cmd_free(oc_cmd *c);

#endif /* OC_EVENT_H */
