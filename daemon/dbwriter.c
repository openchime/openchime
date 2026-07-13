/*
 * OpenChime DB-writer thread + write-job queue. See dbwriter.h and migrate.h.
 *
 * One thread owns the write connection. It pops jobs off a request queue,
 * performs all DB work (local/session AUTH + account REGISTER, SEND persist with
 * idempotency, backfill), and pushes results onto a completion queue, signalling
 * the net thread via an eventfd.
 */

#include "dbwriter.h"
#include "migrate.h"
#include "protocol.h"
#include "auth.h"
#include "jwt.h"
#include "ratelimit.h"
#include "roles.h"

#include <pthread.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

struct oc_dbwriter {
    sqlite3        *db;
    pthread_t       thread;
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    oc_job         *jobs_head, *jobs_tail;   /* net -> writer */
    oc_dbres      *res_head,  *res_tail;    /* writer -> net */
    int             evfd;                    /* signals results ready */
    int             stop;
    int             started;

    /* Auth config (set before serving; read only on the writer thread). */
    uint8_t         auth_methods;            /* advertised in AUTH_CHALLENGE */
    int             oidc_enabled;
    char           *oidc_issuer;
    char           *oidc_audience;
    char           *oidc_pubkey_pem;
    char           *oidc_params;             /* advertised blob ("" if none) */
    oc_ratelimit   *auth_rl;                 /* failed local-auth per account */
    oc_ratelimit   *source_rl;               /* failed local-auth per source IP */
};

/* Failed local-auth throttle (REQ-191, AUTH.md §2): after this many failures
 * within the window, further attempts get AUTH_RATE_LIMITED. The per-source
 * cap is higher than per-account to tolerate many users behind one NAT while
 * still stopping an account-spray from a single IP. */
#define OC_AUTH_MAX_FAILURES        5
#define OC_AUTH_SOURCE_MAX_FAILURES 20
#define OC_AUTH_WINDOW_MS           60000u
#define OC_AUTH_RL_CAPACITY         1024u

static uint64_t dbw_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* --- Job / result allocation ------------------------------------------- */

oc_job *oc_job_new(int type, uint64_t conn_id) {
    oc_job *j = calloc(1, sizeof *j);
    if (j) { j->type = type; j->conn_id = conn_id; }
    return j;
}

int oc_job_set_token(oc_job *j, const void *tok, size_t len) {
    j->token = malloc(len + 1);
    if (!j->token) return -1;
    memcpy(j->token, tok, len);
    j->token[len] = '\0';
    j->token_len = len;
    return 0;
}

int oc_job_set_register(oc_job *j, const char *username, const char *password,
                        uint8_t role, uint32_t iterations) {
    j->username = username ? strdup(username) : NULL;
    j->password = password ? strdup(password) : NULL;
    if ((username && !j->username) || (password && !j->password)) return -1;
    j->role = role;
    j->iterations = iterations;
    return 0;
}

int oc_job_set_body(oc_job *j, const void *body, size_t len) {
    j->body = malloc(len ? len : 1);
    if (!j->body) return -1;
    memcpy(j->body, body, len);
    j->body_len = len;
    return 0;
}

static void job_free(oc_job *j) {
    if (!j) return;
    free(j->token);
    free(j->username);
    free(j->password);
    free(j->body);
    free(j->cursors);
    free(j);
}

void oc_dbres_free(oc_dbres *r) {
    if (!r) return;
    free(r->body);
    free(r->members);
    for (size_t i = 0; i < r->n_replay; i++) free(r->replay[i].body);
    free(r->replay);
    free(r);
}

/* Replay is bounded per request; a client with more backlog issues a follow-up
 * BACKFILL_REQUEST with an advanced cursor (PROTOCOL.md §6.2). */
#define OC_BACKFILL_MAX 500

/* --- Job processing (runs on the writer thread) ------------------------- */

/* Session lifetime — the daemon's own expiry, no longer tied to a provider
 * token (REQ-181, AUTH.md §4). */
#define OC_SESSION_TTL_MS (30ull * 24 * 60 * 60 * 1000)

static uint8_t role_to_u8(const char *r) {
    if (r && strcmp(r, "owner") == 0) return OC_ROLE_OWNER;
    if (r && strcmp(r, "admin") == 0) return OC_ROLE_ADMIN;
    return OC_ROLE_MEMBER;
}

static const char *u8_to_role(uint8_t r) {
    if (r == OC_ROLE_OWNER) return "owner";
    if (r == OC_ROLE_ADMIN) return "admin";
    return "member";
}

/* Build "local:<username>" into buf; returns length, or 0 if username is empty
 * or too long to namespace safely. */
