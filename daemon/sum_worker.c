/* The summary worker (sum_worker.h). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "sum_worker.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "json.h"

#define DAY_MS 86400000ll

typedef struct req {
    uint64_t    conn_id;
    uint32_t    req_id;
    int64_t     channel, start_ms, end_ms;
    int         tz;
    struct req *next;
} req;

/* One build: the connection, the engine, the gate and the nodes made so far. */
typedef struct {
    const oc_sum_worker_cfg *cfg;
    sqlite3      *db;
    const char   *version;
    void        **engine;         /* the open handle, opened on first need */
    oc_sum_load  *gate;           /* NULL: no gate */
    volatile int *stopping;
    oc_sum_new   *nodes;
    int           n_nodes, cap_nodes;
    int64_t       channel;
    char          channel_name[128];
    char          err[600];
    int           calls;
} build;

/* A child: a stored node, or one made in this build. */
typedef struct {
    int         is_new;
    int64_t     id;                /* stored: its id; new: index into nodes */
    const char *body;              /* borrowed */
    char       *owned;             /* a stored node's body, read */
    int64_t     start_ms, end_ms;
} ref;

struct oc_sum_worker {
    oc_sum_worker_cfg cfg;
    char              version[160];
    sqlite3          *db;
    pthread_t         thread;
    pthread_mutex_t   mu;
    pthread_cond_t    cv;
    req              *head, *tail;
    int               queued;
    volatile int      stopping;
    oc_sum_load       gate;
};

static uint64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static int64_t wall_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void oc_sum_version(const oc_sum_engine *e, size_t threshold, uint64_t gap_ms, char *out, size_t cap) {
    snprintf(out, cap, "%s/%s/%zu/%llu", e && e->version ? e->version : "none", SUM_PROMPT_VERSION, threshold,
             (unsigned long long)(gap_ms / 1000u));
}

const char *oc_sum_worker_version(const oc_sum_worker *w) { return w->version; }

/* --- the gate, as the engine asks it ------------------------------------------ */

static int gate_fn(void *vctx) {
    build *b = vctx;
    if (*b->stopping) return 1;
    if (!b->gate) return 0;
    while (oc_sum_load_busy(b->gate)) {
        if (*b->stopping) return 1;
        /* Memory that stays low: give up this answer and the model with it. */
        if (oc_sum_load_should_unload(b->gate)) return 1;
        struct timespec ts = { 0, 500 * 1000000L };
        nanosleep(&ts, NULL);
    }
    return 0;
}

/* --- nodes ---------------------------------------------------------------------- */

static int add_node(build *b, const oc_sum_new *proto) {
    if (b->n_nodes == b->cap_nodes) {
        int nc = b->cap_nodes ? b->cap_nodes * 2 : 16;
        oc_sum_new *nn = realloc(b->nodes, (size_t)nc * sizeof *nn);
        if (!nn) return -1;
        b->nodes = nn;
        b->cap_nodes = nc;
    }
    b->nodes[b->n_nodes] = *proto;
    return b->n_nodes++;
}

static const char *const EMPTY_BODY =
    "{\"overview\":\"\",\"decisions\":[],\"actions\":[],\"problems\":[],\"facts\":[]}";

static int ensure_engine(build *b) {
    if (*b->engine) return 0;
    const oc_sum_engine *e = b->cfg->engine;
    char err[256] = "";
    *b->engine = e->open(e->ctx, err, sizeof err);
    if (!*b->engine) { snprintf(b->err, sizeof b->err, "the model did not load: %s", err); return -1; }
    return 0;
}

/* Ask the model; the checked, stored-form body into a heap string. An answer
 * that runs past its budget is asked for again with room for one item of each
 * kind, rather than losing the piece. */
