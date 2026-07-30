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
/* `reclaimed` means the server removed the bytes by age or storage pressure
 * (REQ-215/217) while keeping the row, so the message still reads sensibly. A
 * frontend should show it as unavailable rather than offer a download. */
typedef struct {
    uint64_t id;
    char     filename[128];
    char     mime[64];
    uint64_t size;
    uint8_t  reclaimed;
} oc_attachment;

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
    uint8_t      pinned;      /* pinned to its channel (REQ-230) */
    uint64_t     pinned_by;   /* who pinned it (0 = unknown) */
    uint64_t     pinned_at;
    /* Saved for later by THIS user (REQ-231). Private, unlike a pin: it arrives
     * only on this connection, from SAVED_UPDATED — live when you save, and
     * replayed after a reconnect so a bookmark survives a reload. */
    uint8_t      saved;
    uint64_t     saved_at;
} oc_msg;

typedef struct {
    uint64_t channel_id;
    char    *name;         /* heap; NULL until a CHANNEL_LIST entry names it */
    oc_msg  *msgs;
    size_t   n_msgs, cap_msgs;
    char    *topic;        /* heap; NULL = none (REQ-034) */
    uint8_t  archived;     /* read-only; hidden from the default list (REQ-035) */
    uint64_t created_at;   /* from CHANNEL_INFO; shown in the About surface */
    /* The newest message, for a scannable list. Seeded by CHANNEL_LIST and kept
     * live by BROADCAST — otherwise it would be right only at connect. */
    char     preview[128];
    uint64_t preview_author;
    uint64_t high_water;   /* dedup mark: ignore message_id <= this (ARCH-45) */
    uint64_t read_marker;  /* high_water as of the last mark-read; drives unread */
    int      unread;       /* messages from others since the last mark-read */
    /* Server-reported, from CHANNEL_LIST — the only activity a cache-less client
     * (ARCH-88) knows about a channel it has not opened. `srv_unread` seeds the
     * badge before any backfill; `last_message_at` drives Recency sorting. */
    uint64_t last_message_at;
    uint32_t srv_unread;
    uint8_t  joined;
    uint8_t  history_requested; /* a backfill has been asked for (once per open) */
    uint8_t  kind;             /* OC_CHANNEL_KIND / _DM */
    uint8_t  is_public;        /* 1 public, 0 private/DM (REQ-031) */
    uint64_t peer_id;          /* DM: the other participant (for the title) */
    uint8_t  notify_level;     /* OC_NOTIFY_ALL/_MENTIONS/_NONE (REQ-130) */
    /* Muted (REQ-137, WIN-40) — NOT the same as level=NONE. Level decides whether
     * the daemon notifies; muted also de-emphasises the row and suppresses its
     * unread badge, so a conversation can be quiet but still countable, or
     * countable but silent. */
    uint8_t  muted;
    /* Per-member read cursors (REQ-090 seen-by): the highest message id each
     * member has read in this channel. Advance-only; drives "seen by …". */
    oc_read_cursor_view *readers;
    size_t   n_readers, cap_readers;
} oc_channel;

typedef struct { uint64_t user_id; uint8_t status; } oc_presence_row;

/* A tenant roster entry (from LIST_USERS): id, display name, role, disabled. */
typedef struct {
    uint64_t user_id;
    char     name[64];
    uint8_t  role;
    uint8_t  disabled;
    /* Custom status (REQ-241, WIN-53) and profile fields (REQ-240, WIN-47). Kept on
     * the roster entry rather than in a side table so a member list can render a
     * status without a second lookup — the roster is small and already in memory.
     * An EXPIRED status arrives empty: the daemon applies that rule, so no client
     * needs its own clock to decide. */
    char     status_emoji[24];
    char     status_text[80];
    uint64_t status_expires;
    char     title[64];
    char     timezone[48];
    uint64_t avatar_id;
} oc_member;

/* An ephemeral "user is typing in channel" mark, expiring on a timeout. */
typedef struct { uint64_t channel_id; uint64_t user_id; long long seen; } oc_typing_row;

/* One full-text search hit (REQ-080). */
typedef struct { uint64_t message_id, channel_id, author_id, server_time; char *snippet; } oc_search_result;

/* One reactor entry (from LIST_REACTIONS -> REACTIONS, REQ-071): who reacted
 * with which emoji on the inspected message. */
typedef struct { uint64_t user_id; char emoji[40]; } oc_reactor_row;

/* One entry of a channel's pins list (REQ-230). The body travels with it, so a
 * frontend can render the list without the message being in loaded history. */
typedef struct {
    uint64_t message_id, author_id, server_time, pinned_by, pinned_at;
    char    *body;                /* heap */
    char     attach_name[128];    /* first attachment, "" if none */
} oc_pinned_row;

