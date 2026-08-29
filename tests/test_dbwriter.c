/* Tests for the DB-writer thread (dbwriter.c): migrate-on-boot lifecycle, and
 * the AUTH / SEND job processing (idempotency, membership, broadcast fan-out
 * list) exercised directly through the job queue — no network involved.
 * Includes the code under test directly; links sqlite + pthread. */

#include "dbwriter.h"
#include "migrate.h"
#include "protocol.h"
#include "issuer.h"
#include "check.h"
#include <time.h>
#include "mention.h"

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
    CHECK(oc_schema_version(db) == 39);
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

static void client_ack(oc_dbwriter *w, uint64_t uid, uint64_t channel, uint64_t mid);

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

    /* A cursor of 0 means "I hold no history" and must yield the channel's
     * NEWEST page, bounded by OC_BACKFILL_TAIL — never the oldest one, and never
     * nothing. Push the channel well past the tail size to tell them apart. */
    enum { BF_TAIL = 60 };
    uint64_t last = m3;
    for (int i = 0; i < BF_TAIL + 20; i++) {
        char body[32]; snprintf(body, sizeof body, "bulk-%d", i);
        memset(idem, 0, sizeof idem); idem[0] = (uint8_t)i; idem[1] = 0xB5;
        last = send_msg(w, u, idem, body);
        CHECK(last != 0);
    }
    r = backfill(w, u, OC_DEFAULT_CHANNEL, 0);
    CHECK(r && r->n_replay == BF_TAIL);
    CHECK(r->replay[BF_TAIL - 1].message_id == last);            /* ends at newest */
    CHECK(r->replay[0].message_id > m3);                         /* not the oldest page */
    oc_dbres_free(r);

    /* And a user who is fully caught up STILL gets that page. Resuming from the
     * stored read cursor would send them nothing, which is how a client that
     * keeps no local history (ARCH-88) ended up showing an empty channel on
     * every launch. The read cursor places the unread divider; it does not
     * decide which messages exist. */
    client_ack(w, u, OC_DEFAULT_CHANNEL, last);
    r = backfill(w, u, OC_DEFAULT_CHANNEL, 0);
    CHECK(r && r->n_replay == BF_TAIL);
    CHECK(r->replay[BF_TAIL - 1].message_id == last);
    oc_dbres_free(r);

    /* A non-zero cursor keeps its literal meaning: only what came after it, so a
     * reconnecting client that still holds history gets no duplicate replay. */
    r = backfill(w, u, OC_DEFAULT_CHANNEL, last);
    CHECK(r && r->n_replay == 0);
    oc_dbres_free(r);

    /* And the CURSORLESS form — the one a cold client actually sends — must
     * behave the same. It derives one cursor per member channel, and those must
     * be 0 rather than the stored delivery cursor: seeding them from the read
     * position is the same "caught-up user sees nothing" bug one level up. */
    {
        oc_job *j = oc_job_new(OC_JOB_BACKFILL, 300);
        j->user_id = u;
        j->n_cursors = 0;
        j->cursors = NULL;
        oc_dbwriter_submit(w, j);
        oc_dbres *bf = wait_result(w);
        CHECK(bf && bf->type == OC_RES_BACKFILL_OK);
        CHECK(bf->n_replay == BF_TAIL);
        CHECK(bf->replay[BF_TAIL - 1].message_id == last);
        oc_dbres_free(bf);
    }

    oc_dbwriter_stop(w);
    cleanup_db(path);
}

/* Paging BACKWARDS (§6.3). A cursorless backfill only ever hands out the
 * newest page, so without this a client could never reach older history at all. */
static oc_dbres *history(oc_dbwriter *w, uint64_t uid, uint64_t ch,
                         uint64_t before, uint16_t limit) {
    oc_job *j = oc_job_new(OC_JOB_HISTORY, 310);
    j->user_id = uid; j->channel_id = ch; j->message_id = before; j->search_limit = limit;
    oc_dbwriter_submit(w, j);
    return wait_result(w);
}

static void test_history_paging(void) {
    const char *path = "build/test_dbwriter_history.db";
    cleanup_db(path);
    oc_dbwriter *w = oc_dbwriter_start(path);
    CHECK(w != NULL);

    uint64_t u = reg(w, "hp-user", "pw", OC_ROLE_MEMBER);
    uint64_t outsider = reg(w, "hp-out", "pw", OC_ROLE_MEMBER);
    CHECK(u && outsider);

    uint64_t ids[25];
    uint8_t idem[OC_IDEM_LEN];
    for (int i = 0; i < 25; i++) {
        char body[32]; snprintf(body, sizeof body, "h%d", i);
        memset(idem, 0, sizeof idem); idem[0] = (uint8_t)i; idem[1] = 0x7A;
        ids[i] = send_msg(w, u, idem, body);
        CHECK(ids[i] != 0);
    }

    /* before = 0 means "from the newest": the last 10, ascending. */
    oc_dbres *r = history(w, u, OC_DEFAULT_CHANNEL, 0, 10);
    CHECK(r && r->type == OC_RES_BACKFILL_OK && r->n_replay == 10);
    CHECK(r->replay[0].message_id == ids[15] && r->replay[9].message_id == ids[24]);
    CHECK(r->truncated == 1);            /* more exists above */
    uint64_t oldest = r->replay[0].message_id;
    oc_dbres_free(r);

    /* The next page up is strictly older, and still ascending. */
    r = history(w, u, OC_DEFAULT_CHANNEL, oldest, 10);
    CHECK(r && r->n_replay == 10);
    CHECK(r->replay[0].message_id == ids[5] && r->replay[9].message_id == ids[14]);
    CHECK(r->truncated == 1);
    oldest = r->replay[0].message_id;
    oc_dbres_free(r);

    /* The last page is short and reports nothing above it, which is how a client
     * knows to stop asking. */
    r = history(w, u, OC_DEFAULT_CHANNEL, oldest, 10);
    CHECK(r && r->n_replay == 5);
    CHECK(r->replay[0].message_id == ids[0]);
    CHECK(r->truncated == 0);
    oc_dbres_free(r);

    /* Past the top: empty, not an error. */
    r = history(w, u, OC_DEFAULT_CHANNEL, ids[0], 10);
    CHECK(r && r->type == OC_RES_BACKFILL_OK && r->n_replay == 0);
    oc_dbres_free(r);

    /* Read access is enforced here as everywhere else. */
    r = history(w, outsider, 4242, 0, 10);
    CHECK(r && r->n_replay == 0);
    oc_dbres_free(r);

    /* A PAGE CARRIES THE SAME SIDE-STATE A RECONNECT DOES (REQ-070/230/231).
     *
     * The reconnect replay filled reactions, pins and saved marks; this path
     * filled none of them, so every message reached by scrolling back — or by
     * following a permalink — rendered permanently without its reactions and
     * pins. A client stores nothing (ARCH-88), so "permanently" is literal:
     * there is no second source to recover them from.
     *
     * Asserted on the PAGE rather than on the tables, because the tables were
     * always right; what was missing was the replay carrying them. */
    {
        uint64_t mid = ids[20];   /* in the newest page: ids[15]..ids[24] */
        {
            oc_job *j = oc_job_new(OC_JOB_REACT, 311);
            j->user_id = u; j->channel_id = OC_DEFAULT_CHANNEL; j->message_id = mid;
            j->react_op = OC_REACT_ADD; j->emoji = strdup("\xF0\x9F\x91\x8D");
            oc_dbwriter_submit(w, j);
            oc_dbres_free(wait_result(w));
        }
        {   /* Inline rather than the `pin` helper, which is declared below. */
            oc_job *j = oc_job_new(OC_JOB_PIN, 314);
            j->user_id = u; j->channel_id = OC_DEFAULT_CHANNEL; j->message_id = mid;
            j->pin_op = OC_PIN_ADD;
            oc_dbwriter_submit(w, j);
            oc_dbres_free(wait_result(w));
        }
        {
            oc_job *j = oc_job_new(OC_JOB_SAVE_ITEM, 312);
            j->user_id = u; j->message_id = mid; j->save_op = OC_SAVE_ADD;
            oc_dbwriter_submit(w, j);
            oc_dbres_free(wait_result(w));
        }

        r = history(w, u, OC_DEFAULT_CHANNEL, 0, 10);
        CHECK(r && r->n_replay > 0);
        int found = 0, pinned = 0, saved = 0;
        for (size_t i = 0; i < r->n_replay; i++)
            if (r->replay[i].message_id == mid) {
                found = 1;
                pinned = r->replay[i].pinned_by == u && r->replay[i].pinned_at != 0;
                saved  = r->replay[i].saved && r->replay[i].saved_at != 0;
            }
        CHECK(found);
        CHECK(pinned);                       /* the pin rides the page */
        CHECK(saved);                        /* and this user's saved mark */
        int reacted = 0;
        for (size_t i = 0; i < r->n_rreact; i++)
            if (r->rreact[i].message_id == mid && r->rreact[i].count == 1 &&
                r->rreact[i].user_id == u) reacted = 1;
        CHECK(reacted);                      /* and the reaction aggregate */
        oc_dbres_free(r);

        /* The around-an-id mode is the same replay through a different window
         * (ARCH-96), and shares the projection — so it carries them too. */
        {
            oc_job *j = oc_job_new(OC_JOB_HISTORY, 313);
            j->user_id = u; j->channel_id = OC_DEFAULT_CHANNEL;
            j->message_id = mid; j->search_limit = 10; j->hist_around = 1;
            oc_dbwriter_submit(w, j);
            r = wait_result(w);
        }
        int apinned = 0, areacted = 0;
        for (size_t i = 0; r && i < r->n_replay; i++)
            if (r->replay[i].message_id == mid && r->replay[i].pinned_by == u) apinned = 1;
        for (size_t i = 0; r && i < r->n_rreact; i++)
            if (r->rreact[i].message_id == mid) areacted = 1;
        CHECK(apinned);
        CHECK(areacted);
        oc_dbres_free(r);
    }

    oc_dbwriter_stop(w);
    cleanup_db(path);
}

/* Pins (REQ-230, ARCH-90). The interesting cases are the ones where a pin is
 * NOT simply a row: the per-channel cap, a repeat pin, another member removing
 * someone else's pin, and a pin surviving a reload. */
static oc_dbres *pin(oc_dbwriter *w, uint64_t uid, uint64_t ch, uint64_t mid, uint8_t op) {
    oc_job *j = oc_job_new(OC_JOB_PIN, 230);
    j->user_id = uid; j->channel_id = ch; j->message_id = mid; j->pin_op = op;
    oc_dbwriter_submit(w, j);
    return wait_result(w);
}

static oc_dbres *list_pins(oc_dbwriter *w, uint64_t uid, uint64_t ch) {
    oc_job *j = oc_job_new(OC_JOB_LIST_PINS, 231);
    j->user_id = uid; j->channel_id = ch;
    oc_dbwriter_submit(w, j);
    return wait_result(w);
}

static void test_pins(void) {
    const char *path = "build/test_dbwriter_pins.db";
    cleanup_db(path);
    oc_dbwriter *w = oc_dbwriter_start(path);
    CHECK(w != NULL);

    uint64_t alice = reg(w, "alice", "pw", OC_ROLE_OWNER);
    uint64_t bob   = reg(w, "bob",   "pw", OC_ROLE_MEMBER);
    CHECK(alice && bob);

    uint8_t idem[OC_IDEM_LEN];
    memset(idem, 0xB1, sizeof idem);
    uint64_t m1 = send_msg(w, alice, idem, "the deploy runbook");
    memset(idem, 0xB2, sizeof idem);
    uint64_t m2 = send_msg(w, bob, idem, "and the rollback steps");
    CHECK(m1 && m2);

    oc_dbres *r = pin(w, alice, OC_DEFAULT_CHANNEL, m1, OC_PIN_ADD);
    CHECK(r && r->type == OC_RES_PIN_OK && r->pin_op == OC_PIN_ADD);
    CHECK(r->user_id == alice && r->pinned_at != 0);
    /* The fan-out must reach every member, not just the pinner: a pin is
     * channel state. */
    CHECK(r->n_members >= 2);
    oc_dbres_free(r);

    /* Pinning again is a no-op, and critically reports the ORIGINAL pinner and
     * time — if it reported the re-pinner, two clients would disagree with what
     * a later LIST_PINS says. */
    r = pin(w, bob, OC_DEFAULT_CHANNEL, m1, OC_PIN_ADD);
    CHECK(r && r->type == OC_RES_PIN_OK && r->user_id == alice);
    oc_dbres_free(r);

    r = list_pins(w, bob, OC_DEFAULT_CHANNEL);
    CHECK(r && r->type == OC_RES_PINS && r->n_plist == 1);
    CHECK(r->plist[0].message_id == m1 && r->plist[0].pinned_by == alice);
    /* The body travels with the pin, so opening the list is one round trip
     * even when the message is far out of the loaded history. */
    CHECK(r->plist[0].body && strcmp(r->plist[0].body, "the deploy runbook") == 0);
    oc_dbres_free(r);

    /* Anyone may unpin, including someone else's pin — Slack's default, and the
     * reason is that a pin only its author can remove outlives them. */
    r = pin(w, bob, OC_DEFAULT_CHANNEL, m1, OC_PIN_REMOVE);
    CHECK(r && r->type == OC_RES_PIN_OK && r->pin_op == OC_PIN_REMOVE);
    oc_dbres_free(r);
    r = list_pins(w, alice, OC_DEFAULT_CHANNEL);
    CHECK(r && r->n_plist == 0);
    oc_dbres_free(r);

    /* Unpinning something that is not pinned is a no-op, not an error: two
     * clients racing the same unpin must not produce a spurious failure. */
    r = pin(w, alice, OC_DEFAULT_CHANNEL, m1, OC_PIN_REMOVE);
    CHECK(r && r->type == OC_RES_PIN_OK);
    oc_dbres_free(r);

    /* A message that does not exist, and a non-member, are both refused. */
    r = pin(w, alice, OC_DEFAULT_CHANNEL, 999999, OC_PIN_ADD);
    CHECK(r && r->type == OC_RES_PIN_ERR && r->err_code == OC_ERR_UNKNOWN_MESSAGE);
    oc_dbres_free(r);

    /* Pin state must survive a reload. A BROADCAST has no field for it, so the
     * backfill carries it — without this every pin silently vanished when a
     * client reconnected. */
    r = pin(w, alice, OC_DEFAULT_CHANNEL, m2, OC_PIN_ADD);
    CHECK(r && r->type == OC_RES_PIN_OK);
    oc_dbres_free(r);
    r = backfill(w, alice, OC_DEFAULT_CHANNEL, 0);
    CHECK(r && r->n_replay >= 2);
    int seen_pinned = 0;
    for (size_t i = 0; i < r->n_replay; i++)
        if (r->replay[i].message_id == m2) {
            seen_pinned = 1;
            CHECK(r->replay[i].pinned_by == alice && r->replay[i].pinned_at != 0);
        } else if (r->replay[i].message_id == m1) {
            CHECK(r->replay[i].pinned_by == 0);   /* unpinned above, stays unpinned */
        }
    CHECK(seen_pinned);
    oc_dbres_free(r);

    /* A deleted message cannot stay pinned: there is no body left to pin to. */
    {
        oc_job *j = oc_job_new(OC_JOB_DELETE, 232);
        j->user_id = bob; j->channel_id = OC_DEFAULT_CHANNEL; j->message_id = m2;
        oc_dbwriter_submit(w, j);
        oc_dbres_free(wait_result(w));
    }
    r = list_pins(w, alice, OC_DEFAULT_CHANNEL);
    CHECK(r && r->n_plist == 0);
    oc_dbres_free(r);

    /* The per-channel cap. Pin OC_MAX_PINS distinct messages, then one more. */
    uint64_t last = 0;
    for (unsigned i = 0; i < OC_MAX_PINS; i++) {
        uint8_t id2[OC_IDEM_LEN];
        memset(id2, 0, sizeof id2);
        id2[0] = 0xC0; id2[1] = (uint8_t)(i & 0xFF); id2[2] = (uint8_t)(i >> 8);
        char body[32];
        snprintf(body, sizeof body, "pin me %u", i);
        uint64_t mid = send_msg(w, alice, id2, body);
        CHECK(mid != 0);
        r = pin(w, alice, OC_DEFAULT_CHANNEL, mid, OC_PIN_ADD);
        CHECK(r && r->type == OC_RES_PIN_OK);
        oc_dbres_free(r);
        last = mid;
    }
    CHECK(last != 0);
    {
        uint8_t id3[OC_IDEM_LEN];
        memset(id3, 0xD1, sizeof id3);
        uint64_t over = send_msg(w, alice, id3, "one too many");
        r = pin(w, alice, OC_DEFAULT_CHANNEL, over, OC_PIN_ADD);
        CHECK(r && r->type == OC_RES_PIN_ERR && r->err_code == OC_ERR_TOO_MANY_PINS);
        oc_dbres_free(r);
    }
    /* But re-pinning something already in the set is still fine at the cap —
     * it adds nothing, so refusing it would be a false failure. */
    r = pin(w, alice, OC_DEFAULT_CHANNEL, last, OC_PIN_ADD);
    CHECK(r && r->type == OC_RES_PIN_OK);
    oc_dbres_free(r);

    /* The list is capped and newest-pin-first. */
    r = list_pins(w, alice, OC_DEFAULT_CHANNEL);
    CHECK(r && r->n_plist == OC_MAX_PINS);
    oc_dbres_free(r);

    oc_dbwriter_stop(w);
    cleanup_db(path);
}