static char *ask(build *b, const char *system, const oc_sum_buf *prompt, const oc_sum_ids *ids, int rollup,
                 uint32_t *tokens, uint32_t *cpu) {
    if (ensure_engine(b) != 0) return NULL;
    const oc_sum_engine *e = b->cfg->engine;
    char *raw = NULL, err[256] = "";
    oc_sum_run_stats st;
    int rc = -1;
    uint32_t cpu_all = 0;
    for (int attempt = 0; attempt < 2 && rc != 0; attempt++) {
        oc_sum_buf g = {0};
        if (oc_sum_grammar(ids, rollup, attempt ? 1 : SUM_MAX_ITEMS, &g) != 0) {
            oc_sum_buf_free(&g);
            snprintf(b->err, sizeof b->err, "out of memory");
            return NULL;
        }
        free(raw);
        raw = NULL;
        memset(&st, 0, sizeof st);
        rc = e->run(*b->engine, system, prompt->p, g.p, SUM_MAX_OUT, gate_fn, b, &raw, &st, err, sizeof err);
        oc_sum_buf_free(&g);
        cpu_all += st.cpu_ms;
        b->calls++;
        if (rc != 0 && (*b->stopping || strcmp(err, "stopped") == 0)) break;
    }
    if (rc != 0) { snprintf(b->err, sizeof b->err, "the model gave no answer: %s", err); free(raw); return NULL; }
    oc_sum_buf out = {0};
    int dropped = 0;
    int kept = oc_sum_check(raw, strlen(raw), ids, rollup, &out, &dropped);
    free(raw);
    if (kept < 0) {
        oc_sum_buf_free(&out);
        snprintf(b->err, sizeof b->err, "the model's answer was not a summary");
        return NULL;
    }
    if (tokens) *tokens = st.prompt_tokens;
    if (cpu) *cpu = cpu_all;
    return out.p;
}

static int64_t chunk_start(const oc_sum_msg *m, const oc_sum_chunk *c) {
    int64_t s = INT64_MAX;
    for (int k = 0; k < c->n; k++) if (m[c->idx[k]].created_ms < s) s = m[c->idx[k]].created_ms;
    return s == INT64_MAX ? 0 : s;
}

/* A chunk's summary: found, or made. */
static int build_chunk(build *b, const oc_sum_window *w, const oc_sum_chunk *c, ref *out) {
    memset(out, 0, sizeof *out);
    oc_sum_input *in = malloc((size_t)(c->n ? c->n : 1) * sizeof *in);
    if (!in) return -1;
    for (int k = 0; k < c->n; k++) {
        in[k].kind = OC_SUM_IN_MSG;
        in[k].id = w->msgs[c->idx[k]].id;
        in[k].stamp = w->msgs[c->idx[k]].edited_ms;
    }
    char ikey[72];
    oc_sum_ikey('c', in, c->n, ikey, sizeof ikey);
    out->start_ms = chunk_start(w->msgs, c);
    out->end_ms = c->end_ms;
    char *body = NULL;
    int64_t id;
    if (oc_sum_find_ikey(b->db, ikey, b->version, &id, &body)) {
        free(in);
        out->id = id;
        out->body = out->owned = body;
        return 0;
    }
    for (int i = 0; i < b->n_nodes; i++)
        if (!strcmp(b->nodes[i].ikey, ikey)) {
            free(in);
            out->is_new = 1; out->id = i; out->body = b->nodes[i].body;
            return 0;
        }
    oc_sum_buf prompt = {0};
    oc_sum_ids ids;
    uint32_t tok = 0, cpu = 0;
    if (oc_sum_render_chunk(w->channel, w->msgs, c, &prompt, &ids) != 0) {
        oc_sum_buf_free(&prompt); oc_sum_ids_free(&ids); free(in);
        snprintf(b->err, sizeof b->err, "out of memory");
        return -1;
    }
    body = ask(b, OC_SUM_SYSTEM_LEAF, &prompt, &ids, 0, &tok, &cpu);
    oc_sum_buf_free(&prompt);
    oc_sum_ids_free(&ids);
    if (!body) { free(in); return -1; }
    oc_sum_new n;
    memset(&n, 0, sizeof n);
    n.kind = OC_SUM_KIND_CHUNK;
    snprintf(n.ikey, sizeof n.ikey, "%s", ikey);
    n.root_id = c->root_id;
    n.first_msg_id = w->msgs[c->idx[0]].id;
    n.last_msg_id = w->msgs[c->idx[c->n - 1]].id;
    n.start_ms = out->start_ms;
    n.end_ms = out->end_ms;
    n.body = body;
    n.tokens_in = tok;
    n.cpu_ms = cpu;
    n.in = in;
    n.n_in = c->n;
    int idx = add_node(b, &n);
    if (idx < 0) { free(body); free(in); return -1; }
    out->is_new = 1;
    out->id = idx;
    out->body = b->nodes[idx].body;
    return 0;
}

