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
    sqlite3        *db;                       /* the single write connection (ARCH-5) */
    sqlite3        *rdb;                      /* read-only connection for query jobs (ARCH-66) */
    pthread_t       thread;                   /* writer */
    pthread_t       reader;                   /* read-only query worker */
    pthread_mutex_t mu;
    pthread_cond_t  cv;                       /* wakes the writer */
    pthread_cond_t  read_cv;                  /* wakes the reader */
    oc_job         *jobs_head, *jobs_tail;    /* net -> writer (write jobs) */
    oc_job         *rjobs_head, *rjobs_tail;  /* net -> reader (read-only jobs) */
    oc_dbres      *res_head,  *res_tail;     /* writer|reader -> net */
    int             evfd;                    /* signals results ready */
    int             stop;
    int             started;
    int             reader_started;

    /* Auth config (set before serving; read only on the writer thread). */
    uint8_t         auth_methods;            /* advertised in AUTH_CHALLENGE */
    int             oidc_enabled;
    char           *oidc_issuer;
    char           *oidc_audience;
    char           *oidc_pubkey_pem;
    char           *oidc_params;             /* advertised blob ("" if none) */
    int             max_users;               /* registered-user cap (CP-7); 0 = unlimited */
    oc_ratelimit   *auth_rl;                 /* failed local-auth per account */
    oc_ratelimit   *source_rl;               /* failed local-auth per source IP */

    /* Idempotency-map pruning (ARCH-44): drop sent_messages rows older than the
     * retention window, at most once per interval. Writer-thread state only. */
    uint64_t        idem_retention_ms;
    uint64_t        prune_interval_ms;
    uint64_t        last_prune_ms;
};

/* Failed local-auth throttle (REQ-191, AUTH.md §2): after this many failures
 * within the window, further attempts get AUTH_RATE_LIMITED. The per-source
 * cap is higher than per-account to tolerate many users behind one NAT while
 * still stopping an account-spray from a single IP. */
#define OC_AUTH_MAX_FAILURES        5
#define OC_AUTH_SOURCE_MAX_FAILURES 20
#define OC_AUTH_WINDOW_MS           60000u
#define OC_AUTH_RL_CAPACITY         1024u

/* Idempotency retention (ARCH-44): keep a (channel, token) -> id mapping this
 * long — enough to cover realistic reconnect-retry — then prune it. Pruning runs
 * at most once per interval on the writer thread. */
#define OC_IDEM_RETENTION_MS (24ull * 60 * 60 * 1000)
#define OC_PRUNE_INTERVAL_MS (60ull * 60 * 1000)

static uint64_t dbw_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* --- audit log (REQ-251, ARCH-79) ----------------------------------------- */

/* Append one audit entry. Best-effort by design: auditing must never fail the
 * action it describes — a full disk should not make role changes impossible —
 * so errors are swallowed. Writer thread only, so entries inherit the same
 * single-writer ordering as the data they describe.
 *
 * `detail` must never carry the secret involved (ARCH-79): that a password
 * changed, never the password. */
static void audit_log(sqlite3 *db, int family, const char *action,
                      uint64_t actor_id, const char *actor_name,
                      uint64_t target_id, const char *target,
                      int outcome, const char *detail) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO audit_log(at_ms, family, action, actor_id, actor_name,"
            " target_id, target, outcome, detail) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9);", -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)dbw_now_ms());
    sqlite3_bind_int(st, 2, family);
    sqlite3_bind_text(st, 3, action, -1, SQLITE_TRANSIENT);
    if (actor_id) sqlite3_bind_int64(st, 4, (sqlite3_int64)actor_id);
    else          sqlite3_bind_null(st, 4);
    if (actor_name) sqlite3_bind_text(st, 5, actor_name, -1, SQLITE_TRANSIENT);
    else            sqlite3_bind_null(st, 5);
    if (target_id) sqlite3_bind_int64(st, 6, (sqlite3_int64)target_id);
    else           sqlite3_bind_null(st, 6);
    if (target) sqlite3_bind_text(st, 7, target, -1, SQLITE_TRANSIENT);
    else        sqlite3_bind_null(st, 7);
    sqlite3_bind_int(st, 8, outcome ? 1 : 0);
    if (detail) sqlite3_bind_text(st, 9, detail, -1, SQLITE_TRANSIENT);
    else        sqlite3_bind_null(st, 9);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

/* Resolve a user's display name for denormalizing into an entry, so the log
 * still reads sensibly after the user is removed. */
static char *audit_name_of(sqlite3 *db, uint64_t uid) {
    if (!uid) return NULL;
    sqlite3_stmt *st = NULL;
    char *out = NULL;
    if (sqlite3_prepare_v2(db, "SELECT COALESCE(display_name, subject) FROM users WHERE id=?;",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)uid);
        if (sqlite3_step(st) == SQLITE_ROW) {
            const char *v = (const char *)sqlite3_column_text(st, 0);
            if (v) out = strdup(v);
        }
        sqlite3_finalize(st);
    }
    return out;
}

/* Convenience: log with the actor's name looked up. */
static void audit_actor(sqlite3 *db, int family, const char *action,
                        uint64_t actor_id, uint64_t target_id, const char *target,
                        int outcome, const char *detail) {
    char *nm = audit_name_of(db, actor_id);
    audit_log(db, family, action, actor_id, nm, target_id, target, outcome, detail);
    free(nm);
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
    free(j->ch_name);
    free(j->emoji);
    free(j->cert_pem);
    free(j->key_pem);
    free(j->enroll_privkey);
    free(j->enroll_audience);
    free(j->filename);
    free(j->mime);
    free(j->cs_client_type);
    free(j->cs_key);
    free(j->cs_value);
    free(j->pf_name);
    free(j->pf_old_pw);
    free(j->pf_new_pw);
    free(j->device_token);
    free(j);
}

/* Free the heap metadata of an attachment list (filename/mime per entry). */
static void free_attach_meta(oc_attach_meta *a, size_t n) {
    for (size_t i = 0; i < n; i++) { free(a[i].filename); free(a[i].mime); }
}

void oc_dbres_free(oc_dbres *r) {
    if (!r) return;
    free(r->body);
    free(r->members);
    free(r->author_name);
    free_attach_meta(r->attach, r->n_attach);
    for (size_t i = 0; i < r->n_replay; i++) { free(r->replay[i].body); free(r->replay[i].author_name); free_attach_meta(r->replay[i].attach, r->replay[i].n_attach); }
    free(r->replay);
    for (size_t i = 0; i < r->n_rreact; i++) free(r->rreact[i].emoji);
    free(r->rreact);
    free(r->ch_name);
    for (size_t i = 0; i < r->n_chlist; i++) free(r->chlist[i].name);
    free(r->chlist);
    for (size_t i = 0; i < r->n_ulist; i++) { free(r->ulist[i].email); free(r->ulist[i].display_name); }
    free(r->ulist);
    free(r->emoji);
    for (size_t i = 0; i < r->n_rlist; i++) free(r->rlist[i].emoji);
    free(r->rlist);
    for (size_t i = 0; i < r->n_thread; i++) { free(r->thread[i].body); free(r->thread[i].author_name); free_attach_meta(r->thread[i].attach, r->thread[i].n_attach); }
    free(r->thread);
    for (size_t i = 0; i < r->n_search; i++) { free(r->search[i].body); free(r->search[i].author_name); free_attach_meta(r->search[i].attach, r->search[i].n_attach); }
    free(r->search);
    free(r->cert_pem);
    free(r->key_pem);
    free(r->enroll_privkey);
    free(r->enroll_audience);
    free(r->storage_key);
    for (size_t i = 0; i < r->n_reclaim; i++) free(r->reclaim[i].storage_key);
    free(r->reclaim);
    for (size_t i = 0; i < r->n_audit; i++) {
        free(r->audit[i].actor_name); free(r->audit[i].action);
        free(r->audit[i].target);     free(r->audit[i].detail);
    }
    free(r->audit);
    free(r->filename);
    free(r->mime);
    for (size_t i = 0; i < r->n_whlist; i++) free(r->whlist[i].label);
    free(r->whlist);
    free(r->nprefs);
    free(r->cs_client_type);
    for (size_t i = 0; i < r->n_cslist; i++) { free(r->cslist[i].key); free(r->cslist[i].value); }
    free(r->cslist);
    free(r->profile_name);
    free(r->rcur);
    free(r);
}

/* Replay is bounded per request; a client with more backlog issues a follow-up
 * BACKFILL_REQUEST with an advanced cursor (PROTOCOL.md §6.2). */
#define OC_BACKFILL_MAX 500
/* Per-channel tail handed to a client that holds no history. Big enough to
 * fill a tall window and scroll a little; small enough that a cold client in
 * many channels does not spend its whole OC_BACKFILL_MAX budget on the first. */
#define OC_BACKFILL_TAIL 60

/* --- Job processing (runs on the writer thread) ------------------------- */

/* Session lifetime — the daemon's own expiry, no longer tied to a provider
 * token (REQ-181, AUTH.md §4). */
#define OC_SESSION_TTL_MS (30ull * 24 * 60 * 60 * 1000)

/* Invite-token lifetime (AUTH.md §2): the invitee must redeem within this window. */
#define OC_INVITE_TTL_MS (7ull * 24 * 60 * 60 * 1000)

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

/* True when adding a NEW user `subject` would exceed the cap (CP-7,
 * OPENCHIME_MAX_USERS). max_users <= 0 is unlimited; an already-existing subject is
 * never capped (idempotent bootstrap / re-login). Counts active users only — a
 * removed member (disabled=1) frees a seat. */
static int user_slots_full(sqlite3 *db, const char *subject, size_t sublen, int max_users) {
    if (max_users <= 0) return 0;
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "SELECT 1 FROM users WHERE subject=?;", -1, &st, NULL);
    sqlite3_bind_text(st, 1, subject, (int)sublen, SQLITE_TRANSIENT);
    int exists = (sqlite3_step(st) == SQLITE_ROW);
    sqlite3_finalize(st);
    if (exists) return 0;
    int count = 0;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM users WHERE disabled=0;", -1, &st, NULL);
    if (sqlite3_step(st) == SQLITE_ROW) count = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return count >= max_users;
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
        "INSERT OR IGNORE INTO users(subject, display_name, role, created_at_ms) "
        "VALUES(?, ?, ?, ?);", -1, &st, NULL);
    sqlite3_bind_text(st, 1, subject, (int)sublen, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, username, (int)ulen, SQLITE_TRANSIENT);   /* display name = login name */
    sqlite3_bind_text(st, 3, u8_to_role(role), -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 4, (sqlite3_int64)now);
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

/* Has this user been removed from the tenant (REQ-033)? A removed member is
 * locked out of every auth path but their row survives for message authorship. */
static int user_disabled(sqlite3 *db, uint64_t uid) {
    sqlite3_stmt *st = NULL;
    int disabled = 0;
    sqlite3_prepare_v2(db, "SELECT disabled FROM users WHERE id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)uid);
    if (sqlite3_step(st) == SQLITE_ROW) disabled = sqlite3_column_int(st, 0) != 0;
    sqlite3_finalize(st);
    return disabled;
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

static oc_dbres *process_register(oc_dbwriter *w, const oc_job *j) {
    sqlite3 *db = w->db;
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    /* Registered-user cap (CP-7): refuse a new account at the workspace limit. */
    if (w->max_users > 0) {
        char subject[256];
        const char *uname = j->username ? j->username : "";
        size_t sublen = local_subject(subject, sizeof subject, uname, strlen(uname));
        if (sublen > 0 && user_slots_full(db, subject, sublen, w->max_users)) {
            r->type = OC_RES_REGISTER_ERR; r->err_code = OC_ERR_USER_LIMIT; return r;
        }
    }
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
            /* Throttled attempts are dropped SILENTLY and are deliberately not
             * audited (REQ-251b). Logging them would hand an attacker the very
             * amplification the limiter exists to remove: one packet, one row.
             * The failures that got far enough to be checked are logged below,
             * and the limiter caps those at 5/min/account and 20/min/source. */
            r->type = OC_RES_AUTH_ERR; r->err_code = OC_ERR_AUTH_RATE_LIMITED; return r;
        }
        uid = verify_local(db, (const char *)user.ptr, user.len,
                           (const char *)pass.ptr, pass.len, &role);
        if (uid == 0) {
            oc_ratelimit_record(w->auth_rl, acct, now);
            if (has_src) oc_ratelimit_record(w->source_rl, j->source, now);
            /* The attempted username and source, never the attempted password
             * (ARCH-79). Bounded by the limiter above — a throttled attempt has
             * already returned — so a spray yields a handful of rows per window
             * rather than one per packet. */
            audit_log(db, OC_AUDIT_SECURITY, "auth.failed", 0, NULL, 0, acct, 0,
                      j->source[0] ? j->source : NULL);
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
        /* Registered-user cap (CP-7): a first-time OIDC login can't provision a new
         * user past the workspace limit (an existing user still logs in). */
        if (user_slots_full(db, subject, strlen(subject), w->max_users)) {
            r->type = OC_RES_AUTH_ERR; r->err_code = OC_ERR_USER_LIMIT; return r;
        }
        uid = upsert_oidc_user(db, subject, claims.email, claims.name);
        if (uid) role = get_role(db, uid);   /* membership ensured on the common path */
    } else if (j->method == OC_AUTH_SESSION) {
        uid = lookup_session(db, (const uint8_t *)j->token, j->token_len, &role, &sess_exp);
        fresh = 0;
    } else {
        r->type = OC_RES_AUTH_ERR; r->err_code = OC_ERR_AUTH_REQUIRED; return r;
    }

    /* A removed member is refused regardless of how identity was proven (REQ-033).
     * Reported as an invalid credential so removal isn't disclosed. */
    if (uid && user_disabled(db, uid)) uid = 0;
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
        /* A denied privileged action is exactly what an audit log is for. */
        audit_actor(db, OC_AUDIT_SECURITY, "role.change.denied", j->user_id,
                    j->target_user_id, NULL, 0, "insufficient privilege");
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
    audit_actor(db, OC_AUDIT_ADMIN, "role.change", j->user_id,
                j->target_user_id, u8_to_role(next), 1, NULL);
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
        /* TRANSIENT: `hash` is block-scoped and would dangle at sqlite3_step()
         * (below, outside this block) under SQLITE_STATIC — copy it now. */
        sqlite3_bind_blob(st, 1, hash, sizeof hash, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)j->user_id);
    }
    sqlite3_step(st);
    sqlite3_finalize(st);
    audit_actor(db, OC_AUDIT_SECURITY, "session.revoke", j->user_id, 0,
                j->scope == OC_LOGOUT_ALL ? "all" : "this", 1, NULL);
    r->type = OC_RES_LOGOUT_OK;
    return r;
}

/* --- Admin ops (REQ-033, tenant-level; owner/admin) --------------------- */

/* Enumerate every tenant user. Available to any authenticated user (a client
 * needs the roster to address messages and pick op targets). */
static oc_dbres *process_list_users(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->type = OC_RES_USER_LIST;

    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "SELECT id, role, disabled, COALESCE(email,''), COALESCE(display_name,'') "
        "FROM users ORDER BY id;", -1, &st, NULL);
    size_t cap = 8, n = 0;
    oc_user_row *arr = malloc(cap * sizeof *arr);
    while (arr && sqlite3_step(st) == SQLITE_ROW) {
        if (n == cap) { cap *= 2; oc_user_row *g = realloc(arr, cap * sizeof *arr); if (!g) break; arr = g; }
        arr[n].user_id      = (uint64_t)sqlite3_column_int64(st, 0);
        arr[n].role         = role_to_u8((const char *)sqlite3_column_text(st, 1));
        arr[n].disabled     = (uint8_t)(sqlite3_column_int(st, 2) != 0);
        arr[n].email        = strdup((const char *)sqlite3_column_text(st, 3));
        arr[n].display_name = strdup((const char *)sqlite3_column_text(st, 4));
        n++;
    }
    sqlite3_finalize(st);
    r->ulist = arr;
    r->n_ulist = n;
    return r;
}

/* Mint an invite token for a new local account (REQ-033). Owner/admin only; only
 * an owner may invite at an elevated (admin/owner) role. Only the token's SHA-256
 * is stored; the raw token is returned once for the actor to share. */
/* Mint an invite for `role`: random token to the caller, only its SHA-256 +
 * expiry stored. `created_by` is the issuing user (0 -> NULL, e.g. a first-run
 * setup token with no issuer). Returns 0 and fills token/expiry, or -1. */
static int mint_invite(sqlite3 *db, uint64_t created_by, uint8_t role,
                       uint8_t token[OC_INVITE_TOKEN_LEN], uint64_t *expiry_out) {
    uint8_t hash[OC_SHA256_LEN];
    if (oc_rand_bytes(token, OC_INVITE_TOKEN_LEN) != 0 ||
        oc_sha256(token, OC_INVITE_TOKEN_LEN, hash) != 0) return -1;
    uint64_t expiry = dbw_now_ms() + OC_INVITE_TTL_MS;
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "INSERT INTO invites(token_hash,created_by,role,expires_at_ms) VALUES(?,?,?,?);",
        -1, &st, NULL);
    sqlite3_bind_blob(st, 1, hash, sizeof hash, SQLITE_STATIC);
    if (created_by) sqlite3_bind_int64(st, 2, (sqlite3_int64)created_by);
    else            sqlite3_bind_null(st, 2);
    sqlite3_bind_text(st, 3, u8_to_role(role), -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 4, (sqlite3_int64)expiry);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) return -1;
    if (expiry_out) *expiry_out = expiry;
    return 0;
}

