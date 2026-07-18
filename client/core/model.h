/*
 * OpenChime client — the view-model + reducers (ARCH-74). Frontend-agnostic C:
 * a frontend owns an oc_model, folds net→UI events into it each tick with
 * oc_model_apply, and renders it. All logic/state lives here so a frontend is
 * pure view + input. Single-threaded on the frontend, so no locking.
 */

#ifndef OC_MODEL_H
#define OC_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "event.h"

/* One emoji's aggregate on a message: the running count and whether we reacted. */
typedef struct { char emoji[40]; uint32_t count; uint8_t mine; } oc_reaction;

/* One synced client setting (the daemon-side config bucket). key/value sizes
 * mirror OC_SETTING_KEY_MAX / OC_SETTING_VALUE_MAX (+1 for NUL). */
typedef struct { char key[65]; char value[513]; } oc_setting;

/* One member's read cursor in a channel (REQ-090 seen-by). */
typedef struct { uint64_t user_id; uint64_t message_id; } oc_read_cursor_view;

/* One attachment hanging off a message (REQ-140): the server id (used to
 * download it), its filename + mime, and the byte size. */
typedef struct { uint64_t id; char filename[128]; char mime[64]; uint64_t size; } oc_attachment;

typedef struct {
    char    *body;         /* heap */
    char     author_name[64]; /* author display name ("" = fall back to id) */
    uint64_t author_id;
    uint64_t message_id;
    uint64_t server_time;
    oc_reaction *reactions;   /* heap, NULL until the message gets a reaction */
    uint8_t      n_reactions, cap_reactions;
    oc_attachment *attach;    /* heap, NULL until the message carries an attachment */
    uint8_t      n_attach, cap_attach;
    uint8_t      edited;      /* body was edited (REQ-051) */
    uint8_t      deleted;     /* tombstoned (REQ-052) */
    uint32_t     reply_count; /* thread replies to this message (REQ-060) */
} oc_msg;

typedef struct {
    uint64_t channel_id;
    char    *name;         /* heap; NULL until a CHANNEL_LIST entry names it */
    oc_msg  *msgs;
    size_t   n_msgs, cap_msgs;
    uint64_t high_water;   /* dedup mark: ignore message_id <= this (ARCH-45) */
    uint64_t read_marker;  /* high_water as of the last mark-read; drives unread */
    int      unread;       /* messages from others since the last mark-read */
    uint8_t  joined;
    uint8_t  history_requested; /* a backfill has been asked for (once per open) */
    uint8_t  kind;             /* OC_CHANNEL_KIND / _DM */
    uint64_t peer_id;          /* DM: the other participant (for the title) */
    uint8_t  notify_level;     /* OC_NOTIFY_ALL/_MENTIONS/_NONE (REQ-130) */
    /* Per-member read cursors (REQ-090 seen-by): the highest message id each
     * member has read in this channel. Advance-only; drives "seen by …". */
    oc_read_cursor_view *readers;
    size_t   n_readers, cap_readers;
} oc_channel;

typedef struct { uint64_t user_id; uint8_t status; } oc_presence_row;

/* A tenant roster entry (from LIST_USERS): id, display name, role, disabled. */
typedef struct { uint64_t user_id; char name[64]; uint8_t role; uint8_t disabled; } oc_member;

/* An ephemeral "user is typing in channel" mark, expiring on a timeout. */
typedef struct { uint64_t channel_id; uint64_t user_id; long long seen; } oc_typing_row;

/* One full-text search hit (REQ-080). */
typedef struct { uint64_t message_id, channel_id, author_id, server_time; char *snippet; } oc_search_result;

/* One reactor entry (from LIST_REACTIONS -> REACTIONS, REQ-071): who reacted
 * with which emoji on the inspected message. */
typedef struct { uint64_t user_id; char emoji[40]; } oc_reactor_row;

/* One incoming-webhook entry (from LIST_WEBHOOKS -> WEBHOOK_LIST, REQ-170): the
 * webhook's id + label + disabled flag. Tokens are never listed (shown once at
 * creation, in webhook_token). */
typedef struct { uint64_t webhook_id; char label[64]; uint8_t disabled; } oc_webhook_view;

