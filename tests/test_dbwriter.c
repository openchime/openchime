/* Tests for the DB-writer thread (dbwriter.c): migrate-on-boot lifecycle, and
 * the AUTH / SEND job processing (idempotency, membership, broadcast fan-out
 * list) exercised directly through the job queue — no network involved.
 * Includes the code under test directly; links sqlite + pthread. */

#include "dbwriter.h"
#include "migrate.h"
#include "protocol.h"
#include "issuer.h"
#include "check.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int table_exists(sqlite3 *db, const char *name) {
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?;", -1, &st, NULL);
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    int found = (sqlite3_step(st) == SQLITE_ROW);
    sqlite3_finalize(st);
    return found;
}

/* Block until a result is available (bounded), draining the eventfd. */
static oc_dbres *wait_result(oc_dbwriter *w) {
    for (int i = 0; i < 500; i++) {
        oc_dbres *r = oc_dbwriter_next_result(w);
        if (r) return r;
        usleep(2000);
    }
    return NULL;
}

static void cleanup_db(const char *path) {
    unlink(path);
    char wal[256], shm[256];
    snprintf(wal, sizeof wal, "%s-wal", path);
    snprintf(shm, sizeof shm, "%s-shm", path);
    unlink(wal); unlink(shm);
}

static void test_start_migrates_and_stops(void) {
    const char *path = "build/test_dbwriter1.db";
    cleanup_db(path);
    oc_dbwriter *w = oc_dbwriter_start(path);
    CHECK(w != NULL);
    oc_dbwriter_stop(w);

    sqlite3 *db = NULL;
    CHECK(sqlite3_open(path, &db) == SQLITE_OK);
    CHECK(oc_schema_version(db) == 18);
    CHECK(table_exists(db, "messages"));
    CHECK(table_exists(db, "sessions"));
    sqlite3_close(db);
    cleanup_db(path);
}

/* Low PBKDF2 rounds keep the tests fast; the production default is 600k. */
#define TEST_PW_ITERS 2048

static uint64_t reg(oc_dbwriter *w, const char *user, const char *pass, uint8_t role) {
    return oc_dbwriter_register_local(w, user, pass, role, TEST_PW_ITERS);
}

/* Local auth over a conn; optionally captures the minted session token + role. */
static uint64_t auth_local(oc_dbwriter *w, uint64_t conn_id, const char *user,
                           const char *pass, uint8_t *token_out, uint8_t *role_out) {
    uint8_t cbuf[512]; oc_wbuf cw; oc_wbuf_init(&cw, cbuf, sizeof cbuf);
    oc_encode_local_credential(&cw, oc_slice_str(user), oc_slice_str(pass));
    oc_job *j = oc_job_new(OC_JOB_AUTH, conn_id);
    j->method = OC_AUTH_LOCAL;
    oc_job_set_token(j, cbuf, cw.len);
    oc_dbwriter_submit(w, j);
    oc_dbres *r = wait_result(w);
    uint64_t uid = 0;
    if (r && r->type == OC_RES_AUTH_OK && r->conn_id == conn_id) {
        uid = r->user_id;
        if (token_out && r->has_session_token)
            memcpy(token_out, r->session_token, OC_SESSION_TOKEN_LEN);
        if (role_out) *role_out = r->role;
    }
    oc_dbres_free(r);
    return uid;
}

/* Local auth from `source` (NULL = none), returning the reason code (0 on OK). */
static uint16_t auth_local_from(oc_dbwriter *w, uint64_t conn_id, const char *user,
                                const char *pass, const char *source) {
    uint8_t cbuf[512]; oc_wbuf cw; oc_wbuf_init(&cw, cbuf, sizeof cbuf);
    oc_encode_local_credential(&cw, oc_slice_str(user), oc_slice_str(pass));
    oc_job *j = oc_job_new(OC_JOB_AUTH, conn_id);
    j->method = OC_AUTH_LOCAL;
    if (source) {
        size_t n = strlen(source);
        if (n >= sizeof j->source) n = sizeof j->source - 1;
        memcpy(j->source, source, n);
        j->source[n] = '\0';
    }
    oc_job_set_token(j, cbuf, cw.len);
    oc_dbwriter_submit(w, j);
    oc_dbres *r = wait_result(w);
    uint16_t code = (r && r->type == OC_RES_AUTH_OK) ? 0 : (r ? r->err_code : 0xFFFF);
    oc_dbres_free(r);
    return code;
}

static uint16_t auth_local_code(oc_dbwriter *w, uint64_t conn_id, const char *user, const char *pass) {
    return auth_local_from(w, conn_id, user, pass, NULL);
}

/* Reconnect with a previously minted session token. */
static uint64_t auth_session(oc_dbwriter *w, uint64_t conn_id, const uint8_t token[OC_SESSION_TOKEN_LEN]) {
    oc_job *j = oc_job_new(OC_JOB_AUTH, conn_id);
    j->method = OC_AUTH_SESSION;
    oc_job_set_token(j, token, OC_SESSION_TOKEN_LEN);
    oc_dbwriter_submit(w, j);
    oc_dbres *r = wait_result(w);
    uint64_t uid = (r && r->type == OC_RES_AUTH_OK && r->conn_id == conn_id) ? r->user_id : 0;
    oc_dbres_free(r);
    return uid;
}

static void test_auth_and_send(void) {
    const char *path = "build/test_dbwriter2.db";
    cleanup_db(path);
    oc_dbwriter *w = oc_dbwriter_start(path);
    CHECK(w != NULL);

    /* Register two local accounts (owner + member); each gets a distinct id and
     * joins the default channel. */
    uint64_t a = reg(w, "alice", "pw-alice", OC_ROLE_OWNER);
    uint64_t b = reg(w, "bob",   "pw-bob",   OC_ROLE_MEMBER);
    CHECK(a != 0 && b != 0 && a != b);

    /* Local auth returns the account's id + role and mints a session token. */
    uint8_t atoken[OC_SESSION_TOKEN_LEN]; uint8_t arole = 0xFF;
    memset(atoken, 0, sizeof atoken);
    CHECK(auth_local(w, 10, "alice", "pw-alice", atoken, &arole) == a);
    CHECK(arole == OC_ROLE_OWNER);
    /* A wrong password is rejected; an unknown user too. */
    CHECK(auth_local(w, 11, "alice", "wrong",    NULL, NULL) == 0);
    CHECK(auth_local(w, 11, "nobody", "whatever", NULL, NULL) == 0);
    /* Reconnect with the minted session token resolves the same user. */
    CHECK(auth_session(w, 12, atoken) == a);
    /* A bogus session token is rejected. */
    uint8_t bogus[OC_SESSION_TOKEN_LEN]; memset(bogus, 0xEE, sizeof bogus);
    CHECK(auth_session(w, 12, bogus) == 0);
    /* bob logs in for the send test below. */
    CHECK(auth_local(w, 13, "bob", "pw-bob", NULL, NULL) == b);

    /* user-a sends to the default channel: gets an id and both members back. */
    uint8_t idem[OC_IDEM_LEN];
    memset(idem, 0x11, sizeof idem);
    oc_job *j = oc_job_new(OC_JOB_SEND, 10);
    j->user_id = a; j->channel_id = OC_DEFAULT_CHANNEL;
    memcpy(j->idem, idem, sizeof idem);
    oc_job_set_body(j, "hello all", 9);
    oc_dbwriter_submit(w, j);

    oc_dbres *r = wait_result(w);
    CHECK(r && r->type == OC_RES_SEND_OK);
    CHECK(r->message_id > 0);
    CHECK(r->duplicate == 0);
    CHECK(r->body_len == 9 && memcmp(r->body, "hello all", 9) == 0);
    int saw_a = 0, saw_b = 0;
    for (size_t i = 0; i < r->n_members; i++) {
        if (r->members[i] == a) saw_a = 1;
        if (r->members[i] == b) saw_b = 1;
    }
    CHECK(saw_a && saw_b);
    uint64_t first_id = r->message_id;
    oc_dbres_free(r);

    /* Same idempotency token -> replay of the original id, no new broadcast. */
    j = oc_job_new(OC_JOB_SEND, 10);
    j->user_id = a; j->channel_id = OC_DEFAULT_CHANNEL;
    memcpy(j->idem, idem, sizeof idem);
    oc_job_set_body(j, "hello all", 9);
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_SEND_OK && r->duplicate == 1);
    CHECK(r->message_id == first_id);
    oc_dbres_free(r);

    /* A different token allocates a strictly greater id (monotonic, ARCH-43). */
    j = oc_job_new(OC_JOB_SEND, 11);
    j->user_id = b; j->channel_id = OC_DEFAULT_CHANNEL;
    memset(j->idem, 0x22, OC_IDEM_LEN);
    oc_job_set_body(j, "hi", 2);
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_SEND_OK && r->message_id > first_id);
    oc_dbres_free(r);

    /* A send to a channel that does not exist is rejected. (The public/private
     * membership gate is exercised in test_channels; the default channel is
     * public, so posting to it auto-joins rather than rejecting.) */
    j = oc_job_new(OC_JOB_SEND, 99);
    j->user_id = b; j->channel_id = 4242;
    memset(j->idem, 0x33, OC_IDEM_LEN);
    oc_job_set_body(j, "x", 1);
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_SEND_ERR && r->err_code == OC_ERR_UNKNOWN_CHANNEL);
    oc_dbres_free(r);

    oc_dbwriter_stop(w);
    cleanup_db(path);
}

static uint64_t send_msg(oc_dbwriter *w, uint64_t uid, const uint8_t idem[OC_IDEM_LEN], const char *body) {
    oc_job *j = oc_job_new(OC_JOB_SEND, 1);
    j->user_id = uid; j->channel_id = OC_DEFAULT_CHANNEL;
    memcpy(j->idem, idem, OC_IDEM_LEN);
    oc_job_set_body(j, body, strlen(body));
    oc_dbwriter_submit(w, j);
    oc_dbres *r = wait_result(w);
    uint64_t id = (r && r->type == OC_RES_SEND_OK) ? r->message_id : 0;
    oc_dbres_free(r);
    return id;
}

static oc_dbres *backfill(oc_dbwriter *w, uint64_t uid, uint64_t channel, uint64_t after) {
    oc_job *j = oc_job_new(OC_JOB_BACKFILL, 1);
    j->user_id = uid;
    j->cursors = malloc(sizeof(oc_bf_cursor));
    j->cursors[0].channel_id = channel;
    j->cursors[0].after_message_id = after;
    j->n_cursors = 1;
    oc_dbwriter_submit(w, j);
    return wait_result(w);
}

static void test_backfill(void) {
    const char *path = "build/test_dbwriter3.db";
    cleanup_db(path);
    oc_dbwriter *w = oc_dbwriter_start(path);
    CHECK(w != NULL);

    uint64_t u = reg(w, "bf-user", "pw", OC_ROLE_MEMBER);
    CHECK(u != 0);
    uint8_t idem[OC_IDEM_LEN];
    memset(idem, 1, sizeof idem); uint64_t m1 = send_msg(w, u, idem, "one");
    memset(idem, 2, sizeof idem); uint64_t m2 = send_msg(w, u, idem, "two");
    memset(idem, 3, sizeof idem); uint64_t m3 = send_msg(w, u, idem, "three");
    CHECK(m1 && m2 > m1 && m3 > m2);

    /* From m1: replay m2, m3 in ascending order; high-water is m3. */
    oc_dbres *r = backfill(w, u, OC_DEFAULT_CHANNEL, m1);
    CHECK(r && r->type == OC_RES_BACKFILL_OK && r->n_replay == 2);
    CHECK(r->replay[0].message_id == m2 && r->replay[1].message_id == m3);
    CHECK(r->replay[0].body_len == 3 && memcmp(r->replay[0].body, "two", 3) == 0);
    CHECK(r->high_water == m3);
    oc_dbres_free(r);

    /* From 0: all three. */
    r = backfill(w, u, OC_DEFAULT_CHANNEL, 0);
    CHECK(r && r->n_replay == 3 && r->replay[0].message_id == m1);
    oc_dbres_free(r);

    /* A channel the user isn't a member of replays nothing. */
    r = backfill(w, u, 999, 0);
    CHECK(r && r->type == OC_RES_BACKFILL_OK && r->n_replay == 0);
    oc_dbres_free(r);

    oc_dbwriter_stop(w);
    cleanup_db(path);
}