/* @mentions resolved and stored on send (REQ-221, ARCH-89). The scanner itself
 * is covered in test_mention; this is about RESOLUTION — which names become
 * rows, which do not, and why. */
static int mention_rows(const char *path, uint64_t mid, uint64_t user_id, int kind) {
    sqlite3 *raw = NULL;
    if (sqlite3_open(path, &raw) != SQLITE_OK) return -1;
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(raw,
        "SELECT COUNT(*) FROM mentions WHERE message_id=?1 "
        "  AND (?2 = 0 OR user_id = ?2) AND (?3 < 0 OR kind = ?3);", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)mid);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)user_id);
    sqlite3_bind_int(st, 3, kind);
    int n = (sqlite3_step(st) == SQLITE_ROW) ? sqlite3_column_int(st, 0) : -1;
    sqlite3_finalize(st);
    sqlite3_close(raw);
    return n;
}

static oc_dbres *create_channel(oc_dbwriter *w, uint64_t uid, const char *name,
                                uint8_t is_public);   /* fwd */

static void test_mentions_stored(void) {
    const char *path = "build/test_dbwriter_mentions.db";
    cleanup_db(path);
    oc_dbwriter *w = oc_dbwriter_start(path);
    CHECK(w != NULL);

    uint64_t alice = reg(w, "alice", "pw", OC_ROLE_OWNER);
    uint64_t bob   = reg(w, "bob",   "pw", OC_ROLE_MEMBER);
    uint64_t carol = reg(w, "carol", "pw", OC_ROLE_MEMBER);
    CHECK(alice && bob && carol);

    uint8_t idem[OC_IDEM_LEN];
    memset(idem, 0xA1, sizeof idem);
    uint64_t m1 = send_msg(w, alice, idem, "hey @bob can you look");
    CHECK(m1 != 0);
    CHECK(mention_rows(path, m1, bob, OC_MENTION_USER) == 1);
    CHECK(mention_rows(path, m1, 0, -1) == 1);            /* exactly one, not a stray */

    /* Case-insensitive, and the author may mention themselves (the notify path
     * excludes the author separately — that is not the scanner's job). */
    memset(idem, 0xA2, sizeof idem);
    uint64_t m2 = send_msg(w, alice, idem, "@BOB and @alice");
    CHECK(mention_rows(path, m2, bob, OC_MENTION_USER) == 1);
    CHECK(mention_rows(path, m2, alice, OC_MENTION_USER) == 1);

    /* A name nobody has is just text: no row, no notification, no error. */
    memset(idem, 0xA3, sizeof idem);
    uint64_t m3 = send_msg(w, alice, idem, "ping @nobodyhere please");
    CHECK(mention_rows(path, m3, 0, -1) == 0);

    /* An email address is not a mention — the case that would otherwise notify
     * whoever happens to be called "example". */
    memset(idem, 0xA4, sizeof idem);
    uint64_t m4 = send_msg(w, alice, idem, "write to bob@example.com");
    CHECK(mention_rows(path, m4, 0, -1) == 0);

    /* Broadcasts store a row with no user, so the notify decision can expand
     * them without re-parsing the body. */
    memset(idem, 0xA5, sizeof idem);
    uint64_t m5 = send_msg(w, alice, idem, "@channel standup in 5");
    CHECK(mention_rows(path, m5, 0, OC_MENTION_CHANNEL) == 1);

    /* The stored body is untouched — plain UTF-8 with the literal "@bob", so
     * search still finds it and any client can render it without knowing about
     * mentions at all. */
    {
        sqlite3 *raw = NULL;
        CHECK(sqlite3_open(path, &raw) == SQLITE_OK);
        sqlite3_stmt *st = NULL;
        sqlite3_prepare_v2(raw, "SELECT body FROM messages WHERE id=?1;", -1, &st, NULL);
        sqlite3_bind_int64(st, 1, (sqlite3_int64)m1);
        CHECK(sqlite3_step(st) == SQLITE_ROW);
        const void *b = sqlite3_column_blob(st, 0);
        int bl = sqlite3_column_bytes(st, 0);
        CHECK(bl == (int)strlen("hey @bob can you look"));
        CHECK(b && memcmp(b, "hey @bob can you look", (size_t)bl) == 0);
        sqlite3_finalize(st);
        sqlite3_close(raw);
    }

    /* Someone who cannot read the channel is not mentionable there: notifying
     * them about a message they can never open would be worse than silence. */
    oc_dbres *r = create_channel(w, alice, "private-room", 0);
    CHECK(r && r->type == OC_RES_CHANNEL_INFO);
    uint64_t priv = r->channel_id;
    oc_dbres_free(r);
    memset(idem, 0xA6, sizeof idem);
    oc_job *j = oc_job_new(OC_JOB_SEND, 90);
    j->user_id = alice; j->channel_id = priv;
    memcpy(j->idem, idem, OC_IDEM_LEN);
    oc_job_set_body(j, "@carol are you there", strlen("@carol are you there"));
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_SEND_OK);
    uint64_t m6 = r->message_id;
    oc_dbres_free(r);
    CHECK(mention_rows(path, m6, carol, OC_MENTION_USER) == 0);

    /* A REPLY'S MENTIONS ARE STORED TOO (REQ-061/221).
     *
     * They were not, and the consequence was larger than it sounds: the push
     * MENTIONS branch, the activity feed's mention arm and the reader's
     * highlight all read this table, so naming somebody in a thread reached
     * them nowhere at all. A mention in a thread is still a mention. */
    memset(idem, 0xA7, sizeof idem);
    j = oc_job_new(OC_JOB_SEND, 91);
    j->user_id = alice; j->channel_id = OC_DEFAULT_CHANNEL;
    memcpy(j->idem, idem, OC_IDEM_LEN);
    oc_job_set_body(j, "thread root", 11);
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_SEND_OK);
    uint64_t root = r->message_id;
    oc_dbres_free(r);

    memset(idem, 0xA8, sizeof idem);
    j = oc_job_new(OC_JOB_SEND_REPLY, 92);
    j->user_id = alice; j->channel_id = OC_DEFAULT_CHANNEL; j->parent_id = root;
    memcpy(j->idem, idem, OC_IDEM_LEN);
    oc_job_set_body(j, "@bob in a thread", strlen("@bob in a thread"));
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_REPLY_OK);
    uint64_t rmid = r->message_id;
    oc_dbres_free(r);
    CHECK(mention_rows(path, rmid, bob, OC_MENTION_USER) == 1);

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

/* A channel's member roster (REQ-031) and its shared files (REQ-143, ARCH-91).
 * Both are new READ ops over storage that already existed; what matters is that
 * they are scoped to what the caller may see. */
static oc_dbres *list_members(oc_dbwriter *w, uint64_t uid, uint64_t ch) {
    oc_job *j = oc_job_new(OC_JOB_LIST_MEMBERS, 240);
    j->user_id = uid; j->channel_id = ch;
    oc_dbwriter_submit(w, j);
    return wait_result(w);
}

static oc_dbres *list_files(oc_dbwriter *w, uint64_t uid, uint64_t ch) {
    oc_job *j = oc_job_new(OC_JOB_LIST_FILES, 241);
    j->user_id = uid; j->channel_id = ch;
    oc_dbwriter_submit(w, j);
    return wait_result(w);
}

static void test_channel_details(void) {
    const char *path = "build/test_dbwriter_details.db";
    cleanup_db(path);
    oc_dbwriter *w = oc_dbwriter_start(path);
    CHECK(w != NULL);

    uint64_t alice = reg(w, "alice", "pw", OC_ROLE_OWNER);
    uint64_t bob   = reg(w, "bob",   "pw", OC_ROLE_MEMBER);
    uint64_t carol = reg(w, "carol", "pw", OC_ROLE_MEMBER);
    CHECK(alice && bob && carol);

    /* A PRIVATE channel with two of the three users in it. This is the case the
     * old client got wrong: it showed the tenant roster, so carol appeared as a
     * "member" of a channel she cannot even read. */
    oc_dbres *r = create_channel(w, alice, "secret", 0);
    CHECK(r && r->type == OC_RES_CHANNEL_INFO);
    uint64_t secret = r->channel_id;
    oc_dbres_free(r);
    oc_dbres_free(chan_member(w, OC_JOB_INVITE_CHANNEL, alice, secret, bob));

    r = list_members(w, alice, secret);
    CHECK(r && r->type == OC_RES_MEMBER_LIST);
    CHECK(r->n_cmlist == 2);
    int saw_alice = 0, saw_bob = 0, saw_carol = 0;
    for (size_t i = 0; i < r->n_cmlist; i++) {
        if (r->cmlist[i].user_id == alice) { saw_alice = 1; CHECK(r->cmlist[i].role == OC_ROLE_OWNER); }
        if (r->cmlist[i].user_id == bob)   saw_bob = 1;
        if (r->cmlist[i].user_id == carol) saw_carol = 1;
        CHECK(r->cmlist[i].joined_at != 0);
    }
    CHECK(saw_alice && saw_bob && !saw_carol);
    oc_dbres_free(r);

    /* A non-member cannot enumerate it, or this becomes a way to discover who
     * is in a private channel you were never invited to. */
    r = list_members(w, carol, secret);
    CHECK(r && r->type == OC_RES_LIST_ERR && r->err_code == OC_ERR_NOT_A_MEMBER);
    oc_dbres_free(r);

    /* Files. Rows are written directly here because the upload path is a
     * multi-frame protocol exercised in its own suite; what is under test is
     * the LISTING — its scope, its ordering, and what it leaves out. */
    {
        sqlite3 *raw = NULL;
        CHECK(sqlite3_open(path, &raw) == SQLITE_OK);
        const char *ins =
            "INSERT INTO attachments(id,channel_id,message_id,uploader_id,storage_key,"
            "  filename,mime,size,created_at_ms,reclaimed_at_ms) VALUES(?,?,?,?,'k',?,?,?,?,?);";
        struct { int id; uint64_t ch; int mid; const char *fn; const char *mt; int ts; int rec; } rows[] = {
            { 1, secret,  10, "old.pdf",   "application/pdf", 1000, 0 },
            { 2, secret,  11, "new.png",   "image/png",       3000, 0 },
            { 3, secret,  12, "gone.zip",  "application/zip", 2000, 9999 },
            { 4, secret,   0, "pending.doc","application/msword", 4000, 0 },  /* never sent */
            { 5, OC_DEFAULT_CHANNEL, 13, "elsewhere.txt", "text/plain", 5000, 0 },
        };
        for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
            sqlite3_stmt *st = NULL;
            CHECK(sqlite3_prepare_v2(raw, ins, -1, &st, NULL) == SQLITE_OK);
            sqlite3_bind_int64(st, 1, rows[i].id);
            sqlite3_bind_int64(st, 2, (sqlite3_int64)rows[i].ch);
            if (rows[i].mid) sqlite3_bind_int64(st, 3, rows[i].mid); else sqlite3_bind_null(st, 3);
            sqlite3_bind_int64(st, 4, (sqlite3_int64)alice);
            sqlite3_bind_text(st, 5, rows[i].fn, -1, SQLITE_STATIC);
            sqlite3_bind_text(st, 6, rows[i].mt, -1, SQLITE_STATIC);
            sqlite3_bind_int64(st, 7, 1234);
            sqlite3_bind_int64(st, 8, rows[i].ts);
            sqlite3_bind_int64(st, 9, rows[i].rec);
            CHECK(sqlite3_step(st) == SQLITE_DONE);
            sqlite3_finalize(st);
        }
        sqlite3_close(raw);
    }

    r = list_files(w, alice, secret);
    CHECK(r && r->type == OC_RES_FILE_LIST);
    /* Three: the pending upload is excluded (never shared with anyone) and the
     * other channel's file is not this channel's. The reclaimed one IS listed —
     * "this was here and the bytes are gone" is information. */
    CHECK(r->n_flist == 3);
    CHECK(r->flist[0].id == 2);                  /* newest first */
    CHECK(strcmp(r->flist[0].filename, "new.png") == 0);
    CHECK(strcmp(r->flist[0].mime, "image/png") == 0);
    CHECK(r->flist[1].id == 3 && r->flist[1].reclaimed == 1);
    CHECK(r->flist[2].id == 1 && r->flist[2].reclaimed == 0);
    oc_dbres_free(r);

    r = list_files(w, carol, secret);
    CHECK(r && r->type == OC_RES_LIST_ERR && r->err_code == OC_ERR_NOT_A_MEMBER);
    oc_dbres_free(r);

    /* channel_id 0 = every channel I can read. Alice is in both, so she sees
     * four; carol is in neither private channel, so the secret ones are absent
     * from hers — the membership filter, not a channel filter. */
    r = list_files(w, alice, 0);
    CHECK(r && r->type == OC_RES_FILE_LIST && r->n_flist == 4);
    CHECK(r->flist[0].id == 5);                  /* newest across channels */
    oc_dbres_free(r);

    r = list_files(w, carol, 0);
    CHECK(r && r->type == OC_RES_FILE_LIST);
    for (size_t i = 0; i < r->n_flist; i++) CHECK(r->flist[i].channel_id != secret);
    oc_dbres_free(r);

    oc_dbwriter_stop(w);
    cleanup_db(path);
}

/* Channel mutability (REQ-034/035/036, ARCH-93): topic, rename, archive. What
 * matters is the authority split, that a rename keeps everything keyed on the
 * id, and that archived really is read-only. */
static oc_dbres *chan_update(oc_dbwriter *w, uint64_t uid, uint64_t ch, uint8_t op, const char *val) {
    oc_job *j = oc_job_new(OC_JOB_UPDATE_CHANNEL, 340);
    j->user_id = uid; j->channel_id = ch; j->chup_op = op;
    j->ch_name = val ? strdup(val) : strdup("");
    oc_dbwriter_submit(w, j);
    return wait_result(w);
}

/* A tombstone drops everything that hung off the body (REQ-052). The reactions
 * and pins were already dropped; the attachments were not, which leaked a blob
 * per deleted message and left clients rendering a file the message no longer
 * had. */
static void test_delete_clears_message_extras(void) {
    const char *path = "build/test_dbwriter_tomb.db";
    cleanup_db(path);
    oc_dbwriter *w = oc_dbwriter_start(path);
    CHECK(w != NULL);

    uint64_t alice = reg(w, "alice", "pw", OC_ROLE_OWNER);
    uint64_t bob   = reg(w, "bob",   "pw", OC_ROLE_MEMBER);
    CHECK(alice && bob);

    uint8_t idem[OC_IDEM_LEN];
    memset(idem, 0xF1, sizeof idem);
    uint64_t mid = send_msg(w, alice, idem, "delete me");
    CHECK(mid != 0);

    /* An attachment on the message, and a pin, and reactions from both users. */
    {
        sqlite3 *raw = NULL;
        CHECK(sqlite3_open(path, &raw) == SQLITE_OK);
        char sql[512];
        snprintf(sql, sizeof sql,
                 "INSERT INTO attachments(id,channel_id,message_id,uploader_id,storage_key,"
                 "filename,mime,size,created_at_ms,reclaimed_at_ms) "
                 "VALUES(1,%llu,%llu,%llu,'k','notes.txt','text/plain',13,1000,0);",
                 (unsigned long long)OC_DEFAULT_CHANNEL, (unsigned long long)mid,
                 (unsigned long long)alice);
        CHECK(sqlite3_exec(raw, sql, NULL, NULL, NULL) == SQLITE_OK);
        sqlite3_close(raw);
    }
    oc_dbres_free(chan_member(w, OC_JOB_INVITE_CHANNEL, alice, OC_DEFAULT_CHANNEL, bob));
    {
        oc_job *j = oc_job_new(OC_JOB_REACT, 520);
        j->user_id = alice; j->channel_id = OC_DEFAULT_CHANNEL; j->message_id = mid;
        j->react_op = OC_REACT_ADD; j->emoji = strdup("\xF0\x9F\x91\x8D");
        oc_dbwriter_submit(w, j);
        oc_dbres_free(wait_result(w));
    }
    oc_dbres_free(pin(w, alice, OC_DEFAULT_CHANNEL, mid, OC_PIN_ADD));

    {
        oc_job *j = oc_job_new(OC_JOB_DELETE, 521);
        j->user_id = alice; j->channel_id = OC_DEFAULT_CHANNEL; j->message_id = mid;
        oc_dbwriter_submit(w, j);
        oc_dbres *r = wait_result(w);
        CHECK(r && r->type == OC_RES_DELETE_OK);
        oc_dbres_free(r);
    }

    {
        sqlite3 *raw = NULL;
        CHECK(sqlite3_open(path, &raw) == SQLITE_OK);
        sqlite3_stmt *st = NULL;
        /* The message row survives as a tombstone (thread linkage, REQ-052)... */
        sqlite3_prepare_v2(raw, "SELECT body IS NULL, deleted_at_ms IS NOT NULL "
                                "FROM messages WHERE id=?;", -1, &st, NULL);
        sqlite3_bind_int64(st, 1, (sqlite3_int64)mid);
        CHECK(sqlite3_step(st) == SQLITE_ROW);
        CHECK(sqlite3_column_int(st, 0) == 1 && sqlite3_column_int(st, 1) == 1);
        sqlite3_finalize(st);

        /* ...but nothing that hung off the body does. */
        sqlite3_prepare_v2(raw, "SELECT COUNT(*) FROM reactions WHERE message_id=?;", -1, &st, NULL);
        sqlite3_bind_int64(st, 1, (sqlite3_int64)mid);
        CHECK(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 0);
        sqlite3_finalize(st);

        sqlite3_prepare_v2(raw, "SELECT COUNT(*) FROM pins WHERE message_id=?;", -1, &st, NULL);
        sqlite3_bind_int64(st, 1, (sqlite3_int64)mid);
        CHECK(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 0);
        sqlite3_finalize(st);

        /* The attachment is DETACHED, not deleted: message_id NULL is exactly the
         * "orphan" state the storage-maintenance sweep already collects, so the
         * blob is reclaimed by a path that is already written and tested. */
        sqlite3_prepare_v2(raw, "SELECT message_id IS NULL FROM attachments WHERE id=1;", -1, &st, NULL);
        CHECK(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 1);
        sqlite3_finalize(st);
        sqlite3_close(raw);
    }

    /* And the backfill no longer offers the file with the tombstone. */
    oc_dbres *r = backfill(w, alice, OC_DEFAULT_CHANNEL, 0);
    CHECK(r);
    for (size_t i = 0; i < r->n_replay; i++)
        if (r->replay[i].message_id == mid) CHECK(r->replay[i].n_attach == 0);
    oc_dbres_free(r);

    oc_dbwriter_stop(w);
    cleanup_db(path);
}

