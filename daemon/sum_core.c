/* Channel and DM summaries: the engine-free core (sum_core.h). */
#define _POSIX_C_SOURCE 200809L
#include "sum_core.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "json.h"

/* --- the growing string ------------------------------------------------------ */

static int buf_grow(oc_sum_buf *b, size_t more) {
    if (b->oom) return -1;
    if (b->n + more + 1 <= b->cap) return 0;
    size_t nc = b->cap ? b->cap : 256;
    while (nc < b->n + more + 1) nc *= 2;
    char *np = realloc(b->p, nc);
    if (!np) { b->oom = 1; return -1; }
    b->p = np;
    b->cap = nc;
    return 0;
}

void oc_sum_buf_add(oc_sum_buf *b, const char *s, size_t n) {
    if (buf_grow(b, n) != 0) return;
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = '\0';
}

void oc_sum_buf_puts(oc_sum_buf *b, const char *s) { oc_sum_buf_add(b, s, strlen(s)); }

void oc_sum_buf_printf(oc_sum_buf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char small[256];
    int n = vsnprintf(small, sizeof small, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof small) { oc_sum_buf_add(b, small, (size_t)n); return; }
    if (buf_grow(b, (size_t)n) != 0) return;
    va_start(ap, fmt);
    vsnprintf(b->p + b->n, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b->n += (size_t)n;
}

void oc_sum_buf_json(oc_sum_buf *b, const char *s) {
    oc_sum_buf_add(b, "\"", 1);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"':  oc_sum_buf_add(b, "\\\"", 2); break;
        case '\\': oc_sum_buf_add(b, "\\\\", 2); break;
        case '\n': oc_sum_buf_add(b, "\\n", 2); break;
        case '\r': oc_sum_buf_add(b, "\\r", 2); break;
        case '\t': oc_sum_buf_add(b, "\\t", 2); break;
        default:
            if (*p < 0x20) oc_sum_buf_printf(b, "\\u%04x", *p);
            else oc_sum_buf_add(b, (const char *)p, 1);
        }
    }
    oc_sum_buf_add(b, "\"", 1);
}

void oc_sum_buf_free(oc_sum_buf *b) { free(b->p); memset(b, 0, sizeof *b); }

/* --- cutting a channel into pieces ------------------------------------------- */

size_t oc_sum_msg_tokens(const oc_sum_msg *m) {
    size_t bytes = 24 + (m->author ? strlen(m->author) : 0) + (m->text ? strlen(m->text) : 0);
    return bytes / SUM_BYTES_PER_TOKEN + 1;
}

typedef struct {
    int64_t root;
    int    *idx;
    int     n, cap;
    int64_t end_ms;
    size_t  tokens;
} unit;

static int cmp_unit(const void *a, const void *b) {
    const unit *x = a, *y = b;
    if (x->end_ms != y->end_ms) return x->end_ms < y->end_ms ? -1 : 1;
    return x->root < y->root ? -1 : x->root > y->root;
}

static int add_idx(int **arr, int *n, int *cap, int v) {
    if (*n == *cap) {
        int nc = *cap ? *cap * 2 : 8;
        int *na = realloc(*arr, (size_t)nc * sizeof *na);
        if (!na) return -1;
        *arr = na;
        *cap = nc;
    }
    (*arr)[(*n)++] = v;
    return 0;
}

static int push_piece(oc_sum_cut *c, int *cap, oc_sum_piece p) {
    if (c->n == *cap) {
        int nc = *cap ? *cap * 2 : 16;
        oc_sum_piece *np = realloc(c->pieces, (size_t)nc * sizeof *np);
        if (!np) return -1;
        c->pieces = np;
        *cap = nc;
    }
    c->pieces[c->n++] = p;
    return 0;
}

static int chunk_from(const int *idx, int n, int64_t root, int64_t end_ms, oc_sum_chunk *out) {
    out->idx = malloc((size_t)(n ? n : 1) * sizeof *out->idx);
    if (!out->idx) return -1;
    memcpy(out->idx, idx, (size_t)n * sizeof *idx);
    out->n = n;
    out->root_id = root;
    out->end_ms = end_ms;
    return 0;
}

