/*
 * OpenChime DB-writer thread + write-job queue. See dbwriter.h and migrate.h.
 *
 * One thread owns the write connection. It pops jobs off a request queue,
 * performs all DB work (AUTH upsert, SEND persist with idempotency), and pushes
 * results onto a completion queue, signalling the net thread via an eventfd.
 */

#include "dbwriter.h"
#include "migrate.h"
#include "protocol.h"

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
};

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
    free(j->body);
    free(j);
}

void oc_dbres_free(oc_dbres *r) {
    if (!r) return;
    free(r->body);
    free(r->members);
    free(r);
}

/* --- Job processing (runs on the writer thread) ------------------------- */

/* Look up (or insert) a user by subject; returns the user id or 0 on error. */
static uint64_t upsert_user(sqlite3 *db, const char *subject) {
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
        "INSERT OR IGNORE INTO users(subject, created_at_ms) VALUES(?, ?);", -1, &st, NULL);
    sqlite3_bind_text(st, 1, subject, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)dbw_now_ms());
    sqlite3_step(st);
    sqlite3_finalize(st);

    uint64_t id = 0;
    sqlite3_prepare_v2(db, "SELECT id FROM users WHERE subject=?;", -1, &st, NULL);
    sqlite3_bind_text(st, 1, subject, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) id = (uint64_t)sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return id;
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

static oc_dbres *process_auth(sqlite3 *db, const oc_job *j) {
    oc_dbres *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->conn_id = j->conn_id;

    uint64_t uid = upsert_user(db, j->token ? j->token : "");
    if (uid == 0) { r->type = OC_RES_AUTH_ERR; r->err_code = OC_ERR_INTERNAL; return r; }
    ensure_default_membership(db, uid);

    r->type = OC_RES_AUTH_OK;
    r->user_id = uid;
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
    r->author_id = j->author_id;
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

    if (!is_member(db, j->channel_id, j->author_id)) {
        r->type = OC_RES_SEND_ERR; r->err_code = OC_ERR_NOT_A_MEMBER;
        return r;
    }

    uint64_t ts = dbw_now_ms();
    sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);

    sqlite3_prepare_v2(db,
        "INSERT INTO messages(channel_id, author_id, body, created_at_ms) "
        "VALUES(?, ?, ?, ?);", -1, &st, NULL);
    sqlite3_bind_int64(st, 1, (sqlite3_int64)j->channel_id);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)j->author_id);
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
        if (j->type == OC_JOB_AUTH)      r = process_auth(w->db, j);
        else if (j->type == OC_JOB_SEND) r = process_send(w->db, j);
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

/* --- Lifecycle ---------------------------------------------------------- */

oc_dbwriter *oc_dbwriter_start(const char *path) {
    oc_dbwriter *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    w->evfd = -1;

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
    if (w->evfd >= 0) close(w->evfd);
    sqlite3_close(w->db);
    free(w);
}