/* OIDC-mode auth: a test issuer stands in for the central service (AUTH.md
 * §3.6). Proves the wiring — configure -> JWT verify -> JIT-provision -> session
 * -> reconnect — on top of the crypto that test_jwt covers in isolation. */
static void test_oidc_auth(void) {
    const char *path = "build/test_dbwriter_oidc.db";
    cleanup_db(path);
    oc_dbwriter *w = oc_dbwriter_start(path);
    CHECK(w != NULL);

    oc_issuer is;
    CHECK(oc_issuer_init(&is, "oc-dbw-oidc") == 0);
    const char *ISS = "https://auth.openchime.io";
    const char *AUD = "acme.example";
    CHECK(oc_dbwriter_configure_oidc(w, ISS, AUD, is.pem,
                                     "authorize=https://auth.openchime.io/authorize") == 0);
    CHECK(oc_dbwriter_auth_methods(w) == (OC_AUTH_OIDC | OC_AUTH_SESSION));
    CHECK(strlen(oc_dbwriter_oidc_params(w)) > 0);

    const char *HDR = "{\"alg\":\"ES256\",\"typ\":\"JWT\"}";
    char payload[512];
    /* exp far in the real future — the daemon checks against wall-clock time. */
    snprintf(payload, sizeof payload,
        "{\"iss\":\"%s\",\"aud\":\"%s\",\"sub\":\"google|42\","
        "\"email\":\"a@acme.example\",\"name\":\"A\",\"exp\":4102444800}", ISS, AUD);
    char token[2048];
    size_t tlen = oc_issuer_mint(&is, HDR, payload, token);

    /* A valid central JWT provisions the user and mints a session. */
    uint8_t sess[OC_SESSION_TOKEN_LEN]; memset(sess, 0, sizeof sess);
    uint64_t uid = 0; uint8_t role = 0xFF; int has_tok = 0;
    {
        oc_job *j = oc_job_new(OC_JOB_AUTH, 20);
        j->method = OC_AUTH_OIDC;
        oc_job_set_token(j, token, tlen);
        oc_dbwriter_submit(w, j);
        oc_dbres *r = wait_result(w);
        CHECK(r && r->type == OC_RES_AUTH_OK);
        if (r && r->type == OC_RES_AUTH_OK) {
            uid = r->user_id; role = r->role; has_tok = r->has_session_token;
            if (r->has_session_token) memcpy(sess, r->session_token, OC_SESSION_TOKEN_LEN);
        }
        oc_dbres_free(r);
    }
    CHECK(uid != 0);
    CHECK(role == OC_ROLE_MEMBER);
    CHECK(has_tok == 1);

    /* Re-auth with the same JWT is idempotent (same user). */
    {
        oc_job *j = oc_job_new(OC_JOB_AUTH, 21);
        j->method = OC_AUTH_OIDC;
        oc_job_set_token(j, token, tlen);
        oc_dbwriter_submit(w, j);
        oc_dbres *r = wait_result(w);
        CHECK(r && r->type == OC_RES_AUTH_OK && r->user_id == uid);
        oc_dbres_free(r);
    }

    /* The minted session token reconnects to the same user. */
    CHECK(has_tok && auth_session(w, 22, sess) == uid);

    /* A JWT minted for a different audience is rejected. */
    {
        char bad[512];
        snprintf(bad, sizeof bad,
            "{\"iss\":\"%s\",\"aud\":\"other.example\",\"sub\":\"google|42\",\"exp\":4102444800}", ISS);
        char bt[2048];
        size_t bl = oc_issuer_mint(&is, HDR, bad, bt);
        oc_job *j = oc_job_new(OC_JOB_AUTH, 23);
        j->method = OC_AUTH_OIDC;
        oc_job_set_token(j, bt, bl);
        oc_dbwriter_submit(w, j);
        oc_dbres *r = wait_result(w);
        CHECK(r && r->type == OC_RES_AUTH_ERR && r->err_code == OC_ERR_AUTH_INVALID_TOKEN);
        oc_dbres_free(r);
    }

    /* Local auth is refused in OIDC mode (one mode per tenant). */
    CHECK(auth_local(w, 24, "someone", "pw", NULL, NULL) == 0);

    oc_issuer_free(&is);
    oc_dbwriter_stop(w);
    cleanup_db(path);
}

/* Failed local-auth attempts are throttled per account (REQ-191). After the
 * configured number of failures the account is AUTH_RATE_LIMITED — even with
 * the correct password — while other accounts stay unaffected. */
static void test_auth_rate_limit(void) {
    const char *path = "build/test_dbwriter_rl.db";
    cleanup_db(path);
    oc_dbwriter *w = oc_dbwriter_start(path);
    CHECK(w != NULL);

    CHECK(reg(w, "victim", "correct-horse", OC_ROLE_MEMBER) != 0);
    CHECK(reg(w, "other",  "other-pw",      OC_ROLE_MEMBER) != 0);

    /* Burn through the failure budget (OC_AUTH_MAX_FAILURES = 5). */
    for (int i = 0; i < 5; i++)
        CHECK(auth_local_code(w, 30, "victim", "wrong") == OC_ERR_AUTH_INVALID_TOKEN);

    /* Now the account is throttled — even the correct password is refused. */
    CHECK(auth_local_code(w, 30, "victim", "correct-horse") == OC_ERR_AUTH_RATE_LIMITED);

    /* A different account is unaffected. */
    CHECK(auth_local_code(w, 31, "other", "other-pw") == 0);

    oc_dbwriter_stop(w);
    cleanup_db(path);
}

/* Per-source throttling: an account-spray from one IP is stopped even though no
 * single account trips the per-account limit (REQ-191). */
static void test_source_rate_limit(void) {
    const char *path = "build/test_dbwriter_srcrl.db";
    cleanup_db(path);
    oc_dbwriter *w = oc_dbwriter_start(path);
    CHECK(w != NULL);

    CHECK(reg(w, "target", "right-pw", OC_ROLE_MEMBER) != 0);

    /* 20 failures (OC_AUTH_SOURCE_MAX_FAILURES) from one IP, each against a
     * distinct username so the per-account limiter never trips. */
    for (int i = 0; i < 20; i++) {
        char u[32];
        snprintf(u, sizeof u, "spray%d", i);
        CHECK(auth_local_from(w, 60, u, "x", "203.0.113.9") == OC_ERR_AUTH_INVALID_TOKEN);
    }

    /* The IP is now throttled — even a correct login from it is refused. */
    CHECK(auth_local_from(w, 60, "target", "right-pw", "203.0.113.9") == OC_ERR_AUTH_RATE_LIMITED);

    /* The same account from a different IP still succeeds. */
    CHECK(auth_local_from(w, 61, "target", "right-pw", "198.51.100.7") == 0);

    oc_dbwriter_stop(w);
    cleanup_db(path);
}

/* Submit a SET_ROLE job (actor changes target's role); returns the result's
 * reason code (0 on OC_RES_SETROLE_OK). */
static uint16_t set_role(oc_dbwriter *w, uint64_t actor, uint64_t target, uint8_t next) {
    oc_job *j = oc_job_new(OC_JOB_SET_ROLE, 40);
    j->user_id = actor;
    j->target_user_id = target;
    j->role = next;
    oc_dbwriter_submit(w, j);
    oc_dbres *r = wait_result(w);
    uint16_t code = (r && r->type == OC_RES_SETROLE_OK) ? 0 : (r ? r->err_code : 0xFFFF);
    oc_dbres_free(r);
    return code;
}

/* Revoke sessions via a LOGOUT job; returns the reason code (0 on OK). */
static uint16_t do_logout(oc_dbwriter *w, uint64_t uid, uint8_t scope, const uint8_t *token) {
    oc_job *j = oc_job_new(OC_JOB_LOGOUT, 70);
    j->user_id = uid;
    j->scope = scope;
    if (token) oc_job_set_token(j, token, OC_SESSION_TOKEN_LEN);
    oc_dbwriter_submit(w, j);
    oc_dbres *r = wait_result(w);
    uint16_t code = (r && r->type == OC_RES_LOGOUT_OK) ? 0 : (r ? r->err_code : 0xFFFF);
    oc_dbres_free(r);
    return code;
}

/* Session revocation (REQ-182): LOGOUT scope THIS drops one session, scope ALL
 * drops them all, and a user cannot revoke another user's session. */
static void test_logout(void) {
    const char *path = "build/test_dbwriter_logout.db";
    cleanup_db(path);
    oc_dbwriter *w = oc_dbwriter_start(path);
    CHECK(w != NULL);

    uint64_t uid = reg(w, "victim", "pw", OC_ROLE_MEMBER);
    uint64_t other = reg(w, "other", "pw", OC_ROLE_MEMBER);
    CHECK(uid && other);

    /* Two independent sessions for the same user. */
    uint8_t t1[OC_SESSION_TOKEN_LEN], t2[OC_SESSION_TOKEN_LEN];
    memset(t1, 0, sizeof t1); memset(t2, 0, sizeof t2);
    CHECK(auth_local(w, 80, "victim", "pw", t1, NULL) == uid);
    CHECK(auth_local(w, 81, "victim", "pw", t2, NULL) == uid);
    CHECK(auth_session(w, 82, t1) == uid);
    CHECK(auth_session(w, 83, t2) == uid);

    /* Another user cannot revoke victim's session (scoped to the actor). */
    CHECK(do_logout(w, other, OC_LOGOUT_THIS, t1) == 0);   /* no-op delete */
    CHECK(auth_session(w, 84, t1) == uid);                 /* still valid */

    /* Scope THIS revokes exactly the presented token. */
    CHECK(do_logout(w, uid, OC_LOGOUT_THIS, t1) == 0);
    CHECK(auth_session(w, 85, t1) == 0);                   /* revoked */
    CHECK(auth_session(w, 86, t2) == uid);                 /* the other survives */

    /* Scope ALL revokes everything the user has. */
    CHECK(do_logout(w, uid, OC_LOGOUT_ALL, NULL) == 0);
    CHECK(auth_session(w, 87, t2) == 0);

    oc_dbwriter_stop(w);
    cleanup_db(path);
}

/* Role enforcement (ARCH-60): the policy matrix and the ≥1-owner invariant. */
static void test_role_enforcement(void) {
    const char *path = "build/test_dbwriter_roles.db";
    cleanup_db(path);
    oc_dbwriter *w = oc_dbwriter_start(path);
    CHECK(w != NULL);

    uint64_t owner  = reg(w, "owner",  "pw", OC_ROLE_OWNER);
    uint64_t admin  = reg(w, "admin",  "pw", OC_ROLE_ADMIN);
    uint64_t member = reg(w, "member", "pw", OC_ROLE_MEMBER);
    uint64_t m2     = reg(w, "member2", "pw", OC_ROLE_MEMBER);
    CHECK(owner && admin && member && m2);

    /* A member may not change roles. */
    CHECK(set_role(w, member, m2, OC_ROLE_ADMIN) == OC_ERR_FORBIDDEN);
    /* An admin may promote a member... */
    CHECK(set_role(w, admin, m2, OC_ROLE_ADMIN) == 0);
    /* ...but may not grant owner, nor touch another admin/owner. */
    CHECK(set_role(w, admin, member, OC_ROLE_OWNER) == OC_ERR_FORBIDDEN);
    CHECK(set_role(w, admin, m2, OC_ROLE_MEMBER) == OC_ERR_FORBIDDEN); /* m2 is now admin */
    CHECK(set_role(w, admin, owner, OC_ROLE_MEMBER) == OC_ERR_FORBIDDEN);

    /* The owner may promote and demote freely... */
    CHECK(set_role(w, owner, member, OC_ROLE_ADMIN) == 0);
    CHECK(set_role(w, owner, member, OC_ROLE_MEMBER) == 0);

    /* ...except demoting the last owner, which the invariant refuses. */
    CHECK(set_role(w, owner, owner, OC_ROLE_MEMBER) == OC_ERR_LAST_OWNER);
    /* With a second owner, the first may then be demoted. */
    CHECK(set_role(w, owner, admin, OC_ROLE_OWNER) == 0);   /* admin -> owner */
    CHECK(set_role(w, owner, owner, OC_ROLE_MEMBER) == 0);  /* now safe */

    /* An unknown target is forbidden (no existence disclosure). */
    CHECK(set_role(w, admin, 99999, OC_ROLE_ADMIN) == OC_ERR_FORBIDDEN);

    oc_dbwriter_stop(w);
    cleanup_db(path);
}