static oc_dbres *process_invite_user(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;

    uint8_t actor_role = OC_ROLE_MEMBER;
    if (!user_role(db, j->user_id, &actor_role) || !oc_role_can_manage_members(actor_role)) {
        r->type = OC_RES_INVITE_ERR; r->err_code = OC_ERR_FORBIDDEN; return r;
    }
    uint8_t want = j->role > OC_ROLE_OWNER ? OC_ROLE_MEMBER : j->role;
    if (want != OC_ROLE_MEMBER && actor_role != OC_ROLE_OWNER) {
        r->type = OC_RES_INVITE_ERR; r->err_code = OC_ERR_FORBIDDEN; return r;
    }

    uint8_t token[OC_INVITE_TOKEN_LEN]; uint64_t expiry = 0;
    if (mint_invite(db, j->user_id, want, token, &expiry) != 0) {
        r->type = OC_RES_INVITE_ERR; r->err_code = OC_ERR_INTERNAL; return r;
    }
    audit_actor(db, OC_AUDIT_ADMIN, "user.invite", j->user_id, 0,
                u8_to_role(j->role), 1, NULL);
    r->type = OC_RES_INVITE_OK;
    memcpy(r->session_token, token, OC_INVITE_TOKEN_LEN);  /* carries the invite token */
    r->session_expiry = expiry;
    r->role = want;
    return r;
}

/* First-run bootstrap (REQ-024): if the tenant has no owner yet, mint a one-time
 * owner invite so the operator can create the first owner by redeeming it (no
 * pre-existing admin needed, air-gapped-safe). Returns INVITE_OK with the token,
 * or INVITE_ERR/err_code 0 when an owner already exists (nothing to do). */
static oc_dbres *process_setup_invite(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    if (count_owners(db) > 0) { r->type = OC_RES_INVITE_ERR; r->err_code = 0; return r; }
    uint8_t token[OC_INVITE_TOKEN_LEN]; uint64_t expiry = 0;
    if (mint_invite(db, 0, OC_ROLE_OWNER, token, &expiry) != 0) {
        r->type = OC_RES_INVITE_ERR; r->err_code = OC_ERR_INTERNAL; return r;
    }
    r->type = OC_RES_INVITE_OK;
    memcpy(r->session_token, token, OC_INVITE_TOKEN_LEN);
    r->session_expiry = expiry;
    r->role = OC_ROLE_OWNER;
    return r;
}

/* Redeem an invite (pre-auth): create the local account with the invite's role,
 * single-use-consume the token, and mint a session — the redeeming client is now
 * authenticated (result is an AUTH_OK). Any failure is a non-disclosing
 * AUTH_INVALID_TOKEN. */
static oc_dbres *process_redeem(oc_dbwriter *w, const oc_job *j) {
    sqlite3 *db = w->db;
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;

    if (j->token_len != OC_INVITE_TOKEN_LEN) {
        r->type = OC_RES_AUTH_ERR; r->err_code = OC_ERR_AUTH_INVALID_TOKEN; return r;
    }
    uint8_t hash[OC_SHA256_LEN];
    if (oc_sha256((const uint8_t *)j->token, j->token_len, hash) != 0) {
        r->type = OC_RES_AUTH_ERR; r->err_code = OC_ERR_AUTH_INVALID_TOKEN; return r;
    }
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "SELECT role, expires_at_ms, consumed_at_ms FROM invites WHERE token_hash=?;",
        -1, &st, NULL);
    sqlite3_bind_blob(st, 1, hash, sizeof hash, SQLITE_STATIC);
    uint8_t role = OC_ROLE_MEMBER; uint64_t expiry = 0; int consumed = 1, found = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        role     = role_to_u8((const char *)sqlite3_column_text(st, 0));
        expiry   = (uint64_t)sqlite3_column_int64(st, 1);
        consumed = sqlite3_column_type(st, 2) != SQLITE_NULL;
        found = 1;
    }
    sqlite3_finalize(st);
    if (!found || consumed || (expiry != 0 && dbw_now_ms() >= expiry)) {
        r->type = OC_RES_AUTH_ERR; r->err_code = OC_ERR_AUTH_INVALID_TOKEN; return r;
    }

    /* The username must be free — register_local is INSERT-OR-IGNORE, so a
     * collision would silently map to the existing account without setting the
     * new password. Reject rather than hijack. */
    const char *user = j->username ? j->username : "";
    const char *pass = j->password ? j->password : "";
    char subject[256];
    size_t sublen = local_subject(subject, sizeof subject, user, strlen(user));
    if (sublen == 0 || strlen(pass) == 0) {
        r->type = OC_RES_AUTH_ERR; r->err_code = OC_ERR_AUTH_INVALID_TOKEN; return r;
    }
    sqlite3_prepare_v2(db, "SELECT 1 FROM users WHERE subject=?;", -1, &st, NULL);
    sqlite3_bind_text(st, 1, subject, (int)sublen, SQLITE_TRANSIENT);
    int taken = (sqlite3_step(st) == SQLITE_ROW);
    sqlite3_finalize(st);
    if (taken) { r->type = OC_RES_AUTH_ERR; r->err_code = OC_ERR_AUTH_INVALID_TOKEN; return r; }

    /* Registered-user cap (CP-7): a redeem that would create a new member is
     * refused once the workspace is at its limit. */
    if (user_slots_full(db, subject, sublen, w->max_users)) {
        r->type = OC_RES_AUTH_ERR; r->err_code = OC_ERR_USER_LIMIT; return r;
    }

    uint64_t uid = register_local(db, user, strlen(user), pass, strlen(pass), role, j->iterations);
    if (uid == 0) { r->type = OC_RES_AUTH_ERR; r->err_code = OC_ERR_INTERNAL; return r; }

    /* Consume the invite (single-use); the single writer thread makes this atomic
     * with the account creation above. */
    sqlite3_prepare_v2(db, "UPDATE invites SET consumed_at_ms=? WHERE token_hash=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)dbw_now_ms());
    sqlite3_bind_blob(st, 2, hash, sizeof hash, SQLITE_STATIC);
    sqlite3_step(st);
    sqlite3_finalize(st);

    uint8_t token[OC_SESSION_TOKEN_LEN]; uint64_t sexp = 0;
    if (mint_session(db, uid, token, &sexp) != 0) {
        r->type = OC_RES_AUTH_ERR; r->err_code = OC_ERR_INTERNAL; return r;
    }
    r->type = OC_RES_AUTH_OK;
    r->user_id = uid;
    r->role = role;
    memcpy(r->session_token, token, sizeof token);
    r->has_session_token = 1;
    r->session_expiry = sexp;
    return r;
}

/* Load the persisted TLS identity (ARCH-66b); result cert_pem/key_pem are NULL
 * when none is stored yet (first run). */
static oc_dbres *process_load_identity(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->type = OC_RES_IDENTITY;
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "SELECT cert_pem, key_pem FROM server_identity WHERE id=1;", -1, &st, NULL);
    if (sqlite3_step(st) == SQLITE_ROW) {
        r->cert_pem = strdup((const char *)sqlite3_column_text(st, 0));
        r->key_pem  = strdup((const char *)sqlite3_column_text(st, 1));
    }
    sqlite3_finalize(st);
    return r;
}

/* Persist the TLS identity so a database restored onto a new box keeps the same
 * TOFU cert. */
static oc_dbres *process_store_identity(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO server_identity(id,cert_pem,key_pem,created_at_ms) "
        "VALUES(1,?,?,?);", -1, &st, NULL);
    sqlite3_bind_text(st, 1, j->cert_pem ? j->cert_pem : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, j->key_pem ? j->key_pem : "", -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)dbw_now_ms());
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc == SQLITE_DONE) r->type = OC_RES_OK;
    else { r->err_code = OC_ERR_INTERNAL; }
    return r;
}

/* Load the persisted federated-enrollment identity (CP-8); enroll_present=0 when
 * none is stored yet (first run). */
static oc_dbres *process_load_enrollment(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->type = OC_RES_ENROLLMENT;
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "SELECT privkey_pem, audience, state FROM enrollment WHERE id=1;", -1, &st, NULL);
    if (sqlite3_step(st) == SQLITE_ROW) {
        r->enroll_privkey  = strdup((const char *)sqlite3_column_text(st, 0));
        r->enroll_audience = strdup((const char *)sqlite3_column_text(st, 1));
        const char *state  = (const char *)sqlite3_column_text(st, 2);
        r->enroll_active   = (state && strcmp(state, "active") == 0);
        r->enroll_present  = 1;
    }
    sqlite3_finalize(st);
    return r;
}

/* Persist the enrollment keypair + audience + state (CP-8). */
static oc_dbres *process_store_enrollment(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO enrollment(id,privkey_pem,audience,state,activated_at_ms,created_at_ms) "
        "VALUES(1,?,?,?,?,?);", -1, &st, NULL);
    sqlite3_bind_text(st, 1, j->enroll_privkey  ? j->enroll_privkey  : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, j->enroll_audience ? j->enroll_audience : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, j->enroll_active ? "active" : "pending", -1, SQLITE_STATIC);
    if (j->enroll_active) sqlite3_bind_int64(st, 4, (sqlite3_int64)dbw_now_ms());
    else sqlite3_bind_null(st, 4);
    sqlite3_bind_int64(st, 5, (sqlite3_int64)dbw_now_ms());
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc == SQLITE_DONE) r->type = OC_RES_OK;
    else { r->err_code = OC_ERR_INTERNAL; }
    return r;
}

/* Remove a member from the tenant (REQ-033): lock them out (disabled=1) and
 * revoke access — sessions, channel memberships, and local password. The row
 * survives so their authored messages keep a valid author. Owner/admin only; an
 * admin cannot remove an admin/owner, and the last owner cannot be removed. */
static oc_dbres *process_remove_user(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;

    uint8_t actor_role = OC_ROLE_MEMBER, target_role = OC_ROLE_MEMBER;
    if (!user_role(db, j->user_id, &actor_role) ||
        !user_role(db, j->target_user_id, &target_role)) {
        r->type = OC_RES_USER_ERR; r->err_code = OC_ERR_FORBIDDEN; return r;
    }
    if (!oc_role_can_manage_members(actor_role)) {
        r->type = OC_RES_USER_ERR; r->err_code = OC_ERR_FORBIDDEN; return r;
    }
    if (target_role != OC_ROLE_MEMBER && actor_role != OC_ROLE_OWNER) {
        r->type = OC_RES_USER_ERR; r->err_code = OC_ERR_FORBIDDEN; return r;
    }
    if (target_role == OC_ROLE_OWNER && count_owners(db) <= 1) {
        r->type = OC_RES_USER_ERR; r->err_code = OC_ERR_LAST_OWNER; return r;
    }

    sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "UPDATE users SET disabled=1 WHERE id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->target_user_id); sqlite3_step(st); sqlite3_finalize(st);
    sqlite3_prepare_v2(db, "DELETE FROM sessions WHERE user_id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->target_user_id); sqlite3_step(st); sqlite3_finalize(st);
    /* Their DM conversations go entirely — messages, membership and the channel
     * itself. Deleting only the membership row (what this used to do) left a
     * half-membered DM that no OPEN_DM could match, so the next attempt created a
     * DUPLICATE conversation; migration 0019's unique dm_key now forbids that
     * state, which makes removing the channel the only coherent option. */
    sqlite3_prepare_v2(db,
        "DELETE FROM messages WHERE channel_id IN (SELECT channel_id FROM channel_members "
        "  WHERE user_id=?1 AND channel_id IN (SELECT id FROM channels WHERE kind='dm'));",
        -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->target_user_id); sqlite3_step(st); sqlite3_finalize(st);
    sqlite3_prepare_v2(db,
        "DELETE FROM channels WHERE kind='dm' AND id IN "
        "  (SELECT channel_id FROM channel_members WHERE user_id=?1);", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->target_user_id); sqlite3_step(st); sqlite3_finalize(st);
    /* Now the ordinary channel memberships (the DM rows went with their channels,
     * but any stragglers are swept here too). */
    sqlite3_prepare_v2(db, "DELETE FROM channel_members WHERE user_id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->target_user_id); sqlite3_step(st); sqlite3_finalize(st);
    sqlite3_prepare_v2(db,
        "DELETE FROM channel_members WHERE channel_id NOT IN (SELECT id FROM channels);",
        -1, &st, NULL);
    sqlite3_step(st); sqlite3_finalize(st);
    sqlite3_prepare_v2(db, "DELETE FROM local_credentials WHERE user_id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->target_user_id); sqlite3_step(st); sqlite3_finalize(st);
    sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);

    audit_actor(db, OC_AUDIT_ADMIN, "user.remove", j->user_id,
                j->target_user_id, NULL, 1, NULL);
    r->type = OC_RES_USER_UPDATED;
    r->user_id = j->target_user_id;
    r->role = target_role;
    r->disabled = 1;
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

/* Does the channel exist (any kind — a named channel or a DM)? Fills *is_public.
 * Used by the read/post-access gate, which applies to DMs too. */
static int channel_exists(sqlite3 *db, uint64_t channel_id, uint8_t *is_public) {
    sqlite3_stmt *st = NULL;
    int found = 0;
    sqlite3_prepare_v2(db,
        "SELECT is_public FROM channels WHERE id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)channel_id);
    if (sqlite3_step(st) == SQLITE_ROW) {
        if (is_public) *is_public = (uint8_t)(sqlite3_column_int(st, 0) != 0);
        found = 1;
    }
    sqlite3_finalize(st);
    return found;
}

/* Does a *named channel* (kind='channel', not a DM) exist? The channel-
 * management ops (join/leave/invite/remove) operate only on named channels;
 * DMs are managed only via OPEN_DM. Fills *is_public. */
static int named_channel_exists(sqlite3 *db, uint64_t channel_id, uint8_t *is_public) {
    sqlite3_stmt *st = NULL;
    int found = 0;
    sqlite3_prepare_v2(db,
        "SELECT is_public FROM channels WHERE id=? AND kind='channel';", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)channel_id);
    if (sqlite3_step(st) == SQLITE_ROW) {
        if (is_public) *is_public = (uint8_t)(sqlite3_column_int(st, 0) != 0);
        found = 1;
    }
    sqlite3_finalize(st);
    return found;
}

static void add_membership(sqlite3 *db, uint64_t channel_id, uint64_t user_id) {
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "INSERT OR IGNORE INTO channel_members(channel_id,user_id,joined_at_ms) "
        "VALUES(?,?,?);", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)channel_id);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)user_id);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)dbw_now_ms());
    sqlite3_step(st);
    sqlite3_finalize(st);
}

/* May the user read (backfill) this channel? Public channels are open to any
 * tenant user; private channels are members-only (REQ-031). */
static int channel_read_access(sqlite3 *db, uint64_t channel_id, uint64_t user_id) {
    uint8_t is_public = 0;
    if (!channel_exists(db, channel_id, &is_public)) return 0;
    return is_public || is_member(db, channel_id, user_id);
}

/* Post access (REQ-031). CH_OK: allowed (a public channel auto-joins the poster
 * so broadcasts reach them); CH_UNKNOWN: no such channel; CH_DENIED: a private
 * channel the user does not belong to. */
enum { CH_OK = 0, CH_UNKNOWN = 1, CH_DENIED = 2 };
static int channel_post_access(sqlite3 *db, uint64_t channel_id, uint64_t user_id) {
    uint8_t is_public = 0;
    if (!channel_exists(db, channel_id, &is_public)) return CH_UNKNOWN;
    if (is_member(db, channel_id, user_id)) return CH_OK;
    if (is_public) { add_membership(db, channel_id, user_id); return CH_OK; }
    return CH_DENIED;
}

/* Load the attachments linked to message `mid` into `out` (up to OC_MAX_ATTACH),
 * ascending id. Serves the SEND broadcast and backfill replay (REQ-140). */
static void load_message_attachments(sqlite3 *db, uint64_t mid, oc_attach_meta *out, size_t *n) {
    *n = 0;
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "SELECT id, filename, mime, size, reclaimed_at_ms FROM attachments "
        "WHERE message_id=? ORDER BY id LIMIT ?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)mid);
    sqlite3_bind_int(st, 2, (int)OC_MAX_ATTACH);
    while (sqlite3_step(st) == SQLITE_ROW && *n < OC_MAX_ATTACH) {
        oc_attach_meta *a = &out[(*n)++];
        a->id = (uint64_t)sqlite3_column_int64(st, 0);
        a->filename = strdup((const char *)sqlite3_column_text(st, 1));
        a->mime = strdup((const char *)sqlite3_column_text(st, 2));
        a->size = (uint64_t)sqlite3_column_int64(st, 3);
        /* Tombstoned: the row survives so the message stays readable, but the
         * bytes are gone. Telling the client here means it can render "no
         * longer available" in place rather than offering a download that is
         * guaranteed to fail (REQ-215/217). */
        a->reclaimed = sqlite3_column_int64(st, 4) != 0 ? 1 : 0;
    }
    sqlite3_finalize(st);
}

/* Link the caller's pending attachments to message `mid` (REQ-140). An id links
 * only if it is a finalized, still-unlinked attachment the same user uploaded to
 * this same channel; any other id is silently ignored (it simply isn't shared). */