static size_t local_subject(char *buf, size_t cap, const char *username, size_t ulen) {
    if (ulen == 0 || ulen + 6 > cap) return 0;
    memcpy(buf, "local:", 6);
    memcpy(buf + 6, username, ulen);
    return ulen + 6;
}

static void ensure_default_membership(sqlite3 *db, uint64_t user_id) {
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "INSERT OR IGNORE INTO channels(id,kind,name,is_public,created_at_ms) "
        "VALUES(?, 'channel', 'general', 1, ?);", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, OC_DEFAULT_CHANNEL);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)dbw_now_ms());
    sqlite3_step(st);
    sqlite3_finalize(st);

    sqlite3_prepare_v2(db,
        "INSERT OR IGNORE INTO channel_members(channel_id,user_id,joined_at_ms) "
        "VALUES(?, ?, ?);", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, OC_DEFAULT_CHANNEL);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)user_id);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)dbw_now_ms());
    sqlite3_step(st);
    sqlite3_finalize(st);
}

/* Create a local account: `local:<username>` user + PBKDF2 credential (AUTH.md
 * §2). Idempotent — INSERT OR IGNORE means re-running bootstrap never clobbers
 * an existing password. Returns the user id, or 0 on error. */
static uint64_t register_local(sqlite3 *db, const char *username, size_t ulen,
                               const char *password, size_t plen,
                               uint8_t role, uint32_t iterations) {
    char subject[256];
    size_t sublen = local_subject(subject, sizeof subject, username, ulen);
    if (sublen == 0 || plen == 0) return 0;
    if (iterations == 0) iterations = OC_PW_ITERATIONS;

    uint8_t salt[OC_PW_SALT_LEN], hash[OC_PW_HASH_LEN];
    if (oc_rand_bytes(salt, sizeof salt) != 0) return 0;
    if (oc_pw_derive(password, plen, salt, sizeof salt, iterations, hash) != 0) return 0;

    uint64_t now = dbw_now_ms();
    sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);

    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "INSERT OR IGNORE INTO users(subject, role, created_at_ms) VALUES(?, ?, ?);", -1, &st, NULL);
    sqlite3_bind_text(st, 1, subject, (int)sublen, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, u8_to_role(role), -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)now);
    sqlite3_step(st);
    sqlite3_finalize(st);

    uint64_t uid = 0;
    sqlite3_prepare_v2(db, "SELECT id FROM users WHERE subject=?;", -1, &st, NULL);
    sqlite3_bind_text(st, 1, subject, (int)sublen, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) uid = (uint64_t)sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    if (uid == 0) { sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL); return 0; }

    sqlite3_prepare_v2(db,
        "INSERT OR IGNORE INTO local_credentials(user_id,salt,iterations,hash,updated_at_ms) "
        "VALUES(?,?,?,?,?);", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)uid);
    sqlite3_bind_blob(st, 2, salt, sizeof salt, SQLITE_STATIC);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)iterations);
    sqlite3_bind_blob(st, 4, hash, sizeof hash, SQLITE_STATIC);
    sqlite3_bind_int64(st, 5, (sqlite3_int64)now);
    sqlite3_step(st);
    sqlite3_finalize(st);

    sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
    ensure_default_membership(db, uid);
    return uid;
}

/* Verify a local username+password against local_credentials, constant-time on
 * the hash. Returns user id + role on success, 0 otherwise. */
static uint64_t verify_local(sqlite3 *db, const char *username, size_t ulen,
                             const char *password, size_t plen, uint8_t *role_out) {
    char subject[256];
    size_t sublen = local_subject(subject, sizeof subject, username, ulen);
    if (sublen == 0) return 0;

    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "SELECT u.id, u.role, c.salt, c.iterations, c.hash FROM users u "
        "JOIN local_credentials c ON c.user_id = u.id WHERE u.subject = ?;", -1, &st, NULL);
    sqlite3_bind_text(st, 1, subject, (int)sublen, SQLITE_TRANSIENT);

    uint64_t uid = 0; uint8_t role = OC_ROLE_MEMBER; int ok = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        uint64_t cand = (uint64_t)sqlite3_column_int64(st, 0);
        uint8_t  crole = role_to_u8((const char *)sqlite3_column_text(st, 1));
        const void *salt = sqlite3_column_blob(st, 2);
        int slen = sqlite3_column_bytes(st, 2);
        uint32_t iters = (uint32_t)sqlite3_column_int64(st, 3);
        const void *stored = sqlite3_column_blob(st, 4);
        int hlen = sqlite3_column_bytes(st, 4);
        uint8_t derived[OC_PW_HASH_LEN];
        if (salt && stored && hlen == (int)OC_PW_HASH_LEN &&
            oc_pw_derive(password, plen, salt, (size_t)slen, iters, derived) == 0 &&
            oc_ct_eq(derived, stored, OC_PW_HASH_LEN)) {
            ok = 1; uid = cand; role = crole;
        }
    }
    sqlite3_finalize(st);
    if (!ok) return 0;
    if (role_out) *role_out = role;
    return uid;
}