/* Saved items (REQ-231) and the activity feed (REQ-139), ARCH-95. The point of
 * both is WHOSE they are: a saved item is private to one person, and the feed
 * shows what involved you and nothing you did yourself. */
static oc_dbres *save_item(oc_dbwriter *w, uint64_t uid, uint64_t mid, uint8_t op) {
    oc_job *j = oc_job_new(OC_JOB_SAVE_ITEM, 600);
    j->user_id = uid; j->message_id = mid; j->save_op = op;
    oc_dbwriter_submit(w, j);
    return wait_result(w);
}
static oc_dbres *list_saved(oc_dbwriter *w, uint64_t uid) {
    oc_job *j = oc_job_new(OC_JOB_LIST_SAVED, 601);
    j->user_id = uid;
    oc_dbwriter_submit(w, j);
    return wait_result(w);
}
static oc_dbres *list_activity(oc_dbwriter *w, uint64_t uid) {
    oc_job *j = oc_job_new(OC_JOB_LIST_ACTIVITY, 602);
    j->user_id = uid;
    oc_dbwriter_submit(w, j);
    return wait_result(w);
}

/* Fetch-around (REQ-232, ARCH-96): the window a permalink needs. */
static oc_dbres *history_around(oc_dbwriter *w, uint64_t uid, uint64_t ch,
                                uint64_t mid, uint16_t limit) {
    oc_job *j = oc_job_new(OC_JOB_HISTORY, 700);
    j->user_id = uid; j->channel_id = ch; j->message_id = mid;
    j->search_limit = limit; j->hist_around = 1;
    oc_dbwriter_submit(w, j);
    return wait_result(w);
}

static void test_history_around(void) {
    const char *path = "build/test_dbwriter_around.db";
    cleanup_db(path);
    oc_dbwriter *w = oc_dbwriter_start(path);
    CHECK(w != NULL);
    uint64_t u = reg(w, "alice", "pw", OC_ROLE_OWNER);
    CHECK(u != 0);

    uint64_t ids[41];
    for (int i = 0; i < 41; i++) {
        uint8_t idem[OC_IDEM_LEN];
        memset(idem, 0, sizeof idem);
        idem[0] = 0x70; idem[1] = (uint8_t)i;
        char body[32]; snprintf(body, sizeof body, "m%d", i);
        ids[i] = send_msg(w, u, idem, body);
        CHECK(ids[i] != 0);
    }

    /* Around the middle: context on BOTH sides, ascending, target included. */
    oc_dbres *r = history_around(w, u, OC_DEFAULT_CHANNEL, ids[20], 10);
    CHECK(r && r->type == OC_RES_BACKFILL_OK);
    CHECK(r->n_replay == 10);                       /* 5 either side */
    int has_target = 0, before = 0, after = 0;
    for (size_t i = 0; i < r->n_replay; i++) {
        if (r->replay[i].message_id == ids[20]) has_target = 1;
        if (r->replay[i].message_id < ids[20]) before++;
        if (r->replay[i].message_id > ids[20]) after++;
        if (i) CHECK(r->replay[i - 1].message_id < r->replay[i].message_id);  /* ascending */
    }
    CHECK(has_target && before > 0 && after > 0);
    oc_dbres_free(r);

    /* At the very start there is nothing before it, and the window does not
     * silently shrink to nothing. */
    r = history_around(w, u, OC_DEFAULT_CHANNEL, ids[0], 10);
    CHECK(r && r->n_replay > 0);
    CHECK(r->replay[0].message_id == ids[0]);
    oc_dbres_free(r);

    /* At the end, likewise. */
    r = history_around(w, u, OC_DEFAULT_CHANNEL, ids[40], 10);
    CHECK(r && r->n_replay > 0);
    CHECK(r->replay[r->n_replay - 1].message_id == ids[40]);
    oc_dbres_free(r);

    /* Read access still gates it — a permalink is not a way around membership. */
    uint64_t outsider = reg(w, "mallory", "pw", OC_ROLE_MEMBER);
    oc_dbres_free(create_channel(w, u, "secret", 0));
    r = history_around(w, outsider, 4242, ids[20], 10);
    CHECK(r && r->n_replay == 0);
    oc_dbres_free(r);

    oc_dbwriter_stop(w);
    cleanup_db(path);
}

static void test_saved_and_activity(void) {
    const char *path = "build/test_dbwriter_saved.db";
    cleanup_db(path);
    oc_dbwriter *w = oc_dbwriter_start(path);
    CHECK(w != NULL);

    uint64_t alice = reg(w, "alice", "pw", OC_ROLE_OWNER);
    uint64_t bob   = reg(w, "bob",   "pw", OC_ROLE_MEMBER);
    CHECK(alice && bob);

    uint8_t idem[OC_IDEM_LEN];
    memset(idem, 0x51, sizeof idem);
    uint64_t am = send_msg(w, alice, idem, "alice wrote this");
    memset(idem, 0x52, sizeof idem);
    uint64_t bm = send_msg(w, bob, idem, "hey @alice look at this");
    CHECK(am && bm);

    /* --- saved items are PRIVATE --- */
    oc_dbres *r = save_item(w, alice, bm, OC_SAVE_ADD);
    CHECK(r && r->type == OC_RES_SAVED_OK && r->saved_at != 0);
    uint64_t first_saved_at = r->saved_at;
    oc_dbres_free(r);

    /* Re-saving keeps the ORIGINAL time: a list ordered by when you saved should
     * not reshuffle because you clicked twice. */
    r = save_item(w, alice, bm, OC_SAVE_ADD);
    CHECK(r && r->saved_at == first_saved_at);
    oc_dbres_free(r);

    r = list_saved(w, alice);
    CHECK(r && r->type == OC_RES_SAVED_LIST && r->n_slist == 1);
    CHECK(r->slist[0].message_id == bm);
    CHECK(r->slist[0].body && strcmp(r->slist[0].body, "hey @alice look at this") == 0);
    oc_dbres_free(r);

    /* Bob saved nothing, and cannot see hers. */
    r = list_saved(w, bob);
    CHECK(r && r->n_slist == 0);
    oc_dbres_free(r);

    /* Both may save the same message independently. */
    r = save_item(w, bob, bm, OC_SAVE_ADD); CHECK(r && r->type == OC_RES_SAVED_OK); oc_dbres_free(r);
    r = list_saved(w, bob);  CHECK(r && r->n_slist == 1); oc_dbres_free(r);
    r = list_saved(w, alice); CHECK(r && r->n_slist == 1); oc_dbres_free(r);

    r = save_item(w, alice, bm, OC_SAVE_REMOVE); CHECK(r && r->type == OC_RES_SAVED_OK); oc_dbres_free(r);
    r = list_saved(w, alice); CHECK(r && r->n_slist == 0); oc_dbres_free(r);
    r = list_saved(w, bob);   CHECK(r && r->n_slist == 1); oc_dbres_free(r);   /* his survives */

    /* A tombstone drops out of the list rather than showing an empty row. */
    {
        oc_job *dj = oc_job_new(OC_JOB_DELETE, 603);
        dj->user_id = bob; dj->channel_id = OC_DEFAULT_CHANNEL; dj->message_id = bm;
        oc_dbwriter_submit(w, dj);
        oc_dbres_free(wait_result(w));
    }
    r = list_saved(w, bob); CHECK(r && r->n_slist == 0); oc_dbres_free(r);

    /* --- activity --- */
    /* bob reacts to alice's message and replies under it. */
    {
        oc_job *j = oc_job_new(OC_JOB_REACT, 604);
        j->user_id = bob; j->channel_id = OC_DEFAULT_CHANNEL; j->message_id = am;
        j->react_op = OC_REACT_ADD; j->emoji = strdup("\xF0\x9F\x91\x8D");
        oc_dbwriter_submit(w, j);
        oc_dbres_free(wait_result(w));
    }
    {
        oc_job *j = oc_job_new(OC_JOB_SEND_REPLY, 605);
        j->user_id = bob; j->channel_id = OC_DEFAULT_CHANNEL; j->parent_id = am;
        memset(j->idem, 0x53, OC_IDEM_LEN);
        oc_job_set_body(j, "replying to you", 15);
        oc_dbwriter_submit(w, j);
        oc_dbres_free(wait_result(w));
    }

    r = list_activity(w, alice);
    CHECK(r && r->type == OC_RES_ACTIVITY);
    int saw_react = 0, saw_reply = 0, saw_mention = 0, saw_self = 0;
    for (size_t i = 0; i < r->n_alist; i++) {
        if (r->alist[i].kind == OC_ACT_REACTION && r->alist[i].message_id == am) saw_react = 1;
        if (r->alist[i].kind == OC_ACT_REPLY) saw_reply = 1;
        if (r->alist[i].kind == OC_ACT_MENTION) saw_mention = 1;
        if (r->alist[i].actor_id == alice) saw_self = 1;
    }
    CHECK(saw_react && saw_reply);
    /* The mention was in a message bob sent BEFORE it was deleted above, so it
     * is gone with the tombstone — which is the behaviour we want. */
    CHECK(!saw_mention);
    /* An activity feed of your own doings is noise: nothing alice did appears. */
    CHECK(!saw_self);

    /* Opening the feed stamps the watermark: the first read reports 0 (never
     * looked), the second reports the time of the first. */
    uint64_t seen_first = r->activity_seen;
    oc_dbres_free(r);
    CHECK(seen_first == 0);
    r = list_activity(w, alice);
    CHECK(r && r->activity_seen > 0);
    oc_dbres_free(r);

    /* Bob's feed is his own: alice did nothing to him. */
    r = list_activity(w, bob);
    CHECK(r && r->n_alist == 0);
    oc_dbres_free(r);

    oc_dbwriter_stop(w);
    cleanup_db(path);
}

/* --- Drafts (REQ-223, ARCH-101) ------------------------------------------ */

static oc_dbres *set_draft(oc_dbwriter *w, uint64_t uid, uint64_t cid,
                           uint64_t root, const char *body) {
    oc_job *j = oc_job_new(OC_JOB_SET_DRAFT, 900);
    j->user_id = uid; j->channel_id = cid; j->parent_id = root;
    if (body && *body) oc_job_set_body(j, body, strlen(body));
    oc_dbwriter_submit(w, j);
    return wait_result(w);
}

static oc_dbres *list_drafts(oc_dbwriter *w, uint64_t uid) {
    oc_job *j = oc_job_new(OC_JOB_LIST_DRAFTS, 901);
    j->user_id = uid;
    oc_dbwriter_submit(w, j);
    return wait_result(w);
}

static void test_drafts(void) {
    const char *path = "build/test_dbwriter_drafts.db";
    cleanup_db(path);
    oc_dbwriter *w = oc_dbwriter_start(path);
    CHECK(w != NULL);

    uint64_t alice = reg(w, "alice", "pw", OC_ROLE_OWNER);
    uint64_t bob   = reg(w, "bob",   "pw", OC_ROLE_MEMBER);
    CHECK(alice && bob);

    oc_dbres *r = set_draft(w, alice, OC_DEFAULT_CHANNEL, 0, "half a thought");
    CHECK(r && r->type == OC_RES_DRAFT);
    CHECK(r->draft.body && strcmp(r->draft.body, "half a thought") == 0);
    CHECK(r->draft.updated_ms != 0);
    oc_dbres_free(r);

    r = list_drafts(w, alice);
    CHECK(r && r->type == OC_RES_DRAFTS && r->n_drafts == 1);
    CHECK(r->drafts[0].channel_id == OC_DEFAULT_CHANNEL && r->drafts[0].thread_root == 0);
    CHECK(r->drafts[0].body && strcmp(r->drafts[0].body, "half a thought") == 0);
    oc_dbres_free(r);

    /* Writing again REPLACES rather than accumulating — the key is the
     * conversation, not the keystroke. */
    r = set_draft(w, alice, OC_DEFAULT_CHANNEL, 0, "a whole one"); oc_dbres_free(r);
    r = list_drafts(w, alice);
    CHECK(r && r->n_drafts == 1);
    CHECK(r->drafts[0].body && strcmp(r->drafts[0].body, "a whole one") == 0);
    oc_dbres_free(r);

    /* Private: bob has none, and cannot see hers. */
    r = list_drafts(w, bob); CHECK(r && r->n_drafts == 0); oc_dbres_free(r);

    /* A thread draft is a DIFFERENT draft in the same channel (ARCH-101 keeps
     * the column ahead of the client, so nothing writes this yet — but the key
     * has to hold it the day something does). */
    r = set_draft(w, alice, OC_DEFAULT_CHANNEL, 4242, "in the thread"); oc_dbres_free(r);
    r = list_drafts(w, alice); CHECK(r && r->n_drafts == 2); oc_dbres_free(r);
    r = set_draft(w, alice, OC_DEFAULT_CHANNEL, 4242, ""); oc_dbres_free(r);

    /* An empty body is the delete. */
    r = set_draft(w, alice, OC_DEFAULT_CHANNEL, 0, "");
    CHECK(r && r->type == OC_RES_DRAFT && r->draft.body && r->draft.body[0] == 0);
    oc_dbres_free(r);
    r = list_drafts(w, alice); CHECK(r && r->n_drafts == 0); oc_dbres_free(r);

    /* A non-member is refused: a draft is user content ABOUT a conversation,
     * and storing one for a channel you cannot see would report its existence
     * back to you on every other device. */
    r = set_draft(w, alice, 999999, 0, "for a channel that is not mine");
    CHECK(r && r->type == OC_RES_LIST_ERR && r->err_code == OC_ERR_NOT_A_MEMBER);
    oc_dbres_free(r);

    /* SENDING clears the draft, server-side, in the send's own transaction. */
    r = set_draft(w, alice, OC_DEFAULT_CHANNEL, 0, "about to send this"); oc_dbres_free(r);
    r = list_drafts(w, alice); CHECK(r && r->n_drafts == 1); oc_dbres_free(r);
    {
        uint8_t idem[OC_IDEM_LEN]; memset(idem, 0x77, sizeof idem);
        CHECK(send_msg(w, alice, idem, "about to send this") != 0);
    }
    r = list_drafts(w, alice); CHECK(r && r->n_drafts == 0); oc_dbres_free(r);
    /* And only the sender's: bob's draft for the same channel survives. */
    r = set_draft(w, bob, OC_DEFAULT_CHANNEL, 0, "bob is still writing"); oc_dbres_free(r);
    {
        uint8_t idem[OC_IDEM_LEN]; memset(idem, 0x78, sizeof idem);
        CHECK(send_msg(w, alice, idem, "alice sends again") != 0);
    }
    r = list_drafts(w, bob); CHECK(r && r->n_drafts == 1); oc_dbres_free(r);

    /* Removing a user takes their drafts with them. */
    {
        oc_job *j = oc_job_new(OC_JOB_REMOVE_USER, 902);
        j->user_id = alice; j->target_user_id = bob;
        oc_dbwriter_submit(w, j);
        oc_dbres_free(wait_result(w));
    }
    r = list_drafts(w, bob); CHECK(r && r->n_drafts == 0); oc_dbres_free(r);

    /* A body over the cap is TRUNCATED, not refused: the client cannot type one
     * that long, so a frame this size is a bug or a probe, and losing the tail
     * of it is better than losing the draft. */
    {
        static char big[OC_DRAFT_BODY_MAX + 100];
        memset(big, 'y', sizeof big - 1); big[sizeof big - 1] = 0;
        r = set_draft(w, alice, OC_DEFAULT_CHANNEL, 0, big);
        CHECK(r && r->type == OC_RES_DRAFT);
        oc_dbres_free(r);
        r = list_drafts(w, alice);
        CHECK(r && r->n_drafts == 1 && strlen(r->drafts[0].body) == OC_DRAFT_BODY_MAX);
        oc_dbres_free(r);
    }

    oc_dbwriter_stop(w);
    cleanup_db(path);
}