static const char *name_of(void *ctx, int64_t uid) {
    static __thread char nm[128];
    oc_sum_user_name(((build *)ctx)->db, uid, nm, sizeof nm);
    return nm;
}

static void ref_free(ref *r, int n) { for (int i = 0; i < n; i++) free(r[i].owned); }

/* Roll children up into one node of `kind`, cutting a long list into sections
 * first. `proto` carries the node's own fields (period, thread). */
static int rollup(build *b, ref *kids, int n, int kind, const oc_sum_new *proto, ref *out) {
    memset(out, 0, sizeof *out);
    /* Too long for one prompt: sections, each rolled up, then those. */
    oc_sum_child *ch = malloc((size_t)(n ? n : 1) * sizeof *ch);
    if (!ch) return -1;
    for (int i = 0; i < n; i++) { ch[i].body = kids[i].body; ch[i].start_ms = kids[i].start_ms; ch[i].end_ms = kids[i].end_ms; }
    if (n > 1 && oc_sum_rollup_tokens(ch, n) > b->cfg->threshold) {
        ref *sec = malloc((size_t)n * sizeof *sec);
        int ns = 0, s = 0, rc = -1;
        if (!sec) { free(ch); return -1; }
        while (s < n) {
            int e = s + 1;
            while (e < n && oc_sum_rollup_tokens(ch + s, e - s + 1) <= b->cfg->threshold) e++;
            if (e - s == 1 && e < n) e++;     /* a section of one adds nothing */
            oc_sum_new sp;
            memset(&sp, 0, sizeof sp);
            sp.start_ms = kids[s].start_ms;
            sp.end_ms = kids[e - 1].end_ms;
            sp.tz_offset_min = proto->tz_offset_min;
            if (rollup(b, kids + s, e - s, OC_SUM_KIND_SECTION, &sp, &sec[ns]) != 0) goto sec_out;
            ns++;
            s = e;
        }
        rc = rollup(b, sec, ns, kind, proto, out);
    sec_out:
        ref_free(sec, ns);
        free(sec);
        free(ch);
        return rc;
    }

    oc_sum_input *in = malloc((size_t)(n ? n : 1) * sizeof *in);
    if (!in) { free(ch); return -1; }
    for (int i = 0; i < n; i++) { in[i].kind = kids[i].is_new ? OC_SUM_IN_NEW : OC_SUM_IN_NODE; in[i].id = kids[i].id; in[i].stamp = 0; }
    char ikey[72];
    oc_sum_ikey(kind == OC_SUM_KIND_THREAD ? 't' : kind == OC_SUM_KIND_SECTION ? 's' : 'p', in, n, ikey, sizeof ikey);
    /* An existing node over exactly these children (a stored child list only). */
    int all_stored = 1;
    for (int i = 0; i < n; i++) if (kids[i].is_new) all_stored = 0;
    if (all_stored && kind != OC_SUM_KIND_PERIOD) {
        char *body = NULL;
        int64_t id;
        if (oc_sum_find_ikey(b->db, ikey, b->version, &id, &body)) {
            free(in); free(ch);
            out->id = id; out->body = out->owned = body;
            out->start_ms = proto->start_ms; out->end_ms = proto->end_ms;
            return 0;
        }
    }
    char *body = NULL;
    uint32_t tok = 0, cpu = 0;
    if (n == 0) {
        body = strdup(EMPTY_BODY);
    } else if (n == 1) {
        body = strdup(kids[0].body);       /* one child is its own summary */
    } else {
        oc_sum_buf prompt = {0};
        oc_sum_ids ids;
        if (oc_sum_render_rollup(b->channel_name, ch, n, name_of, b, &prompt, &ids) != 0) {
            oc_sum_buf_free(&prompt); oc_sum_ids_free(&ids); free(in); free(ch);
            return -1;
        }
        body = ask(b, OC_SUM_SYSTEM_ROLLUP, &prompt, &ids, 1, &tok, &cpu);
        oc_sum_buf_free(&prompt);
        oc_sum_ids_free(&ids);
    }
    free(ch);
    if (!body) { free(in); return -1; }
    oc_sum_new x = *proto;
    x.kind = kind;
    snprintf(x.ikey, sizeof x.ikey, "%s", ikey);
    x.body = body;
    x.tokens_in = tok;
    x.cpu_ms = cpu;
    x.in = in;
    x.n_in = n;
    int idx = add_node(b, &x);
    if (idx < 0) { free(body); free(in); return -1; }
    out->is_new = 1;
    out->id = idx;
    out->body = b->nodes[idx].body;
    out->start_ms = x.start_ms;
    out->end_ms = x.end_ms;
    return 0;
}