int oc_sum_cut_build(const oc_sum_msg *msgs, int n, size_t threshold, uint64_t gap_ms, oc_sum_cut *out) {
    memset(out, 0, sizeof *out);
    unit *units = NULL;
    int nu = 0, cu = 0, rc = -1;
    int pcap = 0;
    int *cur = NULL, ncur = 0, ccur = 0;
    size_t cur_tokens = 0;
    int64_t cur_end = 0, cur_root = 0;
    int cur_units = 0;
    /* Group by thread root, in message order. The roots are few per channel
     * window, so a linear search for each message's unit is fine here. */
    for (int i = 0; i < n; i++) {
        int64_t root = msgs[i].parent_id ? msgs[i].parent_id : msgs[i].id;
        int u = -1;
        for (int k = nu - 1; k >= 0; k--) if (units[k].root == root) { u = k; break; }
        if (u < 0) {
            if (nu == cu) {
                int nc = cu ? cu * 2 : 32;
                unit *nn = realloc(units, (size_t)nc * sizeof *nn);
                if (!nn) goto done;
                units = nn;
                cu = nc;
            }
            memset(&units[nu], 0, sizeof units[nu]);
            units[nu].root = root;
            u = nu++;
        }
        if (add_idx(&units[u].idx, &units[u].n, &units[u].cap, i) != 0) goto done;
        if (msgs[i].created_ms > units[u].end_ms) units[u].end_ms = msgs[i].created_ms;
        units[u].tokens += oc_sum_msg_tokens(&msgs[i]);
    }
    /* A thread is placed at its last activity. */
    qsort(units, (size_t)nu, sizeof *units, cmp_unit);

#define FLUSH()                                                                     \
    do {                                                                            \
        if (ncur) {                                                                 \
            oc_sum_piece p; memset(&p, 0, sizeof p);                                \
            p.chunks = malloc(sizeof *p.chunks);                                    \
            if (!p.chunks) goto done;                                               \
            p.n_chunks = 1;                                                         \
            p.root_id = cur_units == 1 ? cur_root : 0;                              \
            p.end_ms = cur_end;                                                     \
            if (chunk_from(cur, ncur, p.root_id, cur_end, &p.chunks[0]) != 0) { free(p.chunks); goto done; } \
            if (push_piece(out, &pcap, p) != 0) { free(p.chunks[0].idx); free(p.chunks); goto done; } \
            ncur = 0; cur_tokens = 0; cur_units = 0; cur_root = 0;                  \
        }                                                                           \
    } while (0)

    for (int u = 0; u < nu; u++) {
        unit *x = &units[u];
        int is_thread = x->n > 1;
        if (is_thread && x->tokens > threshold) {
            /* Too big for one piece: its own chunks of whole messages. */
            FLUSH();
            oc_sum_piece p;
            memset(&p, 0, sizeof p);
            p.is_big_thread = 1;
            p.root_id = x->root;
            p.end_ms = x->end_ms;
            int ccap = 0, s = 0;
            while (s < x->n) {
                size_t t = 0;
                int e = s;
                while (e < x->n && (e == s || t + oc_sum_msg_tokens(&msgs[x->idx[e]]) <= threshold))
                    t += oc_sum_msg_tokens(&msgs[x->idx[e++]]);
                if (p.n_chunks == ccap) {
                    int nc = ccap ? ccap * 2 : 4;
                    oc_sum_chunk *nch = realloc(p.chunks, (size_t)nc * sizeof *nch);
                    if (!nch) goto big_fail;
                    p.chunks = nch;
                    ccap = nc;
                }
                int64_t last = 0;
                for (int k = s; k < e; k++) if (msgs[x->idx[k]].created_ms > last) last = msgs[x->idx[k]].created_ms;
                if (chunk_from(x->idx + s, e - s, x->root, last, &p.chunks[p.n_chunks]) != 0) goto big_fail;
                p.n_chunks++;
                s = e;
            }
            if (push_piece(out, &pcap, p) != 0) goto big_fail;
            continue;
        big_fail:
            for (int k = 0; k < p.n_chunks; k++) free(p.chunks[k].idx);
            free(p.chunks);
            goto done;
        }
        if (ncur && ((uint64_t)(x->end_ms - cur_end) > gap_ms || cur_tokens + x->tokens > threshold))
            FLUSH();
        for (int k = 0; k < x->n; k++)
            if (add_idx(&cur, &ncur, &ccur, x->idx[k]) != 0) goto done;
        cur_tokens += x->tokens;
        cur_end = x->end_ms;
        cur_root = is_thread ? x->root : 0;
        cur_units++;
    }
    FLUSH();
#undef FLUSH
    rc = 0;
done:
    for (int u = 0; u < nu; u++) free(units[u].idx);
    free(units);
    free(cur);
    if (rc != 0) oc_sum_cut_free(out);
    return rc;
}