static void link_attachments(sqlite3 *db, uint64_t mid, uint64_t channel_id,
                             uint64_t uploader_id, const uint64_t *ids, uint16_t n) {
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "UPDATE attachments SET message_id=? "
        "WHERE id=? AND message_id IS NULL AND sha256 IS NOT NULL "
        "  AND uploader_id=? AND channel_id=?;", -1, &st, NULL);
    for (uint16_t i = 0; i < n && i < OC_MAX_ATTACH; i++) {
        sqlite3_reset(st);
        sqlite3_bind_int64(st, 1, (sqlite3_int64)mid);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)ids[i]);
        sqlite3_bind_int64(st, 3, (sqlite3_int64)uploader_id);
        sqlite3_bind_int64(st, 4, (sqlite3_int64)channel_id);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
}

/* The author's display name (users.display_name), or NULL if unset. Caller frees.
 * Used to stamp a live BROADCAST with the sender's name (ARCH-74 client). */
static char *lookup_display_name(sqlite3 *db, uint64_t user_id) {
    sqlite3_stmt *st = NULL;
    char *name = NULL;
    if (sqlite3_prepare_v2(db, "SELECT display_name FROM users WHERE id=?;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)user_id);
        if (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char *dn = sqlite3_column_text(st, 0);
            if (dn && dn[0]) name = strdup((const char *)dn);
        }
        sqlite3_finalize(st);
    }
    return name;
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

    int acc = channel_post_access(db, j->channel_id, j->user_id);
    if (acc == CH_UNKNOWN) { r->type = OC_RES_SEND_ERR; r->err_code = OC_ERR_UNKNOWN_CHANNEL; return r; }
    if (acc == CH_DENIED)  { r->type = OC_RES_SEND_ERR; r->err_code = OC_ERR_NOT_A_MEMBER;   return r; }

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

    /* Link any referenced attachments atomically with the message (REQ-140), then
     * load their metadata for the broadcast so every member sees them inline. */
    if (j->n_attach) {
        link_attachments(db, mid, j->channel_id, j->user_id, j->attach_ids, j->n_attach);
        load_message_attachments(db, mid, r->attach, &r->n_attach);
    }

    sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);

    r->type = OC_RES_SEND_OK;
    r->message_id = mid;
    r->server_time = ts;
    r->author_name = lookup_display_name(db, j->user_id);   /* name for the live BROADCAST */
    if (j->body_len) { r->body = malloc(j->body_len); if (r->body) { memcpy(r->body, j->body, j->body_len); r->body_len = j->body_len; } }
    load_members(db, j->channel_id, r);
    return r;
}

/* Look up a message's author and tombstone state within a channel. Returns 1 if
 * the (channel_id, message_id) row exists, filling *author and *deleted (1 if
 * already tombstoned); 0 if there is no such message. */
static int message_lookup(sqlite3 *db, uint64_t channel_id, uint64_t message_id,
                          uint64_t *author, int *deleted) {
    sqlite3_stmt *st = NULL;
    int found = 0;
    sqlite3_prepare_v2(db,
        "SELECT author_id, deleted_at_ms FROM messages WHERE id=? AND channel_id=?;",
        -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)message_id);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)channel_id);
    if (sqlite3_step(st) == SQLITE_ROW) {
        *author  = (uint64_t)sqlite3_column_int64(st, 0);
        *deleted = sqlite3_column_type(st, 1) != SQLITE_NULL;
        found = 1;
    }
    sqlite3_finalize(st);
    return found;
}

/* Edit one's own message (REQ-051): only the author may edit, the body is
 * replaced and edited_at_ms is stamped while the original id/created_at/position
 * are kept. A tombstoned message is no longer editable. */
static oc_dbres *process_edit(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->channel_id = j->channel_id;
    r->message_id = j->message_id;

    uint64_t author = 0; int deleted = 0;
    if (!message_lookup(db, j->channel_id, j->message_id, &author, &deleted) || deleted) {
        r->type = OC_RES_EDIT_ERR; r->err_code = OC_ERR_UNKNOWN_MESSAGE; return r;
    }
    if (author != j->user_id) {   /* no moderator edit — author only (REQ-032) */
        r->type = OC_RES_EDIT_ERR; r->err_code = OC_ERR_FORBIDDEN; return r;
    }

    uint64_t ts = dbw_now_ms();
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "UPDATE messages SET body=?, edited_at_ms=? WHERE id=?;", -1, &st, NULL);
    sqlite3_bind_blob(st, 1, j->body, (int)j->body_len, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)ts);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)j->message_id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        r->type = OC_RES_EDIT_ERR; r->err_code = OC_ERR_INTERNAL; return r;
    }

    r->type = OC_RES_EDIT_OK;
    r->author_id = author;
    r->server_time = ts;   /* edited_at_ms */
    if (j->body_len) { r->body = malloc(j->body_len); if (r->body) { memcpy(r->body, j->body, j->body_len); r->body_len = j->body_len; } }
    load_members(db, j->channel_id, r);
    return r;
}

/* Delete a message as a tombstone (REQ-052): the author may delete their own,
 * and an admin/owner who belongs to the channel may delete any (moderation,
 * REQ-032). The body is nulled while id/author/timestamps survive; deleted_by
 * records who removed it, distinguishing a self- from a moderator-delete. */
static oc_dbres *process_delete(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->channel_id = j->channel_id;
    r->message_id = j->message_id;

    uint64_t author = 0; int deleted = 0;
    if (!message_lookup(db, j->channel_id, j->message_id, &author, &deleted) || deleted) {
        r->type = OC_RES_DELETE_ERR; r->err_code = OC_ERR_UNKNOWN_MESSAGE; return r;
    }
    /* The actor must belong to the channel in every case (REQ-031/032). */
    if (!is_member(db, j->channel_id, j->user_id)) {
        r->type = OC_RES_DELETE_ERR; r->err_code = OC_ERR_FORBIDDEN; return r;
    }
    if (author != j->user_id && !oc_role_can_moderate(get_role(db, j->user_id))) {
        r->type = OC_RES_DELETE_ERR; r->err_code = OC_ERR_FORBIDDEN; return r;
    }

    uint64_t ts = dbw_now_ms();
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "UPDATE messages SET body=NULL, deleted_at_ms=?, deleted_by=? WHERE id=?;",
        -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)ts);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)j->user_id);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)j->message_id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        r->type = OC_RES_DELETE_ERR; r->err_code = OC_ERR_INTERNAL; return r;
    }
    /* A tombstone has no body to react to; drop its reactions (REQ-052/070). */
    sqlite3_prepare_v2(db, "DELETE FROM reactions WHERE message_id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->message_id);
    sqlite3_step(st);
    sqlite3_finalize(st);

    /* Only a MODERATOR delete is auditable governance; a user removing their
     * own message is ordinary use and would drown the log (REQ-032 already
     * distinguishes the two via deleted_by). */
    if (author != j->user_id)
        audit_actor(db, OC_AUDIT_MODERATION, "message.delete", j->user_id,
                    j->message_id, NULL, 1, "moderator delete");

    r->type = OC_RES_DELETE_OK;
    r->author_id = author;
    r->user_id = j->user_id;   /* deleted_by */
    r->server_time = ts;       /* deleted_at_ms */
    load_members(db, j->channel_id, r);
    return r;
}

/* Fill a result's ch_* fields from the channel row + the actor's membership.
 * Returns 1 if the channel exists (result is a valid CHANNEL_INFO), 0 if not. */
static int load_channel_info(sqlite3 *db, uint64_t channel_id, uint64_t actor, oc_dbres *r) {
    sqlite3_stmt *st = NULL;
    int found = 0;
    sqlite3_prepare_v2(db,
        "SELECT kind, name, is_public, created_at_ms FROM channels WHERE id=?;",
        -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)channel_id);
    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *kn = sqlite3_column_text(st, 0);
        const unsigned char *nm = sqlite3_column_text(st, 1);
        r->ch_kind       = (kn && strcmp((const char *)kn, "dm") == 0) ? OC_CHANNEL_KIND_DM : OC_CHANNEL_KIND;
        r->ch_name       = strdup(nm ? (const char *)nm : "");
        r->ch_is_public  = (uint8_t)(sqlite3_column_int(st, 2) != 0);
        r->ch_created_at = (uint64_t)sqlite3_column_int64(st, 3);
        r->channel_id    = channel_id;
        found = 1;
    }
    sqlite3_finalize(st);
    if (found) r->ch_joined = (uint8_t)(is_member(db, channel_id, actor) ? 1 : 0);
    return found;
}

/* Create a named channel (REQ-050); the creator auto-joins. */
static oc_dbres *process_create_channel(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;

    size_t nlen = j->ch_name ? strlen(j->ch_name) : 0;
    if (nlen == 0 || nlen > OC_MAX_CHANNEL_NAME) {
        r->type = OC_RES_CHANNEL_ERR; r->err_code = OC_ERR_INVALID_CHANNEL; return r;
    }

    /* Names are unique case-insensitively (migration 0020). Check first so the
     * caller gets a specific "that name is taken" rather than a generic internal
     * error off the unique index — the index is the backstop, not the message. */
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "SELECT 1 FROM channels WHERE kind='channel' AND lower(name)=lower(?1);", -1, &st, NULL);
    sqlite3_bind_text(st, 1, j->ch_name, (int)nlen, SQLITE_STATIC);
    int taken = (sqlite3_step(st) == SQLITE_ROW);
    sqlite3_finalize(st);
    if (taken) {
        r->type = OC_RES_CHANNEL_ERR; r->err_code = OC_ERR_CHANNEL_EXISTS; return r;
    }

    sqlite3_prepare_v2(db,
        "INSERT INTO channels(kind,name,is_public,created_at_ms) VALUES('channel',?,?,?);",
        -1, &st, NULL);
    sqlite3_bind_text(st, 1, j->ch_name, (int)nlen, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, j->ch_is_public ? 1 : 0);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)dbw_now_ms());
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        r->type = OC_RES_CHANNEL_ERR; r->err_code = OC_ERR_INTERNAL; return r;
    }
    uint64_t cid = (uint64_t)sqlite3_last_insert_rowid(db);
    add_membership(db, cid, j->user_id);

    r->type = OC_RES_CHANNEL_INFO;
    load_channel_info(db, cid, j->user_id, r);
    return r;
}

/* List the channels visible to the user: every public channel plus any private
 * channel they belong to, each flagged with whether they are a member. */
static oc_dbres *process_list_channels(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->type = OC_RES_CHANNEL_LIST;

    /* Every public named channel, plus any named channel or DM the user belongs
     * to. DMs are members-only (is_public=0) and surface only to their two
     * participants (REQ-050). */
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "SELECT c.id, c.name, c.is_public, "
        "  EXISTS(SELECT 1 FROM channel_members m WHERE m.channel_id=c.id AND m.user_id=?1), c.kind, "
        /* Newest top-level message, and how many the user has not acked (REQ-090)
         * — so a cache-less client can sort and badge the sidebar immediately. */
        "  (SELECT COALESCE(MAX(x.created_at_ms),0) FROM messages x "
        "     WHERE x.channel_id=c.id AND x.parent_id IS NULL), "
        "  (SELECT COUNT(*) FROM messages x "
        "     WHERE x.channel_id=c.id AND x.parent_id IS NULL AND x.author_id<>?1 "
        "       AND x.id > COALESCE((SELECT dc.message_id FROM delivery_cursors dc "
        "                             WHERE dc.user_id=?1 AND dc.channel_id=c.id),0)), "
        /* A DM has no name; the client titles it by its peer, so send that too —
         * otherwise a cache-less client shows "direct message" until it opens one. */
        "  COALESCE((SELECT m2.user_id FROM channel_members m2 "
        "              WHERE m2.channel_id=c.id AND m2.user_id<>?1 LIMIT 1), ?1) "
        "FROM channels c WHERE "
        "  (c.kind='channel' AND (c.is_public=1 OR EXISTS(SELECT 1 FROM channel_members m WHERE m.channel_id=c.id AND m.user_id=?1))) "
        "  OR (c.kind='dm' AND EXISTS(SELECT 1 FROM channel_members m WHERE m.channel_id=c.id AND m.user_id=?1)) "
        "ORDER BY c.id;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->user_id);

    size_t cap = 8, n = 0;
    oc_channel_row *arr = malloc(cap * sizeof *arr);
    while (arr && sqlite3_step(st) == SQLITE_ROW) {
        if (n == cap) { cap *= 2; oc_channel_row *g = realloc(arr, cap * sizeof *arr); if (!g) break; arr = g; }
        const unsigned char *nm = sqlite3_column_text(st, 1);
        const unsigned char *kn = sqlite3_column_text(st, 4);
        arr[n].channel_id = (uint64_t)sqlite3_column_int64(st, 0);
        arr[n].name       = strdup(nm ? (const char *)nm : "");
        arr[n].is_public  = (uint8_t)(sqlite3_column_int(st, 2) != 0);
        arr[n].joined     = (uint8_t)(sqlite3_column_int(st, 3) != 0);
        arr[n].kind       = (kn && strcmp((const char *)kn, "dm") == 0) ? OC_CHANNEL_KIND_DM : OC_CHANNEL_KIND;
        arr[n].last_message_at = (uint64_t)sqlite3_column_int64(st, 5);
        arr[n].unread          = (uint32_t)sqlite3_column_int64(st, 6);
        arr[n].peer_id         = (uint64_t)sqlite3_column_int64(st, 7);
        n++;
    }
    sqlite3_finalize(st);
    r->chlist = arr;
    r->n_chlist = n;
    return r;
}

/* Join a channel: public channels are self-joinable; a private channel requires
 * an existing membership (obtained via INVITE), else FORBIDDEN. */
static oc_dbres *process_join_channel(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;

    uint8_t is_public = 0;
    if (!named_channel_exists(db, j->channel_id, &is_public)) {
        r->type = OC_RES_CHANNEL_ERR; r->err_code = OC_ERR_UNKNOWN_CHANNEL; return r;
    }
    if (!is_public && !is_member(db, j->channel_id, j->user_id)) {
        r->type = OC_RES_CHANNEL_ERR; r->err_code = OC_ERR_FORBIDDEN; return r;
    }
    add_membership(db, j->channel_id, j->user_id);
    r->type = OC_RES_CHANNEL_INFO;
    load_channel_info(db, j->channel_id, j->user_id, r);
    return r;
}

/* Leave a channel: drop the caller's own membership (idempotent). */
static oc_dbres *process_leave_channel(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;

    if (!named_channel_exists(db, j->channel_id, NULL)) {
        r->type = OC_RES_CHANNEL_ERR; r->err_code = OC_ERR_UNKNOWN_CHANNEL; return r;
    }
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "DELETE FROM channel_members WHERE channel_id=? AND user_id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->channel_id);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)j->user_id);
    sqlite3_step(st);
    sqlite3_finalize(st);

    r->type = OC_RES_CHANNEL_INFO;
    load_channel_info(db, j->channel_id, j->user_id, r);   /* ch_joined now 0 */
    return r;
}

/* Invite another user to a channel (REQ-033, channel-level): any existing member
 * may add anyone — the mechanism that makes a private channel reachable. The
 * result acks the actor and flags the target so the net thread can push it a
 * CHANNEL_INFO. */
static oc_dbres *process_invite_channel(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;

    if (!named_channel_exists(db, j->channel_id, NULL)) {
        r->type = OC_RES_CHANNEL_ERR; r->err_code = OC_ERR_UNKNOWN_CHANNEL; return r;
    }
    if (!is_member(db, j->channel_id, j->user_id)) {
        r->type = OC_RES_CHANNEL_ERR; r->err_code = OC_ERR_NOT_A_MEMBER; return r;
    }
    uint8_t trole = OC_ROLE_MEMBER;
    if (!user_role(db, j->target_user_id, &trole)) {   /* unknown target: no disclosure */
        r->type = OC_RES_CHANNEL_ERR; r->err_code = OC_ERR_FORBIDDEN; return r;
    }
    add_membership(db, j->channel_id, j->target_user_id);

    r->type = OC_RES_CHANNEL_INFO;
    load_channel_info(db, j->channel_id, j->user_id, r);
    r->push_user_id = j->target_user_id;
    return r;
}

/* Remove another user from a channel (REQ-033): any existing member may. */
static oc_dbres *process_remove_channel(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;

    if (!named_channel_exists(db, j->channel_id, NULL)) {
        r->type = OC_RES_CHANNEL_ERR; r->err_code = OC_ERR_UNKNOWN_CHANNEL; return r;
    }
    if (!is_member(db, j->channel_id, j->user_id)) {
        r->type = OC_RES_CHANNEL_ERR; r->err_code = OC_ERR_NOT_A_MEMBER; return r;
    }
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "DELETE FROM channel_members WHERE channel_id=? AND user_id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->channel_id);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)j->target_user_id);
    sqlite3_step(st);
    sqlite3_finalize(st);

    r->type = OC_RES_CHANNEL_INFO;
    load_channel_info(db, j->channel_id, j->user_id, r);
    return r;
}

/* Open (or get) the DM between the caller and target (REQ-050). A DM is a
 * kind='dm' channel with exactly its participants — two for a normal DM, or one
 * for a **self-DM** (target == self: a personal "notes to self" space, REQ-055).
 * Idempotent — an existing DM is returned rather than a duplicate created.
 * Replies CHANNEL_INFO (kind=DM), pushed to the peer (not for a self-DM).
 * Messaging/backfill/search then work through the normal membership path. */