/* Mint a session: random 32-byte token to the caller, only its SHA-256 stored
 * (AUTH.md §4). Returns 0 and fills token/expiry, or -1. */
static int mint_session(sqlite3 *db, uint64_t user_id,
                        uint8_t token_out[OC_SESSION_TOKEN_LEN], uint64_t *expiry_out) {
    uint8_t token[OC_SESSION_TOKEN_LEN], hash[OC_SHA256_LEN];
    if (oc_rand_bytes(token, sizeof token) != 0) return -1;
    if (oc_sha256(token, sizeof token, hash) != 0) return -1;

    uint64_t now = dbw_now_ms(), expiry = now + OC_SESSION_TTL_MS;
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "INSERT INTO sessions(token_hash,user_id,created_at_ms,expires_at_ms,last_seen_ms) "
        "VALUES(?,?,?,?,?);", -1, &st, NULL);
    sqlite3_bind_blob(st, 1, hash, sizeof hash, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)user_id);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)now);
    sqlite3_bind_int64(st, 4, (sqlite3_int64)expiry);
    sqlite3_bind_int64(st, 5, (sqlite3_int64)now);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) return -1;

    memcpy(token_out, token, sizeof token);
    if (expiry_out) *expiry_out = expiry;
    return 0;
}

/* Reconnect: hash the presented token, look up a live session, touch last_seen.
 * Returns user id + role + expiry, or 0 if unknown/expired (AUTH.md §4). */
static uint64_t lookup_session(sqlite3 *db, const uint8_t *token, size_t tlen,
                               uint8_t *role_out, uint64_t *expiry_out) {
    if (tlen != OC_SESSION_TOKEN_LEN) return 0;
    uint8_t hash[OC_SHA256_LEN];
    if (oc_sha256(token, tlen, hash) != 0) return 0;

    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "SELECT s.user_id, s.expires_at_ms, u.role FROM sessions s "
        "JOIN users u ON u.id = s.user_id WHERE s.token_hash = ?;", -1, &st, NULL);
    sqlite3_bind_blob(st, 1, hash, sizeof hash, SQLITE_STATIC);
    uint64_t uid = 0, exp = 0; uint8_t role = OC_ROLE_MEMBER;
    if (sqlite3_step(st) == SQLITE_ROW) {
        uid  = (uint64_t)sqlite3_column_int64(st, 0);
        exp  = (uint64_t)sqlite3_column_int64(st, 1);
        role = role_to_u8((const char *)sqlite3_column_text(st, 2));
    }
    sqlite3_finalize(st);
    if (uid == 0) return 0;
    if (exp != 0 && dbw_now_ms() >= exp) return 0;   /* expired */

    sqlite3_prepare_v2(db, "UPDATE sessions SET last_seen_ms=? WHERE token_hash=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)dbw_now_ms());
    sqlite3_bind_blob(st, 2, hash, sizeof hash, SQLITE_STATIC);
    sqlite3_step(st);
    sqlite3_finalize(st);

    if (role_out) *role_out = role;
    if (expiry_out) *expiry_out = exp;
    return uid;
}

/* Look up a user's role; returns 1 and sets *role_out if the user exists. */
static int user_role(sqlite3 *db, uint64_t uid, uint8_t *role_out) {
    sqlite3_stmt *st = NULL;
    int found = 0;
    sqlite3_prepare_v2(db, "SELECT role FROM users WHERE id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)uid);
    if (sqlite3_step(st) == SQLITE_ROW) {
        *role_out = role_to_u8((const char *)sqlite3_column_text(st, 0));
        found = 1;
    }
    sqlite3_finalize(st);
    return found;
}

static uint8_t get_role(sqlite3 *db, uint64_t uid) {
    uint8_t role = OC_ROLE_MEMBER;
    user_role(db, uid, &role);
    return role;
}

static int count_owners(sqlite3 *db) {
    sqlite3_stmt *st = NULL;
    int n = 0;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM users WHERE role='owner';", -1, &st, NULL);
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return n;
}

/* Just-in-time provision an OIDC user by subject (AUTH.md §4); refreshes the
 * email/name on each login. Returns the user id, or 0. Role defaults to member
 * via the schema; promotion is a separate action. */
static uint64_t upsert_oidc_user(sqlite3 *db, const char *subject,
                                 const char *email, const char *name) {
    uint64_t now = dbw_now_ms();
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "INSERT INTO users(subject, email, display_name, created_at_ms) VALUES(?,?,?,?) "
        "ON CONFLICT(subject) DO UPDATE SET email=excluded.email, "
        "display_name=excluded.display_name;", -1, &st, NULL);
    sqlite3_bind_text(st, 1, subject, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, email, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, name, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 4, (sqlite3_int64)now);
    sqlite3_step(st);
    sqlite3_finalize(st);

    uint64_t uid = 0;
    sqlite3_prepare_v2(db, "SELECT id FROM users WHERE subject=?;", -1, &st, NULL);
    sqlite3_bind_text(st, 1, subject, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) uid = (uint64_t)sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return uid;
}