void oc_sum_cut_free(oc_sum_cut *c) {
    for (int i = 0; i < c->n; i++) {
        for (int k = 0; k < c->pieces[i].n_chunks; k++) free(c->pieces[i].chunks[k].idx);
        free(c->pieces[i].chunks);
    }
    free(c->pieces);
    memset(c, 0, sizeof *c);
}

/* --- ids --------------------------------------------------------------------- */

void oc_sum_ids_free(oc_sum_ids *ids) {
    for (int i = 0; i < ids->n_person; i++) free(ids->person_name[i]);
    free(ids->person);
    free(ids->person_name);
    free(ids->msg);
    free(ids->msg_text);
    for (int i = 0; i < ids->n_item; i++) { free(ids->item_text[i]); free(ids->item_refs[i]); }
    free(ids->item_text);
    free(ids->item_refs);
    free(ids->item_nrefs);
    memset(ids, 0, sizeof *ids);
}

static int person_of(oc_sum_ids *ids, int64_t uid, const char *name) {
    for (int i = 0; i < ids->n_person; i++) if (ids->person[i] == uid) return i + 1;
    int64_t *np = realloc(ids->person, (size_t)(ids->n_person + 1) * sizeof *np);
    if (!np) return -1;
    ids->person = np;
    char **nn = realloc(ids->person_name, (size_t)(ids->n_person + 1) * sizeof *nn);
    if (!nn) return -1;
    ids->person_name = nn;
    ids->person_name[ids->n_person] = strdup(name && *name ? name : "someone");
    if (!ids->person_name[ids->n_person]) return -1;
    ids->person[ids->n_person] = uid;
    return ++ids->n_person;
}

static void when(int64_t ms, char *out, size_t cap) {
    time_t t = (time_t)(ms / 1000);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, cap, "%a %d %b %H:%M", &tm);
}

static const char *const LINE_FIELDS =
    "Fields: overview = two or three sentences on what happened. decisions = things agreed or "
    "decided, with who decided (by). actions = follow-ups someone committed to or was asked to do: "
    "who does it and what, open or done. problems = things broken, blocked or worrying, open or "
    "resolved. facts = important numbers, dates, tickets, names of customers or releases. Every "
    "item cites (refs) the ids it comes from. Leave a list empty when there is nothing for it.";

const char *const OC_SUM_SYSTEM_LEAF =
    "You extract what happened in an excerpt of a team chat, for someone who was away. "
    "People are P1, P2, ...; messages are m1, m2, .... Use only the people and messages listed. "
    "Every item must cite the messages it comes from. State only what the messages say; never "
    "guess names, numbers or outcomes. Keep each text short.";

const char *const OC_SUM_SYSTEM_ROLLUP =
    "You merge summaries of consecutive parts of a team chat into one, for someone who was away. "
    "People are P1, P2, ...; the parts' items are i1, i2, .... Keep every decision and every "
    "action still open. Combine items that say the same thing. Mark an action done when a later "
    "part says it was done. Every item must cite the items (i#) it comes from. State only what "
    "the items say. Keep each text short.";

int oc_sum_render_chunk(const char *channel, const oc_sum_msg *msgs, const oc_sum_chunk *c,
                        oc_sum_buf *out, oc_sum_ids *ids) {
    memset(ids, 0, sizeof *ids);
    ids->msg = malloc((size_t)(c->n ? c->n : 1) * sizeof *ids->msg);
    ids->msg_text = malloc((size_t)(c->n ? c->n : 1) * sizeof *ids->msg_text);
    if (!ids->msg || !ids->msg_text) return -1;
    int *pid = malloc((size_t)(c->n ? c->n : 1) * sizeof *pid);
    if (!pid) return -1;
    for (int k = 0; k < c->n; k++) {
        const oc_sum_msg *m = &msgs[c->idx[k]];
        pid[k] = person_of(ids, m->author_id, m->author);
        if (pid[k] < 0) { free(pid); return -1; }
    }
    oc_sum_buf_printf(out, "Chat excerpt from #%s (times UTC). People:", channel && *channel ? channel : "chat");
    for (int i = 0; i < ids->n_person; i++)
        oc_sum_buf_printf(out, "%s P%d %s", i ? "," : "", i + 1, ids->person_name[i]);
    oc_sum_buf_puts(out, ".\nIndented lines are replies in a thread.\n\n");
    size_t cap = (size_t)SUM_THRESHOLD_TOKENS * SUM_BYTES_PER_TOKEN;
    for (int k = 0; k < c->n; k++) {
        const oc_sum_msg *m = &msgs[c->idx[k]];
        char t[32];
        when(m->created_ms, t, sizeof t);
        ids->msg[ids->n_msg] = m->id;
        ids->msg_text[ids->n_msg] = m->text ? m->text : "";
        ids->n_msg++;
        size_t tl = m->text ? strlen(m->text) : 0;
        oc_sum_buf_printf(out, "%s[m%d] P%d (%s): ", m->parent_id ? "  " : "", ids->n_msg, pid[k], t);
        oc_sum_buf_add(out, m->text ? m->text : "", tl < cap ? tl : cap);
        oc_sum_buf_puts(out, "\n");
    }
    oc_sum_buf_printf(out, "\n%s\n", LINE_FIELDS);
    free(pid);
    return out->oom ? -1 : 0;
}