static oc_dbres *process_open_dm(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    uint64_t self = j->user_id, other = j->target_user_id;
    if (other == 0) {   /* no such user */
        r->type = OC_RES_CHANNEL_ERR; r->err_code = OC_ERR_FORBIDDEN; return r;
    }
    int self_dm = (other == self);
    uint8_t trole = OC_ROLE_MEMBER;
    if (!self_dm && !user_role(db, other, &trole)) {   /* unknown target: no disclosure */
        r->type = OC_RES_CHANNEL_ERR; r->err_code = OC_ERR_FORBIDDEN; return r;
    }

    /* A DM's IDENTITY is its participant set, stored as `dm_key` under a unique
     * index (migration 0019) — not its membership rows, which anything that
     * removes a user could delete out from under us, stranding the channel and
     * letting the next OPEN_DM create a duplicate. One indexed probe. */
    char key[48];
    if (self_dm) snprintf(key, sizeof key, "%llu", (unsigned long long)self);
    else snprintf(key, sizeof key, "%llu,%llu",
                  (unsigned long long)(self < other ? self : other),
                  (unsigned long long)(self < other ? other : self));

    uint64_t cid = 0;
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "SELECT id FROM channels WHERE kind='dm' AND dm_key=?1;", -1, &st, NULL);
    sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) cid = (uint64_t)sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);

    /* Membership may have been damaged by an older build; re-assert it so the
     * conversation stays reachable rather than silently empty. */
    if (cid != 0) {
        add_membership(db, cid, self);
        if (!self_dm) add_membership(db, cid, other);
    }

    if (cid == 0) {
        sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);
        sqlite3_prepare_v2(db,
            "INSERT INTO channels(kind,name,is_public,created_at_ms,dm_key) "
            "VALUES('dm',NULL,0,?1,?2);", -1, &st, NULL);
        sqlite3_bind_int64(st, 1, (sqlite3_int64)dbw_now_ms());
        sqlite3_bind_text(st, 2, key, -1, SQLITE_TRANSIENT);
        int rc = sqlite3_step(st);
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE) {
            sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
            r->type = OC_RES_CHANNEL_ERR; r->err_code = OC_ERR_INTERNAL; return r;
        }
        cid = (uint64_t)sqlite3_last_insert_rowid(db);
        add_membership(db, cid, self);
        if (!self_dm) add_membership(db, cid, other);
        sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
    }

    r->type = OC_RES_CHANNEL_INFO;
    load_channel_info(db, cid, self, r);
    r->ch_peer = other;                      /* the DM's other participant (self for a self-DM) */
    r->push_user_id = self_dm ? 0 : other;   /* no peer to push a self-DM to */
    return r;
}

/* Toggle an emoji reaction on a message (REQ-070). A user may react to any
 * message they can read; the (message,user,emoji) PK makes a repeat add a no-op
 * (no stacking) and remove a delete. Returns the new aggregate count for the
 * emoji plus the member set for the fan-out. */
static oc_dbres *process_react(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->channel_id = j->channel_id;
    r->message_id = j->message_id;

    size_t elen = j->emoji ? strlen(j->emoji) : 0;
    if (elen == 0 || elen > OC_MAX_EMOJI) {
        r->type = OC_RES_REACTION_ERR; r->err_code = OC_ERR_INVALID_REACTION; return r;
    }
    uint64_t author = 0; int deleted = 0;
    if (!message_lookup(db, j->channel_id, j->message_id, &author, &deleted) || deleted) {
        r->type = OC_RES_REACTION_ERR; r->err_code = OC_ERR_UNKNOWN_MESSAGE; return r;
    }
    if (!channel_read_access(db, j->channel_id, j->user_id)) {
        r->type = OC_RES_REACTION_ERR; r->err_code = OC_ERR_NOT_A_MEMBER; return r;
    }

    sqlite3_stmt *st = NULL;
    if (j->react_op == OC_REACT_ADD) {
        sqlite3_prepare_v2(db,
            "INSERT OR IGNORE INTO reactions(message_id,user_id,emoji,created_at_ms) "
            "VALUES(?,?,?,?);", -1, &st, NULL);
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->message_id);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)j->user_id);
        sqlite3_bind_text(st, 3, j->emoji, (int)elen, SQLITE_STATIC);
        sqlite3_bind_int64(st, 4, (sqlite3_int64)dbw_now_ms());
    } else {
        sqlite3_prepare_v2(db,
            "DELETE FROM reactions WHERE message_id=? AND user_id=? AND emoji=?;", -1, &st, NULL);
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->message_id);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)j->user_id);
        sqlite3_bind_text(st, 3, j->emoji, (int)elen, SQLITE_STATIC);
    }
    sqlite3_step(st);
    sqlite3_finalize(st);

    uint64_t count = 0;
    sqlite3_prepare_v2(db,
        "SELECT COUNT(*) FROM reactions WHERE message_id=? AND emoji=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->message_id);
    sqlite3_bind_text(st, 2, j->emoji, (int)elen, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) count = (uint64_t)sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);

    r->type = OC_RES_REACTION_OK;
    r->user_id = j->user_id;
    r->emoji = strdup(j->emoji);
    r->react_op = j->react_op;
    r->react_count = count;
    load_members(db, j->channel_id, r);
    return r;
}

/* Every reaction on a message: (emoji, user_id) rows for inspection (REQ-071). */
static oc_dbres *process_list_reactions(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->message_id = j->message_id;

    uint64_t author = 0; int deleted = 0;
    if (!message_lookup(db, j->channel_id, j->message_id, &author, &deleted)) {
        r->type = OC_RES_REACTION_ERR; r->err_code = OC_ERR_UNKNOWN_MESSAGE; return r;
    }
    if (!channel_read_access(db, j->channel_id, j->user_id)) {
        r->type = OC_RES_REACTION_ERR; r->err_code = OC_ERR_NOT_A_MEMBER; return r;
    }
    r->type = OC_RES_REACTIONS;

    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "SELECT emoji, user_id FROM reactions WHERE message_id=? ORDER BY emoji, user_id;",
        -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->message_id);
    size_t cap = 8, n = 0;
    oc_reaction_row *arr = malloc(cap * sizeof *arr);
    while (arr && sqlite3_step(st) == SQLITE_ROW) {
        if (n == cap) { cap *= 2; oc_reaction_row *g = realloc(arr, cap * sizeof *arr); if (!g) break; arr = g; }
        arr[n].emoji   = strdup((const char *)sqlite3_column_text(st, 0));
        arr[n].user_id = (uint64_t)sqlite3_column_int64(st, 1);
        n++;
    }
    sqlite3_finalize(st);
    r->rlist = arr;
    r->n_rlist = n;
    return r;
}

/* Count a thread's replies and the time of the latest one. */
static void thread_stats(sqlite3 *db, uint64_t root, uint32_t *count, uint64_t *last) {
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "SELECT COUNT(*), COALESCE(MAX(created_at_ms),0) FROM messages WHERE parent_id=?;",
        -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)root);
    if (sqlite3_step(st) == SQLITE_ROW) {
        if (count) *count = (uint32_t)sqlite3_column_int64(st, 0);
        if (last)  *last  = (uint64_t)sqlite3_column_int64(st, 1);
    }
    sqlite3_finalize(st);
}

/* Post a threaded reply (REQ-060). The reply threads under a top-level root
 * (replying to a reply flattens to that reply's root) and is NOT delivered to
 * the channel's main scroll — the net thread fans it out as a THREAD_REPLY.
 * Idempotent on (channel, token) exactly like SEND. */
static oc_dbres *process_send_reply(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->channel_id = j->channel_id;
    r->author_id = j->user_id;
    memcpy(r->idem, j->idem, OC_IDEM_LEN);

    /* Idempotent replay: a known (channel, token) re-acks the original id. */
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "SELECT s.message_id, m.created_at_ms, m.parent_id FROM sent_messages s "
        "JOIN messages m ON m.id = s.message_id "
        "WHERE s.channel_id=? AND s.idempotency_token=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->channel_id);
    sqlite3_bind_blob(st, 2, j->idem, OC_IDEM_LEN, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) {
        r->type = OC_RES_REPLY_OK;
        r->message_id = (uint64_t)sqlite3_column_int64(st, 0);
        r->server_time = (uint64_t)sqlite3_column_int64(st, 1);
        r->parent_id = (uint64_t)sqlite3_column_int64(st, 2);
        r->duplicate = 1;
        sqlite3_finalize(st);
        return r;
    }
    sqlite3_finalize(st);

    /* The parent must exist in this channel and not be tombstoned. */
    uint64_t pauthor = 0; int pdel = 0;
    if (!message_lookup(db, j->channel_id, j->parent_id, &pauthor, &pdel) || pdel) {
        r->type = OC_RES_REPLY_ERR; r->err_code = OC_ERR_UNKNOWN_MESSAGE; return r;
    }
    /* Flatten: if the parent is itself a reply, thread under its root. */
    uint64_t root = j->parent_id;
    sqlite3_prepare_v2(db, "SELECT parent_id FROM messages WHERE id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->parent_id);
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) != SQLITE_NULL)
        root = (uint64_t)sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);

    int acc = channel_post_access(db, j->channel_id, j->user_id);
    if (acc == CH_UNKNOWN) { r->type = OC_RES_REPLY_ERR; r->err_code = OC_ERR_UNKNOWN_CHANNEL; return r; }
    if (acc == CH_DENIED)  { r->type = OC_RES_REPLY_ERR; r->err_code = OC_ERR_NOT_A_MEMBER;   return r; }

    uint64_t ts = dbw_now_ms();
    sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);
    sqlite3_prepare_v2(db,
        "INSERT INTO messages(channel_id, author_id, body, created_at_ms, parent_id) "
        "VALUES(?, ?, ?, ?, ?);", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->channel_id);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)j->user_id);
    sqlite3_bind_blob(st, 3, j->body, (int)j->body_len, SQLITE_STATIC);
    sqlite3_bind_int64(st, 4, (sqlite3_int64)ts);
    sqlite3_bind_int64(st, 5, (sqlite3_int64)root);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        r->type = OC_RES_REPLY_ERR; r->err_code = OC_ERR_INTERNAL; return r;
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

    /* Link any referenced attachments to this reply, like SEND (REQ-140). */
    if (j->n_attach) {
        link_attachments(db, mid, j->channel_id, j->user_id, j->attach_ids, j->n_attach);
        load_message_attachments(db, mid, r->attach, &r->n_attach);
    }

    sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);

    uint32_t count = 0;
    thread_stats(db, root, &count, NULL);
    r->type = OC_RES_REPLY_OK;
    r->message_id = mid;
    r->server_time = ts;
    r->parent_id = root;
    r->reply_count = count;
    if (j->body_len) { r->body = malloc(j->body_len); if (r->body) { memcpy(r->body, j->body, j->body_len); r->body_len = j->body_len; } }
    load_members(db, j->channel_id, r);
    return r;
}

/* Return a thread's replies (REQ-060), oldest first. Errors are carried on the
 * OC_RES_THREAD result via err_code (net thread emits an ERROR then). */
static oc_dbres *process_list_thread(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->type = OC_RES_THREAD;
    r->parent_id = j->parent_id;

    uint64_t pa = 0; int pd = 0;
    if (!message_lookup(db, j->channel_id, j->parent_id, &pa, &pd)) {
        r->err_code = OC_ERR_UNKNOWN_MESSAGE; return r;
    }
    if (!channel_read_access(db, j->channel_id, j->user_id)) {
        r->err_code = OC_ERR_NOT_A_MEMBER; return r;
    }

    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "SELECT id, author_id, created_at_ms, body FROM messages "
        "WHERE parent_id=? ORDER BY id LIMIT ?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->parent_id);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)OC_BACKFILL_MAX);
    size_t cap = 8, n = 0;
    oc_replay_msg *arr = malloc(cap * sizeof *arr);
    while (arr && sqlite3_step(st) == SQLITE_ROW) {
        if (n == cap) { cap *= 2; oc_replay_msg *g = realloc(arr, cap * sizeof *arr); if (!g) break; arr = g; }
        oc_replay_msg *m = &arr[n];
        memset(m, 0, sizeof *m);
        m->message_id  = (uint64_t)sqlite3_column_int64(st, 0);
        m->channel_id  = j->channel_id;
        m->author_id   = (uint64_t)sqlite3_column_int64(st, 1);
        m->server_time = (uint64_t)sqlite3_column_int64(st, 2);
        const void *b = sqlite3_column_blob(st, 3);
        int blen = sqlite3_column_bytes(st, 3);
        if (b && blen > 0) { m->body = malloc((size_t)blen); if (m->body) { memcpy(m->body, b, (size_t)blen); m->body_len = (size_t)blen; } }
        load_message_attachments(db, m->message_id, m->attach, &m->n_attach);  /* REQ-140 */
        n++;
    }
    sqlite3_finalize(st);
    r->thread = arr;
    r->n_thread = n;
    r->truncated = (n >= OC_BACKFILL_MAX);
    return r;
}

/* Max search results per response (REQ-080 has no history cutoff; this bounds a
 * single response, and a client can refine the query for more). */
#define OC_SEARCH_MAX 50

/* Turn arbitrary user input into a safe FTS5 MATCH string: each whitespace-
 * separated token becomes a double-quoted phrase (internal quotes doubled per
 * FTS5), so query punctuation can never trip the FTS5 grammar. Multiple tokens
 * are implicitly ANDed. Returns the token count (0 = nothing to search). */
static int build_fts_query(const char *in, size_t inlen, char *out, size_t outcap) {
    size_t o = 0; int tokens = 0; size_t i = 0;
    while (i < inlen) {
        while (i < inlen && (in[i] == ' ' || in[i] == '\t' || in[i] == '\n' || in[i] == '\r')) i++;
        if (i >= inlen) break;
        /* One token spans until the next whitespace. */
        if (o + 2 >= outcap) break;
        if (tokens > 0) { if (o + 1 >= outcap) break; out[o++] = ' '; }
        out[o++] = '"';
        while (i < inlen && in[i] != ' ' && in[i] != '\t' && in[i] != '\n' && in[i] != '\r') {
            if (in[i] == '"') { if (o + 2 >= outcap) break; out[o++] = '"'; out[o++] = '"'; }
            else              { if (o + 1 >= outcap) break; out[o++] = in[i]; }
            i++;
        }
        if (o + 1 >= outcap) break;
        out[o++] = '"';
        tokens++;
    }
    out[o < outcap ? o : outcap - 1] = '\0';
    return tokens;
}

/* Full-text search over message bodies (REQ-080). Scoped to the channels the
 * user can read (public or a member, REQ-031); tombstones are excluded. Results
 * are FTS5 snippets, newest first, bounded by OC_SEARCH_MAX. */
static oc_dbres *process_search(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->type = OC_RES_SEARCH;

    char fts[1024];
    if (!j->body || j->body_len == 0) return r;   /* empty query -> no results */
    if (build_fts_query((const char *)j->body, j->body_len, fts, sizeof fts) == 0) return r;

    uint16_t lim = j->search_limit;
    if (lim == 0 || lim > OC_SEARCH_MAX) lim = OC_SEARCH_MAX;

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
        "SELECT m.id, m.channel_id, m.author_id, m.created_at_ms, "
        "  snippet(messages_fts, 0, '', '', ' ... ', 12) "
        "FROM messages_fts "
        "JOIN messages m ON m.id = messages_fts.rowid "
        "JOIN channels c ON c.id = m.channel_id "
        "WHERE messages_fts MATCH ?1 AND m.deleted_at_ms IS NULL "
        "  AND (c.is_public=1 OR EXISTS(SELECT 1 FROM channel_members cm "
        "       WHERE cm.channel_id=m.channel_id AND cm.user_id=?2)) "
        "ORDER BY m.id DESC LIMIT ?3;", -1, &st, NULL) != SQLITE_OK) {
        return r;   /* malformed FTS query -> empty results, never a fatal error */
    }
    sqlite3_bind_text(st, 1, fts, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)j->user_id);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)lim);

    size_t cap = 8, n = 0;
    oc_replay_msg *arr = malloc(cap * sizeof *arr);
    while (arr && sqlite3_step(st) == SQLITE_ROW) {
        if (n == cap) { cap *= 2; oc_replay_msg *g = realloc(arr, cap * sizeof *arr); if (!g) break; arr = g; }
        oc_replay_msg *m = &arr[n];
        memset(m, 0, sizeof *m);
        m->message_id  = (uint64_t)sqlite3_column_int64(st, 0);
        m->channel_id  = (uint64_t)sqlite3_column_int64(st, 1);
        m->author_id   = (uint64_t)sqlite3_column_int64(st, 2);
        m->server_time = (uint64_t)sqlite3_column_int64(st, 3);
        const unsigned char *snip = sqlite3_column_text(st, 4);
        int slen = sqlite3_column_bytes(st, 4);
        if (snip && slen > 0) { m->body = malloc((size_t)slen); if (m->body) { memcpy(m->body, snip, (size_t)slen); m->body_len = (size_t)slen; } }
        n++;
    }
    sqlite3_finalize(st);
    r->search = arr;
    r->n_search = n;
    r->truncated = (n >= lim);   /* filled the requested limit -> maybe more */
    return r;
}

