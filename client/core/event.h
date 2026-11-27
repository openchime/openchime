/*
 * OpenChime client — the events/commands crossing the UI↔net queues (ARCH-62).
 * net→UI events report connection + protocol state; UI→net commands carry user
 * actions. Variable-length bodies are heap-owned by the struct.
 */

#ifndef OC_EVENT_H
#define OC_EVENT_H

#include <stdint.h>

/* For the schedule and alert-list shapes carried on events and commands: they
 * are wire types, and re-declaring them here would be a second definition to
 * keep in step (REQ-135/136). */
#include "protocol.h"

/* Storage usage, policy, and what maintenance has reclaimed (REQ-214/215),
 * carried on OC_EV_STORAGE and folded into the model for the frontend to
 * render. Owner/admin only — the daemon refuses the request otherwise. */
/* The largest certificate (DER) a frontend is handed to show; a larger one is
 * judged by its fingerprint alone. */
#define OC_CERT_DER_MAX 8192

/* What an OC_EV_ERROR is about, beside its sentence: the stable kind a frontend
 * (or a test) acts on, since the words are for a person and may change. A
 * refusal from the daemon also carries its OC_ERR_* code. */
/* What the client says when the daemon refuses an export (EXPORT_UNAVAILABLE,
 * ARCH-135): the notice on OC_EV_XFER that ends an OC_CMD_EXPORT. */
#define OC_EXPORT_UNAVAILABLE_TEXT "Export is not available right now"
/* An export's transfer tag: its attachment id with the top bit set, so it is
 * never taken for a download or fetch of the same video, whose tag is the id. */
#define OC_EXPORT_TAG(attachment_id) ((uint64_t)(attachment_id) | (1ull << 63))

typedef enum {
    OC_CERR_OTHER = 0,          /* anything not named below */
    OC_CERR_REFUSED,            /* the daemon refused a request (err_code says which) */
    OC_CERR_AUTH,               /* the daemon refused a sign-in (err_code says which) */
    OC_CERR_SIGNIN_CANCELLED,   /* the person stopped a sign-in */
    OC_CERR_SIGNIN_TIMEOUT,     /* a browser sign-in was not finished in time */
    OC_CERR_SIGNIN_UNFINISHED,  /* the browser came back without a sign-in */
    OC_CERR_LOCAL,              /* this computer could not start what was asked */
    OC_CERR_UNREACHABLE,        /* the server could not be reached */
    OC_CERR_CERT,               /* the server's certificate is not the one expected */
    OC_CERR_RECONNECTING,       /* the connection was lost; a reconnect is due */
    OC_CERR_PROTOCOL,           /* the server spoke a protocol this client does not */
    OC_CERR_PDM_CLOSED          /* an encrypted conversation is not open here: nothing was sent */
} oc_client_err;

typedef struct {
    uint64_t total_bytes, avail_bytes, attach_bytes, attach_count;
    uint64_t rec_orphan, rec_expired, rec_evicted, last_reclaim_ms;
    uint64_t max_age_days, reserve_bytes;
    uint8_t  evict_enabled, under_pressure;
} oc_storage_view;

/* The workspace's usage (REQ-043), carried on OC_EV_USAGE and kept in the model
 * for the console's Usage tab. Owner/admin only. A channel with an empty name is
 * a private one the reader is not in: shown as "private channel". */
typedef struct {
    uint16_t       days, n_days, n_weeks, n_channels;
    oc_usage_day   day[OC_USAGE_MAX_DAYS];     /* oldest first */
    oc_usage_week  week[OC_USAGE_MAX_WEEKS];   /* oldest first */
    struct {
        uint64_t id;
        uint32_t messages;
        uint8_t  is_private;
        char     name[128];
    } channel[OC_USAGE_TOP_CHANNELS];          /* busiest first */
} oc_usage_view;

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
    uint64_t channel_id; /* the channel it happened in; 0 none */
} oc_audit_view;

/* One of the daemon's critical failures as an owner or admin sees it (REQ-263). */
typedef struct {
    uint64_t id, first_ms, last_ms;
    uint32_t count;
    uint8_t  current, acked;
    char     key[OC_MAX_ALERT_KEY + 1];
    char     message[OC_MAX_ALERT_MESSAGE + 1];
} oc_srvalert_view;

/* A call as the model holds it (REQ-150, REQ-301-305, CALLS.md §1): what
 * CALL_STATE says of a call in Calls home, or -- on CALL_JOINED / CALL_ROSTER --
 * of the call this client is in, with each participant's slot. `from_channel`
 * is the conversation it was started from, 0 for none. Heap-owned by the event
 * and handed to the model, which keeps its own copy. */
typedef struct {
    uint64_t from_channel, call_id, starter, started_at;
    uint64_t chat_channel;   /* the call's own chat: a conversation in no list */
    uint8_t  recording;      /* it is being recorded (CALLS.md §8) */
    uint64_t rec_by;         /* ...started by this user; 0 when not recorded */
    char     subject[OC_CALL_SUBJECT_MAX + 1];
    uint32_t epoch;
    uint8_t  ended;
    uint8_t  my_slot;
    uint16_t n_parts, n_invited;
    uint64_t parts[OC_MAX_CALL_PARTICIPANTS];
    uint8_t  slots[OC_MAX_CALL_PARTICIPANTS];
    uint8_t  away[OC_MAX_CALL_PARTICIPANTS];   /* connection gone, seat held (OC_CALL_PART_AWAY) */
    uint8_t  camera[OC_MAX_CALL_PARTICIPANTS]; /* camera on (OC_CALL_PART_CAMERA, REQ-160) */
    uint8_t  recorder[OC_MAX_CALL_PARTICIPANTS]; /* the Recorder (OC_CALL_PART_RECORDER, CALLS.md §8) */
    uint64_t invited[OC_MAX_CALL_INVITES];
    uint64_t sharer;      /* who is sharing a screen, 0 for nobody (REQ-161) */
} oc_call_view;

/* A call as the history lists it (CALLS.md §1), as this user saw it: one row
 * of CALL_HISTORY_PAGE. `flags` OC_CALL_HIST_*; `end_reason` OC_CALL_ENDED_*. */
typedef struct {
    uint64_t call_id, starter, started_at, ended_at, from_channel, chat_channel;
    uint8_t  end_reason, flags;
    uint16_t n_people;
    uint8_t  n_faces;                           /* who took part, first joined first */
    uint64_t faces[OC_CALL_HIST_FACES];
    char     subject[OC_CALL_SUBJECT_MAX + 1];
} oc_call_hist_view;
/* One page of the history: `before_ms` what was asked for (0 the newest). */
typedef struct {
    uint64_t          before_ms;
    uint16_t          count;
    uint8_t           more;
    oc_call_hist_view rows[OC_CALL_HISTORY_PAGE];
} oc_call_history_view;

/* A call's page (CALLS.md §1): its people's stays, who was asked, the
 * timeline, its recordings and its transcript's lines from `line_from`.
 * Heap-owned with its lines (oc_call_page_view_free). */
typedef struct { uint64_t at, user_id; uint8_t kind; char detail[OC_CALL_SUBJECT_MAX + 1]; } oc_call_event_view;
typedef struct { uint64_t id, speaker; uint32_t start_ms, end_ms; char *text; } oc_call_line_view;
typedef struct {
    uint64_t           call_id, starter, started_at, ended_at, from_channel, chat_channel;
    uint8_t            end_reason;
    char               subject[OC_CALL_SUBJECT_MAX + 1];
    uint16_t           n_stays, n_invitees, n_events, n_recordings;
    oc_call_stay       stays[OC_CALL_PAGE_STAYS];
    oc_call_invitee    invitees[OC_CALL_PAGE_INVITEES];
    oc_call_event_view events[OC_CALL_PAGE_EVENTS];
    oc_call_recording  recordings[OC_CALL_PAGE_RECORDINGS];
    uint8_t            has_transcript;
    char               transcript_lang[16];
    uint32_t           n_lines_total, line_from;
    uint32_t           n_lines;          /* held: the model's copy gathers pages of them */
    oc_call_line_view *lines;
} oc_call_page_view;
void oc_call_page_view_free(oc_call_page_view *p);

