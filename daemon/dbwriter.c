/*
 * OpenChime DB-writer thread + write-job queue. See dbwriter.h and migrate.h.
 *
 * One thread owns the write connection. It pops jobs off a request queue,
 * performs all DB work (local/session AUTH + account REGISTER, SEND persist with
 * idempotency, backfill), and pushes results onto a completion queue, signalling
 * the net thread via an eventfd.
 */

#include <stdarg.h>
#include "richtext.h"   /* oc_rt_plain_excerpt: excerpts hide spoilers */
#include "action.h"       /* what makes a message an action (ARCH-115) */
#include "todo.h"         /* what makes a message a to-do list */
#include "mention.h"      /* the shared @mention scanner (ARCH-89) */
#include "speakable.h"    /* what read-aloud says for a body (ARCH-111) */
#include "voice_pick.h"   /* a default read-aloud voice (REQ-292) */
#include "dbwriter.h"
#include "callrec_render.h"   /* a recording's size (CALLS.md §8) */
#include "e2e_hpke.h"
#include "pwpolicy.h"

#include "config.h"   /* the page size a files listing answers with (REQ-143) */
#include "unfurl.h"   /* OC_UNFURL_MAX_URLS: the store re-validates presence */
#include "url.h"
#include "migrate.h"
#include "protocol.h"
#include "auth.h"
#include "authpool.h"
#include "joinrules.h"
#include "jwt.h"
#include "scopes.h"
#include "oauth.h"    /* the AI clients' tokens and codes (AUTH.md §8.13) */
#include "localissuer.h"
#include "ratelimit.h"
#include "roles.h"
#include "srccount.h"   /* oc_source_key */
#include "totp.h"
#include "webauthn.h"
#include "sum_store.h"    /* summaries purged with what they say (ARCH-116) */
#include "sum_worker.h"

#include <pthread.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <strings.h>
#include <mbedtls/sha256.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

/* Read-only query workers (ARCH-66), each with its own connection to the WAL
 * database. A connection's read jobs all go to one of them -- the one its id
 * picks -- so they complete in the order they were submitted, as they did when
 * there was one; jobs with no connection go round in turn. */
#define OC_DB_READERS 3
#define OC_AUTH_THREADS 2

typedef struct {
    struct oc_dbwriter *w;
    sqlite3        *rdb;
    pthread_t       th;
    int             started;
    pthread_cond_t  cv;                       /* wakes this reader */
    oc_job         *head, *tail;              /* net -> this reader */
} db_reader;

struct oc_dbwriter {
    sqlite3        *db;                       /* the single write connection (ARCH-5) */
    pthread_t       thread;                   /* writer */
    pthread_mutex_t mu;
    pthread_cond_t  cv;                       /* wakes the writer */
    oc_job         *jobs_head, *jobs_tail;    /* net -> writer (write jobs) */
    db_reader       readers[OC_DB_READERS];
    unsigned        next_reader;              /* for jobs no connection owns */
    oc_dbres      *res_head,  *res_tail;     /* writer|readers -> net */
    /* The auth pool (AUTH.md §2): a password's key derivation -- deliberately
     * the slowest thing a sign-in does -- runs here, so a burst of sign-ins
     * does not hold the writer, and with it everyone's sends. */
    oc_authpool    *auth;
    int             auth_deferred;           /* writer-only: the job went on to a reader */
    int             evfd;                    /* signals results ready */
    int             stop;
    int             hold;                     /* oc_dbwriter_hold: OC_DBW_HOLD_* not served */
    int             started;

    /* Auth config (set before serving; read only on the writer thread). */
    uint8_t         auth_methods;            /* advertised in AUTH_CHALLENGE */
    int             oidc_enabled;
    char           *oidc_issuer;
    char           *oidc_audience;
    char           *oidc_pubkey_pem;         /* pinned beneath the published keys ("" none) */
    /* What relay tokens are verified against: the pinned keys and the ones the
     * relay last published (relaykeys.h), swapped whole under keys_mu, which is
     * all another thread touches. */
    pthread_mutex_t keys_mu;
    char           *oidc_keyset_pem;
    char           *relay_origin;            /* where the relay is ("" if nowhere) */
    struct oc_seen_jti *seen_jti;            /* relay and local tokens already used (AUTH.md §8.2) */
    oc_local_issuer *local_iss;              /* this daemon's issuer for local accounts (§8.10) */
    oc_join_rules  *join_rules;              /* who may join by OIDC (AUTH.md §8.4) */
    int             email_link_any;          /* an emailed code may stand in for a provider */
    oc_ratelimit   *step_rl;                 /* wrong second-step codes, per account */
    char            direct_iss[4][256];      /* the direct connections' issuers (AUTH.md §8.5) */
    int             n_direct_iss;
    uint8_t         factor_key[OC_FACTOR_KEY_LEN];
    int             have_factor_key;         /* without it no step can be passed */
    int             local_mfa;               /* OC_MFA_*: when the second step is asked */
    int             max_users;               /* registered-user cap (AUTH.md §2); 0 = unlimited */
    uint32_t        pw_mem;                  /* Argon2id memory, KiB, for new keys and the unknown-name check */
    oc_ratelimit   *auth_rl;                 /* failed local-auth per account */
    oc_ratelimit   *source_rl;               /* failed sign-ins per source IP, every source */
    oc_ratelimit   *signup_rl;               /* accounts made per source IP, the sign-up page */

    /* Idempotency-map pruning (ARCH-44): drop sent_messages rows older than the
     * retention window, at most once per interval. Writer-thread state only. */
    uint64_t        idem_retention_ms;
    uint64_t        restore_window_ms;    /* how long a delete can be taken back */
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
/* Accounts made on the sign-up page per source in an hour (AUTH.md §2): a team
 * behind one address joins freely, a script making accounts does not. */
#define OC_SIGNUP_SOURCE_MAX        10
#define OC_SIGNUP_WINDOW_MS         (60u * 60u * 1000u)
/* A second step's own limiter (AUTH.md §8.6): five wrong codes per account in
 * five minutes, apart from the password's, so guessing a code is no faster
 * than guessing the password was. */
#define OC_STEP_MAX_FAILURES        5
#define OC_STEP_WINDOW_MS           (5u * 60u * 1000u)

/* Idempotency retention (ARCH-44): keep a (channel, token) -> id mapping this
 * long — enough to cover realistic reconnect-retry — then prune it. Pruning runs
 * at most once per interval on the writer thread. */
#define OC_IDEM_RETENTION_MS (24ull * 60 * 60 * 1000)
#define OC_PRUNE_INTERVAL_MS (60ull * 60 * 1000)

static uint64_t snooze_until(sqlite3 *db, uint64_t uid);   /* fwd — auth and the prefs snapshot both read it */
static void fill_schedule(sqlite3 *db, uint64_t uid, oc_dbres *r);      /* fwd — REQ-136 */
static void fill_alert_prefs(sqlite3 *db, uint64_t uid, oc_dbres *r);   /* fwd — REQ-135 */
static void build_schedule(sqlite3 *db, uint64_t uid, oc_dbres *r);     /* fwd — REQ-136 */

static uint64_t g_dbw_clock_ahead_ms;   /* a test's advance of the writer's clock (dbwriter.h) */
void oc_dbwriter_advance_clock_ms(uint64_t ms) {
    if (ms) __atomic_add_fetch(&g_dbw_clock_ahead_ms, ms, __ATOMIC_RELAXED);
    else    __atomic_store_n(&g_dbw_clock_ahead_ms, 0, __ATOMIC_RELAXED);
}

static uint64_t dbw_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u +
           __atomic_load_n(&g_dbw_clock_ahead_ms, __ATOMIC_RELAXED);
}

/* --- audit log (REQ-251, ARCH-79) ----------------------------------------- */

/* Append one audit entry. Best-effort by design: auditing must never fail the
 * action it describes — a full disk should not make role changes impossible —
 * so errors are swallowed. Writer thread only, so entries inherit the same
 * single-writer ordering as the data they describe.
 *
 * `detail` must never carry the secret involved (ARCH-79): that a password
 * changed, never the password. */
static void audit_log_in(sqlite3 *db, int family, const char *action,
                         uint64_t actor_id, const char *actor_name, uint64_t channel_id,
                         uint64_t target_id, const char *target,
                         int outcome, const char *detail) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO audit_log(at_ms, family, action, actor_id, actor_name,"
            " target_id, target, outcome, detail, channel_id) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10);", -1, &st, NULL) != SQLITE_OK) return;
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
    if (channel_id) sqlite3_bind_int64(st, 10, (sqlite3_int64)channel_id);
    else            sqlite3_bind_null(st, 10);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

/* An entry that happened in no conversation. */
static void audit_log(sqlite3 *db, int family, const char *action,
                      uint64_t actor_id, const char *actor_name,
                      uint64_t target_id, const char *target,
                      int outcome, const char *detail) {
    audit_log_in(db, family, action, actor_id, actor_name, 0, target_id, target, outcome, detail);
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

/* ...for an entry in channel `channel_id`: a change to the channel, or to a
 * message or member in it, which a reader can then go to. */
static void audit_actor_in(sqlite3 *db, int family, const char *action, uint64_t actor_id,
                           uint64_t channel_id, uint64_t target_id, const char *target,
                           int outcome, const char *detail) {
    char *nm = audit_name_of(db, actor_id);
    audit_log_in(db, family, action, actor_id, nm, channel_id, target_id, target, outcome, detail);
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

int oc_job_set_proof(oc_job *j, const void *proof, size_t len) {
    j->proof = malloc(len + 1);
    if (!j->proof) return -1;
    memcpy(j->proof, proof, len);
    j->proof[len] = '\0';
    j->proof_len = len;
    return 0;
}

int oc_job_set_email(oc_job *j, const void *email, size_t len) {
    j->email = malloc(len + 1);
    if (!j->email) return -1;
    memcpy(j->email, email, len);
    j->email[len] = '\0';
    return 0;
}

int oc_job_set_register(oc_job *j, const char *username, const char *password,
                        uint8_t role, uint32_t mem_kib) {
    j->username = username ? strdup(username) : NULL;
    j->password = password ? strdup(password) : NULL;
    if ((username && !j->username) || (password && !j->password)) return -1;
    j->role = role;
    j->mem_kib = mem_kib;
    return 0;
}

int oc_job_set_display(oc_job *j, const char *display) {
    free(j->display);
    j->display = display ? strdup(display) : NULL;
    return display && !j->display ? -1 : 0;
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
    for (uint8_t i = 0; i < j->n_poll_opts; i++) free(j->poll_opts[i]);
    free(j->poll_opts);
    for (uint8_t i = 0; i < j->n_kw_terms; i++) free(j->kw_terms[i]);
    free(j->kw_terms);
    free(j->pri_people);
    free(j->sched_days);
    free(j->alert_key); free(j->alert_message);
    free(j->snip_title); free(j->snip_lang); free(j->snip_text);
    free(j->todo_name); free(j->todo_desc);
    free(j->token);
    free(j->proof);
    free(j->email);
    free(j->wa_cred); free(j->wa_cd); free(j->wa_ad); free(j->wa_sig); free(j->wa_att);
    free(j->direct);
    free(j->sq_from);
    free(j->sq_in);
    free(j->username);
    free(j->password);
    free(j->display);
    free(j->body);
    free(j->cursors);
    free(j->ch_name);
    free(j->pf_full_name);
    free(j->pf_title);
    free(j->pf_pronouns);
    free(j->pf_voice_id);
    free(j->tts_model_version);
    free(j->tts_voices);
    free(j->tts_text);
    free(j->tts_lang);
    free(j->tts_blob_key);
    free(j->exp_version);
    free(j->exp_blob_key);
    for (int i = 0; i < 2; i++) { free(j->pv_keys[i]); free(j->pv_mime[i]); }
    free(j->agent_name); free(j->token_name); free(j->token_scopes); free(j->token_from);
    free(j->owh_url); free(j->owh_events); free(j->owh_reason);
    free(j->mcp_query); free(j->mcp_from); free(j->mcp_action); free(j->mcp_target); free(j->mcp_detail);
    free(j->oa_client_id); free(j->oa_name); free(j->oa_uris); free(j->oa_redirect); free(j->oa_challenge);
    free(j->oa_state); free(j->oa_resource); free(j->oa_request); free(j->oa_code); free(j->oa_verifier);
    free(j->oa_refresh); free(j->via);
    free(j->pf_phone);
    free(j->pf_timezone);
    free(j->emoji);
    free(j->cert_pem);
    free(j->key_pem);
    if (j->tls_state) { oc_tls_state_free(j->tls_state); free(j->tls_state); }
    free(j->enroll_privkey);
    free(j->enroll_audience);
    free(j->filename);
    free(j->mime);
    free(j->cs_client_type);
    free(j->cs_key);
    free(j->cs_value);
    free(j->recipients);
    free(j->pf_name);
    free(j->pf_new_pw);
    free(j->device_token);
    free(j->unf_url);
    free(j->unf_title);
    free(j->unf_descr);
    free(j->sum_version);
    free(j->call_uids);
    free(j->call_subject);
    free(j->call_spool);
    free(j->tr_lang);
    for (size_t i = 0; i < j->n_tr_lines; i++) free(j->tr_lines[i].text);
    free(j->tr_lines);
    free(j->grp_handle); free(j->grp_name); free(j->grp_desc); free(j->grp_uids);
    free(j);
}

/* Free the heap metadata of an attachment list (filename/mime per entry). */
static void free_attach_meta(oc_attach_meta *a, size_t n) {
    for (size_t i = 0; i < n; i++) {
        free(a[i].filename); free(a[i].mime); free(a[i].title); free(a[i].language);
    }
}

/* Reaction aggregates for a replayed window (REQ-070/071). A BROADCAST has no
 * room for them, so a client that stores nothing (ARCH-88) loses every reaction
 * on any reload that does not go through the live fan-out.
 *
 * Shared by BOTH replay paths deliberately. It was inline in the reconnect
 * backfill and simply absent from the history page, so scrolling back or
 * following a permalink rendered messages permanently without their reactions —
 * the defect class this replay exists to prevent, reintroduced by a second
 * copy that was never written. One filler, both callers, as the unfurl fill
 * below already does it. */
/* Each replayed parent's repliers (THREAD_META, REQ-060), beside its count. */
static uint8_t thread_repliers(sqlite3 *db, uint64_t root, uint64_t *out);   /* below */
static void thread_stats(sqlite3 *db, uint64_t root, uint32_t *count, uint64_t *last);   /* below */
static void fill_replay_repliers(sqlite3 *db, oc_dbres *r) {
    for (size_t i = 0; i < r->n_replay; i++)
        if (r->replay[i].reply_count)
            r->replay[i].n_repliers = thread_repliers(db, r->replay[i].message_id, r->replay[i].repliers);
}

static void fill_replay_reactions(sqlite3 *db, oc_dbres *r, uint64_t for_user) {
    if (!r->n_replay) return;
    size_t cap = 16, n = 0;
    struct oc_replay_react *ra = malloc(cap * sizeof *ra);
    for (size_t i = 0; i < r->n_replay && ra; i++) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT emoji, COUNT(*), "
                /* Prefer the requesting user's own id so the client can mark
                 * the chip as theirs; MIN() just picks a stable stand-in. */
                "  COALESCE(MAX(CASE WHEN user_id=?2 THEN user_id END), MIN(user_id)) "
                /* Not the people this user has muted (SET_USER_MUTE): their reactions
                 * are no part of the chips, as REACTION_UPDATED leaves them out. */
                "FROM reactions WHERE message_id=?1 "
                "  AND user_id NOT IN (SELECT muted_id FROM muted_users WHERE user_id=?2) "
                "GROUP BY emoji ORDER BY emoji;",
                -1, &st, NULL) != SQLITE_OK) break;
        sqlite3_bind_int64(st, 1, (sqlite3_int64)r->replay[i].message_id);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)for_user);
        while (sqlite3_step(st) == SQLITE_ROW) {
            if (n == cap) {
                cap *= 2;
                struct oc_replay_react *g = realloc(ra, cap * sizeof *ra);
                if (!g) break;
                ra = g;
            }
            const char *em = (const char *)sqlite3_column_text(st, 0);
            ra[n].message_id = r->replay[i].message_id;
            ra[n].channel_id = r->replay[i].channel_id;
            ra[n].count      = (uint64_t)sqlite3_column_int64(st, 1);
            ra[n].user_id    = (uint64_t)sqlite3_column_int64(st, 2);
            ra[n].emoji      = strdup(em ? em : "");
            n++;
        }
        sqlite3_finalize(st);
    }
    r->rreact = ra;
    r->n_rreact = n;
}

/* Unfurl rows for a replayed window (REQ-222, ARCH-105). A BROADCAST carries
 * none — an unfurl is fetched after the send and travels on its own frame — so
 * without this every preview vanished on reload, the same defect class the
 * reaction and pin replays exist to prevent. One query per replayed message,
 * the shape the reaction aggregates already use. */
static void fill_replay_unfurls(sqlite3 *db, oc_dbres *r) {
    if (!r->n_replay) return;
    size_t cap = 8, n = 0;
    struct oc_replay_unfurl *ua = malloc(cap * sizeof *ua);
    for (size_t i = 0; i < r->n_replay && ua; i++) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT url, COALESCE(title,''), COALESCE(descr,'') "
                "FROM unfurls WHERE message_id=?1 ORDER BY url;", -1, &st, NULL) != SQLITE_OK)
            break;
        sqlite3_bind_int64(st, 1, (sqlite3_int64)r->replay[i].message_id);
        while (sqlite3_step(st) == SQLITE_ROW) {
            if (n == cap) {
                cap *= 2;
                struct oc_replay_unfurl *g = realloc(ua, cap * sizeof *ua);
                if (!g) break;
                ua = g;
            }
            const char *u = (const char *)sqlite3_column_text(st, 0);
            const char *t = (const char *)sqlite3_column_text(st, 1);
            const char *d = (const char *)sqlite3_column_text(st, 2);
            ua[n].message_id = r->replay[i].message_id;
            ua[n].channel_id = r->replay[i].channel_id;
            ua[n].url   = strdup(u ? u : "");
            ua[n].title = strdup(t ? t : "");
            ua[n].descr = strdup(d ? d : "");
            n++;
        }
        sqlite3_finalize(st);
    }
    r->runfurl = ua;
    r->n_runfurl = n;
}

/* Append the forward reference of ONE message, if it has one (REQ-057).
 *
 * The same function serves the live send and both replay paths, which is the
 * whole point: reactions, pins and unfurls each got added to a fan-out and
 * forgotten in a replay, so the reference a client sees on send and the one it
 * sees after a reload are produced by a single query or they will eventually
 * disagree. The live caller reads the row BACK after inserting it, so what the
 * sender is told is what was actually stored. */
static void append_forward(sqlite3 *db, oc_dbres *r, uint64_t message_id, uint64_t channel_id) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT src_channel, src_message, src_author, excerpt, n_attach, attach_name, src_action "
            "FROM forwards WHERE message_id=?1;", -1, &st, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)message_id);
    if (sqlite3_step(st) == SQLITE_ROW) {
        struct oc_replay_forward *g = realloc(r->rfwd, (r->n_rfwd + 1) * sizeof *g);
        if (g) {
            r->rfwd = g;
            struct oc_replay_forward *f = &g[r->n_rfwd];
            const char *ex = (const char *)sqlite3_column_text(st, 3);
            const char *an = (const char *)sqlite3_column_text(st, 5);
            f->message_id = message_id;
            f->channel_id = channel_id;
            f->src_channel = (uint64_t)sqlite3_column_int64(st, 0);
            f->src_message = (uint64_t)sqlite3_column_int64(st, 1);
            f->src_author  = (uint64_t)sqlite3_column_int64(st, 2);
            f->excerpt     = strdup(ex ? ex : "");
            f->n_attach    = (uint16_t)sqlite3_column_int(st, 4);
            f->attach_name = strdup(an ? an : "");
            f->src_action  = (uint8_t)sqlite3_column_int(st, 6);
            r->n_rfwd++;
        }
    }
    sqlite3_finalize(st);
}

/* Forward references for a replayed window (REQ-057). */
static void fill_replay_forwards(sqlite3 *db, oc_dbres *r) {
    for (size_t i = 0; i < r->n_replay; i++)
        append_forward(db, r, r->replay[i].message_id, r->replay[i].channel_id);
}

/* Record whether message `mid` is an action (REQ-058, ARCH-115), from the row
 * as it stands: any earlier answer is replaced, so send, edit and restore all
 * call this one function and an edit that adds or removes "/me" needs nothing
 * of its own. The caller holds the transaction. A call event is never an
 * action, and a tombstone has no body to be one. */
static int channel_is_e2e(sqlite3 *db, uint64_t channel_id);
static int e2e_closed(sqlite3 *db, uint64_t channel_id);

static void store_action(sqlite3 *db, uint64_t mid) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "DELETE FROM actions WHERE message_id=?1;", -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)mid);
    sqlite3_step(st);
    sqlite3_finalize(st);
    {   /* An encrypted DM's body is ciphertext: no action to find in it. */
        uint64_t cid = 0;
        if (sqlite3_prepare_v2(db, "SELECT channel_id FROM messages WHERE id=?1;", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)mid);
            if (sqlite3_step(st) == SQLITE_ROW) cid = (uint64_t)sqlite3_column_int64(st, 0);
        }
        sqlite3_finalize(st);
        st = NULL;
        if (cid && channel_is_e2e(db, cid)) return;
    }
    if (sqlite3_prepare_v2(db,
            "INSERT INTO actions(message_id, channel_id, actor_id, actor_name, text_start, text_len, created_at_ms) "
            "SELECT id, channel_id, author_id, NULLIF(author_name, ''), ?2, ?3, created_at_ms "
            "FROM messages WHERE id=?1;", -1, &st, NULL) != SQLITE_OK)
        return;
    sqlite3_stmt *q = NULL;
    uint32_t start = 0, tlen = 0;
    int is = 0;
    if (sqlite3_prepare_v2(db, "SELECT body FROM messages WHERE id=?1 AND kind=0 AND body IS NOT NULL;",
                           -1, &q, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(q, 1, (sqlite3_int64)mid);
        if (sqlite3_step(q) == SQLITE_ROW)
            is = oc_action_parse((const char *)sqlite3_column_blob(q, 0), (size_t)sqlite3_column_bytes(q, 0),
                                 &start, &tlen);
        sqlite3_finalize(q);
    }
    if (is) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)mid);
        sqlite3_bind_int64(st, 2, start);
        sqlite3_bind_int64(st, 3, tlen);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
}

/* Append message `mid`'s action, read back from the table (REQ-058, ARCH-115).
 * One function for the live result and both replays, as append_forward is.
 * With `always`, a message that is not an action still gets an entry, with
 * text_len 0. Every LIVE message frame -- send, reply, webhook post, edit,
 * restore -- is followed by exactly one, so a client can hold the message until
 * its action arrives and hand both on together: an edit or a restore has to be
 * able to say "no longer", and a toast must never see a message without its
 * answer. A replay sends only the actions there are. */
static void append_action(sqlite3 *db, oc_dbres *r, uint64_t mid, uint64_t channel_id, int always) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT actor_id, actor_name, text_start, text_len FROM actions WHERE message_id=?1;",
            -1, &st, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)mid);
    int row = sqlite3_step(st) == SQLITE_ROW;
    if (row || always) {
        struct oc_replay_action *g = realloc(r->ract, (r->n_ract + 1) * sizeof *g);
        if (g) {
            r->ract = g;
            struct oc_replay_action *a = &g[r->n_ract++];
            memset(a, 0, sizeof *a);
            a->message_id = mid;
            a->channel_id = channel_id;
            if (row) {
                const char *nm = (const char *)sqlite3_column_text(st, 1);
                a->actor_id   = (uint64_t)sqlite3_column_int64(st, 0);
                a->actor_name = nm ? strdup(nm) : NULL;
                a->text_start = (uint32_t)sqlite3_column_int64(st, 2);
                a->text_len   = (uint32_t)sqlite3_column_int64(st, 3);
            }
        }
    }
    sqlite3_finalize(st);
}

/* Actions for a replayed window (REQ-058). */
static void fill_replay_actions(sqlite3 *db, oc_dbres *r) {
    for (size_t i = 0; i < r->n_replay; i++)
        append_action(db, r, r->replay[i].message_id, r->replay[i].channel_id, 0);
}

/* --- Polls (REQ-225, ARCH-119) ------------------------------------------- */

/* Whether job `j`'s poll is one the daemon will store: two to ten options,
 * each named and within OC_POLL_OPTION_MAX, a question, no files or forward
 * riding on it, and a deadline, if any, still to come. */
static int poll_options_valid(const oc_job *j) {
    if (j->n_poll_opts < 2 || j->n_poll_opts > OC_POLL_MAX_OPTIONS) return 0;
    for (uint8_t i = 0; i < j->n_poll_opts; i++) {
        size_t n = j->poll_opts[i] ? strlen(j->poll_opts[i]) : 0;
        if (n == 0 || n > OC_POLL_OPTION_MAX) return 0;
    }
    return 1;
}

static int poll_valid(const oc_job *j, uint64_t now) {
    if (!poll_options_valid(j)) return 0;
    if (!j->body_len || j->n_attach || j->src_message) return 0;
    if (j->poll_close_at_ms && j->poll_close_at_ms <= now) return 0;
    return 1;
}

/* Poll `mid`'s options, as job `j` names them, in the caller's transaction. */
static void store_poll_options(sqlite3 *db, uint64_t mid, const oc_job *j) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "INSERT INTO poll_options(message_id, idx, label) VALUES(?1, ?2, ?3);",
                           -1, &st, NULL) != SQLITE_OK)
        return;
    for (uint8_t i = 0; i < j->n_poll_opts; i++) {
        sqlite3_reset(st);
        sqlite3_bind_int64(st, 1, (sqlite3_int64)mid);
        sqlite3_bind_int(st, 2, i);
        sqlite3_bind_text(st, 3, j->poll_opts[i], -1, SQLITE_STATIC);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
}

/* The poll beside message `mid`, in the caller's transaction. */
static void store_poll(sqlite3 *db, uint64_t mid, uint64_t channel_id, const oc_job *j, uint64_t ts) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO polls(message_id, channel_id, multi, anonymous, close_at_ms, created_at_ms) "
            "VALUES(?1, ?2, ?3, ?4, NULLIF(?5, 0), ?6);", -1, &st, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)mid);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)channel_id);
    sqlite3_bind_int(st, 3, (j->poll_flags & OC_POLL_MULTI) ? 1 : 0);
    sqlite3_bind_int(st, 4, (j->poll_flags & OC_POLL_ANONYMOUS) ? 1 : 0);
    sqlite3_bind_int64(st, 5, (sqlite3_int64)j->poll_close_at_ms);
    sqlite3_bind_int64(st, 6, (sqlite3_int64)ts);
    sqlite3_step(st);
    sqlite3_finalize(st);
    store_poll_options(db, mid, j);
}

/* A poll's rows go with its message: on a delete, in the caller's transaction. */
static void drop_poll(sqlite3 *db, uint64_t mid) {
    static const char *const sql[] = {
        /* Not a poll's, but a message's that goes with it on a delete too:
         * whoever folded it away (migration 0070). */
        "DELETE FROM collapsed_messages WHERE message_id=?1;",
        "DELETE FROM poll_votes WHERE message_id=?1;",
        "DELETE FROM poll_options WHERE message_id=?1;",
        "DELETE FROM polls WHERE message_id=?1;",
    };
    for (size_t i = 0; i < sizeof sql / sizeof sql[0]; i++) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(db, sql[i], -1, &st, NULL) != SQLITE_OK) continue;
        sqlite3_bind_int64(st, 1, (sqlite3_int64)mid);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
}

/* Append message `mid`'s poll -- definition and tallies, read back from the
 * tables -- if it has one. One function for the live send, a vote and every
 * replay, as append_forward is. `viewer` nonzero: the result is for that user,
 * so their own selection rides along (OC_POLL_HAS_MINE); 0 for one that goes
 * to the whole conversation. */
static void append_poll(sqlite3 *db, oc_dbres *r, uint64_t mid, uint64_t channel_id, uint64_t viewer) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT multi, anonymous, COALESCE(close_at_ms, 0) FROM polls WHERE message_id=?1;",
                           -1, &st, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)mid);
    if (sqlite3_step(st) != SQLITE_ROW) { sqlite3_finalize(st); return; }
    struct oc_replay_poll *g = realloc(r->rpoll, (r->n_rpoll + 1) * sizeof *g);
    if (!g) { sqlite3_finalize(st); return; }
    r->rpoll = g;
    struct oc_replay_poll *p = &g[r->n_rpoll++];
    memset(p, 0, sizeof *p);
    p->message_id = mid;
    p->channel_id = channel_id;
    if (sqlite3_column_int(st, 0)) p->flags |= OC_POLL_MULTI;
    if (sqlite3_column_int(st, 1)) p->flags |= OC_POLL_ANONYMOUS;
    p->close_at_ms = (uint64_t)sqlite3_column_int64(st, 2);
    sqlite3_finalize(st);

    if (sqlite3_prepare_v2(db,
            "SELECT o.idx, o.label, (SELECT COUNT(*) FROM poll_votes v WHERE v.message_id=o.message_id AND v.idx=o.idx) "
            "FROM poll_options o WHERE o.message_id=?1 ORDER BY o.idx;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)mid);
        while (sqlite3_step(st) == SQLITE_ROW && p->n_options < OC_POLL_MAX_OPTIONS) {
            const char *l = (const char *)sqlite3_column_text(st, 1);
            p->labels[p->n_options] = strdup(l ? l : "");
            p->counts[p->n_options] = (uint32_t)sqlite3_column_int64(st, 2);
            p->n_options++;
        }
        sqlite3_finalize(st);
    }
    if (sqlite3_prepare_v2(db, "SELECT COUNT(DISTINCT user_id) FROM poll_votes WHERE message_id=?1;",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)mid);
        if (sqlite3_step(st) == SQLITE_ROW) p->voters = (uint32_t)sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    if (viewer) {
        p->flags |= OC_POLL_HAS_MINE;
        if (sqlite3_prepare_v2(db, "SELECT idx FROM poll_votes WHERE message_id=?1 AND user_id=?2;",
                               -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)mid);
            sqlite3_bind_int64(st, 2, (sqlite3_int64)viewer);
            while (sqlite3_step(st) == SQLITE_ROW) {
                int i = sqlite3_column_int(st, 0);
                if (i >= 0 && i < OC_POLL_MAX_OPTIONS) p->mine |= (uint16_t)(1u << i);
            }
            sqlite3_finalize(st);
        }
    }
}

/* Polls for a replayed window, each with the asker's own selection (REQ-225). */
/* The reader's reminders on the saved rows of a replay (REQ-283), so the
 * SAVED_UPDATED it turns into carries them rather than reading as none. */
static void fill_replay_reminders(sqlite3 *db, oc_dbres *r, uint64_t viewer) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT COALESCE(remind_at_ms,0) FROM saved_items WHERE user_id=?1 AND message_id=?2;",
                           -1, &st, NULL) != SQLITE_OK) return;
    for (size_t i = 0; i < r->n_replay; i++) {
        if (!r->replay[i].saved) continue;
        sqlite3_reset(st);
        sqlite3_bind_int64(st, 1, (sqlite3_int64)viewer);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)r->replay[i].message_id);
        if (sqlite3_step(st) == SQLITE_ROW) r->replay[i].remind_at = (uint64_t)sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
}

static void fill_replay_polls(sqlite3 *db, oc_dbres *r, uint64_t viewer) {
    for (size_t i = 0; i < r->n_replay; i++)
        append_poll(db, r, r->replay[i].message_id, r->replay[i].channel_id, viewer);
}

/* --- To-do lists (todo.h) -------------------------------------------------- */

/* Whether body `b` makes a to-do list, and if it is one the daemon refuses, why:
 * 0 for an ordinary message or a good list (*is_list set for the latter), else
 * the error to answer the send with. */
static uint16_t todo_check(const uint8_t *b, size_t n, oc_todo_parsed *tp, int *is_list) {
    *is_list = 0;
    int k = oc_todo_parse((const char *)b, n, tp);
    if (k == OC_TODO_NONE) return 0;
    if (k == OC_TODO_DUP) return OC_ERR_TODO_DUPLICATE;
    if (k != OC_TODO_OK) return OC_ERR_TODO_INVALID;
    *is_list = 1;
    return 0;
}

/* The list body `b` makes, beside message `mid`, in the caller's transaction. */
static void store_todo(sqlite3 *db, uint64_t mid, uint64_t channel_id, uint64_t author,
                       const uint8_t *b, const oc_todo_parsed *tp, uint64_t ts) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO todo_lists(message_id, channel_id, author_id, title, created_at_ms) "
            "VALUES(?1, ?2, ?3, ?4, ?5);", -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)mid);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)channel_id);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)author);
    sqlite3_bind_text (st, 4, (const char *)b + tp->title.off, (int)tp->title.len, SQLITE_STATIC);
    sqlite3_bind_int64(st, 5, (sqlite3_int64)ts);
    sqlite3_step(st);
    sqlite3_finalize(st);
    if (sqlite3_prepare_v2(db,
            "INSERT INTO todo_tasks(message_id, idx, name, description) VALUES(?1, ?2, ?3, ?4);",
            -1, &st, NULL) != SQLITE_OK) return;
    for (uint16_t i = 0; i < tp->n_tasks; i++) {
        sqlite3_reset(st);
        sqlite3_bind_int64(st, 1, (sqlite3_int64)mid);
        sqlite3_bind_int  (st, 2, i);
        sqlite3_bind_text (st, 3, (const char *)b + tp->name[i].off, (int)tp->name[i].len, SQLITE_STATIC);
        sqlite3_bind_text (st, 4, (const char *)b + tp->desc[i].off, (int)tp->desc[i].len, SQLITE_STATIC);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
}

/* Append message `mid`'s list -- its whole state, read back from the tables --
 * if it has one and the message stands: one entry per OC_TODO_CHUNK tasks. One
 * function for the send, every change and every replay, as append_poll is. */
static void append_todo(sqlite3 *db, oc_dbres *r, uint64_t mid) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT t.channel_id, t.author_id, t.title, (SELECT COUNT(*) FROM todo_tasks k WHERE k.message_id=t.message_id) "
            "  FROM todo_lists t JOIN messages m ON m.id = t.message_id "
            " WHERE t.message_id=?1 AND m.deleted_at_ms IS NULL;", -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)mid);
    if (sqlite3_step(st) != SQLITE_ROW) { sqlite3_finalize(st); return; }
    uint64_t ch = (uint64_t)sqlite3_column_int64(st, 0), au = (uint64_t)sqlite3_column_int64(st, 1);
    const char *tt = (const char *)sqlite3_column_text(st, 2);
    char *title = strdup(tt ? tt : "");
    int total = sqlite3_column_int(st, 3);
    sqlite3_finalize(st);
    if (!title) return;
    if (total > (int)OC_TODO_MAX_TASKS) total = (int)OC_TODO_MAX_TASKS;
    size_t chunks = total ? ((size_t)total + OC_TODO_CHUNK - 1) / OC_TODO_CHUNK : 1;
    struct oc_replay_todo *g = realloc(r->rtodo, (r->n_rtodo + chunks) * sizeof *g);
    if (!g) { free(title); return; }
    r->rtodo = g;
    size_t base = r->n_rtodo;
    for (size_t c = 0; c < chunks; c++) {
        struct oc_replay_todo *e = &g[base + c];
        memset(e, 0, sizeof *e);
        e->message_id = mid; e->channel_id = ch; e->author_id = au;
        e->title = c == 0 ? title : strdup(title);
        e->total = (uint16_t)total;
        e->first = (uint16_t)(c * OC_TODO_CHUNK);
    }
    r->n_rtodo += chunks;
    if (sqlite3_prepare_v2(db,
            "SELECT idx, name, description, done, COALESCE(done_by,0), COALESCE(done_at_ms,0) "
            "  FROM todo_tasks WHERE message_id=?1 ORDER BY idx LIMIT ?2;", -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)mid);
    sqlite3_bind_int  (st, 2, total);
    int i = 0;
    while (i < total && sqlite3_step(st) == SQLITE_ROW) {
        struct oc_replay_todo *e = &g[base + (size_t)i / OC_TODO_CHUNK];
        const char *nm = (const char *)sqlite3_column_text(st, 1);
        const char *ds = (const char *)sqlite3_column_text(st, 2);
        e->tasks[e->n].name = strdup(nm ? nm : "");
        e->tasks[e->n].desc = strdup(ds ? ds : "");
        e->tasks[e->n].done = (uint8_t)(sqlite3_column_int(st, 3) != 0);
        e->tasks[e->n].done_by = (uint64_t)sqlite3_column_int64(st, 4);
        e->tasks[e->n].done_at_ms = (uint64_t)sqlite3_column_int64(st, 5);
        e->n++;
        i++;
    }
    sqlite3_finalize(st);
}

/* Lists for a replayed window. */
/* The call a call message is about (CALLS.md §1): a call event's own column,
 * read for those rows alone. */
static void fill_replay_calls(sqlite3 *db, oc_dbres *r) {
    sqlite3_stmt *st = NULL;
    for (size_t i = 0; i < r->n_replay; i++) {
        if (r->replay[i].kind != OC_MSG_KIND_CALL) continue;
        if (!st && sqlite3_prepare_v2(db, "SELECT COALESCE(call_id,0) FROM messages WHERE id=?1;",
                                      -1, &st, NULL) != SQLITE_OK) break;
        sqlite3_reset(st);
        sqlite3_bind_int64(st, 1, (sqlite3_int64)r->replay[i].message_id);
        if (sqlite3_step(st) == SQLITE_ROW) r->replay[i].call_id = (uint64_t)sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
}

static void fill_replay_todos(sqlite3 *db, oc_dbres *r) {
    for (size_t i = 0; i < r->n_replay; i++) append_todo(db, r, r->replay[i].message_id);
}

static void group_row_free(oc_group_row *g);

void oc_job_free(oc_job *j) { job_free(j); }

void oc_dbres_free(oc_dbres *r) {
    if (!r) return;
    free(r->dest_members);
    free(r->usage);
    free(r->mu_ids);
    free(r->bl_ids);
    free(r->blk_entries);
    free(r->alert_rows);
    /* The emoji catalogue (REQ-072): each name, and each old name. */
    for (size_t i = 0; i < r->n_elist; i++) {
        free(r->elist[i].name);
        for (uint8_t k = 0; k < r->elist[i].n_aliases; k++) free(r->elist[i].aliases[k]);
    }
    free(r->elist);
    free(r->body);
    free(r->pk_creds);
    /* 53's profile strings, alongside every other heap field. */
    free(r->st_emoji); free(r->st_text); free(r->pf_title); free(r->pf_tz);
    free(r->pf_full_name); free(r->pf_pronouns); free(r->pf_phone); free(r->pf_voice_id);
    free(r->pfd); free(r->pfv);
    free(r->pf_ooo_message);
    free(r->tts_text); free(r->tts_blob_key);
    free(r->exp_blob_key); free(r->exp_src_key);
    free(r->pv_key); free(r->pv_mime); free(r->pv_drop[0]); free(r->pv_drop[1]);
    for (size_t i = 0; i < r->n_ptokens; i++) {
        free(r->ptokens[i].name); free(r->ptokens[i].scopes); free(r->ptokens[i].last_used_from);
    }
    free(r->ptokens);
    free(r->tok_scopes);
    free(r->oa_client_id); free(r->oa_name); free(r->oa_uris); free(r->oa_request); free(r->oa_code);
    free(r->oa_redirect); free(r->oa_state); free(r->oa_error); free(r->oa_access); free(r->oa_refresh); free(r->via);
    for (size_t i = 0; i < r->n_oa_conns; i++) { free(r->oa_conns[i].user_name); free(r->oa_conns[i].client_name); }
    free(r->oa_conns);
    free(r->owh_url); free(r->owh_events);
    for (size_t i = 0; i < r->n_owh_rows; i++) {
        free(r->owh_rows[i].url); free(r->owh_rows[i].events); free(r->owh_rows[i].disabled_reason);
    }
    free(r->owh_rows);
    for (size_t i = 0; i < r->n_owh_due; i++) {
        free(r->owh_due[i].url); free(r->owh_due[i].event_id); free(r->owh_due[i].payload);
    }
    free(r->owh_due);
    for (size_t i = 0; i < r->n_mcp_chans; i++) free(r->mcp_chans[i].name);
    free(r->mcp_chans);
    for (size_t i = 0; i < r->n_mcp_rows; i++) {
        oc_mcp_row *m = &r->mcp_rows[i];
        free(m->author); free(m->handle); free(m->kind); free(m->text); free(m->channel);
        for (size_t k = 0; k < m->n_files; k++) { free(m->file_names[k]); free(m->file_types[k]); }
        free(m->file_names); free(m->file_types); free(m->file_sizes);
    }
    free(r->mcp_rows);
    free(r->sum_body); free(r->sum_version); free(r->sum_places); free(r->sum_notices);
    for (size_t i = 0; i < r->n_stt_names; i++) free(r->stt_names[i]);
    free(r->stt_names);
    free(r->call_uids);
    free(r->call_subject);
    for (size_t i = 0; i < r->n_recs; i++) free(r->rec_spools ? r->rec_spools[i] : NULL);
    free(r->rec_ids); free(r->rec_spools);
    free(r->rec_people);
    free(r->frame);
    free(r->fchans);
    for (size_t i = 0; i < r->n_sessions; i++) free((void *)r->sessions[i].device_label.ptr);
    free(r->sessions);
    free(r->members);
    free(r->member_participant);
    free(r->author_name);
    free_attach_meta(r->attach, r->n_attach);
    for (size_t i = 0; i < r->n_replay; i++) { free(r->replay[i].body); free(r->replay[i].author_name); free(r->replay[i].via); free_attach_meta(r->replay[i].attach, r->replay[i].n_attach); }
    free(r->replay);
    for (size_t i = 0; i < r->n_rreact; i++) free(r->rreact[i].emoji);
    free(r->rreact);
    for (size_t i = 0; i < r->n_runfurl; i++) {
        free(r->runfurl[i].url); free(r->runfurl[i].title); free(r->runfurl[i].descr);
    }
    free(r->runfurl);
    for (size_t i = 0; i < r->n_rfwd; i++) { free(r->rfwd[i].excerpt); free(r->rfwd[i].attach_name); }
    free(r->rfwd);
    for (size_t i = 0; i < r->n_ract; i++) free(r->ract[i].actor_name);
    free(r->ract);
    for (size_t i = 0; i < r->n_rpoll; i++)
        for (uint8_t k = 0; k < r->rpoll[i].n_options; k++) free(r->rpoll[i].labels[k]);
    free(r->rpoll);
    for (size_t i = 0; i < r->n_rtodo; i++) {
        free(r->rtodo[i].title);
        for (uint8_t k = 0; k < r->rtodo[i].n; k++) { free(r->rtodo[i].tasks[k].name); free(r->rtodo[i].tasks[k].desc); }
    }
    free(r->rtodo);
    free(r->pvoters);
    free(r->unf_url);
    free(r->unf_title);
    free(r->unf_descr);
    free(r->invite_email);
    for (size_t i = 0; i < r->n_plist; i++) { free(r->plist[i].body); free(r->plist[i].attach_name); }
    free(r->plist);
    free(r->cmlist);
    for (size_t i = 0; i < r->n_slist; i++) { free(r->slist[i].body); free(r->slist[i].attach_name); }
    free(r->slist);
    for (size_t i = 0; i < r->n_alist; i++) free(r->alist[i].text);
    free(r->alist);
    for (size_t i = 0; i < r->n_threads; i++) free(r->threads[i].preview);
    free(r->threads);
    for (size_t i = 0; i < r->n_flist; i++) { free(r->flist[i].filename); free(r->flist[i].mime); }
    free(r->flist);
    free(r->ch_name);
    free(r->ch_topic);
    free(r->ch_managers);
    for (size_t i = 0; i < r->n_groups; i++) group_row_free(&r->groups[i]);
    free(r->groups);
    for (size_t i = 0; i < r->n_mchan; i++) { free(r->mchan[i].name); free(r->mchan[i].topic); free(r->mchan[i].members); }
    free(r->mchan);
    free(r->mchg);
    for (size_t i = 0; i < r->n_chlist; i++) { free(r->chlist[i].name); free(r->chlist[i].topic); free(r->chlist[i].preview); }
    free(r->chlist);
    for (size_t i = 0; i < r->n_ulist; i++) {
        free(r->ulist[i].email); free(r->ulist[i].display_name);
        free(r->ulist[i].title); free(r->ulist[i].timezone);
        free(r->ulist[i].status_emoji); free(r->ulist[i].status_text);
        free(r->ulist[i].full_name); free(r->ulist[i].pronouns); free(r->ulist[i].voice_id);
        free(r->ulist[i].ooo_message);
    }
    free(r->ulist);
    free(r->emoji);
    free(r->react_adj_user);
    free(r->react_adj_minus);
    for (size_t i = 0; i < r->n_rlist; i++) free(r->rlist[i].emoji);
    free(r->rlist);
    for (size_t i = 0; i < r->n_thread; i++) { free(r->thread[i].body); free(r->thread[i].author_name); free(r->thread[i].via); free_attach_meta(r->thread[i].attach, r->thread[i].n_attach); }
    free(r->thread);
    for (size_t i = 0; i < r->n_search; i++) { free(r->search[i].body); free(r->search[i].author_name); free(r->search[i].via); free_attach_meta(r->search[i].attach, r->search[i].n_attach); }
    free(r->search);
    free(r->cert_pem);
    free(r->key_pem);
    if (r->tls_state) { oc_tls_state_free(r->tls_state); free(r->tls_state); }
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
    free(r->draft.body); free(r->draft.recipients);
    for (size_t i = 0; i < r->n_drafts; i++) { free(r->drafts[i].body); free(r->drafts[i].recipients); }
    free(r->drafts);
    free(r->sched.body); free(r->sched.fail_reason);
    for (size_t i = 0; i < r->n_scheds; i++) { free(r->scheds[i].body); free(r->scheds[i].fail_reason); }
    free(r->scheds);
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
/* The session policy (REQ-181), set before serving and read on the writer. */
static uint64_t g_session_ttl_ms = OC_SESSION_TTL_MS;
/* A demo workspace (oc_dbwriter_set_demo), read on the writer only. */
static int g_dbw_demo;
static uint64_t g_session_idle_ms;   /* 0: never refused for being idle */

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

static int membership_sync(sqlite3 *db, uint64_t channel_id, uint64_t user_id, int direct);
static void owh_member(sqlite3 *db, uint64_t channel_id, uint64_t uid, int joined);
static int owh_txn_begin(sqlite3 *db);
static void owh_txn_end(sqlite3 *db, int own);
static uint64_t *ids_of(sqlite3 *db, const char *sql, uint64_t a, size_t *n_out);

/* The public, unarchived channels OPENCHIME_DEFAULT_CHANNELS names, by name:
 * `user_id` joins each (a new member); with `report` set, each name that
 * matches none is said in the log instead (once, at start). */
static void join_default_channels(sqlite3 *db, uint64_t user_id, int report) {
    const char *list = oc_config_get()->default_channels;
    if (!list || !list[0]) return;
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", list);
    for (char *save = NULL, *nm = strtok_r(buf, ",", &save); nm; nm = strtok_r(NULL, ",", &save)) {
        while (*nm == ' ' || *nm == '#') nm++;
        size_t l = strlen(nm);
        while (l && nm[l - 1] == ' ') nm[--l] = '\0';
        if (!l) continue;
        sqlite3_stmt *st = NULL;
        uint64_t cid = 0;
        sqlite3_prepare_v2(db, "SELECT id FROM channels WHERE kind='channel' AND name=?1 AND is_public=1 "
                               "AND archived_at_ms IS NULL;", -1, &st, NULL);
        sqlite3_bind_text(st, 1, nm, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) cid = (uint64_t)sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
        if (report) {
            if (!cid) fprintf(stderr, "openchimed: OPENCHIME_DEFAULT_CHANNELS: no public channel named %s; it is skipped\n", nm);
        } else if (cid) {
            membership_sync(db, cid, user_id, 1);
        }
    }
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

    membership_sync(db, OC_DEFAULT_CHANNEL, user_id, 1);
}

/* At most `max` bytes of `s`, cut back to a UTF-8 boundary. */
static int utf8_prefix_len(const char *s, int max) {
    int n = (int)strlen(s);
    if (n <= max) return n;
    n = max;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
    return n;
}

/* A managed workspace's welcome in #general, written the one time the channel
 * is created: the existing topic and description columns, nothing authored and
 * nobody posing as the workspace. A #general that exists already -- made by an
 * earlier boot, or edited since -- is not touched. */
static oc_dbres *process_welcome_general(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->type = OC_RES_OK;
    const char *name = (j->ch_name && j->ch_name[0]) ? j->ch_name : NULL;
    int nl = name ? utf8_prefix_len(name, 64) : 0;
    char topic[OC_MAX_TOPIC + 1], descr[OC_MAX_DESCRIPTION + 1];
    if (name) {
        snprintf(topic, sizeof topic, "Everyone in %.*s is here. Say hello.", nl, name);
        snprintf(descr, sizeof descr,
                 "Welcome to %.*s. Everyone who joins the workspace is a member of #general, "
                 "so it is the place for news and questions meant for all. Invite people "
                 "from the workspace menu, and start a channel for each team or topic.",
                 nl, name);
    } else {
        snprintf(topic, sizeof topic, "Everyone in the workspace is here. Say hello.");
        snprintf(descr, sizeof descr,
                 "Welcome. Everyone who joins the workspace is a member of #general, so it "
                 "is the place for news and questions meant for all. Invite people from the "
                 "workspace menu, and start a channel for each team or topic.");
    }
    sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, NULL);
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "INSERT OR IGNORE INTO channels(id,kind,name,is_public,created_at_ms) "
        "VALUES(?, 'channel', 'general', 1, ?);", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, OC_DEFAULT_CHANNEL);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)dbw_now_ms());
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    int created = rc == SQLITE_DONE && sqlite3_changes(db) == 1;
    if (created) {
        sqlite3_prepare_v2(db, "UPDATE channels SET topic=?, description=? WHERE id=?;",
                           -1, &st, NULL);
        sqlite3_bind_text(st, 1, topic, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, descr, -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 3, OC_DEFAULT_CHANNEL);
        created = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
    }
    sqlite3_exec(db, created ? "COMMIT;" : "ROLLBACK;", NULL, NULL, NULL);
    r->channel_id = created ? OC_DEFAULT_CHANNEL : 0;
    return r;
}

/* True when adding a NEW user `subject` would exceed the cap (AUTH.md §2,
 * OPENCHIME_MANAGED_MAX_USERS). max_users <= 0 is unlimited; an already-existing subject is
 * never capped (idempotent bootstrap / re-login). Counts active people only — a
 * removed member (disabled=1) frees a seat, and a visitor, agent or bot never
 * takes one (SCHEMA.md §3bj). */
static int user_slots_full(sqlite3 *db, const char *subject, size_t sublen, int max_users) {
    if (max_users <= 0) return 0;
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "SELECT 1 FROM users WHERE subject=?;", -1, &st, NULL);
    sqlite3_bind_text(st, 1, subject, (int)sublen, SQLITE_TRANSIENT);
    int exists = (sqlite3_step(st) == SQLITE_ROW);
    sqlite3_finalize(st);
    if (exists) return 0;
    int count = 0;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM users WHERE disabled=0 AND is_system=0 AND kind='human';", -1, &st, NULL);
    if (sqlite3_step(st) == SQLITE_ROW) count = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return count >= max_users;
}

/* Create a local account: `local:<username>` user + Argon2id credential (AUTH.md
 * §2). Idempotent — INSERT OR IGNORE means re-running bootstrap never clobbers
 * an existing password. Returns the user id, or 0 on error. */
/* The next credential version (migration 0047): taken inside the caller's
 * transaction, so a write that rolls back gives it back. 0 on failure. */
static uint64_t next_credential_version(sqlite3 *db) {
    sqlite3_stmt *st = NULL;
    uint64_t v = 0;
    if (sqlite3_prepare_v2(db,
            "UPDATE credential_version_seq SET next = next + 1 WHERE id = 1 RETURNING next - 1;",
            -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
        v = (uint64_t)sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return v;
}

/* The account the job in hand created, or 0. Set by the two statements that
 * make a user row -- register_local and create_oidc_user, which every way in
 * goes through -- and read once the job is done, so whatever the job was, the
 * new member is announced to everyone connected (writer_loop). The writer's
 * own thread only. */
static __thread uint64_t g_joined_uid;

/* Notices (SCHEMA.md §3be): messages the workspace's own sender posts, each a
 * result of its own (OC_RES_NOTICE) chained onto the job's answer by the
 * writer loop, so connected members see it arrive. */
static __thread oc_dbres *g_notices;
/* The workspace's own sender, when this job made it: everyone connected is
 * told its roster row, so their clients know it before its first notice. */
static __thread uint64_t g_sender_made;
static uint64_t workspace_sender(sqlite3 *db);
static uint64_t notice_post(sqlite3 *db, uint64_t channel_id, const char *body, size_t len);
static void notice_channel(sqlite3 *db, uint64_t channel_id, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
static void notice_named(sqlite3 *db, const char *channel_name, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
static void notice_person(sqlite3 *db, uint64_t user_id, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static uint64_t register_local(sqlite3 *db, const char *username, size_t ulen,
                               const char *password, size_t plen,
                               uint8_t role, uint32_t mem_kib) {
    char subject[256];
    size_t sublen = local_subject(subject, sizeof subject, username, ulen);
    if (sublen == 0 || plen == 0) return 0;
    if (mem_kib == 0) mem_kib = OC_PW_MEM_KIB;

    uint8_t salt[OC_PW_SALT_LEN], hash[OC_PW_HASH_LEN];
    if (oc_rand_bytes(salt, sizeof salt) != 0) return 0;
    if (oc_pw_derive(password, plen, salt, sizeof salt, mem_kib, hash) != 0) return 0;

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
    int made = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db) > 0;
    sqlite3_finalize(st);

    uint64_t uid = 0;
    sqlite3_prepare_v2(db, "SELECT id FROM users WHERE subject=?;", -1, &st, NULL);
    sqlite3_bind_text(st, 1, subject, (int)sublen, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) uid = (uint64_t)sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    if (uid == 0) { sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL); return 0; }

    sqlite3_prepare_v2(db,
        "INSERT OR IGNORE INTO local_credentials(user_id,salt,kdf,hash,updated_at_ms,version) "
        "VALUES(?,?,?,?,?,?);", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)uid);
    sqlite3_bind_blob(st, 2, salt, sizeof salt, SQLITE_STATIC);
    char kdf[OC_PW_KDF_MAX];
    oc_pw_kdf_format(mem_kib, kdf);
    sqlite3_bind_text(st, 3, kdf, -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob(st, 4, hash, sizeof hash, SQLITE_STATIC);
    sqlite3_bind_int64(st, 5, (sqlite3_int64)now);
    uint64_t version = next_credential_version(db);
    sqlite3_bind_int64(st, 6, (sqlite3_int64)version);
    if (version == 0) {   /* 0 is what rows before the version carry: never given */
        sqlite3_finalize(st);
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        return 0;
    }
    sqlite3_step(st);
    sqlite3_finalize(st);

    sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
    ensure_default_membership(db, uid);
    if (made) g_joined_uid = uid;
    return uid;
}

/* A local account's stored credential and its version (migration 0047), for the
 * auth pool to check a password against. 1 found, 0 no such account (or no
 * password). */
static int local_credential(sqlite3 *db, const char *username, size_t ulen, uint64_t *uid,
                            uint8_t *salt, size_t salt_cap, size_t *slen, uint32_t *mem_kib,
                            uint8_t stored[OC_PW_HASH_LEN], uint64_t *version) {
    char subject[256];
    size_t sublen = local_subject(subject, sizeof subject, username, ulen);
    if (sublen == 0) return 0;
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "SELECT u.id, c.salt, c.kdf, c.hash, c.version FROM users u "
        "JOIN local_credentials c ON c.user_id = u.id WHERE u.subject = ?;", -1, &st, NULL);
    sqlite3_bind_text(st, 1, subject, (int)sublen, SQLITE_TRANSIENT);
    int found = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const void *sb = sqlite3_column_blob(st, 1);
        int sl = sqlite3_column_bytes(st, 1);
        const void *hb = sqlite3_column_blob(st, 3);
        int hl = sqlite3_column_bytes(st, 3);
        const char *kdf = (const char *)sqlite3_column_text(st, 2);
        /* Settings this daemon does not make (a hash from before Argon2id, or
         * other passes or lanes) are no credential: no password matches. */
        if (sb && hb && sl > 0 && (size_t)sl <= salt_cap && hl == (int)OC_PW_HASH_LEN &&
            oc_pw_kdf_parse(kdf, mem_kib) == 0) {
            *uid = (uint64_t)sqlite3_column_int64(st, 0);
            memcpy(salt, sb, (size_t)sl);
            *slen = (size_t)sl;
            memcpy(stored, hb, OC_PW_HASH_LEN);
            *version = (uint64_t)sqlite3_column_int64(st, 4);
            found = 1;
        }
    }
    sqlite3_finalize(st);
    return found;
}

/* Is `version` still user `uid`'s stored credential? A password changed, or the
 * account went, between the reader's fetch and now: then it is not. Versions
 * are never given twice (migration 0047), so a match is the same credential. */
static int credential_current(sqlite3 *db, uint64_t uid, uint64_t version, uint8_t *role_out) {
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "SELECT u.role FROM local_credentials c JOIN users u ON u.id = c.user_id "
        "WHERE c.user_id = ? AND c.version = ?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)uid);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)version);
    int same = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        same = 1;
        if (role_out) *role_out = role_to_u8((const char *)sqlite3_column_text(st, 0));
    }
    sqlite3_finalize(st);
    return same;
}

/* Mint a session: random 32-byte token to the caller, only its SHA-256 stored
 * (AUTH.md §4). Returns 0 and fills token/expiry, or -1. */
/* A member was active today (REQ-043's usage figures): one row a member a UTC
 * day, written the first time and ignored after. */
static void mark_active_day(sqlite3 *db, uint64_t user_id) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO user_active_days(user_id, day) VALUES(?, ?);",
                           -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)user_id);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)(dbw_now_ms() / 86400000ull));
    sqlite3_step(st);
    sqlite3_finalize(st);
}

/* `out_session_id` (optional) reports the row created, so a SESSION_LIST can mark
 * which entry is the connection asking (REQ-182). */
static int mint_session(sqlite3 *db, uint64_t user_id,
                        uint8_t token_out[OC_SESSION_TOKEN_LEN], uint64_t *expiry_out,
                        uint64_t *out_session_id) {
    uint8_t token[OC_SESSION_TOKEN_LEN], hash[OC_SHA256_LEN];
    if (oc_rand_bytes(token, sizeof token) != 0) return -1;
    if (oc_sha256(token, sizeof token, hash) != 0) return -1;

    uint64_t now = dbw_now_ms(), expiry = now + __atomic_load_n(&g_session_ttl_ms, __ATOMIC_RELAXED);
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
    if (rc == SQLITE_DONE && out_session_id)
        *out_session_id = (uint64_t)sqlite3_last_insert_rowid(db);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) return -1;
    mark_active_day(db, user_id);   /* signing in is using the workspace today */

    memcpy(token_out, token, sizeof token);
    if (expiry_out) *expiry_out = expiry;
    return 0;
}

/* Reconnect: hash the presented token, look up a live session, touch last_seen.
 * Returns user id + role + expiry, or 0 if unknown/expired (AUTH.md §4). */
static uint64_t lookup_session(sqlite3 *db, const uint8_t *token, size_t tlen,
                               uint8_t *role_out, uint64_t *expiry_out,
                               uint64_t *out_session_id) {
    if (tlen != OC_SESSION_TOKEN_LEN) return 0;
    uint8_t hash[OC_SHA256_LEN];
    if (oc_sha256(token, tlen, hash) != 0) return 0;

    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "SELECT s.user_id, s.expires_at_ms, u.role, s.id, COALESCE(s.last_seen_ms, s.created_at_ms) "
        "FROM sessions s JOIN users u ON u.id = s.user_id WHERE s.token_hash = ?;", -1, &st, NULL);
    sqlite3_bind_blob(st, 1, hash, sizeof hash, SQLITE_STATIC);
    uint64_t uid = 0, exp = 0, seen = 0; uint8_t role = OC_ROLE_MEMBER;
    if (sqlite3_step(st) == SQLITE_ROW) {
        uid  = (uint64_t)sqlite3_column_int64(st, 0);
        exp  = (uint64_t)sqlite3_column_int64(st, 1);
        role = role_to_u8((const char *)sqlite3_column_text(st, 2));
        seen = (uint64_t)sqlite3_column_int64(st, 4);
        /* The session's own id, so SESSION_LIST can mark which row is this device
         * (REQ-182). Reported through the out-param below rather than a global. */
        if (out_session_id) *out_session_id = (uint64_t)sqlite3_column_int64(st, 3);
    }
    sqlite3_finalize(st);
    if (uid == 0) return 0;
    if (exp != 0 && dbw_now_ms() >= exp) return 0;   /* expired */
    /* Unused for longer than the workspace allows (REQ-181): gone, as though
     * signed out. `last_seen_ms` moves while a session is in use (sessions_seen),
     * so this is idleness, not age. */
    uint64_t idle = __atomic_load_n(&g_session_idle_ms, __ATOMIC_RELAXED);
    if (idle && dbw_now_ms() - seen >= idle) {
        sqlite3_prepare_v2(db, "DELETE FROM sessions WHERE token_hash=?;", -1, &st, NULL);
        sqlite3_bind_blob(st, 1, hash, sizeof hash, SQLITE_STATIC);
        sqlite3_step(st);
        sqlite3_finalize(st);
        return 0;
    }

    sqlite3_prepare_v2(db, "UPDATE sessions SET last_seen_ms=? WHERE token_hash=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)dbw_now_ms());
    sqlite3_bind_blob(st, 2, hash, sizeof hash, SQLITE_STATIC);
    sqlite3_step(st);
    sqlite3_finalize(st);
    mark_active_day(db, uid);       /* and so is coming back to a session */

    if (role_out) *role_out = role;
    if (expiry_out) *expiry_out = exp;
    return uid;
}

/* Look up a user's role; returns 1 and sets *role_out if the user exists. */
static int user_role(sqlite3 *db, uint64_t uid, uint8_t *role_out) {
    sqlite3_stmt *st = NULL;
    int found = 0;
    /* The workspace's own sender is nobody's to act on: as a target it is
     * unknown, so no role, removal, DM, invite or group reaches it. */
    sqlite3_prepare_v2(db, "SELECT role FROM users WHERE id=? AND is_system=0;", -1, &st, NULL);
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

/* --- Critical failures (REQ-263) -------------------------------------------- */

#define OC_ALERTS_KEEP 100   /* rows kept; the oldest that no longer hold go first */

/* How many entries no owner or admin has acknowledged, and how many still hold. */
static void alerts_counts(sqlite3 *db, uint32_t *unacked, uint32_t *current) {
    sqlite3_stmt *st = NULL;
    *unacked = *current = 0;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FILTER (WHERE acked_ms IS NULL),"
                               " COUNT(*) FILTER (WHERE current = 1) FROM alerts;", -1, &st, NULL) != SQLITE_OK) return;
    if (sqlite3_step(st) == SQLITE_ROW) {
        *unacked = (uint32_t)sqlite3_column_int(st, 0);
        *current = (uint32_t)sqlite3_column_int(st, 1);
    }
    sqlite3_finalize(st);
}

/* An owner's or admin's sign-in carries the counts, so the client can show them
 * at once; nobody else's does. */
/* A person's summary notices not yet seen, oldest first: told again at sign-in
 * (REQ-310, SUMMARIES.md §5). */
static void notices_on_auth(sqlite3 *db, uint64_t uid, oc_dbres *r) {
    sqlite3_stmt *st = NULL;
    size_t cap = 0;
    if (sqlite3_prepare_v2(db, "SELECT id, channel_id, scope, status, start_ms, end_ms, made_at_ms "
                               "FROM summary_notices WHERE user_id=?1 AND seen_at_ms IS NULL ORDER BY id;",
                           -1, &st, NULL) != SQLITE_OK) { sqlite3_finalize(st); return; }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)uid);
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (r->n_sum_notices == cap) {
            size_t nc = cap ? cap * 2 : 8;
            struct oc_sum_notice_row *nv = realloc(r->sum_notices, nc * sizeof *nv);
            if (!nv) break;
            r->sum_notices = nv;
            cap = nc;
        }
        struct oc_sum_notice_row *n = &r->sum_notices[r->n_sum_notices++];
        n->id = (uint64_t)sqlite3_column_int64(st, 0);
        n->channel_id = (uint64_t)sqlite3_column_int64(st, 1);
        n->scope = (uint8_t)sqlite3_column_int(st, 2);
        n->status = (uint8_t)sqlite3_column_int(st, 3);
        n->start_ms = (uint64_t)sqlite3_column_int64(st, 4);
        n->end_ms = (uint64_t)sqlite3_column_int64(st, 5);
        n->made_at_ms = (uint64_t)sqlite3_column_int64(st, 6);
    }
    sqlite3_finalize(st);
}

static void load_profile_fields(sqlite3 *db, oc_dbres *r);   /* with the profile, below */

static void alerts_on_auth(sqlite3 *db, oc_dbres *r) {
    if (!oc_role_can_manage_members(r->role)) return;
    r->alerts_admin = 1;
    alerts_counts(db, &r->alerts_unacked, &r->alerts_current);
}

/* The counts, and to whom: every owner and admin, by their role now -- the net
 * thread sends it to whichever of their connections are open. */
static oc_dbres *alerts_summary(sqlite3 *db) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->type = OC_RES_ALERTS_SUMMARY;
    r->alerts_admin = 1;
    alerts_counts(db, &r->alerts_unacked, &r->alerts_current);
    sqlite3_stmt *st = NULL;
    size_t cap = 0;
    sqlite3_prepare_v2(db, "SELECT id FROM users WHERE role IN ('owner','admin') AND disabled=0;", -1, &st, NULL);
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (r->n_members == cap) {
            size_t nc = cap ? cap * 2 : 8;
            uint64_t *m = realloc(r->members, nc * sizeof *m);
            if (!m) break;
            r->members = m; cap = nc;
        }
        r->members[r->n_members++] = (uint64_t)sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    return r;
}

/* At most `cap` bytes of `s`, cut before a UTF-8 continuation byte. */
static size_t utf8_cut(const char *s, size_t cap) {
    size_t n = strlen(s);
    if (n <= cap) return n;
    while (cap > 0 && ((unsigned char)s[cap] & 0xC0) == 0x80) cap--;
    return cap;
}

/* Raise: again on the entry that still holds for this key, else a new one; an
 * acknowledged entry that holds stays acknowledged -- the failure is known --
 * until it stops and comes back. Then the oldest that no longer hold go, down to
 * OC_ALERTS_KEEP. Clear: the entry stops holding; nothing to say if none did. */
static oc_dbres *process_alert(sqlite3 *db, const oc_job *j) {
    const char *key = j->alert_key ? j->alert_key : "";
    uint64_t now = dbw_now_ms();
    sqlite3_stmt *st = NULL;
    if (j->type == OC_JOB_ALERT_CLEAR) {
        sqlite3_prepare_v2(db, "UPDATE alerts SET current=0, cleared_ms=?2 WHERE key=?1 AND current=1;", -1, &st, NULL);
        sqlite3_bind_text(st, 1, key, (int)utf8_cut(key, OC_MAX_ALERT_KEY), SQLITE_STATIC);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)now);
        sqlite3_step(st);
        sqlite3_finalize(st);
        return sqlite3_changes(db) ? alerts_summary(db) : NULL;
    }
    const char *msg = j->alert_message ? j->alert_message : "";
    sqlite3_prepare_v2(db, "UPDATE alerts SET message=?2, last_ms=?3, count=count+1 WHERE key=?1 AND current=1;",
                       -1, &st, NULL);
    sqlite3_bind_text(st, 1, key, (int)utf8_cut(key, OC_MAX_ALERT_KEY), SQLITE_STATIC);
    sqlite3_bind_text(st, 2, msg, (int)utf8_cut(msg, OC_MAX_ALERT_MESSAGE), SQLITE_STATIC);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)now);
    sqlite3_step(st);
    sqlite3_finalize(st);
    if (sqlite3_changes(db) == 0) {
        sqlite3_prepare_v2(db, "INSERT INTO alerts(key, message, first_ms, last_ms) VALUES(?1, ?2, ?3, ?3);",
                           -1, &st, NULL);
        sqlite3_bind_text(st, 1, key, (int)utf8_cut(key, OC_MAX_ALERT_KEY), SQLITE_STATIC);
        sqlite3_bind_text(st, 2, msg, (int)utf8_cut(msg, OC_MAX_ALERT_MESSAGE), SQLITE_STATIC);
        sqlite3_bind_int64(st, 3, (sqlite3_int64)now);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    sqlite3_prepare_v2(db, "DELETE FROM alerts WHERE id IN (SELECT id FROM alerts WHERE current=0 "
                           "ORDER BY last_ms ASC, id ASC LIMIT max(0, (SELECT COUNT(*) FROM alerts) - ?1));",
                       -1, &st, NULL);
    sqlite3_bind_int(st, 1, OC_ALERTS_KEEP);
    sqlite3_step(st);
    sqlite3_finalize(st);
    return alerts_summary(db);
}

/* The entries, newest first, for an owner or admin -- the role read now, so a
 * demotion takes effect at once. Read. */
static oc_dbres *process_alerts_list(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    uint8_t role = OC_ROLE_MEMBER;
    if (!user_role(db, j->user_id, &role) || !oc_role_can_manage_members(role)) {
        r->type = OC_RES_ALERTS_ERR; r->err_code = OC_ERR_FORBIDDEN; return r;
    }
    r->type = OC_RES_ALERTS;
    r->alert_rows = calloc(OC_MAX_ALERTS, sizeof *r->alert_rows);
    if (!r->alert_rows) { r->type = OC_RES_ALERTS_ERR; r->err_code = OC_ERR_INTERNAL; return r; }
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "SELECT id, first_ms, last_ms, count, current, acked_ms IS NOT NULL, key, message "
                           "FROM alerts ORDER BY last_ms DESC, id DESC LIMIT ?1;", -1, &st, NULL);
    sqlite3_bind_int(st, 1, OC_MAX_ALERTS);
    while (sqlite3_step(st) == SQLITE_ROW && r->n_alert_rows < OC_MAX_ALERTS) {
        struct oc_alert_row *a = &r->alert_rows[r->n_alert_rows++];
        a->id = (uint64_t)sqlite3_column_int64(st, 0);
        a->first_ms = (uint64_t)sqlite3_column_int64(st, 1);
        a->last_ms = (uint64_t)sqlite3_column_int64(st, 2);
        a->count = (uint32_t)sqlite3_column_int(st, 3);
        a->current = (uint8_t)sqlite3_column_int(st, 4);
        a->acked = (uint8_t)sqlite3_column_int(st, 5);
        const char *k = (const char *)sqlite3_column_text(st, 6), *m = (const char *)sqlite3_column_text(st, 7);
        snprintf(a->key, sizeof a->key, "%.*s", (int)utf8_cut(k ? k : "", OC_MAX_ALERT_KEY), k ? k : "");
        snprintf(a->message, sizeof a->message, "%.*s", (int)utf8_cut(m ? m : "", OC_MAX_ALERT_MESSAGE), m ? m : "");
    }
    sqlite3_finalize(st);
    return r;
}

/* Acknowledged by an owner or admin: one entry, or every one (0). Audited, and
 * every owner and admin connected is told the new counts. Write. */
static oc_dbres *process_alert_ack(sqlite3 *db, const oc_job *j) {
    uint8_t role = OC_ROLE_MEMBER;
    if (!user_role(db, j->user_id, &role) || !oc_role_can_manage_members(role)) {
        oc_dbres *r = calloc(1, sizeof *r);
        if (!r) return NULL;
        r->conn_id = j->conn_id;
        r->type = OC_RES_ALERTS_ERR; r->err_code = OC_ERR_FORBIDDEN;
        return r;
    }
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "UPDATE alerts SET acked_ms=?1, acked_by=?2 "
                           "WHERE acked_ms IS NULL AND (?3 = 0 OR id = ?3);", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)dbw_now_ms());
    sqlite3_bind_int64(st, 2, (sqlite3_int64)j->user_id);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)j->alert_id);
    sqlite3_step(st);
    sqlite3_finalize(st);
    int n = sqlite3_changes(db);
    if (n) {
        char detail[48];
        snprintf(detail, sizeof detail, "entries=%d", n);
        audit_actor(db, OC_AUDIT_ADMIN, "alert.ack", j->user_id, j->alert_id, NULL, 1, detail);
    }
    return alerts_summary(db);
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

/* Is this user a web visitor (SCHEMA.md §3bj)? A visitor reaches nothing but
 * its own conversation: it is no DM's participant, no channel's invitee and no
 * group's member, and every refusal reads as an unknown user's does. */
static int user_not_person(sqlite3 *db, uint64_t uid);   /* an agent's, a bot's or a visitor's row */
static int user_kind_is(sqlite3 *db, uint64_t uid, const char *kind);
static int actor_manages(sqlite3 *db, uint64_t uid);
static int user_is_visitor(sqlite3 *db, uint64_t uid) {
    sqlite3_stmt *st = NULL;
    int visitor = 0;
    sqlite3_prepare_v2(db, "SELECT kind='visitor' FROM users WHERE id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)uid);
    if (sqlite3_step(st) == SQLITE_ROW) visitor = sqlite3_column_int(st, 0) != 0;
    sqlite3_finalize(st);
    return visitor;
}

/* Owners who can still sign in. A removed owner keeps the role on its row, and a
 * workspace whose only owners are removed has nobody to run it. */
static int count_active_owners(sqlite3 *db) {
    sqlite3_stmt *st = NULL;
    int n = 0;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM users WHERE role='owner' AND disabled=0;",
                       -1, &st, NULL);
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return n;
}

static void set_role(sqlite3 *db, uint64_t uid, uint8_t role) {
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "UPDATE users SET role=? WHERE id=?;", -1, &st, NULL);
    sqlite3_bind_text(st, 1, u8_to_role(role), -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)uid);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

/* A relay token is good once (AUTH.md §8.2). The ids of the tokens already
 * accepted are held in memory until they expire — a token lives at most
 * OC_JWT_MAX_LIFETIME_SECS, which is what keeps this small — and a full table
 * refuses rather than forgets: forgetting is what a replay would need. */
#define OC_SEEN_JTI_CAP 4096

struct oc_seen_jti {
    struct { char jti[OC_JWT_MAX_SHORT]; uint64_t exp; } slot[OC_SEEN_JTI_CAP];
};

/* 1 if `jti` was new and is now recorded; 0 if it was seen before, or if no
 * slot is free. `exp` and `now` are seconds. */
static int seen_jti_claim(struct oc_seen_jti *t, const char *jti, uint64_t exp, uint64_t now) {
    int free_slot = -1;
    for (int i = 0; i < OC_SEEN_JTI_CAP; i++) {
        if (t->slot[i].jti[0] == '\0' || t->slot[i].exp + 120u < now) {
            t->slot[i].jti[0] = '\0';
            if (free_slot < 0) free_slot = i;
        } else if (strcmp(t->slot[i].jti, jti) == 0) {
            return 0;
        }
    }
    if (free_slot < 0) return 0;
    snprintf(t->slot[free_slot].jti, sizeof t->slot[free_slot].jti, "%s", jti);
    t->slot[free_slot].exp = exp;
    return 1;
}

/* A person who signs in by OIDC is (upstream issuer, subject) — AUTH.md §8.4 —
 * and a token's `sub` spells the pair "<issuer>|<subject>". The issuer is a URL
 * and holds no bar, so the first bar is the seam. 0 unless both halves exist. */
static int split_identity(const char *sub, char *issuer, size_t icap,
                          const char **subject) {
    const char *bar = strchr(sub, '|');
    if (!bar || bar == sub || bar[1] == '\0') return 0;
    size_t n = (size_t)(bar - sub);
    if (n >= icap) return 0;
    memcpy(issuer, sub, n);
    issuer[n] = '\0';
    *subject = bar + 1;
    return 1;
}

/* The user this identity belongs to, or 0. */
static uint64_t identity_user(sqlite3 *db, const char *issuer, const char *subject) {
    sqlite3_stmt *st = NULL;
    uint64_t uid = 0;
    sqlite3_prepare_v2(db, "SELECT user_id FROM user_identities WHERE issuer=? AND subject=?;",
                       -1, &st, NULL);
    sqlite3_bind_text(st, 1, issuer, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, subject, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) uid = (uint64_t)sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return uid;
}

static uint64_t user_by_subject(sqlite3 *db, const char *subject) {
    sqlite3_stmt *st = NULL;
    uint64_t uid = 0;
    sqlite3_prepare_v2(db, "SELECT id FROM users WHERE subject=? AND is_system=0;", -1, &st, NULL);
    sqlite3_bind_text(st, 1, subject, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) uid = (uint64_t)sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return uid;
}

/* The one active user a verified address is (AUTH.md §8.4): someone with an
 * identity whose provider verified that same address, compared without case. 0 if
 * nobody -- or if the address is more than one person's, since it cannot then
 * say which -- and never a disabled or removed user: a new way of signing in does
 * not revive an account. */
static uint64_t user_by_verified_email(sqlite3 *db, const char *email) {
    sqlite3_stmt *st = NULL;
    uint64_t found = 0;
    int n = 0;
    sqlite3_prepare_v2(db,
        "SELECT DISTINCT i.user_id FROM user_identities i JOIN users u ON u.id = i.user_id "
        "WHERE i.email_verified = 1 AND lower(i.email) = lower(?1) AND u.disabled = 0;", -1, &st, NULL);
    sqlite3_bind_text(st, 1, email, -1, SQLITE_STATIC);
    while (sqlite3_step(st) == SQLITE_ROW && n < 2) { found = (uint64_t)sqlite3_column_int64(st, 0); n++; }
    sqlite3_finalize(st);
    return n == 1 ? found : 0;
}

/* Record what the provider said this time. The identity row is the only thing a
 * later sign-in touches: the display name and address on `users` are the
 * person's own once they exist. Creates the row when `uid` has none yet. */
static void identity_touch(sqlite3 *db, uint64_t uid, const char *issuer, const char *subject,
                           const oc_jwt_claims *c) {
    uint64_t now = dbw_now_ms();
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "INSERT INTO user_identities(user_id, issuer, subject, idp, tenant, email, "
        " email_verified, first_seen_ms, last_login_ms) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?8) "
        "ON CONFLICT(issuer, subject) DO UPDATE SET idp=excluded.idp, tenant=excluded.tenant, "
        " email=excluded.email, email_verified=excluded.email_verified, "
        " last_login_ms=excluded.last_login_ms;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)uid);
    sqlite3_bind_text(st, 2, issuer, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, subject, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 4, c->idp, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 5, c->tenant, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 6, c->email, -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 7, c->email_verified ? 1 : 0);
    sqlite3_bind_int64(st, 8, (sqlite3_int64)now);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

/* Just-in-time provision a person's first OIDC sign-in: the name and address
 * come from the token this once. `legacy` keeps `users.subject` unique and
 * readable for what still keys on it. Returns the user id, or 0. */
static uint64_t create_oidc_user(sqlite3 *db, const char *legacy,
                                 const char *email, const char *name) {
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "INSERT INTO users(subject, email, display_name, created_at_ms) VALUES(?,?,?,?);",
        -1, &st, NULL);
    sqlite3_bind_text(st, 1, legacy, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, email, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, name, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 4, (sqlite3_int64)dbw_now_ms());
    int ok = (sqlite3_step(st) == SQLITE_DONE);
    sqlite3_finalize(st);
    if (!ok) return 0;
    uint64_t uid = (uint64_t)sqlite3_last_insert_rowid(db);
    g_joined_uid = uid;
    return uid;
}

static oc_dbres *process_register(oc_dbwriter *w, const oc_job *j) {
    sqlite3 *db = w->db;
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    /* Registered-user cap (AUTH.md §2): refuse a new account at the workspace limit. */
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
        j->role, j->mem_kib ? j->mem_kib : w->pw_mem);
    if (uid == 0) { r->type = OC_RES_REGISTER_ERR; r->err_code = OC_ERR_INTERNAL; return r; }
    r->type = OC_RES_REGISTER_OK;
    r->user_id = uid;
    r->role = j->role;
    return r;
}

/* An unconsumed, unexpired invite bound to `email` (AUTH.md §8.4). 1 and the
 * role it grants, or 0. The newest wins when an address was invited twice. */
static int invite_for_email(sqlite3 *db, const char *email, uint8_t *role) {
    sqlite3_stmt *st = NULL;
    int found = 0;
    sqlite3_prepare_v2(db,
        "SELECT role FROM invites WHERE email = lower(?1) AND consumed_at_ms IS NULL "
        "AND expires_at_ms > ?2 ORDER BY expires_at_ms DESC LIMIT 1;", -1, &st, NULL);
    sqlite3_bind_text(st, 1, email, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)dbw_now_ms());
    if (sqlite3_step(st) == SQLITE_ROW) {
        *role = role_to_u8((const char *)sqlite3_column_text(st, 0));
        found = 1;
    }
    sqlite3_finalize(st);
    return found;
}

/* Every open invite for the address is spent at once: the person has joined. */
static void consume_email_invite(sqlite3 *db, const char *email) {
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "UPDATE invites SET consumed_at_ms=?2 WHERE email = lower(?1) AND consumed_at_ms IS NULL;",
        -1, &st, NULL);
    sqlite3_bind_text(st, 1, email, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)dbw_now_ms());
    sqlite3_step(st);
    sqlite3_finalize(st);
}

static const char *jwt_reason(oc_jwt_result jr) {
    switch (jr) {
    case OC_JWT_E_ALG:       return "alg";
    case OC_JWT_E_SIGNATURE: return "signature";
    case OC_JWT_E_CLAIMS:    return "claims";
    case OC_JWT_E_EXPIRED:   return "expired";
    case OC_JWT_E_KEY:       return "key";
    default:                 return "format";
    }
}

/* The sign-in limiter counts a source by oc_source_key -- an IPv6 client by its
 * /64 -- while the audit keeps the address itself. One writer thread, so one
 * static buffer is enough. */
static const char *src_key(const char *source) {
    static char k[OC_SRC_LEN];
    oc_source_key(source, k, sizeof k);
    return k;
}

/* A refused OIDC sign-in: counted against its source and written down, exactly
 * as a wrong password is (REQ-191, REQ-251). Never the token — only why it was
 * refused and where it came from. Bounded by the limiter: a throttled attempt
 * returned before it got here. */
static oc_dbres *oidc_refuse(oc_dbwriter *w, const oc_job *j, oc_dbres *r, uint16_t code,
                             const char *action, const char *who, const char *reason) {
    char detail[OC_JWT_MAX_FIELD + OC_JWT_MAX_SHORT + 96];
    if (j->source[0]) oc_ratelimit_record(w->source_rl, src_key(j->source), dbw_now_ms());
    snprintf(detail, sizeof detail, "%s%s%s", reason, j->source[0] ? " from=" : "", j->source);
    audit_log(w->db, OC_AUDIT_SECURITY, action, 0, NULL, 0, who, 0, detail);
    r->type = OC_RES_AUTH_ERR;
    r->err_code = code;
    return r;
}

/* Prove identity (local password, an OIDC ES256 JWT, or an existing session
 * token) and converge on a daemon-issued session (AUTH.md §4). */
static void submit_to_reader(oc_dbwriter *w, oc_job *j);

static oc_dbres *web_finish(oc_dbwriter *w, const oc_job *j, oc_dbres *r, uint64_t uid);

/* An OIDC identity whose token has been checked, from the relay (`src`
 * "relay") or a direct connection ("direct"), admitted (AUTH.md §8.4): found
 * by its identity -- or by an older account's subject, or linked by a verified
 * address -- or created if the join rules or an invitation admit it. NULL with
 * `*uid_out`/`*role_out` set on success (uid 0 is a refusal the caller turns
 * into one), or `r` filled as the refusal. */
static oc_dbres *oidc_admit(oc_dbwriter *w, const oc_job *j, oc_dbres *r, const oc_jwt_claims *claims,
                            const char *src, uint64_t *uid_out, uint8_t *role_out, char *how, size_t howcap) {
    sqlite3 *db = w->db;
    int direct = strcmp(src, "direct") == 0;
    /* Namespace by source: "oidc:<central issuer>|<provider sub>" (AUTH.md §4). */
    char subject[OC_JWT_MAX_FIELD * 2 + 8];
    snprintf(subject, sizeof subject, "oidc:%s|%s", claims->iss, claims->sub);
    /* Who may join (AUTH.md §8.4). The rules speak only to an identity this
     * workspace has not seen — a known one signs in without them — except the
     * owner rule, which also restores an owner to a workspace left with none. */
    oc_join_verdict verdict = oc_join_rules_eval(w->join_rules, claims->sub, claims->idp, claims->tenant,
                                                 claims->email, claims->email_verified);
    char issuer[OC_JWT_MAX_FIELD];
    const char *person = NULL;
    if (!split_identity(claims->sub, issuer, sizeof issuer, &person)) {
        char why[48];
        snprintf(why, sizeof why, "source=%s reason=subject", src);
        return oidc_refuse(w, j, r, OC_ERR_AUTH_INVALID_TOKEN, "auth.failed", claims->email, why);
    }
    /* A provider this deployment connects to directly speaks for its people
     * itself: the relay does not deliver its identities (AUTH.md §8.4). */
    if (!direct)
        for (int i = 0; i < w->n_direct_iss; i++)
            if (strcmp(issuer, w->direct_iss[i]) == 0)
                return oidc_refuse(w, j, r, OC_ERR_AUTH_NOT_ALLOWED, "auth.denied", claims->email,
                                   "source=relay reason=direct-issuer");
    /* An account from before identities were rows is found by the string it was
     * filed under, and gets its row at this sign-in. */
    uint64_t uid = identity_user(db, issuer, person);
    if (!uid) uid = user_by_subject(db, subject);
    /* A new way in for someone already here: an identity not seen before,
     * whose provider verified an address a member's provider verified too, is
     * that member -- Google one day, an emailed code the next, one account.
     * Nothing below refuses what the checks above let through, so this comes
     * after them: it never admits an identity the relay may not deliver. */
    int linked = 0;
    /* A direct connection is the operator's own and links nobody by address:
     * it and the relay are never one person on an email's word. */
    if (!uid && !direct && claims->email_verified && claims->email[0])
        linked = (uid = user_by_verified_email(db, claims->email)) != 0;
    if (linked && w->n_direct_iss) {
        /* Nor the other way: a relay sign-in links to no one a direct
         * connection knows. */
        sqlite3_stmt *st = NULL;
        sqlite3_prepare_v2(db, "SELECT issuer FROM user_identities WHERE user_id=?;", -1, &st, NULL);
        sqlite3_bind_int64(st, 1, (sqlite3_int64)uid);
        while (linked && sqlite3_step(st) == SQLITE_ROW) {
            const char *iss = (const char *)sqlite3_column_text(st, 0);
            for (int i = 0; iss && i < w->n_direct_iss; i++)
                if (strcmp(iss, w->direct_iss[i]) == 0) { linked = 0; uid = 0; }
        }
        sqlite3_finalize(st);
    }
    /* ...but never downward: an emailed code proves only the mailbox, so it
     * does not stand in for a provider -- and its second factor -- the person
     * already signs in with, unless the workspace allows it. An emailed code
     * for an address is always the same identity, so one that would link is
     * one onto a provider's. The other way, a provider for someone known by
     * emailed code, is the stronger proof and links. */
    if (linked && strcmp(claims->idp, "email") == 0 && !w->email_link_any) {
        char why[48];
        snprintf(why, sizeof why, "source=%s idp=email reason=downgrade", src);
        return oidc_refuse(w, j, r, OC_ERR_AUTH_USE_PROVIDER, "auth.denied", claims->email, why);
    }
    int known = uid != 0;
    /* An invite bound to this verified address admits it where no rule does,
     * and says what role it joins with. */
    uint8_t invited_role = OC_ROLE_MEMBER;
    int invited = 0;
    if (!known && verdict == OC_JOIN_DENY && claims->email_verified && claims->email[0])
        invited = invite_for_email(db, claims->email, &invited_role);
    if (invited) verdict = OC_JOIN_MEMBER;
    if (!known && verdict == OC_JOIN_DENY) {
        char detail[OC_JWT_MAX_FIELD + OC_JWT_MAX_SHORT + 48];
        snprintf(detail, sizeof detail, "source=%s idp=%s tenant=%s", src, claims->idp, claims->tenant);
        return oidc_refuse(w, j, r, OC_ERR_AUTH_NOT_ALLOWED, "auth.denied", claims->email, detail);
    }
    snprintf(how, howcap, "source=%s idp=%s", src, claims->idp);
    /* Registered-user cap (AUTH.md §2): a first-time OIDC login can't provision a new
     * user past the workspace limit (an existing user still logs in). */
    if (!known && user_slots_full(db, subject, strlen(subject), w->max_users)) {
        r->type = OC_RES_AUTH_ERR; r->err_code = OC_ERR_USER_LIMIT; return r;
    }
    if (!known) uid = create_oidc_user(db, subject, claims->email, claims->name);
    if (uid) identity_touch(db, uid, issuer, person, claims);
    if (linked) {
        char detail[OC_JWT_MAX_FIELD + OC_JWT_MAX_SHORT + 48];
        snprintf(detail, sizeof detail, "source=%s idp=%s tenant=%s", src, claims->idp, claims->tenant);
        audit_actor(db, OC_AUDIT_SECURITY, "auth.subject_linked", uid, uid, NULL, 1, detail);
    }
    if (uid && invited) {
        consume_email_invite(db, claims->email);
        if (invited_role != OC_ROLE_MEMBER) set_role(db, uid, invited_role);
    }
    if (uid && verdict == OC_JOIN_OWNER && !user_disabled(db, uid) &&
        (!known || count_active_owners(db) == 0))
        set_role(db, uid, OC_ROLE_OWNER);
    *uid_out = uid;
    if (uid) *role_out = get_role(db, uid);   /* membership ensured on the common path */
    return NULL;
}

static oc_dbres *process_auth_job(oc_dbwriter *w, const oc_job *j) {
    sqlite3 *db = w->db;
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;

    uint64_t uid = 0, sess_exp = 0, sess_id = 0;
    uint8_t role = OC_ROLE_MEMBER;
    int fresh = 1;   /* mint a new session unless this is a session re-auth */
    char how[OC_JWT_MAX_SHORT + 32] = "source=local";   /* what auth.success records */

    if (j->direct) {
        /* A direct connection (AUTH.md §8.5): its worker exchanged the code and
         * checked the provider's token; what is left is who may join. */
        const oc_direct_auth *d = j->direct;
        if (d->err) {
            char why[96];
            snprintf(why, sizeof why, "source=direct reason=%s", d->reason[0] ? d->reason : "exchange");
            return oidc_refuse(w, j, r, d->err, "auth.failed", d->email[0] ? d->email : NULL, why);
        }
        oc_jwt_claims claims;
        memset(&claims, 0, sizeof claims);
        if (strlen(d->sub) >= sizeof claims.sub)
            return oidc_refuse(w, j, r, OC_ERR_AUTH_INVALID_TOKEN, "auth.failed", NULL, "source=direct reason=subject");
        snprintf(claims.iss, sizeof claims.iss, "%s", d->iss);
        memcpy(claims.sub, d->sub, strlen(d->sub) + 1);   /* checked to fit above */
        snprintf(claims.email, sizeof claims.email, "%s", d->email);
        snprintf(claims.name, sizeof claims.name, "%s", d->name);
        snprintf(claims.tenant, sizeof claims.tenant, "%s", d->tenant);
        snprintf(claims.idp, sizeof claims.idp, "%s", d->idp);
        claims.email_verified = d->email_verified;
        oc_dbres *refused = oidc_admit(w, j, r, &claims, "direct", &uid, &role, how, sizeof how);
        if (refused) return refused;
    } else if (j->method == OC_AUTH_LOCAL) {
        if (!(w->auth_methods & OC_AUTH_LOCAL)) {
            r->type = OC_RES_AUTH_ERR; r->err_code = OC_ERR_AUTH_REQUIRED; return r;
        }
        /* A password is typed into the daemon's own pages, never sent in a
         * frame (AUTH.md §8.10): the net loop refuses the frame form, so a local
         * job is always a page's. */
        if (!j->web) {
            r->type = OC_RES_AUTH_ERR; r->err_code = OC_ERR_AUTH_SOURCE_UNAVAILABLE; return r;
        }
        oc_slice cred = { (const uint8_t *)j->token, j->token_len };
        oc_slice user, pass;
        if (oc_parse_local_credential(cred, &user, &pass) != OC_OK) {
            r->type = OC_RES_AUTH_ERR; r->err_code = OC_ERR_AUTH_INVALID_TOKEN; return r;
        }
        /* Throttle brute force per account and per source IP (REQ-191). Check
         * before the expensive Argon2id so a flood can't also burn CPU. */
        char acct[OC_RL_KEYMAX];
        size_t al = user.len < sizeof acct - 1 ? user.len : sizeof acct - 1;
        memcpy(acct, user.ptr, al);
        acct[al] = '\0';
        int has_src = j->source[0] != '\0';
        uint64_t now = dbw_now_ms();
        if (j->auth_stage == OC_AUTH_STAGE_NEW && (oc_ratelimit_blocked(w->auth_rl, acct, now) ||
            (has_src && oc_ratelimit_blocked(w->source_rl, src_key(j->source), now)))) {
            /* Throttled attempts are dropped SILENTLY and are deliberately not
             * audited (REQ-251b). Logging them would hand an attacker the very
             * amplification the limiter exists to remove: one packet, one row.
             * The failures that got far enough to be checked are logged below,
             * and the limiter caps those at 5/min/account and 20/min/source. */
            r->type = OC_RES_AUTH_ERR; r->err_code = OC_ERR_AUTH_RATE_LIMITED; return r;
        }
        if (j->auth_stage == OC_AUTH_STAGE_NEW) {
            /* On to a reader for the credential, then the pool for the
             * derivation; the same job comes back here, CHECKED. */
            ((oc_job *)j)->auth_stage = OC_AUTH_STAGE_READ;
            submit_to_reader(w, (oc_job *)j);
            w->auth_deferred = 1;
            free(r);
            return NULL;
        }
        /* Back from the pool: the password matched the credential it was
         * checked against, and that is still the stored one. */
        uid = j->auth_ok == 1 && credential_current(db, j->auth_uid, j->auth_version, &role) ? j->auth_uid : 0;
        if (uid == 0) {
            oc_ratelimit_record(w->auth_rl, acct, now);
            if (has_src) oc_ratelimit_record(w->source_rl, src_key(j->source), now);
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
        return web_finish(w, j, r, uid);
    } else if (j->method == OC_AUTH_OIDC && strcmp(j->auth_source, OC_SOURCE_ID_LOCAL) == 0) {
        /* The token this daemon's own pages issued (AUTH.md §8.10): its key, its
         * issuer, bound to this client's verifier, used once, naming a user by
         * id -- no join rules, nothing created. */
        if (!(w->auth_methods & OC_AUTH_LOCAL) || !w->local_iss || !w->seen_jti) {
            r->type = OC_RES_AUTH_ERR; r->err_code = OC_ERR_AUTH_REQUIRED; return r;
        }
        if (j->source[0] && oc_ratelimit_blocked(w->source_rl, src_key(j->source), dbw_now_ms())) {
            r->type = OC_RES_AUTH_ERR; r->err_code = OC_ERR_AUTH_RATE_LIMITED; return r;
        }
        const char *pem = oc_local_issuer_pubkey_pem(w->local_iss);
        oc_jwt_claims claims;
        oc_jwt_result jr = oc_jwt_verify(j->token, j->token_len, pem, strlen(pem) + 1,
                                         oc_local_issuer_name(w->local_iss), OC_LOCAL_AUDIENCE,
                                         dbw_now_ms() / 1000u, &claims);
        if (jr != OC_JWT_OK) {
            char reason[48];
            snprintf(reason, sizeof reason, "source=local reason=%s", jwt_reason(jr));
            return oidc_refuse(w, j, r, OC_ERR_AUTH_INVALID_TOKEN, "auth.failed", NULL, reason);
        }
        if (!oc_jwt_nonce_matches(claims.nonce, (const uint8_t *)j->proof, j->proof_len))
            return oidc_refuse(w, j, r, OC_ERR_AUTH_INVALID_TOKEN, "auth.failed", NULL,
                               "source=local reason=verifier");
        if (!seen_jti_claim(w->seen_jti, claims.jti, claims.exp, dbw_now_ms() / 1000u))
            return oidc_refuse(w, j, r, OC_ERR_AUTH_INVALID_TOKEN, "auth.failed", NULL,
                               "source=local reason=replayed");
        uid = oc_local_subject_uid(claims.sub);
        if (!uid) return oidc_refuse(w, j, r, OC_ERR_AUTH_INVALID_TOKEN, "auth.failed", NULL,
                                     "source=local reason=subject");
        role = get_role(db, uid);
        snprintf(how, sizeof how, "source=local via=browser");
    } else if (j->method == OC_AUTH_OIDC) {
        if (!w->oidc_enabled) {
            r->type = OC_RES_AUTH_ERR; r->err_code = OC_ERR_AUTH_REQUIRED; return r;
        }
        /* The per-source limiter stands in front of every source, and in front of
         * the signature work as it stands in front of Argon2id. Throttled attempts
         * are dropped silently and unaudited, for the reason given above. */
        if (j->source[0] && oc_ratelimit_blocked(w->source_rl, src_key(j->source), dbw_now_ms())) {
            r->type = OC_RES_AUTH_ERR; r->err_code = OC_ERR_AUTH_RATE_LIMITED; return r;
        }
        oc_jwt_claims claims;
        pthread_mutex_lock(&w->keys_mu);
        oc_jwt_result jr = oc_jwt_verify(j->token, j->token_len,
                                         w->oidc_keyset_pem, strlen(w->oidc_keyset_pem) + 1,
                                         w->oidc_issuer, w->oidc_audience,
                                         dbw_now_ms() / 1000u, &claims);
        pthread_mutex_unlock(&w->keys_mu);
        if (jr != OC_JWT_OK) {
            char reason[48];
            snprintf(reason, sizeof reason, "source=relay reason=%s", jwt_reason(jr));
            return oidc_refuse(w, j, r, OC_ERR_AUTH_INVALID_TOKEN, "auth.failed", NULL, reason);
        }
        /* Proof of possession (AUTH.md §8.2): the token is good only in the hands
         * of the client that asked for it, which kept the verifier its nonce is
         * the hash of. */
        if (!oc_jwt_nonce_matches(claims.nonce, (const uint8_t *)j->proof, j->proof_len)) {
            return oidc_refuse(w, j, r, OC_ERR_AUTH_INVALID_TOKEN, "auth.failed", claims.email,
                               "source=relay reason=verifier");
        }
        /* Single use, claimed only once everything else about the token is good, so
         * a token that fails for another reason does not burn its id. */
        if (!w->seen_jti ||
            !seen_jti_claim(w->seen_jti, claims.jti, claims.exp, dbw_now_ms() / 1000u)) {
            return oidc_refuse(w, j, r, OC_ERR_AUTH_INVALID_TOKEN, "auth.failed", claims.email,
                               "source=relay reason=replayed");
        }
        oc_dbres *refused = oidc_admit(w, j, r, &claims, "relay", &uid, &role, how, sizeof how);
        if (refused) return refused;
    } else if (j->method == OC_AUTH_SESSION) {
        /* A session token is 32 random bytes, so guessing one is hopeless — but the
         * limiter is per source, and a source hammering any door is the same source. */
        if (j->source[0] && oc_ratelimit_blocked(w->source_rl, src_key(j->source), dbw_now_ms())) {
            r->type = OC_RES_AUTH_ERR; r->err_code = OC_ERR_AUTH_RATE_LIMITED; return r;
        }
        uid = lookup_session(db, (const uint8_t *)j->token, j->token_len, &role, &sess_exp,
                             &sess_id);
        fresh = 0;
        if (uid == 0 && j->source[0]) oc_ratelimit_record(w->source_rl, src_key(j->source), dbw_now_ms());
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
    r->session_id = sess_id;   /* REQ-182: which row this connection is using */
    alerts_on_auth(db, r);
    notices_on_auth(db, uid, r);
    load_profile_fields(db, r);   /* the workspace's own fields, told at sign-in */
    /* Do-not-disturb rides along, BOTH halves: the net thread keeps the FACT in
     * memory so presence fan-out can carry it, and it has no database of its
     * own. The pause is an instant (REQ-278); the schedule is a rule the net
     * thread evaluates per tick (REQ-136). Seeding the schedule here rather
     * than waiting for the client to ask for its preferences means a colleague
     * inside their quiet hours reads as such from the first frame. */
    r->snooze_until_ms = snooze_until(db, uid);
    fill_schedule(db, uid, r);
    if (fresh) {
        uint8_t token[OC_SESSION_TOKEN_LEN]; uint64_t expiry = 0;
        if (mint_session(db, uid, token, &expiry, &sess_id) != 0) {
            r->type = OC_RES_AUTH_ERR; r->err_code = OC_ERR_INTERNAL; return r;
        }
        memcpy(r->session_token, token, sizeof token);
        r->session_id = sess_id;    /* the row just made, not the none above */
        r->has_session_token = 1;
        r->session_expiry = expiry;
        /* Only a fresh local/OIDC login, never a session reconnect (REQ-251):
         * "who signed in, when" means a new credential was proven, not that an
         * already-authenticated connection resumed. */
        audit_actor(db, OC_AUDIT_SECURITY, "auth.success", uid, 0, NULL, 1, how);
    } else {
        r->has_session_token = 0;   /* no new token on reconnect (PROTOCOL.md §4.3) */
        r->session_expiry = sess_exp;
    }
    return r;
}

/* A password check from the daemon's pages is done (AUTH.md §8.10): a sign-in
 * ends with an ID token for the client that sent the person there; a password
 * change stores the new password, derived on the pool, against the version
 * that was checked. A removed member is refused either way, as an invalid
 * credential. */
static int store_new_password(sqlite3 *db, const oc_job *j, uint64_t uid, uint64_t keep_session,
                              uint16_t *err);

/* Whether `uid` has a second step set up (AUTH.md §8.6). */
static int has_second_step(sqlite3 *db, uint64_t uid) {
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "SELECT 1 FROM local_totp WHERE user_id=? AND confirmed_at_ms IS NOT NULL;",
                       -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)uid);
    int yes = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    return yes;
}

/* `uid`'s passkeys on relying party `rp`: their ids, base64url, comma-separated
 * (heap), or NULL for none. */
static char *passkeys_for(sqlite3 *db, uint64_t uid, const char *rp) {
    if (!rp || !rp[0]) return NULL;
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "SELECT cred_id FROM webauthn_credentials WHERE user_id=? AND rp_id=? ORDER BY id LIMIT 16;",
                       -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)uid);
    sqlite3_bind_text(st, 2, rp, -1, SQLITE_TRANSIENT);
    char *out = NULL;
    size_t len = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        int n = sqlite3_column_bytes(st, 0);
        if (n <= 0 || n > (int)OC_WA_CRED_MAX) continue;
        char enc[4 * ((OC_WA_CRED_MAX + 2) / 3) + 2];
        size_t el = oc_base64url_encode(sqlite3_column_blob(st, 0), (size_t)n, enc);
        char *g = realloc(out, len + el + 2);
        if (!g) break;
        out = g;
        if (len) out[len++] = ',';
        memcpy(out + len, enc, el);
        len += el;
        out[len] = '\0';
    }
    sqlite3_finalize(st);
    return out;
}

/* A passkey's answer for `uid` (AUTH.md §8.6), on the relying party, challenge
 * and origin its ticket named: 1 and its counter moved on, or 0. */
static int check_passkey(sqlite3 *db, uint64_t uid, const oc_job *j) {
    uint8_t id[OC_WA_CRED_MAX], cd[2048], ad[1024], sig[600];
    long il = j->wa_cred ? oc_base64url_decode(j->wa_cred, strlen(j->wa_cred), id, sizeof id) : -1;
    long cl = j->wa_cd ? oc_base64url_decode(j->wa_cd, strlen(j->wa_cd), cd, sizeof cd) : -1;
    long al = j->wa_ad ? oc_base64url_decode(j->wa_ad, strlen(j->wa_ad), ad, sizeof ad) : -1;
    long sl = j->wa_sig ? oc_base64url_decode(j->wa_sig, strlen(j->wa_sig), sig, sizeof sig) : -1;
    if (il <= 0 || cl <= 0 || al <= 0 || sl <= 0 || !j->wa_rp[0] || !j->wa_challenge[0]) return 0;
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "SELECT id, cose_key, sign_count FROM webauthn_credentials "
                           "WHERE user_id=? AND cred_id=? AND rp_id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)uid);
    sqlite3_bind_blob(st, 2, id, (int)il, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, j->wa_rp, -1, SQLITE_TRANSIENT);
    int ok = 0;
    int64_t row = 0;
    uint32_t count = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        row = sqlite3_column_int64(st, 0);
        ok = oc_webauthn_assert(cd, (size_t)cl, ad, (size_t)al, sig, (size_t)sl,
                                sqlite3_column_blob(st, 1), (size_t)sqlite3_column_bytes(st, 1),
                                (uint32_t)sqlite3_column_int64(st, 2), j->wa_challenge, j->wa_origin, j->wa_rp,
                                &count) == OC_WA_OK;
    }
    sqlite3_finalize(st);
    if (!ok) return 0;
    sqlite3_prepare_v2(db, "UPDATE webauthn_credentials SET sign_count=?, last_used_at_ms=? WHERE id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)count);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)dbw_now_ms());
    sqlite3_bind_int64(st, 3, row);
    sqlite3_step(st);
    sqlite3_finalize(st);
    return 1;
}

/* What a page's password check ends in. `stepped`: the second step has been
 * passed too, or the account has none; otherwise an account with one is asked
 * for it (WEB_STEP2) and nothing is minted or stored yet. */
static oc_dbres *security_begin(oc_dbwriter *w, const oc_job *j, oc_dbres *r, uint64_t uid);

static oc_dbres *web_finish_as(oc_dbwriter *w, const oc_job *j, oc_dbres *r, uint64_t uid, int stepped) {
    sqlite3 *db = w->db;
    if (uid && user_disabled(db, uid)) uid = 0;
    if (!uid) { r->type = OC_RES_WEB_ERR; r->err_code = OC_ERR_AUTH_INVALID_TOKEN; return r; }
    if (j->web == OC_WEB_SECURITY) return security_begin(w, j, r, uid);
    int has_step = w->local_mfa != OC_MFA_OFF && has_second_step(db, uid);
    /* Required and not set up: no sign-in until it is -- the password is right,
     * so the page can say where to set it up. */
    if (!stepped && !has_step && w->local_mfa == OC_MFA_REQUIRED && j->web == OC_WEB_SIGNIN) {
        r->type = OC_RES_WEB_ERR; r->err_code = OC_ERR_AUTH_SETUP_REQUIRED; return r;
    }
    if (!stepped && j->web != OC_WEB_SIGNUP && has_step) {
        r->type = OC_RES_WEB_STEP2;
        r->user_id = uid;
        r->step_version = j->auth_version;
        r->pk_creds = passkeys_for(db, uid, j->wa_rp);   /* offered on its trusted name only */
        if (j->web == OC_WEB_PASSWORD && j->pw_derived) {
            r->step_pw = 1;
            r->step_mem = j->pw_mem;
            memcpy(r->step_salt, j->pw_salt, sizeof r->step_salt);
            memcpy(r->step_hash, j->pw_hash, sizeof r->step_hash);
        }
        return r;
    }
    if (j->web == OC_WEB_PASSWORD) {
        uint16_t err = 0;
        if (store_new_password(db, j, uid, 0, &err) != 0) { r->type = OC_RES_WEB_ERR; r->err_code = err; return r; }
        audit_actor(db, OC_AUDIT_ACCOUNT, "password.change", uid, 0, NULL, 1, "via=browser");
        r->type = OC_RES_WEB_OK;
        r->user_id = uid;
        r->revoked = 1;   /* the page is no session: every device signs in again */
        return r;
    }
    char *tok = w->local_iss ? oc_local_issuer_mint(w->local_iss, uid, j->web_nonce, dbw_now_ms() / 1000u)
                             : NULL;
    if (!tok) { r->type = OC_RES_WEB_ERR; r->err_code = OC_ERR_INTERNAL; return r; }
    r->type = OC_RES_WEB_OK;
    r->user_id = uid;
    r->body = (uint8_t *)tok;
    r->body_len = strlen(tok);
    return r;
}

static oc_dbres *web_finish(oc_dbwriter *w, const oc_job *j, oc_dbres *r, uint64_t uid) {
    return web_finish_as(w, j, r, uid, 0);
}

/* A recovery code as typed -- any case, with or without its dash and spaces --
 * reduced to the ten characters it stands for. 0, or -1 if it is not one. */
static int recovery_norm(const char *in, char out[11]) {
    size_t n = 0;
    for (; in && *in; in++) {
        char c = *in;
        if (c == '-' || c == ' ') continue;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) || n == 10) return -1;
        out[n++] = c;
    }
    out[n] = '\0';
    return n == 10 ? 0 : -1;
}

/* Spend one of `uid`'s recovery codes: 1 if `code` is one not yet used, now used. */
static int spend_recovery(sqlite3 *db, uint64_t uid, const char *code) {
    char norm[11];
    if (recovery_norm(code, norm) != 0) return 0;
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "SELECT id, code_hash FROM local_recovery WHERE user_id=? AND used_at_ms IS NULL;",
                       -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)uid);
    int64_t hit = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const uint8_t *h = sqlite3_column_blob(st, 1);
        if (sqlite3_column_bytes(st, 1) != 16 + OC_SHA256_LEN || !h) continue;
        uint8_t buf[16 + 10], dig[OC_SHA256_LEN];
        memcpy(buf, h, 16);
        memcpy(buf + 16, norm, 10);
        if (oc_sha256(buf, sizeof buf, dig) != 0) continue;
        /* Every row is compared whole, so the time says nothing of which matched. */
        if (oc_ct_eq(dig, h + 16, OC_SHA256_LEN) && !hit) hit = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    if (!hit) return 0;
    sqlite3_prepare_v2(db, "UPDATE local_recovery SET used_at_ms=? WHERE id=? AND used_at_ms IS NULL;",
                       -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)dbw_now_ms());
    sqlite3_bind_int64(st, 2, hit);
    int ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db) == 1;
    sqlite3_finalize(st);
    return ok;
}

/* Check a TOTP code against `uid`'s sealed secret, and on a match take its step,
 * so the same code is not good twice. 1 matched; 0 not; -1 the factor key is
 * missing or does not open the secret -- which refuses, and says so. */
static int check_totp(oc_dbwriter *w, uint64_t uid, const char *code) {
    sqlite3 *db = w->db;
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "SELECT secret, last_step FROM local_totp WHERE user_id=? AND confirmed_at_ms IS NOT NULL;",
                       -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)uid);
    uint8_t sealed[OC_TOTP_SEALED_LEN];
    int have = 0;
    uint64_t last = 0;
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_bytes(st, 0) == (int)sizeof sealed) {
        memcpy(sealed, sqlite3_column_blob(st, 0), sizeof sealed);
        last = (uint64_t)sqlite3_column_int64(st, 1);
        have = 1;
    }
    sqlite3_finalize(st);
    if (!have) return 0;
    uint8_t secret[OC_TOTP_SECRET_LEN];
    if (!w->have_factor_key || oc_factor_open(w->factor_key, uid, sealed, sizeof sealed, secret) != 0) {
        fprintf(stderr, "openchimed: a second step was refused: the factor key %s (factor.key in the data directory)\n",
                w->have_factor_key ? "does not open its secret" : "is missing");
        return -1;
    }
    uint64_t matched = 0;
    int ok = oc_totp_verify(secret, sizeof secret, code, dbw_now_ms() / 1000u, last, &matched);
    oc_e2e_wipe(secret, sizeof secret);
    if (!ok) return 0;
    sqlite3_prepare_v2(db, "UPDATE local_totp SET last_step=? WHERE user_id=? AND last_step < ?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)matched);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)uid);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)matched);
    ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db) == 1;
    sqlite3_finalize(st);
    return ok;
}

/* A page's second step (AUTH.md §8.6): the code -- a TOTP code, or a recovery
 * code, spent -- against the account whose password was right, then what that
 * page does: a sign-in's token, or the password change it was holding. The
 * password must still be the one checked; wrong codes have a limiter of their
 * own. */
static oc_dbres *process_auth_step(oc_dbwriter *w, const oc_job *j) {
    sqlite3 *db = w->db;
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    uint64_t uid = j->user_id, now = dbw_now_ms();
    char key[32];
    snprintf(key, sizeof key, "uid:%llu", (unsigned long long)uid);
    if (oc_ratelimit_blocked(w->step_rl, key, now)) {
        r->type = OC_RES_WEB_ERR; r->err_code = OC_ERR_AUTH_RATE_LIMITED; return r;
    }
    uint8_t role = 0;
    if (!uid || !credential_current(db, uid, j->auth_version, &role) || user_disabled(db, uid)) {
        r->type = OC_RES_WEB_ERR; r->err_code = OC_ERR_AUTH_INVALID_TOKEN; return r;
    }
    const char *code = j->token ? j->token : "";
    int ok = 0, recovery = 0;
    if (j->wa_cd) ok = check_passkey(db, uid, j);   /* a passkey, in place of a code */
    else {
        ok = strlen(code) == OC_TOTP_DIGITS ? check_totp(w, uid, code) : 0;
        if (ok == 0 && strlen(code) != OC_TOTP_DIGITS) recovery = ok = spend_recovery(db, uid, code);
    }
    if (ok != 1) {
        oc_ratelimit_record(w->step_rl, key, now);
        audit_actor(db, OC_AUDIT_SECURITY, "auth.failed", uid, 0, NULL, 0,
                    ok < 0 ? "step=totp reason=factor-key" : "step=code");
        r->type = OC_RES_WEB_ERR;
        r->err_code = ok < 0 ? OC_ERR_INTERNAL : OC_ERR_AUTH_INVALID_TOKEN;
        return r;
    }
    oc_ratelimit_reset(w->step_rl, key);
    if (recovery) audit_actor(db, OC_AUDIT_SECURITY, "auth.recovery_code", uid, 0, NULL, 1, NULL);
    return web_finish_as(w, j, r, uid, 1);
}

/* The second step's page, past its password (AUTH.md §8.6): an account that has
 * one is told so, and can turn it off with a code; one that has none gets a new
 * secret, sealed and kept unconfirmed until its first code comes back. */
static oc_dbres *security_begin(oc_dbwriter *w, const oc_job *j, oc_dbres *r, uint64_t uid) {
    sqlite3 *db = w->db;
    r->type = OC_RES_WEB_SECURITY;
    r->user_id = uid;
    r->step_version = j->auth_version;
    if (has_second_step(db, uid)) { r->step_pw = 1; return r; }
    uint8_t secret[OC_TOTP_SECRET_LEN], sealed[OC_TOTP_SEALED_LEN];
    char b32[64];
    if (!w->have_factor_key || oc_rand_bytes(secret, sizeof secret) != 0 ||
        oc_factor_seal(w->factor_key, uid, secret, sealed) != 0 ||
        oc_base32_encode(secret, sizeof secret, b32, sizeof b32) < 0) {
        oc_e2e_wipe(secret, sizeof secret);
        if (!w->have_factor_key)
            fprintf(stderr, "openchimed: a second step cannot be set up: the factor key is missing "
                            "(factor.key in the data directory)\n");
        r->type = OC_RES_WEB_ERR; r->err_code = OC_ERR_INTERNAL; return r;
    }
    oc_e2e_wipe(secret, sizeof secret);
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "INSERT INTO local_totp(user_id, secret, confirmed_at_ms, last_step) VALUES(?, ?, NULL, 0) "
        "ON CONFLICT(user_id) DO UPDATE SET secret=excluded.secret, last_step=0 "
        "WHERE local_totp.confirmed_at_ms IS NULL;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)uid);
    sqlite3_bind_blob(st, 2, sealed, sizeof sealed, SQLITE_TRANSIENT);
    int ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    if (!ok || !(r->body = (uint8_t *)strdup(b32))) {
        oc_e2e_wipe(b32, sizeof b32);
        r->type = OC_RES_WEB_ERR; r->err_code = OC_ERR_INTERNAL; return r;
    }
    r->body_len = strlen(b32);
    oc_e2e_wipe(b32, sizeof b32);
    return r;
}

/* Ten new recovery codes for `uid`, replacing any: written as "XXXXX-XXXXX"
 * lines into a heap string for the page to show once; only their salted hashes
 * are kept. NULL on failure. */
static char *new_recovery_codes(sqlite3 *db, uint64_t uid) {
    static const char A[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";   /* nothing to misread */
    char *out = calloc(10, 12 + 1);
    if (!out) return NULL;
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "DELETE FROM local_recovery WHERE user_id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)uid);
    sqlite3_step(st);
    sqlite3_finalize(st);
    size_t o = 0;
    for (int k = 0; k < 10; k++) {
        uint8_t rnd[10], row[16 + OC_SHA256_LEN], buf[16 + 10];
        char code[11];
        if (oc_rand_bytes(rnd, sizeof rnd) != 0 || oc_rand_bytes(row, 16) != 0) { free(out); return NULL; }
        for (int i = 0; i < 10; i++) code[i] = A[rnd[i] & 31];
        code[10] = '\0';
        memcpy(buf, row, 16);
        memcpy(buf + 16, code, 10);
        if (oc_sha256(buf, sizeof buf, row + 16) != 0) { free(out); return NULL; }
        sqlite3_prepare_v2(db, "INSERT INTO local_recovery(user_id, code_hash) VALUES(?, ?);", -1, &st, NULL);
        sqlite3_bind_int64(st, 1, (sqlite3_int64)uid);
        sqlite3_bind_blob(st, 2, row, sizeof row, SQLITE_TRANSIENT);
        int ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
        if (!ok) { free(out); return NULL; }
        o += (size_t)snprintf(out + o, 13 + 1, "%.5s-%.5s\n", code, code + 5);
        oc_e2e_wipe(code, sizeof code);
    }
    return out;
}

/* The second step's page, past its password and with a code (AUTH.md §8.6):
 * confirming a new secret turns the step on and hands out recovery codes;
 * turning it off takes a current code or a recovery code. The password must
 * still be the one checked; wrong codes count as a step's do. */
static oc_dbres *process_security(oc_dbwriter *w, const oc_job *j) {
    sqlite3 *db = w->db;
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    uint64_t uid = j->user_id, now = dbw_now_ms();
    char key[32];
    snprintf(key, sizeof key, "uid:%llu", (unsigned long long)uid);
    if (oc_ratelimit_blocked(w->step_rl, key, now)) {
        r->type = OC_RES_WEB_ERR; r->err_code = OC_ERR_AUTH_RATE_LIMITED; return r;
    }
    uint8_t role = 0;
    if (!uid || !credential_current(db, uid, j->auth_version, &role) || user_disabled(db, uid)) {
        r->type = OC_RES_WEB_ERR; r->err_code = OC_ERR_AUTH_INVALID_TOKEN; return r;
    }
    const char *code = j->token ? j->token : "";
    int ok = 0;
    sqlite3_stmt *st = NULL;
    if (j->scope == OC_SEC_CONFIRM_TOTP) {
        /* The new secret's first code: proof the authenticator has it. */
        uint8_t sealed[OC_TOTP_SEALED_LEN], secret[OC_TOTP_SECRET_LEN];
        int have = 0;
        sqlite3_prepare_v2(db, "SELECT secret FROM local_totp WHERE user_id=? AND confirmed_at_ms IS NULL;",
                           -1, &st, NULL);
        sqlite3_bind_int64(st, 1, (sqlite3_int64)uid);
        if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_bytes(st, 0) == (int)sizeof sealed) {
            memcpy(sealed, sqlite3_column_blob(st, 0), sizeof sealed);
            have = 1;
        }
        sqlite3_finalize(st);
        uint64_t matched = 0;
        if (have && w->have_factor_key && oc_factor_open(w->factor_key, uid, sealed, sizeof sealed, secret) == 0) {
            ok = oc_totp_verify(secret, sizeof secret, code, now / 1000u, 0, &matched);
            oc_e2e_wipe(secret, sizeof secret);
        }
        char *codes = NULL;
        if (ok) {
            sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);
            sqlite3_prepare_v2(db, "UPDATE local_totp SET confirmed_at_ms=?, last_step=? "
                                   "WHERE user_id=? AND confirmed_at_ms IS NULL;", -1, &st, NULL);
            sqlite3_bind_int64(st, 1, (sqlite3_int64)now);
            sqlite3_bind_int64(st, 2, (sqlite3_int64)matched);
            sqlite3_bind_int64(st, 3, (sqlite3_int64)uid);
            ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db) == 1;
            sqlite3_finalize(st);
            if (ok) codes = new_recovery_codes(db, uid);
            sqlite3_exec(db, ok && codes ? "COMMIT;" : "ROLLBACK;", NULL, NULL, NULL);
            if (ok && !codes) { r->type = OC_RES_WEB_ERR; r->err_code = OC_ERR_INTERNAL; return r; }
        }
        if (ok) {
            oc_ratelimit_reset(w->step_rl, key);
            audit_actor(db, OC_AUDIT_SECURITY, "auth.step_on", uid, 0, NULL, 1, "step=totp");
            r->type = OC_RES_WEB_OK;
            r->user_id = uid;
            r->body = (uint8_t *)codes;
            r->body_len = strlen(codes);
            return r;
        }
    } else if (j->scope == OC_SEC_PASSKEY_ASK) {
        /* A code first: a password alone does not add a way past the step. */
        ok = strlen(code) == OC_TOTP_DIGITS ? check_totp(w, uid, code) : spend_recovery(db, uid, code);
        if (ok == 1) {
            oc_ratelimit_reset(w->step_rl, key);
            r->type = OC_RES_WEB_OK;
            r->user_id = uid;
            r->step_pw = OC_SEC_PASSKEY_ASK;
            r->pk_creds = passkeys_for(db, uid, j->wa_rp);
            return r;
        }
    } else if (j->scope == OC_SEC_PASSKEY_ADD) {
        uint8_t cd[2048], att[4096];
        long cl = j->wa_cd ? oc_base64url_decode(j->wa_cd, strlen(j->wa_cd), cd, sizeof cd) : -1;
        long al = j->wa_att ? oc_base64url_decode(j->wa_att, strlen(j->wa_att), att, sizeof att) : -1;
        oc_wa_cred cr;
        if (cl > 0 && al > 0 && has_second_step(db, uid) &&
            oc_webauthn_register(cd, (size_t)cl, att, (size_t)al, j->wa_challenge, j->wa_origin, j->wa_rp, &cr) == OC_WA_OK) {
            sqlite3_prepare_v2(db, "INSERT INTO webauthn_credentials(user_id, cred_id, cose_key, sign_count, rp_id, "
                                   "created_at_ms) VALUES(?,?,?,?,?,?);", -1, &st, NULL);
            sqlite3_bind_int64(st, 1, (sqlite3_int64)uid);
            sqlite3_bind_blob (st, 2, cr.cred_id, (int)cr.cred_len, SQLITE_TRANSIENT);
            sqlite3_bind_blob (st, 3, cr.cose, (int)cr.cose_len, SQLITE_TRANSIENT);
            sqlite3_bind_int64(st, 4, (sqlite3_int64)cr.count);
            sqlite3_bind_text (st, 5, j->wa_rp, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(st, 6, (sqlite3_int64)now);
            ok = sqlite3_step(st) == SQLITE_DONE;   /* a credential already here is refused by its id */
            sqlite3_finalize(st);
        }
        if (ok == 1) {
            audit_actor(db, OC_AUDIT_SECURITY, "auth.passkey_added", uid, 0, NULL, 1, NULL);
            r->type = OC_RES_WEB_OK;
            r->user_id = uid;
            r->step_pw = OC_SEC_PASSKEY_ADD;
            return r;
        }
    } else if (j->scope == OC_SEC_TURN_OFF) {
        ok = strlen(code) == OC_TOTP_DIGITS ? check_totp(w, uid, code) : spend_recovery(db, uid, code);
        if (ok == 1) {
            static const char *const DEL[] = { "DELETE FROM local_totp WHERE user_id=?;",
                                               "DELETE FROM local_recovery WHERE user_id=?;",
                                               "DELETE FROM webauthn_credentials WHERE user_id=?;" };
            for (int i = 0; i < 3; i++) {
                sqlite3_prepare_v2(db, DEL[i], -1, &st, NULL);
                sqlite3_bind_int64(st, 1, (sqlite3_int64)uid);
                sqlite3_step(st);
                sqlite3_finalize(st);
            }
            oc_ratelimit_reset(w->step_rl, key);
            audit_actor(db, OC_AUDIT_SECURITY, "auth.step_off", uid, 0, NULL, 1, NULL);
            r->type = OC_RES_WEB_OK;
            r->user_id = uid;
            return r;
        }
    }
    oc_ratelimit_record(w->step_rl, key, now);
    audit_actor(db, OC_AUDIT_SECURITY, "auth.failed", uid, 0, NULL, 0,
                ok < 0 ? "step=totp reason=factor-key" : "step=code");
    r->type = OC_RES_WEB_ERR;
    r->err_code = ok < 0 ? OC_ERR_INTERNAL : OC_ERR_AUTH_INVALID_TOKEN;
    return r;
}

#define OC_RESET_TTL_MS (24ull * 60 * 60 * 1000)

/* An administrator's reset of a local account (AUTH.md §2): a one-time link,
 * good for a day, on which its person sets a new password. Who may reset whom
 * is who may remove whom; a removed account may be reset, for its return. */
static oc_dbres *process_reset_credential(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    uint8_t actor_role = OC_ROLE_MEMBER, target_role = OC_ROLE_MEMBER;
    if (!user_role(db, j->user_id, &actor_role) || !user_role(db, j->target_user_id, &target_role) ||
        !oc_role_can_manage_members(actor_role) ||
        (target_role != OC_ROLE_MEMBER && actor_role != OC_ROLE_OWNER)) {
        audit_actor(db, OC_AUDIT_SECURITY, "credential.reset.denied", j->user_id, j->target_user_id, NULL, 0, NULL);
        r->type = OC_RES_USER_ERR; r->err_code = OC_ERR_FORBIDDEN; return r;
    }
    /* Only an account with a password here: a provider's is the provider's. */
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "SELECT 1 FROM users WHERE id=? AND subject LIKE 'local:%';", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->target_user_id);
    int local = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    if (!local) { r->type = OC_RES_USER_ERR; r->err_code = OC_ERR_FORBIDDEN; return r; }
    uint8_t tok[32], hash[OC_SHA256_LEN];
    char hex[65];
    if (oc_rand_bytes(tok, sizeof tok) != 0 || oc_sha256(tok, sizeof tok, hash) != 0) {
        r->type = OC_RES_USER_ERR; r->err_code = OC_ERR_INTERNAL; return r;
    }
    for (size_t i = 0; i < sizeof tok; i++) snprintf(hex + 2 * i, 3, "%02x", tok[i]);
    oc_e2e_wipe(tok, sizeof tok);
    uint64_t now = dbw_now_ms();
    sqlite3_prepare_v2(db,
        "INSERT INTO credential_resets(token_hash, user_id, created_by, created_at_ms, expires_at_ms, clear_factors) "
        "VALUES(?,?,?,?,?,?);", -1, &st, NULL);
    sqlite3_bind_blob (st, 1, hash, sizeof hash, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)j->target_user_id);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)j->user_id);
    sqlite3_bind_int64(st, 4, (sqlite3_int64)now);
    sqlite3_bind_int64(st, 5, (sqlite3_int64)(now + OC_RESET_TTL_MS));
    sqlite3_bind_int  (st, 6, j->scope == 1);
    int ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    if (!ok || !(r->body = (uint8_t *)strdup(hex))) {
        oc_e2e_wipe(hex, sizeof hex);
        r->type = OC_RES_USER_ERR; r->err_code = OC_ERR_INTERNAL; return r;
    }
    oc_e2e_wipe(hex, sizeof hex);
    r->body_len = 64;
    audit_actor(db, OC_AUDIT_SECURITY, "credential.reset.issue", j->user_id, j->target_user_id, NULL, 1,
                j->scope == 1 ? "clear_factors=1" : NULL);
    r->type = OC_RES_CREDENTIAL_RESET;
    r->user_id = j->target_user_id;
    return r;
}

/* Bring a removed member back (REQ-033): who may do it is who may remove them.
 * Nothing removal dropped comes back -- channels, groups, drafts -- but the
 * workspace's default channel at the next sign-in; a local account, whose
 * password went too, comes back with a reset link to set one. */
static oc_dbres *process_enable_user(oc_dbwriter *w, const oc_job *j) {
    sqlite3 *db = w->db;
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    uint8_t actor_role = OC_ROLE_MEMBER, target_role = OC_ROLE_MEMBER;
    if (!user_role(db, j->user_id, &actor_role) || !user_role(db, j->target_user_id, &target_role) ||
        !oc_role_can_manage_members(actor_role) ||
        (target_role != OC_ROLE_MEMBER && actor_role != OC_ROLE_OWNER) || !user_disabled(db, j->target_user_id)) {
        r->type = OC_RES_USER_ERR; r->err_code = OC_ERR_FORBIDDEN; return r;
    }
    /* Back is a seat taken, as a new member's is (AUTH.md §2). */
    if (user_slots_full(db, "", 0, w->max_users)) { r->type = OC_RES_USER_ERR; r->err_code = OC_ERR_USER_LIMIT; return r; }
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "UPDATE users SET disabled=0 WHERE id=? AND disabled=1;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->target_user_id);
    int ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db) == 1;
    sqlite3_finalize(st);
    if (!ok) { r->type = OC_RES_USER_ERR; r->err_code = OC_ERR_INTERNAL; return r; }
    audit_actor(db, OC_AUDIT_ADMIN, "user.enable", j->user_id, j->target_user_id, NULL, 1, NULL);
    /* A local account's way back in: the link a reset makes. */
    oc_job rj = *j;
    rj.scope = 0;
    oc_dbres *link = process_reset_credential(db, &rj);
    if (link && link->type == OC_RES_CREDENTIAL_RESET) { r->body = link->body; r->body_len = link->body_len; link->body = NULL; }
    oc_dbres_free(link);
    r->type = OC_RES_USER_UPDATED;
    r->user_id = j->target_user_id;
    r->role = target_role;
    r->disabled = 0;
    return r;
}

/* Sign one of the asker's own sessions out (REQ-182): its row goes -- the
 * delete is scoped to the asker, so another's session id does nothing -- and
 * the list comes back without it; its connection closes (`revoked_session`). */
static oc_dbres *process_list_sessions(sqlite3 *db, const oc_job *j);
static oc_dbres *process_revoke_session(sqlite3 *db, const oc_job *j) {
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "DELETE FROM sessions WHERE id=? AND user_id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->target_user_id);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)j->user_id);
    int gone = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db) == 1;
    sqlite3_finalize(st);
    if (gone) audit_actor(db, OC_AUDIT_SECURITY, "session.revoke", j->user_id, 0, "one", 1, NULL);
    oc_dbres *r = process_list_sessions(db, j);
    if (r && gone) { r->user_id = j->user_id; r->revoked_session = j->target_user_id; }
    return r;
}

/* Sessions in use since their last mark (REQ-181): seen now. One statement per
 * batch, which the net loop sends at most hourly per connection. */
static oc_dbres *process_sessions_seen(sqlite3 *db, const oc_job *j) {
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "UPDATE sessions SET last_seen_ms=? WHERE id=?;", -1, &st, NULL);
    uint64_t now = dbw_now_ms();
    for (uint16_t i = 0; i < j->n_grp_uids; i++) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)now);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)j->grp_uids[i]);
        sqlite3_step(st);
        sqlite3_reset(st);
    }
    sqlite3_finalize(st);
    /* Each one's member was active today (REQ-043): the net loop sends this at
     * least once in every UTC day a connection is used. */
    st = NULL;
    if (sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO user_active_days(user_id, day) "
                               "SELECT user_id, ? FROM sessions WHERE id=?;", -1, &st, NULL) == SQLITE_OK) {
        for (uint16_t i = 0; i < j->n_grp_uids; i++) {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)(now / 86400000ull));
            sqlite3_bind_int64(st, 2, (sqlite3_int64)j->grp_uids[i]);
            sqlite3_step(st);
            sqlite3_reset(st);
        }
        sqlite3_finalize(st);
    }
    return NULL;
}

/* The live reset a link's token names: its account and whether it clears the
 * second step. 1, or 0 if there is none -- unknown, spent or past its day. */
static int reset_lookup(sqlite3 *db, const char *hex, uint8_t hash[OC_SHA256_LEN], uint64_t *uid, int *clear) {
    uint8_t tok[32];
    if (!hex || strlen(hex) != 64) return 0;
    for (int i = 0; i < 32; i++) {
        unsigned v;
        if (sscanf(hex + 2 * i, "%2x", &v) != 1) return 0;
        tok[i] = (uint8_t)v;
    }
    if (oc_sha256(tok, sizeof tok, hash) != 0) return 0;
    oc_e2e_wipe(tok, sizeof tok);
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "SELECT user_id, clear_factors FROM credential_resets "
                           "WHERE token_hash=? AND consumed_at_ms IS NULL AND expires_at_ms > ?;", -1, &st, NULL);
    sqlite3_bind_blob(st, 1, hash, OC_SHA256_LEN, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)dbw_now_ms());
    int found = sqlite3_step(st) == SQLITE_ROW;
    if (found) { *uid = (uint64_t)sqlite3_column_int64(st, 0); *clear = sqlite3_column_int(st, 1); }
    sqlite3_finalize(st);
    return found;
}

/* The reset page (AUTH.md §2): the link's token and a new password. The token is
 * checked, the password derived on the auth pool -- no old one to check -- and
 * then, the token still unspent, stored: a new version, every session revoked,
 * the second step cleared if the reset said so. */
static oc_dbres *process_reset_redeem(oc_dbwriter *w, const oc_job *j) {
    sqlite3 *db = w->db;
    uint8_t hash[OC_SHA256_LEN];
    uint64_t uid = 0;
    int clear = 0;
    oc_dbres *r = NULL;
    int live = reset_lookup(db, j->token, hash, &uid, &clear) && !user_disabled(db, uid);
    if (j->auth_stage == OC_AUTH_STAGE_NEW) {
        /* The one rule the page cannot apply: whose account this is is known
         * only here, from the token. A local account's new password may not be
         * built on its username, as at sign-up and on a change (AUTH.md §2);
         * refused, the token is not spent and nothing is hashed. */
        if (live && j->pf_new_pw && j->pf_new_pw[0]) {
            char subject[256] = "";
            sqlite3_stmt *sq = NULL;
            sqlite3_prepare_v2(db, "SELECT subject FROM users WHERE id=?;", -1, &sq, NULL);
            sqlite3_bind_int64(sq, 1, (sqlite3_int64)uid);
            if (sqlite3_step(sq) == SQLITE_ROW && sqlite3_column_text(sq, 0))
                snprintf(subject, sizeof subject, "%s", (const char *)sqlite3_column_text(sq, 0));
            sqlite3_finalize(sq);
            if (strncmp(subject, "local:", 6) == 0) {
                char norm[OC_PW_NORM_MAX];
                oc_pw_verdict pv = oc_pw_check(j->pf_new_pw, subject + 6, NULL, norm, sizeof norm);
                oc_e2e_wipe(norm, sizeof norm);
                if (pv == OC_PW_PERSONAL) {
                    if (!(r = calloc(1, sizeof *r))) return NULL;
                    r->conn_id = j->conn_id;
                    r->type = OC_RES_WEB_ERR; r->pw_verdict = pv;
                    return r;
                }
            }
        }
        oc_auth_check *chk = live && j->pf_new_pw && j->pf_new_pw[0] ? calloc(1, sizeof *chk) : NULL;
        if (chk && oc_rand_bytes(chk->new_salt, sizeof chk->new_salt) == 0) {
            chk->new_password = j->pf_new_pw;
            chk->new_pwlen = strlen(j->pf_new_pw);
            chk->new_mem = w->pw_mem;
            chk->owner = (oc_job *)j;
            ((oc_job *)j)->auth_stage = OC_AUTH_STAGE_READ;
            oc_authpool_submit(w->auth, chk);
            w->auth_deferred = 1;
            return NULL;
        }
        free(chk);
        if (!(r = calloc(1, sizeof *r))) return NULL;
        r->conn_id = j->conn_id;
        r->type = OC_RES_WEB_ERR; r->err_code = live ? OC_ERR_INTERNAL : OC_ERR_AUTH_INVALID_TOKEN;
        return r;
    }
    if (!(r = calloc(1, sizeof *r))) return NULL;
    r->conn_id = j->conn_id;
    if (!live || j->auth_ok != 1 || !j->pw_derived) {
        r->type = OC_RES_WEB_ERR; r->err_code = live ? OC_ERR_INTERNAL : OC_ERR_AUTH_INVALID_TOKEN; return r;
    }
    uint64_t version = next_credential_version(db), now = dbw_now_ms();
    sqlite3_stmt *st = NULL;
    sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);
    sqlite3_prepare_v2(db, "UPDATE credential_resets SET consumed_at_ms=? WHERE token_hash=? AND consumed_at_ms IS NULL;",
                       -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)now);
    sqlite3_bind_blob(st, 2, hash, sizeof hash, SQLITE_STATIC);
    int ok = version && sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db) == 1;
    sqlite3_finalize(st);
    if (ok) {
        sqlite3_prepare_v2(db,
            "INSERT INTO local_credentials(user_id, salt, kdf, hash, updated_at_ms, version) VALUES(?,?,?,?,?,?) "
            "ON CONFLICT(user_id) DO UPDATE SET salt=excluded.salt, kdf=excluded.kdf, "
            " hash=excluded.hash, updated_at_ms=excluded.updated_at_ms, version=excluded.version;", -1, &st, NULL);
        sqlite3_bind_int64(st, 1, (sqlite3_int64)uid);
        sqlite3_bind_blob (st, 2, j->pw_salt, OC_PW_SALT_LEN, SQLITE_TRANSIENT);
        char kdf[OC_PW_KDF_MAX];
        oc_pw_kdf_format(j->pw_mem, kdf);
        sqlite3_bind_text (st, 3, kdf, -1, SQLITE_TRANSIENT);
        sqlite3_bind_blob (st, 4, j->pw_hash, OC_PW_HASH_LEN, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 5, (sqlite3_int64)now);
        sqlite3_bind_int64(st, 6, (sqlite3_int64)version);
        ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
    }
    static const char *const WIPE[] = { "DELETE FROM sessions WHERE user_id=?;",
                                        "DELETE FROM local_totp WHERE user_id=?;",
                                        "DELETE FROM local_recovery WHERE user_id=?;",
                                        "DELETE FROM webauthn_credentials WHERE user_id=?;" };
    for (int i = 0; ok && i < (clear ? 4 : 1); i++) {
        sqlite3_prepare_v2(db, WIPE[i], -1, &st, NULL);
        sqlite3_bind_int64(st, 1, (sqlite3_int64)uid);
        ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
    }
    sqlite3_exec(db, ok ? "COMMIT;" : "ROLLBACK;", NULL, NULL, NULL);
    if (!ok) { r->type = OC_RES_WEB_ERR; r->err_code = OC_ERR_INTERNAL; return r; }
    audit_actor(db, OC_AUDIT_ACCOUNT, "credential.reset", uid, 0, NULL, 1, clear ? "via=browser clear_factors=1" : "via=browser");
    r->type = OC_RES_WEB_OK;
    r->user_id = uid;
    r->revoked = 1;   /* every device signs in again, with the new password */
    return r;
}

/* An AUTH job, from a frame or the daemon's pages; a page's refusal is a page's. */
static oc_dbres *process_auth(oc_dbwriter *w, const oc_job *j) {
    oc_dbres *r = process_auth_job(w, j);
    if (r && j->web && r->type == OC_RES_AUTH_ERR) r->type = OC_RES_WEB_ERR;
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
    if (target_cur == OC_ROLE_OWNER && next != OC_ROLE_OWNER && count_active_owners(db) <= 1) {
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
    if (next != target_cur && j->target_user_id != j->user_id) {
        char *who = audit_name_of(db, j->user_id);
        notice_person(db, j->target_user_id, "@%s made you %s.", who ? who : "someone",
                      next == OC_ROLE_OWNER ? "an owner" : next == OC_ROLE_ADMIN ? "an admin" : "a member");
        free(who);
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
    r->revoked = j->scope == OC_LOGOUT_ALL;
    return r;
}

/* --- Admin ops (REQ-033, tenant-level; owner/admin) --------------------- */

/* The roster's rows: everyone but visitors (REQ-289; a visitor is no member,
 * SCHEMA.md §3bj), or (`only` non-zero) the one user. */
static void user_rows(sqlite3 *db, uint64_t only, oc_dbres *r) {
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "SELECT id, role, disabled, COALESCE(email,''), COALESCE(display_name,''), "
        "       COALESCE(avatar_attachment_id,0), COALESCE(title,''), COALESCE(timezone,''), "
        "       COALESCE(status_emoji,''), COALESCE(status_text,''), status_expires_ms, "
        "       COALESCE(full_name,''), COALESCE(pronouns,''), COALESCE(voice_id,''), is_system, "
        "       ooo_start_ms, ooo_end_ms, ooo_message, kind='agent' "
        "FROM users WHERE (?1 = 0 AND kind <> 'visitor') OR id = ?1 ORDER BY id;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)only);
    size_t cap = 8, n = 0;
    oc_user_row *arr = malloc(cap * sizeof *arr);
    while (arr && sqlite3_step(st) == SQLITE_ROW) {
        if (n == cap) { cap *= 2; oc_user_row *g = realloc(arr, cap * sizeof *arr); if (!g) break; arr = g; }
        arr[n].user_id      = (uint64_t)sqlite3_column_int64(st, 0);
        arr[n].role         = role_to_u8((const char *)sqlite3_column_text(st, 1));
        arr[n].disabled     = (uint8_t)(sqlite3_column_int(st, 2) != 0);
        arr[n].email        = strdup((const char *)sqlite3_column_text(st, 3));
        arr[n].display_name = strdup((const char *)sqlite3_column_text(st, 4));
        /* The roster carries the avatar so a transcript can draw every
         * author's picture without a PROFILE_INFO round trip per author. */
        arr[n].avatar_id    = (uint64_t)sqlite3_column_int64(st, 5);
        /* REQ-289: the profile fields, so a client learns them for everyone
         * rather than only for itself. Expired status reads as absent — the same
         * rule build_profile applies, applied in the same place. */
        arr[n].title        = strdup((const char *)sqlite3_column_text(st, 6));
        arr[n].timezone     = strdup((const char *)sqlite3_column_text(st, 7));
        {
            uint64_t exp = (uint64_t)sqlite3_column_int64(st, 10);
            int expired = (exp != 0 && exp <= dbw_now_ms());
            arr[n].status_emoji = strdup(expired ? "" : (const char *)sqlite3_column_text(st, 8));
            arr[n].status_text  = strdup(expired ? "" : (const char *)sqlite3_column_text(st, 9));
        }
        arr[n].full_name    = strdup((const char *)sqlite3_column_text(st, 11));
        arr[n].pronouns     = strdup((const char *)sqlite3_column_text(st, 12));
        arr[n].voice_id     = strdup((const char *)sqlite3_column_text(st, 13));
        arr[n].flags        = (uint8_t)((sqlite3_column_int(st, 14) ? OC_USER_SYSTEM : 0) |
                                        (sqlite3_column_int(st, 18) ? OC_USER_AGENT : 0));
        arr[n].ooo_start    = (uint64_t)sqlite3_column_int64(st, 15);
        arr[n].ooo_end      = (uint64_t)sqlite3_column_int64(st, 16);
        arr[n].ooo_message  = strdup((const char *)sqlite3_column_text(st, 17));
        n++;
    }
    sqlite3_finalize(st);
    r->ulist = arr;
    r->n_ulist = n;
}

/* Enumerate every tenant user. Available to any authenticated user (a client
 * needs the roster to address messages and pick op targets). */
static oc_dbres *process_list_users(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->type = OC_RES_USER_LIST;
    user_rows(db, 0, r);
    return r;
}

/* Mint an invite token for a new local account (REQ-033). Owner/admin only; only
 * an owner may invite at an elevated (admin/owner) role. Only the token's SHA-256
 * is stored; the raw token is returned once for the actor to share. */
/* Mint an invite for `role`: random token to the caller, only its SHA-256 +
 * expiry stored. `created_by` is the issuing user (0 -> NULL, e.g. a first-run
 * setup token with no issuer). Returns 0 and fills token/expiry, or -1. */
/* `hash_out` (may be NULL) receives the SHA-256 the row is keyed by. `uses` is
 * how many may redeem it (REQ-026): 1, N, or 0 for no limit until it expires. */
static int mint_invite_for(sqlite3 *db, uint64_t created_by, uint8_t role, const char *email,
                           uint16_t uses, uint8_t token[OC_INVITE_TOKEN_LEN], uint64_t *expiry_out,
                           uint8_t hash_out[OC_SHA256_LEN]) {
    uint8_t hash[OC_SHA256_LEN];
    if (oc_rand_bytes(token, OC_INVITE_TOKEN_LEN) != 0 ||
        oc_sha256(token, OC_INVITE_TOKEN_LEN, hash) != 0) return -1;
    if (hash_out) memcpy(hash_out, hash, sizeof hash);
    uint64_t expiry = dbw_now_ms() + OC_INVITE_TTL_MS;
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "INSERT INTO invites(token_hash,created_by,role,expires_at_ms,email,uses,created_at_ms) "
        "VALUES(?,?,?,?,lower(?),?,?);",
        -1, &st, NULL);
    sqlite3_bind_blob(st, 1, hash, sizeof hash, SQLITE_STATIC);
    if (created_by) sqlite3_bind_int64(st, 2, (sqlite3_int64)created_by);
    else            sqlite3_bind_null(st, 2);
    sqlite3_bind_text(st, 3, u8_to_role(role), -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 4, (sqlite3_int64)expiry);
    if (email && email[0]) sqlite3_bind_text(st, 5, email, -1, SQLITE_STATIC);
    else                   sqlite3_bind_null(st, 5);
    sqlite3_bind_int(st, 6, uses);
    sqlite3_bind_int64(st, 7, (sqlite3_int64)dbw_now_ms());
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) return -1;
    if (expiry_out) *expiry_out = expiry;
    return 0;
}

static int mint_invite(sqlite3 *db, uint64_t created_by, uint8_t role,
                       uint8_t token[OC_INVITE_TOKEN_LEN], uint64_t *expiry_out) {
    return mint_invite_for(db, created_by, role, NULL, 1, token, expiry_out, NULL);
}

/* The id an invitation is reported to central under (ARCH-85's invitation
 * mail): 16 bytes of SHA-256 over a label and the row's key, as lowercase hex.
 * Derived rather than stored, so every retry of one report names the same
 * invitation and the table needs no column for it. The key is the hash of a
 * token nobody holds -- an address-bound invite hands out no token -- and the
 * label keeps the id from being that hash. */
static void invite_report_id(const uint8_t hash[OC_SHA256_LEN], char out[33]) {
    static const char LABEL[] = "openchime-invite-id-v1|";
    uint8_t buf[sizeof LABEL - 1 + OC_SHA256_LEN], id[OC_SHA256_LEN];
    memcpy(buf, LABEL, sizeof LABEL - 1);
    memcpy(buf + sizeof LABEL - 1, hash, OC_SHA256_LEN);
    if (oc_sha256(buf, sizeof buf, id) != 0) { out[0] = '\0'; return; }
    static const char HEX[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) { out[2 * i] = HEX[id[i] >> 4]; out[2 * i + 1] = HEX[id[i] & 15]; }
    out[32] = '\0';
}

static oc_dbres *process_invite_user(oc_dbwriter *w, const oc_job *j) {
    sqlite3 *db = w->db;
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
    /* An invite names an address or mints a bearer token, never both: bound to an
     * address it is spent by that address's verified sign-in (AUTH.md §8.4), and
     * the token that comes back is zeros — there is nothing to hand anybody. */
    const char *email = (j->email && j->email[0]) ? j->email : NULL;
    if (email && !oc_email_plausible(email)) {
        r->type = OC_RES_INVITE_ERR; r->err_code = OC_ERR_FORBIDDEN; return r;
    }
    /* How many may redeem it (REQ-026): 1 is an invitation to one person; N, or
     * 0 for no limit, is a link to pass around. One address spends its invite
     * once, whatever was asked. A link passed around makes members only:
     * nobody mints admins by forwarding a message. */
    uint16_t uses = email ? 1 : j->uses;
    if (uses != 1 && want != OC_ROLE_MEMBER) {
        r->type = OC_RES_INVITE_ERR; r->err_code = OC_ERR_FORBIDDEN; return r;
    }
    /* Only a provider's verified sign-in spends an address-bound invite: the
     * relay's, or a direct connection's, which goes through the same admission
     * and spends it the same way. Where neither is on, nothing could ever redeem
     * it, and minting it anyway would hand the inviter a row that sits there
     * until it expires. */
    if (email && !(w->auth_methods & OC_AUTH_OIDC) && w->n_direct_iss == 0) {
        r->type = OC_RES_INVITE_ERR; r->err_code = OC_ERR_INVITE_UNREDEEMABLE; return r;
    }
    uint8_t token[OC_INVITE_TOKEN_LEN], hash[OC_SHA256_LEN]; uint64_t expiry = 0;
    if (mint_invite_for(db, j->user_id, want, email, uses, token, &expiry, hash) != 0) {
        r->type = OC_RES_INVITE_ERR; r->err_code = OC_ERR_INTERNAL; return r;
    }
    if (email) {
        memset(token, 0, sizeof token);
        /* What the invitation mail report needs (ARCH-85): the address as it was
         * stored, and an id that stays the same for this invite. */
        invite_report_id(hash, r->invite_id);
        r->invite_email = strdup(email);
        for (char *p = r->invite_email; p && *p; p++)
            if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');
    }
    audit_actor(db, OC_AUDIT_ADMIN, "user.invite", j->user_id, 0,
                u8_to_role(j->role), 1, email);
    r->type = OC_RES_INVITE_OK;
    memcpy(r->session_token, token, OC_INVITE_TOKEN_LEN);  /* carries the invite token */
    r->session_expiry = expiry;
    r->role = want;
    r->invite_uses = uses;
    return r;
}

/* Unredeemed setup tokens are spent: a newer one replaces them, and an owner who
 * can sign in ends them all. */
static void spend_setup_tokens(sqlite3 *db) {
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "UPDATE invites SET consumed_at_ms=? WHERE created_by IS NULL AND consumed_at_ms IS NULL;",
        -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)dbw_now_ms());
    sqlite3_step(st);
    sqlite3_finalize(st);
}

/* First-run bootstrap (REQ-024): if the tenant has no owner who can sign in, mint
 * a one-time owner invite so the operator can create one by redeeming it (no
 * pre-existing admin needed, air-gapped-safe). Only the newest works: each boot
 * prints one, and a log keeps every one it printed. Returns INVITE_OK with the
 * token, or INVITE_ERR/err_code 0 when an owner exists (nothing to do). */
static oc_dbres *process_setup_invite(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    if (count_active_owners(db) > 0) { r->type = OC_RES_INVITE_ERR; r->err_code = 0; return r; }
    spend_setup_tokens(db);
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
static oc_dbres *process_redeem_job(oc_dbwriter *w, const oc_job *j);

/* A sign-up from the pages is a guess at an invitation like any other, so its
 * source is limited as a sign-in's is; a refusal is the page's. */
static oc_dbres *process_redeem(oc_dbwriter *w, const oc_job *j) {
    int limited = 0;
    if (j->web && j->source[0] && oc_ratelimit_blocked(w->source_rl, src_key(j->source), dbw_now_ms()))
        limited = OC_ERR_AUTH_RATE_LIMITED;
    else if (j->web && j->source[0] && w->signup_rl &&
             oc_ratelimit_blocked(w->signup_rl, src_key(j->source), dbw_now_ms()))
        limited = OC_ERR_SIGNUP_LIMITED;
    if (limited) {
        oc_dbres *r = calloc(1, sizeof *r);
        if (!r) return NULL;
        r->conn_id = j->conn_id;
        r->type = OC_RES_WEB_ERR; r->err_code = limited;
        return r;
    }
    oc_dbres *r = process_redeem_job(w, j);
    if (r && j->web && r->type == OC_RES_AUTH_ERR) {
        r->type = OC_RES_WEB_ERR;
        if (r->err_code == OC_ERR_AUTH_INVALID_TOKEN && j->source[0])
            oc_ratelimit_record(w->source_rl, src_key(j->source), dbw_now_ms());
    } else if (r && j->web && j->source[0] && w->signup_rl && r->type != OC_RES_WEB_ERR) {
        oc_ratelimit_record(w->signup_rl, src_key(j->source), dbw_now_ms());   /* an account made */
    }
    return r;
}

static oc_dbres *process_redeem_job(oc_dbwriter *w, const oc_job *j) {
    sqlite3 *db = w->db;
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;

    /* A bearer invite makes a LOCAL account. Where local accounts are off, it would
     * be the phishable credential the provider is there to remove (AUTH.md §8.4). */
    if (!(w->auth_methods & OC_AUTH_LOCAL)) {
        r->type = OC_RES_AUTH_ERR; r->err_code = OC_ERR_AUTH_REQUIRED; return r;
    }
    /* Sign-up happens on the daemon's pages (AUTH.md §8.10); nothing else
     * makes this job. */
    if (!j->web) {
        r->type = OC_RES_AUTH_ERR; r->err_code = OC_ERR_AUTH_SOURCE_UNAVAILABLE; return r;
    }
    uint8_t hash[OC_SHA256_LEN];
    sqlite3_stmt *st = NULL;
    uint8_t role = OC_ROLE_MEMBER; uint64_t expiry = 0; int consumed = 1, found = 0;
    /* NULL created_by is the first-run setup token (REQ-024, mint_invite's own
     * created_by==0 -> NULL convention) — the signal that this redemption is an
     * owner bootstrap, not an ordinary invite; role alone can't tell them apart,
     * since an owner may also invite another owner at an ordinary role. */
    uint64_t created_by = 0;
    int uses = 1, used = 0;
    /* A demo workspace's visitor (AUTH.md §8.12): central's signed
     * word, under its published keys, for this workspace, for a member, once.
     * Its id is kept as a spent invite (the hash of "demo-invite:<jti>", which
     * no 32-byte token can match), so a restart does not make it new again. */
    oc_jwt_invite di = { 0 };
    if (j->demo_invite) {
        if (!g_dbw_demo || !w->oidc_issuer || !w->oidc_audience) {
            r->type = OC_RES_AUTH_ERR; r->err_code = OC_ERR_AUTH_INVALID_TOKEN; return r;
        }
        pthread_mutex_lock(&w->keys_mu);
        oc_jwt_result vr = w->oidc_keyset_pem
            ? oc_jwt_verify_demo_invite(j->token, j->token_len, w->oidc_keyset_pem, strlen(w->oidc_keyset_pem) + 1,
                                        w->oidc_issuer, w->oidc_audience, dbw_now_ms() / 1000u, &di)
            : OC_JWT_E_KEY;
        pthread_mutex_unlock(&w->keys_mu);
        char key[OC_JWT_MAX_SHORT + 16];
        snprintf(key, sizeof key, "demo-invite:%s", di.jti);
        if (vr != OC_JWT_OK || oc_sha256(key, strlen(key), hash) != 0) {
            r->type = OC_RES_AUTH_ERR; r->err_code = OC_ERR_AUTH_INVALID_TOKEN; return r;
        }
        sqlite3_prepare_v2(db, "SELECT 1 FROM invites WHERE token_hash=?;", -1, &st, NULL);
        sqlite3_bind_blob(st, 1, hash, sizeof hash, SQLITE_STATIC);
        int spent = sqlite3_step(st) == SQLITE_ROW;
        sqlite3_finalize(st);
        if (spent) { r->type = OC_RES_AUTH_ERR; r->err_code = OC_ERR_AUTH_INVALID_TOKEN; return r; }
        role = OC_ROLE_MEMBER; created_by = UINT64_MAX;   /* nobody here invited them */
    } else {
    if (j->token_len != OC_INVITE_TOKEN_LEN) {
        r->type = OC_RES_AUTH_ERR; r->err_code = OC_ERR_AUTH_INVALID_TOKEN; return r;
    }
    if (oc_sha256((const uint8_t *)j->token, j->token_len, hash) != 0) {
        r->type = OC_RES_AUTH_ERR; r->err_code = OC_ERR_AUTH_INVALID_TOKEN; return r;
    }
    sqlite3_prepare_v2(db,
        "SELECT role, expires_at_ms, consumed_at_ms, created_by, uses, used FROM invites "
        "WHERE token_hash=? AND email IS NULL;",
        -1, &st, NULL);
    sqlite3_bind_blob(st, 1, hash, sizeof hash, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) {
        role       = role_to_u8((const char *)sqlite3_column_text(st, 0));
        expiry     = (uint64_t)sqlite3_column_int64(st, 1);
        consumed   = sqlite3_column_type(st, 2) != SQLITE_NULL;
        created_by = sqlite3_column_type(st, 3) != SQLITE_NULL
                     ? (uint64_t)sqlite3_column_int64(st, 3) : 0;
        uses       = sqlite3_column_int(st, 4);
        used       = sqlite3_column_int(st, 5);
        found = 1;
    }
    sqlite3_finalize(st);
    if (!found || consumed || (expiry != 0 && dbw_now_ms() >= expiry)) {
        r->type = OC_RES_AUTH_ERR; r->err_code = OC_ERR_AUTH_INVALID_TOKEN; return r;
    }
    }
    /* A setup token makes an owner only for a workspace with none who can sign
     * in: one printed before the first owner signed up makes no second. */
    if (!created_by && count_active_owners(db) > 0) {
        spend_setup_tokens(db);
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

    /* A demo visitor may not take the name of someone who runs the demo: an
     * owner's or an admin's, ignoring case and the spaces around it (AUTH.md
     * §8.12). The invite is not spent. */
    if (j->demo_invite && j->display) {
        sqlite3_prepare_v2(db, "SELECT 1 FROM users WHERE role IN ('owner','admin') "
                               "AND lower(trim(display_name)) = lower(trim(?));", -1, &st, NULL);
        sqlite3_bind_text(st, 1, j->display, -1, SQLITE_TRANSIENT);
        int reserved = sqlite3_step(st) == SQLITE_ROW;
        sqlite3_finalize(st);
        if (reserved) { r->type = OC_RES_AUTH_ERR; r->err_code = OC_ERR_GROUP_HANDLE_TAKEN; return r; }
    }

    /* Registered-user cap (AUTH.md §2): a redeem that would create a new member is
     * refused once the workspace is at its limit. */
    if (user_slots_full(db, subject, sublen, w->max_users)) {
        r->type = OC_RES_AUTH_ERR; r->err_code = OC_ERR_USER_LIMIT; return r;
    }

    uint64_t uid = register_local(db, user, strlen(user), pass, strlen(pass), role, j->mem_kib ? j->mem_kib : w->pw_mem);
    if (uid == 0) { r->type = OC_RES_AUTH_ERR; r->err_code = OC_ERR_INTERNAL; return r; }

    if (j->demo_invite) {
        /* Spent at once, as a consumed member invite of one use. */
        sqlite3_prepare_v2(db, "INSERT INTO invites(token_hash, created_by, role, expires_at_ms, consumed_at_ms, uses, used, "
                               "created_at_ms) VALUES(?, NULL, 'member', ?, ?, 1, 1, ?3);", -1, &st, NULL);
        sqlite3_bind_blob(st, 1, hash, sizeof hash, SQLITE_STATIC);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)(di.exp * 1000u));
        sqlite3_bind_int64(st, 3, (sqlite3_int64)dbw_now_ms());
        sqlite3_step(st);
        sqlite3_finalize(st);
        /* The name the visitor gave, in place of the username made from it. */
        if (j->display && j->display[0]) {
            sqlite3_prepare_v2(db, "UPDATE users SET display_name=? WHERE id=?;", -1, &st, NULL);
            sqlite3_bind_text(st, 1, j->display, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(st, 2, (sqlite3_int64)uid);
            sqlite3_step(st);
            sqlite3_finalize(st);
        }
        /* Entering the demo is agreeing to its terms; this entry is when. */
        audit_log(db, OC_AUDIT_ACCOUNT, "user.demo_join", 0, NULL, uid, user, 1, "via=browser terms=accepted");
        return web_finish(w, j, r, uid);
    }
    /* Take one use; the last one consumes the invite (REQ-026). The single
     * writer thread makes this atomic with the account creation above. */
    int last = uses != 0 && used + 1 >= uses;
    sqlite3_prepare_v2(db, "UPDATE invites SET used=used+1, consumed_at_ms=COALESCE(?,consumed_at_ms) "
                           "WHERE token_hash=?;", -1, &st, NULL);
    if (last) sqlite3_bind_int64(st, 1, (sqlite3_int64)dbw_now_ms());
    else      sqlite3_bind_null(st, 1);
    sqlite3_bind_blob(st, 2, hash, sizeof hash, SQLITE_STATIC);
    sqlite3_step(st);
    sqlite3_finalize(st);

    /* From the sign-up page: the account is made, and the person goes on
     * signed in, with an ID token like a sign-in's. Two distinct actions
     * (REQ-251), not one "invite consumed": a bootstrap has no inviting actor
     * to name (mint_invite's created_by is NULL), an ordinary invite always does. */
    if (created_by) audit_actor(db, OC_AUDIT_ACCOUNT, "invite.redeem", uid, 0, NULL, 1, "via=browser");
    else            audit_log(db, OC_AUDIT_ACCOUNT, "user.bootstrap", 0, NULL, uid, user, 1, "via=browser");
    return web_finish(w, j, r, uid);
}

/* A demo workspace's sample content (AUTH.md §8.12), written once, at its
 * first start: what the demo is, in #general, and two more channels with a
 * little in each, by "OpenChime Demo" -- an account nobody signs in as (no
 * credential, removed), so nobody can edit its messages, and only a moderator
 * delete them. A workspace that has it already is left alone. */
static oc_dbres *process_seed_demo(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->type = OC_RES_OK;
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "SELECT 1 FROM users WHERE subject='demo:openchime';", -1, &st, NULL);
    int have = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    if (have) return r;

    const char *ws = j->ch_name && j->ch_name[0] ? j->ch_name : "this workspace";
    int wl = utf8_prefix_len(ws, 64);
    uint64_t now = dbw_now_ms();
    sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, NULL);
    sqlite3_prepare_v2(db, "INSERT INTO users(subject, display_name, role, created_at_ms, disabled) "
                           "VALUES('demo:openchime', 'OpenChime Demo', 'member', ?, 1);", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)now);
    int ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    uint64_t author = (uint64_t)sqlite3_last_insert_rowid(db);

    static const struct { const char *name, *topic, *descr; } CH[] = {
        { "general", "The public demo of OpenChime. Everything here is erased every night.",
          "This is a public demo: everyone who signs up can read and post here. It is reset every night "
          "at midnight UTC, and some things have daily limits so the demo stays usable for everyone." },
        { "random", "Anything at all.", "Say hello, try a reaction, start a thread." },
        { "feedback", "Tell us what you think.", "What worked, what didn't, what you would want. We read it." },
    };
    uint64_t cid[3] = { OC_DEFAULT_CHANNEL, 0, 0 };
    for (int i = 0; ok && i < 3; i++) {
        if (i == 0)
            sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO channels(id,kind,name,is_public,created_at_ms) "
                                   "VALUES(1,'channel','general',1,?);", -1, &st, NULL);
        else
            sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO channels(kind,name,is_public,created_at_ms) "
                                   "VALUES('channel',?,1,?);", -1, &st, NULL);
        if (i == 0) sqlite3_bind_int64(st, 1, (sqlite3_int64)now);
        else { sqlite3_bind_text(st, 1, CH[i].name, -1, SQLITE_STATIC); sqlite3_bind_int64(st, 2, (sqlite3_int64)now); }
        ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
        sqlite3_prepare_v2(db, "SELECT id FROM channels WHERE kind='channel' AND name=?;", -1, &st, NULL);
        sqlite3_bind_text(st, 1, CH[i].name, -1, SQLITE_STATIC);
        if (ok && sqlite3_step(st) == SQLITE_ROW) cid[i] = (uint64_t)sqlite3_column_int64(st, 0);
        else ok = 0;
        sqlite3_finalize(st);
        sqlite3_prepare_v2(db, "UPDATE channels SET topic=?, description=? WHERE id=?;", -1, &st, NULL);
        sqlite3_bind_text(st, 1, CH[i].topic, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, CH[i].descr, -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 3, (sqlite3_int64)cid[i]);
        if (ok) ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
    }
    char welcome[512];
    snprintf(welcome, sizeof welcome,
             "Welcome to %.*s, the public demo of OpenChime. Post, react, start a thread, share a file "
             "or start a call: everything works, within a few daily limits. Anyone who signs up sees what "
             "you write here, and it is all erased every night.", wl, ws);
    const struct { int ch; const char *body; } MSG[] = {
        { 0, welcome },
        { 0, "Try the read-aloud and voice input from the composer, and the summary of a channel from its menu." },
        { 0, "Please be kind: this is a shared space, and the people here are trying it out like you." },
        { 1, "This is #random. Anything goes, within reason." },
        { 2, "What did you like, and what was missing? Say so here." },
    };
    for (size_t i = 0; ok && i < sizeof MSG / sizeof *MSG; i++) {
        sqlite3_prepare_v2(db, "INSERT INTO messages(channel_id, author_id, body, created_at_ms) VALUES(?,?,?,?);",
                           -1, &st, NULL);
        sqlite3_bind_int64(st, 1, (sqlite3_int64)cid[MSG[i].ch]);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)author);
        sqlite3_bind_text(st, 3, MSG[i].body, -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 4, (sqlite3_int64)(now - (sizeof MSG / sizeof *MSG - i) * 60000u));
        ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
    }
    sqlite3_exec(db, ok ? "COMMIT;" : "ROLLBACK;", NULL, NULL, NULL);
    if (!ok) return r;                    /* channel_id 0: not written */
    r->channel_id = OC_DEFAULT_CHANNEL;   /* written now */
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
    sqlite3_prepare_v2(db, "SELECT cert_pem, key_pem FROM ident.server_identity WHERE id=1;", -1, &st, NULL);
    if (sqlite3_step(st) == SQLITE_ROW) {
        r->cert_pem = strdup((const char *)sqlite3_column_text(st, 0));
        r->key_pem  = strdup((const char *)sqlite3_column_text(st, 1));
    }
    sqlite3_finalize(st);
    return r;
}

void oc_tls_state_free(oc_tls_state *s) {
    if (!s) return;
    free(s->acme_directory); free(s->acme_url); free(s->source); free(s->names); free(s->chain_pem);
    if (s->acme_key_pem) { memset(s->acme_key_pem, 0, strlen(s->acme_key_pem)); free(s->acme_key_pem); }
    if (s->key_pem) { memset(s->key_pem, 0, strlen(s->key_pem)); free(s->key_pem); }
    memset(s, 0, sizeof *s);
}

static char *col_dup(sqlite3_stmt *st, int i) {
    const unsigned char *t = sqlite3_column_text(st, i);
    return t ? strdup((const char *)t) : NULL;
}

/* The kept ACME account and CA-issued certificate (migration 0049). */
static oc_dbres *process_load_tls_state(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->type = OC_RES_TLS_STATE;
    oc_tls_state *s = r->tls_state = calloc(1, sizeof *s);
    if (!s) return r;
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "SELECT directory_url, account_key_pem, account_url FROM ident.acme_account WHERE id=1;", -1, &st, NULL);
    if (sqlite3_step(st) == SQLITE_ROW) {
        s->acme_directory = col_dup(st, 0); s->acme_key_pem = col_dup(st, 1); s->acme_url = col_dup(st, 2);
    }
    sqlite3_finalize(st); st = NULL;
    sqlite3_prepare_v2(db, "SELECT source, names, chain_pem, key_pem, issued_at_ms, not_after_ms "
                           "FROM ident.tls_certificate WHERE id=1;", -1, &st, NULL);
    if (sqlite3_step(st) == SQLITE_ROW) {
        s->source = col_dup(st, 0); s->names = col_dup(st, 1); s->chain_pem = col_dup(st, 2); s->key_pem = col_dup(st, 3);
        s->issued_ms = (uint64_t)sqlite3_column_int64(st, 4); s->not_after_ms = (uint64_t)sqlite3_column_int64(st, 5);
    }
    sqlite3_finalize(st);
    return r;
}

/* Keep what was obtained; nothing is answered (a background worker's write). */
static oc_dbres *process_store_tls(sqlite3 *db, const oc_job *j) {
    const oc_tls_state *s = j->tls_state;
    if (!s) return NULL;
    sqlite3_stmt *st = NULL;
    if (j->type == OC_JOB_STORE_ACME_ACCOUNT) {
        sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO ident.acme_account(id,directory_url,account_key_pem,account_url,created_at_ms) "
                               "VALUES(1,?,?,?,?);", -1, &st, NULL);
        sqlite3_bind_text(st, 1, s->acme_directory ? s->acme_directory : "", -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, s->acme_key_pem ? s->acme_key_pem : "", -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 3, s->acme_url ? s->acme_url : "", -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 4, (sqlite3_int64)dbw_now_ms());
    } else {
        sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO ident.tls_certificate(id,source,names,chain_pem,key_pem,issued_at_ms,not_after_ms) "
                               "VALUES(1,?,?,?,?,?,?);", -1, &st, NULL);
        sqlite3_bind_text(st, 1, s->source ? s->source : "", -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, s->names ? s->names : "", -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 3, s->chain_pem ? s->chain_pem : "", -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 4, s->key_pem ? s->key_pem : "", -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 5, (sqlite3_int64)s->issued_ms);
        sqlite3_bind_int64(st, 6, (sqlite3_int64)s->not_after_ms);
    }
    if (sqlite3_step(st) != SQLITE_DONE)
        fprintf(stderr, "openchimed: could not keep the TLS certificate: %s\n", sqlite3_errmsg(db));
    sqlite3_finalize(st);
    return NULL;
}

/* Persist the TLS identity so a database restored onto a new box keeps the same
 * trusted certificate. */
static oc_dbres *process_store_identity(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO ident.server_identity(id,cert_pem,key_pem,created_at_ms) "
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

/* Load the persisted enrollment identity (ARCH-84); enroll_present=0 when
 * none is stored yet (first run). */
static oc_dbres *process_load_enrollment(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->type = OC_RES_ENROLLMENT;
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "SELECT privkey_pem, audience, state FROM ident.enrollment WHERE id=1;", -1, &st, NULL);
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

/* Persist the enrollment keypair + audience + state (ARCH-84). */
static oc_dbres *process_store_enrollment(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO ident.enrollment(id,privkey_pem,audience,state,activated_at_ms,created_at_ms) "
        "VALUES(1,?,?,?,?,?);", -1, &st, NULL);
    sqlite3_bind_text(st, 1, j->enroll_privkey  ? j->enroll_privkey  : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, j->enroll_audience ? j->enroll_audience : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, j->enroll_active ? "active" : "pending", -1, SQLITE_STATIC);
    if (j->enroll_active) sqlite3_bind_int64(st, 4, (sqlite3_int64)dbw_now_ms());
    else sqlite3_bind_null(st, 4);
    sqlite3_bind_int64(st, 5, (sqlite3_int64)dbw_now_ms());
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (j->enroll_quiet) { free(r); return NULL; }   /* push_result ignores NULL */
    if (rc == SQLITE_DONE) r->type = OC_RES_OK;
    else { r->err_code = OC_ERR_INTERNAL; }
    return r;
}

/* A message changed (sent, edited, deleted, restored): every summary built on
 * it, or on its thread, or covering its moment, goes (REQ-310, ARCH-116). In
 * the transaction that changed it, so a summary never outlives what it says. */
static void sum_purge_msg(sqlite3 *db, uint64_t message_id) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT channel_id, COALESCE(parent_id, id), created_at_ms FROM messages WHERE id=?1;",
                           -1, &st, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)message_id);
    if (sqlite3_step(st) == SQLITE_ROW)
        oc_sum_purge(db, sqlite3_column_int64(st, 0), (int64_t)message_id, sqlite3_column_int64(st, 1),
                     sqlite3_column_int64(st, 2));
    sqlite3_finalize(st);
}

/* Remove a member from the tenant (REQ-033): deactivate them. What goes is the
 * account's reach -- sign-in, sessions, the local password, channel and group
 * memberships, pushes, webhooks, drafts and scheduled messages. What stays is
 * everything anyone said: every message, file, reaction and pin, and every DM
 * and group DM they were in, whole, with them still in it, so the others keep
 * the conversation and opening a DM with them again finds it. The row survives,
 * `disabled`, which clients show as deactivated. Owner/admin only; an admin
 * cannot remove an admin/owner, and the last owner cannot be removed. */
/* An account deactivated (REQ-033): nothing is deleted. The person is marked
 * disabled and signed out everywhere; out of every group and every channel but
 * their DMs; their password, webhooks, devices, drafts and scheduled messages
 * gone. A removal and a person's own deactivation are this one act, told apart
 * in the audit log by `action`, done by `actor`. */
static void deactivate_user(sqlite3 *db, uint64_t uid, uint64_t actor, const char *action) {
    sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "UPDATE users SET disabled=1 WHERE id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)uid); sqlite3_step(st); sqlite3_finalize(st);
    sqlite3_prepare_v2(db, "DELETE FROM sessions WHERE user_id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)uid); sqlite3_step(st); sqlite3_finalize(st);
    /* Out of every group first, so the rule leaves them in no channel; then out
     * of every channel but the DMs and group DMs, which keep them as a member so
     * the conversation stays whole for everyone else in it. */
    sqlite3_prepare_v2(db, "DELETE FROM user_group_members WHERE user_id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)uid); sqlite3_step(st); sqlite3_finalize(st);
    {
        size_t nch = 0;
        uint64_t *chans = ids_of(db,
            "SELECT cm.channel_id FROM channel_members cm JOIN channels c ON c.id = cm.channel_id "
            " WHERE cm.user_id=?1 AND c.kind <> 'dm';",
            uid, &nch);
        for (size_t c = 0; c < nch; c++) membership_sync(db, chans[c], uid, 0);
        free(chans);
    }
    sqlite3_prepare_v2(db, "DELETE FROM local_credentials WHERE user_id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)uid); sqlite3_step(st); sqlite3_finalize(st);
    /* What acts for them when they are not there stops too: their webhooks post
     * in their name, and their devices would keep being woken for the workspace. */
    sqlite3_prepare_v2(db, "UPDATE webhooks SET disabled=1 WHERE creator_id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)uid); sqlite3_step(st); sqlite3_finalize(st);
    sqlite3_prepare_v2(db, "DELETE FROM device_tokens WHERE user_id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)uid); sqlite3_step(st); sqlite3_finalize(st);
    /* Their drafts go with them (REQ-223, ARCH-101): unsent, so nobody else's. */
    sqlite3_prepare_v2(db, "DELETE FROM drafts WHERE user_id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)uid); sqlite3_step(st); sqlite3_finalize(st);
    /* And anything they had scheduled (REQ-224): the account is gone, so the
     * promise cannot be kept and there is nobody left to tell. */
    sqlite3_prepare_v2(db, "DELETE FROM scheduled_messages WHERE user_id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)uid); sqlite3_step(st); sqlite3_finalize(st);
    /* And the AI clients connected as them (AUTH.md §8.13): revoked, their
     * tokens gone, so not one more request is answered as them. */
    sqlite3_prepare_v2(db, "DELETE FROM mcp_access_tokens WHERE connection_id IN"
                           " (SELECT id FROM mcp_connections WHERE user_id=?);", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)uid); sqlite3_step(st); sqlite3_finalize(st);
    sqlite3_prepare_v2(db, "UPDATE mcp_connections SET revoked_at_ms=?2, refresh_hash=NULL"
                           " WHERE user_id=?1 AND revoked_at_ms=0;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)uid); sqlite3_bind_int64(st, 2, (sqlite3_int64)dbw_now_ms());
    sqlite3_step(st); sqlite3_finalize(st);
    sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);

    audit_actor(db, OC_AUDIT_ADMIN, action, actor, uid, NULL, 1, NULL);
}

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
    if (target_role == OC_ROLE_OWNER && count_active_owners(db) <= 1) {
        r->type = OC_RES_USER_ERR; r->err_code = OC_ERR_LAST_OWNER; return r;
    }

    deactivate_user(db, j->target_user_id, j->user_id, "user.remove");

    r->type = OC_RES_USER_UPDATED;
    r->user_id = j->target_user_id;
    r->role = target_role;
    r->disabled = 1;
    return r;
}

/* A person deactivates their own account: a removal's state exactly, recorded
 * as their own act. The last active owner may not leave the workspace without
 * one (LAST_OWNER): another owner is needed first. */
static oc_dbres *process_deactivate_self(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    uint8_t role = OC_ROLE_MEMBER;
    if (!user_role(db, j->user_id, &role) || user_disabled(db, j->user_id)) {
        r->type = OC_RES_USER_ERR; r->err_code = OC_ERR_FORBIDDEN; return r;
    }
    if (role == OC_ROLE_OWNER && count_active_owners(db) <= 1) {
        r->type = OC_RES_USER_ERR; r->err_code = OC_ERR_LAST_OWNER; return r;
    }
    deactivate_user(db, j->user_id, j->user_id, "account.deactivate_self");
    r->type = OC_RES_USER_UPDATED;
    r->user_id = j->user_id;
    r->role = role;
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

static void withhold_from_blockers(sqlite3 *db, uint64_t channel_id, uint64_t author, uint64_t mid,
                                   oc_dbres *r);   /* below, with the blocks */

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

/* Answer ARCH-104's participation question for every member already in
 * r->members, so the net thread can stamp THREAD_REPLY's per-recipient byte
 * without a database of its own. One prepared statement stepped per member: the
 * audience is a channel's membership, and the alternative — a second copy of the
 * predicate shaped as a join — is the duplication OC_THREAD_PARTICIPANT_SQL
 * exists to prevent.
 *
 * On allocation failure the array stays NULL, which the fan-out reads as "nobody
 * is a participant": a missed toast, not a wrong one. */
static uint8_t thread_participant(sqlite3 *db, uint64_t user_id, uint64_t root_id) {
    if (!root_id) return 0;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT " OC_THREAD_PARTICIPANT_SQL("?1", "?2") ";",
                           -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)user_id);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)root_id);
    uint8_t in = (sqlite3_step(st) == SQLITE_ROW) ? (uint8_t)(sqlite3_column_int(st, 0) != 0) : 0;
    sqlite3_finalize(st);
    return in;
}

static void load_thread_participation(sqlite3 *db, uint64_t root_id, oc_dbres *r) {
    if (!r->members || !r->n_members || !root_id) return;
    uint8_t *flags = calloc(r->n_members, sizeof *flags);
    if (!flags) return;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT " OC_THREAD_PARTICIPANT_SQL("?1", "?2") ";",
            -1, &st, NULL) != SQLITE_OK) { free(flags); return; }
    for (size_t i = 0; i < r->n_members; i++) {
        sqlite3_reset(st);
        sqlite3_bind_int64(st, 1, (sqlite3_int64)r->members[i]);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)root_id);
        if (sqlite3_step(st) == SQLITE_ROW)
            flags[i] = (uint8_t)(sqlite3_column_int(st, 0) != 0);
    }
    sqlite3_finalize(st);
    r->member_participant = flags;
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

/* Added directly: joined, invited, created, posted into, a DM. */
static void add_membership(sqlite3 *db, uint64_t channel_id, uint64_t user_id) {
    membership_sync(db, channel_id, user_id, 1);
}

/* --- Membership, and user groups (REQ-307-309, ARCH-114) ----------------------
 *
 * channel_members is a channel's EFFECTIVE membership: what every access check,
 * fan-out, push and badge query reads. A row exists exactly when the person was
 * added directly (`direct`), or is in a group the channel has (channel_groups).
 * membership_sync is the one place a row is written or deleted, and it writes
 * that rule, so no sequence of joins, leaves, group edits and attachments can
 * leave a row the rule would not. */

/* Is `user_id` in a group that `channel_id` has? */
static int member_via_group(sqlite3 *db, uint64_t channel_id, uint64_t user_id) {
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "SELECT 1 FROM channel_groups cg JOIN user_group_members gm ON gm.group_id = cg.group_id "
        "WHERE cg.channel_id = ?1 AND gm.user_id = ?2 LIMIT 1;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)channel_id);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)user_id);
    int via = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    return via;
}

/* In the channel only through a group: a row, not added directly, which a
 * group the channel has accounts for. What leave and remove refuse (REQ-309). */
static int member_only_via_group(sqlite3 *db, uint64_t channel_id, uint64_t user_id) {
    sqlite3_stmt *st = NULL;
    int only = 0;
    sqlite3_prepare_v2(db, "SELECT direct FROM channel_members WHERE channel_id=? AND user_id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)channel_id);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)user_id);
    if (sqlite3_step(st) == SQLITE_ROW) only = sqlite3_column_int(st, 0) == 0;
    sqlite3_finalize(st);
    return only && member_via_group(db, channel_id, user_id);
}

/* Set the direct half of (channel, user) -- 1 added, 0 not, -1 as it is -- and
 * make the row what the rule says. +1 if the person gained the channel, -1 if
 * they lost it, 0 if their membership did not change. A row that stays keeps
 * the time its person joined. */
static int membership_sync(sqlite3 *db, uint64_t channel_id, uint64_t user_id, int direct) {
    sqlite3_stmt *st = NULL;
    int exists = 0, cur_direct = 0;
    sqlite3_prepare_v2(db, "SELECT direct FROM channel_members WHERE channel_id=? AND user_id=?;",
                       -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)channel_id);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)user_id);
    if (sqlite3_step(st) == SQLITE_ROW) { exists = 1; cur_direct = sqlite3_column_int(st, 0); }
    sqlite3_finalize(st);
    int want_direct = direct < 0 ? (exists && cur_direct) : direct;
    int want = want_direct || member_via_group(db, channel_id, user_id);
    if (want && !exists) {
        sqlite3_prepare_v2(db,
            "INSERT INTO channel_members(channel_id,user_id,joined_at_ms,direct) VALUES(?,?,?,?);",
            -1, &st, NULL);
        sqlite3_bind_int64(st, 1, (sqlite3_int64)channel_id);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)user_id);
        sqlite3_bind_int64(st, 3, (sqlite3_int64)dbw_now_ms());
        sqlite3_bind_int(st, 4, want_direct);
        int own = owh_txn_begin(db);
        sqlite3_step(st);
        sqlite3_finalize(st);
        owh_member(db, channel_id, user_id, 1);   /* in the same transaction */
        owh_txn_end(db, own);
        return 1;
    }
    if (want && cur_direct != want_direct) {
        sqlite3_prepare_v2(db, "UPDATE channel_members SET direct=? WHERE channel_id=? AND user_id=?;",
                           -1, &st, NULL);
        sqlite3_bind_int(st, 1, want_direct);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)channel_id);
        sqlite3_bind_int64(st, 3, (sqlite3_int64)user_id);
        sqlite3_step(st);
        sqlite3_finalize(st);
        return 0;
    }
    if (!want && exists) {
        int own = owh_txn_begin(db);
        sqlite3_prepare_v2(db, "DELETE FROM channel_members WHERE channel_id=? AND user_id=?;", -1, &st, NULL);
        sqlite3_bind_int64(st, 1, (sqlite3_int64)channel_id);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)user_id);
        sqlite3_step(st);
        sqlite3_finalize(st);
        /* Out of the channel is out of its management too (REQ-036b), and
         * coming back does not bring the role back. */
        sqlite3_prepare_v2(db, "DELETE FROM channel_managers WHERE channel_id=? AND user_id=?;", -1, &st, NULL);
        sqlite3_bind_int64(st, 1, (sqlite3_int64)channel_id);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)user_id);
        sqlite3_step(st);
        sqlite3_finalize(st);
        owh_member(db, channel_id, user_id, 0);
        owh_txn_end(db, own);
        return -1;
    }
    return 0;
}

/* The ids a query returns in its first column, into a heap array. */
static uint64_t *ids_of(sqlite3 *db, const char *sql, uint64_t a, size_t *n_out) {
    sqlite3_stmt *st = NULL;
    *n_out = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)a);
    size_t cap = 16, n = 0;
    uint64_t *arr = malloc(cap * sizeof *arr);
    while (arr && sqlite3_step(st) == SQLITE_ROW) {
        if (n == cap) { cap *= 2; uint64_t *g = realloc(arr, cap * sizeof *arr); if (!g) break; arr = g; }
        arr[n++] = (uint64_t)sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    *n_out = n;
    return arr;
}

/* ---- Outgoing webhooks (docs/details/WEBHOOKS.md) -------------------------
 * An owner or admin points the workspace at an https address and names the
 * events it is sent and the channels they come from. Each event a webhook
 * receives is queued here, as a row of webhook_deliveries, in the transaction
 * that wrote the event; the delivery worker (daemon/webhooks_out.c) takes the
 * rows due, signs and sends them, and says how each went. */

/* The events a webhook can be sent, in the order a list shows them. */
static const char *const OWH_EVENTS[] = {
    "message", "reaction_added", "reaction_removed", "member_joined_channel",
    "member_left_channel", "channel_rename", "channel_archive", "channel_unarchive",
    "user_change",
};
#define OWH_N_EVENTS (sizeof OWH_EVENTS / sizeof OWH_EVENTS[0])

/* A growing string; `bad` once an allocation failed, and then it stays empty. */
typedef struct { char *p; size_t n, cap; int bad; } owh_sb;

static void sb_add(owh_sb *b, const char *s, size_t n) {
    if (b->bad) return;
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->n + n + 1) cap *= 2;
        char *g = realloc(b->p, cap);
        if (!g) { b->bad = 1; return; }
        b->p = g; b->cap = cap;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = '\0';
}
static void sb_str(owh_sb *b, const char *s) { sb_add(b, s, strlen(s)); }
static void sb_fmt(owh_sb *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void sb_fmt(owh_sb *b, const char *fmt, ...) {
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof tmp) { b->bad = 1; return; }
    sb_add(b, tmp, (size_t)n);
}
/* `s` as a JSON string, quoted. */
static void sb_json(owh_sb *b, const char *s, size_t n) {
    sb_add(b, "\"", 1);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') { char e[2] = { '\\', (char)c }; sb_add(b, e, 2); }
        else if (c == '\n') sb_add(b, "\\n", 2);
        else if (c == '\r') sb_add(b, "\\r", 2);
        else if (c == '\t') sb_add(b, "\\t", 2);
        else if (c < 0x20) sb_fmt(b, "\\u%04x", c);
        else sb_add(b, (const char *)&c, 1);
    }
    sb_add(b, "\"", 1);
}
static void sb_jstr(owh_sb *b, const char *s) { sb_json(b, s ? s : "", s ? strlen(s) : 0); }

/* Whether `url` is an address a webhook may have: https, a host, no space or
 * control character. Any host: the address is the owner's or admin's choice. */
static int owh_url_ok(const char *url) {
    if (!url) return 0;
    size_t n = strlen(url);
    if (n <= 8 || n > 2048 || strncasecmp(url, "https://", 8) != 0) return 0;
    if (url[8] == '/' || url[8] == '?' || url[8] == '#') return 0;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)url[i] <= 0x20 || url[i] == 0x7f) return 0;
    return 1;
}

/* The event names in `in` (separated by spaces) into `out`, each once and in
 * the catalog's order. 0, or -1 for an empty list or a name not allowed. */
static int owh_events_canon(const char *in, char *out, size_t cap) {
    int want[OWH_N_EVENTS] = { 0 };
    int any = 0;
    const char *p = in ? in : "";
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *e = p;
        while (*e && *e != ' ') e++;
        size_t k = 0;
        for (; k < OWH_N_EVENTS; k++)
            if (strlen(OWH_EVENTS[k]) == (size_t)(e - p) && memcmp(OWH_EVENTS[k], p, (size_t)(e - p)) == 0) break;
        if (k == OWH_N_EVENTS) return -1;
        want[k] = 1; any = 1;
        p = e;
    }
    if (!any) return -1;
    size_t n = 0;
    out[0] = '\0';
    for (size_t k = 0; k < OWH_N_EVENTS; k++) {
        if (!want[k]) continue;
        int w = snprintf(out + n, cap - n, "%s%s", n ? " " : "", OWH_EVENTS[k]);
        if (w < 0 || (size_t)w >= cap - n) return -1;
        n += (size_t)w;
    }
    return 0;
}

/* The workspace's id in an envelope: `T` and its audience id's short form
 * (the `ws_` dropped), or `T0` when it is not enrolled. */
static void owh_team(sqlite3 *db, char *out, size_t cap) {
    snprintf(out, cap, "T0");
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT audience FROM ident.enrollment WHERE id=1;", -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0)) {
        const char *a = (const char *)sqlite3_column_text(st, 0);
        if (strncmp(a, "ws_", 3) == 0) a += 3;
        if (*a) snprintf(out, cap, "T%s", a);
    }
    sqlite3_finalize(st);
}

/* A channel a webhook may watch: a named one, public or private, never a DM or
 * group DM. 1, 0 for one of those, -1 for no channel at all. */
static int owh_channel_named(sqlite3 *db, uint64_t channel_id) {
    sqlite3_stmt *st = NULL;
    int ok = -1;
    if (sqlite3_prepare_v2(db, "SELECT kind='channel' AND e2e=0 FROM channels WHERE id=?1;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)channel_id);
        if (sqlite3_step(st) == SQLITE_ROW) ok = sqlite3_column_int(st, 0) ? 1 : 0;
    }
    sqlite3_finalize(st);
    return ok;
}

/* The enabled webhooks that receive `type` from channel `channel_id` (0: an
 * event of no channel, user_change), into a heap array. */
static uint64_t *owh_targets(sqlite3 *db, uint64_t channel_id, const char *type, size_t *n_out) {
    *n_out = 0;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT id FROM outgoing_webhooks w WHERE enabled=1"
            " AND (' ' || events || ' ') LIKE ('% ' || ?2 || ' %')"
            " AND (?1 = 0 OR EXISTS(SELECT 1 FROM outgoing_webhook_channels c"
            "                       WHERE c.webhook_id=w.id AND c.channel_id=?1))"
            " ORDER BY id;", -1, &st, NULL) != SQLITE_OK) {
        sqlite3_finalize(st);
        return NULL;
    }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)channel_id);
    sqlite3_bind_text(st, 2, type, -1, SQLITE_STATIC);
    uint64_t *ids = NULL;
    size_t n = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        uint64_t *g = realloc(ids, (n + 1) * sizeof *g);
        if (!g) break;
        ids = g;
        ids[n++] = (uint64_t)sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    *n_out = n;
    return ids;
}

/* Who is to receive an event of `type` in `channel_id`: the webhooks, or NULL
 * when none -- then the event is not even built. A DM's and a group DM's
 * events go nowhere. */
static uint64_t *owh_wanted(sqlite3 *db, uint64_t channel_id, const char *type, size_t *n) {
    *n = 0;
    if (channel_id && owh_channel_named(db, channel_id) != 1) return NULL;
    return owh_targets(db, channel_id, type, n);
}

/* Queue `event` (a JSON object) for each of `ids`, in its envelope. The
 * caller holds the transaction that wrote the event. */
static void owh_queue(sqlite3 *db, const uint64_t *ids, size_t n, const char *event) {
    if (!n || !event) return;
    char team[80];
    owh_team(db, team, sizeof team);
    uint64_t now = dbw_now_ms();
    for (size_t i = 0; i < n; i++) {
        uint8_t rnd[10];
        if (oc_rand_bytes(rnd, sizeof rnd) != 0) continue;
        char evid[3 + 2 * sizeof rnd];
        memcpy(evid, "Ev", 2);
        for (size_t k = 0; k < sizeof rnd; k++) snprintf(evid + 2 + 2 * k, 3, "%02x", rnd[k]);
        owh_sb b = { 0 };
        sb_str(&b, "{\"type\":\"event_callback\",\"team_id\":");
        sb_jstr(&b, team);
        sb_fmt(&b, ",\"api_app_id\":\"W%llu\",\"event_id\":\"%s\",\"event_time\":%llu,"
                   "\"authorizations\":[],\"event\":",
               (unsigned long long)ids[i], evid, (unsigned long long)(now / 1000u));
        sb_str(&b, event);
        sb_str(&b, "}");
        sqlite3_stmt *st = NULL;
        if (!b.bad && sqlite3_prepare_v2(db,
                "INSERT INTO webhook_deliveries(webhook_id,event_id,payload,next_at_ms) VALUES(?1,?2,?3,?4);",
                -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)ids[i]);
            sqlite3_bind_text(st, 2, evid, -1, SQLITE_STATIC);
            sqlite3_bind_text(st, 3, b.p, (int)b.n, SQLITE_STATIC);
            sqlite3_bind_int64(st, 4, (sqlite3_int64)now);
            sqlite3_step(st);
        }
        sqlite3_finalize(st);
        free(b.p);
    }
}

/* A channel's type as the convention gives it in a message event. */
static const char *owh_channel_type(sqlite3 *db, uint64_t channel_id) {
    sqlite3_stmt *st = NULL;
    int pub = 1;
    if (sqlite3_prepare_v2(db, "SELECT is_public FROM channels WHERE id=?1;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)channel_id);
        if (sqlite3_step(st) == SQLITE_ROW) pub = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    return pub ? "channel" : "group";
}

/* A body's text as an app reads it: every mention of a person as `<@U42>`,
 * whichever form its author typed, and @here, @channel and @everyone as the
 * convention writes them. A name that names no member is left as written. */
static void owh_text(sqlite3 *db, owh_sb *b, uint64_t channel_id, const char *body, size_t len) {
    oc_mention m[OC_MENTION_MAX];
    size_t found = oc_mention_scan(body, len, m, OC_MENTION_MAX);
    if (found > OC_MENTION_MAX) found = OC_MENTION_MAX;
    owh_sb t = { 0 };
    size_t at = 0;
    for (size_t i = 0; i < found; i++) {
        if (m[i].start < at || m[i].start + m[i].len > len) continue;
        char rep[48] = "";
        if (m[i].kind == OC_MENTION_HERE) snprintf(rep, sizeof rep, "<!here>");
        else if (m[i].kind == OC_MENTION_CHANNEL) snprintf(rep, sizeof rep, "<!channel>");
        else if (m[i].kind == OC_MENTION_EVERYONE) snprintf(rep, sizeof rep, "<!everyone>");
        else if (m[i].kind == OC_MENTION_USER) {
            uint64_t uid = m[i].user_id;
            if (!uid) {
                sqlite3_stmt *q = NULL;
                if (sqlite3_prepare_v2(db,
                        "SELECT u.id FROM users u JOIN channel_members cm ON cm.user_id = u.id "
                        "WHERE cm.channel_id = ?1 AND u.is_system = 0 AND lower(u.display_name) = lower(?2) "
                        "ORDER BY u.id LIMIT 1;", -1, &q, NULL) == SQLITE_OK) {
                    sqlite3_bind_int64(q, 1, (sqlite3_int64)channel_id);
                    sqlite3_bind_text(q, 2, m[i].name, -1, SQLITE_STATIC);
                    if (sqlite3_step(q) == SQLITE_ROW) uid = (uint64_t)sqlite3_column_int64(q, 0);
                }
                sqlite3_finalize(q);
            }
            if (uid) snprintf(rep, sizeof rep, "<@U%llu>", (unsigned long long)uid);
        }
        if (!rep[0]) continue;
        sb_add(&t, body + at, m[i].start - at);
        sb_str(&t, rep);
        at = m[i].start + m[i].len;
    }
    sb_add(&t, body + at, len - at);
    if (t.bad) { sb_json(b, body, len); free(t.p); return; }
    sb_json(b, t.p ? t.p : "", t.n);
    free(t.p);
}

enum { OWH_MSG_NEW, OWH_MSG_CHANGED, OWH_MSG_DELETED };

/* Message `mid` posted, changed or deleted: a `message` event, with the
 * subtype the convention gives it. */
static void owh_message(sqlite3 *db, uint64_t mid, int what) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT m.channel_id, m.author_id, COALESCE(m.body,''), COALESCE(m.parent_id,0), m.kind,"
            "       u.kind <> 'human', EXISTS(SELECT 1 FROM actions a WHERE a.message_id=m.id),"
            "       COALESCE(m.deleted_by, 0)"
            "  FROM messages m JOIN users u ON u.id = m.author_id WHERE m.id=?1;", -1, &st, NULL) != SQLITE_OK) {
        sqlite3_finalize(st);
        return;
    }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)mid);
    if (sqlite3_step(st) != SQLITE_ROW || sqlite3_column_int(st, 4) != (int)OC_MSG_KIND_MESSAGE) {
        sqlite3_finalize(st);
        return;
    }
    uint64_t cid = (uint64_t)sqlite3_column_int64(st, 0), author = (uint64_t)sqlite3_column_int64(st, 1);
    char *body = strdup((const char *)sqlite3_column_text(st, 2));
    size_t blen = body ? strlen(body) : 0;
    uint64_t parent = (uint64_t)sqlite3_column_int64(st, 3);
    int bot = sqlite3_column_int(st, 5), action = sqlite3_column_int(st, 6);
    uint64_t by = (uint64_t)sqlite3_column_int64(st, 7);
    sqlite3_finalize(st);
    size_t n = 0;
    uint64_t *ids = body ? owh_wanted(db, cid, "message", &n) : NULL;
    if (!n) { free(ids); free(body); return; }
    const char *text = body;
    size_t tlen = blen;
    if (action && tlen >= 3 && strncmp(text, "/me", 3) == 0) {    /* the action's words, "/me" gone */
        text += 3; tlen -= 3;
        while (tlen && *text == ' ') { text++; tlen--; }
    }
    const char *ctype = owh_channel_type(db, cid);
    unsigned long long c = (unsigned long long)cid, u = (unsigned long long)author, ts = (unsigned long long)mid;
    owh_sb b = { 0 };
    if (what == OWH_MSG_DELETED) {
        sb_fmt(&b, "{\"type\":\"message\",\"subtype\":\"message_deleted\",\"hidden\":true,"
                   "\"channel\":\"C%llu\",\"channel_type\":\"%s\",\"deleted_ts\":\"%llu\","
                   "\"ts\":\"%llu\",\"event_ts\":\"%llu\"", c, ctype, ts, ts, ts);
        if (by) sb_fmt(&b, ",\"user\":\"U%llu\"", (unsigned long long)by);
        sb_str(&b, "}");
    } else {
        owh_sb m = { 0 };
        sb_str(&m, "{\"type\":\"message\"");
        if (what == OWH_MSG_NEW && bot) sb_str(&m, ",\"subtype\":\"bot_message\"");
        else if (what == OWH_MSG_NEW && action) sb_str(&m, ",\"subtype\":\"me_message\"");
        if (what == OWH_MSG_NEW) sb_fmt(&m, ",\"channel\":\"C%llu\",\"channel_type\":\"%s\"", c, ctype);
        sb_fmt(&m, ",\"user\":\"U%llu\",\"text\":", u);
        owh_text(db, &m, cid, text, tlen);
        sb_fmt(&m, ",\"ts\":\"%llu\"", ts);
        if (parent) sb_fmt(&m, ",\"thread_ts\":\"%llu\"", (unsigned long long)parent);
        if (what == OWH_MSG_NEW) sb_fmt(&m, ",\"event_ts\":\"%llu\"}", ts);
        else sb_fmt(&m, ",\"edited\":{\"user\":\"U%llu\",\"ts\":\"%llu\"}}", u, ts);
        if (what == OWH_MSG_NEW) sb_add(&b, m.p ? m.p : "", m.n);
        else {
            sb_fmt(&b, "{\"type\":\"message\",\"subtype\":\"message_changed\",\"hidden\":true,"
                       "\"channel\":\"C%llu\",\"channel_type\":\"%s\",\"message\":", c, ctype);
            sb_add(&b, m.p ? m.p : "", m.n);
            sb_fmt(&b, ",\"ts\":\"%llu\",\"event_ts\":\"%llu\"}", ts, ts);
        }
        if (m.bad) b.bad = 1;
        free(m.p);
    }
    if (!b.bad) owh_queue(db, ids, n, b.p);
    free(b.p);
    free(ids);
    free(body);
}

/* A reaction added to or taken from message `mid` by `uid`. */
static void owh_reaction(sqlite3 *db, uint64_t channel_id, uint64_t mid, uint64_t uid, const char *emoji, int added) {
    size_t n = 0;
    uint64_t *ids = owh_wanted(db, channel_id, added ? "reaction_added" : "reaction_removed", &n);
    if (!n) { free(ids); return; }
    uint64_t author = 0;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT author_id FROM messages WHERE id=?1;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)mid);
        if (sqlite3_step(st) == SQLITE_ROW) author = (uint64_t)sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    owh_sb b = { 0 };
    sb_fmt(&b, "{\"type\":\"%s\",\"user\":\"U%llu\",\"reaction\":", added ? "reaction_added" : "reaction_removed",
           (unsigned long long)uid);
    sb_jstr(&b, emoji);
    sb_fmt(&b, ",\"item_user\":\"U%llu\",\"item\":{\"type\":\"message\",\"channel\":\"C%llu\",\"ts\":\"%llu\"},"
               "\"event_ts\":\"%llu\"}", (unsigned long long)author, (unsigned long long)channel_id,
           (unsigned long long)mid, (unsigned long long)mid);
    if (!b.bad) owh_queue(db, ids, n, b.p);
    free(b.p);
    free(ids);
}

/* `uid` joined or left `channel_id`. */
static void owh_member(sqlite3 *db, uint64_t channel_id, uint64_t uid, int joined) {
    const char *type = joined ? "member_joined_channel" : "member_left_channel";
    size_t n = 0;
    uint64_t *ids = owh_wanted(db, channel_id, type, &n);
    if (!n) { free(ids); return; }
    owh_sb b = { 0 };
    sb_fmt(&b, "{\"type\":\"%s\",\"user\":\"U%llu\",\"channel\":\"C%llu\",\"channel_type\":\"%s\"}", type,
           (unsigned long long)uid, (unsigned long long)channel_id,
           strcmp(owh_channel_type(db, channel_id), "channel") == 0 ? "C" : "G");
    if (!b.bad) owh_queue(db, ids, n, b.p);
    free(b.p);
    free(ids);
}

/* `channel_id` renamed (`what` channel_rename), archived or unarchived, by `uid`. */
static void owh_channel(sqlite3 *db, uint64_t channel_id, uint64_t uid, const char *what) {
    size_t n = 0;
    uint64_t *ids = owh_wanted(db, channel_id, what, &n);
    if (!n) { free(ids); return; }
    owh_sb b = { 0 };
    if (strcmp(what, "channel_rename") == 0) {
        sqlite3_stmt *st = NULL;
        char *name = NULL;
        uint64_t created = 0;
        if (sqlite3_prepare_v2(db, "SELECT COALESCE(name,''), created_at_ms FROM channels WHERE id=?1;", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)channel_id);
            if (sqlite3_step(st) == SQLITE_ROW) {
                name = strdup((const char *)sqlite3_column_text(st, 0));
                created = (uint64_t)sqlite3_column_int64(st, 1);
            }
        }
        sqlite3_finalize(st);
        sb_fmt(&b, "{\"type\":\"channel_rename\",\"channel\":{\"id\":\"C%llu\",\"name\":", (unsigned long long)channel_id);
        sb_jstr(&b, name);
        sb_fmt(&b, ",\"created\":%llu}}", (unsigned long long)(created / 1000u));
        free(name);
    } else {
        sb_fmt(&b, "{\"type\":\"%s\",\"channel\":\"C%llu\",\"user\":\"U%llu\"}", what,
               (unsigned long long)channel_id, (unsigned long long)uid);
    }
    if (!b.bad) owh_queue(db, ids, n, b.p);
    free(b.p);
    free(ids);
}

/* `uid`'s profile changed: user_change, to every webhook that takes it,
 * whatever its channels. */
static void owh_user_change(sqlite3 *db, uint64_t uid) {
    size_t n = 0;
    uint64_t *ids = owh_wanted(db, 0, "user_change", &n);
    if (!n) { free(ids); return; }
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT COALESCE(display_name,''), COALESCE(full_name,''), COALESCE(title,''),"
            "       COALESCE(pronouns,''), COALESCE(timezone,''), kind <> 'human'"
            "  FROM users WHERE id=?1 AND is_system=0;", -1, &st, NULL) != SQLITE_OK ||
        (sqlite3_bind_int64(st, 1, (sqlite3_int64)uid), sqlite3_step(st)) != SQLITE_ROW) {
        sqlite3_finalize(st);
        free(ids);
        return;
    }
    owh_sb b = { 0 };
    sb_fmt(&b, "{\"type\":\"user_change\",\"user\":{\"id\":\"U%llu\",\"name\":", (unsigned long long)uid);
    sb_jstr(&b, (const char *)sqlite3_column_text(st, 0));
    sb_str(&b, ",\"real_name\":");
    sb_jstr(&b, (const char *)sqlite3_column_text(st, 1));
    sb_str(&b, ",\"tz\":");
    sb_jstr(&b, (const char *)sqlite3_column_text(st, 4));
    sb_fmt(&b, ",\"is_bot\":%s,\"profile\":{\"display_name\":", sqlite3_column_int(st, 5) ? "true" : "false");
    sb_jstr(&b, (const char *)sqlite3_column_text(st, 0));
    sb_str(&b, ",\"real_name\":");
    sb_jstr(&b, (const char *)sqlite3_column_text(st, 1));
    sb_str(&b, ",\"title\":");
    sb_jstr(&b, (const char *)sqlite3_column_text(st, 2));
    sb_str(&b, ",\"pronouns\":");
    sb_jstr(&b, (const char *)sqlite3_column_text(st, 3));
    sb_str(&b, "}},\"event_ts\":");
    sb_fmt(&b, "\"%llu\"}", (unsigned long long)dbw_now_ms());
    sqlite3_finalize(st);
    if (!b.bad) owh_queue(db, ids, n, b.p);
    free(b.p);
    free(ids);
}

/* Begin a transaction unless the caller holds one; 1 when this began it. */
static int owh_txn_begin(sqlite3 *db) {
    int own = sqlite3_get_autocommit(db);
    if (own) sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);
    return own;
}
static void owh_txn_end(sqlite3 *db, int own) {
    if (own) sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
}

static oc_dbres *owh_refuse(const oc_job *j, uint16_t code) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->type = OC_RES_OUTWH_ERR;
    r->err_code = code;
    return r;
}

/* The webhooks into `r` as OC_RES_OUTWH_LIST, oldest first: never a secret. */
static void owh_rows(sqlite3 *db, oc_dbres *r) {
    r->type = OC_RES_OUTWH_LIST;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT id, url, events, enabled, disabled_reason, created_by, created_at_ms"
            " FROM outgoing_webhooks ORDER BY id;", -1, &st, NULL) != SQLITE_OK) {
        sqlite3_finalize(st);
        return;
    }
    while (sqlite3_step(st) == SQLITE_ROW && r->n_owh_rows < OC_OUTWH_LIST_MAX) {
        oc_outwh_row *g = realloc(r->owh_rows, (r->n_owh_rows + 1) * sizeof *g);
        if (!g) break;
        r->owh_rows = g;
        oc_outwh_row *o = &r->owh_rows[r->n_owh_rows++];
        memset(o, 0, sizeof *o);
        o->id = (uint64_t)sqlite3_column_int64(st, 0);
        o->url = strdup((const char *)sqlite3_column_text(st, 1));
        o->events = strdup((const char *)sqlite3_column_text(st, 2));
        o->enabled = (uint8_t)sqlite3_column_int(st, 3);
        o->disabled_reason = strdup((const char *)sqlite3_column_text(st, 4));
        o->created_by = (uint64_t)sqlite3_column_int64(st, 5);
        o->created_at_ms = (uint64_t)sqlite3_column_int64(st, 6);
        sqlite3_stmt *c = NULL;
        if (sqlite3_prepare_v2(db, "SELECT channel_id FROM outgoing_webhook_channels WHERE webhook_id=?1"
                                   " ORDER BY channel_id;", -1, &c, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(c, 1, (sqlite3_int64)o->id);
            while (sqlite3_step(c) == SQLITE_ROW && o->n_channels < OC_OUTWH_MAX_CHANNELS)
                o->channels[o->n_channels++] = (uint64_t)sqlite3_column_int64(c, 0);
        }
        sqlite3_finalize(c);
    }
    sqlite3_finalize(st);
}

static oc_dbres *process_owh_create(oc_dbwriter *w, const oc_job *j) {
    sqlite3 *db = w->db;
    if (!actor_manages(db, j->user_id)) return owh_refuse(j, OC_ERR_FORBIDDEN);
    if (!owh_url_ok(j->owh_url)) return owh_refuse(j, OC_ERR_WEBHOOK_URL);
    char events[256];
    if (owh_events_canon(j->owh_events, events, sizeof events) != 0) return owh_refuse(j, OC_ERR_WEBHOOK_EVENT);
    if (j->owh_n_channels > OC_OUTWH_MAX_CHANNELS) return owh_refuse(j, OC_ERR_FORBIDDEN);
    for (uint16_t i = 0; i < j->owh_n_channels; i++) {
        int named = owh_channel_named(db, j->owh_channels[i]);
        if (named < 0) return owh_refuse(j, OC_ERR_UNKNOWN_CHANNEL);
        if (!named) return owh_refuse(j, OC_ERR_FORBIDDEN);   /* a DM or group DM */
    }
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->user_id = j->user_id;
    r->owh_url = strdup(j->owh_url);
    r->owh_events = strdup(events);
    r->owh_n_channels = j->owh_n_channels;
    memcpy(r->owh_channels, j->owh_channels, j->owh_n_channels * sizeof j->owh_channels[0]);
    if (!j->owh_verified) {
        /* Not saved yet: the endpoint is checked first, signed with this. */
        if (oc_rand_bytes(r->owh_secret, sizeof r->owh_secret) != 0) { r->type = OC_RES_OUTWH_ERR; r->err_code = OC_ERR_INTERNAL; }
        else r->type = OC_RES_OUTWH_CHECK;
        return r;
    }
    if (!w->have_factor_key) { r->type = OC_RES_OUTWH_ERR; r->err_code = OC_ERR_INTERNAL; return r; }
    sqlite3_exec(db, "SAVEPOINT owh_create;", NULL, NULL, NULL);
    sqlite3_stmt *st = NULL;
    uint64_t id = 0;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO outgoing_webhooks(url, events, secret_sealed, created_by, created_at_ms)"
            " VALUES(?1, ?2, x'', ?3, ?4);", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text (st, 1, j->owh_url, -1, SQLITE_STATIC);
        sqlite3_bind_text (st, 2, events, -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 3, (sqlite3_int64)j->user_id);
        sqlite3_bind_int64(st, 4, (sqlite3_int64)dbw_now_ms());
        if (sqlite3_step(st) == SQLITE_DONE) id = (uint64_t)sqlite3_last_insert_rowid(db);
    }
    sqlite3_finalize(st);
    /* Sealed bound to its webhook: moved to another row, it does not open. */
    uint8_t sealed[OC_TOTP_SEALED_LEN];
    int ok = id && oc_factor_seal(w->factor_key, id, j->owh_secret, sealed) == 0;
    if (ok) {
        st = NULL;
        ok = sqlite3_prepare_v2(db, "UPDATE outgoing_webhooks SET secret_sealed=?1 WHERE id=?2;", -1, &st, NULL) == SQLITE_OK;
        if (ok) {
            sqlite3_bind_blob (st, 1, sealed, (int)sizeof sealed, SQLITE_STATIC);
            sqlite3_bind_int64(st, 2, (sqlite3_int64)id);
            ok = sqlite3_step(st) == SQLITE_DONE;
        }
        sqlite3_finalize(st);
    }
    for (uint16_t i = 0; ok && i < j->owh_n_channels; i++) {
        st = NULL;
        ok = sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO outgoing_webhook_channels(webhook_id, channel_id) VALUES(?1, ?2);",
                                -1, &st, NULL) == SQLITE_OK;
        if (ok) {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)id);
            sqlite3_bind_int64(st, 2, (sqlite3_int64)j->owh_channels[i]);
            ok = sqlite3_step(st) == SQLITE_DONE;
        }
        sqlite3_finalize(st);
    }
    if (!ok) {
        sqlite3_exec(db, "ROLLBACK TO owh_create; RELEASE owh_create;", NULL, NULL, NULL);
        r->type = OC_RES_OUTWH_ERR;
        r->err_code = OC_ERR_INTERNAL;
        return r;
    }
    audit_actor(db, OC_AUDIT_ADMIN, "webhook.outgoing.create", j->user_id, id, j->owh_url, 1, events);
    sqlite3_exec(db, "RELEASE owh_create;", NULL, NULL, NULL);
    r->type = OC_RES_OUTWH_SECRET;
    r->message_id = id;
    memcpy(r->owh_secret, j->owh_secret, sizeof r->owh_secret);
    return r;
}

static oc_dbres *process_owh_list(sqlite3 *db, const oc_job *j) {
    if (!actor_manages(db, j->user_id)) return owh_refuse(j, OC_ERR_FORBIDDEN);
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    owh_rows(db, r);
    return r;
}

/* Webhook `id`'s address, or NULL when there is no such webhook. Heap. */
static char *owh_url_of(sqlite3 *db, uint64_t id) {
    sqlite3_stmt *st = NULL;
    char *url = NULL;
    if (sqlite3_prepare_v2(db, "SELECT url FROM outgoing_webhooks WHERE id=?1;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)id);
        if (sqlite3_step(st) == SQLITE_ROW) url = strdup((const char *)sqlite3_column_text(st, 0));
    }
    sqlite3_finalize(st);
    return url;
}

/* Run `sql` with ?1 bound to `id`. */
static void owh_exec_id(sqlite3 *db, const char *sql, uint64_t id) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)id);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
}

static oc_dbres *process_owh_set(sqlite3 *db, const oc_job *j) {
    if (!actor_manages(db, j->user_id)) return owh_refuse(j, OC_ERR_FORBIDDEN);
    char *url = owh_url_of(db, j->owh_id);
    if (!url) return owh_refuse(j, OC_ERR_FORBIDDEN);
    /* Turned on, it starts afresh: no failing day carried over. */
    owh_exec_id(db, j->owh_enabled
        ? "UPDATE outgoing_webhooks SET enabled=1, disabled_reason='', failing_since_ms=0 WHERE id=?1;"
        : "UPDATE outgoing_webhooks SET enabled=0, disabled_reason='' WHERE id=?1;", j->owh_id);
    audit_actor(db, OC_AUDIT_ADMIN, j->owh_enabled ? "webhook.outgoing.enable" : "webhook.outgoing.disable",
                j->user_id, j->owh_id, url, 1, NULL);
    free(url);
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    owh_rows(db, r);
    return r;
}

static oc_dbres *process_owh_delete(sqlite3 *db, const oc_job *j) {
    if (!actor_manages(db, j->user_id)) return owh_refuse(j, OC_ERR_FORBIDDEN);
    char *url = owh_url_of(db, j->owh_id);
    if (!url) return owh_refuse(j, OC_ERR_FORBIDDEN);
    int own = owh_txn_begin(db);
    owh_exec_id(db, "DELETE FROM webhook_deliveries WHERE webhook_id=?1;", j->owh_id);
    owh_exec_id(db, "DELETE FROM outgoing_webhook_channels WHERE webhook_id=?1;", j->owh_id);
    owh_exec_id(db, "DELETE FROM outgoing_webhooks WHERE id=?1;", j->owh_id);
    audit_actor(db, OC_AUDIT_ADMIN, "webhook.outgoing.delete", j->user_id, j->owh_id, url, 1, NULL);
    owh_txn_end(db, own);
    free(url);
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    owh_rows(db, r);
    return r;
}

/* The deliveries due, for the worker, each held a while so the next take does
 * not hand it out again; one never answered is taken again once its hold
 * ends, after a restart too. */
static oc_dbres *process_owh_take(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->type = OC_RES_OUTWH_DUE;
    uint64_t now = dbw_now_ms();
    unsigned limit = j->owh_limit && j->owh_limit < OC_OUTWH_TAKE_MAX ? j->owh_limit : OC_OUTWH_TAKE_MAX;
    int own = owh_txn_begin(db);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT d.id, d.webhook_id, d.attempts, w.url, d.event_id, d.payload, w.secret_sealed, d.last_reason"
            "  FROM webhook_deliveries d JOIN outgoing_webhooks w ON w.id = d.webhook_id"
            " WHERE w.enabled = 1 AND d.next_at_ms <= ?1 ORDER BY d.next_at_ms, d.id LIMIT ?2;",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)now);
        sqlite3_bind_int  (st, 2, (int)limit);
        while (sqlite3_step(st) == SQLITE_ROW) {
            oc_outwh_due *g = realloc(r->owh_due, (r->n_owh_due + 1) * sizeof *g);
            if (!g) break;
            r->owh_due = g;
            oc_outwh_due *d = &r->owh_due[r->n_owh_due++];
            memset(d, 0, sizeof *d);
            d->id = (uint64_t)sqlite3_column_int64(st, 0);
            d->webhook_id = (uint64_t)sqlite3_column_int64(st, 1);
            d->attempts = (uint32_t)sqlite3_column_int(st, 2);
            d->url = strdup((const char *)sqlite3_column_text(st, 3));
            d->event_id = strdup((const char *)sqlite3_column_text(st, 4));
            d->payload = strdup((const char *)sqlite3_column_text(st, 5));
            int sl = sqlite3_column_bytes(st, 6);
            if (sl > 0 && (size_t)sl <= sizeof d->sealed) {
                memcpy(d->sealed, sqlite3_column_blob(st, 6), (size_t)sl);
                d->sealed_len = (size_t)sl;
            }
            snprintf(d->last_reason, sizeof d->last_reason, "%s", (const char *)sqlite3_column_text(st, 7));
        }
    }
    sqlite3_finalize(st);
    for (size_t i = 0; i < r->n_owh_due; i++) {
        st = NULL;
        if (sqlite3_prepare_v2(db, "UPDATE webhook_deliveries SET next_at_ms=?2 WHERE id=?1;", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)r->owh_due[i].id);
            sqlite3_bind_int64(st, 2, (sqlite3_int64)(now + OC_OUTWH_LEASE_MS));
            sqlite3_step(st);
        }
        sqlite3_finalize(st);
    }
    owh_txn_end(db, own);
    return r;
}

/* A delivery's outcome. Success ends it and the webhook's failing; a failure
 * is tried again after 1 s, doubling, up to OC_OUTWH_ATTEMPTS in all. A
 * webhook failing for a day is turned off, every owner and admin is told, and
 * the change is audited. */
static oc_dbres *process_owh_done(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->type = OC_RES_OUTWH_DONE;
    uint64_t now = dbw_now_ms();
    uint64_t hook = 0, failing = 0;
    uint32_t attempts = 0;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT d.webhook_id, d.attempts, w.failing_since_ms FROM webhook_deliveries d"
            "  JOIN outgoing_webhooks w ON w.id = d.webhook_id WHERE d.id=?1;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->owh_delivery);
        if (sqlite3_step(st) == SQLITE_ROW) {
            hook = (uint64_t)sqlite3_column_int64(st, 0);
            attempts = (uint32_t)sqlite3_column_int(st, 1);
            failing = (uint64_t)sqlite3_column_int64(st, 2);
        }
    }
    sqlite3_finalize(st);
    r->message_id = hook;
    if (!hook) return r;                       /* its webhook went meanwhile */
    int own = owh_txn_begin(db);
    if (j->owh_ok) {
        owh_exec_id(db, "DELETE FROM webhook_deliveries WHERE id=?1;", j->owh_delivery);
        owh_exec_id(db, "UPDATE outgoing_webhooks SET failing_since_ms=0 WHERE id=?1;", hook);
        owh_txn_end(db, own);
        return r;
    }
    attempts++;
    if (attempts >= OC_OUTWH_ATTEMPTS) {
        owh_exec_id(db, "DELETE FROM webhook_deliveries WHERE id=?1;", j->owh_delivery);
    } else {
        st = NULL;
        if (sqlite3_prepare_v2(db, "UPDATE webhook_deliveries SET attempts=?2, next_at_ms=?3, last_reason=?4 WHERE id=?1;",
                               -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)j->owh_delivery);
            sqlite3_bind_int  (st, 2, (int)attempts);
            sqlite3_bind_int64(st, 3, (sqlite3_int64)(now + ((uint64_t)OC_OUTWH_FIRST_RETRY_MS << (attempts - 1))));
            sqlite3_bind_text (st, 4, j->owh_reason ? j->owh_reason : "", -1, SQLITE_STATIC);
            sqlite3_step(st);
        }
        sqlite3_finalize(st);
    }
    if (!failing) {
        st = NULL;
        if (sqlite3_prepare_v2(db, "UPDATE outgoing_webhooks SET failing_since_ms=?2 WHERE id=?1;", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)hook);
            sqlite3_bind_int64(st, 2, (sqlite3_int64)now);
            sqlite3_step(st);
        }
        sqlite3_finalize(st);
    } else if (now - failing >= OC_OUTWH_DISABLE_MS) {
        char *url = owh_url_of(db, hook);
        owh_exec_id(db, "UPDATE outgoing_webhooks SET enabled=0,"
                        " disabled_reason='its address failed every delivery for a day' WHERE id=?1;", hook);
        audit_log(db, OC_AUDIT_ADMIN, "webhook.outgoing.disable", 0, NULL, hook, url, 1,
                  "failed every delivery for a day");
        size_t n = 0;
        uint64_t *who = ids_of(db, "SELECT id FROM users WHERE role IN ('owner','admin') AND disabled=0"
                                   " AND kind='human' AND is_system=0 AND ?1=?1;", 0, &n);
        for (size_t i = 0; i < n; i++)
            notice_person(db, who[i], "The outgoing webhook to %s was turned off: every delivery to it failed "
                          "for a day. Turn it on again in Admin, Webhooks, once its address answers.",
                          url ? url : "its address");
        free(who);
        free(url);
        r->owh_disabled = 1;
    }
    owh_txn_end(db, own);
    return r;
}

/* A channel's groups, in the order they were added. */
static uint16_t channel_group_ids(sqlite3 *db, uint64_t channel_id, uint64_t *out, uint16_t cap) {
    sqlite3_stmt *st = NULL;
    uint16_t n = 0;
    sqlite3_prepare_v2(db, "SELECT group_id FROM channel_groups WHERE channel_id=? "
                           "ORDER BY added_at_ms, group_id;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)channel_id);
    while (sqlite3_step(st) == SQLITE_ROW && n < cap) out[n++] = (uint64_t)sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return n;
}

/* Note that `user_id` gained (joined) or lost `channel_id`, and carry the
 * channel's brief once, loaded after the whole change so it says how things
 * stand. `groups_changed` marks a channel whose groups themselves changed. */
static void note_change(sqlite3 *db, oc_dbres *r, uint64_t channel_id, uint64_t user_id, int joined) {
    if (user_id) {
        oc_memb_change *g = realloc(r->mchg, (r->n_mchg + 1) * sizeof *g);
        if (!g) return;
        r->mchg = g;
        r->mchg[r->n_mchg++] = (oc_memb_change){ channel_id, user_id, (uint8_t)(joined ? 1 : 0) };
    }
    for (size_t i = 0; i < r->n_mchan; i++) if (r->mchan[i].channel_id == channel_id) return;
    oc_chan_brief *b = realloc(r->mchan, (r->n_mchan + 1) * sizeof *b);
    if (!b) return;
    r->mchan = b;
    memset(&r->mchan[r->n_mchan], 0, sizeof r->mchan[r->n_mchan]);
    r->mchan[r->n_mchan++].channel_id = channel_id;
    (void)db;
}

/* Fill every noted channel's brief: its CHANNEL_INFO fields, its groups and its
 * members as they now are. Called once, at the end of a change. */
static void fill_briefs(sqlite3 *db, oc_dbres *r) {
    for (size_t i = 0; i < r->n_mchan; i++) {
        oc_chan_brief *b = &r->mchan[i];
        sqlite3_stmt *st = NULL;
        sqlite3_prepare_v2(db, "SELECT kind, name, is_public, created_at_ms, topic, archived_at_ms "
                               "FROM channels WHERE id=?;", -1, &st, NULL);
        sqlite3_bind_int64(st, 1, (sqlite3_int64)b->channel_id);
        if (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char *kn = sqlite3_column_text(st, 0);
            const unsigned char *nm = sqlite3_column_text(st, 1);
            const unsigned char *tp = sqlite3_column_text(st, 4);
            b->kind = (kn && strcmp((const char *)kn, "dm") == 0) ? OC_CHANNEL_KIND_DM : OC_CHANNEL_KIND;
            b->name = strdup(nm ? (const char *)nm : "");
            b->is_public = (uint8_t)(sqlite3_column_int(st, 2) != 0);
            b->created_at = (uint64_t)sqlite3_column_int64(st, 3);
            b->topic = (tp && tp[0]) ? strdup((const char *)tp) : NULL;
            b->archived = (uint8_t)(sqlite3_column_type(st, 5) != SQLITE_NULL);
        }
        sqlite3_finalize(st);
        b->n_groups = channel_group_ids(db, b->channel_id, b->groups, OC_MAX_CHANNEL_GROUPS);
        b->members = ids_of(db, "SELECT user_id FROM channel_members WHERE channel_id=?1;",
                            b->channel_id, &b->n_members);
    }
}

/* One group, whole, into `g`. 1 found, 0 not. */
static int load_group(sqlite3 *db, uint64_t group_id, oc_group_row *g) {
    sqlite3_stmt *st = NULL;
    int found = 0;
    memset(g, 0, sizeof *g);
    sqlite3_prepare_v2(db, "SELECT handle, name, description, created_by FROM user_groups WHERE id=?;",
                       -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)group_id);
    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *h = sqlite3_column_text(st, 0), *n = sqlite3_column_text(st, 1),
                            *d = sqlite3_column_text(st, 2);
        g->id = group_id;
        g->handle = strdup(h ? (const char *)h : "");
        g->name = strdup(n ? (const char *)n : "");
        g->description = strdup(d ? (const char *)d : "");
        g->created_by = (uint64_t)sqlite3_column_int64(st, 3);
        found = 1;
    }
    sqlite3_finalize(st);
    if (!found) return 0;
    size_t n = 0;
    g->members = ids_of(db, "SELECT gm.user_id FROM user_group_members gm JOIN users u ON u.id = gm.user_id "
                            "WHERE gm.group_id=?1 AND u.disabled = 0 ORDER BY gm.added_at_ms, gm.user_id;",
                        group_id, &n);
    g->n_members = (uint16_t)(n > OC_MAX_GROUP_MEMBERS ? OC_MAX_GROUP_MEMBERS : n);
    return 1;
}

static void group_row_free(oc_group_row *g) {
    free(g->handle); free(g->name); free(g->description); free(g->members);
}

/* A handle is what `@` names (the mention scanner's characters), 1..48 long,
 * not beginning with a digit or a dot. */
static int group_handle_valid(const char *h, size_t n) {
    if (n == 0 || n > OC_GROUP_HANDLE_MAX) return 0;
    if ((h[0] >= '0' && h[0] <= '9') || h[0] == '.' || h[0] == '-' || h[0] == '_') return 0;
    for (size_t i = 0; i < n; i++) {
        char c = h[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '_' || c == '-')) return 0;
    }
    return 1;
}

/* Does something else already answer to @`name`: a broadcast word, another
 * group (not `except_group`), or -- with `people` -- a person's display name?
 * Compared without case, as mentions are. */
static int name_taken(sqlite3 *db, const char *name, uint64_t except_group, int people) {
    static const char *const RESERVED[] = { "here", "channel", "everyone" };
    for (size_t i = 0; i < sizeof RESERVED / sizeof *RESERVED; i++)
        if (strcasecmp(name, RESERVED[i]) == 0) return 1;
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "SELECT 1 FROM user_groups WHERE lower(handle) = lower(?1) AND id <> ?2 LIMIT 1;",
                       -1, &st, NULL);
    sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)except_group);
    int taken = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    if (taken || !people) return taken;
    sqlite3_prepare_v2(db, "SELECT 1 FROM users WHERE lower(display_name) = lower(?1) LIMIT 1;", -1, &st, NULL);
    sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
    taken = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    return taken;
}

/* A group with this handle, or 0. */
static uint64_t group_by_handle(sqlite3 *db, const char *handle) {
    sqlite3_stmt *st = NULL;
    uint64_t id = 0;
    sqlite3_prepare_v2(db, "SELECT id FROM user_groups WHERE lower(handle) = lower(?1);", -1, &st, NULL);
    sqlite3_bind_text(st, 1, handle, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) id = (uint64_t)sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return id;
}

static int group_exists(sqlite3 *db, uint64_t group_id) {
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "SELECT 1 FROM user_groups WHERE id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)group_id);
    int found = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    return found;
}

static oc_dbres *group_err(const oc_job *j, uint16_t code) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->type = OC_RES_GROUP_ERR;
    r->err_code = code;
    r->group_id = j->group_id;
    return r;
}

/* The group, whole, as the one a CHANGED result carries. */
static oc_dbres *group_changed(sqlite3 *db, const oc_job *j, uint64_t group_id, oc_dbres *r) {
    if (!r && !(r = calloc(1, sizeof *r))) return NULL;
    r->conn_id = j->conn_id;
    r->type = OC_RES_GROUP_CHANGED;
    r->groups = calloc(1, sizeof *r->groups);
    if (r->groups && load_group(db, group_id, &r->groups[0])) r->n_groups = 1;
    fill_briefs(db, r);
    return r;
}

/* Owners and admins manage groups (REQ-307). */
static int may_manage_groups(sqlite3 *db, uint64_t actor) {
    return get_role(db, actor) >= OC_ROLE_ADMIN && !user_disabled(db, actor);
}

/* Every group, whole, ordered by handle. A read. */
static oc_dbres *process_list_groups(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->type = OC_RES_GROUP_LIST;
    size_t n = 0;
    uint64_t *ids = ids_of(db, "SELECT id FROM user_groups WHERE ?1 = ?1 ORDER BY lower(handle);", 0, &n);
    if (n > OC_MAX_GROUPS) n = OC_MAX_GROUPS;
    r->groups = n ? calloc(n, sizeof *r->groups) : NULL;
    for (size_t i = 0; r->groups && i < n; i++)
        if (load_group(db, ids[i], &r->groups[r->n_groups])) r->n_groups++;
    free(ids);
    return r;
}

/* The text of a group, as a CREATE or UPDATE gave it: in bounds, or the error. */
static uint16_t group_text_check(sqlite3 *db, const oc_job *j, uint64_t except_group) {
    const char *h = j->grp_handle ? j->grp_handle : "";
    const char *nm = j->grp_name ? j->grp_name : "";
    const char *d = j->grp_desc ? j->grp_desc : "";
    if (!group_handle_valid(h, strlen(h)) || strlen(nm) == 0 || strlen(nm) > OC_GROUP_NAME_MAX ||
        strlen(d) > OC_GROUP_DESC_MAX) return OC_ERR_INVALID_GROUP;
    if (name_taken(db, h, except_group, 1)) return OC_ERR_GROUP_HANDLE_TAKEN;
    return 0;
}

static oc_dbres *process_create_group(sqlite3 *db, const oc_job *j) {
    if (!may_manage_groups(db, j->user_id)) return group_err(j, OC_ERR_FORBIDDEN);
    uint16_t bad = group_text_check(db, j, 0);
    if (bad) return group_err(j, bad);
    size_t have = 0;
    free(ids_of(db, "SELECT id FROM user_groups WHERE ?1 = ?1;", 0, &have));
    if (have >= OC_MAX_GROUPS) return group_err(j, OC_ERR_INVALID_GROUP);
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "INSERT INTO user_groups(handle,name,description,created_by,created_at_ms) "
                           "VALUES(?,?,?,?,?);", -1, &st, NULL);
    sqlite3_bind_text(st, 1, j->grp_handle, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, j->grp_name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, j->grp_desc ? j->grp_desc : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 4, (sqlite3_int64)j->user_id);
    sqlite3_bind_int64(st, 5, (sqlite3_int64)dbw_now_ms());
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) return group_err(j, OC_ERR_GROUP_HANDLE_TAKEN);   /* the unique index */
    uint64_t gid = (uint64_t)sqlite3_last_insert_rowid(db);
    audit_actor(db, OC_AUDIT_ADMIN, "group.create", j->user_id, 0, j->grp_handle, 1, NULL);
    return group_changed(db, j, gid, NULL);
}

static oc_dbres *process_update_group(sqlite3 *db, const oc_job *j) {
    if (!may_manage_groups(db, j->user_id)) return group_err(j, OC_ERR_FORBIDDEN);
    if (!group_exists(db, j->group_id)) return group_err(j, OC_ERR_UNKNOWN_GROUP);
    uint16_t bad = group_text_check(db, j, j->group_id);
    if (bad) return group_err(j, bad);
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "UPDATE user_groups SET handle=?, name=?, description=? WHERE id=?;", -1, &st, NULL);
    sqlite3_bind_text(st, 1, j->grp_handle, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, j->grp_name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, j->grp_desc ? j->grp_desc : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 4, (sqlite3_int64)j->group_id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) return group_err(j, OC_ERR_GROUP_HANDLE_TAKEN);
    audit_actor(db, OC_AUDIT_ADMIN, "group.update", j->user_id, 0, j->grp_handle, 1, NULL);
    return group_changed(db, j, j->group_id, NULL);
}

/* Deleting a group takes it out of every channel it was in, and out of the
 * membership it gave, in one transaction. Past mentions keep their people and
 * lose only the reference to a group that no longer exists. */
static oc_dbres *process_delete_group(sqlite3 *db, const oc_job *j) {
    if (!may_manage_groups(db, j->user_id)) return group_err(j, OC_ERR_FORBIDDEN);
    if (!group_exists(db, j->group_id)) return group_err(j, OC_ERR_UNKNOWN_GROUP);
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->type = OC_RES_GROUP_DELETED;
    r->group_id = j->group_id;
    size_t nch = 0, nu = 0;
    uint64_t *chans = ids_of(db, "SELECT channel_id FROM channel_groups WHERE group_id=?1;", j->group_id, &nch);
    uint64_t *users = ids_of(db, "SELECT user_id FROM user_group_members WHERE group_id=?1;", j->group_id, &nu);
    sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);
    sqlite3_stmt *st = NULL;
    static const char *const DEL[] = {
        "DELETE FROM channel_groups WHERE group_id=?;",
        "DELETE FROM user_group_members WHERE group_id=?;",
        "UPDATE mentions SET group_id = NULL WHERE group_id=?;",
        "DELETE FROM user_groups WHERE id=?;",
    };
    for (size_t k = 0; k < sizeof DEL / sizeof *DEL; k++) {
        sqlite3_prepare_v2(db, DEL[k], -1, &st, NULL);
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->group_id);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    for (size_t c = 0; c < nch; c++) {
        note_change(db, r, chans[c], 0, 0);
        r->mchan[r->n_mchan - 1].groups_changed = 1;
        for (size_t u = 0; u < nu; u++)
            if (membership_sync(db, chans[c], users[u], -1) < 0) note_change(db, r, chans[c], users[u], 0);
    }
    sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
    free(chans); free(users);
    audit_actor(db, OC_AUDIT_ADMIN, "group.delete", j->user_id, 0, NULL, 1, NULL);
    fill_briefs(db, r);
    return r;
}

/* Add or remove people. Each one added gains every channel the group is in,
 * and each one removed loses those it has only through it. */
static oc_dbres *process_group_members(sqlite3 *db, const oc_job *j, int add) {
    if (!may_manage_groups(db, j->user_id)) return group_err(j, OC_ERR_FORBIDDEN);
    if (!group_exists(db, j->group_id)) return group_err(j, OC_ERR_UNKNOWN_GROUP);
    if (add) {
        size_t have = 0;
        free(ids_of(db, "SELECT user_id FROM user_group_members WHERE group_id=?1;", j->group_id, &have));
        if (have + j->n_grp_uids > OC_MAX_GROUP_MEMBERS) return group_err(j, OC_ERR_INVALID_GROUP);
    }
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    size_t nch = 0;
    uint64_t *chans = ids_of(db, "SELECT channel_id FROM channel_groups WHERE group_id=?1;", j->group_id, &nch);
    sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);
    for (uint16_t i = 0; i < j->n_grp_uids; i++) {
        uint64_t uid = j->grp_uids[i];
        sqlite3_stmt *st = NULL;
        if (add) {
            /* A removed member, or a visitor, who would gain channels through
             * the group (SCHEMA.md §3bj), is not added. */
            if (user_disabled(db, uid) || user_not_person(db, uid)) continue;
            uint8_t role;
            if (!user_role(db, uid, &role)) continue;          /* no such person: nothing to add */
            sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO user_group_members(group_id,user_id,added_at_ms) "
                                   "VALUES(?,?,?);", -1, &st, NULL);
            sqlite3_bind_int64(st, 3, (sqlite3_int64)dbw_now_ms());
        } else {
            sqlite3_prepare_v2(db, "DELETE FROM user_group_members WHERE group_id=? AND user_id=?;", -1, &st, NULL);
        }
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->group_id);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)uid);
        sqlite3_step(st);
        int changed = sqlite3_changes(db) > 0;
        sqlite3_finalize(st);
        if (!changed) continue;
        audit_actor(db, OC_AUDIT_ADMIN, add ? "group.member.add" : "group.member.remove", j->user_id, uid,
                    NULL, 1, NULL);
        if (add && uid != j->user_id) {
            char *who = audit_name_of(db, j->user_id), *handle = NULL;
            sqlite3_stmt *hq = NULL;
            sqlite3_prepare_v2(db, "SELECT handle FROM user_groups WHERE id=?1;", -1, &hq, NULL);
            sqlite3_bind_int64(hq, 1, (sqlite3_int64)j->group_id);
            if (sqlite3_step(hq) == SQLITE_ROW && sqlite3_column_text(hq, 0))
                handle = strdup((const char *)sqlite3_column_text(hq, 0));
            sqlite3_finalize(hq);
            notice_person(db, uid, "@%s added you to the group @%s.", who ? who : "someone", handle ? handle : "group");
            free(who); free(handle);
        }
        for (size_t c = 0; c < nch; c++) {
            int d = membership_sync(db, chans[c], uid, -1);
            if (d) note_change(db, r, chans[c], uid, d > 0);
        }
    }
    sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
    free(chans);
    return group_changed(db, j, j->group_id, r);
}

/* Give a channel a group, or take it away (REQ-309): any member of the channel
 * may, as any member may invite a person. Answered as an INVITE is -- the
 * channel's state to the actor -- with who gained or lost it by the change. */
static int load_channel_info(sqlite3 *db, uint64_t channel_id, uint64_t actor, oc_dbres *r);

static oc_dbres *process_channel_group(sqlite3 *db, const oc_job *j, int add) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    if (!named_channel_exists(db, j->channel_id, NULL)) {
        r->type = OC_RES_CHANNEL_ERR; r->err_code = OC_ERR_UNKNOWN_CHANNEL; return r;
    }
    if (!is_member(db, j->channel_id, j->user_id)) {
        r->type = OC_RES_CHANNEL_ERR; r->err_code = OC_ERR_NOT_A_MEMBER; return r;
    }
    if (!group_exists(db, j->group_id)) {
        r->type = OC_RES_CHANNEL_ERR; r->err_code = OC_ERR_UNKNOWN_GROUP; return r;
    }
    uint64_t have[OC_MAX_CHANNEL_GROUPS];
    uint16_t nh = channel_group_ids(db, j->channel_id, have, OC_MAX_CHANNEL_GROUPS);
    int already = 0;
    for (uint16_t i = 0; i < nh; i++) if (have[i] == j->group_id) already = 1;
    if (add && !already && nh >= OC_MAX_CHANNEL_GROUPS) {
        r->type = OC_RES_CHANNEL_ERR; r->err_code = OC_ERR_INVALID_GROUP; return r;
    }
    size_t nu = 0;
    uint64_t *users = ids_of(db, "SELECT user_id FROM user_group_members WHERE group_id=?1;", j->group_id, &nu);
    sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);
    sqlite3_stmt *st = NULL;
    if (add) {
        sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO channel_groups(channel_id,group_id,added_by,added_at_ms) "
                               "VALUES(?,?,?,?);", -1, &st, NULL);
        sqlite3_bind_int64(st, 3, (sqlite3_int64)j->user_id);
        sqlite3_bind_int64(st, 4, (sqlite3_int64)dbw_now_ms());
    } else {
        sqlite3_prepare_v2(db, "DELETE FROM channel_groups WHERE channel_id=? AND group_id=?;", -1, &st, NULL);
    }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->channel_id);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)j->group_id);
    sqlite3_step(st);
    int changed = sqlite3_changes(db) > 0;
    sqlite3_finalize(st);
    if (changed) {
        note_change(db, r, j->channel_id, 0, 0);
        r->mchan[r->n_mchan - 1].groups_changed = 1;
        /* Told, always, as one added alone is: someone put them in the
         * channel, through a group of theirs. */
        char *who = NULL, *ch = NULL, *handle = NULL;
        if (add) {
            who = audit_name_of(db, j->user_id);
            sqlite3_stmt *q = NULL;
            sqlite3_prepare_v2(db, "SELECT (SELECT name FROM channels WHERE id=?1), "
                                   "(SELECT handle FROM user_groups WHERE id=?2);", -1, &q, NULL);
            sqlite3_bind_int64(q, 1, (sqlite3_int64)j->channel_id);
            sqlite3_bind_int64(q, 2, (sqlite3_int64)j->group_id);
            if (sqlite3_step(q) == SQLITE_ROW) {
                if (sqlite3_column_text(q, 0)) ch = strdup((const char *)sqlite3_column_text(q, 0));
                if (sqlite3_column_text(q, 1)) handle = strdup((const char *)sqlite3_column_text(q, 1));
            }
            sqlite3_finalize(q);
        }
        for (size_t u = 0; u < nu; u++) {
            int d = membership_sync(db, j->channel_id, users[u], -1);
            if (d) note_change(db, r, j->channel_id, users[u], d > 0);
            if (d > 0 && users[u] != j->user_id)
                notice_person(db, users[u], "@%s added you to #%s, through the group @%s.",
                              who ? who : "someone", ch ? ch : "a channel", handle ? handle : "group");
        }
        free(who); free(ch); free(handle);
    }
    sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
    free(users);
    if (changed)
        audit_actor_in(db, OC_AUDIT_ADMIN, add ? "channel.group.add" : "channel.group.remove", j->user_id,
                       j->channel_id, 0, NULL, 1, NULL);
    r->type = OC_RES_CHANNEL_INFO;
    load_channel_info(db, j->channel_id, j->user_id, r);
    fill_briefs(db, r);
    return r;
}

/* May the user read (backfill) this channel? Public channels are open to any
 * tenant user; private channels are members-only (REQ-031). */
static int channel_read_access(sqlite3 *db, uint64_t channel_id, uint64_t user_id) {
    uint8_t is_public = 0;
    if (!channel_exists(db, channel_id, &is_public)) return 0;
    return is_public || is_member(db, channel_id, user_id);
}

/* Is this channel archived (REQ-035)? archived_at_ms non-NULL is the flag. */
static int channel_is_archived(sqlite3 *db, uint64_t channel_id) {
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "SELECT archived_at_ms FROM channels WHERE id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)channel_id);
    int archived = (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) != SQLITE_NULL);
    sqlite3_finalize(st);
    return archived;
}

/* Post access (REQ-031). CH_OK: allowed (a public channel auto-joins the poster
 * so broadcasts reach them); CH_UNKNOWN: no such channel; CH_DENIED: a private
 * channel the user does not belong to. */
enum { CH_OK = 0, CH_UNKNOWN = 1, CH_DENIED = 2, CH_ARCHIVED = 3 };

/* Whether `channel_id` is a DM with the workspace's own sender: what the
 * workspace says there takes no reply, since nobody reads it. */
static int workspace_dm(sqlite3 *db, uint64_t channel_id) {
    sqlite3_stmt *st = NULL;
    int yes = 0;
    sqlite3_prepare_v2(db,
        "SELECT 1 FROM channels c JOIN channel_members cm ON cm.channel_id = c.id "
        "JOIN users u ON u.id = cm.user_id WHERE c.id = ?1 AND c.kind = 'dm' AND u.is_system = 1;",
        -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)channel_id);
    yes = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    return yes;
}
static int channel_post_access(sqlite3 *db, uint64_t channel_id, uint64_t user_id) {
    uint8_t is_public = 0;
    if (!channel_exists(db, channel_id, &is_public)) return CH_UNKNOWN;
    /* Archived is read-only (REQ-035), enforced here so every write path that
     * has a user behind it — send, threaded reply, attachment upload — inherits
     * it rather than each remembering. Checked before membership so an archived
     * channel refuses uniformly, and so a public one does not silently auto-join
     * someone into a dead room.
     *
     * The incoming-webhook post is the one writer with NO user to check, so it
     * cannot come through here and tests channel_is_archived directly. This
     * comment used to claim it inherited the rule; it did not, and a token
     * holder could write into an archived channel. */
    if (channel_is_archived(db, channel_id)) return CH_ARCHIVED;
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
        "SELECT a.id, a.filename, a.mime, a.size, a.reclaimed_at_ms, "
        "       m.kind, m.duration_ms, m.width, m.height, m.poster_id, m.title, m.language "
        "  FROM attachments a LEFT JOIN attachment_media m ON m.attachment_id = a.id "
        " WHERE a.message_id=? ORDER BY a.id LIMIT ?;", -1, &st, NULL);
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
        /* A video message's media row (REQ-165); NULLs for an ordinary file. */
        a->media_kind  = (uint8_t)sqlite3_column_int(st, 5);
        a->duration_ms = (uint32_t)sqlite3_column_int64(st, 6);
        a->width       = (uint16_t)sqlite3_column_int(st, 7);
        a->height      = (uint16_t)sqlite3_column_int(st, 8);
        a->poster_id   = (uint64_t)sqlite3_column_int64(st, 9);
        /* A snippet's title and language (REQ-226). */
        const unsigned char *tt = sqlite3_column_text(st, 10), *lg = sqlite3_column_text(st, 11);
        a->title    = (a->media_kind == OC_MEDIA_SNIPPET && tt) ? strdup((const char *)tt) : NULL;
        a->language = (a->media_kind == OC_MEDIA_SNIPPET && lg) ? strdup((const char *)lg) : NULL;
    }
    sqlite3_finalize(st);
}

/* Resolve the @mentions in a message body and record them (REQ-221, ARCH-89).
 *
 * The scan itself is shared/mention.c, which the client links too — the rule
 * for "what is a mention" must be one implementation or the highlight a reader
 * sees and the notification the server sends can disagree, and nobody would be
 * able to tell from either side alone.
 *
 * Resolution happens HERE because only the daemon has the roster. An unmatched
 * name is simply not a mention: it stays as text in the body and notifies
 * nobody, which is the right outcome for a typo. */
/* Keyword hits (REQ-135, ARCH-103). Written into `mentions` with their own kind,
 * so the push query's MENTIONS branch, the activity feed and the reader's
 * highlight all keep working with no further change — the three places a
 * parallel table would have had to be taught about, where the first one that
 * forgot would be a keyword that notifies but never appears.
 *
 * Matched by the SHARED scanner, never by SQL: "deploy" must not match
 * "deployment", phrases are allowed, and the client has to highlight exactly
 * what the server notified on (ARCH-89's argument, second time).
 *
 * The author's own keywords are skipped — your own message is not news — and
 * a hit is skipped where the @ scanner already named that person, so one message
 * cannot notify them twice for the same reason.
 *
 * Thread messages included, deliberately diverging from Slack: a thread is where
 * the substantive discussion usually is, and the worst place to go deaf. */

/* Whether the send of message `mid` stored a broadcast mention row (@here,
 * @channel, @everyone; REQ-221): store_mentions writes one when the
 * workspace's rule let the author notify the channel, and none for an author
 * it does not, or for a message stored without mentions (a webhook's, the
 * workspace's notice, and every message in an encrypted DM, whose body is
 * ciphertext here). Read from the rows rather than decided again, so
 * history says what the send did, whatever the rule or the channel's size is
 * now; and asked of the rows alone, not the body, because an edit changes the
 * body and stores no mentions (PROTOCOL.md §5.5): a broadcast an edit removes
 * and a later one puts back is the one the send stored. The byte BROADCAST
 * and THREAD_REPLY carry: a client knows a channel's size only for the member
 * list it has loaded, and counts a broadcast its current body holds only when
 * this is 1. */
static uint8_t broadcast_stored(sqlite3 *db, uint64_t mid) {
    sqlite3_stmt *st = NULL;
    int stored = 0;
    if (sqlite3_prepare_v2(db,
            "SELECT EXISTS(SELECT 1 FROM mentions WHERE message_id=?1 AND kind IN (?2,?3,?4));",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)mid);
        sqlite3_bind_int(st, 2, (int)OC_MENTION_HERE);
        sqlite3_bind_int(st, 3, (int)OC_MENTION_CHANNEL);
        sqlite3_bind_int(st, 4, (int)OC_MENTION_EVERYONE);
        if (sqlite3_step(st) == SQLITE_ROW) stored = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    return (uint8_t)(stored != 0);
}

/* Index the mentions of every message that has none: the one-time pass a
 * database loaded from an export needs (migrate.c, 0060), with the same scanner
 * a send uses, so the server's unread count and the client's reading of the
 * text agree. A message whose mentions are already rows is left alone, so the
 * pass is safe to repeat. */
static void store_mentions(sqlite3 *db, uint64_t mid, uint64_t channel_id,
                           const void *body, size_t body_len, uint64_t ts,
                           oc_mention_unresolved *unres, uint16_t *unres_total);   /* below */
static void index_mentions(sqlite3 *db) {
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT m.id, m.channel_id, m.body, m.created_at_ms FROM messages m "
            " WHERE m.body IS NOT NULL AND m.deleted_at_ms IS NULL "
            "   AND NOT EXISTS(SELECT 1 FROM mentions x WHERE x.message_id = m.id) "
            " ORDER BY m.id;", -1, &q, NULL) != SQLITE_OK) return;
    sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);
    size_t n = 0;
    while (sqlite3_step(q) == SQLITE_ROW) {
        uint64_t mid = (uint64_t)sqlite3_column_int64(q, 0);
        uint64_t cid = (uint64_t)sqlite3_column_int64(q, 1);
        const void *body = sqlite3_column_blob(q, 2);
        size_t blen = (size_t)sqlite3_column_bytes(q, 2);
        uint64_t ts = (uint64_t)sqlite3_column_int64(q, 3);
        if (!body || !blen || !memchr(body, '@', blen)) continue;
        oc_mention_unresolved unres; memset(&unres, 0, sizeof unres);
        uint16_t unres_total = 0;
        store_mentions(db, mid, cid, body, blen, ts, &unres, &unres_total);
        n++;
    }
    sqlite3_finalize(q);
    sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
    if (n) fprintf(stderr, "dbwriter: scanned %zu messages for mentions they had no rows for\n", n);
}

static void store_keyword_hits(sqlite3 *db, uint64_t mid, uint64_t channel_id,
                               uint64_t author_id, const void *body, size_t body_len,
                               uint64_t ts) {
    if (!body || !body_len || channel_is_e2e(db, channel_id)) return;
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT k.user_id, k.term FROM notify_keywords k "
            "  JOIN channel_members cm ON cm.user_id = k.user_id "
            " WHERE cm.channel_id = ?1 AND k.user_id <> ?2;", -1, &q, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_int64(q, 1, (sqlite3_int64)channel_id);
    sqlite3_bind_int64(q, 2, (sqlite3_int64)author_id);
    sqlite3_stmt *ins = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO mentions(message_id, channel_id, user_id, kind, "
            "                     span_start, span_len, created_at_ms) "
            "SELECT ?1,?2,?3,?4,?5,?6,?7 WHERE NOT EXISTS("
            "   SELECT 1 FROM mentions WHERE message_id=?1 AND user_id=?3);",
            -1, &ins, NULL) != SQLITE_OK) { sqlite3_finalize(q); return; }
    while (sqlite3_step(q) == SQLITE_ROW) {
        uint64_t uid = (uint64_t)sqlite3_column_int64(q, 0);
        const unsigned char *term = sqlite3_column_text(q, 1);
        size_t st_off = 0, st_len = 0;
        /* A keyword inside a spoiler alerts nobody (MARKDOWN.md §1d). */
        if (!term || !oc_rt_keyword_match((const char *)body, body_len,
                                          (const char *)term, &st_off, &st_len)) continue;
        sqlite3_reset(ins);
        sqlite3_bind_int64(ins, 1, (sqlite3_int64)mid);
        sqlite3_bind_int64(ins, 2, (sqlite3_int64)channel_id);
        sqlite3_bind_int64(ins, 3, (sqlite3_int64)uid);
        sqlite3_bind_int  (ins, 4, OC_MENTION_KEYWORD);
        sqlite3_bind_int  (ins, 5, (int)st_off);
        sqlite3_bind_int  (ins, 6, (int)st_len);
        sqlite3_bind_int64(ins, 7, (sqlite3_int64)ts);
        sqlite3_step(ins);
    }
    sqlite3_finalize(ins);
    sqlite3_finalize(q);
}

/* What the sender can DO about a name that resolved to nobody here (REQ-287),
 * answered where the channel is in hand rather than guessed at by the client.
 * Shared by SEND and SEND_REPLY: a mention in a thread is still a mention, and
 * the notice it produces has to say the same things. */
static void fill_unresolved_context(sqlite3 *db, uint64_t channel_id,
                                    oc_mention_unresolved *unres) {
    if (!unres->count) return;
    sqlite3_stmt *ci = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT kind, is_public, archived_at_ms FROM channels WHERE id = ?1;",
            -1, &ci, NULL) != SQLITE_OK) return;
    sqlite3_bind_int64(ci, 1, (sqlite3_int64)channel_id);
    if (sqlite3_step(ci) == SQLITE_ROW) {
        const unsigned char *kind = sqlite3_column_text(ci, 0);
        int is_dm = kind && strcmp((const char *)kind, "dm") == 0;
        unres->is_private = sqlite3_column_int(ci, 1) ? 0 : 1;
        /* A DM has nobody to add, and an archived channel takes no writes
         * (REQ-035): offering the action would be offering a failure. */
        unres->can_add = (!is_dm && sqlite3_column_type(ci, 2) == SQLITE_NULL) ? 1 : 0;
    }
    sqlite3_finalize(ci);
}

/* Everyone a message named but could not reach, distinct: the notice lists
 * eight, and a group can name more (REQ-308). */
typedef struct { uint64_t *ids; size_t n, cap; } unreached_set;

static void unreached_add(unreached_set *u, uint64_t id) {
    for (size_t i = 0; i < u->n; i++) if (u->ids[i] == id) return;
    if (u->n == u->cap) {
        size_t cap = u->cap ? u->cap * 2 : 16;
        uint64_t *g = realloc(u->ids, cap * sizeof *g);
        if (!g) return;
        u->ids = g; u->cap = cap;
    }
    u->ids[u->n++] = id;
}

static void store_mentions(sqlite3 *db, uint64_t mid, uint64_t channel_id,
                           const void *body, size_t body_len, uint64_t ts,
                           oc_mention_unresolved *unres, uint16_t *unres_total) {
    unreached_set unreached = { NULL, 0, 0 };
    /* An encrypted DM's body is ciphertext (PRIVATE-DM.md): no mention of any
     * kind is read from it, so a broadcast there stores no row and notifies
     * nobody, and its message carries broadcast_stored 0 (REQ-221). */
    if (!body || !body_len || channel_is_e2e(db, channel_id)) return;
    oc_mention m[OC_MENTION_MAX];
    size_t n = oc_mention_scan((const char *)body, body_len, m, OC_MENTION_MAX);
    if (n > OC_MENTION_MAX) n = OC_MENTION_MAX;   /* the rest are ignored, not an error */
    /* A mention inside a spoiler notifies nobody (MARKDOWN.md §1d): it gets no
     * row, of any kind, broadcasts and groups included. */
    {
        size_t k = 0;
        for (size_t i = 0; i < n; i++)
            if (!oc_rt_in_spoiler((const char *)body, body_len, m[i].start)) m[k++] = m[i];
        n = k;
    }

    sqlite3_stmt *ins = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO mentions(message_id, channel_id, user_id, kind, "
            "                     span_start, span_len, created_at_ms) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7);", -1, &ins, NULL) != SQLITE_OK)
        return;

    /* Public channels resolve a mention against the whole roster; private ones
     * only against their members (REQ-288). The split is not a nicety: in a
     * public channel the person can already open the message, so the notification
     * is the only thing missing, and withholding it just means they find out late
     * or never. In a private one they cannot read it at all, and a notification
     * pointing at something unreadable is worse than silence. */
    /* Who may notify a whole large channel (OPENCHIME_BROADCAST_MENTIONS and
     * _LARGE, REQ-221): from anyone else @channel, @everyone and @here stay in
     * the message as text and get no row, so they notify nobody. */
    int broadcast_ok = 1;
    {
        const oc_config *cf = oc_config_get();
        if (cf->broadcast_mentions != 0) {
            uint64_t author = 0;
            int64_t members = 0;
            sqlite3_stmt *bq = NULL;
            if (sqlite3_prepare_v2(db,
                    "SELECT author_id, (SELECT COUNT(*) FROM channel_members WHERE channel_id=?2) "
                    "FROM messages WHERE id=?1;", -1, &bq, NULL) == SQLITE_OK) {
                sqlite3_bind_int64(bq, 1, (sqlite3_int64)mid);
                sqlite3_bind_int64(bq, 2, (sqlite3_int64)channel_id);
                if (sqlite3_step(bq) == SQLITE_ROW) {
                    author = (uint64_t)sqlite3_column_int64(bq, 0);
                    members = sqlite3_column_int64(bq, 1);
                }
                sqlite3_finalize(bq);
            }
            if (members >= cf->broadcast_large)
                broadcast_ok = cf->broadcast_mentions == 1 && get_role(db, author) >= OC_ROLE_ADMIN;
        }
    }
    int chan_public = 0;
    {
        sqlite3_stmt *cq = NULL;
        if (sqlite3_prepare_v2(db, "SELECT is_public, kind FROM channels WHERE id = ?1;",
                               -1, &cq, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(cq, 1, (sqlite3_int64)channel_id);
            if (sqlite3_step(cq) == SQLITE_ROW) {
                const unsigned char *k = sqlite3_column_text(cq, 1);
                /* A DM is never "public" here whatever the column says — there is
                 * no joining one, so there is nobody outside it to notify. */
                chan_public = sqlite3_column_int(cq, 0) && !(k && strcmp((const char *)k, "dm") == 0);
            }
            sqlite3_finalize(cq);
        }
    }

    uint64_t named[OC_MENTION_MAX];
    size_t n_named = 0;
    for (size_t i = 0; i < n; i++) {
        int64_t uid = 0;
        /* A group's handle first (REQ-308): it wins a clash with a person's
         * name, which only an identity provider's name at first sign-in can
         * make. Each member it reaches gets a row of their own, as a personal
         * mention would, resolved now: who is in the group later changes later
         * mentions, not this one. Who it cannot reach is told to the sender as
         * a person not in the channel is (REQ-287), and in a public channel
         * still gets the mention (REQ-288). */
        uint64_t gid = m[i].kind == OC_MENTION_USER && !m[i].user_id ? group_by_handle(db, m[i].name) : 0;
        if (gid) {
            oc_group_row g;
            if (!load_group(db, gid, &g)) continue;
            sqlite3_stmt *gi = NULL;
            sqlite3_prepare_v2(db,
                "INSERT INTO mentions(message_id, channel_id, user_id, kind, span_start, span_len, "
                "                     created_at_ms, group_id) VALUES(?1,?2,?3,?4,?5,?6,?7,?8);",
                -1, &gi, NULL);
            for (uint16_t k = 0; gi && k < g.n_members; k++) {
                uint64_t who = g.members[k];
                int in = is_member(db, channel_id, who);
                if (!in) {
                    unreached_add(&unreached, who);
                    if (unres && unres->count < OC_UNRESOLVED_MAX) {
                        int dup = 0;
                        for (uint16_t q = 0; q < unres->count; q++)
                            if (unres->who[q].user_id == who) { dup = 1; break; }
                        if (!dup) {
                            sqlite3_stmt *nq = NULL;
                            sqlite3_prepare_v2(db, "SELECT display_name FROM users WHERE id=?;", -1, &nq, NULL);
                            sqlite3_bind_int64(nq, 1, (sqlite3_int64)who);
                            const unsigned char *dn = sqlite3_step(nq) == SQLITE_ROW ? sqlite3_column_text(nq, 0) : NULL;
                            unres->who[unres->count].user_id = who;
                            snprintf(unres->who[unres->count].name, sizeof unres->who[unres->count].name,
                                     "%.*s", (int)(sizeof unres->who[unres->count].name - 1),
                                     dn ? (const char *)dn : "");
                            unres->count++;
                            sqlite3_finalize(nq);
                        }
                    }
                    if (!chan_public) continue;
                }
                sqlite3_reset(gi);
                sqlite3_bind_int64(gi, 1, (sqlite3_int64)mid);
                sqlite3_bind_int64(gi, 2, (sqlite3_int64)channel_id);
                sqlite3_bind_int64(gi, 3, (sqlite3_int64)who);
                sqlite3_bind_int(gi, 4, (int)OC_MENTION_GROUP);
                sqlite3_bind_int64(gi, 5, (sqlite3_int64)m[i].start);
                sqlite3_bind_int64(gi, 6, (sqlite3_int64)m[i].len);
                sqlite3_bind_int64(gi, 7, (sqlite3_int64)ts);
                sqlite3_bind_int64(gi, 8, (sqlite3_int64)gid);
                sqlite3_step(gi);
            }
            sqlite3_finalize(gi);
            group_row_free(&g);
            continue;
        }
        if ((m[i].kind == OC_MENTION_HERE || m[i].kind == OC_MENTION_CHANNEL ||
             m[i].kind == OC_MENTION_EVERYONE) && !broadcast_ok)
            continue;
        if (m[i].kind == OC_MENTION_USER) {
            /* Match the display name case-insensitively among this channel's
             * members -- or, for the id form (<@U42>), the id itself, under
             * the same rules. Membership is asked first regardless of channel
             * kind, because "are they in it" is also what REQ-287 reports. */
            sqlite3_stmt *q = NULL;
            if (sqlite3_prepare_v2(db,
                    "SELECT u.id FROM users u JOIN channel_members cm ON cm.user_id = u.id "
                    "WHERE cm.channel_id = ?1 AND u.disabled = 0 AND u.is_system = 0 "
                    "  AND (CASE WHEN ?3 <> 0 THEN u.id = ?3 "
                    "            ELSE lower(u.display_name) = lower(?2) END) LIMIT 1;", -1, &q, NULL) != SQLITE_OK)
                continue;
            sqlite3_bind_int64(q, 1, (sqlite3_int64)channel_id);
            sqlite3_bind_text(q, 2, m[i].name, -1, SQLITE_STATIC);
            sqlite3_bind_int64(q, 3, (sqlite3_int64)m[i].user_id);
            if (sqlite3_step(q) == SQLITE_ROW) uid = sqlite3_column_int64(q, 0);
            sqlite3_finalize(q);
            if (!uid) {
                /* Not a member — but is it a PERSON? A typo and a colleague who
                 * is simply not in this channel look identical from here, and
                 * they are completely different situations for the sender
                 * (REQ-287). Ask the tenant roster before deciding it was noise.
                 * A visitor is not on it (SCHEMA.md §3bj): naming one is noise,
                 * and says nothing about who writes in through the widget. */
                uint64_t outsider = 0;
                sqlite3_stmt *w = NULL;
                if (sqlite3_prepare_v2(db,
                        "SELECT id, display_name FROM users "
                        "WHERE disabled = 0 AND is_system = 0 AND kind <> 'visitor' "
                        "  AND (CASE WHEN ?2 <> 0 THEN id = ?2 "
                        "            ELSE lower(display_name) = lower(?1) END) LIMIT 1;",
                        -1, &w, NULL) == SQLITE_OK) {
                    sqlite3_bind_text(w, 1, m[i].name, -1, SQLITE_STATIC);
                    sqlite3_bind_int64(w, 2, (sqlite3_int64)m[i].user_id);
                    if (sqlite3_step(w) == SQLITE_ROW) {
                        outsider = (uint64_t)sqlite3_column_int64(w, 0);
                        unreached_add(&unreached, outsider);
                        if (unres && unres->count < OC_UNRESOLVED_MAX) {
                            int dup = 0;
                            for (uint16_t k = 0; k < unres->count; k++)
                                if (unres->who[k].user_id == outsider) { dup = 1; break; }
                            /* Mentioning the same absent person twice in one
                             * message is one problem, not two. */
                            if (!dup) {
                                const unsigned char *dn = sqlite3_column_text(w, 1);
                                unres->who[unres->count].user_id = outsider;
                                /* The fallback is the mention token AS TYPED,
                                 * which the scanner allows to be longer than
                                 * any legal display name -- so this copy is
                                 * bounded on purpose, and the precision says
                                 * so rather than leaving the compiler to
                                 * notice the buffer is the smaller of the
                                 * two. An @-token that does not fit cannot
                                 * name a real user anyway, which is why it is
                                 * in the unresolved list. */
                                snprintf(unres->who[unres->count].name,
                                         sizeof unres->who[unres->count].name,
                                         "%.*s",
                                         (int)(sizeof unres->who[unres->count].name - 1),
                                         dn ? (const char *)dn : m[i].name);
                                unres->count++;
                            }
                        }
                    }
                    sqlite3_finalize(w);
                }
                /* REQ-288: in a public channel they still get the mention, so it
                 * is stored and reaches their activity feed. The sender is told
                 * either way (REQ-287) — being notified is not the same as being
                 * IN the channel, and only membership gets them what comes next. */
                if (outsider && chan_public) uid = (int64_t)outsider;
                else continue;                     /* private, or not a person: just text */
            }
            /* One row per person per message, however often and in whichever
             * form they are named (`@ada` and `<@U42>`): one row is one
             * notification. */
            int again = 0;
            for (size_t q2 = 0; q2 < n_named && !again; q2++) again = named[q2] == (uint64_t)uid;
            if (again) continue;
            if (n_named < OC_MENTION_MAX) named[n_named++] = (uint64_t)uid;
        }
        sqlite3_reset(ins);
        sqlite3_bind_int64(ins, 1, (sqlite3_int64)mid);
        sqlite3_bind_int64(ins, 2, (sqlite3_int64)channel_id);
        if (uid) sqlite3_bind_int64(ins, 3, uid); else sqlite3_bind_null(ins, 3);
        sqlite3_bind_int(ins, 4, (int)m[i].kind);
        sqlite3_bind_int64(ins, 5, (sqlite3_int64)m[i].start);
        sqlite3_bind_int64(ins, 6, (sqlite3_int64)m[i].len);
        sqlite3_bind_int64(ins, 7, (sqlite3_int64)ts);
        sqlite3_step(ins);
    }
    sqlite3_finalize(ins);
    if (unres_total) *unres_total = (uint16_t)(unreached.n > 65535 ? 65535 : unreached.n);
    free(unreached.ids);
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

/* Forget one draft. Used by the send paths and the user cascade; a no-op when
 * there is nothing stored, which is the common case. */
static void drop_draft(sqlite3 *db, uint64_t user_id, uint64_t channel_id, uint64_t thread_root) {
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "DELETE FROM drafts WHERE user_id=? AND channel_id=? AND thread_root=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)user_id);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)channel_id);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)thread_root);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

/* Record what a forward points at (REQ-057), inside the send's transaction.
 *
 * The forwarder's read access to the SOURCE is checked with the same gate every
 * other read uses. Without it, forwarding is a way to read a channel you were
 * never in: ask the daemon to quote it at you and it obligingly does.
 *
 * A source that cannot be read, or no longer exists, is NOT an error. The
 * message sends as an ordinary message with no reference — refusing would turn
 * a stale permalink into a failure the sender cannot act on, and a forward of
 * something since deleted is a nuisance, not an attack.
 *
 * The author, the excerpt and the attachment count are read here rather than
 * taken from the client, and they are a SNAPSHOT: editing the original later
 * does not rewrite what was forwarded. The excerpt is truncated on a UTF-8
 * boundary — splitting a sequence would put invalid bytes in a TEXT column. */
#define OC_FORWARD_EXCERPT_MAX 240
static void store_forward(sqlite3 *db, uint64_t mid, const oc_job *j) {
    if (!j->src_channel || !j->src_message) return;
    /* Nothing goes into or out of an encrypted DM this way: its text is
     * ciphertext, and an excerpt of it here would be one in plain view. */
    if (channel_is_e2e(db, j->src_channel) || channel_is_e2e(db, j->channel_id)) return;
    if (!channel_read_access(db, j->src_channel, j->user_id)) return;

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT m.author_id, COALESCE(m.body, ''), "
            "       (SELECT COUNT(*) FROM attachments a WHERE a.message_id = m.id), "
            /* The FIRST file's name, which is what a card can show; the count
             * beside it covers the rest ("report.txt and 2 more"). */
            "       COALESCE((SELECT a.filename FROM attachments a "
            "                 WHERE a.message_id = m.id ORDER BY a.id LIMIT 1), ''), "
            /* An action's excerpt is its action text, and the forward says so
             * (REQ-058): the card reads "<author> <text>". */
            "       (SELECT ac.text_start FROM actions ac WHERE ac.message_id = m.id) "
            "FROM messages m WHERE m.id=?1 AND m.channel_id=?2 "
            "  AND m.deleted_at_ms IS NULL;", -1, &st, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->src_message);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)j->src_channel);
    uint64_t author = 0, n_attach = 0;
    char excerpt[OC_FORWARD_EXCERPT_MAX + 1];
    char aname[256] = "";
    size_t ex_len = 0;
    int found = 0, src_action = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *b = sqlite3_column_text(st, 1);
        size_t bl = (size_t)sqlite3_column_bytes(st, 1);
        if (sqlite3_column_type(st, 4) != SQLITE_NULL) {
            size_t ts = (size_t)sqlite3_column_int64(st, 4);
            if (b && ts < bl) { b += ts; bl -= ts; src_action = 1; }
        }
        author   = (uint64_t)sqlite3_column_int64(st, 0);
        n_attach = (uint64_t)sqlite3_column_int64(st, 2);
        const char *an = (const char *)sqlite3_column_text(st, 3);
        if (an) snprintf(aname, sizeof aname, "%s", an);
        /* Cut at a character, a spoiler hidden (MARKDOWN.md §1d). */
        ex_len = b ? oc_rt_plain_excerpt((const char *)b, bl, excerpt, sizeof excerpt) : 0;
        found = 1;
    }
    sqlite3_finalize(st);
    if (!found) return;
    excerpt[ex_len] = 0;

    sqlite3_prepare_v2(db,
        "INSERT INTO forwards(message_id, src_channel, src_message, src_author, "
        "                     excerpt, n_attach, attach_name, src_action) "
        "VALUES(?, ?, ?, ?, ?, ?, ?, ?);", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)mid);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)j->src_channel);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)j->src_message);
    sqlite3_bind_int64(st, 4, (sqlite3_int64)author);
    sqlite3_bind_text (st, 5, excerpt, (int)ex_len, SQLITE_STATIC);
    sqlite3_bind_int64(st, 6, (sqlite3_int64)n_attach);
    sqlite3_bind_text (st, 7, aname, -1, SQLITE_STATIC);
    sqlite3_bind_int  (st, 8, src_action);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

/* The AI client a message was posted through (messages.via, AUTH.md §8.13):
 * heap, or NULL for a message that came through none. */
static char *message_via(sqlite3 *db, uint64_t message_id) {
    sqlite3_stmt *st = NULL;
    char *out = NULL;
    if (sqlite3_prepare_v2(db, "SELECT via FROM messages WHERE id=?1 AND via<>'';", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)message_id);
        if (sqlite3_step(st) == SQLITE_ROW) {
            const char *v = (const char *)sqlite3_column_text(st, 0);
            if (v) out = strdup(v);
        }
    }
    sqlite3_finalize(st);
    return out;
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
    if (acc == CH_ARCHIVED){ r->type = OC_RES_SEND_ERR; r->err_code = OC_ERR_CHANNEL_ARCHIVED; return r; }
    if (workspace_dm(db, j->channel_id)) { r->type = OC_RES_SEND_ERR; r->err_code = OC_ERR_WORKSPACE_SENDER; return r; }
    /* An encrypted DM carries text only: no files, no forwards (PRIVATE-DM.md). */
    r->ch_e2e = (uint8_t)channel_is_e2e(db, j->channel_id);
    if (r->ch_e2e && (j->n_attach || j->src_message || j->n_poll_opts)) { r->type = OC_RES_SEND_ERR; r->err_code = OC_ERR_FORBIDDEN; return r; }
    /* An AI client never posts into an encrypted DM: it would be plain text there. */
    if (r->ch_e2e && j->via && j->via[0]) { r->type = OC_RES_SEND_ERR; r->err_code = OC_ERR_FORBIDDEN; return r; }

    uint64_t ts = dbw_now_ms();
    /* A poll (REQ-225): its options are checked here, where it is stored. */
    if (j->n_poll_opts && !poll_valid(j, ts)) { r->type = OC_RES_SEND_ERR; r->err_code = OC_ERR_INVALID_POLL; return r; }
    /* A "/todo" body makes a to-do list, checked here against its limits. An
     * encrypted DM's body is not the daemon's to read, and a poll is a poll. */
    oc_todo_parsed tp;
    int is_todo = 0;
    if (!r->ch_e2e && !j->n_poll_opts) {
        uint16_t te = todo_check(j->body, j->body_len, &tp, &is_todo);
        if (te) { r->type = OC_RES_SEND_ERR; r->err_code = te; return r; }
    }
    sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);

    sqlite3_prepare_v2(db,
        "INSERT INTO messages(channel_id, author_id, body, created_at_ms, via) "
        "VALUES(?, ?, ?, ?, ?);", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->channel_id);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)j->user_id);
    sqlite3_bind_blob(st, 3, j->body, (int)j->body_len, SQLITE_STATIC);
    sqlite3_bind_int64(st, 4, (sqlite3_int64)ts);
    sqlite3_bind_text(st, 5, j->via ? j->via : "", -1, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        r->type = OC_RES_SEND_ERR; r->err_code = OC_ERR_INTERNAL;
        return r;
    }
    uint64_t mid = (uint64_t)sqlite3_last_insert_rowid(db);
    /* Collected during resolution and carried back so the sender can be told
     * (REQ-287); the message itself is stored either way. */
    r->unres.channel_id = j->channel_id;
    r->unres.message_id = mid;
    store_mentions(db, mid, j->channel_id, j->body, j->body_len, ts, &r->unres, &r->unres_total);
    store_keyword_hits(db, mid, j->channel_id, j->user_id, j->body, j->body_len, ts);
    fill_unresolved_context(db, j->channel_id, &r->unres);

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

    /* What this message forwards, if anything (REQ-057). */
    store_forward(db, mid, j);
    store_action(db, mid);   /* REQ-058 */
    if (j->n_poll_opts) store_poll(db, mid, j->channel_id, j, ts);   /* REQ-225 */
    if (is_todo) store_todo(db, mid, j->channel_id, j->user_id, j->body, &tp, ts);

    /* The draft this message came from is gone (REQ-223, ARCH-101), in the same
     * transaction as the send. Server-side rather than a client courtesy: a
     * client that dies between the send and its own cleanup would otherwise
     * leave the sent text sitting in the composer of every other device. */
    drop_draft(db, j->user_id, j->channel_id, 0);
    owh_message(db, mid, OWH_MSG_NEW);   /* queued with the message it carries */

    sum_purge_msg(db, mid);
    sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);

    r->type = OC_RES_SEND_OK;
    r->message_id = mid;
    r->server_time = ts;
    r->author_name = lookup_display_name(db, j->user_id);   /* name for the live BROADCAST */
    if (j->via && j->via[0]) r->via = strdup(j->via);
    /* Read the reference back rather than echoing what was asked for: what the
     * channel is told is what actually landed in the table, or nothing. */
    append_forward(db, r, mid, j->channel_id);
    /* Always, action or not: the client pairs a live message with the ACTION
     * that follows it, so a toast or a preview never sees one without the
     * other (REQ-058, ARCH-115). */
    append_action(db, r, mid, j->channel_id, 1);
    /* The poll, read back as the forward is, for the frame after the ACTION. */
    if (j->n_poll_opts) append_poll(db, r, mid, j->channel_id, 0);
    if (is_todo) append_todo(db, r, mid);   /* and the list, after it */
    if (j->body_len) { r->body = malloc(j->body_len); if (r->body) { memcpy(r->body, j->body, j->body_len); r->body_len = j->body_len; } }
    r->broadcast_stored = broadcast_stored(db, mid);
    load_members(db, j->channel_id, r);
    withhold_from_blockers(db, j->channel_id, j->user_id, mid, r);   /* in a DM, not to whoever blocks the author */
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
/* Whether an author's own edit (`edit` set) or delete of message `mid` is still
 * open under the workspace's window (OPENCHIME_EDIT_WINDOW / _DELETE_WINDOW):
 * always unless set, never under `never`, else for that long after it was
 * sent, by the daemon's clock. A change to the setting applies to every
 * message from then on. */
static int author_window_open(sqlite3 *db, uint64_t mid, int edit) {
    const oc_config *cfg = oc_config_get();
    int64_t win = oc_config_window_ms(edit ? cfg->edit_window : cfg->delete_window);
    if (win < 0) return 1;
    if (win == 0) return 0;
    sqlite3_stmt *st = NULL;
    int64_t sent = 0;
    sqlite3_prepare_v2(db, "SELECT created_at_ms FROM messages WHERE id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)mid);
    if (sqlite3_step(st) == SQLITE_ROW) sent = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return (int64_t)dbw_now_ms() - sent <= win;
}

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
    {   /* A to-do list's message is not edited: its title is renamed and its
         * tasks added, and the body that made them stays as it was written. */
        sqlite3_stmt *tq = NULL;
        int is_todo = 0;
        if (sqlite3_prepare_v2(db, "SELECT 1 FROM todo_lists WHERE message_id=?1;", -1, &tq, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(tq, 1, (sqlite3_int64)j->message_id);
            is_todo = sqlite3_step(tq) == SQLITE_ROW;
            sqlite3_finalize(tq);
        }
        if (is_todo) { r->type = OC_RES_EDIT_ERR; r->err_code = OC_ERR_TODO_NO_EDIT; return r; }
    }
    if (author != j->user_id) {   /* no moderator edit — author only (REQ-032) */
        r->type = OC_RES_EDIT_ERR; r->err_code = OC_ERR_FORBIDDEN; return r;
    }
    if (!author_window_open(db, j->message_id, 1)) {
        r->type = OC_RES_EDIT_ERR; r->err_code = OC_ERR_EDIT_WINDOW_CLOSED; return r;
    }
    if (e2e_closed(db, j->channel_id)) { r->type = OC_RES_EDIT_ERR; r->err_code = OC_ERR_CHANNEL_ARCHIVED; return r; }

    uint64_t ts = dbw_now_ms();
    sqlite3_stmt *st = NULL;
    /* One transaction: the body and the action it may make (REQ-058) change
     * together or not at all. */
    sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);
    sqlite3_prepare_v2(db,
        "UPDATE messages SET body=?, edited_at_ms=? WHERE id=?;", -1, &st, NULL);
    sqlite3_bind_blob(st, 1, j->body, (int)j->body_len, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)ts);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)j->message_id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        r->type = OC_RES_EDIT_ERR; r->err_code = OC_ERR_INTERNAL; return r;
    }

    /* Any stored unfurls describe the OLD body. Drop them all; the net thread
     * re-extracts from the new body and re-fetches, and the store step
     * re-validates presence — so a URL that survived the edit gets its preview
     * back within seconds, and one that left cannot be replayed (ARCH-105). */
    sqlite3_prepare_v2(db, "DELETE FROM unfurls WHERE message_id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->message_id);
    sqlite3_step(st);
    sqlite3_finalize(st);
    /* An edit can make an action or unmake one (REQ-058). */
    store_action(db, j->message_id);
    owh_message(db, j->message_id, OWH_MSG_CHANGED);
    sum_purge_msg(db, j->message_id);
    sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);

    r->ch_e2e = (uint8_t)channel_is_e2e(db, j->channel_id);
    r->type = OC_RES_EDIT_OK;
    r->author_id = author;
    r->server_time = ts;   /* edited_at_ms */
    append_action(db, r, j->message_id, j->channel_id, 1);
    if (j->body_len) { r->body = malloc(j->body_len); if (r->body) { memcpy(r->body, j->body, j->body_len); r->body_len = j->body_len; } }
    load_members(db, j->channel_id, r);
    return r;
}

/* Store a completed link unfurl (REQ-222, ARCH-105) and hand the net thread
 * its fan-out. Submitted by the unfurl worker (conn_id 0), never by a client
 * frame. The message is re-validated here: it may have been tombstoned — or
 * edited so the URL is gone — while the fetch was in flight, and a preview for
 * text that no longer exists must be neither stored nor announced. The silent
 * outcome is OC_RES_OK, which the net thread ignores. Write. */
static oc_dbres *process_unfurl_store(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = 0;
    r->type = OC_RES_OK;
    if (!j->unf_url || !j->unf_title) return r;

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT channel_id, deleted_at_ms IS NOT NULL, body "
            "FROM messages WHERE id=?1;", -1, &st, NULL) != SQLITE_OK)
        return r;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->message_id);
    int present = 0;
    if (sqlite3_step(st) == SQLITE_ROW &&
        (uint64_t)sqlite3_column_int64(st, 0) == j->channel_id &&
        sqlite3_column_int(st, 1) == 0) {
        const char *body = (const char *)sqlite3_column_blob(st, 2);
        size_t body_len  = (size_t)sqlite3_column_bytes(st, 2);
        oc_url_span sp[OC_UNFURL_MAX_URLS];
        /* Nor one an edit has since put in a spoiler, with its fetch in flight
         * (MARKDOWN.md §1d). */
        size_t nn = body ? oc_rt_visible_urls(body, body_len, sp, OC_UNFURL_MAX_URLS) : 0;
        size_t ul = strlen(j->unf_url);
        for (size_t i = 0; i < nn; i++)
            if (sp[i].len == ul && memcmp(body + sp[i].start, j->unf_url, ul) == 0)
                { present = 1; break; }
    }
    sqlite3_finalize(st);
    if (!present) return r;

    uint64_t ts = dbw_now_ms();
    sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO unfurls(message_id, channel_id, url, title, descr, created_at_ms) "
        "VALUES(?, ?, ?, ?, ?, ?);", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->message_id);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)j->channel_id);
    sqlite3_bind_text(st, 3, j->unf_url, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 4, j->unf_title, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 5, j->unf_descr ? j->unf_descr : "", -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 6, (sqlite3_int64)ts);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) return r;

    r->type = OC_RES_UNFURL_STORED;
    r->message_id = j->message_id;
    r->channel_id = j->channel_id;
    r->unf_url   = strdup(j->unf_url);
    r->unf_title = strdup(j->unf_title);
    r->unf_descr = strdup(j->unf_descr ? j->unf_descr : "");
    load_members(db, j->channel_id, r);
    return r;
}

/* Delete a message as a tombstone (REQ-052): the author may delete their own,
 * and an admin/owner who belongs to the channel may delete any (moderation,
 * REQ-032). The body is nulled while id/author/timestamps survive; deleted_by
 * records who removed it, distinguishing a self- from a moderator-delete. */
/* How long a delete can be taken back (REQ-052), unless a test sets its own. */
#define OC_RESTORE_WINDOW_MS 120000u

/* Holds past the window go: what a delete gave up is kept no longer than it can
 * be asked back. */
static void holds_purge(sqlite3 *db, uint64_t now, uint64_t window) {
    uint64_t cut = now > window ? now - window : 0;
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "DELETE FROM deleted_hold_reactions WHERE message_id IN "
                           "(SELECT message_id FROM deleted_holds WHERE deleted_at_ms < ?1);", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)cut); sqlite3_step(st); sqlite3_finalize(st);
    sqlite3_prepare_v2(db, "DELETE FROM deleted_holds WHERE deleted_at_ms < ?1;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)cut); sqlite3_step(st); sqlite3_finalize(st);
}

/* Keep what a delete is about to give up, so it can be taken back: the body,
 * the files' ids, the reactions. */
static void hold_deleted(sqlite3 *db, uint64_t message_id, uint64_t by, uint64_t at) {
    sqlite3_stmt *st = NULL;
    {   /* An encrypted DM's message is not kept for an undo: its key is gone. */
        uint64_t cid = 0;
        if (sqlite3_prepare_v2(db, "SELECT channel_id FROM messages WHERE id=?1;", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)message_id);
            if (sqlite3_step(st) == SQLITE_ROW) cid = (uint64_t)sqlite3_column_int64(st, 0);
        }
        sqlite3_finalize(st);
        st = NULL;
        if (cid && channel_is_e2e(db, cid)) return;
    }
    char ids[OC_MAX_ATTACH * 21 + 1] = "";
    size_t n = 0;
    sqlite3_prepare_v2(db, "SELECT id FROM attachments WHERE message_id=? ORDER BY id;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)message_id);
    while (sqlite3_step(st) == SQLITE_ROW && n + 22 < sizeof ids)
        n += (size_t)snprintf(ids + n, sizeof ids - n, "%s%lld", n ? "," : "", (long long)sqlite3_column_int64(st, 0));
    sqlite3_finalize(st);
    sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO deleted_holds(message_id, body, deleted_by, deleted_at_ms, attach_ids) "
                           "SELECT id, body, ?2, ?3, ?4 FROM messages WHERE id=?1;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)message_id);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)by);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)at);
    sqlite3_bind_text(st, 4, ids, -1, SQLITE_STATIC);
    sqlite3_step(st); sqlite3_finalize(st);
    sqlite3_prepare_v2(db, "DELETE FROM deleted_hold_reactions WHERE message_id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)message_id); sqlite3_step(st); sqlite3_finalize(st);
    sqlite3_prepare_v2(db, "INSERT INTO deleted_hold_reactions(message_id, user_id, emoji, created_at_ms) "
                           "SELECT message_id, user_id, emoji, created_at_ms FROM reactions WHERE message_id=?;",
                       -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)message_id); sqlite3_step(st); sqlite3_finalize(st);
}

/* Take a delete back (REQ-052): only the person who made it, only while it is
 * held, and only into a channel they still belong to. The body, its files --
 * those the storage sweep has not reclaimed -- and its reactions come back; a
 * pin and link previews do not (a pin is a deliberate act of its own, previews
 * are drawn again). Audited when it undoes a moderator's delete, as that delete
 * was. Write. */
static oc_dbres *process_restore(oc_dbwriter *w, const oc_job *j) {
    sqlite3 *db = w->db;
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->channel_id = j->channel_id;
    r->message_id = j->message_id;
    uint64_t now = dbw_now_ms();
    holds_purge(db, now, w->restore_window_ms);
    uint64_t author = 0; int deleted = 0;
    sqlite3_stmt *st = NULL;
    char *body = NULL, ids[OC_MAX_ATTACH * 21 + 1] = "";
    uint64_t by = 0;
    int held = 0;
    if (message_lookup(db, j->channel_id, j->message_id, &author, &deleted) && deleted) {
        sqlite3_prepare_v2(db, "SELECT body, deleted_by, attach_ids FROM deleted_holds WHERE message_id=?;",
                           -1, &st, NULL);
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->message_id);
        if (sqlite3_step(st) == SQLITE_ROW) {
            const char *b = (const char *)sqlite3_column_text(st, 0), *a = (const char *)sqlite3_column_text(st, 2);
            body = strdup(b ? b : "");
            by = (uint64_t)sqlite3_column_int64(st, 1);
            snprintf(ids, sizeof ids, "%s", a ? a : "");
            held = 1;
        }
        sqlite3_finalize(st);
    }
    if (!held || !body) {
        free(body);
        r->type = OC_RES_RESTORE_ERR; r->err_code = OC_ERR_UNKNOWN_MESSAGE; return r;   /* gone for good */
    }
    if (by != j->user_id || !is_member(db, j->channel_id, j->user_id)) {
        free(body);
        r->type = OC_RES_RESTORE_ERR; r->err_code = OC_ERR_FORBIDDEN; return r;
    }
    sqlite3_prepare_v2(db, "UPDATE messages SET body=?, deleted_at_ms=NULL, deleted_by=NULL WHERE id=?;", -1, &st, NULL);
    sqlite3_bind_text(st, 1, body, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)j->message_id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) { free(body); r->type = OC_RES_RESTORE_ERR; r->err_code = OC_ERR_INTERNAL; return r; }
    sum_purge_msg(db, j->message_id);
    for (char *p = ids; *p; ) {
        long long aid = strtoll(p, &p, 10);
        if (aid > 0) {
            sqlite3_prepare_v2(db, "UPDATE attachments SET message_id=?1 WHERE id=?2 AND message_id IS NULL "
                                   "AND reclaimed_at_ms = 0;", -1, &st, NULL);
            sqlite3_bind_int64(st, 1, (sqlite3_int64)j->message_id);
            sqlite3_bind_int64(st, 2, (sqlite3_int64)aid);
            sqlite3_step(st); sqlite3_finalize(st);
        }
        while (*p == ',') p++;
    }
    sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO reactions(message_id, user_id, emoji, created_at_ms) "
                           "SELECT message_id, user_id, emoji, created_at_ms FROM deleted_hold_reactions "
                           "WHERE message_id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->message_id); sqlite3_step(st); sqlite3_finalize(st);
    sqlite3_prepare_v2(db, "DELETE FROM deleted_hold_reactions WHERE message_id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->message_id); sqlite3_step(st); sqlite3_finalize(st);
    sqlite3_prepare_v2(db, "DELETE FROM deleted_holds WHERE message_id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->message_id); sqlite3_step(st); sqlite3_finalize(st);
    /* The action comes back with the body that made it (REQ-058). */
    store_action(db, j->message_id);
    if (author != j->user_id)
        audit_actor_in(db, OC_AUDIT_MODERATION, "message.restore", j->user_id, j->channel_id,
                       j->message_id, NULL, 1, "moderator delete undone");
    r->type = OC_RES_RESTORE_OK;
    r->author_id = author;
    r->body = (uint8_t *)body;
    r->body_len = strlen(body);
    r->broadcast_stored = broadcast_stored(db, j->message_id);
    sqlite3_prepare_v2(db, "SELECT created_at_ms FROM messages WHERE id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->message_id);
    if (sqlite3_step(st) == SQLITE_ROW) r->server_time = (uint64_t)sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    load_message_attachments(db, j->message_id, r->attach, &r->n_attach);
    append_action(db, r, j->message_id, j->channel_id, 1);
    append_todo(db, r, j->message_id);   /* its list, as it stood when deleted */
    load_members(db, j->channel_id, r);
    return r;
}

static oc_dbres *process_delete(oc_dbwriter *w, const oc_job *j) {
    sqlite3 *db = w->db;
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
    /* The window is an author's, for their own message: a moderator removing
     * someone else's is never limited (REQ-032). */
    if (author == j->user_id && !author_window_open(db, j->message_id, 0)) {
        r->type = OC_RES_DELETE_ERR; r->err_code = OC_ERR_EDIT_WINDOW_CLOSED; return r;
    }
    if (e2e_closed(db, j->channel_id)) { r->type = OC_RES_DELETE_ERR; r->err_code = OC_ERR_CHANNEL_ARCHIVED; return r; }

    uint64_t ts = dbw_now_ms();
    holds_purge(db, ts, w->restore_window_ms);
    hold_deleted(db, j->message_id, j->user_id, ts);   /* before anything below gives it up */
    sqlite3_stmt *st = NULL;
    int own = owh_txn_begin(db);   /* the tombstone and its webhooks' event together */
    sqlite3_prepare_v2(db,
        "UPDATE messages SET body=NULL, deleted_at_ms=?, deleted_by=? WHERE id=?;",
        -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)ts);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)j->user_id);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)j->message_id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        if (own) sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        r->type = OC_RES_DELETE_ERR; r->err_code = OC_ERR_INTERNAL; return r;
    }
    sum_purge_msg(db, j->message_id);
    /* Detach the attachments (REQ-052). The row is not deleted and the blob is
     * not deleted here: setting message_id NULL makes them *orphans*, which is
     * exactly the state the storage-maintenance orphan sweep already collects
     * (ARCH-78) — so the bytes are reclaimed by a path that is already written
     * and tested, on its own schedule, off the writer thread. What matters for
     * correctness is that they leave the message immediately: a tombstone that
     * still lists a file is offering something it no longer has. A snippet's
     * words leave the search index with it (REQ-226). */
    sqlite3_prepare_v2(db, "DELETE FROM snippets_fts WHERE rowid IN "
                           "(SELECT id FROM attachments WHERE message_id=?);", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->message_id); sqlite3_step(st); sqlite3_finalize(st);
    sqlite3_prepare_v2(db, "UPDATE attachments SET message_id=NULL WHERE message_id=?;",
                       -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->message_id); sqlite3_step(st); sqlite3_finalize(st);

    /* A tombstone has nothing to pin to either (REQ-052/230). Dropped before the
     * reactions so the order matches the table dependencies. */
    sqlite3_prepare_v2(db, "DELETE FROM pins WHERE message_id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->message_id); sqlite3_step(st); sqlite3_finalize(st);

    /* Nor anything to be reminded of (REQ-283): the reminders go now, so a
     * restore does not bring one back to fire late. The saved rows stay hidden
     * behind the tombstone, as they always have, for a restore to show again. */
    sqlite3_prepare_v2(db, "UPDATE saved_items SET remind_at_ms=NULL, reminded_at_ms=NULL WHERE message_id=?;",
                       -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->message_id); sqlite3_step(st); sqlite3_finalize(st);

    /* A tombstone has no links to preview either (REQ-052/222). */
    sqlite3_prepare_v2(db, "DELETE FROM unfurls WHERE message_id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->message_id); sqlite3_step(st); sqlite3_finalize(st);

    /* Nor a body to be an action (REQ-058); a restore re-derives it. */
    store_action(db, j->message_id);

    /* A tombstone has no body to react to; drop its reactions (REQ-052/070). */
    sqlite3_prepare_v2(db, "DELETE FROM reactions WHERE message_id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->message_id);
    sqlite3_step(st);
    sqlite3_finalize(st);
    /* Nor a question to vote on: the poll goes with it (REQ-225). A restore
     * brings back the question as a message, not the poll -- as with pins. */
    drop_poll(db, j->message_id);
    owh_message(db, j->message_id, OWH_MSG_DELETED);
    owh_txn_end(db, own);

    /* Only a MODERATOR delete is auditable governance; a user removing their
     * own message is ordinary use and would drown the log (REQ-032 already
     * distinguishes the two via deleted_by). */
    if (author != j->user_id)
        audit_actor_in(db, OC_AUDIT_MODERATION, "message.delete", j->user_id, j->channel_id,
                       j->message_id, NULL, 1, "moderator delete");

    r->type = OC_RES_DELETE_OK;
    r->author_id = author;
    r->user_id = j->user_id;   /* deleted_by */
    r->server_time = ts;       /* deleted_at_ms */
    /* A reply's thread names its repliers afresh, without this one if it was
     * their only reply (REQ-060): the net thread sends the parent's THREAD_META. */
    {
        sqlite3_stmt *pq = NULL;
        if (sqlite3_prepare_v2(db, "SELECT COALESCE(parent_id,0) FROM messages WHERE id=?1;",
                               -1, &pq, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(pq, 1, (sqlite3_int64)j->message_id);
            if (sqlite3_step(pq) == SQLITE_ROW) r->parent_id = (uint64_t)sqlite3_column_int64(pq, 0);
            sqlite3_finalize(pq);
        }
        if (r->parent_id) {
            thread_stats(db, r->parent_id, &r->reply_count, &r->thread_last_reply_at);
            r->n_repliers = thread_repliers(db, r->parent_id, r->repliers);
        }
    }
    load_members(db, j->channel_id, r);
    return r;
}

/* Fill a result's ch_* fields from the channel row + the actor's membership.
 * Returns 1 if the channel exists (result is a valid CHANNEL_INFO), 0 if not. */
static int load_channel_info(sqlite3 *db, uint64_t channel_id, uint64_t actor, oc_dbres *r) {
    sqlite3_stmt *st = NULL;
    int found = 0;
    sqlite3_prepare_v2(db,
        "SELECT kind, name, is_public, created_at_ms, topic, archived_at_ms, e2e, COALESCE(created_by,0) "
        "  FROM channels WHERE id=?;",
        -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)channel_id);
    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *kn = sqlite3_column_text(st, 0);
        const unsigned char *nm = sqlite3_column_text(st, 1);
        r->ch_kind       = (kn && strcmp((const char *)kn, "dm") == 0) ? OC_CHANNEL_KIND_DM : OC_CHANNEL_KIND;
        r->ch_name       = strdup(nm ? (const char *)nm : "");
        r->ch_is_public  = (uint8_t)(sqlite3_column_int(st, 2) != 0);
        r->ch_created_at = (uint64_t)sqlite3_column_int64(st, 3);
        const unsigned char *tp = sqlite3_column_text(st, 4);
        r->ch_topic      = (tp && tp[0]) ? strdup((const char *)tp) : NULL;
        r->ch_archived   = (uint8_t)(sqlite3_column_type(st, 5) != SQLITE_NULL);
        r->ch_e2e        = (uint8_t)(sqlite3_column_int(st, 6) != 0);
        r->ch_creator    = (uint64_t)sqlite3_column_int64(st, 7);
        r->channel_id    = channel_id;
        found = 1;
    }
    sqlite3_finalize(st);
    /* Its managers (REQ-036b), in the order they were named. */
    free(r->ch_managers); r->ch_managers = NULL; r->n_ch_managers = 0;
    if (found && (r->ch_managers = calloc(OC_MAX_CHANNEL_MANAGERS, sizeof *r->ch_managers)) != NULL) {
        sqlite3_prepare_v2(db, "SELECT user_id FROM channel_managers WHERE channel_id=?1 "
                               "ORDER BY granted_at_ms, user_id LIMIT ?2;", -1, &st, NULL);
        sqlite3_bind_int64(st, 1, (sqlite3_int64)channel_id);
        sqlite3_bind_int(st, 2, (int)OC_MAX_CHANNEL_MANAGERS);
        while (r->n_ch_managers < OC_MAX_CHANNEL_MANAGERS && sqlite3_step(st) == SQLITE_ROW)
            r->ch_managers[r->n_ch_managers++] = (uint64_t)sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    if (found) r->ch_joined = (uint8_t)(is_member(db, channel_id, actor) ? 1 : 0);
    if (found) r->n_ch_groups = channel_group_ids(db, channel_id, r->ch_groups, OC_MAX_CHANNEL_GROUPS);
    /* A DM with more than two participants is a GROUP DM (REQ-056) — the same
     * `kind`, distinguished by its participant set, which is what a DM's identity
     * has always been. Load them so the client can name it. */
    r->n_ch_peers = 0;
    if (found && r->ch_kind == OC_CHANNEL_KIND_DM) {
        sqlite3_stmt *ps = NULL;
        sqlite3_prepare_v2(db,
            "SELECT user_id FROM channel_members WHERE channel_id=? "
            "ORDER BY joined_at_ms, user_id LIMIT 9;", -1, &ps, NULL);
        sqlite3_bind_int64(ps, 1, (sqlite3_int64)channel_id);
        uint64_t tmp[9]; int tn = 0;
        while (sqlite3_step(ps) == SQLITE_ROW && tn < 9)
            tmp[tn++] = (uint64_t)sqlite3_column_int64(ps, 0);
        sqlite3_finalize(ps);
        if (tn > 2) {
            for (int i = 0; i < tn; i++) r->ch_peers[i] = tmp[i];
            r->n_ch_peers = (uint16_t)tn;
        }
    }
    return found;
}

/* Change a channel: topic, name, archive, unarchive (REQ-034/035/036, ARCH-93).
 *
 * One handler for four verbs because they are one row's state and one fan-out.
 * Authority splits on blast radius: a topic is already visible to the channel
 * and trivially corrected, so any member may set it; a rename or an archive
 * changes what people who are NOT looking at the channel see, so both are
 * owner/admin. */
/* A channel's change, said in it (OPENCHIME_NOTICE_CHANNEL_CHANGES): who, and
 * `what` they did. */
static void channel_change_notice(sqlite3 *db, const oc_job *j, const char *what) {
    if (!oc_config_get()->notice_channel_changes) return;
    char *who = audit_name_of(db, j->user_id);
    notice_channel(db, j->channel_id, "@%s %s", who ? who : "someone", what);
    free(who);
}

/* Is `user_id` a manager of `channel_id` (REQ-036b)? */
static int is_channel_manager(sqlite3 *db, uint64_t channel_id, uint64_t user_id) {
    sqlite3_stmt *st = NULL;
    int yes = 0;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM channel_managers WHERE channel_id=?1 AND user_id=?2;",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)channel_id);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)user_id);
        yes = sqlite3_step(st) == SQLITE_ROW;
    }
    sqlite3_finalize(st);
    return yes;
}

/* Grant (`grant` 1) or revoke a channel's manager role (REQ-036b, ARCH-134):
 * by a manager of the channel, an admin or an owner; granted only to a member,
 * revoked from anyone holding it. Holding it already, or not holding it, is a
 * success that changes nothing. Told to every member as CHANNEL_INFO. */
static oc_dbres *process_channel_manager(sqlite3 *db, const oc_job *j, int grant) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->channel_id = j->channel_id;
    sqlite3_stmt *st = NULL;
    int exists = 0, is_dm = 0;
    sqlite3_prepare_v2(db, "SELECT kind='dm' FROM channels WHERE id=?1;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->channel_id);
    if (sqlite3_step(st) == SQLITE_ROW) { exists = 1; is_dm = sqlite3_column_int(st, 0); }
    sqlite3_finalize(st);
    if (!exists) { r->type = OC_RES_CHANNEL_ERR; r->err_code = OC_ERR_UNKNOWN_CHANNEL; return r; }
    if (is_dm)   { r->type = OC_RES_CHANNEL_ERR; r->err_code = OC_ERR_INVALID_CHANNEL; return r; }
    uint8_t role = OC_ROLE_MEMBER;
    user_role(db, j->user_id, &role);
    if (!oc_role_can_moderate(role) && !is_channel_manager(db, j->channel_id, j->user_id)) {
        r->type = OC_RES_CHANNEL_ERR; r->err_code = OC_ERR_FORBIDDEN; return r;
    }
    uint64_t who = j->target_user_id;
    int changed = 0;
    if (grant) {
        if (!who || !is_member(db, j->channel_id, who)) {
            r->type = OC_RES_CHANNEL_ERR; r->err_code = OC_ERR_NOT_A_MEMBER; return r;
        }
        if (!is_channel_manager(db, j->channel_id, who)) {
            int n = 0;
            sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM channel_managers WHERE channel_id=?1;", -1, &st, NULL);
            sqlite3_bind_int64(st, 1, (sqlite3_int64)j->channel_id);
            if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
            sqlite3_finalize(st);
            if (n >= (int)OC_MAX_CHANNEL_MANAGERS) {
                r->type = OC_RES_CHANNEL_ERR; r->err_code = OC_ERR_INVALID_CHANNEL; return r;
            }
            sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO channel_managers(channel_id, user_id, granted_by, granted_at_ms) "
                                   "VALUES(?1, ?2, ?3, ?4);", -1, &st, NULL);
            sqlite3_bind_int64(st, 1, (sqlite3_int64)j->channel_id);
            sqlite3_bind_int64(st, 2, (sqlite3_int64)who);
            sqlite3_bind_int64(st, 3, (sqlite3_int64)j->user_id);
            sqlite3_bind_int64(st, 4, (sqlite3_int64)dbw_now_ms());
            changed = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db) == 1;
            sqlite3_finalize(st);
        }
    } else {
        sqlite3_prepare_v2(db, "DELETE FROM channel_managers WHERE channel_id=?1 AND user_id=?2;", -1, &st, NULL);
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->channel_id);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)who);
        changed = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db) == 1;
        sqlite3_finalize(st);
    }
    if (changed) {
        char cid[24];
        snprintf(cid, sizeof cid, "%llu", (unsigned long long)j->channel_id);
        audit_actor_in(db, OC_AUDIT_ADMIN, grant ? "channel.manager.grant" : "channel.manager.revoke",
                       j->user_id, j->channel_id, who, cid, 1, NULL);
    }
    r->type = OC_RES_CHANNEL_INFO;
    load_channel_info(db, j->channel_id, j->user_id, r);
    r->ch_fanout = 1;              /* every member's view of who runs it */
    load_members(db, j->channel_id, r);
    return r;
}

static oc_dbres *process_update_channel(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->channel_id = j->channel_id;

    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "SELECT kind FROM channels WHERE id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->channel_id);
    int exists = 0, is_dm = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *kn = sqlite3_column_text(st, 0);
        exists = 1; is_dm = (kn && strcmp((const char *)kn, "dm") == 0);
    }
    sqlite3_finalize(st);
    if (!exists) { r->type = OC_RES_CHANNEL_ERR; r->err_code = OC_ERR_UNKNOWN_CHANNEL; return r; }
    /* A DM has no name to rename, no topic worth setting, and archiving one is
     * "close the conversation" — a different feature (REQ-235-adjacent), not
     * this one. */
    if (is_dm) { r->type = OC_RES_CHANNEL_ERR; r->err_code = OC_ERR_INVALID_CHANNEL; return r; }
    if (!is_member(db, j->channel_id, j->user_id)) {
        r->type = OC_RES_CHANNEL_ERR; r->err_code = OC_ERR_NOT_A_MEMBER; return r;
    }

    uint8_t role = OC_ROLE_MEMBER;
    user_role(db, j->user_id, &role);
    size_t vlen = j->ch_name ? strlen(j->ch_name) : 0;

    /* A demo's channels say what the demo is: their text is a moderator's. */
    if (g_dbw_demo && (j->chup_op == OC_CHUP_TOPIC || j->chup_op == OC_CHUP_DESCRIPTION) &&
        !oc_role_can_moderate(role)) {
        r->type = OC_RES_CHANNEL_ERR; r->err_code = OC_ERR_FORBIDDEN; return r;
    }
    if (j->chup_op == OC_CHUP_TOPIC) {
        if (vlen > OC_MAX_TOPIC) {
            r->type = OC_RES_CHANNEL_ERR; r->err_code = OC_ERR_INVALID_CHANNEL; return r;
        }
        sqlite3_prepare_v2(db, "UPDATE channels SET topic=? WHERE id=?;", -1, &st, NULL);
        if (vlen) sqlite3_bind_text(st, 1, j->ch_name, (int)vlen, SQLITE_STATIC);
        else      sqlite3_bind_null(st, 1);          /* "" clears the topic */
        sqlite3_bind_int64(st, 2, (sqlite3_int64)j->channel_id);
        sqlite3_step(st); sqlite3_finalize(st);
        audit_actor_in(db, OC_AUDIT_ADMIN, "channel.topic", j->user_id, j->channel_id, 0, NULL, 1, NULL);
    } else if (j->chup_op == OC_CHUP_DESCRIPTION) {
        /* Any member, on the topic's reasoning (ARCH-93): it is visible to
         * everyone in the channel and a wrong one is corrected in seconds. */
        if (vlen > OC_MAX_DESCRIPTION) {
            r->type = OC_RES_CHANNEL_ERR; r->err_code = OC_ERR_INVALID_CHANNEL; return r;
        }
        sqlite3_prepare_v2(db, "UPDATE channels SET description=? WHERE id=?;", -1, &st, NULL);
        if (vlen) sqlite3_bind_text(st, 1, j->ch_name, (int)vlen, SQLITE_STATIC);
        else      sqlite3_bind_null(st, 1);          /* "" clears it */
        sqlite3_bind_int64(st, 2, (sqlite3_int64)j->channel_id);
        sqlite3_step(st); sqlite3_finalize(st);
        audit_actor_in(db, OC_AUDIT_ADMIN, "channel.description", j->user_id, j->channel_id, 0, NULL, 1, NULL);
        channel_change_notice(db, j, vlen ? "changed this channel's description." : "cleared this channel's description.");
        /* Announced on its own frame, not CHANNEL_INFO: the text does not ride
         * the channel-state frame, so a sidebar has nothing to redraw -- only a
         * member looking at the About has anything to show. */
        r->type = OC_RES_CHANNEL_DESCRIPTION;
        if (vlen) {
            r->body = malloc(vlen);
            if (r->body) { memcpy(r->body, j->ch_name, vlen); r->body_len = vlen; }
        }
        r->ch_fanout = 1;
        load_members(db, j->channel_id, r);
        return r;
    } else if (j->chup_op == OC_CHUP_RENAME) {
        /* An owner or admin, or a manager of this channel (REQ-036b). */
        if (!oc_role_can_moderate(role) && !is_channel_manager(db, j->channel_id, j->user_id)) {
            r->type = OC_RES_CHANNEL_ERR; r->err_code = OC_ERR_FORBIDDEN; return r;
        }
        if (vlen == 0 || vlen > OC_MAX_CHANNEL_NAME) {
            r->type = OC_RES_CHANNEL_ERR; r->err_code = OC_ERR_INVALID_CHANNEL; return r;
        }
        /* Pre-check so the client gets a usable error instead of a constraint
         * failure — the same shape migration 0020 established for create. */
        sqlite3_prepare_v2(db,
            "SELECT 1 FROM channels WHERE kind='channel' AND lower(name)=lower(?) AND id<>?;",
            -1, &st, NULL);
        sqlite3_bind_text(st, 1, j->ch_name, (int)vlen, SQLITE_STATIC);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)j->channel_id);
        int taken = (sqlite3_step(st) == SQLITE_ROW);
        sqlite3_finalize(st);
        if (taken) { r->type = OC_RES_CHANNEL_ERR; r->err_code = OC_ERR_CHANNEL_EXISTS; return r; }

        int own = owh_txn_begin(db);
        sqlite3_prepare_v2(db, "UPDATE channels SET name=? WHERE id=?;", -1, &st, NULL);
        sqlite3_bind_text(st, 1, j->ch_name, (int)vlen, SQLITE_STATIC);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)j->channel_id);
        sqlite3_step(st); sqlite3_finalize(st);
        owh_channel(db, j->channel_id, j->user_id, "channel_rename");
        owh_txn_end(db, own);
        /* The id is untouched, so membership, history and cursors all follow the
         * rename for free — nothing durable was ever keyed on the name. */
        audit_actor_in(db, OC_AUDIT_ADMIN, "channel.rename", j->user_id, j->channel_id, 0, j->ch_name, 1, NULL);
        {
            char what[OC_MAX_CHANNEL_NAME + 48];
            snprintf(what, sizeof what, "renamed this channel to #%.*s.", (int)vlen, j->ch_name);
            channel_change_notice(db, j, what);
        }
    } else if (j->chup_op == OC_CHUP_PRIVATE || j->chup_op == OC_CHUP_PUBLIC) {
        /* Visibility (REQ-031). Owner/admin, like a rename and an archive: it changes
         * what people who are NOT in the channel can see, which is the line this
         * codebase draws between "any member" and "moderator". */
        if (!oc_role_can_moderate(role)) {
            r->type = OC_RES_CHANNEL_ERR; r->err_code = OC_ERR_FORBIDDEN; return r;
        }
        int pub = (j->chup_op == OC_CHUP_PUBLIC);
        /* No membership surgery in either direction, and that is deliberate:
         *
         *   PUBLIC -> PRIVATE: read access is `is_public=1 OR is_member`, so the flag
         *   alone pins the audience to the people who actually JOINED. Anyone who was
         *   only browsing loses access, which is what "make it private" means.
         *
         *   PRIVATE -> PUBLIC: nobody is added, because membership is a subscription
         *   (it drives the sidebar and delivery cursors), not permission. Everyone can
         *   now READ it; the members are still the members.
         *
         * The history is untouched either way — a channel's messages have never been
         * keyed on its visibility, so there is nothing to migrate and nothing that can
         * be half-migrated. */
        sqlite3_prepare_v2(db, "UPDATE channels SET is_public=? WHERE id=?;", -1, &st, NULL);
        sqlite3_bind_int  (st, 1, pub ? 1 : 0);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)j->channel_id);
        sqlite3_step(st); sqlite3_finalize(st);
        /* Audited as two distinct actions rather than one "visibility changed": going
         * public is a disclosure of everything said in there while it was private, and
         * an audit reader should not have to open the row to see which way it went. */
        audit_actor_in(db, OC_AUDIT_ADMIN, pub ? "channel.public" : "channel.private",
                       j->user_id, j->channel_id, 0, NULL, 1, NULL);
        channel_change_notice(db, j, pub ? "made this channel public." : "made this channel private.");
    } else if (j->chup_op == OC_CHUP_ARCHIVE || j->chup_op == OC_CHUP_UNARCHIVE) {
        if (!oc_role_can_moderate(role) && !is_channel_manager(db, j->channel_id, j->user_id)) {
            r->type = OC_RES_CHANNEL_ERR; r->err_code = OC_ERR_FORBIDDEN; return r;
        }
        int on = (j->chup_op == OC_CHUP_ARCHIVE);
        sqlite3_prepare_v2(db, "UPDATE channels SET archived_at_ms=? WHERE id=?;", -1, &st, NULL);
        if (on) sqlite3_bind_int64(st, 1, (sqlite3_int64)dbw_now_ms());
        else    sqlite3_bind_null(st, 1);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)j->channel_id);
        int own = owh_txn_begin(db);
        sqlite3_step(st); sqlite3_finalize(st);
        owh_channel(db, j->channel_id, j->user_id, on ? "channel_archive" : "channel_unarchive");
        owh_txn_end(db, own);
        audit_actor_in(db, OC_AUDIT_ADMIN, on ? "channel.archive" : "channel.unarchive",
                       j->user_id, j->channel_id, 0, NULL, 1, NULL);
        channel_change_notice(db, j, on ? "archived this channel." : "unarchived this channel.");
    } else {
        r->type = OC_RES_CHANNEL_ERR; r->err_code = OC_ERR_INVALID_CHANNEL; return r;
    }

    r->type = OC_RES_CHANNEL_INFO;
    load_channel_info(db, j->channel_id, j->user_id, r);
    r->ch_fanout = 1;              /* everyone's sidebar is affected, not just mine */
    load_members(db, j->channel_id, r);
    return r;
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
        "INSERT INTO channels(kind,name,is_public,created_at_ms,created_by) VALUES('channel',?,?,?,?);",
        -1, &st, NULL);
    sqlite3_bind_text(st, 1, j->ch_name, (int)nlen, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, j->ch_is_public ? 1 : 0);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)dbw_now_ms());
    sqlite3_bind_int64(st, 4, (sqlite3_int64)j->user_id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        r->type = OC_RES_CHANNEL_ERR; r->err_code = OC_ERR_INTERNAL; return r;
    }
    uint64_t cid = (uint64_t)sqlite3_last_insert_rowid(db);
    add_membership(db, cid, j->user_id);
    /* Its creator is its first manager (REQ-036b). */
    sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO channel_managers(channel_id, user_id, granted_by, granted_at_ms) "
                           "VALUES(?1, ?2, ?2, ?3);", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)cid);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)j->user_id);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)dbw_now_ms());
    sqlite3_step(st);
    sqlite3_finalize(st);
    /* A new public channel, said where the workspace is told to say it. */
    if (j->ch_is_public) {
        char *who = audit_name_of(db, j->user_id);
        notice_named(db, oc_config_get()->notice_channel_created, "@%s created #%.*s.",
                     who ? who : "someone", (int)nlen, j->ch_name);
        free(who);
    }

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
        "     WHERE x.channel_id=c.id AND (x.parent_id IS NULL OR x.also_channel=1)), "
        /* A badge counts what WOULD HAVE NOTIFIED, with the schedule and the
         * pause left out: those govern when to interrupt you, not whether a
         * message mattered, so a night of quiet hours still leaves a count in
         * the morning. Everything else is the evaluator's — mute silences
         * absolutely, a priority person pierces the level, and MENTIONS is
         * satisfied by a mention or a keyword hit. Thread replies are not here
         * -- they badge the Threads view -- unless also posted to the channel
         * (REQ-062), when they count here once, as a top-level message does.
         *
         * This is oc_notify_decide reduced for quiet=0, paused=0 and written as
         * SQL, because a badge is a COUNT over a channel's backlog rather than a
         * decision about one message. It is a restatement, and the parity sweep
         * is what stops it drifting: every state it can express is compared
         * against the evaluator's own answer. */
        "  (SELECT COUNT(*) FROM messages x "
        "     WHERE x.channel_id=c.id AND (x.parent_id IS NULL OR x.also_channel=1) AND x.author_id<>?1 "
        /* The workspace's notices are no one's unread (oc_notify_decide's `own`). */
        "       AND x.author_id NOT IN (SELECT su.id FROM users su WHERE su.is_system=1) "
        "       AND x.deleted_at_ms IS NULL "
        "       AND x.id > COALESCE((SELECT dc.message_id FROM delivery_cursors dc "
        "                             WHERE dc.user_id=?1 AND dc.channel_id=c.id),0) "
        "       AND COALESCE((SELECT np.muted FROM notification_prefs np "
        "                      WHERE np.user_id=?1 AND np.channel_id=c.id),0) = 0 "
        /* Nor is a person the reader muted (`muted_author`), whoever else they are. */
        "       AND NOT EXISTS(SELECT 1 FROM muted_users mu "
        "                       WHERE mu.user_id=?1 AND mu.muted_id=x.author_id) "
        /* Nor what a block withheld from them, which they never receive. */
        "       AND NOT EXISTS(SELECT 1 FROM withheld_messages wm WHERE wm.user_id=?1 AND wm.message_id=x.id) "
        "       AND ( EXISTS(SELECT 1 FROM priority_people pp "
        "                     WHERE pp.user_id=?1 AND pp.person_id=x.author_id) "
        "          OR COALESCE((SELECT np.level FROM notification_prefs np "
        "                        WHERE np.user_id=?1 AND np.channel_id=c.id), "
        "                      (SELECT u2.notify_default FROM users u2 WHERE u2.id=?1)) = 0 "
        "          OR ( COALESCE((SELECT np.level FROM notification_prefs np "
        "                          WHERE np.user_id=?1 AND np.channel_id=c.id), "
        "                        (SELECT u2.notify_default FROM users u2 WHERE u2.id=?1)) = 1 "
        "               AND EXISTS(SELECT 1 FROM mentions mn WHERE mn.message_id=x.id "
        "                           AND ((mn.user_id=?1 AND mn.kind IN (0,4,5)) "
        "                                OR mn.kind IN (1,2,3))) ) ) ), "
        /* A DM has no name; the client titles it by its peer, so send that too —
         * otherwise a cache-less client shows "direct message" until it opens one. */
        "  COALESCE((SELECT m2.user_id FROM channel_members m2 "
        "              WHERE m2.channel_id=c.id AND m2.user_id<>?1 LIMIT 1), ?1), "
        "  c.topic, c.archived_at_ms, c.created_at_ms, "
        /* The newest top-level message, for the list preview. Tombstones are
         * skipped: "(deleted)" is not a useful thing to show as the latest
         * activity, and the row below it usually is. An action's preview is its
         * action text, flagged, so the client reads "<author> <text>" (REQ-058). */
        "  (SELECT oc_excerpt(substr(CASE WHEN (SELECT e2e FROM channels WHERE id=x.channel_id)=1 THEN '' ELSE COALESCE(x.body,'') END,1+COALESCE((SELECT ac.text_start FROM actions ac WHERE ac.message_id=x.id),0)),?2) FROM messages x "
        "     WHERE x.channel_id=c.id AND (x.parent_id IS NULL OR x.also_channel=1) AND x.deleted_at_ms IS NULL "
        "     ORDER BY x.id DESC LIMIT 1), "
        "  COALESCE((SELECT x.author_id FROM messages x "
        "     WHERE x.channel_id=c.id AND (x.parent_id IS NULL OR x.also_channel=1) AND x.deleted_at_ms IS NULL "
        "     ORDER BY x.id DESC LIMIT 1),0), "
        "  COALESCE((SELECT EXISTS(SELECT 1 FROM actions ac WHERE ac.message_id=x.id) FROM messages x "
        "     WHERE x.channel_id=c.id AND (x.parent_id IS NULL OR x.also_channel=1) AND x.deleted_at_ms IS NULL "
        "     ORDER BY x.id DESC LIMIT 1),0), "
        "  c.e2e "
        /* A call's chat is the call's, reached from the call, and in no list
         * of conversations (CALLS.md §1). */
        "FROM channels c WHERE c.call_id IS NULL AND ( "
        "  (c.kind='channel' AND (c.is_public=1 OR EXISTS(SELECT 1 FROM channel_members m WHERE m.channel_id=c.id AND m.user_id=?1))) "
        "  OR (c.kind='dm' AND EXISTS(SELECT 1 FROM channel_members m WHERE m.channel_id=c.id AND m.user_id=?1)) "
        /* An archived channel is hidden from the default list unless you are a
         * member of it (REQ-035) — hidden, not deleted: a member keeps the way
         * back in, and its history stays searchable for everyone who could read
         * it before. */
        "  AND (c.archived_at_ms IS NULL "
        "       OR EXISTS(SELECT 1 FROM channel_members m WHERE m.channel_id=c.id AND m.user_id=?1)) ) "
        "ORDER BY c.id;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->user_id);
    /* ?2 is the preview length. An unbound parameter is NULL, and
     * substr(x, 1, NULL) is NULL — so forgetting this bind does not fail, it
     * silently returns an empty preview for every channel. */
    sqlite3_bind_int(st, 2, (int)OC_MAX_PREVIEW);

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
        const unsigned char *tp = sqlite3_column_text(st, 8);
        arr[n].topic           = (tp && tp[0]) ? strdup((const char *)tp) : NULL;
        arr[n].archived        = (uint8_t)(sqlite3_column_type(st, 9) != SQLITE_NULL);
        arr[n].created_at      = (uint64_t)sqlite3_column_int64(st, 10);
        const unsigned char *pv = sqlite3_column_text(st, 11);
        arr[n].preview         = (pv && pv[0]) ? strdup((const char *)pv) : NULL;
        arr[n].preview_author  = (uint64_t)sqlite3_column_int64(st, 12);
        arr[n].preview_action  = (uint8_t)(sqlite3_column_int(st, 13) != 0);
        arr[n].e2e             = (uint8_t)(sqlite3_column_int(st, 14) != 0);
        /* An encrypted DM's latest body is ciphertext: no preview of it. */
        if (arr[n].e2e) { free(arr[n].preview); arr[n].preview = NULL; arr[n].preview_action = 0; }
        arr[n].n_peers         = 0;
        n++;
    }
    sqlite3_finalize(st);

    /* Group DMs (REQ-056): one pass over the participants of the DMs just listed.
     * A correlated subquery per row would have to return N values, which SQL cannot
     * do in one column — so this is a second, small query rather than a clever one. */
    for (size_t i = 0; i < n; i++) {
        if (arr[i].kind != OC_CHANNEL_KIND_DM) continue;
        sqlite3_stmt *ps = NULL;
        sqlite3_prepare_v2(db,
            "SELECT user_id FROM channel_members WHERE channel_id=? "
            "ORDER BY joined_at_ms, user_id LIMIT 9;", -1, &ps, NULL);
        sqlite3_bind_int64(ps, 1, (sqlite3_int64)arr[i].channel_id);
        uint64_t tmp[9]; int tn = 0;
        while (sqlite3_step(ps) == SQLITE_ROW && tn < 9)
            tmp[tn++] = (uint64_t)sqlite3_column_int64(ps, 0);
        sqlite3_finalize(ps);
        if (tn > 2) {
            for (int k = 0; k < tn; k++) arr[i].peers[k] = tmp[k];
            arr[i].n_peers = (uint16_t)tn;
        }
    }

    /* Each channel's groups (REQ-309), so a client knows at sign-in which of
     * its channels it is in through a group. */
    for (size_t i = 0; i < n; i++)
        if (arr[i].kind == OC_CHANNEL_KIND)
            arr[i].n_groups = channel_group_ids(db, arr[i].channel_id, arr[i].groups, OC_MAX_CHANNEL_GROUPS);

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
    /* A visitor reaches only its own conversation (SCHEMA.md §3bj). */
    if (user_is_visitor(db, j->user_id)) {
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
    /* In only through a group, a leave would change nothing: the group keeps
     * them in. Refused, so the client can say why and offer a mute (REQ-309).
     * Added directly as well, the leave clears the direct half, and the group
     * keeps them in. */
    if (member_only_via_group(db, j->channel_id, j->user_id)) {
        r->type = OC_RES_CHANNEL_ERR; r->err_code = OC_ERR_MEMBER_VIA_GROUP; return r;
    }
    membership_sync(db, j->channel_id, j->user_id, 0);

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
    if (!user_role(db, j->target_user_id, &trole) || user_disabled(db, j->target_user_id) ||
        user_is_visitor(db, j->target_user_id) || user_is_visitor(db, j->user_id)) {
        /* unknown or removed target, or a visitor on either side (SCHEMA.md
         * §3bj): no disclosure */
        r->type = OC_RES_CHANNEL_ERR; r->err_code = OC_ERR_FORBIDDEN; return r;
    }
    /* An agent (docs/details/MCP.md §6) is put in a channel by an owner or an
     * admin alone, who must be in it -- adding one shows the channel's history
     * to whoever holds its token -- and is never put anywhere by itself. A
     * bot, until apps install it, is put nowhere. */
    int agent = user_kind_is(db, j->target_user_id, "agent");
    if ((agent && !actor_manages(db, j->user_id)) || user_kind_is(db, j->target_user_id, "bot") ||
        !user_kind_is(db, j->user_id, "human")) {
        r->type = OC_RES_CHANNEL_ERR; r->err_code = OC_ERR_FORBIDDEN; return r;
    }
    int was_in = is_member(db, j->channel_id, j->target_user_id);
    add_membership(db, j->channel_id, j->target_user_id);
    if (agent && !was_in)
        audit_actor_in(db, OC_AUDIT_ADMIN, "agent.channel.add", j->user_id, j->channel_id, j->target_user_id, NULL, 1, NULL);
    /* Told, always: someone put them somewhere. An agent has no notices. */
    if (!was_in && !agent && j->target_user_id != j->user_id) {
        char *who = audit_name_of(db, j->user_id), *ch = NULL;
        sqlite3_stmt *nq = NULL;
        sqlite3_prepare_v2(db, "SELECT name FROM channels WHERE id=?1;", -1, &nq, NULL);
        sqlite3_bind_int64(nq, 1, (sqlite3_int64)j->channel_id);
        if (sqlite3_step(nq) == SQLITE_ROW && sqlite3_column_text(nq, 0))
            ch = strdup((const char *)sqlite3_column_text(nq, 0));
        sqlite3_finalize(nq);
        notice_person(db, j->target_user_id, "@%s added you to #%s.", who ? who : "someone", ch ? ch : "a channel");
        free(who); free(ch);
    }

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
    /* In only through a group, they cannot be removed alone: the group would
     * keep them in (REQ-309). Take them out of the group, or the group off.
     * Added directly as well, the remove clears the direct half. */
    if (member_only_via_group(db, j->channel_id, j->target_user_id)) {
        r->type = OC_RES_CHANNEL_ERR; r->err_code = OC_ERR_MEMBER_VIA_GROUP; return r;
    }
    /* An agent is taken out by an owner or an admin alone (docs/details/MCP.md
     * §6), and that is an administrative act, audited as one. */
    if (user_kind_is(db, j->target_user_id, "agent")) {
        if (!actor_manages(db, j->user_id)) { r->type = OC_RES_CHANNEL_ERR; r->err_code = OC_ERR_FORBIDDEN; return r; }
        if (membership_sync(db, j->channel_id, j->target_user_id, 0) < 0) {
            audit_actor_in(db, OC_AUDIT_ADMIN, "agent.channel.remove", j->user_id, j->channel_id,
                           j->target_user_id, NULL, 1, NULL);
            note_change(db, r, j->channel_id, j->target_user_id, 0);
        }
        r->type = OC_RES_CHANNEL_INFO;
        load_channel_info(db, j->channel_id, j->user_id, r);
        fill_briefs(db, r);
        return r;
    }
    if (membership_sync(db, j->channel_id, j->target_user_id, 0) < 0) {
        /* REQ-251's moderation family names this explicitly: removing a member
         * from a channel, alongside a moderator deleting another user's message. */
        audit_actor_in(db, OC_AUDIT_MODERATION, "channel.member.remove", j->user_id, j->channel_id,
                       j->target_user_id, NULL, 1, NULL);
        note_change(db, r, j->channel_id, j->target_user_id, 0);   /* they are told */
    }

    r->type = OC_RES_CHANNEL_INFO;
    load_channel_info(db, j->channel_id, j->user_id, r);
    fill_briefs(db, r);
    return r;
}

/* Open (or get) the DM between the caller and target (REQ-050). A DM is a
 * kind='dm' channel with exactly its participants — two for a normal DM, or one
 * for a **self-DM** (target == self: a personal "notes to self" space, REQ-055).
 * Idempotent — an existing DM is returned rather than a duplicate created.
 * Replies CHANNEL_INFO (kind=DM), pushed to the peer (not for a self-DM).
 * Messaging/backfill/search then work through the normal membership path. */
static oc_dbres *process_open_e2e_dm(sqlite3 *db, const oc_job *j);
/* --- Blocks and DM privacy ---------------------------------------------------
 * A block is its blocker's alone. The blocked person cannot start a direct
 * conversation with the blocker, and what they still send into one the two share
 * is kept for its other members but withheld from the blocker: a row in
 * withheld_messages per message, so lifting the block brings none of it back.
 * DM privacy 'nobody' refuses every new direct conversation naming that person.
 * Both refusals are the same OC_ERR_DM_REFUSED, which says neither. Channels are
 * untouched: there, muting a person is the tool. */
static int is_blocked(sqlite3 *db, uint64_t blocker, uint64_t who) {
    sqlite3_stmt *st = NULL;
    int found = 0;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM blocks WHERE user_id=?1 AND blocked_id=?2;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)blocker);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)who);
        found = sqlite3_step(st) == SQLITE_ROW;
    }
    sqlite3_finalize(st);
    return found;
}

static int dm_closed(sqlite3 *db, uint64_t uid) {
    sqlite3_stmt *st = NULL;
    int closed = 0;
    if (sqlite3_prepare_v2(db, "SELECT dm_privacy='nobody' FROM users WHERE id=?1;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)uid);
        closed = sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    return closed;
}

/* A message `author` just wrote in `channel_id`: when that is a direct
 * conversation, every member who blocks the author is taken out of the fan-out
 * in r->members, and the message is recorded as withheld from them. */
static void withhold_from_blockers(sqlite3 *db, uint64_t channel_id, uint64_t author, uint64_t mid, oc_dbres *r) {
    sqlite3_stmt *st = NULL;
    int dm = 0;
    if (sqlite3_prepare_v2(db, "SELECT kind='dm' FROM channels WHERE id=?1;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)channel_id);
        dm = sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    if (!dm || !r->members) return;
    size_t keep = 0;
    for (size_t i = 0; i < r->n_members; i++) {
        uint64_t m = r->members[i];
        if (m != author && is_blocked(db, m, author)) {
            st = NULL;
            if (sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO withheld_messages(user_id, message_id) VALUES(?1, ?2);",
                                   -1, &st, NULL) == SQLITE_OK) {
                sqlite3_bind_int64(st, 1, (sqlite3_int64)m);
                sqlite3_bind_int64(st, 2, (sqlite3_int64)mid);
                sqlite3_step(st);
            }
            sqlite3_finalize(st);
            continue;
        }
        r->members[keep++] = m;
    }
    r->n_members = keep;
}

/* Whom `uid` blocks and their DM privacy, as BLOCKS carries them. */
static void fill_blocks(sqlite3 *db, uint64_t uid, oc_dbres *r) {
    sqlite3_stmt *st = NULL;
    r->bl_ids = calloc(OC_MAX_BLOCKS, sizeof *r->bl_ids);
    if (!r->bl_ids) return;
    sqlite3_prepare_v2(db, "SELECT blocked_id FROM blocks WHERE user_id=?1 ORDER BY blocked_id;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)uid);
    while (r->bl_n < OC_MAX_BLOCKS && sqlite3_step(st) == SQLITE_ROW)
        r->bl_ids[r->bl_n++] = (uint64_t)sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    r->bl_privacy = dm_closed(db, uid) ? OC_DM_PRIVACY_NOBODY : OC_DM_PRIVACY_ANYONE;
    r->bl_have = 1;
}

/* Block or unblock one person (SET_BLOCK), audited in the moderation family.
 * Yourself, nobody, the workspace's own sender or no one at all is ignored. */
static oc_dbres *process_set_block(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->type = OC_RES_BLOCKS;
    r->user_id = j->user_id;
    uint64_t who = j->target_user_id;
    if (who && who != j->user_id) {
        sqlite3_stmt *st = NULL;
        if (j->block_on)
            sqlite3_prepare_v2(db,
                "INSERT OR IGNORE INTO blocks(user_id, blocked_id, created_at_ms) "
                "SELECT ?1, ?2, ?3 WHERE EXISTS(SELECT 1 FROM users WHERE id=?2 AND is_system=0);", -1, &st, NULL);
        else
            sqlite3_prepare_v2(db, "DELETE FROM blocks WHERE user_id=?1 AND blocked_id=?2;", -1, &st, NULL);
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->user_id);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)who);
        if (j->block_on) sqlite3_bind_int64(st, 3, (sqlite3_int64)dbw_now_ms());
        int changed = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db) == 1;
        sqlite3_finalize(st);
        if (changed)
            audit_actor(db, OC_AUDIT_MODERATION, j->block_on ? "block.place" : "block.lift",
                        j->user_id, who, NULL, 1, NULL);
    }
    fill_blocks(db, j->user_id, r);
    return r;
}

static oc_dbres *process_set_dm_privacy(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->type = OC_RES_BLOCKS;
    r->user_id = j->user_id;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "UPDATE users SET dm_privacy=?2 WHERE id=?1;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->user_id);
        sqlite3_bind_text(st, 2, j->dm_privacy == OC_DM_PRIVACY_NOBODY ? "nobody" : "anyone", -1, SQLITE_STATIC);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
    fill_blocks(db, j->user_id, r);
    return r;
}

/* Every block in the workspace, for an owner or admin (LIST_BLOCKS), newest
 * first. Authorized on the CURRENT role. */
static oc_dbres *process_list_blocks(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->type = OC_RES_BLOCK_LIST;
    uint8_t role = OC_ROLE_MEMBER;
    if (!user_role(db, j->user_id, &role) || !oc_role_can_manage_members(role)) {
        r->type = OC_RES_BLOCK_LIST_ERR;
        r->err_code = OC_ERR_FORBIDDEN;
        return r;
    }
    r->blk_entries = calloc(OC_BLOCK_LIST_MAX, sizeof *r->blk_entries);
    if (!r->blk_entries) { r->type = OC_RES_BLOCK_LIST_ERR; r->err_code = OC_ERR_INTERNAL; return r; }
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "SELECT user_id, blocked_id, created_at_ms FROM blocks ORDER BY created_at_ms DESC LIMIT ?1;",
                       -1, &st, NULL);
    sqlite3_bind_int(st, 1, (int)OC_BLOCK_LIST_MAX);
    while (r->n_blk < OC_BLOCK_LIST_MAX && sqlite3_step(st) == SQLITE_ROW) {
        oc_block_entry *e = &r->blk_entries[r->n_blk++];
        e->blocker_id = (uint64_t)sqlite3_column_int64(st, 0);
        e->blocked_id = (uint64_t)sqlite3_column_int64(st, 1);
        e->since_ms   = (uint64_t)sqlite3_column_int64(st, 2);
    }
    sqlite3_finalize(st);
    return r;
}

static oc_dbres *process_open_dm(sqlite3 *db, const oc_job *j) {
    if (j->e2e) return process_open_e2e_dm(db, j);
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
    /* A visitor is nobody's DM, and opens none (SCHEMA.md §3bj): answered as an
     * unknown target is, so nothing says a visitor is there. */
    if (user_not_person(db, self) || user_not_person(db, other)) {
        r->type = OC_RES_CHANNEL_ERR; r->err_code = OC_ERR_FORBIDDEN; return r;
    }
    /* Blocked by them: no conversation, new or old, is opened, and the answer
     * says no more than a closed door does. */
    if (!self_dm && is_blocked(db, other, self)) {
        r->type = OC_RES_CHANNEL_ERR; r->err_code = OC_ERR_DM_REFUSED; return r;
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

    /* A removed member's DM is still there and is found above; a new one with
     * them is refused, as an unknown user is. */
    if (cid == 0 && !self_dm && user_disabled(db, other)) {
        r->type = OC_RES_CHANNEL_ERR; r->err_code = OC_ERR_FORBIDDEN; return r;
    }
    /* Taking no new direct conversations: refused, the same way; one that
     * already exists stands. */
    if (cid == 0 && !self_dm && dm_closed(db, other)) {
        r->type = OC_RES_CHANNEL_ERR; r->err_code = OC_ERR_DM_REFUSED; return r;
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

/* An encrypted DM's channel (PRIVATE-DM.md): a DM like any other, marked
 * `e2e`, made new for each conversation -- its `dm_key` carries the time and a
 * counter, so it never meets the pair's ordinary DM or an earlier encrypted one
 * under the unique index. Answered as CHANNEL_INFO with `pdm_open` set, and
 * channel_id 0 when it could not be made; the net loop goes on with the
 * invitation, and pushes it to nobody until it is accepted. */
static oc_dbres *process_open_e2e_dm(sqlite3 *db, const oc_job *j) {
    static uint64_t seq;
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->type = OC_RES_CHANNEL_INFO;
    r->pdm_open = 1;
    r->ch_peer = j->target_user_id;
    uint64_t self = j->user_id, other = j->target_user_id;
    uint8_t trole = OC_ROLE_MEMBER;
    if (!other || other == self || !user_role(db, other, &trole) || user_disabled(db, other) ||
        user_not_person(db, self) || user_not_person(db, other)) return r;
    /* Always a new conversation: refused, as plainly as any other, to someone
     * the peer blocks or when the peer takes no new ones. */
    if (is_blocked(db, other, self) || dm_closed(db, other)) return r;
    char key[96];
    snprintf(key, sizeof key, "e:%llu,%llu:%llu.%llu",
             (unsigned long long)(self < other ? self : other), (unsigned long long)(self < other ? other : self),
             (unsigned long long)dbw_now_ms(), (unsigned long long)++seq);
    sqlite3_stmt *st = NULL;
    sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);
    sqlite3_prepare_v2(db,
        "INSERT INTO channels(kind,name,is_public,created_at_ms,dm_key,e2e) VALUES('dm',NULL,0,?1,?2,1);",
        -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)dbw_now_ms());
    sqlite3_bind_text(st, 2, key, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) { sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL); return r; }
    uint64_t cid = (uint64_t)sqlite3_last_insert_rowid(db);
    add_membership(db, cid, self);
    add_membership(db, cid, other);
    sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
    load_channel_info(db, cid, self, r);
    r->ch_peer = other;
    return r;
}

/* An encrypted DM ended: closed, as an archived channel is, so nothing more is
 * posted to it; the maintenance pass removes it a day later. */
static oc_dbres *process_e2e_close(sqlite3 *db, const oc_job *j) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "UPDATE channels SET archived_at_ms=?1 WHERE id=?2 AND e2e=1 AND archived_at_ms IS NULL;",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)dbw_now_ms());
        sqlite3_bind_int64(st, 2, (sqlite3_int64)j->channel_id);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
    return NULL;
}

/* 1 when the channel is an encrypted DM: its bodies are ciphertext, and what
 * reads a body (previews, mentions, actions, transcripts, forwards) leaves it be. */
static int channel_is_e2e(sqlite3 *db, uint64_t channel_id) {
    sqlite3_stmt *st = NULL;
    int e2e = 0;
    if (sqlite3_prepare_v2(db, "SELECT e2e FROM channels WHERE id=?1;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)channel_id);
        if (sqlite3_step(st) == SQLITE_ROW) e2e = sqlite3_column_int(st, 0) != 0;
    }
    sqlite3_finalize(st);
    return e2e;
}

/* 1 when the channel is an encrypted DM that has ended: closed for good, so
 * what was said in it is not edited, deleted or reacted to either. */
static int e2e_closed(sqlite3 *db, uint64_t channel_id) {
    sqlite3_stmt *st = NULL;
    int closed = 0;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM channels WHERE id=?1 AND e2e=1 AND archived_at_ms IS NOT NULL;",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)channel_id);
        closed = sqlite3_step(st) == SQLITE_ROW;
    }
    sqlite3_finalize(st);
    return closed;
}

/* Remove a channel and every row about it: those naming it (channel_id), and
 * those naming one of its messages (message_id), whatever table they are in --
 * found from the schema, so a table added later is not missed. */
static void purge_channel(sqlite3 *db, uint64_t cid) {
    sqlite3_stmt *t = NULL;
    char names[64][64]; int is_msg[64]; int n = 0;
    if (sqlite3_prepare_v2(db,
            "SELECT m.name, p.name FROM sqlite_master m JOIN pragma_table_info(m.name) p "
            " WHERE m.type='table' AND m.name NOT IN ('messages','channels') AND m.name NOT LIKE 'messages_fts%' "
            "   AND p.name IN ('channel_id','message_id');", -1, &t, NULL) == SQLITE_OK) {
        while (sqlite3_step(t) == SQLITE_ROW && n < 64) {
            snprintf(names[n], sizeof names[n], "%s", (const char *)sqlite3_column_text(t, 0));
            is_msg[n] = strcmp((const char *)sqlite3_column_text(t, 1), "message_id") == 0;
            n++;
        }
    }
    sqlite3_finalize(t);
    for (int i = 0; i < n; i++) {
        char sql[256];
        if (is_msg[i])
            snprintf(sql, sizeof sql, "DELETE FROM \"%.63s\" WHERE message_id IN (SELECT id FROM messages WHERE channel_id=?1);", names[i]);
        else
            snprintf(sql, sizeof sql, "DELETE FROM \"%.63s\" WHERE channel_id=?1;", names[i]);
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)cid);
            sqlite3_step(st);
        }
        sqlite3_finalize(st);
    }
    const char *last[] = { "DELETE FROM messages WHERE channel_id=?1;", "DELETE FROM channels WHERE id=?1;" };
    for (int i = 0; i < 2; i++) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(db, last[i], -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)cid);
            sqlite3_step(st);
        }
        sqlite3_finalize(st);
    }
}

/* Encrypted DMs a day after they closed go, every row of them. */
#define E2E_KEEP_MS (24ull * 60 * 60 * 1000)
static void purge_closed_e2e(sqlite3 *db, uint64_t now) {
    uint64_t ids[256]; int n = 0;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT id FROM channels WHERE e2e=1 AND archived_at_ms IS NOT NULL AND archived_at_ms < ?1 LIMIT 256;",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)(now > E2E_KEEP_MS ? now - E2E_KEEP_MS : 0));
        while (sqlite3_step(st) == SQLITE_ROW && n < 256) ids[n++] = (uint64_t)sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    if (!n) return;
    sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);
    for (int i = 0; i < n; i++) purge_channel(db, ids[i]);
    sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
}

/* Custom emoji (REQ-072).
 *
 * The catalogue is small and is answered whole: a partial one means a message whose
 * emoji renders on one client and not another, which is worse than a slightly larger
 * frame. The name is the identity (`:shipit:` must mean one image workspace-wide),
 * lowercased and validated here rather than trusted — a name with a colon in it would
 * be unparseable in a message body, and one with an uppercase letter would make
 * `:Shipit:` and `:shipit:` two different emoji.
 */
static int emoji_name_ok(const char *n, char *out, size_t cap) {
    if (!n || !n[0]) return 0;
    size_t at = 0;
    for (const char *p = n; *p; p++) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        int ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '+';
        if (!ok) return 0;
        if (at + 1 >= cap) return 0;
        out[at++] = c;
    }
    out[at] = '\0';
    return at > 0;
}

/* Is `name` an old name some emoji keeps? */
static int emoji_alias_taken(sqlite3 *db, const char *name) {
    sqlite3_stmt *st = NULL;
    int yes = 0;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM custom_emoji_aliases WHERE alias=?1;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
        yes = sqlite3_step(st) == SQLITE_ROW;
    }
    sqlite3_finalize(st);
    return yes;
}

static int64_t emoji_count(sqlite3 *db, const char *sql, const char *arg) {
    sqlite3_stmt *st = NULL;
    int64_t n = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
        if (arg) sqlite3_bind_text(st, 1, arg, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    return n;
}

static void build_emoji_list(sqlite3 *db, oc_dbres *r) {
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "SELECT name, attachment_id, created_by, created_at_ms FROM custom_emoji ORDER BY name LIMIT ?;",
        -1, &st, NULL);
    sqlite3_bind_int(st, 1, (int)OC_MAX_CUSTOM_EMOJI);
    size_t cap = 16, n = 0;
    oc_emoji_row *arr = malloc(cap * sizeof *arr);
    while (arr && sqlite3_step(st) == SQLITE_ROW) {
        if (n == cap) { cap *= 2; oc_emoji_row *g = realloc(arr, cap * sizeof *arr); if (!g) break; arr = g; }
        memset(&arr[n], 0, sizeof arr[n]);
        arr[n].name          = strdup((const char *)sqlite3_column_text(st, 0));
        arr[n].attachment_id = (uint64_t)sqlite3_column_int64(st, 1);
        arr[n].created_by    = (uint64_t)sqlite3_column_int64(st, 2);
        arr[n].created_at    = (uint64_t)sqlite3_column_int64(st, 3);
        n++;
    }
    sqlite3_finalize(st);
    /* Each one's old names, oldest first (REQ-072). */
    for (size_t i = 0; i < n; i++) {
        if (sqlite3_prepare_v2(db, "SELECT alias FROM custom_emoji_aliases WHERE name=?1 ORDER BY rowid LIMIT ?2;",
                               -1, &st, NULL) != SQLITE_OK) break;
        sqlite3_bind_text(st, 1, arr[i].name ? arr[i].name : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 2, (int)OC_MAX_EMOJI_ALIASES);
        while (arr[i].n_aliases < OC_MAX_EMOJI_ALIASES && sqlite3_step(st) == SQLITE_ROW)
            arr[i].aliases[arr[i].n_aliases++] = strdup((const char *)sqlite3_column_text(st, 0));
        sqlite3_finalize(st);
    }
    r->elist = arr;
    r->n_elist = n;
    r->type = OC_RES_EMOJI_LIST;
}

static oc_dbres *process_list_emoji(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    build_emoji_list(db, r);
    return r;
}

static oc_dbres *process_add_emoji(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->user_id = j->user_id;

    char name[OC_EMOJI_NAME_MAX];
    if (!emoji_name_ok(j->ch_name, name, sizeof name)) {
        r->type = OC_RES_LIST_ERR; r->err_code = OC_ERR_MALFORMED_FRAME; return r;
    }
    /* Who may add one is the workspace's configuration
     * (OPENCHIME_CUSTOM_EMOJI_ADD): everyone, or owners and admins. */
    if (oc_config_get()->custom_emoji_add == 1) {
        uint8_t role = OC_ROLE_MEMBER;
        if (!user_role(db, j->user_id, &role) || role < OC_ROLE_ADMIN) {
            r->type = OC_RES_LIST_ERR; r->err_code = OC_ERR_FORBIDDEN; return r;
        }
    }
    /* The image must exist, be finalized, be an image, and be one THIS user
     * uploaded — the same rule as an avatar and for the same reason: an
     * emoji is readable workspace-wide, so the id has to be one the setter was
     * entitled to. */
    sqlite3_stmt *ck = NULL;
    int ok = 0;
    sqlite3_prepare_v2(db,
        "SELECT 1 FROM attachments WHERE id=?1 AND uploader_id=?2 "
        "  AND size > 0 AND mime LIKE 'image/%';", -1, &ck, NULL);
    sqlite3_bind_int64(ck, 1, (sqlite3_int64)j->message_id);
    sqlite3_bind_int64(ck, 2, (sqlite3_int64)j->user_id);
    ok = (sqlite3_step(ck) == SQLITE_ROW);
    sqlite3_finalize(ck);
    if (!ok) { r->type = OC_RES_LIST_ERR; r->err_code = OC_ERR_UNKNOWN_ATTACHMENT; return r; }

    sqlite3_stmt *st = NULL;
    /* An old name an emoji keeps (REQ-072) is taken: it already shows that
     * emoji's image in the messages that used it. */
    if (emoji_alias_taken(db, name)) {
        r->type = OC_RES_LIST_ERR; r->err_code = OC_ERR_CHANNEL_EXISTS; return r;
    }
    /* INSERT, not upsert: silently replacing an existing emoji changes what every
     * message already containing that shortcode means. A rename keeps the old
     * name as an alias (RENAME_EMOJI). */
    sqlite3_prepare_v2(db,
        "INSERT INTO custom_emoji(name, attachment_id, created_by, created_at_ms) "
        "VALUES(?1, ?2, ?3, ?4);", -1, &st, NULL);
    sqlite3_bind_text (st, 1, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)j->message_id);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)j->user_id);
    sqlite3_bind_int64(st, 4, (sqlite3_int64)dbw_now_ms());
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        r->type = OC_RES_LIST_ERR; r->err_code = OC_ERR_CHANNEL_EXISTS; return r;
    }
    build_emoji_list(db, r);
    r->ch_fanout = 1;          /* everyone's picker should learn about it */
    return r;
}

static oc_dbres *process_delete_emoji(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->user_id = j->user_id;
    char name[OC_EMOJI_NAME_MAX];
    if (!emoji_name_ok(j->ch_name, name, sizeof name)) {
        r->type = OC_RES_LIST_ERR; r->err_code = OC_ERR_MALFORMED_FRAME; return r;
    }
    /* The creator or an admin. A shortcode is workspace-wide, so letting anyone
     * delete one lets anyone break every message that used it. */
    uint8_t role = OC_ROLE_MEMBER;
    user_role(db, j->user_id, &role);
    sqlite3_stmt *st = NULL;
    sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);
    sqlite3_prepare_v2(db,
        "DELETE FROM custom_emoji_aliases WHERE name IN "
        "  (SELECT name FROM custom_emoji WHERE name=?1 AND (?2 = 1 OR created_by=?3));",
        -1, &st, NULL);
    sqlite3_bind_text (st, 1, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int  (st, 2, role >= OC_ROLE_ADMIN ? 1 : 0);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)j->user_id);
    sqlite3_step(st);
    sqlite3_finalize(st);
    sqlite3_prepare_v2(db,
        "DELETE FROM custom_emoji WHERE name=?1 AND (?2 = 1 OR created_by=?3);",
        -1, &st, NULL);
    sqlite3_bind_text (st, 1, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int  (st, 2, role >= OC_ROLE_ADMIN ? 1 : 0);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)j->user_id);
    sqlite3_step(st);
    sqlite3_finalize(st);
    if (sqlite3_changes(db) == 0) {
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        r->type = OC_RES_LIST_ERR; r->err_code = OC_ERR_FORBIDDEN; return r;
    }
    sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
    build_emoji_list(db, r);
    r->ch_fanout = 1;
    return r;
}

/* Rename a custom emoji (REQ-072): whoever may delete it. The old name becomes
 * an alias of the same image, and the aliases it had follow it, so nothing that
 * used any of them breaks. A name an emoji or an alias holds is taken. */
static oc_dbres *process_rename_emoji(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->user_id = j->user_id;
    char from[OC_EMOJI_NAME_MAX], to[OC_EMOJI_NAME_MAX];
    if (!emoji_name_ok(j->ch_name, from, sizeof from) || !emoji_name_ok(j->pf_name, to, sizeof to)) {
        r->type = OC_RES_LIST_ERR; r->err_code = OC_ERR_MALFORMED_FRAME; return r;
    }
    uint8_t role = OC_ROLE_MEMBER;
    user_role(db, j->user_id, &role);
    sqlite3_stmt *st = NULL;
    int may = 0;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM custom_emoji WHERE name=?1 AND (?2 = 1 OR created_by=?3);",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text (st, 1, from, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int  (st, 2, role >= OC_ROLE_ADMIN ? 1 : 0);
        sqlite3_bind_int64(st, 3, (sqlite3_int64)j->user_id);
        may = sqlite3_step(st) == SQLITE_ROW;
    }
    sqlite3_finalize(st);
    if (!may) { r->type = OC_RES_LIST_ERR; r->err_code = OC_ERR_FORBIDDEN; return r; }
    if (strcmp(from, to) == 0) { build_emoji_list(db, r); return r; }   /* nothing to do */
    if (emoji_count(db, "SELECT COUNT(*) FROM custom_emoji WHERE name=?1;", to) || emoji_alias_taken(db, to)) {
        r->type = OC_RES_LIST_ERR; r->err_code = OC_ERR_CHANNEL_EXISTS; return r;
    }
    if (emoji_count(db, "SELECT COUNT(*) FROM custom_emoji_aliases WHERE name=?1;", from) + 1 > OC_MAX_EMOJI_ALIASES ||
        emoji_count(db, "SELECT COUNT(*) FROM custom_emoji_aliases;", NULL) + 1 > OC_MAX_EMOJI_ALIASES_TOTAL) {
        r->type = OC_RES_LIST_ERR; r->err_code = OC_ERR_EMOJI_ALIAS_LIMIT; return r;
    }
    sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);
    int ok = 1;
    static const char *const sql[] = {
        "UPDATE custom_emoji SET name=?2 WHERE name=?1;",
        /* The cascade already moved them; said again so it holds without it. */
        "UPDATE custom_emoji_aliases SET name=?2 WHERE name=?1;",
        "INSERT INTO custom_emoji_aliases(alias, name) VALUES(?1, ?2);",
    };
    for (size_t i = 0; ok && i < sizeof sql / sizeof sql[0]; i++) {
        st = NULL;
        if (sqlite3_prepare_v2(db, sql[i], -1, &st, NULL) != SQLITE_OK) { ok = 0; break; }
        sqlite3_bind_text(st, 1, from, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, to, -1, SQLITE_TRANSIENT);
        ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
    }
    sqlite3_exec(db, ok ? "COMMIT;" : "ROLLBACK;", NULL, NULL, NULL);
    if (!ok) { r->type = OC_RES_LIST_ERR; r->err_code = OC_ERR_INTERNAL; return r; }
    build_emoji_list(db, r);
    r->ch_fanout = 1;          /* every picker and every message follows the name */
    return r;
}

/* A GROUP DM (REQ-056).
 *
 * Deliberately NOT a new `channels.kind`. A group DM is a DM with more than two
 * participants, and the identity of a DM is already its participant set — the
 * `dm_key` under a unique index (migration 0019). So the same key with three or
 * more ids in it IS the group, which means: no migration, no second code path for
 * membership or read access, and no way for a 1:1 and a group to disagree about
 * what a DM is. The client tells them apart by the participant count, which it can
 * see from the roster it already fetches.
 *
 * Reopening the same set returns the same conversation, exactly like OPEN_DM — a
 * group DM you cannot get back to is a lost conversation. */
static oc_dbres *process_open_group_dm(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;

    /* The participant set: the caller plus the named others, deduplicated. Sorted,
     * because the key must not depend on the order the client listed them in. */
    uint64_t ids[OC_MAX_GROUP_DM + 1];
    int n = 0;
    /* A visitor opens no DM (SCHEMA.md §3bj). */
    if (user_is_visitor(db, j->user_id)) {
        r->type = OC_RES_CHANNEL_ERR; r->err_code = OC_ERR_FORBIDDEN; return r;
    }
    ids[n++] = j->user_id;
    for (uint16_t i = 0; i < j->n_group_uids && n <= (int)OC_MAX_GROUP_DM; i++) {
        uint64_t u = j->group_uids[i];
        if (!u) continue;
        int dup = 0;
        for (int k = 0; k < n; k++) if (ids[k] == u) dup = 1;
        if (dup) continue;
        uint8_t role = OC_ROLE_MEMBER;
        /* An unknown or disabled user, or a visitor, is refused without saying
         * which — the same no-disclosure rule OPEN_DM follows. */
        if (!user_role(db, u, &role) || user_disabled(db, u) || user_not_person(db, u)) {
            r->type = OC_RES_CHANNEL_ERR; r->err_code = OC_ERR_FORBIDDEN; return r;
        }
        /* Anyone named who blocks the caller: refused, as a 1:1 is. */
        if (is_blocked(db, u, j->user_id)) {
            r->type = OC_RES_CHANNEL_ERR; r->err_code = OC_ERR_DM_REFUSED; return r;
        }
        ids[n++] = u;
    }
    /* Three is the floor: two participants is a 1:1 DM and must reuse that path, or
     * the same pair would end up with two conversations. */
    if (n < 3) { r->type = OC_RES_CHANNEL_ERR; r->err_code = OC_ERR_MALFORMED_FRAME; return r; }
    for (int a = 0; a < n; a++)
        for (int b = a + 1; b < n; b++)
            if (ids[b] < ids[a]) { uint64_t t = ids[a]; ids[a] = ids[b]; ids[b] = t; }

    char key[24 * (OC_MAX_GROUP_DM + 1)];
    size_t at = 0;
    for (int i = 0; i < n; i++)
        at += (size_t)snprintf(key + at, sizeof key - at, "%s%llu", i ? "," : "",
                               (unsigned long long)ids[i]);

    uint64_t cid = 0;
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "SELECT id FROM channels WHERE kind='dm' AND dm_key=?1;", -1, &st, NULL);
    sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) cid = (uint64_t)sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);

    /* A new one naming anyone who takes no new direct conversations is refused. */
    if (cid == 0)
        for (int i = 0; i < n; i++)
            if (ids[i] != j->user_id && dm_closed(db, ids[i])) {
                r->type = OC_RES_CHANNEL_ERR; r->err_code = OC_ERR_DM_REFUSED; return r;
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
        for (int i = 0; i < n; i++) add_membership(db, cid, ids[i]);
        sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
    } else {
        /* Re-assert membership, as OPEN_DM does: anything that removed a user must
         * not leave a group conversation unreachable for them. */
        for (int i = 0; i < n; i++) add_membership(db, cid, ids[i]);
    }

    r->type = OC_RES_CHANNEL_INFO;
    load_channel_info(db, cid, j->user_id, r);
    /* peer 0 marks it as a group rather than a 1:1 — there is no single other
     * person to name, and the client titles it from the roster. */
    r->ch_peer = 0;
    /* Fan the new conversation out to the other participants through the members
     * list the CHANNEL_INFO broadcast already uses, so it appears in their sidebar
     * now rather than at their next refresh. push_user_id carries one user and a
     * group has several. */
    r->ch_fanout = 1;
    r->members = malloc((size_t)n * sizeof *r->members);
    if (r->members) {
        r->n_members = 0;
        for (int i = 0; i < n; i++) r->members[r->n_members++] = ids[i];
    }
    return r;
}

/* A channel's member roster (REQ-031).
 *
 * The membership has always been stored; what was missing was any way for a
 * client to READ it, so a frontend showed the tenant roster beside a channel
 * name and called it "members" — wrong the moment a workspace holds more people
 * than one channel does. */
/* A channel's description, for whoever may READ the channel (REQ-034): any
 * tenant user for a public one -- whose topic the channel directory already
 * shows them -- and members only for a private one.
 *
 * "No such channel" and "a private channel you are not in" get the SAME answer,
 * because channel_read_access cannot tell them apart and must not: a different
 * error for the second would say the channel exists. */
static oc_dbres *process_get_channel_description(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->channel_id = j->channel_id;
    if (!channel_read_access(db, j->channel_id, j->user_id)) {
        r->type = OC_RES_LIST_ERR; r->err_code = OC_ERR_UNKNOWN_CHANNEL; return r;
    }
    r->type = OC_RES_CHANNEL_DESCRIPTION;

    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "SELECT description FROM channels WHERE id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->channel_id);
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) != SQLITE_NULL) {
        const unsigned char *d = sqlite3_column_text(st, 0);
        size_t n = (size_t)sqlite3_column_bytes(st, 0);
        if (n > OC_MAX_DESCRIPTION) n = OC_MAX_DESCRIPTION;   /* never more than the cap */
        if (d && n) {
            r->body = malloc(n);
            if (r->body) { memcpy(r->body, d, n); r->body_len = n; }
        }
    }
    sqlite3_finalize(st);
    return r;
}

static oc_dbres *process_list_members(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->channel_id = j->channel_id;

    /* You may only enumerate a channel you can read -- a public one, or one
     * you are in -- or this becomes a way to discover who is in a private
     * channel you were never invited to. */
    if (!channel_read_access(db, j->channel_id, j->user_id)) {
        r->type = OC_RES_LIST_ERR; r->err_code = OC_ERR_NOT_A_MEMBER; return r;
    }
    r->type = OC_RES_MEMBER_LIST;

    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "SELECT cm.user_id, cm.joined_at_ms, u.role, "
        "       cm.direct = 0 AND EXISTS(SELECT 1 FROM channel_groups cg "
        "           JOIN user_group_members gm ON gm.group_id = cg.group_id "
        "           WHERE cg.channel_id = cm.channel_id AND gm.user_id = cm.user_id) "
        "  FROM channel_members cm "
        "  JOIN users u ON u.id = cm.user_id "
        " WHERE cm.channel_id=? AND (u.disabled=0 OR EXISTS(SELECT 1 FROM channels c "
        "       WHERE c.id = cm.channel_id AND c.kind='dm')) "
        " ORDER BY cm.joined_at_ms LIMIT ?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->channel_id);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)OC_MAX_MEMBER_LIST);

    oc_chanmem_row *arr = calloc(OC_MAX_MEMBER_LIST, sizeof *arr);
    size_t n = 0;
    while (arr && n < OC_MAX_MEMBER_LIST && sqlite3_step(st) == SQLITE_ROW) {
        arr[n].user_id   = (uint64_t)sqlite3_column_int64(st, 0);
        arr[n].joined_at = (uint64_t)sqlite3_column_int64(st, 1);
        arr[n].role      = role_to_u8((const char *)sqlite3_column_text(st, 2));
        arr[n].only_via_group = (uint8_t)(sqlite3_column_int(st, 3) != 0);
        n++;
    }
    sqlite3_finalize(st);
    r->cmlist = arr;
    r->n_cmlist = n;
    return r;
}

/* Files shared in a channel — or, with channel_id 0, across every channel the
 * caller can read (REQ-143, ARCH-91).
 *
 * Pending uploads (message_id NULL) are excluded: an upload that never reached
 * a message was never shared with anyone. Reclaimed rows are KEPT and flagged,
 * because "this was here and the bytes are gone" (REQ-215/217) is information
 * where a silently missing row is not. */
/* Which channels hold files, with counts.
 *
 * The client used to build this from the 200-row LIST_FILES page, so a channel whose
 * newest upload fell outside that window was invisible in the Files column. One
 * GROUP BY answers it exactly, and it is cheap: the same membership filter as
 * LIST_FILES over an index that migration 0023 already added. */
/* The caller's own live sessions (REQ-182). Expired rows are excluded: you cannot
 * revoke what is already dead, and listing them would pad the view with rows that do
 * nothing. Tokens are never selected — only their hashes exist. */
static oc_dbres *process_list_sessions(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->type = OC_RES_SESSION_LIST;
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "SELECT id, created_at_ms, COALESCE(last_seen_ms,0), expires_at_ms, "
        "       COALESCE(device_label,'') FROM sessions "
        " WHERE user_id=?1 AND expires_at_ms > ?2 "
        " ORDER BY COALESCE(last_seen_ms, created_at_ms) DESC LIMIT ?3;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->user_id);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)dbw_now_ms());
    sqlite3_bind_int64(st, 3, (sqlite3_int64)OC_MAX_SESSIONS);
    oc_session_entry *arr = calloc(OC_MAX_SESSIONS, sizeof *arr);
    size_t n = 0;
    while (arr && n < OC_MAX_SESSIONS && sqlite3_step(st) == SQLITE_ROW) {
        arr[n].session_id = (uint64_t)sqlite3_column_int64(st, 0);
        arr[n].created_at = (uint64_t)sqlite3_column_int64(st, 1);
        arr[n].last_seen  = (uint64_t)sqlite3_column_int64(st, 2);
        arr[n].expires_at = (uint64_t)sqlite3_column_int64(st, 3);
        /* j->message_id carries the asking connection's session id. */
        arr[n].current    = (arr[n].session_id == j->message_id) ? 1 : 0;
        const unsigned char *lb = sqlite3_column_text(st, 4);
        char *cp = strdup(lb ? (const char *)lb : "");
        arr[n].device_label.ptr = (const uint8_t *)cp;
        arr[n].device_label.len = cp ? strlen(cp) : 0;
        n++;
    }
    sqlite3_finalize(st);
    r->sessions = arr; r->n_sessions = n;
    return r;
}

static oc_dbres *process_list_file_channels(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->type = OC_RES_FILE_CHANNELS;
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "SELECT a.channel_id, COUNT(*) FROM attachments a "
        " WHERE a.message_id IS NOT NULL AND a.channel_id IN "
        "       (SELECT channel_id FROM channel_members WHERE user_id=?1) "
        /* A call's chat keeps its files, reached from the call: it is in no
         * list of conversations (CALLS.md §1). */
        "   AND a.channel_id NOT IN (SELECT id FROM channels WHERE call_id IS NOT NULL) "
        " GROUP BY a.channel_id ORDER BY COUNT(*) DESC LIMIT ?2;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->user_id);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)OC_MAX_FILE_CHANNELS);
    oc_file_channel_entry *arr = calloc(OC_MAX_FILE_CHANNELS, sizeof *arr);
    size_t n = 0;
    while (arr && n < OC_MAX_FILE_CHANNELS && sqlite3_step(st) == SQLITE_ROW) {
        arr[n].channel_id = (uint64_t)sqlite3_column_int64(st, 0);
        arr[n].count      = (uint32_t)sqlite3_column_int64(st, 1);
        n++;
    }
    sqlite3_finalize(st);
    r->fchans = arr; r->n_fchans = n;
    return r;
}

static oc_dbres *process_list_files(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->channel_id = j->channel_id;

    /* A channel's files, to anyone who can read it (its messages carry them). */
    if (j->channel_id && !channel_read_access(db, j->channel_id, j->user_id)) {
        r->type = OC_RES_LIST_ERR; r->err_code = OC_ERR_NOT_A_MEMBER; return r;
    }
    r->type = OC_RES_FILE_LIST;

    sqlite3_stmt *st = NULL;
    /* One channel reads 0023's index directly; the workspace-wide form filters
     * by membership instead, which is the same set a backfill would show. */
    /* Newest first, and the cursor is BOTH keys: two files uploaded in the same
     * millisecond order by id, so a page boundary between them neither repeats a
     * row nor drops one. `(created, id) < (?, ?)` is a row-value comparison,
     * which is exactly that rule written once. A first page asks with no cursor.
     *
     * One row more than the page is read, and not sent: it is how the daemon
     * knows whether to say there are more, without a second COUNT over the same
     * index. */
    /* One page's worth, from the configuration read at startup (config.c): the
     * query path must not read the environment, which another thread may be
     * changing. Clamped here too, because a writer driven without a loaded
     * configuration -- the suite that tests this query does exactly that -- would
     * otherwise read a page of zero rows and answer that a channel has no files. */
    int cfg_page = oc_config_get()->file_page;
    if (cfg_page < 1 || cfg_page > (int)OC_MAX_FILE_LIST) cfg_page = (int)OC_MAX_FILE_LIST;
    size_t page = (size_t)cfg_page;
    int paged = j->files_before_ms != 0 || j->files_before_id != 0;
    const char *sql = j->channel_id
        ? (paged
        ? "SELECT a.id, a.channel_id, a.message_id, a.uploader_id, a.size, "
          "       a.created_at_ms, a.reclaimed_at_ms, a.filename, a.mime, m.kind, m.duration_ms "
          "  FROM attachments a LEFT JOIN attachment_media m ON m.attachment_id = a.id "
          " WHERE a.channel_id=?1 AND a.message_id IS NOT NULL "
          "   AND (a.created_at_ms, a.id) < (?3, ?4) "
          "   AND NOT EXISTS(SELECT 1 FROM withheld_messages wm WHERE wm.user_id=?5 AND wm.message_id=a.message_id) "
          " ORDER BY a.created_at_ms DESC, a.id DESC LIMIT ?2;"
        : "SELECT a.id, a.channel_id, a.message_id, a.uploader_id, a.size, "
          "       a.created_at_ms, a.reclaimed_at_ms, a.filename, a.mime, m.kind, m.duration_ms "
          "  FROM attachments a LEFT JOIN attachment_media m ON m.attachment_id = a.id "
          " WHERE a.channel_id=?1 AND a.message_id IS NOT NULL "
          "   AND NOT EXISTS(SELECT 1 FROM withheld_messages wm WHERE wm.user_id=?5 AND wm.message_id=a.message_id) "
          " ORDER BY a.created_at_ms DESC, a.id DESC LIMIT ?2;")
        : (paged
        ? "SELECT a.id, a.channel_id, a.message_id, a.uploader_id, a.size, "
          "       a.created_at_ms, a.reclaimed_at_ms, a.filename, a.mime, m.kind, m.duration_ms "
          "  FROM attachments a LEFT JOIN attachment_media m ON m.attachment_id = a.id "
          " WHERE a.message_id IS NOT NULL AND a.channel_id IN "
          "       (SELECT channel_id FROM channel_members WHERE user_id=?1) "
          "   AND (a.created_at_ms, a.id) < (?3, ?4) "
          "   AND NOT EXISTS(SELECT 1 FROM withheld_messages wm WHERE wm.user_id=?5 AND wm.message_id=a.message_id) "
          " ORDER BY a.created_at_ms DESC, a.id DESC LIMIT ?2;"
        : "SELECT a.id, a.channel_id, a.message_id, a.uploader_id, a.size, "
          "       a.created_at_ms, a.reclaimed_at_ms, a.filename, a.mime, m.kind, m.duration_ms "
          "  FROM attachments a LEFT JOIN attachment_media m ON m.attachment_id = a.id "
          " WHERE a.message_id IS NOT NULL AND a.channel_id IN "
          "       (SELECT channel_id FROM channel_members WHERE user_id=?1) "
          "   AND NOT EXISTS(SELECT 1 FROM withheld_messages wm WHERE wm.user_id=?5 AND wm.message_id=a.message_id) "
          " ORDER BY a.created_at_ms DESC, a.id DESC LIMIT ?2;");
    sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)(j->channel_id ? j->channel_id : j->user_id));
    sqlite3_bind_int64(st, 2, (sqlite3_int64)(page + 1));
    /* The asker: a file on a message a block withheld from them is not theirs to list. */
    sqlite3_bind_int64(st, 5, (sqlite3_int64)j->user_id);
    if (paged) {
        sqlite3_bind_int64(st, 3, (sqlite3_int64)j->files_before_ms);
        sqlite3_bind_int64(st, 4, (sqlite3_int64)j->files_before_id);
    }

    oc_file_row *arr = calloc(page, sizeof *arr);
    size_t n = 0;
    while (arr && n < page && sqlite3_step(st) == SQLITE_ROW) {
        arr[n].id          = (uint64_t)sqlite3_column_int64(st, 0);
        arr[n].channel_id  = (uint64_t)sqlite3_column_int64(st, 1);
        arr[n].message_id  = (uint64_t)sqlite3_column_int64(st, 2);
        arr[n].uploader_id = (uint64_t)sqlite3_column_int64(st, 3);
        arr[n].size        = (uint64_t)sqlite3_column_int64(st, 4);
        arr[n].created_at  = (uint64_t)sqlite3_column_int64(st, 5);
        arr[n].reclaimed   = sqlite3_column_int64(st, 6) != 0;
        const unsigned char *fn = sqlite3_column_text(st, 7);
        const unsigned char *mt = sqlite3_column_text(st, 8);
        arr[n].filename = strdup(fn ? (const char *)fn : "");
        arr[n].mime     = strdup(mt ? (const char *)mt : "");
        arr[n].media_kind  = (uint8_t)sqlite3_column_int(st, 9);
        arr[n].duration_ms = (uint32_t)sqlite3_column_int64(st, 10);
        n++;
    }
    /* The page filled: whether a row beyond it exists is the one extra step. */
    r->flist_more = (n == page && sqlite3_step(st) == SQLITE_ROW) ? 1 : 0;
    sqlite3_finalize(st);
    r->flist = arr;
    r->n_flist = n;
    return r;
}

/* Save or unsave a message (REQ-231, ARCH-95).
 *
 * Private, so there is nothing to fan out — the ack goes to the actor and stops.
 * Keyed (user, message), which is what makes it the mirror of a pin rather than
 * another one: two people may save the same message and neither sees the other.
 * Saving twice is a no-op, as re-pinning is. */
static oc_dbres *process_save_item(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->user_id = j->user_id;   /* told to every connection of theirs */
    r->message_id = j->message_id;
    r->save_op = j->save_op;

    /* You may save anything you can READ; the channel check is the same one that
     * gates seeing it at all. A tombstone has nothing worth keeping. */
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "SELECT channel_id FROM messages WHERE id=? AND deleted_at_ms IS NULL;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->message_id);
    uint64_t cid = (sqlite3_step(st) == SQLITE_ROW) ? (uint64_t)sqlite3_column_int64(st, 0) : 0;
    sqlite3_finalize(st);
    if (!cid) { r->type = OC_RES_LIST_ERR; r->err_code = OC_ERR_UNKNOWN_MESSAGE; return r; }
    if (channel_is_e2e(db, cid)) { r->type = OC_RES_LIST_ERR; r->err_code = OC_ERR_FORBIDDEN; return r; }
    if (!is_member(db, cid, j->user_id)) {
        r->type = OC_RES_LIST_ERR; r->err_code = OC_ERR_NOT_A_MEMBER; return r;
    }

    uint64_t now = dbw_now_ms();
    if (j->save_op == OC_SAVE_ADD) {
        sqlite3_prepare_v2(db,
            "INSERT OR IGNORE INTO saved_items(user_id,message_id,created_at_ms) VALUES(?,?,?);",
            -1, &st, NULL);
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->user_id);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)j->message_id);
        sqlite3_bind_int64(st, 3, (sqlite3_int64)now);
        sqlite3_step(st); sqlite3_finalize(st);
        /* The reminder (REQ-283) is what this ADD says it is: a time sets it
         * afresh, to fire once more; 0 clears it. The frame is the item's whole
         * state, so a client re-saving one sends the reminder it holds. */
        sqlite3_prepare_v2(db,
            "UPDATE saved_items SET remind_at_ms=NULLIF(?3,0), reminded_at_ms=NULL "
            " WHERE user_id=?1 AND message_id=?2;", -1, &st, NULL);
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->user_id);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)j->message_id);
        sqlite3_bind_int64(st, 3, (sqlite3_int64)j->remind_at_ms);
        sqlite3_step(st); sqlite3_finalize(st);
        r->remind_at = j->remind_at_ms;
        /* Report the ORIGINAL save time, so a re-save does not appear to move the
         * item to the top of a list ordered by when you saved it. */
        sqlite3_prepare_v2(db, "SELECT created_at_ms FROM saved_items WHERE user_id=? AND message_id=?;",
                           -1, &st, NULL);
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->user_id);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)j->message_id);
        if (sqlite3_step(st) == SQLITE_ROW) r->saved_at = (uint64_t)sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    } else {
        sqlite3_prepare_v2(db, "DELETE FROM saved_items WHERE user_id=? AND message_id=?;",
                           -1, &st, NULL);
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->user_id);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)j->message_id);
        sqlite3_step(st); sqlite3_finalize(st);
    }
    r->type = OC_RES_SAVED_OK;
    return r;
}

/* Fold a message away for its reader, or open it again (PROTOCOL.md
 * SET_COLLAPSED): a message in a channel they are a member of, not a tombstone.
 * The answer is for every connection of theirs. Write. */
/* Whether `channel_id` is a direct conversation: nothing moves into or out of
 * one, since its readers never chose to share it with a channel. */
static int channel_is_dm(sqlite3 *db, uint64_t channel_id) {
    sqlite3_stmt *st = NULL;
    int yes = 0;
    if (sqlite3_prepare_v2(db, "SELECT kind='dm' FROM channels WHERE id=?1;", -1, &st, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)channel_id);
    if (sqlite3_step(st) == SQLITE_ROW) yes = sqlite3_column_int(st, 0) != 0;
    sqlite3_finalize(st);
    return yes;
}

/* Move a message (REQ-053): a top-level message, or a thread with all of its
 * replies, to another channel's top level; a message with no replies into a
 * thread; a reply out to a top level. The ids, authors, times, edits,
 * reactions, attachments and pins stay; the rows that name a message's
 * channel besides `messages` follow it, in one transaction. An owner or admin
 * moves anything, an author their own, and the mover must be able to post in
 * the destination. A line is left where it was unless the mover said not to.
 *
 * The result is MESSAGE_MOVED for both channels' members; each client reads
 * the moved rows back as it reads any others, so what a reader is shown is
 * keyed on the reader (saved, collapsed) and not on the mover. */
static oc_dbres *process_move_message(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->message_id = j->message_id;
    r->type = OC_RES_MOVE_ERR;

    uint64_t src = 0, author = 0, parent = 0;
    int deleted = 0, found = 0;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT channel_id, author_id, deleted_at_ms, COALESCE(parent_id,0) FROM messages WHERE id=?1;",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->message_id);
        if (sqlite3_step(st) == SQLITE_ROW) {
            src     = (uint64_t)sqlite3_column_int64(st, 0);
            author  = (uint64_t)sqlite3_column_int64(st, 1);
            deleted = sqlite3_column_type(st, 2) != SQLITE_NULL;
            parent  = (uint64_t)sqlite3_column_int64(st, 3);
            found = 1;
        }
        sqlite3_finalize(st);
    }
    r->channel_id = src;
    if (!found || deleted || !channel_read_access(db, src, j->user_id)) {
        r->err_code = OC_ERR_UNKNOWN_MESSAGE; return r;
    }
    uint8_t role = 0;
    int can = author == j->user_id ||
              (user_role(db, j->user_id, &role) && (role == OC_ROLE_ADMIN || role == OC_ROLE_OWNER));
    if (!can) { r->err_code = OC_ERR_FORBIDDEN; return r; }
    if (channel_is_archived(db, src)) { r->err_code = OC_ERR_CHANNEL_ARCHIVED; return r; }
    uint8_t dest_public = 0;
    if (!channel_exists(db, j->channel_id, &dest_public)) { r->err_code = OC_ERR_UNKNOWN_CHANNEL; return r; }
    if (channel_is_e2e(db, src) || channel_is_e2e(db, j->channel_id) ||
        channel_is_dm(db, src) || channel_is_dm(db, j->channel_id)) {
        r->err_code = OC_ERR_CANNOT_MOVE; return r;
    }

    uint32_t replies = 0; uint64_t last = 0;
    thread_stats(db, j->message_id, &replies, &last);
    if (j->parent_id) {
        /* Into a thread: a top-level, living message in the destination, and
         * never one's own thread. A root with replies cannot become a reply --
         * there is no thread inside a thread. */
        uint64_t pa = 0; int pd = 0;
        if (j->parent_id == j->message_id || replies > 0) { r->err_code = OC_ERR_CANNOT_MOVE; return r; }
        if (!message_lookup(db, j->channel_id, j->parent_id, &pa, &pd) || pd) {
            r->err_code = OC_ERR_UNKNOWN_MESSAGE; return r;
        }
        uint64_t pp = 0;
        if (sqlite3_prepare_v2(db, "SELECT COALESCE(parent_id,0) FROM messages WHERE id=?1;", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)j->parent_id);
            if (sqlite3_step(st) == SQLITE_ROW) pp = (uint64_t)sqlite3_column_int64(st, 0);
            sqlite3_finalize(st);
        }
        if (pp) { r->err_code = OC_ERR_CANNOT_MOVE; return r; }
    }
    if (src == j->channel_id && parent == j->parent_id) { r->err_code = OC_ERR_CANNOT_MOVE; return r; }
    /* Last, as it is the one check that writes: a public destination joins the
     * mover, as a send there would. */
    int acc = channel_post_access(db, j->channel_id, j->user_id);
    if (acc == CH_ARCHIVED) { r->err_code = OC_ERR_CHANNEL_ARCHIVED; return r; }
    if (acc != CH_OK)       { r->err_code = OC_ERR_FORBIDDEN; return r; }

    sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, NULL);
    /* The moved set: the message and, when it is a root, its replies. */
    sqlite3_exec(db, "CREATE TEMP TABLE IF NOT EXISTS move_set(id INTEGER PRIMARY KEY); DELETE FROM move_set;",
                 NULL, NULL, NULL);
    int ok = 1;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO temp.move_set(id) SELECT id FROM messages WHERE id=?1 OR (?2 AND parent_id=?1);",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->message_id);
        sqlite3_bind_int(st, 2, parent == 0);
        ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
    } else ok = 0;
    static const char *const follow[] = {
        "UPDATE messages SET channel_id=?1 WHERE id IN (SELECT id FROM temp.move_set);",
        "UPDATE mentions SET channel_id=?1 WHERE message_id IN (SELECT id FROM temp.move_set);",
        "UPDATE pins SET channel_id=?1 WHERE message_id IN (SELECT id FROM temp.move_set);",
        "UPDATE attachments SET channel_id=?1 WHERE message_id IN (SELECT id FROM temp.move_set);",
        "UPDATE unfurls SET channel_id=?1 WHERE message_id IN (SELECT id FROM temp.move_set);",
        "UPDATE polls SET channel_id=?1 WHERE message_id IN (SELECT id FROM temp.move_set);",
        "UPDATE todo_lists SET channel_id=?1 WHERE message_id IN (SELECT id FROM temp.move_set);",
        "UPDATE actions SET channel_id=?1 WHERE message_id IN (SELECT id FROM temp.move_set);",
        "UPDATE thread_follows SET channel_id=?1 WHERE root_id IN (SELECT id FROM temp.move_set);",
        "UPDATE sent_messages SET channel_id=?1 WHERE message_id IN (SELECT id FROM temp.move_set);",
    };
    for (size_t i = 0; ok && i < sizeof follow / sizeof follow[0]; i++) {
        if (sqlite3_prepare_v2(db, follow[i], -1, &st, NULL) != SQLITE_OK) { ok = 0; break; }
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->channel_id);
        ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
    }
    if (ok && sqlite3_prepare_v2(db, "UPDATE messages SET parent_id=NULLIF(?2,0) WHERE id=?1;",
                                 -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->message_id);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)j->parent_id);
        ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
    }
    uint32_t count = 0;
    if (ok && sqlite3_prepare_v2(db, "SELECT id FROM temp.move_set;", -1, &st, NULL) == SQLITE_OK) {
        /* A summary that read it there no longer describes that channel. */
        while (sqlite3_step(st) == SQLITE_ROW) { sum_purge_msg(db, (uint64_t)sqlite3_column_int64(st, 0)); count++; }
        sqlite3_finalize(st);
    }
    if (!ok) {
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        r->err_code = OC_ERR_INTERNAL; return r;
    }

    char detail[96];
    snprintf(detail, sizeof detail, "from %llu to %llu, %u message%s",
             (unsigned long long)src, (unsigned long long)j->channel_id, count, count == 1 ? "" : "s");
    /* In the channel it was moved to, where it now is. */
    audit_actor_in(db, OC_AUDIT_MODERATION, "message.move", j->user_id, j->channel_id, j->message_id,
                   NULL, 1, detail);
    if (j->move_marker) {
        char *who = audit_name_of(db, j->user_id);
        char *dn = NULL;
        if (sqlite3_prepare_v2(db, "SELECT name FROM channels WHERE id=?1;", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)j->channel_id);
            if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0))
                dn = strdup((const char *)sqlite3_column_text(st, 0));
            sqlite3_finalize(st);
        }
        char line[OC_MAX_BODY_SIZE < 512 ? OC_MAX_BODY_SIZE : 512];
        /* The words name where it went and link to it there: a permalink with no
         * host is this workspace's, which a client follows in the app (ARCH-96). */
        int len = snprintf(line, sizeof line, "%s moved [%s](openchime:///c/%llu/m/%llu) to #%s.",
                           who ? who : "Someone", count > 1 ? "a thread" : "a message",
                           (unsigned long long)j->channel_id, (unsigned long long)j->message_id,
                           dn ? dn : "another channel");
        if (len > 0) notice_post(db, src, line, (size_t)len < sizeof line ? (size_t)len : sizeof line - 1);
        free(who); free(dn);
    }
    sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);

    r->type = OC_RES_MESSAGE_MOVED;
    r->user_id = j->user_id;
    r->move_to = j->channel_id;
    r->parent_id = j->parent_id;
    r->reply_count = count;
    load_members(db, src, r);
    {
        oc_dbres d; memset(&d, 0, sizeof d);
        load_members(db, j->channel_id, &d);
        r->dest_members = d.members;
        r->n_dest_members = d.n_members;
    }
    return r;
}

static oc_dbres *process_set_collapsed(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->user_id = j->user_id;
    r->message_id = j->message_id;
    r->save_op = j->save_op ? 1 : 0;
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "SELECT channel_id FROM messages WHERE id=? AND deleted_at_ms IS NULL;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->message_id);
    uint64_t cid = (sqlite3_step(st) == SQLITE_ROW) ? (uint64_t)sqlite3_column_int64(st, 0) : 0;
    sqlite3_finalize(st);
    if (!cid) { r->type = OC_RES_LIST_ERR; r->err_code = OC_ERR_UNKNOWN_MESSAGE; return r; }
    if (!is_member(db, cid, j->user_id)) {
        r->type = OC_RES_LIST_ERR; r->err_code = OC_ERR_NOT_A_MEMBER; return r;
    }
    sqlite3_prepare_v2(db, r->save_op
        ? "INSERT OR IGNORE INTO collapsed_messages(user_id,message_id,created_at_ms) VALUES(?1,?2,?3);"
        : "DELETE FROM collapsed_messages WHERE user_id=?1 AND message_id=?2;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->user_id);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)j->message_id);
    if (r->save_op) sqlite3_bind_int64(st, 3, (sqlite3_int64)dbw_now_ms());
    sqlite3_step(st);
    sqlite3_finalize(st);
    r->type = OC_RES_COLLAPSED;
    return r;
}

/* My saved items, newest save first (REQ-231). Bodies travel with them, as with
 * pins: a saved message is usually far outside loaded history. */
static oc_dbres *process_list_saved(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->type = OC_RES_SAVED_LIST;

    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        /* An action travels as its action text, flagged (REQ-058). */
        "SELECT s.message_id, m.channel_id, m.author_id, m.created_at_ms, s.created_at_ms, substr(m.body,1+COALESCE((SELECT ac.text_start FROM actions ac WHERE ac.message_id=m.id),0)), "
        "       (SELECT a.filename FROM attachments a WHERE a.message_id = m.id ORDER BY a.id LIMIT 1), "
        "       EXISTS(SELECT 1 FROM actions ac WHERE ac.message_id=m.id), "
        "       COALESCE(s.remind_at_ms, 0) "
        "  FROM saved_items s JOIN messages m ON m.id = s.message_id "
        " WHERE s.user_id=?1 AND m.deleted_at_ms IS NULL "
        /* Still gated on membership: leaving a channel should not keep leaking
         * its messages through your saved list. */
        "   AND EXISTS(SELECT 1 FROM channel_members cm "
        "               WHERE cm.channel_id=m.channel_id AND cm.user_id=?1) "
        " ORDER BY s.created_at_ms DESC LIMIT ?2;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->user_id);
    sqlite3_bind_int(st, 2, (int)OC_MAX_SAVED);

    oc_saved_row *arr = calloc(OC_MAX_SAVED, sizeof *arr);
    size_t n = 0;
    while (arr && n < OC_MAX_SAVED && sqlite3_step(st) == SQLITE_ROW) {
        arr[n].message_id = (uint64_t)sqlite3_column_int64(st, 0);
        arr[n].channel_id = (uint64_t)sqlite3_column_int64(st, 1);
        arr[n].author_id  = (uint64_t)sqlite3_column_int64(st, 2);
        arr[n].created_at = (uint64_t)sqlite3_column_int64(st, 3);
        arr[n].saved_at   = (uint64_t)sqlite3_column_int64(st, 4);
        const unsigned char *b = sqlite3_column_text(st, 5);
        const unsigned char *an = sqlite3_column_text(st, 6);
        arr[n].body = b ? strdup((const char *)b) : NULL;
        arr[n].attach_name = an ? strdup((const char *)an) : NULL;
        arr[n].action = (uint8_t)(sqlite3_column_int(st, 7) != 0);
        arr[n].remind_at = (uint64_t)sqlite3_column_int64(st, 8);
        n++;
    }
    sqlite3_finalize(st);
    r->slist = arr; r->n_slist = n;
    return r;
}

/* What involved me (REQ-139, ARCH-95): a union of three bounded queries over
 * rows that already exist and are already indexed — no maintained list to drift
 * out of sync with the truth.
 *
 *   mentions  — `idx_mentions_user`, built by ARCH-89 for exactly this read.
 *   reactions — someone else reacting to a message I wrote.
 *   replies   — someone else replying under a top-level message I wrote.
 *
 * Each excludes the actor being me: an activity feed of my own doings is noise.
 * Each is gated on my still being a member, so leaving a channel stops it
 * leaking. Ordered newest-first and capped as one page. */
static oc_dbres *process_list_activity(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->type = OC_RES_ACTIVITY;

    /* Read the watermark BEFORE stamping it, so this response can tell the
     * client what was already seen. */
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "SELECT activity_seen_ms FROM users WHERE id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->user_id);
    if (sqlite3_step(st) == SQLITE_ROW) r->activity_seen = (uint64_t)sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);

    /* The three UNREAD views (REQ-139). One query with three predicates, not
     * three features: messages past my read cursor, in a conversation I belong
     * to, narrowed by the channel's kind or its notification level.
     *
     * `delivery_cursors` is REQ-090's, already maintained per (user, channel) —
     * LEFT JOINed because a channel you have never acked has no row and every
     * message in it is unread, which an inner join would report as none.
     *
     * Muted conversations are excluded (REQ-137): mute is the strongest "do not
     * hear from this", and an inbox that lists what you muted is an inbox you
     * stop trusting. Your own messages are excluded too — you have read what
     * you wrote. */
    if (j->act_filter != OC_ACTF_INVOLVED) {
        static const char *USQL =
            "SELECT ?5 AS kind, m.id, m.channel_id, m.author_id, m.created_at_ms, "
            "       oc_excerpt(substr(CASE WHEN (SELECT e2e FROM channels WHERE id=m.channel_id)=1 THEN '' ELSE COALESCE(m.body,'') END,1+COALESCE((SELECT ac.text_start FROM actions ac WHERE ac.message_id=m.id),0)),?2) AS text, EXISTS(SELECT 1 FROM actions ac WHERE ac.message_id=m.id) AS act "
            "  FROM messages m "
            "  JOIN channel_members cm ON cm.channel_id = m.channel_id AND cm.user_id = ?1 "
            "  JOIN channels c ON c.id = m.channel_id "
            "  LEFT JOIN delivery_cursors dc ON dc.channel_id = m.channel_id AND dc.user_id = ?1 "
            "  LEFT JOIN notification_prefs np ON np.channel_id = m.channel_id AND np.user_id = ?1 "
            " WHERE m.deleted_at_ms IS NULL "
            "   AND m.author_id <> ?1 "
            "   AND m.author_id NOT IN (SELECT su.id FROM users su WHERE su.is_system=1) "
            "   AND m.id > COALESCE(dc.message_id, 0) "
            "   AND COALESCE(np.muted, 0) = 0 "
            /* Nor from a person I muted: their messages are unread for nobody here. */
            "   AND NOT EXISTS(SELECT 1 FROM muted_users mu WHERE mu.user_id = ?1 AND mu.muted_id = m.author_id) "
            "   AND NOT EXISTS(SELECT 1 FROM withheld_messages wm WHERE wm.user_id = ?1 AND wm.message_id = m.id) "
            "   AND ( ?4 = 1 "
            "         OR (?4 = 2 AND c.kind = 'dm') "
            "         OR (?4 = 3 AND c.kind <> 'dm' "
            "             AND COALESCE(np.level, (SELECT notify_default FROM users WHERE id=?1)) = 0) ) "
            " ORDER BY m.created_at_ms DESC LIMIT ?3;";
        sqlite3_prepare_v2(db, USQL, -1, &st, NULL);
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->user_id);
        sqlite3_bind_int  (st, 2, (int)OC_MAX_PREVIEW);
        sqlite3_bind_int  (st, 3, (int)OC_MAX_ACTIVITY);
        sqlite3_bind_int  (st, 4, (int)j->act_filter);
        sqlite3_bind_int  (st, 5, (int)OC_ACT_UNREAD);
        oc_activity_row *uarr = calloc(OC_MAX_ACTIVITY, sizeof *uarr);
        size_t un = 0;
        while (uarr && un < OC_MAX_ACTIVITY && sqlite3_step(st) == SQLITE_ROW) {
            uarr[un].kind       = (uint8_t)sqlite3_column_int(st, 0);
            uarr[un].message_id = (uint64_t)sqlite3_column_int64(st, 1);
            uarr[un].channel_id = (uint64_t)sqlite3_column_int64(st, 2);
            uarr[un].actor_id   = (uint64_t)sqlite3_column_int64(st, 3);
            uarr[un].at         = (uint64_t)sqlite3_column_int64(st, 4);
            const unsigned char *t = sqlite3_column_text(st, 5);
            uarr[un].text = t ? strdup((const char *)t) : NULL;
            uarr[un].action = (uint8_t)(sqlite3_column_int(st, 6) != 0);   /* REQ-058 */
            un++;
        }
        sqlite3_finalize(st);
        r->alist = uarr; r->n_alist = un;
        /* No watermark stamp here: `activity_seen_ms` marks what is new in the
         * INVOLVED feed, and reading your unreads is not the same act. What
         * clears an unread is acking the conversation (REQ-090). */
        return r;
    }

    static const char *SQL =
        "SELECT kind, message_id, channel_id, actor_id, at, text, act FROM ("
        /* --- mentions of me --- */
        "  SELECT 0 AS kind, mn.message_id AS message_id, mn.channel_id AS channel_id, "
        "         m.author_id AS actor_id, m.created_at_ms AS at, "
        "         oc_excerpt(substr(CASE WHEN (SELECT e2e FROM channels WHERE id=m.channel_id)=1 THEN '' ELSE COALESCE(m.body,'') END,1+COALESCE((SELECT ac.text_start FROM actions ac WHERE ac.message_id=m.id),0)),?2) AS text, EXISTS(SELECT 1 FROM actions ac WHERE ac.message_id=m.id) AS act "
        "    FROM mentions mn JOIN messages m ON m.id = mn.message_id "
        /* Mine -- by name, keyword or group (0, 4, 5) -- or a broadcast
         * (1, 2, 3), named as push.c names them: `kind <> 0` also let in
         * somebody else's keyword hit, and would let in somebody else's row
         * of a group mention. */
        "   WHERE (mn.user_id = ?1 OR mn.kind IN (1,2,3)) AND m.author_id <> ?1 "
        "     AND m.deleted_at_ms IS NULL "
        /* Membership OR a public channel (REQ-288). This gate is where a mention
         * of a non-member actually lands: store_mentions can record the row, but
         * if this still demanded membership the feed would filter it straight
         * back out and the feature would look built and do nothing. A public
         * channel is readable by everyone here, so showing it discloses nothing
         * the person could not already open. */
        "     AND ( EXISTS(SELECT 1 FROM channel_members cm "
        "                   WHERE cm.channel_id = mn.channel_id AND cm.user_id = ?1) "
        "           OR EXISTS(SELECT 1 FROM channels c "
        "                      WHERE c.id = mn.channel_id AND c.is_public = 1 "
        "                        AND c.kind <> 'dm') ) "
        "  UNION ALL "
        /* --- reactions to what I wrote --- */
        "  SELECT 1, rx.message_id, m.channel_id, rx.user_id, rx.created_at_ms, rx.emoji, 0 "
        "    FROM reactions rx JOIN messages m ON m.id = rx.message_id "
        "   WHERE m.author_id = ?1 AND rx.user_id <> ?1 AND m.deleted_at_ms IS NULL "
        "     AND EXISTS(SELECT 1 FROM channel_members cm "
        "                 WHERE cm.channel_id = m.channel_id AND cm.user_id = ?1) "
        "  UNION ALL "
        /* --- replies under something I wrote --- */
        "  SELECT 2, c.id, c.channel_id, c.author_id, c.created_at_ms, "
        "         oc_excerpt(substr(CASE WHEN (SELECT e2e FROM channels WHERE id=c.channel_id)=1 THEN '' ELSE COALESCE(c.body,'') END,1+COALESCE((SELECT ac.text_start FROM actions ac WHERE ac.message_id=c.id),0)),?2), EXISTS(SELECT 1 FROM actions ac WHERE ac.message_id=c.id) "
        "    FROM messages c JOIN messages p ON p.id = c.parent_id "
        "   WHERE p.author_id = ?1 AND c.author_id <> ?1 AND c.deleted_at_ms IS NULL "
        "     AND EXISTS(SELECT 1 FROM channel_members cm "
        "                 WHERE cm.channel_id = c.channel_id AND cm.user_id = ?1) "
        "  UNION ALL "
        /* --- summaries I asked for, made while I was not watching (REQ-310):
         * the notice in message_id, its scope as text, its status as action --- */
        "  SELECT 4, sn.id, sn.channel_id, 0, sn.made_at_ms, CAST(sn.scope AS TEXT), sn.status "
        "    FROM summary_notices sn WHERE sn.user_id = ?1 "
        "  UNION ALL "
        /* --- my reminders that fired (REQ-283): the message, at the moment it
         * fired, by its author; gone with the reminder or the save --- */
        "  SELECT 5, s.message_id, m.channel_id, m.author_id, s.reminded_at_ms, "
        "         oc_excerpt(substr(COALESCE(m.body,''),1+COALESCE((SELECT ac.text_start FROM actions ac WHERE ac.message_id=m.id),0)),?2), EXISTS(SELECT 1 FROM actions ac WHERE ac.message_id=m.id) "
        "    FROM saved_items s JOIN messages m ON m.id = s.message_id "
        "   WHERE s.user_id = ?1 AND s.reminded_at_ms IS NOT NULL AND m.deleted_at_ms IS NULL "
        "     AND EXISTS(SELECT 1 FROM channel_members cm "
        "                 WHERE cm.channel_id = m.channel_id AND cm.user_id = ?1) "
        /* A person I muted mentions, reacts and replies to nobody's feed of mine;
         * my own reminders stay, whoever wrote the message they are on. */
        /* What a block withheld from me -- a mention, a reply -- is not mine to see. */
        ") act_all WHERE (kind = 5 OR actor_id NOT IN (SELECT mu.muted_id FROM muted_users mu WHERE mu.user_id = ?1)) "
        "  AND NOT EXISTS(SELECT 1 FROM withheld_messages wm "
        "                  WHERE wm.user_id = ?1 AND wm.message_id = act_all.message_id) "
        "  ORDER BY at DESC LIMIT ?3;";

    sqlite3_prepare_v2(db, SQL, -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->user_id);
    sqlite3_bind_int(st, 2, (int)OC_MAX_PREVIEW);
    sqlite3_bind_int(st, 3, (int)OC_MAX_ACTIVITY);

    oc_activity_row *arr = calloc(OC_MAX_ACTIVITY, sizeof *arr);
    size_t n = 0;
    while (arr && n < OC_MAX_ACTIVITY && sqlite3_step(st) == SQLITE_ROW) {
        arr[n].kind       = (uint8_t)sqlite3_column_int(st, 0);
        arr[n].message_id = (uint64_t)sqlite3_column_int64(st, 1);
        arr[n].channel_id = (uint64_t)sqlite3_column_int64(st, 2);
        arr[n].actor_id   = (uint64_t)sqlite3_column_int64(st, 3);
        arr[n].at         = (uint64_t)sqlite3_column_int64(st, 4);
        const unsigned char *t = sqlite3_column_text(st, 5);
        arr[n].text = t ? strdup((const char *)t) : NULL;
        /* REQ-058's flag; a summary's status (OC_ACT_SUMMARY). */
        arr[n].action = arr[n].kind == OC_ACT_SUMMARY ? (uint8_t)sqlite3_column_int(st, 6)
                                                      : (uint8_t)(sqlite3_column_int(st, 6) != 0);
        n++;
    }
    sqlite3_finalize(st);
    r->alist = arr; r->n_alist = n;

    /* Stamp the watermark now that we have read it: opening the feed IS seeing
     * it. Nothing finer, by ARCH-95 — per-item read state is what a table would
     * be for, and there is no table. */
    sqlite3_prepare_v2(db, "UPDATE users SET activity_seen_ms=? WHERE id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)dbw_now_ms());
    sqlite3_bind_int64(st, 2, (sqlite3_int64)j->user_id);
    sqlite3_step(st); sqlite3_finalize(st);
    return r;
}

/* Pin or unpin a message (REQ-230, ARCH-90).
 *
 * A pin belongs to the channel, so any member may place one and any member may
 * remove one — including someone else's. That is Slack's default, and the
 * alternative fails worse: a pin only its author can remove outlives its
 * usefulness the moment that person leaves.
 *
 * Pinning an already-pinned message is a no-op, not an error, matching how
 * REACT treats a repeat add. The cap is checked before the insert so a channel
 * cannot grow an unbounded pin list. */
static oc_dbres *process_pin(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->channel_id = j->channel_id;
    r->message_id = j->message_id;
    r->user_id = j->user_id;
    r->pin_op = j->pin_op;

    /* The message must exist, live in this channel, and not be a tombstone:
     * there is nothing to pin to a message whose body is gone (REQ-052). */
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "SELECT 1 FROM messages WHERE id=? AND channel_id=? AND deleted_at_ms IS NULL;",
        -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->message_id);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)j->channel_id);
    int exists = (sqlite3_step(st) == SQLITE_ROW);
    sqlite3_finalize(st);
    if (!exists) { r->type = OC_RES_PIN_ERR; r->err_code = OC_ERR_UNKNOWN_MESSAGE; return r; }
    if (!is_member(db, j->channel_id, j->user_id)) {
        r->type = OC_RES_PIN_ERR; r->err_code = OC_ERR_NOT_A_MEMBER; return r;
    }
    if (channel_is_e2e(db, j->channel_id)) { r->type = OC_RES_PIN_ERR; r->err_code = OC_ERR_FORBIDDEN; return r; }

    uint64_t now = dbw_now_ms();
    if (j->pin_op == OC_PIN_ADD) {
        /* The cap is per channel and excludes a re-pin of something already
         * pinned, which is why the count is taken before the insert but only
         * blocks when this message is not already in the set. */
        sqlite3_prepare_v2(db,
            "SELECT COUNT(*), SUM(message_id=?2) FROM pins WHERE channel_id=?1;", -1, &st, NULL);
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->channel_id);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)j->message_id);
        uint64_t n = 0; int already = 0;
        if (sqlite3_step(st) == SQLITE_ROW) {
            n = (uint64_t)sqlite3_column_int64(st, 0);
            already = sqlite3_column_int(st, 1) > 0;
        }
        sqlite3_finalize(st);
        if (!already && n >= OC_MAX_PINS) {
            r->type = OC_RES_PIN_ERR; r->err_code = OC_ERR_TOO_MANY_PINS; return r;
        }
        sqlite3_prepare_v2(db,
            "INSERT OR IGNORE INTO pins(message_id,channel_id,pinned_by,created_at_ms) "
            "VALUES(?,?,?,?);", -1, &st, NULL);
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->message_id);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)j->channel_id);
        sqlite3_bind_int64(st, 3, (sqlite3_int64)j->user_id);
        sqlite3_bind_int64(st, 4, (sqlite3_int64)now);
        sqlite3_step(st); sqlite3_finalize(st);
        /* Report the pin's real time and pinner, which for a re-pin are the
         * original ones — the fan-out must agree with what a later LIST_PINS
         * will say, or two clients disagree about who pinned it. */
        sqlite3_prepare_v2(db, "SELECT pinned_by, created_at_ms FROM pins WHERE message_id=?;",
                           -1, &st, NULL);
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->message_id);
        if (sqlite3_step(st) == SQLITE_ROW) {
            r->user_id   = (uint64_t)sqlite3_column_int64(st, 0);
            r->pinned_at = (uint64_t)sqlite3_column_int64(st, 1);
        }
        sqlite3_finalize(st);
    } else {
        sqlite3_prepare_v2(db, "DELETE FROM pins WHERE message_id=? AND channel_id=?;",
                           -1, &st, NULL);
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->message_id);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)j->channel_id);
        sqlite3_step(st); sqlite3_finalize(st);
        r->pinned_at = now;
    }

    r->type = OC_RES_PIN_OK;
    load_members(db, j->channel_id, r);
    return r;
}

/* A channel's pinned messages, newest pin first (REQ-230). The body travels
 * with each row: a pin is usually old enough to be outside the client's loaded
 * history, so returning ids alone would make opening the list a fetch storm. */
static oc_dbres *process_list_pins(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->channel_id = j->channel_id;

    /* A channel's pins, to anyone who can read it. */
    if (!channel_read_access(db, j->channel_id, j->user_id)) {
        r->type = OC_RES_PIN_ERR; r->err_code = OC_ERR_NOT_A_MEMBER; return r;
    }
    r->type = OC_RES_PINS;

    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "SELECT p.message_id, m.author_id, m.created_at_ms, "
        "       COALESCE(p.pinned_by,0), p.created_at_ms, substr(m.body,1+COALESCE((SELECT ac.text_start FROM actions ac WHERE ac.message_id=m.id),0)), "
        /* An attachment-only message has no body, so the list needs something
         * to show for it or the row renders blank. */
        "       (SELECT a.filename FROM attachments a WHERE a.message_id = m.id "
        "         ORDER BY a.id LIMIT 1), "
        /* An action travels as its action text, flagged (REQ-058). */
        "       EXISTS(SELECT 1 FROM actions ac WHERE ac.message_id=m.id) "
        "  FROM pins p JOIN messages m ON m.id = p.message_id "
        " WHERE p.channel_id=?1 AND m.deleted_at_ms IS NULL "
        /* Not a message a block withheld from the asker. */
        "   AND NOT EXISTS(SELECT 1 FROM withheld_messages wm WHERE wm.user_id=?3 AND wm.message_id=m.id) "
        " ORDER BY p.created_at_ms DESC LIMIT ?2;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->channel_id);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)OC_MAX_PINS);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)j->user_id);

    oc_pin_row *arr = calloc(OC_MAX_PINS, sizeof *arr);
    size_t n = 0;
    while (arr && n < OC_MAX_PINS && sqlite3_step(st) == SQLITE_ROW) {
        arr[n].message_id    = (uint64_t)sqlite3_column_int64(st, 0);
        arr[n].author_id     = (uint64_t)sqlite3_column_int64(st, 1);
        arr[n].created_at_ms = (uint64_t)sqlite3_column_int64(st, 2);
        arr[n].pinned_by     = (uint64_t)sqlite3_column_int64(st, 3);
        arr[n].pinned_at     = (uint64_t)sqlite3_column_int64(st, 4);
        const unsigned char *b = sqlite3_column_text(st, 5);
        arr[n].body = b ? strdup((const char *)b) : NULL;
        const unsigned char *an = sqlite3_column_text(st, 6);
        arr[n].attach_name = an ? strdup((const char *)an) : NULL;
        arr[n].action = (uint8_t)(sqlite3_column_int(st, 7) != 0);
        n++;
    }
    sqlite3_finalize(st);
    r->plist = arr;
    r->n_plist = n;
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
    if (e2e_closed(db, j->channel_id)) { r->type = OC_RES_REACTION_ERR; r->err_code = OC_ERR_CHANNEL_ARCHIVED; return r; }

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
    int own = owh_txn_begin(db);
    sqlite3_step(st);
    sqlite3_finalize(st);
    if (sqlite3_changes(db) > 0) {   /* a reaction already there, or already gone, is no event */
        char *em = strndup(j->emoji, elen);
        owh_reaction(db, j->channel_id, j->message_id, j->user_id, em, j->react_op == OC_REACT_ADD);
        free(em);
    }
    owh_txn_end(db, own);

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
    /* Whoever has muted someone who reacted with this emoji is sent the count
     * without them (SET_USER_MUTE): a muted person's reactions stay out of the chips
     * they see, as their messages stay out of the badges. */
    if (sqlite3_prepare_v2(db,
            "SELECT mu.user_id, COUNT(*) FROM reactions rx "
            "  JOIN muted_users mu ON mu.muted_id = rx.user_id "
            " WHERE rx.message_id=?1 AND rx.emoji=?2 GROUP BY mu.user_id;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->message_id);
        sqlite3_bind_text(st, 2, j->emoji, (int)elen, SQLITE_STATIC);
        size_t cap = 0;
        while (sqlite3_step(st) == SQLITE_ROW) {
            if (r->n_react_adj == cap) {
                size_t nc = cap ? cap * 2 : 4;
                uint64_t *u = realloc(r->react_adj_user, nc * sizeof *u);
                if (!u) break;
                r->react_adj_user = u;
                uint64_t *k = realloc(r->react_adj_minus, nc * sizeof *k);
                if (!k) break;
                r->react_adj_minus = k;
                cap = nc;
            }
            r->react_adj_user[r->n_react_adj] = (uint64_t)sqlite3_column_int64(st, 0);
            r->react_adj_minus[r->n_react_adj] = (uint64_t)sqlite3_column_int64(st, 1);
            r->n_react_adj++;
        }
        sqlite3_finalize(st);
    }
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

/* A vote (REQ-225): the voter's whole selection, replacing what they had --
 * none withdraws it. Single choice is enforced here, since the table's key
 * has to admit the multiple-choice case; a poll past its deadline takes no
 * more. The answer is the new tally with the voter's own selection. Votes
 * never notify: the net thread sends the tally and nothing else. */
static oc_dbres *process_vote(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->channel_id = j->channel_id;
    r->message_id = j->message_id;
    r->type = OC_RES_POLL_ERR;

    uint64_t author = 0; int deleted = 0;
    if (!message_lookup(db, j->channel_id, j->message_id, &author, &deleted) || deleted) {
        r->err_code = OC_ERR_UNKNOWN_MESSAGE; return r;
    }
    if (!channel_read_access(db, j->channel_id, j->user_id)) { r->err_code = OC_ERR_NOT_A_MEMBER; return r; }
    if (e2e_closed(db, j->channel_id)) { r->err_code = OC_ERR_CHANNEL_ARCHIVED; return r; }

    sqlite3_stmt *st = NULL;
    int found = 0, multi = 0, n_opts = 0;
    uint64_t close_at = 0;
    if (sqlite3_prepare_v2(db,
            "SELECT multi, COALESCE(close_at_ms, 0), (SELECT COUNT(*) FROM poll_options o WHERE o.message_id=p.message_id) "
            "FROM polls p WHERE message_id=?1;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->message_id);
        if (sqlite3_step(st) == SQLITE_ROW) {
            found = 1;
            multi = sqlite3_column_int(st, 0);
            close_at = (uint64_t)sqlite3_column_int64(st, 1);
            n_opts = sqlite3_column_int(st, 2);
        }
        sqlite3_finalize(st);
    }
    if (!found) { r->err_code = OC_ERR_UNKNOWN_MESSAGE; return r; }
    uint64_t now = dbw_now_ms();
    if (close_at && now >= close_at) { r->err_code = OC_ERR_POLL_CLOSED; return r; }
    if (!multi && j->n_poll_picks > 1) { r->err_code = OC_ERR_INVALID_POLL; return r; }
    uint16_t seen = 0;
    for (uint8_t i = 0; i < j->n_poll_picks; i++) {
        uint8_t k = j->poll_picks[i];
        if (k >= n_opts || (seen & (1u << k))) { r->err_code = OC_ERR_INVALID_POLL; return r; }
        seen |= (uint16_t)(1u << k);
    }

    sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);
    if (sqlite3_prepare_v2(db, "DELETE FROM poll_votes WHERE message_id=?1 AND user_id=?2;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->message_id);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)j->user_id);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    if (j->n_poll_picks &&
        sqlite3_prepare_v2(db, "INSERT INTO poll_votes(message_id, user_id, idx, voted_at_ms) VALUES(?1, ?2, ?3, ?4);",
                           -1, &st, NULL) == SQLITE_OK) {
        for (uint8_t i = 0; i < j->n_poll_picks; i++) {
            sqlite3_reset(st);
            sqlite3_bind_int64(st, 1, (sqlite3_int64)j->message_id);
            sqlite3_bind_int64(st, 2, (sqlite3_int64)j->user_id);
            sqlite3_bind_int(st, 3, j->poll_picks[i]);
            sqlite3_bind_int64(st, 4, (sqlite3_int64)now);
            sqlite3_step(st);
        }
        sqlite3_finalize(st);
    }
    sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);

    r->type = OC_RES_POLL;
    r->user_id = j->user_id;
    append_poll(db, r, j->message_id, j->channel_id, j->user_id);
    load_members(db, j->channel_id, r);
    return r;
}

/* The author changes a poll (REQ-225, ARCH-119): everything until anyone has
 * voted; after that the deadline only, since options or settings moving under
 * cast votes would change what those votes meant. The job carries all of it
 * as it is now to be, so "unchanged" is a comparison with what is stored. The
 * deadline may be put back, brought forward, or set to now to close it; 0
 * means it never closes. */
static oc_dbres *process_edit_poll(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->channel_id = j->channel_id;
    r->message_id = j->message_id;
    r->type = OC_RES_POLL_ERR;

    uint64_t author = 0; int deleted = 0;
    if (!message_lookup(db, j->channel_id, j->message_id, &author, &deleted) || deleted) {
        r->err_code = OC_ERR_UNKNOWN_MESSAGE; return r;
    }
    if (author != j->user_id) { r->err_code = OC_ERR_FORBIDDEN; return r; }
    int acc = channel_post_access(db, j->channel_id, j->user_id);
    if (acc == CH_DENIED || acc == CH_UNKNOWN) { r->err_code = OC_ERR_NOT_A_MEMBER; return r; }
    if (acc == CH_ARCHIVED) { r->err_code = OC_ERR_CHANNEL_ARCHIVED; return r; }
    if (!poll_options_valid(j)) { r->err_code = OC_ERR_INVALID_POLL; return r; }

    sqlite3_stmt *st = NULL;
    int found = 0, multi = 0, anon = 0, votes = 0;
    if (sqlite3_prepare_v2(db,
            "SELECT multi, anonymous, (SELECT COUNT(*) FROM poll_votes v WHERE v.message_id=p.message_id) "
            "FROM polls p WHERE message_id=?1;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->message_id);
        if (sqlite3_step(st) == SQLITE_ROW) {
            found = 1;
            multi = sqlite3_column_int(st, 0);
            anon = sqlite3_column_int(st, 1);
            votes = sqlite3_column_int(st, 2);
        }
        sqlite3_finalize(st);
    }
    if (!found) { r->err_code = OC_ERR_UNKNOWN_MESSAGE; return r; }
    int want_multi = (j->poll_flags & OC_POLL_MULTI) != 0, want_anon = (j->poll_flags & OC_POLL_ANONYMOUS) != 0;
    int same = want_multi == multi && want_anon == anon;
    if (same && sqlite3_prepare_v2(db, "SELECT idx, label FROM poll_options WHERE message_id=?1 ORDER BY idx;",
                                   -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->message_id);
        uint8_t n = 0;
        while (same && sqlite3_step(st) == SQLITE_ROW) {
            const char *l = (const char *)sqlite3_column_text(st, 1);
            if (n >= j->n_poll_opts || strcmp(l ? l : "", j->poll_opts[n]) != 0) same = 0;
            n++;
        }
        if (n != j->n_poll_opts) same = 0;
        sqlite3_finalize(st);
    }
    if (!same && votes) { r->err_code = OC_ERR_POLL_HAS_VOTES; return r; }

    sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);
    if (sqlite3_prepare_v2(db, "UPDATE polls SET multi=?2, anonymous=?3, close_at_ms=NULLIF(?4, 0) WHERE message_id=?1;",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->message_id);
        sqlite3_bind_int(st, 2, want_multi);
        sqlite3_bind_int(st, 3, want_anon);
        sqlite3_bind_int64(st, 4, (sqlite3_int64)j->poll_close_at_ms);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    if (!same) {
        if (sqlite3_prepare_v2(db, "DELETE FROM poll_options WHERE message_id=?1;", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)j->message_id);
            sqlite3_step(st);
            sqlite3_finalize(st);
        }
        store_poll_options(db, j->message_id, j);
    }
    sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);

    r->type = OC_RES_POLL_EDITED;
    append_poll(db, r, j->message_id, j->channel_id, 0);
    load_members(db, j->channel_id, r);
    return r;
}

/* Who picked what on a poll (REQ-225), for a reader of the conversation. An
 * anonymous poll is refused HERE: the rows exist, one vote each needs them,
 * and this is the one place that could send them. */
static oc_dbres *process_list_poll_voters(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->channel_id = j->channel_id;
    r->message_id = j->message_id;
    r->type = OC_RES_POLL_ERR;

    uint64_t author = 0; int deleted = 0;
    if (!message_lookup(db, j->channel_id, j->message_id, &author, &deleted) || deleted) {
        r->err_code = OC_ERR_UNKNOWN_MESSAGE; return r;
    }
    if (!channel_read_access(db, j->channel_id, j->user_id)) { r->err_code = OC_ERR_NOT_A_MEMBER; return r; }
    sqlite3_stmt *st = NULL;
    int found = 0, anon = 0;
    if (sqlite3_prepare_v2(db, "SELECT anonymous FROM polls WHERE message_id=?1;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->message_id);
        if (sqlite3_step(st) == SQLITE_ROW) { found = 1; anon = sqlite3_column_int(st, 0); }
        sqlite3_finalize(st);
    }
    if (!found) { r->err_code = OC_ERR_UNKNOWN_MESSAGE; return r; }
    if (anon) { r->err_code = OC_ERR_FORBIDDEN; return r; }

    r->type = OC_RES_POLL_VOTERS;
    if (sqlite3_prepare_v2(db, "SELECT idx, user_id FROM poll_votes WHERE message_id=?1 ORDER BY idx, user_id;",
                           -1, &st, NULL) != SQLITE_OK)
        return r;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->message_id);
    size_t cap = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (r->n_pvoters == cap) {
            size_t nc = cap ? cap * 2 : 16;
            oc_poll_voter *g = realloc(r->pvoters, nc * sizeof *g);
            if (!g) break;
            r->pvoters = g; cap = nc;
        }
        r->pvoters[r->n_pvoters].option = (uint8_t)sqlite3_column_int(st, 0);
        r->pvoters[r->n_pvoters].user_id = (uint64_t)sqlite3_column_int64(st, 1);
        r->n_pvoters++;
    }
    sqlite3_finalize(st);
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

/* The most recent distinct authors of `root`'s replies that are not deleted,
 * newest first, at most OC_THREAD_REPLIERS_MAX (REQ-060). A deleted reply is
 * still counted in the thread's reply count, but names nobody here. */
static uint8_t thread_repliers(sqlite3 *db, uint64_t root, uint64_t *out) {
    sqlite3_stmt *st = NULL;
    uint8_t n = 0;
    if (sqlite3_prepare_v2(db,
            "SELECT author_id FROM messages WHERE parent_id=?1 AND deleted_at_ms IS NULL "
            " GROUP BY author_id ORDER BY MAX(created_at_ms) DESC, MAX(id) DESC LIMIT ?2;",
            -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)root);
    sqlite3_bind_int(st, 2, OC_THREAD_REPLIERS_MAX);
    while (n < OC_THREAD_REPLIERS_MAX && sqlite3_step(st) == SQLITE_ROW)
        out[n++] = (uint64_t)sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return n;
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
        "SELECT s.message_id, m.created_at_ms, m.parent_id, m.also_channel FROM sent_messages s "
        "JOIN messages m ON m.id = s.message_id "
        "WHERE s.channel_id=? AND s.idempotency_token=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->channel_id);
    sqlite3_bind_blob(st, 2, j->idem, OC_IDEM_LEN, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) {
        r->type = OC_RES_REPLY_OK;
        r->message_id = (uint64_t)sqlite3_column_int64(st, 0);
        r->server_time = (uint64_t)sqlite3_column_int64(st, 1);
        r->parent_id = (uint64_t)sqlite3_column_int64(st, 2);
        r->also_channel = (uint8_t)sqlite3_column_int(st, 3);
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
    if (workspace_dm(db, j->channel_id)) { r->type = OC_RES_REPLY_ERR; r->err_code = OC_ERR_WORKSPACE_SENDER; return r; }
    if (acc == CH_ARCHIVED){ r->type = OC_RES_REPLY_ERR; r->err_code = OC_ERR_CHANNEL_ARCHIVED; return r; }
    r->ch_e2e = (uint8_t)channel_is_e2e(db, j->channel_id);
    if (r->ch_e2e && (j->n_attach || j->src_message || j->n_poll_opts)) { r->type = OC_RES_REPLY_ERR; r->err_code = OC_ERR_FORBIDDEN; return r; }
    if (r->ch_e2e && j->via && j->via[0]) { r->type = OC_RES_REPLY_ERR; r->err_code = OC_ERR_FORBIDDEN; return r; }

    uint64_t ts = dbw_now_ms();
    if (j->n_poll_opts && !poll_valid(j, ts)) { r->type = OC_RES_REPLY_ERR; r->err_code = OC_ERR_INVALID_POLL; return r; }
    oc_todo_parsed tp;   /* a reply can be a to-do list, as a channel message can */
    int is_todo = 0;
    if (!r->ch_e2e && !j->n_poll_opts) {
        uint16_t te = todo_check(j->body, j->body_len, &tp, &is_todo);
        if (te) { r->type = OC_RES_REPLY_ERR; r->err_code = te; return r; }
    }
    sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);
    /* Also to the channel (REQ-062): one row, flagged, never a copy -- the main
     * scroll's queries take a flagged reply as they take a top-level message. */
    sqlite3_prepare_v2(db,
        "INSERT INTO messages(channel_id, author_id, body, created_at_ms, parent_id, also_channel, via) "
        "VALUES(?, ?, ?, ?, ?, ?, ?);", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->channel_id);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)j->user_id);
    sqlite3_bind_blob(st, 3, j->body, (int)j->body_len, SQLITE_STATIC);
    sqlite3_bind_int64(st, 4, (sqlite3_int64)ts);
    sqlite3_bind_int64(st, 5, (sqlite3_int64)root);
    sqlite3_bind_int(st, 6, j->also_channel ? 1 : 0);
    sqlite3_bind_text(st, 7, j->via ? j->via : "", -1, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        r->type = OC_RES_REPLY_ERR; r->err_code = OC_ERR_INTERNAL; return r;
    }
    uint64_t mid = (uint64_t)sqlite3_last_insert_rowid(db);
    /* A reply's mentions are stored exactly as a channel message's are. They
     * were not, and nothing downstream could work without them: the push
     * MENTIONS branch, the activity feed's mention arm and the reader's
     * highlight all read this table, so naming somebody in a thread reached
     * them nowhere at all. */
    r->unres.channel_id = j->channel_id;
    r->unres.message_id = mid;
    store_mentions(db, mid, j->channel_id, j->body, j->body_len, ts, &r->unres, &r->unres_total);
    /* Keywords fire in THREADS (REQ-135), which is a deliberate divergence:
     * Slack's help says keywords in thread messages never notify, and a thread
     * is where the substantive discussion usually is — the worst place to go
     * deaf. */
    store_keyword_hits(db, mid, j->channel_id, j->user_id, j->body, j->body_len, ts);
    fill_unresolved_context(db, j->channel_id, &r->unres);
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

    /* This THREAD's draft, keyed by its root — not the channel's. No client
     * writes one yet (ARCH-101 keeps the column ahead of the client), so today
     * this deletes nothing; it is here so the day one does, the clear already
     * works and is not a second thing to remember. */
    drop_draft(db, j->user_id, j->channel_id, j->parent_id);
    store_action(db, mid);   /* a reply can be an action too (REQ-058) */
    if (j->n_poll_opts) store_poll(db, mid, j->channel_id, j, ts);   /* and a poll (REQ-225) */
    if (is_todo) store_todo(db, mid, j->channel_id, j->user_id, j->body, &tp, ts);
    owh_message(db, mid, OWH_MSG_NEW);

    sum_purge_msg(db, mid);
    sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);

    uint32_t count = 0;
    thread_stats(db, root, &count, &r->thread_last_reply_at);
    r->n_repliers = thread_repliers(db, root, r->repliers);
    r->type = OC_RES_REPLY_OK;
    r->message_id = mid;
    r->server_time = ts;
    r->parent_id = root;
    r->reply_count = count;
    r->also_channel = j->also_channel ? 1 : 0;
    if (j->via && j->via[0]) r->via = strdup(j->via);
    append_action(db, r, mid, j->channel_id, 1);   /* always: the client pairs them (ARCH-115) */
    if (j->n_poll_opts) append_poll(db, r, mid, j->channel_id, 0);
    if (is_todo) append_todo(db, r, mid);
    if (j->body_len) { r->body = malloc(j->body_len); if (r->body) { memcpy(r->body, j->body, j->body_len); r->body_len = j->body_len; } }
    r->broadcast_stored = broadcast_stored(db, mid);
    load_members(db, j->channel_id, r);
    withhold_from_blockers(db, j->channel_id, j->user_id, mid, r);   /* in a DM, not to whoever blocks the author */
    /* Asked AFTER the reply is committed, so the author of the reply being
     * fanned out counts as a participant on the strength of it. */
    load_thread_participation(db, root, r);
    return r;
}

/* --- working a to-do list -------------------------------------------------- *
 * Anyone who can read the message adds a task or marks one; only its author
 * renames the list. Nothing changes in an archived channel. Each answers the
 * whole list to the conversation, so every reader converges on the daemon's
 * state, whatever order changes arrive in. */

/* The checks every change makes: the message stands in the channel named, has
 * a list, and the asker may read it there, which is not archived. 0 when it
 * may go ahead, filling *list_author; else the error. */
static uint16_t todo_target(sqlite3 *db, const oc_job *j, uint64_t *list_author, uint16_t *count) {
    uint64_t author = 0; int deleted = 0;
    if (!message_lookup(db, j->channel_id, j->message_id, &author, &deleted) || deleted)
        return OC_ERR_UNKNOWN_MESSAGE;
    if (!channel_read_access(db, j->channel_id, j->user_id)) return OC_ERR_UNKNOWN_MESSAGE;
    sqlite3_stmt *st = NULL;
    int found = 0;
    if (sqlite3_prepare_v2(db,
            "SELECT author_id, (SELECT COUNT(*) FROM todo_tasks k WHERE k.message_id=t.message_id) "
            "  FROM todo_lists t WHERE message_id=?1;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->message_id);
        if (sqlite3_step(st) == SQLITE_ROW) {
            *list_author = (uint64_t)sqlite3_column_int64(st, 0);
            *count = (uint16_t)sqlite3_column_int(st, 1);
            found = 1;
        }
        sqlite3_finalize(st);
    }
    if (!found) return OC_ERR_UNKNOWN_MESSAGE;
    if (channel_is_archived(db, j->channel_id) || e2e_closed(db, j->channel_id)) return OC_ERR_CHANNEL_ARCHIVED;
    return 0;
}

/* A string as a list keeps it: spaces and tabs off both ends. */
static size_t todo_trimmed(const char *s, size_t *start) {
    size_t a = 0, b = s ? strlen(s) : 0;
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) b--;
    *start = a;
    return b - a;
}

static oc_dbres *process_todo(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->channel_id = j->channel_id;
    r->message_id = j->message_id;
    r->type = OC_RES_TODO_ERR;
    uint64_t list_author = 0;
    uint16_t count = 0;
    uint16_t err = todo_target(db, j, &list_author, &count);
    if (err) { r->err_code = err; return r; }
    sqlite3_stmt *st = NULL;
    int ok = 0;
    if (j->type == OC_JOB_TODO_ADD) {
        size_t na, da, nl = todo_trimmed(j->todo_name, &na), dl = todo_trimmed(j->todo_desc, &da);
        if (nl == 0 || nl > OC_TODO_NAME_MAX || dl > OC_TODO_DESC_MAX) { r->err_code = OC_ERR_TODO_INVALID; return r; }
        if (count >= OC_TODO_MAX_TASKS) { r->err_code = OC_ERR_TODO_FULL; return r; }
        int dup = 0;
        if (sqlite3_prepare_v2(db, "SELECT 1 FROM todo_tasks WHERE message_id=?1 AND name=?2;", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)j->message_id);
            sqlite3_bind_text (st, 2, j->todo_name + na, (int)nl, SQLITE_STATIC);
            dup = sqlite3_step(st) == SQLITE_ROW;
            sqlite3_finalize(st);
        }
        if (dup) { r->err_code = OC_ERR_TODO_DUPLICATE; return r; }
        if (sqlite3_prepare_v2(db,
                "INSERT INTO todo_tasks(message_id, idx, name, description) "
                "VALUES(?1, (SELECT COUNT(*) FROM todo_tasks WHERE message_id=?1), ?2, ?3);",
                -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)j->message_id);
            sqlite3_bind_text (st, 2, j->todo_name + na, (int)nl, SQLITE_STATIC);
            sqlite3_bind_text (st, 3, j->todo_desc ? j->todo_desc + da : "", (int)dl, SQLITE_STATIC);
            ok = sqlite3_step(st) == SQLITE_DONE;
            sqlite3_finalize(st);
        }
    } else if (j->type == OC_JOB_TODO_MARK) {
        if (j->todo_task >= count) { r->err_code = OC_ERR_TODO_INVALID; return r; }
        /* The state wanted, not a toggle: already there, nothing changes --
         * not even who marked it -- so concurrent marks agree. */
        if (sqlite3_prepare_v2(db,
                "UPDATE todo_tasks SET done=?3, done_by=CASE WHEN ?3 THEN ?4 END, "
                "  done_at_ms=CASE WHEN ?3 THEN ?5 END "
                " WHERE message_id=?1 AND idx=?2 AND done<>?3;", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)j->message_id);
            sqlite3_bind_int  (st, 2, j->todo_task);
            sqlite3_bind_int  (st, 3, j->todo_done ? 1 : 0);
            sqlite3_bind_int64(st, 4, (sqlite3_int64)j->user_id);
            sqlite3_bind_int64(st, 5, (sqlite3_int64)dbw_now_ms());
            ok = sqlite3_step(st) == SQLITE_DONE;
            sqlite3_finalize(st);
        }
    } else {   /* OC_JOB_TODO_RENAME */
        if (list_author != j->user_id) { r->err_code = OC_ERR_FORBIDDEN; return r; }
        size_t ta, tl = todo_trimmed(j->todo_name, &ta);
        if (tl == 0 || tl > OC_TODO_TITLE_MAX) { r->err_code = OC_ERR_TODO_INVALID; return r; }
        if (sqlite3_prepare_v2(db, "UPDATE todo_lists SET title=?2 WHERE message_id=?1;", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)j->message_id);
            sqlite3_bind_text (st, 2, j->todo_name + ta, (int)tl, SQLITE_STATIC);
            ok = sqlite3_step(st) == SQLITE_DONE;
            sqlite3_finalize(st);
        }
    }
    if (!ok) { r->err_code = OC_ERR_INTERNAL; return r; }
    r->type = OC_RES_TODO;
    r->user_id = j->user_id;
    append_todo(db, r, j->message_id);
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
    r->list_participant = thread_participant(db, j->user_id, j->parent_id);

    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "SELECT id, author_id, created_at_ms, body FROM messages "
        "WHERE parent_id=?1 "
        /* Not a reply a block withheld from this reader, as backfill leaves it out. */
        "  AND NOT EXISTS(SELECT 1 FROM withheld_messages wm WHERE wm.user_id=?3 AND wm.message_id=messages.id) "
        "ORDER BY id LIMIT ?2;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->parent_id);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)OC_BACKFILL_MAX);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)j->user_id);
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
        m->broadcast_stored = broadcast_stored(db, m->message_id);
        m->via = message_via(db, m->message_id);
        load_message_attachments(db, m->message_id, m->attach, &m->n_attach);  /* REQ-140 */
        n++;
    }
    sqlite3_finalize(st);
    r->thread = arr;
    r->n_thread = n;
    r->truncated = (n >= OC_BACKFILL_MAX);
    for (size_t i = 0; i < n; i++) append_action(db, r, arr[i].message_id, j->channel_id, 0);   /* REQ-058 */
    for (size_t i = 0; i < n; i++) append_poll(db, r, arr[i].message_id, j->channel_id, j->user_id);   /* REQ-225 */
    for (size_t i = 0; i < n; i++) append_todo(db, r, arr[i].message_id);
    return r;
}

/* Max search results per response (REQ-080 has no history cutoff; this bounds a
 * single response, and a client can refine the query for more). */
#define OC_SEARCH_MAX 50

/* Turn arbitrary user input into a safe FTS5 MATCH string: each whitespace-
 * separated token becomes a double-quoted phrase (internal quotes doubled per
 * FTS5), so query punctuation can never trip the FTS5 grammar. Tokens are
 * ANDed. A token that is a person's display name (case-insensitive) also
 * matches their id-form mentions, `<@U42>`, which FTS5 indexes as the token
 * `u42` (REQ-221): ("ada" OR "u42"). Returns the token count (0 = nothing to
 * search). */
static int build_fts_query(sqlite3 *db, const char *in, size_t inlen, char *out, size_t outcap) {
    size_t o = 0; int tokens = 0; size_t i = 0;
    sqlite3_stmt *who = NULL;
    if (db) sqlite3_prepare_v2(db,
        "SELECT id FROM users WHERE is_system = 0 AND lower(display_name) = lower(?1) LIMIT 1;",
        -1, &who, NULL);
    while (i < inlen) {
        while (i < inlen && (in[i] == ' ' || in[i] == '\t' || in[i] == '\n' || in[i] == '\r')) i++;
        if (i >= inlen) break;
        size_t s = i;
        while (i < inlen && in[i] != ' ' && in[i] != '\t' && in[i] != '\n' && in[i] != '\r') i++;
        uint64_t uid = 0;
        if (who) {
            sqlite3_reset(who);
            sqlite3_bind_text(who, 1, in + s, (int)(i - s), SQLITE_STATIC);
            if (sqlite3_step(who) == SQLITE_ROW) uid = (uint64_t)sqlite3_column_int64(who, 0);
        }
        char idtok[32] = "";
        if (uid) snprintf(idtok, sizeof idtok, " OR \"u%llu\")", (unsigned long long)uid);
        /* The whole token, its quotes, the joiner and any id part, or none of it. */
        size_t need = (tokens ? 5 : 0) + (uid ? 1 : 0) + 2 + strlen(idtok) + 1;
        for (size_t q = s; q < i; q++) need += in[q] == '"' ? 2 : 1;
        if (o + need >= outcap) break;
        if (tokens) { memcpy(out + o, " AND ", 5); o += 5; }
        if (uid) out[o++] = '(';
        out[o++] = '"';
        for (size_t q = s; q < i; q++) {
            if (in[q] == '"') out[o++] = '"';
            out[o++] = in[q];
        }
        out[o++] = '"';
        memcpy(out + o, idtok, strlen(idtok));
        o += strlen(idtok);
        tokens++;
    }
    sqlite3_finalize(who);
    out[o < outcap ? o : outcap - 1] = '\0';
    return tokens;
}

/* Full-text search over message bodies (REQ-080). Scoped to the channels the
 * user can read (public or a member, REQ-031); tombstones are excluded. Results
 * are FTS5 snippets, bounded by OC_SEARCH_MAX, in the order asked for: newest
 * first, oldest first, or by relevance (bm25, best first, ties by id). Each
 * pages by a keyset (protocol.h, oc_search), never an offset. */
static oc_dbres *process_search(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->type = OC_RES_SEARCH;

    char fts[1024];
    int have_text = (j->body && j->body_len > 0 &&
                     build_fts_query(db, (const char *)j->body, j->body_len, fts, sizeof fts) != 0);
    /* filters alone are a valid search — "everything alice posted in #design"
     * needs no words. Only a query with NEITHER text nor filters is empty. */
    int have_filter = (j->sq_from && j->sq_from[0]) || (j->sq_in && j->sq_in[0]) ||
                      j->sq_has || j->sq_after || j->sq_before;
    if (!have_text && !have_filter) return r;

    uint16_t lim = j->search_limit;
    if (lim == 0 || lim > OC_SEARCH_MAX) lim = OC_SEARCH_MAX;
    /* Nothing ranks a filters-only search: relevance is newest first there. */
    int sort = j->search_sort;
    if (sort != OC_SEARCH_OLDEST && sort != OC_SEARCH_RELEVANCE) sort = OC_SEARCH_NEWEST;
    if (sort == OC_SEARCH_RELEVANCE && !have_text) sort = OC_SEARCH_NEWEST;
    /* The rank as it is compared and sent: an integer, so the cursor a client
     * hands back matches the row it came from exactly. bm25 is lower for a
     * better match. */
    const char *rank_sql = sort == OC_SEARCH_RELEVANCE
        ? "CAST(ROUND(bm25(messages_fts) * 1000000.0) AS INTEGER)" : "0";

    /* Built up rather than one literal, because the FTS join must DISAPPEAR when there
     * is no text: `messages_fts MATCH ''` matches nothing, so a filters-only search
     * would silently return zero rows. */
    char sql[3072];
    int k = snprintf(sql, sizeof sql,
        "SELECT m.id, m.channel_id, m.author_id, m.created_at_ms, %s, "
        "       (SELECT ac.text_start FROM actions ac WHERE ac.message_id=m.id), %s "
        "FROM %s messages m %s "
        "JOIN channels c ON c.id = m.channel_id "
        "WHERE m.deleted_at_ms IS NULL "
        /* A call event (REQ-304) is history, not something anyone said. */
        "  AND m.kind = 0 AND c.e2e = 0 "
        "  AND (c.is_public=1 OR EXISTS(SELECT 1 FROM channel_members cm "
        "       WHERE cm.channel_id=m.channel_id AND cm.user_id=?2)) "
        /* Not what a block withheld from the searcher: search is no way round it. */
        "  AND NOT EXISTS(SELECT 1 FROM withheld_messages wm WHERE wm.user_id=?2 AND wm.message_id=m.id) ",
        /* A snippet is a window on the body that could open or close inside
         * a spoiler (MARKDOWN.md §1d): a body with one is excerpted whole,
         * through the helper, instead. */
        have_text ? "CASE WHEN oc_has_spoiler(m.body) THEN oc_excerpt(m.body,160) "
                    "ELSE snippet(messages_fts, 0, '', '', ' ... ', 12) END"
                  : "oc_excerpt(CASE WHEN (SELECT e2e FROM channels WHERE id=m.channel_id)=1 THEN '' ELSE COALESCE(m.body,'') END,160)",
        rank_sql,
        have_text ? "messages_fts JOIN" : "",
        have_text ? "ON m.id = messages_fts.rowid" : "");
    if (have_text) k += snprintf(sql + k, sizeof sql - (size_t)k, " AND messages_fts MATCH ?1 ");
    /* Keyset cursor, which is stable while people keep posting — an OFFSET
     * would skip or repeat rows as the table grows underneath: the rows after
     * the last one shown, in this order. */
    if (j->message_id) {
        if (sort == OC_SEARCH_NEWEST)
            k += snprintf(sql + k, sizeof sql - (size_t)k, " AND m.id < ?4 ");
        else if (sort == OC_SEARCH_OLDEST)
            k += snprintf(sql + k, sizeof sql - (size_t)k, " AND m.id > ?4 ");
        else
            /* The cursor row's rank as it is NOW: a message posted since the
             * last page moves every bm25 score (the count, the lengths, the
             * IDF), so the rank the client was sent would no longer sit where
             * its row does. Recomputed, both sides are measured alike; the
             * client's copy stands in when the row no longer matches. */
            k += snprintf(sql + k, sizeof sql - (size_t)k,
                          " AND (%s > COALESCE((SELECT CAST(ROUND(bm25(messages_fts) * 1000000.0) AS INTEGER) "
                          "                    FROM messages_fts WHERE messages_fts MATCH ?1 AND rowid = ?4), ?9) "
                          "      OR (%s = COALESCE((SELECT CAST(ROUND(bm25(messages_fts) * 1000000.0) AS INTEGER) "
                          "                    FROM messages_fts WHERE messages_fts MATCH ?1 AND rowid = ?4), ?9) "
                          "          AND m.id > ?4)) ", rank_sql, rank_sql);
    }
    if (j->sq_from && j->sq_from[0])
        k += snprintf(sql + k, sizeof sql - (size_t)k,
                      " AND m.author_id IN (SELECT id FROM users WHERE "
                      "     LOWER(COALESCE(display_name,''))=LOWER(?5) OR "
                      "     LOWER(COALESCE(email,''))=LOWER(?5)) ");
    if (j->sq_in && j->sq_in[0])
        k += snprintf(sql + k, sizeof sql - (size_t)k,
                      " AND LOWER(COALESCE(c.name,''))=LOWER(?6) ");
    if (j->sq_has & 0x01u)   /* file: any attachment */
        k += snprintf(sql + k, sizeof sql - (size_t)k,
                      " AND EXISTS(SELECT 1 FROM attachments a WHERE a.message_id=m.id) ");
    if (j->sq_has & 0x04u)   /* image */
        k += snprintf(sql + k, sizeof sql - (size_t)k,
                      " AND EXISTS(SELECT 1 FROM attachments a WHERE a.message_id=m.id "
                      "            AND a.mime LIKE 'image/%%') ");
    if (j->sq_has & 0x08u)   /* video: by type, so an uploaded .mp4 counts as well as a recorded message */
        k += snprintf(sql + k, sizeof sql - (size_t)k,
                      " AND EXISTS(SELECT 1 FROM attachments a WHERE a.message_id=m.id "
                      "            AND a.mime LIKE 'video/%%') ");
    if (j->sq_has & 0x02u)   /* link: cheap and honest — a substring, not a parser */
        /* CAST, because a message body is stored as a BLOB (sqlite3_bind_blob in
         * process_send: the bytes are UTF-8 and we never let SQLite reinterpret
         * them). LIKE on a BLOB is false for every pattern, so `has:link` matched
         * NOTHING — silently, which is the worst way for a filter to be wrong.
         * Found by fixing the test that was supposed to cover this: it asserted
         * r->n_replay, a field search never fills, so every count was zero and
         * every assertion passed. */
        k += snprintf(sql + k, sizeof sql - (size_t)k,
                      " AND (CAST(m.body AS TEXT) LIKE '%%http://%%' OR "
                      "      CAST(m.body AS TEXT) LIKE '%%https://%%') ");
    if (j->sq_after)  k += snprintf(sql + k, sizeof sql - (size_t)k, " AND m.created_at_ms >= ?7 ");
    if (j->sq_before) k += snprintf(sql + k, sizeof sql - (size_t)k, " AND m.created_at_ms <= ?8 ");
    snprintf(sql + k, sizeof sql - (size_t)k, " ORDER BY %s LIMIT ?3;",
             sort == OC_SEARCH_OLDEST ? "m.id ASC" : sort == OC_SEARCH_RELEVANCE ? "7, m.id ASC" : "m.id DESC");

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        return r;   /* malformed FTS query -> empty results, never a fatal error */
    }
    if (have_text) sqlite3_bind_text(st, 1, fts, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)j->user_id);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)lim);
    if (j->message_id) sqlite3_bind_int64(st, 4, (sqlite3_int64)j->message_id);
    if (j->sq_from && j->sq_from[0]) sqlite3_bind_text(st, 5, j->sq_from, -1, SQLITE_STATIC);
    if (j->sq_in && j->sq_in[0])     sqlite3_bind_text(st, 6, j->sq_in, -1, SQLITE_STATIC);
    if (j->sq_after)  sqlite3_bind_int64(st, 7, (sqlite3_int64)j->sq_after);
    if (j->sq_before) sqlite3_bind_int64(st, 8, (sqlite3_int64)j->sq_before);
    if (j->message_id && sort == OC_SEARCH_RELEVANCE)
        sqlite3_bind_int64(st, 9, (sqlite3_int64)j->search_cursor_rank);

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
        m->rank        = (int64_t)sqlite3_column_int64(st, 6);
        const unsigned char *snip = sqlite3_column_text(st, 4);
        int slen = sqlite3_column_bytes(st, 4);
        /* An action's snippet is cut from its action text (REQ-058). A snippet
         * that starts where the body does still holds the "/me" and its spaces,
         * which are exactly text_start bytes; one cut from further in has none. */
        if (sqlite3_column_type(st, 5) != SQLITE_NULL) {
            int ts = sqlite3_column_int(st, 5);
            m->action = 1;
            if (snip && ts > 3 && slen > ts && memcmp(snip, "/me", 3) == 0) {
                int sp = 3;
                while (sp < ts && snip[sp] == ' ') sp++;
                if (sp == ts) { snip += ts; slen -= ts; }
            }
        }
        if (snip && slen > 0) { m->body = malloc((size_t)slen); if (m->body) { memcpy(m->body, snip, (size_t)slen); m->body_len = (size_t)slen; } }
        n++;
    }
    sqlite3_finalize(st);
    /* Snippets (REQ-226): a title or the first OC_SNIPPET_INDEX_MAX bytes of
     * one find the message carrying it, read under the same gate. They join
     * the first page of a plain text search, newest first, after the message
     * hits and without repeating one; a filtered or later page is the
     * messages' alone, since those filters and cursors are the messages'. */
    if (arr && have_text && !have_filter && !j->message_id && n < lim &&
        sqlite3_prepare_v2(db,
            "SELECT m.id, m.channel_id, m.author_id, m.created_at_ms, md.title "
            "  FROM snippets_fts s JOIN attachments a ON a.id = s.rowid "
            "  JOIN attachment_media md ON md.attachment_id = a.id "
            "  JOIN messages m ON m.id = a.message_id "
            "  JOIN channels c ON c.id = m.channel_id "
            " WHERE snippets_fts MATCH ?1 AND m.deleted_at_ms IS NULL AND a.reclaimed_at_ms = 0 "
            "   AND c.e2e = 0 "
            "   AND (c.is_public=1 OR EXISTS(SELECT 1 FROM channel_members cm "
            "        WHERE cm.channel_id=m.channel_id AND cm.user_id=?2)) "
            "   AND NOT EXISTS(SELECT 1 FROM withheld_messages wm WHERE wm.user_id=?2 AND wm.message_id=m.id) "
            " ORDER BY m.id DESC LIMIT ?3;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, fts, -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)j->user_id);
        sqlite3_bind_int64(st, 3, (sqlite3_int64)lim);
        while (n < lim && sqlite3_step(st) == SQLITE_ROW) {
            uint64_t id = (uint64_t)sqlite3_column_int64(st, 0);
            int dup = 0;
            for (size_t q = 0; q < n && !dup; q++) dup = arr[q].message_id == id;
            if (dup) continue;
            if (n == cap) { cap *= 2; oc_replay_msg *g = realloc(arr, cap * sizeof *arr); if (!g) break; arr = g; }
            oc_replay_msg *m = &arr[n];
            memset(m, 0, sizeof *m);
            m->message_id  = id;
            m->channel_id  = (uint64_t)sqlite3_column_int64(st, 1);
            m->author_id   = (uint64_t)sqlite3_column_int64(st, 2);
            m->server_time = (uint64_t)sqlite3_column_int64(st, 3);
            const char *tt = (const char *)sqlite3_column_text(st, 4);
            char line[OC_SNIPPET_TITLE_MAX + 16];
            int ll = snprintf(line, sizeof line, "Snippet: %s", tt ? tt : "");
            if (ll > 0) {
                m->body = malloc((size_t)ll);
                if (m->body) { memcpy(m->body, line, (size_t)ll); m->body_len = (size_t)ll; }
            }
            n++;
        }
        sqlite3_finalize(st);
    }
    /* A call's transcript (CALLS.md §1): its lines are found by the call's
     * people, joining the first page of a plain text search as snippets do,
     * newest call first. A hit names the call and the line, which its page
     * opens at. */
    if (arr && have_text && !have_filter && !j->message_id && n < lim &&
        sqlite3_prepare_v2(db,
            "SELECT l.id, t.call_id, COALESCE((SELECT ch.id FROM channels ch WHERE ch.call_id=t.call_id),0),"
            "       COALESCE(l.speaker,0), c.started_at_ms + l.start_ms,"
            "       snippet(call_transcripts_fts, 0, '', '', ' ... ', 12)"
            "  FROM call_transcripts_fts f JOIN call_transcript_lines l ON l.id = f.rowid"
            "  JOIN call_transcripts t ON t.id = l.transcript_id"
            "  JOIN calls c ON c.id = t.call_id"
            " WHERE call_transcripts_fts MATCH ?1"
            "   AND EXISTS(SELECT 1 FROM call_invitees i WHERE i.call_id=t.call_id AND i.user_id=?2)"
            " ORDER BY c.started_at_ms DESC, l.start_ms LIMIT ?3;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, fts, -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)j->user_id);
        sqlite3_bind_int64(st, 3, (sqlite3_int64)lim);
        while (n < lim && sqlite3_step(st) == SQLITE_ROW) {
            if (n == cap) { cap *= 2; oc_replay_msg *g = realloc(arr, cap * sizeof *arr); if (!g) break; arr = g; }
            oc_replay_msg *m = &arr[n];
            memset(m, 0, sizeof *m);
            m->call_line   = (uint64_t)sqlite3_column_int64(st, 0);
            m->call_id     = (uint64_t)sqlite3_column_int64(st, 1);
            m->channel_id  = (uint64_t)sqlite3_column_int64(st, 2);
            m->author_id   = (uint64_t)sqlite3_column_int64(st, 3);
            m->server_time = (uint64_t)sqlite3_column_int64(st, 4);
            const void *sn = sqlite3_column_text(st, 5);
            int sl = sqlite3_column_bytes(st, 5);
            if (sn && sl > 0 && (m->body = malloc((size_t)sl)) != NULL) { memcpy(m->body, sn, (size_t)sl); m->body_len = (size_t)sl; }
            n++;
        }
        sqlite3_finalize(st);
    }
    r->search = arr;
    r->n_search = n;
    r->truncated = (n >= lim);   /* filled the requested limit -> maybe more */
    for (size_t i = 0; i < n; i++) if (arr[i].message_id) append_todo(db, r, arr[i].message_id);   /* each list as it stands */
    return r;
}

/* Record a client's cumulative delivery cursor (REQ-090). CLIENT_ACK(N) means
 * everything <= N in the channel has been received; the stored cursor only ever
 * advances. When it does advance, answer with a READ_CURSOR result so the net
 * thread can drive seen-by: fan the acker's new cursor to the channel members and
 * backfill the acker with the other members' current cursors. A stale/duplicate
 * ack (no advance) has no reply. */
/* One cursor's fan-out, exactly as process_client_ack builds it: who to tell,
 * and what everybody else's cursor is. Split out so the bulk sweep below and
 * the single ack cannot drift into describing the same event two ways. */
static oc_dbres *read_cursor_result(sqlite3 *db, uint64_t conn_id, uint64_t user_id,
                                    uint64_t channel_id, uint64_t message_id) {
    sqlite3_stmt *st = NULL;
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->type = OC_RES_READ_CURSOR;
    r->conn_id = conn_id;
    r->channel_id = channel_id;
    r->user_id = user_id;
    r->message_id = message_id;
    load_members(db, channel_id, r);
    /* Read receipts (REQ-090): with the workspace's off, nobody's position
     * goes anywhere; with this reader's own off, theirs does not, and they
     * still see the others'. */
    int ws_on = oc_config_get()->read_receipts == 0;
    r->rc_share = 0;
    if (ws_on) {
        sqlite3_prepare_v2(db, "SELECT share_receipts FROM users WHERE id=?1;", -1, &st, NULL);
        sqlite3_bind_int64(st, 1, (sqlite3_int64)user_id);
        r->rc_share = sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 1;
        sqlite3_finalize(st);
    }
    if (!ws_on) return r;

    size_t cap = 8, n = 0;
    r->rcur = malloc(cap * sizeof *r->rcur);
    /* The others who share, and only a read made since they did: one from
     * before (receipts_since_ms) is never shown. */
    sqlite3_prepare_v2(db,
        "SELECT dc.user_id, dc.message_id FROM delivery_cursors dc JOIN users u ON u.id = dc.user_id "
        " WHERE dc.channel_id=? AND dc.user_id<>? AND u.share_receipts = 1 "
        "   AND u.receipts_since_ms >= 0 AND dc.updated_at_ms > u.receipts_since_ms;",
        -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)channel_id);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)user_id);
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

/* Whether I share my read receipts (REQ-090), set or asked. Turning it on
 * starts from now: what I read before is never shown. */
static oc_dbres *process_read_receipts(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->type = OC_RES_READ_RECEIPTS;
    r->conn_id = j->conn_id;
    r->user_id = j->user_id;
    sqlite3_stmt *st = NULL;
    if (j->receipts_op != OC_RECEIPTS_ASK) {
        sqlite3_prepare_v2(db,
            "UPDATE users SET share_receipts=?2, "
            "  receipts_since_ms = CASE WHEN ?2 = 1 AND share_receipts = 0 AND receipts_since_ms >= 0 "
            "                           THEN ?3 ELSE receipts_since_ms END WHERE id=?1;", -1, &st, NULL);
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->user_id);
        sqlite3_bind_int(st, 2, j->receipts_op ? 1 : 0);
        sqlite3_bind_int64(st, 3, (sqlite3_int64)dbw_now_ms());
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    r->rc_asked = j->receipts_op == OC_RECEIPTS_ASK;
    sqlite3_prepare_v2(db, "SELECT share_receipts FROM users WHERE id=?1;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->user_id);
    r->rc_share = sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 1;
    sqlite3_finalize(st);
    return r;
}

/* The workspace's switch, as this daemon starts (OPENCHIME_READ_RECEIPTS):
 * off marks everyone (-1) so that nothing read meanwhile is ever shown; on
 * again starts the marked from now. */
static void receipts_start(sqlite3 *db) {
    sqlite3_stmt *st = NULL;
    if (oc_config_get()->read_receipts != 0) {
        sqlite3_exec(db, "UPDATE users SET receipts_since_ms = -1;", NULL, NULL, NULL);
        return;
    }
    sqlite3_prepare_v2(db, "UPDATE users SET receipts_since_ms = ?1 WHERE receipts_since_ms < 0;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)dbw_now_ms());
    sqlite3_step(st);
    sqlite3_finalize(st);
}

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

    return read_cursor_result(db, j->conn_id, j->user_id, j->channel_id, j->message_id);
}

/* Catch-up (OC_MSG_MARK_ALL_READ, REQ-238): advance every one of this user's memberships
 * to that channel's newest message, in one transaction.
 *
 * The target is read from the DATABASE, not from the client: "everything" means
 * everything the daemon holds when the request arrives, so a message that landed
 * while the frame was in flight is included rather than left as a single stubborn
 * unread the user already tried to clear.
 *
 * Only channels that actually MOVE produce a result, for the reason the single
 * ack has: a replayed catch-up must not fan a cursor nobody advanced. The
 * results are chained and the whole chain is queued at once (push_result), so
 * sixty channels are one job and one wake rather than sixty of each. */
static oc_dbres *process_mark_all_read(sqlite3 *db, const oc_job *j) {
    sqlite3_stmt *st = NULL;
    oc_dbres *head = NULL, *tail = NULL;
    struct { uint64_t channel_id, message_id; } *adv = NULL;
    size_t cap = 0, n = 0;

    /* Every membership whose newest message is past this user's cursor, with the
     * id to advance to. One query: the loop that follows must not ask the
     * database a question per channel, which is the cost this op exists to
     * remove. A channel with no messages has no MAX and is skipped by the
     * comparison, not by a special case. */
    sqlite3_prepare_v2(db,
        "SELECT cm.channel_id, MAX(m.id) AS newest "
        "FROM channel_members cm "
        "JOIN messages m ON m.channel_id = cm.channel_id "
        "LEFT JOIN delivery_cursors dc "
        "  ON dc.channel_id = cm.channel_id AND dc.user_id = cm.user_id "
        "WHERE cm.user_id = ? "
        "GROUP BY cm.channel_id "
        "HAVING newest > COALESCE(dc.message_id, 0);", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->user_id);
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (n == cap) {
            size_t nc = cap ? cap * 2 : 16;
            void *g = realloc(adv, nc * sizeof *adv);
            if (!g) break;
            adv = g; cap = nc;
        }
        adv[n].channel_id = (uint64_t)sqlite3_column_int64(st, 0);
        adv[n].message_id = (uint64_t)sqlite3_column_int64(st, 1);
        n++;
    }
    sqlite3_finalize(st);
    if (!n) { free(adv); return NULL; }

    uint64_t now = dbw_now_ms();
    sqlite3_prepare_v2(db,
        "INSERT INTO delivery_cursors(user_id,channel_id,message_id,updated_at_ms) "
        "VALUES(?,?,?,?) ON CONFLICT(user_id,channel_id) DO UPDATE SET "
        "message_id=MAX(message_id,excluded.message_id), updated_at_ms=excluded.updated_at_ms;",
        -1, &st, NULL);
    for (size_t i = 0; i < n; i++) {
        sqlite3_reset(st);
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->user_id);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)adv[i].channel_id);
        sqlite3_bind_int64(st, 3, (sqlite3_int64)adv[i].message_id);
        sqlite3_bind_int64(st, 4, (sqlite3_int64)now);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);

    for (size_t i = 0; i < n; i++) {
        oc_dbres *r = read_cursor_result(db, j->conn_id, j->user_id,
                                         adv[i].channel_id, adv[i].message_id);
        if (!r) continue;
        if (tail) tail->next = r; else head = r;
        tail = r;
    }
    free(adv);
    return head;
}

/* Replay messages newer than each cursor, for channels the user belongs to,
 * bounded by OC_BACKFILL_MAX total (REQ-101, PROTOCOL.md §6). A request with no
 * cursors (count=0) sends every member channel its newest page, as a cursor of
 * 0 would; a non-empty list covers only the channels it names. */
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

    /* Effective cursors: the client's explicit list (channels left out of it
     * are skipped), or — when it sent none — every member channel with a
     * cursor of 0, which the loop below turns into its newest page. */
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
                    "  (SELECT id FROM messages WHERE channel_id=? AND (parent_id IS NULL OR also_channel=1) "
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
            "  COALESCE(NULLIF(m.author_name,''), u.display_name, ''), "
            /* Pin state travels with the message: a BROADCAST has no field for
             * it, so a reconnecting client would otherwise lose every pin. */
            "  COALESCE(p.pinned_by,0), COALESCE(p.created_at_ms,0), "
            /* And this user's saved-for-later state, for the same reason. LEFT
             * JOIN rather than EXISTS so the timestamp comes with it, and keyed on
             * the REQUESTING user because a saved item is private (REQ-231). */
            "  COALESCE(s.created_at_ms,0), m.kind, "
            /* And whether this user folded it away, for the same reason. */
            "  EXISTS(SELECT 1 FROM collapsed_messages cm WHERE cm.message_id = m.id AND cm.user_id = ?4), "
            /* A reply also posted to the channel is here with its thread (REQ-062). */
            "  COALESCE(m.parent_id,0) "
            "FROM messages m LEFT JOIN users u ON u.id = m.author_id "
            "                LEFT JOIN pins  p ON p.message_id = m.id "
            "                LEFT JOIN saved_items s ON s.message_id = m.id AND s.user_id = ?4 "
            /* Every marker numbered explicitly. An anonymous `?` takes the next
             * unused index, so introducing ?4 ABOVE these silently renumbered them
             * to 5, 6 and 7 — the binds still succeeded, the query saw NULLs, and
             * backfill returned nothing. 36 tests caught it; reading would not
             * have. */
            "WHERE m.channel_id=?1 AND m.id>?2 AND (m.parent_id IS NULL OR m.also_channel=1) "
            /* Not what was withheld from this reader by a block, ever. */
            "  AND NOT EXISTS(SELECT 1 FROM withheld_messages wm WHERE wm.user_id=?4 AND wm.message_id=m.id) "
            "ORDER BY m.id LIMIT ?3;", -1, &st, NULL);
        sqlite3_bind_int64(st, 4, (sqlite3_int64)j->user_id);
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
            m->pinned_by = (uint64_t)sqlite3_column_int64(st, 7);
            m->pinned_at = (uint64_t)sqlite3_column_int64(st, 8);
            m->saved_at  = (uint64_t)sqlite3_column_int64(st, 9);
            m->saved     = m->saved_at != 0;
            m->kind      = (uint8_t)sqlite3_column_int(st, 10);
            m->via       = message_via(db, m->message_id);
            m->broadcast_stored = broadcast_stored(db, m->message_id);
            m->collapsed = (uint8_t)sqlite3_column_int(st, 11);
            m->parent_id = (uint64_t)sqlite3_column_int64(st, 12);
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

    /* Reactions and unfurls for the window just built. Both are shared with the
     * history page (§6.3), so the two replay paths cannot drift. */
    fill_replay_repliers(db, r);
    fill_replay_reactions(db, r, j->user_id);
    fill_replay_unfurls(db, r);
    fill_replay_forwards(db, r);
    fill_replay_actions(db, r);
    fill_replay_polls(db, r, j->user_id);   /* REQ-225 */
    fill_replay_reminders(db, r, j->user_id);   /* REQ-283 */
    fill_replay_todos(db, r);
    fill_replay_calls(db, r);
    return r;
}

/* Page backwards through one channel (§6.3): the newest `search_limit`
 * top-level messages strictly older than `message_id`. Answers with the same
 * BACKFILL_OK shape as the forward replay, so the net thread needs no new
 * emit path and the client folds the rows in the same way.
 *
 * `truncated` here means "there is more above this page", which is what lets a
 * client stop asking at the top of the channel instead of retrying forever. */
/* Jump to a date: the first message at or after `find_at_ms` in a conversation's
 * main scroll, or in one thread of it; the last before it when none is later
 * (found_exact 0); none at all, message_id 0. Read under the gate history uses,
 * so it finds nothing a reader could not scroll to. */
static oc_dbres *process_find_at_time(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    uint8_t pub = 0;
    if (!channel_exists(db, j->channel_id, &pub)) {
        r->type = OC_RES_LIST_ERR; r->err_code = OC_ERR_UNKNOWN_CHANNEL; return r;
    }
    if (!channel_read_access(db, j->channel_id, j->user_id)) {
        r->type = OC_RES_LIST_ERR; r->err_code = OC_ERR_NOT_A_MEMBER; return r;
    }
    r->type = OC_RES_FOUND_AT_TIME;
    r->channel_id = j->channel_id;
    r->parent_id = j->parent_id;
    /* A thread is its root and its replies: the root is where it begins. */
    static const char *const q[2][2] = {
        { "SELECT id, created_at_ms FROM messages WHERE channel_id=?1 AND parent_id IS NULL "
          "AND created_at_ms>=?2 ORDER BY created_at_ms, id LIMIT 1;",
          "SELECT id, created_at_ms FROM messages WHERE channel_id=?1 AND parent_id IS NULL "
          "AND created_at_ms<?2 ORDER BY created_at_ms DESC, id DESC LIMIT 1;" },
        { "SELECT id, created_at_ms FROM messages WHERE channel_id=?1 AND (id=?3 OR parent_id=?3) "
          "AND created_at_ms>=?2 ORDER BY created_at_ms, id LIMIT 1;",
          "SELECT id, created_at_ms FROM messages WHERE channel_id=?1 AND (id=?3 OR parent_id=?3) "
          "AND created_at_ms<?2 ORDER BY created_at_ms DESC, id DESC LIMIT 1;" },
    };
    int th = j->parent_id != 0;
    for (int k = 0; k < 2 && !r->message_id; k++) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(db, q[th][k], -1, &st, NULL) != SQLITE_OK) break;
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->channel_id);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)j->find_at_ms);
        if (th) sqlite3_bind_int64(st, 3, (sqlite3_int64)j->parent_id);
        if (sqlite3_step(st) == SQLITE_ROW) {
            r->message_id  = (uint64_t)sqlite3_column_int64(st, 0);
            r->server_time = (uint64_t)sqlite3_column_int64(st, 1);
            r->found_exact = (uint8_t)(k == 0);
        }
        sqlite3_finalize(st);
    }
    return r;
}

static oc_dbres *process_history(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->type = OC_RES_BACKFILL_OK;
    r->channel_id = j->channel_id;
    /* A link names the channel a message was in when it was copied; a moved
     * message (REQ-053) is read where it is now, for a reader who may. */
    uint64_t chan = j->channel_id;
    if (j->hist_around && j->message_id) {
        sqlite3_stmt *ls = NULL;
        if (sqlite3_prepare_v2(db, "SELECT channel_id FROM messages WHERE id=?1;", -1, &ls, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(ls, 1, (sqlite3_int64)j->message_id);
            if (sqlite3_step(ls) == SQLITE_ROW) chan = (uint64_t)sqlite3_column_int64(ls, 0);
            sqlite3_finalize(ls);
        }
        if (chan != j->channel_id && channel_read_access(db, chan, j->user_id)) r->channel_id = chan;
        else chan = j->channel_id;
    }
    if (!channel_read_access(db, chan, j->user_id)) return r;

    uint16_t lim = j->search_limit;
    if (lim == 0 || lim > OC_BACKFILL_TAIL) lim = OC_BACKFILL_TAIL;
    uint64_t before = j->message_id ? j->message_id : (uint64_t)INT64_MAX;
    /* `around` splits the budget either side of the target, so ?3 is half. */
    if (j->hist_around) { before = j->message_id; if (lim > 1) lim = (uint16_t)(lim / 2); }

    sqlite3_stmt *st = NULL;
    /* Two modes, one replay (ARCH-96). Both select the same columns and both end
     * ascending, which is the order a replay must arrive in for the client's
     * high-water dedup to behave — only the window differs:
     *
     *   page   — the newest `limit` messages OLDER than `before` (§6.3).
     *   around — half either side of an id, so a permalink lands mid-screen
     *            with context rather than pinned to an edge (REQ-232).
     *
     * The projection is spelled once and shared, because every field in it —
     * reply counts, the webhook author-name override, attachments loaded below —
     * has been a bug in the replay path at least once, and a second copy is a
     * second place to forget one. */
#define OC_HIST_COLS \
    "  SELECT m.id AS id, m.author_id AS author_id, m.created_at_ms AS created_at_ms," \
    "         m.body AS body," \
    "         (SELECT COUNT(*) FROM messages c WHERE c.parent_id=m.id) AS reply_count," \
    "         (SELECT COALESCE(MAX(c.created_at_ms),0) FROM messages c WHERE c.parent_id=m.id) AS last_reply," \
    "         COALESCE(NULLIF(m.author_name,''), u.display_name, '') AS author_name," \
    /* Pin and saved state, exactly as the reconnect replay carries them: a
     * BROADCAST has no field for either, so a client that keeps nothing
     * (ARCH-88) loses both on any reload that is not a reconnect — which is
     * every scroll-back and every permalink. `saved` is keyed on the REQUESTING
     * user because it is private (REQ-231), where a pin belongs to the channel. */ \
    "         COALESCE(p.pinned_by,0) AS pinned_by," \
    "         COALESCE(p.created_at_ms,0) AS pinned_at," \
    "         COALESCE(s.created_at_ms,0) AS saved_at," \
    "         m.kind AS kind," \
    "         EXISTS(SELECT 1 FROM collapsed_messages cm WHERE cm.message_id = m.id AND cm.user_id = ?4) AS collapsed," \
    "         COALESCE(m.parent_id,0) AS parent_id" \
    "    FROM messages m LEFT JOIN users u ON u.id = m.author_id" \
    "                    LEFT JOIN pins p ON p.message_id = m.id" \
    "                    LEFT JOIN saved_items s ON s.message_id = m.id AND s.user_id = ?4"
    /* What a block withheld from this reader stays out of every page of it. */
#define OC_HIST_NOT_WITHHELD \
    " AND NOT EXISTS(SELECT 1 FROM withheld_messages wm WHERE wm.user_id = ?4 AND wm.message_id = m.id)"

    const char *sql = j->hist_around
        ? "SELECT id, author_id, created_at_ms, body, reply_count, last_reply, author_name,"
          "       pinned_by, pinned_at, saved_at, kind, collapsed, parent_id FROM ("
          "  SELECT * FROM (" OC_HIST_COLS
          "   WHERE m.channel_id=?1 AND (m.parent_id IS NULL OR m.also_channel=1) AND m.id<=?2" OC_HIST_NOT_WITHHELD
          "   ORDER BY m.id DESC LIMIT ?3)"
          "  UNION ALL"
          "  SELECT * FROM (" OC_HIST_COLS
          "   WHERE m.channel_id=?1 AND (m.parent_id IS NULL OR m.also_channel=1) AND m.id>?2" OC_HIST_NOT_WITHHELD
          "   ORDER BY m.id ASC LIMIT ?3)"
          ") ORDER BY id;"
        : "SELECT id, author_id, created_at_ms, body, reply_count, last_reply, author_name,"
          "       pinned_by, pinned_at, saved_at, kind, collapsed, parent_id FROM ("
          OC_HIST_COLS
          "   WHERE m.channel_id=?1 AND m.id<?2 AND (m.parent_id IS NULL OR m.also_channel=1)" OC_HIST_NOT_WITHHELD
          "   ORDER BY m.id DESC LIMIT ?3"
          ") ORDER BY id;";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return r;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)chan);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)before);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)lim);
    /* Numbered explicitly, like the backfill query: an anonymous marker takes
     * the next unused index, so adding one inside OC_HIST_COLS would silently
     * renumber the three above and the query would see NULLs. */
    sqlite3_bind_int64(st, 4, (sqlite3_int64)j->user_id);

    size_t cap = 16, n = 0;
    oc_replay_msg *arr = malloc(cap * sizeof *arr);
    while (arr && sqlite3_step(st) == SQLITE_ROW) {
        if (n == cap) {
            cap *= 2;
            oc_replay_msg *g = realloc(arr, cap * sizeof *arr);
            if (!g) break;
            arr = g;
        }
        oc_replay_msg *m = &arr[n];
        memset(m, 0, sizeof *m);
        m->message_id  = (uint64_t)sqlite3_column_int64(st, 0);
        m->channel_id  = chan;
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
        if (an && an[0]) m->author_name = strdup(an);
        m->pinned_by = (uint64_t)sqlite3_column_int64(st, 7);
        m->pinned_at = (uint64_t)sqlite3_column_int64(st, 8);
        m->saved_at  = (uint64_t)sqlite3_column_int64(st, 9);
        m->saved     = m->saved_at != 0;
        m->kind      = (uint8_t)sqlite3_column_int(st, 10);
        m->via       = message_via(db, m->message_id);
        m->broadcast_stored = broadcast_stored(db, m->message_id);
        m->collapsed = (uint8_t)sqlite3_column_int(st, 11);
        m->parent_id = (uint64_t)sqlite3_column_int64(st, 12);
        load_message_attachments(db, m->message_id, m->attach, &m->n_attach);
        n++;
    }
    sqlite3_finalize(st);
    r->replay = arr;
    r->n_replay = n;

    /* Anything older still? Answered from the oldest row we just took. */
    if (n > 0) {
        sqlite3_stmt *mt = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT EXISTS(SELECT 1 FROM messages WHERE channel_id=?1 AND id<?2 "
                "AND (parent_id IS NULL OR also_channel=1));", -1, &mt, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(mt, 1, (sqlite3_int64)chan);
            sqlite3_bind_int64(mt, 2, (sqlite3_int64)arr[0].message_id);
            if (sqlite3_step(mt) == SQLITE_ROW)
                r->truncated = (uint8_t)sqlite3_column_int(mt, 0);
            sqlite3_finalize(mt);
        }
    }
    /* The same two fills the reconnect replay runs. Reactions were absent here
     * entirely, which is what made a scroll-back or a permalink render messages
     * permanently without them. */
    fill_replay_repliers(db, r);
    fill_replay_reactions(db, r, j->user_id);
    fill_replay_unfurls(db, r);
    fill_replay_forwards(db, r);
    fill_replay_actions(db, r);
    fill_replay_polls(db, r, j->user_id);   /* REQ-225 */
    fill_replay_reminders(db, r, j->user_id);   /* REQ-283 */
    fill_replay_todos(db, r);
    fill_replay_calls(db, r);
    return r;
}

/* --- Queue plumbing ----------------------------------------------------- */

static void push_result(oc_dbwriter *w, oc_dbres *r) {
    if (!r) return;
    /* A CHAIN, not a result: one job may answer with many (catch-up advances
     * every channel at once). This used to clear `next` and take the head,
     * which silently dropped the rest -- fine while every processor returned
     * exactly one, and a leak plus a lost fan-out the moment one did not. */
    oc_dbres *last = r;
    while (last->next) last = last->next;
    pthread_mutex_lock(&w->mu);
    if (w->res_tail) w->res_tail->next = r; else w->res_head = r;
    w->res_tail = last;
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
    r->parent_id = j->parent_id; /* ...and in which thread, 0 the main scroll (REQ-121) */
    if (channel_read_access(db, j->channel_id, j->user_id)) {
        load_members(db, j->channel_id, r);
        /* A visitor is told nobody is typing (SCHEMA.md §3bj): the relay lists
         * no visitor, so the team's typing stays the team's. */
        size_t k = 0;
        for (size_t i = 0; i < r->n_members; i++)
            if (!user_is_visitor(db, r->members[i])) r->members[k++] = r->members[i];
        r->n_members = k;
    }
    return r;
}

/* An account that exists and is not disabled: someone a call can invite. */
static int user_is_active(sqlite3 *db, uint64_t user_id) {
    sqlite3_stmt *st = NULL;
    int ok = 0;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM users WHERE id=? AND disabled=0;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)user_id);
        ok = sqlite3_step(st) == SQLITE_ROW;
    }
    sqlite3_finalize(st);
    return ok;
}

/* Who of the people named may be invited to a call (CALLS.md §1): an account
 * that is on and is not a visitor. Whether the asker is in the call, and the
 * cap, are the net thread's, which holds the call. Read (query connection). */
static oc_dbres *process_call_auth(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->user_id = j->user_id;
    r->call_op = j->call_op;
    r->call_id = j->call_id;
    r->type = OC_RES_CALL_AUTH;
    if (j->n_call_uids) {
        r->call_uids = malloc(j->n_call_uids * sizeof *r->call_uids);
        for (uint16_t i = 0; r->call_uids && i < j->n_call_uids; i++) {
            uint64_t u = j->call_uids[i];
            if (u && u != j->user_id && user_is_active(db, u) && !user_is_visitor(db, u))
                r->call_uids[r->n_call_uids++] = u;
        }
    }
    return r;
}

/* --- A call's record (CALLS.md §1, SCHEMA.md `calls`) --------------------- */

/* One statement with up to four integer binds and an optional text one, run
 * to its end; 0 when it did. */
static int call_exec(sqlite3 *db, const char *sql, int n, const sqlite3_int64 *v,
                     int text_at, const char *text) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) { sqlite3_finalize(st); return -1; }
    for (int i = 0; i < n; i++) sqlite3_bind_int64(st, i + 1, v[i]);
    if (text_at) sqlite3_bind_text(st, text_at, text ? text : "", -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

/* What a call's message in its conversation says (CALLS.md §1), so it reads
 * sensibly anywhere -- in a client that draws it as the call, and in one that
 * shows only the body: "alice started a call: Q3 pricing review", and once it
 * is over, how long it lasted. Heap, or NULL. */
static char *call_message_text(sqlite3 *db, uint64_t call) {
    sqlite3_stmt *st = NULL;
    char *out = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT c.subject, c.started_at_ms, c.ended_at_ms, COALESCE(NULLIF(u.display_name,''), 'Someone')"
            "  FROM calls c LEFT JOIN users u ON u.id = c.starter WHERE c.id = ?1;", -1, &st, NULL) != SQLITE_OK) {
        sqlite3_finalize(st);
        return NULL;
    }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)call);
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *subject = (const char *)sqlite3_column_text(st, 0);
        const char *who = (const char *)sqlite3_column_text(st, 3);
        char len[48] = "";
        if (sqlite3_column_type(st, 2) != SQLITE_NULL) {
            uint64_t ms = (uint64_t)(sqlite3_column_int64(st, 2) - sqlite3_column_int64(st, 1));
            uint64_t min = ms / 60000;
            if (min == 0)      snprintf(len, sizeof len, " (ended after less than a minute)");
            else if (min == 1) snprintf(len, sizeof len, " (ended after 1 minute)");
            else               snprintf(len, sizeof len, " (ended after %llu minutes)", (unsigned long long)min);
        }
        size_t n = strlen(who ? who : "") + strlen(subject ? subject : "") + strlen(len) + 32;
        if ((out = malloc(n)) != NULL)
            snprintf(out, n, "%s started a call: %s%s", who ? who : "", subject ? subject : "", len);
    }
    sqlite3_finalize(st);
    return out;
}

/* The call's message, brought up to date with the call (a rename, the end):
 * its body rewritten in place -- it is the daemon's, so this is not an edit
 * anyone made -- and answered as OC_RES_CALL_EVENT for the conversation's
 * members, or NULL when the call left no message. */
static oc_dbres *call_message_refresh(sqlite3 *db, uint64_t call) {
    sqlite3_stmt *st = NULL;
    uint64_t mid = 0, ch = 0, author = 0, at = 0;
    if (sqlite3_prepare_v2(db, "SELECT id, channel_id, author_id, created_at_ms FROM messages "
                               "WHERE call_id = ?1 AND kind = 1 AND deleted_at_ms IS NULL LIMIT 1;",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)call);
        if (sqlite3_step(st) == SQLITE_ROW) {
            mid = (uint64_t)sqlite3_column_int64(st, 0);
            ch = (uint64_t)sqlite3_column_int64(st, 1);
            author = (uint64_t)sqlite3_column_int64(st, 2);
            at = (uint64_t)sqlite3_column_int64(st, 3);
        }
    }
    sqlite3_finalize(st);
    if (!mid) return NULL;
    char *text = call_message_text(db, call);
    if (!text) return NULL;
    sqlite3_int64 v[1] = { (sqlite3_int64)mid };
    if (call_exec(db, "UPDATE messages SET body = ?2 WHERE id = ?1;", 1, v, 2, text) != 0) { free(text); return NULL; }
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) { free(text); return NULL; }
    r->type = OC_RES_CALL_EVENT;
    r->message_id = mid;
    r->channel_id = ch;
    r->author_id = author;
    r->server_time = at;
    r->call_id = call;
    r->body = (uint8_t *)text;
    r->body_len = strlen(text);
    /* Not an action, said as every live message frame says it (ARCH-115). */
    append_action(db, r, mid, ch, 1);
    load_members(db, ch, r);
    return r;
}

/* Invite `uid` to `call`, asked by `by`, unless already asked. */
static void call_row_invite(sqlite3 *db, uint64_t call, uint64_t uid, uint64_t by, uint64_t ts) {
    sqlite3_int64 v[4] = { (sqlite3_int64)call, (sqlite3_int64)uid, (sqlite3_int64)by, (sqlite3_int64)ts };
    call_exec(db, "INSERT OR IGNORE INTO call_invitees(call_id, user_id, invited_by, invited_at_ms) "
                  "VALUES(?1, ?2, ?3, ?4);", 4, v, 0, NULL);
}

/* A call begins (CALLS.md §1): its row, and its invitees -- the starter first,
 * then each person named who has an account that is on, is not a visitor and
 * is not the starter. A call started from a conversation needs the starter to
 * be able to read it; who else is in it does not matter, since the call is not
 * the conversation's. The subject is checked by the net thread as it is here:
 * one that is blank or too long is refused. Write. */
static oc_dbres *process_call_start(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->user_id = j->user_id;
    r->channel_id = j->channel_id;
    r->gen = j->gen;
    memcpy(r->call_key, j->call_key, sizeof r->call_key);
    r->call_codecs = j->call_codecs;
    r->type = OC_RES_CALL_ERR;
    size_t sl = j->call_subject ? strlen(j->call_subject) : 0;
    int blank = 1;
    for (size_t i = 0; i < sl; i++) if (!isspace((unsigned char)j->call_subject[i])) blank = 0;
    if (blank || sl > OC_CALL_SUBJECT_MAX) { r->err_code = OC_ERR_CALL_SUBJECT; return r; }
    if (j->channel_id && !channel_read_access(db, j->channel_id, j->user_id)) {
        r->err_code = OC_ERR_NOT_A_MEMBER;
        return r;
    }
    r->err_code = OC_ERR_INTERNAL;
    uint64_t ts = dbw_now_ms();
    if (sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK) return r;
    sqlite3_stmt *st = NULL;
    int ok = sqlite3_prepare_v2(db,
        "INSERT INTO calls(subject, starter, from_channel, started_at_ms) VALUES(?1, ?2, ?3, ?4);",
        -1, &st, NULL) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_text (st, 1, j->call_subject, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)j->user_id);
        if (j->channel_id) sqlite3_bind_int64(st, 3, (sqlite3_int64)j->channel_id);
        else               sqlite3_bind_null (st, 3);
        sqlite3_bind_int64(st, 4, (sqlite3_int64)ts);
        ok = sqlite3_step(st) == SQLITE_DONE;
    }
    sqlite3_finalize(st);
    uint64_t call = ok ? (uint64_t)sqlite3_last_insert_rowid(db) : 0;
    /* The call's chat: a conversation of its own, a DM in kind, marked with
     * its call, whose members are the call's invitees (SCHEMA.md §3bz). */
    uint64_t chat = 0;
    if (ok) {
        sqlite3_stmt *cs = NULL;
        ok = sqlite3_prepare_v2(db, "INSERT INTO channels(kind, is_public, created_at_ms, call_id) VALUES('dm', 0, ?1, ?2);",
                                -1, &cs, NULL) == SQLITE_OK;
        if (ok) {
            sqlite3_bind_int64(cs, 1, (sqlite3_int64)ts);
            sqlite3_bind_int64(cs, 2, (sqlite3_int64)call);
            ok = sqlite3_step(cs) == SQLITE_DONE;
        }
        sqlite3_finalize(cs);
        if (ok) chat = (uint64_t)sqlite3_last_insert_rowid(db);
    }
    if (ok) {
        call_row_invite(db, call, j->user_id, j->user_id, ts);
        membership_sync(db, chat, j->user_id, 1);
        if (j->n_call_uids) r->call_uids = malloc(j->n_call_uids * sizeof *r->call_uids);
        for (uint16_t i = 0; r->call_uids && i < j->n_call_uids; i++) {
            uint64_t u = j->call_uids[i];
            int dup = 0;
            for (size_t k = 0; k < r->n_call_uids; k++) if (r->call_uids[k] == u) dup = 1;
            if (dup || !u || u == j->user_id || !user_is_active(db, u) || user_is_visitor(db, u)) continue;
            call_row_invite(db, call, u, j->user_id, ts);
            membership_sync(db, chat, u, 1);
            r->call_uids[r->n_call_uids++] = u;
        }
    }
    /* Started from a conversation: one message there about it, which is all
     * the conversation gets. It notifies nobody -- the invitation does that,
     * and only for those asked. */
    char *text = ok && j->channel_id ? call_message_text(db, call) : NULL;
    if (ok && j->channel_id) {
        st = NULL;
        ok = text && sqlite3_prepare_v2(db,
            "INSERT INTO messages(channel_id, author_id, body, created_at_ms, kind, call_id) VALUES(?1, ?2, ?3, ?4, ?5, ?6);",
            -1, &st, NULL) == SQLITE_OK;
        if (ok) {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)j->channel_id);
            sqlite3_bind_int64(st, 2, (sqlite3_int64)j->user_id);
            sqlite3_bind_text (st, 3, text, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(st, 4, (sqlite3_int64)ts);
            sqlite3_bind_int  (st, 5, (int)OC_MSG_KIND_CALL);
            sqlite3_bind_int64(st, 6, (sqlite3_int64)call);
            ok = sqlite3_step(st) == SQLITE_DONE;
        }
        sqlite3_finalize(st);
        if (ok) r->message_id = (uint64_t)sqlite3_last_insert_rowid(db);
    }
    if (!ok || sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        free(r->call_uids); r->call_uids = NULL; r->n_call_uids = 0;
        free(text);
        r->message_id = 0;
        return r;
    }
    r->type = OC_RES_CALL_STARTED;
    r->err_code = 0;
    r->call_id = call;
    r->call_chat = chat;
    r->call_subject = strdup(j->call_subject);
    r->server_time = ts;
    if (r->message_id) {
        r->author_id = j->user_id;
        r->body = (uint8_t *)text;
        r->body_len = strlen(text);
        text = NULL;
        /* A call message is never an action, and says so like every live
         * message frame does (ARCH-115). */
        append_action(db, r, r->message_id, j->channel_id, 1);
        load_members(db, j->channel_id, r);
    }
    free(text);
    return r;
}

static const char *const CALL_EVENT_KINDS[] = {
    NULL, "camera_on", "camera_off", "share_on", "share_off", "record_on", "record_off", "subject"
};

/* One change to a call's record, as the net thread makes it: a stay opened
 * or closed, people asked, an invitation declined, an event on the timeline,
 * a new subject, the end. Write, fire and forget -- except that a new subject
 * or the end changes the message the call left in its conversation, which is
 * answered for its members (OC_RES_CALL_EVENT). */
static oc_dbres *process_call_log(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = 0;
    r->type = OC_RES_OK;
    uint64_t ts = dbw_now_ms();
    sqlite3_int64 v[4] = { (sqlite3_int64)j->call_id, (sqlite3_int64)j->user_id, (sqlite3_int64)ts, 0 };
    switch (j->call_log) {
    case OC_CALL_LOG_JOIN:
        call_exec(db, "INSERT INTO call_participants(call_id, user_id, joined_at_ms) VALUES(?1, ?2, ?3);",
                  3, v, 0, NULL);
        break;
    case OC_CALL_LOG_LEAVE:
        call_exec(db, "UPDATE call_participants SET left_at_ms=?3 "
                      "WHERE call_id=?1 AND user_id=?2 AND left_at_ms IS NULL;", 3, v, 0, NULL);
        break;
    case OC_CALL_LOG_INVITE: {
        /* Asked to the call, and so a member of its chat. */
        uint64_t chat = 0;
        sqlite3_stmt *cs = NULL;
        if (sqlite3_prepare_v2(db, "SELECT id FROM channels WHERE call_id=?1;", -1, &cs, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(cs, 1, (sqlite3_int64)j->call_id);
            if (sqlite3_step(cs) == SQLITE_ROW) chat = (uint64_t)sqlite3_column_int64(cs, 0);
        }
        sqlite3_finalize(cs);
        for (uint16_t i = 0; i < j->n_call_uids; i++) {
            call_row_invite(db, j->call_id, j->call_uids[i], j->user_id, ts);
            if (chat) membership_sync(db, chat, j->call_uids[i], 1);
        }
        break;
    }
    case OC_CALL_LOG_DECLINE:
        call_exec(db, "UPDATE call_invitees SET declined_at_ms=?3 "
                      "WHERE call_id=?1 AND user_id=?2 AND declined_at_ms IS NULL;", 3, v, 0, NULL);
        break;
    case OC_CALL_LOG_SUBJECT: {
        call_exec(db, "UPDATE calls SET subject=?4 WHERE id=?1;", 1, v, 4, j->call_subject);
        call_exec(db, "INSERT INTO call_events(call_id, at_ms, user_id, kind, detail) "
                      "VALUES(?1, ?3, ?2, 'subject', ?4);", 3, v, 4, j->call_subject);
        oc_dbres *m = call_message_refresh(db, j->call_id);
        if (m) { free(r); return m; }
        break;
    }
    case OC_CALL_LOG_EVENT:
        if (j->call_event >= 1 && j->call_event < sizeof CALL_EVENT_KINDS / sizeof CALL_EVENT_KINDS[0])
            call_exec(db, "INSERT INTO call_events(call_id, at_ms, user_id, kind) VALUES(?1, ?3, ?2, ?4);",
                      3, v, 4, CALL_EVENT_KINDS[j->call_event]);
        break;
    case OC_CALL_LOG_END: {
        static const char *const why[] = { "left", "ended", "stopped" };
        sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, NULL);
        call_exec(db, "UPDATE call_participants SET left_at_ms=?3 WHERE call_id=?1 AND left_at_ms IS NULL;",
                  3, v, 0, NULL);
        call_exec(db, "UPDATE calls SET ended_at_ms=?3, end_reason=?4 WHERE id=?1 AND ended_at_ms IS NULL;",
                  3, v, 4, why[j->call_reason <= OC_CALL_END_STOPPED ? j->call_reason : 0]);
        if (sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        oc_dbres *m = call_message_refresh(db, j->call_id);
        if (m) { free(r); return m; }
        break;
    }
    default: break;
    }
    return r;
}

static int reclaim_add(sqlite3 *db, oc_dbres *r, size_t cap,
                       uint64_t aid, const char *key, uint64_t now, int reason);

/* Whether `uid` was asked to `call` -- the starter included -- the one test of
 * who may see a call, its page, chat, recording and transcript (CALLS.md §1). */
static int call_invitee(sqlite3 *db, uint64_t call, uint64_t uid) {
    sqlite3_stmt *st = NULL;
    int yes = 0;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM call_invitees WHERE call_id=?1 AND user_id=?2;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)call);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)uid);
        yes = sqlite3_step(st) == SQLITE_ROW;
    }
    sqlite3_finalize(st);
    return yes;
}

static uint8_t call_end_wire(const unsigned char *why) {
    if (!why) return OC_CALL_RUNNING;
    if (!strcmp((const char *)why, "ended"))   return OC_CALL_ENDED_ALL;
    if (!strcmp((const char *)why, "stopped")) return OC_CALL_ENDED_STOPPED;
    return OC_CALL_ENDED_LEFT;
}

static uint8_t call_event_wire(const unsigned char *kind) {
    for (uint8_t k = 1; kind && k < sizeof CALL_EVENT_KINDS / sizeof CALL_EVENT_KINDS[0]; k++)
        if (!strcmp((const char *)kind, CALL_EVENT_KINDS[k])) return k;
    return 0;
}

/* Strings a page is built from, kept until it is encoded. */
typedef struct { char **v; size_t n, cap; } str_arena;
static oc_slice arena_keep(str_arena *a, const unsigned char *t) {
    char *d = strdup(t ? (const char *)t : "");
    if (d && a->n == a->cap) {
        size_t cap = a->cap ? a->cap * 2 : 64;
        char **g = realloc(a->v, cap * sizeof *g);
        if (!g) { free(d); d = NULL; } else { a->v = g; a->cap = cap; }
    }
    if (!d) return oc_slice_str("");
    a->v[a->n++] = d;
    return oc_slice_str(d);
}

/* The answer, encoded here whole, into r->frame. */
static void call_frame_take(oc_dbres *r, const uint8_t *buf, size_t len) {
    if ((r->frame = malloc(len)) != NULL) { memcpy(r->frame, buf, len); r->frame_len = len; }
}

/* Calls home's history (CALLS.md §1): the calls this user was asked to, newest
 * first, as they saw them -- running and asked, taken part in, missed,
 * declined. Read. */
static oc_dbres *process_call_history(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->type = OC_RES_CALL_FRAME;
    unsigned lim = j->call_limit && j->call_limit <= OC_CALL_HISTORY_PAGE ? j->call_limit : OC_CALL_HISTORY_PAGE;
    sqlite3_stmt *st = NULL;
    oc_call_hist_entry ents[OC_CALL_HISTORY_PAGE];
    char *subj[OC_CALL_HISTORY_PAGE];
    uint16_t n = 0;
    uint8_t more = 0;
    if (sqlite3_prepare_v2(db,
            "SELECT c.id, c.subject, c.starter, c.started_at_ms, COALESCE(c.ended_at_ms,0), c.end_reason,"
            "       COALESCE(c.from_channel,0), COALESCE((SELECT ch.id FROM channels ch WHERE ch.call_id=c.id),0),"
            "       (SELECT COUNT(DISTINCT p.user_id) FROM call_participants p WHERE p.call_id=c.id),"
            "       EXISTS(SELECT 1 FROM call_participants p WHERE p.call_id=c.id AND p.user_id=?1),"
            "       i.declined_at_ms IS NOT NULL,"
            "       EXISTS(SELECT 1 FROM call_recordings cr WHERE cr.call_id=c.id AND cr.state='ready'),"
            "       EXISTS(SELECT 1 FROM call_transcripts t WHERE t.call_id=c.id)"
            "  FROM call_invitees i JOIN calls c ON c.id = i.call_id"
            " WHERE i.user_id=?1 AND (?2 = 0 OR c.started_at_ms < ?2)"
            " ORDER BY c.started_at_ms DESC, c.id DESC LIMIT ?3;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->user_id);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)j->call_before_ms);
        sqlite3_bind_int64(st, 3, (sqlite3_int64)lim + 1);
        while (sqlite3_step(st) == SQLITE_ROW) {
            if (n == lim) { more = 1; break; }
            oc_call_hist_entry *e = &ents[n];
            memset(e, 0, sizeof *e);
            e->call_id = (uint64_t)sqlite3_column_int64(st, 0);
            const char *t = (const char *)sqlite3_column_text(st, 1);
            subj[n] = strdup(t ? t : "");
            e->subject = oc_slice_str(subj[n] ? subj[n] : "");
            e->starter = (uint64_t)sqlite3_column_int64(st, 2);
            e->started_at = (uint64_t)sqlite3_column_int64(st, 3);
            e->ended_at = (uint64_t)sqlite3_column_int64(st, 4);
            e->end_reason = sqlite3_column_type(st, 5) == SQLITE_NULL ? OC_CALL_RUNNING
                                                                       : call_end_wire(sqlite3_column_text(st, 5));
            e->from_channel = (uint64_t)sqlite3_column_int64(st, 6);
            e->chat_channel = (uint64_t)sqlite3_column_int64(st, 7);
            e->n_people = (uint16_t)sqlite3_column_int(st, 8);
            int joined = sqlite3_column_int(st, 9), declined = sqlite3_column_int(st, 10);
            int mine = e->starter == j->user_id, over = e->ended_at != 0;
            e->flags = (uint8_t)((joined ? OC_CALL_HIST_JOINED : 0) |
                                 (declined ? OC_CALL_HIST_DECLINED : 0) |
                                 (!joined && !declined && !mine && over ? OC_CALL_HIST_MISSED : 0) |
                                 (!joined && !declined && !mine && !over ? OC_CALL_HIST_PENDING : 0) |
                                 (sqlite3_column_int(st, 11) ? OC_CALL_HIST_RECORDING : 0) |
                                 (sqlite3_column_int(st, 12) ? OC_CALL_HIST_TRANSCRIPT : 0));
            n++;
        }
    }
    sqlite3_finalize(st);
    /* Each row's faces: who took part, in the order they first joined. */
    st = NULL;
    if (n && sqlite3_prepare_v2(db, "SELECT user_id FROM call_participants WHERE call_id=?1 GROUP BY user_id"
                                    " ORDER BY MIN(joined_at_ms), user_id LIMIT ?2;", -1, &st, NULL) == SQLITE_OK) {
        for (uint16_t i = 0; i < n; i++) {
            sqlite3_reset(st);
            sqlite3_bind_int64(st, 1, (sqlite3_int64)ents[i].call_id);
            sqlite3_bind_int(st, 2, (int)OC_CALL_HIST_FACES);
            while (ents[i].n_faces < OC_CALL_HIST_FACES && sqlite3_step(st) == SQLITE_ROW)
                ents[i].faces[ents[i].n_faces++] = (uint64_t)sqlite3_column_int64(st, 0);
        }
    }
    sqlite3_finalize(st);
    uint8_t *buf = malloc(OC_MAX_FRAME_SIZE);
    if (buf) {
        oc_wbuf w; oc_wbuf_init(&w, buf, OC_MAX_FRAME_SIZE);
        oc_call_history_page pg = { n, ents, more };
        if (oc_encode_call_history_page(&w, OC_PROTOCOL_VERSION, &pg) == OC_OK) call_frame_take(r, buf, w.len);
        free(buf);
    }
    for (uint16_t i = 0; i < n; i++) free(subj[i]);
    return r;
}

/* One call's page (CALLS.md §1), for its invitees and nobody else: when and
 * how long, who came and went and who was asked, the timeline, the
 * recordings, and the transcript's lines from `call_line_from`, as many as fit
 * one frame. Read. */
static oc_dbres *process_call_detail(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->call_id = j->call_id;
    if (!call_invitee(db, j->call_id, j->user_id)) {
        r->type = OC_RES_CALL_ERR;
        r->err_code = OC_ERR_NOT_A_MEMBER;
        return r;
    }
    r->type = OC_RES_CALL_FRAME;
    /* The strings live in an arena freed at the end: the frame is encoded here. */
    str_arena ar = { NULL, 0, 0 };
    #define KEEP(t) arena_keep(&ar, (t))
    oc_call_page pg;
    memset(&pg, 0, sizeof pg);
    static __thread oc_call_stay stays[OC_CALL_PAGE_STAYS];
    static __thread oc_call_invitee invs[OC_CALL_PAGE_INVITEES];
    static __thread oc_call_event evs[OC_CALL_PAGE_EVENTS];
    static __thread oc_call_recording recs[OC_CALL_PAGE_RECORDINGS];
    static __thread oc_call_tline lines[OC_CALL_PAGE_LINES];
    pg.stays = stays; pg.invitees = invs; pg.events = evs; pg.recordings = recs; pg.lines = lines;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT subject, starter, started_at_ms, COALESCE(ended_at_ms,0), end_reason, COALESCE(from_channel,0),"
            "       COALESCE((SELECT id FROM channels WHERE call_id=calls.id),0) FROM calls WHERE id=?1;",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->call_id);
        if (sqlite3_step(st) == SQLITE_ROW) {
            pg.call_id = j->call_id;
            pg.subject = KEEP(sqlite3_column_text(st, 0));
            pg.starter = (uint64_t)sqlite3_column_int64(st, 1);
            pg.started_at = (uint64_t)sqlite3_column_int64(st, 2);
            pg.ended_at = (uint64_t)sqlite3_column_int64(st, 3);
            pg.end_reason = sqlite3_column_type(st, 4) == SQLITE_NULL ? OC_CALL_RUNNING : call_end_wire(sqlite3_column_text(st, 4));
            pg.from_channel = (uint64_t)sqlite3_column_int64(st, 5);
            pg.chat_channel = (uint64_t)sqlite3_column_int64(st, 6);
        }
    }
    sqlite3_finalize(st); st = NULL;
    oc_call_stay *sp = stays; oc_call_invitee *ip = invs; oc_call_event *ep = evs;
    oc_call_recording *rp = recs; oc_call_tline *lp = lines;
    if (sqlite3_prepare_v2(db, "SELECT user_id, joined_at_ms, COALESCE(left_at_ms,0) FROM call_participants "
                               "WHERE call_id=?1 ORDER BY joined_at_ms, id LIMIT ?2;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->call_id);
        sqlite3_bind_int(st, 2, (int)OC_CALL_PAGE_STAYS);
        while (sqlite3_step(st) == SQLITE_ROW) {
            sp[pg.n_stays].user_id = (uint64_t)sqlite3_column_int64(st, 0);
            sp[pg.n_stays].joined_at = (uint64_t)sqlite3_column_int64(st, 1);
            sp[pg.n_stays].left_at = (uint64_t)sqlite3_column_int64(st, 2);
            pg.n_stays++;
        }
    }
    sqlite3_finalize(st); st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT user_id, invited_by, invited_at_ms, COALESCE(declined_at_ms,0) FROM call_invitees "
                               "WHERE call_id=?1 ORDER BY invited_at_ms, user_id LIMIT ?2;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->call_id);
        sqlite3_bind_int(st, 2, (int)OC_CALL_PAGE_INVITEES);
        while (sqlite3_step(st) == SQLITE_ROW) {
            ip[pg.n_invitees].user_id = (uint64_t)sqlite3_column_int64(st, 0);
            ip[pg.n_invitees].invited_by = (uint64_t)sqlite3_column_int64(st, 1);
            ip[pg.n_invitees].invited_at = (uint64_t)sqlite3_column_int64(st, 2);
            ip[pg.n_invitees].declined_at = (uint64_t)sqlite3_column_int64(st, 3);
            pg.n_invitees++;
        }
    }
    sqlite3_finalize(st); st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT at_ms, user_id, kind, COALESCE(detail,'') FROM call_events "
                               "WHERE call_id=?1 ORDER BY at_ms, id LIMIT ?2;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->call_id);
        sqlite3_bind_int(st, 2, (int)OC_CALL_PAGE_EVENTS);
        while (sqlite3_step(st) == SQLITE_ROW) {
            ep[pg.n_events].at = (uint64_t)sqlite3_column_int64(st, 0);
            ep[pg.n_events].user_id = (uint64_t)sqlite3_column_int64(st, 1);
            ep[pg.n_events].kind = call_event_wire(sqlite3_column_text(st, 2));
            ep[pg.n_events].detail = KEEP(sqlite3_column_text(st, 3));
            pg.n_events++;
        }
    }
    sqlite3_finalize(st); st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT id, state, started_by, started_at_ms, COALESCE(stopped_at_ms,0),"
                               "       COALESCE(duration_ms,0), COALESCE(bytes,0), COALESCE(attachment_id,0)"
                               "  FROM call_recordings WHERE call_id=?1 ORDER BY id LIMIT ?2;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->call_id);
        sqlite3_bind_int(st, 2, (int)OC_CALL_PAGE_RECORDINGS);
        while (sqlite3_step(st) == SQLITE_ROW) {
            oc_call_recording *rc = &rp[pg.n_recordings];
            const char *state = (const char *)sqlite3_column_text(st, 1);
            rc->id = (uint64_t)sqlite3_column_int64(st, 0);
            rc->state = !state ? 0 : !strcmp(state, "recording") ? OC_CALL_REC_RECORDING
                      : !strcmp(state, "processing") ? OC_CALL_REC_PROCESSING
                      : !strcmp(state, "ready") ? OC_CALL_REC_READY : OC_CALL_REC_FAILED;
            rc->started_by = (uint64_t)sqlite3_column_int64(st, 2);
            rc->started_at = (uint64_t)sqlite3_column_int64(st, 3);
            rc->stopped_at = (uint64_t)sqlite3_column_int64(st, 4);
            rc->duration_ms = (uint32_t)sqlite3_column_int64(st, 5);
            rc->bytes = (uint64_t)sqlite3_column_int64(st, 6);
            rc->attachment_id = rc->state == OC_CALL_REC_READY ? (uint64_t)sqlite3_column_int64(st, 7) : 0;
            pg.n_recordings++;
        }
    }
    sqlite3_finalize(st); st = NULL;
    uint64_t tid = 0;
    if (sqlite3_prepare_v2(db, "SELECT id, COALESCE(language,''), (SELECT COUNT(*) FROM call_transcript_lines l "
                               "WHERE l.transcript_id=t.id) FROM call_transcripts t WHERE call_id=?1;",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->call_id);
        if (sqlite3_step(st) == SQLITE_ROW) {
            tid = (uint64_t)sqlite3_column_int64(st, 0);
            pg.has_transcript = 1;
            pg.transcript_lang = KEEP(sqlite3_column_text(st, 1));
            pg.n_lines_total = (uint32_t)sqlite3_column_int64(st, 2);
        }
    }
    sqlite3_finalize(st); st = NULL;
    pg.line_from = j->call_line_from;
    if (tid && sqlite3_prepare_v2(db, "SELECT id, COALESCE(speaker,0), start_ms, end_ms, text FROM call_transcript_lines "
                                      "WHERE transcript_id=?1 ORDER BY start_ms, id LIMIT ?2 OFFSET ?3;",
                                  -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)tid);
        sqlite3_bind_int(st, 2, (int)OC_CALL_PAGE_LINES);
        sqlite3_bind_int64(st, 3, (sqlite3_int64)j->call_line_from);
        size_t text = 0;
        while (sqlite3_step(st) == SQLITE_ROW) {
            size_t tl = (size_t)sqlite3_column_bytes(st, 4);
            if (pg.n_lines && text + tl > OC_CALL_PAGE_TEXT) break;   /* the rest on the next page */
            text += tl;
            lp[pg.n_lines].id = (uint64_t)sqlite3_column_int64(st, 0);
            lp[pg.n_lines].speaker = (uint64_t)sqlite3_column_int64(st, 1);
            lp[pg.n_lines].start_ms = (uint32_t)sqlite3_column_int64(st, 2);
            lp[pg.n_lines].end_ms = (uint32_t)sqlite3_column_int64(st, 3);
            lp[pg.n_lines].text = KEEP(sqlite3_column_text(st, 4));
            pg.n_lines++;
        }
    }
    sqlite3_finalize(st);
    #undef KEEP
    uint8_t *buf = malloc(OC_MAX_FRAME_SIZE);
    if (buf && pg.call_id) {
        oc_wbuf w; oc_wbuf_init(&w, buf, OC_MAX_FRAME_SIZE);
        if (oc_encode_call_page(&w, OC_PROTOCOL_VERSION, &pg) == OC_OK) call_frame_take(r, buf, w.len);
    }
    free(buf);
    for (size_t i = 0; i < ar.n; i++) free(ar.v[i]);
    free(ar.v);
    if (!r->frame) { r->type = OC_RES_CALL_ERR; r->err_code = OC_ERR_INTERNAL; }
    return r;
}

/* Delete a call's transcript -- an admin's to do -- or one of its recordings
 * -- the person who started it, or an admin (CALLS.md §1). The call's invitees
 * are told; a recording's bytes go. Write. */
static oc_dbres *process_call_delete(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->call_id = j->call_id;
    r->call_del_what = j->call_del_what;
    r->call_del_id = j->call_del_id;
    r->type = OC_RES_CALL_ERR;
    uint8_t role = OC_ROLE_MEMBER;
    int admin = user_role(db, j->user_id, &role) && (role == OC_ROLE_ADMIN || role == OC_ROLE_OWNER);
    sqlite3_stmt *st = NULL;
    sqlite3_int64 v[2] = { (sqlite3_int64)j->call_id, (sqlite3_int64)j->call_del_id };
    if (j->call_del_what == OC_CALL_DEL_TRANSCRIPT) {
        if (!admin) { r->err_code = OC_ERR_FORBIDDEN; return r; }
        uint64_t tid = 0;
        if (sqlite3_prepare_v2(db, "SELECT id FROM call_transcripts WHERE call_id=?1;", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, v[0]);
            if (sqlite3_step(st) == SQLITE_ROW) tid = (uint64_t)sqlite3_column_int64(st, 0);
        }
        sqlite3_finalize(st);
        if (!tid) { r->err_code = OC_ERR_NOT_IN_CALL; return r; }
        sqlite3_int64 t[1] = { (sqlite3_int64)tid };
        sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, NULL);
        call_exec(db, "DELETE FROM call_transcript_lines WHERE transcript_id=?1;", 1, t, 0, NULL);
        call_exec(db, "DELETE FROM call_transcripts WHERE id=?1;", 1, t, 0, NULL);
        if (sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) {
            sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
            r->err_code = OC_ERR_INTERNAL;
            return r;
        }
        audit_actor(db, OC_AUDIT_ADMIN, "call.transcript.delete", j->user_id, j->call_id, "call", 1, NULL);
    } else if (j->call_del_what == OC_CALL_DEL_RECORDING) {
        uint64_t by = 0, aid = 0;
        int found = 0;
        if (sqlite3_prepare_v2(db, "SELECT started_by, COALESCE(attachment_id,0) FROM call_recordings "
                                   "WHERE id=?2 AND call_id=?1 AND state IN ('ready','failed');", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, v[0]);
            sqlite3_bind_int64(st, 2, v[1]);
            if (sqlite3_step(st) == SQLITE_ROW) {
                found = 1;
                by = (uint64_t)sqlite3_column_int64(st, 0);
                aid = (uint64_t)sqlite3_column_int64(st, 1);
            }
        }
        sqlite3_finalize(st);
        if (!found) { r->err_code = OC_ERR_NOT_IN_CALL; return r; }
        if (by != j->user_id && !admin) { r->err_code = OC_ERR_FORBIDDEN; return r; }
        char *key = NULL;
        if (aid && sqlite3_prepare_v2(db, "SELECT storage_key FROM attachments WHERE id=?1;", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)aid);
            if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0)) key = strdup((const char *)sqlite3_column_text(st, 0));
        }
        sqlite3_finalize(st);
        call_exec(db, "DELETE FROM call_recordings WHERE id=?2 AND call_id=?1;", 2, v, 0, NULL);
        if (key) {
            r->reclaim = calloc(1, sizeof *r->reclaim);
            if (r->reclaim) reclaim_add(db, r, 1, aid, key, dbw_now_ms(), OC_RECLAIM_DELETED);
            free(key);
        }
        if (by != j->user_id)
            audit_actor(db, OC_AUDIT_ADMIN, "call.recording.delete", j->user_id, j->call_id, "call", 1, NULL);
    } else {
        r->err_code = OC_ERR_NOT_IN_CALL;
        return r;
    }
    r->type = OC_RES_CALL_DELETED;
    r->err_code = 0;
    /* Who is told: the call's invitees, the deleter besides. */
    if (sqlite3_prepare_v2(db, "SELECT user_id FROM call_invitees WHERE call_id=?1;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, v[0]);
        size_t cap = 16;
        r->members = malloc(cap * sizeof *r->members);
        while (r->members && sqlite3_step(st) == SQLITE_ROW) {
            if (r->n_members == cap) {
                uint64_t *g = realloc(r->members, cap * 2 * sizeof *g);
                if (!g) break;
                r->members = g; cap *= 2;
            }
            r->members[r->n_members++] = (uint64_t)sqlite3_column_int64(st, 0);
        }
    }
    sqlite3_finalize(st);
    return r;
}

/* A transcript stored (the daemon's transcript function), replacing the one
 * the call had: its lines indexed for search. Write, fire and forget. */
static oc_dbres *process_call_transcript(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = 0;
    r->type = OC_RES_OK;
    sqlite3_int64 v[2] = { (sqlite3_int64)j->call_id, (sqlite3_int64)dbw_now_ms() };
    if (sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK) return r;
    call_exec(db, "DELETE FROM call_transcript_lines WHERE transcript_id IN "
                  "(SELECT id FROM call_transcripts WHERE call_id=?1);", 1, v, 0, NULL);
    call_exec(db, "DELETE FROM call_transcripts WHERE call_id=?1;", 1, v, 0, NULL);
    int ok = call_exec(db, "INSERT INTO call_transcripts(call_id, created_at_ms, language) "
                           "SELECT ?1, ?2, NULLIF(?3,'') FROM calls WHERE id=?1;", 2, v, 3, j->tr_lang) == 0 &&
             sqlite3_changes(db) == 1;
    uint64_t tid = ok ? (uint64_t)sqlite3_last_insert_rowid(db) : 0;
    sqlite3_stmt *st = NULL;
    if (ok && sqlite3_prepare_v2(db, "INSERT INTO call_transcript_lines(transcript_id, speaker, start_ms, end_ms, text) "
                                     "VALUES(?1, NULLIF(?2,0), ?3, ?4, ?5);", -1, &st, NULL) == SQLITE_OK) {
        for (size_t i = 0; ok && i < j->n_tr_lines; i++) {
            sqlite3_reset(st);
            sqlite3_bind_int64(st, 1, (sqlite3_int64)tid);
            sqlite3_bind_int64(st, 2, (sqlite3_int64)j->tr_lines[i].speaker);
            sqlite3_bind_int64(st, 3, (sqlite3_int64)j->tr_lines[i].start_ms);
            sqlite3_bind_int64(st, 4, (sqlite3_int64)j->tr_lines[i].end_ms);
            sqlite3_bind_text(st, 5, j->tr_lines[i].text ? j->tr_lines[i].text : "", -1, SQLITE_STATIC);
            ok = sqlite3_step(st) == SQLITE_DONE;
        }
    }
    sqlite3_finalize(st);
    if (!ok || sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
    return r;
}

/* A recording begins (CALLS.md §8): its row, unless the recordings already
 * take the space they may. Write. */
static oc_dbres *process_recording_start(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->call_id = j->call_id;
    r->user_id = j->user_id;
    r->type = OC_RES_CALL_ERR;
    r->err_code = OC_ERR_RECORDING_UNAVAILABLE;
    sqlite3_stmt *st = NULL;
    if (j->call_cap_bytes) {
        uint64_t used = 0;
        if (sqlite3_prepare_v2(db, "SELECT COALESCE(SUM(bytes),0) FROM call_recordings;", -1, &st, NULL) == SQLITE_OK &&
            sqlite3_step(st) == SQLITE_ROW) used = (uint64_t)sqlite3_column_int64(st, 0);
        sqlite3_finalize(st); st = NULL;
        if (used >= j->call_cap_bytes) return r;
    }
    if (sqlite3_prepare_v2(db, "INSERT INTO call_recordings(call_id, started_by, started_at_ms, state, spool) "
                               "VALUES(?1, ?2, ?3, 'recording', ?4);", -1, &st, NULL) != SQLITE_OK) {
        sqlite3_finalize(st);
        r->err_code = OC_ERR_INTERNAL;
        return r;
    }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->call_id);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)j->user_id);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)dbw_now_ms());
    sqlite3_bind_text(st, 4, j->call_spool ? j->call_spool : "", -1, SQLITE_TRANSIENT);
    int ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    if (!ok) { r->err_code = OC_ERR_INTERNAL; return r; }
    r->rec_id = (uint64_t)sqlite3_last_insert_rowid(db);
    /* Its spool is a directory of its own under the one named: <dir>/<id>. */
    sqlite3_int64 v[1] = { (sqlite3_int64)r->rec_id };
    call_exec(db, "UPDATE call_recordings SET spool = spool || '/' || id WHERE id=?1;", 1, v, 0, NULL);
    r->type = OC_RES_RECORDING_STARTED;
    r->err_code = 0;
    return r;
}

/* A recording stopped: processing makes it from its spool. Write, fire and
 * forget. */
static oc_dbres *process_recording_stop(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->type = OC_RES_OK;
    sqlite3_int64 v[2] = { (sqlite3_int64)j->rec_id, (sqlite3_int64)dbw_now_ms() };
    call_exec(db, "UPDATE call_recordings SET stopped_at_ms=?2, state='processing' "
                  "WHERE id=?1 AND state='recording';", 2, v, 0, NULL);
    return r;
}

/* Who people in a recording are: each one's name (its display name) and the
 * store key of their avatar, while its bytes are there. Read. */
static oc_dbres *process_rec_people(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->type = OC_RES_REC_PEOPLE;
    r->call_id = j->call_id;
    if (!j->n_call_uids || !(r->rec_people = calloc(j->n_call_uids, sizeof *r->rec_people))) return r;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT COALESCE(u.display_name, ''),"
            "       COALESCE((SELECT a.storage_key FROM attachments a WHERE a.id = u.avatar_attachment_id"
            "                 AND a.reclaimed_at_ms = 0), '')"
            "  FROM users u WHERE u.id = ?1;", -1, &st, NULL) != SQLITE_OK) { sqlite3_finalize(st); return r; }
    for (uint16_t i = 0; i < j->n_call_uids; i++) {
        sqlite3_reset(st);
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->call_uids[i]);
        if (sqlite3_step(st) != SQLITE_ROW) continue;
        oc_rec_person *p = &r->rec_people[r->n_rec_people++];
        p->user = j->call_uids[i];
        snprintf(p->name, sizeof p->name, "%s", (const char *)sqlite3_column_text(st, 0));
        snprintf(p->avatar_key, sizeof p->avatar_key, "%s", (const char *)sqlite3_column_text(st, 1));
    }
    sqlite3_finalize(st);
    return r;
}

/* The recordings stopped and not yet made, oldest first, with their spools:
 * what the worker is given as the daemon starts. Read. */
static oc_dbres *process_recordings_pending(sqlite3 *db, const oc_job *j) {
    (void)j;
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->type = OC_RES_RECORDINGS_PENDING;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT id, spool FROM call_recordings WHERE state='processing' AND spool IS NOT NULL "
                               "ORDER BY id LIMIT 64;", -1, &st, NULL) != SQLITE_OK) { sqlite3_finalize(st); return r; }
    r->rec_ids = calloc(64, sizeof *r->rec_ids);
    r->rec_spools = calloc(64, sizeof *r->rec_spools);
    while (r->rec_ids && r->rec_spools && sqlite3_step(st) == SQLITE_ROW) {
        const char *sp = (const char *)sqlite3_column_text(st, 1);
        if (!(r->rec_spools[r->n_recs] = strdup(sp ? sp : ""))) break;
        r->rec_ids[r->n_recs++] = (uint64_t)sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    return r;
}

/* A recording made (CALLS.md §8): an attachment of the call's chat -- a video,
 * as a video message is -- carried by a message there from who started it,
 * and the recording ready with it. Or not made: failed, with why. Write. */
static oc_dbres *process_recording_done(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->type = OC_RES_OK;
    sqlite3_int64 v[1] = { (sqlite3_int64)j->rec_id };
    if (j->rec_failed) {
        char why[200];
        snprintf(why, sizeof why, "%.*s", (int)(j->body_len < 190 ? j->body_len : 190), j->body ? (const char *)j->body : "");
        call_exec(db, "UPDATE call_recordings SET state='failed', spool=NULL, error=?2 WHERE id=?1;", 1, v, 2, why);
        return r;
    }
    uint64_t call = 0, by = 0, chat = 0;
    char subject[OC_CALL_SUBJECT_MAX + 1] = "";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT r.call_id, r.started_by, c.subject, (SELECT id FROM channels WHERE call_id=c.id) "
                               "FROM call_recordings r JOIN calls c ON c.id=r.call_id WHERE r.id=?1;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, v[0]);
        if (sqlite3_step(st) == SQLITE_ROW) {
            call = (uint64_t)sqlite3_column_int64(st, 0);
            by = (uint64_t)sqlite3_column_int64(st, 1);
            snprintf(subject, sizeof subject, "%s", sqlite3_column_text(st, 2) ? (const char *)sqlite3_column_text(st, 2) : "");
            chat = (uint64_t)sqlite3_column_int64(st, 3);
        }
    }
    sqlite3_finalize(st); st = NULL;
    if (!call || !chat) return r;
    uint64_t ts = dbw_now_ms();
    char fname[OC_CALL_SUBJECT_MAX + 32], body[OC_CALL_SUBJECT_MAX + 32];
    snprintf(fname, sizeof fname, "Recording of %s.mp4", subject);
    snprintf(body, sizeof body, "Recording of %s", subject);
    if (sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK) return r;
    int ok = sqlite3_prepare_v2(db, "INSERT INTO messages(channel_id, author_id, body, created_at_ms) VALUES(?1, ?2, ?3, ?4);",
                                -1, &st, NULL) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)chat);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)by);
        sqlite3_bind_blob (st, 3, body, (int)strlen(body), SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 4, (sqlite3_int64)ts);
        ok = sqlite3_step(st) == SQLITE_DONE;
    }
    sqlite3_finalize(st); st = NULL;
    uint64_t mid = ok ? (uint64_t)sqlite3_last_insert_rowid(db) : 0, aid = 0;
    if (ok) {
        ok = sqlite3_prepare_v2(db, "INSERT INTO attachments(channel_id, message_id, uploader_id, storage_key, filename, mime, "
                                    "size, created_at_ms, sha256) VALUES(?1, ?2, ?3, ?4, ?5, 'video/mp4', ?6, ?7, ?8);",
                                -1, &st, NULL) == SQLITE_OK;
        if (ok) {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)chat);
            sqlite3_bind_int64(st, 2, (sqlite3_int64)mid);
            sqlite3_bind_int64(st, 3, (sqlite3_int64)by);
            sqlite3_bind_text (st, 4, j->call_spool ? j->call_spool : "", -1, SQLITE_TRANSIENT);
            sqlite3_bind_text (st, 5, fname, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(st, 6, (sqlite3_int64)j->exp_bytes);
            sqlite3_bind_int64(st, 7, (sqlite3_int64)ts);
            sqlite3_bind_blob (st, 8, j->att_sha256, (int)sizeof j->att_sha256, SQLITE_TRANSIENT);
            ok = sqlite3_step(st) == SQLITE_DONE;
        }
        sqlite3_finalize(st); st = NULL;
        aid = ok ? (uint64_t)sqlite3_last_insert_rowid(db) : 0;
    }
    if (ok) {
        sqlite3_int64 m[6] = { (sqlite3_int64)aid, (sqlite3_int64)j->exp_duration_ms, OC_CALLREC_WIDTH, OC_CALLREC_HEIGHT,
                               (sqlite3_int64)ts, OC_MEDIA_VIDEO_MESSAGE };
        /* A video, played as a video message is. */
        ok = call_exec(db, "INSERT INTO attachment_media(attachment_id, kind, duration_ms, width, height, poster_id, created_at_ms) "
                           "VALUES(?1, ?6, ?2, ?3, ?4, NULL, ?5);", 6, m, 0, NULL) == 0;
    }
    if (ok) {
        sqlite3_int64 u[4] = { (sqlite3_int64)j->rec_id, (sqlite3_int64)aid, (sqlite3_int64)j->exp_bytes,
                               (sqlite3_int64)j->exp_duration_ms };
        ok = call_exec(db, "UPDATE call_recordings SET state='ready', spool=NULL, attachment_id=?2, bytes=?3, "
                           "duration_ms=?4 WHERE id=?1;", 4, u, 0, NULL) == 0;
    }
    if (!ok || sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        call_exec(db, "UPDATE call_recordings SET state='failed', spool=NULL, error='its row could not be written' "
                      "WHERE id=?1;", 1, v, 0, NULL);
        return r;
    }
    /* The message, to the call's people, as a message with its file is sent. */
    r->type = OC_RES_RECORDING_DONE;
    r->message_id = mid;
    r->channel_id = chat;
    r->author_id = by;
    r->server_time = ts;
    r->call_id = call;
    r->body = malloc(strlen(body));
    if (r->body) { memcpy(r->body, body, strlen(body)); r->body_len = strlen(body); }
    load_message_attachments(db, mid, r->attach, &r->n_attach);
    append_action(db, r, mid, chat, 1);
    load_members(db, chat, r);
    return r;
}

int oc_dbwriter_call_transcript(oc_dbwriter *w, uint64_t call_id, const char *language,
                                const oc_transcript_line *lines, size_t n) {
    oc_job *j = oc_job_new(OC_JOB_CALL_TRANSCRIPT, 0);
    if (!j) return -1;
    j->call_id = call_id;
    j->tr_lang = strdup(language ? language : "");
    j->tr_lines = n ? calloc(n, sizeof *j->tr_lines) : NULL;
    if (!j->tr_lang || (n && !j->tr_lines)) { oc_job_free(j); return -1; }
    for (size_t i = 0; i < n; i++) {
        j->tr_lines[i] = lines[i];
        j->tr_lines[i].text = strdup(lines[i].text ? lines[i].text : "");
        j->n_tr_lines = i + 1;
        if (!j->tr_lines[i].text) { oc_job_free(j); return -1; }
    }
    oc_dbwriter_submit(w, j);
    return 0;
}

/* The daemon stopped with calls open: each ended then, by a stop, with every
 * stay in it closed. When is not known, so it is the last thing the call
 * recorded -- its latest join, leave or event, else its start. Run once, as
 * the writer starts, before any call can begin. */
static void calls_close_stopped(sqlite3 *db) {
    /* Which they are, first: their messages say they ended, below. */
    uint64_t ids[256];
    int n = 0;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT id FROM calls WHERE ended_at_ms IS NULL;", -1, &st, NULL) == SQLITE_OK)
        while (n < 256 && sqlite3_step(st) == SQLITE_ROW) ids[n++] = (uint64_t)sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    sqlite3_exec(db,
        "BEGIN IMMEDIATE;"
        "UPDATE calls SET end_reason='stopped', ended_at_ms=MAX(started_at_ms,"
        "  COALESCE((SELECT MAX(MAX(joined_at_ms, COALESCE(left_at_ms, 0))) FROM call_participants p"
        "            WHERE p.call_id=calls.id), 0),"
        "  COALESCE((SELECT MAX(at_ms) FROM call_events e WHERE e.call_id=calls.id), 0))"
        " WHERE ended_at_ms IS NULL;"
        "UPDATE call_participants SET left_at_ms=(SELECT ended_at_ms FROM calls c WHERE c.id=call_participants.call_id)"
        " WHERE left_at_ms IS NULL;"
        "COMMIT;", NULL, NULL, NULL);
    /* A recording the daemon stopped in the middle of: what its spool holds is
     * processed as for any stopped recording (CALLS.md §8). */
    sqlite3_exec(db, "UPDATE call_recordings SET state='processing', stopped_at_ms=COALESCE(stopped_at_ms,"
                     " (SELECT ended_at_ms FROM calls c WHERE c.id=call_recordings.call_id)) WHERE state='recording';",
                 NULL, NULL, NULL);
    /* Nobody is connected yet to be told; whoever reads them sees they ended. */
    for (int i = 0; i < n; i++) oc_dbres_free(call_message_refresh(db, ids[i]));
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
    if (workspace_dm(db, j->channel_id)) { r->type = OC_RES_ATTACH_ERR; r->err_code = OC_ERR_WORKSPACE_SENDER; return r; }
    if (acc == CH_ARCHIVED){ r->type = OC_RES_ATTACH_ERR; r->err_code = OC_ERR_CHANNEL_ARCHIVED; return r; }
    if (channel_is_e2e(db, j->channel_id)) { r->type = OC_RES_ATTACH_ERR; r->err_code = OC_ERR_FORBIDDEN; return r; }

    /* Idempotent replay, the same contract SEND honours: a known
     * (channel, token) hands back the row the first attempt minted rather than
     * minting a second. The token has always been on the wire and in the job;
     * until migration 0037 nothing read it, so a client that retried after a
     * dropped connection created an orphan pending row for every attempt.
     *
     * The reply is deliberately identical to a first attempt's — same id, same
     * storage key — because the retrying client is one that never heard the
     * first answer, and its next move is to stream the bytes.
     *
     * Scoped to the UPLOADER too, unlike the `sent_messages` map: what comes
     * back here is a key the net loop opens for WRITING, so this must not hand
     * one member the row another member declared (migration 0037). */
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "SELECT id, storage_key, size FROM attachments "
        "WHERE channel_id=? AND uploader_id=? AND idem_token=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->channel_id);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)j->user_id);
    sqlite3_bind_blob (st, 3, j->idem, OC_IDEM_LEN, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) {
        r->type = OC_RES_ATTACH_CREATED;
        r->attachment_id = (uint64_t)sqlite3_column_int64(st, 0);
        const unsigned char *k = sqlite3_column_text(st, 1);
        r->storage_key = strdup(k ? (const char *)k : "");
        r->att_size = (uint64_t)sqlite3_column_int64(st, 2);
        r->duplicate = 1;
        sqlite3_finalize(st);
        return r;
    }
    sqlite3_finalize(st);

    sqlite3_prepare_v2(db,
        "INSERT INTO attachments(channel_id, message_id, uploader_id, storage_key, "
        "  filename, mime, size, sha256, created_at_ms, idem_token) "
        "VALUES(?, NULL, ?, '', ?, ?, ?, NULL, ?, ?);", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->channel_id);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)j->user_id);
    sqlite3_bind_text (st, 3, j->filename ? j->filename : "", -1, SQLITE_STATIC);
    sqlite3_bind_text (st, 4, j->mime ? j->mime : "", -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 5, (sqlite3_int64)j->att_size);
    sqlite3_bind_int64(st, 6, (sqlite3_int64)dbw_now_ms());
    sqlite3_bind_blob (st, 7, j->idem, OC_IDEM_LEN, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) { r->type = OC_RES_ATTACH_ERR; r->err_code = OC_ERR_INTERNAL; return r; }

    uint64_t aid = (uint64_t)sqlite3_last_insert_rowid(db);
    /* The storage key is 128 random bits in hex — opaque, never on the wire, and
     * never handed out twice. Not the row id: a database restored to an earlier
     * time hands its row ids out again, and a key must always name the same
     * bytes, in the store and in the cache in front of it (blob_cache.c). Keys
     * made before this were the row id in hex, and stay as they are. */
    uint8_t rnd[16];
    if (oc_rand_bytes(rnd, sizeof rnd) != 0) { r->type = OC_RES_ATTACH_ERR; r->err_code = OC_ERR_INTERNAL; return r; }
    char key[33];
    for (size_t i = 0; i < sizeof rnd; i++) snprintf(key + 2 * i, 3, "%02x", rnd[i]);
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
        "SELECT size, uploader_id, sha256, storage_key, mime FROM attachments WHERE id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->attachment_id);
    int found = sqlite3_step(st) == SQLITE_ROW;
    uint64_t declared = found ? (uint64_t)sqlite3_column_int64(st, 0) : 0;
    uint64_t owner    = found ? (uint64_t)sqlite3_column_int64(st, 1) : 0;
    int already       = found && sqlite3_column_type(st, 2) != SQLITE_NULL;
    /* The key and type ride back so the net loop can have the image previewed
     * (ARCH-137) without asking again. */
    if (found) {
        const char *k = (const char *)sqlite3_column_text(st, 3);
        const char *m = (const char *)sqlite3_column_text(st, 4);
        r->storage_key = strdup(k ? k : "");
        r->mime = strdup(m ? m : "");
    }
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

/* ATTACH_MEDIA_SET marks a finalized upload as a video message (REQ-162/164,
 * ARCH-110) and names its poster. Everything checkable is checked here; what is
 * not — whether the bytes really are a VP9 and Opus MP4 of that length — the
 * daemon cannot know without a codec it deliberately does not link, so those are
 * stored as the client's report and the byte cap is the enforced bound.
 *
 * The poster must be the same person's upload to the same channel. That is what
 * makes it readable exactly when its video is: download authorization is the
 * channel read gate on the attachment's own channel (process_attach_lookup), and
 * the two share a channel by construction, so no exception to that gate is
 * needed. Write. */
/* Whether `n` bytes at `s` are whole UTF-8 with no NUL: what goes into the
 * text index has to be text. */
static int utf8_whole(const char *s, size_t n) {
    const unsigned char *u = (const unsigned char *)s;
    size_t i = 0;
    while (i < n) {
        unsigned c = u[i];
        size_t k = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
        if (c == 0 || k == 0 || i + k > n) return 0;
        uint32_t cp = k == 1 ? c : k == 2 ? (c & 0x1F) : k == 3 ? (c & 0x0F) : (c & 0x07);
        for (size_t q = 1; q < k; q++) {
            if ((u[i + q] & 0xC0) != 0x80) return 0;
            cp = (cp << 6) | (u[i + q] & 0x3F);
        }
        /* Overlong forms, surrogates and past the last code point. */
        if ((k == 2 && cp < 0x80) || (k == 3 && cp < 0x800) || (k == 4 && cp < 0x10000) ||
            (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) return 0;
        i += k;
    }
    return 1;
}

static oc_dbres *process_attach_media_set(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->attachment_id = j->attachment_id;
    r->type = OC_RES_MEDIA_ERR;

    if (j->media_kind == OC_MEDIA_SNIPPET) {
        /* A snippet (REQ-226): the caller's finished, unlinked text/plain
         * upload, at most OC_SNIPPET_MAX_SIZE, with a title. Its title and the
         * first bytes of its text go into snippets_fts, so a search finds the
         * message carrying it; the row goes when the attachment does. */
        if (!j->snip_title || !j->snip_title[0] || !j->snip_lang || !j->snip_text ||
            !utf8_whole(j->snip_title, strlen(j->snip_title)) ||
            !utf8_whole(j->snip_text, j->snip_text_len)) {
            r->err_code = OC_ERR_MEDIA_INVALID; return r;
        }
        for (const char *q = j->snip_lang; *q; q++)
            if (!((*q >= 'a' && *q <= 'z') || (*q >= '0' && *q <= '9') || *q == '+' || *q == '#' || *q == '-')) {
                r->err_code = OC_ERR_MEDIA_INVALID; return r;
            }
        sqlite3_stmt *st = NULL;
        sqlite3_prepare_v2(db,
            "SELECT uploader_id, message_id, sha256 IS NOT NULL, reclaimed_at_ms, mime, size "
            "  FROM attachments WHERE id=?;", -1, &st, NULL);
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->attachment_id);
        int found = sqlite3_step(st) == SQLITE_ROW;
        uint64_t up = found ? (uint64_t)sqlite3_column_int64(st, 0) : 0;
        int linked  = found && sqlite3_column_type(st, 1) != SQLITE_NULL;
        int final   = found && sqlite3_column_int(st, 2);
        int gone    = found && sqlite3_column_int64(st, 3) != 0;
        const unsigned char *mm = found ? sqlite3_column_text(st, 4) : NULL;
        int plain   = mm && strcmp((const char *)mm, "text/plain") == 0;
        uint64_t sz = found ? (uint64_t)sqlite3_column_int64(st, 5) : 0;
        sqlite3_finalize(st);
        if (!found || up != j->user_id || !final || gone) { r->err_code = OC_ERR_UNKNOWN_ATTACHMENT; return r; }
        if (linked || !plain) { r->err_code = OC_ERR_MEDIA_INVALID; return r; }
        if (sz > OC_SNIPPET_MAX_SIZE) { r->err_code = OC_ERR_MEDIA_TOO_LARGE; return r; }
        sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, NULL);
        int ok = 1;
        sqlite3_prepare_v2(db,
            "INSERT INTO attachment_media(attachment_id, kind, duration_ms, width, height, poster_id, "
            "  created_at_ms, title, language) VALUES(?1, ?2, 0, 0, 0, NULL, ?3, ?4, ?5) "
            "ON CONFLICT(attachment_id) DO UPDATE SET kind=?2, title=?4, language=?5;", -1, &st, NULL);
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->attachment_id);
        sqlite3_bind_int  (st, 2, (int)OC_MEDIA_SNIPPET);
        sqlite3_bind_int64(st, 3, (sqlite3_int64)dbw_now_ms());
        sqlite3_bind_text (st, 4, j->snip_title, -1, SQLITE_STATIC);
        sqlite3_bind_text (st, 5, j->snip_lang, -1, SQLITE_STATIC);
        ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
        if (ok) {
            sqlite3_prepare_v2(db, "DELETE FROM snippets_fts WHERE rowid=?1;", -1, &st, NULL);
            sqlite3_bind_int64(st, 1, (sqlite3_int64)j->attachment_id);
            sqlite3_step(st); sqlite3_finalize(st);
            sqlite3_prepare_v2(db, "INSERT INTO snippets_fts(rowid, title, body) VALUES(?1, ?2, ?3);",
                               -1, &st, NULL);
            sqlite3_bind_int64(st, 1, (sqlite3_int64)j->attachment_id);
            sqlite3_bind_text (st, 2, j->snip_title, -1, SQLITE_STATIC);
            sqlite3_bind_text (st, 3, j->snip_text, (int)j->snip_text_len, SQLITE_STATIC);
            ok = sqlite3_step(st) == SQLITE_DONE;
            sqlite3_finalize(st);
        }
        sqlite3_exec(db, ok ? "COMMIT;" : "ROLLBACK;", NULL, NULL, NULL);
        if (!ok) { r->err_code = OC_ERR_INTERNAL; return r; }
        r->type = OC_RES_MEDIA_OK;
        return r;
    }

    if (j->media_kind != OC_MEDIA_VIDEO_MESSAGE ||
        j->media_duration_ms == 0 || j->media_duration_ms > OC_MEDIA_MAX_DURATION_MS ||
        j->media_width < 2 || j->media_width > OC_MEDIA_MAX_WIDTH || (j->media_width & 1) ||
        j->media_height < 2 || j->media_height > OC_MEDIA_MAX_HEIGHT || (j->media_height & 1) ||
        j->media_poster_id == 0 || j->media_poster_id == j->attachment_id) {
        r->err_code = OC_ERR_MEDIA_INVALID;
        return r;
    }

    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "SELECT channel_id, uploader_id, message_id, sha256 IS NOT NULL, reclaimed_at_ms, mime, size "
        "  FROM attachments WHERE id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->attachment_id);
    int found = sqlite3_step(st) == SQLITE_ROW;
    uint64_t vch = found ? (uint64_t)sqlite3_column_int64(st, 0) : 0;
    uint64_t vup = found ? (uint64_t)sqlite3_column_int64(st, 1) : 0;
    int linked   = found && sqlite3_column_type(st, 2) != SQLITE_NULL;
    int final    = found && sqlite3_column_int(st, 3);
    int gone     = found && sqlite3_column_int64(st, 4) != 0;
    const unsigned char *vm = found ? sqlite3_column_text(st, 5) : NULL;
    int is_mp4   = vm && strcmp((const char *)vm, "video/mp4") == 0;
    uint64_t vsz = found ? (uint64_t)sqlite3_column_int64(st, 6) : 0;
    sqlite3_finalize(st);
    /* Not the caller's, not there, or not finished: indistinguishable on
     * purpose, as for every other attachment operation. */
    if (!found || vup != j->user_id || !final || gone) { r->err_code = OC_ERR_UNKNOWN_ATTACHMENT; return r; }
    /* Once linked the message is out; its media facts are not rewritten after. */
    if (linked || !is_mp4) { r->err_code = OC_ERR_MEDIA_INVALID; return r; }
    if (vsz > j->att_size)  { r->err_code = OC_ERR_MEDIA_TOO_LARGE; return r; }

    sqlite3_prepare_v2(db,
        "SELECT channel_id, uploader_id, message_id, sha256 IS NOT NULL, reclaimed_at_ms, mime, size, "
        "       EXISTS(SELECT 1 FROM attachment_media WHERE attachment_id=?1) "
        "  FROM attachments WHERE id=?1;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->media_poster_id);
    found = sqlite3_step(st) == SQLITE_ROW;
    int poster_ok = found &&
        (uint64_t)sqlite3_column_int64(st, 0) == vch &&
        (uint64_t)sqlite3_column_int64(st, 1) == j->user_id &&
        sqlite3_column_type(st, 2) == SQLITE_NULL &&           /* a poster is never a message's file */
        sqlite3_column_int(st, 3) &&
        sqlite3_column_int64(st, 4) == 0 &&
        sqlite3_column_text(st, 5) && strcmp((const char *)sqlite3_column_text(st, 5), "image/jpeg") == 0 &&
        (uint64_t)sqlite3_column_int64(st, 6) <= OC_MEDIA_MAX_POSTER_SIZE &&
        sqlite3_column_int(st, 7) == 0;                        /* a video cannot be another's poster */
    sqlite3_finalize(st);
    if (!poster_ok) { r->err_code = OC_ERR_MEDIA_INVALID; return r; }

    sqlite3_prepare_v2(db,
        "INSERT INTO attachment_media(attachment_id, kind, duration_ms, width, height, poster_id, created_at_ms) "
        "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7) "
        "ON CONFLICT(attachment_id) DO UPDATE SET kind=?2, duration_ms=?3, width=?4, height=?5, "
        "  poster_id=?6;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->attachment_id);
    sqlite3_bind_int  (st, 2, (int)j->media_kind);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)j->media_duration_ms);
    sqlite3_bind_int  (st, 4, (int)j->media_width);
    sqlite3_bind_int  (st, 5, (int)j->media_height);
    sqlite3_bind_int64(st, 6, (sqlite3_int64)j->media_poster_id);
    sqlite3_bind_int64(st, 7, (sqlite3_int64)dbw_now_ms());
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) { r->err_code = OC_ERR_INTERNAL; return r; }
    r->type = OC_RES_MEDIA_OK;
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

    /* An AVATAR is readable by every authenticated user, regardless of the
     * channel the image was uploaded to. It has to be: a picture is drawn beside
     * every message its owner wrote, in channels the viewer shares with them but the
     * uploader's own upload channel is not. The exposure is bounded by
     * process_set_avatar, which only accepts an attachment the SETTER uploaded — so
     * this cannot be used to publish somebody else's private file. */
    int is_avatar = 0;
    {
        sqlite3_stmt *av = NULL;
        sqlite3_prepare_v2(db, "SELECT 1 FROM users WHERE avatar_attachment_id=?1;", -1, &av, NULL);
        sqlite3_bind_int64(av, 1, (sqlite3_int64)j->attachment_id);
        is_avatar = (sqlite3_step(av) == SQLITE_ROW);
        sqlite3_finalize(av);
    }
    if (!is_avatar && !channel_read_access(db, cid, j->user_id)) {
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

/* Image previews (ARCH-137). STORE records what a transfer worker made of an
 * upload: its dimensions on `attachments`, and a row per rung. The rows are
 * written only while the attachment is still there and not reclaimed; if it
 * went while its previews were being made, the rungs' keys come back in
 * pv_drop for the net loop to delete, so no preview outlives its original.
 * Write. */
static oc_dbres *process_preview_store(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->attachment_id = j->attachment_id;
    r->type = OC_RES_PREVIEW_STORED;
    sqlite3_stmt *st = NULL;
    int live = 0;
    if (sqlite3_prepare_v2(db, "SELECT reclaimed_at_ms FROM attachments WHERE id=?1;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->attachment_id);
        live = sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int64(st, 0) == 0;
    }
    sqlite3_finalize(st);
    st = NULL;
    if (live && j->pv_width && j->pv_height &&
        sqlite3_prepare_v2(db, "UPDATE attachments SET width=?2, height=?3 WHERE id=?1;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->attachment_id);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)j->pv_width);
        sqlite3_bind_int64(st, 3, (sqlite3_int64)j->pv_height);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
    uint64_t now = dbw_now_ms();
    for (int i = 0; i < j->pv_count && i < 2; i++) {
        int stored = 0;
        st = NULL;
        if (live && j->pv_keys[i] &&
            sqlite3_prepare_v2(db,
                "INSERT OR REPLACE INTO attachment_previews"
                "  (attachment_id, rung, storage_key, width, height, size, mime, created_at_ms, last_used_ms)"
                " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?8);", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)j->attachment_id);
            sqlite3_bind_int  (st, 2, i + 1);
            sqlite3_bind_text (st, 3, j->pv_keys[i], -1, SQLITE_STATIC);
            sqlite3_bind_int64(st, 4, (sqlite3_int64)j->pv_w[i]);
            sqlite3_bind_int64(st, 5, (sqlite3_int64)j->pv_h[i]);
            sqlite3_bind_int64(st, 6, (sqlite3_int64)j->pv_size[i]);
            sqlite3_bind_text (st, 7, j->pv_mime[i] ? j->pv_mime[i] : "", -1, SQLITE_STATIC);
            sqlite3_bind_int64(st, 8, (sqlite3_int64)now);
            stored = sqlite3_step(st) == SQLITE_DONE;
        }
        sqlite3_finalize(st);
        if (!stored && j->pv_keys[i]) r->pv_drop[i] = strdup(j->pv_keys[i]);
    }
    return r;
}

/* PREVIEW_LOOKUP: the download's own gate (process_attach_lookup: the
 * channel's read check, or an avatar's everyone), then the rung asked for. A
 * rung that was never made, or was reclaimed, is PREVIEW_MISSING, which a
 * client answers by downloading the original. Read (query connection). */
static oc_dbres *process_preview_lookup(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = process_attach_lookup(db, j);
    if (!r) return NULL;
    if (r->type != OC_RES_ATTACH_META) {
        r->type = OC_RES_PREVIEW_ERR;
        return r;
    }
    r->pv_rung = j->pv_rung;
    sqlite3_stmt *st = NULL;
    r->type = OC_RES_PREVIEW_ERR;
    r->err_code = OC_ERR_PREVIEW_MISSING;
    if (sqlite3_prepare_v2(db,
            "SELECT storage_key, width, height, size, mime FROM attachment_previews"
            " WHERE attachment_id=?1 AND rung=?2;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->attachment_id);
        sqlite3_bind_int  (st, 2, j->pv_rung);
        if (sqlite3_step(st) == SQLITE_ROW) {
            const char *k = (const char *)sqlite3_column_text(st, 0);
            const char *m = (const char *)sqlite3_column_text(st, 4);
            r->pv_key = strdup(k ? k : "");
            r->pv_w = (uint32_t)sqlite3_column_int64(st, 1);
            r->pv_h = (uint32_t)sqlite3_column_int64(st, 2);
            r->pv_size = (uint64_t)sqlite3_column_int64(st, 3);
            r->pv_mime = strdup(m ? m : "");
            if (r->pv_key && r->pv_mime) { r->type = OC_RES_PREVIEW_META; r->err_code = 0; }
        }
    }
    sqlite3_finalize(st);
    return r;
}

/* PREVIEW_TOUCH: a rung served, so it is the last of its kind reclaimed. Write,
 * fire and forget. */
static oc_dbres *process_preview_touch(sqlite3 *db, const oc_job *j) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "UPDATE attachment_previews SET last_used_ms=?3 WHERE attachment_id=?1 AND rung=?2;",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->attachment_id);
        sqlite3_bind_int  (st, 2, j->pv_rung);
        sqlite3_bind_int64(st, 3, (sqlite3_int64)dbw_now_ms());
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
    oc_dbres *r = calloc(1, sizeof *r);
    if (r) { r->type = OC_RES_OK; r->conn_id = j->conn_id; }
    return r;
}

/* --- The MCP surface's reads (docs/details/MCP.md §4) ----------------------------
 * An agent reads the channels it is a member of and nothing else: no DM, no
 * group DM, no end-to-end channel (the daemon holds only ciphertext), no
 * public channel it has not been put in. Every read checks that
 * here, at the moment it is made. Deleted messages are not returned. */

static char *mcp_dup(sqlite3_stmt *st, int i) {
    const char *t = (const char *)sqlite3_column_text(st, i);
    return strdup(t ? t : "");
}

/* An agent reads its channels; a person through an AI client (`person`) also
 * their DMs and group DMs. Never an encrypted one. */
static int agent_reads(sqlite3 *db, uint64_t channel, uint64_t agent, int person) {
    sqlite3_stmt *st = NULL;
    int ok = 0;
    if (sqlite3_prepare_v2(db,
            "SELECT 1 FROM channels c JOIN channel_members cm ON cm.channel_id=c.id"
            " WHERE c.id=?1 AND cm.user_id=?2 AND (c.kind='channel' OR ?3=1) AND c.e2e=0;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)channel);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)agent);
        sqlite3_bind_int(st, 3, person ? 1 : 0);
        ok = sqlite3_step(st) == SQLITE_ROW;
    }
    sqlite3_finalize(st);
    return ok;
}

static oc_dbres *process_mcp_channels(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->type = OC_RES_MCP_CHANNELS;
    sqlite3_stmt *st = NULL;
    /* A DM or group DM is named by the others in it, by handle, in order:
     * "alice", "alice,@bob" -- shown as "@alice", "@alice,@bob". */
    if (sqlite3_prepare_v2(db,
            "SELECT c.id, CASE WHEN c.kind='dm' THEN"
            "   (SELECT group_concat(h, ',@') FROM (SELECT CASE WHEN substr(u.subject,1,6)='local:' THEN substr(u.subject,7)"
            "      ELSE COALESCE(NULLIF(u.display_name,''), 'user'||u.id) END AS h"
            "     FROM channel_members o JOIN users u ON u.id=o.user_id WHERE o.channel_id=c.id AND o.user_id<>?1 ORDER BY h))"
            "   ELSE c.name END, c.is_public, c.kind='dm'"
            " FROM channels c JOIN channel_members cm ON cm.channel_id=c.id"
            " WHERE cm.user_id=?1 AND (c.kind='channel' OR ?2=1) AND c.e2e=0 ORDER BY c.kind='dm', 2, c.id;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->user_id);
        sqlite3_bind_int(st, 2, j->mcp_person ? 1 : 0);
        while (sqlite3_step(st) == SQLITE_ROW) {
            oc_mcp_chan *g = realloc(r->mcp_chans, (r->n_mcp_chans + 1) * sizeof *g);
            if (!g) break;
            r->mcp_chans = g;
            oc_mcp_chan *c = &r->mcp_chans[r->n_mcp_chans++];
            c->id = (uint64_t)sqlite3_column_int64(st, 0);
            c->name = mcp_dup(st, 1);
            c->is_public = (uint8_t)sqlite3_column_int(st, 2);
            c->is_dm = (uint8_t)sqlite3_column_int(st, 3);
        }
    }
    sqlite3_finalize(st);
    return r;
}

/* The columns every MCP read takes, in this order (mcp_row_fill). */
#define MCP_COLS \
    "m.id, m.channel_id, m.created_at_ms, m.body, (m.edited_at_ms IS NOT NULL)," \
    " (SELECT COUNT(*) FROM messages c WHERE c.parent_id=m.id AND c.deleted_at_ms IS NULL)," \
    " COALESCE(NULLIF(m.author_name,''), u.display_name, '')," \
    " CASE WHEN COALESCE(m.author_name,'') <> '' THEN 'webhook' WHEN u.kind='agent' THEN 'agent' ELSE 'person' END," \
    " CASE WHEN substr(u.subject, 1, 6)='local:' THEN substr(u.subject, 7) ELSE '' END," \
    " ch.name"

static void mcp_row_fill(sqlite3 *db, sqlite3_stmt *st, oc_mcp_row *m) {
    memset(m, 0, sizeof *m);
    m->id = (uint64_t)sqlite3_column_int64(st, 0);
    m->channel_id = (uint64_t)sqlite3_column_int64(st, 1);
    m->at_ms = (uint64_t)sqlite3_column_int64(st, 2);
    const void *b = sqlite3_column_blob(st, 3);
    int bl = sqlite3_column_bytes(st, 3);
    m->text = malloc((size_t)(bl > 0 ? bl : 0) + 1);
    if (m->text) { if (bl > 0) memcpy(m->text, b, (size_t)bl); m->text[bl > 0 ? bl : 0] = '\0'; m->text_len = (size_t)(bl > 0 ? bl : 0); }
    m->edited = (uint8_t)sqlite3_column_int(st, 4);
    m->replies = (uint32_t)sqlite3_column_int64(st, 5);
    m->author = mcp_dup(st, 6);
    m->kind = mcp_dup(st, 7);
    m->handle = mcp_dup(st, 8);
    m->channel = mcp_dup(st, 9);
    sqlite3_stmt *fs = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT filename, mime, size FROM attachments WHERE message_id=?1 AND reclaimed_at_ms=0 ORDER BY id;",
            -1, &fs, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(fs, 1, (sqlite3_int64)m->id);
        while (sqlite3_step(fs) == SQLITE_ROW) {
            size_t k = m->n_files;
            char **gn = realloc(m->file_names, (k + 1) * sizeof *gn);
            if (gn) m->file_names = gn;
            char **gt = realloc(m->file_types, (k + 1) * sizeof *gt);
            if (gt) m->file_types = gt;
            uint64_t *gs = realloc(m->file_sizes, (k + 1) * sizeof *gs);
            if (gs) m->file_sizes = gs;
            if (!gn || !gt || !gs) break;
            m->file_names[k] = mcp_dup(fs, 0);
            m->file_types[k] = mcp_dup(fs, 1);
            m->file_sizes[k] = (uint64_t)sqlite3_column_int64(fs, 2);
            m->n_files++;
        }
    }
    sqlite3_finalize(fs);
}

static int mcp_row_add(oc_dbres *r) {
    oc_mcp_row *g = realloc(r->mcp_rows, (r->n_mcp_rows + 1) * sizeof *g);
    if (!g) return -1;
    r->mcp_rows = g;
    r->n_mcp_rows++;
    return 0;
}

static int mcp_exists(sqlite3 *db, const char *sql, uint64_t a, uint64_t b, uint64_t c) {
    sqlite3_stmt *st = NULL;
    int yes = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)a);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)b);
        sqlite3_bind_int64(st, 3, (sqlite3_int64)c);
        yes = sqlite3_step(st) == SQLITE_ROW;
    }
    sqlite3_finalize(st);
    return yes;
}

/* MCP_READ: a conversation's top-level messages, or one thread's replies,
 * newest first by page unless after or since asks for the other way. */
static oc_dbres *process_mcp_read(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->type = OC_RES_MCP_MESSAGES;
    r->channel_id = j->channel_id;
    if (!agent_reads(db, j->channel_id, j->user_id, j->mcp_person)) { r->err_code = OC_ERR_UNKNOWN_CHANNEL; return r; }
    if (j->mcp_thread &&
        !mcp_exists(db, "SELECT 1 FROM messages WHERE id=?1 AND channel_id=?2 AND parent_id IS NULL AND ?3=?3;",
                    j->mcp_thread, j->channel_id, 0)) {
        r->err_code = OC_ERR_UNKNOWN_MESSAGE;
        return r;
    }
    int forward = j->mcp_has_after || (j->mcp_has_since && !j->mcp_has_before);
    uint16_t lim = j->mcp_limit ? j->mcp_limit : 50;
    char sql[1600];
    snprintf(sql, sizeof sql,
             "SELECT " MCP_COLS " FROM messages m LEFT JOIN users u ON u.id=m.author_id JOIN channels ch ON ch.id=m.channel_id"
             " WHERE m.channel_id=?1 AND m.deleted_at_ms IS NULL AND m.body IS NOT NULL"
             " AND %s"
             " AND NOT EXISTS(SELECT 1 FROM withheld_messages wm WHERE wm.user_id=?6 AND wm.message_id=m.id)"
             " AND (?2=0 OR m.id < ?2) AND (?3=0 OR m.id > ?3) AND (?4=0 OR m.created_at_ms >= ?4)"
             " ORDER BY m.id %s LIMIT ?5;",
             j->mcp_thread ? "m.parent_id=?7" : "(m.parent_id IS NULL OR m.also_channel=1) AND ?7=?7",
             forward ? "ASC" : "DESC");
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) { sqlite3_finalize(st); r->err_code = OC_ERR_INTERNAL; return r; }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->channel_id);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)(j->mcp_has_before ? j->mcp_before : 0));
    sqlite3_bind_int64(st, 3, (sqlite3_int64)(j->mcp_has_after ? j->mcp_after : 0));
    sqlite3_bind_int64(st, 4, (sqlite3_int64)(j->mcp_has_since ? j->mcp_since : 0));
    sqlite3_bind_int64(st, 5, (sqlite3_int64)lim + 1);
    sqlite3_bind_int64(st, 6, (sqlite3_int64)j->user_id);
    sqlite3_bind_int64(st, 7, (sqlite3_int64)j->mcp_thread);
    size_t got = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (got++ == lim) { if (forward) r->mcp_more_newer = 1; else r->mcp_more_older = 1; break; }
        if (mcp_row_add(r) != 0) break;
        mcp_row_fill(db, st, &r->mcp_rows[r->n_mcp_rows - 1]);
    }
    sqlite3_finalize(st);
    /* In id order, whichever way it was read. */
    if (!forward)
        for (size_t a = 0, b = r->n_mcp_rows; a + 1 < b; a++, b--) {
            oc_mcp_row t = r->mcp_rows[a]; r->mcp_rows[a] = r->mcp_rows[b - 1]; r->mcp_rows[b - 1] = t;
        }
    r->mcp_newest_kept = (uint8_t)!forward;
    /* What lies beyond the page's other end: before it when read forwards, after
     * it when read backwards from a cursor. */
    if (r->n_mcp_rows) {
        const char *older = j->mcp_thread
            ? "SELECT 1 FROM messages WHERE channel_id=?1 AND parent_id=?3 AND deleted_at_ms IS NULL AND id < ?2;"
            : "SELECT 1 FROM messages WHERE channel_id=?1 AND (parent_id IS NULL OR also_channel=1) AND deleted_at_ms IS NULL AND id < ?2 AND ?3=?3;";
        const char *newer = j->mcp_thread
            ? "SELECT 1 FROM messages WHERE channel_id=?1 AND parent_id=?3 AND deleted_at_ms IS NULL AND id > ?2;"
            : "SELECT 1 FROM messages WHERE channel_id=?1 AND (parent_id IS NULL OR also_channel=1) AND deleted_at_ms IS NULL AND id > ?2 AND ?3=?3;";
        if (forward) r->mcp_more_older = (uint8_t)mcp_exists(db, older, j->channel_id, r->mcp_rows[0].id, j->mcp_thread);
        else r->mcp_more_newer = (uint8_t)mcp_exists(db, newer, j->channel_id, r->mcp_rows[r->n_mcp_rows - 1].id, j->mcp_thread);
    }
    return r;
}

/* The words of a query as an FTS5 expression: each a quoted phrase, all
 * required, so nothing the caller types is taken as an operator. */
static int mcp_fts_query(const char *q, char *out, size_t cap) {
    size_t o = 0;
    int words = 0;
    while (*q) {
        while (*q == ' ' || *q == '\t' || *q == '\n') q++;
        if (!*q) break;
        if (o + 3 >= cap) return -1;
        if (words++) out[o++] = ' ';
        out[o++] = '"';
        while (*q && *q != ' ' && *q != '\t' && *q != '\n') {
            if (o + 3 >= cap) return -1;
            if (*q == '"') out[o++] = '"';
            out[o++] = *q++;
        }
        out[o++] = '"';
    }
    out[o] = '\0';
    return words ? 0 : -1;
}

static oc_dbres *process_mcp_search(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->type = OC_RES_MCP_MESSAGES;
    if (j->channel_id && !agent_reads(db, j->channel_id, j->user_id, j->mcp_person)) { r->err_code = OC_ERR_UNKNOWN_CHANNEL; return r; }
    char fq[1200];
    if (!j->mcp_query || mcp_fts_query(j->mcp_query, fq, sizeof fq) != 0) return r;
    uint64_t from_uid = 0;
    if (j->mcp_from && j->mcp_from[0]) {
        const char *h = j->mcp_from[0] == '@' ? j->mcp_from + 1 : j->mcp_from;
        sqlite3_stmt *u = NULL;
        if (sqlite3_prepare_v2(db, "SELECT id FROM users WHERE subject='local:'||?1 OR display_name=?1 ORDER BY id LIMIT 1;",
                               -1, &u, NULL) == SQLITE_OK) {
            sqlite3_bind_text(u, 1, h, -1, SQLITE_STATIC);
            if (sqlite3_step(u) == SQLITE_ROW) from_uid = (uint64_t)sqlite3_column_int64(u, 0);
        }
        sqlite3_finalize(u);
        if (!from_uid) return r;                       /* nobody by that name wrote anything */
    }
    uint16_t lim = j->mcp_limit ? j->mcp_limit : 20;
    sqlite3_stmt *st = NULL;
    const char *sql =
        "SELECT * FROM (SELECT " MCP_COLS ", bm25(messages_fts) AS rank"
        "  FROM messages_fts JOIN messages m ON m.id=messages_fts.rowid"
        "  JOIN channels ch ON ch.id=m.channel_id AND (ch.kind='channel' OR ?13=1) AND ch.e2e=0"
        "  JOIN channel_members cm ON cm.channel_id=m.channel_id AND cm.user_id=?2"
        "  LEFT JOIN users u ON u.id=m.author_id"
        " WHERE messages_fts MATCH ?1 AND m.deleted_at_ms IS NULL AND m.body IS NOT NULL"
        "   AND ((ch.is_public=1 AND ?3=1) OR (ch.is_public=0 AND ?4=1))"
        "   AND (?5=0 OR m.channel_id=?5) AND (?6=0 OR m.author_id=?6)"
        "   AND (?7=0 OR m.created_at_ms >= ?7) AND (?8=0 OR m.created_at_ms < ?8))"
        " WHERE (?9=0 OR rank > ?10 OR (rank = ?10 AND id > ?11))"
        " ORDER BY rank, id LIMIT ?12;";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) { sqlite3_finalize(st); r->err_code = OC_ERR_INTERNAL; return r; }
    sqlite3_bind_text (st, 1, fq, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)j->user_id);
    sqlite3_bind_int  (st, 3, j->mcp_pub);
    sqlite3_bind_int  (st, 4, j->mcp_priv);
    sqlite3_bind_int64(st, 5, (sqlite3_int64)j->channel_id);
    sqlite3_bind_int64(st, 6, (sqlite3_int64)from_uid);
    sqlite3_bind_int64(st, 7, (sqlite3_int64)(j->mcp_has_since ? j->mcp_since : 0));
    sqlite3_bind_int64(st, 8, (sqlite3_int64)(j->mcp_has_until ? j->mcp_until : 0));
    sqlite3_bind_int  (st, 9, j->mcp_has_cursor);
    sqlite3_bind_double(st, 10, j->mcp_rank);
    sqlite3_bind_int64(st, 11, (sqlite3_int64)j->mcp_after);
    sqlite3_bind_int64(st, 12, (sqlite3_int64)lim + 1);
    sqlite3_bind_int  (st, 13, j->mcp_person ? 1 : 0);
    size_t got = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (got++ == lim) { r->mcp_more_newer = 1; break; }   /* another page follows */
        if (mcp_row_add(r) != 0) break;
        oc_mcp_row *m = &r->mcp_rows[r->n_mcp_rows - 1];
        mcp_row_fill(db, st, m);
        m->rank = sqlite3_column_double(st, 10);
    }
    sqlite3_finalize(st);
    return r;
}

/* MCP_AUDIT: the row, its detail led by the token's name (token_id), so an
 * owner sees which of an agent's tokens did it; never the token itself. */
static oc_dbres *process_mcp_audit(sqlite3 *db, const oc_job *j) {
    char *nm = j->user_id ? audit_name_of(db, j->user_id) : NULL;
    char tok[128] = "", detail[512];
    if (j->token_id) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(db, "SELECT name FROM principal_tokens WHERE id=?1;", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)j->token_id);
            if (sqlite3_step(st) == SQLITE_ROW)
                snprintf(tok, sizeof tok, "%s", (const char *)sqlite3_column_text(st, 0));
        }
        sqlite3_finalize(st);
    }
    if (tok[0]) snprintf(detail, sizeof detail, "token=%s%s%s", tok, j->mcp_detail ? " " : "", j->mcp_detail ? j->mcp_detail : "");
    else snprintf(detail, sizeof detail, "%s", j->mcp_detail ? j->mcp_detail : "");
    audit_log(db, j->mcp_family ? j->mcp_family : OC_AUDIT_MCP, j->mcp_action ? j->mcp_action : "mcp",
              j->user_id, nm, j->target_user_id, j->mcp_target, j->mcp_outcome, detail[0] ? detail : NULL);
    free(nm);
    oc_dbres *r = calloc(1, sizeof *r);
    if (r) { r->type = OC_RES_OK; r->conn_id = 0; }
    return r;
}

/* --- Agents and their tokens (docs/details/MCP.md §3.1, §6, §7) ---------------
 * An agent is a `users` row of kind 'agent' with a name and nothing of a
 * person's: no password, no identity-provider subject, no session, no device.
 * It acts only through a token: 32 random bytes, shown once as
 * OC_PRINCIPAL_TOKEN_PREFIX and the bytes in base64url, stored as their
 * SHA-256 in `principal_tokens` (migration 0090), which holds the tokens of
 * kind 'agent' and, later, 'bot' rows alone. Everything here is an owner's or
 * an admin's, and audited in the administrative family, never with a token. */

static int user_kind_is(sqlite3 *db, uint64_t uid, const char *kind) {
    sqlite3_stmt *st = NULL;
    int yes = 0;
    if (sqlite3_prepare_v2(db, "SELECT kind=?2 FROM users WHERE id=?1;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)uid);
        sqlite3_bind_text(st, 2, kind, -1, SQLITE_STATIC);
        yes = sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) != 0;
    }
    sqlite3_finalize(st);
    return yes;
}

/* Not a person: a visitor, an agent or a bot. None of them is in a DM, a
 * group DM or a user group. */
static int user_not_person(sqlite3 *db, uint64_t uid) { return !user_kind_is(db, uid, "human"); }

static int actor_manages(sqlite3 *db, uint64_t uid) {
    uint8_t role = OC_ROLE_MEMBER;
    return user_role(db, uid, &role) && !user_disabled(db, uid) && user_kind_is(db, uid, "human") &&
           (role == OC_ROLE_OWNER || role == OC_ROLE_ADMIN);
}

/* A live agent: of kind 'agent' and not removed. */
static int agent_live(sqlite3 *db, uint64_t uid) {
    return uid && user_kind_is(db, uid, "agent") && !user_disabled(db, uid);
}

static oc_dbres *agent_refuse(const oc_job *j, uint16_t code) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->type = OC_RES_AGENT_ERR;
    r->err_code = code;
    return r;
}

/* Mint a token for `agent` into `r` (OC_RES_AGENT_TOKEN): the checks a mint
 * makes -- the scopes in the catalog, at most OC_AGENT_MAX_TOKENS -- and the
 * row. 0, or the error code. The store takes rows of kind 'agent' and 'bot'
 * alone. */
static uint16_t mint_token(sqlite3 *db, uint64_t agent, uint64_t actor, const char *name, const char *scopes,
                           oc_dbres *r) {
    if (!user_kind_is(db, agent, "agent") && !user_kind_is(db, agent, "bot")) return OC_ERR_FORBIDDEN;
    char canon[512];
    if (oc_scopes_normalize(scopes ? scopes : "", scopes ? strlen(scopes) : 0, canon, sizeof canon) != 0)
        return OC_ERR_UNKNOWN_SCOPE;
    if (!name || !name[0] || strlen(name) > OC_MAX_CHANNEL_NAME) return OC_ERR_INVALID_CHANNEL;
    sqlite3_stmt *st = NULL;
    int64_t held = 0;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM principal_tokens WHERE user_id=?1;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)agent);
        if (sqlite3_step(st) == SQLITE_ROW) held = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    if (held >= OC_AGENT_MAX_TOKENS) return OC_ERR_TOO_MANY_TOKENS;
    uint8_t token[OC_SESSION_TOKEN_LEN], hash[OC_SHA256_LEN];
    if (oc_rand_bytes(token, sizeof token) != 0 || oc_sha256(token, sizeof token, hash) != 0) return OC_ERR_INTERNAL;
    st = NULL;
    int rc = SQLITE_ERROR;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO principal_tokens(token_hash, user_id, name, scopes, created_by, created_at_ms)"
            " VALUES(?1, ?2, ?3, ?4, ?5, ?6);", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_blob (st, 1, hash, sizeof hash, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)agent);
        sqlite3_bind_text (st, 3, name, -1, SQLITE_STATIC);
        sqlite3_bind_text (st, 4, canon, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 5, (sqlite3_int64)actor);
        sqlite3_bind_int64(st, 6, (sqlite3_int64)dbw_now_ms());
        rc = sqlite3_step(st);
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) return OC_ERR_INTERNAL;
    r->type = OC_RES_AGENT_TOKEN;
    r->user_id = agent;
    r->message_id = (uint64_t)sqlite3_last_insert_rowid(db);   /* before the audit row's insert */
    r->tok_scopes = strdup(canon);
    memcpy(r->session_token, token, sizeof token);               /* the token, shown once */
    audit_actor(db, OC_AUDIT_ADMIN, "agent.token.mint", actor, agent, name, 1, NULL);
    return 0;
}

static oc_dbres *process_agent_create(sqlite3 *db, const oc_job *j) {
    if (!actor_manages(db, j->user_id)) return agent_refuse(j, OC_ERR_FORBIDDEN);
    if (!j->agent_name || !j->agent_name[0] || strlen(j->agent_name) > OC_MAX_CHANNEL_NAME)
        return agent_refuse(j, OC_ERR_INVALID_CHANNEL);
    char canon[512];
    if (oc_scopes_normalize(j->token_scopes ? j->token_scopes : "", j->token_scopes ? strlen(j->token_scopes) : 0,
                            canon, sizeof canon) != 0)
        return agent_refuse(j, OC_ERR_UNKNOWN_SCOPE);
    uint8_t rnd[16];
    if (oc_rand_bytes(rnd, sizeof rnd) != 0) return agent_refuse(j, OC_ERR_INTERNAL);
    char subject[6 + 2 * sizeof rnd + 1];
    memcpy(subject, "agent:", 6);
    for (size_t i = 0; i < sizeof rnd; i++) snprintf(subject + 6 + 2 * i, 3, "%02x", rnd[i]);
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    sqlite3_exec(db, "SAVEPOINT agent_create;", NULL, NULL, NULL);
    sqlite3_stmt *st = NULL;
    int rc = SQLITE_ERROR;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO users(subject, display_name, role, kind, created_at_ms) VALUES(?1, ?2, 'member', 'agent', ?3);",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text (st, 1, subject, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text (st, 2, j->agent_name, -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 3, (sqlite3_int64)dbw_now_ms());
        rc = sqlite3_step(st);
    }
    sqlite3_finalize(st);
    uint64_t agent = rc == SQLITE_DONE ? (uint64_t)sqlite3_last_insert_rowid(db) : 0;
    uint16_t err = agent ? 0 : OC_ERR_INTERNAL;
    if (!err) {
        audit_actor(db, OC_AUDIT_ADMIN, "agent.create", j->user_id, agent, j->agent_name, 1, NULL);
        err = mint_token(db, agent, j->user_id, j->token_name && j->token_name[0] ? j->token_name : "first",
                         canon, r);
    }
    if (err) {
        sqlite3_exec(db, "ROLLBACK TO agent_create; RELEASE agent_create;", NULL, NULL, NULL);
        r->type = OC_RES_AGENT_ERR;
        r->err_code = err;
        return r;
    }
    sqlite3_exec(db, "RELEASE agent_create;", NULL, NULL, NULL);
    user_rows(db, agent, r);             /* the new row, for every roster */
    return r;
}

static oc_dbres *process_token_mint(sqlite3 *db, const oc_job *j) {
    if (!actor_manages(db, j->user_id)) return agent_refuse(j, OC_ERR_FORBIDDEN);
    /* The store's own rule decides the kind (mint_token): an agent's row or a
     * bot's, and no other. A removed one holds none. */
    if (!j->target_user_id || user_disabled(db, j->target_user_id)) return agent_refuse(j, OC_ERR_FORBIDDEN);
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    uint16_t err = mint_token(db, j->target_user_id, j->user_id, j->token_name, j->token_scopes, r);
    if (err) { r->type = OC_RES_AGENT_ERR; r->err_code = err; }
    return r;
}

/* An agent's tokens into `r` as OC_RES_TOKEN_LIST: never a token or its hash. */
static void token_rows(sqlite3 *db, uint64_t agent, oc_dbres *r) {
    r->type = OC_RES_TOKEN_LIST;
    r->user_id = agent;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT id, name, scopes, created_by, created_at_ms, last_used_at_ms, last_used_from"
            " FROM principal_tokens WHERE user_id=?1 ORDER BY id;", -1, &st, NULL) != SQLITE_OK) {
        sqlite3_finalize(st);
        return;
    }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)agent);
    while (sqlite3_step(st) == SQLITE_ROW) {
        oc_token_row *g = realloc(r->ptokens, (r->n_ptokens + 1) * sizeof *g);
        if (!g) break;
        r->ptokens = g;
        oc_token_row *t = &r->ptokens[r->n_ptokens++];
        t->id = (uint64_t)sqlite3_column_int64(st, 0);
        t->name = strdup((const char *)sqlite3_column_text(st, 1));
        t->scopes = strdup((const char *)sqlite3_column_text(st, 2));
        t->created_by = (uint64_t)sqlite3_column_int64(st, 3);
        t->created_at_ms = (uint64_t)sqlite3_column_int64(st, 4);
        t->last_used_at_ms = (uint64_t)sqlite3_column_int64(st, 5);
        t->last_used_from = strdup((const char *)sqlite3_column_text(st, 6));
    }
    sqlite3_finalize(st);
}

static oc_dbres *process_token_list(sqlite3 *db, const oc_job *j) {
    if (!actor_manages(db, j->user_id)) return agent_refuse(j, OC_ERR_FORBIDDEN);
    if (!agent_live(db, j->target_user_id)) return agent_refuse(j, OC_ERR_FORBIDDEN);
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    token_rows(db, j->target_user_id, r);
    return r;
}

static oc_dbres *process_token_revoke(sqlite3 *db, const oc_job *j) {
    if (!actor_manages(db, j->user_id)) return agent_refuse(j, OC_ERR_FORBIDDEN);
    if (!agent_live(db, j->target_user_id)) return agent_refuse(j, OC_ERR_FORBIDDEN);
    char *name = NULL;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT name FROM principal_tokens WHERE id=?1 AND user_id=?2;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->token_id);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)j->target_user_id);
        if (sqlite3_step(st) == SQLITE_ROW) name = strdup((const char *)sqlite3_column_text(st, 0));
    }
    sqlite3_finalize(st);
    if (!name) return agent_refuse(j, OC_ERR_FORBIDDEN);
    st = NULL;
    if (sqlite3_prepare_v2(db, "DELETE FROM principal_tokens WHERE id=?1;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->token_id);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
    audit_actor(db, OC_AUDIT_ADMIN, "agent.token.revoke", j->user_id, j->target_user_id, name, 1, NULL);
    free(name);
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    token_rows(db, j->target_user_id, r);
    return r;
}

/* Removing an agent: its tokens and its channel memberships in one
 * transaction, its row marked removed; its messages stay under its name. The
 * channels it left are told as for a member leaving. */
static oc_dbres *process_agent_remove(sqlite3 *db, const oc_job *j) {
    if (!actor_manages(db, j->user_id)) return agent_refuse(j, OC_ERR_FORBIDDEN);
    if (!agent_live(db, j->target_user_id)) return agent_refuse(j, OC_ERR_FORBIDDEN);
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    sqlite3_exec(db, "SAVEPOINT agent_remove;", NULL, NULL, NULL);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "DELETE FROM principal_tokens WHERE user_id=?1;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->target_user_id);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
    st = NULL;
    uint64_t chans[512];
    size_t nch = 0;
    if (sqlite3_prepare_v2(db, "SELECT channel_id FROM channel_members WHERE user_id=?1;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->target_user_id);
        while (nch < sizeof chans / sizeof chans[0] && sqlite3_step(st) == SQLITE_ROW)
            chans[nch++] = (uint64_t)sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    st = NULL;
    if (sqlite3_prepare_v2(db, "DELETE FROM channel_members WHERE user_id=?1;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->target_user_id);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
    for (size_t i = 0; i < nch; i++) note_change(db, r, chans[i], j->target_user_id, 0);
    st = NULL;
    if (sqlite3_prepare_v2(db, "UPDATE users SET disabled=1 WHERE id=?1;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->target_user_id);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
    audit_actor(db, OC_AUDIT_ADMIN, "agent.remove", j->user_id, j->target_user_id, NULL, 1, NULL);
    sqlite3_exec(db, "RELEASE agent_remove;", NULL, NULL, NULL);
    r->type = OC_RES_USER_UPDATED;
    r->user_id = j->target_user_id;
    r->role = OC_ROLE_MEMBER;
    r->disabled = 1;
    return r;
}

/* The lookup a request authenticates with: the presented token's agent and
 * scopes, in one indexed probe, never cached, so a revoked token or one whose
 * agent is gone is refused at once. Its last use is written at most once a
 * minute, on this writer's clock. */
#define TOKEN_USE_EVERY_MS 60000u
static oc_dbres *process_token_lookup(sqlite3 *db, const oc_job *j) {
    size_t plen = strlen(OC_PRINCIPAL_TOKEN_PREFIX);
    uint8_t raw[OC_SESSION_TOKEN_LEN + 4], hash[OC_SHA256_LEN];
    if (!j->token || j->token_len <= plen || memcmp(j->token, OC_PRINCIPAL_TOKEN_PREFIX, plen) != 0 ||
        oc_base64url_decode(j->token + plen, j->token_len - plen, raw, sizeof raw) != OC_SESSION_TOKEN_LEN ||
        oc_sha256(raw, OC_SESSION_TOKEN_LEN, hash) != 0)
        return agent_refuse(j, OC_ERR_AUTH_INVALID_TOKEN);
    sqlite3_stmt *st = NULL;
    oc_dbres *r = NULL;
    uint64_t last = 0, id = 0;
    if (sqlite3_prepare_v2(db,
            "SELECT t.id, t.user_id, t.scopes, t.last_used_at_ms FROM principal_tokens t JOIN users u ON u.id=t.user_id"
            " WHERE t.token_hash=?1 AND u.disabled=0 AND u.kind IN ('agent','bot');", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_blob(st, 1, hash, sizeof hash, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW && (r = calloc(1, sizeof *r))) {
            r->conn_id = j->conn_id;
            r->type = OC_RES_TOKEN_PRINCIPAL;
            id = (uint64_t)sqlite3_column_int64(st, 0);
            r->message_id = id;
            r->user_id = (uint64_t)sqlite3_column_int64(st, 1);
            r->tok_scopes = strdup((const char *)sqlite3_column_text(st, 2));
            last = (uint64_t)sqlite3_column_int64(st, 3);
        }
    }
    sqlite3_finalize(st);
    if (!r) return agent_refuse(j, OC_ERR_AUTH_INVALID_TOKEN);
    uint64_t now = dbw_now_ms();
    if (now >= last + TOKEN_USE_EVERY_MS) {
        st = NULL;
        if (sqlite3_prepare_v2(db, "UPDATE principal_tokens SET last_used_at_ms=?2, last_used_from=?3 WHERE id=?1;",
                               -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)id);
            sqlite3_bind_int64(st, 2, (sqlite3_int64)now);
            sqlite3_bind_text (st, 3, j->token_from ? j->token_from : "", -1, SQLITE_STATIC);
            sqlite3_step(st);
        }
        sqlite3_finalize(st);
    }
    return r;
}

/* --- A person's AI clients (docs/details/AUTH.md §8.13; SCHEMA.md 0091) -------
 * The daemon as the MCP endpoint's authorization server. A client identifies
 * itself (registered, or by its metadata document); an authorize request waits
 * as an approval with an eight-letter code until the person enters that code
 * in their signed-in app and approves or declines; the page collects the
 * outcome and hands the client a one-minute authorization code; the token
 * endpoint exchanges it, with PKCE, for an hour's access token and a refresh
 * token for the connection's lifetime. Every secret is kept as its SHA-256. */

static oc_dbres *oa_res(const oc_job *j, int type) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->type = type;
    return r;
}

/* A refusal: `oauth` the OAuth error code (the token endpoint's) or the page's
 * sentence; `code` the reason a person's app is told. */
static oc_dbres *oa_refuse(const oc_job *j, const char *oauth, uint16_t code) {
    oc_dbres *r = oa_res(j, OC_RES_OAUTH_ERR);
    if (!r) return NULL;
    r->oa_error = strdup(oauth ? oauth : "");
    r->err_code = code;
    return r;
}

static void oa_bind_hash(sqlite3_stmt *st, int i, const char *secret) {
    uint8_t h[32];
    if (!secret || oc_oauth_hash(secret, strlen(secret), h) != 0) memset(h, 0, sizeof h);
    sqlite3_bind_blob(st, i, h, sizeof h, SQLITE_TRANSIENT);
}

static char *oa_col(sqlite3_stmt *st, int i) {
    const char *t = (const char *)sqlite3_column_text(st, i);
    return strdup(t ? t : "");
}

/* Whether `uid` may connect an AI client as themself: a person (not a
 * visitor, an agent or the workspace), not removed. */
static int oa_person(sqlite3 *db, uint64_t uid) {
    sqlite3_stmt *st = NULL;
    int ok = 0;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM users WHERE id=?1 AND kind='human' AND disabled=0 AND is_system=0;",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)uid);
        ok = sqlite3_step(st) == SQLITE_ROW;
    }
    sqlite3_finalize(st);
    return ok;
}

static oc_dbres *process_oauth_register(sqlite3 *db, const oc_job *j) {
    char id[OC_OAUTH_TOKEN_MAX];
    if (!j->oa_name || !j->oa_uris || oc_oauth_mint(OC_OAUTH_CLIENT_PREFIX, id, sizeof id) != 0)
        return oa_refuse(j, "server_error", OC_ERR_INTERNAL);
    sqlite3_stmt *st = NULL;
    int ok = 0;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO oauth_clients(client_id, kind, name, redirect_uris, created_at_ms, created_from)"
            " VALUES(?1, 'registered', ?2, ?3, ?4, ?5);", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, j->oa_name, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 3, j->oa_uris, -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 4, (sqlite3_int64)dbw_now_ms());
        sqlite3_bind_text(st, 5, j->token_from ? j->token_from : "", -1, SQLITE_STATIC);
        ok = sqlite3_step(st) == SQLITE_DONE;
    }
    sqlite3_finalize(st);
    if (!ok) return oa_refuse(j, "server_error", OC_ERR_INTERNAL);
    oc_dbres *r = oa_res(j, OC_RES_OAUTH_CLIENT);
    if (r) { r->oa_client_id = strdup(id); r->oa_name = strdup(j->oa_name); r->oa_uris = strdup(j->oa_uris); }
    return r;
}

static oc_dbres *process_oauth_doc_store(sqlite3 *db, const oc_job *j) {
    /* A fetch that failed comes here too, its sentence in oa_state, so the
     * request waiting on it is answered in order. */
    if (!j->oa_client_id || !j->oa_name || !j->oa_uris)
        return oa_refuse(j, j->oa_state ? j->oa_state : "The client's metadata document could not be read.", OC_ERR_INTERNAL);
    sqlite3_stmt *st = NULL;
    uint64_t now = dbw_now_ms();
    int ok = 0;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO oauth_clients(client_id, kind, name, redirect_uris, created_at_ms, fetched_at_ms)"
            " VALUES(?1, 'document', ?2, ?3, ?4, ?4)"
            " ON CONFLICT(client_id) DO UPDATE SET name=excluded.name, redirect_uris=excluded.redirect_uris,"
            "   fetched_at_ms=excluded.fetched_at_ms WHERE kind='document';", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, j->oa_client_id, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, j->oa_name, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 3, j->oa_uris, -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 4, (sqlite3_int64)now);
        ok = sqlite3_step(st) == SQLITE_DONE;
    }
    sqlite3_finalize(st);
    if (!ok) return oa_refuse(j, "server_error", OC_ERR_INTERNAL);
    oc_dbres *r = oa_res(j, OC_RES_OAUTH_CLIENT);
    if (r) { r->oa_client_id = strdup(j->oa_client_id); r->oa_name = strdup(j->oa_name); r->oa_uris = strdup(j->oa_uris); }
    return r;
}

/* An authorize request, checked by the loop, against its client: a document
 * not yet fetched or a day old is fetched first; a redirect URI the client
 * did not declare is refused. Then a pending approval with a new handle and
 * code, ten minutes long. */
static oc_dbres *process_oauth_begin(sqlite3 *db, const oc_job *j) {
    if (!j->oa_client_id || !j->oa_redirect || !j->oa_challenge || !j->oa_state || !j->oa_resource)
        return oa_refuse(j, "The request is incomplete.", OC_ERR_INTERNAL);
    uint64_t now = dbw_now_ms();
    /* Approvals an hour past their end are of no more use to anyone. */
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "DELETE FROM oauth_approvals WHERE expires_at_ms < ?1;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)(now > 3600000u ? now - 3600000u : 0));
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
    st = NULL;
    char *name = NULL, *uris = NULL, *kind = NULL;
    uint64_t fetched = 0;
    if (sqlite3_prepare_v2(db, "SELECT name, redirect_uris, kind, fetched_at_ms FROM oauth_clients WHERE client_id=?1;",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, j->oa_client_id, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) {
            name = oa_col(st, 0); uris = oa_col(st, 1); kind = oa_col(st, 2);
            fetched = (uint64_t)sqlite3_column_int64(st, 3);
        }
    }
    sqlite3_finalize(st);
    int is_doc = oc_oauth_client_id_is_url(j->oa_client_id, 1);
    if (is_doc && (!name || (kind && strcmp(kind, "document") == 0 && now >= fetched + OC_OAUTH_DOC_TTL_MS))) {
        free(name); free(uris); free(kind);
        return oa_res(j, OC_RES_OAUTH_NEED_FETCH);
    }
    free(kind);
    if (!name || !uris) {
        free(name); free(uris);
        return oa_refuse(j, "This workspace does not know the client that sent you here.", OC_ERR_INTERNAL);
    }
    if (!oc_oauth_uris_has(uris, j->oa_redirect)) {
        free(name); free(uris);
        return oa_refuse(j, "The client asked to return somewhere it did not declare.", OC_ERR_INTERNAL);
    }
    free(uris);
    char request[OC_OAUTH_TOKEN_MAX], code[OC_OAUTH_CODE_LEN + 1];
    int ok = 0;
    for (int tries = 0; tries < 4 && !ok; tries++) {
        if (oc_oauth_mint(OC_OAUTH_REQUEST_PREFIX, request, sizeof request) != 0 || oc_oauth_approval_code(code) != 0) break;
        st = NULL;
        if (sqlite3_prepare_v2(db,
                "INSERT INTO oauth_approvals(request_hash, code_hash, client_id, client_name, redirect_uri, code_challenge,"
                " state, resource, created_at_ms, expires_at_ms, created_from)"
                " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11);", -1, &st, NULL) == SQLITE_OK) {
            oa_bind_hash(st, 1, request);
            oa_bind_hash(st, 2, code);
            sqlite3_bind_text(st, 3, j->oa_client_id, -1, SQLITE_STATIC);
            sqlite3_bind_text(st, 4, name, -1, SQLITE_STATIC);
            sqlite3_bind_text(st, 5, j->oa_redirect, -1, SQLITE_STATIC);
            sqlite3_bind_text(st, 6, j->oa_challenge, -1, SQLITE_STATIC);
            sqlite3_bind_text(st, 7, j->oa_state, -1, SQLITE_STATIC);
            sqlite3_bind_text(st, 8, j->oa_resource, -1, SQLITE_STATIC);
            sqlite3_bind_int64(st, 9, (sqlite3_int64)now);
            sqlite3_bind_int64(st, 10, (sqlite3_int64)(now + OC_OAUTH_PENDING_TTL_MS));
            sqlite3_bind_text(st, 11, j->token_from ? j->token_from : "", -1, SQLITE_STATIC);
            ok = sqlite3_step(st) == SQLITE_DONE;   /* a code taken already: another try */
        }
        sqlite3_finalize(st);
    }
    if (!ok) { free(name); return oa_refuse(j, "The workspace could not take the request just now.", OC_ERR_INTERNAL); }
    oc_dbres *r = oa_res(j, OC_RES_OAUTH_PAGE);
    if (!r) { free(name); return NULL; }
    r->oa_page = OC_OAUTH_PAGE_PENDING;
    r->oa_name = name;
    r->oa_request = strdup(request);
    r->oa_code = strdup(code);
    return r;
}

/* The page reloading itself, by its handle: still pending (the client's name;
 * the code, kept hashed only, is the page's to carry in its reload), expired,
 * or acted on -- collected once, as a redirect with an authorization code or
 * access_denied. */
static oc_dbres *process_oauth_poll(sqlite3 *db, const oc_job *j) {
    sqlite3_stmt *st = NULL;
    uint64_t now = dbw_now_ms();
    oc_dbres *r = NULL;
    if (!j->oa_request) return oa_refuse(j, "This request is not one this workspace made.", OC_ERR_INTERNAL);
    if (sqlite3_prepare_v2(db,
            "SELECT id, status, expires_at_ms, client_name, redirect_uri, state FROM oauth_approvals WHERE request_hash=?1;",
            -1, &st, NULL) != SQLITE_OK) {
        sqlite3_finalize(st);
        return oa_refuse(j, "The workspace could not look the request up.", OC_ERR_INTERNAL);
    }
    oa_bind_hash(st, 1, j->oa_request);
    if (sqlite3_step(st) != SQLITE_ROW) {
        sqlite3_finalize(st);
        return oa_refuse(j, "This request is not one this workspace made, or it is over.", OC_ERR_INTERNAL);
    }
    uint64_t id = (uint64_t)sqlite3_column_int64(st, 0);
    int status = sqlite3_column_int(st, 1);
    uint64_t expires = (uint64_t)sqlite3_column_int64(st, 2);
    char *name = oa_col(st, 3), *redirect = oa_col(st, 4), *state = oa_col(st, 5);
    sqlite3_finalize(st);
    if (status == 0 && now < expires) {
        r = oa_res(j, OC_RES_OAUTH_PAGE);
        if (r) { r->oa_page = OC_OAUTH_PAGE_PENDING; r->oa_name = name; name = NULL; }
    } else if (status == 1 || status == 2) {
        /* Collected once: the next reload finds it over. */
        char code[OC_OAUTH_TOKEN_MAX] = "";
        int ok = status == 2 || oc_oauth_mint(OC_OAUTH_CODE_PREFIX, code, sizeof code) == 0;
        st = NULL;
        if (ok && sqlite3_prepare_v2(db,
                "UPDATE oauth_approvals SET status=3, auth_code_hash=?2, auth_code_until=?3 WHERE id=?1 AND status IN (1,2);",
                -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)id);
            if (status == 1) oa_bind_hash(st, 2, code);
            else sqlite3_bind_null(st, 2);
            sqlite3_bind_int64(st, 3, (sqlite3_int64)(status == 1 ? now + OC_OAUTH_CODE_TTL_MS : 0));
            ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db) == 1;
        } else ok = 0;
        sqlite3_finalize(st);
        if (!ok) r = oa_refuse(j, "The workspace could not finish the request.", OC_ERR_INTERNAL);
        else if ((r = oa_res(j, OC_RES_OAUTH_REDIRECT))) {
            r->oa_redirect = redirect; redirect = NULL;
            r->oa_state = state; state = NULL;
            if (status == 1) r->oa_code = strdup(code);
            else r->oa_error = strdup("access_denied");
        }
    } else if (status == 0) {
        r = oa_res(j, OC_RES_OAUTH_PAGE);
        if (r) r->oa_page = OC_OAUTH_PAGE_EXPIRED;
    } else {
        r = oa_refuse(j, "This request is over: start again from the AI client.", OC_ERR_INTERNAL);
    }
    free(name); free(redirect); free(state);
    return r;
}

/* The approval a person's code names, pending and in time: its id, the
 * client's name and where it returns. 0, or -1. */
static int oa_pending(sqlite3 *db, const char *typed, uint64_t *id, char **name, char **redirect) {
    char code[OC_OAUTH_CODE_LEN + 1];
    if (!typed || oc_oauth_code_normalize(typed, code) != 0) return -1;
    sqlite3_stmt *st = NULL;
    int found = 0;
    if (sqlite3_prepare_v2(db,
            "SELECT id, client_name, redirect_uri FROM oauth_approvals WHERE code_hash=?1 AND status=0 AND expires_at_ms > ?2;",
            -1, &st, NULL) == SQLITE_OK) {
        oa_bind_hash(st, 1, code);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)dbw_now_ms());
        if (sqlite3_step(st) == SQLITE_ROW) {
            *id = (uint64_t)sqlite3_column_int64(st, 0);
            if (name) *name = oa_col(st, 1);
            if (redirect) *redirect = oa_col(st, 2);
            found = 1;
        }
    }
    sqlite3_finalize(st);
    return found ? 0 : -1;
}

static oc_dbres *process_oauth_code_find(sqlite3 *db, const oc_job *j) {
    if (!oa_person(db, j->user_id)) return oa_refuse(j, NULL, OC_ERR_FORBIDDEN);
    uint64_t id = 0;
    char *name = NULL, *redirect = NULL;
    if (oa_pending(db, j->oa_code, &id, &name, &redirect) != 0) return oa_refuse(j, NULL, OC_ERR_UNKNOWN_CODE);
    oc_dbres *r = oa_res(j, OC_RES_OAUTH_APPROVAL);
    if (!r) { free(name); free(redirect); return NULL; }
    r->oa_name = name;
    r->oa_redirect = redirect;
    r->oa_code = strdup(j->oa_code);
    return r;
}

static oc_dbres *process_oauth_decide(sqlite3 *db, const oc_job *j) {
    if (!oa_person(db, j->user_id)) return oa_refuse(j, NULL, OC_ERR_FORBIDDEN);
    if (j->oa_approve && !oc_oauth_lifetime_ok(j->oa_lifetime)) return oa_refuse(j, NULL, OC_ERR_BAD_LIFETIME);
    uint64_t id = 0;
    char *name = NULL;
    if (oa_pending(db, j->oa_code, &id, &name, NULL) != 0) return oa_refuse(j, NULL, OC_ERR_UNKNOWN_CODE);
    sqlite3_stmt *st = NULL;
    int ok = 0;
    if (sqlite3_prepare_v2(db, "UPDATE oauth_approvals SET status=?2, approved_by=?3, lifetime_days=?4 WHERE id=?1 AND status=0;",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)id);
        sqlite3_bind_int(st, 2, j->oa_approve ? 1 : 2);
        sqlite3_bind_int64(st, 3, (sqlite3_int64)j->user_id);
        sqlite3_bind_int64(st, 4, (sqlite3_int64)(j->oa_approve ? j->oa_lifetime : 0));
        ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db) == 1;
    }
    sqlite3_finalize(st);
    if (!ok) { free(name); return oa_refuse(j, NULL, OC_ERR_UNKNOWN_CODE); }
    char detail[64];
    if (j->oa_lifetime) snprintf(detail, sizeof detail, "lifetime=%u days", j->oa_lifetime);
    else snprintf(detail, sizeof detail, "lifetime=never");
    audit_actor(db, OC_AUDIT_ACCOUNT, j->oa_approve ? "mcp.connection_approve" : "mcp.connection_decline",
                j->user_id, 0, name, 1, j->oa_approve ? detail : NULL);
    oc_dbres *r = oa_res(j, OC_RES_OAUTH_DECIDED);
    if (r) { r->oa_name = name; name = NULL; r->oa_approve_echo = j->oa_approve; }
    free(name);
    return r;
}

/* A new access token for connection `conn`, its refresh token rotated: both
 * into `r` (shown once), only their hashes kept. 0, or -1. */
static int oa_issue(sqlite3 *db, uint64_t conn, oc_dbres *r) {
    char access[OC_OAUTH_TOKEN_MAX], refresh[OC_OAUTH_TOKEN_MAX];
    if (oc_oauth_mint(OC_OAUTH_ACCESS_PREFIX, access, sizeof access) != 0 ||
        oc_oauth_mint(OC_OAUTH_REFRESH_PREFIX, refresh, sizeof refresh) != 0)
        return -1;
    uint64_t now = dbw_now_ms();
    sqlite3_stmt *st = NULL;
    int ok = 0;
    if (sqlite3_prepare_v2(db, "DELETE FROM mcp_access_tokens WHERE connection_id=?1 AND expires_at_ms <= ?2;",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)conn);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)now);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
    st = NULL;
    if (sqlite3_prepare_v2(db, "INSERT INTO mcp_access_tokens(token_hash, connection_id, expires_at_ms) VALUES(?1, ?2, ?3);",
                           -1, &st, NULL) == SQLITE_OK) {
        oa_bind_hash(st, 1, access);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)conn);
        sqlite3_bind_int64(st, 3, (sqlite3_int64)(now + OC_OAUTH_ACCESS_TTL_MS));
        ok = sqlite3_step(st) == SQLITE_DONE;
    }
    sqlite3_finalize(st);
    st = NULL;
    if (ok && sqlite3_prepare_v2(db, "UPDATE mcp_connections SET refresh_hash=?2 WHERE id=?1;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)conn);
        oa_bind_hash(st, 2, refresh);
        ok = sqlite3_step(st) == SQLITE_DONE;
    } else ok = 0;
    sqlite3_finalize(st);
    if (!ok) return -1;
    r->oa_access = strdup(access);
    r->oa_refresh = strdup(refresh);
    return r->oa_access && r->oa_refresh ? 0 : -1;
}

static oc_dbres *process_oauth_exchange(sqlite3 *db, const oc_job *j) {
    if (!j->oa_code || !j->oa_verifier || !j->oa_redirect || !j->oa_client_id) return oa_refuse(j, "invalid_request", 0);
    sqlite3_stmt *st = NULL;
    uint64_t now = dbw_now_ms();
    uint64_t id = 0, user = 0, until = 0;
    uint32_t days = 0;
    char *client = NULL, *name = NULL, *redirect = NULL, *challenge = NULL, *resource = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT id, approved_by, lifetime_days, auth_code_until, client_id, client_name, redirect_uri, code_challenge, resource"
            " FROM oauth_approvals WHERE auth_code_hash=?1 AND status=3;", -1, &st, NULL) == SQLITE_OK) {
        oa_bind_hash(st, 1, j->oa_code);
        if (sqlite3_step(st) == SQLITE_ROW) {
            id = (uint64_t)sqlite3_column_int64(st, 0);
            user = (uint64_t)sqlite3_column_int64(st, 1);
            days = (uint32_t)sqlite3_column_int64(st, 2);
            until = (uint64_t)sqlite3_column_int64(st, 3);
            client = oa_col(st, 4); name = oa_col(st, 5); redirect = oa_col(st, 6);
            challenge = oa_col(st, 7); resource = oa_col(st, 8);
        }
    }
    sqlite3_finalize(st);
    if (!id) return oa_refuse(j, "invalid_grant", 0);
    /* Single use: whatever happens next, this code is spent. */
    st = NULL;
    if (sqlite3_prepare_v2(db, "UPDATE oauth_approvals SET auth_code_hash=NULL WHERE id=?1;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)id);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
    oc_dbres *r = NULL;
    if (now > until || strcmp(client, j->oa_client_id) != 0 || strcmp(redirect, j->oa_redirect) != 0 ||
        (j->oa_resource && j->oa_resource[0] && strcmp(j->oa_resource, resource) != 0) ||
        !oc_oauth_pkce_ok(j->oa_verifier, challenge) || !oa_person(db, user)) {
        r = oa_refuse(j, "invalid_grant", 0);
    } else {
        sqlite3_exec(db, "SAVEPOINT oauth_exchange;", NULL, NULL, NULL);
        st = NULL;
        uint64_t conn = 0;
        if (sqlite3_prepare_v2(db,
                "INSERT INTO mcp_connections(user_id, client_id, client_name, resource, lifetime_days, expires_at_ms, created_at_ms)"
                " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7);", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)user);
            sqlite3_bind_text(st, 2, client, -1, SQLITE_STATIC);
            sqlite3_bind_text(st, 3, name, -1, SQLITE_STATIC);
            sqlite3_bind_text(st, 4, resource, -1, SQLITE_STATIC);
            sqlite3_bind_int64(st, 5, (sqlite3_int64)days);
            sqlite3_bind_int64(st, 6, (sqlite3_int64)(days ? now + (uint64_t)days * 86400000u : 0));
            sqlite3_bind_int64(st, 7, (sqlite3_int64)now);
            if (sqlite3_step(st) == SQLITE_DONE) conn = (uint64_t)sqlite3_last_insert_rowid(db);
        }
        sqlite3_finalize(st);
        r = oa_res(j, OC_RES_OAUTH_TOKENS);
        if (r && (!conn || oa_issue(db, conn, r) != 0)) {
            sqlite3_exec(db, "ROLLBACK TO oauth_exchange;", NULL, NULL, NULL);
            oc_dbres_free(r);
            r = oa_refuse(j, "server_error", 0);
        }
        sqlite3_exec(db, "RELEASE oauth_exchange;", NULL, NULL, NULL);
    }
    free(client); free(name); free(redirect); free(challenge); free(resource);
    return r;
}

static oc_dbres *process_oauth_refresh(sqlite3 *db, const oc_job *j) {
    if (!j->oa_refresh || !j->oa_client_id) return oa_refuse(j, "invalid_request", 0);
    sqlite3_stmt *st = NULL;
    uint64_t now = dbw_now_ms();
    uint64_t conn = 0, user = 0, expires = 0, revoked = 0;
    char *client = NULL, *resource = NULL, *name = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT id, user_id, expires_at_ms, revoked_at_ms, client_id, resource, client_name FROM mcp_connections"
            " WHERE refresh_hash=?1;", -1, &st, NULL) == SQLITE_OK) {
        oa_bind_hash(st, 1, j->oa_refresh);
        if (sqlite3_step(st) == SQLITE_ROW) {
            conn = (uint64_t)sqlite3_column_int64(st, 0);
            user = (uint64_t)sqlite3_column_int64(st, 1);
            expires = (uint64_t)sqlite3_column_int64(st, 2);
            revoked = (uint64_t)sqlite3_column_int64(st, 3);
            client = oa_col(st, 4); resource = oa_col(st, 5); name = oa_col(st, 6);
        }
    }
    sqlite3_finalize(st);
    const char *why = !conn ? "unknown" : revoked ? "revoked" : (expires && now >= expires) ? "expired"
                    : strcmp(client, j->oa_client_id) != 0 ? "another client"
                    : (j->oa_resource && j->oa_resource[0] && strcmp(j->oa_resource, resource) != 0) ? "another resource"
                    : !oa_person(db, user) ? "removed" : NULL;
    oc_dbres *r = NULL;
    if (why) {
        /* Refused: audited, the person as actor when the token was theirs. */
        char detail[96];
        snprintf(detail, sizeof detail, "refresh refused: %s", why);
        if (user) audit_actor(db, OC_AUDIT_SECURITY, "mcp.refresh_refused", user, 0, name, 0, detail);
        else audit_log(db, OC_AUDIT_SECURITY, "mcp.refresh_refused", 0, NULL, 0, NULL, 0, detail);
        r = oa_refuse(j, "invalid_grant", 0);
    } else {
        r = oa_res(j, OC_RES_OAUTH_TOKENS);
        if (r && oa_issue(db, conn, r) != 0) { oc_dbres_free(r); r = oa_refuse(j, "server_error", 0); }
    }
    free(client); free(resource); free(name);
    return r;
}

/* The lookup a person's access token authenticates with at /mcp: the token in
 * time, its connection neither revoked nor past its lifetime, its person a
 * person still. Never cached. The connection's last use is written at most
 * once a minute. */
static oc_dbres *process_oauth_lookup(sqlite3 *db, const oc_job *j) {
    if (!j->token || oc_oauth_token_of(j->token, j->token_len) != OC_OAUTH_TOKEN_PERSON) return agent_refuse(j, OC_ERR_AUTH_INVALID_TOKEN);
    sqlite3_stmt *st = NULL;
    uint64_t now = dbw_now_ms(), conn = 0, last = 0;
    oc_dbres *r = NULL;
    uint8_t h[32];
    if (oc_oauth_hash(j->token, j->token_len, h) != 0) return agent_refuse(j, OC_ERR_AUTH_INVALID_TOKEN);
    if (sqlite3_prepare_v2(db,
            "SELECT c.id, c.user_id, c.client_name, c.last_used_at_ms FROM mcp_access_tokens t"
            " JOIN mcp_connections c ON c.id=t.connection_id JOIN users u ON u.id=c.user_id"
            " WHERE t.token_hash=?1 AND t.expires_at_ms > ?2 AND c.revoked_at_ms=0"
            "   AND (c.expires_at_ms=0 OR c.expires_at_ms > ?2) AND u.kind='human' AND u.disabled=0;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_blob(st, 1, h, sizeof h, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)now);
        if (sqlite3_step(st) == SQLITE_ROW && (r = oa_res(j, OC_RES_TOKEN_PRINCIPAL))) {
            conn = (uint64_t)sqlite3_column_int64(st, 0);
            r->message_id = conn;
            r->user_id = (uint64_t)sqlite3_column_int64(st, 1);
            r->oa_name = oa_col(st, 2);
            r->oa_person = 1;
            last = (uint64_t)sqlite3_column_int64(st, 3);
        }
    }
    sqlite3_finalize(st);
    if (!r) return agent_refuse(j, OC_ERR_AUTH_INVALID_TOKEN);
    if (now >= last + TOKEN_USE_EVERY_MS) {
        st = NULL;
        if (sqlite3_prepare_v2(db, "UPDATE mcp_connections SET last_used_at_ms=?2, last_used_from=?3 WHERE id=?1;",
                               -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)conn);
            sqlite3_bind_int64(st, 2, (sqlite3_int64)now);
            sqlite3_bind_text (st, 3, j->token_from ? j->token_from : "", -1, SQLITE_STATIC);
            sqlite3_step(st);
        }
        sqlite3_finalize(st);
    }
    return r;
}

/* The connections still in force: the caller's own, or with `all` (an owner
 * or admin) everyone's. */
static void oa_list_into(sqlite3 *db, oc_dbres *r, uint64_t uid, int all) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT c.id, c.user_id, COALESCE(u.display_name, u.subject), c.client_name, c.created_at_ms, c.last_used_at_ms,"
            " c.expires_at_ms, c.lifetime_days FROM mcp_connections c JOIN users u ON u.id=c.user_id"
            " WHERE c.revoked_at_ms=0 AND (c.expires_at_ms=0 OR c.expires_at_ms > ?2) AND (?3=1 OR c.user_id=?1)"
            " ORDER BY c.created_at_ms DESC, c.id DESC LIMIT 500;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)uid);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)dbw_now_ms());
        sqlite3_bind_int(st, 3, all ? 1 : 0);
        while (sqlite3_step(st) == SQLITE_ROW) {
            oc_conn_row *g = realloc(r->oa_conns, (r->n_oa_conns + 1) * sizeof *g);
            if (!g) break;
            r->oa_conns = g;
            oc_conn_row *c = &r->oa_conns[r->n_oa_conns++];
            c->id = (uint64_t)sqlite3_column_int64(st, 0);
            c->user_id = (uint64_t)sqlite3_column_int64(st, 1);
            c->user_name = oa_col(st, 2);
            c->client_name = oa_col(st, 3);
            c->created_ms = (uint64_t)sqlite3_column_int64(st, 4);
            c->last_used_ms = (uint64_t)sqlite3_column_int64(st, 5);
            c->expires_ms = (uint64_t)sqlite3_column_int64(st, 6);
            c->lifetime_days = (uint32_t)sqlite3_column_int64(st, 7);
        }
    }
    sqlite3_finalize(st);
}

static oc_dbres *process_oauth_list(sqlite3 *db, const oc_job *j) {
    if (j->oa_all && get_role(db, j->user_id) < OC_ROLE_ADMIN) return oa_refuse(j, NULL, OC_ERR_FORBIDDEN);
    oc_dbres *r = oa_res(j, OC_RES_OAUTH_LIST);
    if (!r) return NULL;
    r->oa_approve_echo = j->oa_all;
    oa_list_into(db, r, j->user_id, j->oa_all);
    return r;
}

/* Revoking: one's own, or anyone's as an owner or admin. Audited in the
 * account family for one's own, the administrative for another's. */
static oc_dbres *process_oauth_revoke(sqlite3 *db, const oc_job *j) {
    int admin = get_role(db, j->user_id) >= OC_ROLE_ADMIN;
    if (j->oa_all && !admin) return oa_refuse(j, NULL, OC_ERR_FORBIDDEN);
    sqlite3_stmt *st = NULL;
    uint64_t owner = 0;
    char *name = NULL;
    if (sqlite3_prepare_v2(db, "SELECT user_id, client_name FROM mcp_connections WHERE id=?1 AND revoked_at_ms=0;",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->oa_conn_id);
        if (sqlite3_step(st) == SQLITE_ROW) { owner = (uint64_t)sqlite3_column_int64(st, 0); name = oa_col(st, 1); }
    }
    sqlite3_finalize(st);
    if (!owner || (owner != j->user_id && !admin)) { free(name); return oa_refuse(j, NULL, OC_ERR_UNKNOWN_CONNECTION); }
    sqlite3_exec(db, "SAVEPOINT oauth_revoke;", NULL, NULL, NULL);
    st = NULL;
    if (sqlite3_prepare_v2(db, "UPDATE mcp_connections SET revoked_at_ms=?2, refresh_hash=NULL WHERE id=?1;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->oa_conn_id);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)dbw_now_ms());
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
    st = NULL;
    if (sqlite3_prepare_v2(db, "DELETE FROM mcp_access_tokens WHERE connection_id=?1;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->oa_conn_id);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
    sqlite3_exec(db, "RELEASE oauth_revoke;", NULL, NULL, NULL);
    char detail[48];
    snprintf(detail, sizeof detail, "connection=%llu", (unsigned long long)j->oa_conn_id);
    audit_actor(db, owner == j->user_id ? OC_AUDIT_ACCOUNT : OC_AUDIT_ADMIN, "mcp.connection_revoke", j->user_id,
                owner, name, 1, detail);
    free(name);
    oc_dbres *r = oa_res(j, OC_RES_OAUTH_LIST);
    if (!r) return NULL;
    r->oa_approve_echo = j->oa_all;
    oa_list_into(db, r, j->user_id, j->oa_all);
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
    if (acc == CH_ARCHIVED){ r->type = OC_RES_WEBHOOK_ERR; r->err_code = OC_ERR_CHANNEL_ARCHIVED; return r; }
    if (channel_is_e2e(db, j->channel_id)) { r->type = OC_RES_WEBHOOK_ERR; r->err_code = OC_ERR_FORBIDDEN; return r; }

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
        "SELECT w.channel_id, w.creator_id, w.label FROM webhooks w JOIN users u ON u.id=w.creator_id "
        " WHERE w.token_hash=? AND w.disabled=0 AND u.disabled=0;", -1, &st, NULL);
    sqlite3_bind_blob(st, 1, hash, sizeof hash, SQLITE_TRANSIENT);
    int found = sqlite3_step(st) == SQLITE_ROW;
    uint64_t cid = found ? (uint64_t)sqlite3_column_int64(st, 0) : 0;
    uint64_t creator = found ? (uint64_t)sqlite3_column_int64(st, 1) : 0;
    const char *label = found ? (const char *)sqlite3_column_text(st, 2) : NULL;
    char *label_dup = (label && label[0]) ? strdup(label) : NULL;  /* copy before finalize */
    sqlite3_finalize(st);
    if (!found) { free(label_dup); r->type = OC_RES_WEBHOOK_ERR; r->err_code = OC_ERR_UNKNOWN_WEBHOOK; return r; }

    /* Archived is read-only (REQ-035), and that has to hold for the ONE writer
     * that is not a client. SEND, SEND_REPLY and UPLOAD_BEGIN inherit it from
     * channel_post_access; a webhook post cannot use that function, because it
     * would also re-check the creator's membership and so silently break every
     * webhook whose creator has since left the channel — a different change
     * from this one. The archived test is therefore made directly, against the
     * same helper that function uses, so the two cannot disagree about what
     * archived means.
     *
     * Without it a third party holding a token wrote into a channel the product
     * presents as read-only: the composer is locked, the About panel says
     * "Archived", and the message was still stored, broadcast to every member,
     * and answered with {"ok":true}. */
    if (channel_is_archived(db, cid)) {
        free(label_dup);
        r->type = OC_RES_WEBHOOK_ERR;
        r->err_code = OC_ERR_CHANNEL_ARCHIVED;
        return r;
    }

    /* Post as the webhook's label (display-name override, REQ-170), falling back
     * to the creator's identity if the webhook was created without a label. */
    uint64_t ts = dbw_now_ms();
    /* The message and the action it may be (REQ-058) land together; an action
     * a webhook posts is told in its label, as the message is. */
    sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);
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
    if (rc != SQLITE_DONE) {
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        free(label_dup); r->type = OC_RES_WEBHOOK_ERR; r->err_code = OC_ERR_INTERNAL; return r;
    }
    uint64_t mid = (uint64_t)sqlite3_last_insert_rowid(db);
    store_action(db, mid);
    sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);

    r->type = OC_RES_WEBHOOK_POSTED;
    r->message_id = mid;
    append_action(db, r, mid, cid, 1);   /* always: the client pairs them (ARCH-115) */
    r->channel_id = cid;
    r->author_id = creator;
    r->author_name = label_dup;   /* transferred; freed by oc_dbres_free */
    r->server_time = ts;
    if (j->body_len) { r->body = malloc(j->body_len); if (r->body) { memcpy(r->body, j->body, j->body_len); r->body_len = j->body_len; } }
    r->broadcast_stored = broadcast_stored(db, mid);
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
    /* Every other piece of notification state rides the same answer — each in
     * its own frame, because NOTIFY_PREFS ends in a repeated list and anything
     * added to its fixed part shifts every entry after the first. One request,
     * so a client never has to assemble its settings from four round trips or
     * decide what to show while half of them are outstanding. */
    r->snooze_until_ms = snooze_until(db, user_id);       /* REQ-278 */
    fill_schedule(db, user_id, r);                        /* REQ-136 */
    fill_alert_prefs(db, user_id, r);                     /* REQ-135 */
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "SELECT notify_default FROM users WHERE id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)user_id);
    if (sqlite3_step(st) == SQLITE_ROW)
        r->np_default = (uint8_t)sqlite3_column_int(st, 0);          /* REQ-134 */
    sqlite3_finalize(st);

    sqlite3_prepare_v2(db,
        "SELECT channel_id, level, muted, follow_threads FROM notification_prefs WHERE user_id=? "
        "ORDER BY channel_id LIMIT ?;",
        -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)user_id);
    sqlite3_bind_int(st, 2, (int)OC_MAX_NOTIFY_PREFS);
    size_t cap = 8;
    r->nprefs = malloc(cap * sizeof *r->nprefs);
    while (r->nprefs && sqlite3_step(st) == SQLITE_ROW && r->n_nprefs < OC_MAX_NOTIFY_PREFS) {
        if (r->n_nprefs == cap) { cap *= 2; oc_notify_pref_row *g = realloc(r->nprefs, cap * sizeof *g); if (!g) break; r->nprefs = g; }
        r->nprefs[r->n_nprefs].channel_id = (uint64_t)sqlite3_column_int64(st, 0);
        r->nprefs[r->n_nprefs].level = (uint8_t)sqlite3_column_int(st, 1);
        r->nprefs[r->n_nprefs].muted = (uint8_t)sqlite3_column_int(st, 2);
        r->nprefs[r->n_nprefs].follow_threads = (uint8_t)sqlite3_column_int(st, 3);
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
    /* UPSERT on the level ALONE, not INSERT OR REPLACE: replacing the row would
     * reset `muted` to its default every time somebody changed the level, silently
     * un-muting a conversation as a side effect of an unrelated setting. */
    sqlite3_prepare_v2(db,
        "INSERT INTO notification_prefs(user_id, channel_id, level) VALUES(?1, ?2, ?3) "
        "ON CONFLICT(user_id, channel_id) DO UPDATE SET level=excluded.level;",
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

/* The global notification default (REQ-134): the level for a channel with no
 * per-channel row. Answers with the whole prefs snapshot, so a client's view of "what
 * happens by default" cannot drift from the server's. */
static oc_dbres *process_set_notify_default(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    if (j->notify_level > OC_NOTIFY_NONE) {
        r->type = OC_RES_NOTIFY_ERR; r->err_code = OC_ERR_MALFORMED_FRAME; r->user_id = j->user_id; return r;
    }
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "UPDATE users SET notify_default=? WHERE id=?;", -1, &st, NULL);
    sqlite3_bind_int(st, 1, (int)j->notify_level);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)j->user_id);
    sqlite3_step(st);
    sqlite3_finalize(st);
    r->type = OC_RES_NOTIFY_PREFS;
    r->user_id = j->user_id;
    build_notify_prefs(db, j->user_id, r);
    return r;
}

/* Mute a conversation (REQ-137). Distinct from level=none: this one also
 * de-emphasises the row and suppresses its badge, which the client does from the
 * flag. Same upsert discipline as the level above — touch one column. */
static oc_dbres *process_set_mute(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    if (!channel_read_access(db, j->channel_id, j->user_id)) {
        r->type = OC_RES_NOTIFY_ERR; r->err_code = OC_ERR_NOT_A_MEMBER; r->user_id = j->user_id; return r;
    }
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "INSERT INTO notification_prefs(user_id, channel_id, level, muted) "
        "VALUES(?1, ?2, 0, ?3) "
        "ON CONFLICT(user_id, channel_id) DO UPDATE SET muted=excluded.muted;",
        -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->user_id);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)j->channel_id);
    sqlite3_bind_int  (st, 3, j->hook_disabled ? 1 : 0);   /* reused: the mute flag */
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) { r->type = OC_RES_NOTIFY_ERR; r->err_code = OC_ERR_INTERNAL;
                             r->user_id = j->user_id; return r; }
    r->type = OC_RES_NOTIFY_PREFS;
    r->user_id = j->user_id;
    build_notify_prefs(db, j->user_id, r);
    return r;
}

/* Follow every thread in a conversation, or stop (REQ-282). The same upsert
 * discipline as mute's: one column touched, the level and mute left as they
 * were. Answers with the whole snapshot, which the net thread sends to each of
 * the user's connections. */
static oc_dbres *process_set_channel_thread_follow(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    if (!channel_read_access(db, j->channel_id, j->user_id)) {
        r->type = OC_RES_NOTIFY_ERR; r->err_code = OC_ERR_NOT_A_MEMBER; r->user_id = j->user_id; return r;
    }
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "INSERT INTO notification_prefs(user_id, channel_id, level, follow_threads) "
        "VALUES(?1, ?2, (SELECT notify_default FROM users WHERE id=?1), ?3) "
        "ON CONFLICT(user_id, channel_id) DO UPDATE SET follow_threads=excluded.follow_threads;",
        -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->user_id);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)j->channel_id);
    sqlite3_bind_int  (st, 3, j->follow_on ? 1 : 0);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) { r->type = OC_RES_NOTIFY_ERR; r->err_code = OC_ERR_INTERNAL;
                             r->user_id = j->user_id; return r; }
    build_notify_prefs(db, j->user_id, r);
    return r;
}

/* Mark unread (REQ-235): set the read cursor DELIBERATELY, including
 * backwards, which the ack path must never do.
 *
 * process_client_ack upserts MAX(message_id, excluded.message_id) so a replayed ack
 * cannot rewind anyone's cursor — a correctness property, not an oversight. This op
 * exists precisely so marking unread does not require relaxing it.
 *
 * message_id 0 means "all unread": the cursor goes to 0 rather than being deleted, so
 * the row keeps existing and the unread count query has something to compare against.
 */
static oc_dbres *process_set_read_cursor(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->channel_id = j->channel_id;
    if (!channel_read_access(db, j->channel_id, j->user_id)) {
        r->type = OC_RES_NOTIFY_ERR; r->err_code = OC_ERR_NOT_A_MEMBER; r->user_id = j->user_id; return r;
    }
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "INSERT INTO delivery_cursors(user_id,channel_id,message_id,updated_at_ms) "
        "VALUES(?1,?2,?3,?4) ON CONFLICT(user_id,channel_id) DO UPDATE SET "
        "message_id=excluded.message_id, updated_at_ms=excluded.updated_at_ms;",
        -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->user_id);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)j->channel_id);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)j->message_id);
    sqlite3_bind_int64(st, 4, (sqlite3_int64)dbw_now_ms());
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) { r->type = OC_RES_NOTIFY_ERR; r->err_code = OC_ERR_INTERNAL;
                             r->user_id = j->user_id; return r; }
    /* Told to the reader's own connections, once this commit has landed: each
     * device resets its position, and one asking for the channel list after this
     * reads the new cursor rather than racing it. */
    r->type = OC_RES_OWN_READ_CURSOR;
    r->user_id = j->user_id;
    r->message_id = j->message_id;
    return r;
}

/* ---- custom status + profile fields (REQ-241/122, REQ-240) -------------------
 *
 * Both live on `users`. Status carries an EXPIRY that the daemon enforces, because a
 * client that is not running cannot clear its own status — "in a meeting until 3pm"
 * has to stop being true whether or not you are online.
 */

/* The workspace's own profile fields, in position order, onto `r`: the
 * PROFILE_FIELDS a change answers, or the sign-in that tells them. */
static void load_profile_fields(sqlite3 *db, oc_dbres *r) {
    r->pfd = calloc(OC_MAX_PROFILE_FIELDS, sizeof *r->pfd);
    if (!r->pfd) return;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT id, kind='link', position, label FROM profile_fields "
                               "ORDER BY position, id LIMIT ?1;", -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_int(st, 1, (int)OC_MAX_PROFILE_FIELDS);
    while (r->n_pfd < OC_MAX_PROFILE_FIELDS && sqlite3_step(st) == SQLITE_ROW) {
        struct oc_pfd_row *f = &r->pfd[r->n_pfd++];
        f->id       = (uint64_t)sqlite3_column_int64(st, 0);
        f->kind     = sqlite3_column_int(st, 1) ? OC_PROFILE_FIELD_LINK : OC_PROFILE_FIELD_TEXT;
        f->position = (uint16_t)sqlite3_column_int(st, 2);
        const unsigned char *l = sqlite3_column_text(st, 3);
        snprintf(f->label, sizeof f->label, "%s", l ? (const char *)l : "");
    }
    sqlite3_finalize(st);
}

static void fill_profile_fields(sqlite3 *db, oc_dbres *r) {
    r->type = OC_RES_PROFILE_FIELDS;
    load_profile_fields(db, r);
}

/* `uid`'s values for the workspace's profile fields, in the fields' order. */
static void load_profile_values(sqlite3 *db, uint64_t uid, oc_dbres *r) {
    r->pfv = calloc(OC_MAX_PROFILE_FIELDS, sizeof *r->pfv);
    if (!r->pfv) return;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT v.field_id, v.value FROM profile_field_values v "
            "  JOIN profile_fields f ON f.id = v.field_id "
            " WHERE v.user_id = ?1 ORDER BY f.position, f.id LIMIT ?2;", -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)uid);
    sqlite3_bind_int(st, 2, (int)OC_MAX_PROFILE_FIELDS);
    while (r->n_pfv < OC_MAX_PROFILE_FIELDS && sqlite3_step(st) == SQLITE_ROW) {
        struct oc_pfv_row *v = &r->pfv[r->n_pfv++];
        v->field_id = (uint64_t)sqlite3_column_int64(st, 0);
        const unsigned char *t = sqlite3_column_text(st, 1);
        snprintf(v->value, sizeof v->value, "%s", t ? (const char *)t : "");
    }
    sqlite3_finalize(st);
}

/* Fill a PROFILE_INFO result for `uid`. Expired status reads as absent: the row is
 * left alone (a lazy sweep beats a timer thread) and every reader applies the same
 * rule, so nobody sees a stale status even before it is cleaned up. */
static void build_profile(sqlite3 *db, uint64_t uid, oc_dbres *r) {
    sqlite3_stmt *st = NULL;
    r->type = OC_RES_PROFILE_INFO;
    r->user_id = uid;
    sqlite3_prepare_v2(db,
        "SELECT COALESCE(display_name,''), COALESCE(email,''), COALESCE(status_emoji,''), "
        "       COALESCE(status_text,''), status_expires_ms, COALESCE(title,''), "
        "       COALESCE(timezone,''), COALESCE(avatar_attachment_id,0), role, "
        "       COALESCE(full_name,''), COALESCE(pronouns,''), COALESCE(phone,''), "
        "       COALESCE(voice_id,''), ooo_start_ms, ooo_end_ms, ooo_message "
        "  FROM users WHERE id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)uid);
    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *dn = sqlite3_column_text(st, 0);
        const unsigned char *em = sqlite3_column_text(st, 1);
        const unsigned char *se = sqlite3_column_text(st, 2);
        const unsigned char *sx = sqlite3_column_text(st, 3);
        uint64_t exp = (uint64_t)sqlite3_column_int64(st, 4);
        const unsigned char *ti = sqlite3_column_text(st, 5);
        const unsigned char *tz = sqlite3_column_text(st, 6);
        int expired = (exp != 0 && exp <= dbw_now_ms());
        r->author_name = strdup(dn ? (const char *)dn : "");
        /* r->body is the generic byte payload (uint8_t*), reused here for the email.
         * Cast once and measure the source, rather than strlen on a uint8_t*. */
        const char *emv = em ? (const char *)em : "";
        r->body        = (uint8_t *)strdup(emv);
        r->body_len    = strlen(emv);
        r->st_emoji    = strdup(expired ? "" : (se ? (const char *)se : ""));
        r->st_text     = strdup(expired ? "" : (sx ? (const char *)sx : ""));
        r->st_expires  = expired ? 0 : exp;
        r->pf_title    = strdup(ti ? (const char *)ti : "");
        r->pf_tz       = strdup(tz ? (const char *)tz : "");
        r->pf_avatar   = (uint64_t)sqlite3_column_int64(st, 7);
        const unsigned char *rl = sqlite3_column_text(st, 8);
        r->role = (rl && !strcmp((const char *)rl, "owner")) ? OC_ROLE_OWNER
                : (rl && !strcmp((const char *)rl, "admin")) ? OC_ROLE_ADMIN
                                                             : OC_ROLE_MEMBER;
        const unsigned char *fn = sqlite3_column_text(st, 9);
        const unsigned char *pr = sqlite3_column_text(st, 10);
        const unsigned char *ph = sqlite3_column_text(st, 11);
        r->pf_full_name = strdup(fn ? (const char *)fn : "");
        r->pf_pronouns  = strdup(pr ? (const char *)pr : "");
        r->pf_phone     = strdup(ph ? (const char *)ph : "");
        const unsigned char *vc = sqlite3_column_text(st, 12);
        r->pf_voice_id  = strdup(vc ? (const char *)vc : "");
        r->pf_ooo_start = (uint64_t)sqlite3_column_int64(st, 13);
        r->pf_ooo_end   = (uint64_t)sqlite3_column_int64(st, 14);
        const unsigned char *om = sqlite3_column_text(st, 15);
        r->pf_ooo_message = strdup(om ? (const char *)om : "");
    }
    sqlite3_finalize(st);
    load_profile_values(db, uid, r);
}

static oc_dbres *process_set_status(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    sqlite3_stmt *st = NULL;
    /* Empty text clears it, including any expiry: "no status" is one state, not two. */
    int clearing = !j->ch_name || !j->ch_name[0];
    sqlite3_prepare_v2(db,
        "UPDATE users SET status_emoji=?1, status_text=?2, status_expires_ms=?3 WHERE id=?4;",
        -1, &st, NULL);
    /* The LENGTH, not -1: oc_job_set_body copies exactly `len` bytes and does NOT
     * NUL-terminate, so binding with -1 read past the end until it happened to find a
     * zero — a real overread that stored a 4-byte emoji as 6 bytes of which 2 were
     * whatever followed in the heap. Every bind of j->body must pass j->body_len. */
    sqlite3_bind_text (st, 1, clearing ? "" : (j->body ? (const char *)j->body : ""),
                       clearing ? 0 : (int)j->body_len, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, 2, clearing ? "" : j->ch_name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)(clearing ? 0 : j->message_id));  /* expires_at */
    sqlite3_bind_int64(st, 4, (sqlite3_int64)j->user_id);
    sqlite3_step(st);
    sqlite3_finalize(st);
    audit_actor(db, OC_AUDIT_ADMIN, "user.status", j->user_id, j->user_id, NULL, 1, NULL);
    build_profile(db, j->user_id, r);
    return r;
}

/* A local account's own address (REQ-240): one address, nothing else; stored as
 * the account's, never as a verified identity, so it admits nobody under a join
 * rule or an invitation (AUTH.md §8.4). A provider's account keeps the address
 * its provider gives, refreshed at each sign-in. Empty clears it. */
static oc_dbres *profile_err(const oc_job *j, uint16_t code);   /* below */
static oc_dbres *process_set_email(sqlite3 *db, const oc_job *j) {
    const char *e = j->pf_name ? j->pf_name : "";
    size_t n = strlen(e);
    const char *at = strchr(e, '@');
    int ok = n == 0 || (n <= OC_MAX_EMAIL && at && at != e && at[1] && !strchr(at + 1, '@') &&
                        !strpbrk(e, " \t\r\n<>\",;"));
    if (!ok) return profile_err(j, OC_ERR_FORBIDDEN);
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "UPDATE users SET email=lower(?) WHERE id=? AND subject LIKE 'local:%';", -1, &st, NULL);
    if (n) sqlite3_bind_text(st, 1, e, (int)n, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 1);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)j->user_id);
    int changed = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db) == 1;
    sqlite3_finalize(st);
    if (!changed) return profile_err(j, OC_ERR_FORBIDDEN);   /* not a local account */
    audit_actor(db, OC_AUDIT_ACCOUNT, "account.email", j->user_id, 0, NULL, 1, NULL);
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    build_profile(db, j->user_id, r);
    r->pf_fanout = 1;
    return r;
}

static oc_dbres *process_set_profile(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    sqlite3_stmt *st = NULL;
    /* One statement for the whole screen: the fields are edited together and
     * committed together, so a partial write cannot leave the card showing a
     * mix of what was saved and what was not. */
    sqlite3_prepare_v2(db,
        "UPDATE users SET full_name=?1, title=?2, pronouns=?3, phone=?4, timezone=?5, "
        /* An empty voice keeps the one in place: a client that does not know
         * about read-aloud sends nothing there, and must not clear it. */
        "       voice_id=CASE WHEN ?7 = '' THEN voice_id ELSE ?7 END "
        "  WHERE id=?6;", -1, &st, NULL);
    sqlite3_bind_text (st, 1, j->pf_full_name ? j->pf_full_name : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, 2, j->pf_title     ? j->pf_title     : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, 3, j->pf_pronouns  ? j->pf_pronouns  : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, 4, j->pf_phone     ? j->pf_phone     : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, 5, j->pf_timezone  ? j->pf_timezone  : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 6, (sqlite3_int64)j->user_id);
    sqlite3_bind_text (st, 7, j->pf_voice_id  ? j->pf_voice_id  : "", -1, SQLITE_TRANSIENT);
    int own = owh_txn_begin(db);
    sqlite3_step(st);
    sqlite3_finalize(st);
    owh_user_change(db, j->user_id);
    owh_txn_end(db, own);
    build_profile(db, j->user_id, r);
    r->pf_fanout = 1;
    return r;
}

/* The avatar. An attachment id, validated here rather than trusted: it must
 * exist, be finalized, be an image, and be one THIS user uploaded. Without the last
 * check any member could point their avatar at somebody else's private-channel
 * attachment and have the daemon serve it to the whole workspace — the relaxation in
 * attachment_read_access() below makes an avatar readable by everyone, so the id has
 * to be one the user was entitled to in the first place. */
static oc_dbres *process_set_avatar(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    uint64_t aid = j->message_id;          /* 0 clears */
    if (aid) {
        sqlite3_stmt *ck = NULL;
        int ok = 0;
        sqlite3_prepare_v2(db,
            "SELECT 1 FROM attachments WHERE id=?1 AND uploader_id=?2 "
            "  AND size > 0 AND mime LIKE 'image/%' "
            /* Finished (its bytes arrived: sha256 is set when they do, as
             * link_attachments requires) and not reclaimed: an image that is
             * there. */
            "  AND sha256 IS NOT NULL AND reclaimed_at_ms = 0;", -1, &ck, NULL);
        sqlite3_bind_int64(ck, 1, (sqlite3_int64)aid);
        sqlite3_bind_int64(ck, 2, (sqlite3_int64)j->user_id);
        ok = (sqlite3_step(ck) == SQLITE_ROW);
        sqlite3_finalize(ck);
        if (!ok) {
            r->type = OC_RES_LIST_ERR; r->err_code = OC_ERR_UNKNOWN_ATTACHMENT;
            r->user_id = j->user_id;
            return r;
        }
    }
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "UPDATE users SET avatar_attachment_id=?1 WHERE id=?2;", -1, &st, NULL);
    if (aid) sqlite3_bind_int64(st, 1, (sqlite3_int64)aid);
    else     sqlite3_bind_null (st, 1);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)j->user_id);
    sqlite3_step(st);
    sqlite3_finalize(st);
    build_profile(db, j->user_id, r);
    return r;
}

/* The SQL functions every connection needs: oc_excerpt and oc_has_spoiler,
 * which the excerpts are built with, and oc_unspoiled, which the search
 * index's triggers call. One set, in migrate.c, so a test's own connection
 * registers exactly what the daemon's do. */
static void sql_functions(sqlite3 *db) {
    oc_migrate_functions(db);
}

/* Out of office (REQ-122). Stored as given; an end of 0 or already past clears
 * all three. The answer is the person's whole profile to their connections,
 * and their roster row to everyone, whose marks and composer notices follow. */
static oc_dbres *process_set_out_of_office(sqlite3 *db, const oc_job *j) {
    const char *msg = j->pf_name ? j->pf_name : "";
    size_t n = strlen(msg);
    uint64_t now = dbw_now_ms();
    int clear = j->ooo_end == 0 || j->ooo_end <= now;
    if (!clear && (j->ooo_end <= j->ooo_start || n > OC_OOO_MESSAGE_MAX))
        return profile_err(j, OC_ERR_INVALID_OUT_OF_OFFICE);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "UPDATE users SET ooo_start_ms=?1, ooo_end_ms=?2, ooo_message=?3 WHERE id=?4;",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, clear ? 0 : (sqlite3_int64)j->ooo_start);
        sqlite3_bind_int64(st, 2, clear ? 0 : (sqlite3_int64)j->ooo_end);
        sqlite3_bind_text (st, 3, clear ? "" : msg, clear ? 0 : (int)n, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 4, (sqlite3_int64)j->user_id);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    build_profile(db, j->user_id, r);
    r->pf_fanout = 1;
    user_rows(db, j->user_id, r);
    r->joined = r->n_ulist != 0;   /* the row, to everyone signed in */
    return r;
}

static oc_dbres *process_get_profile(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    /* Any tenant member may read any member's profile — the roster is already
     * visible to everyone (REQ-030), so this adds no exposure. The phone is the
     * one field that is not on the roster: it goes to its owner only. */
    uint64_t whose = j->message_id ? j->message_id : j->user_id;
    build_profile(db, whose, r);
    if (whose != j->user_id) { free(r->pf_phone); r->pf_phone = NULL; }
    return r;
}

/* ---- the workspace's own profile fields (SCHEMA.md §3bs) ---------------------
 *
 * Defined by an owner or admin, at most OC_MAX_PROFILE_FIELDS, each text or a
 * link; each person fills their own. A value is never more than 200 bytes, and a
 * link's starts https:// so a card never offers a script or a local file. */

static int pf_text_ok(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) if ((unsigned char)s[i] < 0x20 || s[i] == 0x7f) return 0;
    return 1;
}

static oc_dbres *process_set_profile_field(sqlite3 *db, const oc_job *j) {
    uint8_t role = OC_ROLE_MEMBER;
    if (!user_role(db, j->user_id, &role) || !oc_role_can_manage_members(role))
        return profile_err(j, OC_ERR_FORBIDDEN);
    uint64_t id = j->message_id;
    const char *label = j->pf_name ? j->pf_name : "";
    size_t n = strlen(label);
    sqlite3_stmt *st = NULL;
    int ok = 0;
    if (j->pfd_op == OC_PROFILE_FIELD_DELETE) {
        if (!id) return profile_err(j, OC_ERR_INVALID_PROFILE_FIELD);
        sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);
        if (sqlite3_prepare_v2(db, "DELETE FROM profile_field_values WHERE field_id=?1;", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)id);
            sqlite3_step(st);
        }
        sqlite3_finalize(st); st = NULL;
        if (sqlite3_prepare_v2(db, "DELETE FROM profile_fields WHERE id=?1;", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)id);
            ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db) == 1;
        }
        sqlite3_finalize(st);
        sqlite3_exec(db, ok ? "COMMIT;" : "ROLLBACK;", NULL, NULL, NULL);
        if (!ok) return profile_err(j, OC_ERR_INVALID_PROFILE_FIELD);
        audit_actor(db, OC_AUDIT_ADMIN, "profile_field.delete", j->user_id, 0, NULL, 1, NULL);
    } else {
        if (j->pfd_op != OC_PROFILE_FIELD_SET || n == 0 || n > OC_PROFILE_FIELD_LABEL_MAX ||
            !pf_text_ok(label, n) || j->pfd_kind > OC_PROFILE_FIELD_LINK)
            return profile_err(j, OC_ERR_INVALID_PROFILE_FIELD);
        if (id == 0) {
            /* A new one, within the cap, last unless a place is asked for. */
            if (sqlite3_prepare_v2(db,
                    "INSERT INTO profile_fields(label, kind, position, created_at_ms) "
                    "SELECT ?1, ?2, CASE WHEN ?3 > 0 THEN ?3 "
                    "                    ELSE (SELECT COALESCE(MAX(position), 0) + 1 FROM profile_fields) END, ?4 "
                    " WHERE (SELECT COUNT(*) FROM profile_fields) < ?5;", -1, &st, NULL) == SQLITE_OK) {
                sqlite3_bind_text (st, 1, label, (int)n, SQLITE_TRANSIENT);
                sqlite3_bind_text (st, 2, j->pfd_kind == OC_PROFILE_FIELD_LINK ? "link" : "text", -1, SQLITE_STATIC);
                sqlite3_bind_int  (st, 3, (int)j->pfd_position);
                sqlite3_bind_int64(st, 4, (sqlite3_int64)dbw_now_ms());
                sqlite3_bind_int  (st, 5, (int)OC_MAX_PROFILE_FIELDS);
                ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db) == 1;
            }
        } else {
            /* A rename, and a move when a place is given. The kind stays what it
             * was made: values already given were checked against it. */
            if (sqlite3_prepare_v2(db,
                    "UPDATE profile_fields SET label=?1, "
                    "       position=CASE WHEN ?2 > 0 THEN ?2 ELSE position END WHERE id=?3;",
                    -1, &st, NULL) == SQLITE_OK) {
                sqlite3_bind_text (st, 1, label, (int)n, SQLITE_TRANSIENT);
                sqlite3_bind_int  (st, 2, (int)j->pfd_position);
                sqlite3_bind_int64(st, 3, (sqlite3_int64)id);
                ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db) == 1;
            }
        }
        sqlite3_finalize(st);
        if (!ok) return profile_err(j, OC_ERR_INVALID_PROFILE_FIELD);
        audit_actor(db, OC_AUDIT_ADMIN, "profile_field.set", j->user_id, 0, NULL, 1, NULL);
    }
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->user_id = j->user_id;
    fill_profile_fields(db, r);
    r->ch_fanout = 1;   /* every client's cards and editors change with it */
    return r;
}

static oc_dbres *process_set_profile_field_value(sqlite3 *db, const oc_job *j) {
    const char *v = j->pf_name ? j->pf_name : "";
    size_t n = strlen(v);
    sqlite3_stmt *st = NULL;
    int kind = -1;
    if (sqlite3_prepare_v2(db, "SELECT kind='link' FROM profile_fields WHERE id=?1;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->message_id);
        if (sqlite3_step(st) == SQLITE_ROW) kind = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st); st = NULL;
    if (kind < 0 || n > OC_PROFILE_FIELD_VALUE_MAX || !pf_text_ok(v, n) ||
        (kind == 1 && n && (strncmp(v, "https://", 8) != 0 || n == 8 || strpbrk(v, " <>\"'`"))))
        return profile_err(j, OC_ERR_INVALID_PROFILE_FIELD);
    if (n == 0)
        sqlite3_prepare_v2(db, "DELETE FROM profile_field_values WHERE user_id=?1 AND field_id=?2;", -1, &st, NULL);
    else
        sqlite3_prepare_v2(db,
            "INSERT INTO profile_field_values(user_id, field_id, value) VALUES(?1, ?2, ?3) "
            "ON CONFLICT(user_id, field_id) DO UPDATE SET value=excluded.value;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->user_id);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)j->message_id);
    if (n) sqlite3_bind_text(st, 3, v, (int)n, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    build_profile(db, j->user_id, r);
    r->pf_fanout = 1;   /* every device of theirs, as SET_PROFILE */
    return r;
}

/* Set the caller's do-not-disturb window (REQ-131). Write. */
/* The pause's end instant for `uid`, ENFORCED ON READ: a stamp that has passed
 * reads as 0, so nobody — the push worker, the net thread, the user's own client
 * — needs a sweep or a clock of their own to agree that it is over (REQ-278,
 * the pattern migration 0027 proved for status expiry). */
static uint64_t snooze_until(sqlite3 *db, uint64_t uid) {
    sqlite3_stmt *st = NULL;
    uint64_t until = 0;
    sqlite3_prepare_v2(db, "SELECT dnd_until_ms FROM users WHERE id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)uid);
    if (sqlite3_step(st) == SQLITE_ROW) until = (uint64_t)sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return (until && until <= dbw_now_ms()) ? 0 : until;
}

/* Pause notifications until an instant (REQ-278). The wire carries
 * MINUTES FROM NOW, as Slack's `dnd.setSnooze` does — every preset is a duration
 * and only the client knows the timezone that turns "until tomorrow" into a
 * moment — so the instant is resolved here, once. 0 minutes ends the pause:
 * ending early is "until now", not a second op. Write. */
/* Refresh the stored UTC offset (ARCH-103). Fire-and-forget: it returns no
 * result, because nothing observable changed for this session — the offset only
 * matters later, to the push worker deciding whether a per-weekday quiet-hours
 * window is currently in force on the recipient's own calendar day. Clamped to
 * the range real offsets occupy, so a broken or hostile client cannot move
 * somebody's local day by an arbitrary amount. */
/* The offset quiet hours are evaluated against (ARCH-103), refreshed by the
 * client from the OS on every connect.
 *
 * It ANSWERS, with the schedule the offset belongs to. It used to return NULL,
 * which meant a timezone change reached the net thread through nothing — and
 * that is the common path rather than an edge case, because the refresh lands
 * AFTER the AUTH_OK that seeded the cache. A first connect from a new machine
 * would otherwise have evaluated somebody's quiet hours against a stale
 * offset, or against zero. */
static oc_dbres *process_set_tz_offset(sqlite3 *db, const oc_job *j) {
    int off = j->tz_offset_min;
    if (off < -720) off = -720;        /* UTC-12, the westmost real offset */
    if (off >  840) off =  840;        /* UTC+14, the eastmost */
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "UPDATE users SET tz_offset_min=?1 WHERE id=?2;", -1, &st, NULL);
    sqlite3_bind_int(st, 1, off);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)j->user_id);
    sqlite3_step(st);
    sqlite3_finalize(st);

    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    build_schedule(db, j->user_id, r);
    return r;
}

static oc_dbres *process_set_snooze(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->type = OC_RES_SNOOZE;
    r->user_id = j->user_id;
    /* Capped so a bad client cannot silence somebody for a decade by arithmetic;
     * a week is already far past any preset the requirement names. */
    uint32_t mins = j->snooze_minutes > 7u * 24u * 60u ? 7u * 24u * 60u : j->snooze_minutes;
    uint64_t until = mins ? dbw_now_ms() + (uint64_t)mins * 60000ull : 0;
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "UPDATE users SET dnd_until_ms=?1 WHERE id=?2;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)until);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)j->user_id);
    sqlite3_step(st);
    sqlite3_finalize(st);
    r->snooze_until_ms = until;
    return r;
}

/* --- threads across channels (REQ-062, ARCH-104) --------------------------- */

/* Fill one summary row from a stepped statement whose columns are, in order:
 * root_id, channel_id, root_author, root_at, last_reply_at, replies, unread,
 * following, preview. */
static void thread_row_from(sqlite3_stmt *st, oc_thread_row *t) {
    t->root_id      = (uint64_t)sqlite3_column_int64(st, 0);
    t->channel_id   = (uint64_t)sqlite3_column_int64(st, 1);
    t->root_author  = (uint64_t)sqlite3_column_int64(st, 2);
    t->root_at      = (uint64_t)sqlite3_column_int64(st, 3);
    t->last_reply_at= (uint64_t)sqlite3_column_int64(st, 4);
    t->reply_count  = (uint32_t)sqlite3_column_int(st, 5);
    t->unread       = (uint32_t)sqlite3_column_int(st, 6);
    t->following    = (uint8_t)sqlite3_column_int(st, 7);
    const unsigned char *p = sqlite3_column_text(st, 8);
    t->preview      = strdup(p ? (const char *)p : "");
    t->action       = (uint8_t)(sqlite3_column_int(st, 9) != 0);
}

/* The one query the product did not have: every thread I am in, across every
 * channel, newest activity first.
 *
 * Membership of the channel is checked first, as everywhere else — a thread in a
 * conversation I have left is not mine to see. Participation is derived, and an
 * explicit unfollow wins over it, which is what "turn off replies" means. A root
 * with no replies is not a thread yet, so it is excluded rather than listed as an
 * empty one. */
static oc_dbres *process_list_threads(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->type = OC_RES_THREAD_LIST;
    r->user_id = j->user_id;
    static const char *SQL =
        "SELECT r.id, r.channel_id, r.author_id, r.created_at_ms, "
        "       (SELECT MAX(x.created_at_ms) FROM messages x "
        "         WHERE x.parent_id = r.id AND x.deleted_at_ms IS NULL) AS last_at, "
        "       (SELECT COUNT(*) FROM messages x "
        "         WHERE x.parent_id = r.id AND x.deleted_at_ms IS NULL) AS replies, "
        /* A muted person's replies are no badge (REQ-284), nor what a block
         * withheld from me, as in the channel's unread; the same tests in
         * build_thread_one below. */
        "       (SELECT COUNT(*) FROM messages x "
        "         WHERE x.parent_id = r.id AND x.deleted_at_ms IS NULL "
        "           AND x.author_id <> ?1 "
        "           AND NOT EXISTS(SELECT 1 FROM muted_users mu WHERE mu.user_id = ?1 AND mu.muted_id = x.author_id) "
        "           AND NOT EXISTS(SELECT 1 FROM withheld_messages wm WHERE wm.user_id = ?1 AND wm.message_id = x.id) "
        "           AND x.id > COALESCE((SELECT tr.last_read_reply_id FROM thread_reads tr "
        "                                 WHERE tr.user_id = ?1 AND tr.root_id = r.id), 0)) AS unread, "
        "       COALESCE((SELECT tf.state FROM thread_follows tf "
        "                  WHERE tf.user_id = ?1 AND tf.root_id = r.id), 1) AS following, "
        /* An action root previews as its action text, flagged (REQ-058). */
        "       oc_excerpt(substr(CASE WHEN (SELECT e2e FROM channels WHERE id=r.channel_id)=1 THEN '' ELSE COALESCE(r.body,'') END,1+COALESCE((SELECT ac.text_start FROM actions ac WHERE ac.message_id=r.id),0)),?2), EXISTS(SELECT 1 FROM actions ac WHERE ac.message_id=r.id) "
        "  FROM messages r "
        "  JOIN channel_members cm ON cm.channel_id = r.channel_id AND cm.user_id = ?1 "
        " WHERE r.parent_id IS NULL AND r.deleted_at_ms IS NULL "
        /* In it, one way or another — and not if I said otherwise, which
         * outranks having replied. The same predicate the push audience and
         * THREAD_REPLY's per-recipient byte ask (ARCH-104). */
        "   AND " OC_THREAD_PARTICIPANT_SQL("?1", "r.id") " "
        "   AND EXISTS(SELECT 1 FROM messages z WHERE z.parent_id = r.id "
        "               AND z.deleted_at_ms IS NULL) "
        "   AND (?4 = 0 OR unread > 0) "
        " ORDER BY last_at DESC LIMIT ?3;";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, SQL, -1, &st, NULL) != SQLITE_OK) return r;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->user_id);
    sqlite3_bind_int  (st, 2, (int)OC_MAX_PREVIEW);
    sqlite3_bind_int  (st, 3, (int)OC_MAX_THREADS);
    sqlite3_bind_int  (st, 4, (int)j->thread_filter);
    oc_thread_row *arr = calloc(OC_MAX_THREADS, sizeof *arr);
    size_t n = 0;
    while (arr && n < OC_MAX_THREADS && sqlite3_step(st) == SQLITE_ROW)
        thread_row_from(st, &arr[n++]);
    sqlite3_finalize(st);
    r->threads = arr; r->n_threads = n;
    return r;
}

/* One thread's summary, for the ack of a follow or a read mark: the client folds
 * the row that changed rather than re-listing everything, which is the shape
 * DRAFT already uses and the reason a list op is not needed here. */
static void build_thread_one(sqlite3 *db, uint64_t uid, uint64_t root_id, oc_dbres *r) {
    r->type = OC_RES_THREAD_ONE;
    r->user_id = uid;
    static const char *SQL =
        "SELECT r.id, r.channel_id, r.author_id, r.created_at_ms, "
        "       COALESCE((SELECT MAX(x.created_at_ms) FROM messages x "
        "         WHERE x.parent_id = r.id AND x.deleted_at_ms IS NULL),0), "
        "       (SELECT COUNT(*) FROM messages x "
        "         WHERE x.parent_id = r.id AND x.deleted_at_ms IS NULL), "
        "       (SELECT COUNT(*) FROM messages x "
        "         WHERE x.parent_id = r.id AND x.deleted_at_ms IS NULL "
        "           AND x.author_id <> ?1 "
        "           AND NOT EXISTS(SELECT 1 FROM muted_users mu WHERE mu.user_id = ?1 AND mu.muted_id = x.author_id) "
        "           AND NOT EXISTS(SELECT 1 FROM withheld_messages wm WHERE wm.user_id = ?1 AND wm.message_id = x.id) "
        "           AND x.id > COALESCE((SELECT tr.last_read_reply_id FROM thread_reads tr "
        "                                 WHERE tr.user_id = ?1 AND tr.root_id = r.id), 0)), "
        "       COALESCE((SELECT tf.state FROM thread_follows tf "
        "                  WHERE tf.user_id = ?1 AND tf.root_id = r.id), 1), "
        /* An action root previews as its action text, flagged (REQ-058). */
        "       oc_excerpt(substr(CASE WHEN (SELECT e2e FROM channels WHERE id=r.channel_id)=1 THEN '' ELSE COALESCE(r.body,'') END,1+COALESCE((SELECT ac.text_start FROM actions ac WHERE ac.message_id=r.id),0)),?2), EXISTS(SELECT 1 FROM actions ac WHERE ac.message_id=r.id) "
        "  FROM messages r WHERE r.id = ?3;";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, SQL, -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)uid);
    sqlite3_bind_int  (st, 2, (int)OC_MAX_PREVIEW);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)root_id);
    if (sqlite3_step(st) == SQLITE_ROW) {
        r->threads = calloc(1, sizeof *r->threads);
        if (r->threads) { thread_row_from(st, &r->threads[0]); r->n_threads = 1; }
    }
    sqlite3_finalize(st);
}

static oc_dbres *process_set_thread_follow(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    if (!channel_read_access(db, j->channel_id, j->user_id)) {
        r->type = OC_RES_LIST_ERR; r->err_code = OC_ERR_NOT_A_MEMBER; r->user_id = j->user_id;
        return r;
    }
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "INSERT INTO thread_follows(user_id, root_id, channel_id, state, updated_ms) "
        "VALUES(?1,?2,?3,?4,?5) ON CONFLICT(user_id, root_id) DO UPDATE SET "
        "state=excluded.state, updated_ms=excluded.updated_ms;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->user_id);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)j->parent_id);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)j->channel_id);
    sqlite3_bind_int  (st, 4, j->follow_on ? 1 : 0);
    sqlite3_bind_int64(st, 5, (sqlite3_int64)dbw_now_ms());
    sqlite3_step(st);
    sqlite3_finalize(st);
    build_thread_one(db, j->user_id, j->parent_id, r);
    return r;
}

/* Mark a thread's replies read. `message_id` 0 means all of them, which is what
 * opening a thread means; the cursor only ever advances, like REQ-090's. */
static oc_dbres *process_mark_thread_read(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    uint64_t up_to = j->message_id;
    if (!up_to) {
        sqlite3_stmt *q = NULL;
        sqlite3_prepare_v2(db,
            "SELECT COALESCE(MAX(id),0) FROM messages WHERE parent_id=?;", -1, &q, NULL);
        sqlite3_bind_int64(q, 1, (sqlite3_int64)j->parent_id);
        if (sqlite3_step(q) == SQLITE_ROW) up_to = (uint64_t)sqlite3_column_int64(q, 0);
        sqlite3_finalize(q);
    }
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "INSERT INTO thread_reads(user_id, root_id, last_read_reply_id, updated_ms) "
        "VALUES(?1,?2,?3,?4) ON CONFLICT(user_id, root_id) DO UPDATE SET "
        "last_read_reply_id=MAX(last_read_reply_id, excluded.last_read_reply_id), "
        "updated_ms=excluded.updated_ms;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->user_id);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)j->parent_id);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)up_to);
    sqlite3_bind_int64(st, 4, (sqlite3_int64)dbw_now_ms());
    sqlite3_step(st);
    sqlite3_finalize(st);
    build_thread_one(db, j->user_id, j->parent_id, r);
    return r;
}

/* Read the schedule as stored (REQ-136). Modes 0-2 need no rows at all; only
 * *custom* reads the per-weekday table, which is why three of the four cases
 * cost one query. */
static void fill_schedule(sqlite3 *db, uint64_t uid, oc_dbres *r) {
    sqlite3_stmt *st = NULL;
    int own = 1;
    sqlite3_prepare_v2(db,
        "SELECT dnd_mode, tz_offset_min, allow_start_min, allow_end_min, dnd_own "
        "  FROM users WHERE id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)uid);
    if (sqlite3_step(st) == SQLITE_ROW) {
        r->sc_mode           = (uint8_t)sqlite3_column_int(st, 0);
        r->sc_tz_offset_min  = (int16_t)sqlite3_column_int(st, 1);
        r->sc_start_min      = (uint16_t)sqlite3_column_int(st, 2);
        r->sc_end_min        = (uint16_t)sqlite3_column_int(st, 3);
        own = sqlite3_column_int(st, 4);
    }
    sqlite3_finalize(st);
    /* Never set their own: the workspace's default, resolved and marked, so
     * every device of theirs and the push decision agree (OPENCHIME_DEFAULT_QUIET_HOURS). */
    int dm, ds, de;
    if (!own && oc_config_quiet_hours(oc_config_get()->default_quiet_hours, &dm, &ds, &de)) {
        r->sc_mode = (uint8_t)(dm | OC_DND_FROM_DEFAULT);
        r->sc_start_min = (uint16_t)ds;
        r->sc_end_min = (uint16_t)de;
        return;
    }
    sqlite3_prepare_v2(db,
        "SELECT weekday, enabled, start_min, end_min FROM notify_schedule "
        " WHERE user_id=? ORDER BY weekday;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)uid);
    while (r->sc_n_days < OC_SCHEDULE_DAYS && sqlite3_step(st) == SQLITE_ROW) {
        oc_schedule_day *d = &r->sc_days[r->sc_n_days++];
        d->weekday   = (uint8_t)sqlite3_column_int(st, 0);
        d->enabled   = (uint8_t)sqlite3_column_int(st, 1);
        d->start_min = (uint16_t)sqlite3_column_int(st, 2);
        d->end_min   = (uint16_t)sqlite3_column_int(st, 3);
    }
    sqlite3_finalize(st);
}

static void build_schedule(sqlite3 *db, uint64_t uid, oc_dbres *r) {
    r->type = OC_RES_SCHEDULE;
    r->user_id = uid;
    fill_schedule(db, uid, r);
}

/* Set the schedule (REQ-136). Replaces what was there — including the
 * per-weekday rows, which are deleted before the new ones land, because a
 * schedule is one fact and a merge would leave a day nobody can see set. */
static oc_dbres *process_set_schedule(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    sqlite3_stmt *st = NULL;
    /* OC_DND_FROM_DEFAULT alone: drop my own and follow the workspace's
     * default again. Anything else is my own, which wins from now on. */
    int to_default = j->sched_mode == OC_DND_FROM_DEFAULT;
    sqlite3_prepare_v2(db,
        "UPDATE users SET dnd_mode=?1, tz_offset_min=?2, allow_start_min=?3, "
        "                 allow_end_min=?4, dnd_own=?6 WHERE id=?5;", -1, &st, NULL);
    sqlite3_bind_int  (st, 6, to_default ? 0 : 1);
    sqlite3_bind_int  (st, 1, j->sched_mode > OC_DND_CUSTOM ? 0 : (int)j->sched_mode);
    sqlite3_bind_int  (st, 2, (int)j->sched_tz_offset_min);
    sqlite3_bind_int  (st, 3, (int)j->sched_start_min);
    sqlite3_bind_int  (st, 4, (int)j->sched_end_min);
    sqlite3_bind_int64(st, 5, (sqlite3_int64)j->user_id);
    sqlite3_step(st);
    sqlite3_finalize(st);

    sqlite3_prepare_v2(db, "DELETE FROM notify_schedule WHERE user_id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->user_id);
    sqlite3_step(st);
    sqlite3_finalize(st);

    for (uint8_t i = 0; !to_default && i < j->n_sched_days && j->sched_days; i++) {
        const oc_schedule_day *d = &j->sched_days[i];
        if (d->weekday > 6) continue;              /* a day that does not exist */
        sqlite3_prepare_v2(db,
            "INSERT INTO notify_schedule(user_id,weekday,enabled,start_min,end_min) "
            "VALUES(?1,?2,?3,?4,?5);", -1, &st, NULL);
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->user_id);
        sqlite3_bind_int  (st, 2, (int)d->weekday);
        sqlite3_bind_int  (st, 3, d->enabled ? 1 : 0);
        sqlite3_bind_int  (st, 4, (int)d->start_min);
        sqlite3_bind_int  (st, 5, (int)d->end_min);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    build_schedule(db, j->user_id, r);
    return r;
}

/* The people `uid` has muted, as MUTED_USERS carries them: read with the alert
 * preferences, so a sign-in tells the client both lists about WHO at once. */
static void fill_muted_users(sqlite3 *db, uint64_t uid, oc_dbres *r) {
    sqlite3_stmt *st = NULL;
    r->mu_ids = calloc(OC_MAX_MUTED_USERS, sizeof *r->mu_ids);
    if (!r->mu_ids) return;
    sqlite3_prepare_v2(db, "SELECT muted_id FROM muted_users WHERE user_id=? ORDER BY muted_id;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)uid);
    while (r->mu_n < OC_MAX_MUTED_USERS && sqlite3_step(st) == SQLITE_ROW)
        r->mu_ids[r->mu_n++] = (uint64_t)sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    r->mu_have = 1;
}

/* Mute or unmute one person, for the asker alone (SET_USER_MUTE). Muting
 * yourself, the workspace's own sender or nobody is refused by doing nothing;
 * the muted person is told nothing. Answered with the whole list, to every
 * connection of the asker. */
static oc_dbres *process_set_user_mute(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->type = OC_RES_MUTED_USERS;
    r->user_id = j->user_id;
    uint64_t who = j->target_user_id;
    if (who && who != j->user_id) {
        sqlite3_stmt *st = NULL;
        if (j->mute_on)
            sqlite3_prepare_v2(db,
                "INSERT OR IGNORE INTO muted_users(user_id, muted_id, created_at_ms) "
                "SELECT ?1, ?2, ?3 WHERE EXISTS(SELECT 1 FROM users WHERE id=?2 AND is_system=0);",
                -1, &st, NULL);
        else
            sqlite3_prepare_v2(db, "DELETE FROM muted_users WHERE user_id=?1 AND muted_id=?2;", -1, &st, NULL);
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->user_id);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)who);
        if (j->mute_on) sqlite3_bind_int64(st, 3, (sqlite3_int64)dbw_now_ms());
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    fill_muted_users(db, j->user_id, r);
    return r;
}

/* My keywords and my priority people (REQ-135). Read together because they are
 * one idea in the product — "what reaches me regardless of level" — and a client
 * that showed one without the other would be showing half a setting. */
static void fill_alert_prefs(sqlite3 *db, uint64_t uid, oc_dbres *r) {
    sqlite3_stmt *st = NULL;
    r->al_terms = calloc(OC_MAX_KEYWORDS, sizeof *r->al_terms);
    sqlite3_prepare_v2(db, "SELECT term FROM notify_keywords WHERE user_id=? ORDER BY term;",
                       -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)uid);
    while (r->al_terms && r->al_n_terms < OC_MAX_KEYWORDS && sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *t = sqlite3_column_text(st, 0);
        r->al_terms[r->al_n_terms++] = strdup(t ? (const char *)t : "");
    }
    sqlite3_finalize(st);
    r->al_people = calloc(OC_MAX_PRIORITY, sizeof *r->al_people);
    sqlite3_prepare_v2(db, "SELECT person_id FROM priority_people WHERE user_id=? ORDER BY person_id;",
                       -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)uid);
    while (r->al_people && r->al_n_people < OC_MAX_PRIORITY && sqlite3_step(st) == SQLITE_ROW)
        r->al_people[r->al_n_people++] = (uint64_t)sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    fill_muted_users(db, uid, r);
    fill_blocks(db, uid, r);   /* whom I block, and who may start a DM with me */
}

static void build_alert_prefs(sqlite3 *db, uint64_t uid, oc_dbres *r) {
    r->type = OC_RES_ALERT_PREFS;
    r->user_id = uid;
    fill_alert_prefs(db, uid, r);
}

static oc_dbres *process_set_keywords(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "DELETE FROM notify_keywords WHERE user_id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->user_id);
    sqlite3_step(st);
    sqlite3_finalize(st);
    for (uint8_t i = 0; i < j->n_kw_terms && j->kw_terms; i++) {
        const char *t = j->kw_terms[i];
        if (!t || !*t) continue;
        /* Stored lowercased, because matching is case-insensitive (REQ-135) and
         * folding once on write beats folding on every message. */
        char low[OC_KEYWORD_MAX];
        size_t n = 0;
        for (; t[n] && n + 1 < sizeof low; n++) low[n] = (char)tolower((unsigned char)t[n]);
        low[n] = '\0';
        if (!n) continue;
        sqlite3_prepare_v2(db,
            "INSERT OR IGNORE INTO notify_keywords(user_id, term) VALUES(?1, ?2);",
            -1, &st, NULL);
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->user_id);
        sqlite3_bind_text (st, 2, low, -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    build_alert_prefs(db, j->user_id, r);
    return r;
}

static oc_dbres *process_set_priority(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "DELETE FROM priority_people WHERE user_id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->user_id);
    sqlite3_step(st);
    sqlite3_finalize(st);
    for (uint8_t i = 0; i < j->n_pri_people && j->pri_people; i++) {
        uint64_t pid = j->pri_people[i];
        /* Yourself is not a priority person: your own messages never notify you,
         * so the row would be a setting that can never fire. */
        if (!pid || pid == j->user_id) continue;
        sqlite3_prepare_v2(db,
            "INSERT OR IGNORE INTO priority_people(user_id, person_id) "
            "SELECT ?1, ?2 WHERE EXISTS(SELECT 1 FROM users WHERE id=?2);", -1, &st, NULL);
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->user_id);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)pid);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    build_alert_prefs(db, j->user_id, r);
    return r;
}

/* Register a push device token (ARCH-85). Upsert on (user, token): re-registering
 * the same token just refreshes last_seen + platform. Write. */
static oc_dbres *process_register_device_token(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    const char *platform = j->device_platform == OC_PUSH_FCM ? "fcm" : "apns";
    if (j->device_platform > OC_PUSH_FCM || !j->device_token ||
        !oc_device_token_ok((const uint8_t *)j->device_token, strlen(j->device_token))) {
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
/* ---- invite management (REQ-026) ------------------------------------
 *
 * The `invites` table has carried role, expires_at_ms and consumed_at_ms since
 * migration 0002; nothing could read them, so a minted invite was write-only —
 * no way to see what was outstanding, and no way to take one back.
 *
 * Outstanding means: not consumed and not expired. A consumed one is history (the
 * audit log has it) and an expired one is already inert, so listing either would
 * pad the view with rows nobody can act on.
 */
static oc_dbres *process_list_invites(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    uint8_t role = OC_ROLE_MEMBER;
    user_role(db, j->user_id, &role);
    if (role < OC_ROLE_ADMIN) {
        r->type = OC_RES_LIST_ERR; r->err_code = OC_ERR_FORBIDDEN; return r;
    }
    r->type = OC_RES_INVITE_LIST;

    /* rowid AS the id: the table is keyed by token_hash, and a hash is exactly what
     * must not travel. rowid is stable for the life of the row, which is all a
     * revoke needs. */
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "SELECT rowid, role, expires_at_ms, COALESCE(created_by,0), uses, used, COALESCE(created_at_ms,0) FROM invites "
        " WHERE consumed_at_ms IS NULL AND expires_at_ms > ?1 "
        " ORDER BY expires_at_ms ASC LIMIT ?2;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)dbw_now_ms());
    sqlite3_bind_int64(st, 2, (sqlite3_int64)OC_MAX_INVITES);
    oc_invite_entry *arr = calloc(OC_MAX_INVITES, sizeof *arr);
    size_t n = 0;
    while (arr && n < OC_MAX_INVITES && sqlite3_step(st) == SQLITE_ROW) {
        arr[n].invite_id  = (uint64_t)sqlite3_column_int64(st, 0);
        const unsigned char *rl = sqlite3_column_text(st, 1);
        arr[n].role = (rl && !strcmp((const char *)rl, "owner")) ? OC_ROLE_OWNER
                    : (rl && !strcmp((const char *)rl, "admin")) ? OC_ROLE_ADMIN
                                                                 : OC_ROLE_MEMBER;
        arr[n].expires_at = (uint64_t)sqlite3_column_int64(st, 2);
        arr[n].created_by = (uint64_t)sqlite3_column_int64(st, 3);
        arr[n].uses       = (uint16_t)sqlite3_column_int(st, 4);
        arr[n].used       = (uint16_t)sqlite3_column_int(st, 5);
        arr[n].created_at = (uint64_t)sqlite3_column_int64(st, 6);   /* 0: made before the column */
        n++;
    }
    sqlite3_finalize(st);
    r->invites = arr; r->n_invites = n;
    return r;
}

static oc_dbres *process_revoke_invite(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    uint8_t role = OC_ROLE_MEMBER;
    user_role(db, j->user_id, &role);
    if (role < OC_ROLE_ADMIN) {
        r->type = OC_RES_LIST_ERR; r->err_code = OC_ERR_FORBIDDEN; return r;
    }
    /* DELETE, not a consumed-stamp: a revoked invite was never redeemed, and
     * marking it consumed would claim in the audit trail that somebody used it. */
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "DELETE FROM invites WHERE rowid=? AND consumed_at_ms IS NULL;",
                       -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->message_id);   /* invite rowid */
    sqlite3_step(st);
    int gone = sqlite3_changes(db);
    sqlite3_finalize(st);
    if (!gone) { r->type = OC_RES_LIST_ERR; r->err_code = OC_ERR_UNKNOWN_MESSAGE; return r; }
    audit_actor(db, OC_AUDIT_ADMIN, "invite.revoke", j->user_id, j->message_id, NULL, 1, NULL);
    r->type = OC_RES_INVITE_REVOKED;
    r->message_id = j->message_id;
    return r;
}

/* ---- webhook lifecycle --------------------------------------------
 *
 * `webhooks.disabled` has existed since migration 0016 and nothing could set it.
 * Reveal is absent because it is IMPOSSIBLE: only the token's SHA-256 is stored, so
 * a lost token can be replaced but never shown again.
 */
static oc_dbres *process_set_webhook_state(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    /* Authorised by the webhook's CHANNEL, not by a tenant role: a webhook belongs
     * to a channel, and its members are who can post there anyway. */
    sqlite3_stmt *st = NULL;
    uint64_t cid = 0;
    sqlite3_prepare_v2(db, "SELECT channel_id FROM webhooks WHERE id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->message_id);
    if (sqlite3_step(st) == SQLITE_ROW) cid = (uint64_t)sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    if (!cid) { r->type = OC_RES_WEBHOOK_ERR; r->err_code = OC_ERR_UNKNOWN_WEBHOOK; return r; }
    if (!is_member(db, cid, j->user_id)) {
        r->type = OC_RES_WEBHOOK_ERR; r->err_code = OC_ERR_NOT_A_MEMBER; return r;
    }
    sqlite3_prepare_v2(db, "UPDATE webhooks SET disabled=? WHERE id=?;", -1, &st, NULL);
    sqlite3_bind_int(st, 1, j->hook_disabled ? 1 : 0);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)j->message_id);
    sqlite3_step(st);
    sqlite3_finalize(st);
    audit_actor(db, OC_AUDIT_ADMIN, j->hook_disabled ? "webhook.disable" : "webhook.enable",
                j->user_id, j->message_id, NULL, 1, NULL);
    /* Reply with the channel's list so the client's view cannot drift from the
     * truth — the same shape LIST_WEBHOOKS answers with. */
    oc_job lj = *j; lj.channel_id = cid;
    free(r);
    return process_list_webhooks(db, &lj);
}

static oc_dbres *process_rotate_webhook(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    sqlite3_stmt *st = NULL;
    uint64_t cid = 0;
    sqlite3_prepare_v2(db, "SELECT channel_id FROM webhooks WHERE id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->message_id);
    if (sqlite3_step(st) == SQLITE_ROW) cid = (uint64_t)sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    if (!cid) { r->type = OC_RES_WEBHOOK_ERR; r->err_code = OC_ERR_UNKNOWN_WEBHOOK; return r; }
    if (!is_member(db, cid, j->user_id)) {
        r->type = OC_RES_WEBHOOK_ERR; r->err_code = OC_ERR_NOT_A_MEMBER; return r;
    }
    uint8_t token[OC_SESSION_TOKEN_LEN], hash[OC_SHA256_LEN];
    if (oc_rand_bytes(token, sizeof token) != 0 || oc_sha256(token, sizeof token, hash) != 0) {
        r->type = OC_RES_WEBHOOK_ERR; r->err_code = OC_ERR_INTERNAL; return r;
    }
    sqlite3_prepare_v2(db, "UPDATE webhooks SET token_hash=? WHERE id=?;", -1, &st, NULL);
    sqlite3_bind_blob (st, 1, hash, sizeof hash, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)j->message_id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) { r->type = OC_RES_WEBHOOK_ERR; r->err_code = OC_ERR_INTERNAL; return r; }
    audit_actor(db, OC_AUDIT_ADMIN, "webhook.rotate", j->user_id, j->message_id, NULL, 1, NULL);
    /* The same shown-once frame CREATE uses: it is the same situation, and the old
     * token stops working the instant this commits. */
    r->type = OC_RES_WEBHOOK_CREATED;
    r->message_id = j->message_id;
    r->channel_id = cid;
    memcpy(r->session_token, token, sizeof token);
    return r;
}

/* --- Drafts (REQ-223, ARCH-101) ------------------------------------------ */

/* Upsert a draft, or delete it when the body is empty. Returns the row as
 * OC_RES_DRAFT so the net thread can fan it to the user's other devices; a
 * delete is carried as the same frame with an empty body, which is exactly what
 * the client folds as "this draft is gone". */
static oc_dbres *process_set_draft(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->user_id = j->user_id;
    /* Read access, as every other read of a conversation has it: a public
     * channel you have not joined is one you can type in (sending joins you),
     * so a draft there is yours to keep. A private channel you are not in would
     * leak its existence back to you on any other device, so that is refused.
     * Membership alone was too strict: the client writes a conversation's draft
     * on the way out, and every public channel you had only read answered
     * "you are not in that channel" as you left it.
     *
     * channel_id 0 is the UNADDRESSED case (REQ-229) and has no channel to
     * check: it belongs to nobody's conversation yet, which is the point. */
    if (j->channel_id && !channel_read_access(db, j->channel_id, j->user_id)) {
        r->type = OC_RES_LIST_ERR; r->err_code = OC_ERR_NOT_A_MEMBER; return r;
    }
    if (channel_is_e2e(db, j->channel_id) && j->channel_id) { r->type = OC_RES_LIST_ERR; r->err_code = OC_ERR_FORBIDDEN; return r; }
    size_t blen = j->body_len > OC_DRAFT_BODY_MAX ? OC_DRAFT_BODY_MAX : j->body_len;
    /* One reading, used for the stored row and the echoed one: two calls can
     * straddle a millisecond, and a device that compares them would think the
     * copy it just received was newer than the one it wrote. */
    uint64_t now = dbw_now_ms();
    sqlite3_stmt *st = NULL;
    if (!j->channel_id) {
        /* Unaddressed: one per user, which is what the pane holds. Written as
         * delete-then-insert because the partial unique index deliberately does
         * NOT cover these rows — there is no conversation to be unique on. */
        sqlite3_prepare_v2(db, "DELETE FROM drafts WHERE user_id=? AND channel_id IS NULL;",
                           -1, &st, NULL);
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->user_id);
        sqlite3_step(st); sqlite3_finalize(st);
        if (blen) {
            sqlite3_prepare_v2(db,
                "INSERT INTO drafts (user_id, channel_id, thread_root, recipients, body, updated_ms) "
                "VALUES (?, NULL, 0, ?, ?, ?);", -1, &st, NULL);
            sqlite3_bind_int64(st, 1, (sqlite3_int64)j->user_id);
            sqlite3_bind_text (st, 2, j->recipients ? j->recipients : "", -1, SQLITE_TRANSIENT);
            sqlite3_bind_text (st, 3, (const char *)j->body, (int)blen, SQLITE_TRANSIENT);
            sqlite3_bind_int64(st, 4, (sqlite3_int64)now);
            sqlite3_step(st); sqlite3_finalize(st);
        }
        r->type = OC_RES_DRAFT;
        r->draft.id = (uint64_t)sqlite3_last_insert_rowid(db);
        r->draft.channel_id = 0;
        r->draft.thread_root = 0;
        r->draft.updated_ms = now;
        r->draft.recipients = strdup(j->recipients ? j->recipients : "");
        r->draft.body = blen ? strndup((const char *)j->body, blen) : strdup("");
        return r;
    }
    if (blen == 0) {
        sqlite3_prepare_v2(db,
            "DELETE FROM drafts WHERE user_id=? AND channel_id=? AND thread_root=?;",
            -1, &st, NULL);
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->user_id);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)j->channel_id);
        sqlite3_bind_int64(st, 3, (sqlite3_int64)j->parent_id);
        sqlite3_step(st); sqlite3_finalize(st);
    } else {
        sqlite3_prepare_v2(db,
            "INSERT INTO drafts (user_id, channel_id, thread_root, body, updated_ms) "
            "VALUES (?,?,?,?,?) "
            /* The WHERE is not decoration: migration 0031 made this a PARTIAL
             * unique index, and SQLite requires a partial index's predicate
             * repeated here or the target "does not match any PRIMARY KEY or
             * UNIQUE constraint" and every upsert fails. It did — silently, from
             * the daemon's side, which is why the integration test hung waiting
             * for a reply that was never coming rather than failing. */
            "ON CONFLICT(user_id, channel_id, thread_root) WHERE channel_id IS NOT NULL "
            "DO UPDATE SET body=excluded.body, updated_ms=excluded.updated_ms;", -1, &st, NULL);
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->user_id);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)j->channel_id);
        sqlite3_bind_int64(st, 3, (sqlite3_int64)j->parent_id);
        sqlite3_bind_text (st, 4, (const char *)j->body, (int)blen, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 5, (sqlite3_int64)now);
        sqlite3_step(st); sqlite3_finalize(st);
    }
    r->type = OC_RES_DRAFT;
    r->draft.id          = (uint64_t)sqlite3_last_insert_rowid(db);
    r->draft.channel_id  = j->channel_id;
    r->draft.thread_root = j->parent_id;
    r->draft.updated_ms  = now;
    r->draft.recipients  = strdup("");
    r->draft.body = blen ? strndup((const char *)j->body, blen) : strdup("");
    return r;
}

/* Every draft this user holds, newest first. Bounded by OC_MAX_DRAFTS: the cap
 * exists so one account cannot make a reply the net thread has to stream
 * without end, not because more would be meaningless. */
static oc_dbres *process_list_drafts(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->user_id = j->user_id;
    r->type = OC_RES_DRAFTS;
    size_t cap = 16;
    r->drafts = malloc(cap * sizeof *r->drafts);
    if (!r->drafts) return r;
    sqlite3_stmt *st = NULL;
    /* Listed for the conversations the user can READ, the rule the write
     * follows (process_set_draft): a public channel whether or not they have
     * joined it, a private one only while they are in it. A draft for a private
     * channel the user has since left stays stored (ARCH-101: leaving is
     * reversible) but is not listed — "a draft for a channel you are not in is
     * simply invisible until you return". */
    sqlite3_prepare_v2(db,
        /* LEFT JOINs, and the test allows a NULL channel: an UNADDRESSED draft
         * (REQ-229) belongs to no conversation yet, so an inner join would hide
         * exactly the drafts the New Message pane depends on. */
        "SELECT d.channel_id, d.thread_root, d.body, d.updated_ms, d.id, d.recipients "
        "FROM drafts d "
        "LEFT JOIN channel_members m ON m.channel_id = d.channel_id AND m.user_id = d.user_id "
        "LEFT JOIN channels c ON c.id = d.channel_id "
        "WHERE d.user_id=? AND (d.channel_id IS NULL OR m.user_id IS NOT NULL OR c.is_public = 1) "
        "ORDER BY d.updated_ms DESC LIMIT ?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->user_id);
    sqlite3_bind_int  (st, 2, (int)OC_MAX_DRAFTS);
    while (sqlite3_step(st) == SQLITE_ROW && r->n_drafts < OC_MAX_DRAFTS) {
        if (r->n_drafts == cap) {
            cap *= 2;
            oc_draft_row *g = realloc(r->drafts, cap * sizeof *g);
            if (!g) break;
            r->drafts = g;
        }
        const unsigned char *b = sqlite3_column_text(st, 2);
        const unsigned char *rc2 = sqlite3_column_text(st, 5);
        r->drafts[r->n_drafts].channel_id  = (uint64_t)sqlite3_column_int64(st, 0);
        r->drafts[r->n_drafts].thread_root = (uint64_t)sqlite3_column_int64(st, 1);
        r->drafts[r->n_drafts].body        = strdup(b ? (const char *)b : "");
        r->drafts[r->n_drafts].updated_ms  = (uint64_t)sqlite3_column_int64(st, 3);
        r->drafts[r->n_drafts].id          = (uint64_t)sqlite3_column_int64(st, 4);
        r->drafts[r->n_drafts].recipients  = strdup(rc2 ? (const char *)rc2 : "");
        r->n_drafts++;
    }
    sqlite3_finalize(st);
    return r;
}

/* --- Scheduled messages (REQ-224, ARCH-102) ------------------------------ */

/* Load one row into `out`; 0 if it is not this user's or does not exist. */
static int sched_load(sqlite3 *db, uint64_t id, uint64_t user_id, oc_sched_row *out) {
    sqlite3_stmt *st = NULL;
    int got = 0;
    sqlite3_prepare_v2(db,
        "SELECT id, channel_id, thread_root, send_at_ms, created_ms, state, "
        "       fail_reason, body FROM scheduled_messages WHERE id=? AND user_id=?;",
        -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)id);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)user_id);
    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *stt = sqlite3_column_text(st, 5);
        const unsigned char *fr  = sqlite3_column_text(st, 6);
        const unsigned char *bd  = sqlite3_column_text(st, 7);
        out->id          = (uint64_t)sqlite3_column_int64(st, 0);
        out->channel_id  = (uint64_t)sqlite3_column_int64(st, 1);
        out->thread_root = (uint64_t)sqlite3_column_int64(st, 2);
        out->send_at_ms  = (uint64_t)sqlite3_column_int64(st, 3);
        out->created_ms  = (uint64_t)sqlite3_column_int64(st, 4);
        out->state = (stt && !strcmp((const char *)stt, "sent"))   ? OC_SCHED_SENT :
                     (stt && !strcmp((const char *)stt, "failed")) ? OC_SCHED_FAILED
                                                                   : OC_SCHED_PENDING;
        out->fail_reason = strdup(fr ? (const char *)fr : "");
        out->body        = strdup(bd ? (const char *)bd : "");
        got = 1;
    }
    sqlite3_finalize(st);
    return got;
}

static oc_dbres *process_schedule(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id; r->user_id = j->user_id;
    if (!j->channel_id || !is_member(db, j->channel_id, j->user_id)) {
        r->type = OC_RES_LIST_ERR; r->err_code = OC_ERR_NOT_A_MEMBER; return r;
    }
    if (channel_is_e2e(db, j->channel_id)) { r->type = OC_RES_LIST_ERR; r->err_code = OC_ERR_FORBIDDEN; return r; }
    /* An empty scheduled message is refused rather than stored: there is nothing
     * to deliver, and a row that fires into nothing at 09:00 is worse than a
     * refusal now. BODY_TOO_LARGE would be a lie about which end of the range
     * was wrong, so this reuses the send path's own "there is no message here"
     * refusal — INVALID_MESSAGE — added beside it. */
    if (!j->body_len) { r->type = OC_RES_LIST_ERR; r->err_code = OC_ERR_INVALID_MESSAGE; return r; }
    size_t blen = j->body_len > OC_MAX_BODY_SIZE ? OC_MAX_BODY_SIZE : j->body_len;
    uint64_t now = dbw_now_ms();
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "INSERT INTO scheduled_messages "
        "  (user_id, channel_id, thread_root, body, send_at_ms, created_ms, state) "
        "VALUES (?,?,?,?,?,?,'pending');", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->user_id);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)j->channel_id);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)j->parent_id);
    sqlite3_bind_text (st, 4, (const char *)j->body, (int)blen, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 5, (sqlite3_int64)j->sched_at_ms);
    sqlite3_bind_int64(st, 6, (sqlite3_int64)now);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) { r->type = OC_RES_LIST_ERR; r->err_code = OC_ERR_INTERNAL; return r; }
    r->type = OC_RES_SCHEDULED;
    r->sched.id          = (uint64_t)sqlite3_last_insert_rowid(db);
    r->sched.channel_id  = j->channel_id;
    r->sched.thread_root = j->parent_id;
    r->sched.send_at_ms  = j->sched_at_ms;
    r->sched.created_ms  = now;
    r->sched.state       = OC_SCHED_PENDING;
    r->sched.fail_reason = strdup("");
    r->sched.body        = strndup((const char *)j->body, blen);
    return r;
}

static oc_dbres *process_list_scheduled(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id; r->user_id = j->user_id;
    r->type = OC_RES_SCHEDULED_LIST;
    size_t cap = 16;
    r->scheds = malloc(cap * sizeof *r->scheds);
    if (!r->scheds) return r;
    sqlite3_stmt *st = NULL;
    /* Everything still waiting, plus anything that FAILED — a message that was
     * promised and could not be sent is the one thing its author must see
     * (ARCH-102). Delivered ones drop out: they are messages now. */
    sqlite3_prepare_v2(db,
        "SELECT id, channel_id, thread_root, send_at_ms, created_ms, state, fail_reason, body "
        "FROM scheduled_messages WHERE user_id=? AND state IN ('pending','failed') "
        "ORDER BY send_at_ms ASC LIMIT ?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->user_id);
    sqlite3_bind_int  (st, 2, (int)OC_MAX_SCHEDULED);
    while (sqlite3_step(st) == SQLITE_ROW && r->n_scheds < OC_MAX_SCHEDULED) {
        if (r->n_scheds == cap) {
            cap *= 2;
            oc_sched_row *g = realloc(r->scheds, cap * sizeof *g);
            if (!g) break;
            r->scheds = g;
        }
        oc_sched_row *row = &r->scheds[r->n_scheds];
        const unsigned char *stt = sqlite3_column_text(st, 5);
        const unsigned char *fr  = sqlite3_column_text(st, 6);
        const unsigned char *bd  = sqlite3_column_text(st, 7);
        row->id          = (uint64_t)sqlite3_column_int64(st, 0);
        row->channel_id  = (uint64_t)sqlite3_column_int64(st, 1);
        row->thread_root = (uint64_t)sqlite3_column_int64(st, 2);
        row->send_at_ms  = (uint64_t)sqlite3_column_int64(st, 3);
        row->created_ms  = (uint64_t)sqlite3_column_int64(st, 4);
        row->state = (stt && !strcmp((const char *)stt, "failed")) ? OC_SCHED_FAILED
                                                                  : OC_SCHED_PENDING;
        row->fail_reason = strdup(fr ? (const char *)fr : "");
        row->body        = strdup(bd ? (const char *)bd : "");
        r->n_scheds++;
    }
    sqlite3_finalize(st);
    return r;
}

static oc_dbres *process_cancel_scheduled(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id; r->user_id = j->user_id;
    oc_sched_row row = {0};
    if (!sched_load(db, j->message_id, j->user_id, &row)) {
        r->type = OC_RES_LIST_ERR; r->err_code = OC_ERR_UNKNOWN_MESSAGE; return r;
    }
    free(row.fail_reason); free(row.body);
    sqlite3_stmt *st = NULL;
    /* Only a message that has not gone: cancelling one already delivered is a
     * request to unsend, which is REQ-058's territory and not this op's. */
    sqlite3_prepare_v2(db,
        "DELETE FROM scheduled_messages WHERE id=? AND user_id=? AND state<>'sent';",
        -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->message_id);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)j->user_id);
    sqlite3_step(st); sqlite3_finalize(st);
    r->type = OC_RES_SCHEDULED;
    r->sched.id = j->message_id;
    r->sched.state = OC_SCHED_GONE;      /* the client drops it on this */
    r->sched.fail_reason = strdup("");
    r->sched.body = strdup("");
    return r;
}

static oc_dbres *process_update_scheduled(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id; r->user_id = j->user_id;
    oc_sched_row row = {0};
    if (!sched_load(db, j->message_id, j->user_id, &row) || row.state == OC_SCHED_SENT) {
        free(row.fail_reason); free(row.body);
        r->type = OC_RES_LIST_ERR; r->err_code = OC_ERR_UNKNOWN_MESSAGE; return r;
    }
    size_t blen = j->body_len > OC_MAX_BODY_SIZE ? OC_MAX_BODY_SIZE : j->body_len;
    sqlite3_stmt *st = NULL;
    /* Editing a FAILED one puts it back in the queue: the author has just fixed
     * whatever the reason said, and making them cancel and retype would be a
     * punishment for the daemon's report. */
    sqlite3_prepare_v2(db,
        "UPDATE scheduled_messages SET body=?, send_at_ms=?, state='pending', "
        "  fail_reason=NULL WHERE id=? AND user_id=?;", -1, &st, NULL);
    sqlite3_bind_text (st, 1, blen ? (const char *)j->body : row.body, blen ? (int)blen : -1,
                       SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)(j->sched_at_ms ? j->sched_at_ms : row.send_at_ms));
    sqlite3_bind_int64(st, 3, (sqlite3_int64)j->message_id);
    sqlite3_bind_int64(st, 4, (sqlite3_int64)j->user_id);
    sqlite3_step(st); sqlite3_finalize(st);
    free(row.fail_reason); free(row.body);
    r->type = OC_RES_SCHEDULED;
    if (!sched_load(db, j->message_id, j->user_id, &r->sched)) {
        r->sched.fail_reason = strdup(""); r->sched.body = strdup("");
    }
    return r;
}

/* The sweep, ONE due message per job. Delivery is literally `process_send`, and
 * the result is handed back UNCHANGED — so the net thread broadcasts it, pushes
 * it and acks it through the same code a typed message uses, without knowing it
 * was scheduled. That is ARCH-102's "the ordinary send path" taken at its word
 * rather than reimplemented beside it.
 *
 * One at a time because the result IS the send: the netloop raises another job
 * as soon as one fires (see maybe_fire_scheduled), so a backlog drains at queue
 * speed rather than one per tick. */
/* Reminders come due (REQ-283): every saved item whose time has passed and
 * that has not fired, still the owner's to read, is marked fired and answered
 * as one OC_RES_REMINDER_DUE each, for the owner's connections and push. Run on
 * the scheduled-message tick, so a reminder lands within that tick of its
 * time. A batch at a time; the rest on the next tick. */
static oc_dbres *process_fire_reminders(sqlite3 *db, const oc_job *j) {
    (void)j;
    uint64_t now = dbw_now_ms();
    oc_dbres *head = NULL, **tail = &head;
    sqlite3_stmt *st = NULL;
    sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, NULL);
    if (sqlite3_prepare_v2(db,
            "SELECT s.user_id, s.message_id, m.channel_id, s.created_at_ms, s.remind_at_ms "
            "  FROM saved_items s JOIN messages m ON m.id = s.message_id "
            " WHERE s.remind_at_ms IS NOT NULL AND s.reminded_at_ms IS NULL AND s.remind_at_ms <= ?1 "
            "   AND m.deleted_at_ms IS NULL "
            "   AND EXISTS(SELECT 1 FROM channel_members cm "
            "               WHERE cm.channel_id = m.channel_id AND cm.user_id = s.user_id) "
            " ORDER BY s.remind_at_ms LIMIT 64;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)now);
        while (sqlite3_step(st) == SQLITE_ROW) {
            oc_dbres *r = calloc(1, sizeof *r);
            if (!r) break;
            r->type = OC_RES_REMINDER_DUE;
            r->user_id    = (uint64_t)sqlite3_column_int64(st, 0);
            r->message_id = (uint64_t)sqlite3_column_int64(st, 1);
            r->channel_id = (uint64_t)sqlite3_column_int64(st, 2);
            r->saved_at   = (uint64_t)sqlite3_column_int64(st, 3);
            r->remind_at  = (uint64_t)sqlite3_column_int64(st, 4);
            r->server_time = now;
            *tail = r; tail = &r->next;
        }
        sqlite3_finalize(st);
    }
    for (oc_dbres *r = head; r; r = r->next) {
        if (sqlite3_prepare_v2(db, "UPDATE saved_items SET reminded_at_ms=?3 WHERE user_id=?1 AND message_id=?2;",
                               -1, &st, NULL) != SQLITE_OK) break;
        sqlite3_bind_int64(st, 1, (sqlite3_int64)r->user_id);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)r->message_id);
        sqlite3_bind_int64(st, 3, (sqlite3_int64)now);
        sqlite3_step(st); sqlite3_finalize(st);
    }
    sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
    if (!head) {                                /* nothing due: a benign no-op */
        head = calloc(1, sizeof *head);
        if (head) head->type = OC_RES_REMINDER_DUE;
    }
    return head;
}

static oc_dbres *process_fire_scheduled(sqlite3 *db, const oc_job *j) {
    (void)j;
    uint64_t id = 0, uid = 0, cid = 0, root = 0;
    char *body = NULL;
    uint64_t now = dbw_now_ms();
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "SELECT id, user_id, channel_id, thread_root, body FROM scheduled_messages "
        "WHERE state='pending' AND send_at_ms <= ? ORDER BY send_at_ms LIMIT 1;",
        -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)now);
    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *b = sqlite3_column_text(st, 4);
        id   = (uint64_t)sqlite3_column_int64(st, 0);
        uid  = (uint64_t)sqlite3_column_int64(st, 1);
        cid  = (uint64_t)sqlite3_column_int64(st, 2);
        root = (uint64_t)sqlite3_column_int64(st, 3);
        body = strdup(b ? (const char *)b : "");
    }
    sqlite3_finalize(st);
    if (!id) {                                  /* nothing due: a benign no-op */
        oc_dbres *idle = calloc(1, sizeof *idle);
        if (idle) idle->type = OC_RES_SCHED_FIRED;
        free(body);
        return idle;
    }

    /* The two ways a promise can no longer be kept (ARCH-102), both reported
     * rather than silently dropped or forced through a read-only channel. */
    const char *fail = NULL;
    if (!is_member(db, cid, uid)) fail = "you are no longer in that conversation";
    else {
        sqlite3_stmt *cs = NULL;
        int archived = 0;
        /* `archived_at_ms`, not `archived`: REQ-035 stores WHEN, and non-NULL is
         * the flag (migration 0026). Named wrongly here at first, which prepared
         * a statement that never stepped and left every channel looking open —
         * the failure was silent, and only visible because the test asserted
         * the specific REASON rather than merely that it failed. */
        sqlite3_prepare_v2(db, "SELECT archived_at_ms IS NOT NULL FROM channels WHERE id=?;",
                           -1, &cs, NULL);
        sqlite3_bind_int64(cs, 1, (sqlite3_int64)cid);
        if (sqlite3_step(cs) == SQLITE_ROW) archived = sqlite3_column_int(cs, 0);
        sqlite3_finalize(cs);
        if (archived) fail = "that channel was archived before it could be sent";
    }
    if (fail) {
        sqlite3_prepare_v2(db,
            "UPDATE scheduled_messages SET state='failed', fail_reason=? WHERE id=?;",
            -1, &st, NULL);
        sqlite3_bind_text (st, 1, fail, -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)id);
        sqlite3_step(st); sqlite3_finalize(st);
        free(body);
        oc_dbres *failed = calloc(1, sizeof *failed);
        if (failed) failed->type = OC_RES_SCHED_FIRED;   /* the author sees it on next list */
        return failed;
    }

    oc_job sj;
    memset(&sj, 0, sizeof sj);
    /* A reply is a different send: process_send does not read parent_id, so a
     * scheduled thread reply went out as a channel message. It takes the reply
     * path, which also fans it out as a THREAD_REPLY rather than to the main
     * scroll (REQ-060). */
    sj.type = root ? OC_JOB_SEND_REPLY : OC_JOB_SEND;
    sj.conn_id = 0;                    /* nobody is waiting for an ack */
    sj.user_id = uid;
    sj.channel_id = cid;
    sj.parent_id = root;
    sj.body = (uint8_t *)body;
    sj.body_len = body ? strlen(body) : 0;
    /* The token is minted HERE (REQ-093, ARCH-102): it identifies a send
     * attempt, and the client's belonged to the scheduling request. Derived
     * from the row id, so a row swept twice deduplicates instead of double
     * posting — which is exactly what idempotency is for. */
    memcpy(sj.idem, "sched", 5);
    memcpy(sj.idem + 5, &id, sizeof id);
    oc_dbres *sent = root ? process_send_reply(db, &sj) : process_send(db, &sj);
    int ok = sent && (sent->type == OC_RES_SEND_OK || sent->type == OC_RES_REPLY_OK);
    /* The one failure a reply adds: the message it answers is gone. Named, like
     * the two above, because a promise not kept is owed a reason. */
    const char *why = (sent && sent->type == OC_RES_REPLY_ERR && sent->err_code == OC_ERR_UNKNOWN_MESSAGE)
                    ? "the message it replies to was deleted"
                    : "the server could not deliver it";
    sqlite3_prepare_v2(db, ok
        ? "UPDATE scheduled_messages SET state='sent', message_id=? WHERE id=?;"
        : "UPDATE scheduled_messages SET state='failed', message_id=?, "
          "  fail_reason=? WHERE id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)(ok ? sent->message_id : 0));
    if (ok) sqlite3_bind_int64(st, 2, (sqlite3_int64)id);
    else  { sqlite3_bind_text(st, 2, why, -1, SQLITE_STATIC);
            sqlite3_bind_int64(st, 3, (sqlite3_int64)id); }
    sqlite3_step(st); sqlite3_finalize(st);
    free(body);
    if (sent) return sent;             /* handed back UNCHANGED — see the note above */
    oc_dbres *none = calloc(1, sizeof *none);
    if (none) none->type = OC_RES_SCHED_FIRED;
    return none;
}

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
    /* A group's handle is not a name a person can take: @name has to mean one
     * thing (REQ-307). */
    if (group_by_handle(db, name)) return profile_err(j, OC_ERR_GROUP_HANDLE_TAKEN);
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "UPDATE users SET display_name=? WHERE id=?;", -1, &st, NULL);
    sqlite3_bind_text (st, 1, name, (int)nlen, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)j->user_id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) return profile_err(j, OC_ERR_INTERNAL);
    return profile_ok(j, strdup(name));
}

/* Change your local password (AUTH.md §2). Like a sign-in, it passes through a
 * reader, which fetches the credential and its version, and the auth pool, which
 * checks the old password and derives the new one; it comes back here CHECKED.
 * The new one is stored only if the credential is still the version the old one
 * was checked against -- one statement, so no other change can land between
 * the test and the write. A non-local (OIDC) account, a wrong old password, or a
 * change that another beat to it is FORBIDDEN. On success the self ack echoes
 * the unchanged display name. Write. */
/* Store the new password a check derived, against the version that was checked
 * -- one statement, so no other change can land between the test and the
 * write -- and revoke every session of the user but `keep_session` (0: all),
 * since each was opened with the old password (REQ-182). 0, or -1 with `err`
 * FORBIDDEN (changed meanwhile) or INTERNAL. */
static int store_new_password(sqlite3 *db, const oc_job *j, uint64_t uid, uint64_t keep_session,
                              uint16_t *err) {
    *err = OC_ERR_INTERNAL;
    if (!j->pw_derived) return -1;
    uint64_t version = next_credential_version(db);
    if (!version) return -1;
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "UPDATE local_credentials SET salt=?, kdf=?, hash=?, updated_at_ms=?, version=? "
        "WHERE user_id=? AND version=?;", -1, &st, NULL);
    sqlite3_bind_blob (st, 1, j->pw_salt, OC_PW_SALT_LEN, SQLITE_TRANSIENT);
    char kdf[OC_PW_KDF_MAX];
    oc_pw_kdf_format(j->pw_mem, kdf);
    sqlite3_bind_text (st, 2, kdf, -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob (st, 3, j->pw_hash, OC_PW_HASH_LEN, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 4, (sqlite3_int64)dbw_now_ms());
    sqlite3_bind_int64(st, 5, (sqlite3_int64)version);
    sqlite3_bind_int64(st, 6, (sqlite3_int64)uid);
    sqlite3_bind_int64(st, 7, (sqlite3_int64)j->auth_version);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) return -1;
    if (sqlite3_changes(db) != 1) { *err = OC_ERR_FORBIDDEN; return -1; }
    sqlite3_prepare_v2(db, "DELETE FROM sessions WHERE user_id=? AND id<>?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)uid);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)keep_session);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

/* ---- read-aloud (REQ-291-293, ARCH-111) ------------------------------------ */

/* The names a mention resolves to, for the speakable text. The roster is looked
 * up per name rather than preloaded: a message has at most a handful of mentions
 * (OC_MENTION_MAX), and the same query the mention store uses answers it. */
typedef struct { sqlite3 *db; uint64_t channel_id; char name[64]; } speak_names;

static const char *speak_resolve(void *ctx, const char *name) {
    speak_names *sn = ctx;
    sqlite3_stmt *st = NULL;
    /* An id-form mention (<@U42>) comes as "U<digits>" (speakable.h). */
    uint64_t id = 0;
    if (name[0] == 'U' && name[1]) {
        const char *p = name + 1;
        while (*p >= '0' && *p <= '9') p++;
        if (!*p) id = strtoull(name + 1, NULL, 10);
    }
    if (sqlite3_prepare_v2(sn->db,
            "SELECT u.display_name FROM users u"
            "  JOIN channel_members cm ON cm.user_id = u.id"
            " WHERE cm.channel_id = ?1 AND u.disabled = 0"
            "   AND (CASE WHEN ?3 <> 0 THEN u.id = ?3"
            "             ELSE lower(u.display_name) = lower(?2) END) LIMIT 1;", -1, &st, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)sn->channel_id);
    sqlite3_bind_text(st, 2, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)id);
    const char *out = NULL;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *dn = sqlite3_column_text(st, 0);
        if (dn) { snprintf(sn->name, sizeof sn->name, "%s", (const char *)dn); out = sn->name; }
    }
    sqlite3_finalize(st);
    return out;
}


/* Everything one AUDIO_GET needs to decide (ARCH-111): the read gate, what is
 * said, in whose voice, and whether that rendering already exists. Read. */
/* A rendering's handle: what was said and who says it, so the same text in the
 * same voice is one rendering and an edit is simply another handle. Shared by
 * messages and voice previews, which is what lets a preview use the cache. */
static void tts_handle_of(const char *text, size_t n, int voice, uint8_t out[32]) {
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    mbedtls_sha256_update(&sha, (const unsigned char *)text, n);
    mbedtls_sha256_update(&sha, (const unsigned char *)"\0", 1);
    mbedtls_sha256_update(&sha, (const unsigned char *)&voice, sizeof voice);
    mbedtls_sha256_finish(&sha, out);
    mbedtls_sha256_free(&sha);
}

/* Whether `r`'s handle is already rendered for this model version. */
static void tts_probe_cache(sqlite3 *db, const oc_job *j, oc_dbres *r) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT blob_key, bytes, duration_ms FROM rendered_audio"
            " WHERE handle = ?1 AND model_version = ?2;", -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_blob(st, 1, r->tts_handle, 32, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, j->tts_model_version ? j->tts_model_version : "", -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) {
        r->tts_cached = 1;
        r->tts_blob_key = strdup((const char *)sqlite3_column_text(st, 0));
        r->tts_bytes = (uint64_t)sqlite3_column_int64(st, 1);
        r->tts_duration_ms = (uint32_t)sqlite3_column_int(st, 2);
    }
    sqlite3_finalize(st);
}

static oc_dbres *process_tts_lookup(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->tts_seq = j->tts_seq;
    r->message_id = j->message_id;
    r->type = OC_RES_TTS_ERR;
    r->err_code = OC_ERR_UNKNOWN_MESSAGE;

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT m.channel_id, m.author_id, m.deleted_at_ms IS NOT NULL, m.body,"
            "       COALESCE(u.voice_id,''), COALESCE(u.pronouns,''), m.kind,"
            /* An action (REQ-058) is said as its actor doing it: the name
             * first, then the action text. */
            "       ac.text_start, COALESCE(ac.actor_name, u.display_name, '')"
            "  FROM messages m LEFT JOIN users u ON u.id = m.author_id"
            "  LEFT JOIN actions ac ON ac.message_id = m.id"
            " WHERE m.id = ?1;", -1, &st, NULL) != SQLITE_OK)
        return r;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->message_id);
    if (sqlite3_step(st) != SQLITE_ROW || sqlite3_column_int(st, 2) != 0) {
        sqlite3_finalize(st);
        return r;
    }
    uint64_t cid = (uint64_t)sqlite3_column_int64(st, 0);
    uint64_t author = (uint64_t)sqlite3_column_int64(st, 1);
    const char *body = (const char *)sqlite3_column_blob(st, 3);
    size_t body_len = (size_t)sqlite3_column_bytes(st, 3);
    char have_voice[64], pronouns[64];
    snprintf(have_voice, sizeof have_voice, "%s", (const char *)sqlite3_column_text(st, 4));
    snprintf(pronouns, sizeof pronouns, "%s", (const char *)sqlite3_column_text(st, 5));
    /* A call event (REQ-304) is not something its author said: nothing to read. */
    if (sqlite3_column_int(st, 6) != OC_MSG_KIND_MESSAGE) body_len = 0;
    char actor[128] = "";
    if (sqlite3_column_type(st, 7) != SQLITE_NULL) {
        size_t ts = (size_t)sqlite3_column_int64(st, 7);
        if (ts < body_len) { body += ts; body_len -= ts; }
        snprintf(actor, sizeof actor, "%s", (const char *)sqlite3_column_text(st, 8));
    }
    char *body_copy = body_len ? malloc(body_len) : NULL;
    if (body_copy) memcpy(body_copy, body, body_len);
    sqlite3_finalize(st);

    /* The same gate as reading the message: speech is the message (REQ-291).
     * An encrypted DM's is ciphertext, nothing to say (PRIVATE-DM.md). */
    if (!channel_read_access(db, cid, j->user_id) || channel_is_e2e(db, cid)) {
        free(body_copy);
        r->err_code = OC_ERR_FORBIDDEN;
        return r;
    }
    r->channel_id = cid;
    r->tts_author_id = author;

    speak_names sn = { db, cid, "" };
    char *speak = malloc(OC_SPEAK_MAX + 1);
    size_t n = 0;
    if (speak && body_copy) {
        /* The actor's name, then the action, as one text: one rendering, and its
         * handle changes when either does. */
        size_t pre = 0;
        if (actor[0]) pre = (size_t)snprintf(speak, OC_SPEAK_MAX + 1, "%s ", actor);
        if (pre > OC_SPEAK_MAX / 2) pre = 0;
        size_t said = oc_speakable(body_copy, body_len, speak_resolve, &sn, speak + pre, OC_SPEAK_MAX + 1 - pre);
        n = said ? pre + said : 0;
    }
    free(body_copy);
    if (n == 0) {
        free(speak);
        r->err_code = OC_ERR_NOT_RENDERABLE;          /* nothing to say (REQ-294) */
        return r;
    }

    int persist = 0;
    int voice = oc_voice_pick(j->tts_lang, j->tts_voices, have_voice, pronouns, author, &persist);
    r->tts_voice = (uint8_t)voice;
    r->tts_persist = (uint8_t)persist;

    /* The handle is what was said and who says it, so the same text in the same
     * voice is one rendering and an edit is simply another handle. */
    tts_handle_of(speak, n, voice, r->tts_handle);
    r->type = OC_RES_TTS_META;
    r->tts_text = speak;
    tts_probe_cache(db, j, r);
    return r;
}

/* A voice's audition (REQ-292): its preview sentence, through exactly the path a
 * message takes -- the same handle, so the same cache, so each voice's sample is
 * rendered once for the whole deployment and then served. It is nobody's message:
 * there is no read gate, no author and no voice to persist, and it answers as
 * message 0, which no message is. */
/* Voice input's check before a segment is recognized (ARCH-112): the same
 * access a typed message would need, read-only -- free talk posts, so it needs
 * post access (a public channel the user has not joined is fine: the send will
 * join them, as a typed send does); push to talk only returns the words to the
 * speaker's own composer, so read access is enough. On success, the members'
 * display names, for a spoken "at Name". */
static oc_dbres *process_stt_prep(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->type = OC_RES_STT_PREP;
    r->stt_req = j->stt_req;
    r->channel_id = j->channel_id;
    uint8_t is_public = 0;
    if (!channel_exists(db, j->channel_id, &is_public)) { r->err_code = OC_ERR_UNKNOWN_CHANNEL; return r; }
    int member = is_member(db, j->channel_id, j->user_id);
    if (j->stt_mode == OC_STT_MODE_FREE) {
        if (channel_is_archived(db, j->channel_id)) { r->err_code = OC_ERR_CHANNEL_ARCHIVED; return r; }
        if (!member && !is_public) { r->err_code = OC_ERR_NOT_A_MEMBER; return r; }
    } else if (!member && !is_public) {
        r->err_code = OC_ERR_NOT_A_MEMBER;
        return r;
    }
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT u.display_name FROM channel_members m JOIN users u ON u.id = m.user_id "
            "WHERE m.channel_id = ?1 AND u.disabled = 0 AND u.display_name IS NOT NULL "
            "AND u.display_name <> '' LIMIT 512;", -1, &st, NULL) != SQLITE_OK)
        return r;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->channel_id);
    size_t cap = 16;
    r->stt_names = malloc(cap * sizeof *r->stt_names);
    while (r->stt_names && sqlite3_step(st) == SQLITE_ROW) {
        if (r->n_stt_names == cap) {
            char **g = realloc(r->stt_names, cap * 2 * sizeof *g);
            if (!g) break;
            r->stt_names = g;
            cap *= 2;
        }
        const char *name = (const char *)sqlite3_column_text(st, 0);
        char *dup = name ? strdup(name) : NULL;
        if (dup) r->stt_names[r->n_stt_names++] = dup;
    }
    sqlite3_finalize(st);
    return r;
}

static oc_dbres *process_tts_preview(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->tts_seq = j->tts_seq;
    r->message_id = 0;
    r->type = OC_RES_TTS_ERR;
    r->err_code = OC_ERR_NOT_RENDERABLE;
    if (!j->tts_text || !j->tts_text[0]) return r;
    int voice = (int)j->tts_voice;
    r->tts_voice = (uint8_t)voice;
    tts_handle_of(j->tts_text, strlen(j->tts_text), voice, r->tts_handle);
    r->tts_text = strdup(j->tts_text);
    if (!r->tts_text) { r->err_code = OC_ERR_INTERNAL; return r; }
    r->type = OC_RES_TTS_META;
    r->tts_warm = j->tts_warm;
    tts_probe_cache(db, j, r);
    return r;
}

/* A finished rendering becomes a cache row. Write, fire and forget: if it is
 * lost the next request renders again, which is the whole point of a cache that
 * needs no tombstone. */
static oc_dbres *process_tts_store(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = 0;
    /* Said whether or not the row went in: the net thread lets go of the
     * rendering either way, and a lookup after a failed write renders again. */
    r->type = OC_RES_TTS_STORED;
    memcpy(r->tts_handle, j->tts_handle, 32);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO rendered_audio"
            "  (handle, model_version, blob_key, bytes, duration_ms, created_at_ms, last_used_ms)"
            "  VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?6);", -1, &st, NULL) != SQLITE_OK)
        return r;
    uint64_t now = dbw_now_ms();
    sqlite3_bind_blob (st, 1, j->tts_handle, 32, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, 2, j->tts_model_version ? j->tts_model_version : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, 3, j->tts_blob_key ? j->tts_blob_key : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 4, (sqlite3_int64)j->tts_bytes);
    sqlite3_bind_int  (st, 5, (int)j->tts_duration_ms);
    sqlite3_bind_int64(st, 6, (sqlite3_int64)now);
    sqlite3_step(st);
    sqlite3_finalize(st);
    return r;
}

/* Serving a stored rendering makes it recently used, which is what the storage
 * sweep evicts by. Write, fire and forget. */
static oc_dbres *process_tts_touch(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = 0;
    r->type = OC_RES_OK;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "UPDATE rendered_audio SET last_used_ms = ?3"
            " WHERE handle = ?1 AND model_version = ?2;", -1, &st, NULL) != SQLITE_OK)
        return r;
    sqlite3_bind_blob (st, 1, j->tts_handle, 32, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, 2, j->tts_model_version ? j->tts_model_version : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)dbw_now_ms());
    sqlite3_step(st);
    sqlite3_finalize(st);
    return r;
}

/* A message's speech was looked up under a handle (REQ-250): recorded, so the
 * rendering goes when the message ages out. Fire and forget, as a touch is. */
static oc_dbres *process_tts_use(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = 0;
    r->type = OC_RES_OK;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT OR IGNORE INTO rendered_audio_uses(message_id, handle)"
            " SELECT id, ?2 FROM messages WHERE id = ?1;", -1, &st, NULL) != SQLITE_OK)
        return r;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->message_id);
    sqlite3_bind_blob (st, 2, j->tts_handle, 32, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
    return r;
}

/* The export of a video message (ARCH-135). The gate is the download's: the
 * copy is the video, so whoever may read the video may have it. Then it must be
 * a video message -- an attachment with a media row of kind 1 -- whose bytes are
 * still here and are not an encrypted DM's ciphertext. The answer names the
 * copy already made for these codecs, or the source to make it from. */
static oc_dbres *process_export_lookup(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->exp_attachment_id = j->attachment_id;
    r->type = OC_RES_EXPORT_ERR;
    r->err_code = OC_ERR_UNKNOWN_ATTACHMENT;

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT a.channel_id, a.storage_key, a.size, a.reclaimed_at_ms, m.kind"
            "  FROM attachments a LEFT JOIN attachment_media m ON m.attachment_id = a.id"
            " WHERE a.id = ?1 AND a.sha256 IS NOT NULL AND a.message_id IS NOT NULL;", -1, &st, NULL) != SQLITE_OK)
        return r;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->attachment_id);
    if (sqlite3_step(st) != SQLITE_ROW) { sqlite3_finalize(st); return r; }
    uint64_t cid = (uint64_t)sqlite3_column_int64(st, 0);
    const char *key = (const char *)sqlite3_column_text(st, 1);
    char *src_key = strdup(key ? key : "");
    uint64_t size = (uint64_t)sqlite3_column_int64(st, 2);
    int gone = sqlite3_column_int64(st, 3) != 0;
    int video = sqlite3_column_type(st, 4) != SQLITE_NULL && sqlite3_column_int(st, 4) == 1;
    sqlite3_finalize(st);
    if (!src_key) { r->err_code = OC_ERR_INTERNAL; return r; }
    if (!channel_read_access(db, cid, j->user_id)) { free(src_key); r->err_code = OC_ERR_FORBIDDEN; return r; }
    if (gone) { free(src_key); r->err_code = OC_ERR_ATTACHMENT_GONE; return r; }
    if (!video || channel_is_e2e(db, cid)) { free(src_key); r->err_code = OC_ERR_MEDIA_INVALID; return r; }

    r->type = OC_RES_EXPORT_META;
    r->channel_id = cid;
    r->exp_src_key = src_key;
    r->exp_src_size = size;
    if (sqlite3_prepare_v2(db,
            "SELECT blob_key, bytes, duration_ms FROM exported_video"
            " WHERE attachment_id = ?1 AND version = ?2;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->attachment_id);
        sqlite3_bind_text(st, 2, j->exp_version ? j->exp_version : "", -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) {
            const char *bk = (const char *)sqlite3_column_text(st, 0);
            r->exp_blob_key = strdup(bk ? bk : "");
            r->exp_bytes = (uint64_t)sqlite3_column_int64(st, 1);
            r->exp_duration_ms = (uint32_t)sqlite3_column_int(st, 2);
            r->exp_cached = r->exp_blob_key != NULL;
        }
    }
    sqlite3_finalize(st);
    return r;
}

/* A finished copy becomes a cache row, unless its video went while it was being
 * made: then the row would point at a copy of nothing, and the blob is left for
 * the orphan sweep. Write, fire and forget. */
static oc_dbres *process_export_store(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = 0;
    r->type = OC_RES_OK;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO exported_video"
            "  (attachment_id, version, blob_key, bytes, duration_ms, created_at_ms, last_used_ms)"
            "  SELECT ?1, ?2, ?3, ?4, ?5, ?6, ?6 FROM attachments"
            "   WHERE id = ?1 AND reclaimed_at_ms = 0;", -1, &st, NULL) != SQLITE_OK)
        return r;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->attachment_id);
    sqlite3_bind_text (st, 2, j->exp_version ? j->exp_version : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, 3, j->exp_blob_key ? j->exp_blob_key : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 4, (sqlite3_int64)j->exp_bytes);
    sqlite3_bind_int  (st, 5, (int)j->exp_duration_ms);
    sqlite3_bind_int64(st, 6, (sqlite3_int64)dbw_now_ms());
    sqlite3_step(st);
    sqlite3_finalize(st);
    return r;
}

/* Serving a stored copy makes it recently used, which is what the storage sweep
 * evicts by. Write, fire and forget. */
static oc_dbres *process_export_touch(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = 0;
    r->type = OC_RES_OK;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "UPDATE exported_video SET last_used_ms = ?3 WHERE attachment_id = ?1 AND version = ?2;",
            -1, &st, NULL) != SQLITE_OK)
        return r;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->attachment_id);
    sqlite3_bind_text (st, 2, j->exp_version ? j->exp_version : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)dbw_now_ms());
    sqlite3_step(st);
    sqlite3_finalize(st);
    return r;
}

/* Write back the voice the daemon chose for an author, once, so the profile
 * shows it. Never overwrites a voice the user set. Write, fire and forget. */
static oc_dbres *process_tts_voice_set(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = 0;
    r->type = OC_RES_OK;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "UPDATE users SET voice_id = ?2"
            " WHERE id = ?1 AND (voice_id IS NULL OR voice_id = '');", -1, &st, NULL) != SQLITE_OK)
        return r;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->user_id);
    sqlite3_bind_text (st, 2, j->pf_voice_id ? j->pf_voice_id : "", -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
    return r;
}

/* Read-only query jobs run on the reader connection (ARCH-66), off the writer
 * thread, so a heavy search/backfill can't stall message sends or auth. */
static oc_dbres *process_storage_status(sqlite3 *db, const oc_job *j);
static oc_dbres *process_audit_query(sqlite3 *db, const oc_job *j);
static oc_dbres *process_usage(sqlite3 *db, const oc_job *j);

static int is_read_job(int type) {
    return type == OC_JOB_BACKFILL || type == OC_JOB_HISTORY || type == OC_JOB_SEARCH ||
           type == OC_JOB_LIST_CHANNELS || type == OC_JOB_LIST_USERS ||
           type == OC_JOB_LIST_REACTIONS || type == OC_JOB_LIST_PINS ||
           type == OC_JOB_LIST_MEMBERS || type == OC_JOB_LIST_FILES ||
           type == OC_JOB_LIST_SAVED || type == OC_JOB_LIST_THREAD ||
           type == OC_JOB_TYPING || type == OC_JOB_ATTACH_LOOKUP ||
           type == OC_JOB_LIST_WEBHOOKS || type == OC_JOB_LIST_NOTIFY_PREFS ||
           /* Read-only, so it goes to the reader thread (ARCH-66) like every other
            * list. Its three siblings — revoke, set-state, rotate — write. */
           type == OC_JOB_LIST_INVITES || type == OC_JOB_GET_PROFILE ||
           type == OC_JOB_LIST_FILE_CHANNELS || type == OC_JOB_LIST_SESSIONS ||
           type == OC_JOB_LIST_EMOJI || type == OC_JOB_GET_CHANNEL_DESCRIPTION ||
           type == OC_JOB_LIST_CLIENT_SETTINGS ||
           type == OC_JOB_CALL_AUTH || type == OC_JOB_CALL_HISTORY || type == OC_JOB_CALL_DETAIL ||
           type == OC_JOB_RECORDINGS_PENDING || type == OC_JOB_REC_PEOPLE ||
           type == OC_JOB_STORAGE_STATUS || type == OC_JOB_ALERTS_LIST ||
           type == OC_JOB_AUDIT_QUERY || type == OC_JOB_USAGE || type == OC_JOB_LIST_BLOCKS ||
           type == OC_JOB_TTS_LOOKUP || type == OC_JOB_TTS_PREVIEW ||
           type == OC_JOB_EXPORT_LOOKUP || type == OC_JOB_PREVIEW_LOOKUP ||
           type == OC_JOB_MCP_CHANNELS || type == OC_JOB_MCP_READ || type == OC_JOB_MCP_SEARCH ||
           type == OC_JOB_STT_PREP || type == OC_JOB_LIST_GROUPS ||
           type == OC_JOB_SUMMARY_LOOKUP || type == OC_JOB_LIST_POLL_VOTERS;
}

static oc_dbres *process_summary_lookup(sqlite3 *rdb, const oc_job *j);

/* Dispatch a read-only job against `rdb`. */
static oc_dbres *process_read(sqlite3 *rdb, const oc_job *j) {
    if (j->type == OC_JOB_SUMMARY_LOOKUP) return process_summary_lookup(rdb, j);
    if (j->type == OC_JOB_BACKFILL)       return process_backfill(rdb, j);
    if (j->type == OC_JOB_HISTORY)        return process_history(rdb, j);
    if (j->type == OC_JOB_SEARCH)         return process_search(rdb, j);
    if (j->type == OC_JOB_LIST_CHANNELS)  return process_list_channels(rdb, j);
    if (j->type == OC_JOB_LIST_USERS)     return process_list_users(rdb, j);
    if (j->type == OC_JOB_LIST_REACTIONS) return process_list_reactions(rdb, j);
    if (j->type == OC_JOB_LIST_POLL_VOTERS) return process_list_poll_voters(rdb, j);
    if (j->type == OC_JOB_LIST_PINS)      return process_list_pins(rdb, j);
    if (j->type == OC_JOB_LIST_MEMBERS)   return process_list_members(rdb, j);
    if (j->type == OC_JOB_GET_CHANNEL_DESCRIPTION) return process_get_channel_description(rdb, j);
    if (j->type == OC_JOB_LIST_FILES)     return process_list_files(rdb, j);
    if (j->type == OC_JOB_LIST_SAVED)     return process_list_saved(rdb, j);
    if (j->type == OC_JOB_LIST_THREAD)    return process_list_thread(rdb, j);
    if (j->type == OC_JOB_TYPING)         return process_typing(rdb, j);
    if (j->type == OC_JOB_ATTACH_LOOKUP)  return process_attach_lookup(rdb, j);
    if (j->type == OC_JOB_STORAGE_STATUS) return process_storage_status(rdb, j);
    if (j->type == OC_JOB_ALERTS_LIST)    return process_alerts_list(rdb, j);
    if (j->type == OC_JOB_AUDIT_QUERY)    return process_audit_query(rdb, j);
    if (j->type == OC_JOB_USAGE)          return process_usage(rdb, j);
    if (j->type == OC_JOB_LIST_BLOCKS)    return process_list_blocks(rdb, j);
    if (j->type == OC_JOB_TTS_LOOKUP)     return process_tts_lookup(rdb, j);
    if (j->type == OC_JOB_TTS_PREVIEW)    return process_tts_preview(rdb, j);
    if (j->type == OC_JOB_EXPORT_LOOKUP)  return process_export_lookup(rdb, j);
    if (j->type == OC_JOB_PREVIEW_LOOKUP) return process_preview_lookup(rdb, j);
    if (j->type == OC_JOB_MCP_CHANNELS)  return process_mcp_channels(rdb, j);
    if (j->type == OC_JOB_MCP_READ)      return process_mcp_read(rdb, j);
    if (j->type == OC_JOB_MCP_SEARCH)    return process_mcp_search(rdb, j);
    if (j->type == OC_JOB_STT_PREP)       return process_stt_prep(rdb, j);
    if (j->type == OC_JOB_LIST_WEBHOOKS)  return process_list_webhooks(rdb, j);
    if (j->type == OC_JOB_LIST_INVITES)   return process_list_invites(rdb, j);
    if (j->type == OC_JOB_GET_PROFILE)    return process_get_profile(rdb, j);
    if (j->type == OC_JOB_LIST_FILE_CHANNELS) return process_list_file_channels(rdb, j);
    if (j->type == OC_JOB_LIST_SESSIONS)  return process_list_sessions(rdb, j);
    if (j->type == OC_JOB_LIST_EMOJI)     return process_list_emoji(rdb, j);
    if (j->type == OC_JOB_LIST_GROUPS)    return process_list_groups(rdb, j);
    if (j->type == OC_JOB_LIST_NOTIFY_PREFS) return process_list_notify_prefs(rdb, j);
    if (j->type == OC_JOB_LIST_CLIENT_SETTINGS) return process_list_client_settings(rdb, j);
    if (j->type == OC_JOB_CALL_AUTH)      return process_call_auth(rdb, j);
    if (j->type == OC_JOB_CALL_HISTORY)   return process_call_history(rdb, j);
    if (j->type == OC_JOB_CALL_DETAIL)    return process_call_detail(rdb, j);
    if (j->type == OC_JOB_RECORDINGS_PENDING) return process_recordings_pending(rdb, j);
    if (j->type == OC_JOB_REC_PEOPLE)         return process_rec_people(rdb, j);
    return NULL;
}

/* --- summaries (REQ-310, ARCH-116) ------------------------------------------------ */

/* The worker's batch: it waits on `cv` until the writer has stored it. */
struct oc_sum_batch {
    pthread_mutex_t      mu;
    pthread_cond_t       cv;
    int                  done, rc;
    const oc_sum_answer *a;       /* STORE */
    oc_sum_new          *nodes;
    int                  n;
};

static oc_dbres *sum_answer(uint64_t conn_id, uint32_t req, uint8_t status, uint64_t channel);

/* Where every request in the queue is: the one being made first (0), then the
 * rest in the order they came, each with how many are ahead of it. */
static int sum_places(sqlite3 *db, oc_dbres *r) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT conn_id, req_id FROM summary_requests ORDER BY state='running' DESC, id;",
                           -1, &st, NULL) != SQLITE_OK)
        return -1;
    size_t cap = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (r->n_sum_places == cap) {
            size_t nc = cap ? cap * 2 : 16;
            struct oc_sum_place *nv = realloc(r->sum_places, nc * sizeof *nv);
            if (!nv) break;
            r->sum_places = nv;
            cap = nc;
        }
        struct oc_sum_place *pl = &r->sum_places[r->n_sum_places];
        pl->conn_id = (uint64_t)sqlite3_column_int64(st, 0);
        pl->req_id = (uint32_t)sqlite3_column_int64(st, 1);
        pl->position = r->n_sum_places > 0xFFFF ? 0xFFFF : (uint16_t)r->n_sum_places;
        r->n_sum_places++;
    }
    sqlite3_finalize(st);
    return 0;
}

static oc_dbres *places_result(sqlite3 *db) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->type = OC_RES_SUMMARY_QUEUE;
    sum_places(db, r);
    return r;
}

/* A request with no stored answer joins the queue, unless it is full. */
static oc_dbres *process_summary_queue(sqlite3 *db, const oc_job *j) {
    sqlite3_stmt *st = NULL;
    int n = 0;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM summary_requests;", -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW)
        n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    if (n >= SUM_QUEUE_MAX) {
        oc_dbres *r = sum_answer(j->conn_id, j->sum_req, OC_SUM_UNAVAILABLE, j->channel_id);
        if (r) {
            r->type = OC_RES_SUMMARY_QUEUE;
            r->sum_start = (int64_t)j->sum_start;
            r->sum_end = (int64_t)j->sum_end;
        }
        return r;
    }
    /* The same span asked for again by the same person -- after a reconnect, or
     * from another device -- is the request already waiting: the asker watches
     * it now. A range is the same when its days are; the other spans when they
     * start the same day. */
    int joined = 0;
    if (sqlite3_prepare_v2(db,
            "UPDATE summary_requests SET conn_id=?1, req_id=?2 WHERE id=(SELECT id FROM summary_requests "
            "WHERE user_id=?3 AND channel_id=?4 AND scope=?5 AND tz_offset_min=?6 AND start_ms=?7 "
            "AND (scope<>?9 OR end_ms=?8) ORDER BY id LIMIT 1);", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->conn_id);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)j->sum_req);
        sqlite3_bind_int64(st, 3, (sqlite3_int64)j->user_id);
        sqlite3_bind_int64(st, 4, (sqlite3_int64)j->channel_id);
        sqlite3_bind_int(st, 5, j->sum_scope);
        sqlite3_bind_int(st, 6, j->sum_tz);
        sqlite3_bind_int64(st, 7, (sqlite3_int64)j->sum_start);
        sqlite3_bind_int64(st, 8, (sqlite3_int64)j->sum_end);
        sqlite3_bind_int(st, 9, OC_SUM_RANGE);
        if (sqlite3_step(st) == SQLITE_DONE) joined = sqlite3_changes(db) == 1;
    }
    sqlite3_finalize(st);
    st = NULL;
    if (!joined && sqlite3_prepare_v2(db,
            "INSERT INTO summary_requests (conn_id, req_id, user_id, channel_id, scope, start_ms, end_ms, "
            "tz_offset_min, created_at_ms) VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9);", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->conn_id);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)j->sum_req);
        sqlite3_bind_int64(st, 3, (sqlite3_int64)j->user_id);
        sqlite3_bind_int64(st, 4, (sqlite3_int64)j->channel_id);
        sqlite3_bind_int(st, 5, j->sum_scope);
        sqlite3_bind_int64(st, 6, (sqlite3_int64)j->sum_start);
        sqlite3_bind_int64(st, 7, (sqlite3_int64)j->sum_end);
        sqlite3_bind_int(st, 8, j->sum_tz);
        sqlite3_bind_int64(st, 9, (sqlite3_int64)dbw_now_ms());
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
    oc_dbres *r = places_result(db);
    if (r) { r->conn_id = j->conn_id; r->sum_req = j->sum_req; r->channel_id = j->channel_id; }
    return r;
}

static oc_dbres *process_summary_take(sqlite3 *db, const oc_job *j) {
    struct oc_sum_batch *b = j->sum_batch;
    int taken = 0;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "UPDATE summary_requests SET state='running', started_at_ms=?2 "
                               "WHERE id=?1 AND state='queued';", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->sum_row);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)dbw_now_ms());
        if (sqlite3_step(st) == SQLITE_DONE) taken = sqlite3_changes(db) == 1;
    }
    sqlite3_finalize(st);
    oc_dbres *r = places_result(db);
    pthread_mutex_lock(&b->mu);
    b->rc = taken;
    b->done = 1;
    pthread_cond_signal(&b->cv);
    pthread_mutex_unlock(&b->mu);
    return r;
}

/* A connection closed (DROP: all it watched) or its person stopped watching
 * one request (DETACH): the requests go on, watched by nobody, and are told as
 * notices when made. */
static oc_dbres *process_summary_drop(sqlite3 *db, const oc_job *j) {
    sqlite3_stmt *st = NULL;
    const char *sql = j->type == OC_JOB_SUMMARY_DETACH
        ? "UPDATE summary_requests SET conn_id=0, req_id=0 WHERE conn_id=?1 AND req_id=?2;"
        : "UPDATE summary_requests SET conn_id=0, req_id=0 WHERE conn_id=?1;";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->conn_id);
        if (j->type == OC_JOB_SUMMARY_DETACH) sqlite3_bind_int64(st, 2, (sqlite3_int64)j->sum_req);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
    oc_dbres *r = calloc(1, sizeof *r);
    if (r) r->type = OC_RES_OK;
    return r;
}

/* Its asker cancelled a request: out of the queue, and answered CANCELLED;
 * one being made is named (`sum_row`) for the worker to stop. A request
 * already answered is left as it was. */
static oc_dbres *process_summary_cancel(sqlite3 *db, const oc_job *j) {
    sqlite3_stmt *st = NULL;
    int64_t row = 0;
    int running = 0;
    if (sqlite3_prepare_v2(db, "SELECT id, state='running', channel_id FROM summary_requests "
                               "WHERE conn_id=?1 AND req_id=?2;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->conn_id);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)j->sum_req);
        if (sqlite3_step(st) == SQLITE_ROW) { row = sqlite3_column_int64(st, 0); running = sqlite3_column_int(st, 1); }
    }
    sqlite3_finalize(st);
    if (!row) {
        oc_dbres *r = calloc(1, sizeof *r);
        if (r) r->type = OC_RES_OK;
        return r;
    }
    if (sqlite3_prepare_v2(db, "DELETE FROM summary_requests WHERE id=?1;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, row);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
    oc_dbres *r = sum_answer(j->conn_id, j->sum_req, OC_SUM_CANCELLED, j->channel_id);
    if (r) {
        r->sum_body = strdup("Summary canceled.");
        r->sum_version = strdup("");
        r->sum_row = running ? row : 0;
        sum_places(db, r);
    }
    return r;
}

/* A notice opened: its summary as it was made (or why it was not), answered as
 * SUMMARY to the asking request, and the notice seen. Only its own person's. */
static oc_dbres *process_summary_open(sqlite3 *db, const oc_job *j) {
    sqlite3_stmt *st = NULL;
    oc_dbres *r = NULL;
    if (sqlite3_prepare_v2(db, "SELECT channel_id, start_ms, end_ms, tz_offset_min, status, body, reason "
                               "FROM summary_notices WHERE id=?1 AND user_id=?2;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->sum_notice);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)j->user_id);
        if (sqlite3_step(st) == SQLITE_ROW) {
            int status = sqlite3_column_int(st, 4);
            r = sum_answer(j->conn_id, j->sum_req, (uint8_t)status, (uint64_t)sqlite3_column_int64(st, 0));
            if (r) {
                r->sum_start = sqlite3_column_int64(st, 1);
                r->sum_end = sqlite3_column_int64(st, 2);
                r->sum_tz = sqlite3_column_int(st, 3);
                const char *t = (const char *)sqlite3_column_text(st, status == OC_SUM_OK ? 5 : 6);
                r->sum_body = strdup(t ? t : "");
                r->sum_version = strdup("");
                r->sum_id = j->sum_notice;
            }
        }
    }
    sqlite3_finalize(st);
    if (!r) {
        r = sum_answer(j->conn_id, j->sum_req, OC_SUM_UNAVAILABLE, 0);
        if (r) { r->sum_body = strdup("This summary is no longer kept."); r->sum_version = strdup(""); }
        return r;
    }
    if (sqlite3_prepare_v2(db, "UPDATE summary_notices SET seen_at_ms=?3 WHERE id=?1 AND user_id=?2 "
                               "AND seen_at_ms IS NULL;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->sum_notice);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)j->user_id);
        sqlite3_bind_int64(st, 3, (sqlite3_int64)dbw_now_ms());
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
    return r;
}

/* A notice seen without being opened: dismissed, or its summary delivered to
 * the person watching it. */
static oc_dbres *process_summary_seen(sqlite3 *db, const oc_job *j) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "UPDATE summary_notices SET seen_at_ms=?3 WHERE id=?1 AND user_id=?2 "
                               "AND seen_at_ms IS NULL;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->sum_notice);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)j->user_id);
        sqlite3_bind_int64(st, 3, (sqlite3_int64)dbw_now_ms());
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
    oc_dbres *r = calloc(1, sizeof *r);
    if (r) r->type = OC_RES_OK;
    return r;
}

/* Summaries did not come up: what someone is watching is answered and leaves
 * the queue; what nobody watches waits for a start that brings them up. */
static oc_dbres *process_summary_fail(sqlite3 *db) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->type = OC_RES_SUMMARY_FAILALL;
    sum_places(db, r);
    sqlite3_exec(db, "DELETE FROM summary_requests WHERE conn_id<>0;", NULL, NULL, NULL);
    return r;
}

static oc_dbres *sum_answer(uint64_t conn_id, uint32_t req, uint8_t status, uint64_t channel) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->type = OC_RES_SUMMARY;
    r->conn_id = conn_id;
    r->sum_req = req;
    r->sum_status = status;
    r->channel_id = channel;
    return r;
}

static oc_dbres *process_summary_store(sqlite3 *db, const oc_job *j) {
    struct oc_sum_batch *b = j->sum_batch;
    const oc_sum_answer *a = b->a;
    oc_dbres *r = NULL;
    int rc = b->n ? oc_sum_store(db, a->channel, a->version, b->nodes, b->n) : 0;
    /* A request's answer -- unless it was cancelled meanwhile (its row gone):
     * who watches it now, and whose it is. */
    sqlite3_stmt *st = NULL;
    uint64_t conn = 0, user = 0;
    uint32_t req = 0;
    int scope = 0, found = 0;
    /* A build that failed is answered whatever became of its pieces. */
    if (a->request_id && (rc != 1 || !a->ok) &&
        sqlite3_prepare_v2(db, "SELECT conn_id, req_id, user_id, scope FROM summary_requests WHERE id=?1;", -1, &st,
                           NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)a->request_id);
        if (sqlite3_step(st) == SQLITE_ROW) {
            conn = (uint64_t)sqlite3_column_int64(st, 0);
            req = (uint32_t)sqlite3_column_int64(st, 1);
            user = (uint64_t)sqlite3_column_int64(st, 2);
            scope = sqlite3_column_int(st, 3);
            found = 1;
        }
    }
    sqlite3_finalize(st);
    st = NULL;
    if (found) {
        /* Answered: out of the queue, and kept as a notice -- seen at once if
         * its watcher is given it, told to its person otherwise. */
        if (sqlite3_prepare_v2(db, "DELETE FROM summary_requests WHERE id=?1;", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)a->request_id);
            sqlite3_step(st);
        }
        sqlite3_finalize(st);
        st = NULL;
        int ok = a->ok && rc == 0;
        uint8_t status = ok ? OC_SUM_OK : OC_SUM_FAILED;
        const char *why = a->err ? a->err : "The summary could not be made.";
        uint64_t now = dbw_now_ms(), notice = 0;
        if (sqlite3_prepare_v2(db,
                "INSERT INTO summary_notices (user_id, channel_id, scope, start_ms, end_ms, tz_offset_min, status, "
                "body, reason, made_at_ms) VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10);", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)user);
            sqlite3_bind_int64(st, 2, (sqlite3_int64)a->channel);
            sqlite3_bind_int(st, 3, scope);
            sqlite3_bind_int64(st, 4, (sqlite3_int64)a->start_ms);
            sqlite3_bind_int64(st, 5, (sqlite3_int64)a->end_ms);
            sqlite3_bind_int(st, 6, a->tz_offset_min);
            sqlite3_bind_int(st, 7, status);
            if (ok) sqlite3_bind_text(st, 8, a->body, -1, SQLITE_TRANSIENT);
            else sqlite3_bind_text(st, 9, why, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(st, 10, (sqlite3_int64)now);
            if (sqlite3_step(st) == SQLITE_DONE) notice = (uint64_t)sqlite3_last_insert_rowid(db);
        }
        sqlite3_finalize(st);
        r = sum_answer(conn, req, status, (uint64_t)a->channel);
        if (r) {
            r->sum_start = a->start_ms;
            r->sum_end = a->end_ms;
            r->sum_tz = a->tz_offset_min;
            r->sum_id = ok ? (uint64_t)(b->n ? b->nodes[b->n - 1].id : a->summary_id) : 0;
            r->sum_body = strdup(ok ? a->body : why);
            r->sum_version = strdup(a->version ? a->version : "");
            r->sum_notice = notice;
            r->sum_user = user;
            r->sum_scope = (uint8_t)scope;
            r->sum_made_at = now;
            sum_places(db, r);
        }
    }
    if (!r) {
        r = calloc(1, sizeof *r);
        if (r) r->type = OC_RES_OK;
    }
    pthread_mutex_lock(&b->mu);
    b->rc = rc;
    b->done = 1;
    pthread_cond_signal(&b->cv);
    pthread_mutex_unlock(&b->mu);
    return r;
}

int oc_dbwriter_sum_take(oc_dbwriter *w, int64_t row) {
    struct oc_sum_batch b;
    memset(&b, 0, sizeof b);
    pthread_mutex_init(&b.mu, NULL);
    pthread_cond_init(&b.cv, NULL);
    oc_job *j = oc_job_new(OC_JOB_SUMMARY_TAKE, 0);
    if (!j) { pthread_mutex_destroy(&b.mu); pthread_cond_destroy(&b.cv); return -1; }
    j->sum_batch = &b;
    j->sum_row = row;
    oc_dbwriter_submit(w, j);
    pthread_mutex_lock(&b.mu);
    while (!b.done) pthread_cond_wait(&b.cv, &b.mu);
    pthread_mutex_unlock(&b.mu);
    pthread_mutex_destroy(&b.mu);
    pthread_cond_destroy(&b.cv);
    return b.rc;
}

int oc_dbwriter_sum_store(oc_dbwriter *w, const oc_sum_answer *a, oc_sum_new *nodes, int n) {
    struct oc_sum_batch b;
    memset(&b, 0, sizeof b);
    pthread_mutex_init(&b.mu, NULL);
    pthread_cond_init(&b.cv, NULL);
    b.a = a;
    b.nodes = nodes;
    b.n = n;
    oc_job *j = oc_job_new(OC_JOB_SUMMARY_STORE, 0);
    if (!j) { pthread_mutex_destroy(&b.mu); pthread_cond_destroy(&b.cv); return -1; }
    j->sum_batch = &b;
    oc_dbwriter_submit(w, j);
    pthread_mutex_lock(&b.mu);
    while (!b.done) pthread_cond_wait(&b.cv, &b.mu);
    pthread_mutex_unlock(&b.mu);
    pthread_mutex_destroy(&b.mu);
    pthread_cond_destroy(&b.cv);
    return b.rc;
}

/* A SUMMARIZE: who may read the channel, the span as the reader means it, and
 * the summary if one is stored (an older version's too: it is served while the
 * worker makes the new one). */
static oc_dbres *process_summary_lookup(sqlite3 *rdb, const oc_job *j) {
    if (!channel_read_access(rdb, j->channel_id, j->user_id))
        return sum_answer(j->conn_id, j->sum_req, OC_SUM_FORBIDDEN, j->channel_id);
    int tz = 0;
    int64_t now = (int64_t)dbw_now_ms();
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(rdb, "SELECT COALESCE(tz_offset_min,0) FROM users WHERE id=?1;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)j->user_id);
        if (sqlite3_step(st) == SQLITE_ROW) tz = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    const int64_t day = 86400000;
    int64_t today = oc_sum_day_start(now, tz), start = 0, end = 0;
    switch (j->sum_scope) {
    case OC_SUM_WEEK:
        start = today - 6 * day; end = today + day;
        break;
    case OC_SUM_TODAY:
        start = today; end = now + 1;
        break;
    case OC_SUM_DAILY:   /* since yesterday: yesterday, and today so far */
        start = today - day; end = now + 1;
        break;
    case OC_SUM_RANGE:
        start = (int64_t)j->sum_start; end = (int64_t)j->sum_end;
        if (end <= start)
            return sum_answer(j->conn_id, j->sum_req, OC_SUM_FAILED, j->channel_id);
        break;
    default: {   /* OC_SUM_UNREAD: from the reader's place to now */
        start = 0;
        if (sqlite3_prepare_v2(rdb,
                "SELECT m.created_at_ms FROM delivery_cursors c JOIN messages m ON m.id=c.message_id "
                "WHERE c.user_id=?1 AND c.channel_id=?2;", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)j->user_id);
            sqlite3_bind_int64(st, 2, (sqlite3_int64)j->channel_id);
            if (sqlite3_step(st) == SQLITE_ROW) {
                int64_t c = sqlite3_column_int64(st, 0) + 1;
                if (c > start) start = c;
            }
        }
        sqlite3_finalize(st);
        end = now + 1;
        break;
    }
    }
    int64_t id = 0;
    char *body = NULL, version[160] = "";
    /* One made by an earlier version is answered while its rebuild waits, when
     * it has the shape clients read. */
    int found = oc_sum_find_period(rdb, (int64_t)j->channel_id, start, end, tz, j->sum_version ? j->sum_version : "",
                                   1, &id, &body, version, sizeof version);
    if (found && !oc_sum_body_readable(body)) { free(body); body = NULL; found = 0; }
    if (found) {
        oc_dbres *r = sum_answer(j->conn_id, j->sum_req, OC_SUM_OK, j->channel_id);
        if (r) {
            r->sum_id = (uint64_t)id;
            r->sum_start = start; r->sum_end = end; r->sum_tz = tz;
            r->sum_body = oc_sum_client_body(rdb, body, (int64_t)j->channel_id, start, end);
            r->sum_version = strdup(version);
            if (!r->sum_body) r->sum_status = OC_SUM_FAILED;
        }
        free(body);
        return r;
    }
    oc_dbres *r = sum_answer(j->conn_id, j->sum_req, OC_SUM_OK, j->channel_id);
    if (r) {
        r->type = OC_RES_SUMMARY_NEED; r->sum_start = start; r->sum_end = end; r->sum_tz = tz;
        r->sum_scope = j->sum_scope;
    }
    return r;
}

/* Dispatch a write (or auth) job against the single write connection. */
static oc_dbres *process_storage_maint(sqlite3 *db, const oc_job *j);

static oc_dbres *process_write(oc_dbwriter *w, const oc_job *j) {
    if (j->type == OC_JOB_AUTH)          return process_auth(w, j);
    if (j->type == OC_JOB_AUTH_STEP)     return process_auth_step(w, j);
    if (j->type == OC_JOB_SECURITY)      return process_security(w, j);
    if (j->type == OC_JOB_RESET_CREDENTIAL) return process_reset_credential(w->db, j);
    if (j->type == OC_JOB_RESET_REDEEM)  return process_reset_redeem(w, j);
    if (j->type == OC_JOB_ENABLE_USER)   return process_enable_user(w, j);
    if (j->type == OC_JOB_REVOKE_SESSION) return process_revoke_session(w->db, j);
    if (j->type == OC_JOB_SESSIONS_SEEN) return process_sessions_seen(w->db, j);
    if (j->type == OC_JOB_SEND)          return process_send(w->db, j);
    if (j->type == OC_JOB_REGISTER)      return process_register(w, j);
    if (j->type == OC_JOB_SET_ROLE)      return process_set_role(w->db, j);
    if (j->type == OC_JOB_LOGOUT)        return process_logout(w->db, j);
    if (j->type == OC_JOB_EDIT)          return process_edit(w->db, j);
    if (j->type == OC_JOB_UNFURL_STORE)  return process_unfurl_store(w->db, j);
    if (j->type == OC_JOB_SUMMARY_STORE) return process_summary_store(w->db, j);
    if (j->type == OC_JOB_SUMMARY_QUEUE) return process_summary_queue(w->db, j);
    if (j->type == OC_JOB_SUMMARY_TAKE)  return process_summary_take(w->db, j);
    if (j->type == OC_JOB_SUMMARY_DROP)  return process_summary_drop(w->db, j);
    if (j->type == OC_JOB_SUMMARY_FAIL)  return process_summary_fail(w->db);
    if (j->type == OC_JOB_SUMMARY_DETACH) return process_summary_drop(w->db, j);
    if (j->type == OC_JOB_SUMMARY_CANCEL) return process_summary_cancel(w->db, j);
    if (j->type == OC_JOB_SUMMARY_OPEN)  return process_summary_open(w->db, j);
    if (j->type == OC_JOB_SUMMARY_SEEN)  return process_summary_seen(w->db, j);
    if (j->type == OC_JOB_TTS_STORE)     return process_tts_store(w->db, j);
    if (j->type == OC_JOB_TTS_TOUCH)     return process_tts_touch(w->db, j);
    if (j->type == OC_JOB_TTS_USE)       return process_tts_use(w->db, j);
    if (j->type == OC_JOB_EXPORT_STORE)  return process_export_store(w->db, j);
    if (j->type == OC_JOB_EXPORT_TOUCH)  return process_export_touch(w->db, j);
    if (j->type == OC_JOB_PREVIEW_STORE) return process_preview_store(w->db, j);
    if (j->type == OC_JOB_PREVIEW_TOUCH) return process_preview_touch(w->db, j);
    if (j->type == OC_JOB_TTS_VOICE_SET) return process_tts_voice_set(w->db, j);
    if (j->type == OC_JOB_DELETE)        return process_delete(w, j);
    if (j->type == OC_JOB_RESTORE)       return process_restore(w, j);
    if (j->type == OC_JOB_CREATE_CHANNEL) return process_create_channel(w->db, j);
    if (j->type == OC_JOB_JOIN_CHANNEL)   return process_join_channel(w->db, j);
    if (j->type == OC_JOB_LEAVE_CHANNEL)  return process_leave_channel(w->db, j);
    if (j->type == OC_JOB_INVITE_CHANNEL) return process_invite_channel(w->db, j);
    if (j->type == OC_JOB_AGENT_CREATE)   return process_agent_create(w->db, j);
    if (j->type == OC_JOB_AGENT_REMOVE)   return process_agent_remove(w->db, j);
    if (j->type == OC_JOB_TOKEN_MINT)     return process_token_mint(w->db, j);
    if (j->type == OC_JOB_TOKEN_LIST)     return process_token_list(w->db, j);
    if (j->type == OC_JOB_TOKEN_REVOKE)   return process_token_revoke(w->db, j);
    if (j->type == OC_JOB_TOKEN_LOOKUP)   return process_token_lookup(w->db, j);
    if (j->type == OC_JOB_MCP_AUDIT)      return process_mcp_audit(w->db, j);
    if (j->type == OC_JOB_OAUTH_REGISTER)  return process_oauth_register(w->db, j);
    if (j->type == OC_JOB_OAUTH_DOC_STORE) return process_oauth_doc_store(w->db, j);
    if (j->type == OC_JOB_OAUTH_BEGIN)     return process_oauth_begin(w->db, j);
    if (j->type == OC_JOB_OAUTH_POLL)      return process_oauth_poll(w->db, j);
    if (j->type == OC_JOB_OAUTH_CODE_FIND) return process_oauth_code_find(w->db, j);
    if (j->type == OC_JOB_OAUTH_DECIDE)    return process_oauth_decide(w->db, j);
    if (j->type == OC_JOB_OAUTH_EXCHANGE)  return process_oauth_exchange(w->db, j);
    if (j->type == OC_JOB_OAUTH_REFRESH)   return process_oauth_refresh(w->db, j);
    if (j->type == OC_JOB_OAUTH_LOOKUP)    return process_oauth_lookup(w->db, j);
    if (j->type == OC_JOB_OAUTH_LIST)      return process_oauth_list(w->db, j);
    if (j->type == OC_JOB_OAUTH_REVOKE)    return process_oauth_revoke(w->db, j);
    if (j->type == OC_JOB_OUTWH_CREATE)   return process_owh_create(w, j);
    if (j->type == OC_JOB_OUTWH_LIST)     return process_owh_list(w->db, j);
    if (j->type == OC_JOB_OUTWH_SET)      return process_owh_set(w->db, j);
    if (j->type == OC_JOB_OUTWH_DELETE)   return process_owh_delete(w->db, j);
    if (j->type == OC_JOB_OUTWH_TAKE)     return process_owh_take(w->db, j);
    if (j->type == OC_JOB_OUTWH_DONE)     return process_owh_done(w->db, j);
    if (j->type == OC_JOB_REMOVE_CHANNEL) return process_remove_channel(w->db, j);
    if (j->type == OC_JOB_OPEN_DM)        return process_open_dm(w->db, j);
    if (j->type == OC_JOB_E2E_CLOSE)      return process_e2e_close(w->db, j);
    if (j->type == OC_JOB_OPEN_GROUP_DM) return process_open_group_dm(w->db, j);
    if (j->type == OC_JOB_ADD_EMOJI)      return process_add_emoji(w->db, j);
    if (j->type == OC_JOB_DELETE_EMOJI)   return process_delete_emoji(w->db, j);
    if (j->type == OC_JOB_RENAME_EMOJI)   return process_rename_emoji(w->db, j);
    if (j->type == OC_JOB_CREATE_GROUP)   return process_create_group(w->db, j);
    if (j->type == OC_JOB_UPDATE_GROUP)   return process_update_group(w->db, j);
    if (j->type == OC_JOB_DELETE_GROUP)   return process_delete_group(w->db, j);
    if (j->type == OC_JOB_GROUP_ADD_MEMBERS)    return process_group_members(w->db, j, 1);
    if (j->type == OC_JOB_GROUP_REMOVE_MEMBERS) return process_group_members(w->db, j, 0);
    if (j->type == OC_JOB_CHANNEL_ADD_GROUP)    return process_channel_group(w->db, j, 1);
    if (j->type == OC_JOB_CHANNEL_REMOVE_GROUP) return process_channel_group(w->db, j, 0);
    if (j->type == OC_JOB_INVITE_USER)    return process_invite_user(w, j);
    if (j->type == OC_JOB_FIRE_SCHEDULED)    return process_fire_scheduled(w->db, j);
    if (j->type == OC_JOB_FIRE_REMINDERS)    return process_fire_reminders(w->db, j);
    if (j->type == OC_JOB_SCHEDULE)          return process_schedule(w->db, j);
    if (j->type == OC_JOB_LIST_SCHEDULED)    return process_list_scheduled(w->db, j);
    if (j->type == OC_JOB_CANCEL_SCHEDULED)  return process_cancel_scheduled(w->db, j);
    if (j->type == OC_JOB_UPDATE_SCHEDULED)  return process_update_scheduled(w->db, j);
    if (j->type == OC_JOB_SET_DRAFT)      return process_set_draft(w->db, j);
    if (j->type == OC_JOB_LIST_DRAFTS)    return process_list_drafts(w->db, j);
    if (j->type == OC_JOB_REMOVE_USER)    return process_remove_user(w->db, j);
    if (j->type == OC_JOB_REDEEM)         return process_redeem(w, j);
    if (j->type == OC_JOB_REACT)          return process_react(w->db, j);
    if (j->type == OC_JOB_VOTE)           return process_vote(w->db, j);
    if (j->type == OC_JOB_EDIT_POLL)      return process_edit_poll(w->db, j);
    if (j->type == OC_JOB_PIN)            return process_pin(w->db, j);
    if (j->type == OC_JOB_UPDATE_CHANNEL) return process_update_channel(w->db, j);
    if (j->type == OC_JOB_CHANNEL_ADD_MANAGER)    return process_channel_manager(w->db, j, 1);
    if (j->type == OC_JOB_CHANNEL_REMOVE_MANAGER) return process_channel_manager(w->db, j, 0);
    if (j->type == OC_JOB_SAVE_ITEM)      return process_save_item(w->db, j);
    if (j->type == OC_JOB_SET_COLLAPSED)  return process_set_collapsed(w->db, j);
    if (j->type == OC_JOB_FIND_AT_TIME)   return process_find_at_time(w->db, j);
    if (j->type == OC_JOB_MOVE_MESSAGE)   return process_move_message(w->db, j);
    if (j->type == OC_JOB_TODO_ADD || j->type == OC_JOB_TODO_MARK || j->type == OC_JOB_TODO_RENAME)
        return process_todo(w->db, j);
    /* On the WRITER because it stamps the seen watermark; the read half would
     * otherwise be a reader job with a write in it. */
    if (j->type == OC_JOB_LIST_ACTIVITY)  return process_list_activity(w->db, j);
    if (j->type == OC_JOB_SEND_REPLY)     return process_send_reply(w->db, j);
    if (j->type == OC_JOB_SETUP_INVITE)   return process_setup_invite(w->db, j);
    if (j->type == OC_JOB_CLIENT_ACK)     return process_client_ack(w->db, j);
    if (j->type == OC_JOB_MARK_ALL_READ)  return process_mark_all_read(w->db, j);
    if (j->type == OC_JOB_LOAD_IDENTITY)  return process_load_identity(w->db, j);
    if (j->type == OC_JOB_STORE_IDENTITY) return process_store_identity(w->db, j);
    if (j->type == OC_JOB_LOAD_TLS_STATE) return process_load_tls_state(w->db, j);
    if (j->type == OC_JOB_STORE_ACME_ACCOUNT || j->type == OC_JOB_STORE_TLS_CERT) return process_store_tls(w->db, j);
    if (j->type == OC_JOB_ALERT_RAISE || j->type == OC_JOB_ALERT_CLEAR) return process_alert(w->db, j);
    if (j->type == OC_JOB_ALERT_ACK) return process_alert_ack(w->db, j);
    if (j->type == OC_JOB_LOAD_ENROLLMENT)  return process_load_enrollment(w->db, j);
    if (j->type == OC_JOB_STORE_ENROLLMENT) return process_store_enrollment(w->db, j);
    if (j->type == OC_JOB_WELCOME_GENERAL)  return process_welcome_general(w->db, j);
    if (j->type == OC_JOB_SEED_DEMO)        return process_seed_demo(w->db, j);
    if (j->type == OC_JOB_ATTACH_CREATE)   return process_attach_create(w->db, j);
    if (j->type == OC_JOB_ATTACH_FINALIZE) return process_attach_finalize(w->db, j);
    if (j->type == OC_JOB_ATTACH_MEDIA_SET) return process_attach_media_set(w->db, j);
    if (j->type == OC_JOB_CREATE_WEBHOOK)  return process_create_webhook(w->db, j);
    if (j->type == OC_JOB_REVOKE_INVITE)     return process_revoke_invite(w->db, j);
    if (j->type == OC_JOB_SET_WEBHOOK_STATE) return process_set_webhook_state(w->db, j);
    if (j->type == OC_JOB_ROTATE_WEBHOOK)    return process_rotate_webhook(w->db, j);
    if (j->type == OC_JOB_WEBHOOK_POST)    return process_webhook_post(w->db, j);
    if (j->type == OC_JOB_CALL_START)      return process_call_start(w->db, j);
    if (j->type == OC_JOB_CALL_LOG)        return process_call_log(w->db, j);
    if (j->type == OC_JOB_CALL_DELETE)     return process_call_delete(w->db, j);
    if (j->type == OC_JOB_CALL_TRANSCRIPT) return process_call_transcript(w->db, j);
    if (j->type == OC_JOB_RECORDING_START) return process_recording_start(w->db, j);
    if (j->type == OC_JOB_RECORDING_STOP)  return process_recording_stop(w->db, j);
    if (j->type == OC_JOB_RECORDING_DONE)  return process_recording_done(w->db, j);
    if (j->type == OC_JOB_DELETE_WEBHOOK)  return process_delete_webhook(w->db, j);
    if (j->type == OC_JOB_SET_NOTIFY_PREF) return process_set_notify_pref(w->db, j);
    if (j->type == OC_JOB_SET_CHANNEL_THREAD_FOLLOW) return process_set_channel_thread_follow(w->db, j);
    if (j->type == OC_JOB_SET_MUTE)        return process_set_mute(w->db, j);
    if (j->type == OC_JOB_SET_NOTIFY_DEFAULT) return process_set_notify_default(w->db, j);
    if (j->type == OC_JOB_SET_STATUS)      return process_set_status(w->db, j);
    if (j->type == OC_JOB_SET_PROFILE)     return process_set_profile(w->db, j);
    if (j->type == OC_JOB_SET_PROFILE_FIELD)       return process_set_profile_field(w->db, j);
    if (j->type == OC_JOB_SET_PROFILE_FIELD_VALUE) return process_set_profile_field_value(w->db, j);
    if (j->type == OC_JOB_SET_OUT_OF_OFFICE) return process_set_out_of_office(w->db, j);
    if (j->type == OC_JOB_SET_EMAIL)       return process_set_email(w->db, j);
    if (j->type == OC_JOB_SET_AVATAR)      return process_set_avatar(w->db, j);
    if (j->type == OC_JOB_DEACTIVATE_SELF) return process_deactivate_self(w->db, j);
    if (j->type == OC_JOB_SET_READ_CURSOR) return process_set_read_cursor(w->db, j);
    if (j->type == OC_JOB_LIST_THREADS)    return process_list_threads(w->db, j);
    if (j->type == OC_JOB_SET_THREAD_FOLLOW) return process_set_thread_follow(w->db, j);
    if (j->type == OC_JOB_MARK_THREAD_READ)  return process_mark_thread_read(w->db, j);
    if (j->type == OC_JOB_SET_SCHEDULE)    return process_set_schedule(w->db, j);
    if (j->type == OC_JOB_SET_KEYWORDS)    return process_set_keywords(w->db, j);
    if (j->type == OC_JOB_SET_PRIORITY)    return process_set_priority(w->db, j);
    if (j->type == OC_JOB_SET_USER_MUTE)   return process_set_user_mute(w->db, j);
    if (j->type == OC_JOB_SET_BLOCK)       return process_set_block(w->db, j);
    if (j->type == OC_JOB_SET_DM_PRIVACY)  return process_set_dm_privacy(w->db, j);
    if (j->type == OC_JOB_SET_SNOOZE)      return process_set_snooze(w->db, j);
    if (j->type == OC_JOB_SET_TZ_OFFSET)   return process_set_tz_offset(w->db, j);
    if (j->type == OC_JOB_READ_RECEIPTS)   return process_read_receipts(w->db, j);
    if (j->type == OC_JOB_REGISTER_DEVICE_TOKEN)   return process_register_device_token(w->db, j);
    if (j->type == OC_JOB_UNREGISTER_DEVICE_TOKEN) return process_unregister_device_token(w->db, j);
    if (j->type == OC_JOB_PRUNE_DEVICE_TOKEN)      return process_prune_device_token(w->db, j);
    if (j->type == OC_JOB_SET_CLIENT_SETTING) return process_set_client_setting(w->db, j);
    if (j->type == OC_JOB_SET_DISPLAY_NAME)   return process_set_display_name(w->db, j);
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



/* Days since the epoch from a civil date, and back (H. Hinnant's algorithms),
 * for the ISO weeks of the usage figures. */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}
static int64_t year_of_day(int64_t z) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    return (int64_t)yoe + era * 400 + (m <= 2);
}

/* The ISO week a day falls in, as yyyyww, and the Monday it starts on: the week
 * belongs to the year its Thursday is in (ISO 8601). Day 0, 1970-01-01, was a
 * Thursday. */
uint32_t oc_usage_iso_week(uint32_t day, uint32_t *monday) {
    int64_t d = (int64_t)day;
    int64_t wd = ((d + 3) % 7 + 7) % 7;              /* Monday 0 .. Sunday 6 */
    int64_t thu = d - wd + 3;
    int64_t y = year_of_day(thu);
    int64_t week = (thu - days_from_civil(y, 1, 1)) / 7 + 1;
    if (monday) *monday = (uint32_t)(d - wd);
    return (uint32_t)(y * 100 + week);
}

/* The workspace's usage for an owner or admin (REQ-043): active members per day
 * and per ISO week, messages posted per day (the workspace's own messages and
 * the call events the daemon writes not counted), and the ten busiest channels -- all counted, nothing anyone wrote
 * read. A private channel's name goes only to someone who is in it. Authorized
 * on the CURRENT role, like the storage report. */
static oc_dbres *process_usage(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->type = OC_RES_USAGE;
    r->conn_id = j->conn_id;
    uint8_t role = OC_ROLE_MEMBER;
    if (!user_role(db, j->user_id, &role) || !oc_role_can_manage_members(role)) {
        r->type = OC_RES_USAGE_ERR;
        r->err_code = OC_ERR_FORBIDDEN;
        return r;
    }
    oc_usage_report *u = calloc(1, sizeof *u);
    if (!u) { r->type = OC_RES_USAGE_ERR; r->err_code = OC_ERR_INTERNAL; return r; }
    r->usage = u;
    uint16_t days = j->usage_days;
    if (days < 1) days = 1;
    if (days > OC_USAGE_MAX_DAYS) days = OC_USAGE_MAX_DAYS;
    const uint32_t today = (uint32_t)(dbw_now_ms() / 86400000ull);
    const uint32_t first = today - (uint32_t)days + 1;
    u->days = days;
    u->n_days = days;
    for (uint16_t i = 0; i < days; i++) u->day[i].day = first + i;

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT day, COUNT(*) FROM user_active_days WHERE day BETWEEN ?1 AND ?2 GROUP BY day;",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, first);
        sqlite3_bind_int64(st, 2, today);
        while (sqlite3_step(st) == SQLITE_ROW) {
            int64_t d = sqlite3_column_int64(st, 0);
            if (d >= first && d <= today) u->day[d - first].active = (uint32_t)sqlite3_column_int64(st, 1);
        }
        sqlite3_finalize(st);
    }
    st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT m.created_at_ms / 86400000, COUNT(*) FROM messages m JOIN users u ON u.id = m.author_id"
            " WHERE m.created_at_ms >= ?1 AND m.created_at_ms < ?2 AND u.is_system = 0 AND m.kind = 0"
            " GROUP BY m.created_at_ms / 86400000;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)first * 86400000);
        sqlite3_bind_int64(st, 2, ((sqlite3_int64)today + 1) * 86400000);
        while (sqlite3_step(st) == SQLITE_ROW) {
            int64_t d = sqlite3_column_int64(st, 0);
            if (d >= first && d <= today) u->day[d - first].messages = (uint32_t)sqlite3_column_int64(st, 1);
        }
        sqlite3_finalize(st);
    }
    /* Each ISO week the period touches, its members counted once, over the days
     * of it inside the period. */
    st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(DISTINCT user_id) FROM user_active_days WHERE day BETWEEN ?1 AND ?2;",
                           -1, &st, NULL) == SQLITE_OK) {
        uint32_t d = first;
        while (d <= today && u->n_weeks < OC_USAGE_MAX_WEEKS) {
            uint32_t monday = 0, wk = oc_usage_iso_week(d, &monday);
            uint32_t end = monday + 6 < today ? monday + 6 : today;
            sqlite3_bind_int64(st, 1, d);
            sqlite3_bind_int64(st, 2, end);
            uint32_t n = sqlite3_step(st) == SQLITE_ROW ? (uint32_t)sqlite3_column_int64(st, 0) : 0;
            sqlite3_reset(st);
            u->week[u->n_weeks++] = (oc_usage_week){ wk, n };
            d = end + 1;
        }
        sqlite3_finalize(st);
    }
    /* The busiest channels: channels only, never a DM, by messages in the period. */
    st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT c.id, COALESCE(c.name, ''), c.is_public, COUNT(*) AS n,"
            "  EXISTS (SELECT 1 FROM channel_members cm WHERE cm.channel_id = c.id AND cm.user_id = ?3)"
            " FROM messages m JOIN channels c ON c.id = m.channel_id JOIN users u ON u.id = m.author_id"
            " WHERE c.kind = 'channel' AND m.created_at_ms >= ?1 AND m.created_at_ms < ?2 AND u.is_system = 0"
            "   AND m.kind = 0"
            " GROUP BY c.id ORDER BY n DESC, c.id LIMIT ?4;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)first * 86400000);
        sqlite3_bind_int64(st, 2, ((sqlite3_int64)today + 1) * 86400000);
        sqlite3_bind_int64(st, 3, (sqlite3_int64)j->user_id);
        sqlite3_bind_int(st, 4, OC_USAGE_TOP_CHANNELS);
        while (sqlite3_step(st) == SQLITE_ROW && u->n_channels < OC_USAGE_TOP_CHANNELS) {
            uint16_t k = u->n_channels++;
            int is_public = sqlite3_column_int(st, 2), member = sqlite3_column_int(st, 4);
            u->ch_id[k] = (uint64_t)sqlite3_column_int64(st, 0);
            u->ch_messages[k] = (uint32_t)sqlite3_column_int64(st, 3);
            u->ch_private[k] = (uint8_t)!is_public;
            if (is_public || member)
                snprintf(u->ch_name[k], sizeof u->ch_name[k], "%s", (const char *)sqlite3_column_text(st, 1));
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
            " outcome, detail, channel_id FROM audit_log "
            "WHERE (?1 = 0 OR at_ms < ?1) AND (?3 = 0 OR family = ?3) AND (?4 = 0 OR actor_id = ?4)"
            "  AND (?5 = '' OR action = ?5) "
            "ORDER BY at_ms DESC, id DESC LIMIT ?2;",
            -1, &st, NULL) != SQLITE_OK) {
        r->type = OC_RES_AUDIT_ERR; r->err_code = OC_ERR_INTERNAL; return r;
    }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->audit_before_ms);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)lim);
    /* The filters (PROTOCOL.md AUDIT_QUERY): applied here, so a page holds
     * only matching rows however sparse they are in recent history. */
    sqlite3_bind_int(st, 3, j->audit_family);
    sqlite3_bind_int64(st, 4, (sqlite3_int64)j->audit_actor);
    sqlite3_bind_text(st, 5, j->body ? (const char *)j->body : "", j->body ? (int)j->body_len : 0, SQLITE_TRANSIENT);
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
        a->channel_id = (uint64_t)sqlite3_column_int64(st, 9);   /* NULL reads 0 */
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
    uint64_t now = dbw_now_ms();
    /* The MCP access family has its own age, whatever the others' (REQ-325). */
    {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(db, "DELETE FROM audit_log WHERE family=?1 AND at_ms < ?2;", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int(st, 1, OC_AUDIT_MCP);
            sqlite3_bind_int64(st, 2, (sqlite3_int64)(now > OC_AUDIT_MCP_KEEP_MS ? now - OC_AUDIT_MCP_KEEP_MS : 0));
            sqlite3_step(st);
        }
        sqlite3_finalize(st);
    }
    if (!max_age_ms) return;
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
    /* A snippet whose bytes go is no longer anything to find (REQ-226). */
    if (sqlite3_prepare_v2(db, "DELETE FROM snippets_fts WHERE rowid=?1;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)aid);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    r->reclaim[r->n_reclaim].storage_key = dup;
    r->reclaim[r->n_reclaim].attachment_id = aid;
    r->n_reclaim++;
    return 1;
}

/* A video just reclaimed takes its poster with it, for the same reason and in
 * the same pass (REQ-164). A full batch leaves the poster for the orphan tier
 * of the next pass, which collects it because its video is no longer live. */
static void reclaim_poster_of(sqlite3 *db, oc_dbres *r, size_t cap, uint64_t video_id,
                              uint64_t now, int reason) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT a.id, a.storage_key FROM attachment_media m "
            "  JOIN attachments a ON a.id = m.poster_id "
            " WHERE m.attachment_id=?1 AND a.reclaimed_at_ms = 0;", -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)video_id);
    if (sqlite3_step(st) == SQLITE_ROW)
        reclaim_add(db, r, cap, (uint64_t)sqlite3_column_int64(st, 0),
                    (const char *)sqlite3_column_text(st, 1), now, reason);
    sqlite3_finalize(st);
}

/* Run one maintenance pass. Three tiers in order, stopping at the batch cap so a
 * badly over-limit box recovers across several passes instead of stalling the
 * daemon in one long sweep (REQ-212/218). */
/* Calls' recordings kept their days (OPENCHIME_CALL_RECORDING_DAYS, CALLS.md
 * §8): one whose call ended longer ago goes -- its bytes as any file reclaimed,
 * the message that carried it showing it gone, and its row. Sixteen a pass.
 * (The space cap is kept at the start: at it, no recording starts.) */
static void prune_recordings(sqlite3 *db, oc_dbres *r, size_t cap, uint64_t now, uint32_t days) {
    uint64_t keep = (uint64_t)days * 24ull * 60 * 60 * 1000;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT r.id, a.id, a.storage_key FROM call_recordings r"
            "  JOIN calls c ON c.id = r.call_id JOIN attachments a ON a.id = r.attachment_id"
            " WHERE r.state = 'ready' AND c.ended_at_ms IS NOT NULL AND c.ended_at_ms < ?1 ORDER BY r.id LIMIT 16;",
            -1, &st, NULL) != SQLITE_OK) { sqlite3_finalize(st); return; }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)(now > keep ? now - keep : 0));
    uint64_t gone[16]; int ng = 0;
    while (ng < 16 && r->n_reclaim < cap && sqlite3_step(st) == SQLITE_ROW) {
        reclaim_add(db, r, cap, (uint64_t)sqlite3_column_int64(st, 1), (const char *)sqlite3_column_text(st, 2), now,
                    OC_RECLAIM_EXPIRED);
        gone[ng++] = (uint64_t)sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    for (int i = 0; i < ng; i++) {
        sqlite3_int64 v[1] = { (sqlite3_int64)gone[i] };
        call_exec(db, "DELETE FROM call_recordings WHERE id=?1;", 1, v, 0, NULL);
    }
}

/* Message retention (REQ-250). Run a statement over the set in temp.aged_ids. */
static void aged_exec(sqlite3 *db, const char *sql) {
    sqlite3_exec(db, sql, NULL, NULL, NULL);
}

/* Every table that names a message by `message_id` -- found from the schema, as
 * purge_channel does, so a table added later is not missed -- loses the rows
 * naming the messages in temp.aged_ids. Attachments are not among them: they
 * are detached, not deleted, so the orphan sweep reclaims their bytes. */
static void aged_delete_named(sqlite3 *db) {
    sqlite3_stmt *t = NULL;
    char names[64][64]; int n = 0;
    if (sqlite3_prepare_v2(db,
            "SELECT m.name FROM sqlite_master m JOIN pragma_table_info(m.name) p "
            " WHERE m.type='table' AND m.name NOT IN ('messages','attachments') "
            "   AND m.name NOT LIKE 'messages_fts%' AND m.name NOT LIKE 'sqlite_%' "
            "   AND p.name = 'message_id';", -1, &t, NULL) == SQLITE_OK) {
        while (sqlite3_step(t) == SQLITE_ROW && n < 64)
            snprintf(names[n++], sizeof names[0], "%s", (const char *)sqlite3_column_text(t, 0));
    }
    sqlite3_finalize(t);
    for (int i = 0; i < n; i++) {
        char sql[192];
        snprintf(sql, sizeof sql, "DELETE FROM \"%.63s\" WHERE message_id IN (SELECT id FROM temp.aged_ids);", names[i]);
        aged_exec(db, sql);
    }
}

/* Remove by age every message older than `keep_ms`, a thread only whole: a
 * top-level message goes with all its replies once it and its newest reply are
 * both past the age, and not before. At most `batch` threads a pass, so a long
 * backlog clears over several. With each message go its search entries (the
 * delete trigger), every row naming it, every summary built on it and every
 * read-aloud rendering it was spoken under; its attachments are detached for
 * the orphan sweep. A forward of it elsewhere keeps its excerpt: that belongs
 * to the forward, which ages out by its own time. */
static void purge_aged(sqlite3 *db, oc_dbres *r, size_t cap, uint64_t now, uint64_t keep_ms, uint32_t batch) {
    if (!keep_ms) return;
    const uint64_t cutoff = now > keep_ms ? now - keep_ms : 0;
    uint64_t roots[256]; int nr = 0;
    if (batch == 0 || batch > 256) batch = 256;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT m.id FROM messages m WHERE m.parent_id IS NULL AND m.created_at_ms < ?1"
            "   AND NOT EXISTS (SELECT 1 FROM messages x WHERE x.parent_id = m.id AND x.created_at_ms >= ?1)"
            " ORDER BY m.id LIMIT ?2;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)cutoff);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)batch);
        while (sqlite3_step(st) == SQLITE_ROW && nr < 256) roots[nr++] = (uint64_t)sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    if (!nr) return;

    aged_exec(db, "CREATE TEMP TABLE IF NOT EXISTS aged_ids(id INTEGER PRIMARY KEY);");
    aged_exec(db, "DELETE FROM temp.aged_ids;");
    /* A thread whose renderings would not fit what is left of this pass's
     * reclaim list waits for the next pass, rather than leaving bytes behind. */
    for (int i = 0; i < nr; i++) {
        int64_t renders = 0;
        if (sqlite3_prepare_v2(db,
                "SELECT COUNT(*) FROM rendered_audio a WHERE a.handle IN"
                " (SELECT u.handle FROM rendered_audio_uses u JOIN messages m ON m.id = u.message_id"
                "   WHERE m.id = ?1 OR m.parent_id = ?1);", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)roots[i]);
            if (sqlite3_step(st) == SQLITE_ROW) renders = sqlite3_column_int64(st, 0);
        }
        sqlite3_finalize(st);
        if (r->n_reclaim + (size_t)renders > cap) break;
        if (sqlite3_prepare_v2(db,
                "INSERT OR IGNORE INTO temp.aged_ids(id) SELECT id FROM messages WHERE id = ?1 OR parent_id = ?1;",
                -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)roots[i]);
            sqlite3_step(st);
        }
        sqlite3_finalize(st);
        /* Its renderings: the rows go now and their bytes with this pass. */
        if (sqlite3_prepare_v2(db,
                "DELETE FROM rendered_audio WHERE handle IN"
                " (SELECT u.handle FROM rendered_audio_uses u JOIN messages m ON m.id = u.message_id"
                "   WHERE m.id = ?1 OR m.parent_id = ?1) RETURNING blob_key;", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)roots[i]);
            while (sqlite3_step(st) == SQLITE_ROW && r->n_reclaim < cap) {
                const char *key = (const char *)sqlite3_column_text(st, 0);
                char *dup = key ? strdup(key) : NULL;
                if (!dup) continue;
                r->reclaim[r->n_reclaim].storage_key = dup;
                r->reclaim[r->n_reclaim].attachment_id = 0;
                r->n_reclaim++;
                r->maint_renders++;
            }
        }
        sqlite3_finalize(st);
    }

    /* Summaries first, while the messages they were built on can still be read. */
    if (sqlite3_prepare_v2(db, "SELECT id FROM temp.aged_ids ORDER BY id;", -1, &st, NULL) == SQLITE_OK)
        while (sqlite3_step(st) == SQLITE_ROW) sum_purge_msg(db, (uint64_t)sqlite3_column_int64(st, 0));
    sqlite3_finalize(st);

    aged_exec(db, "DELETE FROM snippets_fts WHERE rowid IN"
                  " (SELECT id FROM attachments WHERE message_id IN (SELECT id FROM temp.aged_ids));");
    aged_exec(db, "UPDATE attachments SET message_id = NULL WHERE message_id IN (SELECT id FROM temp.aged_ids);");
    aged_delete_named(db);
    aged_exec(db, "DELETE FROM thread_follows WHERE root_id IN (SELECT id FROM temp.aged_ids);");
    aged_exec(db, "DELETE FROM thread_reads WHERE root_id IN (SELECT id FROM temp.aged_ids);");
    /* Replies before their roots: a reply names its root. */
    aged_exec(db, "DELETE FROM messages WHERE parent_id IS NOT NULL AND id IN (SELECT id FROM temp.aged_ids);");
    aged_exec(db, "DELETE FROM messages WHERE id IN (SELECT id FROM temp.aged_ids);");
    aged_exec(db, "DELETE FROM temp.aged_ids;");
}

/* A change of retention is audited (REQ-250, REQ-251): it is the operator's
 * setting, read at start, so at start the writer compares it with the value the
 * newest `retention.change` entry recorded (none recorded: off) and, when they
 * differ, records one administrative entry, by no user, old and new in
 * `detail`. An unchanged value records nothing. */
static void retention_audit(sqlite3 *db, int days) {
    int was = 0;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT detail FROM audit_log WHERE action = 'retention.change' ORDER BY id DESC LIMIT 1;",
                           -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) {
        const char *d = (const char *)sqlite3_column_text(st, 0);
        const char *n = d ? strstr(d, "new=") : NULL;
        if (n) was = atoi(n + 4);
    }
    sqlite3_finalize(st);
    if (was == days) return;
    char detail[64];
    snprintf(detail, sizeof detail, "old=%d new=%d", was, days);
    audit_log(db, OC_AUDIT_ADMIN, "retention.change", 0, NULL, 0, "workspace", 1, detail);
}

/* Count of one statement bound to one id. */
static int64_t aged_count(sqlite3 *db, const char *sql, uint64_t id) {
    sqlite3_stmt *st = NULL;
    int64_t n = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)id);
        if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    return n;
}

/* Run one statement bound to one id, queueing each blob key it RETURNs for
 * reclaim as the daemon's own work (no attachment row behind it). */
static void aged_reclaim_keys(sqlite3 *db, oc_dbres *r, size_t cap, const char *sql, uint64_t id) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)id);
        while (sqlite3_step(st) == SQLITE_ROW && r->n_reclaim < cap) {
            const char *key = (const char *)sqlite3_column_text(st, 0);
            char *dup = key ? strdup(key) : NULL;
            if (!dup) continue;
            r->reclaim[r->n_reclaim].storage_key = dup;
            r->reclaim[r->n_reclaim].attachment_id = 0;
            r->n_reclaim++;
        }
    }
    sqlite3_finalize(st);
}

static void aged_exec_id(sqlite3 *db, const char *sql, uint64_t id) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)id);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
}

/* Remove by age every call that ended longer ago than `keep_ms` (REQ-250): its
 * people, invitations, timeline, recordings, transcripts and its chat, whose
 * files -- the recording among them -- their previews and exported copies, and every
 * read-aloud rendering of its messages are reclaimed. The message a call left
 * in the conversation it was started from is a message, and ages by its own
 * time; until then it stays, naming no call. A call not yet ended never goes.
 * At most `batch` calls a pass, and a call whose bytes would not fit what is
 * left of this pass's reclaim list waits for the next. */
static void purge_aged_calls(sqlite3 *db, oc_dbres *r, size_t cap, uint64_t now, uint64_t keep_ms, uint32_t batch) {
    if (!keep_ms) return;
    const uint64_t cutoff = now > keep_ms ? now - keep_ms : 0;
    uint64_t calls[256]; int nc = 0;
    if (batch == 0 || batch > 256) batch = 256;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT id FROM calls WHERE ended_at_ms IS NOT NULL AND ended_at_ms < ?1 ORDER BY id LIMIT ?2;",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)cutoff);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)batch);
        while (sqlite3_step(st) == SQLITE_ROW && nc < 256) calls[nc++] = (uint64_t)sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    for (int i = 0; i < nc; i++) {
        const uint64_t call = calls[i];
        /* What its chat's bytes need of the reclaim list: every file not yet
         * reclaimed, every exported copy of one, every rendering of a message. */
        int64_t need =
            aged_count(db, "SELECT COUNT(*) FROM attachments a JOIN channels c ON c.id = a.channel_id"
                           " WHERE c.call_id = ?1 AND a.reclaimed_at_ms = 0;", call) +
            aged_count(db, "SELECT COUNT(*) FROM exported_video e JOIN attachments a ON a.id = e.attachment_id"
                           " JOIN channels c ON c.id = a.channel_id WHERE c.call_id = ?1;", call) +
            aged_count(db, "SELECT COUNT(*) FROM attachment_previews p JOIN attachments a ON a.id = p.attachment_id"
                           " JOIN channels c ON c.id = a.channel_id WHERE c.call_id = ?1;", call) +
            aged_count(db, "SELECT COUNT(*) FROM rendered_audio WHERE handle IN (SELECT u.handle FROM rendered_audio_uses u"
                           " JOIN messages m ON m.id = u.message_id JOIN channels c ON c.id = m.channel_id WHERE c.call_id = ?1);", call);
        if (r->n_reclaim + (size_t)need > cap) break;

        /* The chat's files, the recording among them: tombstoned and queued. */
        if (sqlite3_prepare_v2(db, "SELECT a.id, a.storage_key FROM attachments a JOIN channels c ON c.id = a.channel_id"
                                   " WHERE c.call_id = ?1 AND a.reclaimed_at_ms = 0;", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)call);
            while (sqlite3_step(st) == SQLITE_ROW)
                reclaim_add(db, r, cap, (uint64_t)sqlite3_column_int64(st, 0), (const char *)sqlite3_column_text(st, 1),
                            now, OC_RECLAIM_EXPIRED);
        }
        sqlite3_finalize(st);
        aged_reclaim_keys(db, r, cap,
            "DELETE FROM exported_video WHERE attachment_id IN (SELECT a.id FROM attachments a"
            " JOIN channels c ON c.id = a.channel_id WHERE c.call_id = ?1) RETURNING blob_key;", call);
        aged_reclaim_keys(db, r, cap,
            "DELETE FROM attachment_previews WHERE attachment_id IN (SELECT a.id FROM attachments a"
            " JOIN channels c ON c.id = a.channel_id WHERE c.call_id = ?1) RETURNING storage_key;", call);
        aged_reclaim_keys(db, r, cap,
            "DELETE FROM rendered_audio WHERE handle IN (SELECT u.handle FROM rendered_audio_uses u"
            " JOIN messages m ON m.id = u.message_id JOIN channels c ON c.id = m.channel_id WHERE c.call_id = ?1)"
            " RETURNING blob_key;", call);

        /* The call's own rows, then its chat and the chat's files, then the call. */
        aged_exec_id(db, "UPDATE messages SET call_id = NULL WHERE call_id = ?1 AND channel_id NOT IN"
                         " (SELECT id FROM channels WHERE call_id = ?1);", call);
        aged_exec_id(db, "DELETE FROM call_recordings WHERE call_id = ?1;", call);
        aged_exec_id(db, "DELETE FROM call_transcript_lines WHERE transcript_id IN"
                         " (SELECT id FROM call_transcripts WHERE call_id = ?1);", call);
        aged_exec_id(db, "DELETE FROM call_transcripts WHERE call_id = ?1;", call);
        aged_exec_id(db, "DELETE FROM call_events WHERE call_id = ?1;", call);
        aged_exec_id(db, "DELETE FROM call_invitees WHERE call_id = ?1;", call);
        aged_exec_id(db, "DELETE FROM call_participants WHERE call_id = ?1;", call);
        aged_exec_id(db, "DELETE FROM snippets_fts WHERE rowid IN (SELECT a.id FROM attachments a"
                         " JOIN channels c ON c.id = a.channel_id WHERE c.call_id = ?1);", call);
        aged_exec_id(db, "DELETE FROM attachment_media WHERE attachment_id IN (SELECT a.id FROM attachments a"
                         " JOIN channels c ON c.id = a.channel_id WHERE c.call_id = ?1) OR poster_id IN"
                         " (SELECT a.id FROM attachments a JOIN channels c ON c.id = a.channel_id WHERE c.call_id = ?1);", call);
        uint64_t chats[8]; int nch = 0;
        if (sqlite3_prepare_v2(db, "SELECT id FROM channels WHERE call_id = ?1;", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)call);
            while (sqlite3_step(st) == SQLITE_ROW && nch < 8) chats[nch++] = (uint64_t)sqlite3_column_int64(st, 0);
        }
        sqlite3_finalize(st);
        for (int k = 0; k < nch; k++) {
            /* The files go first, by message and by channel, so purge_channel
             * meets no row naming them. */
            aged_exec_id(db, "UPDATE attachments SET message_id = NULL WHERE channel_id = ?1;", chats[k]);
            aged_exec_id(db, "DELETE FROM attachments WHERE channel_id = ?1;", chats[k]);
            purge_channel(db, chats[k]);
        }
        aged_exec_id(db, "DELETE FROM calls WHERE id = ?1;", call);
    }
}

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
    /* Encrypted DMs closed a day ago (PRIVATE-DM.md). */
    purge_closed_e2e(db, now);
    /* Summary notices a day after they were seen; unseen ones stay. */
    if (sqlite3_prepare_v2(db, "DELETE FROM summary_notices WHERE seen_at_ms IS NOT NULL AND seen_at_ms < ?1;", -1,
                           &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)(now > SUM_NOTICE_KEEP_MS ? now - SUM_NOTICE_KEEP_MS : 0));
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
    st = NULL;
    /* Calls' recordings kept their days (OPENCHIME_CALL_RECORDING_DAYS, CALLS.md
     * §8): one older, from its call's end, goes -- its bytes and its row; the
     * message that carried it shows it gone, as any file reclaimed. */
    if (j->maint_rec_days) prune_recordings(db, r, cap, now, j->maint_rec_days);
    /* Message retention (REQ-250): messages, and calls, past the age go, in one
     * transaction, a bounded batch a pass. */
    if (j->maint_retention_days) {
        const uint64_t keep = (uint64_t)j->maint_retention_days * 24ull * 60 * 60 * 1000;
        const uint32_t batch = j->maint_batch ? j->maint_batch : 64;
        sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);
        purge_aged(db, r, cap, now, keep, batch);
        purge_aged_calls(db, r, cap, now, keep, batch);
        sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
    }

    /* Tier 0 (ARCH-111): read-aloud renderings, before anything a user uploaded.
     * A rendering is the daemon's own work and can be made again from the message
     * it speaks, so under pressure it is the cheapest thing in the store to lose:
     * the cost is one re-render, and nothing is shown in its place. Least recently
     * listened to first, and no tombstone — unlike an attachment (ARCH-77), there
     * is nothing for a reader to be told about. */
    if (j->maint_evict && r->n_reclaim < cap) {
        st = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT handle, model_version, blob_key FROM rendered_audio"
                " ORDER BY last_used_ms ASC LIMIT ?1;", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)(cap - r->n_reclaim));
            while (sqlite3_step(st) == SQLITE_ROW && r->n_reclaim < cap) {
                const void *handle = sqlite3_column_blob(st, 0);
                int handle_len = sqlite3_column_bytes(st, 0);
                const unsigned char *ver = sqlite3_column_text(st, 1);
                const char *key = (const char *)sqlite3_column_text(st, 2);
                char *dup = key ? strdup(key) : NULL;
                if (!dup) continue;
                sqlite3_stmt *del = NULL;
                int gone = 0;
                if (sqlite3_prepare_v2(db,
                        "DELETE FROM rendered_audio WHERE handle = ?1 AND model_version = ?2;",
                        -1, &del, NULL) == SQLITE_OK) {
                    sqlite3_bind_blob(del, 1, handle, handle_len, SQLITE_TRANSIENT);
                    sqlite3_bind_text(del, 2, ver ? (const char *)ver : "", -1, SQLITE_TRANSIENT);
                    gone = sqlite3_step(del) == SQLITE_DONE && sqlite3_changes(db) > 0;
                    sqlite3_finalize(del);
                }
                if (!gone) { free(dup); continue; }
                /* attachment_id 0: this is not an attachment, and the net thread
                 * needs only the key to delete the bytes. */
                r->reclaim[r->n_reclaim].storage_key = dup;
                r->reclaim[r->n_reclaim].attachment_id = 0;
                r->n_reclaim++;
                r->maint_renders++;
            }
            sqlite3_finalize(st);
        }
    }

    /* Exported copies (ARCH-135) go with their video: every pass, pressure or
     * not, a copy whose video has been reclaimed is deleted. Then, under
     * pressure, they are reclaimed with the read-aloud renderings above, least
     * recently used first: a copy is the daemon's own work and is made again
     * from its video, so it is as cheap to lose. No tombstone either. */
    for (int under_pressure = 0; under_pressure < 2 && r->n_reclaim < cap; under_pressure++) {
        if (under_pressure && !j->maint_evict) break;
        st = NULL;
        if (sqlite3_prepare_v2(db, under_pressure
                ? "SELECT attachment_id, version, blob_key FROM exported_video"
                  " ORDER BY last_used_ms ASC LIMIT ?1;"
                : "SELECT e.attachment_id, e.version, e.blob_key FROM exported_video e"
                  "  JOIN attachments a ON a.id = e.attachment_id"
                  " WHERE a.reclaimed_at_ms != 0 LIMIT ?1;", -1, &st, NULL) != SQLITE_OK) {
            sqlite3_finalize(st);
            continue;
        }
        sqlite3_bind_int64(st, 1, (sqlite3_int64)(cap - r->n_reclaim));
        while (sqlite3_step(st) == SQLITE_ROW && r->n_reclaim < cap) {
            sqlite3_int64 att = sqlite3_column_int64(st, 0);
            const unsigned char *ver = sqlite3_column_text(st, 1);
            const char *key = (const char *)sqlite3_column_text(st, 2);
            char *dup = key ? strdup(key) : NULL;
            if (!dup) continue;
            sqlite3_stmt *del = NULL;
            int gone = 0;
            if (sqlite3_prepare_v2(db, "DELETE FROM exported_video WHERE attachment_id = ?1 AND version = ?2;",
                                   -1, &del, NULL) == SQLITE_OK) {
                sqlite3_bind_int64(del, 1, att);
                sqlite3_bind_text(del, 2, ver ? (const char *)ver : "", -1, SQLITE_TRANSIENT);
                gone = sqlite3_step(del) == SQLITE_DONE && sqlite3_changes(db) > 0;
                sqlite3_finalize(del);
            }
            if (!gone) { free(dup); continue; }
            r->reclaim[r->n_reclaim].storage_key = dup;     /* not an attachment: the key alone */
            r->reclaim[r->n_reclaim].attachment_id = 0;
            r->n_reclaim++;
            r->maint_renders++;
        }
        sqlite3_finalize(st);
    }

    /* Image previews (ARCH-137) are derived data too, and go the same way as
     * the exported copies: every pass, pressure or not, a preview whose
     * attachment's bytes are gone (reclaimed, or the row itself deleted) goes
     * with them; and under pressure, least recently served first, before any
     * attachment tier below. No tombstone: a client that finds a rung missing
     * shows the original. */
    for (int under_pressure = 0; under_pressure < 2 && r->n_reclaim < cap; under_pressure++) {
        if (under_pressure && !j->maint_evict) break;
        st = NULL;
        if (sqlite3_prepare_v2(db, under_pressure
                ? "SELECT attachment_id, rung, storage_key FROM attachment_previews"
                  " ORDER BY last_used_ms ASC LIMIT ?1;"
                : "SELECT p.attachment_id, p.rung, p.storage_key FROM attachment_previews p"
                  "  LEFT JOIN attachments a ON a.id = p.attachment_id"
                  " WHERE a.id IS NULL OR a.reclaimed_at_ms != 0 LIMIT ?1;", -1, &st, NULL) != SQLITE_OK) {
            sqlite3_finalize(st);
            continue;
        }
        sqlite3_bind_int64(st, 1, (sqlite3_int64)(cap - r->n_reclaim));
        while (sqlite3_step(st) == SQLITE_ROW && r->n_reclaim < cap) {
            sqlite3_int64 att = sqlite3_column_int64(st, 0);
            int rung = sqlite3_column_int(st, 1);
            const char *key = (const char *)sqlite3_column_text(st, 2);
            char *dup = key ? strdup(key) : NULL;
            if (!dup) continue;
            sqlite3_stmt *del = NULL;
            int gone = 0;
            if (sqlite3_prepare_v2(db, "DELETE FROM attachment_previews WHERE attachment_id = ?1 AND rung = ?2;",
                                   -1, &del, NULL) == SQLITE_OK) {
                sqlite3_bind_int64(del, 1, att);
                sqlite3_bind_int(del, 2, rung);
                gone = sqlite3_step(del) == SQLITE_DONE && sqlite3_changes(db) > 0;
                sqlite3_finalize(del);
            }
            if (!gone) { free(dup); continue; }
            r->reclaim[r->n_reclaim].storage_key = dup;     /* not an attachment: the key alone */
            r->reclaim[r->n_reclaim].attachment_id = 0;
            r->n_reclaim++;
            r->maint_previews++;
        }
        sqlite3_finalize(st);
    }

    /* Tier 1 (REQ-213): orphans — uploaded but never referenced by a message,
     * and old enough that an in-flight upload cannot be caught by mistake. This
     * is the sweep migration 0009's index was created for, and it is pure
     * garbage collection: nobody was ever promised these bytes. */
    if (sqlite3_prepare_v2(db,
            "SELECT id, storage_key FROM attachments "
            "WHERE message_id IS NULL AND reclaimed_at_ms = 0 AND created_at_ms < ?1 "
            /* An AVATAR is an attachment no message references, so it looks
             * exactly like an orphan to this sweep — and would have been collected an
             * hour after being set, leaving every profile picture in the workspace
             * silently blank. The other two tiers below need the same exclusion for
             * the same reason: an avatar is in use even though nothing points at it
             * from `messages`. */
            "  AND id NOT IN (SELECT avatar_attachment_id FROM users "
            "                  WHERE avatar_attachment_id IS NOT NULL) "
            "  AND id NOT IN (SELECT attachment_id FROM custom_emoji) "
            /* A video message's POSTER is the same case again, with a lifetime
             * that ends: it is in use for exactly as long as its video is linked
             * and not reclaimed. Once the video goes — deleted with its message,
             * never sent, or reclaimed by the tiers below — the poster is an
             * orphan like any other and this tier collects it. */
            "  AND id NOT IN (SELECT m.poster_id FROM attachment_media m "
            "                   JOIN attachments v ON v.id = m.attachment_id "
            "                  WHERE m.poster_id IS NOT NULL "
            "                    AND v.message_id IS NOT NULL AND v.reclaimed_at_ms = 0) "
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
                "  AND id NOT IN (SELECT avatar_attachment_id FROM users "
                "                  WHERE avatar_attachment_id IS NOT NULL) "
                /* A custom emoji is in use for the same non-obvious reason an avatar
                 * is: no message references it, so it looks like an orphan. */
                "  AND id NOT IN (SELECT attachment_id FROM custom_emoji) "
                /* Posters are never chosen on their own account: one goes with
                 * its video (reclaim_poster_of), so a video is never left
                 * showing a blank still. */
                "  AND id NOT IN (SELECT poster_id FROM attachment_media WHERE poster_id IS NOT NULL) "
                "ORDER BY created_at_ms ASC LIMIT ?2;", -1, &st, NULL) == SQLITE_OK) {
            uint64_t cutoff = (now > j->maint_max_age_ms) ? now - j->maint_max_age_ms : 0;
            sqlite3_bind_int64(st, 1, (sqlite3_int64)cutoff);
            sqlite3_bind_int64(st, 2, (sqlite3_int64)(cap - r->n_reclaim));
            while (sqlite3_step(st) == SQLITE_ROW && r->n_reclaim < cap) {
                if (reclaim_add(db, r, cap, (uint64_t)sqlite3_column_int64(st, 0),
                                (const char *)sqlite3_column_text(st, 1), now, OC_RECLAIM_EXPIRED)) {
                    r->maint_expired++;
                    reclaim_poster_of(db, r, cap, (uint64_t)sqlite3_column_int64(st, 0), now, OC_RECLAIM_EXPIRED);
                }
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
                "  AND id NOT IN (SELECT avatar_attachment_id FROM users "
                "                  WHERE avatar_attachment_id IS NOT NULL) "
                /* A custom emoji is in use for the same non-obvious reason an avatar
                 * is: no message references it, so it looks like an orphan. */
                "  AND id NOT IN (SELECT attachment_id FROM custom_emoji) "
                /* Posters are never chosen on their own account: one goes with
                 * its video (reclaim_poster_of), so a video is never left
                 * showing a blank still. */
                "  AND id NOT IN (SELECT poster_id FROM attachment_media WHERE poster_id IS NOT NULL) "
                "ORDER BY created_at_ms ASC LIMIT ?2;", -1, &st, NULL) == SQLITE_OK) {
            uint64_t cutoff = (now > j->maint_grace_ms) ? now - j->maint_grace_ms : 0;
            sqlite3_bind_int64(st, 1, (sqlite3_int64)cutoff);
            sqlite3_bind_int64(st, 2, (sqlite3_int64)(cap - r->n_reclaim));
            while (sqlite3_step(st) == SQLITE_ROW && r->n_reclaim < cap) {
                if (reclaim_add(db, r, cap, (uint64_t)sqlite3_column_int64(st, 0),
                                (const char *)sqlite3_column_text(st, 1), now, OC_RECLAIM_EVICTED)) {
                    r->maint_evicted++;
                    reclaim_poster_of(db, r, cap, (uint64_t)sqlite3_column_int64(st, 0), now, OC_RECLAIM_EVICTED);
                }
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
    pthread_mutex_lock(&w->mu);    /* the test knob may change these meanwhile */
    int due = w->last_prune_ms == 0 || now - w->last_prune_ms >= w->prune_interval_ms;
    if (due) w->last_prune_ms = now;
    uint64_t retention = w->idem_retention_ms;
    pthread_mutex_unlock(&w->mu);
    if (!due) return;
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(w->db, "DELETE FROM sent_messages WHERE created_at_ms < ?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)(now - retention));
    sqlite3_step(st);
    sqlite3_finalize(st);
}

/* The workspace's own sender (SCHEMA.md §3be): its user id, made the first
 * time it is needed. No credential and no identity, so nothing signs in as
 * it. 0 if it could not be made. */
static uint64_t workspace_sender(sqlite3 *db) {
    sqlite3_stmt *st = NULL;
    uint64_t id = 0;
    sqlite3_prepare_v2(db, "SELECT id FROM users WHERE is_system=1 ORDER BY id LIMIT 1;", -1, &st, NULL);
    if (sqlite3_step(st) == SQLITE_ROW) id = (uint64_t)sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    if (id) return id;
    sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO users(subject, display_name, role, created_at_ms, is_system) "
                           "VALUES('system:workspace', 'Workspace', 'member', ?1, 1);", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)dbw_now_ms());
    sqlite3_step(st);
    sqlite3_finalize(st);
    sqlite3_prepare_v2(db, "SELECT id FROM users WHERE subject='system:workspace' AND is_system=1;", -1, &st, NULL);
    if (sqlite3_step(st) == SQLITE_ROW) id = (uint64_t)sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    g_sender_made = id;
    return id;
}

/* Post `body` in `channel_id` as the workspace's own sender, inside the
 * caller's transaction when there is one, and queue it for the members
 * connected. It is stored without mentions: a notice notifies nobody, and no
 * unread count includes it. The message id, or 0. */
static uint64_t notice_post(sqlite3 *db, uint64_t channel_id, const char *body, size_t len) {
    uint64_t sys = workspace_sender(db);
    if (!sys || !channel_id || !body) return 0;
    if (len > OC_MAX_BODY_SIZE) len = (size_t)utf8_prefix_len(body, OC_MAX_BODY_SIZE);
    uint64_t ts = dbw_now_ms();
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "INSERT INTO messages(channel_id, author_id, body, created_at_ms) VALUES(?,?,?,?);",
                       -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)channel_id);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)sys);
    sqlite3_bind_blob(st, 3, body, (int)len, SQLITE_STATIC);
    sqlite3_bind_int64(st, 4, (sqlite3_int64)ts);
    int ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    if (!ok) return 0;
    uint64_t mid = (uint64_t)sqlite3_last_insert_rowid(db);
    oc_dbres *r = calloc(1, sizeof *r);
    if (r) {
        r->type = OC_RES_NOTICE;
        r->message_id = mid;
        r->channel_id = channel_id;
        r->author_id = sys;
        r->server_time = ts;
        r->body = malloc(len ? len : 1);
        if (r->body) { memcpy(r->body, body, len); r->body_len = len; }
        r->broadcast_stored = broadcast_stored(db, mid);
        append_action(db, r, mid, channel_id, 1);
        load_members(db, channel_id, r);
        oc_dbres **tail = &g_notices;
        while (*tail) tail = &(*tail)->next;
        *tail = r;
    }
    return mid;
}

/* The DM between the workspace's own sender and `uid`, made if need be; 0 if
 * it cannot be. */
static uint64_t workspace_dm_with(sqlite3 *db, uint64_t uid) {
    uint64_t sys = workspace_sender(db), cid = 0;
    if (!sys || !uid || sys == uid) return 0;
    char key[48];
    snprintf(key, sizeof key, "%llu,%llu", (unsigned long long)(sys < uid ? sys : uid),
             (unsigned long long)(sys < uid ? uid : sys));
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO channels(kind,name,is_public,created_at_ms,dm_key) "
                           "VALUES('dm',NULL,0,?1,?2);", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)dbw_now_ms());
    sqlite3_bind_text(st, 2, key, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
    sqlite3_prepare_v2(db, "SELECT id FROM channels WHERE kind='dm' AND dm_key=?1;", -1, &st, NULL);
    sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) cid = (uint64_t)sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    if (cid) { add_membership(db, cid, sys); add_membership(db, cid, uid); }
    return cid;
}

/* Run `stmt` in a transaction of its own unless the caller is already in one. */
#define NOTICE_TXN(db, stmt) do { \
        int own_txn_ = sqlite3_get_autocommit(db); \
        if (own_txn_) sqlite3_exec((db), "BEGIN;", NULL, NULL, NULL); \
        stmt; \
        if (own_txn_) sqlite3_exec((db), "COMMIT;", NULL, NULL, NULL); \
    } while (0)

static void notice_vpost(sqlite3 *db, uint64_t channel_id, uint64_t dm_user, const char *fmt, va_list ap) {
    char body[1024];
    int n = vsnprintf(body, sizeof body, fmt, ap);
    if (n <= 0) return;
    size_t len = (size_t)n < sizeof body ? (size_t)n : sizeof body - 1;
    NOTICE_TXN(db, {
        uint64_t cid = dm_user ? workspace_dm_with(db, dm_user) : channel_id;
        if (cid) notice_post(db, cid, body, len);
    });
}

/* A notice in `channel_id` (OPENCHIME_NOTICE_CHANNEL_CHANGES). */
static void notice_channel(sqlite3 *db, uint64_t channel_id, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    notice_vpost(db, channel_id, 0, fmt, ap);
    va_end(ap);
}

/* The public, unarchived channel `name` names (a leading # allowed), or 0. */
static uint64_t notice_target(sqlite3 *db, const char *name) {
    if (!name) return 0;
    while (*name == ' ' || *name == '#') name++;
    if (!*name) return 0;
    sqlite3_stmt *st = NULL;
    uint64_t cid = 0;
    sqlite3_prepare_v2(db, "SELECT id FROM channels WHERE kind='channel' AND name=?1 AND is_public=1 "
                           "AND archived_at_ms IS NULL;", -1, &st, NULL);
    sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) cid = (uint64_t)sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return cid;
}

/* A notice in the channel a variable names (OPENCHIME_NOTICE_MEMBER_JOINED,
 * _CHANNEL_CREATED); nothing when it is empty or names no such channel. */
static void notice_named(sqlite3 *db, const char *channel_name, const char *fmt, ...) {
    uint64_t cid = notice_target(db, channel_name);
    if (!cid) return;
    va_list ap;
    va_start(ap, fmt);
    notice_vpost(db, cid, 0, fmt, ap);
    va_end(ap);
}

/* A notice to one person, in their DM with the workspace. */
static void notice_person(sqlite3 *db, uint64_t user_id, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    notice_vpost(db, 0, user_id, fmt, ap);
    va_end(ap);
}

/* A variable naming a channel that is none, said once at start. */
static void notice_report(sqlite3 *db) {
    static const struct { const char *var; size_t off; } V[] = {
        { "OPENCHIME_NOTICE_MEMBER_JOINED", offsetof(oc_config, notice_member_joined) },
        { "OPENCHIME_NOTICE_CHANNEL_CREATED", offsetof(oc_config, notice_channel_created) },
    };
    for (size_t i = 0; i < sizeof V / sizeof V[0]; i++) {
        const char *v = *(const char *const *)((const char *)oc_config_get() + V[i].off);
        if (v && v[0] && !notice_target(db, v))
            fprintf(stderr, "openchimed: %s: no public channel named %s; nothing is posted\n", V[i].var, v);
    }
}

/* The welcome a new member is sent (OPENCHIME_WELCOME_MESSAGE): the text as it
 * is when they join, in a DM from the workspace's own sender. Unset sends
 * nothing. `\n` in the value is a line break, since one line is what a
 * variable holds. */
static void send_welcome(sqlite3 *db, uint64_t uid) {
    const char *text = oc_config_get()->welcome_message;
    if (!text || !*text || !uid) return;
    size_t tl = strlen(text);
    char *body = malloc(tl + 1);
    if (!body) return;
    size_t n = 0;
    for (size_t i = 0; i < tl; i++) {
        if (text[i] == '\\' && text[i + 1] == 'n') { body[n++] = '\n'; i++; }
        else body[n++] = text[i];
    }
    body[n] = '\0';
    NOTICE_TXN(db, {
        uint64_t cid = workspace_dm_with(db, uid);
        if (cid) notice_post(db, cid, body, n);
    });
    free(body);
}

static void *writer_loop(void *arg) {
    oc_dbwriter *w = (oc_dbwriter *)arg;
    for (;;) {
        pthread_mutex_lock(&w->mu);
        while (!w->stop && (!w->jobs_head || (w->hold & OC_DBW_HOLD_WRITER)))
            pthread_cond_wait(&w->cv, &w->mu);
        if (w->stop && !w->jobs_head) { pthread_mutex_unlock(&w->mu); break; }
        oc_job *j = w->jobs_head;
        w->jobs_head = j->next;
        if (!w->jobs_head) w->jobs_tail = NULL;
        pthread_mutex_unlock(&w->mu);

        w->auth_deferred = 0;
        g_joined_uid = 0;
        g_notices = NULL;
        g_sender_made = 0;
        oc_dbres *r = process_write(w, j);
        /* A new member joins the workspace's default channels, whichever way
         * their account was made (OPENCHIME_DEFAULT_CHANNELS). */
        if (g_joined_uid) join_default_channels(w->db, g_joined_uid, 0);
        /* And is sent the workspace's welcome, as it reads now; and the
         * workspace says they joined, where it is told to. */
        if (g_joined_uid) {
            send_welcome(w->db, g_joined_uid);
            char *who = audit_name_of(w->db, g_joined_uid);
            notice_named(w->db, oc_config_get()->notice_member_joined, "@%s joined the workspace.",
                         who ? who : "someone");
            free(who);
        }
        /* A sender made just now is told to everyone first (USER_LIST, flagged),
         * so no client takes its first notice for a person's message. */
        if (g_sender_made) {
            oc_dbres *ro = calloc(1, sizeof *ro);
            if (ro) {
                ro->type = OC_RES_ROSTER_ROW;
                user_rows(w->db, g_sender_made, ro);
                ro->joined = ro->n_ulist != 0;
                ro->next = g_notices;
                g_notices = ro;
            }
        }
        /* The notices the job posted ride on its answer. */
        if (g_notices) {
            if (r) { oc_dbres *t = r; while (t->next) t = t->next; t->next = g_notices; }
            else r = g_notices;
            g_notices = NULL;
        }
        /* Joined while receipts are off: marked, as everyone was at start. */
        if (g_joined_uid && oc_config_get()->read_receipts != 0) {
            sqlite3_stmt *rs = NULL;
            sqlite3_prepare_v2(w->db, "UPDATE users SET receipts_since_ms = -1 WHERE id=?1;", -1, &rs, NULL);
            sqlite3_bind_int64(rs, 1, (sqlite3_int64)g_joined_uid);
            sqlite3_step(rs);
            sqlite3_finalize(rs);
        }
        /* A member who did not exist before this job: their roster row rides on
         * its result, for everyone connected, whatever the job answers. */
        if (g_joined_uid && r && !r->ulist) {
            user_rows(w->db, g_joined_uid, r);
            r->joined = r->n_ulist != 0;
        }
        for (oc_dbres *q = r; q; q = q->next) q->gen = j->gen;
        if (!w->auth_deferred) job_free(j);   /* else the auth pool has it (process_auth) */
        push_result(w, r);
        maybe_prune_idem(w);
    }
    return NULL;
}

static void auth_read_credential(oc_dbwriter *w, sqlite3 *rdb, oc_job *j);

static void *reader_loop(void *arg) {
    db_reader *rd = (db_reader *)arg;
    oc_dbwriter *w = rd->w;
    for (;;) {
        pthread_mutex_lock(&w->mu);
        while (!w->stop && (!rd->head || (w->hold & OC_DBW_HOLD_READERS)))
            pthread_cond_wait(&rd->cv, &w->mu);
        if (w->stop && !rd->head) { pthread_mutex_unlock(&w->mu); break; }
        oc_job *j = rd->head;
        rd->head = j->next;
        if (!rd->head) rd->tail = NULL;
        pthread_mutex_unlock(&w->mu);

        if (j->type == OC_JOB_AUTH) {
            auth_read_credential(w, rd->rdb, j);   /* a password's credential (process_auth) */
            continue;
        }
        oc_dbres *r = process_read(rd->rdb, j);
        for (oc_dbres *q = r; q; q = q->next) q->gen = j->gen;
        job_free(j);
        push_result(w, r);
    }
    return NULL;
}

/* The auth pool is done with a check: the job goes back to the writer to be
 * finished, CHECKED. A check never made (the pool is stopping) ends here. */
static void auth_checked(oc_auth_check *chk, void *ctx) {
    oc_dbwriter *w = ctx;
    oc_job *j = chk->owner;
    int ok = chk->ok;
    if (chk->derived) {   /* a password change: the new password's key */
        memcpy(j->pw_salt, chk->new_salt, OC_PW_SALT_LEN);
        memcpy(j->pw_hash, chk->new_hash, OC_PW_HASH_LEN);
        j->pw_mem = chk->new_mem;
        j->pw_derived = 1;
    }
    oc_e2e_wipe(chk, sizeof *chk);
    free(chk);
    if (ok < 0) { job_free(j); return; }
    j->auth_ok = ok;
    j->auth_stage = OC_AUTH_STAGE_CHECKED;
    oc_dbwriter_submit(w, j);
}

/* A reader's part of a page's password check: fetch the credential and its
 * version by the name signed in with and hand the check to the pool; the
 * password page also gets a fresh salt for the new password. No account, or no
 * memory, goes straight back to the writer as a failed check. */
static void auth_read_credential(oc_dbwriter *w, sqlite3 *rdb, oc_job *j) {
    oc_auth_check *chk = calloc(1, sizeof *chk);
    uint64_t uid = 0, version = 0;
    int found = 0;
    if (chk) {
        oc_slice cred = { (const uint8_t *)j->token, j->token_len }, user, pass;
        if (oc_parse_local_credential(cred, &user, &pass) == OC_OK) {
            chk->password = (const char *)pass.ptr;     /* in j->token, which outlives the check */
            chk->pwlen = pass.len;
            found = local_credential(rdb, (const char *)user.ptr, user.len, &uid, chk->salt, sizeof chk->salt,
                                     &chk->slen, &chk->mem, chk->stored, &version);
        }
        /* A change from the password page: the new password's key too, as a
         * signed-in change gets it. */
        if (found && j->web == OC_WEB_PASSWORD) {
            found = j->pf_new_pw && j->pf_new_pw[0] &&
                    oc_rand_bytes(chk->new_salt, sizeof chk->new_salt) == 0;
            if (found) {
                chk->new_password = j->pf_new_pw;
                chk->new_pwlen = strlen(j->pf_new_pw);
                chk->new_mem = w->pw_mem;
            }
        }
    }
    /* A name with no account is checked all the same, against a credential no
     * password matches, so the time to answer does not tell which names exist.
     * It is a check against user 0, which no sign-in completes. */
    if (!found && chk && chk->password &&
        oc_rand_bytes(chk->salt, OC_PW_SALT_LEN) == 0 && oc_rand_bytes(chk->stored, sizeof chk->stored) == 0) {
        chk->slen = OC_PW_SALT_LEN;
        chk->mem = w->pw_mem;
        chk->new_password = NULL;
        uid = 0;
        found = 1;
    }
    if (found) {
        j->auth_uid = uid;
        j->auth_version = version;
        chk->owner = j;
        oc_authpool_submit(w->auth, chk);
        return;
    }
    free(chk);
    j->auth_ok = 0;
    j->auth_stage = OC_AUTH_STAGE_CHECKED;
    oc_dbwriter_submit(w, j);
}

/* Queue `j` on the reader its connection uses, whatever its type. */
static void submit_to_reader(oc_dbwriter *w, oc_job *j) {
    pthread_mutex_lock(&w->mu);
    j->next = NULL;
    db_reader *rd = &w->readers[j->conn_id ? j->conn_id % OC_DB_READERS
                                           : w->next_reader++ % OC_DB_READERS];
    if (rd->tail) rd->tail->next = j; else rd->head = j;
    rd->tail = j;
    pthread_cond_signal(&rd->cv);
    pthread_mutex_unlock(&w->mu);
}

void oc_dbwriter_hold_auth(oc_dbwriter *w, int on) { oc_authpool_hold(w->auth, on); }

size_t oc_dbwriter_jobs_waiting(oc_dbwriter *w, int type) {
    size_t n = 0;
    pthread_mutex_lock(&w->mu);
    for (const oc_job *j = w->jobs_head; j; j = j->next) n += j->type == type;
    for (int i = 0; i < OC_DB_READERS; i++)
        for (const oc_job *j = w->readers[i].head; j; j = j->next) n += j->type == type;
    pthread_mutex_unlock(&w->mu);
    return n;
}

void oc_dbwriter_hold(oc_dbwriter *w, int what) {
    pthread_mutex_lock(&w->mu);
    w->hold = what;
    pthread_cond_broadcast(&w->cv);
    for (int i = 0; i < OC_DB_READERS; i++) pthread_cond_broadcast(&w->readers[i].cv);
    pthread_mutex_unlock(&w->mu);
}
size_t oc_dbwriter_auth_waiting(oc_dbwriter *w) { return oc_authpool_waiting(w->auth); }

void oc_dbwriter_submit(oc_dbwriter *w, oc_job *j) {
    pthread_mutex_lock(&w->mu);
    j->next = NULL;
    if (is_read_job(j->type)) {                 /* route to a reader (ARCH-66) */
        db_reader *rd = &w->readers[j->conn_id ? j->conn_id % OC_DB_READERS
                                               : w->next_reader++ % OC_DB_READERS];
        if (rd->tail) rd->tail->next = j; else rd->head = j;
        rd->tail = j;
        pthread_cond_signal(&rd->cv);
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

int oc_dbwriter_factor_key(oc_dbwriter *w, uint8_t key[OC_FACTOR_KEY_LEN]) {
    if (!w || !w->have_factor_key) return -1;
    memcpy(key, w->factor_key, OC_FACTOR_KEY_LEN);
    return 0;
}

int oc_dbwriter_configure_oidc(oc_dbwriter *w, const char *issuer,
                               const char *audience, const char *pubkey_pem,
                               const char *relay_origin) {
    if (!issuer || !audience || !pubkey_pem) return -1;
    free(w->oidc_issuer); free(w->oidc_audience);
    free(w->oidc_pubkey_pem); free(w->oidc_keyset_pem); free(w->relay_origin);
    w->oidc_issuer     = strdup(issuer);
    w->oidc_audience   = strdup(audience);
    w->oidc_pubkey_pem = strdup(pubkey_pem);
    w->relay_origin    = strdup(relay_origin ? relay_origin : "");
    pthread_mutex_lock(&w->keys_mu);
    free(w->oidc_keyset_pem);
    w->oidc_keyset_pem = strdup(pubkey_pem);
    pthread_mutex_unlock(&w->keys_mu);
    if (!w->oidc_keyset_pem) return -1;
    if (!w->seen_jti) w->seen_jti = calloc(1, sizeof *w->seen_jti);
    if (!w->seen_jti) return -1;
    if (!w->oidc_issuer || !w->oidc_audience || !w->oidc_pubkey_pem || !w->relay_origin)
        return -1;
    w->oidc_enabled = 1;
    /* A source is added, not swapped in: which others are on is the caller's to
     * say (oc_dbwriter_set_local_enabled). */
    w->auth_methods |= OC_AUTH_OIDC;
    return 0;
}

void oc_dbwriter_set_relay_keys(oc_dbwriter *w, const char *fetched_pem) {
    if (!w || !fetched_pem || !w->oidc_pubkey_pem) return;
    size_t a = strlen(w->oidc_pubkey_pem), b = strlen(fetched_pem);
    char *set = malloc(a + 1 + b + 1);
    if (!set) return;
    memcpy(set, w->oidc_pubkey_pem, a);
    if (a) set[a++] = '\n';
    memcpy(set + a, fetched_pem, b + 1);
    pthread_mutex_lock(&w->keys_mu);
    char *old = w->oidc_keyset_pem;
    w->oidc_keyset_pem = set;
    pthread_mutex_unlock(&w->keys_mu);
    free(old);
}

void oc_dbwriter_set_local_enabled(oc_dbwriter *w, int on) {
    if (on) w->auth_methods |= OC_AUTH_LOCAL;
    else    w->auth_methods &= (uint8_t)~OC_AUTH_LOCAL;
}

const char *oc_dbwriter_oidc_audience(oc_dbwriter *w) {
    return w->oidc_audience ? w->oidc_audience : "";
}

/* Registered-user cap (AUTH.md §2, OPENCHIME_MANAGED_MAX_USERS). <=0 means unlimited. Set before
 * serving; read only on the writer thread. */
void oc_dbwriter_set_max_users(oc_dbwriter *w, int max_users) {
    w->max_users = max_users > 0 ? max_users : 0;
}

/* Argon2id memory (KiB) for keys the writer makes and the unknown-name check;
 * tests only, which hash with little. Set before serving. */
void oc_dbwriter_set_pw_mem(oc_dbwriter *w, uint32_t mem_kib) {
    w->pw_mem = mem_kib ? mem_kib : OC_PW_MEM_KIB;
}

uint8_t oc_dbwriter_auth_methods(oc_dbwriter *w) { return w->auth_methods; }

int oc_dbwriter_local_browser(oc_dbwriter *w) {
    return (w->auth_methods & OC_AUTH_LOCAL) && w->local_iss != NULL;
}


int oc_dbwriter_configure_join_rules(oc_dbwriter *w, const char *spec,
                                     char *err, size_t errcap) {
    oc_join_rules *rules = oc_join_rules_parse(spec, err, errcap);
    if (!rules) return -1;
    oc_join_rules_free(w->join_rules);
    w->join_rules = rules;
    return 0;
}

void oc_dbwriter_set_email_link(oc_dbwriter *w, int any) { w->email_link_any = any != 0; }
void oc_dbwriter_set_local_mfa(oc_dbwriter *w, int policy) { w->local_mfa = policy; }
void oc_dbwriter_set_session_policy(oc_dbwriter *w, uint64_t ttl_ms, uint64_t idle_ms) {
    (void)w;
    __atomic_store_n(&g_session_ttl_ms, ttl_ms ? ttl_ms : OC_SESSION_TTL_MS, __ATOMIC_RELAXED);
    __atomic_store_n(&g_session_idle_ms, idle_ms, __ATOMIC_RELAXED);
}
int  oc_dbwriter_local_mfa(oc_dbwriter *w) { return w->local_mfa; }
void oc_dbwriter_set_direct_issuers(oc_dbwriter *w, const char *const *issuers, int n) {
    w->n_direct_iss = 0;
    for (int i = 0; i < n && i < 4; i++)
        snprintf(w->direct_iss[w->n_direct_iss++], sizeof w->direct_iss[0], "%s", issuers[i]);
}

void oc_dbwriter_set_factor_key(oc_dbwriter *w, const uint8_t *key32) {
    w->have_factor_key = key32 != NULL;
    if (key32) memcpy(w->factor_key, key32, OC_FACTOR_KEY_LEN);
    else memset(w->factor_key, 0, sizeof w->factor_key);
}

const char *oc_dbwriter_relay_origin(oc_dbwriter *w) {
    return w->relay_origin ? w->relay_origin : "";
}

void oc_dbwriter_set_restore_window_ms(oc_dbwriter *w, uint64_t ms) {
    pthread_mutex_lock(&w->mu);
    w->restore_window_ms = ms;
    pthread_mutex_unlock(&w->mu);
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
/* The blocking helpers' next result, past the notices a job posted beside
 * its answer (OC_RES_NOTICE), which only a live net loop has members to send
 * to. */
static oc_dbres *sync_next(oc_dbwriter *w) {
    for (;;) {
        oc_dbres *r = oc_dbwriter_next_result(w);
        if (!r || (r->type != OC_RES_NOTICE && r->type != OC_RES_ROSTER_ROW)) return r;
        oc_dbres_free(r);
    }
}

uint64_t oc_dbwriter_register_local(oc_dbwriter *w, const char *username,
                                    const char *password, uint8_t role,
                                    uint32_t mem_kib) {
    oc_job *j = oc_job_new(OC_JOB_REGISTER, 0);
    if (!j || oc_job_set_register(j, username, password, role, mem_kib) != 0) {
        job_free(j);
        return 0;
    }
    oc_dbwriter_submit(w, j);
    for (int i = 0; i < 3000; i++) {
        oc_dbres *r = sync_next(w);
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
        oc_dbres *r = sync_next(w);
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
        oc_dbres *r = sync_next(w);
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
        oc_dbres *r = sync_next(w);
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
        oc_dbres *r = sync_next(w);
        if (r) {
            int ok = (r->type == OC_RES_OK);
            oc_dbres_free(r);
            return ok;
        }
        usleep(1000);
    }
    return 0;
}

/* Load the persisted enrollment identity (ARCH-84). Returns 1 (+ heap
 * privkey/audience/active) if a row exists, else 0. Setup-time only. */
int oc_dbwriter_load_enrollment(oc_dbwriter *w, char **privkey_out, char **audience_out, int *active_out) {
    *privkey_out = NULL; *audience_out = NULL; *active_out = 0;
    oc_job *j = oc_job_new(OC_JOB_LOAD_ENROLLMENT, 0);
    if (!j) return 0;
    oc_dbwriter_submit(w, j);
    for (int i = 0; i < 3000; i++) {
        oc_dbres *r = sync_next(w);
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

/* Persist the enrollment keypair + audience + state (ARCH-84). Returns 1 on success.
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
        oc_dbres *r = sync_next(w);
        if (r) {
            int ok = (r->type == OC_RES_OK);
            oc_dbres_free(r);
            return ok;
        }
        usleep(1000);
    }
    return 0;
}

int oc_dbwriter_load_tls_state(oc_dbwriter *w, oc_tls_state *out) {
    memset(out, 0, sizeof *out);
    oc_job *j = oc_job_new(OC_JOB_LOAD_TLS_STATE, 0);
    if (!j) return 0;
    oc_dbwriter_submit(w, j);
    for (int i = 0; i < 3000; i++) {
        oc_dbres *r = sync_next(w);
        if (r) {
            int ok = r->type == OC_RES_TLS_STATE && r->tls_state;
            if (ok) { *out = *r->tls_state; memset(r->tls_state, 0, sizeof *r->tls_state); }
            oc_dbres_free(r);
            return ok;
        }
        usleep(1000);
    }
    return 0;
}

static char *dup_or_null(const char *s) { return s ? strdup(s) : NULL; }

void oc_dbwriter_store_acme_account(oc_dbwriter *w, const char *directory, const char *key_pem, const char *url) {
    oc_job *j = oc_job_new(OC_JOB_STORE_ACME_ACCOUNT, 0);
    if (!j) return;
    j->tls_state = calloc(1, sizeof *j->tls_state);
    if (!j->tls_state) { job_free(j); return; }
    j->tls_state->acme_directory = dup_or_null(directory);
    j->tls_state->acme_key_pem = dup_or_null(key_pem);
    j->tls_state->acme_url = dup_or_null(url);
    oc_dbwriter_submit(w, j);
}

void oc_dbwriter_store_tls_cert(oc_dbwriter *w, const char *source, const char *names, const char *chain_pem,
                                const char *key_pem, uint64_t issued_ms, uint64_t not_after_ms) {
    oc_job *j = oc_job_new(OC_JOB_STORE_TLS_CERT, 0);
    if (!j) return;
    j->tls_state = calloc(1, sizeof *j->tls_state);
    if (!j->tls_state) { job_free(j); return; }
    j->tls_state->source = dup_or_null(source);
    j->tls_state->names = dup_or_null(names);
    j->tls_state->chain_pem = dup_or_null(chain_pem);
    j->tls_state->key_pem = dup_or_null(key_pem);
    j->tls_state->issued_ms = issued_ms;
    j->tls_state->not_after_ms = not_after_ms;
    oc_dbwriter_submit(w, j);
}

void oc_dbwriter_alert(oc_dbwriter *w, const char *key, const char *message) {
    oc_job *j = oc_job_new(OC_JOB_ALERT_RAISE, 0);
    if (!j) return;
    j->alert_key = dup_or_null(key); j->alert_message = dup_or_null(message);
    oc_dbwriter_submit(w, j);
}

void oc_dbwriter_alert_clear(oc_dbwriter *w, const char *key) {
    oc_job *j = oc_job_new(OC_JOB_ALERT_CLEAR, 0);
    if (!j) return;
    j->alert_key = dup_or_null(key);
    oc_dbwriter_submit(w, j);
}

void oc_dbwriter_note_enrollment_active(oc_dbwriter *w, const char *privkey_pem, const char *audience) {
    if (!privkey_pem || !audience) return;
    oc_job *j = oc_job_new(OC_JOB_STORE_ENROLLMENT, 0);
    if (!j) return;
    j->enroll_privkey  = strdup(privkey_pem);
    j->enroll_audience = strdup(audience);
    j->enroll_active   = 1;
    j->enroll_quiet    = 1;
    if (!j->enroll_privkey || !j->enroll_audience) { job_free(j); return; }
    oc_dbwriter_submit(w, j);
}

int oc_dbwriter_welcome_general(oc_dbwriter *w, int deployment_mode, const char *workspace_name) {
    if (deployment_mode != OC_DEPLOY_MANAGED) return 0;   /* self-hosted: #general as ever */
    oc_job *j = oc_job_new(OC_JOB_WELCOME_GENERAL, 0);
    if (!j) return 0;
    j->ch_name = strdup(workspace_name ? workspace_name : "");
    if (!j->ch_name) { job_free(j); return 0; }
    oc_dbwriter_submit(w, j);
    for (int i = 0; i < 3000; i++) {
        oc_dbres *r = sync_next(w);
        if (r) {
            int wrote = r->type == OC_RES_OK && r->channel_id == OC_DEFAULT_CHANNEL;
            oc_dbres_free(r);
            return wrote;
        }
        usleep(1000);
    }
    return 0;
}

void oc_dbwriter_set_demo(oc_dbwriter *w, int on) { (void)w; g_dbw_demo = on != 0; }

int oc_dbwriter_seed_demo(oc_dbwriter *w, const char *workspace_name) {
    oc_job *j = oc_job_new(OC_JOB_SEED_DEMO, 0);
    if (!j) return 0;
    j->ch_name = strdup(workspace_name ? workspace_name : "");
    if (!j->ch_name) { job_free(j); return 0; }
    oc_dbwriter_submit(w, j);
    for (int i = 0; i < 3000; i++) {
        oc_dbres *r = sync_next(w);
        if (r) {
            int wrote = r->type == OC_RES_OK && r->channel_id == OC_DEFAULT_CHANNEL;
            oc_dbres_free(r);
            return wrote;
        }
        usleep(1000);
    }
    return 0;
}

/* --- Lifecycle ---------------------------------------------------------- */

/* This daemon's issuer for its local accounts (AUTH.md §8.10): the one kept, or
 * a new one, made and kept now. Setup, before any thread runs. */
/* The factor key (AUTH.md §8.6), kept in the identity (migration 0064). Its
 * row; else the file it was kept in before, taken in once; else a new one --
 * unless second steps are sealed here, which a new key could never open: then
 * none, and the reason. 0 kept, 2 taken in, 1 made, -1 none. Runs before the
 * threads start, on the writer's connection. */
static int factor_key_settle(oc_dbwriter *w, const char *file, char *why, size_t cap) {
    sqlite3_stmt *st = NULL;
    int got = 0;
    if (sqlite3_prepare_v2(w->db, "SELECT key FROM ident.factor_key WHERE id=1;", -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW && sqlite3_column_bytes(st, 0) == OC_FACTOR_KEY_LEN) {
        memcpy(w->factor_key, sqlite3_column_blob(st, 0), OC_FACTOR_KEY_LEN);
        got = 1;
    }
    sqlite3_finalize(st);
    if (got) return 0;

    int made = 0;
    char ferr[256] = "";
    if (access(file, F_OK) == 0) {
        if (oc_factor_key_load(file, w->factor_key, ferr, sizeof ferr) != 0) {
            snprintf(why, cap, "%s", ferr);
            return -1;
        }
    } else {
        int sealed = 0;
        if (sqlite3_prepare_v2(w->db, "SELECT 1 FROM local_totp LIMIT 1;", -1, &st, NULL) == SQLITE_OK)
            sealed = sqlite3_step(st) == SQLITE_ROW;
        sqlite3_finalize(st);
        if (sealed) {
            snprintf(why, cap, "it is missing, and second steps sealed under it are kept: a new key could not "
                               "open them -- restore the identity it was kept in");
            return -1;
        }
        if (oc_rand_bytes(w->factor_key, OC_FACTOR_KEY_LEN) != 0) {
            snprintf(why, cap, "no random bytes for a new one");
            return -1;
        }
        made = 1;
    }
    int ok = sqlite3_prepare_v2(w->db, "INSERT INTO ident.factor_key(id, key, created_at_ms) VALUES (1,?,?);",
                                -1, &st, NULL) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_blob(st, 1, w->factor_key, OC_FACTOR_KEY_LEN, SQLITE_STATIC);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)dbw_now_ms());
        ok = sqlite3_step(st) == SQLITE_DONE;
    }
    sqlite3_finalize(st);
    if (!ok) {
        memset(w->factor_key, 0, sizeof w->factor_key);
        snprintf(why, cap, "it could not be kept: %s", sqlite3_errmsg(w->db));
        return -1;
    }
    /* Taken in: the identity is where it is kept now, and a copy left beside
     * the database would be one more place to lose it from. */
    if (!made) unlink(file);
    return made ? 1 : 2;
}

const char *oc_dbwriter_local_issuer_name(oc_dbwriter *w) {
    return w && w->local_iss ? oc_local_issuer_name(w->local_iss) : NULL;
}

static int local_issuer_load(oc_dbwriter *w) {
    char *key = NULL, *iss = NULL;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(w->db, "SELECT key_pem, issuer FROM ident.local_issuer WHERE id=1;", -1, &st, NULL) != SQLITE_OK)
        return -1;
    if (sqlite3_step(st) == SQLITE_ROW) {
        key = strdup((const char *)sqlite3_column_text(st, 0));
        iss = strdup((const char *)sqlite3_column_text(st, 1));
    }
    sqlite3_finalize(st);
    if (!key || !iss) {
        free(key); free(iss);
        if (oc_local_issuer_generate(&key, &iss) != 0) return -1;
        sqlite3_prepare_v2(w->db, "INSERT INTO ident.local_issuer(id, key_pem, issuer, created_at_ms) VALUES (1,?,?,?);",
                           -1, &st, NULL);
        sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, iss, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 3, (sqlite3_int64)dbw_now_ms());
        int rc = sqlite3_step(st);
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE) { free(key); free(iss); return -1; }
    }
    w->local_iss = oc_local_issuer_open(key, iss);
    oc_e2e_wipe(key, strlen(key));
    free(key); free(iss);
    if (!w->seen_jti) w->seen_jti = calloc(1, sizeof *w->seen_jti);
    return w->local_iss && w->seen_jti ? 0 : -1;
}

oc_dbwriter *oc_dbwriter_start(const char *path) {
    return oc_dbwriter_start_boot(path, NULL);
}

oc_dbwriter *oc_dbwriter_start_boot(const char *path, oc_dbwriter_boot *b) {
    const char *identity_path = b ? b->identity_path : NULL;
    if (b) { b->too_new = 0; b->factor = -1; b->factor_why[0] = '\0'; }
    oc_dbwriter *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    pthread_mutex_init(&w->keys_mu, NULL);
    w->evfd = -1;
    w->auth_methods = OC_AUTH_LOCAL | OC_AUTH_SESSION;  /* local mode by default */
    w->idem_retention_ms = OC_IDEM_RETENTION_MS;
    w->restore_window_ms = OC_RESTORE_WINDOW_MS;
    w->pw_mem = OC_PW_MEM_KIB;
    w->prune_interval_ms = OC_PRUNE_INTERVAL_MS;
    w->auth_rl   = oc_ratelimit_new(OC_AUTH_MAX_FAILURES, OC_AUTH_WINDOW_MS, OC_AUTH_RL_CAPACITY);
    w->source_rl = oc_ratelimit_new(OC_AUTH_SOURCE_MAX_FAILURES, OC_AUTH_WINDOW_MS, OC_AUTH_RL_CAPACITY);
    w->signup_rl = oc_ratelimit_new(OC_SIGNUP_SOURCE_MAX, OC_SIGNUP_WINDOW_MS, OC_AUTH_RL_CAPACITY);
    w->step_rl   = oc_ratelimit_new(OC_STEP_MAX_FAILURES, OC_STEP_WINDOW_MS, OC_AUTH_RL_CAPACITY);
    w->local_mfa = OC_MFA_OPTIONAL;
    if (!w->auth_rl || !w->source_rl || !w->step_rl) {
        oc_ratelimit_free(w->auth_rl); oc_ratelimit_free(w->source_rl); oc_ratelimit_free(w->step_rl);
        pthread_mutex_destroy(&w->keys_mu);
        free(w); return NULL;
    }

    if (sqlite3_open(path, &w->db) != SQLITE_OK) {
        fprintf(stderr, "dbwriter: open %s failed: %s\n", path, sqlite3_errmsg(w->db));
        goto fail;
    }
    /* A replicator beside the daemon (the image's Litestream, ARCH-3) takes the
     * write lock for a moment at each checkpoint: the writer waits it out, as
     * the readers do, rather than failing a commit with SQLITE_BUSY. */
    sqlite3_busy_timeout(w->db, 5000);
    sql_functions(w->db);
    char *err = NULL;
    if (sqlite3_exec(w->db, "PRAGMA journal_mode=WAL; PRAGMA foreign_keys=ON;",
                     NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "dbwriter: pragma failed: %s\n", err ? err : "?");
        sqlite3_free(err); goto fail;
    }
    /* The box's identity (ARCH-66b): its own file beside the database, attached
     * before migrating so 0064 can move it there, and in WAL so the replicator
     * streams it as it does the database. Without a path, one in memory. */
    if (oc_migrate_attach_identity(w->db, identity_path, &err) != SQLITE_OK ||
        (identity_path && *identity_path &&
         sqlite3_exec(w->db, "PRAGMA ident.journal_mode=WAL;", NULL, NULL, &err) != SQLITE_OK)) {
        fprintf(stderr, "dbwriter: the identity %s could not be opened: %s\n",
                identity_path && *identity_path ? identity_path : "(memory)", err ? err : "?");
        sqlite3_free(err); goto fail;
    }
    int schema_before = oc_schema_version(w->db);
    int mrc = oc_migrate_default(w->db, &err);
    if (mrc == OC_MIGRATE_TOO_NEW) {
        fprintf(stderr, "dbwriter: %s was written by a newer daemon (schema %d; this one knows up to %d): "
                "refusing to run on it\n", path, schema_before, OC_MIGRATIONS[OC_MIGRATIONS_COUNT - 1].version);
        if (b) b->too_new = 1;
        goto fail;
    }
    if (mrc != SQLITE_OK) {
        fprintf(stderr, "dbwriter: migration failed: %s\n", err ? err : "?");
        sqlite3_free(err); goto fail;
    }
    if (schema_before < 60) index_mentions(w->db);
    join_default_channels(w->db, 0, 1);   /* a default channel that is not one, said once */
    notice_report(w->db);
    receipts_start(w->db);
    /* An encrypted DM's keys were on its two devices, joined through the daemon
     * that stopped: none still open can go on, so each is closed now, and the
     * maintenance pass removes it a day later (PRIVATE-DM.md). */
    {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(w->db, "UPDATE channels SET archived_at_ms=?1 WHERE e2e=1 AND archived_at_ms IS NULL;",
                               -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)dbw_now_ms());
            sqlite3_step(st);
        }
        sqlite3_finalize(st);
    }
    /* The summary queue outlives the daemon, its connections do not: every
     * request waits again, watched by nobody, and is made and told as a notice
     * (migration 0059). */
    sqlite3_exec(w->db, "UPDATE summary_requests SET state='queued', conn_id=0, req_id=0;", NULL, NULL, NULL);
    /* A call open when the daemon stopped ended then (CALLS.md §1). */
    calls_close_stopped(w->db);
    retention_audit(w->db, oc_config_get()->retention_days);
    if (local_issuer_load(w) != 0) {
        fprintf(stderr, "dbwriter: the local issuer could not be made\n");
        goto fail;
    }
    if (b && b->factor_key_file) {
        b->factor = factor_key_settle(w, b->factor_key_file, b->factor_why, sizeof b->factor_why);
        if (b->factor >= 0) w->have_factor_key = 1;
    }
    w->evfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (w->evfd < 0) { fprintf(stderr, "dbwriter: eventfd failed\n"); goto fail; }

    /* Read-only connections for query jobs (ARCH-66): the same WAL file, opened
     * query_only so they can never write. WAL lets them read concurrently with
     * the writer, and with each other, without blocking it. */
    for (int i = 0; i < OC_DB_READERS; i++) {
        db_reader *rd = &w->readers[i];
        rd->w = w;
        if (sqlite3_open(path, &rd->rdb) != SQLITE_OK) {
            fprintf(stderr, "dbwriter: open read conn failed: %s\n", sqlite3_errmsg(rd->rdb));
            goto fail;
        }
        sqlite3_busy_timeout(rd->rdb, 5000);
        sqlite3_exec(rd->rdb, "PRAGMA query_only=1;", NULL, NULL, NULL);
        sql_functions(rd->rdb);
    }

    pthread_mutex_init(&w->mu, NULL);
    pthread_cond_init(&w->cv, NULL);
    for (int i = 0; i < OC_DB_READERS; i++) pthread_cond_init(&w->readers[i].cv, NULL);
    if (pthread_create(&w->thread, NULL, writer_loop, w) != 0) {
        fprintf(stderr, "dbwriter: writer thread create failed\n");
        goto fail_threads;
    }
    w->started = 1;
    for (int i = 0; i < OC_DB_READERS; i++) {
        if (pthread_create(&w->readers[i].th, NULL, reader_loop, &w->readers[i]) != 0) {
            fprintf(stderr, "dbwriter: reader thread create failed\n");
            goto fail_threads;
        }
        w->readers[i].started = 1;
    }
    if (!(w->auth = oc_authpool_start(OC_AUTH_THREADS, auth_checked, w))) {
        fprintf(stderr, "dbwriter: auth pool start failed\n");
        goto fail_threads;
    }
    return w;

fail_threads:
    pthread_mutex_lock(&w->mu);
    w->stop = 1;
    pthread_cond_signal(&w->cv);
    for (int i = 0; i < OC_DB_READERS; i++) pthread_cond_signal(&w->readers[i].cv);
    pthread_mutex_unlock(&w->mu);
    if (w->started) pthread_join(w->thread, NULL);
    for (int i = 0; i < OC_DB_READERS; i++) if (w->readers[i].started) pthread_join(w->readers[i].th, NULL);
    pthread_mutex_destroy(&w->mu);
    pthread_cond_destroy(&w->cv);
    for (int i = 0; i < OC_DB_READERS; i++) pthread_cond_destroy(&w->readers[i].cv);
fail:
    if (w->evfd >= 0) close(w->evfd);
    for (int i = 0; i < OC_DB_READERS; i++) sqlite3_close(w->readers[i].rdb);
    sqlite3_close(w->db);
    oc_local_issuer_close(w->local_iss);
    free(w->seen_jti);
    pthread_mutex_destroy(&w->keys_mu);
    free(w);
    return NULL;
}

void oc_dbwriter_stop(oc_dbwriter *w) {
    if (!w) return;
    if (w->started) {
        pthread_mutex_lock(&w->mu);
        w->stop = 1;
        pthread_cond_signal(&w->cv);
        for (int i = 0; i < OC_DB_READERS; i++) pthread_cond_signal(&w->readers[i].cv);
        pthread_mutex_unlock(&w->mu);
        /* Readers first, since one may hand the pool a check; then the pool,
         * whose finished checks go to the writer's queue; then the writer. What
         * reaches the queue after the writer has gone is freed below. */
        for (int i = 0; i < OC_DB_READERS; i++) if (w->readers[i].started) pthread_join(w->readers[i].th, NULL);
        oc_authpool_stop(w->auth);
        w->auth = NULL;
        pthread_join(w->thread, NULL);
        pthread_mutex_destroy(&w->mu);
        pthread_cond_destroy(&w->cv);
        for (int i = 0; i < OC_DB_READERS; i++) pthread_cond_destroy(&w->readers[i].cv);
    }
    for (oc_job *j = w->jobs_head; j; ) { oc_job *n = j->next; job_free(j); j = n; }
    for (int i = 0; i < OC_DB_READERS; i++)
        for (oc_job *j = w->readers[i].head; j; ) { oc_job *n = j->next; job_free(j); j = n; }
    for (oc_dbres *r = w->res_head; r; ) { oc_dbres *n = r->next; oc_dbres_free(r); r = n; }
    free(w->oidc_issuer); free(w->oidc_audience);
    free(w->oidc_pubkey_pem); free(w->oidc_keyset_pem); free(w->relay_origin);
    free(w->seen_jti);
    oc_local_issuer_close(w->local_iss);
    oc_join_rules_free(w->join_rules);
    oc_ratelimit_free(w->auth_rl);
    oc_ratelimit_free(w->source_rl);
    oc_ratelimit_free(w->signup_rl);
    oc_ratelimit_free(w->step_rl);
    oc_e2e_wipe(w->factor_key, sizeof w->factor_key);
    if (w->evfd >= 0) close(w->evfd);
    for (int i = 0; i < OC_DB_READERS; i++) sqlite3_close(w->readers[i].rdb);
    sqlite3_close(w->db);
    pthread_mutex_destroy(&w->keys_mu);
    free(w);
}

int oc_db_blob_referenced(void *db_path, const char *const *keys, size_t n, uint8_t *ref) {
    sqlite3 *db = NULL;
    if (!db_path || sqlite3_open_v2((const char *)db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    sqlite3_busy_timeout(db, 5000);
    sqlite3_stmt *st = NULL;
    int rc = -1;
    if (sqlite3_prepare_v2(db,
            "SELECT EXISTS(SELECT 1 FROM attachments WHERE storage_key=?1)"
            "    OR EXISTS(SELECT 1 FROM rendered_audio WHERE blob_key=?1)"
            "    OR EXISTS(SELECT 1 FROM exported_video WHERE blob_key=?1)"
            "    OR EXISTS(SELECT 1 FROM attachment_previews WHERE storage_key=?1);",
            -1, &st, NULL) == SQLITE_OK) {
        rc = 0;
        for (size_t i = 0; i < n && rc == 0; i++) {
            sqlite3_reset(st);
            sqlite3_bind_text(st, 1, keys[i], -1, SQLITE_STATIC);
            if (sqlite3_step(st) == SQLITE_ROW) ref[i] = (uint8_t)(sqlite3_column_int(st, 0) != 0);
            else rc = -1;
        }
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return rc;
}