/* One member of a CHANNEL (REQ-031) — distinct from oc_member, which is the
 * tenant roster. Keeping both is the point: showing the tenant's people beside a
 * channel name was the bug this replaces. */
typedef struct { uint64_t user_id, joined_at; uint8_t role; } oc_chan_member;

/* (channel, count) — the shape of a census row (WIN-82). */
typedef struct { uint64_t channel_id; uint32_t count; } oc_chan_count;

/* One file shared in a channel (REQ-143, ARCH-91). */
typedef struct {
    uint64_t id, channel_id, message_id, uploader_id, size, created_at;
    uint8_t  reclaimed;
    char     filename[128], mime[64];
} oc_file_view;

/* One outstanding invite (REQ-026, WIN-46). No token: only its SHA-256 is stored
 * server-side, so there is nothing to carry. */
typedef struct { uint64_t invite_id, expires_at, created_by; uint8_t role; } oc_invite_row;

/* One saved item (REQ-231) — private to this user. */
typedef struct {
    uint64_t message_id, channel_id, author_id, server_time, saved_at;
    char    *body;                 /* heap */
    char     attach_name[128];
} oc_saved_view;

/* One activity item (REQ-139): `text` is the message for a mention or reply and
 * the emoji for a reaction. */
typedef struct {
    uint8_t  kind;
    uint64_t message_id, channel_id, actor_id, at;
    char    *text;                 /* heap */
} oc_activity_view;

/* One incoming-webhook entry (from LIST_WEBHOOKS -> WEBHOOK_LIST, REQ-170): the
 * webhook's id + label + disabled flag. Tokens are never listed (shown once at
 * creation, in webhook_token). */
typedef struct { uint64_t webhook_id; char label[64]; uint8_t disabled; } oc_webhook_view;

typedef struct {
    bool     connected;
    bool     authed;
    uint64_t user_id;                 /* our own id, from AUTH_OK */
    /* Workspace facts from WORKSPACE_INFO (infra config the daemon serves). */
    uint8_t  deployment_mode;         /* 0 standalone · 1 federated · 2 managed */
    uint32_t max_users;               /* 0 = unlimited */
    char     workspace_name[64];      /* "" ⇒ frontend derives from the host subdomain */
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
    /* The server said it had more hits than it sent. There is no cursor on the
     * wire to page with (WIN-38), so this exists to be honest about the cap
     * rather than to drive a load-more. */
    uint8_t   search_truncated;
    /* The open "who reacted" view (REQ-071): the inspected message + its full
     * reactor list. reactlist_open is 0 when no such overlay is open. */
    uint8_t   reactlist_open;
    uint64_t  reactlist_message;
    oc_reactor_row *reactors;
    size_t    n_reactors, cap_reactors;
    /* The open pins view (REQ-230): a channel's pinned messages. pinlist_open
     * is 0 when no such overlay is open. Distinct from the per-message `pinned`
     * flag, which is what marks a pin inline in the transcript. */
    uint8_t   pinlist_open;
    uint64_t  pinlist_channel;
    uint8_t   pinlist_loading;   /* asked, terminator not yet seen */
    oc_pinned_row *pins;
    size_t    n_pins, cap_pins;
    /* The selected channel's own member roster (REQ-031) and shared files
     * (REQ-143). Both are per-channel views, refreshed on open rather than
     * cached: a client stores nothing (ARCH-88) and a stale roster is worse
     * than a moment's load. */
    uint64_t  chanmem_channel;
    uint8_t   chanmem_loading;
    oc_chan_member *chanmem;
    size_t    n_chanmem, cap_chanmem;
    uint8_t   filelist_open, filelist_loading;
    uint64_t  filelist_channel;      /* 0 = the workspace-wide view */
    oc_file_view *files;
    size_t    n_files, cap_files;
    /* Which channels hold files, with counts (WIN-82). Server-computed, so the Files
     * column is complete rather than "whatever was in the newest 200". */
    oc_chan_count *fchans;
    size_t    n_fchans, cap_fchans;
    /* Saved items (REQ-231) and the activity feed (REQ-139). Both are per-user
     * lists refreshed on open — a client caches nothing (ARCH-88). */
    uint8_t   saved_open, saved_loading;
    oc_saved_view *saved;
    size_t    n_saved, cap_saved;
    uint8_t   activity_open, activity_loading;
    oc_activity_view *activity;
    size_t    n_activity, cap_activity;
    uint64_t  activity_seen;      /* watermark from the last feed read */
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
    /* Outstanding invites (REQ-026, WIN-46). Refreshed on open, like every other
     * admin report — a client caches nothing (ARCH-88). */
    uint8_t   invites_open, invites_loading;
    oc_invite_row *invites;
    size_t    n_invites, cap_invites;
    uint64_t  webhook_new_id;
    /* The synced client-settings bucket (the daemon-side config layer). A
     * CLIENT_SETTINGS frame — solicited or a device-sync push — replaces it
     * wholesale (OC_EV_SETTINGS_BEGIN clears, OC_EV_SETTING entries refill). The
     * frontend reads values with oc_model_setting and layers them over its
     * machine-local file defaults. */
    oc_setting *settings;
    size_t    n_settings, cap_settings;
    uint8_t   settings_synced;        /* a snapshot has arrived at least once */

    /* Storage report (REQ-214). Owner/admin only, so it stays zeroed for a
     * member — the daemon refuses the request rather than sending zeros.
     * `storage_open` is frontend view state for the overlay. */
    oc_storage_view storage;
    uint8_t   storage_have;
    uint8_t   storage_open;

    /* Audit log page (REQ-251), newest first. Owner/admin only. */
    oc_audit_view *audit;
    size_t    n_audit, cap_audit;
    uint8_t   audit_open;
    char     status[160];             /* last status / error line */
    /* The last hard error (auth failed, unreachable, …). Unlike `status` it is
     * NOT overwritten by the "disconnected" line, so a login flow can read the
     * reason after the connection drops; cleared on a successful connect. */
    /* The most recent in-memory attachment fetch (WIN-17). The model holds the
     * bytes until a frontend takes them; taking transfers ownership, so nothing
     * accumulates if nobody asks. */
    uint64_t fetched_attachment;
    uint8_t *fetched_data;
    size_t   fetched_len;

    /* When the net thread will next attempt a reconnect, as a monotonic
     * millisecond stamp (0 = not backing off). The error text carries the delay
     * once per backoff, which cannot tick; a frontend that wants a live
     * countdown needs the deadline itself (WIN-55). */
    uint64_t reconnect_at_ms;

    char     last_error[160];
    /* Bumped every time an error arrives, even an identical one. A frontend that
     * notices only when the TEXT changes stays silent when you repeat a failing
     * action — the second attempt looks like it worked. */
    uint32_t error_seq;
} oc_model;