/* Submit an EDIT job; caller frees the returned result. */
static oc_dbres *do_edit(oc_dbwriter *w, uint64_t uid, uint64_t channel,
                         uint64_t mid, const char *body) {
    oc_job *j = oc_job_new(OC_JOB_EDIT, 50);
    j->user_id = uid; j->channel_id = channel; j->message_id = mid;
    oc_job_set_body(j, body, strlen(body));
    oc_dbwriter_submit(w, j);
    return wait_result(w);
}

/* Submit a DELETE job; caller frees the returned result. */
static oc_dbres *do_delete(oc_dbwriter *w, uint64_t uid, uint64_t channel, uint64_t mid) {
    oc_job *j = oc_job_new(OC_JOB_DELETE, 51);
    j->user_id = uid; j->channel_id = channel; j->message_id = mid;
    oc_dbwriter_submit(w, j);
    return wait_result(w);
}

/* Message management (REQ-032/051/052): author-only edit, self- and
 * moderator-delete tombstones, and the not-found / forbidden gates. */
static void test_edit_delete(void) {
    const char *path = "build/test_dbwriter_msgmgmt.db";
    cleanup_db(path);
    oc_dbwriter *w = oc_dbwriter_start(path);
    CHECK(w != NULL);

    uint64_t owner  = reg(w, "md-owner",  "pw", OC_ROLE_OWNER);
    uint64_t author = reg(w, "md-author", "pw", OC_ROLE_MEMBER);
    uint64_t other  = reg(w, "md-other",  "pw", OC_ROLE_MEMBER);
    CHECK(owner && author && other);

    uint8_t idem[OC_IDEM_LEN];
    memset(idem, 0xA1, sizeof idem);
    uint64_t mid = send_msg(w, author, idem, "original");
    CHECK(mid != 0);

    /* A non-author may not edit (no moderator edit, REQ-032). */
    oc_dbres *r = do_edit(w, other, OC_DEFAULT_CHANNEL, mid, "hijacked");
    CHECK(r && r->type == OC_RES_EDIT_ERR && r->err_code == OC_ERR_FORBIDDEN);
    oc_dbres_free(r);

    /* Editing a message that does not exist is UNKNOWN_MESSAGE. */
    r = do_edit(w, author, OC_DEFAULT_CHANNEL, 99999, "nope");
    CHECK(r && r->type == OC_RES_EDIT_ERR && r->err_code == OC_ERR_UNKNOWN_MESSAGE);
    oc_dbres_free(r);

    /* The author edits: OK, new body echoed, fan-out spans the members. */
    r = do_edit(w, author, OC_DEFAULT_CHANNEL, mid, "edited body");
    CHECK(r && r->type == OC_RES_EDIT_OK);
    CHECK(r->message_id == mid && r->author_id == author);
    CHECK(r->server_time != 0);   /* edited_at_ms stamped */
    CHECK(r->body_len == 11 && memcmp(r->body, "edited body", 11) == 0);
    CHECK(r->n_members == 3);
    oc_dbres_free(r);

    /* The edit persisted (backfill replays the new body). */
    r = backfill(w, author, OC_DEFAULT_CHANNEL, mid - 1);
    CHECK(r && r->n_replay >= 1);
    CHECK(r->replay[0].message_id == mid);
    CHECK(r->replay[0].body_len == 11 && memcmp(r->replay[0].body, "edited body", 11) == 0);
    oc_dbres_free(r);

    /* A non-author, non-moderator member may not delete someone else's message. */
    r = do_delete(w, other, OC_DEFAULT_CHANNEL, mid);
    CHECK(r && r->type == OC_RES_DELETE_ERR && r->err_code == OC_ERR_FORBIDDEN);
    oc_dbres_free(r);

    /* The author deletes their own: tombstone, deleted_by == author (REQ-052). */
    r = do_delete(w, author, OC_DEFAULT_CHANNEL, mid);
    CHECK(r && r->type == OC_RES_DELETE_OK);
    CHECK(r->message_id == mid && r->author_id == author && r->user_id == author);
    CHECK(r->n_members == 3);
    oc_dbres_free(r);

    /* A tombstoned message is no longer editable, and re-delete is a no-op error. */
    r = do_edit(w, author, OC_DEFAULT_CHANNEL, mid, "resurrect");
    CHECK(r && r->type == OC_RES_EDIT_ERR && r->err_code == OC_ERR_UNKNOWN_MESSAGE);
    oc_dbres_free(r);
    r = do_delete(w, author, OC_DEFAULT_CHANNEL, mid);
    CHECK(r && r->type == OC_RES_DELETE_ERR && r->err_code == OC_ERR_UNKNOWN_MESSAGE);
    oc_dbres_free(r);

    /* Tombstone survives replay with an empty body (thread linkage preserved). */
    r = backfill(w, author, OC_DEFAULT_CHANNEL, mid - 1);
    CHECK(r && r->n_replay >= 1 && r->replay[0].message_id == mid);
    CHECK(r->replay[0].body_len == 0);
    oc_dbres_free(r);

    /* Moderation: an owner deletes another user's message, deleted_by == owner. */
    memset(idem, 0xB2, sizeof idem);
    uint64_t mid2 = send_msg(w, author, idem, "moderate me");
    CHECK(mid2 != 0);
    r = do_delete(w, owner, OC_DEFAULT_CHANNEL, mid2);
    CHECK(r && r->type == OC_RES_DELETE_OK);
    CHECK(r->author_id == author && r->user_id == owner);   /* self vs moderator */
    oc_dbres_free(r);

    oc_dbwriter_stop(w);
    cleanup_db(path);
}

/* --- Channel management (REQ-031/033/050) ------------------------------- */

static oc_dbres *create_channel(oc_dbwriter *w, uint64_t uid, const char *name, uint8_t is_public) {
    oc_job *j = oc_job_new(OC_JOB_CREATE_CHANNEL, 90);
    j->user_id = uid; j->ch_is_public = is_public;
    j->ch_name = name ? strdup(name) : NULL;
    oc_dbwriter_submit(w, j);
    return wait_result(w);
}

static oc_dbres *list_channels(oc_dbwriter *w, uint64_t uid) {
    oc_job *j = oc_job_new(OC_JOB_LIST_CHANNELS, 91);
    j->user_id = uid;
    oc_dbwriter_submit(w, j);
    return wait_result(w);
}

/* JOIN (type OC_JOB_JOIN_CHANNEL) or LEAVE (OC_JOB_LEAVE_CHANNEL). */
static oc_dbres *chan_ref(oc_dbwriter *w, int type, uint64_t uid, uint64_t channel) {
    oc_job *j = oc_job_new(type, 92);
    j->user_id = uid; j->channel_id = channel;
    oc_dbwriter_submit(w, j);
    return wait_result(w);
}

/* INVITE (OC_JOB_INVITE_CHANNEL) or REMOVE (OC_JOB_REMOVE_CHANNEL). */
static oc_dbres *chan_member(oc_dbwriter *w, int type, uint64_t actor, uint64_t channel, uint64_t target) {
    oc_job *j = oc_job_new(type, 93);
    j->user_id = actor; j->channel_id = channel; j->target_user_id = target;
    oc_dbwriter_submit(w, j);
    return wait_result(w);
}

static uint8_t g_send_seq = 0;
static uint16_t send_to(oc_dbwriter *w, uint64_t uid, uint64_t channel, const char *body) {
    oc_job *j = oc_job_new(OC_JOB_SEND, 94);
    j->user_id = uid; j->channel_id = channel;
    memset(j->idem, ++g_send_seq, OC_IDEM_LEN);
    oc_job_set_body(j, body, strlen(body));
    oc_dbwriter_submit(w, j);
    oc_dbres *r = wait_result(w);
    uint16_t code = (r && r->type == OC_RES_SEND_OK) ? 0 : (r ? r->err_code : 0xFFFF);
    oc_dbres_free(r);
    return code;
}

static int list_has(oc_dbres *r, uint64_t cid, int *joined_out) {
    for (size_t i = 0; i < r->n_chlist; i++)
        if (r->chlist[i].channel_id == cid) {
            if (joined_out) *joined_out = r->chlist[i].joined;
            return 1;
        }
    return 0;
}

static void test_channels(void) {
    const char *path = "build/test_dbwriter_channels.db";
    cleanup_db(path);
    oc_dbwriter *w = oc_dbwriter_start(path);
    CHECK(w != NULL);

    uint64_t alice = reg(w, "ch-alice", "pw", OC_ROLE_MEMBER);
    uint64_t bob   = reg(w, "ch-bob",   "pw", OC_ROLE_MEMBER);
    uint64_t carol = reg(w, "ch-carol", "pw", OC_ROLE_MEMBER);
    CHECK(alice && bob && carol);

    /* alice creates a private channel and auto-joins it. */
    oc_dbres *r = create_channel(w, alice, "secret", 0);
    CHECK(r && r->type == OC_RES_CHANNEL_INFO);
    CHECK(r->ch_is_public == 0 && r->ch_joined == 1);
    uint64_t secret = r->channel_id;
    CHECK(secret != OC_DEFAULT_CHANNEL);
    oc_dbres_free(r);

    /* An empty name is rejected. */
    r = create_channel(w, alice, "", 0);
    CHECK(r && r->type == OC_RES_CHANNEL_ERR && r->err_code == OC_ERR_INVALID_CHANNEL);
    oc_dbres_free(r);

    /* alice sees general + secret (joined); bob sees general but not secret. */
    int joined = -1;
    r = list_channels(w, alice);
    CHECK(r && list_has(r, OC_DEFAULT_CHANNEL, NULL) && list_has(r, secret, &joined) && joined == 1);
    oc_dbres_free(r);
    r = list_channels(w, bob);
    CHECK(r && list_has(r, OC_DEFAULT_CHANNEL, NULL) && !list_has(r, secret, NULL));
    oc_dbres_free(r);

    /* bob cannot self-join a private channel, nor post to it. */
    r = chan_ref(w, OC_JOB_JOIN_CHANNEL, bob, secret);
    CHECK(r && r->type == OC_RES_CHANNEL_ERR && r->err_code == OC_ERR_FORBIDDEN);
    oc_dbres_free(r);
    CHECK(send_to(w, bob, secret, "sneaking in") == OC_ERR_NOT_A_MEMBER);

    /* alice invites bob; the result flags bob for the CHANNEL_INFO push. */
    r = chan_member(w, OC_JOB_INVITE_CHANNEL, alice, secret, bob);
    CHECK(r && r->type == OC_RES_CHANNEL_INFO && r->push_user_id == bob);
    oc_dbres_free(r);

    /* Now bob is a member: he can post, and secret shows up joined in his list. */
    CHECK(send_to(w, bob, secret, "made it") == 0);
    r = list_channels(w, bob);
    CHECK(r && list_has(r, secret, &joined) && joined == 1);
    oc_dbres_free(r);

    /* A non-member (carol) cannot invite into secret. */
    r = chan_member(w, OC_JOB_INVITE_CHANNEL, carol, secret, alice);
    CHECK(r && r->type == OC_RES_CHANNEL_ERR && r->err_code == OC_ERR_NOT_A_MEMBER);
    oc_dbres_free(r);

    /* Private read gate: carol (non-member) backfills nothing from secret,
     * even though it has messages. */
    r = backfill(w, carol, secret, 0);
    CHECK(r && r->type == OC_RES_BACKFILL_OK && r->n_replay == 0);
    oc_dbres_free(r);

    /* Public channel: anyone may post (auto-joining) and read. */
    r = create_channel(w, alice, "townhall", 1);
    CHECK(r && r->type == OC_RES_CHANNEL_INFO && r->ch_is_public == 1);
    uint64_t townhall = r->channel_id;
    oc_dbres_free(r);

    /* carol posts to the public channel without an explicit join -> auto-joined. */
    CHECK(send_to(w, carol, townhall, "hello town") == 0);
    r = list_channels(w, carol);
    CHECK(r && list_has(r, townhall, &joined) && joined == 1);
    oc_dbres_free(r);

    /* carol can also read the public channel by backfill. */
    r = backfill(w, carol, townhall, 0);
    CHECK(r && r->type == OC_RES_BACKFILL_OK && r->n_replay >= 1);
    oc_dbres_free(r);

    /* Posting to a channel that does not exist is UNKNOWN_CHANNEL. */
    CHECK(send_to(w, alice, 99999, "void") == OC_ERR_UNKNOWN_CHANNEL);

    /* bob leaves secret: he loses post access again. */
    r = chan_ref(w, OC_JOB_LEAVE_CHANNEL, bob, secret);
    CHECK(r && r->type == OC_RES_CHANNEL_INFO && r->ch_joined == 0);
    oc_dbres_free(r);
    CHECK(send_to(w, bob, secret, "back in?") == OC_ERR_NOT_A_MEMBER);

    /* alice re-invites bob, then removes him: post access is revoked. */
    r = chan_member(w, OC_JOB_INVITE_CHANNEL, alice, secret, bob);
    CHECK(r && r->type == OC_RES_CHANNEL_INFO);
    oc_dbres_free(r);
    CHECK(send_to(w, bob, secret, "member again") == 0);
    r = chan_member(w, OC_JOB_REMOVE_CHANNEL, alice, secret, bob);
    CHECK(r && r->type == OC_RES_CHANNEL_INFO);
    oc_dbres_free(r);
    CHECK(send_to(w, bob, secret, "removed") == OC_ERR_NOT_A_MEMBER);

    oc_dbwriter_stop(w);
    cleanup_db(path);
}

