/* Summaries in the database (sum_store.h). */
#define _POSIX_C_SOURCE 200809L
#include "sum_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "speakable.h"

void oc_sum_window_free(oc_sum_window *w) {
    for (int i = 0; i < w->n; i++) {
        free((char *)w->msgs[i].author);
        free((char *)w->msgs[i].text);
    }
    free(w->msgs);
    memset(w, 0, sizeof *w);
}

static int cmp_msg(const void *a, const void *b) {
    const oc_sum_msg *x = a, *y = b;
    return x->id < y->id ? -1 : x->id > y->id;
}

/* People's messages only: not a deleted one, not a call event, not an
 * integration's post (a webhook signs with its own name). */
#define PEOPLE " deleted_at_ms IS NULL AND kind=0 AND author_name IS NULL "

int oc_sum_load_window(sqlite3 *db, int64_t channel, int64_t start_ms, int64_t end_ms, oc_sum_window *w) {
    memset(w, 0, sizeof *w);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT COALESCE(name,'') FROM channels WHERE id=?1;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, channel);
        if (sqlite3_step(st) == SQLITE_ROW)
            snprintf(w->channel, sizeof w->channel, "%s", (const char *)sqlite3_column_text(st, 0));
    }
    sqlite3_finalize(st);
    if (!w->channel[0]) snprintf(w->channel, sizeof w->channel, "direct message");

    /* Every thread with something in the period; its last activity decides
     * whether it is the period's. */
    int64_t *roots = NULL;
    int nr = 0, cr = 0, rc = -1;
    if (sqlite3_prepare_v2(db,
            "SELECT DISTINCT COALESCE(parent_id, id) FROM messages "
            "WHERE channel_id=?1 AND created_at_ms>=?2 AND created_at_ms<?3 AND" PEOPLE ";",
            -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(st, 1, channel);
    sqlite3_bind_int64(st, 2, start_ms);
    sqlite3_bind_int64(st, 3, end_ms);
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (nr == cr) {
            int nc = cr ? cr * 2 : 64;
            int64_t *nn = realloc(roots, (size_t)nc * sizeof *nn);
            if (!nn) { sqlite3_finalize(st); free(roots); return -1; }
            roots = nn;
            cr = nc;
        }
        roots[nr++] = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);

    sqlite3_stmt *last = NULL, *rows = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT MAX(created_at_ms) FROM messages WHERE (id=?1 OR parent_id=?1) AND" PEOPLE ";",
            -1, &last, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db,
            "SELECT m.id, COALESCE(m.parent_id,0), m.author_id, m.created_at_ms, COALESCE(m.edited_at_ms,0), "
            "m.body, COALESCE(u.display_name,'') FROM messages m LEFT JOIN users u ON u.id=m.author_id "
            "WHERE (m.id=?1 OR m.parent_id=?1) AND m.deleted_at_ms IS NULL AND m.kind=0 AND m.author_name IS NULL "
            "ORDER BY m.id;", -1, &rows, NULL) != SQLITE_OK)
        goto done;
    int cm = 0;
    for (int r = 0; r < nr; r++) {
        sqlite3_reset(last);
        sqlite3_bind_int64(last, 1, roots[r]);
        if (sqlite3_step(last) != SQLITE_ROW || sqlite3_column_int64(last, 0) >= end_ms) continue;
        sqlite3_reset(rows);
        sqlite3_bind_int64(rows, 1, roots[r]);
        while (sqlite3_step(rows) == SQLITE_ROW) {
            if (w->n == cm) {
                int nc = cm ? cm * 2 : 128;
                oc_sum_msg *nn = realloc(w->msgs, (size_t)nc * sizeof *nn);
                if (!nn) goto done;
                w->msgs = nn;
                cm = nc;
            }
            oc_sum_msg *m = &w->msgs[w->n];
            memset(m, 0, sizeof *m);
            m->id = sqlite3_column_int64(rows, 0);
            m->parent_id = sqlite3_column_int64(rows, 1);
            m->author_id = sqlite3_column_int64(rows, 2);
            m->created_ms = sqlite3_column_int64(rows, 3);
            m->edited_ms = sqlite3_column_int64(rows, 4);
            const char *body = (const char *)sqlite3_column_blob(rows, 5);
            size_t blen = (size_t)sqlite3_column_bytes(rows, 5);
            char speak[OC_SPEAK_MAX + 1];
            size_t sl = body ? oc_speakable(body, blen, NULL, NULL, speak, sizeof speak) : 0;
            speak[sl] = '\0';
            m->text = strdup(sl ? speak : "(an attachment)");
            m->author = strdup((const char *)sqlite3_column_text(rows, 6));
            if (!m->text || !m->author) { free((char *)m->text); free((char *)m->author); goto done; }
            w->n++;
        }
    }
    qsort(w->msgs, (size_t)w->n, sizeof *w->msgs, cmp_msg);
    rc = 0;