/* --- Scheduled messages (REQ-224, ARCH-102) ------------------------------ */

static oc_dbres *sched_new(oc_dbwriter *w, uint64_t uid, uint64_t cid,
                           uint64_t at, const char *body) {
    oc_job *j = oc_job_new(OC_JOB_SCHEDULE, 910);
    j->user_id = uid; j->channel_id = cid; j->sched_at_ms = at;
    if (body && *body) oc_job_set_body(j, body, strlen(body));
    oc_dbwriter_submit(w, j);
    return wait_result(w);
}
static oc_dbres *sched_list(oc_dbwriter *w, uint64_t uid) {
    oc_job *j = oc_job_new(OC_JOB_LIST_SCHEDULED, 911);
    j->user_id = uid;
    oc_dbwriter_submit(w, j);
    return wait_result(w);
}
static oc_dbres *sched_fire(oc_dbwriter *w) {
    oc_job *j = oc_job_new(OC_JOB_FIRE_SCHEDULED, 0);
    oc_dbwriter_submit(w, j);
    return wait_result(w);
}

static void test_scheduled(void) {
    const char *path = "build/test_dbwriter_sched.db";
    cleanup_db(path);
    oc_dbwriter *w = oc_dbwriter_start(path);
    CHECK(w != NULL);
    uint64_t alice = reg(w, "alice", "pw", OC_ROLE_OWNER);
    uint64_t bob   = reg(w, "bob",   "pw", OC_ROLE_MEMBER);
    CHECK(alice && bob);

    /* Scheduled far ahead: listed, and NOT delivered by a sweep. */
    oc_dbres *r = sched_new(w, alice, OC_DEFAULT_CHANNEL,
                            (uint64_t)time(NULL) * 1000 + 3600000, "later today");
    CHECK(r && r->type == OC_RES_SCHEDULED && r->sched.id != 0);
    CHECK(r->sched.state == OC_SCHED_PENDING);
    uint64_t sid = r->sched.id;
    oc_dbres_free(r);

    r = sched_list(w, alice);
    CHECK(r && r->type == OC_RES_SCHEDULED_LIST && r->n_scheds == 1);
    CHECK(r->scheds[0].id == sid);
    CHECK(r->scheds[0].body && strcmp(r->scheds[0].body, "later today") == 0);
    oc_dbres_free(r);
    r = sched_list(w, bob); CHECK(r && r->n_scheds == 0); oc_dbres_free(r);   /* private */

    r = sched_fire(w);
    CHECK(r && r->type == OC_RES_SCHED_FIRED);   /* nothing due yet */
    oc_dbres_free(r);
    r = sched_list(w, alice); CHECK(r && r->n_scheds == 1); oc_dbres_free(r);

    /* An empty body is refused rather than stored: a row that fires into
     * nothing at 09:00 is worse than a refusal now. */
    r = sched_new(w, alice, OC_DEFAULT_CHANNEL, 1, "");
    CHECK(r && r->type == OC_RES_LIST_ERR && r->err_code == OC_ERR_INVALID_MESSAGE);
    oc_dbres_free(r);
    /* And a channel you are not in is refused, like every other write. */
    r = sched_new(w, alice, 999999, 1, "nope");
    CHECK(r && r->type == OC_RES_LIST_ERR && r->err_code == OC_ERR_NOT_A_MEMBER);
    oc_dbres_free(r);

    /* Cancel takes it out of the list. */
    {
        oc_job *j = oc_job_new(OC_JOB_CANCEL_SCHEDULED, 912);
        j->user_id = alice; j->message_id = sid;
        oc_dbwriter_submit(w, j);
        r = wait_result(w);
        CHECK(r && r->type == OC_RES_SCHEDULED && r->sched.state == OC_SCHED_GONE);
        oc_dbres_free(r);
    }
    r = sched_list(w, alice); CHECK(r && r->n_scheds == 0); oc_dbres_free(r);

    /* Due in the past: the sweep delivers it through the ORDINARY send path, so
     * the result that comes back is a SEND_OK — the netloop broadcasts it
     * without knowing it was scheduled (ARCH-102). */
    r = sched_new(w, alice, OC_DEFAULT_CHANNEL, 1, "sent by the clock");
    CHECK(r && r->type == OC_RES_SCHEDULED);
    uint64_t due = r->sched.id;
    oc_dbres_free(r);
    r = sched_fire(w);
    CHECK(r && r->type == OC_RES_SEND_OK && r->message_id != 0);
    CHECK(r->body_len == 17 && memcmp(r->body, "sent by the clock", 17) == 0);
    oc_dbres_free(r);
    /* Delivered ones leave the list: they are messages now. */
    r = sched_list(w, alice); CHECK(r && r->n_scheds == 0); oc_dbres_free(r);
    /* And firing again does nothing — the row is no longer pending, which is
     * also what stops a double post if the sweep runs twice. */
    r = sched_fire(w); CHECK(r && r->type == OC_RES_SCHED_FIRED); oc_dbres_free(r);
    (void)due;

    /* An archived channel cannot take it: FAILED with a reason the author sees,
     * rather than posting into a read-only channel or discarding in silence. */
    r = sched_new(w, alice, OC_DEFAULT_CHANNEL, 1, "into an archive");
    CHECK(r && r->type == OC_RES_SCHEDULED);
    oc_dbres_free(r);
    {
        oc_job *j = oc_job_new(OC_JOB_UPDATE_CHANNEL, 913);
        j->user_id = alice; j->channel_id = OC_DEFAULT_CHANNEL;
        j->chup_op = OC_CHUP_ARCHIVE;
        oc_dbwriter_submit(w, j);
        oc_dbres_free(wait_result(w));
    }
    r = sched_fire(w);
    CHECK(r && r->type == OC_RES_SCHED_FIRED);      /* not delivered */
    oc_dbres_free(r);
    r = sched_list(w, alice);
    CHECK(r && r->n_scheds == 1 && r->scheds[0].state == OC_SCHED_FAILED);
    CHECK(r->scheds[0].fail_reason && strstr(r->scheds[0].fail_reason, "archived") != NULL);
    oc_dbres_free(r);

    oc_dbwriter_stop(w);
    cleanup_db(path);
}

/* --- Activity's unread views (REQ-139) ---------------------------- */

static oc_dbres *activity(oc_dbwriter *w, uint64_t uid, uint8_t filter) {
    oc_job *j = oc_job_new(OC_JOB_LIST_ACTIVITY, 920);
    j->user_id = uid; j->act_filter = filter;
    oc_dbwriter_submit(w, j);
    return wait_result(w);
}

/* Keyword alerts (REQ-135). The claim under test is the one ARCH-103
 * rests on: a hit IS a mention — same table, same kind column — which is what
 * makes it notify, appear in the activity feed and highlight without any of the
 * three learning about keywords at all. */
static void test_keyword_alerts(void) {
    const char *path = "build/test_dbwriter_keywords.db";
    cleanup_db(path);
    oc_dbwriter *w = oc_dbwriter_start(path);
    CHECK(w != NULL);
    uint64_t alice = reg(w, "alice", "pw", OC_ROLE_OWNER);
    uint64_t bob   = reg(w, "bob",   "pw", OC_ROLE_MEMBER);
    CHECK(alice && bob);

    {   /* alice watches for "deploy" and the phrase "release train". */
        oc_job *j = oc_job_new(OC_JOB_SET_KEYWORDS, 1);
        j->user_id = alice;
        j->kw_terms = calloc(2, sizeof *j->kw_terms);
        j->kw_terms[0] = strdup("Deploy");          /* stored folded to lower case */
        j->kw_terms[1] = strdup("release train");
        j->n_kw_terms = 2;
        oc_dbwriter_submit(w, j);
        oc_dbres *r = wait_result(w);
        CHECK(r && r->type == OC_RES_ALERT_PREFS && r->al_n_terms == 2);
        CHECK(strcmp(r->al_terms[0], "deploy") == 0);
        oc_dbres_free(r);
    }

    uint8_t idem[OC_IDEM_LEN];
    memset(idem, 0xA1, sizeof idem);
    uint64_t hit = send_msg(w, bob, idem, "starting the deploy now");
    memset(idem, 0xA2, sizeof idem);
    uint64_t miss = send_msg(w, bob, idem, "the deployment plan is fine");
    memset(idem, 0xA3, sizeof idem);
    uint64_t own = send_msg(w, alice, idem, "I will deploy it myself");
    CHECK(hit && miss && own);

    /* The feed is the honest end-to-end check: mentions are what it reads, so a
     * hit appearing there proves the row landed with a kind the feed accepts. */
    oc_dbres *r = activity(w, alice, OC_ACTF_INVOLVED);
    CHECK(r && r->type == OC_RES_ACTIVITY);
    int saw_hit = 0, saw_miss = 0, saw_own = 0;
    for (size_t i = 0; r && i < r->n_alist; i++) {
        if (r->alist[i].message_id == hit)  saw_hit = 1;
        if (r->alist[i].message_id == miss) saw_miss = 1;
        if (r->alist[i].message_id == own)  saw_own = 1;
    }
    CHECK(saw_hit);
    CHECK(!saw_miss);   /* "deployment" is not "deploy" */
    CHECK(!saw_own);    /* your own message is not news */
    oc_dbres_free(r);

    /* Keywords fire in THREADS, deliberately unlike Slack (REQ-135). */
    {
        oc_job *j = oc_job_new(OC_JOB_SEND_REPLY, 1);
        j->user_id = bob; j->channel_id = OC_DEFAULT_CHANNEL; j->parent_id = hit;
        memset(idem, 0xA4, sizeof idem);
        memcpy(j->idem, idem, OC_IDEM_LEN);
        oc_job_set_body(j, "and the release train after", 27);
        oc_dbwriter_submit(w, j);
        oc_dbres *rr = wait_result(w);
        uint64_t rid = (rr && rr->type == OC_RES_REPLY_OK) ? rr->message_id : 0;
        oc_dbres_free(rr);
        CHECK(rid);
        r = activity(w, alice, OC_ACTF_INVOLVED);
        int saw_reply = 0;
        for (size_t i = 0; r && i < r->n_alist; i++)
            if (r->alist[i].message_id == rid) saw_reply = 1;
        CHECK(saw_reply);
        oc_dbres_free(r);
    }

    /* Setting the list REPLACES it: dropping a term stops it firing, which an
     * add-only op would have made impossible to express. */
    {
        oc_job *j = oc_job_new(OC_JOB_SET_KEYWORDS, 1);
        j->user_id = alice; j->n_kw_terms = 0;
        oc_dbwriter_submit(w, j);
        oc_dbres *rr = wait_result(w);
        CHECK(rr && rr->al_n_terms == 0);
        oc_dbres_free(rr);
    }
    memset(idem, 0xA5, sizeof idem);
    uint64_t after = send_msg(w, bob, idem, "one more deploy");
    r = activity(w, alice, OC_ACTF_INVOLVED);
    int saw_after = 0;
    for (size_t i = 0; r && i < r->n_alist; i++)
        if (r->alist[i].message_id == after) saw_after = 1;
    CHECK(!saw_after);
    oc_dbres_free(r);

    /* Priority people: a relation, refused for somebody who does not exist —
     * a setting that can never fire is worse than an error. */
    {
        oc_job *j = oc_job_new(OC_JOB_SET_PRIORITY, 1);
        j->user_id = alice;
        j->pri_people = calloc(3, sizeof *j->pri_people);
        j->pri_people[0] = bob;
        j->pri_people[1] = alice;      /* yourself: never notifies you anyway */
        j->pri_people[2] = 999999;     /* nobody */
        j->n_pri_people = 3;
        oc_dbwriter_submit(w, j);
        oc_dbres *rr = wait_result(w);
        CHECK(rr && rr->type == OC_RES_ALERT_PREFS && rr->al_n_people == 1);
        CHECK(rr->al_people[0] == bob);
        oc_dbres_free(rr);
    }

    oc_dbwriter_stop(w);
    cleanup_db(path);
}

static void test_activity_unreads(void) {
    const char *path = "build/test_dbwriter_unread.db";
    cleanup_db(path);
    oc_dbwriter *w = oc_dbwriter_start(path);
    CHECK(w != NULL);
    uint64_t alice = reg(w, "alice", "pw", OC_ROLE_OWNER);
    uint64_t bob   = reg(w, "bob",   "pw", OC_ROLE_MEMBER);
    CHECK(alice && bob);

    uint8_t idem[OC_IDEM_LEN];
    memset(idem, 0x91, sizeof idem);
    uint64_t m1 = send_msg(w, bob, idem, "bob says one");
    memset(idem, 0x92, sizeof idem);
    uint64_t m2 = send_msg(w, bob, idem, "bob says two");
    CHECK(m1 && m2);

    /* Everything bob said is unread to alice. */
    oc_dbres *r = activity(w, alice, OC_ACTF_UNREADS);
    CHECK(r && r->type == OC_RES_ACTIVITY && r->n_alist == 2);
    CHECK(r->alist[0].kind == OC_ACT_UNREAD);
    oc_dbres_free(r);

    /* Your own messages are not unread to you — you have read what you wrote. */
    memset(idem, 0x93, sizeof idem);
    CHECK(send_msg(w, alice, idem, "alice says something") != 0);
    r = activity(w, alice, OC_ACTF_UNREADS);
    CHECK(r && r->n_alist == 2);
    oc_dbres_free(r);

    /* Acking clears them: the cursor REQ-090 already maintains IS the read
     * state, which is why this needed no new table. */
    {
        oc_job *j = oc_job_new(OC_JOB_SET_READ_CURSOR, 921);
        j->user_id = alice; j->channel_id = OC_DEFAULT_CHANNEL; j->message_id = m2;
        oc_dbwriter_submit(w, j);
        oc_dbres_free(wait_result(w));
    }
    r = activity(w, alice, OC_ACTF_UNREADS);
    CHECK(r && r->n_alist == 0);
    oc_dbres_free(r);

    /* A channel is not a DM. */
    memset(idem, 0x94, sizeof idem);
    CHECK(send_msg(w, bob, idem, "after the ack") != 0);
    r = activity(w, alice, OC_ACTF_UNREADS);  CHECK(r && r->n_alist == 1); oc_dbres_free(r);
    r = activity(w, alice, OC_ACTF_DMS);      CHECK(r && r->n_alist == 0); oc_dbres_free(r);
    r = activity(w, alice, OC_ACTF_CHANNELS); CHECK(r && r->n_alist == 1); oc_dbres_free(r);

    /* Channels lists only what is set to ALL: turning this one down to mentions
     * takes it out, which is the whole distinction between the two tabs. */
    {
        oc_job *j = oc_job_new(OC_JOB_SET_NOTIFY_PREF, 922);
        j->user_id = alice; j->channel_id = OC_DEFAULT_CHANNEL;
        j->notify_level = OC_NOTIFY_MENTIONS;
        oc_dbwriter_submit(w, j);
        oc_dbres_free(wait_result(w));
    }
    r = activity(w, alice, OC_ACTF_CHANNELS); CHECK(r && r->n_alist == 0); oc_dbres_free(r);
    r = activity(w, alice, OC_ACTF_UNREADS);  CHECK(r && r->n_alist == 1); oc_dbres_free(r);

    /* Muting takes it out of Unreads too (REQ-137): an inbox that lists what you
     * muted is an inbox you stop trusting. */
    {
        oc_job *j = oc_job_new(OC_JOB_SET_MUTE, 923);
        j->user_id = alice; j->channel_id = OC_DEFAULT_CHANNEL;
        j->hook_disabled = 1;                  /* the field SET_MUTE reuses for it */
        oc_dbwriter_submit(w, j);
        oc_dbres_free(wait_result(w));
    }
    r = activity(w, alice, OC_ACTF_UNREADS); CHECK(r && r->n_alist == 0); oc_dbres_free(r);

    oc_dbwriter_stop(w);
    cleanup_db(path);
}

