/*
 * OpenChime DB-writer thread (ARCH-5) + the write-job queue.
 *
 * The single SQLite write connection is owned by one dedicated thread; the
 * network event loop never touches the database. The net thread submits jobs
 * (AUTH, SEND) via oc_dbwriter_submit and is woken to collect results by an
 * eventfd it polls in epoll (ARCH-22) — keeping all socket I/O on the net
 * thread and all DB writes on this one. On startup the writer opens the
 * database in WAL mode and applies migrations (ARCH-27).
 */

#ifndef OPENCHIME_DBWRITER_H
#define OPENCHIME_DBWRITER_H

#include <stddef.h>
#include <stdint.h>

#include "protocol.h"   /* OC_SESSION_TOKEN_LEN, OC_AUTH_*, OC_ROLE_* */

#define OC_IDEM_LEN 16

/* The stub AUTH auto-provisions this one shared channel and joins every user to
 * it, so the SEND/BROADCAST path has somewhere to deliver. Real channel
 * create/join is a later feature. */
#define OC_DEFAULT_CHANNEL 1

/* --- Jobs (net thread -> writer) ---------------------------------------- */

enum { OC_JOB_AUTH = 1, OC_JOB_SEND = 2, OC_JOB_BACKFILL = 3, OC_JOB_REGISTER = 4,
       OC_JOB_SET_ROLE = 5, OC_JOB_LOGOUT = 6, OC_JOB_EDIT = 7, OC_JOB_DELETE = 8,
       OC_JOB_CREATE_CHANNEL = 9, OC_JOB_LIST_CHANNELS = 10, OC_JOB_JOIN_CHANNEL = 11,
       OC_JOB_LEAVE_CHANNEL = 12, OC_JOB_INVITE_CHANNEL = 13, OC_JOB_REMOVE_CHANNEL = 14,
       OC_JOB_LIST_USERS = 15, OC_JOB_INVITE_USER = 16, OC_JOB_REMOVE_USER = 17,
       OC_JOB_REDEEM = 18, OC_JOB_REACT = 19, OC_JOB_LIST_REACTIONS = 20,
       OC_JOB_SEND_REPLY = 21, OC_JOB_LIST_THREAD = 22, OC_JOB_SEARCH = 23,
       OC_JOB_SETUP_INVITE = 24, OC_JOB_CLIENT_ACK = 25,
       OC_JOB_LOAD_IDENTITY = 26, OC_JOB_STORE_IDENTITY = 27, OC_JOB_OPEN_DM = 28,
       OC_JOB_TYPING = 29,
       /* Pins (REQ-230, ARCH-90). PIN is a write (add/remove); LIST_PINS is a
        * read served on the reader connection. */
       OC_JOB_PIN = 62, OC_JOB_LIST_PINS = 63,
       /* Channel details (REQ-031, REQ-143). Both reads. */
       OC_JOB_LIST_MEMBERS = 64, OC_JOB_LIST_FILES = 65,
       /* Channel mutability (REQ-034/035/036, ARCH-93). */
       OC_JOB_UPDATE_CHANNEL = 66,
       /* Saved items + activity (REQ-231/139, ARCH-95). SAVE is a write; the
        * two listings are reads. */
       OC_JOB_SAVE_ITEM = 67, OC_JOB_LIST_SAVED = 68, OC_JOB_LIST_ACTIVITY = 69,
       /* Attachments (REQ-140/141, ARCH-69/70). CREATE mints a pending row + a
        * storage key on UPLOAD_BEGIN (write); FINALIZE records the streamed
        * size + digest on UPLOAD_END (write); LOOKUP authorizes + fetches the
        * pointer for a download (read). */
       OC_JOB_ATTACH_CREATE = 30, OC_JOB_ATTACH_FINALIZE = 31,
       OC_JOB_ATTACH_LOOKUP = 32,
       /* Incoming webhooks (REQ-170). CREATE_WEBHOOK mints a per-channel token
        * for a client; WEBHOOK_POST resolves a token presented over HTTP and
        * posts the message as the webhook's creator. */
       OC_JOB_CREATE_WEBHOOK = 33, OC_JOB_WEBHOOK_POST = 34,
       OC_JOB_LIST_WEBHOOKS = 35, OC_JOB_DELETE_WEBHOOK = 36,
       /* Notification preferences (REQ-130/131). */
       OC_JOB_SET_NOTIFY_PREF = 37, OC_JOB_SET_DND = 38,
       OC_JOB_LIST_NOTIFY_PREFS = 39,
       /* Audio call join authorization (REQ-150): read job, channel access gate. */
       OC_JOB_CALL_AUTH = 40,
       /* Synced client settings bucket. SET upserts/deletes one key; LIST reads a
        * client_type bucket. Both answer with a CLIENT_SETTINGS snapshot. */
       OC_JOB_SET_CLIENT_SETTING = 41, OC_JOB_LIST_CLIENT_SETTINGS = 42,
       /* Self-service profile (REQ-020): rename yourself / rotate your local
        * password (verifies the old one). Both are writes. */
       OC_JOB_SET_DISPLAY_NAME = 43, OC_JOB_CHANGE_PASSWORD = 44,
       /* Storage maintenance pass (ARCH-78): find what to reclaim. Read-mostly
        * but it tombstones rows, so it runs on the writer. */
       OC_JOB_STORAGE_MAINT = 45,
       /* Storage usage report for an owner/admin (REQ-214). Read-only. */
       OC_JOB_STORAGE_STATUS = 46,
       OC_JOB_AUDIT_QUERY = 47,
       OC_JOB_LOAD_ENROLLMENT = 48, OC_JOB_STORE_ENROLLMENT = 49,
       /* Push device tokens (ARCH-85, REQ-132). REGISTER/UNREGISTER are client
        * writes; PRUNE is submitted by the push worker when central reports a
        * token stale (fire-and-forget, no result). */
       OC_JOB_REGISTER_DEVICE_TOKEN = 50, OC_JOB_UNREGISTER_DEVICE_TOKEN = 51,
       OC_JOB_PRUNE_DEVICE_TOKEN = 52,
       /* Page backwards through one channel's history (§6.3). Read-only; it
        * answers with the same BACKFILL_OK shape the forward replay uses. */
       OC_JOB_HISTORY = 53,
       /* Invite management (REQ-026, WIN-46) and webhook lifecycle (WIN-48). Both
        * read/write tables that already had the columns; only the ops were missing. */
       OC_JOB_LIST_INVITES = 54, OC_JOB_REVOKE_INVITE = 55,
       OC_JOB_SET_WEBHOOK_STATE = 56, OC_JOB_ROTATE_WEBHOOK = 57,
       /* Mute (REQ-137) and mark-unread (REQ-235). The latter is deliberately NOT
        * OC_JOB_CLIENT_ACK: that one may only advance. */
       OC_JOB_SET_MUTE = 58, OC_JOB_SET_READ_CURSOR = 59,
       /* Custom status (REQ-241) and profile fields (REQ-240).
        *
        * Numbered from 70, PAST the current maximum, not into the gap at 54-61: this
        * enum has gaps AND a later block (PIN..LIST_ACTIVITY at 62-69), so "the next
        * free-looking number" collided GET_PROFILE with OC_JOB_PIN. The dispatcher
        * then routed every pin to the profile handler, which the pin tests caught —
        * reading the enum would not have, because the two declarations are 200 lines
        * apart. New jobs go after the highest value, always. */
       OC_JOB_SET_STATUS = 70, OC_JOB_SET_PROFILE = 71, OC_JOB_GET_PROFILE = 72 };

