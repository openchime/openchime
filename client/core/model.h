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
/* Custom emoji (REQ-072): the workspace catalogue, replaced wholesale by each
 * EMOJI_LIST — a partial catalogue means a shortcode that renders on one client and
 * not another. The image is an attachment id, which is what a frontend fetches. */
typedef struct { char name[48]; uint64_t attachment_id; uint64_t created_by; } oc_custom_emoji;

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

/* One link preview under a message (REQ-222, ARCH-105): the daemon's fetched
 * title + description for a URL the body carries. All heap. */
typedef struct { char *url, *title, *descr; } oc_msg_unfurl;

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
    /* Link previews (REQ-222): fanned after the BROADCAST, replayed on
     * backfill, cleared on edit (the daemon drops and re-fetches). */
    oc_msg_unfurl *unfurls;   /* heap, NULL until one arrives */
    uint8_t        n_unfurls, cap_unfurls;
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
    /* GROUP DM (REQ-056): every participant, including you. n_peers is 0 for a
     * channel and for a 1:1 DM — a DM IS a group when it has more than two people
     * in it, which is what a DM's identity has always been on the server. Carried on
     * the channel so the sidebar can title it on first paint. */
    uint64_t peers[9];
    uint16_t n_peers;
    uint8_t  notify_level;     /* OC_NOTIFY_ALL/_MENTIONS/_NONE (REQ-130) */
    /* Muted (REQ-137) — NOT the same as level=NONE. Level decides whether
     * the daemon notifies; muted also de-emphasises the row and suppresses its
     * unread badge, so a conversation can be quiet but still countable, or
     * countable but silent. */
    uint8_t  muted;
    /* Per-member read cursors (REQ-090 seen-by): the highest message id each
     * member has read in this channel. Advance-only; drives "seen by …". */
    oc_read_cursor_view *readers;
    size_t   n_readers, cap_readers;
} oc_channel;

/* `dnd` is a SECOND AXIS beside status, not a status value (REQ-122): a person
 * can be online and not to be disturbed, and one field could not say both. */
typedef struct { uint64_t user_id; uint8_t status; uint8_t dnd; } oc_presence_row;