int64_t oc_sum_day_start(int64_t t, int tz) {
    int64_t off = (int64_t)tz * 60000;
    int64_t l = t + off;
    int64_t d = l >= 0 ? l / DAY_MS : (l - DAY_MS + 1) / DAY_MS;
    return d * DAY_MS - off;
}

/* The period [start, end): found, or made from its days (a long one) or from
 * its pieces (a day or less). */
static int build_period(build *b, int64_t start, int64_t end, int tz, ref *out) {
    memset(out, 0, sizeof *out);
    char *body = NULL;
    int64_t id;
    if (oc_sum_find_period(b->db, b->channel, start, end, tz, b->version, 0, &id, &body, NULL, 0)) {
        out->id = id; out->body = out->owned = body; out->start_ms = start; out->end_ms = end;
        return 0;
    }
    ref *kids = NULL;
    int nk = 0, ck = 0, rc = -1;
    if (end - start > DAY_MS + DAY_MS / 2) {
        for (int64_t d = oc_sum_day_start(start, tz); d < end; d += DAY_MS) {
            int64_t s = d < start ? start : d, e = d + DAY_MS > end ? end : d + DAY_MS;
            if (nk == ck) {
                int nc = ck ? ck * 2 : 8;
                ref *nn = realloc(kids, (size_t)nc * sizeof *nn);
                if (!nn) goto out;
                kids = nn; ck = nc;
            }
            if (build_period(b, s, e, tz, &kids[nk]) != 0) goto out;
            nk++;
        }
    } else {
        oc_sum_window w;
        if (oc_sum_load_window(b->db, b->channel, start, end, &w) != 0) { snprintf(b->err, sizeof b->err, "reading the channel failed"); goto out; }
        snprintf(b->channel_name, sizeof b->channel_name, "%s", w.channel);
        oc_sum_cut cut;
        if (oc_sum_cut_build(w.msgs, w.n, b->cfg->threshold, b->cfg->gap_ms, &cut) != 0) { oc_sum_window_free(&w); goto out; }
        kids = calloc((size_t)(cut.n ? cut.n : 1), sizeof *kids);
        if (!kids) { oc_sum_cut_free(&cut); oc_sum_window_free(&w); goto out; }
        for (int p = 0; p < cut.n; p++) {
            const oc_sum_piece *pc = &cut.pieces[p];
            if (!pc->is_big_thread) {
                if (build_chunk(b, &w, &pc->chunks[0], &kids[nk]) != 0) { oc_sum_cut_free(&cut); oc_sum_window_free(&w); goto out; }
                nk++;
                continue;
            }
            ref *tc = calloc((size_t)pc->n_chunks, sizeof *tc);
            int nt = 0, ok = tc != NULL;
            for (int k = 0; ok && k < pc->n_chunks; k++) {
                if (build_chunk(b, &w, &pc->chunks[k], &tc[nt]) != 0) ok = 0; else nt++;
            }
            if (ok) {
                oc_sum_new tp;
                memset(&tp, 0, sizeof tp);
                tp.root_id = pc->root_id;
                tp.start_ms = tc[0].start_ms;
                tp.end_ms = pc->end_ms;
                ok = rollup(b, tc, nt, OC_SUM_KIND_THREAD, &tp, &kids[nk]) == 0;
                if (ok) nk++;
            }
            if (tc) ref_free(tc, nt);
            free(tc);
            if (!ok) { oc_sum_cut_free(&cut); oc_sum_window_free(&w); goto out; }
        }
        oc_sum_cut_free(&cut);
        oc_sum_window_free(&w);
    }
    /* Days with nothing to say add nothing to a rollup. */
    for (int i = 0; i < nk; ) {
        if (nk > 0 && oc_sum_body_empty(kids[i].body)) {
            free(kids[i].owned);
            memmove(&kids[i], &kids[i + 1], (size_t)(nk - i - 1) * sizeof *kids);
            nk--;
        } else {
            i++;
        }
    }
    oc_sum_new pp;
    memset(&pp, 0, sizeof pp);
    pp.start_ms = start;
    pp.end_ms = end;
    pp.tz_offset_min = tz;
    rc = rollup(b, kids, nk, OC_SUM_KIND_PERIOD, &pp, out);
    /* A period node is always its own row, even over one child, so the next
     * request for the period finds it. */
    if (rc == 0 && !out->is_new) {
        oc_sum_input *in = malloc(sizeof *in);
        char *bd = strdup(out->body);
        if (!in || !bd) { free(in); free(bd); rc = -1; goto out; }
        in[0].kind = OC_SUM_IN_NODE; in[0].id = out->id; in[0].stamp = 0;
        pp.kind = OC_SUM_KIND_PERIOD;
        oc_sum_ikey('p', in, 1, pp.ikey, sizeof pp.ikey);
        pp.body = bd; pp.in = in; pp.n_in = 1;
        int idx = add_node(b, &pp);
        if (idx < 0) { free(in); free(bd); rc = -1; goto out; }
        free(out->owned);
        out->owned = NULL;
        out->is_new = 1; out->id = idx; out->body = b->nodes[idx].body;
    }
out:
    ref_free(kids, nk);
    free(kids);
    return rc;
}