/* Per-channel reconnect cursor: replay messages with id > after_message_id. */
typedef struct { uint64_t channel_id; uint64_t after_message_id; } oc_bf_cursor;

typedef struct oc_job {
    struct oc_job *next;
    int            type;
    uint64_t       conn_id;   /* originating connection, echoed on the result */
    uint64_t       user_id;   /* the authenticated user (for SEND author / backfill auth) */

    /* AUTH */
    uint8_t        method;    /* OC_AUTH_LOCAL / OC_AUTH_OIDC / OC_AUTH_SESSION */
    char          *token;     /* heap; the raw credential bytes (method-specific) */
    size_t         token_len; /* credential length (token has a trailing NUL too) */
    char           source[46];/* peer IP string, for per-source rate limiting ("" if none) */

    /* REGISTER (create a local account; AUTH.md §2 — bootstrap / invite) */
    char          *username;  /* heap */
    char          *password;  /* heap */
    uint8_t        role;      /* OC_ROLE_* for the new account (also SET_ROLE next) */
    uint32_t       iterations;/* PBKDF2 rounds (0 -> OC_PW_ITERATIONS default) */

    /* SET_ROLE (change a user's tenant role; ARCH-60). Actor is `user_id`.
     * Also carries the target for channel INVITE/REMOVE. */
    uint64_t       target_user_id;

    /* CREATE_CHANNEL */
    char          *ch_name;    /* heap */
    uint8_t        ch_is_public;

    /* STORE_IDENTITY (persist the TLS cert+key PEM) */
    char          *cert_pem;   /* heap */
    char          *key_pem;    /* heap */

    /* STORE_ENROLLMENT (CP-8): the federated keypair + audience + state. */
    char          *enroll_privkey;   /* heap */
    char          *enroll_audience;  /* heap */
    int            enroll_active;

    /* REGISTER/UNREGISTER/PRUNE_DEVICE_TOKEN (ARCH-85). */
    char          *device_token;     /* heap */
    uint8_t        device_platform;  /* OC_PUSH_APNS / OC_PUSH_FCM */

    /* REACT (channel_id + message_id above); emoji is heap, op is add/remove. */
    char          *emoji;      /* heap */
    uint8_t        react_op;

    /* PIN (channel_id + message_id above); op is add/remove (REQ-230). */
    uint8_t        pin_op;

    /* UPDATE_CHANNEL (channel_id above): op + the new topic/name in ch_name. */
    uint8_t        chup_op;

    /* SAVE_ITEM (message_id above): add/remove. */
    uint8_t        save_op;
    /* SET_WEBHOOK_STATE's desired state (WIN-48). Its own field rather than reusing
     * one of the *_op flags above: a reader should not have to know that "save_op"
     * secretly means "disabled" for a different job type. */
    uint8_t        hook_disabled;

    /* HISTORY: 0 pages backwards from message_id, 1 fetches AROUND it (ARCH-96). */
    uint8_t        hist_around;

    /* SEARCH: query text is carried in body/body_len; this bounds the result. */
    uint16_t       search_limit;

    /* LOGOUT (revoke sessions; REQ-182). Actor is `user_id`; the token to revoke
     * (scope THIS) is carried in `token`/`token_len`. */
    uint8_t        scope;     /* OC_LOGOUT_THIS / OC_LOGOUT_ALL */

    /* SEND / EDIT / DELETE / SEND_REPLY */
    uint64_t       channel_id;
    uint64_t       message_id; /* target message for EDIT / DELETE */
    uint64_t       parent_id;  /* thread parent for SEND_REPLY / LIST_THREAD */
    uint8_t        idem[OC_IDEM_LEN];
    uint8_t       *body;      /* heap (SEND / EDIT new body) */
    size_t         body_len;
    /* SEND: attachment ids to link to this message (REQ-140). */
    uint64_t       attach_ids[OC_MAX_ATTACH];
    uint16_t       n_attach;

    /* Notification prefs (REQ-130/131). SET_NOTIFY_PREF uses channel_id + level;
     * SET_DND uses the dnd_* fields. */
    uint8_t        notify_level;
    uint8_t        dnd_enabled;
    uint16_t       dnd_start_min, dnd_end_min;

    /* BACKFILL */
    oc_bf_cursor  *cursors;   /* heap */
    size_t         n_cursors;

    /* Attachments (REQ-140). CREATE reads channel_id/user_id/idem + att_size +
     * filename/mime; FINALIZE + LOOKUP read attachment_id; FINALIZE also carries
     * the streamed size (att_size) and digest (att_sha256). */
    uint64_t       attachment_id;
    uint64_t       att_size;
    char          *filename;   /* heap */
    char          *mime;       /* heap */
    uint8_t        att_sha256[32];

    /* Synced client settings. SET uses client_type + key + value (empty value
     * deletes); LIST uses client_type only. */
    char          *cs_client_type; /* heap */
    char          *cs_key;         /* heap */
    char          *cs_value;       /* heap */

    /* Self-service profile (REQ-020). SET_DISPLAY_NAME uses pf_name;
     * CHANGE_PASSWORD uses pf_old_pw + pf_new_pw. */
    char          *pf_name;        /* heap */
    char          *pf_old_pw;      /* heap */
    char          *pf_new_pw;      /* heap */

    /* Storage maintenance pass inputs (ARCH-78). */
    uint64_t       maint_max_age_ms;  /* expire attachments older than this (0 = never) */
    uint64_t       maint_grace_ms;    /* never reclaim anything younger than this */
    uint32_t       maint_batch;       /* cap on rows reclaimed in one pass */
    uint32_t       audit_limit;       /* AUDIT_QUERY: rows per page */
    uint64_t       audit_before_ms;   /* AUDIT_QUERY: page backwards from here (0 = newest) */
    uint64_t       audit_max_age_ms;  /* STORAGE_MAINT: age out audit entries past this */
    int            maint_evict;       /* also evict oldest under pressure (REQ-215) */
} oc_job;