/* --- Tenant admin ops (REQ-033) ----------------------------------------- */

static oc_dbres *invite_user(oc_dbwriter *w, uint64_t actor, uint8_t role) {
    oc_job *j = oc_job_new(OC_JOB_INVITE_USER, 95);
    j->user_id = actor; j->role = role;
    oc_dbwriter_submit(w, j);
    return wait_result(w);
}

static oc_dbres *redeem_invite(oc_dbwriter *w, const uint8_t *token, const char *user, const char *pass) {
    oc_job *j = oc_job_new(OC_JOB_REDEEM, 96);
    oc_job_set_register(j, user, pass, 0, TEST_PW_ITERS);   /* fast PBKDF2 for tests */
    oc_job_set_token(j, token, OC_INVITE_TOKEN_LEN);
    oc_dbwriter_submit(w, j);
    return wait_result(w);
}

static oc_dbres *list_users(oc_dbwriter *w, uint64_t actor) {
    oc_job *j = oc_job_new(OC_JOB_LIST_USERS, 98);
    j->user_id = actor;
    oc_dbwriter_submit(w, j);
    return wait_result(w);
}

static uint16_t remove_user(oc_dbwriter *w, uint64_t actor, uint64_t target) {
    oc_job *j = oc_job_new(OC_JOB_REMOVE_USER, 97);
    j->user_id = actor; j->target_user_id = target;
    oc_dbwriter_submit(w, j);
    oc_dbres *r = wait_result(w);
    uint16_t code = (r && r->type == OC_RES_USER_UPDATED) ? 0 : (r ? r->err_code : 0xFFFF);
    oc_dbres_free(r);
    return code;
}

static void test_admin_ops(void) {
    const char *path = "build/test_dbwriter_admin.db";
    cleanup_db(path);
    oc_dbwriter *w = oc_dbwriter_start(path);
    CHECK(w != NULL);

    uint64_t owner  = reg(w, "ad-owner",  "pw", OC_ROLE_OWNER);
    uint64_t admin  = reg(w, "ad-admin",  "pw", OC_ROLE_ADMIN);
    uint64_t member = reg(w, "ad-member", "pw", OC_ROLE_MEMBER);
    CHECK(owner && admin && member);

    /* --- Invites --- */
    /* A member cannot invite. */
    oc_dbres *r = invite_user(w, member, OC_ROLE_MEMBER);
    CHECK(r && r->type == OC_RES_INVITE_ERR && r->err_code == OC_ERR_FORBIDDEN);
    oc_dbres_free(r);

    /* An admin can invite a member, but not at an elevated role. */
    r = invite_user(w, admin, OC_ROLE_ADMIN);
    CHECK(r && r->type == OC_RES_INVITE_ERR && r->err_code == OC_ERR_FORBIDDEN);
    oc_dbres_free(r);
    r = invite_user(w, admin, OC_ROLE_MEMBER);
    CHECK(r && r->type == OC_RES_INVITE_OK && r->role == OC_ROLE_MEMBER);
    uint8_t tok_member[OC_INVITE_TOKEN_LEN];
    memcpy(tok_member, r->session_token, OC_INVITE_TOKEN_LEN);
    oc_dbres_free(r);

    /* An owner can invite at an elevated (admin) role. */
    r = invite_user(w, owner, OC_ROLE_ADMIN);
    CHECK(r && r->type == OC_RES_INVITE_OK && r->role == OC_ROLE_ADMIN);
    uint8_t tok_admin[OC_INVITE_TOKEN_LEN];
    memcpy(tok_admin, r->session_token, OC_INVITE_TOKEN_LEN);
    oc_dbres_free(r);

    /* --- Redemption --- */
    /* Redeem the member invite: a new account is created + authenticated. */
    r = redeem_invite(w, tok_member, "newbie", "newpw");
    CHECK(r && r->type == OC_RES_AUTH_OK && r->role == OC_ROLE_MEMBER && r->has_session_token);
    uint64_t newbie = r->user_id;
    CHECK(newbie != 0);
    oc_dbres_free(r);
    /* The new account can now log in with its password. */
    CHECK(auth_local(w, 100, "newbie", "newpw", NULL, NULL) == newbie);

    /* The token is single-use: a second redemption is refused. */
    r = redeem_invite(w, tok_member, "newbie2", "x");
    CHECK(r && r->type == OC_RES_AUTH_ERR && r->err_code == OC_ERR_AUTH_INVALID_TOKEN);
    oc_dbres_free(r);

    /* The admin invite yields an admin account. */
    r = redeem_invite(w, tok_admin, "boss", "bosspw");
    CHECK(r && r->type == OC_RES_AUTH_OK && r->role == OC_ROLE_ADMIN);
    oc_dbres_free(r);

    /* A redemption onto a taken username is refused and does NOT consume the
     * token (so it can still be redeemed with a free name). */
    r = invite_user(w, owner, OC_ROLE_MEMBER);
    CHECK(r && r->type == OC_RES_INVITE_OK);
    uint8_t tok_reuse[OC_INVITE_TOKEN_LEN];
    memcpy(tok_reuse, r->session_token, OC_INVITE_TOKEN_LEN);
    oc_dbres_free(r);
    r = redeem_invite(w, tok_reuse, "ad-owner", "whatever");   /* username already exists */
    CHECK(r && r->type == OC_RES_AUTH_ERR && r->err_code == OC_ERR_AUTH_INVALID_TOKEN);
    oc_dbres_free(r);
    r = redeem_invite(w, tok_reuse, "fresh", "freshpw");       /* same token, free name */
    CHECK(r && r->type == OC_RES_AUTH_OK);
    oc_dbres_free(r);

    /* A garbage token is refused. */
    uint8_t bogus[OC_INVITE_TOKEN_LEN]; memset(bogus, 0xEE, sizeof bogus);
    r = redeem_invite(w, bogus, "ghost", "x");
    CHECK(r && r->type == OC_RES_AUTH_ERR && r->err_code == OC_ERR_AUTH_INVALID_TOKEN);
    oc_dbres_free(r);

    /* --- Listing --- */
    r = list_users(w, member);   /* any authed user may list */
    CHECK(r && r->type == OC_RES_USER_LIST);
    int saw_owner = 0, saw_newbie = 0;
    for (size_t i = 0; i < r->n_ulist; i++) {
        if (r->ulist[i].user_id == owner)  { saw_owner = 1; CHECK(r->ulist[i].role == OC_ROLE_OWNER); }
        if (r->ulist[i].user_id == newbie) saw_newbie = 1;
    }
    CHECK(saw_owner && saw_newbie);
    oc_dbres_free(r);

    /* --- Removal --- */
    /* Give the member a live session, to prove removal revokes it. */
    uint8_t mtok[OC_SESSION_TOKEN_LEN]; memset(mtok, 0, sizeof mtok);
    CHECK(auth_local(w, 101, "ad-member", "pw", mtok, NULL) == member);
    CHECK(auth_session(w, 102, mtok) == member);

    /* A member cannot remove anyone; an admin cannot remove an owner. */
    CHECK(remove_user(w, member, admin) == OC_ERR_FORBIDDEN);
    CHECK(remove_user(w, admin, owner) == OC_ERR_FORBIDDEN);
    /* The last owner cannot remove themselves. */
    CHECK(remove_user(w, owner, owner) == OC_ERR_LAST_OWNER);

    /* An owner removes the member: session revoked, password gone, locked out. */
    CHECK(remove_user(w, owner, member) == 0);
    CHECK(auth_session(w, 103, mtok) == 0);                      /* session revoked */
    CHECK(auth_local(w, 104, "ad-member", "pw", NULL, NULL) == 0); /* login refused */

    oc_dbwriter_stop(w);
    cleanup_db(path);
}

/* --- Reactions (REQ-070/071) -------------------------------------------- */

static oc_dbres *react(oc_dbwriter *w, uint64_t uid, uint64_t ch, uint64_t mid,
                       const char *emoji, uint8_t op) {
    oc_job *j = oc_job_new(OC_JOB_REACT, 110);
    j->user_id = uid; j->channel_id = ch; j->message_id = mid; j->react_op = op;
    j->emoji = strdup(emoji);
    oc_dbwriter_submit(w, j);
    return wait_result(w);
}

static oc_dbres *list_reactions_r(oc_dbwriter *w, uint64_t uid, uint64_t ch, uint64_t mid) {
    oc_job *j = oc_job_new(OC_JOB_LIST_REACTIONS, 111);
    j->user_id = uid; j->channel_id = ch; j->message_id = mid;
    oc_dbwriter_submit(w, j);
    return wait_result(w);
}

static uint64_t send_id(oc_dbwriter *w, uint64_t uid, uint64_t ch, const char *body) {
    oc_job *j = oc_job_new(OC_JOB_SEND, 112);
    j->user_id = uid; j->channel_id = ch;
    memset(j->idem, ++g_send_seq, OC_IDEM_LEN);
    oc_job_set_body(j, body, strlen(body));
    oc_dbwriter_submit(w, j);
    oc_dbres *r = wait_result(w);
    uint64_t id = (r && r->type == OC_RES_SEND_OK) ? r->message_id : 0;
    oc_dbres_free(r);
    return id;
}