char *oc_sum_client_body(sqlite3 *db, const char *body) {
    oc_json d;
    if (!body || oc_json_parse(&d, body, strlen(body)) != 0) return NULL;
    int64_t seen[64];
    int ns = 0;
    oc_sum_buf out = {0};
    oc_sum_buf_puts(&out, "{\"summary\":");
    oc_sum_buf_puts(&out, body);
    oc_sum_buf_puts(&out, ",\"people\":{");
    const char *lists[2] = { "decisions", "actions" };
    for (int l = 0; l < 2; l++) {
        int arr = oc_json_get(&d, 0, lists[l]);
        if (arr < 0 || d.t[arr].type != JSMN_ARRAY) continue;
        int i = arr + 1;
        for (int k = 0; k < d.t[arr].size; k++, i = oc_json_skip(&d, i)) {
            int64_t ids[5];
            int nid = 0;
            uint64_t v;
            if (l == 1) {
                if (oc_json_u64(&d, oc_json_get(&d, i, "who"), &v) == 0 && v) ids[nid++] = (int64_t)v;
            } else {
                int by = oc_json_get(&d, i, "by");
                if (by >= 0 && d.t[by].type == JSMN_ARRAY) {
                    int j = by + 1;
                    for (int x = 0; x < d.t[by].size && nid < 4; x++, j = oc_json_skip(&d, j))
                        if (oc_json_u64(&d, j, &v) == 0) ids[nid++] = (int64_t)v;
                }
            }
            for (int x = 0; x < nid; x++) {
                int dup = 0;
                for (int y = 0; y < ns; y++) if (seen[y] == ids[x]) dup = 1;
                if (dup || ns == 64) continue;
                seen[ns++] = ids[x];
                char nm[128];
                oc_sum_user_name(db, ids[x], nm, sizeof nm);
                oc_sum_buf_printf(&out, "%s\"%lld\":", ns > 1 ? "," : "", (long long)ids[x]);
                oc_sum_buf_json(&out, nm);
            }
        }
    }
    oc_sum_buf_puts(&out, "}}");
    oc_json_free(&d);
    if (out.oom) { oc_sum_buf_free(&out); return NULL; }
    return out.p;
}

