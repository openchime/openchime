/*
 * OpenChime DB-writer thread + write-job queue. See dbwriter.h and migrate.h.
 *
 * One thread owns the write connection. It pops jobs off a request queue,
 * performs all DB work (local/session AUTH + account REGISTER, SEND persist with
 * idempotency, backfill), and pushes results onto a completion queue, signalling
 * the net thread via an eventfd.
 */

#include "mention.h"      /* the shared @mention scanner (ARCH-89) */
#include "dbwriter.h"
#include "unfurl.h"   /* OC_UNFURL_MAX_URLS: the store re-validates presence */
#include "url.h"
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
#include <ctype.h>
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

static uint64_t snooze_until(sqlite3 *db, uint64_t uid);   /* fwd — auth and the prefs snapshot both read it */
static void fill_schedule(sqlite3 *db, uint64_t uid, oc_dbres *r);      /* fwd — REQ-136 */
static void fill_alert_prefs(sqlite3 *db, uint64_t uid, oc_dbres *r);   /* fwd — REQ-135 */
static void build_schedule(sqlite3 *db, uint64_t uid, oc_dbres *r);     /* fwd — REQ-136 */

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
    free(j->sq_from);
    free(j->sq_in);
    free(j->username);
    free(j->password);
    free(j->body);
    free(j->cursors);
    free(j->ch_name);
    free(j->pf_full_name);
    free(j->pf_title);
    free(j->pf_pronouns);
    free(j->pf_phone);
    free(j->pf_timezone);
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
    free(j->recipients);
    free(j->pf_name);
    free(j->pf_old_pw);
    free(j->pf_new_pw);
    free(j->device_token);
    free(j->unf_url);
    free(j->unf_title);
    free(j->unf_descr);
    free(j);
}