/* Record a client's cumulative delivery cursor (REQ-090). CLIENT_ACK(N) means
 * everything <= N in the channel has been received; the stored cursor only ever
 * advances. When it does advance, answer with a READ_CURSOR result so the net
 * thread can drive seen-by: fan the acker's new cursor to the channel members and
 * backfill the acker with the other members' current cursors. A stale/duplicate
 * ack (no advance) has no reply. */
static oc_dbres *process_client_ack(sqlite3 *db, const oc_job *j) {
    /* Was this a real advance? Only broadcast if so (avoids fan-out on replays). */
    sqlite3_stmt *st = NULL;
    uint64_t prev = 0;
    sqlite3_prepare_v2(db,
        "SELECT message_id FROM delivery_cursors WHERE user_id=? AND channel_id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->user_id);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)j->channel_id);
    if (sqlite3_step(st) == SQLITE_ROW) prev = (uint64_t)sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);

    sqlite3_prepare_v2(db,
        "INSERT INTO delivery_cursors(user_id,channel_id,message_id,updated_at_ms) "
        "VALUES(?,?,?,?) ON CONFLICT(user_id,channel_id) DO UPDATE SET "
        "message_id=MAX(message_id,excluded.message_id), updated_at_ms=excluded.updated_at_ms;",
        -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->user_id);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)j->channel_id);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)j->message_id);
    sqlite3_bind_int64(st, 4, (sqlite3_int64)dbw_now_ms());
    sqlite3_step(st);
    sqlite3_finalize(st);

    if (j->message_id <= prev) return NULL;   /* no advance: nothing to broadcast */

    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->type = OC_RES_READ_CURSOR;
    r->conn_id = j->conn_id;
    r->channel_id = j->channel_id;
    r->user_id = j->user_id;
    r->message_id = j->message_id;
    load_members(db, j->channel_id, r);   /* who to fan the acker's cursor to */

    /* The other members' current cursors, to bootstrap the acker's seen-by view. */
    size_t cap = 8, n = 0;
    r->rcur = malloc(cap * sizeof *r->rcur);
    sqlite3_prepare_v2(db,
        "SELECT user_id, message_id FROM delivery_cursors WHERE channel_id=? AND user_id<>?;",
        -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->channel_id);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)j->user_id);
    while (r->rcur && sqlite3_step(st) == SQLITE_ROW) {
        if (n == cap) { cap *= 2; oc_read_cursor_row *g = realloc(r->rcur, cap * sizeof *g); if (!g) break; r->rcur = g; }
        r->rcur[n].user_id    = (uint64_t)sqlite3_column_int64(st, 0);
        r->rcur[n].message_id = (uint64_t)sqlite3_column_int64(st, 1);
        n++;
    }
    sqlite3_finalize(st);
    r->n_rcur = n;
    return r;
}

/* Replay messages newer than each cursor, for channels the user belongs to,
 * bounded by OC_BACKFILL_MAX total (REQ-101, PROTOCOL.md §6). A request with no
 * cursors (count=0) resumes every member channel from the server's stored
 * delivery cursor (REQ-090) — 0 for a fresh client, else its last ack. */
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

    /* Effective cursors: the client's explicit list, or — when it sent none —
     * every member channel resumed from the stored delivery cursor (REQ-090):
     * 0 for a fresh client, else its last ack. */
    const oc_bf_cursor *curs = j->cursors;
    size_t ncurs = j->n_cursors;
    oc_bf_cursor *derived = NULL;
    if (ncurs == 0) {
        /* Every member channel, each with a cursor of 0 — "I hold no history".
         * The per-channel loop below turns that into the channel's newest page.
         *
         * These must NOT be seeded from delivery_cursors. A cursorless request
         * comes from a client that has nothing (ARCH-88), and resuming a
         * caught-up user from where they last read sends them nothing at all. */
        sqlite3_prepare_v2(db,
            "SELECT cm.channel_id, 0 FROM channel_members cm "
            "WHERE cm.user_id=? ORDER BY cm.channel_id;", -1, &st, NULL);
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->user_id);
        size_t dcap = 8, dn = 0;
        derived = malloc(dcap * sizeof *derived);
        while (derived && sqlite3_step(st) == SQLITE_ROW) {
            if (dn == dcap) { dcap *= 2; oc_bf_cursor *g = realloc(derived, dcap * sizeof *derived); if (!g) break; derived = g; }
            derived[dn].channel_id = (uint64_t)sqlite3_column_int64(st, 0);
            derived[dn].after_message_id = (uint64_t)sqlite3_column_int64(st, 1);
            dn++;
        }
        sqlite3_finalize(st);
        curs = derived; ncurs = dn;
    }

    for (size_t ci = 0; ci < ncurs && n < OC_BACKFILL_MAX; ci++) {
        uint64_t ch = curs[ci].channel_id;
        if (!channel_read_access(db, ch, j->user_id)) continue;

        /* A cursor of 0 means "I hold no history" — a client that keeps no local
         * state (ARCH-88). Such a client wants the channel's NEWEST page, so
         * replay the tail: the last OC_BACKFILL_TAIL top-level messages.
         *
         * It must NOT resume from the user's stored read cursor. That cursor is
         * where they last read TO, so a caught-up user would be sent nothing at
         * all and would stare at an empty channel on every launch — which is
         * exactly what a cacheless client did before this. The read cursor still
         * matters, but for placing the unread divider (REQ-236), not for choosing
         * which messages exist. A user who is far behind likewise gets the newest
         * page rather than the oldest unread one; older history is paged in by
         * scrolling back.
         *
         * A non-zero cursor keeps its literal meaning — "strictly after this" —
         * because a reconnecting client that still holds history wants only what
         * it missed, with no duplicate replay. */
        uint64_t after = curs[ci].after_message_id;
        if (after == 0) {
            sqlite3_stmt *cst = NULL;
            if (sqlite3_prepare_v2(db,
                    "SELECT COALESCE(MIN(id),0) FROM "
                    "  (SELECT id FROM messages WHERE channel_id=? AND parent_id IS NULL "
                    "   ORDER BY id DESC LIMIT ?);", -1, &cst, NULL) == SQLITE_OK) {
                sqlite3_bind_int64(cst, 1, (sqlite3_int64)ch);
                sqlite3_bind_int64(cst, 2, (sqlite3_int64)OC_BACKFILL_TAIL);
                if (sqlite3_step(cst) == SQLITE_ROW) {
                    uint64_t first = (uint64_t)sqlite3_column_int64(cst, 0);
                    if (first > 0) after = first - 1;   /* 0 stays 0: empty channel */
                }
                sqlite3_finalize(cst);
            }
        }

        /* Only top-level messages are replayed to the main scroll (REQ-060);
         * thread replies (parent_id set) are fetched per-thread via LIST_THREAD.
         * Each row also carries its thread reply count + latest-reply time so the
         * net thread can emit a THREAD_META for parents that have replies. */
        sqlite3_prepare_v2(db,
            "SELECT m.id, m.author_id, m.created_at_ms, m.body, "
            "  (SELECT COUNT(*) FROM messages c WHERE c.parent_id=m.id), "
            "  (SELECT COALESCE(MAX(c.created_at_ms),0) FROM messages c WHERE c.parent_id=m.id), "
            /* The display name to show: a webhook label overrides (REQ-170),
             * else the author's display_name (ARCH-74 client shows names). */
            "  COALESCE(NULLIF(m.author_name,''), u.display_name, '') "
            "FROM messages m LEFT JOIN users u ON u.id = m.author_id "
            "WHERE m.channel_id=? AND m.id>? AND m.parent_id IS NULL "
            "ORDER BY m.id LIMIT ?;", -1, &st, NULL);
        sqlite3_bind_int64(st, 1, (sqlite3_int64)ch);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)after);   /* resolved above */
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
            m->reply_count   = (uint32_t)sqlite3_column_int64(st, 4);
            m->last_reply_at = (uint64_t)sqlite3_column_int64(st, 5);
            const char *an = (const char *)sqlite3_column_text(st, 6);
            if (an && an[0]) m->author_name = strdup(an);   /* webhook display name (REQ-170) */
            /* Re-attach the message's linked attachments so a reconnecting client
             * sees them inline, not just live members (REQ-140). */
            load_message_attachments(db, m->message_id, m->attach, &m->n_attach);
            n++;
        }
        sqlite3_finalize(st);
    }
    free(derived);
    r->replay = arr;
    r->n_replay = n;
    r->truncated = (n >= OC_BACKFILL_MAX);   /* hit the per-response cap */

    /* Reaction aggregates for what we just replayed. One statement per message
     * rather than one big IN(...) because the id list is unbounded and building
     * the SQL text for it would be the only place in this file that does. */
    if (n > 0) {
        size_t rcap = 16, rn = 0;
        struct oc_replay_react *ra = malloc(rcap * sizeof *ra);
        for (size_t i = 0; i < n && ra; i++) {
            sqlite3_stmt *rst = NULL;
            if (sqlite3_prepare_v2(db,
                    "SELECT emoji, COUNT(*), "
                    /* Prefer the requesting user's own id so the client can mark
                     * the chip as theirs; MIN() just picks a stable stand-in. */
                    "  COALESCE(MAX(CASE WHEN user_id=?2 THEN user_id END), MIN(user_id)) "
                    "FROM reactions WHERE message_id=?1 GROUP BY emoji ORDER BY emoji;",
                    -1, &rst, NULL) != SQLITE_OK) break;
            sqlite3_bind_int64(rst, 1, (sqlite3_int64)arr[i].message_id);
            sqlite3_bind_int64(rst, 2, (sqlite3_int64)j->user_id);
            while (sqlite3_step(rst) == SQLITE_ROW) {
                if (rn == rcap) {
                    rcap *= 2;
                    struct oc_replay_react *g = realloc(ra, rcap * sizeof *ra);
                    if (!g) break;
                    ra = g;
                }
                const char *em = (const char *)sqlite3_column_text(rst, 0);
                ra[rn].message_id = arr[i].message_id;
                ra[rn].channel_id = arr[i].channel_id;
                ra[rn].count      = (uint64_t)sqlite3_column_int64(rst, 1);
                ra[rn].user_id    = (uint64_t)sqlite3_column_int64(rst, 2);
                ra[rn].emoji      = strdup(em ? em : "");
                rn++;
            }
            sqlite3_finalize(rst);
        }
        r->rreact = ra;
        r->n_rreact = rn;
    }
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

/* Relay a typing signal (REQ-121): resolve the channel's members so the net
 * thread can fan a TYPING_UPDATE to them. Read-only; no state kept. Members are
 * empty (nothing relayed) if the typer can't read the channel — so typing in a
 * private channel or DM never leaks to non-members. */
static oc_dbres *process_typing(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->type = OC_RES_TYPING;
    r->channel_id = j->channel_id;
    r->author_id = j->user_id;   /* the user who is typing */
    if (channel_read_access(db, j->channel_id, j->user_id))
        load_members(db, j->channel_id, r);
    return r;
}

/* Authorize joining a channel's audio call (REQ-150): the ordinary channel-read
 * gate. The net thread owns the ephemeral call roster; this is just the access
 * check. Read (query connection). */
static oc_dbres *process_call_auth(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->channel_id = j->channel_id;
    r->user_id = j->user_id;
    if (channel_read_access(db, j->channel_id, j->user_id)) {
        r->type = OC_RES_CALL_AUTH;
    } else {
        r->type = OC_RES_CALL_ERR; r->err_code = OC_ERR_NOT_A_MEMBER;
    }
    return r;
}

/* Attachments (REQ-140/141, ARCH-69/70). CREATE mints a pending row (message_id
 * NULL, sha256 NULL) for an upload targeting a channel the caller may post to,
 * and hands back the row id + an opaque storage key so the net thread can open
 * the blob. The blob bytes never touch this thread — only the pointer. Write. */
static oc_dbres *process_attach_create(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->channel_id = j->channel_id;

    if (j->att_size > OC_MAX_ATTACHMENT_SIZE) {
        r->type = OC_RES_ATTACH_ERR; r->err_code = OC_ERR_ATTACHMENT_TOO_LARGE;
        return r;
    }
    int acc = channel_post_access(db, j->channel_id, j->user_id);
    if (acc == CH_UNKNOWN) { r->type = OC_RES_ATTACH_ERR; r->err_code = OC_ERR_UNKNOWN_CHANNEL; return r; }
    if (acc == CH_DENIED)  { r->type = OC_RES_ATTACH_ERR; r->err_code = OC_ERR_NOT_A_MEMBER;   return r; }

    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "INSERT INTO attachments(channel_id, message_id, uploader_id, storage_key, "
        "  filename, mime, size, sha256, created_at_ms) "
        "VALUES(?, NULL, ?, '', ?, ?, ?, NULL, ?);", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->channel_id);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)j->user_id);
    sqlite3_bind_text (st, 3, j->filename ? j->filename : "", -1, SQLITE_STATIC);
    sqlite3_bind_text (st, 4, j->mime ? j->mime : "", -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 5, (sqlite3_int64)j->att_size);
    sqlite3_bind_int64(st, 6, (sqlite3_int64)dbw_now_ms());
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) { r->type = OC_RES_ATTACH_ERR; r->err_code = OC_ERR_INTERNAL; return r; }

    uint64_t aid = (uint64_t)sqlite3_last_insert_rowid(db);
    /* The storage key is the row id in hex — unique, opaque, never on the wire. */
    char key[32];
    snprintf(key, sizeof key, "%016llx", (unsigned long long)aid);
    sqlite3_prepare_v2(db, "UPDATE attachments SET storage_key=? WHERE id=?;", -1, &st, NULL);
    sqlite3_bind_text (st, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)aid);
    sqlite3_step(st);
    sqlite3_finalize(st);

    r->type = OC_RES_ATTACH_CREATED;
    r->attachment_id = aid;
    r->storage_key = strdup(key);
    r->att_size = j->att_size;
    return r;
}

/* FINALIZE records the streamed byte count + digest on UPLOAD_END, verifying the
 * received size matches what UPLOAD_BEGIN declared. A non-NULL sha256 marks the
 * blob complete and thus downloadable; a size mismatch is refused (the net
 * thread discards the partial blob). Only the uploader may finalize, and only a
 * still-pending (unfinalized) row. Write. */
static oc_dbres *process_attach_finalize(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->attachment_id = j->attachment_id;

    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "SELECT size, uploader_id, sha256 FROM attachments WHERE id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->attachment_id);
    int found = sqlite3_step(st) == SQLITE_ROW;
    uint64_t declared = found ? (uint64_t)sqlite3_column_int64(st, 0) : 0;
    uint64_t owner    = found ? (uint64_t)sqlite3_column_int64(st, 1) : 0;
    int already       = found && sqlite3_column_type(st, 2) != SQLITE_NULL;
    sqlite3_finalize(st);

    if (!found || owner != j->user_id || already) {
        r->type = OC_RES_ATTACH_ERR; r->err_code = OC_ERR_UNKNOWN_ATTACHMENT; return r;
    }
    if (j->att_size != declared) {
        r->type = OC_RES_ATTACH_ERR; r->err_code = OC_ERR_TRANSFER_PROTOCOL; return r;
    }

    sqlite3_prepare_v2(db, "UPDATE attachments SET sha256=? WHERE id=?;", -1, &st, NULL);
    sqlite3_bind_blob (st, 1, j->att_sha256, 32, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)j->attachment_id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) { r->type = OC_RES_ATTACH_ERR; r->err_code = OC_ERR_INTERNAL; return r; }

    r->type = OC_RES_ATTACH_OK;
    r->att_size = declared;
    return r;
}

/* LOOKUP authorizes a download and returns the pointer + metadata. Access is the
 * ordinary channel read check on the attachment's channel (REQ-141) — the same
 * gate as reading a message — so proxying needs no signed URL. Only a finalized
 * blob (sha256 present) is served. Read (query connection). */
static oc_dbres *process_attach_lookup(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->attachment_id = j->attachment_id;

    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "SELECT channel_id, storage_key, filename, mime, size, sha256, reclaimed_at_ms "
        "FROM attachments WHERE id=? AND sha256 IS NOT NULL;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->attachment_id);
    if (sqlite3_step(st) != SQLITE_ROW) {
        sqlite3_finalize(st);
        r->type = OC_RES_ATTACH_ERR; r->err_code = OC_ERR_UNKNOWN_ATTACHMENT;
        return r;
    }
    /* Tombstoned (REQ-215/217): the row survives so the message stays readable,
     * but the bytes are gone. Report that distinctly — "no longer available" is
     * a very different thing for a user to see than "no such attachment". */
    if (sqlite3_column_int64(st, 6) != 0) {
        sqlite3_finalize(st);
        r->type = OC_RES_ATTACH_ERR; r->err_code = OC_ERR_ATTACHMENT_GONE;
        return r;
    }
    uint64_t cid = (uint64_t)sqlite3_column_int64(st, 0);
    const char *key = (const char *)sqlite3_column_text(st, 1);
    const char *fn  = (const char *)sqlite3_column_text(st, 2);
    const char *mm  = (const char *)sqlite3_column_text(st, 3);
    uint64_t sz     = (uint64_t)sqlite3_column_int64(st, 4);
    const void *dg  = sqlite3_column_blob(st, 5);
    int dglen       = sqlite3_column_bytes(st, 5);

    if (!channel_read_access(db, cid, j->user_id)) {
        sqlite3_finalize(st);
        r->type = OC_RES_ATTACH_ERR; r->err_code = OC_ERR_FORBIDDEN;
        return r;
    }
    r->type = OC_RES_ATTACH_META;
    r->channel_id = cid;
    r->storage_key = strdup(key ? key : "");
    r->filename = strdup(fn ? fn : "");
    r->mime = strdup(mm ? mm : "");
    r->att_size = sz;
    if (dg && dglen == 32) memcpy(r->att_sha256, dg, 32);
    sqlite3_finalize(st);
    return r;
}