/* Build one period and hand it to the sink; retried once if a message changed
 * while it was being built. */
static int run_build(const oc_sum_worker_cfg *cfg, sqlite3 *db, const char *version, void **engine,
                     oc_sum_load *gate, volatile int *stopping, const req *r, char *err, size_t errcap) {
    for (int attempt = 0; attempt < 2; attempt++) {
        build b;
        memset(&b, 0, sizeof b);
        b.cfg = cfg; b.db = db; b.version = version; b.engine = engine; b.gate = gate; b.stopping = stopping;
        b.channel = r->channel;
        ref top;
        oc_sum_answer a;
        memset(&a, 0, sizeof a);
        a.conn_id = r->conn_id; a.req_id = r->req_id; a.channel = r->channel;
        a.start_ms = r->start_ms; a.end_ms = r->end_ms; a.tz_offset_min = r->tz; a.version = version;
        int rc = build_period(&b, r->start_ms, r->end_ms, r->tz, &top);
        if (rc != 0) {
            snprintf(err, errcap, "%s", b.err[0] ? b.err : "the summary could not be made");
            a.ok = 0; a.err = err;
            cfg->sink.store(cfg->sink.ctx, &a, NULL, 0);
            oc_sum_new_free(b.nodes, b.n_nodes);
            free(b.nodes);
            return -1;
        }
        char *client = oc_sum_client_body(db, top.body);
        a.ok = client != NULL;
        a.body = client;
        a.err = client ? NULL : "out of memory";
        if (!top.is_new) a.summary_id = top.id;
        int st = cfg->sink.store(cfg->sink.ctx, &a, b.nodes, top.is_new ? b.n_nodes : 0);
        free(client);
        free(top.owned);
        oc_sum_new_free(b.nodes, b.n_nodes);
        free(b.nodes);
        if (st == 0) return 0;
        if (st < 0) { snprintf(err, errcap, "storing the summary failed"); return -1; }
    }
    snprintf(err, errcap, "the channel kept changing");
    return -1;
}

static int open_db(const char *path, sqlite3 **db, char *err, size_t errcap) {
    if (sqlite3_open_v2(path, db, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, NULL) != SQLITE_OK) {
        snprintf(err, errcap, "cannot open %s", path);
        sqlite3_close(*db);
        *db = NULL;
        return -1;
    }
    sqlite3_busy_timeout(*db, 5000);
    sqlite3_exec(*db, "PRAGMA query_only=1;", NULL, NULL, NULL);
    return 0;
}

int oc_sum_build_now(const oc_sum_worker_cfg *cfg, int64_t channel, int64_t start_ms, int64_t end_ms,
                     int tz_offset_min, char *err, size_t errcap) {
    sqlite3 *db = NULL;
    if (open_db(cfg->db_path, &db, err, errcap) != 0) return -1;
    char version[160];
    oc_sum_version(cfg->engine, cfg->threshold, cfg->gap_ms, version, sizeof version);
    void *engine = NULL;
    volatile int stopping = 0;
    req r = { 0, 0, channel, start_ms, end_ms, tz_offset_min, NULL };
    int rc = run_build(cfg, db, version, &engine, NULL, &stopping, &r, err, errcap);
    if (engine) cfg->engine->close(engine);
    sqlite3_close(db);
    return rc;
}

/* --- background work ------------------------------------------------------------ */

typedef struct { int64_t channel; int tz; } chan_tz;

/* Recent days of active channels, in their members' time zones, that have no
 * summary yet (or one an older version made). Up to `cap` into `out`. */