static oc_dbres *process_register(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    uint64_t uid = register_local(db,
        j->username ? j->username : "", j->username ? strlen(j->username) : 0,
        j->password ? j->password : "", j->password ? strlen(j->password) : 0,
        j->role, j->iterations);
    if (uid == 0) { r->type = OC_RES_REGISTER_ERR; r->err_code = OC_ERR_INTERNAL; return r; }
    r->type = OC_RES_REGISTER_OK;
    r->user_id = uid;
    r->role = j->role;
    return r;
}

/* Prove identity (local password, an OIDC ES256 JWT, or an existing session
 * token) and converge on a daemon-issued session (AUTH.md §4). */
static oc_dbres *process_auth(oc_dbwriter *w, const oc_job *j) {
    sqlite3 *db = w->db;
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;

    uint64_t uid = 0, sess_exp = 0;
    uint8_t role = OC_ROLE_MEMBER;
    int fresh = 1;   /* mint a new session unless this is a session re-auth */

    if (j->method == OC_AUTH_LOCAL) {
        if (!(w->auth_methods & OC_AUTH_LOCAL)) {
            r->type = OC_RES_AUTH_ERR; r->err_code = OC_ERR_AUTH_REQUIRED; return r;
        }
        oc_slice cred = { (const uint8_t *)j->token, j->token_len };
        oc_slice user, pass;
        if (oc_parse_local_credential(cred, &user, &pass) != OC_OK) {
            r->type = OC_RES_AUTH_ERR; r->err_code = OC_ERR_AUTH_INVALID_TOKEN; return r;
        }
        /* Throttle brute force per account and per source IP (REQ-191). Check
         * before the expensive PBKDF2 so a flood can't also burn CPU. */
        char acct[OC_RL_KEYMAX];
        size_t al = user.len < sizeof acct - 1 ? user.len : sizeof acct - 1;
        memcpy(acct, user.ptr, al);
        acct[al] = '\0';
        int has_src = j->source[0] != '\0';
        uint64_t now = dbw_now_ms();
        if (oc_ratelimit_blocked(w->auth_rl, acct, now) ||
            (has_src && oc_ratelimit_blocked(w->source_rl, j->source, now))) {
            r->type = OC_RES_AUTH_ERR; r->err_code = OC_ERR_AUTH_RATE_LIMITED; return r;
        }
        uid = verify_local(db, (const char *)user.ptr, user.len,
                           (const char *)pass.ptr, pass.len, &role);
        if (uid == 0) {
            oc_ratelimit_record(w->auth_rl, acct, now);
            if (has_src) oc_ratelimit_record(w->source_rl, j->source, now);
        } else {
            /* Success clears the account counter; the source counter is left so a
             * single success can't reset an in-progress account-spray. */
            oc_ratelimit_reset(w->auth_rl, acct);
        }
    } else if (j->method == OC_AUTH_OIDC) {
        if (!w->oidc_enabled) {
            r->type = OC_RES_AUTH_ERR; r->err_code = OC_ERR_AUTH_REQUIRED; return r;
        }
        oc_jwt_claims claims;
        oc_jwt_result jr = oc_jwt_verify(j->token, j->token_len,
                                         w->oidc_pubkey_pem, strlen(w->oidc_pubkey_pem) + 1,
                                         w->oidc_issuer, w->oidc_audience,
                                         dbw_now_ms() / 1000u, &claims);
        if (jr != OC_JWT_OK) {
            r->type = OC_RES_AUTH_ERR; r->err_code = OC_ERR_AUTH_INVALID_TOKEN; return r;
        }
        /* Namespace by source: "oidc:<central issuer>|<provider sub>" (AUTH.md §4). */
        char subject[OC_JWT_MAX_FIELD * 2 + 8];
        snprintf(subject, sizeof subject, "oidc:%s|%s", claims.iss, claims.sub);
        uid = upsert_oidc_user(db, subject, claims.email, claims.name);
        if (uid) role = get_role(db, uid);   /* membership ensured on the common path */
    } else if (j->method == OC_AUTH_SESSION) {
        uid = lookup_session(db, (const uint8_t *)j->token, j->token_len, &role, &sess_exp);
        fresh = 0;
    } else {
        r->type = OC_RES_AUTH_ERR; r->err_code = OC_ERR_AUTH_REQUIRED; return r;
    }

    if (uid == 0) { r->type = OC_RES_AUTH_ERR; r->err_code = OC_ERR_AUTH_INVALID_TOKEN; return r; }
    ensure_default_membership(db, uid);

    r->type = OC_RES_AUTH_OK;
    r->user_id = uid;
    r->role = role;
    if (fresh) {
        uint8_t token[OC_SESSION_TOKEN_LEN]; uint64_t expiry = 0;
        if (mint_session(db, uid, token, &expiry) != 0) {
            r->type = OC_RES_AUTH_ERR; r->err_code = OC_ERR_INTERNAL; return r;
        }
        memcpy(r->session_token, token, sizeof token);
        r->has_session_token = 1;
        r->session_expiry = expiry;
    } else {
        r->has_session_token = 0;   /* no new token on reconnect (PROTOCOL.md §4.3) */
        r->session_expiry = sess_exp;
    }
    return r;
}