/* One user group (REQ-307), whole, as a GROUP_INFO says it. */
typedef struct {
    uint64_t  id, created_by;
    char      handle[OC_GROUP_HANDLE_MAX + 1];
    char      name[OC_GROUP_NAME_MAX + 1];
    char      description[OC_GROUP_DESC_MAX + 1];
    uint16_t  n_members;
    uint64_t  members[OC_MAX_GROUP_MEMBERS];
} oc_group_view;
/* A poll (REQ-225, ARCH-119) as the daemon last told it: its definition and
 * tallies, and this user's own selection, a bit per option, in `mine`. The
 * question is the message body. OC_POLL_* in `flags`; on an event,
 * OC_POLL_HAS_MINE says whether `mine` is known -- a tally sent to the whole
 * conversation leaves the selection the model already holds alone. */
typedef struct {
    uint8_t  flags, n_options;
    uint64_t close_at_ms;      /* 0: never closes */
    uint32_t voters;           /* people, not picks */
    uint16_t mine;
    char     labels[OC_POLL_MAX_OPTIONS][OC_POLL_OPTION_MAX + 1];
    uint32_t counts[OC_POLL_MAX_OPTIONS];
} oc_poll_view;
/* A to-do list as the daemon last told it: its author, title and every task,
 * in order. Strings heap; oc_todo_view_free releases it all. On an event it is
 * one TODO frame's range: tasks[0, todo_n) are tasks [todo_first, ...) of
 * `total`. */
typedef struct { char *name, *desc; uint8_t done; uint64_t done_by, done_at_ms; } oc_todo_task_view;
typedef struct {
    uint64_t author_id;
    char    *title;
    uint16_t total;
    uint16_t cap;                /* entries in `tasks` */
    oc_todo_task_view *tasks;
} oc_todo_view;
void oc_todo_view_free(oc_todo_view *t);