static int idle_candidates(sqlite3 *db, const char *version, req *out, int cap) {
    sqlite3_stmt *st = NULL;
    int64_t now = wall_ms();
    chan_tz ct[256];
    int nct = 0;
    if (sqlite3_prepare_v2(db,
            "SELECT DISTINCT m.channel_id, COALESCE(u.tz_offset_min,0) FROM messages m "
            "JOIN channel_members cm ON cm.channel_id=m.channel_id JOIN users u ON u.id=cm.user_id "
            "WHERE m.created_at_ms>=?1 AND m.deleted_at_ms IS NULL AND m.kind=0 AND m.author_name IS NULL "
            "LIMIT 256;", -1, &st, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_int64(st, 1, now - (int64_t)SUM_IDLE_DAYS * DAY_MS);
    while (sqlite3_step(st) == SQLITE_ROW && nct < 256) {
        ct[nct].channel = sqlite3_column_int64(st, 0);
        ct[nct].tz = sqlite3_column_int(st, 1);
        nct++;
    }
    sqlite3_finalize(st);
    sqlite3_stmt *any = NULL;
    sqlite3_prepare_v2(db,
        "SELECT 1 FROM messages WHERE channel_id=?1 AND created_at_ms>=?2 AND created_at_ms<?3 "
        "AND deleted_at_ms IS NULL AND kind=0 AND author_name IS NULL LIMIT 1;", -1, &any, NULL);
    int n = 0;
    for (int i = 0; i < nct && n < cap; i++) {
        int64_t today = oc_sum_day_start(now, ct[i].tz);
        /* Yesterday back: today is still changing. */
        for (int d = 1; d <= SUM_IDLE_DAYS && n < cap; d++) {
            int64_t s = today - (int64_t)d * DAY_MS, e = s + DAY_MS;
            int64_t id;
            char *body = NULL;
            if (oc_sum_find_period(db, ct[i].channel, s, e, ct[i].tz, version, 0, &id, &body, NULL, 0)) {
                free(body);
                continue;
            }
            sqlite3_reset(any);
            sqlite3_bind_int64(any, 1, ct[i].channel);
            sqlite3_bind_int64(any, 2, s);
            sqlite3_bind_int64(any, 3, e);
            if (!any || sqlite3_step(any) != SQLITE_ROW) continue;
            req r = { 0, 0, ct[i].channel, s, e, ct[i].tz, NULL };
            out[n++] = r;
        }
    }
    sqlite3_finalize(any);
    return n;
}

static void lower_priority(void) {
    struct sched_param sp;
    memset(&sp, 0, sizeof sp);
    /* The calling thread only: Linux applies these per thread. */
    sched_setscheduler(0, SCHED_IDLE, &sp);
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 19);
}

static void *run(void *arg) {
    oc_sum_worker *w = arg;
    lower_priority();
    void *engine = NULL;
    uint64_t last_used = 0, last_scan = 0;
    req idle[32];
    int n_idle = 0, at_idle = 0;
    char err[256];
    for (;;) {
        pthread_mutex_lock(&w->mu);
        if (!w->head && !w->stopping) {
            struct timespec until;
            clock_gettime(CLOCK_MONOTONIC, &until);
            until.tv_sec += 2;
            pthread_cond_timedwait(&w->cv, &w->mu, &until);
        }
        if (w->stopping) { pthread_mutex_unlock(&w->mu); break; }
        req *r = w->head;
        if (r) {
            w->head = r->next;
            if (!w->head) w->tail = NULL;
            w->queued--;
        }
        pthread_mutex_unlock(&w->mu);

        int busy = oc_sum_load_busy(&w->gate);
        if (engine && (oc_sum_load_should_unload(&w->gate) ||
                       (!r && mono_ms() - last_used > SUM_UNLOAD_IDLE_MS))) {
            w->cfg.engine->close(engine);
            engine = NULL;
            fprintf(stderr, "summary: model unloaded\n");
        }
        if (r) {
            /* Someone is waiting: build now (the gate still pauses it). */
            if (run_build(&w->cfg, w->db, w->version, &engine, &w->gate, &w->stopping, r, err, sizeof err) != 0)
                fprintf(stderr, "summary: channel %lld: %s\n", (long long)r->channel, err);
            last_used = mono_ms();
            free(r);
            continue;
        }
        if (!w->cfg.background || busy || !oc_sum_load_settled(&w->gate)) continue;
        if (at_idle == n_idle) {
            if (last_scan && mono_ms() - last_scan < SUM_IDLE_SCAN_MS) continue;
            last_scan = mono_ms();
            if (w->cfg.sink.collect) w->cfg.sink.collect(w->cfg.sink.ctx);
            n_idle = idle_candidates(w->db, w->version, idle, 32);
            at_idle = 0;
            if (!n_idle) continue;
        }
        req *q = &idle[at_idle++];
        if (run_build(&w->cfg, w->db, w->version, &engine, &w->gate, &w->stopping, q, err, sizeof err) != 0)
            fprintf(stderr, "summary: channel %lld, background: %s\n", (long long)q->channel, err);
        last_used = mono_ms();
    }
    if (engine) w->cfg.engine->close(engine);
    return NULL;
}

oc_sum_worker *oc_sum_worker_start(const oc_sum_worker_cfg *cfg, char *err, size_t errcap) {
    oc_sum_worker *w = calloc(1, sizeof *w);
    if (!w) { snprintf(err, errcap, "out of memory"); return NULL; }
    w->cfg = *cfg;
    if (!w->cfg.threshold) w->cfg.threshold = SUM_THRESHOLD_TOKENS;
    if (!w->cfg.gap_ms) w->cfg.gap_ms = SUM_GAP_MS;
    oc_sum_version(cfg->engine, w->cfg.threshold, w->cfg.gap_ms, w->version, sizeof w->version);
    oc_sum_load_init(&w->gate, cfg->probe ? cfg->probe : oc_sum_probe_system());
    if (open_db(cfg->db_path, &w->db, err, errcap) != 0) { free(w); return NULL; }
    pthread_mutex_init(&w->mu, NULL);
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&w->cv, &ca);
    pthread_condattr_destroy(&ca);
    if (pthread_create(&w->thread, NULL, run, w) != 0) {
        snprintf(err, errcap, "cannot start the summary thread");
        sqlite3_close(w->db);
        free(w);
        return NULL;
    }
    return w;
}