/* --- Results (writer -> net thread) ------------------------------------- */

enum { OC_RES_AUTH_OK = 1, OC_RES_AUTH_ERR = 2, OC_RES_SEND_OK = 3,
       OC_RES_SEND_ERR = 4, OC_RES_BACKFILL_OK = 5,
       OC_RES_REGISTER_OK = 6, OC_RES_REGISTER_ERR = 7,
       OC_RES_SETROLE_OK = 8, OC_RES_SETROLE_ERR = 9,
       OC_RES_LOGOUT_OK = 10, OC_RES_LOGOUT_ERR = 11,
       OC_RES_EDIT_OK = 12, OC_RES_EDIT_ERR = 13,
       OC_RES_DELETE_OK = 14, OC_RES_DELETE_ERR = 15,
       OC_RES_CHANNEL_INFO = 16, OC_RES_CHANNEL_ERR = 17,
       OC_RES_CHANNEL_LIST = 18, OC_RES_USER_LIST = 19,
       OC_RES_INVITE_OK = 20, OC_RES_INVITE_ERR = 21,
       OC_RES_USER_UPDATED = 22, OC_RES_USER_ERR = 23,
       OC_RES_REACTION_OK = 24, OC_RES_REACTION_ERR = 25, OC_RES_REACTIONS = 26,
       OC_RES_REPLY_OK = 27, OC_RES_REPLY_ERR = 28, OC_RES_THREAD = 29,
       OC_RES_SEARCH = 30, OC_RES_IDENTITY = 31, OC_RES_OK = 32,
       OC_RES_TYPING = 33,
       /* Attachments. CREATED: a minted pending row + storage key (net thread
        * opens the blob). ATTACH_OK: an upload finalized. ATTACH_META: an
        * authorized download's pointer + metadata. ATTACH_ERR: err_code. */
       OC_RES_ATTACH_CREATED = 34, OC_RES_ATTACH_OK = 35,
       OC_RES_ATTACH_META = 36, OC_RES_ATTACH_ERR = 37,
       /* Webhooks. CREATED: a minted token for a client. POSTED: a message
        * posted via HTTP (carries SEND-style broadcast fields). ERR: err_code. */
       OC_RES_WEBHOOK_CREATED = 38, OC_RES_WEBHOOK_POSTED = 39,
       OC_RES_WEBHOOK_ERR = 40, OC_RES_WEBHOOK_LIST = 41,
       OC_RES_WEBHOOK_DELETED = 42,
       /* Notification prefs snapshot (also a sync push); ERR on a bad set. */
       OC_RES_NOTIFY_PREFS = 43, OC_RES_NOTIFY_ERR = 44,
       /* Call join authorized (net thread then updates in-memory call state) / denied. */
       OC_RES_CALL_AUTH = 45, OC_RES_CALL_ERR = 46,
       /* Synced client settings bucket snapshot (also fanned as a device-sync push). */
       OC_RES_CLIENT_SETTINGS = 47,
       /* Profile change ok (display name fanned tenant-wide; also the self ack for a
        * password change) / denied (bad old password, etc.). */
       OC_RES_PROFILE_UPDATED = 48, OC_RES_PROFILE_ERR = 49,
       /* Storage keys whose bytes the net thread should hand to the transfer
        * pool for deletion (ARCH-78). */
       OC_RES_STORAGE_MAINT = 51,
       OC_RES_STORAGE_STATUS = 52, OC_RES_STORAGE_ERR = 53,
       OC_RES_AUDIT_PAGE = 54, OC_RES_AUDIT_ERR = 55,
       /* A read cursor advanced (REQ-090 seen-by): fan the acker's new cursor to
        * the channel's members + backfill the acker with the others' cursors. */
       OC_RES_READ_CURSOR = 50, OC_RES_ENROLLMENT = 56,
       /* Device-token register/unregister acknowledged / rejected (ARCH-85). */
       OC_RES_DEVICE_TOKEN_OK = 57, OC_RES_DEVICE_TOKEN_ERR = 58,
       /* Pins (REQ-230). PIN_OK fans out to the channel's members; PINS is the
        * channel's pinned-message list; PIN_ERR carries err_code. */
       OC_RES_PIN_OK = 59, OC_RES_PIN_ERR = 60, OC_RES_PINS = 61,
       /* A channel's member roster / its shared files; ERR carries err_code. */
       OC_RES_MEMBER_LIST = 66, OC_RES_FILE_LIST = 67, OC_RES_LIST_ERR = 68,
       OC_RES_SAVED_OK = 69, OC_RES_SAVED_LIST = 70, OC_RES_ACTIVITY = 71,
       OC_RES_INVITE_LIST = 72, OC_RES_INVITE_REVOKED = 73,
       OC_RES_PROFILE_INFO = 74 };

