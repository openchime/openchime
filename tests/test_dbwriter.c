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
    CHECK(oc_schema_version(db) == 6);
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
    test_admin_ops();
    test_reactions();
    test_threads();
    test_search();
    test_backfill();
    return failures;
}