/* A tenant roster entry (from LIST_USERS): id, display name, role, disabled. */
typedef struct {
    uint64_t user_id;
    char     name[64];
    uint8_t  role;
    uint8_t  disabled;
    /* Custom status (REQ-241) and profile fields (REQ-240). Kept on
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

/* One live session (REQ-182). No token: only its hash exists server-side. */
typedef struct {
    uint64_t session_id, created_at, last_seen, expires_at;
    uint8_t  current;
    char     device[64];
} oc_session_row;

/* (channel, count) — the shape of a census row. */
typedef struct { uint64_t channel_id; uint32_t count; } oc_chan_count;

/* One file shared in a channel (REQ-143, ARCH-91). */
typedef struct {
    uint64_t id, channel_id, message_id, uploader_id, size, created_at;
    uint8_t  reclaimed;
    char     filename[128], mime[64];
} oc_file_view;

/* One outstanding invite (REQ-026). No token: only its SHA-256 is stored
 * server-side, so there is nothing to carry. */
typedef struct { uint64_t invite_id, expires_at, created_by; uint8_t role; } oc_invite_row;

/* One saved item (REQ-231) — private to this user. */
typedef struct {
    uint64_t message_id, channel_id, author_id, server_time, saved_at;
    char    *body;                 /* heap */
    char     attach_name[128];
} oc_saved_view;

/* One draft (REQ-223, ARCH-101). Keyed by the conversation, not by time: there
 * is exactly one per (channel, thread root), which is why a write replaces
 * rather than appends. `gen` is the sync generation — see oc_model_drafts_begin. */
typedef struct {
    uint64_t channel_id;
    uint64_t thread_root;
    uint64_t updated_ms;
    char    *body;        /* heap, never NULL for a stored draft */
    uint32_t gen;
} oc_draft_view;

/* One thread in the aggregated view (REQ-062). `gen` is the sync generation, as
 * drafts use it: a list replaces what the server did not mention. */
typedef struct {
    uint64_t root_id, channel_id, root_author;
    uint64_t root_at, last_reply_at;
    uint32_t reply_count, unread;
    uint8_t  following;
    char    *preview;     /* heap */
    uint32_t gen;
} oc_thread_view;

/* One scheduled message (REQ-224). `state` is OC_SCHED_*; a FAILED one keeps its
 * reason, because that is the whole of what its author needs to see. */
typedef struct {
    uint64_t id;
    uint64_t channel_id;
    uint64_t send_at_ms;
    uint8_t  state;
    char    *body;        /* heap */
    char     fail[96];
    uint32_t gen;
} oc_sched_view;

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
    /* The server said it had more hits than it sent. The wire gained a keyset
     * cursor, so this now drives a "Load more" affordance rather than only an
     * apology; it still exists to be honest about the cap
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
    /* The caller's own live sessions (REQ-182). Refreshed on open like every other
     * report; a client caches nothing (ARCH-88). */
    uint8_t   sessions_open, sessions_loading;
    oc_session_row *sessions;
    size_t    n_sessions, cap_sessions;
    /* Which channels hold files, with counts. Server-computed, so the Files
     * column is complete rather than "whatever was in the newest 200". */
    oc_chan_count *fchans;
    size_t    n_fchans, cap_fchans;
    /* Saved items (REQ-231) and the activity feed (REQ-139). Both are per-user
     * lists refreshed on open — a client caches nothing (ARCH-88). */
    uint8_t   saved_open, saved_loading;
    oc_saved_view *saved;
    size_t    n_saved, cap_saved;
    /* Drafts (REQ-223). Always loaded — the sidebar marks conversations that
     * hold one — so unlike `saved` this has no open/close, only a generation
     * that a full LIST bumps so stale entries can be swept when it completes. */
    /* Threads I am in (REQ-062), newest activity first as the server ordered them. */
    oc_thread_view *threads;
    size_t    n_threads, cap_threads;
    uint32_t  thread_gen;
    uint8_t   threads_loading;
    oc_draft_view *drafts;
    size_t    n_drafts, cap_drafts;
    uint32_t  draft_gen;
    oc_sched_view *scheds;
    size_t    n_scheds, cap_scheds;
    uint32_t  sched_gen;
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
    /* The custom-emoji catalogue (REQ-072). */
    oc_custom_emoji *cemoji;
    size_t    n_cemoji, cap_cemoji;
    uint8_t   prefs_open;
    /* The recurring SCHEDULE (REQ-136): `dnd_mode` is OC_DND_*, and the window
     * is the hours notifications are ALLOWED — the opposite sense from the quiet
     * window it replaced, which is why the fields are named for it. */
    uint8_t   dnd_mode;
    int16_t   tz_offset_min;
    uint16_t  allow_start_min, allow_end_min;
    oc_schedule_day sched_days[OC_SCHEDULE_DAYS];
    uint8_t   n_sched_days;
    /* Keyword alerts and priority people (REQ-135). Held so the client can
     * HIGHLIGHT what the server notified on — the same list, matched by the same
     * scanner, which is the whole point of ARCH-103. */
    char      kw_terms[OC_MAX_KEYWORDS][OC_KEYWORD_MAX];
    uint8_t   n_kw_terms;
    uint64_t  pri_people[OC_MAX_PRIORITY];
    uint8_t   n_pri_people;
    /* REQ-278: when the manual PAUSE ends, 0 for none. Distinct from the window
     * above in type as well as in name — one instant against a daily range. */
    uint64_t  snooze_until_ms;
    /* REQ-134: the level a channel takes when it has NO per-channel row. Server
     * state, not a client guess — the daemon is what decides whether to push. */
    uint8_t   notify_default;
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
    /* Outstanding invites (REQ-026). Refreshed on open, like every other
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
    /* The most recent in-memory attachment fetch. The model holds the
     * bytes until a frontend takes them; taking transfers ownership, so nothing
     * accumulates if nobody asks. */
    uint64_t fetched_attachment;
    uint8_t *fetched_data;
    size_t   fetched_len;

    /* When the net thread will next attempt a reconnect, as a monotonic
     * millisecond stamp (0 = not backing off). The error text carries the delay
     * once per backoff, which cannot tick; a frontend that wants a live
     * countdown needs the deadline itself. */
    uint64_t reconnect_at_ms;

    char     last_error[160];
    /* Bumped every time an error arrives, even an identical one. A frontend that
     * notices only when the TEXT changes stays silent when you repeat a failing
     * action — the second attempt looks like it worked. */
    uint32_t error_seq;

    /* REQ-287: the last SEND that named people who are not in that channel.
     * `seq` bumps per occurrence for the same reason `error_seq` does — mention
     * the same absent colleague in two messages and the second must not be
     * swallowed as a duplicate of the first. */
    struct {
        uint64_t channel_id, message_id;
        uint64_t peers[9];
        uint16_t n_peers;
        uint8_t  can_add;      /* the sender may add people here */
        uint8_t  is_private;   /* adding discloses the channel's history */
        char     names[192];   /* comma-joined, ready to show */
        uint32_t seq;
    } unresolved;
} oc_model;