/* net thread -> UI thread */
enum {
    OC_EV_CONNECTED = 1,   /* TLS + handshake up */
    OC_EV_AUTH_OK,         /* authenticated; user_id set */
    OC_EV_AUTH_BROWSER,    /* a browser sign-in is waiting: body = the URL to open */
    OC_EV_AUTH_DEVICE,     /* a device code is waiting (AUTH.md §8.11): body = the URL, topic =
                            * the code to enter, preview = the daemon's fingerprint in hex where
                            * no authority vouches for it (else ""), count = seconds it lasts */
    OC_EV_CERT_UNTRUSTED,  /* the server's certificate is not one to trust: body = its
                            * SHA-256 in hex; der = the certificate; status = 1 if it
                            * replaced one trusted before */
    OC_EV_SIGNED_OUT,      /* the session was refused and there is nothing to fall back on:
                            * the person has to sign in again, and the thread has stopped */
    OC_EV_WORKSPACE_INFO,  /* a WORKSPACE_INFO: status=deployment_mode, count=max_users, body=name, ws_mode, size=max_video_size, ws_audience */
    OC_EV_MESSAGE,         /* a BROADCAST: channel/author/message_id/time + body */
    OC_EV_CHANNEL,         /* a CHANNEL_LIST entry / CHANNEL_INFO: channel_id + name(body)
                              + status, plus topic (author_name) and archived (REQ-034/035) */
    OC_EV_PRESENCE,        /* a PRESENCE_UPDATE: user_id + status */
    OC_EV_REACTION,        /* a REACTION_UPDATED: channel/message/user + emoji/op/count */
    OC_EV_EDIT,            /* a MSG_EDITED: channel/message + new body */
    OC_EV_DELETE,          /* a MSG_DELETED: channel/message tombstone */
    OC_EV_RESTORE,         /* a MSG_RESTORED: channel/message back, `body`; its files follow as ATTACH */
    OC_EV_TYPING,          /* a TYPING_UPDATE: user_id is typing in channel_id */
    OC_EV_THREAD_REPLY,    /* a THREAD_REPLY: parent_id/message + body + count */
    OC_EV_THREAD_END,      /* a THREAD: the LIST_THREAD replay is complete (parent_id) */
    OC_EV_THREAD_META,     /* a THREAD_META: message_id + reply count (backfill) */
    OC_EV_SEARCH_RESULT,   /* a SEARCH_RESULTS entry: channel/message/author + snippet(body) */
    OC_EV_REACTIONS,       /* a REACTIONS entry: message_id + one reactor (user_id + emoji) */
    OC_EV_DND,             /* a NOTIFY_PREFS header (frame start): status=enabled, count=(start<<16|end) */
    OC_EV_SNOOZE,          /* a SNOOZE: server_time = when the pause ends, 0 = not paused (REQ-278) */
    OC_EV_READ_RECEIPTS,   /* READ_RECEIPTS: status = 1 if I share my read receipts (REQ-090) */
    OC_EV_SCHEDULE,        /* a SCHEDULE frame: the recurring allowed-hours schedule (REQ-136) */
    OC_EV_ALERT_PREFS,     /* an ALERT_PREFS frame: my keywords + priority people (REQ-135) */
    OC_EV_MUTED_USERS,     /* a MUTED_USERS frame: everyone I have muted, in `ids`/`n_ids` */
    OC_EV_BLOCKS,          /* a BLOCKS frame: whom I block (`ids`/`n_ids`), my DM privacy in `status` */
    OC_EV_BLOCK_LIST,      /* a BLOCK_LIST: per block, blocker, blocked and since, three to an entry in `ids` */
    OC_EV_THREAD_SUMMARY,  /* one aggregated thread (REQ-062): a list entry AND the push */
    OC_EV_THREADS_END,     /* the thread list's terminator */
    OC_EV_NOTIFY_PREF,     /* a NOTIFY_PREFS entry: channel_id + level(op) */
    OC_EV_USER_UPDATED,    /* a USER_UPDATED: user_id + role(status) + disabled(op) */
    OC_EV_INVITE,          /* an INVITE_CREATED: body=token, op=role, server_time=expires_at */
    OC_EV_CREDENTIAL_RESET,/* a CREDENTIAL_RESET: user_id, body=the reset's token */
    /* One outstanding invite (message_id=invite_id, op=role,
     * server_time=expires_at, user_id=created_by); END terminates a list; REVOKED is
     * the ack. No token: only its hash is stored, so a list cannot carry one. */
    OC_EV_INVITE_ROW,
    OC_EV_INVITE_END,
    OC_EV_INVITE_REVOKED,
    /* A PROFILE_INFO (53): user_id + the fields, carried in the ev's slices. */
    OC_EV_PROFILE_INFO,
    /* one census row (channel_id + count in `count`); BEGIN clears. */
    /* REQ-182: one live session (message_id=id, server_time=created, pinned_at=last
     * seen, channel_id=expiry, op=is-current, body=device label); END terminates. */
    OC_EV_SESSION_ROW,
    OC_EV_SESSIONS_END,
    OC_EV_FILE_CHANNELS_BEGIN,
    OC_EV_FILE_CHANNEL,
    OC_EV_USER,            /* a USER_LIST entry: user_id + name(body) + role(status) + disabled(op) */
    OC_EV_WEBHOOK_INFO,    /* a WEBHOOK_INFO: message_id=webhook_id + channel_id + body=token (shown once) */
    OC_EV_WEBHOOK,         /* a WEBHOOK_LIST entry: message_id=webhook_id + channel_id + body=label + op=disabled */
    OC_EV_WEBHOOK_DELETED, /* a WEBHOOK_DELETED: message_id=webhook_id */
    OC_EV_ATTACHMENT,      /* a message's attachment: channel_id + message_id + parent_id=attachment_id + server_time=size + body=filename + author_name=mime */
    OC_EV_ATTACHMENT_DATA, /* in-memory download finished: message_id=attachment_id, count=bytes, body=the bytes (NOT a C string), op=the preview rung asked for (0: the original was) */
    OC_EV_XFER,            /* a transfer notice: op=phase (0 progress, 1 done, 2 error), body=status text
                            * (NULL on a bare progress tick), xfer_tag/xfer_done/xfer_total */
    OC_EV_MEDIA_POSTED,    /* a video message went out: xfer_tag, attach_id = the video, channel_id */
    OC_EV_READ_STATE,      /* channel_id: mark its currently-loaded messages read (replayed cache is not "unread") */
    /* Custom emoji (REQ-072): BEGIN clears the catalogue, then one per entry —
     * the same shape as the settings snapshot below, and for the same reason: the
     * server's list is the whole truth. */
    OC_EV_EMOJI_BEGIN,
    OC_EV_EMOJI,
    OC_EV_SETTINGS_BEGIN,  /* a CLIENT_SETTINGS frame start: clears the synced bucket before its entries */
    OC_EV_SETTING,         /* one synced setting: author_name=key, body=value */
    OC_EV_PROFILE,         /* a PROFILE_UPDATED: user_id + body=display_name (own = the change ack) */
    OC_EV_READ_CURSOR,     /* a READ_CURSOR: user_id read up to message_id in channel_id (seen-by) */
    OC_EV_STORAGE,         /* a STORAGE_STATUS: usage + policy report (REQ-214) */
    OC_EV_USAGE,           /* a USAGE: the workspace's usage figures (REQ-043): `usage` */
    OC_EV_AUDIT_BEGIN,     /* an AUDIT_PAGE starts: clears the model's page */
    OC_EV_AUDIT,           /* one audit entry (REQ-251) */
    OC_EV_SRVALERTS,       /* an ALERTS_SUMMARY (REQ-263): count = unacknowledged, srvalert_current */
    OC_EV_SRVALERT_BEGIN,  /* an ALERTS answer starts: clears the model's list */
    OC_EV_SRVALERT,        /* one entry of it: `srvalert` */
    OC_EV_PIN,             /* a PIN_UPDATED: channel/message + user_id=pinner, op=add/remove,
                              server_time=when it was pinned (REQ-230) */
    OC_EV_PINNED_MSG,      /* one entry of a pins list: message/author/body + pinner in user_id */
    OC_EV_PINS_END,        /* the pins list is complete: channel_id + count */
    OC_EV_CHAN_MEMBER,     /* one channel member: channel_id + user_id + status=role,
                              server_time=joined_at (REQ-031) */
    OC_EV_CHAN_MEMBERS_END,/* the member list is complete: channel_id + count */
    OC_EV_FILE,            /* one shared file (REQ-143): attach_id + its own channel_id +
                              message_id it was posted with + author_id=uploader +
                              size/reclaimed + body=filename, emoji=mime */
    OC_EV_FILES_END,       /* the files list is complete: channel_id + count */
    OC_EV_SAVED_UPDATED,   /* a message was saved/unsaved: message_id + op (REQ-231) */
    OC_EV_COLLAPSED,       /* a message folded away (op 1) or opened (op 0) for me: message_id + op */
    OC_EV_FOUND_AT_TIME,   /* the message at a time (jump to a date): channel_id, parent_id,
                              message_id (0 none), server_time, op = 1 at or after the time */
    OC_EV_MESSAGE_MOVED,   /* a MESSAGE_MOVED (REQ-053): message_id, channel_id = where it was,
                              to_channel, parent_id = the thread it went into (0 a top
                              level), count = how many moved */
    OC_EV_SAVED_MSG,       /* one saved item (streamed) */
    OC_EV_SAVED_END,       /* the saved list is complete: count */
    /* Drafts (REQ-223). One draft — a list entry AND the device-sync push, which
     * the model folds identically: channel_id, message_id = thread root,
     * server_time = updated_ms, body (empty means it is gone). END completes a
     * LIST and lets the model drop anything the server did not mention. */
    OC_EV_DRAFT,
    OC_EV_DRAFTS_END,
    /* One scheduled message — a list entry AND the push, folded identically:
     * message_id = its id, channel_id, server_time = send_at_ms, op = state,
     * body, author_name = the failure reason when there is one. */
    OC_EV_SCHEDULED,
    OC_EV_SCHEDULED_END,
    OC_EV_ACTIVITY,        /* one activity item: status=kind, user_id=actor (REQ-139) */
    OC_EV_ACTIVITY_END,    /* the feed is complete: count + pinned_at = seen watermark */
    OC_EV_DISCONNECTED,    /* connection dropped/closed */
    OC_EV_BACKOFF,         /* next reconnect attempt: server_time = deadline (ms), 0 = clear */
    OC_EV_ERROR,           /* protocol/transport error; body = human message,
                            * err_kind = OC_CERR_*, err_code = the daemon's OC_ERR_* */
    /* REQ-287: people you named who are not in this channel. body = their names,
     * comma-joined for display; `peers`/`n_peers` carry their ids so the client
     * can offer to add them; status packs can_add|is_private. */
    OC_EV_MENTION_UNRESOLVED,
    /* A link preview (REQ-222, ARCH-105): channel_id + message_id, body = the
     * url, topic = the fetched title, preview = the description — reusing the
     * three heap slots the way ATTACHMENT reuses author_name for a mime type.
     * Arrives after the BROADCAST it belongs to (or on replay), and again for
     * the same url if the daemon re-fetched: the fold is an upsert. */
    OC_EV_UNFURL,
    OC_EV_FORWARD,
    OC_EV_ACTION,     /* a message's action (REQ-058): its own frame, or a replay's */
    /* A poll's definition and tallies (REQ-225): channel_id + message_id, the
     * poll in `poll`. After the message it belongs to, live or replayed. */
    OC_EV_POLL,
    OC_EV_TODO,            /* a to-do list's state: channel_id, message_id, todo (one range) */
    /* One row of a who-voted answer: message_id, user_id, `poll_option`. */
    OC_EV_POLL_VOTERS,
    /* Read-aloud (REQ-291-295, ARCH-111). TTS_INFO replaces what the client knows
     * about the daemon's speech: a BEGIN, then one VOICE per voice, as the emoji
     * and settings lists do. body on BEGIN is the model version, topic the
     * preview sentence; on VOICE, body is the id, topic the label and preview
     * the BCP 47 language it speaks -- the third slot, borrowed the way
     * OC_EV_UNFURL borrows its own. */
    /* The features this daemon offers, by name (CAPABILITIES): body is the names
     * joined by commas, and replaces what was known -- sent on every connection. */
    OC_EV_CAPABILITIES,
    OC_EV_WORKSPACE_SETTINGS,  /* body: "key\x1fvalue\x1e..." -- all of them, replacing what was held */
    OC_EV_TTS_BEGIN,
    OC_EV_TTS_VOICE,
    /* One message's speech has arrived, ready to play: message_id, count=bytes,
     * body=the MP4 (NOT a C string). The model owns it from here. */
    OC_EV_LISTEN_AUDIO,
    /* A voice's audition has arrived, ready to play: body = the MP4 (owned by the
     * event until the model takes it), count = its length. */
    OC_EV_VOICE_PREVIEW,
    /* ...or it will not: message_id, status = the reason code (0 = nothing to
     * say). Talking mode moves on rather than stopping. */
    OC_EV_LISTEN_SKIP,
    /* Voice input (ARCH-112). STT_INFO: body = model version, topic = language,
     * count = the longest segment in ms (0 = not offered). */
    OC_EV_STT_INFO,
    /* A segment answered: count = segment id, op = mode, channel_id and
     * parent_id (the thread root) = where it was spoken, message_id = the
     * message free talk posted (0 in push to talk or when nothing was heard),
     * body = the words. */
    OC_EV_STT_TEXT,
    /* A segment refused: count = segment id, op = mode, status = the reason
     * code (it does not fit a uint8_t, so it rides `size`). */
    OC_EV_STT_ERROR,
    /* Calls (REQ-150, REQ-301-305, CALLS.md §1). STATE: a call for Calls
     * home, in `call` (ended set when it is over). JOINED: this client is in
     * the call in `call`. ROSTER: that call's participants changed. LEFT: this
     * client is in no call any more (left, ended, moved to another device);
     * call_id says which. ERROR: a call request refused, the reason code in
     * `size`. REJOINING: put out of the call call_id by something other than
     * its own act -- the connection, a sweep, a moved media path -- and getting
     * back in; a JOINED ends it, or LOST, which says it could not be done
     * (PROTOCOL.md §5.17). */
    OC_EV_CALL_STATE,
    OC_EV_CALL_JOINED,
    OC_EV_CALL_ROSTER,
    OC_EV_CALL_LEFT,
    OC_EV_CALL_ERROR,
    OC_EV_CALL_REJOINING,
    OC_EV_CALL_LOST,
    /* CALL_HISTORY: a page of this user's calls (`call_hist`), newest first.
     * CALL_PAGE: one call's page (`call_page`). CALL_DELETED: an admin deleted
     * call_id's transcript (op OC_CALL_DEL_TRANSCRIPT) or its recording
     * `message_id` (op OC_CALL_DEL_RECORDING). CALLS.md §1, §8. */
    OC_EV_CALL_HISTORY,
    OC_EV_CALL_PAGE,
    OC_EV_CALL_DELETED,
    /* CHANNEL_DESCRIPTION (REQ-034): channel_id + description in body ("" = none).
     * The answer to a fetch and the announcement of a change are one frame. */
    OC_EV_CHANNEL_DESCRIPTION,
    /* User groups (REQ-307-309). GROUP carries `group` whole; GROUP_DELETED names
     * message_id; GROUPS_END closes a list (count); CHANNEL_GROUPS gives a
     * channel's groups in `groups`/`n_groups`. */
    OC_EV_GROUP,
    OC_EV_GROUP_DELETED,
    OC_EV_GROUPS_END,
    OC_EV_CHANNEL_GROUPS,
    /* After a member list: who is in the channel only through a group
     * (REQ-309), in `ids`/`n_ids`. */
    OC_EV_CHANNEL_VIA_GROUP,
    /* After MENTION_UNRESOLVED: how many the message could not reach in all
     * (count), beyond the names it listed (REQ-308). */
    OC_EV_MENTION_UNRESOLVED_MORE,
    /* How this workspace signs people in, from the AUTH_CHALLENGE of every
     * connection: `count` holds one bit per source kind, 1u << OC_SOURCE_*, and
     * body the ids of its browser sources, each ended by "\n". */
    OC_EV_SIGNIN_SOURCES,
    /* The workspace moved (WORKSPACE_ADDRESS): what was kept is filed under the
     * new key, in `body`, and the next connection dials it. A frontend showing the
     * workspace by its old key relabels it. */
    OC_EV_WORKSPACE_MOVED,
    /* SERVER_BUILD: the daemon's build id, in `body`. */
    OC_EV_SERVER_BUILD,
    /* SUMMARY (REQ-310): the answer to one SUMMARIZE, named by `req_id`.
     * `status` is OC_SUM_*; `body` the summary as JSON when OK, otherwise a
     * sentence saying why. `channel_id`, `span_start`/`span_end` and
     * `summary_id` as the daemon sent them. */
    OC_EV_SUMMARY,
    /* SUMMARY_QUEUED: where the SUMMARIZE named by `req_id` stands; `count` is
     * how many requests are ahead of it, 0 once it is being made. */
    OC_EV_SUMMARY_QUEUED,
    /* SUMMARY_READY: a summary made while its asker was not watching.
     * `summary_id` is the notice, `channel_id`, `sum_scope`, `span_start`/
     * `span_end` what was asked, `status` whether it was made, `made_at` when. */
    OC_EV_SUMMARY_READY,
    /* PDM: an encrypted DM's session (PRIVATE-DM.md, client/core/pdm.h).
     * `channel_id` is the DM's channel, `user_id` the other person, `pdm_kind`
     * what happened, `pdm_safety` the safety number, `pdm_reason` why it ended.
     * What is said in it comes as in any DM (OC_EV_MESSAGE and the rest). */
    OC_EV_PDM,
    /* PROFILE_FIELDS: the workspace's own profile fields, `pfs[0..n_pfs)`
     * with `text` the label, in position order. */
    OC_EV_PROFILE_FIELDS,
    /* Agents and their tokens (docs/details/MCP.md §3.1). AGENT_TOKEN: a token
     * minted, the only time it is sent -- `user_id` the agent, `message_id` the
     * token's id, `body` the token, `topic` its scopes. AGENT_TOKENS_BEGIN:
     * `user_id`'s list follows, replacing what was held. AGENT_TOKEN_ROW: one
     * token of it, never the token -- `message_id` its id, `body` its name,
     * `topic` its scopes, `author_id` who minted it, `created_at`, `server_time`
     * its last use (0 never), `preview` where from. */
    OC_EV_AGENT_TOKEN,
    OC_EV_AGENT_TOKENS_BEGIN,
    OC_EV_AGENT_TOKEN_ROW,
    /* A build's release notes (PROTOCOL.md §5.16r): `size` central's status
     * (200, 404), or 0 when they could not be fetched; `body` its body. */
    OC_EV_RELEASE_NOTES,
    /* A person's AI clients (AUTH.md §8.13). CONNECTION_REQUEST: what a code
     * names -- `preview` the code, `body` the client's name, `topic` where it
     * returns. CONNECTION_DECIDED: `status` 1 approved, 0 declined, `body` the
     * client's name. CONNECTIONS_BEGIN: a list follows, `status` 1 everyone's,
     * replacing what was held. CONNECTION_ROW: one -- `message_id` its id,
     * `user_id` whose, `topic` their name, `body` the client's, `created_at`,
     * `server_time` its last use (0 never), `parent_id` its end (0 none),
     * `count` its lifetime in days (0 never). */
    OC_EV_CONNECTION_REQUEST,
    OC_EV_CONNECTION_DECIDED,
    OC_EV_CONNECTIONS_BEGIN,
    OC_EV_CONNECTION_ROW,
    /* Outgoing webhooks (docs/details/WEBHOOKS.md). OUTWH_SECRET: a webhook
     * saved, the only time its secret is sent -- `message_id` its id, `body`
     * the secret. OUTWH_BEGIN: the list follows, replacing what was held.
     * OUTWH_ROW: one webhook, never its secret -- `message_id` its id, `body`
     * its address, `topic` its events, `preview` why it was turned off,
     * `status` 1 on, `author_id` who made it, `created_at`, `ids` its
     * channels. */
    OC_EV_OUTWH_SECRET,
    OC_EV_OUTWH_BEGIN,
    OC_EV_OUTWH_ROW
};