/* --- reading a stored body ------------------------------------------------------ */

static const char *const KINDS[4] = { "decisions", "actions", "problems", "facts" };

typedef struct {
    char    text[SUM_TEXT_MAX * 2 + 1];
    int64_t who;              /* actions: user id, 0 for the team */
    int64_t by[4];
    int     n_by;
    int64_t refs[32];
    int     n_refs;
    int     open;             /* actions/problems: 1 open, 0 done/resolved */
} item;

static int read_i64_array(const oc_json *d, int arr, int64_t *out, int cap) {
    if (arr < 0 || d->t[arr].type != JSMN_ARRAY) return 0;
    int n = 0, i = arr + 1;
    for (int k = 0; k < d->t[arr].size; k++) {
        uint64_t v;
        if (n < cap && oc_json_u64(d, i, &v) == 0) out[n++] = (int64_t)v;
        i = oc_json_skip(d, i);
    }
    return n;
}

/* The items of one kind of a stored body (already parsed into `d`). */
static int stored_items(const oc_json *d, int kind, item *out, int cap) {
    int arr = oc_json_get(d, 0, KINDS[kind]);
    if (arr < 0 || d->t[arr].type != JSMN_ARRAY) return 0;
    int n = 0, i = arr + 1;
    for (int k = 0; k < d->t[arr].size && n < cap; k++) {
        item *it = &out[n];
        memset(it, 0, sizeof *it);
        const char *tk = kind == 1 ? "what" : "text";
        if (oc_json_get_str(d, i, tk, it->text, sizeof it->text) == 0) {
            if (kind == 0) it->n_by = read_i64_array(d, oc_json_get(d, i, "by"), it->by, 4);
            if (kind == 1) {
                uint64_t w = 0;
                if (oc_json_u64(d, oc_json_get(d, i, "who"), &w) == 0) it->who = (int64_t)w;
            }
            it->n_refs = read_i64_array(d, oc_json_get(d, i, "refs"), it->refs, 32);
            char st[16] = "";
            oc_json_get_str(d, i, "status", st, sizeof st);
            it->open = !strcmp(st, "open");
            n++;
        }
        i = oc_json_skip(d, i);
    }
    return n;
}

int oc_sum_body_empty(const char *body) {
    oc_json d;
    if (!body || oc_json_parse(&d, body, strlen(body)) != 0) return 1;
    char ov[8] = "";
    int empty = oc_json_get_str(&d, 0, "overview", ov, sizeof ov) != 0 || !ov[0];
    for (int kind = 0; kind < 4 && empty; kind++) {
        int arr = oc_json_get(&d, 0, KINDS[kind]);
        if (arr >= 0 && d.t[arr].type == JSMN_ARRAY && d.t[arr].size > 0) empty = 0;
    }
    oc_json_free(&d);
    return empty;
}

int oc_sum_body_overview(const char *body, char *out, size_t cap) {
    oc_json d;
    if (!body || oc_json_parse(&d, body, strlen(body)) != 0) return -1;
    int rc = oc_json_get_str(&d, 0, "overview", out, cap);
    oc_json_free(&d);
    return rc;
}

size_t oc_sum_rollup_tokens(const oc_sum_child *kids, int n) {
    size_t bytes = 0;
    for (int i = 0; i < n; i++) bytes += 48 + (kids[i].body ? strlen(kids[i].body) * 3 / 4 : 0);
    return bytes / SUM_BYTES_PER_TOKEN + 1;
}