done:
    sqlite3_finalize(last);
    sqlite3_finalize(rows);
    free(roots);
    if (rc != 0) oc_sum_window_free(w);
    return rc;
}

void oc_sum_user_name(sqlite3 *db, int64_t user_id, char *out, size_t cap) {
    out[0] = '\0';
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT COALESCE(display_name,'') FROM users WHERE id=?1;", -1, &st, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_int64(st, 1, user_id);
    if (sqlite3_step(st) == SQLITE_ROW) snprintf(out, cap, "%s", (const char *)sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
}

static char *col_dup(sqlite3_stmt *st, int i) {
    const char *b = (const char *)sqlite3_column_blob(st, i);
    int n = sqlite3_column_bytes(st, i);
    char *s = malloc((size_t)n + 1);
    if (!s) return NULL;
    if (n) memcpy(s, b, (size_t)n);
    s[n] = '\0';
    return s;
}

int oc_sum_find_ikey(sqlite3 *db, const char *ikey, const char *version, int64_t *id, char **body) {
    sqlite3_stmt *st = NULL;
    int found = 0;
    if (sqlite3_prepare_v2(db, "SELECT id, body FROM summary_nodes WHERE ikey=?1 AND version=?2 LIMIT 1;",
                           -1, &st, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_text(st, 1, ikey, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, version, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) {
        *id = sqlite3_column_int64(st, 0);
        *body = col_dup(st, 1);
        found = *body != NULL;
    }
    sqlite3_finalize(st);
    return found;
}

int oc_sum_find_period(sqlite3 *db, int64_t channel, int64_t start_ms, int64_t end_ms, int tz_offset_min,
                       const char *version, int allow_old, int64_t *id, char **body,
                       char *got_version, size_t vcap) {
    sqlite3_stmt *st = NULL;
    int found = 0;
    if (sqlite3_prepare_v2(db,
            "SELECT id, body, version FROM summary_nodes WHERE channel_id=?1 AND kind=3 AND start_ms=?2 "
            "AND end_ms=?3 AND tz_offset_min=?4 AND (version=?5 OR ?6) "
            "ORDER BY version=?5 DESC, id DESC LIMIT 1;", -1, &st, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_int64(st, 1, channel);
    sqlite3_bind_int64(st, 2, start_ms);
    sqlite3_bind_int64(st, 3, end_ms);
    sqlite3_bind_int(st, 4, tz_offset_min);
    sqlite3_bind_text(st, 5, version, -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 6, allow_old ? 1 : 0);
    if (sqlite3_step(st) == SQLITE_ROW) {
        *id = sqlite3_column_int64(st, 0);
        *body = col_dup(st, 1);
        if (got_version) snprintf(got_version, vcap, "%s", (const char *)sqlite3_column_text(st, 2));
        found = *body != NULL;
    }
    sqlite3_finalize(st);
    return found;
}

void oc_sum_new_free(oc_sum_new *nodes, int n) {
    for (int i = 0; i < n; i++) { free(nodes[i].body); free(nodes[i].in); }
}

int oc_sum_store(sqlite3 *db, int64_t channel, const char *version, oc_sum_new *nodes, int n) {
    if (sqlite3_exec(db, "SAVEPOINT sum_store;", NULL, NULL, NULL) != SQLITE_OK) return -1;
    sqlite3_stmt *msg = NULL, *node = NULL, *ins = NULL, *inp = NULL;
    int rc = -1;
    if (sqlite3_prepare_v2(db,
            "SELECT COALESCE(edited_at_ms,0) FROM messages WHERE id=?1 AND channel_id=?2 AND deleted_at_ms IS NULL;",
            -1, &msg, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db, "SELECT 1 FROM summary_nodes WHERE id=?1;", -1, &node, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db,
            "INSERT INTO summary_nodes(channel_id, kind, ikey, root_id, first_msg_id, last_msg_id, start_ms, "
            "end_ms, tz_offset_min, version, body, tokens_in, cpu_ms, created_at_ms) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13, CAST(strftime('%s','now') AS INTEGER)*1000);",
            -1, &ins, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db,
            "INSERT INTO summary_inputs(parent_id, ord, child_kind, child_id, child_stamp) VALUES(?1,?2,?3,?4,?5);",
            -1, &inp, NULL) != SQLITE_OK)
        goto out;
    for (int i = 0; i < n; i++) {
        oc_sum_new *x = &nodes[i];
        /* The guard: everything it was built from is as it was read. */
        for (int k = 0; k < x->n_in; k++) {
            const oc_sum_input *in = &x->in[k];
            if (in->kind == OC_SUM_IN_MSG) {
                sqlite3_reset(msg);
                sqlite3_bind_int64(msg, 1, in->id);
                sqlite3_bind_int64(msg, 2, channel);
                if (sqlite3_step(msg) != SQLITE_ROW || sqlite3_column_int64(msg, 0) != in->stamp) { rc = 1; goto out; }
            } else if (in->kind == OC_SUM_IN_NODE) {
                sqlite3_reset(node);
                sqlite3_bind_int64(node, 1, in->id);
                if (sqlite3_step(node) != SQLITE_ROW) { rc = 1; goto out; }
            } else if (in->id < 0 || in->id >= i) {
                goto out;
            }
        }
        sqlite3_reset(ins);
        sqlite3_bind_int64(ins, 1, channel);
        sqlite3_bind_int(ins, 2, x->kind);
        sqlite3_bind_text(ins, 3, x->ikey, -1, SQLITE_STATIC);
        if (x->root_id) sqlite3_bind_int64(ins, 4, x->root_id); else sqlite3_bind_null(ins, 4);
        if (x->first_msg_id) sqlite3_bind_int64(ins, 5, x->first_msg_id); else sqlite3_bind_null(ins, 5);
        if (x->last_msg_id) sqlite3_bind_int64(ins, 6, x->last_msg_id); else sqlite3_bind_null(ins, 6);
        sqlite3_bind_int64(ins, 7, x->start_ms);
        sqlite3_bind_int64(ins, 8, x->end_ms);
        sqlite3_bind_int(ins, 9, x->tz_offset_min);
        sqlite3_bind_text(ins, 10, version, -1, SQLITE_STATIC);
        sqlite3_bind_blob(ins, 11, x->body, (int)strlen(x->body), SQLITE_STATIC);
        sqlite3_bind_int64(ins, 12, x->tokens_in);
        sqlite3_bind_int64(ins, 13, x->cpu_ms);
        if (sqlite3_step(ins) != SQLITE_DONE) goto out;
        x->id = sqlite3_last_insert_rowid(db);
        /* Inputs left by a node deleted without the cascade (a database edited
         * by hand) must not collide with the new node's. */
        char clear[96];
        snprintf(clear, sizeof clear, "DELETE FROM summary_inputs WHERE parent_id=%lld;", (long long)x->id);
        if (sqlite3_exec(db, clear, NULL, NULL, NULL) != SQLITE_OK) goto out;
        for (int k = 0; k < x->n_in; k++) {
            const oc_sum_input *in = &x->in[k];
            sqlite3_reset(inp);
            sqlite3_bind_int64(inp, 1, x->id);
            sqlite3_bind_int(inp, 2, k);
            sqlite3_bind_int(inp, 3, in->kind == OC_SUM_IN_MSG ? 0 : 1);
            sqlite3_bind_int64(inp, 4, in->kind == OC_SUM_IN_NEW ? nodes[in->id].id : in->id);
            sqlite3_bind_int64(inp, 5, in->kind == OC_SUM_IN_MSG ? in->stamp : 0);
            if (sqlite3_step(inp) != SQLITE_DONE) goto out;
        }
    }
    rc = 0;
out:
    sqlite3_finalize(msg);
    sqlite3_finalize(node);
    sqlite3_finalize(ins);
    sqlite3_finalize(inp);
    if (rc < 0) fprintf(stderr, "summary: storing failed: %s\n", sqlite3_errmsg(db));
    if (rc == 0) sqlite3_exec(db, "RELEASE sum_store;", NULL, NULL, NULL);
    else { sqlite3_exec(db, "ROLLBACK TO sum_store;", NULL, NULL, NULL); sqlite3_exec(db, "RELEASE sum_store;", NULL, NULL, NULL); }
    return rc;
}

int oc_sum_purge(sqlite3 *db, int64_t channel, int64_t msg_id, int64_t root_id, int64_t created_ms) {
    sqlite3_stmt *st = NULL;
    /* The seeds -- built on the message, on its thread's root (a thread's
     * nodes all hold its root), or a period whose span holds the moment -- and
     * everything built on them, up the tree. */
    if (sqlite3_prepare_v2(db,
            "WITH RECURSIVE gone(id) AS ("
            "  SELECT parent_id FROM summary_inputs WHERE child_kind=0 AND child_id IN (?2, ?3)"
            "  UNION SELECT id FROM summary_nodes WHERE channel_id=?1 AND kind=3 AND start_ms<=?4 AND end_ms>?4"
            "  UNION SELECT si.parent_id FROM summary_inputs si JOIN gone g ON si.child_kind=1 AND si.child_id=g.id"
            ") DELETE FROM summary_nodes WHERE id IN (SELECT id FROM gone);", -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(st, 1, channel);
    sqlite3_bind_int64(st, 2, msg_id);
    sqlite3_bind_int64(st, 3, root_id ? root_id : msg_id);
    sqlite3_bind_int64(st, 4, created_ms);
    int rc = sqlite3_step(st) == SQLITE_DONE ? sqlite3_changes(db) : -1;
    sqlite3_finalize(st);
    return rc;
}

int oc_sum_collect(sqlite3 *db, int64_t now_ms, int64_t max_age_ms) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "DELETE FROM summary_nodes WHERE created_at_ms < ?1 AND "
            "id NOT IN (SELECT child_id FROM summary_inputs WHERE child_kind=1);", -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(st, 1, now_ms - max_age_ms);
    int rc = sqlite3_step(st) == SQLITE_DONE ? sqlite3_changes(db) : -1;
    sqlite3_finalize(st);
    return rc;
}

void oc_sum_ikey(char tag, const oc_sum_input *in, int n, char *out, size_t cap) {
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < n; i++) {
        int64_t v[3] = { in[i].kind, in[i].id, in[i].stamp };
        const unsigned char *p = (const unsigned char *)v;
        for (size_t k = 0; k < sizeof v; k++) { h ^= p[k]; h *= 1099511628211ull; }
    }
    snprintf(out, cap, "%c:%016llx:%d", tag, (unsigned long long)h, n);
}