/* Incoming webhooks (REQ-170, ARCH-32). CREATE_WEBHOOK mints a per-channel token
 * for a client that can post to the channel; the raw 32-byte token is returned
 * once (only its SHA-256 is stored, like a session). Write. */
static oc_dbres *process_create_webhook(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->channel_id = j->channel_id;

    int acc = channel_post_access(db, j->channel_id, j->user_id);
    if (acc == CH_UNKNOWN) { r->type = OC_RES_WEBHOOK_ERR; r->err_code = OC_ERR_UNKNOWN_CHANNEL; return r; }
    if (acc == CH_DENIED)  { r->type = OC_RES_WEBHOOK_ERR; r->err_code = OC_ERR_NOT_A_MEMBER;   return r; }

    uint8_t token[OC_SESSION_TOKEN_LEN], hash[OC_SHA256_LEN];
    if (oc_rand_bytes(token, sizeof token) != 0 || oc_sha256(token, sizeof token, hash) != 0) {
        r->type = OC_RES_WEBHOOK_ERR; r->err_code = OC_ERR_INTERNAL; return r;
    }
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "INSERT INTO webhooks(channel_id, creator_id, token_hash, label, created_at_ms) "
        "VALUES(?, ?, ?, ?, ?);", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->channel_id);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)j->user_id);
    sqlite3_bind_blob (st, 3, hash, sizeof hash, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, 4, j->ch_name ? j->ch_name : "", -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 5, (sqlite3_int64)dbw_now_ms());
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) { r->type = OC_RES_WEBHOOK_ERR; r->err_code = OC_ERR_INTERNAL; return r; }

    r->type = OC_RES_WEBHOOK_CREATED;
    r->message_id = (uint64_t)sqlite3_last_insert_rowid(db);  /* webhook id */
    /* AFTER last_insert_rowid is read: audit_log INSERTs a row of its own, which
     * would otherwise become "the last inserted row" and hand the caller the
     * audit entry's id instead of the webhook's. Any audit call placed near an
     * INSERT whose generated id is still needed has to come after that read. */
    audit_actor(db, OC_AUDIT_ADMIN, "webhook.create", j->user_id,
                r->message_id, NULL, 1, NULL);
    memcpy(r->session_token, token, sizeof token);            /* raw token, shown once */
    return r;
}

/* WEBHOOK_POST resolves a token presented over HTTP and posts the message body
 * as the webhook's creator (REQ-170). Each POST is a fresh message (no
 * idempotency — the sender is an uncontrolled third party). Result carries the
 * SEND-style broadcast fields so the net thread fans it out to members. Write. */
static oc_dbres *process_webhook_post(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;

    uint8_t hash[OC_SHA256_LEN];
    if (j->token_len != OC_SESSION_TOKEN_LEN || oc_sha256(j->token, j->token_len, hash) != 0) {
        r->type = OC_RES_WEBHOOK_ERR; r->err_code = OC_ERR_UNKNOWN_WEBHOOK; return r;
    }
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "SELECT channel_id, creator_id, label FROM webhooks WHERE token_hash=? AND disabled=0;", -1, &st, NULL);
    sqlite3_bind_blob(st, 1, hash, sizeof hash, SQLITE_TRANSIENT);
    int found = sqlite3_step(st) == SQLITE_ROW;
    uint64_t cid = found ? (uint64_t)sqlite3_column_int64(st, 0) : 0;
    uint64_t creator = found ? (uint64_t)sqlite3_column_int64(st, 1) : 0;
    const char *label = found ? (const char *)sqlite3_column_text(st, 2) : NULL;
    char *label_dup = (label && label[0]) ? strdup(label) : NULL;  /* copy before finalize */
    sqlite3_finalize(st);
    if (!found) { free(label_dup); r->type = OC_RES_WEBHOOK_ERR; r->err_code = OC_ERR_UNKNOWN_WEBHOOK; return r; }

    /* Post as the webhook's label (display-name override, REQ-170), falling back
     * to the creator's identity if the webhook was created without a label. */
    uint64_t ts = dbw_now_ms();
    sqlite3_prepare_v2(db,
        "INSERT INTO messages(channel_id, author_id, body, created_at_ms, author_name) VALUES(?, ?, ?, ?, ?);",
        -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)cid);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)creator);
    sqlite3_bind_blob (st, 3, j->body, (int)j->body_len, SQLITE_STATIC);
    sqlite3_bind_int64(st, 4, (sqlite3_int64)ts);
    if (label_dup) sqlite3_bind_text(st, 5, label_dup, -1, SQLITE_STATIC);
    else           sqlite3_bind_null(st, 5);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) { free(label_dup); r->type = OC_RES_WEBHOOK_ERR; r->err_code = OC_ERR_INTERNAL; return r; }

    r->type = OC_RES_WEBHOOK_POSTED;
    r->message_id = (uint64_t)sqlite3_last_insert_rowid(db);
    r->channel_id = cid;
    r->author_id = creator;
    r->author_name = label_dup;   /* transferred; freed by oc_dbres_free */
    r->server_time = ts;
    if (j->body_len) { r->body = malloc(j->body_len); if (r->body) { memcpy(r->body, j->body, j->body_len); r->body_len = j->body_len; } }
    load_members(db, cid, r);
    return r;
}

/* List a channel's webhooks (REQ-170) — ids, labels, disabled flag; never the
 * token. Gated on channel read access (a member/public-channel viewer). Read. */
static oc_dbres *process_list_webhooks(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->channel_id = j->channel_id;
    if (!channel_read_access(db, j->channel_id, j->user_id)) {
        r->type = OC_RES_WEBHOOK_ERR; r->err_code = OC_ERR_NOT_A_MEMBER; return r;
    }
    r->type = OC_RES_WEBHOOK_LIST;
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "SELECT id, channel_id, label, disabled FROM webhooks WHERE channel_id=? ORDER BY id;",
        -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->channel_id);
    size_t cap = 8;
    r->whlist = malloc(cap * sizeof *r->whlist);
    while (r->whlist && sqlite3_step(st) == SQLITE_ROW) {
        if (r->n_whlist == cap) { cap *= 2; oc_webhook_row *g = realloc(r->whlist, cap * sizeof *g); if (!g) break; r->whlist = g; }
        oc_webhook_row *e = &r->whlist[r->n_whlist++];
        e->id = (uint64_t)sqlite3_column_int64(st, 0);
        e->channel_id = (uint64_t)sqlite3_column_int64(st, 1);
        e->label = strdup((const char *)sqlite3_column_text(st, 2));
        e->disabled = (uint8_t)sqlite3_column_int(st, 3);
    }
    sqlite3_finalize(st);
    return r;
}

/* Delete a webhook (REQ-170). The actor must be a member of the webhook's
 * channel (same gate as creating one). Write. */
static oc_dbres *process_delete_webhook(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;

    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "SELECT channel_id FROM webhooks WHERE id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->message_id);   /* webhook id carried in message_id */
    int found = sqlite3_step(st) == SQLITE_ROW;
    uint64_t cid = found ? (uint64_t)sqlite3_column_int64(st, 0) : 0;
    sqlite3_finalize(st);
    if (!found) { r->type = OC_RES_WEBHOOK_ERR; r->err_code = OC_ERR_UNKNOWN_WEBHOOK; return r; }
    if (!is_member(db, cid, j->user_id)) { r->type = OC_RES_WEBHOOK_ERR; r->err_code = OC_ERR_NOT_A_MEMBER; return r; }

    sqlite3_prepare_v2(db, "DELETE FROM webhooks WHERE id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->message_id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) { r->type = OC_RES_WEBHOOK_ERR; r->err_code = OC_ERR_INTERNAL; return r; }
    audit_actor(db, OC_AUDIT_ADMIN, "webhook.delete", j->user_id,
                j->message_id, NULL, 1, NULL);   /* webhook id rides message_id */
    r->type = OC_RES_WEBHOOK_DELETED;
    r->message_id = j->message_id;   /* echo the removed id */
    return r;
}

/* --- Notification preferences (REQ-130/131) ----------------------------- */

/* Load the user's DND window + per-channel levels into `r` as a NOTIFY_PREFS
 * snapshot. Shared by set (returns a fresh snapshot to sync all devices) and
 * list. */
static void build_notify_prefs(sqlite3 *db, uint64_t user_id, oc_dbres *r) {
    r->type = OC_RES_NOTIFY_PREFS;
    r->user_id = user_id;
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "SELECT dnd_enabled, dnd_start_min, dnd_end_min FROM users WHERE id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)user_id);
    if (sqlite3_step(st) == SQLITE_ROW) {
        r->np_dnd_enabled   = (uint8_t)sqlite3_column_int(st, 0);
        r->np_dnd_start_min = (uint16_t)sqlite3_column_int(st, 1);
        r->np_dnd_end_min   = (uint16_t)sqlite3_column_int(st, 2);
    }
    sqlite3_finalize(st);

    sqlite3_prepare_v2(db,
        "SELECT channel_id, level FROM notification_prefs WHERE user_id=? ORDER BY channel_id LIMIT ?;",
        -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)user_id);
    sqlite3_bind_int(st, 2, (int)OC_MAX_NOTIFY_PREFS);
    size_t cap = 8;
    r->nprefs = malloc(cap * sizeof *r->nprefs);
    while (r->nprefs && sqlite3_step(st) == SQLITE_ROW && r->n_nprefs < OC_MAX_NOTIFY_PREFS) {
        if (r->n_nprefs == cap) { cap *= 2; oc_notify_pref_row *g = realloc(r->nprefs, cap * sizeof *g); if (!g) break; r->nprefs = g; }
        r->nprefs[r->n_nprefs].channel_id = (uint64_t)sqlite3_column_int64(st, 0);
        r->nprefs[r->n_nprefs].level = (uint8_t)sqlite3_column_int(st, 1);
        r->n_nprefs++;
    }
    sqlite3_finalize(st);
}

/* Set a channel's notification level for the caller (REQ-130). The channel must
 * be one the caller can read. Returns a fresh snapshot (net thread syncs it to
 * the user's other devices). Write. */
static oc_dbres *process_set_notify_pref(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    if (j->notify_level > OC_NOTIFY_NONE || !channel_read_access(db, j->channel_id, j->user_id)) {
        r->type = OC_RES_NOTIFY_ERR; r->err_code = OC_ERR_NOT_A_MEMBER; r->user_id = j->user_id; return r;
    }
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO notification_prefs(user_id, channel_id, level) VALUES(?, ?, ?);",
        -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->user_id);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)j->channel_id);
    sqlite3_bind_int  (st, 3, (int)j->notify_level);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) { r->type = OC_RES_NOTIFY_ERR; r->err_code = OC_ERR_INTERNAL; r->user_id = j->user_id; return r; }
    build_notify_prefs(db, j->user_id, r);
    return r;
}

/* Set the caller's do-not-disturb window (REQ-131). Write. */
static oc_dbres *process_set_dnd(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "UPDATE users SET dnd_enabled=?, dnd_start_min=?, dnd_end_min=? WHERE id=?;", -1, &st, NULL);
    sqlite3_bind_int  (st, 1, j->dnd_enabled ? 1 : 0);
    sqlite3_bind_int  (st, 2, (int)j->dnd_start_min);
    sqlite3_bind_int  (st, 3, (int)j->dnd_end_min);
    sqlite3_bind_int64(st, 4, (sqlite3_int64)j->user_id);
    sqlite3_step(st);
    sqlite3_finalize(st);
    build_notify_prefs(db, j->user_id, r);
    return r;
}

/* Register a push device token (ARCH-85). Upsert on (user, token): re-registering
 * the same token just refreshes last_seen + platform. Write. */
static oc_dbres *process_register_device_token(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    const char *platform = j->device_platform == OC_PUSH_FCM ? "fcm" : "apns";
    if (j->device_platform > OC_PUSH_FCM || !j->device_token || !*j->device_token) {
        r->type = OC_RES_DEVICE_TOKEN_ERR; r->err_code = OC_ERR_INVALID_DEVICE_TOKEN; return r;
    }
    uint64_t now = dbw_now_ms();
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "INSERT INTO device_tokens(user_id, platform, token, created_at_ms, last_seen_ms) "
        "VALUES(?,?,?,?,?) "
        "ON CONFLICT(user_id, token) DO UPDATE SET platform=excluded.platform, "
        "last_seen_ms=excluded.last_seen_ms;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->user_id);
    sqlite3_bind_text (st, 2, platform, -1, SQLITE_STATIC);
    sqlite3_bind_text (st, 3, j->device_token, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 4, (sqlite3_int64)now);
    sqlite3_bind_int64(st, 5, (sqlite3_int64)now);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    r->type = (rc == SQLITE_DONE) ? OC_RES_DEVICE_TOKEN_OK : OC_RES_DEVICE_TOKEN_ERR;
    if (rc != SQLITE_DONE) r->err_code = OC_ERR_INTERNAL;
    return r;
}

/* Drop the caller's registration of a token (logout / token rotation). Write. */
static oc_dbres *process_unregister_device_token(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    if (!j->device_token || !*j->device_token) {
        r->type = OC_RES_DEVICE_TOKEN_ERR; r->err_code = OC_ERR_INVALID_DEVICE_TOKEN; return r;
    }
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "DELETE FROM device_tokens WHERE user_id=? AND token=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->user_id);
    sqlite3_bind_text (st, 2, j->device_token, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
    r->type = OC_RES_DEVICE_TOKEN_OK;
    return r;
}

/* Prune a token central reported stale — across all users (a device token is
 * globally unique to a device). Fire-and-forget: no result. Write. */
static oc_dbres *process_prune_device_token(sqlite3 *db, const oc_job *j) {
    if (j->device_token && *j->device_token) {
        sqlite3_stmt *st = NULL;
        sqlite3_prepare_v2(db, "DELETE FROM device_tokens WHERE token=?;", -1, &st, NULL);
        sqlite3_bind_text(st, 1, j->device_token, -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    return NULL;   /* push_result ignores NULL */
}

/* The caller's full notification settings (REQ-130/131). Read. */
static oc_dbres *process_list_notify_prefs(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    build_notify_prefs(db, j->user_id, r);
    return r;
}

/* Load a (user, client_type) settings bucket into `r` as a CLIENT_SETTINGS
 * snapshot. Shared by set (returns a fresh snapshot to sync the user's other
 * devices of the same client_type) and list. */
static void build_client_settings(sqlite3 *db, uint64_t user_id, const char *client_type, oc_dbres *r) {
    r->type = OC_RES_CLIENT_SETTINGS;
    r->user_id = user_id;
    r->cs_client_type = client_type ? strdup(client_type) : strdup("");
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "SELECT key, value FROM client_settings WHERE user_id=? AND client_type=? "
        "ORDER BY key LIMIT ?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)user_id);
    sqlite3_bind_text (st, 2, client_type ? client_type : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int  (st, 3, (int)OC_MAX_CLIENT_SETTINGS);
    size_t cap = 8;
    r->cslist = malloc(cap * sizeof *r->cslist);
    while (r->cslist && sqlite3_step(st) == SQLITE_ROW && r->n_cslist < OC_MAX_CLIENT_SETTINGS) {
        if (r->n_cslist == cap) {
            cap *= 2;
            oc_client_setting_row *g = realloc(r->cslist, cap * sizeof *g);
            if (!g) break;
            r->cslist = g;
        }
        const char *k = (const char *)sqlite3_column_text(st, 0);
        const char *v = (const char *)sqlite3_column_text(st, 1);
        r->cslist[r->n_cslist].key = strdup(k ? k : "");
        r->cslist[r->n_cslist].value = strdup(v ? v : "");
        r->n_cslist++;
    }
    sqlite3_finalize(st);
}

/* Upsert (or, with an empty value, delete) one synced client setting. Returns a
 * fresh bucket snapshot (net thread syncs it to the user's same-type devices).
 * Write. */
static oc_dbres *process_set_client_setting(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    const char *ct = j->cs_client_type ? j->cs_client_type : "";
    const char *key = j->cs_key ? j->cs_key : "";
    const char *val = j->cs_value ? j->cs_value : "";
    sqlite3_stmt *st = NULL;
    if (val[0] == '\0') {
        sqlite3_prepare_v2(db,
            "DELETE FROM client_settings WHERE user_id=? AND client_type=? AND key=?;",
            -1, &st, NULL);
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->user_id);
        sqlite3_bind_text (st, 2, ct, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text (st, 3, key, -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_prepare_v2(db,
            "INSERT INTO client_settings(user_id, client_type, key, value, updated_ms) "
            "VALUES(?, ?, ?, ?, ?) ON CONFLICT(user_id, client_type, key) "
            "DO UPDATE SET value=excluded.value, updated_ms=excluded.updated_ms;",
            -1, &st, NULL);
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->user_id);
        sqlite3_bind_text (st, 2, ct, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text (st, 3, key, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text (st, 4, val, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 5, (sqlite3_int64)dbw_now_ms());
    }
    sqlite3_step(st);
    sqlite3_finalize(st);
    build_client_settings(db, j->user_id, ct, r);
    return r;
}

/* The caller's full settings bucket for a client_type. Read. */
static oc_dbres *process_list_client_settings(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    build_client_settings(db, j->user_id, j->cs_client_type ? j->cs_client_type : "", r);
    return r;
}

/* --- Self-service profile (REQ-020) ------------------------------------- */

/* Ok result: PROFILE_UPDATED carrying `name` (ownership taken) for user_id. */
static oc_dbres *profile_ok(const oc_job *j, char *name) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) { free(name); return NULL; }
    r->type = OC_RES_PROFILE_UPDATED;
    r->conn_id = j->conn_id;
    r->user_id = j->user_id;
    r->profile_name = name ? name : strdup("");
    return r;
}

static oc_dbres *profile_err(const oc_job *j, uint16_t code) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->type = OC_RES_PROFILE_ERR;
    r->conn_id = j->conn_id;
    r->err_code = code;
    r->user_id = j->user_id;
    return r;
}

/* Rename yourself. The new name folds into every client's roster (net thread
 * fans PROFILE_UPDATED tenant-wide). Write. */
static oc_dbres *process_set_display_name(sqlite3 *db, const oc_job *j) {
    const char *name = j->pf_name ? j->pf_name : "";
    size_t nlen = strlen(name);
    if (nlen == 0 || nlen > OC_MAX_DISPLAY_NAME) return profile_err(j, OC_ERR_FORBIDDEN);
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "UPDATE users SET display_name=? WHERE id=?;", -1, &st, NULL);
    sqlite3_bind_text (st, 1, name, (int)nlen, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)j->user_id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) return profile_err(j, OC_ERR_INTERNAL);
    return profile_ok(j, strdup(name));
}

/* Rotate your local password: verify the old one (constant-time), then store a
 * fresh PBKDF2 salt+hash. A non-local (OIDC) account, or a wrong old password,
 * is FORBIDDEN. On success the self ack echoes the unchanged display name. Write. */
static oc_dbres *process_change_password(sqlite3 *db, const oc_job *j) {
    const char *oldpw = j->pf_old_pw ? j->pf_old_pw : "";
    const char *newpw = j->pf_new_pw ? j->pf_new_pw : "";
    if (newpw[0] == '\0') return profile_err(j, OC_ERR_FORBIDDEN);

    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "SELECT salt, iterations, hash FROM local_credentials WHERE user_id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->user_id);
    int ok = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const void *salt = sqlite3_column_blob(st, 0);
        int slen = sqlite3_column_bytes(st, 0);
        uint32_t iters = (uint32_t)sqlite3_column_int64(st, 1);
        const void *stored = sqlite3_column_blob(st, 2);
        int hlen = sqlite3_column_bytes(st, 2);
        uint8_t derived[OC_PW_HASH_LEN];
        if (salt && stored && hlen == (int)OC_PW_HASH_LEN &&
            oc_pw_derive(oldpw, strlen(oldpw), salt, (size_t)slen, iters, derived) == 0 &&
            oc_ct_eq(derived, stored, OC_PW_HASH_LEN)) ok = 1;
    }
    sqlite3_finalize(st);
    if (!ok) return profile_err(j, OC_ERR_FORBIDDEN);

    uint8_t salt[OC_PW_SALT_LEN], hash[OC_PW_HASH_LEN];
    if (oc_rand_bytes(salt, sizeof salt) != 0 ||
        oc_pw_derive(newpw, strlen(newpw), salt, sizeof salt, OC_PW_ITERATIONS, hash) != 0)
        return profile_err(j, OC_ERR_INTERNAL);
    sqlite3_prepare_v2(db,
        "UPDATE local_credentials SET salt=?, iterations=?, hash=?, updated_at_ms=? WHERE user_id=?;",
        -1, &st, NULL);
    sqlite3_bind_blob (st, 1, salt, sizeof salt, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)OC_PW_ITERATIONS);
    sqlite3_bind_blob (st, 3, hash, sizeof hash, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 4, (sqlite3_int64)dbw_now_ms());
    sqlite3_bind_int64(st, 5, (sqlite3_int64)j->user_id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) return profile_err(j, OC_ERR_INTERNAL);
    /* Never the password itself — only that it changed (ARCH-79). */
    audit_actor(db, OC_AUDIT_ACCOUNT, "password.change", j->user_id, 0, NULL, 1, NULL);
    return profile_ok(j, lookup_display_name(db, j->user_id));
}