static int add_item(oc_sum_ids *ids, const char *text, const int64_t *refs, int n_refs) {
    int k = ids->n_item;
    char **nt = realloc(ids->item_text, (size_t)(k + 1) * sizeof *nt);
    if (!nt) return -1;
    ids->item_text = nt;
    int64_t **nr = realloc(ids->item_refs, (size_t)(k + 1) * sizeof *nr);
    if (!nr) return -1;
    ids->item_refs = nr;
    int *nn = realloc(ids->item_nrefs, (size_t)(k + 1) * sizeof *nn);
    if (!nn) return -1;
    ids->item_nrefs = nn;
    ids->item_text[k] = strdup(text);
    ids->item_refs[k] = malloc((size_t)(n_refs ? n_refs : 1) * sizeof **ids->item_refs);
    if (!ids->item_text[k] || !ids->item_refs[k]) { free(ids->item_text[k]); free(ids->item_refs[k]); return -1; }
    memcpy(ids->item_refs[k], refs, (size_t)n_refs * sizeof *refs);
    ids->item_nrefs[k] = n_refs;
    ids->n_item++;
    return k + 1;
}

int oc_sum_render_rollup(const char *channel, const oc_sum_child *kids, int n, oc_sum_name_fn name,
                         void *ctx, oc_sum_buf *out, oc_sum_ids *ids) {
    memset(ids, 0, sizeof *ids);
    oc_sum_buf lines = {0};
    item *its = malloc(sizeof *its * SUM_MAX_ITEMS);
    if (!its) return -1;
    int rc = -1;
    for (int c = 0; c < n; c++) {
        oc_json d;
        if (!kids[c].body || oc_json_parse(&d, kids[c].body, strlen(kids[c].body)) != 0) continue;
        char ov[SUM_TEXT_MAX * 3 + 1] = "";
        oc_json_get_str(&d, 0, "overview", ov, sizeof ov);
        char a[32], b[32];
        when(kids[c].start_ms, a, sizeof a);
        when(kids[c].end_ms, b, sizeof b);
        oc_sum_buf_printf(&lines, "Part %d (%s to %s): %s\n", c + 1, a, b, ov);
        for (int kind = 0; kind < 4; kind++) {
            int m = stored_items(&d, kind, its, SUM_MAX_ITEMS);
            for (int k = 0; k < m; k++) {
                int iid = add_item(ids, its[k].text, its[k].refs, its[k].n_refs);
                if (iid < 0) { oc_json_free(&d); goto out; }
                switch (kind) {
                case 0: {
                    oc_sum_buf_printf(&lines, "  [i%d] Decision", iid);
                    for (int x = 0; x < its[k].n_by; x++) {
                        int p = person_of(ids, its[k].by[x], name ? name(ctx, its[k].by[x]) : NULL);
                        if (p < 0) { oc_json_free(&d); goto out; }
                        oc_sum_buf_printf(&lines, "%sP%d", x ? "," : " by ", p);
                    }
                    oc_sum_buf_printf(&lines, ": %s\n", its[k].text);
                    break;
                }
                case 1: {
                    if (its[k].who) {
                        int p = person_of(ids, its[k].who, name ? name(ctx, its[k].who) : NULL);
                        if (p < 0) { oc_json_free(&d); goto out; }
                        oc_sum_buf_printf(&lines, "  [i%d] Action for P%d (%s): %s\n", iid, p,
                                          its[k].open ? "open" : "done", its[k].text);
                    } else {
                        oc_sum_buf_printf(&lines, "  [i%d] Action for the team (%s): %s\n", iid,
                                          its[k].open ? "open" : "done", its[k].text);
                    }
                    break;
                }
                case 2:
                    oc_sum_buf_printf(&lines, "  [i%d] Problem (%s): %s\n", iid,
                                      its[k].open ? "open" : "resolved", its[k].text);
                    break;
                default:
                    oc_sum_buf_printf(&lines, "  [i%d] Fact: %s\n", iid, its[k].text);
                }
            }
        }
        oc_json_free(&d);
    }
    oc_sum_buf_printf(out, "Summaries of consecutive parts of #%s, oldest first. People:",
                      channel && *channel ? channel : "chat");
    for (int i = 0; i < ids->n_person; i++)
        oc_sum_buf_printf(out, "%s P%d %s", i ? "," : "", i + 1, ids->person_name[i]);
    if (!ids->n_person) oc_sum_buf_puts(out, " none named");
    oc_sum_buf_puts(out, ".\n\n");
    if (lines.p) oc_sum_buf_add(out, lines.p, lines.n);
    oc_sum_buf_printf(out, "\n%s\n", LINE_FIELDS);
    rc = out->oom || lines.oom ? -1 : 0;
out:
    free(its);
    oc_sum_buf_free(&lines);
    return rc;
}