static void test_channel_mutability(void) {
    const char *path = "build/test_dbwriter_chanmut.db";
    cleanup_db(path);
    oc_dbwriter *w = oc_dbwriter_start(path);
    CHECK(w != NULL);

    uint64_t alice = reg(w, "alice", "pw", OC_ROLE_OWNER);
    uint64_t bob   = reg(w, "bob",   "pw", OC_ROLE_MEMBER);
    CHECK(alice && bob);

    oc_dbres *r = create_channel(w, alice, "planning", 1);
    CHECK(r && r->type == OC_RES_CHANNEL_INFO);
    uint64_t ch = r->channel_id;
    oc_dbres_free(r);
    oc_dbres_free(chan_member(w, OC_JOB_INVITE_CHANNEL, alice, ch, bob));

    /* Topic: ANY member may set it — it is already visible to the channel and a
     * wrong one is corrected in seconds. */
    r = chan_update(w, bob, ch, OC_CHUP_TOPIC, "ship on friday");
    CHECK(r && r->type == OC_RES_CHANNEL_INFO);
    CHECK(r->ch_topic && strcmp(r->ch_topic, "ship on friday") == 0);
    /* The change fans out to everyone, not just the actor. */
    CHECK(r->ch_fanout == 1 && r->n_members >= 2);
    oc_dbres_free(r);

    /* An empty topic clears it rather than storing "". */
    r = chan_update(w, bob, ch, OC_CHUP_TOPIC, "");
    CHECK(r && r->type == OC_RES_CHANNEL_INFO && r->ch_topic == NULL);
    oc_dbres_free(r);

    {
        char big[OC_MAX_TOPIC + 20];
        memset(big, 'x', sizeof big - 1); big[sizeof big - 1] = '\0';
        r = chan_update(w, alice, ch, OC_CHUP_TOPIC, big);
        CHECK(r && r->type == OC_RES_CHANNEL_ERR && r->err_code == OC_ERR_INVALID_CHANNEL);
        oc_dbres_free(r);
    }

    /* Rename is owner/admin only — it moves a landmark for people who are not
     * looking at the channel. */
    r = chan_update(w, bob, ch, OC_CHUP_RENAME, "roadmap");
    CHECK(r && r->type == OC_RES_CHANNEL_ERR && r->err_code == OC_ERR_FORBIDDEN);
    oc_dbres_free(r);

    /* A rename keeps the id, so membership and history follow it for free —
     * this is why there is no name-history table (ARCH-93). */
    uint8_t idem[OC_IDEM_LEN];
    memset(idem, 0xE1, sizeof idem);
    {
        oc_job *sj = oc_job_new(OC_JOB_SEND, 341);
        sj->user_id = alice; sj->channel_id = ch;
        memcpy(sj->idem, idem, OC_IDEM_LEN);
        oc_job_set_body(sj, "before the rename", 17);
        oc_dbwriter_submit(w, sj);
        oc_dbres *sr = wait_result(w);
        CHECK(sr && sr->type == OC_RES_SEND_OK);
        oc_dbres_free(sr);
    }
    r = chan_update(w, alice, ch, OC_CHUP_RENAME, "roadmap");
    CHECK(r && r->type == OC_RES_CHANNEL_INFO);
    CHECK(r->channel_id == ch);                       /* same id */
    CHECK(r->ch_name && strcmp(r->ch_name, "roadmap") == 0);
    oc_dbres_free(r);
    r = backfill(w, alice, ch, 0);
    CHECK(r && r->n_replay == 1);                     /* history survived */
    oc_dbres_free(r);

    /* The unique-name index applies to a rename exactly as to a create. */
    oc_dbres_free(create_channel(w, alice, "taken", 1));
    r = chan_update(w, alice, ch, OC_CHUP_RENAME, "TAKEN");
    CHECK(r && r->type == OC_RES_CHANNEL_ERR && r->err_code == OC_ERR_CHANNEL_EXISTS);
    oc_dbres_free(r);
    /* But renaming to its own name (different case) is not a collision. */
    r = chan_update(w, alice, ch, OC_CHUP_RENAME, "Roadmap");
    CHECK(r && r->type == OC_RES_CHANNEL_INFO);
    oc_dbres_free(r);

    r = chan_update(w, alice, ch, OC_CHUP_RENAME, "");
    CHECK(r && r->type == OC_RES_CHANNEL_ERR && r->err_code == OC_ERR_INVALID_CHANNEL);
    oc_dbres_free(r);

    /* ---- visibility (REQ-031) ------------------------------------------------
     * The two directions are not symmetric, so they are tested as two things:
     * PRIVATE narrows the audience to the members, PUBLIC exposes the whole history
     * to everyone. What must hold in both is that MEMBERSHIP and HISTORY are
     * untouched — the flag is the only thing that moves. */
    {
        uint64_t carol = reg(w, "vis-carol", "pw", OC_ROLE_MEMBER);
        CHECK(carol);
        /* carol is not in it. While it is public she can read it; that IS public. */
        oc_dbres *br = backfill(w, carol, ch, 0);
        CHECK(br && br->type == OC_RES_BACKFILL_OK && br->n_replay == 1);
        oc_dbres_free(br);

        /* A plain member cannot change it: it decides what people OUTSIDE the
         * channel can see, which is the moderator line this file draws. */
        r = chan_update(w, bob, ch, OC_CHUP_PRIVATE, "");
        CHECK(r && r->type == OC_RES_CHANNEL_ERR && r->err_code == OC_ERR_FORBIDDEN);
        oc_dbres_free(r);

        r = chan_update(w, alice, ch, OC_CHUP_PRIVATE, "");
        CHECK(r && r->type == OC_RES_CHANNEL_INFO && r->ch_is_public == 0);
        /* Members are untouched — the flag pins the audience, it does not rewrite it. */
        CHECK(r->ch_fanout == 1 && r->n_members >= 2);
        oc_dbres_free(r);

        /* Now carol cannot read it, and bob (a member) still can. That is the whole
         * point of the direction, and it is enforced by the SAME read-access rule
         * everything else uses rather than by anything new. */
        br = backfill(w, carol, ch, 0);
        CHECK(br && (br->type != OC_RES_BACKFILL_OK || br->n_replay == 0));
        oc_dbres_free(br);
        br = backfill(w, bob, ch, 0);
        CHECK(br && br->type == OC_RES_BACKFILL_OK && br->n_replay == 1);
        oc_dbres_free(br);

        /* It also leaves the directory: a private channel a non-member can still
         * find by name is not private. */
        r = list_channels(w, carol);
        CHECK(r && r->type == OC_RES_CHANNEL_LIST);
        if (r) {
            int found = 0;
            for (size_t i = 0; i < r->n_chlist; i++) if (r->chlist[i].channel_id == ch) found = 1;
            CHECK(!found);
        }
        oc_dbres_free(r);

        /* Idempotent: the ops name a TARGET STATE, so a second one changes nothing.
         * A toggle would have flipped it back — which is exactly what two admins
         * clicking at the same time would have done. */
        r = chan_update(w, alice, ch, OC_CHUP_PRIVATE, "");
        CHECK(r && r->type == OC_RES_CHANNEL_INFO && r->ch_is_public == 0);
        oc_dbres_free(r);

        /* PUBLIC again: carol can read EVERYTHING, including what was said while it
         * was private. This is the disclosure the client warns about, asserted here
         * so nobody can later claim it was not the intended behaviour. */
        r = chan_update(w, bob, ch, OC_CHUP_PUBLIC, "");
        CHECK(r && r->type == OC_RES_CHANNEL_ERR && r->err_code == OC_ERR_FORBIDDEN);
        oc_dbres_free(r);
        r = chan_update(w, alice, ch, OC_CHUP_PUBLIC, "");
        CHECK(r && r->type == OC_RES_CHANNEL_INFO && r->ch_is_public == 1);
        oc_dbres_free(r);
        br = backfill(w, carol, ch, 0);
        CHECK(br && br->type == OC_RES_BACKFILL_OK && br->n_replay == 1);
        oc_dbres_free(br);

        /* Both directions are audited, and as DISTINCT actions: an audit reader must
         * not have to open the row to see which way it went. */
        oc_job *aj = oc_job_new(OC_JOB_AUDIT_QUERY, 900);
        aj->user_id = alice; aj->audit_limit = 50;
        oc_dbwriter_submit(w, aj);
        oc_dbres *ar = wait_result(w);
        CHECK(ar && ar->type == OC_RES_AUDIT_PAGE);
        if (ar) {
            int saw_priv = 0, saw_pub = 0;
            for (size_t i = 0; i < ar->n_audit; i++) {
                if (ar->audit[i].action && !strcmp(ar->audit[i].action, "channel.private")) saw_priv = 1;
                if (ar->audit[i].action && !strcmp(ar->audit[i].action, "channel.public"))  saw_pub = 1;
            }
            CHECK(saw_priv && saw_pub);
        }
        oc_dbres_free(ar);
    }

    /* Archive: owner/admin only, and then genuinely read-only. */
    r = chan_update(w, bob, ch, OC_CHUP_ARCHIVE, "");
    CHECK(r && r->type == OC_RES_CHANNEL_ERR && r->err_code == OC_ERR_FORBIDDEN);
    oc_dbres_free(r);

    r = chan_update(w, alice, ch, OC_CHUP_ARCHIVE, "");
    CHECK(r && r->type == OC_RES_CHANNEL_INFO && r->ch_archived == 1);
    oc_dbres_free(r);

    memset(idem, 0xE2, sizeof idem);
    {
        oc_job *sj = oc_job_new(OC_JOB_SEND, 342);
        sj->user_id = alice; sj->channel_id = ch;
        memcpy(sj->idem, idem, OC_IDEM_LEN);
        oc_job_set_body(sj, "after archiving", 15);
        oc_dbwriter_submit(w, sj);
        oc_dbres *sr = wait_result(w);
        CHECK(sr && sr->type == OC_RES_SEND_ERR && sr->err_code == OC_ERR_CHANNEL_ARCHIVED);
        oc_dbres_free(sr);
    }
    /* Read-only, not gone: history is still retrievable (REQ-035). */
    r = backfill(w, alice, ch, 0);
    CHECK(r && r->n_replay == 1);
    oc_dbres_free(r);

    /* A member still sees it listed (they need the way back); the archived flag
     * travels so a client can render it differently. */
    r = list_channels(w, alice);
    CHECK(r && r->type == OC_RES_CHANNEL_LIST);
    int seen = 0;
    for (size_t i = 0; i < r->n_chlist; i++)
        if (r->chlist[i].channel_id == ch) { seen = 1; CHECK(r->chlist[i].archived == 1); }
    CHECK(seen);
    oc_dbres_free(r);

    /* Unarchive restores writability. */
    r = chan_update(w, alice, ch, OC_CHUP_UNARCHIVE, "");
    CHECK(r && r->type == OC_RES_CHANNEL_INFO && r->ch_archived == 0);
    oc_dbres_free(r);
    {
        oc_job *sj = oc_job_new(OC_JOB_SEND, 343);
        sj->user_id = alice; sj->channel_id = ch;
        memcpy(sj->idem, idem, OC_IDEM_LEN);
        oc_job_set_body(sj, "after unarchiving", 17);
        oc_dbwriter_submit(w, sj);
        oc_dbres *sr = wait_result(w);
        CHECK(sr && sr->type == OC_RES_SEND_OK);
        oc_dbres_free(sr);
    }

    /* A DM has no name to rename and no topic worth setting. */
    {
        oc_job *dj = oc_job_new(OC_JOB_OPEN_DM, 344);
        dj->user_id = alice; dj->target_user_id = bob;
        oc_dbwriter_submit(w, dj);
        oc_dbres *dr = wait_result(w);
        CHECK(dr && dr->type == OC_RES_CHANNEL_INFO);
        uint64_t dm = dr->channel_id;
        oc_dbres_free(dr);
        r = chan_update(w, alice, dm, OC_CHUP_TOPIC, "nope");
        CHECK(r && r->type == OC_RES_CHANNEL_ERR && r->err_code == OC_ERR_INVALID_CHANNEL);
        oc_dbres_free(r);
    }

    oc_dbwriter_stop(w);
    cleanup_db(path);
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

    /* A name already in use is rejected, case-insensitively (migration 0020) —
     * two #test channels are indistinguishable in a sidebar, and the caller gets
     * CHANNEL_EXISTS rather than a constraint failure. The privacy flag and the
     * creator do not make a name available again. */
    r = create_channel(w, alice, "secret", 1);
    CHECK(r && r->type == OC_RES_CHANNEL_ERR && r->err_code == OC_ERR_CHANNEL_EXISTS);
    oc_dbres_free(r);
    r = create_channel(w, bob, "SeCrEt", 0);
    CHECK(r && r->type == OC_RES_CHANNEL_ERR && r->err_code == OC_ERR_CHANNEL_EXISTS);
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

    /* The case that actually states the rule: a user who has NEVER joined.
     * carol above was auto-joined by posting, so her replay proves nothing
     * about membership. dave joins nothing and posts nothing, and an explicit
     * cursor on a public channel still replays it — public means readable by
     * anyone in the tenant (REQ-031), and backfill applies the same read gate
     * as every other read. Asserted because it has twice been read as a leak;
     * pinning it means a change in either direction has to be deliberate. */
    uint64_t dave = reg(w, "dave", "pw", OC_ROLE_MEMBER);
    /* A public channel is listed for everyone (that is how it is discoverable),
     * carrying joined = 0 for someone who has not joined — which is what makes
     * dave a genuine non-member for the assertion below. */
    r = list_channels(w, dave);
    joined = -1;
    CHECK(r && list_has(r, townhall, &joined) && joined == 0);
    oc_dbres_free(r);
    r = backfill(w, dave, townhall, 0);
    CHECK(r && r->type == OC_RES_BACKFILL_OK && r->n_replay >= 1);
    oc_dbres_free(r);

    /* And the gate is a gate: the same non-member gets nothing from a private
     * channel that does have messages. */
    r = backfill(w, dave, secret, 0);
    CHECK(r && r->type == OC_RES_BACKFILL_OK && r->n_replay == 0);
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

    /* A backfill carries the reaction state of what it replays. A BROADCAST has
     * no room for it, so without this every reaction disappeared the moment a
     * client reloaded — permanently, since clients keep no local cache
     * (ARCH-88). The aggregate's user_id must be the REQUESTING user whenever
     * they are one of the reactors, because that is what marks the chip as
     * theirs; for a reaction they did not make it is somebody else. */
    {
        oc_dbres *bf = backfill(w, alice, OC_DEFAULT_CHANNEL, 0);
        CHECK(bf && bf->type == OC_RES_BACKFILL_OK);
        int found_own = 0, found_other = 0;
        for (size_t i = 0; i < bf->n_rreact; i++) {
            if (bf->rreact[i].message_id != mid) continue;
            if (strcmp(bf->rreact[i].emoji, ":+1:") == 0) {          /* alice's own */
                found_own = 1;
                CHECK(bf->rreact[i].count == 1);
                CHECK(bf->rreact[i].user_id == alice);
            } else if (strcmp(bf->rreact[i].emoji, ":tada:") == 0) {  /* carol's */
                found_other = 1;
                CHECK(bf->rreact[i].count == 1);
                CHECK(bf->rreact[i].user_id == carol);
            }
        }
        CHECK(found_own && found_other);
        oc_dbres_free(bf);
    }

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


/* Search filters (REQ-081) and paging.
 *
 * This asserts that every filter combination BUILDS and EXECUTES — no malformed SQL,
 * no crash, a well-formed SEARCH result each time — and the relative property that a
 * filter never widens a result set.
 *
 * It does NOT assert absolute row counts, and that is deliberate rather than lazy:
 * searches issued in this suite return 0 rows even after a second of retries, while
 * the identical SQL run against the same database by hand returns all six. The
 * filters-only path (which never touches FTS) is empty too, so the read-only
 * connection (ARCH-66) is not seeing the writer's committed rows at all in this
 * scenario. That is a real defect and a bigger one than this feature — filed separately. Asserting counts here would either fail for a reason unrelated to filters
 * or, worse, be "fixed" by weakening them until they said nothing.
 *
 * The filter SQL itself was verified by capturing the generated statement and running
 * it against the test database directly: 6 rows unfiltered, 3 for from:, exact keyset
 * pages, 0 for a past date window. */
static oc_dbres *search_f(oc_dbwriter *w, uint64_t uid, const char *text,
                          const char *from, const char *in, uint8_t has,
                          uint64_t before_id, uint64_t after_ms, uint64_t before_ms,
                          uint16_t limit) {
    oc_job *j = oc_job_new(OC_JOB_SEARCH, 140);
    j->user_id = uid;
    j->search_limit = limit;
    if (text && text[0]) oc_job_set_body(j, text, strlen(text));
    if (from && from[0]) j->sq_from = strdup(from);
    if (in && in[0])     j->sq_in = strdup(in);
    j->sq_has = has;
    j->message_id = before_id;
    j->sq_after = after_ms;
    j->sq_before = before_ms;
    oc_dbwriter_submit(w, j);
    return wait_result(w);
}

static void test_search_filters_and_paging(void) {
    const char *path = "build/test_dbwriter_searchf.db";
    cleanup_db(path);
    oc_dbwriter *w = oc_dbwriter_start(path);
    CHECK(w != NULL);
    uint64_t alice = reg(w, "sf-alice", "pw", OC_ROLE_OWNER);
    uint64_t bob   = reg(w, "sf-bob",   "pw", OC_ROLE_MEMBER);
    CHECK(alice && bob);

    /* Six messages, alternating authors: alice gets ids 1,3,5 and bob 2,4,6 in the
     * default channel, which both are members of. */
    uint8_t idem[OC_IDEM_LEN];
    for (int i = 0; i < 6; i++) {
        memset(idem, 0xC0 + i, sizeof idem);
        CHECK(send_msg(w, (i % 2) ? bob : alice, idem, "rollback plan detail"));
    }

    /* This suite previously asserted r->n_replay and therefore asserted NOTHING:
     * search results land in r->search / r->n_search (the netloop reads those), so
     * every count was a constant zero and every "filter narrows" check passed
     * vacuously. It was filed as "the read connection sees no rows" — on
     * that evidence. The read connection was fine; the test was reading the wrong
     * field, which is a sharper warning than the bug it was mistaken for: a search
     * test that cannot see results cannot tell a working search from a broken one. */
    oc_dbres *r = search_f(w, alice, "rollback", NULL, NULL, 0, 0, 0, 0, 50);
    CHECK(r && r->type == OC_RES_SEARCH && r->n_search == 6);
    uint64_t newest = (r && r->n_search) ? r->search[0].message_id : 0;
    uint64_t oldest = (r && r->n_search) ? r->search[r->n_search - 1].message_id : 0;
    CHECK(newest > oldest);                       /* ORDER BY m.id DESC */
    oc_dbres_free(r);

    /* from: — three of the six are bob's, matched case-insensitively on the display
     * name, and a name nobody has matches nothing rather than everything. */
    r = search_f(w, alice, "rollback", "sf-bob", NULL, 0, 0, 0, 0, 50);
    CHECK(r && r->n_search == 3);
    oc_dbres_free(r);
    r = search_f(w, alice, "rollback", "SF-BOB", NULL, 0, 0, 0, 0, 50);
    CHECK(r && r->n_search == 3);
    oc_dbres_free(r);
    r = search_f(w, alice, "rollback", "nobody", NULL, 0, 0, 0, 0, 50);
    CHECK(r && r->n_search == 0);
    oc_dbres_free(r);

    /* Filters ALONE are a search: no text, so the FTS join disappears. */
    r = search_f(w, alice, NULL, "sf-bob", NULL, 0, 0, 0, 0, 50);
    CHECK(r && r->n_search == 3);
    oc_dbres_free(r);

    /* in: — the default channel by name, and an unknown channel is empty. */
    r = search_f(w, alice, "rollback", NULL, "general", 0, 0, 0, 0, 50);
    CHECK(r && r->n_search == 6);
    oc_dbres_free(r);
    r = search_f(w, alice, "rollback", NULL, "nosuchchannel", 0, 0, 0, 0, 50);
    CHECK(r && r->n_search == 0);
    oc_dbres_free(r);

    /* has: — none of these six carries an attachment or a URL, so all three masks
     * are empty. Asserted as zero rather than skipped: "has:file found everything"
     * is exactly the failure a vacuous test hides. */
    r = search_f(w, alice, "rollback", NULL, NULL, 0x01u, 0, 0, 0, 50);
    CHECK(r && r->n_search == 0);
    oc_dbres_free(r);
    r = search_f(w, alice, "rollback", NULL, NULL, 0x02u, 0, 0, 0, 50);
    CHECK(r && r->n_search == 0);
    oc_dbres_free(r);
    r = search_f(w, alice, "rollback", NULL, NULL, 0x04u, 0, 0, 0, 50);
    CHECK(r && r->n_search == 0);
    oc_dbres_free(r);

    /* ... and one WITH a link is found by has:link, which is what makes the zeros
     * above meaningful. */
    memset(idem, 0xD0, sizeof idem);
    CHECK(send_msg(w, alice, idem, "rollback notes https://example.com/x"));
    r = search_f(w, alice, "rollback", NULL, NULL, 0x02u, 0, 0, 0, 50);
    CHECK(r && r->n_search == 1);
    oc_dbres_free(r);

    /* Dates. Everything was written just now, so "since 1970" is all seven and
     * "up to 1970" is none — the two directions cannot both be right by accident. */
    r = search_f(w, alice, "rollback", NULL, NULL, 0, 0, 1000, 0, 50);
    CHECK(r && r->n_search == 7);
    oc_dbres_free(r);
    r = search_f(w, alice, "rollback", NULL, NULL, 0, 0, 0, 1000, 50);
    CHECK(r && r->n_search == 0);
    oc_dbres_free(r);

    /* The keyset cursor: id < before_id. Paging with it must cover every
     * row exactly once — the property an OFFSET loses the moment someone posts. */
    r = search_f(w, alice, "rollback", NULL, NULL, 0, 0, 0, 0, 3);
    CHECK(r && r->n_search == 3 && r->truncated);
    uint64_t cursor = (r && r->n_search) ? r->search[r->n_search - 1].message_id : 0;
    uint64_t first_page[3] = { 0, 0, 0 };
    if (r) for (size_t i = 0; i < r->n_search && i < 3; i++) first_page[i] = r->search[i].message_id;
    oc_dbres_free(r);

    r = search_f(w, alice, "rollback", NULL, NULL, 0, cursor, 0, 0, 3);
    CHECK(r && r->n_search == 3);
    if (r) for (size_t i = 0; i < r->n_search; i++) {
        CHECK(r->search[i].message_id < cursor);          /* strictly older */
        for (int k = 0; k < 3; k++) CHECK(r->search[i].message_id != first_page[k]);
    }
    uint64_t cursor2 = (r && r->n_search) ? r->search[r->n_search - 1].message_id : 0;
    oc_dbres_free(r);

    r = search_f(w, alice, "rollback", NULL, NULL, 0, cursor2, 0, 0, 3);
    CHECK(r && r->n_search == 1 && !r->truncated);        /* 7 = 3 + 3 + 1 */
    oc_dbres_free(r);

    /* A query with neither text nor filters returns nothing rather than everything. */
    r = search_f(w, alice, NULL, NULL, NULL, 0, 0, 0, 0, 50);
    CHECK(r && r->type == OC_RES_SEARCH && r->n_search == 0);
    oc_dbres_free(r);

    /* Filters COMBINE as AND, not OR: bob's messages in #general are three, and
     * bob's messages in a channel that does not exist are none. */
    r = search_f(w, alice, "rollback", "sf-bob", "general", 0, 0, 0, 0, 50);
    CHECK(r && r->n_search == 3);
    oc_dbres_free(r);
    r = search_f(w, alice, "rollback", "sf-bob", "nosuchchannel", 0, 0, 0, 0, 50);
    CHECK(r && r->n_search == 0);
    oc_dbres_free(r);

    oc_dbwriter_stop(w);
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

    /* No prior ack: a cursorless backfill replays the channel (3 < the tail). */
    oc_dbres *r = backfill0(w, bob);
    CHECK(r && r->type == OC_RES_BACKFILL_OK && r->n_replay == 3);
    oc_dbres_free(r);

    /* bob acks m2; a synchronous send afterwards flushes the writer past the
     * ack (FIFO), so the reader sees the committed cursor. */
    client_ack(w, bob, OC_DEFAULT_CHANNEL, m2);
    memset(idem, 4, sizeof idem); uint64_t m4 = send_msg(w, alice, idem, "four");
    CHECK(m4 > m3);

    /* The stored cursor must NOT narrow the replay. A cursorless request comes
     * from a client holding no history, so it still gets the channel's newest
     * page — all four here. Resuming from the read position instead is how a
     * caught-up user ended up with an empty channel on every launch; the cursor
     * places the unread divider (REQ-236), it does not decide what exists. */
    r = backfill0(w, bob);
    CHECK(r && r->type == OC_RES_BACKFILL_OK && r->n_replay == 4);
    CHECK(r->replay[0].message_id == m1 && r->replay[3].message_id == m4);
    oc_dbres_free(r);

    /* The cursor itself still advances only forward — acking an older id does
     * not rewind it — which is what read-receipts depend on. */
    client_ack(w, bob, OC_DEFAULT_CHANNEL, m1);
    memset(idem, 5, sizeof idem); uint64_t m5 = send_msg(w, alice, idem, "five");
    r = backfill0(w, bob);
    CHECK(r && r->n_replay == 5 && r->replay[4].message_id == m5);
    oc_dbres_free(r);
    {
        sqlite3 *raw = NULL;
        CHECK(sqlite3_open(path, &raw) == SQLITE_OK);
        sqlite3_stmt *st = NULL;
        sqlite3_prepare_v2(raw, "SELECT message_id FROM delivery_cursors WHERE user_id=?;",
                           -1, &st, NULL);
        sqlite3_bind_int64(st, 1, (sqlite3_int64)bob);
        CHECK(sqlite3_step(st) == SQLITE_ROW);
        CHECK((uint64_t)sqlite3_column_int64(st, 0) == m2);   /* not rewound to m1 */
        sqlite3_finalize(st);
        sqlite3_close(raw);
    }

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
    const char *first_key = r->storage_key;
    char key_copy[64];
    snprintf(key_copy, sizeof key_copy, "%s", first_key ? first_key : "");
    oc_dbres_free(r);

    /* Retrying the SAME (channel, token) hands back the SAME row, rather than
     * minting a second pending one. This is what a client does after a dropped
     * connection: it never heard the first answer, so it asks again — and until
     * migration 0037 every attempt left another orphan row behind, reclaimed
     * eventually by the storage sweep but real until then. Same id AND same
     * storage key, because the retrying client's next move is to stream bytes. */
    j = oc_job_new(OC_JOB_ATTACH_CREATE, 1);
    j->user_id = alice; j->channel_id = OC_DEFAULT_CHANNEL; j->att_size = 2048;
    memcpy(j->idem, idem, sizeof idem);
    j->filename = strdup("notes.txt"); j->mime = strdup("text/plain");
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_ATTACH_CREATED);
    CHECK(r && r->attachment_id == aid);
    CHECK(r && r->storage_key && strcmp(r->storage_key, key_copy) == 0);
    CHECK(r && r->duplicate == 1);   /* and it says so, as SEND does */
    oc_dbres_free(r);

    /* A DIFFERENT token on the same channel is a different upload. Without
     * this the "fix" could simply be returning the first row for everything. */
    {
        uint8_t idem2[OC_IDEM_LEN]; memset(idem2, 0x77, sizeof idem2);
        j = oc_job_new(OC_JOB_ATTACH_CREATE, 1);
        j->user_id = alice; j->channel_id = OC_DEFAULT_CHANNEL; j->att_size = 2048;
        memcpy(j->idem, idem2, sizeof idem2);
        j->filename = strdup("other.txt"); j->mime = strdup("text/plain");
        oc_dbwriter_submit(w, j);
        r = wait_result(w);
        CHECK(r && r->type == OC_RES_ATTACH_CREATED);
        CHECK(r && r->attachment_id != aid);
        CHECK(r && r->duplicate == 0);
        oc_dbres_free(r);
    }

    /* And ANOTHER USER replaying alice's token on the same channel gets their
     * own new row, never hers. This is the case that makes the lookup key
     * (channel, uploader, token) rather than SEND's (channel, token): what comes
     * back is a storage key the net loop opens for WRITING, so handing bob the
     * row alice declared would let him stream over her file. Guessing 128 random
     * bits is not a threat — but the row already records its owner, so there is
     * no reason for the question to exist. */
    {
        j = oc_job_new(OC_JOB_ATTACH_CREATE, 1);
        j->user_id = bob; j->channel_id = OC_DEFAULT_CHANNEL; j->att_size = 2048;
        memcpy(j->idem, idem, sizeof idem);          /* alice's token, verbatim */
        j->filename = strdup("notes.txt"); j->mime = strdup("text/plain");
        oc_dbwriter_submit(w, j);
        r = wait_result(w);
        CHECK(r && r->type == OC_RES_ATTACH_CREATED);
        CHECK(r && r->attachment_id != aid);         /* not alice's row */
        CHECK(r && r->storage_key && strcmp(r->storage_key, key_copy) != 0);
        CHECK(r && r->duplicate == 0);
        oc_dbres_free(r);
    }

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

/* Invite management (REQ-026) and webhook lifecycle. Both touch
 * tables whose columns already existed, so the risk is in the ops and their
 * authorisation — which is what this asserts. */
static void test_invites_and_webhook_lifecycle(void) {
    const char *path = "build/test_dbwriter_invites.db";
    cleanup_db(path);
    oc_dbwriter *w = oc_dbwriter_start(path);
    CHECK(w != NULL);

    uint64_t owner  = reg(w, "iv-owner",  "pw", OC_ROLE_OWNER);
    uint64_t member = reg(w, "iv-member", "pw", OC_ROLE_MEMBER);
    CHECK(owner && member);

    /* Mint two invites, then list them. */
    for (int i = 0; i < 2; i++) {
        oc_job *j = oc_job_new(OC_JOB_INVITE_USER, 1);
        j->user_id = owner; j->role = OC_ROLE_MEMBER;
        oc_dbwriter_submit(w, j);
        oc_dbres *r = wait_result(w);
        CHECK(r && r->type == OC_RES_INVITE_OK);
        oc_dbres_free(r);
    }

    oc_job *j = oc_job_new(OC_JOB_LIST_INVITES, 2);
    j->user_id = owner;
    oc_dbwriter_submit(w, j);
    oc_dbres *r = wait_result(w);
    CHECK(r && r->type == OC_RES_INVITE_LIST && r->n_invites == 2);
    uint64_t iid = r->n_invites ? r->invites[0].invite_id : 0;
    CHECK(iid != 0);
    /* The role travels; the TOKEN must not exist in this result at all. */
    CHECK(r->invites[0].role == OC_ROLE_MEMBER);
    CHECK(r->invites[0].expires_at > 0);
    oc_dbres_free(r);

    /* A plain member may not list. */
    j = oc_job_new(OC_JOB_LIST_INVITES, 3);
    j->user_id = member;
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_LIST_ERR && r->err_code == OC_ERR_FORBIDDEN);
    oc_dbres_free(r);

    /* Nor revoke. */
    j = oc_job_new(OC_JOB_REVOKE_INVITE, 4);
    j->user_id = member; j->message_id = iid;
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_LIST_ERR && r->err_code == OC_ERR_FORBIDDEN);
    oc_dbres_free(r);

    /* The owner can, and it disappears from the list. */
    j = oc_job_new(OC_JOB_REVOKE_INVITE, 5);
    j->user_id = owner; j->message_id = iid;
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_INVITE_REVOKED && r->message_id == iid);
    oc_dbres_free(r);

    j = oc_job_new(OC_JOB_LIST_INVITES, 6);
    j->user_id = owner;
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_INVITE_LIST && r->n_invites == 1);
    oc_dbres_free(r);

    /* Revoking it twice fails rather than silently succeeding. */
    j = oc_job_new(OC_JOB_REVOKE_INVITE, 7);
    j->user_id = owner; j->message_id = iid;
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_LIST_ERR);
    oc_dbres_free(r);

    /* ---- webhook disable / rotate ---- */
    j = oc_job_new(OC_JOB_CREATE_WEBHOOK, 8);
    j->user_id = owner; j->channel_id = OC_DEFAULT_CHANNEL; j->ch_name = strdup("hook");
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_WEBHOOK_CREATED);
    uint64_t wid = r->message_id;
    uint8_t tok1[OC_SESSION_TOKEN_LEN];
    memcpy(tok1, r->session_token, sizeof tok1);
    oc_dbres_free(r);

    /* Disabling answers with the channel's list, and the row says so. */
    j = oc_job_new(OC_JOB_SET_WEBHOOK_STATE, 9);
    j->user_id = owner; j->message_id = wid; j->hook_disabled = 1;
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_WEBHOOK_LIST && r->n_whlist >= 1);
    int saw_disabled = 0;
    for (size_t i = 0; i < r->n_whlist; i++)
        if (r->whlist[i].id == wid && r->whlist[i].disabled) saw_disabled = 1;
    CHECK(saw_disabled);
    oc_dbres_free(r);

    /* A disabled webhook's token stops posting — the point of disabling it. */
    j = oc_job_new(OC_JOB_WEBHOOK_POST, 10);
    oc_job_set_token(j, tok1, sizeof tok1);
    oc_job_set_body(j, "nope", 4);
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_WEBHOOK_ERR);
    oc_dbres_free(r);

    /* Re-enable, then ROTATE: a new token arrives and the old one dies. */
    j = oc_job_new(OC_JOB_SET_WEBHOOK_STATE, 11);
    j->user_id = owner; j->message_id = wid; j->hook_disabled = 0;
    oc_dbwriter_submit(w, j);
    r = wait_result(w); CHECK(r && r->type == OC_RES_WEBHOOK_LIST); oc_dbres_free(r);

    j = oc_job_new(OC_JOB_ROTATE_WEBHOOK, 12);
    j->user_id = owner; j->message_id = wid;
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_WEBHOOK_CREATED && r->message_id == wid);
    uint8_t tok2[OC_SESSION_TOKEN_LEN];
    memcpy(tok2, r->session_token, sizeof tok2);
    CHECK(memcmp(tok1, tok2, sizeof tok1) != 0);
    oc_dbres_free(r);

    j = oc_job_new(OC_JOB_WEBHOOK_POST, 13);
    oc_job_set_token(j, tok1, sizeof tok1);
    oc_job_set_body(j, "old", 3);
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_WEBHOOK_ERR);      /* the rotated-away token */
    oc_dbres_free(r);

    j = oc_job_new(OC_JOB_WEBHOOK_POST, 14);
    oc_job_set_token(j, tok2, sizeof tok2);
    oc_job_set_body(j, "new", 3);
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_WEBHOOK_POSTED);   /* the new one works */
    oc_dbres_free(r);

    /* ARCHIVING STOPS A WEBHOOK TOO (REQ-035).
     *
     * A webhook is the one writer with no user behind it, so it cannot inherit
     * the read-only rule from channel_post_access the way SEND and SEND_REPLY
     * do — and it did not have one of its own. A live token wrote into a channel
     * the product presents as read-only and was answered {"ok":true}.
     *
     * The token stays VALID: this asserts the archived error specifically, not
     * merely that the post failed, because failing as "unknown webhook" would
     * tell an integration its token had been revoked — a different problem with
     * a different fix. */
    {
        oc_job *aj = oc_job_new(OC_JOB_UPDATE_CHANNEL, 15);
        aj->user_id = owner; aj->channel_id = OC_DEFAULT_CHANNEL;
        aj->chup_op = OC_CHUP_ARCHIVE;
        oc_dbwriter_submit(w, aj);
        oc_dbres_free(wait_result(w));
    }
    j = oc_job_new(OC_JOB_WEBHOOK_POST, 16);
    oc_job_set_token(j, tok2, sizeof tok2);
    oc_job_set_body(j, "while archived", 14);
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_WEBHOOK_ERR);
    CHECK(r && r->err_code == OC_ERR_CHANNEL_ARCHIVED);
    oc_dbres_free(r);

    /* And unarchiving restores it, so this is read-only rather than a token
     * that has been quietly burned. */
    {
        oc_job *uj = oc_job_new(OC_JOB_UPDATE_CHANNEL, 17);
        uj->user_id = owner; uj->channel_id = OC_DEFAULT_CHANNEL;
        uj->chup_op = OC_CHUP_UNARCHIVE;
        oc_dbwriter_submit(w, uj);
        oc_dbres_free(wait_result(w));
    }
    j = oc_job_new(OC_JOB_WEBHOOK_POST, 18);
    oc_job_set_token(j, tok2, sizeof tok2);
    oc_job_set_body(j, "after unarchive", 15);
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_WEBHOOK_POSTED);
    oc_dbres_free(r);

    /* Authorisation is by CHANNEL membership, and the default channel is public, so
     * `member` legitimately CAN rotate that one — the earlier version of this test
     * asserted the opposite and failed, which was the test being wrong about its own
     * premise rather than the rule. A private channel gives a real non-member. */
    r = create_channel(w, owner, "hooks-priv", 0);
    CHECK(r && r->type == OC_RES_CHANNEL_INFO);
    uint64_t priv = r->channel_id;
    oc_dbres_free(r);

    j = oc_job_new(OC_JOB_CREATE_WEBHOOK, 15);
    j->user_id = owner; j->channel_id = priv; j->ch_name = strdup("secret");
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_WEBHOOK_CREATED);
    uint64_t pwid = r->message_id;
    oc_dbres_free(r);

    j = oc_job_new(OC_JOB_ROTATE_WEBHOOK, 16);
    j->user_id = member; j->message_id = pwid;
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_WEBHOOK_ERR && r->err_code == OC_ERR_NOT_A_MEMBER);
    oc_dbres_free(r);

    j = oc_job_new(OC_JOB_SET_WEBHOOK_STATE, 17);
    j->user_id = member; j->message_id = pwid; j->hook_disabled = 1;
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_WEBHOOK_ERR && r->err_code == OC_ERR_NOT_A_MEMBER);
    oc_dbres_free(r);

    oc_dbwriter_stop(w);
}