/* Read-only query jobs run on the reader connection (ARCH-66), off the writer
 * thread, so a heavy search/backfill can't stall message sends or auth. */
static oc_dbres *process_storage_status(sqlite3 *db, const oc_job *j);
static oc_dbres *process_audit_query(sqlite3 *db, const oc_job *j);

static int is_read_job(int type) {
    return type == OC_JOB_BACKFILL || type == OC_JOB_SEARCH ||
           type == OC_JOB_LIST_CHANNELS || type == OC_JOB_LIST_USERS ||
           type == OC_JOB_LIST_REACTIONS || type == OC_JOB_LIST_THREAD ||
           type == OC_JOB_TYPING || type == OC_JOB_ATTACH_LOOKUP ||
           type == OC_JOB_LIST_WEBHOOKS || type == OC_JOB_LIST_NOTIFY_PREFS ||
           type == OC_JOB_LIST_CLIENT_SETTINGS ||
           type == OC_JOB_CALL_AUTH ||
           type == OC_JOB_STORAGE_STATUS ||
           type == OC_JOB_AUDIT_QUERY;
}

/* Dispatch a read-only job against `rdb`. */
static oc_dbres *process_read(sqlite3 *rdb, const oc_job *j) {
    if (j->type == OC_JOB_BACKFILL)       return process_backfill(rdb, j);
    if (j->type == OC_JOB_SEARCH)         return process_search(rdb, j);
    if (j->type == OC_JOB_LIST_CHANNELS)  return process_list_channels(rdb, j);
    if (j->type == OC_JOB_LIST_USERS)     return process_list_users(rdb, j);
    if (j->type == OC_JOB_LIST_REACTIONS) return process_list_reactions(rdb, j);
    if (j->type == OC_JOB_LIST_THREAD)    return process_list_thread(rdb, j);
    if (j->type == OC_JOB_TYPING)         return process_typing(rdb, j);
    if (j->type == OC_JOB_ATTACH_LOOKUP)  return process_attach_lookup(rdb, j);
    if (j->type == OC_JOB_STORAGE_STATUS) return process_storage_status(rdb, j);
    if (j->type == OC_JOB_AUDIT_QUERY)    return process_audit_query(rdb, j);
    if (j->type == OC_JOB_LIST_WEBHOOKS)  return process_list_webhooks(rdb, j);
    if (j->type == OC_JOB_LIST_NOTIFY_PREFS) return process_list_notify_prefs(rdb, j);
    if (j->type == OC_JOB_LIST_CLIENT_SETTINGS) return process_list_client_settings(rdb, j);
    if (j->type == OC_JOB_CALL_AUTH)      return process_call_auth(rdb, j);
    return NULL;
}

/* Dispatch a write (or auth) job against the single write connection. */
static oc_dbres *process_storage_maint(sqlite3 *db, const oc_job *j);

static oc_dbres *process_write(oc_dbwriter *w, const oc_job *j) {
    if (j->type == OC_JOB_AUTH)          return process_auth(w, j);
    if (j->type == OC_JOB_SEND)          return process_send(w->db, j);
    if (j->type == OC_JOB_REGISTER)      return process_register(w, j);
    if (j->type == OC_JOB_SET_ROLE)      return process_set_role(w->db, j);
    if (j->type == OC_JOB_LOGOUT)        return process_logout(w->db, j);
    if (j->type == OC_JOB_EDIT)          return process_edit(w->db, j);
    if (j->type == OC_JOB_DELETE)        return process_delete(w->db, j);
    if (j->type == OC_JOB_CREATE_CHANNEL) return process_create_channel(w->db, j);
    if (j->type == OC_JOB_JOIN_CHANNEL)   return process_join_channel(w->db, j);
    if (j->type == OC_JOB_LEAVE_CHANNEL)  return process_leave_channel(w->db, j);
    if (j->type == OC_JOB_INVITE_CHANNEL) return process_invite_channel(w->db, j);
    if (j->type == OC_JOB_REMOVE_CHANNEL) return process_remove_channel(w->db, j);
    if (j->type == OC_JOB_OPEN_DM)        return process_open_dm(w->db, j);
    if (j->type == OC_JOB_INVITE_USER)    return process_invite_user(w->db, j);
    if (j->type == OC_JOB_REMOVE_USER)    return process_remove_user(w->db, j);
    if (j->type == OC_JOB_REDEEM)         return process_redeem(w, j);
    if (j->type == OC_JOB_REACT)          return process_react(w->db, j);
    if (j->type == OC_JOB_SEND_REPLY)     return process_send_reply(w->db, j);
    if (j->type == OC_JOB_SETUP_INVITE)   return process_setup_invite(w->db, j);
    if (j->type == OC_JOB_CLIENT_ACK)     return process_client_ack(w->db, j);
    if (j->type == OC_JOB_LOAD_IDENTITY)  return process_load_identity(w->db, j);
    if (j->type == OC_JOB_STORE_IDENTITY) return process_store_identity(w->db, j);
    if (j->type == OC_JOB_LOAD_ENROLLMENT)  return process_load_enrollment(w->db, j);
    if (j->type == OC_JOB_STORE_ENROLLMENT) return process_store_enrollment(w->db, j);
    if (j->type == OC_JOB_ATTACH_CREATE)   return process_attach_create(w->db, j);
    if (j->type == OC_JOB_ATTACH_FINALIZE) return process_attach_finalize(w->db, j);
    if (j->type == OC_JOB_CREATE_WEBHOOK)  return process_create_webhook(w->db, j);
    if (j->type == OC_JOB_WEBHOOK_POST)    return process_webhook_post(w->db, j);
    if (j->type == OC_JOB_DELETE_WEBHOOK)  return process_delete_webhook(w->db, j);
    if (j->type == OC_JOB_SET_NOTIFY_PREF) return process_set_notify_pref(w->db, j);
    if (j->type == OC_JOB_SET_DND)         return process_set_dnd(w->db, j);
    if (j->type == OC_JOB_REGISTER_DEVICE_TOKEN)   return process_register_device_token(w->db, j);
    if (j->type == OC_JOB_UNREGISTER_DEVICE_TOKEN) return process_unregister_device_token(w->db, j);
    if (j->type == OC_JOB_PRUNE_DEVICE_TOKEN)      return process_prune_device_token(w->db, j);
    if (j->type == OC_JOB_SET_CLIENT_SETTING) return process_set_client_setting(w->db, j);
    if (j->type == OC_JOB_SET_DISPLAY_NAME)   return process_set_display_name(w->db, j);
    if (j->type == OC_JOB_CHANGE_PASSWORD)    return process_change_password(w->db, j);
    if (j->type == OC_JOB_STORAGE_MAINT)      return process_storage_maint(w->db, j);
    return NULL;
}



/* Storage usage for an owner/admin (REQ-214). The authorization check happens on
 * the net thread before this job is submitted; here we only gather. The
 * reclamation counts come from the attachments table's reclaim_reason
 * (migration 0015), which is what makes eviction auditable after the fact
 * without keeping a second, ever-growing log. */
static oc_dbres *process_storage_status(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->type = OC_RES_STORAGE_STATUS;
    r->conn_id = j->conn_id;

    /* Authorization on the CURRENT role, read here rather than trusted from the
     * connection, so a demotion mid-session takes effect at once. */
    uint8_t role = OC_ROLE_MEMBER;
    if (!user_role(db, j->user_id, &role) || !oc_role_can_manage_members(role)) {
        r->type = OC_RES_STORAGE_ERR;
        r->err_code = OC_ERR_FORBIDDEN;
        return r;
    }

    sqlite3_stmt *st = NULL;
    /* Live attachments: those whose bytes are still present. */
    if (sqlite3_prepare_v2(db,
            "SELECT COUNT(*), COALESCE(SUM(size),0) FROM attachments "
            "WHERE reclaimed_at_ms = 0;", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) {
            r->st_attach_count = (uint64_t)sqlite3_column_int64(st, 0);
            r->st_attach_bytes = (uint64_t)sqlite3_column_int64(st, 1);
        }
        sqlite3_finalize(st);
    }
    st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT reclaim_reason, COUNT(*), MAX(reclaimed_at_ms) FROM attachments "
            "WHERE reclaimed_at_ms > 0 GROUP BY reclaim_reason;", -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            uint64_t n = (uint64_t)sqlite3_column_int64(st, 1);
            uint64_t last = (uint64_t)sqlite3_column_int64(st, 2);
            switch (sqlite3_column_int(st, 0)) {
            case OC_RECLAIM_ORPHAN:  r->st_rec_orphan  = n; break;
            case OC_RECLAIM_EXPIRED: r->st_rec_expired = n; break;
            case OC_RECLAIM_EVICTED: r->st_rec_evicted = n; break;
            default: break;
            }
            if (last > r->st_last_reclaim_ms) r->st_last_reclaim_ms = last;
        }
        sqlite3_finalize(st);
    }
    return r;
}



/* Read a page of the audit log, newest first (REQ-251). Paging is by
 * `audit_before_ms` rather than an offset so a page boundary stays stable while
 * new entries arrive. Authorization is on the CURRENT role, like the storage
 * report, so a demotion takes effect mid-session. */
static oc_dbres *process_audit_query(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->type = OC_RES_AUDIT_PAGE;
    r->conn_id = j->conn_id;

    uint8_t role = OC_ROLE_MEMBER;
    if (!user_role(db, j->user_id, &role) || !oc_role_can_manage_members(role)) {
        r->type = OC_RES_AUDIT_ERR;
        r->err_code = OC_ERR_FORBIDDEN;
        return r;
    }

    uint32_t lim = j->audit_limit ? j->audit_limit : 50;
    if (lim > 200) lim = 200;
    r->audit = calloc(lim, sizeof *r->audit);
    if (!r->audit) { r->type = OC_RES_AUDIT_ERR; r->err_code = OC_ERR_INTERNAL; return r; }

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT at_ms, family, action, actor_id, actor_name, target_id, target,"
            " outcome, detail FROM audit_log "
            "WHERE (?1 = 0 OR at_ms < ?1) ORDER BY at_ms DESC, id DESC LIMIT ?2;",
            -1, &st, NULL) != SQLITE_OK) {
        r->type = OC_RES_AUDIT_ERR; r->err_code = OC_ERR_INTERNAL; return r;
    }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->audit_before_ms);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)lim);
    while (sqlite3_step(st) == SQLITE_ROW && r->n_audit < lim) {
        oc_audit_row *a = &r->audit[r->n_audit];
        a->at_ms     = (uint64_t)sqlite3_column_int64(st, 0);
        a->family    = (uint8_t)sqlite3_column_int(st, 1);
        const char *v;
        v = (const char *)sqlite3_column_text(st, 2); a->action     = v ? strdup(v) : NULL;
        a->actor_id  = (uint64_t)sqlite3_column_int64(st, 3);
        v = (const char *)sqlite3_column_text(st, 4); a->actor_name = v ? strdup(v) : NULL;
        a->target_id = (uint64_t)sqlite3_column_int64(st, 5);
        v = (const char *)sqlite3_column_text(st, 6); a->target     = v ? strdup(v) : NULL;
        a->outcome   = (uint8_t)sqlite3_column_int(st, 7);
        v = (const char *)sqlite3_column_text(st, 8); a->detail     = v ? strdup(v) : NULL;
        r->n_audit++;
    }
    sqlite3_finalize(st);
    return r;
}

/* Age out audit entries, PER FAMILY (REQ-251b). A single global cap would let an
 * attacker who can generate failed logins flood the table and push older
 * administrative entries past the limit — the audit trail becomes a way to erase
 * evidence. Each family is pruned against its own age budget instead, so
 * security noise can only evict security noise. */
static void prune_audit(sqlite3 *db, uint64_t max_age_ms) {
    if (!max_age_ms) return;
    uint64_t now = dbw_now_ms();
    uint64_t cutoff = (now > max_age_ms) ? now - max_age_ms : 0;
    static const int FAMILIES[] = { OC_AUDIT_ADMIN, OC_AUDIT_ACCOUNT,
                                    OC_AUDIT_SECURITY, OC_AUDIT_MODERATION };
    for (size_t i = 0; i < sizeof FAMILIES / sizeof FAMILIES[0]; i++) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(db,
                "DELETE FROM audit_log WHERE family=?1 AND at_ms < ?2;",
                -1, &st, NULL) != SQLITE_OK) continue;
        sqlite3_bind_int(st, 1, FAMILIES[i]);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)cutoff);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
}

/* --- storage maintenance (REQ-213/215/217, ARCH-78) ----------------------- */

/* Append one row to the reclaim list, tombstoning it in the same step. Returns 1
 * if it was added. The row is marked reclaimed here, on the writer, while the
 * BYTES are deleted later by the transfer pool — so a crash between the two
 * leaves an orphaned blob (which the next orphan sweep collects) rather than a
 * live row pointing at bytes that are gone. That asymmetry is deliberate:
 * dangling metadata is a visible bug, a stray blob is merely wasted space. */