static void test_reactions(void) {
    const char *path = "build/test_dbwriter_react.db";
    cleanup_db(path);
    oc_dbwriter *w = oc_dbwriter_start(path);
    CHECK(w != NULL);

    uint64_t alice = reg(w, "rx-alice", "pw", OC_ROLE_OWNER);
    uint64_t bob   = reg(w, "rx-bob",   "pw", OC_ROLE_MEMBER);
    uint64_t carol = reg(w, "rx-carol", "pw", OC_ROLE_MEMBER);
    CHECK(alice && bob && carol);

    uint8_t idem[OC_IDEM_LEN]; memset(idem, 0xC1, sizeof idem);
    uint64_t mid = send_msg(w, alice, idem, "react to me");
    CHECK(mid != 0);

    /* alice adds :+1:; the aggregate is 1, and the fan-out spans all members. */
    oc_dbres *r = react(w, alice, OC_DEFAULT_CHANNEL, mid, ":+1:", OC_REACT_ADD);
    CHECK(r && r->type == OC_RES_REACTION_OK);
    CHECK(r->react_op == OC_REACT_ADD && r->react_count == 1 && r->user_id == alice);
    CHECK(r->message_id == mid && r->n_members == 3);
    CHECK(strcmp(r->emoji, ":+1:") == 0);
    oc_dbres_free(r);

    /* bob adds the same emoji -> aggregate 2. */
    r = react(w, bob, OC_DEFAULT_CHANNEL, mid, ":+1:", OC_REACT_ADD);
    CHECK(r && r->type == OC_RES_REACTION_OK && r->react_count == 2);
    oc_dbres_free(r);

    /* A repeat add by alice does not stack (still 2, REQ-070). */
    r = react(w, alice, OC_DEFAULT_CHANNEL, mid, ":+1:", OC_REACT_ADD);
    CHECK(r && r->type == OC_RES_REACTION_OK && r->react_count == 2);
    oc_dbres_free(r);

    /* A distinct emoji aggregates separately. */
    r = react(w, carol, OC_DEFAULT_CHANNEL, mid, ":tada:", OC_REACT_ADD);
    CHECK(r && r->type == OC_RES_REACTION_OK && r->react_count == 1);
    oc_dbres_free(r);

    /* Inspect: three rows (:+1:/alice, :+1:/bob, :tada:/carol), ordered. */
    r = list_reactions_r(w, bob, OC_DEFAULT_CHANNEL, mid);
    CHECK(r && r->type == OC_RES_REACTIONS && r->n_rlist == 3);
    int plus = 0, tada = 0;
    for (size_t i = 0; i < r->n_rlist; i++) {
        if (strcmp(r->rlist[i].emoji, ":+1:") == 0) plus++;
        if (strcmp(r->rlist[i].emoji, ":tada:") == 0) tada++;
    }
    CHECK(plus == 2 && tada == 1);
    oc_dbres_free(r);

    /* Toggle off: bob removes :+1: -> aggregate 1. */
    r = react(w, bob, OC_DEFAULT_CHANNEL, mid, ":+1:", OC_REACT_REMOVE);
    CHECK(r && r->type == OC_RES_REACTION_OK && r->react_op == OC_REACT_REMOVE && r->react_count == 1);
    oc_dbres_free(r);

    /* An empty or oversized emoji is refused. */
    r = react(w, alice, OC_DEFAULT_CHANNEL, mid, "", OC_REACT_ADD);
    CHECK(r && r->type == OC_RES_REACTION_ERR && r->err_code == OC_ERR_INVALID_REACTION);
    oc_dbres_free(r);
    r = react(w, alice, OC_DEFAULT_CHANNEL, mid, "0123456789012345678901234567890123", OC_REACT_ADD);
    CHECK(r && r->type == OC_RES_REACTION_ERR && r->err_code == OC_ERR_INVALID_REACTION);
    oc_dbres_free(r);

    /* Reacting to a message that does not exist is UNKNOWN_MESSAGE. */
    r = react(w, alice, OC_DEFAULT_CHANNEL, 99999, ":+1:", OC_REACT_ADD);
    CHECK(r && r->type == OC_RES_REACTION_ERR && r->err_code == OC_ERR_UNKNOWN_MESSAGE);
    oc_dbres_free(r);

    /* Private read gate: a non-member cannot react to, or inspect, a message in
     * a private channel they do not belong to (REQ-070/031). */
    r = create_channel(w, alice, "vault", 0);
    CHECK(r && r->type == OC_RES_CHANNEL_INFO);
    uint64_t vault = r->channel_id;
    oc_dbres_free(r);
    uint64_t pmid = send_id(w, alice, vault, "secret");
    CHECK(pmid != 0);
    r = react(w, carol, vault, pmid, ":eyes:", OC_REACT_ADD);
    CHECK(r && r->type == OC_RES_REACTION_ERR && r->err_code == OC_ERR_NOT_A_MEMBER);
    oc_dbres_free(r);
    r = list_reactions_r(w, carol, vault, pmid);
    CHECK(r && r->type == OC_RES_REACTION_ERR && r->err_code == OC_ERR_NOT_A_MEMBER);
    oc_dbres_free(r);

    /* Deleting a message clears its reactions and blocks new ones (REQ-052). */
    r = do_delete(w, alice, OC_DEFAULT_CHANNEL, mid);
    CHECK(r && r->type == OC_RES_DELETE_OK);
    oc_dbres_free(r);
    r = list_reactions_r(w, alice, OC_DEFAULT_CHANNEL, mid);
    CHECK(r && r->type == OC_RES_REACTIONS && r->n_rlist == 0);
    oc_dbres_free(r);
    r = react(w, alice, OC_DEFAULT_CHANNEL, mid, ":+1:", OC_REACT_ADD);
    CHECK(r && r->type == OC_RES_REACTION_ERR && r->err_code == OC_ERR_UNKNOWN_MESSAGE);
    oc_dbres_free(r);

    oc_dbwriter_stop(w);
    cleanup_db(path);
}

/* --- Threads (REQ-060) -------------------------------------------------- */

static oc_dbres *send_reply(oc_dbwriter *w, uint64_t uid, uint64_t ch, uint64_t parent, const char *body) {
    oc_job *j = oc_job_new(OC_JOB_SEND_REPLY, 120);
    j->user_id = uid; j->channel_id = ch; j->parent_id = parent;
    memset(j->idem, ++g_send_seq, OC_IDEM_LEN);
    oc_job_set_body(j, body, strlen(body));
    oc_dbwriter_submit(w, j);
    return wait_result(w);
}

static oc_dbres *list_thread_r(oc_dbwriter *w, uint64_t uid, uint64_t ch, uint64_t parent) {
    oc_job *j = oc_job_new(OC_JOB_LIST_THREAD, 121);
    j->user_id = uid; j->channel_id = ch; j->parent_id = parent;
    oc_dbwriter_submit(w, j);
    return wait_result(w);
}

static void test_threads(void) {
    const char *path = "build/test_dbwriter_threads.db";
    cleanup_db(path);
    oc_dbwriter *w = oc_dbwriter_start(path);
    CHECK(w != NULL);

    uint64_t alice = reg(w, "th-alice", "pw", OC_ROLE_OWNER);
    uint64_t bob   = reg(w, "th-bob",   "pw", OC_ROLE_MEMBER);
    CHECK(alice && bob);

    uint8_t idem[OC_IDEM_LEN]; memset(idem, 0xD1, sizeof idem);
    uint64_t mid = send_msg(w, alice, idem, "top-level");
    CHECK(mid != 0);

    /* bob replies under the top-level message: reply threads under mid, count 1. */
    oc_dbres *r = send_reply(w, bob, OC_DEFAULT_CHANNEL, mid, "reply one");
    CHECK(r && r->type == OC_RES_REPLY_OK);
    CHECK(r->parent_id == mid && r->reply_count == 1 && r->message_id > mid);
    CHECK(r->n_members == 2);
    uint64_t reply1 = r->message_id;
    oc_dbres_free(r);

    /* alice replies too -> count 2. */
    r = send_reply(w, alice, OC_DEFAULT_CHANNEL, mid, "reply two");
    CHECK(r && r->type == OC_RES_REPLY_OK && r->reply_count == 2);
    oc_dbres_free(r);

    /* Replying to a reply flattens to the root (parent_id stays mid), count 3. */
    r = send_reply(w, bob, OC_DEFAULT_CHANNEL, reply1, "nested");
    CHECK(r && r->type == OC_RES_REPLY_OK && r->parent_id == mid && r->reply_count == 3);
    oc_dbres_free(r);

    /* Inspect the thread: three replies, oldest first, no error. */
    r = list_thread_r(w, alice, OC_DEFAULT_CHANNEL, mid);
    CHECK(r && r->type == OC_RES_THREAD && r->err_code == 0 && r->n_thread == 3);
    CHECK(r->thread[0].message_id == reply1);
    CHECK(r->thread[0].body_len == 9 && memcmp(r->thread[0].body, "reply one", 9) == 0);
    oc_dbres_free(r);

    /* Replying to a non-existent message is UNKNOWN_MESSAGE. */
    r = send_reply(w, alice, OC_DEFAULT_CHANNEL, 99999, "into the void");
    CHECK(r && r->type == OC_RES_REPLY_ERR && r->err_code == OC_ERR_UNKNOWN_MESSAGE);
    oc_dbres_free(r);

    /* Backfill of the main scroll excludes replies but carries the reply count:
     * only the top-level message replays, with reply_count 3. */
    r = backfill(w, alice, OC_DEFAULT_CHANNEL, 0);
    CHECK(r && r->type == OC_RES_BACKFILL_OK && r->n_replay == 1);
    CHECK(r->replay[0].message_id == mid && r->replay[0].reply_count == 3);
    CHECK(r->replay[0].last_reply_at != 0);
    oc_dbres_free(r);

    /* A tombstoned parent cannot be replied to. */
    r = do_delete(w, alice, OC_DEFAULT_CHANNEL, mid);
    CHECK(r && r->type == OC_RES_DELETE_OK);
    oc_dbres_free(r);
    r = send_reply(w, alice, OC_DEFAULT_CHANNEL, mid, "too late");
    CHECK(r && r->type == OC_RES_REPLY_ERR && r->err_code == OC_ERR_UNKNOWN_MESSAGE);
    oc_dbres_free(r);

    /* Private read gate: a non-member cannot reply to, or open, a thread in a
     * channel they do not belong to. */
    r = create_channel(w, alice, "war-room", 0);
    CHECK(r && r->type == OC_RES_CHANNEL_INFO);
    uint64_t priv = r->channel_id;
    oc_dbres_free(r);
    uint64_t pmid = send_id(w, alice, priv, "classified");
    CHECK(pmid != 0);
    r = send_reply(w, bob, priv, pmid, "intruding");
    CHECK(r && r->type == OC_RES_REPLY_ERR && r->err_code == OC_ERR_NOT_A_MEMBER);
    oc_dbres_free(r);
    r = list_thread_r(w, bob, priv, pmid);
    CHECK(r && r->type == OC_RES_THREAD && r->err_code == OC_ERR_NOT_A_MEMBER);
    oc_dbres_free(r);

    oc_dbwriter_stop(w);
    cleanup_db(path);
}

/* --- Full-text search (REQ-080) ----------------------------------------- */

static oc_dbres *search(oc_dbwriter *w, uint64_t uid, const char *query, uint16_t limit) {
    oc_job *j = oc_job_new(OC_JOB_SEARCH, 130);
    j->user_id = uid; j->search_limit = limit;
    oc_job_set_body(j, query, strlen(query));
    oc_dbwriter_submit(w, j);
    return wait_result(w);
}

