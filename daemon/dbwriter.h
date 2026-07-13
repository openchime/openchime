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

enum { OC_JOB_AUTH = 1, OC_JOB_SEND = 2, OC_JOB_BACKFILL = 3, OC_JOB_REGISTER = 4 };

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

    /* REGISTER (create a local account; AUTH.md §2 — bootstrap / invite) */
    char          *username;  /* heap */
    char          *password;  /* heap */
    uint8_t        role;      /* OC_ROLE_* for the new account */
    uint32_t       iterations;/* PBKDF2 rounds (0 -> OC_PW_ITERATIONS default) */

    /* SEND */
    uint64_t       channel_id;
    uint8_t        idem[OC_IDEM_LEN];
    uint8_t       *body;      /* heap */
    size_t         body_len;

    /* BACKFILL */
    oc_bf_cursor  *cursors;   /* heap */
    size_t         n_cursors;
} oc_job;

/* --- Results (writer -> net thread) ------------------------------------- */

enum { OC_RES_AUTH_OK = 1, OC_RES_AUTH_ERR = 2, OC_RES_SEND_OK = 3,
       OC_RES_SEND_ERR = 4, OC_RES_BACKFILL_OK = 5,
       OC_RES_REGISTER_OK = 6, OC_RES_REGISTER_ERR = 7 };

/* One message to replay on reconnect (rendered as a BROADCAST by the net thread). */
typedef struct {
    uint64_t message_id, channel_id, author_id, server_time;
    uint8_t *body;       /* heap */
    size_t   body_len;
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

    /* BACKFILL_OK */
    oc_replay_msg *replay;    /* heap array, ascending message_id */
    size_t         n_replay;
    uint64_t       high_water;
} oc_dbres;

typedef struct oc_dbwriter oc_dbwriter;

/* Open `path`, WAL + foreign_keys, migrate, start the thread. NULL on failure. */
oc_dbwriter *oc_dbwriter_start(const char *path);
void         oc_dbwriter_stop(oc_dbwriter *w);

/* The eventfd the net thread registers in epoll; readable when results wait. */
int  oc_dbwriter_eventfd(oc_dbwriter *w);

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

/* Hand a job to the writer (transfers ownership; the writer frees it). */
void       oc_dbwriter_submit(oc_dbwriter *w, oc_job *j);
/* Pop the next completed result, or NULL when drained. Caller frees it. */
oc_dbres *oc_dbwriter_next_result(oc_dbwriter *w);
void       oc_dbres_free(oc_dbres *r);

#endif /* OPENCHIME_DBWRITER_H */