static int reclaim_add(sqlite3 *db, oc_dbres *r, size_t cap,
                       uint64_t aid, const char *key, uint64_t now, int reason) {
    if (r->n_reclaim >= cap) return 0;
    char *dup = key ? strdup(key) : NULL;
    if (!dup) return 0;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "UPDATE attachments SET reclaimed_at_ms=?1, reclaim_reason=?3 "
            "WHERE id=?2 AND reclaimed_at_ms=0;",
            -1, &st, NULL) != SQLITE_OK) { free(dup); return 0; }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)now);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)aid);
    sqlite3_bind_int(st, 3, reason);
    int done = (sqlite3_step(st) == SQLITE_DONE) && sqlite3_changes(db) > 0;
    sqlite3_finalize(st);
    if (!done) { free(dup); return 0; }          /* someone else got there first */
    r->reclaim[r->n_reclaim].storage_key = dup;
    r->reclaim[r->n_reclaim].attachment_id = aid;
    r->n_reclaim++;
    return 1;
}

/* Run one maintenance pass. Three tiers in order, stopping at the batch cap so a
 * badly over-limit box recovers across several passes instead of stalling the
 * daemon in one long sweep (REQ-212/218). */
static oc_dbres *process_storage_maint(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->type = OC_RES_STORAGE_MAINT;
    r->conn_id = j->conn_id;

    size_t cap = j->maint_batch ? j->maint_batch : 64;
    r->reclaim = calloc(cap, sizeof *r->reclaim);
    if (!r->reclaim) { free(r); return NULL; }

    uint64_t now = dbw_now_ms();
    sqlite3_stmt *st = NULL;

    /* Age out the audit log alongside the blobs (REQ-251a), per family. */
    prune_audit(db, j->audit_max_age_ms);

    /* Tier 1 (REQ-213): orphans — uploaded but never referenced by a message,
     * and old enough that an in-flight upload cannot be caught by mistake. This
     * is the sweep migration 0009's index was created for, and it is pure
     * garbage collection: nobody was ever promised these bytes. */
    if (sqlite3_prepare_v2(db,
            "SELECT id, storage_key FROM attachments "
            "WHERE message_id IS NULL AND reclaimed_at_ms = 0 AND created_at_ms < ?1 "
            "ORDER BY created_at_ms ASC LIMIT ?2;", -1, &st, NULL) == SQLITE_OK) {
        uint64_t cutoff = (now > j->maint_grace_ms) ? now - j->maint_grace_ms : 0;
        sqlite3_bind_int64(st, 1, (sqlite3_int64)cutoff);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)cap);
        while (sqlite3_step(st) == SQLITE_ROW && r->n_reclaim < cap) {
            if (reclaim_add(db, r, cap, (uint64_t)sqlite3_column_int64(st, 0),
                            (const char *)sqlite3_column_text(st, 1), now, OC_RECLAIM_ORPHAN))
                r->maint_orphans++;
        }
        sqlite3_finalize(st);
    }

    /* Tier 2a (REQ-217): past the configured maximum age. A standing policy, not
     * a response to pressure — it runs whether or not the disk is tight. */
    if (j->maint_max_age_ms && r->n_reclaim < cap) {
        st = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT id, storage_key FROM attachments "
                "WHERE reclaimed_at_ms = 0 AND created_at_ms < ?1 "
                "ORDER BY created_at_ms ASC LIMIT ?2;", -1, &st, NULL) == SQLITE_OK) {
            uint64_t cutoff = (now > j->maint_max_age_ms) ? now - j->maint_max_age_ms : 0;
            sqlite3_bind_int64(st, 1, (sqlite3_int64)cutoff);
            sqlite3_bind_int64(st, 2, (sqlite3_int64)(cap - r->n_reclaim));
            while (sqlite3_step(st) == SQLITE_ROW && r->n_reclaim < cap) {
                if (reclaim_add(db, r, cap, (uint64_t)sqlite3_column_int64(st, 0),
                                (const char *)sqlite3_column_text(st, 1), now, OC_RECLAIM_EXPIRED))
                    r->maint_expired++;
            }
            sqlite3_finalize(st);
        }
    }

    /* Tier 2b (REQ-215): under pressure, evict the oldest attachments regardless
     * of age — the destructive tier, and the only one that removes something a
     * user can still see. The caller sets maint_evict only when free space is
     * actually below the pressure watermark and the operator has not disabled
     * it. The grace window protects a file shared into a live conversation from
     * vanishing mid-discussion. */
    if (j->maint_evict && r->n_reclaim < cap) {
        st = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT id, storage_key FROM attachments "
                "WHERE reclaimed_at_ms = 0 AND created_at_ms < ?1 "
                "ORDER BY created_at_ms ASC LIMIT ?2;", -1, &st, NULL) == SQLITE_OK) {
            uint64_t cutoff = (now > j->maint_grace_ms) ? now - j->maint_grace_ms : 0;
            sqlite3_bind_int64(st, 1, (sqlite3_int64)cutoff);
            sqlite3_bind_int64(st, 2, (sqlite3_int64)(cap - r->n_reclaim));
            while (sqlite3_step(st) == SQLITE_ROW && r->n_reclaim < cap) {
                if (reclaim_add(db, r, cap, (uint64_t)sqlite3_column_int64(st, 0),
                                (const char *)sqlite3_column_text(st, 1), now, OC_RECLAIM_EVICTED))
                    r->maint_evicted++;
            }
            sqlite3_finalize(st);
        }
    }
    return r;
}

/* Prune the idempotency map (ARCH-44) at most once per interval, dropping
 * (channel, token) rows older than the retention window. Writer thread only. */
static void maybe_prune_idem(oc_dbwriter *w) {
    uint64_t now = dbw_now_ms();
    if (w->last_prune_ms != 0 && now - w->last_prune_ms < w->prune_interval_ms) return;
    w->last_prune_ms = now;
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(w->db, "DELETE FROM sent_messages WHERE created_at_ms < ?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)(now - w->idem_retention_ms));
    sqlite3_step(st);
    sqlite3_finalize(st);
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

        oc_dbres *r = process_write(w, j);
        job_free(j);
        push_result(w, r);
        maybe_prune_idem(w);
    }
    return NULL;
}

static void *reader_loop(void *arg) {
    oc_dbwriter *w = (oc_dbwriter *)arg;
    for (;;) {
        pthread_mutex_lock(&w->mu);
        while (!w->stop && !w->rjobs_head)
            pthread_cond_wait(&w->read_cv, &w->mu);
        if (w->stop && !w->rjobs_head) { pthread_mutex_unlock(&w->mu); break; }
        oc_job *j = w->rjobs_head;
        w->rjobs_head = j->next;
        if (!w->rjobs_head) w->rjobs_tail = NULL;
        pthread_mutex_unlock(&w->mu);

        oc_dbres *r = process_read(w->rdb, j);
        job_free(j);
        push_result(w, r);
    }
    return NULL;
}

void oc_dbwriter_submit(oc_dbwriter *w, oc_job *j) {
    pthread_mutex_lock(&w->mu);
    j->next = NULL;
    if (is_read_job(j->type)) {                 /* route to the reader (ARCH-66) */
        if (w->rjobs_tail) w->rjobs_tail->next = j; else w->rjobs_head = j;
        w->rjobs_tail = j;
        pthread_cond_signal(&w->read_cv);
    } else {
        if (w->jobs_tail) w->jobs_tail->next = j; else w->jobs_head = j;
        w->jobs_tail = j;
        pthread_cond_signal(&w->cv);
    }
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

/* Registered-user cap (CP-7, OPENCHIME_MAX_USERS). <=0 means unlimited. Set before
 * serving; read only on the writer thread. */
void oc_dbwriter_set_max_users(oc_dbwriter *w, int max_users) {
    w->max_users = max_users > 0 ? max_users : 0;
}

uint8_t oc_dbwriter_auth_methods(oc_dbwriter *w) { return w->auth_methods; }

const char *oc_dbwriter_oidc_params(oc_dbwriter *w) {
    return w->oidc_params ? w->oidc_params : "";
}

void oc_dbwriter_set_idem_retention(oc_dbwriter *w, uint64_t retention_ms,
                                    uint64_t interval_ms) {
    pthread_mutex_lock(&w->mu);
    w->idem_retention_ms = retention_ms;
    w->prune_interval_ms = interval_ms;
    w->last_prune_ms = 0;   /* let the next write prune immediately */
    pthread_mutex_unlock(&w->mu);
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

/* Register a push device token (ARCH-85). Submit + block for the ack — setup/test
 * helper (drains one result), not the live path (the net loop submits the job from
 * the client frame and consumes the ack via the normal result dispatch). */
int oc_dbwriter_register_device_token(oc_dbwriter *w, uint64_t user_id,
                                      uint8_t platform, const char *token) {
    if (!token || !*token) return 0;
    oc_job *j = oc_job_new(OC_JOB_REGISTER_DEVICE_TOKEN, 0);
    if (!j) return 0;
    j->user_id = user_id;
    j->device_platform = platform;
    j->device_token = strdup(token);
    if (!j->device_token) { job_free(j); return 0; }
    oc_dbwriter_submit(w, j);
    for (int i = 0; i < 3000; i++) {
        oc_dbres *r = oc_dbwriter_next_result(w);
        if (r) {
            int ok = (r->type == OC_RES_DEVICE_TOKEN_OK);
            oc_dbres_free(r);
            return ok;
        }
        usleep(1000);
    }
    return 0;
}

/* Prune a stale token — fire-and-forget (the job returns no result, so this is
 * safe to call from the push worker thread while the net loop consumes results). */
void oc_dbwriter_prune_device_token(oc_dbwriter *w, const char *token) {
    if (!token || !*token) return;
    oc_job *j = oc_job_new(OC_JOB_PRUNE_DEVICE_TOKEN, 0);
    if (!j) return;
    j->device_token = strdup(token);
    if (!j->device_token) { job_free(j); return; }
    oc_dbwriter_submit(w, j);
}

/* Setup-time helper (REQ-024): if the tenant has no owner, mint a one-time owner
 * invite and return its raw token via `token_out` (returns 1); returns 0 if an
 * owner already exists or on error. Like register_local, must run before a live
 * result consumer (it drains one result). */
int oc_dbwriter_setup_invite(oc_dbwriter *w, uint8_t token_out[OC_INVITE_TOKEN_LEN]) {
    oc_job *j = oc_job_new(OC_JOB_SETUP_INVITE, 0);
    if (!j) return 0;
    oc_dbwriter_submit(w, j);
    for (int i = 0; i < 3000; i++) {
        oc_dbres *r = oc_dbwriter_next_result(w);
        if (r) {
            int minted = 0;
            if (r->type == OC_RES_INVITE_OK) {
                memcpy(token_out, r->session_token, OC_INVITE_TOKEN_LEN);
                minted = 1;
            }
            oc_dbres_free(r);
            return minted;
        }
        usleep(1000);
    }
    return 0;
}

/* Load the persisted TLS identity (ARCH-66b). Returns 1 and heap-allocates the
 * cert/key PEM into the out params (caller frees) if one is stored, else 0.
 * Setup-time only. */
int oc_dbwriter_load_identity(oc_dbwriter *w, char **cert_out, char **key_out) {
    *cert_out = NULL; *key_out = NULL;
    oc_job *j = oc_job_new(OC_JOB_LOAD_IDENTITY, 0);
    if (!j) return 0;
    oc_dbwriter_submit(w, j);
    for (int i = 0; i < 3000; i++) {
        oc_dbres *r = oc_dbwriter_next_result(w);
        if (r) {
            int have = 0;
            if (r->type == OC_RES_IDENTITY && r->cert_pem && r->key_pem) {
                *cert_out = strdup(r->cert_pem);
                *key_out  = strdup(r->key_pem);
                have = (*cert_out && *key_out);
            }
            oc_dbres_free(r);
            return have;
        }
        usleep(1000);
    }
    return 0;
}

/* Persist the TLS identity so it survives a restore onto a new box. Returns 1 on success.
 * Setup-time only. */
int oc_dbwriter_store_identity(oc_dbwriter *w, const char *cert_pem, const char *key_pem) {
    oc_job *j = oc_job_new(OC_JOB_STORE_IDENTITY, 0);
    if (!j) return 0;
    j->cert_pem = strdup(cert_pem);
    j->key_pem  = strdup(key_pem);
    if (!j->cert_pem || !j->key_pem) { job_free(j); return 0; }
    oc_dbwriter_submit(w, j);
    for (int i = 0; i < 3000; i++) {
        oc_dbres *r = oc_dbwriter_next_result(w);
        if (r) {
            int ok = (r->type == OC_RES_OK);
            oc_dbres_free(r);
            return ok;
        }
        usleep(1000);
    }
    return 0;
}

/* Load the persisted federated-enrollment identity (CP-8). Returns 1 (+ heap
 * privkey/audience/active) if a row exists, else 0. Setup-time only. */
int oc_dbwriter_load_enrollment(oc_dbwriter *w, char **privkey_out, char **audience_out, int *active_out) {
    *privkey_out = NULL; *audience_out = NULL; *active_out = 0;
    oc_job *j = oc_job_new(OC_JOB_LOAD_ENROLLMENT, 0);
    if (!j) return 0;
    oc_dbwriter_submit(w, j);
    for (int i = 0; i < 3000; i++) {
        oc_dbres *r = oc_dbwriter_next_result(w);
        if (r) {
            int have = 0;
            if (r->type == OC_RES_ENROLLMENT && r->enroll_present &&
                r->enroll_privkey && r->enroll_audience) {
                *privkey_out  = strdup(r->enroll_privkey);
                *audience_out = strdup(r->enroll_audience);
                *active_out   = r->enroll_active;
                have = (*privkey_out && *audience_out);
            }
            oc_dbres_free(r);
            return have;
        }
        usleep(1000);
    }
    return 0;
}

/* Persist the enrollment keypair + audience + state (CP-8). Returns 1 on success.
 * Setup-time only. */
int oc_dbwriter_store_enrollment(oc_dbwriter *w, const char *privkey_pem, const char *audience, int active) {
    oc_job *j = oc_job_new(OC_JOB_STORE_ENROLLMENT, 0);
    if (!j) return 0;
    j->enroll_privkey  = strdup(privkey_pem);
    j->enroll_audience = strdup(audience);
    j->enroll_active   = active;
    if (!j->enroll_privkey || !j->enroll_audience) { job_free(j); return 0; }
    oc_dbwriter_submit(w, j);
    for (int i = 0; i < 3000; i++) {
        oc_dbres *r = oc_dbwriter_next_result(w);
        if (r) {
            int ok = (r->type == OC_RES_OK);
            oc_dbres_free(r);
            return ok;
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
    w->idem_retention_ms = OC_IDEM_RETENTION_MS;
    w->prune_interval_ms = OC_PRUNE_INTERVAL_MS;
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

    /* Read-only connection for query jobs (ARCH-66): the same WAL file, opened
     * query_only so it can never write. WAL lets it read concurrently with the
     * writer without blocking it. */
    if (sqlite3_open(path, &w->rdb) != SQLITE_OK) {
        fprintf(stderr, "dbwriter: open read conn failed: %s\n", sqlite3_errmsg(w->rdb));
        goto fail;
    }
    sqlite3_busy_timeout(w->rdb, 5000);
    sqlite3_exec(w->rdb, "PRAGMA query_only=1;", NULL, NULL, NULL);

    pthread_mutex_init(&w->mu, NULL);
    pthread_cond_init(&w->cv, NULL);
    pthread_cond_init(&w->read_cv, NULL);
    if (pthread_create(&w->thread, NULL, writer_loop, w) != 0) {
        fprintf(stderr, "dbwriter: writer thread create failed\n");
        pthread_mutex_destroy(&w->mu);
        pthread_cond_destroy(&w->cv);
        pthread_cond_destroy(&w->read_cv);
        goto fail;
    }
    w->started = 1;
    if (pthread_create(&w->reader, NULL, reader_loop, w) != 0) {
        fprintf(stderr, "dbwriter: reader thread create failed\n");
        pthread_mutex_lock(&w->mu);
        w->stop = 1;
        pthread_cond_signal(&w->cv);
        pthread_cond_signal(&w->read_cv);
        pthread_mutex_unlock(&w->mu);
        pthread_join(w->thread, NULL);
        pthread_mutex_destroy(&w->mu);
        pthread_cond_destroy(&w->cv);
        pthread_cond_destroy(&w->read_cv);
        w->started = 0;
        goto fail;
    }
    w->reader_started = 1;
    return w;

fail:
    if (w->evfd >= 0) close(w->evfd);
    sqlite3_close(w->rdb);
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
        pthread_cond_signal(&w->read_cv);
        pthread_mutex_unlock(&w->mu);
        pthread_join(w->thread, NULL);
        if (w->reader_started) pthread_join(w->reader, NULL);
        pthread_mutex_destroy(&w->mu);
        pthread_cond_destroy(&w->cv);
        pthread_cond_destroy(&w->read_cv);
    }
    for (oc_job *j = w->jobs_head; j; ) { oc_job *n = j->next; job_free(j); j = n; }
    for (oc_job *j = w->rjobs_head; j; ) { oc_job *n = j->next; job_free(j); j = n; }
    for (oc_dbres *r = w->res_head; r; ) { oc_dbres *n = r->next; oc_dbres_free(r); r = n; }
    free(w->oidc_issuer); free(w->oidc_audience);
    free(w->oidc_pubkey_pem); free(w->oidc_params);
    oc_ratelimit_free(w->auth_rl);
    oc_ratelimit_free(w->source_rl);
    if (w->evfd >= 0) close(w->evfd);
    sqlite3_close(w->rdb);
    sqlite3_close(w->db);
    free(w);
}