void oc_model_init(oc_model *m);
void oc_model_free(oc_model *m);

/* Fold one net event into the model. Takes ownership of `e->body` (moves it into
 * the model and clears it); the caller still frees the event struct itself. */
void oc_model_apply(oc_model *m, oc_ev *e);

/* Find a channel by id, or NULL. */
oc_channel *oc_model_channel(oc_model *m, uint64_t channel_id);

/* ---- the sidebar (WIN-5/6) --------------------------------------------------
 * Grouping, filtering, sorting and collapse are IDENTICAL in every frontend, so
 * they live here rather than being written twice (ARCH-74: the core holds the
 * logic, a frontend is view + input). A frontend asks for the rows and draws
 * them; it does not decide what belongs where.
 *
 * Two sections, matching Slack: Channels (public and private together, the
 * private ones marked) and Direct messages. A DM has no name on the wire — the
 * daemon stores NULL (kind='dm') — so a row carries a rendered `label` and a
 * frontend must never filter on `name`. */
/* Three sections now. STARRED is Slack's shape: pinned conversations lifted to the
 * top and REMOVED from their normal section, so a starred channel appears once. It
 * is last in the enum so the existing per-section arrays keep their indices — the
 * order on SCREEN is decided by the builder, not by this numbering. */
enum { OC_SB_CHANNELS = 0, OC_SB_DMS = 1, OC_SB_STARRED = 2, OC_SB_SECTIONS = 3 };
#define OC_SB_STARRED_MAX 32u
enum { OC_SB_SORT_AZ = 0, OC_SB_SORT_RECENT, OC_SB_SORT_UNREAD };
enum { OC_SB_FILTER_ALL = 0, OC_SB_FILTER_UNREAD, OC_SB_FILTER_ACTIVE };

typedef struct {
    uint8_t  sort[OC_SB_SECTIONS];       /* OC_SB_SORT_*   , per section */
    uint8_t  filter[OC_SB_SECTIONS];     /* OC_SB_FILTER_* , per section */
    uint8_t  collapsed[OC_SB_SECTIONS];  /* 1 = show the header only */
    char     find[64];                   /* "Find a conversation" text, lowercased */
    /* Starred conversations (REQ-234, WIN-41). Ids rather than names: a channel can
     * be renamed and a DM has no name at all. Kept in the OPTIONS rather than the
     * model because it is a per-user display choice, exactly like sort and filter,
     * and the frontend owns persisting it. */
    uint64_t starred[OC_SB_STARRED_MAX];
    uint8_t  n_starred;
} oc_sidebar_opts;

/* Is `channel_id` starred? Shared so a frontend's menu and the builder cannot
 * disagree about it. */
int  oc_sidebar_is_starred(const oc_sidebar_opts *o, uint64_t channel_id);
/* Toggle. Returns 1 when the set changed (a full list refuses silently otherwise). */
int  oc_sidebar_toggle_star(oc_sidebar_opts *o, uint64_t channel_id);

