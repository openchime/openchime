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
       OC_JOB_STORAGE_MAINT = 45 };

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

    /* REACT (channel_id + message_id above); emoji is heap, op is add/remove. */
    char          *emoji;      /* heap */
    uint8_t        react_op;

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
       /* A read cursor advanced (REQ-090 seen-by): fan the acker's new cursor to
        * the channel's members + backfill the acker with the others' cursors. */
       OC_RES_READ_CURSOR = 50 };

/* One row in a REACTIONS result (a distinct emoji + one reacting user). */
typedef struct { char *emoji; uint64_t user_id; } oc_reaction_row;

/* One row in a CHANNEL_LIST result (net thread renders as a list entry). */
typedef struct {
    uint64_t channel_id;
    char    *name;       /* heap */
    uint8_t  is_public;
    uint8_t  joined;     /* 1 if the requesting user is a member */
    uint8_t  kind;       /* OC_CHANNEL_KIND / OC_CHANNEL_KIND_DM */
} oc_channel_row;

/* One row in a NOTIFY_PREFS result (REQ-130): a channel and its level. */
typedef struct { uint64_t channel_id; uint8_t level; } oc_notify_pref_row;

/* One row in a CLIENT_SETTINGS result: a synced key/value. */
typedef struct { char *key; char *value; } oc_client_setting_row;
/* One blob the maintenance pass decided to reclaim (ARCH-78). The row is already
 * tombstoned in SQLite; only the bytes remain, and deleting those is the net
 * thread's job via the transfer pool because it can block on S3. */
typedef struct { char *storage_key; uint64_t attachment_id; } oc_reclaim_row;

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
    uint64_t       high_water;
    uint8_t        truncated;  /* results hit the per-response cap (backfill/search/thread) */

    /* CHANNEL_INFO (create/join/leave/invite/remove ack). channel_id above. */
    uint8_t        ch_kind;
    char          *ch_name;         /* heap */
    uint8_t        ch_is_public;
    uint8_t        ch_joined;       /* the actor's membership after the op */
    uint64_t       ch_created_at;
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
    size_t                  n_reclaim;
    uint64_t                maint_orphans;   /* counts, for the log line */
    uint64_t                maint_expired;
    uint64_t                maint_evicted;

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

/* Hand a job to the writer (transfers ownership; the writer frees it). */
void       oc_dbwriter_submit(oc_dbwriter *w, oc_job *j);
/* Pop the next completed result, or NULL when drained. Caller frees it. */
oc_dbres *oc_dbwriter_next_result(oc_dbwriter *w);
void       oc_dbres_free(oc_dbres *r);

#endif /* OPENCHIME_DBWRITER_H */