/* One saved message (REQ-231). Carries its body for the same reason a pin does:
 * a saved message is usually far outside loaded history. */
typedef struct {
    uint64_t message_id, channel_id, author_id, created_at, saved_at;
    char    *body, *attach_name;   /* heap */
} oc_saved_row;

/* One activity item (REQ-139). `text` is the message body for a mention or a
 * reply, and the emoji for a reaction. */
typedef struct {
    uint8_t  kind;
    uint64_t message_id, channel_id, actor_id, at;
    char    *text;                 /* heap */
} oc_activity_row;

/* One row of a channel's member roster (REQ-031). */
typedef struct { uint64_t user_id, joined_at; uint8_t role; } oc_chanmem_row;

/* One shared file (REQ-143, ARCH-91). */
typedef struct {
    uint64_t id, channel_id, message_id, uploader_id, size, created_at;
    uint8_t  reclaimed;
    char    *filename, *mime;   /* heap */
} oc_file_row;

/* One row in a PINS result. The body travels with it because a pinned message
 * is usually scrolled out of the client's loaded history — a list of bare ids
 * would force the client to fetch each one. */
typedef struct {
    uint64_t message_id, author_id, created_at_ms, pinned_by, pinned_at;
    char    *body;         /* heap; NULL for a tombstoned message */
    char    *attach_name;  /* heap; first attachment's filename, else NULL */
} oc_pin_row;

