/* The summary worker (sum_worker.h). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "sum_worker.h"

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
    char          where[160];     /* "#name", or "a direct message" */
    int           dated;          /* label children with their dates, in `tz` */
    int           tz;
    char          err[600];
    int           calls;
} build;

/* A summary: a stored node, or one made in this build. */
typedef struct {
    int         is_new;
    int64_t     id;                /* stored: its id; new: index into nodes */
    char       *body;              /* heap, owned */
    int64_t     start_ms, end_ms;
    char        ikey[72];
} ref;

typedef struct { int64_t channel, at; } change;
typedef struct { int64_t channel, start, end; int tz; } period_key;
typedef struct { int64_t channel, cursor, first; } backfill;

struct oc_sum_worker {
    oc_sum_worker_cfg cfg;
    char              version[160];
    sqlite3          *db;
    pthread_t         thread;
    pthread_mutex_t   mu;
    pthread_cond_t    cv;
    req              *head, *tail;
    int               queued;
    change           *changes;        /* under mu */
    int               n_changes, cap_changes;
    volatile int      stopping;
    oc_sum_load       gate;
    /* The worker thread's own. */
    period_key       *asked;          /* periods someone asked for */
    int               n_asked, cap_asked;
    backfill         *fill;           /* each channel's chunks, newest back */
    int               n_fill, filled;
};

static oc_sum_worker *g_worker;

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

int64_t oc_sum_day_start(int64_t t, int tz) {
    int64_t off = (int64_t)tz * 60000;
    int64_t l = t + off;
    int64_t d = l >= 0 ? l / DAY_MS : (l - DAY_MS + 1) / DAY_MS;
    return d * DAY_MS - off;
}

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

/* --- keys ----------------------------------------------------------------------- */

#define FNV_START 1469598103934665603ull
static uint64_t fnv(uint64_t h, const void *p, size_t n) {
    const unsigned char *c = p;
    for (size_t k = 0; k < n; k++) { h ^= c[k]; h *= 1099511628211ull; }
    return h;
}

/* A node's key from its children's keys, so the same children always give the
 * same key whether they were stored before or made in this build. A dated
 * node's key holds its zone too: its input carried that zone's dates. */
static void key_of(char tag, const ref *kids, int n, int dated, int tz, char *out, size_t cap) {
    uint64_t h = FNV_START;
    h = fnv(h, &tag, 1);
    for (int i = 0; i < n; i++) h = fnv(h, kids[i].ikey, strlen(kids[i].ikey) + 1);
    if (dated) h = fnv(h, &tz, sizeof tz);
    snprintf(out, cap, "%c:%016llx:%d", tag, (unsigned long long)h, n);
}

/* The key of part `k` of `n` of a message as read. */
static void part_key(int64_t msg, int64_t stamp, int k, int n, char *out, size_t cap) {
    int64_t v[4] = { msg, stamp, k, n };
    uint64_t h = fnv(FNV_START, v, sizeof v);
    snprintf(out, cap, "q:%016llx:%d", (unsigned long long)h, k);
}