/* A workspace profile field (OC_EV_PROFILE_FIELDS: `text` the label), or a
 * person's value for one (OC_EV_PROFILE_INFO: `id` the field, `text` the value). */
typedef struct { uint64_t id; uint8_t kind; uint16_t position;
                 char text[OC_PROFILE_FIELD_VALUE_MAX + 1]; } oc_ev_pf;

/* What OC_EV_PDM reports, in its pdm_kind. The DM's id is `channel_id`, the other
 * person `user_id`. */
enum {
    OC_PDM_EV_INVITED = 1,   /* someone asks for one; accept or decline */
    OC_PDM_EV_OPENING,       /* this device asked; waiting for the other device */
    OC_PDM_EV_OPEN,          /* the handshake is done: `pdm_safety` is the safety number */
    OC_PDM_EV_KEY_CHANGED,   /* done, but the other device's key is not the one remembered;
                                nothing is sent until the person trusts it (OC_CMD_PDM_TRUST) */
    OC_PDM_EV_ENDED = 7,     /* ended, `pdm_reason` an OC_PDM_END_* */
};

typedef struct {
    int      type;
    uint64_t user_id;
    uint64_t channel_id;
    uint64_t author_id;
    uint64_t message_id;
    uint64_t parent_id;    /* THREAD_REPLY: the parent message this replies to */
    uint64_t to_channel;   /* MESSAGE_MOVED: the channel it went to */
    uint64_t remind_at;    /* SAVED_UPDATED, SAVED_MSG: the reminder (REQ-283), 0 for none */
    uint64_t server_time;
    uint8_t  status;       /* PRESENCE: online/away/offline; CHANNEL: joined flag */
    uint8_t  is_public;    /* CHANNEL: 1 public, 0 private/DM */
    oc_storage_view storage;  /* OC_EV_STORAGE */
    oc_audit_view   audit;    /* OC_EV_AUDIT */
    oc_srvalert_view *srvalert; /* heap; OC_EV_SRVALERT */
    oc_usage_view   *usage;     /* heap; OC_EV_USAGE */
    uint32_t        srvalert_current; /* OC_EV_SRVALERTS: how many still hold */
    uint8_t  op;           /* REACTION: add/remove */
    uint32_t count;        /* REACTION: running aggregate count for the emoji */
    /* THREAD_META: the thread's last reply time (in server_time) and its most
     * recent distinct repliers, newest first (REQ-060). */
    uint8_t  n_repliers;
    uint64_t repliers[OC_THREAD_REPLIERS_MAX];
    uint16_t uses, used;   /* INVITE / INVITE_ROW: how many may redeem it (0 = no limit), how many have */
    uint64_t pinned_at;    /* PINNED_MSG: when it was pinned (REQ-230). Its own field
                              because a ms timestamp does not fit in `count`. */
    char    *topic;        /* heap; CHANNEL: the channel topic, NULL = none (REQ-034).
                              Its own field because a topic is up to 250 bytes and
                              every fixed buffer here is smaller. */
    uint64_t muted_until_ms; /* NOTIFY_PREF: when a timed mute ends, 0 for none (REQ-137) */
    uint8_t  also_channel; /* THREAD_REPLY: the reply is in the channel's main
                              scroll too (REQ-062); MESSAGE carries such a reply's
                              thread in `parent_id` */
    uint8_t  participant;  /* THREAD_REPLY: this user is in the thread (REQ-061).
                              Derived server-side (ARCH-104) because the client
                              holds a thread's replies only while it is open. */
    uint8_t  broadcast_stored; /* MESSAGE, RESTORE, THREAD_REPLY: the daemon
                              stored a broadcast mention row for the message, so
                              an @here, @channel or @everyone in it notifies
                              (REQ-221) */
    uint8_t  archived;     /* CHANNEL: the channel is archived (REQ-035) */
    uint8_t  e2e;          /* CHANNEL: an encrypted DM (PRIVATE-DM.md) */
    uint8_t  ws_mode;      /* WORKSPACE_INFO: OC_WS_MODE_* (a public demo, AUTH.md §8.12) */
    uint64_t created_at;   /* CHANNEL (from CHANNEL_INFO): when it was created */
    char    *preview;      /* heap; CHANNEL: newest message text (REQ-050 sidebar) */
    uint64_t preview_author;
    /* CHANNEL: a GROUP DM's participants (REQ-056). Empty for everything else; the
     * single peer of a 1:1 DM stays in `user_id`. */
    uint64_t peers[9];
    uint16_t n_peers;
    uint64_t size;         /* FILE: byte size (REQ-143) */
    uint64_t attach_id;    /* FILE: the attachment's own id */
    uint8_t  reclaimed;    /* FILE: bytes reclaimed; the row is a tombstone */
    char     emoji[40];    /* REACTION: the emoji */
    char     author_name[64]; /* MESSAGE: author display name ("" = fall back to id) */
    char     via[104];     /* MESSAGE / THREAD_REPLY: the AI client it came through, "" for none */
    char    *body;         /* heap; MESSAGE/ERROR/CHANNEL(name) only, else NULL */
    char     ws_audience[132]; /* WORKSPACE_INFO: the workspace's audience id, "" when not enrolled */
    /* One aggregated thread (REQ-062). `count`/`op` carry the reply and unread
     * counts, `pinned_at` the last reply's time — reusing the generic fields the
     * way every other list event here does. */
    /* FORWARD (REQ-057): the reference the daemon resolved for a forwarded
     * message. `message_id`/`channel_id` above name the forward itself;
     * `author_id` is the SOURCE's author. The excerpt rides `body`, which is
     * already the heap-string field every text-bearing event uses. */
    uint64_t src_channel, src_message;
    uint16_t src_n_attach;
    char     src_attach_name[128];   /* the first file's name; "" = none */
    /* An action (REQ-058, ARCH-115). On ACTION: the record itself. On MESSAGE,
     * EDIT, RESTORE and THREAD_REPLY: the action the daemon paired with the
     * message, set by the net thread before the event is handed over, so the
     * model never holds a live message without it (`action_known` 1). On an
     * excerpt event (CHANNEL, SEARCH, ACTIVITY, THREAD, PINNED, SAVED,
     * FORWARD): 1 in `action` when the excerpt is an action's text. */
    uint8_t  action, action_known;
    uint32_t action_start, action_len;
    char     actor_name[64];
    uint32_t reply_count, unread_count;
    uint8_t  following;
    /* REQ-289: the profile fields carried on a USER entry. `emoji` and
     * `author_name` already exist and carry the custom status's two halves. */
    char     pf_title[64];
    char     pf_tz[48];
    char     pf_full_name[64];
    char     pf_pronouns[32];
    char     pf_voice[32];   /* the voice this person is read aloud in (REQ-292) */
    /* PROFILE_INFO only — a phone number is not on the roster. */
    char     pf_phone[40];
    /* The recurring schedule (REQ-136) and the two alert lists (REQ-135). Inline
     * arrays: both are wire-capped and small, and a fixed array needs no
     * ownership rules — the same reasoning `peers` and `gids` already use. */
    oc_schedule_day sched_days[OC_SCHEDULE_DAYS];
    uint8_t  n_sched_days;
    uint8_t  sched_mode;
    int16_t  tz_offset_min;
    uint16_t sched_start_min, sched_end_min;
    char     kw_terms[OC_MAX_KEYWORDS][OC_KEYWORD_MAX];
    uint8_t  n_kw_terms;
    uint64_t pri_people[OC_MAX_PRIORITY];
    uint8_t  n_pri_people;
    /* ATTACHMENT / FILE: a video message's media row (REQ-165); media_kind 0 for
     * an ordinary file. */
    uint8_t  media_kind;
    uint32_t duration_ms;
    uint16_t media_w, media_h;
    uint64_t poster_id;
    /* XFER / MEDIA_POSTED: which queued transfer (the tag its command carried),
     * and bytes moved of the total. */
    uint64_t xfer_tag, xfer_done, xfer_total;
    uint8_t  xfer_file;   /* XFER: which file of a POST_FILES job is moving (0 otherwise) */
    oc_call_view *call;   /* heap; the CALL_* events */
    oc_call_history_view *call_hist;   /* heap; OC_EV_CALL_HISTORY */
    oc_call_page_view    *call_page;   /* heap; OC_EV_CALL_PAGE */
    uint64_t call_id;     /* CALL_LEFT / CALL_REJOINING / CALL_LOST: which call */
    oc_group_view *group; /* heap; OC_EV_GROUP */
    oc_poll_view *poll;   /* heap; OC_EV_POLL */
    oc_todo_view *todo;   /* heap; OC_EV_TODO: one frame's range of a list */
    uint16_t todo_first;
    uint8_t  todo_n;
    uint8_t  poll_option; /* POLL_VOTERS */
    uint64_t groups[OC_MAX_CHANNEL_GROUPS];   /* CHANNEL_GROUPS */
    uint16_t n_groups;
    uint64_t *ids;        /* heap; CHANNEL_VIA_GROUP */
    size_t    n_ids;
    uint8_t  user_system; /* USER: the workspace's own sender (OC_USER_SYSTEM), not a person */
    uint8_t  user_agent;  /* USER: an agent (OC_USER_AGENT), not a person */
    uint8_t  msg_kind;    /* MESSAGE: OC_MSG_KIND_* (a call event, REQ-304) */
    uint8_t *der;         /* heap; CERT_UNTRUSTED: the certificate (DER) */
    size_t    der_len;
    uint32_t req_id;      /* SUMMARY: the request it answers */
    uint64_t summary_id, span_start, span_end;   /* SUMMARY */
    uint8_t  sum_scope;   /* SUMMARY_READY */
    uint64_t made_at;     /* SUMMARY_READY */
    uint8_t  pdm_kind, pdm_reason;   /* PDM */
    char     pdm_safety[72];         /* PDM: OPEN, KEY_CHANGED */
    uint8_t  err_kind;               /* ERROR: OC_CERR_* */
    uint16_t err_code;               /* ERROR: the daemon's OC_ERR_* it answers, 0 if none */
    int64_t  rank;                   /* SEARCH_RESULT: the row's relevance (protocol.h, oc_search) */
    oc_ev_pf *pfs;                   /* heap; PROFILE_FIELDS, and PROFILE_INFO's values */
    uint8_t   n_pfs;
    /* USER and PROFILE_INFO: out of office (REQ-122), as stored. */
    uint64_t ooo_start, ooo_end;
    char     ooo_message[OC_OOO_MESSAGE_MAX + 1];
    /* CHANNEL from CHANNEL_INFO (REQ-036b): the creator, and the managers in
     * `ids`/`n_ids`; `ch_managers_known` says this event carries them, since a
     * CHANNEL_LIST entry does not. */
    uint8_t  ch_managers_known;
    uint64_t ch_creator;
    /* SETTINGS_BEGIN: how many SETTING events follow it, so the model knows
     * when the snapshot is whole. */
    uint16_t n_settings;
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
    OC_CMD_PIN,            /* pin/unpin `message_id` in `channel_id`: op = add/remove */
    OC_CMD_LIST_PINS,      /* list `channel_id`'s pinned messages */
    OC_CMD_UPDATE_CHANNEL, /* set topic / rename / archive / unarchive `channel_id`:
                              op = OC_CHUP_*, body = the new topic or name (REQ-034/035/036) */
    OC_CMD_SAVE_ITEM,      /* save/unsave `message_id`: op = add/remove (REQ-231) */
    OC_CMD_SET_COLLAPSED,  /* fold `message_id` away for me (op 1) or open it (op 0) */
    OC_CMD_FIND_AT_TIME,   /* the message at `server_time` (ms) in `channel_id`, in the thread
                              `message_id` or its main scroll (0) */
    OC_CMD_MOVE_MESSAGE,   /* move `message_id` to `channel_id`, into thread `parent_id`
                              (0 its top level); op 1 leaves a line where it was (REQ-053) */
    OC_CMD_LIST_SAVED,     /* my saved items */
    OC_CMD_LIST_ACTIVITY,  /* what involved me — or what I have not read: op = OC_ACTF_* (REQ-139) */
    OC_CMD_HISTORY_AROUND, /* the messages around `message_id` in `channel_id` (REQ-232) */
    OC_CMD_LIST_MEMBERS,   /* list `channel_id`'s members (REQ-031) */
    OC_CMD_LIST_FILES,     /* list files in `channel_id` (0 = everywhere I can read) */
    OC_CMD_LIST_REACTIONS,  /* inspect who reacted to `message_id` in `channel_id` */
    OC_CMD_SET_NOTIFY_PREF, /* set `channel_id`'s notification level (op = level) */
    OC_CMD_SET_DND,         /* set the DND window: op = enabled, channel_id = start_min, message_id = end_min */
    OC_CMD_SET_SNOOZE,      /* pause notifications: message_id = minutes from now, 0 ends it (REQ-278) */
    OC_CMD_SET_READ_RECEIPTS, /* share my read receipts: message_id = 1, or 0 to stop (REQ-090) */
    OC_CMD_SET_SCHEDULE,    /* the recurring schedule (REQ-136), carried in cmd->sched */
    OC_CMD_SET_KEYWORDS,    /* replace my keyword list (REQ-135), carried in cmd->list */
    OC_CMD_SET_PRIORITY,    /* replace my priority-people list (REQ-135) */
    OC_CMD_SET_USER_MUTE,   /* mute or unmute one person, for me (mute_user_id, op) */
    OC_CMD_SET_BLOCK,       /* block or unblock one person (block_user_id, op) */
    OC_CMD_SET_DM_PRIVACY,  /* who may start a new DM with me (op) */
    OC_CMD_LIST_BLOCKS,     /* owner/admin: every block in the workspace */
    OC_CMD_LIST_THREADS,    /* threads I am in (REQ-062): op = filter */
    OC_CMD_THREAD_FOLLOW,   /* follow/unfollow: channel_id + message_id = root, op = on */
    OC_CMD_CHANNEL_THREAD_FOLLOW,   /* every thread in channel_id: op = on (REQ-282) */
    OC_CMD_MARK_THREAD_READ,/* its replies are read: message_id = root, server_time = up to */
    OC_CMD_LIST_NOTIFY_PREFS, /* request all notification settings (DND + per-channel) */
    OC_CMD_SET_SETTING,     /* upsert a synced client setting: body=key, body2=value (empty value deletes) */
    OC_CMD_LIST_SETTINGS,   /* request the synced client-settings bucket */
    /* Drafts (REQ-223, ARCH-101). SET carries channel_id + message_id as the
     * THREAD ROOT (0 = the channel itself) + body; an empty body deletes, which
     * is the same command rather than a second one. */
    OC_CMD_SET_DRAFT,
    OC_CMD_LIST_DRAFTS,
    /* Scheduled messages (REQ-224). SCHEDULE: channel_id + message_id as the
     * thread root + server_time as the send time + body. CANCEL/UPDATE use
     * message_id as the scheduled row's id. */
    OC_CMD_SCHEDULE,
    OC_CMD_LIST_SCHEDULED,
    OC_CMD_CANCEL_SCHEDULED,
    OC_CMD_UPDATE_SCHEDULED,
    OC_CMD_SET_DISPLAY_NAME, /* change your own display name: body=name */
    OC_CMD_SET_EMAIL,        /* a local account's own address: body=address ("" clears) */
    OC_CMD_MARK_READ,       /* CLIENT_ACK: read `channel_id` up to `message_id` (drives seen-by) */
    OC_CMD_MARK_ALL_READ,   /* MARK_ALL_READ: every membership, in one frame; no fields */
    OC_CMD_GET_CHANNEL_DESCRIPTION, /* ask for `channel_id`'s description (REQ-034) */
    OC_CMD_STORAGE_STATUS,  /* ask for the storage usage report (owner/admin) */
    OC_CMD_USAGE,           /* ask for the usage figures over `usage_days` days (owner/admin) */
    OC_CMD_SRVALERTS_LIST,  /* ask for the daemon's critical failures (owner/admin) */
    OC_CMD_RESTORE,         /* take a delete back: channel_id, message_id (REQ-052) */
    OC_CMD_SRVALERT_ACK,    /* acknowledge one: message_id = its id, 0 = every one */
    OC_CMD_AUDIT_QUERY,     /* page the audit log (owner/admin): message_id = before_ms */
    OC_CMD_SET_ROLE,        /* set a user's tenant role: channel_id = user_id, op = role */
    OC_CMD_INVITE_USER,     /* invite: op = role, body = the address it is bound to (NULL = a token) */
    OC_CMD_REMOVE_USER,     /* remove/disable a user: channel_id = user_id */
    OC_CMD_DEACTIVATE_SELF, /* deactivate my own account (REQ-033) */
    OC_CMD_RESET_CREDENTIAL,/* a reset link for a local account: channel_id = user_id, op = clear its step */
    OC_CMD_ENABLE_USER,     /* bring a removed member back: channel_id = user_id */
    OC_CMD_SET_VOICE,       /* my read-aloud voice, every other field kept: body = voice id */
    OC_CMD_CREATE_WEBHOOK,  /* mint an incoming webhook for `channel_id`: body = label */
    OC_CMD_LIST_WEBHOOKS,   /* list `channel_id`'s webhooks */
    OC_CMD_DELETE_WEBHOOK,  /* delete a webhook: message_id = webhook_id */
    /* / message_id carries the invite or webhook id; `flag` the
     * desired disabled state for SET_WEBHOOK_STATE. */
    OC_CMD_LIST_INVITES,
    OC_CMD_REVOKE_INVITE,
    OC_CMD_SET_WEBHOOK_STATE,
    OC_CMD_ROTATE_WEBHOOK,
    /* / channel_id + op (muted), and channel_id + message_id. */
    OC_CMD_SET_MUTE,
    OC_CMD_SET_READ_CURSOR,
    /* / body=emoji, body2=text, server_time=expiry / body=title,
     * body2=timezone. */
    OC_CMD_SET_STATUS,
    OC_CMD_SET_PROFILE,
    OC_CMD_GET_PROFILE,          /* a member's whole profile: channel_id = user_id, 0 for mine */
    OC_CMD_LIST_FILE_CHANNELS,   /* */
    OC_CMD_LIST_SESSIONS,        /* REQ-182 */
    OC_CMD_REVOKE_SESSION,       /* sign one of your sessions out: message_id = its id */
    OC_CMD_SET_NOTIFY_DEFAULT,   /* REQ-134: op = level */
    OC_CMD_SET_AVATAR,           /* message_id = attachment id, 0 clears */
    OC_CMD_OPEN_GROUP_DM,        /* REQ-056: gids[0..n_gids) */
    OC_CMD_ADD_EMOJI,            /* REQ-072: body = name, message_id = attachment */
    OC_CMD_DELETE_EMOJI,         /* REQ-072: body = name */
    OC_CMD_RENAME_EMOJI,         /* REQ-072: body = name, body2 = the new name */
    OC_CMD_LIST_EMOJI,
    OC_CMD_UPLOAD,          /* upload+post a file to `channel_id`: body = local path */
    OC_CMD_DOWNLOAD,        /* download an attachment: message_id = attachment_id, body = dest path */
    OC_CMD_LOGOUT,          /* revoke this session (op = scope) and close the connection */
    OC_CMD_CHANNEL_INVITE,  /* add a user to a channel: channel_id + message_id = user id */
    OC_CMD_CHANNEL_KICK,    /* remove a user from a channel: same fields */
    OC_CMD_FETCH,           /* download an attachment INTO MEMORY: message_id = attachment_id */
    OC_CMD_HISTORY,         /* page backwards: channel_id, message_id = before-id (0 = newest) */
    /* Post a video message (REQ-162): channel_id, message_id = thread root (0 = the
     * channel), body = caption, body2 = the file name without extension, blob = the
     * MP4, blob2 = the poster JPEG, media_*.
     * Uploads both, sends ATTACH_MEDIA_SET, then SEND or SEND_REPLY. */
    OC_CMD_POST_VIDEO,
    /* Post a snippet (REQ-226): blob = its text (owned), body2 = title, body3 =
     * language, channel_id, message_id = a thread root or 0. Uploaded as
     * text/plain, marked OC_MEDIA_SNIPPET, then posted, as a video message. */
    OC_CMD_POST_SNIPPET,
    /* Post files as ONE message (REQ-140): paths = the local files (or, with
     * mem, the names of in-memory items), body = the
     * text (may be empty), message_id = a thread root or 0. Uploads each in
     * turn, then one SEND or SEND_REPLY carrying the text and every file. */
    OC_CMD_POST_FILES,
    OC_CMD_CANCEL_TRANSFER, /* xfer_tag: drop it from the queue, or abort it if running */
    /* Fetch one message's speech (ARCH-111): message_id. Queued with the other
     * transfers, so it never overlaps a download. */
    OC_CMD_LISTEN_FETCH,
    /* Hear a voice say the audition sentence (REQ-292): body = voice id. */
    OC_CMD_VOICE_PREVIEW,
    /* One segment of speech (ARCH-112): xfer_tag = segment id, op = mode,
     * channel_id and message_id (the thread root) = the target, blob = the
     * 16 kHz mono PCM as little-endian 16-bit samples. Sent at once, beside and
     * never behind the transfer queue. */
    OC_CMD_STT_SEGMENT,
    /* Calls (REQ-150, REQ-301-305, CALLS.md §1), all on `call_id` but START.
     * START begins a call with subject `body`, asking `uids`, from the
     * conversation `channel_id` (0 for none); JOIN joins one; INVITE asks
     * `uids`; LEAVE, DECLINE and END (the starter's) carry nothing else; SHARE
     * starts (op 1) or stops (op 0) sharing a screen (REQ-161); CAMERA turns
     * the camera on (op 1) or off (op 0), and VIDEO_SUB says whose cameras to
     * receive -- every one (op 1), or uids[0..n_uids) (REQ-160); SUBJECT
     * renames the call to `body`; RECORD starts (op 1) or stops (op 0)
     * recording it (CALLS.md §8). HISTORY asks for a page of this user's calls
     * started before `start_ms` (0 the newest); DETAIL for call_id's page, its
     * transcript from line `req_id`; DELETE (an admin's) deletes its
     * transcript (op OC_CALL_DEL_TRANSCRIPT) or its recording `message_id`
     * (op OC_CALL_DEL_RECORDING). JOIN to DELETE are one range. */
    OC_CMD_CALL_JOIN,
    OC_CMD_CALL_INVITE,
    OC_CMD_CALL_LEAVE,
    OC_CMD_CALL_DECLINE,
    OC_CMD_CALL_END,
    OC_CMD_CALL_SHARE,
    OC_CMD_CALL_CAMERA,
    OC_CMD_CALL_VIDEO_SUB,
    OC_CMD_CALL_START,
    OC_CMD_CALL_SUBJECT,
    OC_CMD_CALL_RECORD,
    OC_CMD_CALL_HISTORY,
    OC_CMD_CALL_DETAIL,
    OC_CMD_CALL_DELETE,
    /* User groups (REQ-307-309). CREATE/UPDATE: body = handle, body2 = name,
     * body3 = description, message_id = the group (UPDATE); DELETE: message_id;
     * ADD/REMOVE_MEMBERS: message_id + uids; CHANNEL_ADD/REMOVE_GROUP:
     * channel_id + message_id = the group. */
    OC_CMD_LIST_GROUPS,
    OC_CMD_CREATE_GROUP,
    OC_CMD_UPDATE_GROUP,
    OC_CMD_DELETE_GROUP,
    OC_CMD_GROUP_ADD_MEMBERS,
    OC_CMD_GROUP_REMOVE_MEMBERS,
    OC_CMD_CHANNEL_ADD_GROUP,
    OC_CMD_CHANNEL_REMOVE_GROUP,
    OC_CMD_SUMMARIZE,      /* summarize `channel_id` over `scope` (REQ-310), as `req_id` */
    OC_CMD_SUMMARY_DETACH, /* stop watching request `req_id`: be told when it is made */
    OC_CMD_SUMMARY_CANCEL, /* cancel request `req_id` */
    OC_CMD_SUMMARY_OPEN,   /* open notice `message_id`, answered as SUMMARY to `req_id` */
    OC_CMD_SUMMARY_DISMISS, /* notice `message_id` seen, unopened */
    /* Polls (REQ-225). POST_POLL: body = the question, message_id the thread
     * parent (0 = the channel), poll_* the rest. VOTE: poll_picks, none to
     * withdraw. LIST_POLL_VOTERS: who picked what on `message_id`. */
    OC_CMD_POST_POLL,
    OC_CMD_VOTE,
    /* To-do lists, each about `message_id` in `channel_id`: a task added
     * (body = name, body2 = description), marked (uses = the task, op = done),
     * or the list renamed (body = the title). */
    OC_CMD_TODO_ADD, OC_CMD_TODO_MARK, OC_CMD_TODO_RENAME,
    OC_CMD_LIST_POLL_VOTERS,
    OC_CMD_EDIT_POLL,      /* the author's poll `message_id`: poll_opts/poll_flags/poll_close_at_ms, all of it */
    /* Encrypted DMs (client/core/pdm.h), a range of their own; what is said in
     * one goes as in any DM (OC_CMD_SEND and the rest). */
    OC_CMD_PDM_OPEN,       /* to the person `channel_id` */
    OC_CMD_PDM_ACCEPT,     /* the invitation `channel_id`, on this device */
    OC_CMD_PDM_DECLINE,
    OC_CMD_PDM_CLOSE,
    OC_CMD_PDM_TRUST,      /* go on with the other device's changed key */
    /* The workspace's own profile fields. SET_PROFILE_FIELD: message_id = the
     * field (0 adds), op = OC_PROFILE_FIELD_SET/DELETE, scope = kind, uses =
     * position, body = label. SET_PROFILE_FIELD_VALUE: message_id = the
     * field, body = my value ("" clears it). */
    OC_CMD_SET_PROFILE_FIELD,
    OC_CMD_SET_PROFILE_FIELD_VALUE,
    /* Out of office (REQ-122): start_ms/end_ms, body = the message; an end of
     * 0 clears it. */
    OC_CMD_SET_OUT_OF_OFFICE,
    /* The playable copy of a video message (ARCH-135), saved to a file the
     * person chose: message_id = the video's attachment id, body = dest path.
     * A queued transfer, as OC_CMD_DOWNLOAD is. */
    OC_CMD_EXPORT,
    /* An image attachment's preview INTO MEMORY (ARCH-137): message_id = the
     * attachment, op = the rung (OC_PREVIEW_RUNG_INLINE_WIRE or _EXPANDED_WIRE).
     * Its bytes arrive as OC_EV_ATTACHMENT_DATA with op = that rung; where the
     * daemon has no preview, the original's bytes do, with the same op. Queued
     * as OC_CMD_FETCH is. */
    OC_CMD_FETCH_PREVIEW,
    /* Agents and their tokens: CREATE_AGENT body = the name, body2 = its first
     * token's name, body3 = the scopes; MINT_AGENT_TOKEN message_id = the
     * agent, body = the token's name, body3 = the scopes; REMOVE_AGENT and
     * LIST_AGENT_TOKENS message_id = the agent; REVOKE_AGENT_TOKEN message_id =
     * the agent, channel_id = the token's id. */
    OC_CMD_CREATE_AGENT,
    OC_CMD_MINT_AGENT_TOKEN,
    OC_CMD_REMOVE_AGENT,
    OC_CMD_LIST_AGENT_TOKENS,
    OC_CMD_REVOKE_AGENT_TOKEN,
    /* RELEASE_NOTES: body = the build id (PROTOCOL.md §5.16r). */
    OC_CMD_RELEASE_NOTES,
    /* A person's AI clients: CONNECTION_CODE body = the code; CONNECTION_DECIDE
     * body = the code, op = approve, message_id = the lifetime in days;
     * LIST_CONNECTIONS op = everyone's; REVOKE_CONNECTION message_id = the
     * connection, op = from everyone's. */
    OC_CMD_CONNECTION_CODE,
    OC_CMD_CONNECTION_DECIDE,
    OC_CMD_LIST_CONNECTIONS,
    OC_CMD_REVOKE_CONNECTION,
    /* Outgoing webhooks: CREATE_OUTWH body = the address, body2 = the events,
     * body3 = the channels' ids, in decimal, separated by spaces; LIST_OUTWH;
     * SET_OUTWH message_id = the webhook, channel_id = 1 on, 0 off;
     * DELETE_OUTWH message_id = the webhook. */
    OC_CMD_CREATE_OUTWH,
    OC_CMD_LIST_OUTWH,
    OC_CMD_SET_OUTWH,
    OC_CMD_DELETE_OUTWH,
    OC_CMD_QUIT
};