/* One row in a REACTIONS result (a distinct emoji + one reacting user). */
typedef struct { char *emoji; uint64_t user_id; } oc_reaction_row;

/* One row in a CHANNEL_LIST result (net thread renders as a list entry). */
typedef struct {
    uint64_t channel_id;
    char    *name;       /* heap */
    uint8_t  is_public;
    uint8_t  joined;     /* 1 if the requesting user is a member */
    uint8_t  kind;       /* OC_CHANNEL_KIND / OC_CHANNEL_KIND_DM */
    char    *topic;      /* heap; NULL = none (REQ-034) */
    uint8_t  archived;   /* REQ-035 */
    uint64_t created_at;
    /* Sidebar ordering + badging for a client that caches nothing (ARCH-88):
     * the newest top-level message's time, and how many of them sit past this
     * user's delivery cursor (REQ-090). Both are 0 for an empty channel. */
    uint64_t last_message_at;
    uint32_t unread;
    uint64_t peer_id;    /* DM: the other participant, so a client can name it */
    char    *preview;       /* heap; newest top-level body, truncated */
    uint64_t preview_author;
} oc_channel_row;

/* One row in a NOTIFY_PREFS result (REQ-130): a channel and its level. */
typedef struct { uint64_t channel_id; uint8_t level; uint8_t muted; } oc_notify_pref_row;

/* One row in a CLIENT_SETTINGS result: a synced key/value. */
typedef struct { char *key; char *value; } oc_client_setting_row;
/* One blob the maintenance pass decided to reclaim (ARCH-78). The row is already
 * tombstoned in SQLite; only the bytes remain, and deleting those is the net
 * thread's job via the transfer pool because it can block on S3. */
/* Why a blob was reclaimed, recorded on the attachments row (migration 0015) so
 * REQ-215's audit trail is a query rather than a second log. */
/* Audit families (REQ-251, ARCH-79). The cap is applied per family so a flood of
 * attacker-controlled security events cannot evict administrative history. */
enum { OC_AUDIT_ADMIN = 1, OC_AUDIT_ACCOUNT = 2,
       OC_AUDIT_SECURITY = 3, OC_AUDIT_MODERATION = 4 };

enum { OC_RECLAIM_NONE = 0, OC_RECLAIM_ORPHAN = 1,
       OC_RECLAIM_EXPIRED = 2, OC_RECLAIM_EVICTED = 3 };

typedef struct { char *storage_key; uint64_t attachment_id; } oc_reclaim_row;

/* One audit entry as read back for an admin (REQ-251). */
typedef struct oc_audit_row {
    uint64_t at_ms;
    uint64_t actor_id;
    uint64_t target_id;
    char    *actor_name;   /* heap; denormalized, the actor may since be removed */
    char    *action;       /* heap */
    char    *target;       /* heap */
    char    *detail;       /* heap; never carries the secret involved */
    uint8_t  family;
    uint8_t  outcome;      /* 1 ok, 0 denied/failed */
} oc_audit_row;

/* One row in a READ_CURSOR result: a member's read position in the channel. */
typedef struct { uint64_t user_id; uint64_t message_id; } oc_read_cursor_row;

/* One row in a WEBHOOK_LIST result (REQ-170); the token is never returned. */
typedef struct {
    uint64_t id;
    uint64_t channel_id;
    char    *label;       /* heap */
    uint8_t  disabled;
} oc_webhook_row;

/* One row in a USER_LIST result. */
typedef struct {
    uint64_t user_id;
    uint8_t  role;
    uint8_t  disabled;
    char    *email;         /* heap; may be "" */
    char    *display_name;  /* heap; may be "" */
} oc_user_row;

/* One message to replay on reconnect (rendered as a BROADCAST by the net thread),
 * or one reply in a THREAD. reply_count/last_reply_at are set for backfilled
 * top-level messages that have thread replies (drives THREAD_META). */
/* One attachment linked to a message (REQ-140): its id + metadata for delivery.
 * filename/mime are heap; the blob itself lives in object storage. */
typedef struct {
    uint64_t id;
    char    *filename;   /* heap */
    char    *mime;       /* heap */
    uint64_t size;
    uint8_t  reclaimed;  /* bytes removed by age or pressure; row is a tombstone */
} oc_attach_meta;