static char kind_tag(int kind) {
    return kind == OC_SUM_KIND_THREAD ? 't' : kind == OC_SUM_KIND_SECTION ? 's' : kind == OC_SUM_KIND_PERIOD ? 'p' : 'c';
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

static void ref_free(ref *r, int n) { for (int i = 0; i < n; i++) free(r[i].body); }

/* A node with this key: stored, or made earlier in this build. 1 if found. */
static int find(build *b, const char *ikey, ref *out) {
    char *body = NULL;
    int64_t id;
    if (oc_sum_find_ikey(b->db, ikey, b->version, &id, &body)) {
        out->is_new = 0;
        out->id = id;
        out->body = body;
        snprintf(out->ikey, sizeof out->ikey, "%s", ikey);
        return 1;
    }
    for (int i = 0; i < b->n_nodes; i++)
        if (!strcmp(b->nodes[i].ikey, ikey)) {
            out->body = strdup(b->nodes[i].body);
            if (!out->body) return 0;
            out->is_new = 1;
            out->id = i;
            snprintf(out->ikey, sizeof out->ikey, "%s", ikey);
            return 1;
        }
    return 0;
}

/* Keep a node made in this build; `out` refers to it. Takes `body` and `in`. */
static int keep(build *b, oc_sum_new *x, char *body, oc_sum_input *in, int n_in, ref *out) {
    x->body = body;
    x->in = in;
    x->n_in = n_in;
    int idx = add_node(b, x);
    if (idx < 0) { free(body); free(in); snprintf(b->err, sizeof b->err, "out of memory"); return -1; }
    out->is_new = 1;
    out->id = idx;
    out->body = strdup(body);
    out->start_ms = x->start_ms;
    out->end_ms = x->end_ms;
    snprintf(out->ikey, sizeof out->ikey, "%s", x->ikey);
    return out->body ? 0 : -1;
}

static oc_sum_input *inputs_of(const ref *kids, int n) {
    oc_sum_input *in = malloc((size_t)(n ? n : 1) * sizeof *in);
    if (!in) return NULL;
    for (int i = 0; i < n; i++) {
        in[i].kind = kids[i].is_new ? OC_SUM_IN_NEW : OC_SUM_IN_NODE;
        in[i].id = kids[i].id;
        in[i].stamp = 0;
    }
    return in;
}

/* --- the one summarize step ------------------------------------------------------- */

static int ensure_engine(build *b) {
    if (*b->engine) return 0;
    const oc_sum_engine *e = b->cfg->engine;
    char err[256] = "";
    *b->engine = e->open(e->ctx, err, sizeof err);
    if (!*b->engine) { snprintf(b->err, sizeof b->err, "the model did not load: %s", err); return -1; }
    return 0;
}

/* Summarize `l`: the prompt, the model's answer, and the answer read back. Items
 * the model gave no line numbers are asked about once, in the same
 * conversation; those still without stand for every line. The stored summary
 * (heap), or NULL with b->err set. */
static char *summarize(build *b, const char *intro, const oc_sum_lines *l, const oc_sum_people *pp,
                       uint32_t *tokens, uint32_t *cpu) {
    if (ensure_engine(b) != 0) return NULL;
    const oc_sum_engine *e = b->cfg->engine;
    oc_sum_buf prompt = {0};
    if (oc_sum_prompt(intro, l, &prompt) != 0) {
        oc_sum_buf_free(&prompt);
        snprintf(b->err, sizeof b->err, "out of memory");
        return NULL;
    }
    char *raw = NULL, *more = NULL, err[256] = "", *body = NULL;
    oc_sum_run_stats st, st2;
    memset(&st, 0, sizeof st);
    memset(&st2, 0, sizeof st2);
    oc_sum_buf out = {0}, ask = {0}, q = {0}, g = {0};
    int rc = e->run(*b->engine, OC_SUM_SYSTEM, prompt.p, 0, gate_fn, b, &raw, &st, err, sizeof err);
    b->calls++;
    if (rc != 0) {
        snprintf(b->err, sizeof b->err, "the model gave no answer: %s", err);
        if (raw && *raw && !*b->stopping && strcmp(err, "stopped") != 0)
            fprintf(stderr, "summary: the answer as far as it got:\n%s\n", raw);
        if (b->cfg->trace) b->cfg->trace(b->cfg->trace_ctx, prompt.p, raw ? raw : "", NULL);
        goto done;
    }
    int dropped = 0;
    if (oc_sum_parse(raw, NULL, l, pp, &out, &ask, &dropped) < 0) {
        snprintf(b->err, sizeof b->err, "the model's answer was not a summary");
        fprintf(stderr, "summary: the answer that was not a summary:\n%s\n", raw ? raw : "");
        if (b->cfg->trace) b->cfg->trace(b->cfg->trace_ctx, prompt.p, raw, NULL);
        goto done;
    }
    if (b->cfg->trace) b->cfg->trace(b->cfg->trace_ctx, prompt.p, raw, out.p);
    if (ask.n && e->more) {
        int n_items = 0;
        for (size_t i = 0; i < ask.n; i++) n_items += ask.p[i] == '\n';
        if (oc_sum_followup(ask.p, n_items, l->n, &q, &g) != 0) { snprintf(b->err, sizeof b->err, "out of memory"); goto done; }
        rc = e->more(*b->engine, q.p, g.p, gate_fn, b, &more, &st2, err, sizeof err);
        if (rc != 0 && (*b->stopping || !strcmp(err, "stopped"))) {
            snprintf(b->err, sizeof b->err, "the model gave no answer: %s", err);
            goto done;
        }
        if (rc != 0) fprintf(stderr, "summary: the follow-up gave no line numbers: %s\n", err);
        else {
            oc_sum_buf again = {0};
            if (oc_sum_parse(raw, more, l, pp, &again, NULL, &dropped) < 0) {
                oc_sum_buf_free(&again);
                snprintf(b->err, sizeof b->err, "out of memory");
                goto done;
            }
            oc_sum_buf_free(&out);
            out = again;
        }
        if (b->cfg->trace) b->cfg->trace(b->cfg->trace_ctx, q.p, more ? more : "", out.p);
    }
    body = out.p;
    out.p = NULL;
done:
    free(raw);
    free(more);
    oc_sum_buf_free(&out);
    oc_sum_buf_free(&ask);
    oc_sum_buf_free(&q);
    oc_sum_buf_free(&g);
    oc_sum_buf_free(&prompt);
    if (tokens) *tokens = st.prompt_tokens + st2.prompt_tokens;
    if (cpu) *cpu = st.cpu_ms + st2.cpu_ms;
    return body;
}

/* --- the recursion --------------------------------------------------------------- */

static const char *name_of(void *ctx, int64_t uid) {
    static __thread char nm[128];
    oc_sum_user_name(((build *)ctx)->db, uid, nm, sizeof nm);
    return nm;
}

/* "On Tue 08 Sep:" or "From Tue 08 Sep to Thu 10 Sep:", in the reader's zone. */
static void date_label(const build *b, int64_t start, int64_t end, char *out, size_t cap) {
    char a[32], z[32];
    time_t ts = (time_t)((start + (int64_t)b->tz * 60000) / 1000);
    time_t te = (time_t)((end + (int64_t)b->tz * 60000) / 1000);
    struct tm tm;
    gmtime_r(&ts, &tm);
    strftime(a, sizeof a, "%a %d %b", &tm);
    gmtime_r(&te, &tm);
    strftime(z, sizeof z, "%a %d %b", &tm);
    if (!strcmp(a, z)) snprintf(out, cap, "On %s:", a);
    else snprintf(out, cap, "From %s to %s:", a, z);
}

/* The lines of child summaries (dated when the build is), appended. */
static int kid_lines(build *b, const ref *kids, int n, oc_sum_lines *l, oc_sum_people *pp) {
    for (int i = 0; i < n; i++) {
        char label[96];
        if (b->dated) date_label(b, kids[i].start_ms, kids[i].end_ms, label, sizeof label);
        if (oc_sum_body_lines(kids[i].body, name_of, b, b->dated ? label : NULL, l, pp) != 0) {
            snprintf(b->err, sizeof b->err, "a stored summary could not be read");
            return -1;
        }
    }
    return 0;
}

static size_t kids_tokens(build *b, const ref *kids, int n) {
    oc_sum_lines l = {0};
    oc_sum_people pp = {0};
    size_t t = kid_lines(b, kids, n, &l, &pp) == 0 ? oc_sum_lines_tokens(&l) : (size_t)-1;
    oc_sum_lines_free(&l);
    oc_sum_people_free(&pp);
    return t;
}

/* Summarize these children, together, into one node of `kind`: found if made
 * before, else one model call. */
static int summarize_group(build *b, const ref *kids, int n, int kind, const oc_sum_new *proto, ref *out) {
    memset(out, 0, sizeof *out);
    char ikey[72];
    if (proto->ikey[0]) snprintf(ikey, sizeof ikey, "%s", proto->ikey);
    else key_of(kind_tag(kind), kids, n, b->dated, b->tz, ikey, sizeof ikey);
    /* A period spans what was asked; anything else, what it holds. */
    int64_t start = kind == OC_SUM_KIND_PERIOD || proto->start_ms ? proto->start_ms : kids[0].start_ms;
    int64_t end = kind == OC_SUM_KIND_PERIOD || proto->end_ms ? proto->end_ms : kids[n - 1].end_ms;
    if (kind != OC_SUM_KIND_PERIOD && find(b, ikey, out)) {
        out->start_ms = start;
        out->end_ms = end;
        return 0;
    }
    oc_sum_lines l = {0};
    oc_sum_people pp = {0};
    if (kid_lines(b, kids, n, &l, &pp) != 0) { oc_sum_lines_free(&l); oc_sum_people_free(&pp); return -1; }
    char intro[256];
    snprintf(intro, sizeof intro, "Summaries of consecutive parts of %s, oldest first.", b->where);
    uint32_t tok = 0, cpu = 0;
    char *body = summarize(b, intro, &l, &pp, &tok, &cpu);
    oc_sum_lines_free(&l);
    oc_sum_people_free(&pp);
    if (!body) return -1;
    oc_sum_input *in = inputs_of(kids, n);
    if (!in) { free(body); return -1; }
    oc_sum_new x = *proto;
    x.kind = kind;
    snprintf(x.ikey, sizeof x.ikey, "%s", ikey);
    x.start_ms = start;
    x.end_ms = end;
    x.tz_offset_min = b->dated ? b->tz : 0;
    x.tokens_in = tok;
    x.cpu_ms = cpu;
    return keep(b, &x, body, in, n, out);
}

/* The recursion: what fits is summarized once; what does not is cut, in order,
 * into sections that fit, each summarized, and the sections go through this
 * again. A thing of one part is that part. */
static int reduce(build *b, const ref *kids, int n, int kind, const oc_sum_new *proto, ref *out) {
    memset(out, 0, sizeof *out);
    if (n == 1) {
        *out = kids[0];
        out->body = strdup(kids[0].body);
        return out->body ? 0 : -1;
    }
    size_t total = kids_tokens(b, kids, n);
    if (total == (size_t)-1) return -1;
    if (total <= b->cfg->threshold) return summarize_group(b, kids, n, kind, proto, out);

    /* Sections, in order: as many children as fit together. */
    int *ends = malloc((size_t)n * sizeof *ends);
    ref *sec = calloc((size_t)n, sizeof *sec);
    int ns = 0, ng = 0, singles = 0, rc = -1;
    if (!ends || !sec) { free(ends); free(sec); return -1; }
    for (int s = 0; s < n; ) {
        int e = s + 1;
        while (e < n && kids_tokens(b, kids + s, e - s + 1) <= b->cfg->threshold) e++;
        ends[ng++] = e;
        singles += e - s == 1;
        s = e;
    }
    for (int g = 0, s = 0; g < ng; s = ends[g++]) {
        int e = ends[g];
        /* A child that fits, alone in its section, goes up as it is -- unless
         * every section is one child, when only summarizing them gets shorter. */
        if (e - s == 1 && singles < ng && kids_tokens(b, kids + s, 1) <= b->cfg->threshold) {
            sec[ns] = kids[s];
            sec[ns].body = strdup(kids[s].body);
            if (!sec[ns].body) goto out;
            ns++;
            continue;
        }
        oc_sum_new sp;
        memset(&sp, 0, sizeof sp);
        sp.root_id = proto->root_id;
        if (summarize_group(b, kids + s, e - s, OC_SUM_KIND_SECTION, &sp, &sec[ns]) != 0) goto out;
        ns++;
    }
    /* Each level must come out smaller than what went in, or it never ends. */
    size_t after = kids_tokens(b, sec, ns);
    if (after >= total) {
        snprintf(b->err, sizeof b->err,
                 "the summaries of %d parts came to %zu tokens from %zu: they did not get shorter", n, after, total);
        fprintf(stderr, "summary: %s\n", b->err);
        goto out;
    }
    rc = reduce(b, sec, ns, kind, proto, out);
out:
    ref_free(sec, ns);
    free(sec);
    free(ends);
    return rc;
}

/* --- chunks ------------------------------------------------------------------------ */

static int64_t chunk_start(const oc_sum_msg *m, const oc_sum_chunk *c) {
    int64_t s = INT64_MAX;
    for (int k = 0; k < c->n; k++) if (m[c->idx[k]].created_ms < s) s = m[c->idx[k]].created_ms;
    return s == INT64_MAX ? 0 : s;
}

/* A message too big for a chunk: its parts, each summarized, then summarized
 * together through the recursion. The result is the chunk's (key `ikey`). */
static int big_message(build *b, const oc_sum_msg *m, const oc_sum_chunk *c, const char *ikey, ref *out) {
    const char *who = m->author && *m->author ? m->author : "someone";
    size_t label = (strlen(who) + 40) / SUM_BYTES_PER_TOKEN + 1;
    size_t room = b->cfg->threshold > 2 * label ? b->cfg->threshold - label : b->cfg->threshold;
    char **parts = NULL;
    int np = 0;
    if (oc_sum_split_text(m->text, room, &parts, &np) != 0) { snprintf(b->err, sizeof b->err, "out of memory"); return -1; }
    ref *kids = calloc((size_t)(np ? np : 1), sizeof *kids);
    int nk = 0, rc = -1;
    if (!kids) goto out;
    for (int k = 0; k < np; k++) {
        char key[72];
        part_key(m->id, m->edited_ms, k, np, key, sizeof key);
        if (find(b, key, &kids[nk])) {
            kids[nk].start_ms = kids[nk].end_ms = m->created_ms;
            nk++;
            continue;
        }
        oc_sum_lines l = {0};
        oc_sum_people pp = {0};
        oc_sum_buf t = {0};
        oc_sum_buf_printf(&t, "%s (part %d of %d): %s", who, k + 1, np, parts[k]);
        char intro[256];
        snprintf(intro, sizeof intro, "One long message in %s, part %d of %d.", b->where, k + 1, np);
        uint32_t tok = 0, cpu = 0;
        char *body = NULL;
        if (!t.oom && oc_sum_lines_add(&l, t.p, 0, NULL, &m->id, 1) == 0 && oc_sum_people_add(&pp, m->author_id, who) == 0)
            body = summarize(b, intro, &l, &pp, &tok, &cpu);
        oc_sum_buf_free(&t);
        oc_sum_lines_free(&l);
        oc_sum_people_free(&pp);
        if (!body) goto out;
        oc_sum_input *in = malloc(sizeof *in);
        if (!in) { free(body); goto out; }
        in[0].kind = OC_SUM_IN_MSG; in[0].id = m->id; in[0].stamp = m->edited_ms;
        oc_sum_new x;
        memset(&x, 0, sizeof x);
        x.kind = OC_SUM_KIND_CHUNK;
        snprintf(x.ikey, sizeof x.ikey, "%s", key);
        x.root_id = c->root_id;
        x.first_msg_id = x.last_msg_id = m->id;
        x.start_ms = x.end_ms = m->created_ms;
        x.tokens_in = tok;
        x.cpu_ms = cpu;
        if (keep(b, &x, body, in, 1, &kids[nk]) != 0) goto out;
        nk++;
    }
    oc_sum_new proto;
    memset(&proto, 0, sizeof proto);
    snprintf(proto.ikey, sizeof proto.ikey, "%s", ikey);
    proto.root_id = c->root_id;
    proto.first_msg_id = proto.last_msg_id = m->id;
    proto.start_ms = m->created_ms;
    proto.end_ms = c->end_ms;
    rc = reduce(b, kids, nk, OC_SUM_KIND_CHUNK, &proto, out);
out:
    if (kids) ref_free(kids, nk);
    free(kids);
    oc_sum_parts_free(parts, np);
    return rc;
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
    int64_t start = chunk_start(w->msgs, c);
    if (find(b, ikey, out)) {
        free(in);
        out->start_ms = start;
        out->end_ms = c->end_ms;
        return 0;
    }
    const oc_sum_msg *m0 = &w->msgs[c->idx[0]];
    if (c->n == 1 && oc_sum_msg_tokens(m0) > b->cfg->threshold) {
        free(in);
        return big_message(b, m0, c, ikey, out);
    }
    oc_sum_lines l = {0};
    oc_sum_people pp = {0};
    char intro[256];
    snprintf(intro, sizeof intro, "Messages from %s, oldest first. Indented lines are replies in a thread.", b->where);
    uint32_t tok = 0, cpu = 0;
    char *body = NULL;
    if (oc_sum_chunk_lines(w->msgs, c, &l, &pp) == 0) body = summarize(b, intro, &l, &pp, &tok, &cpu);
    else snprintf(b->err, sizeof b->err, "out of memory");
    oc_sum_lines_free(&l);
    oc_sum_people_free(&pp);
    if (!body) { free(in); return -1; }
    oc_sum_new x;
    memset(&x, 0, sizeof x);
    x.kind = OC_SUM_KIND_CHUNK;
    snprintf(x.ikey, sizeof x.ikey, "%s", ikey);
    x.root_id = c->root_id;
    x.first_msg_id = w->msgs[c->idx[0]].id;
    x.last_msg_id = w->msgs[c->idx[c->n - 1]].id;
    x.start_ms = start;
    x.end_ms = c->end_ms;
    x.tokens_in = tok;
    x.cpu_ms = cpu;
    return keep(b, &x, body, in, c->n, out);
}

/* Every chunk and thread summary of the channel whose last activity is in
 * [start, end), in order: found, or made. The cut starts at the quiet gap
 * before `start`, so it makes the same chunks every cut there makes. */
static int build_pieces(build *b, int64_t start, int64_t end, ref **kids_out, int *n_out) {
    *kids_out = NULL;
    *n_out = 0;
    int64_t from = oc_sum_anchor(b->db, b->channel, start, b->cfg->gap_ms);
    oc_sum_window w;
    if (oc_sum_load_window(b->db, b->channel, from, end, &w) != 0) {
        snprintf(b->err, sizeof b->err, "reading the channel failed");
        return -1;
    }
    if (!strcmp(w.channel, "direct message")) snprintf(b->where, sizeof b->where, "a direct message");
    else snprintf(b->where, sizeof b->where, "#%s", w.channel);
    oc_sum_cut cut;
    if (oc_sum_cut_build(w.msgs, w.n, b->cfg->threshold, b->cfg->gap_ms, &cut) != 0) {
        oc_sum_window_free(&w);
        snprintf(b->err, sizeof b->err, "out of memory");
        return -1;
    }
    ref *kids = calloc((size_t)(cut.n ? cut.n : 1), sizeof *kids);
    int nk = 0, rc = -1;
    if (!kids) goto out;
    for (int p = 0; p < cut.n; p++) {
        const oc_sum_piece *pc = &cut.pieces[p];
        if (pc->end_ms < start) continue;
        if (!pc->is_big_thread) {
            if (build_chunk(b, &w, &pc->chunks[0], &kids[nk]) != 0) goto out;
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
            ok = reduce(b, tc, nt, OC_SUM_KIND_THREAD, &tp, &kids[nk]) == 0;
            if (ok) nk++;
        }
        if (tc) ref_free(tc, nt);
        free(tc);
        if (!ok) goto out;
    }
    rc = 0;
out:
    oc_sum_cut_free(&cut);
    oc_sum_window_free(&w);
    if (rc != 0 && kids) { ref_free(kids, nk); free(kids); kids = NULL; nk = 0; }
    *kids_out = kids;
    *n_out = nk;
    return rc;
}

static const char *const EMPTY_BODY =
    "{\"overview\":\"\",\"decisions\":[],\"actions\":[],\"problems\":[],\"facts\":[]}";

/* The period [start, end) in zone `tz`: found, or made from the chunk and
 * thread summaries in it. */
static int build_period(build *b, int64_t start, int64_t end, int tz, ref *out) {
    memset(out, 0, sizeof *out);
    char *body = NULL;
    int64_t id;
    if (oc_sum_find_period(b->db, b->channel, start, end, tz, b->version, 0, &id, &body, NULL, 0)) {
        out->id = id; out->body = body; out->start_ms = start; out->end_ms = end;
        return 0;
    }
    b->dated = 0;
    ref *kids = NULL;
    int nk = 0;
    if (build_pieces(b, start, end, &kids, &nk) != 0) return -1;
    /* Pieces with nothing to say add nothing. */
    for (int i = 0; i < nk; ) {
        if (oc_sum_body_empty(kids[i].body)) {
            free(kids[i].body);
            memmove(&kids[i], &kids[i + 1], (size_t)(nk - i - 1) * sizeof *kids);
            nk--;
        } else {
            i++;
        }
    }
    b->dated = 1;
    b->tz = tz;
    oc_sum_new pp;
    memset(&pp, 0, sizeof pp);
    pp.kind = OC_SUM_KIND_PERIOD;
    pp.start_ms = start;
    pp.end_ms = end;
    pp.tz_offset_min = tz;
    int rc = -1;
    if (nk >= 2) {
        rc = reduce(b, kids, nk, OC_SUM_KIND_PERIOD, &pp, out);
    } else {
        if (nk) *out = kids[0];
        out->body = strdup(nk ? kids[0].body : EMPTY_BODY);
        rc = out->body ? 0 : -1;
    }
    /* A period is always its own row, even over one child (or none), so the
     * next request for it finds it. */
    if (rc == 0 && !(out->is_new && b->nodes[out->id].kind == OC_SUM_KIND_PERIOD &&
                     b->nodes[out->id].start_ms == start && b->nodes[out->id].end_ms == end)) {
        ref one = *out;
        int n_in = nk || out->ikey[0] ? 1 : 0;
        oc_sum_input *in = n_in ? inputs_of(&one, 1) : malloc(sizeof *in);
        char *bd = strdup(out->body);
        if (!in || !bd) { free(in); free(bd); rc = -1; goto done; }
        key_of('p', &one, n_in, 1, tz, pp.ikey, sizeof pp.ikey);
        free(out->body);
        memset(out, 0, sizeof *out);
        rc = keep(b, &pp, bd, in, n_in, out);
    }
done:
    b->dated = 0;
    ref_free(kids, nk);
    free(kids);
    return rc;
}

char *oc_sum_client_body(sqlite3 *db, const char *body) {
    oc_json d;
    if (!body || oc_json_parse(&d, body, strlen(body)) != 0) return NULL;
    int64_t *seen = NULL;
    int ns = 0;
    oc_sum_buf out = {0};
    oc_sum_buf_puts(&out, "{\"summary\":");
    oc_sum_buf_puts(&out, body);
    oc_sum_buf_puts(&out, ",\"people\":{");
    int arr = oc_json_get(&d, 0, "actions");
    if (arr >= 0 && d.t[arr].type == JSMN_ARRAY) {
        seen = malloc((size_t)(d.t[arr].size ? d.t[arr].size : 1) * sizeof *seen);
        int i = arr + 1;
        for (int k = 0; seen && k < d.t[arr].size; k++, i = oc_json_skip(&d, i)) {
            uint64_t v;
            if (oc_json_u64(&d, oc_json_get(&d, i, "who"), &v) != 0 || !v) continue;
            int dup = 0;
            for (int y = 0; y < ns; y++) if (seen[y] == (int64_t)v) dup = 1;
            if (dup) continue;
            seen[ns++] = (int64_t)v;
            char nm[128];
            oc_sum_user_name(db, (int64_t)v, nm, sizeof nm);
            oc_sum_buf_printf(&out, "%s\"%llu\":", ns > 1 ? "," : "", (unsigned long long)v);
            oc_sum_buf_json(&out, nm);
        }
    }
    oc_sum_buf_puts(&out, "}}");
    free(seen);
    oc_json_free(&d);
    if (out.oom) { oc_sum_buf_free(&out); return NULL; }
    return out.p;
}

static void build_init(build *b, const oc_sum_worker_cfg *cfg, sqlite3 *db, const char *version, void **engine,
                       oc_sum_load *gate, volatile int *stopping, int64_t channel) {
    memset(b, 0, sizeof *b);
    b->cfg = cfg; b->db = db; b->version = version; b->engine = engine; b->gate = gate; b->stopping = stopping;
    b->channel = channel;
    snprintf(b->where, sizeof b->where, "the channel");
}

static void build_done(build *b) {
    oc_sum_new_free(b->nodes, b->n_nodes);
    free(b->nodes);
    b->nodes = NULL;
    b->n_nodes = b->cap_nodes = 0;
}

/* Build one period and hand it to the sink; built again once if a message
 * changed while it was being built. */
static int run_build(const oc_sum_worker_cfg *cfg, sqlite3 *db, const char *version, void **engine,
                     oc_sum_load *gate, volatile int *stopping, const req *r, char *err, size_t errcap) {
    for (int attempt = 0; attempt < 2; attempt++) {
        build b;
        build_init(&b, cfg, db, version, engine, gate, stopping, r->channel);
        ref top;
        oc_sum_answer a;
        memset(&a, 0, sizeof a);
        a.conn_id = r->conn_id; a.req_id = r->req_id; a.channel = r->channel;
        a.start_ms = r->start_ms; a.end_ms = r->end_ms; a.tz_offset_min = r->tz; a.version = version;
        if (build_period(&b, r->start_ms, r->end_ms, r->tz, &top) != 0) {
            snprintf(err, errcap, "%s", b.err[0] ? b.err : "the summary could not be made");
            a.ok = 0; a.err = err;
            cfg->sink.store(cfg->sink.ctx, &a, NULL, 0);
            build_done(&b);
            return -1;
        }
        char *client = oc_sum_client_body(db, top.body);
        a.ok = client != NULL;
        a.body = client;
        a.err = client ? NULL : "out of memory";
        if (!top.is_new) a.summary_id = top.id;
        int st = cfg->sink.store(cfg->sink.ctx, &a, b.nodes, top.is_new ? b.n_nodes : 0);
        free(client);
        free(top.body);
        build_done(&b);
        if (st == 0) return 0;
        if (st < 0) { snprintf(err, errcap, "storing the summary failed"); return -1; }
    }
    snprintf(err, errcap, "the channel kept changing");
    return -1;
}

/* Summarize the chunks of [start, end) and store them, with nobody waiting.
 * 0, 1 when a message changed meanwhile, or -1. */
static int run_chunks(const oc_sum_worker_cfg *cfg, sqlite3 *db, const char *version, void **engine,
                      oc_sum_load *gate, volatile int *stopping, int64_t channel, int64_t start, int64_t end,
                      char *err, size_t errcap) {
    build b;
    build_init(&b, cfg, db, version, engine, gate, stopping, channel);
    ref *kids = NULL;
    int nk = 0;
    int rc = build_pieces(&b, start, end, &kids, &nk);
    if (rc != 0) {
        snprintf(err, errcap, "%s", b.err[0] ? b.err : "the chunks could not be made");
    } else if (b.n_nodes) {
        oc_sum_answer a;
        memset(&a, 0, sizeof a);
        a.channel = channel; a.start_ms = start; a.end_ms = end; a.version = version; a.ok = 1;
        rc = cfg->sink.store(cfg->sink.ctx, &a, b.nodes, b.n_nodes);
        if (rc < 0) snprintf(err, errcap, "storing the summaries failed");
    }
    ref_free(kids, nk);
    free(kids);
    build_done(&b);
    return rc;
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

int oc_sum_chunks_now(const oc_sum_worker_cfg *cfg, int64_t channel, int64_t start_ms, int64_t end_ms,
                      char *err, size_t errcap) {
    sqlite3 *db = NULL;
    if (open_db(cfg->db_path, &db, err, errcap) != 0) return -1;
    char version[160];
    oc_sum_version(cfg->engine, cfg->threshold, cfg->gap_ms, version, sizeof version);
    void *engine = NULL;
    volatile int stopping = 0;
    int rc = run_chunks(cfg, db, version, &engine, NULL, &stopping, channel, start_ms, end_ms, err, errcap);
    if (engine) cfg->engine->close(engine);
    sqlite3_close(db);
    return rc == 0 ? 0 : -1;
}

/* --- background work ------------------------------------------------------------ */

/* A message changed (from the writer's thread): summarize around it again. */
static void on_change(int64_t channel, int64_t at) {
    oc_sum_worker *w = __atomic_load_n(&g_worker, __ATOMIC_ACQUIRE);
    if (!w) return;
    pthread_mutex_lock(&w->mu);
    int dup = 0;
    for (int i = 0; i < w->n_changes && !dup; i++)
        if (w->changes[i].channel == channel && w->changes[i].at <= at) dup = 1;
    if (!dup) {
        if (w->n_changes == w->cap_changes) {
            int nc = w->cap_changes ? w->cap_changes * 2 : 16;
            change *nv = realloc(w->changes, (size_t)nc * sizeof *nv);
            if (nv) { w->changes = nv; w->cap_changes = nc; }
        }
        if (w->n_changes < w->cap_changes) {
            /* An earlier moment in the same channel covers a later one. */
            int k = 0;
            for (int i = 0; i < w->n_changes; i++)
                if (!(w->changes[i].channel == channel && w->changes[i].at > at)) w->changes[k++] = w->changes[i];
            w->n_changes = k;
            w->changes[w->n_changes].channel = channel;
            w->changes[w->n_changes].at = at;
            w->n_changes++;
        }
    }
    pthread_cond_signal(&w->cv);
    pthread_mutex_unlock(&w->mu);
}

static int take_change(oc_sum_worker *w, change *out) {
    pthread_mutex_lock(&w->mu);
    int got = w->n_changes > 0;
    if (got) {
        *out = w->changes[0];
        memmove(w->changes, w->changes + 1, (size_t)(w->n_changes - 1) * sizeof *w->changes);
        w->n_changes--;
    }
    pthread_mutex_unlock(&w->mu);
    return got;
}

/* A period of whole days, as a week, a recap or a range of days is: kept to be
 * made again after a change. Unread runs to the moment it was asked, which no
 * one asks for twice. */
static void remember_period(oc_sum_worker *w, const req *r) {
    if (oc_sum_day_start(r->start_ms, r->tz) != r->start_ms || oc_sum_day_start(r->end_ms, r->tz) != r->end_ms) return;
    for (int i = 0; i < w->n_asked; i++) {
        period_key *k = &w->asked[i];
        if (k->channel == r->channel && k->start == r->start_ms && k->end == r->end_ms && k->tz == r->tz) return;
    }
    if (w->n_asked == w->cap_asked) {
        int nc = w->cap_asked ? w->cap_asked * 2 : 16;
        period_key *nv = realloc(w->asked, (size_t)nc * sizeof *nv);
        if (!nv) return;
        w->asked = nv;
        w->cap_asked = nc;
    }
    period_key k = { r->channel, r->start_ms, r->end_ms, r->tz };
    w->asked[w->n_asked++] = k;
}

/* Every channel with people's messages, newest activity first, each to be
 * summarized from now back to its first message. */
static void fill_load(oc_sum_worker *w) {
    sqlite3_stmt *st = NULL;
    w->filled = 1;
    if (sqlite3_prepare_v2(w->db,
            "SELECT channel_id, MIN(created_at_ms), MAX(created_at_ms) FROM messages "
            "WHERE deleted_at_ms IS NULL AND kind=0 AND author_name IS NULL "
            "GROUP BY channel_id ORDER BY 3 DESC;", -1, &st, NULL) != SQLITE_OK)
        return;
    int cap = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (w->n_fill == cap) {
            int nc = cap ? cap * 2 : 32;
            backfill *nv = realloc(w->fill, (size_t)nc * sizeof *nv);
            if (!nv) break;
            w->fill = nv;
            cap = nc;
        }
        backfill f = { sqlite3_column_int64(st, 0), sqlite3_column_int64(st, 2) + 1, sqlite3_column_int64(st, 1) };
        w->fill[w->n_fill++] = f;
    }
    sqlite3_finalize(st);
}

/* One step of idle work. 1 if there was something to do. */
static int idle_step(oc_sum_worker *w, void **engine) {
    char err[600];
    const char *v = w->version;
    /* What a change left unsummarized. */
    change c;
    if (take_change(w, &c)) {
        int rc = run_chunks(&w->cfg, w->db, v, engine, &w->gate, &w->stopping, c.channel, c.at, wall_ms() + 1,
                            err, sizeof err);
        if (rc == 1) on_change(c.channel, c.at);         /* changed again meanwhile */
        else if (rc < 0 && !w->stopping) fprintf(stderr, "summary: channel %lld: %s\n", (long long)c.channel, err);
        return 1;
    }
    /* Every channel's chunks, the newest first, a day at a time. */
    if (!w->filled) fill_load(w);
    int best = -1;
    for (int i = 0; i < w->n_fill; i++) if (best < 0 || w->fill[i].cursor > w->fill[best].cursor) best = i;
    if (best >= 0) {
        backfill *f = &w->fill[best];
        int64_t from = f->cursor - DAY_MS;
        int rc = run_chunks(&w->cfg, w->db, v, engine, &w->gate, &w->stopping, f->channel, from, f->cursor,
                            err, sizeof err);
        if (rc < 0 && !w->stopping) fprintf(stderr, "summary: channel %lld: %s\n", (long long)f->channel, err);
        if (rc != 1) f->cursor = from;
        if (f->cursor <= f->first) w->fill[best] = w->fill[--w->n_fill];
        return 1;
    }
    /* Periods people asked for that a change has purged. */
    for (int i = 0; i < w->n_asked; i++) {
        period_key *k = &w->asked[i];
        int64_t id;
        char *body = NULL;
        if (oc_sum_find_period(w->db, k->channel, k->start, k->end, k->tz, v, 0, &id, &body, NULL, 0)) {
            free(body);
            continue;
        }
        req r = { 0, 0, k->channel, k->start, k->end, k->tz, NULL };
        if (run_build(&w->cfg, w->db, v, engine, &w->gate, &w->stopping, &r, err, sizeof err) != 0) {
            if (!w->stopping) fprintf(stderr, "summary: channel %lld, background: %s\n", (long long)k->channel, err);
            w->asked[i] = w->asked[--w->n_asked];   /* not again until asked again */
        }
        return 1;
    }
    /* Periods an older model or prompt made. */
    sqlite3_stmt *st = NULL;
    int did = 0;
    if (sqlite3_prepare_v2(w->db,
            "SELECT channel_id, start_ms, end_ms, tz_offset_min FROM summary_nodes WHERE kind=3 AND version<>?1 "
            "AND NOT EXISTS (SELECT 1 FROM summary_nodes n WHERE n.kind=3 AND n.version=?1 AND "
            "n.channel_id=summary_nodes.channel_id AND n.start_ms=summary_nodes.start_ms AND "
            "n.end_ms=summary_nodes.end_ms AND n.tz_offset_min=summary_nodes.tz_offset_min) LIMIT 1;",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, v, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) {
            req r = { 0, 0, sqlite3_column_int64(st, 0), sqlite3_column_int64(st, 1), sqlite3_column_int64(st, 2),
                      sqlite3_column_int(st, 3), NULL };
            sqlite3_finalize(st);
            st = NULL;
            if (run_build(&w->cfg, w->db, v, engine, &w->gate, &w->stopping, &r, err, sizeof err) != 0 && !w->stopping)
                fprintf(stderr, "summary: channel %lld, rebuilding: %s\n", (long long)r.channel, err);
            did = 1;
        }
    }
    sqlite3_finalize(st);
    return did;
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
    uint64_t last_used = 0;
    int idle_left = 1;           /* idle work may be waiting */
    int ready = 0;               /* ...and the machine let it run last time */
    char err[600];
    for (;;) {
        pthread_mutex_lock(&w->mu);
        if (!w->head && !w->stopping && !ready) {
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
        if (w->n_changes) idle_left = 1;
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
            remember_period(w, r);
            if (run_build(&w->cfg, w->db, w->version, &engine, &w->gate, &w->stopping, r, err, sizeof err) != 0)
                fprintf(stderr, "summary: channel %lld: %s\n", (long long)r->channel, err);
            last_used = mono_ms();
            free(r);
            continue;
        }
        ready = w->cfg.background && idle_left && !busy && oc_sum_load_settled(&w->gate);
        if (!ready) continue;
        if (idle_step(w, &engine)) last_used = mono_ms();
        else ready = idle_left = 0;
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
    __atomic_store_n(&g_worker, w, __ATOMIC_RELEASE);
    oc_sum_on_change(on_change);
    if (pthread_create(&w->thread, NULL, run, w) != 0) {
        snprintf(err, errcap, "cannot start the summary thread");
        oc_sum_on_change(NULL);
        __atomic_store_n(&g_worker, NULL, __ATOMIC_RELEASE);
        sqlite3_close(w->db);
        pthread_mutex_destroy(&w->mu);
        pthread_cond_destroy(&w->cv);
        free(w);
        return NULL;
    }
    return w;
}

void oc_sum_worker_stop(oc_sum_worker *w) {
    if (!w) return;
    oc_sum_on_change(NULL);
    __atomic_store_n(&g_worker, NULL, __ATOMIC_RELEASE);
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
    free(w->changes);
    free(w->asked);
    free(w->fill);
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