/* Change a user's tenant role (ARCH-60, §6). Enforces the role policy
 * (roles.c) and the ≥1-owner invariant (REQ-030): the last owner cannot be
 * demoted. The actor is j->user_id (the authenticated caller). */
static oc_dbres *process_set_role(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;

    uint8_t actor_role = OC_ROLE_MEMBER, target_cur = OC_ROLE_MEMBER;
    if (!user_role(db, j->user_id, &actor_role) ||
        !user_role(db, j->target_user_id, &target_cur)) {
        /* Unknown actor or target: don't disclose which — just forbid. */
        r->type = OC_RES_SETROLE_ERR; r->err_code = OC_ERR_FORBIDDEN; return r;
    }
    uint8_t next = j->role;
    if (!oc_role_can_set_role(actor_role, target_cur, next)) {
        r->type = OC_RES_SETROLE_ERR; r->err_code = OC_ERR_FORBIDDEN; return r;
    }
    /* ≥1-owner invariant: refuse demoting the tenant's last owner. */
    if (target_cur == OC_ROLE_OWNER && next != OC_ROLE_OWNER && count_owners(db) <= 1) {
        r->type = OC_RES_SETROLE_ERR; r->err_code = OC_ERR_LAST_OWNER; return r;
    }

    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "UPDATE users SET role=? WHERE id=?;", -1, &st, NULL);
    sqlite3_bind_text(st, 1, u8_to_role(next), -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)j->target_user_id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        r->type = OC_RES_SETROLE_ERR; r->err_code = OC_ERR_INTERNAL; return r;
    }
    r->type = OC_RES_SETROLE_OK;
    r->user_id = j->target_user_id;
    r->role = next;
    return r;
}

/* Revoke sessions (REQ-182): the presented token (scope THIS) or all of the
 * user's sessions (scope ALL). The THIS delete is scoped to the authenticated
 * user, so a leaked token still can't revoke another user's session. */
static oc_dbres *process_logout(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->user_id = j->user_id;

    sqlite3_stmt *st = NULL;
    if (j->scope == OC_LOGOUT_ALL) {
        sqlite3_prepare_v2(db, "DELETE FROM sessions WHERE user_id=?;", -1, &st, NULL);
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->user_id);
    } else {
        uint8_t hash[OC_SHA256_LEN];
        if (j->token_len != OC_SESSION_TOKEN_LEN ||
            oc_sha256(j->token, j->token_len, hash) != 0) {
            r->type = OC_RES_LOGOUT_ERR; r->err_code = OC_ERR_AUTH_INVALID_TOKEN; return r;
        }
        sqlite3_prepare_v2(db,
            "DELETE FROM sessions WHERE token_hash=? AND user_id=?;", -1, &st, NULL);
        sqlite3_bind_blob(st, 1, hash, sizeof hash, SQLITE_STATIC);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)j->user_id);
    }
    sqlite3_step(st);
    sqlite3_finalize(st);
    r->type = OC_RES_LOGOUT_OK;
    return r;
}