/* Custom status (REQ-241/122) and profile fields (REQ-240).
 *
 * The interesting rule is EXPIRY: the daemon must stop reporting a status once it
 * lapses, whether or not the client that set it is still running. */
static void test_status_and_profile(void) {
    const char *path = "build/test_dbwriter_status.db";
    cleanup_db(path);
    oc_dbwriter *w = oc_dbwriter_start(path);
    CHECK(w != NULL);
    uint64_t alice = reg(w, "st-alice", "pw", OC_ROLE_OWNER);
    CHECK(alice);

    /* No status to begin with. */
    oc_job *j = oc_job_new(OC_JOB_GET_PROFILE, 1);
    j->user_id = alice;
    oc_dbwriter_submit(w, j);
    oc_dbres *r = wait_result(w);
    CHECK(r && r->type == OC_RES_PROFILE_INFO && r->user_id == alice);
    CHECK(r->st_emoji && r->st_emoji[0] == '\0');
    CHECK(r->st_text  && r->st_text[0]  == '\0');
    oc_dbres_free(r);

    /* Set one with an expiry well in the future: it comes back intact. */
    j = oc_job_new(OC_JOB_SET_STATUS, 2);
    j->user_id = alice;
    oc_job_set_body(j, "\xF0\x9F\x8D\x95", 4);        /* pizza */
    j->ch_name = strdup("out for lunch");
    j->message_id = (uint64_t)time(NULL) * 1000ull + 3600000ull;
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_PROFILE_INFO);
    CHECK(r->st_text && strcmp(r->st_text, "out for lunch") == 0);
    CHECK(r->st_expires != 0);
    oc_dbres_free(r);

    /* An expiry in the PAST reads as no status at all — the daemon applies the rule
     * on read, so nobody sees a stale status even before any cleanup. */
    j = oc_job_new(OC_JOB_SET_STATUS, 3);
    j->user_id = alice;
    oc_job_set_body(j, "\xF0\x9F\x8D\x95", 4);
    j->ch_name = strdup("stale");
    j->message_id = 1000ull;                            /* 1970 */
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_PROFILE_INFO);
    CHECK(r->st_text && r->st_text[0] == '\0');          /* suppressed */
    CHECK(r->st_expires == 0);
    oc_dbres_free(r);

    /* Clearing: empty text wipes the emoji and the expiry together. */
    j = oc_job_new(OC_JOB_SET_STATUS, 4);
    j->user_id = alice;
    j->ch_name = strdup("");
    j->message_id = 0;
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->st_emoji[0] == '\0' && r->st_expires == 0);
    oc_dbres_free(r);

    /* Profile fields round-trip and do not disturb the status. All five are
     * written together, because the screen commits them together. */
    j = oc_job_new(OC_JOB_SET_PROFILE, 5);
    j->user_id = alice;
    j->pf_full_name = strdup("Alice Liddell");
    j->pf_title     = strdup("Principal Engineer");
    j->pf_pronouns  = strdup("she/her");
    j->pf_phone     = strdup("+1 555 0100");
    j->pf_timezone  = strdup("America/New_York");
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->pf_title && strcmp(r->pf_title, "Principal Engineer") == 0);
    CHECK(r->pf_tz && strcmp(r->pf_tz, "America/New_York") == 0);
    CHECK(r->pf_full_name && strcmp(r->pf_full_name, "Alice Liddell") == 0);
    CHECK(r->pf_pronouns && strcmp(r->pf_pronouns, "she/her") == 0);
    CHECK(r->pf_phone && strcmp(r->pf_phone, "+1 555 0100") == 0);
    CHECK(r->role == OC_ROLE_OWNER);
    /* The status set above survives a profile write: one screen's fields must
     * not clear another's. */
    CHECK(r->st_emoji && r->st_emoji[0] == '\0');
    oc_dbres_free(r);

    /* An empty field CLEARS the column rather than leaving the old value: the
     * user deleted it, which is a thing they did. */
    j = oc_job_new(OC_JOB_SET_PROFILE, 5);
    j->user_id = alice;
    j->pf_full_name = strdup("");
    j->pf_title     = strdup("");
    j->pf_pronouns  = strdup("");
    j->pf_phone     = strdup("");
    j->pf_timezone  = strdup("");
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->pf_title && r->pf_title[0] == '\0');
    CHECK(r->pf_full_name && r->pf_full_name[0] == '\0');
    CHECK(r->pf_phone && r->pf_phone[0] == '\0');
    oc_dbres_free(r);

    oc_dbwriter_stop(w);
}

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
/* Custom emoji (REQ-072). The name IS the identity — `:shipit:` must mean one image
 * workspace-wide — so the properties are: the name is validated and lowercased, the
 * image must be one the caller uploaded (it becomes readable workspace-wide), a
 * duplicate is refused rather than silently replacing what every existing message
 * containing that shortcode means, and only the creator or an admin can delete one. */