void oc_model_init(oc_model *m);
void oc_model_free(oc_model *m);

/* Fold one net event into the model. Takes ownership of `e->body` (moves it into
 * the model and clears it); the caller still frees the event struct itself. */
void oc_model_apply(oc_model *m, oc_ev *e);

/* Find a channel by id, or NULL. */
oc_channel *oc_model_channel(oc_model *m, uint64_t channel_id);

/* ---- the sidebar (6) --------------------------------------------------
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

/* User-defined sections (REQ-234's other half). A custom section is a NAME
 * plus a set of conversation ids, and a conversation in one is removed from
 * Channels/DMs so it still appears exactly once — the same rule Starred follows.
 * Starred wins over a custom section when a conversation is in both: two "lift it
 * out of its section" rules need a precedence and the explicit star is the one the
 * user set most recently by hand.
 *
 * Sections are numbered from OC_SB_CUSTOM_BASE in a row's `section` field, which is
 * why the built-in per-section arrays above are indexed through the accessors
 * below rather than directly — a custom section's sort/filter/collapse live on the
 * section itself, so adding one cannot renumber anyone's saved preferences. */
#define OC_SB_CUSTOM_MAX   8u
#define OC_SB_CUSTOM_IDS   32u
#define OC_SB_CUSTOM_BASE  16
#define OC_SB_NAME_MAX     32
enum { OC_SB_SORT_AZ = 0, OC_SB_SORT_RECENT, OC_SB_SORT_UNREAD };
enum { OC_SB_FILTER_ALL = 0, OC_SB_FILTER_UNREAD, OC_SB_FILTER_ACTIVE };

typedef struct {
    uint8_t  sort[OC_SB_SECTIONS];       /* OC_SB_SORT_*   , per section */
    uint8_t  filter[OC_SB_SECTIONS];     /* OC_SB_FILTER_* , per section */
    uint8_t  collapsed[OC_SB_SECTIONS];  /* 1 = show the header only */
    char     find[64];                   /* "Find a conversation" text, lowercased */
    /* Starred conversations (REQ-234). Ids rather than names: a channel can
     * be renamed and a DM has no name at all. Kept in the OPTIONS rather than the
     * model because it is a per-user display choice, exactly like sort and filter,
     * and the frontend owns persisting it. */
    uint64_t starred[OC_SB_STARRED_MAX];
    uint8_t  n_starred;
    /* Custom sections, in the order they appear on screen (between Starred and
     * Channels). Ids, not names, for the same reason as `starred`. */
    struct {
        char     name[OC_SB_NAME_MAX];
        uint64_t ids[OC_SB_CUSTOM_IDS];
        uint8_t  n_ids;
        uint8_t  sort, filter, collapsed;
    } custom[OC_SB_CUSTOM_MAX];
    uint8_t  n_custom;
} oc_sidebar_opts;

/* Per-section sort / filter / collapse, for BOTH kinds of section. A frontend must
 * go through these rather than indexing o->sort[sec]: a custom section's number is
 * >= OC_SB_CUSTOM_BASE and would run off the end of those arrays. */
uint8_t oc_sb_sort_of(const oc_sidebar_opts *o, int section);
uint8_t oc_sb_filter_of(const oc_sidebar_opts *o, int section);
uint8_t oc_sb_collapsed_of(const oc_sidebar_opts *o, int section);
void    oc_sb_set_sort(oc_sidebar_opts *o, int section, uint8_t v);
void    oc_sb_set_filter(oc_sidebar_opts *o, int section, uint8_t v);
void    oc_sb_set_collapsed(oc_sidebar_opts *o, int section, uint8_t v);
/* Is this a user-defined section, and which one? -1 when it is a built-in. */
int     oc_sb_custom_index(int section);

