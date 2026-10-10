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

/* The messages summarized: people's and integrations' posts, not a deleted
 * one or a call event. */
#define SUMMARIZED " deleted_at_ms IS NULL AND kind=0 "
/* Who wrote a message (m, joined to its author u): an integration's post signs
 * with its own name, anyone else's with theirs. */
#define AUTHOR_NAME " COALESCE(NULLIF(m.author_name,''), u.display_name, '') "

/* A message's plain text, whole, on the heap ("(an attachment)" when it has
 * none); NULL when out of memory. */
static char *plain_text(sqlite3_stmt *st, int col) {
    const char *body = (const char *)sqlite3_column_blob(st, col);
    size_t blen = (size_t)sqlite3_column_bytes(st, col);
    char *speak = malloc(2 * blen + 64);
    if (!speak) return NULL;
    size_t sl = body ? oc_speakable_full(body, blen, NULL, NULL, speak, 2 * blen + 64) : 0;
    speak[sl] = '\0';
    if (sl) return speak;
    free(speak);
    return strdup("(an attachment)");
}

static void (*g_changed)(int64_t channel, int64_t at_ms);

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
            "WHERE channel_id=?1 AND created_at_ms>=?2 AND created_at_ms<?3 AND" SUMMARIZED ";",
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
            "SELECT MAX(created_at_ms) FROM messages WHERE (id=?1 OR parent_id=?1) AND" SUMMARIZED ";",
            -1, &last, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db,
            "SELECT m.id, COALESCE(m.parent_id,0), m.author_id, m.created_at_ms, COALESCE(m.edited_at_ms,0), "
            "m.body," AUTHOR_NAME "FROM messages m LEFT JOIN users u ON u.id=m.author_id "
            "WHERE (m.id=?1 OR m.parent_id=?1) AND m.deleted_at_ms IS NULL AND m.kind=0 "
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
            /* The whole message: a summary must not lose any of it. */
            m->text = plain_text(rows, 5);
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

int oc_sum_span_people(sqlite3 *db, int64_t channel, int64_t start_ms, int64_t end_ms, oc_sum_buf *out) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT CASE WHEN COALESCE(m.author_name,'')<>'' THEN 0 ELSE m.author_id END AS uid,"
            AUTHOR_NAME "AS nm, COUNT(*) AS c FROM messages m LEFT JOIN users u ON u.id=m.author_id "
            "WHERE m.channel_id=?1 AND m.created_at_ms>=?2 AND m.created_at_ms<?3 AND m.deleted_at_ms IS NULL "
            "AND m.kind=0 GROUP BY uid, nm ORDER BY c DESC, nm;", -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(st, 1, channel);
    sqlite3_bind_int64(st, 2, start_ms);
    sqlite3_bind_int64(st, 3, end_ms);
    int64_t count = 0;
    int n = 0;
    oc_sum_buf_puts(out, "\"posters\":[");
    while (sqlite3_step(st) == SQLITE_ROW) {
        oc_sum_buf_printf(out, "%s{\"id\":%lld,\"name\":", n++ ? "," : "", (long long)sqlite3_column_int64(st, 0));
        oc_sum_buf_json(out, (const char *)sqlite3_column_text(st, 1));
        oc_sum_buf_puts(out, "}");
        count += sqlite3_column_int64(st, 2);
    }
    sqlite3_finalize(st);
    oc_sum_buf_printf(out, "],\"count\":%lld", (long long)count);
    return out->oom ? -1 : 0;
}

int oc_sum_sources(sqlite3 *db, const int64_t *ids, int n, oc_sum_buf *out) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT" AUTHOR_NAME ", CASE WHEN COALESCE(m.author_name,'')<>'' THEN 0 ELSE m.author_id END,"
            " m.created_at_ms, COALESCE(m.parent_id,0), m.body FROM messages m LEFT JOIN users u ON u.id=m.author_id "
            "WHERE m.id=?1 AND m.deleted_at_ms IS NULL;", -1, &st, NULL) != SQLITE_OK)
        return -1;
    int k = 0, rc = 0;
    oc_sum_buf_puts(out, "\"sources\":{");
    for (int i = 0; i < n && rc == 0; i++) {
        sqlite3_reset(st);
        sqlite3_bind_int64(st, 1, ids[i]);
        if (sqlite3_step(st) != SQLITE_ROW) continue;
        char *text = plain_text(st, 4);
        if (!text) { rc = -1; break; }
        oc_sum_buf_printf(out, "%s\"%lld\":{\"author\":", k++ ? "," : "", (long long)ids[i]);
        oc_sum_buf_json(out, (const char *)sqlite3_column_text(st, 0));
        oc_sum_buf_printf(out, ",\"author_id\":%lld,\"at\":%lld,\"parent\":%lld,\"text\":",
                          (long long)sqlite3_column_int64(st, 1), (long long)sqlite3_column_int64(st, 2),
                          (long long)sqlite3_column_int64(st, 3));
        oc_sum_buf_json(out, text);
        oc_sum_buf_puts(out, "}");
        free(text);
    }
    sqlite3_finalize(st);
    oc_sum_buf_puts(out, "}");
    return rc == 0 && !out->oom ? 0 : -1;
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
    void (*hook)(int64_t, int64_t) = __atomic_load_n(&g_changed, __ATOMIC_ACQUIRE);
    if (hook) hook(channel, created_ms);
    return rc;
}

void oc_sum_on_change(void (*fn)(int64_t channel, int64_t at_ms)) {
    __atomic_store_n(&g_changed, fn, __ATOMIC_RELEASE);
}

int64_t oc_sum_anchor(sqlite3 *db, int64_t channel, int64_t start_ms, uint64_t gap_ms) {
    sqlite3_stmt *st = NULL;
    int64_t at = start_ms;
    if (sqlite3_prepare_v2(db,
            "SELECT created_at_ms FROM messages WHERE channel_id=?1 AND created_at_ms<?2 AND" SUMMARIZED
            "ORDER BY created_at_ms DESC;", -1, &st, NULL) != SQLITE_OK)
        return start_ms;
    sqlite3_bind_int64(st, 1, channel);
    sqlite3_bind_int64(st, 2, start_ms);
    /* Back from `start` to the first quiet gap: every cut that starts at or
     * before it agrees from it on. */
    int64_t later = start_ms;
    while (sqlite3_step(st) == SQLITE_ROW) {
        int64_t t = sqlite3_column_int64(st, 0);
        if ((uint64_t)(later - t) > gap_ms) break;
        at = t;
        later = t;
    }
    sqlite3_finalize(st);
    return at;
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