static int is_member(sqlite3 *db, uint64_t channel_id, uint64_t user_id) {
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "SELECT 1 FROM channel_members WHERE channel_id=? AND user_id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)channel_id);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)user_id);
    int found = (sqlite3_step(st) == SQLITE_ROW);
    sqlite3_finalize(st);
    return found;
}

/* Collect the channel's member user ids into r->members. */
static void load_members(sqlite3 *db, uint64_t channel_id, oc_dbres *r) {
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "SELECT user_id FROM channel_members WHERE channel_id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)channel_id);
    size_t cap = 8, n = 0;
    uint64_t *arr = malloc(cap * sizeof *arr);
    while (arr && sqlite3_step(st) == SQLITE_ROW) {
        if (n == cap) { cap *= 2; uint64_t *g = realloc(arr, cap * sizeof *arr); if (!g) break; arr = g; }
        arr[n++] = (uint64_t)sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    r->members = arr;
    r->n_members = n;
}

static oc_dbres *process_send(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->channel_id = j->channel_id;
    r->author_id = j->user_id;
    memcpy(r->idem, j->idem, OC_IDEM_LEN);

    /* Idempotent replay: a known (channel, token) re-acks the original id. */
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "SELECT s.message_id, m.created_at_ms FROM sent_messages s "
        "JOIN messages m ON m.id = s.message_id "
        "WHERE s.channel_id=? AND s.idempotency_token=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->channel_id);
    sqlite3_bind_blob(st, 2, j->idem, OC_IDEM_LEN, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) {
        r->type = OC_RES_SEND_OK;
        r->message_id = (uint64_t)sqlite3_column_int64(st, 0);
        r->server_time = (uint64_t)sqlite3_column_int64(st, 1);
        r->duplicate = 1;
        sqlite3_finalize(st);
        return r;
    }
    sqlite3_finalize(st);

    if (!is_member(db, j->channel_id, j->user_id)) {
        r->type = OC_RES_SEND_ERR; r->err_code = OC_ERR_NOT_A_MEMBER;
        return r;
    }

    uint64_t ts = dbw_now_ms();
    sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);

    sqlite3_prepare_v2(db,
        "INSERT INTO messages(channel_id, author_id, body, created_at_ms) "
        "VALUES(?, ?, ?, ?);", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->channel_id);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)j->user_id);
    sqlite3_bind_blob(st, 3, j->body, (int)j->body_len, SQLITE_STATIC);
    sqlite3_bind_int64(st, 4, (sqlite3_int64)ts);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        r->type = OC_RES_SEND_ERR; r->err_code = OC_ERR_INTERNAL;
        return r;
    }
    uint64_t mid = (uint64_t)sqlite3_last_insert_rowid(db);

    sqlite3_prepare_v2(db,
        "INSERT INTO sent_messages(channel_id, idempotency_token, message_id, created_at_ms) "
        "VALUES(?, ?, ?, ?);", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->channel_id);
    sqlite3_bind_blob(st, 2, j->idem, OC_IDEM_LEN, SQLITE_STATIC);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)mid);
    sqlite3_bind_int64(st, 4, (sqlite3_int64)ts);
    sqlite3_step(st);
    sqlite3_finalize(st);

    sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);

    r->type = OC_RES_SEND_OK;
    r->message_id = mid;
    r->server_time = ts;
    if (j->body_len) { r->body = malloc(j->body_len); if (r->body) { memcpy(r->body, j->body, j->body_len); r->body_len = j->body_len; } }
    load_members(db, j->channel_id, r);
    return r;
}

/* Replay messages newer than each cursor, for channels the user belongs to,
 * bounded by OC_BACKFILL_MAX total (REQ-101, PROTOCOL.md §6). */