static void test_search(void) {
    const char *path = "build/test_dbwriter_search.db";
    cleanup_db(path);
    oc_dbwriter *w = oc_dbwriter_start(path);
    CHECK(w != NULL);

    uint64_t alice = reg(w, "se-alice", "pw", OC_ROLE_OWNER);
    uint64_t bob   = reg(w, "se-bob",   "pw", OC_ROLE_MEMBER);
    CHECK(alice && bob);

    uint8_t idem[OC_IDEM_LEN];
    memset(idem, 1, sizeof idem); uint64_t m_fox    = send_msg(w, alice, idem, "the quick brown fox");
    memset(idem, 2, sizeof idem); uint64_t m_deploy = send_msg(w, alice, idem, "deploy the pipeline");
    memset(idem, 3, sizeof idem); uint64_t m_lunch  = send_msg(w, alice, idem, "lunch plans today");
    CHECK(m_fox && m_deploy && m_lunch);

    /* A term matches the one message that contains it, returned as a snippet. */
    oc_dbres *r = search(w, alice, "deploy", 0);
    CHECK(r && r->type == OC_RES_SEARCH && r->n_search == 1);
    CHECK(r->search[0].message_id == m_deploy && r->search[0].body_len > 0);
    oc_dbres_free(r);

    /* A common term matches multiple messages, newest first (id DESC). */
    r = search(w, alice, "the", 0);
    CHECK(r && r->n_search == 2);
    CHECK(r->search[0].message_id == m_deploy && r->search[1].message_id == m_fox);
    oc_dbres_free(r);

    /* Another member of the (public default) channel sees the same history. */
    r = search(w, bob, "quick", 0);
    CHECK(r && r->n_search == 1 && r->search[0].message_id == m_fox);
    oc_dbres_free(r);

    /* Member scoping: a private channel's messages are invisible to non-members. */
    r = create_channel(w, alice, "vault", 0);
    CHECK(r && r->type == OC_RES_CHANNEL_INFO);
    uint64_t vault = r->channel_id;
    oc_dbres_free(r);
    uint64_t m_secret = send_id(w, alice, vault, "secret sauce recipe");
    CHECK(m_secret != 0);
    r = search(w, bob, "secret", 0);
    CHECK(r && r->n_search == 0);                       /* bob is not a member */
    oc_dbres_free(r);
    r = search(w, alice, "secret", 0);
    CHECK(r && r->n_search == 1 && r->search[0].message_id == m_secret);
    oc_dbres_free(r);

    /* An edit re-indexes: the old term stops matching, the new term matches. */
    r = do_edit(w, alice, OC_DEFAULT_CHANNEL, m_deploy, "shipping the release");
    CHECK(r && r->type == OC_RES_EDIT_OK);
    oc_dbres_free(r);
    r = search(w, alice, "deploy", 0);
    CHECK(r && r->n_search == 0);
    oc_dbres_free(r);
    r = search(w, alice, "release", 0);
    CHECK(r && r->n_search == 1 && r->search[0].message_id == m_deploy);
    oc_dbres_free(r);

    /* A tombstoned message drops out of the index (REQ-052). */
    r = do_delete(w, alice, OC_DEFAULT_CHANNEL, m_lunch);
    CHECK(r && r->type == OC_RES_DELETE_OK);
    oc_dbres_free(r);
    r = search(w, alice, "lunch", 0);
    CHECK(r && r->n_search == 0);
    oc_dbres_free(r);

    /* An empty query returns nothing; punctuation-only input cannot crash the
     * FTS grammar (it is quoted term-by-term). */
    r = search(w, alice, "", 0);
    CHECK(r && r->type == OC_RES_SEARCH && r->n_search == 0);
    oc_dbres_free(r);
    r = search(w, alice, "\"(* :^", 0);
    CHECK(r && r->type == OC_RES_SEARCH && r->n_search == 0);
    oc_dbres_free(r);

    oc_dbwriter_stop(w);
    cleanup_db(path);
}

/* Persisted TLS identity (ARCH-66b): store + load round-trips through the DB so
 * a database restored onto a new box keeps the same TOFU cert. */
static void test_tls_identity(void) {
    const char *path = "build/test_dbwriter_identity.db";
    cleanup_db(path);
    oc_dbwriter *w = oc_dbwriter_start(path);
    CHECK(w != NULL);

    /* Fresh tenant: nothing stored. */
    char *c = (char *)1, *k = (char *)1;
    CHECK(oc_dbwriter_load_identity(w, &c, &k) == 0);
    CHECK(c == NULL && k == NULL);

    /* Store, then load returns the same PEMs. */
    CHECK(oc_dbwriter_store_identity(w, "CERT-PEM-DATA", "KEY-PEM-DATA") == 1);
    CHECK(oc_dbwriter_load_identity(w, &c, &k) == 1);
    CHECK(c && k && strcmp(c, "CERT-PEM-DATA") == 0 && strcmp(k, "KEY-PEM-DATA") == 0);
    free(c); free(k);

    /* Re-store replaces the single row. */
    CHECK(oc_dbwriter_store_identity(w, "CERT2", "KEY2") == 1);
    CHECK(oc_dbwriter_load_identity(w, &c, &k) == 1 && strcmp(c, "CERT2") == 0);
    free(c); free(k);

    oc_dbwriter_stop(w);
    cleanup_db(path);
}

/* First-owner setup token (REQ-024): a fresh tenant mints a one-time owner
 * invite; redeeming it creates the owner; afterward none is minted. */
static void test_setup_invite(void) {
    const char *path = "build/test_dbwriter_setup.db";
    cleanup_db(path);
    oc_dbwriter *w = oc_dbwriter_start(path);
    CHECK(w != NULL);

    /* No owner yet -> a setup token is minted. */
    uint8_t tok[OC_INVITE_TOKEN_LEN];
    CHECK(oc_dbwriter_setup_invite(w, tok) == 1);

    /* Redeeming it creates the first owner. */
    oc_dbres *r = redeem_invite(w, tok, "founder", "founder-pw");
    CHECK(r && r->type == OC_RES_AUTH_OK && r->role == OC_ROLE_OWNER);
    oc_dbres_free(r);

    /* An owner now exists -> nothing is minted. */
    CHECK(oc_dbwriter_setup_invite(w, tok) == 0);

    oc_dbwriter_stop(w);
    cleanup_db(path);
}

/* Idempotency-map pruning (ARCH-44): a (channel, token) mapping older than the
 * retention window is dropped, so a much-later retry of the same token is no
 * longer deduplicated, while a recent token still is. */
static void test_idem_pruning(void) {
    const char *path = "build/test_dbwriter_prune.db";
    cleanup_db(path);
    oc_dbwriter *w = oc_dbwriter_start(path);
    CHECK(w != NULL);
    /* Tiny retention + prune-every-write so the test can observe it. */
    /* A generous retention window: the "still deduplicated" checks below do
     * several async round-trips that must land inside it, so a tight window
     * (e.g. 50 ms) races the clock on a slow/loaded CI runner. */
    oc_dbwriter_set_idem_retention(w, 1000 /*ms*/, 0 /*interval*/);

    uint64_t uid = reg(w, "pr-user", "pw", OC_ROLE_MEMBER);
    CHECK(uid != 0);

    uint8_t tokA[OC_IDEM_LEN], tokB[OC_IDEM_LEN];
    memset(tokA, 0xA1, sizeof tokA);
    memset(tokB, 0xB2, sizeof tokB);

    uint64_t m1 = send_msg(w, uid, tokA, "first");
    CHECK(m1 != 0);
    /* Fresh token is still deduplicated. */
    CHECK(send_msg(w, uid, tokA, "first-again") == m1);

    usleep(1300000);   /* age tokA past the 1000ms retention */

    /* A new send triggers the prune, dropping tokA's aged mapping. */
    uint64_t m2 = send_msg(w, uid, tokB, "second");
    CHECK(m2 > m1);

    /* tokA is no longer deduplicated -> a retry allocates a fresh id. */
    uint64_t m3 = send_msg(w, uid, tokA, "first-retry");
    CHECK(m3 != m1 && m3 > m2);

    /* tokB, still within retention, is deduplicated. */
    CHECK(send_msg(w, uid, tokB, "second-again") == m2);

    oc_dbwriter_stop(w);
    cleanup_db(path);
}

/* Server-side delivery accounting (REQ-090): CLIENT_ACK advances a per-(user,
 * channel) cursor, and a cursorless (count=0) backfill resumes each member
 * channel from that cursor. */
static void client_ack(oc_dbwriter *w, uint64_t uid, uint64_t channel, uint64_t mid) {
    oc_job *j = oc_job_new(OC_JOB_CLIENT_ACK, 1);
    j->user_id = uid; j->channel_id = channel; j->message_id = mid;
    oc_dbwriter_submit(w, j);
    /* A real advance now yields a READ_CURSOR result (REQ-090 seen-by); drain it
     * so it doesn't bleed into the next helper's result. A non-advancing ack
     * yields nothing, so the poll is bounded and simply times out. */
    for (int i = 0; i < 50; i++) {
        oc_dbres *r = oc_dbwriter_next_result(w);
        if (r) { oc_dbres_free(r); return; }
        usleep(2000);
    }
}

static oc_dbres *backfill0(oc_dbwriter *w, uint64_t uid) {
    oc_job *j = oc_job_new(OC_JOB_BACKFILL, 1);   /* n_cursors = 0 */
    j->user_id = uid;
    oc_dbwriter_submit(w, j);
    return wait_result(w);
}

static void test_delivery_cursor(void) {
    const char *path = "build/test_dbwriter_delivery.db";
    cleanup_db(path);
    oc_dbwriter *w = oc_dbwriter_start(path);
    CHECK(w != NULL);

    uint64_t alice = reg(w, "dc-alice", "pw", OC_ROLE_OWNER);
    uint64_t bob   = reg(w, "dc-bob",   "pw", OC_ROLE_MEMBER);
    CHECK(alice && bob);

    uint8_t idem[OC_IDEM_LEN];
    memset(idem, 1, sizeof idem); uint64_t m1 = send_msg(w, alice, idem, "one");
    memset(idem, 2, sizeof idem); uint64_t m2 = send_msg(w, alice, idem, "two");
    memset(idem, 3, sizeof idem); uint64_t m3 = send_msg(w, alice, idem, "three");
    CHECK(m1 && m2 > m1 && m3 > m2);

    /* No prior ack: a cursorless backfill replays the whole channel from 0. */
    oc_dbres *r = backfill0(w, bob);
    CHECK(r && r->type == OC_RES_BACKFILL_OK && r->n_replay == 3);
    oc_dbres_free(r);

    /* bob acks m2; a synchronous send afterwards flushes the writer past the
     * ack (FIFO), so the reader sees the committed cursor. */
    client_ack(w, bob, OC_DEFAULT_CHANNEL, m2);
    memset(idem, 4, sizeof idem); uint64_t m4 = send_msg(w, alice, idem, "four");
    CHECK(m4 > m3);

    /* Now a cursorless backfill resumes after m2 -> only m3 and m4. */
    r = backfill0(w, bob);
    CHECK(r && r->type == OC_RES_BACKFILL_OK && r->n_replay == 2);
    CHECK(r->replay[0].message_id == m3 && r->replay[1].message_id == m4);
    oc_dbres_free(r);

    /* The cursor only advances: acking an older id does not rewind it. */
    client_ack(w, bob, OC_DEFAULT_CHANNEL, m1);
    memset(idem, 5, sizeof idem); (void)send_msg(w, alice, idem, "five");   /* barrier */
    r = backfill0(w, bob);
    CHECK(r && r->n_replay == 3);   /* still resuming after m2: m3, m4, m5 */
    oc_dbres_free(r);

    oc_dbwriter_stop(w);
    cleanup_db(path);
}

/* --- Direct messages (REQ-050) ------------------------------------------ */

static oc_dbres *open_dm(oc_dbwriter *w, uint64_t actor, uint64_t target) {
    oc_job *j = oc_job_new(OC_JOB_OPEN_DM, 55);
    j->user_id = actor; j->target_user_id = target;
    oc_dbwriter_submit(w, j);
    return wait_result(w);
}

/* Attachment metadata jobs (REQ-140/141, ARCH-69/70). The blob bytes never touch
 * the dbwriter — these jobs mint/finalize the pointer row and authorize a
 * download by the ordinary channel-read gate. */
