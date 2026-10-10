/* The summary worker (sum_worker.h). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "sum_worker.h"

#include <ctype.h>
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
    int64_t     row;              /* summary_requests.id, 0 for idle work */
    volatile int64_t *cancel;     /* the row its asker cancelled (oc_sum_worker_cancel) */
} req;

/* One build: the connection, the engine, the gate and the nodes made so far. */
typedef struct {
    const oc_sum_worker_cfg *cfg;
    sqlite3      *db;
    const char   *version;
    void        **engine;         /* the open handle, opened on first need */
    oc_sum_load  *gate;           /* NULL: no gate */
    volatile int *stopping;
    volatile int *yield;          /* idle work: set when a request is waiting */
    oc_sum_new   *nodes;
    int           n_nodes, cap_nodes;
    int64_t       channel;
    char          where[160];     /* "#name", or "a direct message" */
    int           dated;          /* label children with their dates, in `tz` */
    int           tz;
    char          err[600];
    int           calls;
    oc_sum_run_stats spent;       /* every call's, added up (the request's log line) */
    int      dropped;             /* notes and sentences the checks took out, in all */
    int      rewrites;            /* calls that wrote a summary or a part of one again, shorter */
    int      reasks;              /* answers that ran away (hit their rail) and were asked once more */
    int64_t       row;            /* the request's row, 0 for idle work */
    volatile int64_t *cancel;     /* stops it when it holds `row` */
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
    volatile int      pending;        /* a request was queued: stop idle work */
    volatile int64_t  cancel_row;     /* a running request its asker cancelled */
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
    /* Its asker cancelled it. */
    if (b->row && b->cancel && __atomic_load_n(b->cancel, __ATOMIC_ACQUIRE) == b->row) return 1;
    /* Idle work gives way to a request someone is waiting on. */
    if (b->yield && __atomic_load_n(b->yield, __ATOMIC_ACQUIRE)) return 1;
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

/* --- one call to the model -------------------------------------------------------- */

static int ensure_engine(build *b) {
    if (*b->engine) return 0;
    const oc_sum_engine *e = b->cfg->engine;
    char err[256] = "";
    *b->engine = e->open(e->ctx, err, sizeof err);
    if (!*b->engine) { snprintf(b->err, sizeof b->err, "the model did not load: %s", err); return -1; }
    return 0;
}

/* `prompt`, its answer held to `grammar` and to `max_out` tokens. The answer
 * (heap), or NULL with b->err set. */
/* One call to the engine, counted and logged. */
static int call_once(build *b, const char *prompt, const char *grammar, int max_out, char **raw,
                     oc_sum_run_stats *st, char *err, size_t errcap) {
    const oc_sum_engine *e = b->cfg->engine;
    memset(st, 0, sizeof *st);
    int rc = e->run(*b->engine, OC_SUM_SYSTEM, prompt, grammar, max_out, gate_fn, b, raw, st, err, errcap);
    b->calls++;
    return rc;
}

static char *ask(build *b, const char *stage, const char *prompt, const char *grammar, int max_out,
                 uint32_t *tokens, uint32_t *cpu) {
    if (ensure_engine(b) != 0) return NULL;
    char *raw = NULL, err[256] = "";
    oc_sum_run_stats st;
    int rc = call_once(b, prompt, grammar, max_out, &raw, &st, err, sizeof err);
    /* An answer that ran away -- reached its rail without ending, which a
     * well-formed answer never does -- is asked for once more, the same
     * request: a provider's or a sampler's bad moment, not the prompt's. */
    if (rc != 0 && strstr(err, "without ending") && !*b->stopping) {
        fprintf(stderr, "summary: the %s answer ran away (%s); asking once more\n", stage, err);
        if (raw && *raw) fprintf(stderr, "summary: the answer as far as it got:\n%s\n", raw);
        b->reasks++;
        b->spent.prompt_tokens += st.prompt_tokens;
        b->spent.output_tokens += st.output_tokens;
        b->spent.wall_ms += st.wall_ms;
        free(raw);
        raw = NULL;
        rc = call_once(b, prompt, grammar, max_out, &raw, &st, err, sizeof err);
    }
    /* Where the time goes, call by call (SUMMARIES.md §6). */
    fprintf(stderr, "summary: call %s in %s: %u tokens in, %u out; reading %.1fs, writing %.1fs, %.1fs in all\n",
            stage, b->where, st.prompt_tokens, st.output_tokens, st.read_ms / 1000.0, st.write_ms / 1000.0,
            st.wall_ms / 1000.0);
    b->spent.prompt_tokens += st.prompt_tokens;
    b->spent.output_tokens += st.output_tokens;
    b->spent.read_ms += st.read_ms;
    b->spent.write_ms += st.write_ms;
    b->spent.wall_ms += st.wall_ms;
    b->spent.retries += st.retries;
    b->spent.wait_ms += st.wait_ms;
    if (tokens) *tokens += st.prompt_tokens;
    if (cpu) *cpu += st.cpu_ms;
    if (rc != 0) {
        snprintf(b->err, sizeof b->err, "the model gave no answer: %s", err);
        if (raw && *raw && !*b->stopping && strcmp(err, "stopped") != 0)
            fprintf(stderr, "summary: the answer as far as it got:\n%s\n", raw);
        if (b->cfg->trace) b->cfg->trace(b->cfg->trace_ctx, prompt, raw ? raw : "", NULL);
        free(raw);
        return NULL;
    }
    return raw;
}

/* Notes on `l`: the stored body (heap), or NULL with b->err set. */
static char *take_notes(build *b, const char *stage, const char *intro, const oc_sum_lines *l, uint32_t *tokens,
                        uint32_t *cpu) {
    oc_sum_buf prompt = {0}, g = {0}, out = {0};
    char *body = NULL;
    int notes_max = oc_sum_notes_for(oc_sum_lines_tokens(l), l->n);
    if (oc_sum_notes_prompt(intro, l, notes_max, &prompt, &g) != 0) { snprintf(b->err, sizeof b->err, "out of memory"); goto done; }
    /* Room for notes past what was asked: a model that writes more loses
     * them in the parser, not the call. */
    char *raw = ask(b, stage, prompt.p, g.p, (notes_max + SUM_NOTES_SLACK) * SUM_NOTE_TOKENS, tokens, cpu);
    if (!raw) goto done;
    int dropped = 0;
    if (oc_sum_parse_notes(raw, l, &out, &dropped, notes_max) < 0) {
        snprintf(b->err, sizeof b->err, "the model's answer was not notes");
        fprintf(stderr, "summary: the answer that was not notes:\n%s\n", raw);
    } else {
        body = out.p;
        out.p = NULL;
    }
    b->dropped += dropped;
    if (dropped) fprintf(stderr, "summary: %d of the notes dropped by the checks\n", dropped);
    if (b->cfg->trace) b->cfg->trace(b->cfg->trace_ctx, prompt.p, raw, body);
    free(raw);
done:
    oc_sum_buf_free(&prompt);
    oc_sum_buf_free(&g);
    oc_sum_buf_free(&out);
    return body;
}

/* A part over `cap` words: rewritten by the model, at most SUM_REWRITES times,
 * each rewrite checked as the first was; then cut at a sentence. -1 only when
 * the model failed outright (stopped, or out of memory). */
static int hold_part(build *b, oc_sum_part *p, int cap, const oc_sum_lines *l, uint32_t *tokens, uint32_t *cpu) {
    for (int k = 0; p->text && k < SUM_REWRITES && (int)oc_sum_words(p->text) > cap; k++) {
        oc_sum_buf prompt = {0}, g = {0};
        if (oc_sum_rewrite_prompt(p->text, cap, &prompt, &g) != 0) { oc_sum_buf_free(&prompt); oc_sum_buf_free(&g); return -1; }
        char *raw = ask(b, "rewrite", prompt.p, g.p, cap * 4 + 16, tokens, cpu);
        b->rewrites++;
        if (b->cfg->trace) b->cfg->trace(b->cfg->trace_ctx, prompt.p, raw ? raw : "", NULL);
        oc_sum_buf_free(&prompt);
        oc_sum_buf_free(&g);
        if (!raw) { if (*b->stopping) return -1; break; }
        /* Citations a rewrite writes are not its own: the part keeps its lines. */
        char *w = raw;
        for (char *r = raw; *r; ) {
            if (*r == '[') { char *e = r + 1; while (isdigit((unsigned char)*e)) e++; if (*e == ']' && e > r + 1) { r = e + 1; continue; } }
            *w++ = *r++;
        }
        *w = '\0';
        char *t = raw;
        while (*t == ' ') t++;
        size_t n = strlen(t);
        while (n && (t[n - 1] == '\n' || t[n - 1] == ' ')) t[--n] = '\0';
        if (*t) {
            char *keep = strdup(t);
            if (keep) {
                char *old = p->text;
                p->text = keep;
                oc_sum_part_check(p, l);
                if (!*p->text) { free(p->text); p->text = old; } else free(old);
            }
        }
        free(raw);
    }
    if (p->text) oc_sum_cut_words(p->text, cap);
    return 0;
}

/* The summary a reader is shown, of `l` (messages, or notes and the messages
 * they cite) over a span of `input_words` words: the stored body (heap), or
 * NULL with b->err set. */
static char *write_summary(build *b, const char *intro, const oc_sum_lines *l, size_t input_words,
                           uint32_t *tokens, uint32_t *cpu) {
    oc_sum_caps c;
    oc_sum_caps_for(input_words, &c);
    oc_sum_buf prompt = {0}, g = {0}, out = {0};
    oc_sum_final f;
    memset(&f, 0, sizeof f);
    char *body = NULL, *raw = NULL;
    if (oc_sum_final_prompt(intro, l, &c, &prompt, &g) != 0) { snprintf(b->err, sizeof b->err, "out of memory"); goto done; }
    /* The answer's rail: room for the whole shape, so an answer that keeps
     * the form always ends, and never more than the context has left after
     * the prompt (SUMMARIES.md §2). The length is asked for in the prompt and
     * held by the shorten pass and the trim below, not here. */
    int rail = c.room * SUM_TOKENS_PER_WORD + SUM_FINAL_SPARE_TOKENS;
    int left = SUM_CTX_TOKENS - (int)(prompt.n / SUM_BYTES_PER_TOKEN) - SUM_FINAL_SPARE_TOKENS;
    if (rail > left) rail = left;
    if (rail < c.total * 2) rail = c.total * 2;
    raw = ask(b, "final", prompt.p, g.p, rail, tokens, cpu);
    if (!raw) goto done;
    int dropped = 0;
    if (oc_sum_parse_final(raw, l, &c, &f, &dropped) != 0) {
        snprintf(b->err, sizeof b->err, "the model's answer was not a summary");
        fprintf(stderr, "summary: the answer that was not a summary:\n%s\n", raw);
        if (b->cfg->trace) b->cfg->trace(b->cfg->trace_ctx, prompt.p, raw, NULL);
        goto done;
    }
    b->dropped += dropped;
    if (dropped) fprintf(stderr, "summary: %d sentences of the summary dropped by the checks\n", dropped);
    oc_sum_final_rank(&f);
    /* Well over its total: handed back once to be written shorter, in the same
     * form and under the same grammar; the shorter answer stands when it is
     * one, else the first does. One call, never two. */
    int words = oc_sum_final_words(&f);
    if (words > c.total + c.total * SUM_SHORTEN_OVER_PCT / 100) {
        oc_sum_buf sp = {0};
        if (oc_sum_shorten_prompt(&f, &c, &sp) != 0) { oc_sum_buf_free(&sp); snprintf(b->err, sizeof b->err, "out of memory"); goto done; }
        char *again = ask(b, "shorten", sp.p, g.p, rail, tokens, cpu);
        b->rewrites++;
        if (b->cfg->trace) b->cfg->trace(b->cfg->trace_ctx, sp.p, again ? again : "", NULL);
        oc_sum_buf_free(&sp);
        if (!again && *b->stopping) goto done;
        if (again) {
            oc_sum_final f2;
            memset(&f2, 0, sizeof f2);
            int d2 = 0;
            if (oc_sum_parse_final(again, l, &c, &f2, &d2) == 0 && oc_sum_final_words(&f2) < words) {
                oc_sum_final_free(&f);
                f = f2;
                b->dropped += d2;
                oc_sum_final_rank(&f);
                fprintf(stderr, "summary: written again: %d words, from %d, for a total of %d\n",
                        oc_sum_final_words(&f), words, c.total);
            } else {
                oc_sum_final_free(&f2);
                fprintf(stderr, "summary: written again no shorter (%d words for a total of %d); the first answer stands\n",
                        words, c.total);
            }
            free(again);
        }
        b->err[0] = '\0';
    }
    /* Each part to its cap, then the whole to its. */
    if (hold_part(b, &f.ov, c.overview, l, tokens, cpu) != 0) goto done;
    for (int t = 0; t < f.nt; t++) {
        if (f.top[t].para.text && hold_part(b, &f.top[t].para, c.para, l, tokens, cpu) != 0) goto done;
        for (int d = 0; d < f.top[t].nd; d++)
            if (hold_part(b, &f.top[t].det[d], c.detail, l, tokens, cpu) != 0) goto done;
    }
    for (int a = 0; a < f.na; a++)
        if (hold_part(b, &f.att[a], c.detail, l, tokens, cpu) != 0) goto done;
    oc_sum_final_fit(&f, &c);
    f.words = input_words;
    if (oc_sum_final_json(&f, &out) != 0) { snprintf(b->err, sizeof b->err, "out of memory"); goto done; }
    if (b->cfg->trace) b->cfg->trace(b->cfg->trace_ctx, prompt.p, raw, out.p);
    body = out.p;
    out.p = NULL;
done:
    free(raw);
    oc_sum_final_free(&f);
    oc_sum_buf_free(&prompt);
    oc_sum_buf_free(&g);
    oc_sum_buf_free(&out);
    return body;
}

/* --- the recursion --------------------------------------------------------------- */

/* "Tue 08 Sep" or "Tue 08 Sep to Thu 10 Sep", in the reader's zone. */
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

/* The lines of child notes, appended: each child headed with its dates when
 * the build is dated. */
static int kid_lines(build *b, const ref *kids, int n, oc_sum_lines *l) {
    for (int i = 0; i < n; i++) {
        char label[96];
        if (b->dated) date_label(b, kids[i].start_ms, kids[i].end_ms, label, sizeof label);
        if (oc_sum_notes_lines(kids[i].body, b->dated ? label : NULL, l) != 0) {
            snprintf(b->err, sizeof b->err, "stored notes could not be read");
            return -1;
        }
    }
    return 0;
}

static size_t kids_tokens(build *b, const ref *kids, int n) {
    oc_sum_lines l = {0};
    size_t t = kid_lines(b, kids, n, &l) == 0 ? oc_sum_lines_tokens(&l) : (size_t)-1;
    oc_sum_lines_free(&l);
    return t;
}

/* These children's notes merged into one node of `kind`: found if made before,
 * else one model call, which keeps fewer notes than the lines it read. */
static int merge(build *b, const ref *kids, int n, int kind, const oc_sum_new *proto, ref *out) {
    memset(out, 0, sizeof *out);
    char ikey[72];
    if (proto->ikey[0]) snprintf(ikey, sizeof ikey, "%s", proto->ikey);
    else key_of(kind_tag(kind), kids, n, b->dated, b->tz, ikey, sizeof ikey);
    int64_t start = proto->start_ms ? proto->start_ms : kids[0].start_ms;
    int64_t end = proto->end_ms ? proto->end_ms : kids[n - 1].end_ms;
    if (find(b, ikey, out)) {
        out->start_ms = start;
        out->end_ms = end;
        return 0;
    }
    oc_sum_lines l = {0};
    if (kid_lines(b, kids, n, &l) != 0) { oc_sum_lines_free(&l); return -1; }
    char intro[384];
    snprintf(intro, sizeof intro, "Notes on consecutive parts of %s, oldest first. Each is: (kind) topic: what "
             "happened.", b->where);
    uint32_t tok = 0, cpu = 0;
    char *body = take_notes(b, "merge", intro, &l, &tok, &cpu);
    oc_sum_lines_free(&l);
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

/* Bring `kids` under `budget` tokens, in place: while they do not fit, cut them,
 * in order, into sections of as many as fit together and merge each section of
 * two or more. Every round merges at least two into one, so it ends; a child
 * alone at the end of a round goes up as it is. */
static int fit(build *b, ref **kidsp, int *np, size_t budget) {
    for (;;) {
        ref *kids = *kidsp;
        int n = *np;
        size_t total = kids_tokens(b, kids, n);
        if (total == (size_t)-1) return -1;
        if (total <= budget || n < 2) return 0;
        ref *sec = calloc((size_t)n, sizeof *sec);
        if (!sec) return -1;
        int ns = 0;
        for (int s = 0; s < n; ) {
            int e = s + 1;
            while (e < n && kids_tokens(b, kids + s, e - s + 1) <= budget) e++;
            if (e - s == 1 && e < n) e++;              /* two always fit: notes are small */
            if (e - s == 1) {
                sec[ns] = kids[s];
                sec[ns].body = strdup(kids[s].body);
                if (!sec[ns].body) { ref_free(sec, ns); free(sec); return -1; }
            } else {
                oc_sum_new sp;
                memset(&sp, 0, sizeof sp);
                if (merge(b, kids + s, e - s, OC_SUM_KIND_SECTION, &sp, &sec[ns]) != 0) {
                    ref_free(sec, ns);
                    free(sec);
                    return -1;
                }
            }
            ns++;
            s = e;
        }
        ref_free(kids, n);
        free(kids);
        *kidsp = sec;
        *np = ns;
    }
}

/* One node of `kind` for these children (a big thread, a big message): fitted,
 * then merged into one. A thing of one part is that part. */
static int reduce_one(build *b, const ref *kids0, int n0, int kind, const oc_sum_new *proto, ref *out) {
    memset(out, 0, sizeof *out);
    ref *kids = calloc((size_t)(n0 ? n0 : 1), sizeof *kids);
    if (!kids) return -1;
    int n = 0, rc = -1;
    for (; n < n0; n++) {
        kids[n] = kids0[n];
        if (!(kids[n].body = strdup(kids0[n].body))) goto out;
    }
    if (n == 1) {
        *out = kids[0];
        kids[0].body = NULL;
        rc = 0;
        goto out;
    }
    if (fit(b, &kids, &n, SUM_INPUT_TOKENS) != 0) goto out;
    rc = n == 1 ? (*out = kids[0], kids[0].body = NULL, 0) : merge(b, kids, n, kind, proto, out);
out:
    ref_free(kids, n);
    free(kids);
    return rc;
}

/* --- chunks ------------------------------------------------------------------------ */

static int64_t chunk_start(const oc_sum_msg *m, const oc_sum_chunk *c) {
    int64_t s = INT64_MAX;
    for (int k = 0; k < c->n; k++) if (m[c->idx[k]].created_ms < s) s = m[c->idx[k]].created_ms;
    return s == INT64_MAX ? 0 : s;
}

/* A message too big for a chunk: notes on each of its parts, then those merged
 * into one. The result is the chunk's (key `ikey`). */
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
        oc_sum_buf t = {0};
        oc_sum_buf_printf(&t, "%s (part %d of %d): %s", who, k + 1, np, parts[k]);
        char intro[256];
        snprintf(intro, sizeof intro, "One long message in %s, part %d of %d.", b->where, k + 1, np);
        uint32_t tok = 0, cpu = 0;
        char *body = NULL;
        if (!t.oom && oc_sum_lines_add_full(&l, t.p, 0, NULL, &m->id, 1, &m->id, 1, who, 0) == 0)
            body = take_notes(b, "long message", intro, &l, &tok, &cpu);
        oc_sum_buf_free(&t);
        oc_sum_lines_free(&l);
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
    rc = reduce_one(b, kids, nk, OC_SUM_KIND_CHUNK, &proto, out);
out:
    if (kids) ref_free(kids, nk);
    free(kids);
    oc_sum_parts_free(parts, np);
    return rc;
}

/* A chunk's notes: found, or made. */
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
    char intro[256];
    snprintf(intro, sizeof intro, "Messages from %s, oldest first. Indented lines are replies in a thread.", b->where);
    uint32_t tok = 0, cpu = 0;
    char *body = NULL;
    if (oc_sum_chunk_lines(w->msgs, c, &l) == 0) body = take_notes(b, "chunk", intro, &l, &tok, &cpu);
    else snprintf(b->err, sizeof b->err, "out of memory");
    oc_sum_lines_free(&l);
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

/* The channel's messages around [start, end), and the cut of them: the cut
 * starts at the quiet gap before `start`, so it makes the same chunks every cut
 * there makes. */
static int load_cut(build *b, int64_t start, int64_t end, oc_sum_window *w, oc_sum_cut *cut) {
    int64_t from = oc_sum_anchor(b->db, b->channel, start, b->cfg->gap_ms);
    if (oc_sum_load_window(b->db, b->channel, from, end, w) != 0) {
        snprintf(b->err, sizeof b->err, "reading the channel failed");
        return -1;
    }
    if (!strcmp(w->channel, "direct message")) snprintf(b->where, sizeof b->where, "a direct message");
    else snprintf(b->where, sizeof b->where, "#%s", w->channel);
    if (oc_sum_cut_build(w->msgs, w->n, b->cfg->threshold, b->cfg->gap_ms, cut) != 0) {
        oc_sum_window_free(w);
        snprintf(b->err, sizeof b->err, "out of memory");
        return -1;
    }
    return 0;
}

/* Every chunk's and big thread's notes of the cut whose last activity is from
 * `start` on, in order: found, or made. */
static int build_pieces(build *b, const oc_sum_window *w, const oc_sum_cut *cut, int64_t start,
                        ref **kids_out, int *n_out) {
    *kids_out = NULL;
    *n_out = 0;
    ref *kids = calloc((size_t)(cut->n ? cut->n : 1), sizeof *kids);
    int nk = 0, rc = -1;
    if (!kids) return -1;
    for (int p = 0; p < cut->n; p++) {
        const oc_sum_piece *pc = &cut->pieces[p];
        if (pc->end_ms < start) continue;
        if (!pc->is_big_thread) {
            if (build_chunk(b, w, &pc->chunks[0], &kids[nk]) != 0) goto out;
            nk++;
            continue;
        }
        ref *tc = calloc((size_t)pc->n_chunks, sizeof *tc);
        int nt = 0, ok = tc != NULL;
        for (int k = 0; ok && k < pc->n_chunks; k++) {
            if (build_chunk(b, w, &pc->chunks[k], &tc[nt]) != 0) ok = 0; else nt++;
        }
        if (ok) {
            oc_sum_new tp;
            memset(&tp, 0, sizeof tp);
            tp.root_id = pc->root_id;
            tp.start_ms = tc[0].start_ms;
            tp.end_ms = pc->end_ms;
            ok = reduce_one(b, tc, nt, OC_SUM_KIND_THREAD, &tp, &kids[nk]) == 0;
            if (ok) nk++;
        }
        if (tc) ref_free(tc, nt);
        free(tc);
        if (!ok) goto out;
    }
    rc = 0;
out:
    if (rc != 0) { ref_free(kids, nk); free(kids); kids = NULL; nk = 0; }
    *kids_out = kids;
    *n_out = nk;
    return rc;
}

static const char *const EMPTY_BODY =
    "{\"overview\":{\"text\":\"\",\"refs\":[]},\"topics\":[],\"attention\":[],\"more\":[]}";

/* The period [start, end) in zone `tz`: found, or made. A span whose messages
 * fit one prompt is summarized from them directly; anything larger from its
 * pieces' notes, fitted (SUMMARIES.md §2). */
static int build_period(build *b, int64_t start, int64_t end, int tz, ref *out) {
    memset(out, 0, sizeof *out);
    char *body = NULL;
    int64_t id;
    if (oc_sum_find_period(b->db, b->channel, start, end, tz, b->version, 0, &id, &body, NULL, 0)) {
        out->id = id; out->body = body; out->start_ms = start; out->end_ms = end;
        return 0;
    }
    oc_sum_window w;
    oc_sum_cut cut;
    if (load_cut(b, start, end, &w, &cut) != 0) return -1;
    b->dated = 0;
    b->tz = tz;
    oc_sum_new pp;
    memset(&pp, 0, sizeof pp);
    pp.kind = OC_SUM_KIND_PERIOD;
    pp.start_ms = start;
    pp.end_ms = end;
    pp.tz_offset_min = tz;
    int rc = -1;
    ref *kids = NULL;
    int nk = 0;
    oc_sum_lines all = {0}, l = {0};
    oc_sum_input *in = NULL;
    int n_in = 0;
    uint32_t tok = 0, cpu = 0;
    /* The span's messages as lines, and every message in them. */
    for (int p = 0; p < cut.n; p++) {
        if (cut.pieces[p].end_ms < start) continue;
        for (int k = 0; k < cut.pieces[p].n_chunks; k++)
            if (oc_sum_chunk_lines(w.msgs, &cut.pieces[p].chunks[k], &all) != 0) goto done;
    }
    size_t words = oc_sum_lines_words(&all);
    if (!all.n) {
        body = strdup(EMPTY_BODY);
        in = malloc(sizeof *in);
        if (!body || !in) goto done;
    } else if (oc_sum_lines_tokens(&all) <= SUM_INPUT_TOKENS) {
        char intro[256];
        snprintf(intro, sizeof intro, "Messages from %s, oldest first. Indented lines are replies in a thread.", b->where);
        if (!(body = write_summary(b, intro, &all, words, &tok, &cpu))) goto done;
        for (int i = 0; i < all.n; i++) n_in += all.v[i].n_all;
        if (!(in = malloc((size_t)(n_in ? n_in : 1) * sizeof *in))) goto done;
        n_in = 0;
        for (int i = 0; i < all.n; i++)
            for (int k = 0; k < all.v[i].n_all; k++) {
                int64_t mid = all.v[i].all[k], stamp = 0;
                for (int q = 0; q < w.n; q++) if (w.msgs[q].id == mid) { stamp = w.msgs[q].edited_ms; break; }
                in[n_in].kind = OC_SUM_IN_MSG; in[n_in].id = mid; in[n_in].stamp = stamp;
                n_in++;
            }
    } else {
        if (build_pieces(b, &w, &cut, start, &kids, &nk) != 0) goto done;
        for (int i = 0; i < nk; ) {
            if (oc_sum_body_empty(kids[i].body)) {
                free(kids[i].body);
                memmove(&kids[i], &kids[i + 1], (size_t)(nk - i - 1) * sizeof *kids);
                nk--;
            } else i++;
        }
        b->dated = 1;
        if (fit(b, &kids, &nk, SUM_INPUT_TOKENS) != 0) goto done;
        if (kid_lines(b, kids, nk, &l) != 0) goto done;
        char intro[256];
        snprintf(intro, sizeof intro, "Notes on %s, oldest first, each \"(kind) topic: what happened\".", b->where);
        if (!(body = write_summary(b, intro, &l, words, &tok, &cpu))) goto done;
        if (!(in = inputs_of(kids, nk))) goto done;
        n_in = nk;
    }
    {
        /* The period's key from what it was made of; a period is always its own
         * row, so the next request for it finds it. */
        oc_sum_ikey('p', in, n_in, pp.ikey, sizeof pp.ikey);
        /* Its span and zone too: two spans over the same messages are two
         * periods. */
        uint64_t h = fnv(FNV_START, pp.ikey, strlen(pp.ikey));
        int64_t span[3] = { start, end, tz };
        h = fnv(h, span, sizeof span);
        snprintf(pp.ikey, sizeof pp.ikey, "p:%016llx:%d", (unsigned long long)h, n_in);
        pp.tokens_in = tok;
        pp.cpu_ms = cpu;
        rc = keep(b, &pp, body, in, n_in, out);
        body = NULL;
        in = NULL;
    }
done:
    free(body);
    free(in);
    b->dated = 0;
    if (kids) { ref_free(kids, nk); free(kids); }
    oc_sum_lines_free(&all);
    oc_sum_lines_free(&l);
    oc_sum_cut_free(&cut);
    oc_sum_window_free(&w);
    return rc;
}

char *oc_sum_client_body(sqlite3 *db, const char *body, int64_t channel, int64_t start_ms, int64_t end_ms) {
    oc_json d;
    if (!body || oc_json_parse(&d, body, strlen(body)) != 0) return NULL;
    /* Every message it cites, anywhere in it -- the overview, the topics and
     * their details, what needs attention -- once each, in order. */
    int64_t *ids = NULL;
    int n = 0, cap = 0, ok = 1;
    for (int i = 0; ok && i + 1 < d.n; i++) {
        if (d.t[i].type != JSMN_STRING || d.t[i].end - d.t[i].start != 4 || strncmp(d.js + d.t[i].start, "refs", 4) ||
            d.t[i + 1].type != JSMN_ARRAY)
            continue;
        int x = i + 2;
        for (int q = 0; ok && q < d.t[i + 1].size; q++, x = oc_json_skip(&d, x)) {
            uint64_t v;
            if (oc_json_u64(&d, x, &v) != 0) continue;
            int dup = 0;
            for (int y = 0; y < n && !dup; y++) dup = ids[y] == (int64_t)v;
            if (dup) continue;
            if (n == cap) {
                int nc = cap ? cap * 2 : 32;
                int64_t *nv = realloc(ids, (size_t)nc * sizeof *nv);
                if (!nv) { ok = 0; break; }
                ids = nv;
                cap = nc;
            }
            ids[n++] = (int64_t)v;
        }
    }
    oc_json_free(&d);
    oc_sum_buf out = {0};
    oc_sum_buf_puts(&out, "{\"summary\":");
    oc_sum_buf_puts(&out, body);
    oc_sum_buf_puts(&out, ",");
    if (!ok || oc_sum_span_people(db, channel, start_ms, end_ms, &out) != 0) ok = 0;
    oc_sum_buf_puts(&out, ",");
    if (!ok || oc_sum_sources(db, ids, n, &out) != 0) ok = 0;
    oc_sum_buf_puts(&out, "}");
    free(ids);
    if (!ok || out.oom) { oc_sum_buf_free(&out); return NULL; }
    return out.p;
}

static void build_init(build *b, const oc_sum_worker_cfg *cfg, sqlite3 *db, const char *version, void **engine,
                       oc_sum_load *gate, volatile int *stopping, volatile int *yield, int64_t channel) {
    memset(b, 0, sizeof *b);
    b->cfg = cfg; b->db = db; b->version = version; b->engine = engine; b->stopping = stopping;
    /* The load gate keeps the model off a machine that is busy; a hosted model
     * does not run here, so only stopping and yielding hold it. */
    b->gate = cfg->engine->remote ? NULL : gate;
    b->yield = yield;
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
 * changed while it was being built. `yield` (idle work only) stops it when a
 * request is waiting. */
static int run_build(const oc_sum_worker_cfg *cfg, sqlite3 *db, const char *version, void **engine,
                     oc_sum_load *gate, volatile int *stopping, volatile int *yield, const req *r, char *err,
                     size_t errcap) {
    for (int attempt = 0; attempt < 2; attempt++) {
        build b;
        build_init(&b, cfg, db, version, engine, gate, stopping, yield, r->channel);
        b.row = r->row;
        b.cancel = r->cancel;
        ref top;
        oc_sum_answer a;
        memset(&a, 0, sizeof a);
        a.conn_id = r->conn_id; a.req_id = r->req_id; a.channel = r->channel; a.request_id = r->row;
        a.start_ms = r->start_ms; a.end_ms = r->end_ms; a.tz_offset_min = r->tz; a.version = version;
        int built = build_period(&b, r->start_ms, r->end_ms, r->tz, &top);
        if (r->row)
            fprintf(stderr, "summary: request %lld spent %d calls: %u tokens in, %u out; reading %.1fs, writing "
                    "%.1fs, %.1fs in all; %d dropped by the checks; %d written again; %d asked again after running "
                    "away; %u retries waiting %.1fs\n",
                    (long long)r->row, b.calls, b.spent.prompt_tokens, b.spent.output_tokens, b.spent.read_ms / 1000.0,
                    b.spent.write_ms / 1000.0, b.spent.wall_ms / 1000.0, b.dropped, b.rewrites, b.reasks, b.spent.retries,
                    b.spent.wait_ms / 1000.0);
        a.dropped = b.dropped;
        a.rewrites = b.rewrites;
        a.reasks = b.reasks;
        a.retries = (int)b.spent.retries;
        a.wait_ms = b.spent.wait_ms;
        if (built != 0) {
            snprintf(err, errcap, "%s", b.err[0] ? b.err : "the summary could not be made");
            a.ok = 0; a.err = err;
            /* What was finished before it failed is kept: the next build of
             * anything over the same messages starts from it (§4). */
            if (b.n_nodes)
                fprintf(stderr, "summary: keeping the %d pieces a failed build made; the next try starts from them\n",
                        b.n_nodes);
            cfg->sink.store(cfg->sink.ctx, &a, b.nodes, b.n_nodes);
            build_done(&b);
            return -1;
        }
        char *client = oc_sum_client_body(db, top.body, r->channel, r->start_ms, r->end_ms);
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
                      oc_sum_load *gate, volatile int *stopping, volatile int *yield, int64_t channel, int64_t start,
                      int64_t end, char *err, size_t errcap) {
    build b;
    build_init(&b, cfg, db, version, engine, gate, stopping, yield, channel);
    ref *kids = NULL;
    int nk = 0;
    oc_sum_window w;
    oc_sum_cut cut;
    int rc = load_cut(&b, start, end, &w, &cut);
    if (rc == 0) {
        rc = build_pieces(&b, &w, &cut, start, &kids, &nk);
        oc_sum_cut_free(&cut);
        oc_sum_window_free(&w);
    }
    if (rc != 0) snprintf(err, errcap, "%s", b.err[0] ? b.err : "the chunks could not be made");
    if (b.n_nodes) {
        /* Stored whether or not every chunk was made: what was is kept. */
        oc_sum_answer a;
        memset(&a, 0, sizeof a);
        a.channel = channel; a.start_ms = start; a.end_ms = end; a.version = version; a.ok = rc == 0;
        a.err = rc == 0 ? NULL : err;
        if (rc != 0)
            fprintf(stderr, "summary: keeping the %d pieces a failed build made; the next try starts from them\n",
                    b.n_nodes);
        int st = cfg->sink.store(cfg->sink.ctx, &a, b.nodes, b.n_nodes);
        if (rc == 0) { rc = st; if (rc < 0) snprintf(err, errcap, "storing the summaries failed"); }
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

int oc_sum_build_keep(const oc_sum_worker_cfg *cfg, void **engine, int64_t channel, int64_t start_ms,
                      int64_t end_ms, int tz_offset_min, char *err, size_t errcap) {
    sqlite3 *db = NULL;
    if (open_db(cfg->db_path, &db, err, errcap) != 0) return -1;
    char version[160];
    oc_sum_version(cfg->engine, cfg->threshold, cfg->gap_ms, version, sizeof version);
    volatile int stopping = 0;
    req r = { 0, 0, channel, start_ms, end_ms, tz_offset_min, 0, NULL };
    int rc = run_build(cfg, db, version, engine, NULL, &stopping, NULL, &r, err, errcap);
    sqlite3_close(db);
    return rc;
}

int oc_sum_build_now(const oc_sum_worker_cfg *cfg, int64_t channel, int64_t start_ms, int64_t end_ms,
                     int tz_offset_min, char *err, size_t errcap) {
    void *engine = NULL;
    int rc = oc_sum_build_keep(cfg, &engine, channel, start_ms, end_ms, tz_offset_min, err, errcap);
    if (engine) cfg->engine->close(engine);
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
    int rc = run_chunks(cfg, db, version, &engine, NULL, &stopping, NULL, channel, start_ms, end_ms, err, errcap);
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

/* Every channel with messages in the last SUM_BACKGROUND_DAYS, newest
 * activity first, each to be summarized from now back to the window's start.
 * Older spans are made when someone asks for them. */
static void fill_load(oc_sum_worker *w) {
    sqlite3_stmt *st = NULL;
    w->filled = 1;
    if (sqlite3_prepare_v2(w->db,
            "SELECT channel_id, MIN(created_at_ms), MAX(created_at_ms) FROM messages "
            "WHERE deleted_at_ms IS NULL AND kind=0 AND created_at_ms>=?1 "
            "GROUP BY channel_id ORDER BY 3 DESC;", -1, &st, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_int64(st, 1, wall_ms() - (int64_t)SUM_BACKGROUND_DAYS * DAY_MS);
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

/* Idle work that failed because a request arrived: not a failure, and done
 * again later. */
static int yielded(oc_sum_worker *w, int rc) {
    return rc < 0 && __atomic_load_n(&w->pending, __ATOMIC_ACQUIRE);
}

/* The oldest request in the queue (summary_requests), if any. */
static int next_request(oc_sum_worker *w, req *out) {
    sqlite3_stmt *st = NULL;
    int got = 0;
    if (sqlite3_prepare_v2(w->db,
            "SELECT id, conn_id, req_id, channel_id, start_ms, end_ms, tz_offset_min FROM summary_requests "
            "WHERE state='queued' ORDER BY id LIMIT 1;", -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW) {
        memset(out, 0, sizeof *out);
        out->row = sqlite3_column_int64(st, 0);
        out->conn_id = (uint64_t)sqlite3_column_int64(st, 1);
        out->req_id = (uint32_t)sqlite3_column_int64(st, 2);
        out->channel = sqlite3_column_int64(st, 3);
        out->start_ms = sqlite3_column_int64(st, 4);
        out->end_ms = sqlite3_column_int64(st, 5);
        out->tz = sqlite3_column_int(st, 6);
        got = 1;
    }
    sqlite3_finalize(st);
    return got;
}

/* One step of idle work. 1 if there was something to do. */
static int idle_step(oc_sum_worker *w, void **engine) {
    char err[600];
    const char *v = w->version;
    /* What a change left unsummarized. */
    change c;
    if (take_change(w, &c)) {
        int rc = run_chunks(&w->cfg, w->db, v, engine, &w->gate, &w->stopping, &w->pending, c.channel, c.at,
                            wall_ms() + 1, err, sizeof err);
        if (rc == 1 || yielded(w, rc)) on_change(c.channel, c.at);   /* changed again, or to do again */
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
        int rc = run_chunks(&w->cfg, w->db, v, engine, &w->gate, &w->stopping, &w->pending, f->channel, from,
                            f->cursor, err, sizeof err);
        if (yielded(w, rc)) return 1;                    /* the same day, after the request */
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
        req r = { 0, 0, k->channel, k->start, k->end, k->tz, 0, NULL };
        int rc = run_build(&w->cfg, w->db, v, engine, &w->gate, &w->stopping, &w->pending, &r, err, sizeof err);
        if (rc != 0 && !yielded(w, rc)) {
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
                      sqlite3_column_int(st, 3), 0, NULL };
            sqlite3_finalize(st);
            st = NULL;
            int rc = run_build(&w->cfg, w->db, v, engine, &w->gate, &w->stopping, &w->pending, &r, err, sizeof err);
            if (rc != 0 && !yielded(w, rc) && !w->stopping)
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
        if (!w->pending && !w->stopping && !ready) {
            struct timespec until;
            clock_gettime(CLOCK_MONOTONIC, &until);
            until.tv_sec += 2;
            pthread_cond_timedwait(&w->cv, &w->mu, &until);
        }
        if (w->stopping) { pthread_mutex_unlock(&w->mu); break; }
        if (w->n_changes) idle_left = 1;
        pthread_mutex_unlock(&w->mu);
        /* The queue is the table; the flag only says to look now and to stop
         * idle work. Cleared before looking, so a request queued meanwhile
         * sets it again. */
        __atomic_store_n(&w->pending, 0, __ATOMIC_RELEASE);
        req rq, *r = NULL;
        if (w->cfg.sink.take && next_request(w, &rq)) {
            int t = w->cfg.sink.take(w->cfg.sink.ctx, rq.row);
            if (t == 1) r = &rq;
            else { __atomic_store_n(&w->pending, 1, __ATOMIC_RELEASE); continue; }   /* gone: look again */
        }

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
            r->cancel = &w->cancel_row;
            uint64_t t0 = mono_ms();
            fprintf(stderr, "summary: request %lld started: channel %lld, %lld..%lld\n", (long long)r->row,
                    (long long)r->channel, (long long)r->start_ms, (long long)r->end_ms);
            int built = run_build(&w->cfg, w->db, w->version, &engine, &w->gate, &w->stopping, NULL, r, err,
                                  sizeof err);
            /* Cancelled: stopped at a pause, or, when the call it was in ran to
             * its end (a hosted model is asked once per call), made and not
             * kept. */
            if (__atomic_load_n(&w->cancel_row, __ATOMIC_ACQUIRE) == r->row)
                fprintf(stderr, "summary: request %lld cancelled after %llus\n", (long long)r->row,
                        (unsigned long long)((mono_ms() - t0) / 1000));
            else if (built != 0)
                fprintf(stderr, "summary: request %lld failed after %llus: %s\n", (long long)r->row,
                        (unsigned long long)((mono_ms() - t0) / 1000), err);
            else
                fprintf(stderr, "summary: request %lld answered after %llus\n", (long long)r->row,
                        (unsigned long long)((mono_ms() - t0) / 1000));
            last_used = mono_ms();
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
    sqlite3_close(w->db);
    pthread_mutex_destroy(&w->mu);
    pthread_cond_destroy(&w->cv);
    free(w->changes);
    free(w->asked);
    free(w->fill);
    free(w);
}

void oc_sum_worker_cancel(oc_sum_worker *w, int64_t row) {
    if (w && row) __atomic_store_n(&w->cancel_row, row, __ATOMIC_RELEASE);
}

void oc_sum_worker_wake(oc_sum_worker *w) {
    if (!w) return;
    pthread_mutex_lock(&w->mu);
    __atomic_store_n(&w->pending, 1, __ATOMIC_RELEASE);
    pthread_cond_signal(&w->cv);
    pthread_mutex_unlock(&w->mu);
}