typedef struct {
    uint64_t message_id, channel_id, author_id, server_time;
    uint8_t *body;       /* heap */
    size_t   body_len;
    uint32_t reply_count;
    uint64_t last_reply_at;
    oc_attach_meta attach[OC_MAX_ATTACH];  /* linked attachments (REQ-140) */
    size_t         n_attach;
    char    *author_name;  /* heap; override display name (webhooks), else NULL */
    /* Pin state (REQ-230). A BROADCAST carries none, so without this every pin
     * vanished the moment a client reloaded — the same defect the reaction
     * replay above exists to prevent. */
    uint64_t pinned_by;    /* 0 when not pinned */
    uint64_t pinned_at;
    /* Saved-for-later state (REQ-231), for the REQUESTING user only — a pin is a
     * channel-wide fact, this is private, so it can never travel in a fan-out
     * BROADCAST. It rides the per-connection replay and the net thread turns it
     * into a SAVED_UPDATED, exactly as pins do above. */
    uint8_t  saved;
    uint64_t saved_at;
} oc_replay_msg;

typedef struct oc_dbres {
    struct oc_dbres *next;
    int            type;
    uint64_t       conn_id;
    uint16_t       err_code;  /* reason code for *_ERR */

    /* AUTH_OK / REGISTER_OK */
    uint64_t       user_id;
    uint8_t        role;                              /* OC_ROLE_* */
    uint64_t       session_expiry;                    /* ms since epoch */
    uint8_t        session_token[OC_SESSION_TOKEN_LEN];
    int            has_session_token;                 /* 0 on session re-auth */

    /* SEND_OK */
    uint64_t       message_id;
    uint64_t       server_time;
    uint64_t       channel_id;
    uint64_t       author_id;
    uint8_t        idem[OC_IDEM_LEN];
    uint8_t       *body;      /* heap; for the broadcast */
    size_t         body_len;
    uint64_t      *members;   /* heap; user ids to fan the broadcast out to */
    size_t         n_members;
    int            duplicate; /* idempotent replay: ack only, no broadcast */
    oc_attach_meta attach[OC_MAX_ATTACH];  /* SEND_OK: attachments linked to this message */
    size_t         n_attach;
    char          *author_name;  /* heap; SEND_OK/WEBHOOK_POSTED override name, else NULL */

    /* BACKFILL_OK */
    oc_replay_msg *replay;    /* heap array, ascending message_id */
    size_t         n_replay;
    /* Reaction aggregates for the replayed messages, one row per
     * (message, emoji). A BROADCAST carries no reaction state, so without these
     * every reaction disappeared from a client that reloaded — permanently,
     * now that clients keep no local cache (ARCH-88). `user_id` is the
     * requesting user when they are one of the reactors, else any other
     * reactor, which is exactly what the client needs to render the "mine"
     * state without a second round trip. */
    struct oc_replay_react { uint64_t message_id, channel_id, user_id, count; char *emoji; }
                  *rreact;
    size_t         n_rreact;
    uint64_t       high_water;
    uint8_t        truncated;  /* results hit the per-response cap (backfill/search/thread) */

    /* CHANNEL_INFO (create/join/leave/invite/remove ack). channel_id above. */
    uint8_t        ch_kind;
    char          *ch_name;         /* heap */
    uint8_t        ch_is_public;
    uint8_t        ch_joined;       /* the actor's membership after the op */
    uint64_t       ch_created_at;
    char          *ch_topic;        /* heap; NULL = none (REQ-034) */
    uint8_t        ch_archived;     /* REQ-035 */
    /* CHANNEL_INFO from an UPDATE_CHANNEL fans to every member, not just the
     * actor: a rename or archive changes what everyone's sidebar should say. */
    uint8_t        ch_fanout;
    uint64_t       ch_peer;         /* DM (ch_kind=1): the other participant's id (0 = not a DM) */
    uint64_t       push_user_id;    /* INVITE: also push CHANNEL_INFO to this user (0 = none) */

    /* CHANNEL_LIST */
    oc_channel_row *chlist;         /* heap array */
    size_t          n_chlist;

    /* Admin ops (REQ-033). USER_UPDATED carries user_id (above) + role + disabled.
     * INVITE_OK reuses session_token/session_expiry/role for the minted invite. */
    uint8_t         disabled;       /* USER_UPDATED: the target's disabled flag */
    oc_user_row    *ulist;          /* USER_LIST: heap array */
    size_t          n_ulist;

    /* Reactions (REQ-070/071). REACTION_OK: emoji + op + aggregate count for the
     * fan-out (message_id/channel_id/user_id above, members for the recipients).
     * REACTIONS: the full per-message reactor list. */
    char           *emoji;          /* heap; REACTION_OK */
    uint8_t         react_op;
    uint64_t        react_count;
    oc_reaction_row *rlist;         /* heap array; REACTIONS */
    size_t           n_rlist;

    /* Pins (REQ-230). PIN_OK reuses message_id/channel_id/user_id/members above
     * for the fan-out; pin_op says which way and pinned_at when. PINS carries
     * the channel's list. */
    uint8_t          pin_op;
    uint64_t         pinned_at;
    oc_pin_row      *plist;         /* heap array; PINS */
    size_t           n_plist;

    /* Channel details (REQ-031, REQ-143). channel_id above. */
    oc_chanmem_row  *cmlist;        /* heap array; MEMBER_LIST */
    size_t           n_cmlist;
    oc_file_row     *flist;         /* heap array; FILE_LIST */
    size_t           n_flist;

    /* Saved items + activity (REQ-231/139). */
    uint8_t          save_op;
    uint64_t         saved_at;
    oc_saved_row    *slist;
    size_t           n_slist;
    oc_activity_row *alist;
    size_t           n_alist;
    uint64_t         activity_seen;

    /* Threads (REQ-060). REPLY_OK reuses message_id/channel_id/author_id/
     * server_time/body/idem/members/duplicate above, plus parent_id + reply_count.
     * THREAD carries the reply list. */
    uint64_t        parent_id;
    uint32_t        reply_count;
    oc_replay_msg  *thread;         /* heap array; THREAD replies */
    size_t          n_thread;

    /* SEARCH results (REQ-080): each row reuses oc_replay_msg with `body`
     * holding the FTS snippet. */
    oc_replay_msg  *search;
    size_t          n_search;

    /* IDENTITY (load): the stored TLS cert+key PEM, or NULL if none. */
    char           *cert_pem;
    char           *key_pem;

    /* ENROLLMENT (load, CP-8): the persisted keypair + audience + state. */
    char           *enroll_privkey;
    char           *enroll_audience;
    int             enroll_active;
    int             enroll_present;

    /* Attachments (REQ-140/141). CREATED: attachment_id (above) + storage_key.
     * META (download): attachment_id + channel_id (above) + storage_key +
     * filename + mime + att_size + att_sha256. */
    uint64_t        attachment_id;
    char           *storage_key;  /* heap */
    char           *filename;      /* heap */
    char           *mime;          /* heap */
    uint64_t        att_size;
    uint8_t         att_sha256[32];

    /* Webhook management (REQ-170). WEBHOOK_LIST: the rows; WEBHOOK_DELETED reuses
     * message_id above to echo the removed webhook id. */
    oc_webhook_row *whlist;        /* heap array */
    size_t          n_whlist;

    /* NOTIFY_PREFS (REQ-130/131): the user's DND window + per-channel levels. */
    oc_notify_pref_row *nprefs;    /* heap array */
    size_t              n_nprefs;
    uint8_t             np_dnd_enabled;
    uint16_t            np_dnd_start_min, np_dnd_end_min;

    /* CLIENT_SETTINGS: the bucket's client_type + its key/value rows. */
    char                   *cs_client_type;  /* heap */
    oc_client_setting_row  *cslist;          /* heap array */
    size_t                  n_cslist;
    oc_reclaim_row         *reclaim;         /* heap array (OC_RES_STORAGE_MAINT) */
    /* Outstanding invites (OC_RES_INVITE_LIST). Ids, never tokens: only a hash is
     * stored, and a list is not a place to hand credentials back. */
    oc_invite_entry        *invites;
    size_t                  n_invites;
    /* OC_RES_PROFILE_INFO (WIN-47/53). Heap strings, freed with the result. Appended
     * at the END rather than inserted mid-struct: the first attempt landed between
     * `reclaim` and `n_reclaim`, splitting a pointer from its count, which is exactly
     * the pairing a reader relies on. */
    char                   *st_emoji, *st_text, *pf_title, *pf_tz;
    uint64_t                st_expires, pf_avatar;
    size_t                  n_reclaim;
    uint64_t                maint_orphans;   /* counts, for the log line */
    uint64_t                maint_expired;
    uint64_t                maint_evicted;
    /* Storage report (REQ-214). The free-space half is filled in by the net
     * thread from its cached statvfs sample; the writer supplies what only the
     * database knows. */
    uint64_t                st_attach_bytes, st_attach_count;
    uint64_t                st_rec_orphan, st_rec_expired, st_rec_evicted;
    uint64_t                st_last_reclaim_ms;
    /* Audit page (REQ-251). `audit` is a heap array of rows, newest first. */
    struct oc_audit_row    *audit;
    size_t                  n_audit;

    /* PROFILE_UPDATED: the (possibly unchanged) display name to broadcast; the
     * subject user is `user_id` above. */
    char                   *profile_name;    /* heap */

    /* READ_CURSOR (REQ-090): the acker (user_id) advanced to message_id in
     * channel_id; members holds the channel members to fan it to, and rcur holds
     * the *other* members' current cursors to backfill the acker. */
    oc_read_cursor_row     *rcur;            /* heap array */
    size_t                  n_rcur;
} oc_dbres;