/* Create a section. Returns its index, or -1 when the list is full or the name is
 * empty. The name is SANITISED (the setting's separators are stripped) because it
 * round-trips through one flat string in client_settings. */
int  oc_sidebar_section_add(oc_sidebar_opts *o, const char *name);
void oc_sidebar_section_rename(oc_sidebar_opts *o, int idx, const char *name);
/* Delete a section. Its conversations return to Channels/DMs rather than
 * disappearing — a section is a view, not a container. */
void oc_sidebar_section_remove(oc_sidebar_opts *o, int idx);
/* Which custom section holds `channel_id`, or -1. */
int  oc_sidebar_section_of(const oc_sidebar_opts *o, uint64_t channel_id);
/* Put `channel_id` in section `idx`, or take it out of every section with idx < 0.
 * A conversation is in at most one section: assigning moves it. Returns 1 when
 * something changed. */
int  oc_sidebar_assign(oc_sidebar_opts *o, uint64_t channel_id, int idx);

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

/* Look one up by shortcode (no colons). Returns the attachment id, or 0. */
uint64_t oc_model_custom_emoji(const oc_model *m, const char *name);

/* Render a DM's title — the peer's name for a 1:1, the participants for a group
 * (REQ-056). Exposed because a frontend needs the same string in its header as the
 * sidebar shows, and two renderers would eventually disagree about who is in a
 * conversation. */
void oc_model_dm_title(const oc_model *m, const oc_channel *c, char *out, size_t cap);

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
void oc_model_sessions_begin(oc_model *m);
void oc_model_close_sessions(oc_model *m);

/* A synced setting's value by key, or NULL if the bucket has no such key. Valid
 * until the next CLIENT_SETTINGS frame folds in. */
const char *oc_model_setting(const oc_model *m, const char *key);
/* The draft for a conversation, or NULL. `thread_root` is 0 for the channel. */
const char *oc_model_draft(const oc_model *m, uint64_t channel_id, uint64_t thread_root);
/* Does this channel hold any draft? (the sidebar marker) */
int         oc_model_has_draft(const oc_model *m, uint64_t channel_id);
/* A full LIST is starting: bump the generation so oc_model_drafts_end can drop
 * whatever the server did not mention this time. */
void        oc_model_drafts_begin(oc_model *m);
/* Apply a draft this client just wrote. The daemon does not echo a draft to the
 * connection that wrote it (ARCH-101 — echoing it back is how a client ends up
 * overwriting the composer someone is still typing in), so without this our own
 * drafts would be invisible to our own sidebar until the next connect. */
void        oc_model_draft_local(oc_model *m, uint64_t channel_id,
                                 uint64_t thread_root, const char *body);
/* How many scheduled messages are waiting, and how many failed — the pane's two
 * counts, and the only reason the sidebar row needs to know about them. */
size_t      oc_model_scheduled_pending(const oc_model *m);
size_t      oc_model_scheduled_failed(const oc_model *m);

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

/* Take the last fetched attachment's bytes, transferring ownership to
 * the caller, which must free() them. Returns NULL when nothing is waiting. */
uint8_t *oc_model_take_attachment(oc_model *m, uint64_t *attachment_id, size_t *len);

/* A user's presence (OC_PRESENCE_OFFLINE if unknown). */
uint8_t oc_model_presence_of(const oc_model *m, uint64_t user_id);
/* The thread list (REQ-062). `oc_model_threads_begin` bumps the sync generation;
 * the terminator sweeps anything the server did not mention. */
void oc_model_threads_begin(oc_model *m);
/* Total unread replies across every thread — the badge on the Threads row. */
uint32_t oc_model_thread_unread(const oc_model *m);
/* Whether a user is not to be disturbed right now (REQ-122). The fact only: the
 * server never sends anyone else's end time. */
int oc_model_dnd_of(const oc_model *m, uint64_t user_id);
/* Are MY notifications paused, and until when (0 = no) — my own end time is the
 * one this client is allowed to know. */
int oc_model_snoozed(const oc_model *m);
/* Does `body` hit one of MY keywords (REQ-135)? The client's half of ARCH-103:
 * the same scanner the daemon notified with, so a highlight and a notification
 * can never disagree. */
int oc_model_keyword_hit(const oc_model *m, const char *body, size_t len,
                         size_t *span_start, size_t *span_len);
/* Is `user_id` one of my priority people? */
int oc_model_is_priority(const oc_model *m, uint64_t user_id);
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