static oc_dbres *process_backfill(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->type = OC_RES_BACKFILL_OK;

    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "SELECT COALESCE(MAX(id),0) FROM messages;", -1, &st, NULL);
    if (sqlite3_step(st) == SQLITE_ROW) r->high_water = (uint64_t)sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);

    size_t cap = 16, n = 0;
    oc_replay_msg *arr = malloc(cap * sizeof *arr);
    if (!arr) return r;

    for (size_t ci = 0; ci < j->n_cursors && n < OC_BACKFILL_MAX; ci++) {
        uint64_t ch = j->cursors[ci].channel_id;
        if (!is_member(db, ch, j->user_id)) continue;

        sqlite3_prepare_v2(db,
            "SELECT id, author_id, created_at_ms, body FROM messages "
            "WHERE channel_id=? AND id>? ORDER BY id LIMIT ?;", -1, &st, NULL);
        sqlite3_bind_int64(st, 1, (sqlite3_int64)ch);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)j->cursors[ci].after_message_id);
        sqlite3_bind_int64(st, 3, (sqlite3_int64)(OC_BACKFILL_MAX - n));
        while (sqlite3_step(st) == SQLITE_ROW && n < OC_BACKFILL_MAX) {
            if (n == cap) {
                cap *= 2;
                oc_replay_msg *g = realloc(arr, cap * sizeof *arr);
                if (!g) break;
                arr = g;
            }
            oc_replay_msg *m = &arr[n];
            memset(m, 0, sizeof *m);
            m->message_id  = (uint64_t)sqlite3_column_int64(st, 0);
            m->channel_id  = ch;
            m->author_id   = (uint64_t)sqlite3_column_int64(st, 1);
            m->server_time = (uint64_t)sqlite3_column_int64(st, 2);
            const void *b = sqlite3_column_blob(st, 3);
            int blen = sqlite3_column_bytes(st, 3);
            if (b && blen > 0) {
                m->body = malloc((size_t)blen);
                if (m->body) { memcpy(m->body, b, (size_t)blen); m->body_len = (size_t)blen; }
            }
            n++;
        }
        sqlite3_finalize(st);
    }
    r->replay = arr;
    r->n_replay = n;
    return r;
}

/* --- Queue plumbing ----------------------------------------------------- */

static void push_result(oc_dbwriter *w, oc_dbres *r) {
    if (!r) return;
    pthread_mutex_lock(&w->mu);
    r->next = NULL;
    if (w->res_tail) w->res_tail->next = r; else w->res_head = r;
    w->res_tail = r;
    pthread_mutex_unlock(&w->mu);
    uint64_t one = 1;
    ssize_t wr = write(w->evfd, &one, sizeof one);
    (void)wr;
}

static void *writer_loop(void *arg) {
    oc_dbwriter *w = (oc_dbwriter *)arg;
    for (;;) {
        pthread_mutex_lock(&w->mu);
        while (!w->stop && !w->jobs_head)
            pthread_cond_wait(&w->cv, &w->mu);
        if (w->stop && !w->jobs_head) { pthread_mutex_unlock(&w->mu); break; }
        oc_job *j = w->jobs_head;
        w->jobs_head = j->next;
        if (!w->jobs_head) w->jobs_tail = NULL;
        pthread_mutex_unlock(&w->mu);

        oc_dbres *r = NULL;
        if (j->type == OC_JOB_AUTH)          r = process_auth(w, j);
        else if (j->type == OC_JOB_SEND)     r = process_send(w->db, j);
        else if (j->type == OC_JOB_BACKFILL) r = process_backfill(w->db, j);
        else if (j->type == OC_JOB_REGISTER) r = process_register(w->db, j);
        else if (j->type == OC_JOB_SET_ROLE) r = process_set_role(w->db, j);
        else if (j->type == OC_JOB_LOGOUT)   r = process_logout(w->db, j);
        job_free(j);
        push_result(w, r);
    }
    return NULL;
}

void oc_dbwriter_submit(oc_dbwriter *w, oc_job *j) {
    pthread_mutex_lock(&w->mu);
    j->next = NULL;
    if (w->jobs_tail) w->jobs_tail->next = j; else w->jobs_head = j;
    w->jobs_tail = j;
    pthread_cond_signal(&w->cv);
    pthread_mutex_unlock(&w->mu);
}

oc_dbres *oc_dbwriter_next_result(oc_dbwriter *w) {
    pthread_mutex_lock(&w->mu);
    oc_dbres *r = w->res_head;
    if (r) { w->res_head = r->next; if (!w->res_head) w->res_tail = NULL; r->next = NULL; }
    pthread_mutex_unlock(&w->mu);
    return r;
}

int oc_dbwriter_eventfd(oc_dbwriter *w) { return w->evfd; }

int oc_dbwriter_configure_oidc(oc_dbwriter *w, const char *issuer,
                               const char *audience, const char *pubkey_pem,
                               const char *oidc_params) {
    if (!issuer || !audience || !pubkey_pem) return -1;
    free(w->oidc_issuer); free(w->oidc_audience);
    free(w->oidc_pubkey_pem); free(w->oidc_params);
    w->oidc_issuer     = strdup(issuer);
    w->oidc_audience   = strdup(audience);
    w->oidc_pubkey_pem = strdup(pubkey_pem);
    w->oidc_params     = strdup(oidc_params ? oidc_params : "");
    if (!w->oidc_issuer || !w->oidc_audience || !w->oidc_pubkey_pem || !w->oidc_params)
        return -1;
    w->oidc_enabled = 1;
    /* v1 is one mode per tenant: OIDC replaces local, session stays. */
    w->auth_methods = OC_AUTH_OIDC | OC_AUTH_SESSION;
    return 0;
}