/* --- the grammar -------------------------------------------------------------- */

int oc_sum_grammar(const oc_sum_ids *ids, int rollup, int max_items, oc_sum_buf *g) {
    if (max_items < 1 || max_items > SUM_MAX_ITEMS) max_items = SUM_MAX_ITEMS;
    oc_sum_buf_printf(g,
        "root ::= \"{\" ws \"\\\"overview\\\":\" ws str \",\" ws \"\\\"decisions\\\":\" ws decs \",\" ws "
        "\"\\\"actions\\\":\" ws acts \",\" ws \"\\\"problems\\\":\" ws probs \",\" ws \"\\\"facts\\\":\" ws facts ws \"}\"\n"
        "decs ::= \"[\" ws ( dec ( \",\" ws dec ){0,%d} )? ws \"]\"\n"
        "dec ::= \"{\" ws \"\\\"text\\\":\" ws str \",\" ws \"\\\"by\\\":\" ws people \",\" ws \"\\\"refs\\\":\" ws refs ws \"}\"\n"
        "acts ::= \"[\" ws ( act ( \",\" ws act ){0,%d} )? ws \"]\"\n"
        "act ::= \"{\" ws \"\\\"who\\\":\" ws who \",\" ws \"\\\"what\\\":\" ws str \",\" ws \"\\\"refs\\\":\" ws refs \",\" ws "
        "\"\\\"status\\\":\" ws ( \"\\\"open\\\"\" | \"\\\"done\\\"\" ) ws \"}\"\n"
        "probs ::= \"[\" ws ( prob ( \",\" ws prob ){0,%d} )? ws \"]\"\n"
        "prob ::= \"{\" ws \"\\\"text\\\":\" ws str \",\" ws \"\\\"refs\\\":\" ws refs \",\" ws "
        "\"\\\"status\\\":\" ws ( \"\\\"open\\\"\" | \"\\\"resolved\\\"\" ) ws \"}\"\n"
        "facts ::= \"[\" ws ( fact ( \",\" ws fact ){0,%d} )? ws \"]\"\n"
        "fact ::= \"{\" ws \"\\\"text\\\":\" ws str \",\" ws \"\\\"refs\\\":\" ws refs ws \"}\"\n"
        "people ::= \"[\" ws ( person ( \",\" ws person ){0,3} )? ws \"]\"\n"
        "who ::= person | \"\\\"team\\\"\"\n"
        "refs ::= \"[\" ws ref ( \",\" ws ref ){0,%d} ws \"]\"\n"
        "str ::= \"\\\"\" [^\"\\\\\\n\\r\\t]{1,%d} \"\\\"\"\n"
        "ws ::= [ \\n]{0,2}\n",
        max_items - 1, max_items - 1, max_items - 1, max_items - 1, SUM_MAX_REFS - 1,
        SUM_TEXT_MAX);
    oc_sum_buf_puts(g, "person ::= ");
    if (!ids->n_person) oc_sum_buf_puts(g, "\"\\\"P1\\\"\"");
    for (int i = 0; i < ids->n_person; i++) oc_sum_buf_printf(g, "%s\"\\\"P%d\\\"\"", i ? " | " : "", i + 1);
    oc_sum_buf_puts(g, "\nref ::= ");
    int nref = rollup ? ids->n_item : ids->n_msg;
    char tag = rollup ? 'i' : 'm';
    if (!nref) oc_sum_buf_printf(g, "\"\\\"%c1\\\"\"", tag);
    for (int i = 0; i < nref; i++) oc_sum_buf_printf(g, "%s\"\\\"%c%d\\\"\"", i ? " | " : "", tag, i + 1);
    oc_sum_buf_puts(g, "\n");
    return g->oom ? -1 : 0;
}

/* --- checking an answer -------------------------------------------------------- */

/* The id an "X12" string names (1-based), or 0 when it is not one of ours. */
static int short_id(const char *s, char tag, int max) {
    if (s[0] != tag) return 0;
    char *end;
    long v = strtol(s + 1, &end, 10);
    if (*end || v < 1 || v > max) return 0;
    return (int)v;
}

