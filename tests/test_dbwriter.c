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
    CHECK(oc_schema_version(db) == 2);
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

    /* A non-member author is rejected. */
    j = oc_job_new(OC_JOB_SEND, 99);
    j->user_id = 9999; j->channel_id = OC_DEFAULT_CHANNEL;
    memset(j->idem, 0x33, OC_IDEM_LEN);
    oc_job_set_body(j, "x", 1);
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_SEND_ERR && r->err_code == OC_ERR_NOT_A_MEMBER);
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

int run_dbwriter_tests(void) {
    printf("test_dbwriter: migrate-on-boot, register + local/session/oidc auth, SEND persist/idempotency/members, backfill\n");
    test_start_migrates_and_stops();
    test_auth_and_send();
    test_oidc_auth();
    test_backfill();
    return failures;
}