uint8_t oc_dbwriter_auth_methods(oc_dbwriter *w) { return w->auth_methods; }

const char *oc_dbwriter_oidc_params(oc_dbwriter *w) {
    return w->oidc_params ? w->oidc_params : "";
}

/* Setup-time helper: submit a REGISTER job and block for its result. Intended
 * for bootstrap / test fixtures before the net loop is serving traffic — it
 * consumes one result from the queue, so it must not race a live consumer. */
uint64_t oc_dbwriter_register_local(oc_dbwriter *w, const char *username,
                                    const char *password, uint8_t role,
                                    uint32_t iterations) {
    oc_job *j = oc_job_new(OC_JOB_REGISTER, 0);
    if (!j || oc_job_set_register(j, username, password, role, iterations) != 0) {
        job_free(j);
        return 0;
    }
    oc_dbwriter_submit(w, j);
    for (int i = 0; i < 3000; i++) {
        oc_dbres *r = oc_dbwriter_next_result(w);
        if (r) {
            uint64_t uid = (r->type == OC_RES_REGISTER_OK) ? r->user_id : 0;
            oc_dbres_free(r);
            return uid;
        }
        usleep(1000);
    }
    return 0;
}

/* --- Lifecycle ---------------------------------------------------------- */

oc_dbwriter *oc_dbwriter_start(const char *path) {
    oc_dbwriter *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    w->evfd = -1;
    w->auth_methods = OC_AUTH_LOCAL | OC_AUTH_SESSION;  /* local mode by default */
    w->auth_rl   = oc_ratelimit_new(OC_AUTH_MAX_FAILURES, OC_AUTH_WINDOW_MS, OC_AUTH_RL_CAPACITY);
    w->source_rl = oc_ratelimit_new(OC_AUTH_SOURCE_MAX_FAILURES, OC_AUTH_WINDOW_MS, OC_AUTH_RL_CAPACITY);
    if (!w->auth_rl || !w->source_rl) {
        oc_ratelimit_free(w->auth_rl); oc_ratelimit_free(w->source_rl);
        free(w); return NULL;
    }

    if (sqlite3_open(path, &w->db) != SQLITE_OK) {
        fprintf(stderr, "dbwriter: open %s failed: %s\n", path, sqlite3_errmsg(w->db));
        goto fail;
    }
    char *err = NULL;
    if (sqlite3_exec(w->db, "PRAGMA journal_mode=WAL; PRAGMA foreign_keys=ON;",
                     NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "dbwriter: pragma failed: %s\n", err ? err : "?");
        sqlite3_free(err); goto fail;
    }
    if (oc_migrate_default(w->db, &err) != SQLITE_OK) {
        fprintf(stderr, "dbwriter: migration failed: %s\n", err ? err : "?");
        sqlite3_free(err); goto fail;
    }

    w->evfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (w->evfd < 0) { fprintf(stderr, "dbwriter: eventfd failed\n"); goto fail; }

    pthread_mutex_init(&w->mu, NULL);
    pthread_cond_init(&w->cv, NULL);
    if (pthread_create(&w->thread, NULL, writer_loop, w) != 0) {
        fprintf(stderr, "dbwriter: thread create failed\n");
        pthread_mutex_destroy(&w->mu);
        pthread_cond_destroy(&w->cv);
        goto fail;
    }
    w->started = 1;
    return w;

fail:
    if (w->evfd >= 0) close(w->evfd);
    sqlite3_close(w->db);
    free(w);
    return NULL;
}

void oc_dbwriter_stop(oc_dbwriter *w) {
    if (!w) return;
    if (w->started) {
        pthread_mutex_lock(&w->mu);
        w->stop = 1;
        pthread_cond_signal(&w->cv);
        pthread_mutex_unlock(&w->mu);
        pthread_join(w->thread, NULL);
        pthread_mutex_destroy(&w->mu);
        pthread_cond_destroy(&w->cv);
    }
    for (oc_job *j = w->jobs_head; j; ) { oc_job *n = j->next; job_free(j); j = n; }
    for (oc_dbres *r = w->res_head; r; ) { oc_dbres *n = r->next; oc_dbres_free(r); r = n; }
    free(w->oidc_issuer); free(w->oidc_audience);
    free(w->oidc_pubkey_pem); free(w->oidc_params);
    oc_ratelimit_free(w->auth_rl);
    oc_ratelimit_free(w->source_rl);
    if (w->evfd >= 0) close(w->evfd);
    sqlite3_close(w->db);
    free(w);
}