/* Every run of digits in `text` must occur in one of `src`. */
static int numbers_ok(const char *text, const char *const *src, int n) {
    for (const char *p = text; *p; ) {
        if (*p < '0' || *p > '9') { p++; continue; }
        const char *s = p;
        while (*p >= '0' && *p <= '9') p++;
        size_t len = (size_t)(p - s);
        char run[32];
        if (len >= sizeof run) return 0;
        memcpy(run, s, len);
        run[len] = '\0';
        int found = 0;
        for (int k = 0; k < n && !found; k++) if (src[k] && strstr(src[k], run)) found = 1;
        if (!found) return 0;
    }
    return 1;
}


/* The model's own ids in a text, as a reader should see them: P3 becomes the
 * person's name, a cited m7 or i7 is dropped. Written into `out`. */
static void readable(const char *in, const oc_sum_ids *ids, char *out, size_t cap) {
    size_t n = 0;
    for (const char *p = in; *p && n + 1 < cap; ) {
        int boundary = p == in || !((p[-1] >= 'A' && p[-1] <= 'Z') || (p[-1] >= 'a' && p[-1] <= 'z') ||
                                    (p[-1] >= '0' && p[-1] <= '9'));
        if (boundary && (*p == 'P' || *p == 'm' || *p == 'i') && p[1] >= '1' && p[1] <= '9') {
            const char *q = p + 1;
            long v = 0;
            while (*q >= '0' && *q <= '9') v = v * 10 + (*q++ - '0');
            int after = !((*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z'));
            if (after && *p == 'P' && v >= 1 && v <= ids->n_person) {
                const char *nm = ids->person_name[v - 1];
                size_t l = strlen(nm);
                if (n + l + 1 >= cap) break;
                memcpy(out + n, nm, l);
                n += l;
                p = q;
                continue;
            }
            if (after && *p != 'P') {
                while (n && out[n - 1] == ' ' && (*q == ' ' || *q == ',' || *q == '.' || *q == ')')) n--;
                if (n && out[n - 1] == '(' && *q == ')') { n--; q++; }
                p = q;
                continue;
            }
        }
        out[n++] = *p++;
    }
    out[n] = '\0';
}

int oc_sum_check(const char *json, size_t len, const oc_sum_ids *ids, int rollup, oc_sum_buf *out,
                 int *dropped) {
    oc_json d;
    int drop = 0, kept = 0;
    if (dropped) *dropped = 0;
    if (!json || oc_json_parse(&d, json, len) != 0) return -1;
    if (d.t[0].type != JSMN_OBJECT) { oc_json_free(&d); return -1; }
    int nref = rollup ? ids->n_item : ids->n_msg;
    /* All the source texts, for the overview's number check. */
    const char **all = malloc((size_t)(nref ? nref : 1) * sizeof *all);
    if (!all) { oc_json_free(&d); return -1; }
    for (int i = 0; i < nref; i++) all[i] = rollup ? ids->item_text[i] : ids->msg_text[i];

    char raw[SUM_TEXT_MAX * 2 + 1], text[SUM_TEXT_MAX * 4 + 1];
    oc_sum_buf_puts(out, "{\"overview\":");
    if (oc_json_get_str(&d, 0, "overview", raw, sizeof raw) != 0) raw[0] = '\0';
    readable(raw, ids, text, sizeof text);
    if (!numbers_ok(text, all, nref)) text[0] = '\0';
    oc_sum_buf_json(out, text);
    for (int kind = 0; kind < 4; kind++) {
        oc_sum_buf_printf(out, ",\"%s\":[", KINDS[kind]);
        int arr = oc_json_get(&d, 0, KINDS[kind]);
        int first = 1;
        /* The texts kept so far, so a repeat is dropped (code merges what is
         * the same; the model is asked only for what is alike). */
        char kept_text[SUM_MAX_ITEMS * 2][SUM_TEXT_MAX * 4 + 1];
        int n_kept = 0;
        if (arr >= 0 && d.t[arr].type == JSMN_ARRAY) {
            int i = arr + 1;
            for (int k = 0; k < d.t[arr].size; k++, i = oc_json_skip(&d, i)) {
                if (d.t[i].type != JSMN_OBJECT) { drop++; continue; }
                if (oc_json_get_str(&d, i, kind == 1 ? "what" : "text", raw, sizeof raw) != 0 || !raw[0]) {
                    drop++;
                    continue;
                }
                readable(raw, ids, text, sizeof text);
                /* refs: every one ours; the message ids they stand for. */
                int ra = oc_json_get(&d, i, "refs");
                if (ra < 0 || d.t[ra].type != JSMN_ARRAY || d.t[ra].size == 0) { drop++; continue; }
                int64_t real[64];
                int nreal = 0, bad = 0;
                const char *srcs[SUM_MAX_REFS * 2];
                int nsrc = 0;
                int ri = ra + 1;
                for (int r = 0; r < d.t[ra].size; r++, ri = oc_json_skip(&d, ri)) {
                    char s[16];
                    int v = oc_json_str(&d, ri, s, sizeof s) == 0 ? short_id(s, rollup ? 'i' : 'm', nref) : 0;
                    if (!v) { bad = 1; break; }
                    if (nsrc < (int)(sizeof srcs / sizeof *srcs)) srcs[nsrc++] = all[v - 1];
                    if (rollup) {
                        for (int x = 0; x < ids->item_nrefs[v - 1] && nreal < 64; x++) {
                            int dup = 0;
                            for (int y = 0; y < nreal; y++) if (real[y] == ids->item_refs[v - 1][x]) dup = 1;
                            if (!dup) real[nreal++] = ids->item_refs[v - 1][x];
                        }
                    } else if (nreal < 64) {
                        int dup = 0;
                        for (int y = 0; y < nreal; y++) if (real[y] == ids->msg[v - 1]) dup = 1;
                        if (!dup) real[nreal++] = ids->msg[v - 1];
                    }
                }
                if (bad || !numbers_ok(text, srcs, nsrc)) { drop++; continue; }
                int repeat = 0;
                for (int x = 0; x < n_kept && !repeat; x++) if (!strcasecmp(kept_text[x], text)) repeat = 1;
                if (repeat) { drop++; continue; }
                if (n_kept < SUM_MAX_ITEMS * 2) snprintf(kept_text[n_kept++], sizeof kept_text[0], "%s", text);
                /* people */
                int64_t who = 0, by[4];
                int nby = 0;
                if (kind == 1) {
                    char s[16] = "";
                    if (oc_json_get_str(&d, i, "who", s, sizeof s) != 0) { drop++; continue; }
                    if (strcmp(s, "team") != 0) {
                        int p = short_id(s, 'P', ids->n_person);
                        if (!p) { drop++; continue; }
                        who = ids->person[p - 1];
                    }
                }
                if (kind == 0) {
                    int pa = oc_json_get(&d, i, "by");
                    if (pa >= 0 && d.t[pa].type == JSMN_ARRAY) {
                        int pi = pa + 1;
                        for (int r = 0; r < d.t[pa].size; r++, pi = oc_json_skip(&d, pi)) {
                            char s[16];
                            int p = oc_json_str(&d, pi, s, sizeof s) == 0 ? short_id(s, 'P', ids->n_person) : 0;
                            if (!p) { bad = 1; break; }
                            if (nby < 4) by[nby++] = ids->person[p - 1];
                        }
                    }
                    if (bad) { drop++; continue; }
                }
                char st[16] = "";
                oc_json_get_str(&d, i, "status", st, sizeof st);
                /* Write the kept item with real ids. */
                oc_sum_buf_puts(out, first ? "{" : ",{");
                first = 0;
                oc_sum_buf_puts(out, kind == 1 ? "\"who\":" : "\"text\":");
                if (kind == 1) {
                    oc_sum_buf_printf(out, "%lld,\"what\":", (long long)who);
                }
                oc_sum_buf_json(out, text);
                if (kind == 0) {
                    oc_sum_buf_puts(out, ",\"by\":[");
                    for (int x = 0; x < nby; x++) oc_sum_buf_printf(out, "%s%lld", x ? "," : "", (long long)by[x]);
                    oc_sum_buf_puts(out, "]");
                }
                oc_sum_buf_puts(out, ",\"refs\":[");
                for (int x = 0; x < nreal; x++) oc_sum_buf_printf(out, "%s%lld", x ? "," : "", (long long)real[x]);
                oc_sum_buf_puts(out, "]");
                if (kind == 1) oc_sum_buf_printf(out, ",\"status\":\"%s\"", strcmp(st, "done") ? "open" : "done");
                if (kind == 2) oc_sum_buf_printf(out, ",\"status\":\"%s\"", strcmp(st, "resolved") ? "open" : "resolved");
                oc_sum_buf_puts(out, "}");
                kept++;
            }
        }
        oc_sum_buf_puts(out, "]");
    }
    oc_sum_buf_puts(out, "}");
    free(all);
    oc_json_free(&d);
    if (dropped) *dropped = drop;
    return out->oom ? -1 : kept;
}