typedef struct {
    uint8_t  is_header;      /* a section header row */
    uint8_t  section;        /* OC_SB_CHANNELS / OC_SB_DMS */
    uint64_t channel_id;     /* 0 on a header */
    char     label[96];      /* rendered: "general", or a DM peer's name */
    uint8_t  is_private;     /* draw a lock */
    uint8_t  is_self;        /* the self-DM: render the name, then a dimmed "you" */
    uint64_t peer_id;        /* DM only: lets a frontend draw an avatar + presence
                              * instead of a bare "@" marker, as Slack does */
    uint8_t  joined;
    int      unread;
    int      section_total;  /* header rows: how many children (before collapse) */
} oc_sidebar_row;

/* Build the sidebar. Returns the row count written to `out` (capped by `cap`).
 * Headers are emitted even when a section is empty or collapsed, so the user can
 * always expand it again. */
size_t oc_model_sidebar(const oc_model *m, const oc_sidebar_opts *o,
                        oc_sidebar_row *out, size_t cap);

/* Default options: both sections open, A-Z, unfiltered. */
void oc_sidebar_opts_defaults(oc_sidebar_opts *o);

/* Serialize/parse the options for the daemon's client_settings bucket, so the
 * choice survives a restart without the client storing anything (ARCH-88).
 * Format is a compact "c:s,f,x;d:s,f,x" so one setting key carries all of it. */
void oc_sidebar_opts_encode(const oc_sidebar_opts *o, char *out, size_t cap);
void oc_sidebar_opts_parse(oc_sidebar_opts *o, const char *s);
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

/* Begin / end a pins inspection for a channel (REQ-230). Begin clears any prior
 * list and marks it loading, so a frontend can tell "no pins yet" from "empty". */
void oc_model_pinlist_begin(oc_model *m, uint64_t channel_id);
void oc_model_close_pinlist(oc_model *m);

/* A channel's own member roster (REQ-031) and shared files (REQ-143). */
void oc_model_chanmem_begin(oc_model *m, uint64_t channel_id);
void oc_model_filelist_begin(oc_model *m, uint64_t channel_id);
void oc_model_close_filelist(oc_model *m);

/* Saved items / activity (REQ-231/139). */
void oc_model_saved_begin(oc_model *m);
void oc_model_close_saved(oc_model *m);
void oc_model_activity_begin(oc_model *m);
void oc_model_close_activity(oc_model *m);

/* Begin listing a channel's incoming webhooks (clears prior entries + the
 * shown-once token, records the channel) / close the overlay. */
void oc_model_weblist_begin(oc_model *m, uint64_t channel_id);
void oc_model_close_weblist(oc_model *m);
void oc_model_invites_begin(oc_model *m);
void oc_model_close_invites(oc_model *m);

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
/* Milliseconds until the next reconnect attempt, 0 when not backing off. Pass
 * the frontend's current monotonic clock — the core does not own a clock. */
uint64_t oc_model_reconnect_in(const oc_model *m, uint64_t now_ms);

/* The clock the core stamps that deadline with. A frontend must read the time
 * from HERE rather than its own source, or the two disagree and the countdown
 * jumps. */
uint64_t oc_model_now_ms(void);

/* Take the last fetched attachment's bytes (WIN-17), transferring ownership to
 * the caller, which must free() them. Returns NULL when nothing is waiting. */
uint8_t *oc_model_take_attachment(oc_model *m, uint64_t *attachment_id, size_t *len);

/* A user's presence (OC_PRESENCE_OFFLINE if unknown). */
uint8_t oc_model_presence_of(const oc_model *m, uint64_t user_id);
/* Record a presence value (used for our own presence, which the server does not
 * echo back to us). */
void oc_model_note_presence(oc_model *m, uint64_t user_id, uint8_t status);

/* Roster lookups: a user's display name ("" if unknown), or an id by name (0). */
const char *oc_model_user_name(const oc_model *m, uint64_t user_id);
uint64_t    oc_model_user_id(const oc_model *m, const char *name);

/* Workspace facts (WORKSPACE_INFO). deployment mode name: "standalone" /
 * "federated" / "managed". workspace_name is "" until a name is configured. */
uint8_t     oc_model_deployment_mode(const oc_model *m);
const char *oc_model_deployment_name(const oc_model *m);
const char *oc_model_workspace_name(const oc_model *m);
uint32_t    oc_model_max_users(const oc_model *m);

/* User ids currently typing in `channel_id` (last seen within the timeout),
 * excluding `exclude` (typically self). Fills `out` up to `cap`; returns count. */
size_t oc_model_typing(const oc_model *m, uint64_t channel_id, uint64_t exclude,
                       uint64_t *out, size_t cap);

#endif /* OC_MODEL_H */