typedef struct {
    int      type;
    uint64_t channel_id;
    uint64_t message_id;   /* REACT */
    uint8_t  op;           /* REACT: add/remove */
    uint16_t uses;         /* INVITE_USER: how many may redeem it; 0 = no limit */
    uint64_t server_time;  /* SCHEDULE: when to send (ms) */
    char    *body;         /* heap; SEND body / REACT emoji / SET_SETTING key */
    char    *body2;        /* heap; SET_SETTING value, else NULL */
    char    *body3;        /* heap; CREATE/UPDATE_GROUP description, else NULL */
    /* SEND: what this message forwards (REQ-057), 0/0 when it forwards nothing. */
    uint64_t src_channel, src_message;
    /* OPEN_GROUP_DM (REQ-056): the other participants. Inline, because the wire
     * caps the count and a fixed array needs no ownership rules. */
    uint64_t gids[8];
    int      n_gids;
    /* SET_SCHEDULE / SET_KEYWORDS / SET_PRIORITY (REQ-135/136), all inline and
     * all wholesale — each command carries the whole setting, because that is
     * what the ops are. */
    oc_schedule_day sched_days[OC_SCHEDULE_DAYS];
    uint8_t  n_sched_days;
    uint8_t  sched_mode;
    int16_t  tz_offset_min;
    uint16_t sched_start_min, sched_end_min;
    char     kw_terms[OC_MAX_KEYWORDS][OC_KEYWORD_MAX];
    uint8_t  n_kw_terms;
    uint64_t pri_people[OC_MAX_PRIORITY];
    uint8_t  n_pri_people;
    /* UPLOAD / DOWNLOAD / FETCH / POST_VIDEO: the tag progress and completion
     * events carry, so a frontend can tell its transfers apart. */
    uint64_t xfer_tag;
    /* POST_VIDEO: owned buffers, and the facts ATTACH_MEDIA_SET reports. */
    uint8_t *blob, *blob2;
    size_t   blob_len, blob2_len;
    uint32_t duration_ms;
    uint16_t media_w, media_h;
    /* POST_FILES: one entry per file, capped by the wire's attachment list. An
     * entry is a local file's heap path, or -- where mem[i] is set -- the name an
     * in-memory item uploads under, with mem[i]/mem_len[i] its owned bytes (a
     * pasted image: nothing written to disk, ARCH-88). */
    char    *paths[OC_MAX_ATTACH];
    uint8_t *mem[OC_MAX_ATTACH];
    size_t   mem_len[OC_MAX_ATTACH];
    uint8_t  n_paths;
    /* CALL_START / CALL_INVITE: the people asked. Inline, capped by the wire. */
    uint64_t call_id;     /* the CALL_* commands but START */
    uint64_t uids[OC_MAX_CALL_INVITES];
    uint16_t n_uids;
    /* SUMMARIZE: the request's id, and its span (OC_SUM_UNREAD...; start and
     * end only for a range). */
    uint32_t req_id;
    uint8_t  scope;
    uint64_t start_ms, end_ms;
    /* POST_POLL / VOTE (REQ-225), inline like the alert lists. */
    char     poll_opts[OC_POLL_MAX_OPTIONS][OC_POLL_OPTION_MAX + 1];
    uint8_t  n_poll_opts, poll_flags;
    uint64_t poll_close_at_ms;
    uint8_t  poll_picks[OC_POLL_MAX_OPTIONS];
    uint8_t  n_poll_picks;
    /* SEARCH: the order is `op` (OC_SEARCH_*), the cursor `message_id` and, by
     * relevance, this rank beside it. */
    int64_t  rank;
    uint64_t parent_id;    /* MOVE_MESSAGE: the destination thread, 0 its top level */
    /* REPLY, and POST_FILES / POST_VIDEO into a thread: the reply is posted to
     * the channel's main scroll too (SEND_REPLY's `also_channel`, REQ-062). */
    uint8_t  also_channel;
    /* USAGE: over how many UTC days (1..OC_USAGE_MAX_DAYS). */
    uint16_t usage_days;
    /* SET_USER_MUTE: the person; `op` 1 mutes, 0 unmutes. */
    uint64_t mute_user_id;
    /* SET_BLOCK: the person, `op` 1 blocks and 0 unblocks; SET_DM_PRIVACY: `op`
     * is the value (OC_DM_PRIVACY_*). */
    uint64_t block_user_id;
} oc_cmd;

oc_cmd *oc_cmd_new(int type);
void    oc_cmd_free(oc_cmd *c);

#endif /* OC_EVENT_H */