/* Free the heap metadata of an attachment list (filename/mime per entry). */
static void free_attach_meta(oc_attach_meta *a, size_t n) {
    for (size_t i = 0; i < n; i++) { free(a[i].filename); free(a[i].mime); }
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
                "FROM reactions WHERE message_id=?1 GROUP BY emoji ORDER BY emoji;",
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
            "SELECT src_channel, src_message, src_author, excerpt, n_attach, attach_name "
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

void oc_dbres_free(oc_dbres *r) {
    if (!r) return;
    free(r->body);
    /* 53's profile strings, alongside every other heap field. */
    free(r->st_emoji); free(r->st_text); free(r->pf_title); free(r->pf_tz);
    free(r->pf_full_name); free(r->pf_pronouns); free(r->pf_phone);
    free(r->fchans);
    for (size_t i = 0; i < r->n_sessions; i++) free((void *)r->sessions[i].device_label.ptr);
    free(r->sessions);
    free(r->members);
    free(r->author_name);
    free_attach_meta(r->attach, r->n_attach);
    for (size_t i = 0; i < r->n_replay; i++) { free(r->replay[i].body); free(r->replay[i].author_name); free_attach_meta(r->replay[i].attach, r->replay[i].n_attach); }
    free(r->replay);
    for (size_t i = 0; i < r->n_rreact; i++) free(r->rreact[i].emoji);
    free(r->rreact);
    for (size_t i = 0; i < r->n_runfurl; i++) {
        free(r->runfurl[i].url); free(r->runfurl[i].title); free(r->runfurl[i].descr);
    }
    free(r->runfurl);
    for (size_t i = 0; i < r->n_rfwd; i++) { free(r->rfwd[i].excerpt); free(r->rfwd[i].attach_name); }
    free(r->rfwd);
    free(r->unf_url);
    free(r->unf_title);
    free(r->unf_descr);
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
    for (size_t i = 0; i < r->n_chlist; i++) { free(r->chlist[i].name); free(r->chlist[i].topic); free(r->chlist[i].preview); }
    free(r->chlist);
    for (size_t i = 0; i < r->n_ulist; i++) {
        free(r->ulist[i].email); free(r->ulist[i].display_name);
        free(r->ulist[i].title); free(r->ulist[i].timezone);
        free(r->ulist[i].status_emoji); free(r->ulist[i].status_text);
        free(r->ulist[i].full_name); free(r->ulist[i].pronouns);
    }
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
/* `out_session_id` (optional) reports the row created, so a SESSION_LIST can mark
 * which entry is the connection asking (REQ-182). */
static int mint_session(sqlite3 *db, uint64_t user_id,
                        uint8_t token_out[OC_SESSION_TOKEN_LEN], uint64_t *expiry_out,
                        uint64_t *out_session_id) {
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
    if (rc == SQLITE_DONE && out_session_id)
        *out_session_id = (uint64_t)sqlite3_last_insert_rowid(db);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) return -1;

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
        "SELECT s.user_id, s.expires_at_ms, u.role, s.id FROM sessions s "
        "JOIN users u ON u.id = s.user_id WHERE s.token_hash = ?;", -1, &st, NULL);
    sqlite3_bind_blob(st, 1, hash, sizeof hash, SQLITE_STATIC);
    uint64_t uid = 0, exp = 0; uint8_t role = OC_ROLE_MEMBER;
    if (sqlite3_step(st) == SQLITE_ROW) {
        uid  = (uint64_t)sqlite3_column_int64(st, 0);
        exp  = (uint64_t)sqlite3_column_int64(st, 1);
        role = role_to_u8((const char *)sqlite3_column_text(st, 2));
        /* The session's own id, so SESSION_LIST can mark which row is this device
         * (REQ-182). Reported through the out-param below rather than a global. */
        if (out_session_id) *out_session_id = (uint64_t)sqlite3_column_int64(st, 3);
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

    uint64_t uid = 0, sess_exp = 0, sess_id = 0;
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
        uid = lookup_session(db, (const uint8_t *)j->token, j->token_len, &role, &sess_exp,
                             &sess_id);
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
    r->session_id = sess_id;   /* REQ-182: which row this connection is using */
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
        "SELECT id, role, disabled, COALESCE(email,''), COALESCE(display_name,''), "
        "       COALESCE(avatar_attachment_id,0), COALESCE(title,''), COALESCE(timezone,''), "
        "       COALESCE(status_emoji,''), COALESCE(status_text,''), status_expires_ms, "
        "       COALESCE(full_name,''), COALESCE(pronouns,'') "
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

    uint8_t token[OC_SESSION_TOKEN_LEN]; uint64_t sexp = 0, sid = 0;
    if (mint_session(db, uid, token, &sexp, &sid) != 0) {
        r->type = OC_RES_AUTH_ERR; r->err_code = OC_ERR_INTERNAL; return r;
    }
    r->type = OC_RES_AUTH_OK;
    r->session_id = sid;
    r->user_id = uid;
    r->role = role;
    r->snooze_until_ms = snooze_until(db, uid);
    fill_schedule(db, uid, r);              /* REQ-136, as above */
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
        "DELETE FROM pins WHERE channel_id IN (SELECT channel_id FROM channel_members "
        "  WHERE user_id=?1 AND channel_id IN (SELECT id FROM channels WHERE kind='dm'));",
        -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->target_user_id); sqlite3_step(st); sqlite3_finalize(st);
    sqlite3_prepare_v2(db,
        "DELETE FROM mentions WHERE channel_id IN (SELECT channel_id FROM channel_members "
        "  WHERE user_id=?1 AND channel_id IN (SELECT id FROM channels WHERE kind='dm'));",
        -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->target_user_id); sqlite3_step(st); sqlite3_finalize(st);
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
    /* Their drafts go with them (REQ-223, ARCH-101) — user content, and the
     * account is gone. The second sweep catches OTHER people's drafts for the
     * DM channels just deleted above: those channels no longer exist, so a row
     * pointing at one is unreachable and would sit there forever. There is no
     * channel-delete op in the product, so this is the only path by which a
     * channel dies and the only place that sweep can live. */
    sqlite3_prepare_v2(db, "DELETE FROM drafts WHERE user_id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->target_user_id); sqlite3_step(st); sqlite3_finalize(st);
    /* And anything they had scheduled (REQ-224): the account is gone, so the
     * promise cannot be kept and there is nobody left to tell. */
    sqlite3_prepare_v2(db, "DELETE FROM scheduled_messages WHERE user_id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->target_user_id); sqlite3_step(st); sqlite3_finalize(st);
    sqlite3_prepare_v2(db,
        "DELETE FROM scheduled_messages WHERE channel_id NOT IN (SELECT id FROM channels);",
        -1, &st, NULL);
    sqlite3_step(st); sqlite3_finalize(st);
    sqlite3_prepare_v2(db,
        "DELETE FROM drafts WHERE channel_id NOT IN (SELECT id FROM channels);", -1, &st, NULL);
    sqlite3_step(st); sqlite3_finalize(st);
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
static void store_keyword_hits(sqlite3 *db, uint64_t mid, uint64_t channel_id,
                               uint64_t author_id, const void *body, size_t body_len,
                               uint64_t ts) {
    if (!body || !body_len) return;
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
        if (!term || !oc_keyword_match((const char *)body, body_len,
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

static void store_mentions(sqlite3 *db, uint64_t mid, uint64_t channel_id,
                           const void *body, size_t body_len, uint64_t ts,
                           oc_mention_unresolved *unres) {
    if (!body || !body_len) return;
    oc_mention m[OC_MENTION_MAX];
    size_t n = oc_mention_scan((const char *)body, body_len, m, OC_MENTION_MAX);
    if (n > OC_MENTION_MAX) n = OC_MENTION_MAX;   /* the rest are ignored, not an error */

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

    for (size_t i = 0; i < n; i++) {
        int64_t uid = 0;
        if (m[i].kind == OC_MENTION_USER) {
            /* Match the display name case-insensitively among this channel's
             * members. Membership is asked first regardless of channel kind,
             * because "are they in it" is also what REQ-287 reports. */
            sqlite3_stmt *q = NULL;
            if (sqlite3_prepare_v2(db,
                    "SELECT u.id FROM users u JOIN channel_members cm ON cm.user_id = u.id "
                    "WHERE cm.channel_id = ?1 AND u.disabled = 0 "
                    "  AND lower(u.display_name) = lower(?2) LIMIT 1;", -1, &q, NULL) != SQLITE_OK)
                continue;
            sqlite3_bind_int64(q, 1, (sqlite3_int64)channel_id);
            sqlite3_bind_text(q, 2, m[i].name, -1, SQLITE_STATIC);
            if (sqlite3_step(q) == SQLITE_ROW) uid = sqlite3_column_int64(q, 0);
            sqlite3_finalize(q);
            if (!uid) {
                /* Not a member — but is it a PERSON? A typo and a colleague who
                 * is simply not in this channel look identical from here, and
                 * they are completely different situations for the sender
                 * (REQ-287). Ask the tenant roster before deciding it was noise. */
                uint64_t outsider = 0;
                sqlite3_stmt *w = NULL;
                if (sqlite3_prepare_v2(db,
                        "SELECT id, display_name FROM users "
                        "WHERE disabled = 0 AND lower(display_name) = lower(?1) LIMIT 1;",
                        -1, &w, NULL) == SQLITE_OK) {
                    sqlite3_bind_text(w, 1, m[i].name, -1, SQLITE_STATIC);
                    if (sqlite3_step(w) == SQLITE_ROW) {
                        outsider = (uint64_t)sqlite3_column_int64(w, 0);
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
    if (!channel_read_access(db, j->src_channel, j->user_id)) return;

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT m.author_id, COALESCE(m.body, ''), "
            "       (SELECT COUNT(*) FROM attachments a WHERE a.message_id = m.id), "
            /* The FIRST file's name, which is what a card can show; the count
             * beside it covers the rest ("report.txt and 2 more"). */
            "       COALESCE((SELECT a.filename FROM attachments a "
            "                 WHERE a.message_id = m.id ORDER BY a.id LIMIT 1), '') "
            "FROM messages m WHERE m.id=?1 AND m.channel_id=?2 "
            "  AND m.deleted_at_ms IS NULL;", -1, &st, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->src_message);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)j->src_channel);
    uint64_t author = 0, n_attach = 0;
    char excerpt[OC_FORWARD_EXCERPT_MAX + 1];
    char aname[256] = "";
    size_t ex_len = 0;
    int found = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *b = sqlite3_column_text(st, 1);
        size_t bl = (size_t)sqlite3_column_bytes(st, 1);
        author   = (uint64_t)sqlite3_column_int64(st, 0);
        n_attach = (uint64_t)sqlite3_column_int64(st, 2);
        const char *an = (const char *)sqlite3_column_text(st, 3);
        if (an) snprintf(aname, sizeof aname, "%s", an);
        if (bl > OC_FORWARD_EXCERPT_MAX) {
            bl = OC_FORWARD_EXCERPT_MAX;
            /* Back off any trailing continuation bytes so the cut lands
             * between characters. */
            while (bl > 0 && (b[bl] & 0xC0) == 0x80) bl--;
        }
        if (bl && b) memcpy(excerpt, b, bl);
        ex_len = bl;
        found = 1;
    }
    sqlite3_finalize(st);
    if (!found) return;
    excerpt[ex_len] = 0;

    sqlite3_prepare_v2(db,
        "INSERT INTO forwards(message_id, src_channel, src_message, src_author, "
        "                     excerpt, n_attach, attach_name) "
        "VALUES(?, ?, ?, ?, ?, ?, ?);", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)mid);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)j->src_channel);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)j->src_message);
    sqlite3_bind_int64(st, 4, (sqlite3_int64)author);
    sqlite3_bind_text (st, 5, excerpt, (int)ex_len, SQLITE_STATIC);
    sqlite3_bind_int64(st, 6, (sqlite3_int64)n_attach);
    sqlite3_bind_text (st, 7, aname, -1, SQLITE_STATIC);
    sqlite3_step(st);
    sqlite3_finalize(st);
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
    /* Collected during resolution and carried back so the sender can be told
     * (REQ-287); the message itself is stored either way. */
    r->unres.channel_id = j->channel_id;
    r->unres.message_id = mid;
    store_mentions(db, mid, j->channel_id, j->body, j->body_len, ts, &r->unres);
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

    /* The draft this message came from is gone (REQ-223, ARCH-101), in the same
     * transaction as the send. Server-side rather than a client courtesy: a
     * client that dies between the send and its own cleanup would otherwise
     * leave the sent text sitting in the composer of every other device. */
    drop_draft(db, j->user_id, j->channel_id, 0);

    sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);

    r->type = OC_RES_SEND_OK;
    r->message_id = mid;
    r->server_time = ts;
    r->author_name = lookup_display_name(db, j->user_id);   /* name for the live BROADCAST */
    /* Read the reference back rather than echoing what was asked for: what the
     * channel is told is what actually landed in the table, or nothing. */
    append_forward(db, r, mid, j->channel_id);
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

    /* Any stored unfurls describe the OLD body. Drop them all; the net thread
     * re-extracts from the new body and re-fetches, and the store step
     * re-validates presence — so a URL that survived the edit gets its preview
     * back within seconds, and one that left cannot be replayed (ARCH-105). */
    sqlite3_prepare_v2(db, "DELETE FROM unfurls WHERE message_id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->message_id);
    sqlite3_step(st);
    sqlite3_finalize(st);

    r->type = OC_RES_EDIT_OK;
    r->author_id = author;
    r->server_time = ts;   /* edited_at_ms */
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
        size_t nn = body ? oc_url_extract(body, body_len, sp, OC_UNFURL_MAX_URLS) : 0;
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
    /* Detach the attachments (REQ-052). The row is not deleted and the blob is
     * not deleted here: setting message_id NULL makes them *orphans*, which is
     * exactly the state the storage-maintenance orphan sweep already collects
     * (ARCH-78) — so the bytes are reclaimed by a path that is already written
     * and tested, on its own schedule, off the writer thread. What matters for
     * correctness is that they leave the message immediately: a tombstone that
     * still lists a file is offering something it no longer has. */
    sqlite3_prepare_v2(db, "UPDATE attachments SET message_id=NULL WHERE message_id=?;",
                       -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->message_id); sqlite3_step(st); sqlite3_finalize(st);

    /* A tombstone has nothing to pin to either (REQ-052/230). Dropped before the
     * reactions so the order matches the table dependencies. */
    sqlite3_prepare_v2(db, "DELETE FROM pins WHERE message_id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->message_id); sqlite3_step(st); sqlite3_finalize(st);

    /* A tombstone has no links to preview either (REQ-052/222). */
    sqlite3_prepare_v2(db, "DELETE FROM unfurls WHERE message_id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->message_id); sqlite3_step(st); sqlite3_finalize(st);

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
        "SELECT kind, name, is_public, created_at_ms, topic, archived_at_ms "
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
        r->channel_id    = channel_id;
        found = 1;
    }
    sqlite3_finalize(st);
    if (found) r->ch_joined = (uint8_t)(is_member(db, channel_id, actor) ? 1 : 0);
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

    if (j->chup_op == OC_CHUP_TOPIC) {
        if (vlen > OC_MAX_TOPIC) {
            r->type = OC_RES_CHANNEL_ERR; r->err_code = OC_ERR_INVALID_CHANNEL; return r;
        }
        sqlite3_prepare_v2(db, "UPDATE channels SET topic=? WHERE id=?;", -1, &st, NULL);
        if (vlen) sqlite3_bind_text(st, 1, j->ch_name, (int)vlen, SQLITE_STATIC);
        else      sqlite3_bind_null(st, 1);          /* "" clears the topic */
        sqlite3_bind_int64(st, 2, (sqlite3_int64)j->channel_id);
        sqlite3_step(st); sqlite3_finalize(st);
        audit_actor(db, OC_AUDIT_ADMIN, "channel.topic", j->user_id, 0, NULL, 1, NULL);
    } else if (j->chup_op == OC_CHUP_RENAME) {
        if (!oc_role_can_moderate(role)) {
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

        sqlite3_prepare_v2(db, "UPDATE channels SET name=? WHERE id=?;", -1, &st, NULL);
        sqlite3_bind_text(st, 1, j->ch_name, (int)vlen, SQLITE_STATIC);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)j->channel_id);
        sqlite3_step(st); sqlite3_finalize(st);
        /* The id is untouched, so membership, history and cursors all follow the
         * rename for free — nothing durable was ever keyed on the name. */
        audit_actor(db, OC_AUDIT_ADMIN, "channel.rename", j->user_id, 0, j->ch_name, 1, NULL);
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
        audit_actor(db, OC_AUDIT_ADMIN, pub ? "channel.public" : "channel.private",
                    j->user_id, 0, NULL, 1, NULL);
    } else if (j->chup_op == OC_CHUP_ARCHIVE || j->chup_op == OC_CHUP_UNARCHIVE) {
        if (!oc_role_can_moderate(role)) {
            r->type = OC_RES_CHANNEL_ERR; r->err_code = OC_ERR_FORBIDDEN; return r;
        }
        int on = (j->chup_op == OC_CHUP_ARCHIVE);
        sqlite3_prepare_v2(db, "UPDATE channels SET archived_at_ms=? WHERE id=?;", -1, &st, NULL);
        if (on) sqlite3_bind_int64(st, 1, (sqlite3_int64)dbw_now_ms());
        else    sqlite3_bind_null(st, 1);
        sqlite3_bind_int64(st, 2, (sqlite3_int64)j->channel_id);
        sqlite3_step(st); sqlite3_finalize(st);
        audit_actor(db, OC_AUDIT_ADMIN, on ? "channel.archive" : "channel.unarchive",
                    j->user_id, 0, NULL, 1, NULL);
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
        "              WHERE m2.channel_id=c.id AND m2.user_id<>?1 LIMIT 1), ?1), "
        "  c.topic, c.archived_at_ms, c.created_at_ms, "
        /* The newest top-level message, for the list preview. Tombstones are
         * skipped: "(deleted)" is not a useful thing to show as the latest
         * activity, and the row below it usually is. */
        "  (SELECT substr(COALESCE(x.body,''),1,?2) FROM messages x "
        "     WHERE x.channel_id=c.id AND x.parent_id IS NULL AND x.deleted_at_ms IS NULL "
        "     ORDER BY x.id DESC LIMIT 1), "
        "  COALESCE((SELECT x.author_id FROM messages x "
        "     WHERE x.channel_id=c.id AND x.parent_id IS NULL AND x.deleted_at_ms IS NULL "
        "     ORDER BY x.id DESC LIMIT 1),0) "
        "FROM channels c WHERE "
        "  (c.kind='channel' AND (c.is_public=1 OR EXISTS(SELECT 1 FROM channel_members m WHERE m.channel_id=c.id AND m.user_id=?1))) "
        "  OR (c.kind='dm' AND EXISTS(SELECT 1 FROM channel_members m WHERE m.channel_id=c.id AND m.user_id=?1)) "
        /* An archived channel is hidden from the default list unless you are a
         * member of it (REQ-035) — hidden, not deleted: a member keeps the way
         * back in, and its history stays searchable for everyone who could read
         * it before. */
        "  AND (c.archived_at_ms IS NULL "
        "       OR EXISTS(SELECT 1 FROM channel_members m WHERE m.channel_id=c.id AND m.user_id=?1)) "
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

static void build_emoji_list(sqlite3 *db, oc_dbres *r) {
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "SELECT name, attachment_id, created_by FROM custom_emoji ORDER BY name LIMIT ?;",
        -1, &st, NULL);
    sqlite3_bind_int(st, 1, (int)OC_MAX_CUSTOM_EMOJI);
    size_t cap = 16, n = 0;
    oc_emoji_row *arr = malloc(cap * sizeof *arr);
    while (arr && sqlite3_step(st) == SQLITE_ROW) {
        if (n == cap) { cap *= 2; oc_emoji_row *g = realloc(arr, cap * sizeof *arr); if (!g) break; arr = g; }
        arr[n].name          = strdup((const char *)sqlite3_column_text(st, 0));
        arr[n].attachment_id = (uint64_t)sqlite3_column_int64(st, 1);
        arr[n].created_by    = (uint64_t)sqlite3_column_int64(st, 2);
        n++;
    }
    sqlite3_finalize(st);
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
    /* INSERT, not upsert: silently replacing an existing emoji changes what every
     * message already containing that shortcode means. Renaming is delete then add,
     * which is a decision somebody makes on purpose. */
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
    sqlite3_prepare_v2(db,
        "DELETE FROM custom_emoji WHERE name=?1 AND (?2 = 1 OR created_by=?3);",
        -1, &st, NULL);
    sqlite3_bind_text (st, 1, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int  (st, 2, role >= OC_ROLE_ADMIN ? 1 : 0);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)j->user_id);
    sqlite3_step(st);
    sqlite3_finalize(st);
    if (sqlite3_changes(db) == 0) {
        r->type = OC_RES_LIST_ERR; r->err_code = OC_ERR_FORBIDDEN; return r;
    }
    build_emoji_list(db, r);
    r->ch_fanout = 1;
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
    ids[n++] = j->user_id;
    for (uint16_t i = 0; i < j->n_group_uids && n <= (int)OC_MAX_GROUP_DM; i++) {
        uint64_t u = j->group_uids[i];
        if (!u) continue;
        int dup = 0;
        for (int k = 0; k < n; k++) if (ids[k] == u) dup = 1;
        if (dup) continue;
        uint8_t role = OC_ROLE_MEMBER;
        /* An unknown or disabled user is refused without saying which — the same
         * no-disclosure rule OPEN_DM follows. */
        if (!user_role(db, u, &role)) {
            r->type = OC_RES_CHANNEL_ERR; r->err_code = OC_ERR_FORBIDDEN; return r;
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
static oc_dbres *process_list_members(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->channel_id = j->channel_id;

    /* You may only enumerate a channel you can read, or this becomes a way to
     * discover who is in a private channel you were never invited to. */
    if (!is_member(db, j->channel_id, j->user_id)) {
        r->type = OC_RES_LIST_ERR; r->err_code = OC_ERR_NOT_A_MEMBER; return r;
    }
    r->type = OC_RES_MEMBER_LIST;

    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "SELECT cm.user_id, cm.joined_at_ms, u.role FROM channel_members cm "
        "  JOIN users u ON u.id = cm.user_id "
        " WHERE cm.channel_id=? AND u.disabled=0 "
        " ORDER BY cm.joined_at_ms LIMIT ?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->channel_id);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)OC_MAX_MEMBER_LIST);

    oc_chanmem_row *arr = calloc(OC_MAX_MEMBER_LIST, sizeof *arr);
    size_t n = 0;
    while (arr && n < OC_MAX_MEMBER_LIST && sqlite3_step(st) == SQLITE_ROW) {
        arr[n].user_id   = (uint64_t)sqlite3_column_int64(st, 0);
        arr[n].joined_at = (uint64_t)sqlite3_column_int64(st, 1);
        arr[n].role      = role_to_u8((const char *)sqlite3_column_text(st, 2));
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

    if (j->channel_id && !is_member(db, j->channel_id, j->user_id)) {
        r->type = OC_RES_LIST_ERR; r->err_code = OC_ERR_NOT_A_MEMBER; return r;
    }
    r->type = OC_RES_FILE_LIST;

    sqlite3_stmt *st = NULL;
    /* One channel reads 0023's index directly; the workspace-wide form filters
     * by membership instead, which is the same set a backfill would show. */
    const char *sql = j->channel_id
        ? "SELECT a.id, a.channel_id, a.message_id, a.uploader_id, a.size, "
          "       a.created_at_ms, a.reclaimed_at_ms, a.filename, a.mime "
          "  FROM attachments a "
          " WHERE a.channel_id=?1 AND a.message_id IS NOT NULL "
          " ORDER BY a.created_at_ms DESC LIMIT ?2;"
        : "SELECT a.id, a.channel_id, a.message_id, a.uploader_id, a.size, "
          "       a.created_at_ms, a.reclaimed_at_ms, a.filename, a.mime "
          "  FROM attachments a "
          " WHERE a.message_id IS NOT NULL AND a.channel_id IN "
          "       (SELECT channel_id FROM channel_members WHERE user_id=?1) "
          " ORDER BY a.created_at_ms DESC LIMIT ?2;";
    sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)(j->channel_id ? j->channel_id : j->user_id));
    sqlite3_bind_int64(st, 2, (sqlite3_int64)OC_MAX_FILE_LIST);

    oc_file_row *arr = calloc(OC_MAX_FILE_LIST, sizeof *arr);
    size_t n = 0;
    while (arr && n < OC_MAX_FILE_LIST && sqlite3_step(st) == SQLITE_ROW) {
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
        n++;
    }
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

/* My saved items, newest save first (REQ-231). Bodies travel with them, as with
 * pins: a saved message is usually far outside loaded history. */
static oc_dbres *process_list_saved(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->type = OC_RES_SAVED_LIST;

    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "SELECT s.message_id, m.channel_id, m.author_id, m.created_at_ms, s.created_at_ms, m.body, "
        "       (SELECT a.filename FROM attachments a WHERE a.message_id = m.id ORDER BY a.id LIMIT 1) "
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
            "       substr(COALESCE(m.body,''),1,?2) AS text "
            "  FROM messages m "
            "  JOIN channel_members cm ON cm.channel_id = m.channel_id AND cm.user_id = ?1 "
            "  JOIN channels c ON c.id = m.channel_id "
            "  LEFT JOIN delivery_cursors dc ON dc.channel_id = m.channel_id AND dc.user_id = ?1 "
            "  LEFT JOIN notification_prefs np ON np.channel_id = m.channel_id AND np.user_id = ?1 "
            " WHERE m.deleted_at_ms IS NULL "
            "   AND m.author_id <> ?1 "
            "   AND m.id > COALESCE(dc.message_id, 0) "
            "   AND COALESCE(np.muted, 0) = 0 "
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
        "SELECT kind, message_id, channel_id, actor_id, at, text FROM ("
        /* --- mentions of me --- */
        "  SELECT 0 AS kind, mn.message_id AS message_id, mn.channel_id AS channel_id, "
        "         m.author_id AS actor_id, m.created_at_ms AS at, "
        "         substr(COALESCE(m.body,''),1,?2) AS text "
        "    FROM mentions mn JOIN messages m ON m.id = mn.message_id "
        "   WHERE (mn.user_id = ?1 OR mn.kind <> 0) AND m.author_id <> ?1 "
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
        "  SELECT 1, rx.message_id, m.channel_id, rx.user_id, rx.created_at_ms, rx.emoji "
        "    FROM reactions rx JOIN messages m ON m.id = rx.message_id "
        "   WHERE m.author_id = ?1 AND rx.user_id <> ?1 AND m.deleted_at_ms IS NULL "
        "     AND EXISTS(SELECT 1 FROM channel_members cm "
        "                 WHERE cm.channel_id = m.channel_id AND cm.user_id = ?1) "
        "  UNION ALL "
        /* --- replies under something I wrote --- */
        "  SELECT 2, c.id, c.channel_id, c.author_id, c.created_at_ms, "
        "         substr(COALESCE(c.body,''),1,?2) "
        "    FROM messages c JOIN messages p ON p.id = c.parent_id "
        "   WHERE p.author_id = ?1 AND c.author_id <> ?1 AND c.deleted_at_ms IS NULL "
        "     AND EXISTS(SELECT 1 FROM channel_members cm "
        "                 WHERE cm.channel_id = c.channel_id AND cm.user_id = ?1) "
        ") ORDER BY at DESC LIMIT ?3;";

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

    if (!is_member(db, j->channel_id, j->user_id)) {
        r->type = OC_RES_PIN_ERR; r->err_code = OC_ERR_NOT_A_MEMBER; return r;
    }
    r->type = OC_RES_PINS;

    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "SELECT p.message_id, m.author_id, m.created_at_ms, "
        "       COALESCE(p.pinned_by,0), p.created_at_ms, m.body, "
        /* An attachment-only message has no body, so the list needs something
         * to show for it or the row renders blank. */
        "       (SELECT a.filename FROM attachments a WHERE a.message_id = m.id "
        "         ORDER BY a.id LIMIT 1) "
        "  FROM pins p JOIN messages m ON m.id = p.message_id "
        " WHERE p.channel_id=? AND m.deleted_at_ms IS NULL "
        " ORDER BY p.created_at_ms DESC LIMIT ?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->channel_id);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)OC_MAX_PINS);

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
    if (acc == CH_ARCHIVED){ r->type = OC_RES_REPLY_ERR; r->err_code = OC_ERR_CHANNEL_ARCHIVED; return r; }

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
    /* A reply's mentions are stored exactly as a channel message's are. They
     * were not, and nothing downstream could work without them: the push
     * MENTIONS branch, the activity feed's mention arm and the reader's
     * highlight all read this table, so naming somebody in a thread reached
     * them nowhere at all. */
    r->unres.channel_id = j->channel_id;
    r->unres.message_id = mid;
    store_mentions(db, mid, j->channel_id, j->body, j->body_len, ts, &r->unres);
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
    int have_text = (j->body && j->body_len > 0 &&
                     build_fts_query((const char *)j->body, j->body_len, fts, sizeof fts) != 0);
    /* filters alone are a valid search — "everything alice posted in #design"
     * needs no words. Only a query with NEITHER text nor filters is empty. */
    int have_filter = (j->sq_from && j->sq_from[0]) || (j->sq_in && j->sq_in[0]) ||
                      j->sq_has || j->sq_after || j->sq_before;
    if (!have_text && !have_filter) return r;

    uint16_t lim = j->search_limit;
    if (lim == 0 || lim > OC_SEARCH_MAX) lim = OC_SEARCH_MAX;

    /* Built up rather than one literal, because the FTS join must DISAPPEAR when there
     * is no text: `messages_fts MATCH ''` matches nothing, so a filters-only search
     * would silently return zero rows. */
    char sql[2048];
    int k = snprintf(sql, sizeof sql,
        "SELECT m.id, m.channel_id, m.author_id, m.created_at_ms, %s "
        "FROM %s messages m %s "
        "JOIN channels c ON c.id = m.channel_id "
        "WHERE m.deleted_at_ms IS NULL "
        "  AND (c.is_public=1 OR EXISTS(SELECT 1 FROM channel_members cm "
        "       WHERE cm.channel_id=m.channel_id AND cm.user_id=?2)) ",
        have_text ? "snippet(messages_fts, 0, '', '', ' ... ', 12)" : "substr(COALESCE(m.body,''),1,160)",
        have_text ? "messages_fts JOIN" : "",
        have_text ? "ON m.id = messages_fts.rowid" : "");
    if (have_text) k += snprintf(sql + k, sizeof sql - (size_t)k, " AND messages_fts MATCH ?1 ");
    /* Keyset cursor: id < before, which is stable while people keep posting —
     * an OFFSET would skip or repeat rows as the table grows underneath. */
    if (j->message_id) k += snprintf(sql + k, sizeof sql - (size_t)k, " AND m.id < ?4 ");
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
    snprintf(sql + k, sizeof sql - (size_t)k, " ORDER BY m.id DESC LIMIT ?3;");

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
            "  COALESCE(NULLIF(m.author_name,''), u.display_name, ''), "
            /* Pin state travels with the message: a BROADCAST has no field for
             * it, so a reconnecting client would otherwise lose every pin. */
            "  COALESCE(p.pinned_by,0), COALESCE(p.created_at_ms,0), "
            /* And this user's saved-for-later state, for the same reason. LEFT
             * JOIN rather than EXISTS so the timestamp comes with it, and keyed on
             * the REQUESTING user because a saved item is private (REQ-231). */
            "  COALESCE(s.created_at_ms,0) "
            "FROM messages m LEFT JOIN users u ON u.id = m.author_id "
            "                LEFT JOIN pins  p ON p.message_id = m.id "
            "                LEFT JOIN saved_items s ON s.message_id = m.id AND s.user_id = ?4 "
            /* Every marker numbered explicitly. An anonymous `?` takes the next
             * unused index, so introducing ?4 ABOVE these silently renumbered them
             * to 5, 6 and 7 — the binds still succeeded, the query saw NULLs, and
             * backfill returned nothing. 36 tests caught it; reading would not
             * have. */
            "WHERE m.channel_id=?1 AND m.id>?2 AND m.parent_id IS NULL "
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
    fill_replay_reactions(db, r, j->user_id);
    fill_replay_unfurls(db, r);
    fill_replay_forwards(db, r);
    return r;
}

/* Page backwards through one channel (§6.3): the newest `search_limit`
 * top-level messages strictly older than `message_id`. Answers with the same
 * BACKFILL_OK shape as the forward replay, so the net thread needs no new
 * emit path and the client folds the rows in the same way.
 *
 * `truncated` here means "there is more above this page", which is what lets a
 * client stop asking at the top of the channel instead of retrying forever. */
static oc_dbres *process_history(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    r->type = OC_RES_BACKFILL_OK;
    r->channel_id = j->channel_id;
    if (!channel_read_access(db, j->channel_id, j->user_id)) return r;

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
    "         COALESCE(s.created_at_ms,0) AS saved_at" \
    "    FROM messages m LEFT JOIN users u ON u.id = m.author_id" \
    "                    LEFT JOIN pins p ON p.message_id = m.id" \
    "                    LEFT JOIN saved_items s ON s.message_id = m.id AND s.user_id = ?4"

    const char *sql = j->hist_around
        ? "SELECT id, author_id, created_at_ms, body, reply_count, last_reply, author_name,"
          "       pinned_by, pinned_at, saved_at FROM ("
          "  SELECT * FROM (" OC_HIST_COLS
          "   WHERE m.channel_id=?1 AND m.parent_id IS NULL AND m.id<=?2"
          "   ORDER BY m.id DESC LIMIT ?3)"
          "  UNION ALL"
          "  SELECT * FROM (" OC_HIST_COLS
          "   WHERE m.channel_id=?1 AND m.parent_id IS NULL AND m.id>?2"
          "   ORDER BY m.id ASC LIMIT ?3)"
          ") ORDER BY id;"
        : "SELECT id, author_id, created_at_ms, body, reply_count, last_reply, author_name,"
          "       pinned_by, pinned_at, saved_at FROM ("
          OC_HIST_COLS
          "   WHERE m.channel_id=?1 AND m.id<?2 AND m.parent_id IS NULL"
          "   ORDER BY m.id DESC LIMIT ?3"
          ") ORDER BY id;";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return r;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->channel_id);
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
        m->channel_id  = j->channel_id;
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
                "AND parent_id IS NULL);", -1, &mt, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(mt, 1, (sqlite3_int64)j->channel_id);
            sqlite3_bind_int64(mt, 2, (sqlite3_int64)arr[0].message_id);
            if (sqlite3_step(mt) == SQLITE_ROW)
                r->truncated = (uint8_t)sqlite3_column_int(mt, 0);
            sqlite3_finalize(mt);
        }
    }
    /* The same two fills the reconnect replay runs. Reactions were absent here
     * entirely, which is what made a scroll-back or a permalink render messages
     * permanently without them. */
    fill_replay_reactions(db, r, j->user_id);
    fill_replay_unfurls(db, r);
    fill_replay_forwards(db, r);
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
    if (acc == CH_ARCHIVED){ r->type = OC_RES_ATTACH_ERR; r->err_code = OC_ERR_CHANNEL_ARCHIVED; return r; }

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
        "SELECT channel_id, level, muted FROM notification_prefs WHERE user_id=? "
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
        r->type = OC_RES_NOTIFY_ERR; r->err_code = OC_E_MALFORMED; r->user_id = j->user_id; return r;
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
    /* Answer with the channel list so the client's unread badge is recomputed by the
     * SERVER rather than guessed locally — the count is a query over messages, and
     * two implementations of it would drift. */
    r->type = OC_RES_READ_CURSOR;
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
        "       COALESCE(full_name,''), COALESCE(pronouns,''), COALESCE(phone,'') "
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
    }
    sqlite3_finalize(st);
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

static oc_dbres *process_set_profile(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    sqlite3_stmt *st = NULL;
    /* One statement for the whole screen: the fields are edited together and
     * committed together, so a partial write cannot leave the card showing a
     * mix of what was saved and what was not. */
    sqlite3_prepare_v2(db,
        "UPDATE users SET full_name=?1, title=?2, pronouns=?3, phone=?4, timezone=?5 "
        "  WHERE id=?6;", -1, &st, NULL);
    sqlite3_bind_text (st, 1, j->pf_full_name ? j->pf_full_name : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, 2, j->pf_title     ? j->pf_title     : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, 3, j->pf_pronouns  ? j->pf_pronouns  : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, 4, j->pf_phone     ? j->pf_phone     : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (st, 5, j->pf_timezone  ? j->pf_timezone  : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 6, (sqlite3_int64)j->user_id);
    sqlite3_step(st);
    sqlite3_finalize(st);
    build_profile(db, j->user_id, r);
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
            "  AND size > 0 AND mime LIKE 'image/%';", -1, &ck, NULL);
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

static oc_dbres *process_get_profile(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;
    /* Any tenant member may read any member's profile — the roster is already
     * visible to everyone (REQ-030), so this adds no exposure. */
    build_profile(db, j->message_id ? j->message_id : j->user_id, r);
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
        "       (SELECT COUNT(*) FROM messages x "
        "         WHERE x.parent_id = r.id AND x.deleted_at_ms IS NULL "
        "           AND x.author_id <> ?1 "
        "           AND x.id > COALESCE((SELECT tr.last_read_reply_id FROM thread_reads tr "
        "                                 WHERE tr.user_id = ?1 AND tr.root_id = r.id), 0)) AS unread, "
        "       COALESCE((SELECT tf.state FROM thread_follows tf "
        "                  WHERE tf.user_id = ?1 AND tf.root_id = r.id), 1) AS following, "
        "       substr(COALESCE(r.body,''),1,?2) "
        "  FROM messages r "
        "  JOIN channel_members cm ON cm.channel_id = r.channel_id AND cm.user_id = ?1 "
        " WHERE r.parent_id IS NULL AND r.deleted_at_ms IS NULL "
        /* In it, one way or another: I wrote it, I replied to it, or I said so. */
        "   AND ( r.author_id = ?1 "
        "      OR EXISTS(SELECT 1 FROM messages y WHERE y.parent_id = r.id "
        "                 AND y.author_id = ?1 AND y.deleted_at_ms IS NULL) "
        "      OR EXISTS(SELECT 1 FROM thread_follows f WHERE f.user_id = ?1 "
        "                 AND f.root_id = r.id AND f.state = 1) ) "
        /* ...unless I said otherwise, which outranks having replied. */
        "   AND COALESCE((SELECT tf2.state FROM thread_follows tf2 "
        "                  WHERE tf2.user_id = ?1 AND tf2.root_id = r.id), 1) = 1 "
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
        "           AND x.id > COALESCE((SELECT tr.last_read_reply_id FROM thread_reads tr "
        "                                 WHERE tr.user_id = ?1 AND tr.root_id = r.id), 0)), "
        "       COALESCE((SELECT tf.state FROM thread_follows tf "
        "                  WHERE tf.user_id = ?1 AND tf.root_id = r.id), 1), "
        "       substr(COALESCE(r.body,''),1,?2) "
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
    sqlite3_prepare_v2(db,
        "SELECT dnd_mode, tz_offset_min, allow_start_min, allow_end_min "
        "  FROM users WHERE id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)uid);
    if (sqlite3_step(st) == SQLITE_ROW) {
        r->sc_mode           = (uint8_t)sqlite3_column_int(st, 0);
        r->sc_tz_offset_min  = (int16_t)sqlite3_column_int(st, 1);
        r->sc_start_min      = (uint16_t)sqlite3_column_int(st, 2);
        r->sc_end_min        = (uint16_t)sqlite3_column_int(st, 3);
    }
    sqlite3_finalize(st);
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
    sqlite3_prepare_v2(db,
        "UPDATE users SET dnd_mode=?1, tz_offset_min=?2, allow_start_min=?3, "
        "                 allow_end_min=?4 WHERE id=?5;", -1, &st, NULL);
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

    for (uint8_t i = 0; i < j->n_sched_days && j->sched_days; i++) {
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
        "SELECT rowid, role, expires_at_ms, COALESCE(created_by,0) FROM invites "
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
        arr[n].created_at = 0;      /* not stored; the table has no created_at_ms */
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
    /* Membership first, as every channel-scoped handler does. A draft is user
     * content about a conversation, and storing one for a channel you cannot
     * see would leak its existence back to you on any other device.
     *
     * channel_id 0 is the UNADDRESSED case (REQ-229) and has no membership to
     * check: it belongs to nobody's conversation yet, which is the point. */
    if (j->channel_id && !is_member(db, j->channel_id, j->user_id)) {
        r->type = OC_RES_LIST_ERR; r->err_code = OC_ERR_NOT_A_MEMBER; return r;
    }
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
    /* Joined to channel_members so a draft for a conversation the user has
     * since left stays stored (ARCH-101: leaving is reversible) but is not
     * listed — "a draft for a channel you are not in is simply invisible until
     * you return". */
    sqlite3_prepare_v2(db,
        /* LEFT JOIN, and the membership test allows a NULL channel: an
         * UNADDRESSED draft (REQ-229) belongs to no conversation yet, so an
         * inner join would hide exactly the drafts the New Message pane
         * depends on. */
        "SELECT d.channel_id, d.thread_root, d.body, d.updated_ms, d.id, d.recipients "
        "FROM drafts d "
        "LEFT JOIN channel_members m ON m.channel_id = d.channel_id AND m.user_id = d.user_id "
        "WHERE d.user_id=? AND (d.channel_id IS NULL OR m.user_id IS NOT NULL) "
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
    sj.type = OC_JOB_SEND;
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
    oc_dbres *sent = process_send(db, &sj);
    int ok = sent && sent->type == OC_RES_SEND_OK;
    sqlite3_prepare_v2(db, ok
        ? "UPDATE scheduled_messages SET state='sent', message_id=? WHERE id=?;"
        : "UPDATE scheduled_messages SET state='failed', message_id=?, "
          "  fail_reason='the server could not deliver it' WHERE id=?;", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)(ok ? sent->message_id : 0));
    sqlite3_bind_int64(st, 2, (sqlite3_int64)id);
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
           type == OC_JOB_LIST_EMOJI ||
           type == OC_JOB_LIST_CLIENT_SETTINGS ||
           type == OC_JOB_CALL_AUTH ||
           type == OC_JOB_STORAGE_STATUS ||
           type == OC_JOB_AUDIT_QUERY;
}

/* Dispatch a read-only job against `rdb`. */
static oc_dbres *process_read(sqlite3 *rdb, const oc_job *j) {
    if (j->type == OC_JOB_BACKFILL)       return process_backfill(rdb, j);
    if (j->type == OC_JOB_HISTORY)        return process_history(rdb, j);
    if (j->type == OC_JOB_SEARCH)         return process_search(rdb, j);
    if (j->type == OC_JOB_LIST_CHANNELS)  return process_list_channels(rdb, j);
    if (j->type == OC_JOB_LIST_USERS)     return process_list_users(rdb, j);
    if (j->type == OC_JOB_LIST_REACTIONS) return process_list_reactions(rdb, j);
    if (j->type == OC_JOB_LIST_PINS)      return process_list_pins(rdb, j);
    if (j->type == OC_JOB_LIST_MEMBERS)   return process_list_members(rdb, j);
    if (j->type == OC_JOB_LIST_FILES)     return process_list_files(rdb, j);
    if (j->type == OC_JOB_LIST_SAVED)     return process_list_saved(rdb, j);
    if (j->type == OC_JOB_LIST_THREAD)    return process_list_thread(rdb, j);
    if (j->type == OC_JOB_TYPING)         return process_typing(rdb, j);
    if (j->type == OC_JOB_ATTACH_LOOKUP)  return process_attach_lookup(rdb, j);
    if (j->type == OC_JOB_STORAGE_STATUS) return process_storage_status(rdb, j);
    if (j->type == OC_JOB_AUDIT_QUERY)    return process_audit_query(rdb, j);
    if (j->type == OC_JOB_LIST_WEBHOOKS)  return process_list_webhooks(rdb, j);
    if (j->type == OC_JOB_LIST_INVITES)   return process_list_invites(rdb, j);
    if (j->type == OC_JOB_GET_PROFILE)    return process_get_profile(rdb, j);
    if (j->type == OC_JOB_LIST_FILE_CHANNELS) return process_list_file_channels(rdb, j);
    if (j->type == OC_JOB_LIST_SESSIONS)  return process_list_sessions(rdb, j);
    if (j->type == OC_JOB_LIST_EMOJI)     return process_list_emoji(rdb, j);
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
    if (j->type == OC_JOB_UNFURL_STORE)  return process_unfurl_store(w->db, j);
    if (j->type == OC_JOB_DELETE)        return process_delete(w->db, j);
    if (j->type == OC_JOB_CREATE_CHANNEL) return process_create_channel(w->db, j);
    if (j->type == OC_JOB_JOIN_CHANNEL)   return process_join_channel(w->db, j);
    if (j->type == OC_JOB_LEAVE_CHANNEL)  return process_leave_channel(w->db, j);
    if (j->type == OC_JOB_INVITE_CHANNEL) return process_invite_channel(w->db, j);
    if (j->type == OC_JOB_REMOVE_CHANNEL) return process_remove_channel(w->db, j);
    if (j->type == OC_JOB_OPEN_DM)        return process_open_dm(w->db, j);
    if (j->type == OC_JOB_OPEN_GROUP_DM) return process_open_group_dm(w->db, j);
    if (j->type == OC_JOB_ADD_EMOJI)      return process_add_emoji(w->db, j);
    if (j->type == OC_JOB_DELETE_EMOJI)   return process_delete_emoji(w->db, j);
    if (j->type == OC_JOB_INVITE_USER)    return process_invite_user(w->db, j);
    if (j->type == OC_JOB_FIRE_SCHEDULED)    return process_fire_scheduled(w->db, j);
    if (j->type == OC_JOB_SCHEDULE)          return process_schedule(w->db, j);
    if (j->type == OC_JOB_LIST_SCHEDULED)    return process_list_scheduled(w->db, j);
    if (j->type == OC_JOB_CANCEL_SCHEDULED)  return process_cancel_scheduled(w->db, j);
    if (j->type == OC_JOB_UPDATE_SCHEDULED)  return process_update_scheduled(w->db, j);
    if (j->type == OC_JOB_SET_DRAFT)      return process_set_draft(w->db, j);
    if (j->type == OC_JOB_LIST_DRAFTS)    return process_list_drafts(w->db, j);
    if (j->type == OC_JOB_REMOVE_USER)    return process_remove_user(w->db, j);
    if (j->type == OC_JOB_REDEEM)         return process_redeem(w, j);
    if (j->type == OC_JOB_REACT)          return process_react(w->db, j);
    if (j->type == OC_JOB_PIN)            return process_pin(w->db, j);
    if (j->type == OC_JOB_UPDATE_CHANNEL) return process_update_channel(w->db, j);
    if (j->type == OC_JOB_SAVE_ITEM)      return process_save_item(w->db, j);
    /* On the WRITER because it stamps the seen watermark; the read half would
     * otherwise be a reader job with a write in it. */
    if (j->type == OC_JOB_LIST_ACTIVITY)  return process_list_activity(w->db, j);
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
    if (j->type == OC_JOB_REVOKE_INVITE)     return process_revoke_invite(w->db, j);
    if (j->type == OC_JOB_SET_WEBHOOK_STATE) return process_set_webhook_state(w->db, j);
    if (j->type == OC_JOB_ROTATE_WEBHOOK)    return process_rotate_webhook(w->db, j);
    if (j->type == OC_JOB_WEBHOOK_POST)    return process_webhook_post(w->db, j);
    if (j->type == OC_JOB_DELETE_WEBHOOK)  return process_delete_webhook(w->db, j);
    if (j->type == OC_JOB_SET_NOTIFY_PREF) return process_set_notify_pref(w->db, j);
    if (j->type == OC_JOB_SET_MUTE)        return process_set_mute(w->db, j);
    if (j->type == OC_JOB_SET_NOTIFY_DEFAULT) return process_set_notify_default(w->db, j);
    if (j->type == OC_JOB_SET_STATUS)      return process_set_status(w->db, j);
    if (j->type == OC_JOB_SET_PROFILE)     return process_set_profile(w->db, j);
    if (j->type == OC_JOB_SET_AVATAR)      return process_set_avatar(w->db, j);
    if (j->type == OC_JOB_SET_READ_CURSOR) return process_set_read_cursor(w->db, j);
    if (j->type == OC_JOB_LIST_THREADS)    return process_list_threads(w->db, j);
    if (j->type == OC_JOB_SET_THREAD_FOLLOW) return process_set_thread_follow(w->db, j);
    if (j->type == OC_JOB_MARK_THREAD_READ)  return process_mark_thread_read(w->db, j);
    if (j->type == OC_JOB_SET_SCHEDULE)    return process_set_schedule(w->db, j);
    if (j->type == OC_JOB_SET_KEYWORDS)    return process_set_keywords(w->db, j);
    if (j->type == OC_JOB_SET_PRIORITY)    return process_set_priority(w->db, j);
    if (j->type == OC_JOB_SET_SNOOZE)      return process_set_snooze(w->db, j);
    if (j->type == OC_JOB_SET_TZ_OFFSET)   return process_set_tz_offset(w->db, j);
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
            /* An AVATAR is an attachment no message references, so it looks
             * exactly like an orphan to this sweep — and would have been collected an
             * hour after being set, leaving every profile picture in the workspace
             * silently blank. The other two tiers below need the same exclusion for
             * the same reason: an avatar is in use even though nothing points at it
             * from `messages`. */
            "  AND id NOT IN (SELECT avatar_attachment_id FROM users "
            "                  WHERE avatar_attachment_id IS NOT NULL) "
            "  AND id NOT IN (SELECT attachment_id FROM custom_emoji) "
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
                "  AND id NOT IN (SELECT avatar_attachment_id FROM users "
                "                  WHERE avatar_attachment_id IS NOT NULL) "
                /* A custom emoji is in use for the same non-obvious reason an avatar
                 * is: no message references it, so it looks like an orphan. */
                "  AND id NOT IN (SELECT attachment_id FROM custom_emoji) "
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