static void test_attachments(void) {
    const char *path = "build/test_dbwriter_attach.db";
    cleanup_db(path);
    oc_dbwriter *w = oc_dbwriter_start(path);
    CHECK(w != NULL);

    uint64_t alice = reg(w, "at-alice", "pw", OC_ROLE_OWNER);
    uint64_t bob   = reg(w, "at-bob",   "pw", OC_ROLE_MEMBER);
    CHECK(alice && bob);

    uint8_t idem[OC_IDEM_LEN]; memset(idem, 0x5A, sizeof idem);
    uint8_t sha[32]; for (int i = 0; i < 32; i++) sha[i] = (uint8_t)i;

    /* CREATE a pending attachment on the default (public) channel. */
    oc_job *j = oc_job_new(OC_JOB_ATTACH_CREATE, 1);
    j->user_id = alice; j->channel_id = OC_DEFAULT_CHANNEL; j->att_size = 2048;
    memcpy(j->idem, idem, sizeof idem);
    j->filename = strdup("notes.txt"); j->mime = strdup("text/plain");
    oc_dbwriter_submit(w, j);
    oc_dbres *r = wait_result(w);
    CHECK(r && r->type == OC_RES_ATTACH_CREATED);
    CHECK(r->attachment_id > 0 && r->storage_key && r->storage_key[0]);
    uint64_t aid = r->attachment_id;
    oc_dbres_free(r);

    /* A download LOOKUP before finalize fails — the blob is not complete. */
    j = oc_job_new(OC_JOB_ATTACH_LOOKUP, 2);
    j->user_id = bob; j->attachment_id = aid;
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_ATTACH_ERR && r->err_code == OC_ERR_UNKNOWN_ATTACHMENT);
    oc_dbres_free(r);

    /* Only the uploader may finalize, and the streamed size must match. */
    j = oc_job_new(OC_JOB_ATTACH_FINALIZE, 3);
    j->user_id = bob; j->attachment_id = aid; j->att_size = 2048; memcpy(j->att_sha256, sha, 32);
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_ATTACH_ERR && r->err_code == OC_ERR_UNKNOWN_ATTACHMENT); /* not owner */
    oc_dbres_free(r);

    j = oc_job_new(OC_JOB_ATTACH_FINALIZE, 4);
    j->user_id = alice; j->attachment_id = aid; j->att_size = 999; memcpy(j->att_sha256, sha, 32);
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_ATTACH_ERR && r->err_code == OC_ERR_TRANSFER_PROTOCOL); /* size mismatch */
    oc_dbres_free(r);

    j = oc_job_new(OC_JOB_ATTACH_FINALIZE, 5);
    j->user_id = alice; j->attachment_id = aid; j->att_size = 2048; memcpy(j->att_sha256, sha, 32);
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_ATTACH_OK && r->att_size == 2048);
    oc_dbres_free(r);

    /* Double-finalize is refused (already complete). */
    j = oc_job_new(OC_JOB_ATTACH_FINALIZE, 6);
    j->user_id = alice; j->attachment_id = aid; j->att_size = 2048; memcpy(j->att_sha256, sha, 32);
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_ATTACH_ERR && r->err_code == OC_ERR_UNKNOWN_ATTACHMENT);
    oc_dbres_free(r);

    /* bob is a member of the public default channel, so his LOOKUP is authorized
     * and returns the pointer + metadata (REQ-141). */
    j = oc_job_new(OC_JOB_ATTACH_LOOKUP, 7);
    j->user_id = bob; j->attachment_id = aid;
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_ATTACH_META);
    CHECK(r->channel_id == OC_DEFAULT_CHANNEL && r->att_size == 2048);
    CHECK(r->filename && strcmp(r->filename, "notes.txt") == 0);
    CHECK(r->mime && strcmp(r->mime, "text/plain") == 0);
    CHECK(r->storage_key && r->storage_key[0]);
    CHECK(memcmp(r->att_sha256, sha, 32) == 0);
    oc_dbres_free(r);

    /* Linking (REQ-140): a SEND referencing the finalized attachment links it and
     * the broadcast carries its metadata inline. */
    uint8_t sidem[OC_IDEM_LEN]; memset(sidem, 0x10, sizeof sidem);
    oc_job *sj = oc_job_new(OC_JOB_SEND, 20);
    sj->user_id = alice; sj->channel_id = OC_DEFAULT_CHANNEL;
    memcpy(sj->idem, sidem, OC_IDEM_LEN);
    oc_job_set_body(sj, "file!", 5);
    sj->attach_ids[0] = aid; sj->n_attach = 1;
    oc_dbwriter_submit(w, sj);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_SEND_OK && r->n_attach == 1);
    CHECK(r->attach[0].id == aid && r->attach[0].size == 2048);
    CHECK(r->attach[0].filename && strcmp(r->attach[0].filename, "notes.txt") == 0);
    uint64_t linked_mid = r->message_id;
    oc_dbres_free(r);

    /* The attachment is now linked; a later SEND can't re-link it. */
    memset(sidem, 0x11, sizeof sidem);
    sj = oc_job_new(OC_JOB_SEND, 21);
    sj->user_id = alice; sj->channel_id = OC_DEFAULT_CHANNEL;
    memcpy(sj->idem, sidem, OC_IDEM_LEN);
    oc_job_set_body(sj, "again", 5);
    sj->attach_ids[0] = aid; sj->n_attach = 1;
    oc_dbwriter_submit(w, sj);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_SEND_OK && r->n_attach == 0);
    oc_dbres_free(r);

    /* A reconnecting member sees the attachment inline via backfill. */
    r = backfill(w, bob, OC_DEFAULT_CHANNEL, linked_mid - 1);
    CHECK(r && r->type == OC_RES_BACKFILL_OK);
    int saw = 0;
    for (size_t i = 0; i < r->n_replay; i++) {
        if (r->replay[i].message_id == linked_mid) {
            saw = 1;
            CHECK(r->replay[i].n_attach == 1 && r->replay[i].attach[0].id == aid);
            CHECK(r->replay[i].attach[0].size == 2048);
        }
    }
    CHECK(saw);
    oc_dbres_free(r);

    /* Thread-reply attachments (REQ-140): a fresh attachment linked from a reply
     * appears on REPLY_OK and in the thread listing. */
    oc_job *cj = oc_job_new(OC_JOB_ATTACH_CREATE, 30);
    cj->user_id = alice; cj->channel_id = OC_DEFAULT_CHANNEL; cj->att_size = 64;
    memset(cj->idem, 0x5C, OC_IDEM_LEN);
    cj->filename = strdup("r.bin"); cj->mime = strdup("application/octet-stream");
    oc_dbwriter_submit(w, cj);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_ATTACH_CREATED);
    uint64_t aid2 = r->attachment_id;
    oc_dbres_free(r);
    cj = oc_job_new(OC_JOB_ATTACH_FINALIZE, 31);
    cj->user_id = alice; cj->attachment_id = aid2; cj->att_size = 64; memcpy(cj->att_sha256, sha, 32);
    oc_dbwriter_submit(w, cj);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_ATTACH_OK);
    oc_dbres_free(r);

    sj = oc_job_new(OC_JOB_SEND_REPLY, 32);
    sj->user_id = alice; sj->channel_id = OC_DEFAULT_CHANNEL; sj->parent_id = linked_mid;
    memset(sj->idem, 0x5D, OC_IDEM_LEN);
    oc_job_set_body(sj, "reply", 5);
    sj->attach_ids[0] = aid2; sj->n_attach = 1;
    oc_dbwriter_submit(w, sj);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_REPLY_OK && r->n_attach == 1 && r->attach[0].id == aid2);
    uint64_t reply_mid = r->message_id;
    oc_dbres_free(r);

    sj = oc_job_new(OC_JOB_LIST_THREAD, 33);
    sj->user_id = bob; sj->channel_id = OC_DEFAULT_CHANNEL; sj->parent_id = linked_mid;
    oc_dbwriter_submit(w, sj);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_THREAD);
    int sawr = 0;
    for (size_t i = 0; i < r->n_thread; i++) {
        if (r->thread[i].message_id == reply_mid) {
            sawr = 1;
            CHECK(r->thread[i].n_attach == 1 && r->thread[i].attach[0].id == aid2);
        }
    }
    CHECK(sawr);
    oc_dbres_free(r);

    /* Access control: an attachment on a private channel alice creates is NOT
     * downloadable by bob, a non-member (the core of REQ-141). */
    r = create_channel(w, alice, "secret", 0);
    CHECK(r && r->type == OC_RES_CHANNEL_INFO && r->ch_is_public == 0);
    uint64_t priv = r->channel_id;
    oc_dbres_free(r);

    j = oc_job_new(OC_JOB_ATTACH_CREATE, 8);
    j->user_id = alice; j->channel_id = priv; j->att_size = 10;
    memset(j->idem, 0x77, OC_IDEM_LEN);
    j->filename = strdup("s.txt"); j->mime = strdup("text/plain");
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_ATTACH_CREATED);
    uint64_t paid = r->attachment_id;
    oc_dbres_free(r);

    j = oc_job_new(OC_JOB_ATTACH_FINALIZE, 9);
    j->user_id = alice; j->attachment_id = paid; j->att_size = 10; memcpy(j->att_sha256, sha, 32);
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_ATTACH_OK);
    oc_dbres_free(r);

    j = oc_job_new(OC_JOB_ATTACH_LOOKUP, 10);
    j->user_id = bob; j->attachment_id = paid;
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_ATTACH_ERR && r->err_code == OC_ERR_FORBIDDEN);
    oc_dbres_free(r);

    /* An oversized declared upload is refused up front. */
    j = oc_job_new(OC_JOB_ATTACH_CREATE, 11);
    j->user_id = alice; j->channel_id = OC_DEFAULT_CHANNEL;
    j->att_size = OC_MAX_ATTACHMENT_SIZE + 1;
    memset(j->idem, 0x33, OC_IDEM_LEN);
    j->filename = strdup("big.bin"); j->mime = strdup("application/octet-stream");
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_ATTACH_ERR && r->err_code == OC_ERR_ATTACHMENT_TOO_LARGE);
    oc_dbres_free(r);

    /* An unknown attachment id looks up as unknown. */
    j = oc_job_new(OC_JOB_ATTACH_LOOKUP, 12);
    j->user_id = alice; j->attachment_id = 999999;
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_ATTACH_ERR && r->err_code == OC_ERR_UNKNOWN_ATTACHMENT);
    oc_dbres_free(r);

    oc_dbwriter_stop(w);
    cleanup_db(path);
}

/* Incoming webhooks (REQ-170): minting a token and posting via it resolve to a
 * message in the scoped channel authored by the webhook's creator. */
static void test_webhooks(void) {
    const char *path = "build/test_dbwriter_webhook.db";
    cleanup_db(path);
    oc_dbwriter *w = oc_dbwriter_start(path);
    CHECK(w != NULL);

    uint64_t alice = reg(w, "wh-alice", "pw", OC_ROLE_OWNER);
    uint64_t bob   = reg(w, "wh-bob",   "pw", OC_ROLE_MEMBER);
    CHECK(alice && bob);

    /* Mint a webhook on the default (public) channel. */
    oc_job *j = oc_job_new(OC_JOB_CREATE_WEBHOOK, 1);
    j->user_id = alice; j->channel_id = OC_DEFAULT_CHANNEL; j->ch_name = strdup("ci");
    oc_dbwriter_submit(w, j);
    oc_dbres *r = wait_result(w);
    CHECK(r && r->type == OC_RES_WEBHOOK_CREATED && r->message_id > 0);
    uint64_t wid = r->message_id;
    uint8_t token[OC_SESSION_TOKEN_LEN];
    memcpy(token, r->session_token, sizeof token);
    oc_dbres_free(r);

    /* Post via the token: a message in the channel authored by the creator. */
    j = oc_job_new(OC_JOB_WEBHOOK_POST, 2);
    oc_job_set_token(j, token, sizeof token);
    oc_job_set_body(j, "from ci", 7);
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_WEBHOOK_POSTED);
    CHECK(r->channel_id == OC_DEFAULT_CHANNEL && r->author_id == alice && r->message_id > 0);
    CHECK(r->body_len == 7 && memcmp(r->body, "from ci", 7) == 0);
    CHECK(r->author_name && strcmp(r->author_name, "ci") == 0);   /* display-name override */
    int saw_alice = 0;
    for (size_t i = 0; i < r->n_members; i++) if (r->members[i] == alice) saw_alice = 1;
    CHECK(saw_alice);
    oc_dbres_free(r);

    /* An unknown token is refused. */
    uint8_t bad[OC_SESSION_TOKEN_LEN]; memset(bad, 0xEE, sizeof bad);
    j = oc_job_new(OC_JOB_WEBHOOK_POST, 3);
    oc_job_set_token(j, bad, sizeof bad);
    oc_job_set_body(j, "x", 1);
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_WEBHOOK_ERR && r->err_code == OC_ERR_UNKNOWN_WEBHOOK);
    oc_dbres_free(r);

    /* Management: the channel's webhooks list (no tokens); delete removes it. */
    j = oc_job_new(OC_JOB_LIST_WEBHOOKS, 5);
    j->user_id = alice; j->channel_id = OC_DEFAULT_CHANNEL;
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_WEBHOOK_LIST && r->n_whlist == 1);
    CHECK(r->whlist[0].id == wid && r->whlist[0].disabled == 0);
    CHECK(r->whlist[0].label && strcmp(r->whlist[0].label, "ci") == 0);
    oc_dbres_free(r);

    j = oc_job_new(OC_JOB_DELETE_WEBHOOK, 6);
    j->user_id = alice; j->message_id = wid;
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_WEBHOOK_DELETED && r->message_id == wid);
    oc_dbres_free(r);

    /* After delete: the token no longer resolves and the list is empty. */
    j = oc_job_new(OC_JOB_WEBHOOK_POST, 7);
    oc_job_set_token(j, token, sizeof token);
    oc_job_set_body(j, "late", 4);
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_WEBHOOK_ERR && r->err_code == OC_ERR_UNKNOWN_WEBHOOK);
    oc_dbres_free(r);

    j = oc_job_new(OC_JOB_LIST_WEBHOOKS, 8);
    j->user_id = alice; j->channel_id = OC_DEFAULT_CHANNEL;
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_WEBHOOK_LIST && r->n_whlist == 0);
    oc_dbres_free(r);

    /* Deleting an unknown id is refused. */
    j = oc_job_new(OC_JOB_DELETE_WEBHOOK, 9);
    j->user_id = alice; j->message_id = 999999;
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_WEBHOOK_ERR && r->err_code == OC_ERR_UNKNOWN_WEBHOOK);
    oc_dbres_free(r);

    /* A non-member cannot mint a webhook on a private channel. */
    r = create_channel(w, alice, "secret", 0);
    CHECK(r && r->type == OC_RES_CHANNEL_INFO);
    uint64_t priv = r->channel_id;
    oc_dbres_free(r);
    j = oc_job_new(OC_JOB_CREATE_WEBHOOK, 4);
    j->user_id = bob; j->channel_id = priv; j->ch_name = strdup("x");
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_WEBHOOK_ERR && r->err_code == OC_ERR_NOT_A_MEMBER);
    oc_dbres_free(r);

    oc_dbwriter_stop(w);
    cleanup_db(path);
}