void oc_sum_worker_stop(oc_sum_worker *w) {
    if (!w) return;
    pthread_mutex_lock(&w->mu);
    w->stopping = 1;
    pthread_cond_broadcast(&w->cv);
    pthread_mutex_unlock(&w->mu);
    pthread_join(w->thread, NULL);
    for (req *r = w->head; r; ) {
        req *nx = r->next;
        oc_sum_answer a;
        memset(&a, 0, sizeof a);
        a.conn_id = r->conn_id; a.req_id = r->req_id; a.channel = r->channel;
        a.err = "the daemon is stopping";
        if (r->conn_id) w->cfg.sink.store(w->cfg.sink.ctx, &a, NULL, 0);
        free(r);
        r = nx;
    }
    sqlite3_close(w->db);
    pthread_mutex_destroy(&w->mu);
    pthread_cond_destroy(&w->cv);
    free(w);
}

int oc_sum_worker_request(oc_sum_worker *w, uint64_t conn_id, uint32_t req_id, int64_t channel,
                          int64_t start_ms, int64_t end_ms, int tz_offset_min) {
    req *r = calloc(1, sizeof *r);
    if (!r) return -1;
    r->conn_id = conn_id; r->req_id = req_id; r->channel = channel;
    r->start_ms = start_ms; r->end_ms = end_ms; r->tz = tz_offset_min;
    pthread_mutex_lock(&w->mu);
    if (w->queued >= SUM_QUEUE_MAX || w->stopping) { pthread_mutex_unlock(&w->mu); free(r); return -1; }
    if (w->tail) w->tail->next = r; else w->head = r;
    w->tail = r;
    w->queued++;
    pthread_cond_signal(&w->cv);
    pthread_mutex_unlock(&w->mu);
    return 0;
}