typedef struct oc_dbwriter oc_dbwriter;

/* Open `path`, WAL + foreign_keys, migrate, start the thread. NULL on failure. */
oc_dbwriter *oc_dbwriter_start(const char *path);
void         oc_dbwriter_stop(oc_dbwriter *w);

/* The eventfd the net thread registers in epoll; readable when results wait. */
int  oc_dbwriter_eventfd(oc_dbwriter *w);

/* Switch the deployment to OIDC mode (AUTH.md §3): AUTH{oidc} tokens are then
 * verified against the pinned ES256 key, and AUTH_CHALLENGE advertises oidc +
 * session (local is disabled — v1 is one mode per tenant). Copies its args;
 * call once before serving traffic. `pubkey_pem` is a PEM SubjectPublicKeyInfo;
 * `oidc_params` is the opaque blob advertised to clients. Returns 0 / -1. */
/* Set the registered-user cap (CP-7, OPENCHIME_MAX_USERS); <=0 = unlimited. Call
 * once before serving. */
void oc_dbwriter_set_max_users(oc_dbwriter *w, int max_users);

int oc_dbwriter_configure_oidc(oc_dbwriter *w, const char *issuer,
                               const char *audience, const char *pubkey_pem,
                               const char *oidc_params);

/* Auth methods bitset (OC_AUTH_*) to advertise in AUTH_CHALLENGE, and the
 * OIDC params blob ("" unless OIDC is configured). For the net loop. */