/* Notification preferences (REQ-130/131): per-channel level + DND, persisted and
 * upserted, gated on channel access. */
static void test_notify_prefs(void) {
    const char *path = "build/test_dbwriter_notify.db";
    cleanup_db(path);
    oc_dbwriter *w = oc_dbwriter_start(path);
    CHECK(w != NULL);
    uint64_t alice = reg(w, "np-alice", "pw", OC_ROLE_OWNER);
    uint64_t bob   = reg(w, "np-bob",   "pw", OC_ROLE_MEMBER);
    CHECK(alice && bob);

    /* Default: no prefs, DND off. */
    oc_job *j = oc_job_new(OC_JOB_LIST_NOTIFY_PREFS, 1);
    j->user_id = alice; oc_dbwriter_submit(w, j);
    oc_dbres *r = wait_result(w);
    CHECK(r && r->type == OC_RES_NOTIFY_PREFS && r->n_nprefs == 0 && r->np_dnd_enabled == 0);
    oc_dbres_free(r);

    /* Set a channel level; the snapshot reflects it. */
    j = oc_job_new(OC_JOB_SET_NOTIFY_PREF, 2);
    j->user_id = alice; j->channel_id = OC_DEFAULT_CHANNEL; j->notify_level = OC_NOTIFY_MENTIONS;
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_NOTIFY_PREFS && r->n_nprefs == 1);
    CHECK(r->nprefs[0].channel_id == OC_DEFAULT_CHANNEL && r->nprefs[0].level == OC_NOTIFY_MENTIONS);
    oc_dbres_free(r);

    /* Set DND; the pref persists alongside. */
    j = oc_job_new(OC_JOB_SET_DND, 3);
    j->user_id = alice; j->dnd_enabled = 1; j->dnd_start_min = 1320; j->dnd_end_min = 480;
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_NOTIFY_PREFS && r->np_dnd_enabled == 1);
    CHECK(r->np_dnd_start_min == 1320 && r->np_dnd_end_min == 480 && r->n_nprefs == 1);
    oc_dbres_free(r);

    /* Re-setting the level upserts (no duplicate row). */
    j = oc_job_new(OC_JOB_SET_NOTIFY_PREF, 4);
    j->user_id = alice; j->channel_id = OC_DEFAULT_CHANNEL; j->notify_level = OC_NOTIFY_NONE;
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->n_nprefs == 1 && r->nprefs[0].level == OC_NOTIFY_NONE);
    oc_dbres_free(r);

    /* An invalid level is refused. */
    j = oc_job_new(OC_JOB_SET_NOTIFY_PREF, 5);
    j->user_id = alice; j->channel_id = OC_DEFAULT_CHANNEL; j->notify_level = 9;
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_NOTIFY_ERR);
    oc_dbres_free(r);

    /* A channel the user can't read is refused. */
    r = create_channel(w, bob, "priv", 0);
    CHECK(r && r->type == OC_RES_CHANNEL_INFO);
    uint64_t priv = r->channel_id;
    oc_dbres_free(r);
    j = oc_job_new(OC_JOB_SET_NOTIFY_PREF, 6);
    j->user_id = alice; j->channel_id = priv; j->notify_level = OC_NOTIFY_ALL;
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_NOTIFY_ERR && r->err_code == OC_ERR_NOT_A_MEMBER);
    oc_dbres_free(r);

    oc_dbwriter_stop(w);
    cleanup_db(path);
}

static void test_dm(void) {
    const char *path = "build/test_dbwriter_dm.db";
    cleanup_db(path);
    oc_dbwriter *w = oc_dbwriter_start(path);
    CHECK(w != NULL);

    uint64_t alice = reg(w, "dm-alice", "pw", OC_ROLE_OWNER);
    uint64_t bob   = reg(w, "dm-bob",   "pw", OC_ROLE_MEMBER);
    uint64_t carol = reg(w, "dm-carol", "pw", OC_ROLE_MEMBER);
    CHECK(alice && bob && carol);

    /* Open a DM alice<->bob: a kind=DM channel, alice joined, bob flagged for push. */
    oc_dbres *r = open_dm(w, alice, bob);
    CHECK(r && r->type == OC_RES_CHANNEL_INFO);
    CHECK(r->ch_kind == OC_CHANNEL_KIND_DM && r->ch_joined == 1 && r->push_user_id == bob);
    uint64_t dm = r->channel_id;
    CHECK(dm != OC_DEFAULT_CHANNEL);
    oc_dbres_free(r);

    /* Idempotent: re-opening (either direction) returns the same channel. */
    r = open_dm(w, alice, bob);
    CHECK(r && r->type == OC_RES_CHANNEL_INFO && r->channel_id == dm);
    oc_dbres_free(r);
    r = open_dm(w, bob, alice);
    CHECK(r && r->type == OC_RES_CHANNEL_INFO && r->channel_id == dm);
    oc_dbres_free(r);

    /* A self-DM (notes to self, REQ-055) is a single-participant DM, distinct
     * from the alice<->bob DM, and idempotent. */
    r = open_dm(w, alice, alice);
    CHECK(r && r->type == OC_RES_CHANNEL_INFO && r->ch_kind == OC_CHANNEL_KIND_DM);
    CHECK(r->ch_joined == 1 && r->push_user_id == 0);   /* no peer to push to */
    uint64_t selfdm = r->channel_id;
    CHECK(selfdm != dm);
    oc_dbres_free(r);
    r = open_dm(w, alice, alice);
    CHECK(r && r->type == OC_RES_CHANNEL_INFO && r->channel_id == selfdm);
    oc_dbres_free(r);
    /* alice can post to and read her self-DM; nobody else can read it. */
    uint64_t smid = send_id(w, alice, selfdm, "note to self");
    CHECK(smid != 0);
    r = backfill(w, alice, selfdm, 0);
    CHECK(r && r->n_replay == 1 && r->replay[0].message_id == smid);
    oc_dbres_free(r);
    r = backfill(w, bob, selfdm, 0);
    CHECK(r && r->n_replay == 0);
    oc_dbres_free(r);

    /* Opening a DM with an unknown user is still refused. */
    r = open_dm(w, alice, 99999);
    CHECK(r && r->type == OC_RES_CHANNEL_ERR && r->err_code == OC_ERR_FORBIDDEN);
    oc_dbres_free(r);

    /* Messaging works through the DM channel; only the two participants read it. */
    uint64_t mid = send_id(w, alice, dm, "hey bob");
    CHECK(mid != 0);
    r = backfill(w, bob, dm, 0);
    CHECK(r && r->type == OC_RES_BACKFILL_OK && r->n_replay == 1 && r->replay[0].message_id == mid);
    oc_dbres_free(r);
    r = backfill(w, carol, dm, 0);                         /* carol is not a participant */
    CHECK(r && r->type == OC_RES_BACKFILL_OK && r->n_replay == 0);
    oc_dbres_free(r);
    CHECK(send_to(w, carol, dm, "eavesdrop") == OC_ERR_NOT_A_MEMBER);

    /* The DM shows in the participants' channel list (kind=DM), not carol's. */
    int joined = -1;
    r = list_channels(w, alice);
    CHECK(r && list_has(r, dm, &joined) && joined == 1);
    int kind_ok = 0;
    for (size_t i = 0; i < r->n_chlist; i++) if (r->chlist[i].channel_id == dm) kind_ok = (r->chlist[i].kind == OC_CHANNEL_KIND_DM);
    CHECK(kind_ok);
    oc_dbres_free(r);
    r = list_channels(w, carol);
    CHECK(r && !list_has(r, dm, NULL));
    oc_dbres_free(r);

    /* A DM is not a named channel: channel-management ops reject it. */
    r = chan_ref(w, OC_JOB_JOIN_CHANNEL, carol, dm);
    CHECK(r && r->type == OC_RES_CHANNEL_ERR && r->err_code == OC_ERR_UNKNOWN_CHANNEL);
    oc_dbres_free(r);

    oc_dbwriter_stop(w);
    cleanup_db(path);
}

/* CP-7: the registered-user cap (OPENCHIME_MAX_USERS). */
static void test_max_users(void) {
    const char *path = "build/test_dbwriter_cap.db";
    cleanup_db(path);
    oc_dbwriter *w = oc_dbwriter_start(path);
    CHECK(w != NULL);
    oc_dbwriter_set_max_users(w, 2);

    uint64_t u1 = reg(w, "alice", "pw", OC_ROLE_OWNER);
    uint64_t u2 = reg(w, "bob", "pw", OC_ROLE_MEMBER);
    CHECK(u1 != 0);
    CHECK(u2 != 0);

    /* A third new user is refused at the cap. */
    CHECK(reg(w, "carol", "pw", OC_ROLE_MEMBER) == 0);

    /* Re-registering an existing user is idempotent, never capped. */
    CHECK(reg(w, "alice", "pw", OC_ROLE_OWNER) == u1);

    /* Lifting the cap (0 = unlimited) lets the new user in. */
    oc_dbwriter_set_max_users(w, 0);
    CHECK(reg(w, "carol", "pw", OC_ROLE_MEMBER) != 0);

    oc_dbwriter_stop(w);
    cleanup_db(path);
}

int run_dbwriter_tests(void) {
    printf("test_dbwriter: migrate-on-boot, register + local/session/oidc auth, rate-limit, roles, SEND persist/idempotency/members, backfill\n");
    test_start_migrates_and_stops();
    test_auth_and_send();
    test_oidc_auth();
    test_auth_rate_limit();
    test_source_rate_limit();
    test_logout();
    test_role_enforcement();
    test_edit_delete();
    test_channels();
    test_dm();
    test_attachments();
    test_webhooks();
    test_notify_prefs();
    test_admin_ops();
    test_reactions();
    test_threads();
    test_search();
    test_setup_invite();
    test_tls_identity();
    test_delivery_cursor();
    test_idem_pruning();
    test_backfill();
    test_max_users();
    return failures;
}