static void test_custom_emoji(void) {
    const char *path = "build/test_dbwriter_emoji.db";
    cleanup_db(path);
    oc_dbwriter *w = oc_dbwriter_start(path);
    CHECK(w != NULL);
    uint64_t alice = reg(w, "ce-alice", "pw", OC_ROLE_OWNER);
    uint64_t bob   = reg(w, "ce-bob",   "pw", OC_ROLE_MEMBER);
    uint64_t carol = reg(w, "ce-carol", "pw", OC_ROLE_MEMBER);
    CHECK(alice && bob && carol);
    {
        sqlite3 *raw = NULL;
        CHECK(sqlite3_open(path, &raw) == SQLITE_OK);
        char sql[512];
        snprintf(sql, sizeof sql,
            "INSERT INTO attachments(id,channel_id,message_id,uploader_id,storage_key,"
            "  filename,mime,size,sha256,created_at_ms) VALUES"
            "  (1,1,NULL,%llu,'k1','a.png','image/png',10,x'00',1),"
            "  (2,1,NULL,%llu,'k2','b.pdf','application/pdf',10,x'00',1),"
            "  (3,1,NULL,%llu,'k3','c.png','image/png',10,x'00',1);",
            (unsigned long long)bob, (unsigned long long)bob, (unsigned long long)carol);
        CHECK(sqlite3_exec(raw, sql, NULL, NULL, NULL) == SQLITE_OK);
        sqlite3_close(raw);
    }

    /* Empty to begin with. */
    oc_job *j = oc_job_new(OC_JOB_LIST_EMOJI, 1);
    j->user_id = alice;
    oc_dbwriter_submit(w, j);
    oc_dbres *r = wait_result(w);
    CHECK(r && r->type == OC_RES_EMOJI_LIST && r->n_elist == 0);
    oc_dbres_free(r);

    /* bob adds one, and the NAME is lowercased — `:Shipit:` and `:shipit:` must not
     * be two different emoji. The answer is the whole catalogue, fanned out. */
    j = oc_job_new(OC_JOB_ADD_EMOJI, 2);
    j->user_id = bob; j->message_id = 1; j->ch_name = strdup("ShipIt");
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_EMOJI_LIST && r->n_elist == 1 && r->ch_fanout);
    if (r && r->n_elist == 1) {
        CHECK(strcmp(r->elist[0].name, "shipit") == 0);
        CHECK(r->elist[0].attachment_id == 1 && r->elist[0].created_by == bob);
    }
    oc_dbres_free(r);

    /* A name with a colon in it is refused: it would be unparseable in a body. */
    const char *bad_names[] = { "no:colon", "with space", "", "emoji!" };
    for (size_t k = 0; k < sizeof bad_names / sizeof bad_names[0]; k++) {
        j = oc_job_new(OC_JOB_ADD_EMOJI, 3);
        j->user_id = bob; j->message_id = 1; j->ch_name = strdup(bad_names[k]);
        oc_dbwriter_submit(w, j);
        r = wait_result(w);
        CHECK(r && r->type == OC_RES_LIST_ERR);
        oc_dbres_free(r);
    }

    /* A non-image, and SOMEBODY ELSE'S attachment, are both refused — an emoji is
     * readable workspace-wide, so the id must be one the caller was entitled to. */
    j = oc_job_new(OC_JOB_ADD_EMOJI, 4);
    j->user_id = bob; j->message_id = 2; j->ch_name = strdup("pdf");
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_LIST_ERR && r->err_code == OC_ERR_UNKNOWN_ATTACHMENT);
    oc_dbres_free(r);
    j = oc_job_new(OC_JOB_ADD_EMOJI, 5);
    j->user_id = bob; j->message_id = 3; j->ch_name = strdup("stolen");
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_LIST_ERR && r->err_code == OC_ERR_UNKNOWN_ATTACHMENT);
    oc_dbres_free(r);

    /* A duplicate name is refused rather than replacing the image, which would change
     * what every message already containing `:shipit:` means. */
    j = oc_job_new(OC_JOB_ADD_EMOJI, 6);
    j->user_id = carol; j->message_id = 3; j->ch_name = strdup("shipit");
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_LIST_ERR);
    oc_dbres_free(r);

    /* Anyone can READ it — an emoji only half the workspace can see is unusable. */
    j = oc_job_new(OC_JOB_ATTACH_LOOKUP, 7);
    j->user_id = carol; j->attachment_id = 1;
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_ATTACH_META);
    oc_dbres_free(r);

    /* carol (neither creator nor admin) cannot delete it; bob can. */
    j = oc_job_new(OC_JOB_DELETE_EMOJI, 8);
    j->user_id = carol; j->ch_name = strdup("shipit");
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_LIST_ERR && r->err_code == OC_ERR_FORBIDDEN);
    oc_dbres_free(r);

    j = oc_job_new(OC_JOB_DELETE_EMOJI, 9);
    j->user_id = bob; j->ch_name = strdup("shipit");
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_EMOJI_LIST && r->n_elist == 0 && r->ch_fanout);
    oc_dbres_free(r);

    /* An OWNER can delete somebody else's: a shortcode is workspace-wide state. */
    j = oc_job_new(OC_JOB_ADD_EMOJI, 10);
    j->user_id = bob; j->message_id = 1; j->ch_name = strdup("again");
    oc_dbwriter_submit(w, j);
    r = wait_result(w); oc_dbres_free(r);
    j = oc_job_new(OC_JOB_DELETE_EMOJI, 11);
    j->user_id = alice; j->ch_name = strdup("again");
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_EMOJI_LIST && r->n_elist == 0);
    oc_dbres_free(r);

    oc_dbwriter_stop(w);
    cleanup_db(path);
}

/* Group DMs (REQ-056). A group DM is a DM with more than two participants — the
 * same `kind`, identified by its participant set, which is what a DM has always
 * been. So the properties to prove are: it is created, reopening the same SET
 * returns the SAME conversation regardless of the order given, two participants is
 * refused (that is a 1:1 and must reuse OPEN_DM or the pair ends up with two
 * conversations), and everyone in it is a member. */
static void test_group_dm(void) {
    const char *path = "build/test_dbwriter_groupdm.db";
    cleanup_db(path);
    oc_dbwriter *w = oc_dbwriter_start(path);
    CHECK(w != NULL);
    uint64_t alice = reg(w, "gd-alice", "pw", OC_ROLE_OWNER);
    uint64_t bob   = reg(w, "gd-bob",   "pw", OC_ROLE_MEMBER);
    uint64_t carol = reg(w, "gd-carol", "pw", OC_ROLE_MEMBER);
    uint64_t dave  = reg(w, "gd-dave",  "pw", OC_ROLE_MEMBER);
    CHECK(alice && bob && carol && dave);

    oc_job *j = oc_job_new(OC_JOB_OPEN_GROUP_DM, 1);
    j->user_id = alice;
    j->group_uids[0] = bob; j->group_uids[1] = carol; j->n_group_uids = 2;
    oc_dbwriter_submit(w, j);
    oc_dbres *r = wait_result(w);
    CHECK(r && r->type == OC_RES_CHANNEL_INFO);
    uint64_t gid = r ? r->channel_id : 0;
    CHECK(gid != 0);
    /* peer 0 and three participants: that pair of facts is how a client tells a
     * group from a 1:1 without a second round trip. */
    CHECK(r && r->ch_peer == 0 && r->n_ch_peers == 3);
    /* And it fans out, or the other two would not see it until their next refresh. */
    CHECK(r && r->ch_fanout && r->n_members == 3);
    oc_dbres_free(r);

    /* The same set in a DIFFERENT order is the same conversation. */
    j = oc_job_new(OC_JOB_OPEN_GROUP_DM, 2);
    j->user_id = alice;
    j->group_uids[0] = carol; j->group_uids[1] = bob; j->n_group_uids = 2;
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->channel_id == gid);
    oc_dbres_free(r);

    /* ... and so is the same set opened by a DIFFERENT participant. */
    j = oc_job_new(OC_JOB_OPEN_GROUP_DM, 3);
    j->user_id = bob;
    j->group_uids[0] = alice; j->group_uids[1] = carol; j->n_group_uids = 2;
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->channel_id == gid);
    oc_dbres_free(r);

    /* A duplicate name collapses rather than making a "different" group. */
    j = oc_job_new(OC_JOB_OPEN_GROUP_DM, 4);
    j->user_id = alice;
    j->group_uids[0] = bob; j->group_uids[1] = bob; j->group_uids[2] = carol;
    j->n_group_uids = 3;
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->channel_id == gid);
    oc_dbres_free(r);

    /* A different set is a different conversation. */
    j = oc_job_new(OC_JOB_OPEN_GROUP_DM, 5);
    j->user_id = alice;
    j->group_uids[0] = bob; j->group_uids[1] = dave; j->n_group_uids = 2;
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->channel_id != 0 && r->channel_id != gid);
    oc_dbres_free(r);

    /* ONE other person is refused: that is a 1:1 DM. */
    j = oc_job_new(OC_JOB_OPEN_GROUP_DM, 6);
    j->user_id = alice;
    j->group_uids[0] = bob; j->n_group_uids = 1;
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_CHANNEL_ERR);
    oc_dbres_free(r);

    /* An unknown user is refused without saying which — OPEN_DM's rule. */
    j = oc_job_new(OC_JOB_OPEN_GROUP_DM, 7);
    j->user_id = alice;
    j->group_uids[0] = bob; j->group_uids[1] = 999999; j->n_group_uids = 2;
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_CHANNEL_ERR && r->err_code == OC_ERR_FORBIDDEN);
    oc_dbres_free(r);

    /* Everyone in it can post and read it; dave (not in it) cannot. */
    uint8_t idem[OC_IDEM_LEN];
    memset(idem, 0x71, sizeof idem);
    {
        oc_job *sj = oc_job_new(OC_JOB_SEND, 8);
        sj->user_id = carol; sj->channel_id = gid;
        memcpy(sj->idem, idem, OC_IDEM_LEN);
        oc_job_set_body(sj, "hi both", 7);
        oc_dbwriter_submit(w, sj);
        r = wait_result(w);
        CHECK(r && r->type == OC_RES_SEND_OK);
        oc_dbres_free(r);
    }
    memset(idem, 0x72, sizeof idem);
    {
        oc_job *sj = oc_job_new(OC_JOB_SEND, 9);
        sj->user_id = dave; sj->channel_id = gid;
        memcpy(sj->idem, idem, OC_IDEM_LEN);
        oc_job_set_body(sj, "let me in", 9);
        oc_dbwriter_submit(w, sj);
        r = wait_result(w);
        CHECK(r && r->type == OC_RES_SEND_ERR);
        oc_dbres_free(r);
    }

    /* It appears in a participant's channel list WITH its participants, so the
     * sidebar can title it on first paint. */
    j = oc_job_new(OC_JOB_LIST_CHANNELS, 10);
    j->user_id = bob;
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_CHANNEL_LIST);
    if (r) {
        int seen = 0;
        for (size_t i = 0; i < r->n_chlist; i++) {
            if (r->chlist[i].channel_id != gid) continue;
            seen = 1;
            CHECK(r->chlist[i].kind == OC_CHANNEL_KIND_DM);
            CHECK(r->chlist[i].n_peers == 3);
        }
        CHECK(seen);
    }
    oc_dbres_free(r);

    oc_dbwriter_stop(w);
    cleanup_db(path);
}