typedef struct {
    bool     connected;
    bool     authed;
    uint64_t user_id;                 /* our own id, from AUTH_OK */
    oc_channel      *channels;
    size_t           n_channels, cap_channels;
    oc_presence_row *presence;
    size_t           n_presence, cap_presence;
    oc_typing_row   *typing;
    size_t           n_typing, cap_typing;
    /* The open thread (REQ-060): the parent's channel + id, and its replies. A
     * frontend opens a thread, gets replies streamed in, and renders parent +
     * replies in place of the channel; thread_open is 0 when no thread is open. */
    uint8_t   thread_open;
    uint64_t  thread_channel, thread_parent;
    oc_msg   *thread_msgs;
    size_t    n_thread_msgs, cap_thread_msgs;
    /* The open search view (REQ-080): the query and its hits. */
    uint8_t   search_open;
    char      search_query[128];
    oc_search_result *search_results;
    size_t    n_search, cap_search;
    /* The open "who reacted" view (REQ-071): the inspected message + its full
     * reactor list. reactlist_open is 0 when no such overlay is open. */
    uint8_t   reactlist_open;
    uint64_t  reactlist_message;
    oc_reactor_row *reactors;
    size_t    n_reactors, cap_reactors;
    /* The tenant roster (REQ, LIST_USERS) + whether the roster view is open. */
    uint8_t   roster_open;
    oc_member *users;
    size_t    n_users, cap_users;
    /* Notification preferences (REQ-130/131): the DND window (minutes since
     * midnight, local) + whether the prefs overlay is open. Per-channel levels
     * live on each oc_channel.notify_level. A NOTIFY_PREFS frame is a full sync. */
    uint8_t   prefs_open;
    uint8_t   dnd_enabled;
    uint16_t  dnd_start_min, dnd_end_min;
    /* The last invite token minted this session (REQ-033, shown once): the
     * token is empty until an INVITE_CREATED arrives. */
    char      invite_token[96];
    uint8_t   invite_role;
    uint64_t  invite_expires;
    /* The incoming-webhook overlay (REQ-170): the channel it lists, its webhooks,
     * and the last-minted token (shown once, empty until a WEBHOOK_INFO arrives).
     * weblist_open is 0 when no such overlay is open. */
    uint8_t   weblist_open;
    uint64_t  weblist_channel;
    oc_webhook_view *webhooks;
    size_t    n_webhooks, cap_webhooks;
    char      webhook_token[80];
    uint64_t  webhook_new_id;
    /* The synced client-settings bucket (the daemon-side config layer). A
     * CLIENT_SETTINGS frame — solicited or a device-sync push — replaces it
     * wholesale (OC_EV_SETTINGS_BEGIN clears, OC_EV_SETTING entries refill). The
     * frontend reads values with oc_model_setting and layers them over its
     * machine-local file defaults. */
    oc_setting *settings;
    size_t    n_settings, cap_settings;
    uint8_t   settings_synced;        /* a snapshot has arrived at least once */
    char     status[160];             /* last status / error line */
    /* The last hard error (auth failed, unreachable, …). Unlike `status` it is
     * NOT overwritten by the "disconnected" line, so a login flow can read the
     * reason after the connection drops; cleared on a successful connect. */
    char     last_error[160];
} oc_model;

void oc_model_init(oc_model *m);
void oc_model_free(oc_model *m);

/* Fold one net event into the model. Takes ownership of `e->body` (moves it into
 * the model and clears it); the caller still frees the event struct itself. */
void oc_model_apply(oc_model *m, oc_ev *e);

/* Find a channel by id, or NULL. */
oc_channel *oc_model_channel(oc_model *m, uint64_t channel_id);
/* Clear a channel's unread count and advance its read marker to high_water. */
void oc_model_mark_read(oc_model *m, uint64_t channel_id);

/* Begin/end viewing a thread. open clears any previously-loaded replies. */
void oc_model_open_thread(oc_model *m, uint64_t channel_id, uint64_t parent_id);
void oc_model_close_thread(oc_model *m);

/* Begin a search (clears prior hits, records the query) / close the search view. */
void oc_model_search_begin(oc_model *m, const char *query);
void oc_model_close_search(oc_model *m);

/* Begin a "who reacted" inspection (clears prior reactors, records the message)
 * / close the overlay. */
void oc_model_reactlist_begin(oc_model *m, uint64_t message_id);
void oc_model_close_reactlist(oc_model *m);

/* Begin listing a channel's incoming webhooks (clears prior entries + the
 * shown-once token, records the channel) / close the overlay. */
void oc_model_weblist_begin(oc_model *m, uint64_t channel_id);
void oc_model_close_weblist(oc_model *m);

/* A synced setting's value by key, or NULL if the bucket has no such key. Valid
 * until the next CLIENT_SETTINGS frame folds in. */
const char *oc_model_setting(const oc_model *m, const char *key);

/* Seen-by (REQ-090): fill `out` with up to `cap` user ids who have read
 * `channel_id` up to at least `message_id`, excluding `exclude` (typically self).
 * Returns the count. */
size_t oc_model_seen_by(const oc_model *m, uint64_t channel_id, uint64_t message_id,
                        uint64_t exclude, uint64_t *out, size_t cap);

/* Open/close the notification-prefs overlay (frontend view state). */
void oc_model_set_prefs_open(oc_model *m, int open);
/* A user's presence (OC_PRESENCE_OFFLINE if unknown). */
uint8_t oc_model_presence_of(const oc_model *m, uint64_t user_id);
/* Record a presence value (used for our own presence, which the server does not
 * echo back to us). */
void oc_model_note_presence(oc_model *m, uint64_t user_id, uint8_t status);

/* Roster lookups: a user's display name ("" if unknown), or an id by name (0). */
const char *oc_model_user_name(const oc_model *m, uint64_t user_id);
uint64_t    oc_model_user_id(const oc_model *m, const char *name);

/* User ids currently typing in `channel_id` (last seen within the timeout),
 * excluding `exclude` (typically self). Fills `out` up to `cap`; returns count. */
size_t oc_model_typing(const oc_model *m, uint64_t channel_id, uint64_t exclude,
                       uint64_t *out, size_t cap);

#endif /* OC_MODEL_H */