uint8_t     oc_dbwriter_auth_methods(oc_dbwriter *w);
const char *oc_dbwriter_oidc_params(oc_dbwriter *w);

/* Override the idempotency-map retention + prune interval (ARCH-44). Production
 * defaults are 24h / 1h; tests set small values to exercise pruning. */
void oc_dbwriter_set_idem_retention(oc_dbwriter *w, uint64_t retention_ms,
                                    uint64_t interval_ms);

/* Allocate a zeroed job of `type` for `conn_id`. Fill in the type's fields
 * (oc_job_set_token / oc_job_set_body copy into heap) then submit. */
oc_job *oc_job_new(int type, uint64_t conn_id);
int     oc_job_set_token(oc_job *j, const void *tok, size_t len);
int     oc_job_set_body(oc_job *j, const void *body, size_t len);
/* Fill a REGISTER job (copies strings). iterations 0 -> OC_PW_ITERATIONS. */
int     oc_job_set_register(oc_job *j, const char *username, const char *password,
                            uint8_t role, uint32_t iterations);

/* Synchronously ensure a local account exists (bootstrap the first owner, or a
 * fixed set of accounts for tests). Runs a REGISTER job through the writer and
 * blocks for its result; returns the user id, or 0 on failure. */
uint64_t oc_dbwriter_register_local(oc_dbwriter *w, const char *username,
                                    const char *password, uint8_t role,
                                    uint32_t iterations);

/* First-run bootstrap (REQ-024): if the tenant has no owner, mint a one-time
 * owner invite and return its raw token in `token_out` (returns 1); returns 0
 * if an owner already exists. Setup-time only (drains one result). */
int oc_dbwriter_setup_invite(oc_dbwriter *w, uint8_t token_out[OC_INVITE_TOKEN_LEN]);

/* Persisted TLS identity (ARCH-66b) so the TOFU cert survives the database being
 * restored onto a new box (the cert lives in the DB, not just on local disk).
 * load returns 1 + heap cert/key PEM (caller frees) if stored, else 0; store
 * returns 1 on success. Setup-time only (each drains one result). */
int oc_dbwriter_load_identity(oc_dbwriter *w, char **cert_out, char **key_out);
int oc_dbwriter_store_identity(oc_dbwriter *w, const char *cert_pem, const char *key_pem);

/* Federated enrollment persistence (CP-8), setup-time only. Load returns 1 and
 * heap-allocates privkey_pem + audience (caller frees) + *active when a row
 * exists, else 0. Store persists the keypair + audience + state; returns 1 on
 * success. */
int oc_dbwriter_load_enrollment(oc_dbwriter *w, char **privkey_out, char **audience_out, int *active_out);
int oc_dbwriter_store_enrollment(oc_dbwriter *w, const char *privkey_pem, const char *audience, int active);

/* Register a push device token (ARCH-85): submits + blocks for the ack; returns 1
 * on success, 0 on a bad platform/empty token. */
int oc_dbwriter_register_device_token(oc_dbwriter *w, uint64_t user_id, uint8_t platform, const char *token);
/* Drop a device token across all users — fire-and-forget. Called by the push
 * worker from its own thread when central reports the token stale. */
void oc_dbwriter_prune_device_token(oc_dbwriter *w, const char *token);

/* Hand a job to the writer (transfers ownership; the writer frees it). */
void       oc_dbwriter_submit(oc_dbwriter *w, oc_job *j);
/* Pop the next completed result, or NULL when drained. Caller frees it. */
oc_dbres *oc_dbwriter_next_result(oc_dbwriter *w);
void       oc_dbres_free(oc_dbres *r);

#endif /* OPENCHIME_DBWRITER_H */