/* The avatar. Three properties, and the second is the one that matters for
 * security: an avatar is readable WORKSPACE-WIDE, so the id has to be one the setter
 * was entitled to in the first place. */
static void test_avatar(void) {
    const char *path = "build/test_dbwriter_avatar.db";
    cleanup_db(path);
    oc_dbwriter *w = oc_dbwriter_start(path);
    CHECK(w != NULL);
    uint64_t alice = reg(w, "av-alice", "pw", OC_ROLE_OWNER);
    uint64_t bob   = reg(w, "av-bob",   "pw", OC_ROLE_MEMBER);
    CHECK(alice && bob);

    /* A private channel bob cannot read, holding an image alice uploaded, plus a
     * non-image and an unfinalized row to reject. */
    oc_dbres *r = create_channel(w, alice, "avpriv", 0);
    CHECK(r && r->type == OC_RES_CHANNEL_INFO);
    uint64_t priv = r->channel_id;
    oc_dbres_free(r);
    {
        sqlite3 *raw = NULL;
        CHECK(sqlite3_open(path, &raw) == SQLITE_OK);
        char sql[512];
        snprintf(sql, sizeof sql,
            "INSERT INTO attachments(id,channel_id,message_id,uploader_id,storage_key,"
            "  filename,mime,size,sha256,created_at_ms) VALUES"
            "  (1,%llu,NULL,%llu,'k1','face.png','image/png',10,x'00',1),"      /* alice's image */
            "  (2,%llu,NULL,%llu,'k2','doc.pdf','application/pdf',10,x'00',1)," /* not an image */
            "  (3,%llu,NULL,%llu,'k3','half.png','image/png',0,NULL,1);",       /* not finalized */
            (unsigned long long)priv, (unsigned long long)alice,
            (unsigned long long)priv, (unsigned long long)alice,
            (unsigned long long)priv, (unsigned long long)alice);
        CHECK(sqlite3_exec(raw, sql, NULL, NULL, NULL) == SQLITE_OK);
        sqlite3_close(raw);
    }

    /* Setting it works, and comes back on the profile. */
    oc_job *j = oc_job_new(OC_JOB_SET_AVATAR, 1);
    j->user_id = alice; j->message_id = 1;
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_PROFILE_INFO && r->pf_avatar == 1);
    oc_dbres_free(r);

    /* A non-image, an unfinalized upload, an id that does not exist, and — the
     * important one — SOMEBODY ELSE'S attachment are all refused. Without the last
     * check any member could publish a private channel's file to the whole
     * workspace by pointing their avatar at it. */
    uint64_t bad[4] = { 2, 3, 999, 1 };
    uint64_t who[4] = { alice, alice, alice, bob };
    for (int k = 0; k < 4; k++) {
        j = oc_job_new(OC_JOB_SET_AVATAR, 2);
        j->user_id = who[k]; j->message_id = bad[k];
        oc_dbwriter_submit(w, j);
        r = wait_result(w);
        CHECK(r && r->type == OC_RES_LIST_ERR && r->err_code == OC_ERR_UNKNOWN_ATTACHMENT);
        oc_dbres_free(r);
    }

    /* alice's avatar survived every rejection. */
    j = oc_job_new(OC_JOB_GET_PROFILE, 3);
    j->user_id = alice;
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->pf_avatar == 1);
    oc_dbres_free(r);

    /* BOB can fetch it even though he cannot read the channel it lives in — a
     * picture is drawn beside its owner's messages in channels the viewer shares
     * with them, which is not where the file was uploaded. */
    j = oc_job_new(OC_JOB_ATTACH_LOOKUP, 4);
    j->user_id = bob; j->attachment_id = 1;
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_ATTACH_META);
    oc_dbres_free(r);

    /* ... and the relaxation is EXACTLY that: the other private attachment in the
     * same channel stays forbidden. */
    j = oc_job_new(OC_JOB_ATTACH_LOOKUP, 5);
    j->user_id = bob; j->attachment_id = 2;
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_ATTACH_ERR && r->err_code == OC_ERR_FORBIDDEN);
    oc_dbres_free(r);

    /* Clearing with 0, and then it is forbidden to bob again — the exception is
     * tied to being an avatar right now, not to having once been one. */
    j = oc_job_new(OC_JOB_SET_AVATAR, 6);
    j->user_id = alice; j->message_id = 0;
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_PROFILE_INFO && r->pf_avatar == 0);
    oc_dbres_free(r);
    j = oc_job_new(OC_JOB_ATTACH_LOOKUP, 7);
    j->user_id = bob; j->attachment_id = 1;
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_ATTACH_ERR && r->err_code == OC_ERR_FORBIDDEN);
    oc_dbres_free(r);

    /* The roster carries it, so a transcript needs no per-author round trip. */
    j = oc_job_new(OC_JOB_SET_AVATAR, 8);
    j->user_id = alice; j->message_id = 1;
    oc_dbwriter_submit(w, j);
    r = wait_result(w); oc_dbres_free(r);
    j = oc_job_new(OC_JOB_LIST_USERS, 9);
    j->user_id = bob;
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_USER_LIST);
    if (r) {
        int saw = 0;
        for (size_t i = 0; i < r->n_ulist; i++)
            if (r->ulist[i].user_id == alice) { CHECK(r->ulist[i].avatar_id == 1); saw = 1; }
            else CHECK(r->ulist[i].avatar_id == 0);
        CHECK(saw);
    }
    oc_dbres_free(r);

    oc_dbwriter_stop(w);
    cleanup_db(path);
}

static void test_notify_prefs(void) {
    const char *path = "build/test_dbwriter_notify.db";
    cleanup_db(path);
    oc_dbwriter *w = oc_dbwriter_start(path);
    CHECK(w != NULL);
    uint64_t alice = reg(w, "np-alice", "pw", OC_ROLE_OWNER);
    uint64_t bob   = reg(w, "np-bob",   "pw", OC_ROLE_MEMBER);
    CHECK(alice && bob);

    /* Default: no prefs, and no schedule — mode 0 is "notify me whenever". */
    oc_job *j = oc_job_new(OC_JOB_LIST_NOTIFY_PREFS, 1);
    j->user_id = alice; oc_dbwriter_submit(w, j);
    oc_dbres *r = wait_result(w);
    CHECK(r && r->type == OC_RES_NOTIFY_PREFS && r->n_nprefs == 0 && r->sc_mode == OC_DND_OFF);
    oc_dbres_free(r);

    /* Set a channel level; the snapshot reflects it. */
    j = oc_job_new(OC_JOB_SET_NOTIFY_PREF, 2);
    j->user_id = alice; j->channel_id = OC_DEFAULT_CHANNEL; j->notify_level = OC_NOTIFY_MENTIONS;
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_NOTIFY_PREFS && r->n_nprefs == 1);
    CHECK(r->nprefs[0].channel_id == OC_DEFAULT_CHANNEL && r->nprefs[0].level == OC_NOTIFY_MENTIONS);
    oc_dbres_free(r);

    /* Set the SCHEDULE (REQ-136); the per-channel pref persists alongside, and
     * the snapshot carries the schedule back. The window is the ALLOWED range
     * now — 08:00 to 22:00 is "notify me during the day", the complement of the
     * quiet window it replaced. */
    j = oc_job_new(OC_JOB_SET_SCHEDULE, 3);
    j->user_id = alice; j->sched_mode = OC_DND_EVERY_DAY;
    j->sched_start_min = 480; j->sched_end_min = 1320; j->sched_tz_offset_min = -300;
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_SCHEDULE && r->sc_mode == OC_DND_EVERY_DAY);
    CHECK(r->sc_start_min == 480 && r->sc_end_min == 1320 && r->sc_tz_offset_min == -300);
    oc_dbres_free(r);
    j = oc_job_new(OC_JOB_LIST_NOTIFY_PREFS, 3);
    j->user_id = alice; oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_NOTIFY_PREFS && r->n_nprefs == 1);
    CHECK(r->sc_mode == OC_DND_EVERY_DAY && r->sc_start_min == 480);
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

    /* REQ-134: the GLOBAL default. It is a separate column from the per-channel
     * override, so setting it must not disturb the row set above, and it must come
     * back on every prefs sync — a default the client has to guess is a default the
     * two sides will eventually disagree about. */
    j = oc_job_new(OC_JOB_LIST_NOTIFY_PREFS, 7);
    j->user_id = alice;
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_NOTIFY_PREFS && r->np_default == OC_NOTIFY_ALL);
    oc_dbres_free(r);

    j = oc_job_new(OC_JOB_SET_NOTIFY_DEFAULT, 8);
    j->user_id = alice; j->notify_level = OC_NOTIFY_MENTIONS;
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_NOTIFY_PREFS && r->np_default == OC_NOTIFY_MENTIONS);
    /* the per-channel override survives untouched */
    CHECK(r && r->n_nprefs == 1 && r->nprefs[0].level == OC_NOTIFY_NONE);
    oc_dbres_free(r);

    /* It persists, and an out-of-range level is refused rather than clamped. */
    j = oc_job_new(OC_JOB_LIST_NOTIFY_PREFS, 9);
    j->user_id = alice;
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->np_default == OC_NOTIFY_MENTIONS);
    oc_dbres_free(r);

    j = oc_job_new(OC_JOB_SET_NOTIFY_DEFAULT, 10);
    j->user_id = alice; j->notify_level = 9;
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_NOTIFY_ERR);
    oc_dbres_free(r);

    j = oc_job_new(OC_JOB_LIST_NOTIFY_PREFS, 11);
    j->user_id = alice;
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->np_default == OC_NOTIFY_MENTIONS);   /* the reject changed nothing */
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

    /* A DM's identity is its participant set (migration 0019), not its membership
     * rows — so even after the membership is destroyed (which remove_user used to
     * do), re-opening returns the SAME channel instead of silently creating a
     * duplicate conversation. This is the defect the unique dm_key forbids. */
    {
        /* Damage the membership behind the writer's back, the way an older
         * remove_user did, using a second connection to the same file. */
        sqlite3 *raw = NULL;
        CHECK(sqlite3_open(path, &raw) == SQLITE_OK);
        char sql[128];
        snprintf(sql, sizeof sql,
                 "DELETE FROM channel_members WHERE channel_id=%llu;",
                 (unsigned long long)selfdm);
        CHECK(sqlite3_exec(raw, sql, NULL, NULL, NULL) == SQLITE_OK);
        r = open_dm(w, alice, alice);
        CHECK(r && r->type == OC_RES_CHANNEL_INFO && r->channel_id == selfdm);
        CHECK(r->ch_joined == 1);          /* membership re-asserted, not orphaned */
        oc_dbres_free(r);

        /* And the unique index means a second row for that set cannot exist. */
        sqlite3_stmt *st = NULL;
        sqlite3_prepare_v2(raw,
            "SELECT COUNT(*) FROM channels WHERE kind='dm' AND dm_key IS NOT NULL "
            "GROUP BY dm_key HAVING COUNT(*) > 1;", -1, &st, NULL);
        CHECK(sqlite3_step(st) == SQLITE_DONE);   /* no participant set duplicated */
        sqlite3_finalize(st);
        sqlite3_close(raw);
    }

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
/* Link unfurls (REQ-222, ARCH-105): the store job re-validates the message,
 * refuses a URL the body no longer carries, and the stored row rides the
 * backfill replay; an edit drops the old body's rows. */
static void test_unfurl_store(void) {
    const char *path = "build/test_dbwriter_unfurl.db";
    cleanup_db(path);
    oc_dbwriter *w = oc_dbwriter_start(path);
    CHECK(w != NULL);

    uint64_t u = reg(w, "uf-user", "pw", OC_ROLE_MEMBER);
    CHECK(u != 0);
    uint8_t idem[OC_IDEM_LEN];
    memset(idem, 0xD1, sizeof idem);
    uint64_t mid = send_msg(w, u, idem, "see https://example.com/x today");
    CHECK(mid != 0);

    /* A completed fetch for a URL the body carries: stored, fanned. */
    oc_job *j = oc_job_new(OC_JOB_UNFURL_STORE, 0);
    j->channel_id = OC_DEFAULT_CHANNEL; j->message_id = mid;
    j->unf_url = strdup("https://example.com/x");
    j->unf_title = strdup("Example X");
    j->unf_descr = strdup("About X");
    oc_dbwriter_submit(w, j);
    oc_dbres *r = wait_result(w);
    CHECK(r && r->type == OC_RES_UNFURL_STORED);
    CHECK(r->message_id == mid && r->channel_id == OC_DEFAULT_CHANNEL);
    CHECK(r->unf_url && strcmp(r->unf_url, "https://example.com/x") == 0);
    CHECK(r->unf_title && strcmp(r->unf_title, "Example X") == 0);
    CHECK(r->n_members >= 1);
    oc_dbres_free(r);

    /* A URL the body does NOT carry — a fetch that outlived an edit, or a
     * forged worker result — is silently refused. */
    j = oc_job_new(OC_JOB_UNFURL_STORE, 0);
    j->channel_id = OC_DEFAULT_CHANNEL; j->message_id = mid;
    j->unf_url = strdup("https://elsewhere.example/y");
    j->unf_title = strdup("Nope");
    j->unf_descr = strdup("");
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_OK);
    oc_dbres_free(r);

    /* The stored row rides the replay (the same guarantee as reactions/pins). */
    r = backfill(w, u, OC_DEFAULT_CHANNEL, 0);
    CHECK(r && r->type == OC_RES_BACKFILL_OK);
    CHECK(r->n_runfurl == 1);
    if (r->n_runfurl == 1) {
        CHECK(r->runfurl[0].message_id == mid);
        CHECK(strcmp(r->runfurl[0].url, "https://example.com/x") == 0);
        CHECK(strcmp(r->runfurl[0].title, "Example X") == 0);
        CHECK(strcmp(r->runfurl[0].descr, "About X") == 0);
    }
    oc_dbres_free(r);

    /* An edit drops the old body's unfurls, so a replay cannot resurrect a
     * preview for text that is gone. */
    j = oc_job_new(OC_JOB_EDIT, 1);
    j->user_id = u; j->channel_id = OC_DEFAULT_CHANNEL; j->message_id = mid;
    oc_job_set_body(j, "no links any more", 17);
    oc_dbwriter_submit(w, j);
    r = wait_result(w);
    CHECK(r && r->type == OC_RES_EDIT_OK);
    oc_dbres_free(r);

    r = backfill(w, u, OC_DEFAULT_CHANNEL, 0);
    CHECK(r && r->n_runfurl == 0);
    oc_dbres_free(r);

    oc_dbwriter_stop(w);
    cleanup_db(path);
}

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
    printf("test_dbwriter: migrate-on-boot, register + local/session/oidc auth, rate-limit, roles, SEND persist/idempotency/members, backfill, mentions, pins, channel details, channel mutability, tombstone cleanup, saved items + activity\n");
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
    test_invites_and_webhook_lifecycle();
    test_status_and_profile();
    test_custom_emoji();
    test_group_dm();
    test_avatar();
    test_notify_prefs();
    test_keyword_alerts();
    test_admin_ops();
    test_reactions();
    test_threads();
    test_search();
    test_search_filters_and_paging();
    test_setup_invite();
    test_tls_identity();
    test_delivery_cursor();
    test_idem_pruning();
    test_backfill();
    test_history_paging();
    test_mentions_stored();
    test_pins();
    test_channel_details();
    test_drafts();
    test_scheduled();
    test_activity_unreads();
    test_channel_mutability();
    test_saved_and_activity();
    test_history_around();
    test_delete_clears_message_extras();
    test_max_users();
    test_unfurl_store();
    return failures;
}
