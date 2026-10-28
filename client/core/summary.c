/* A channel or DM summary as a client reads it (summary.h). */
#include "summary.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "oc_port.h"     /* oc_localtime_r */
#include "protocol.h"    /* OC_SUM_* */

/* jsmn's implementation, private to this file: the daemon carries its own copy
 * (daemon/jwt.c), and the test program links both, so its two public functions
 * take names of their own here. */
#define jsmn_init  oc_summary_jsmn_init
#define jsmn_parse oc_summary_jsmn_parse
#include "jsmn.h"

typedef struct {
    const char *js;
    jsmntok_t  *t;
    int         n;
} doc;

static int skip(const doc *d, int i) {
    int end = i + 1;
    if (d->t[i].type == JSMN_OBJECT || d->t[i].type == JSMN_ARRAY) {
        int kids = d->t[i].size * (d->t[i].type == JSMN_OBJECT ? 2 : 1);
        for (int k = 0; k < kids && end < d->n; k++) end = skip(d, end);
    }
    return end;
}

/* The value of `key` in the object at `obj`, or -1. */
static int get(const doc *d, int obj, const char *key) {
    if (obj < 0 || obj >= d->n || d->t[obj].type != JSMN_OBJECT) return -1;
    size_t kl = strlen(key);
    int i = obj + 1;
    for (int k = 0; k < d->t[obj].size && i + 1 < d->n; k++) {
        const jsmntok_t *kt = &d->t[i];
        if (kt->type == JSMN_STRING && (size_t)(kt->end - kt->start) == kl && !memcmp(d->js + kt->start, key, kl))
            return i + 1;
        i = skip(d, i + 1);
    }
    return -1;
}

static int hex4(const char *s, unsigned *out) {
    unsigned v = 0;
    for (int k = 0; k < 4; k++) {
        char c = s[k];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return 0;
    }
    *out = v;
    return 1;
}

/* The string at token `i`, unescaped, on the heap; NULL if it is not one. */
static char *str(const doc *d, int i) {
    if (i < 0 || i >= d->n || d->t[i].type != JSMN_STRING) return NULL;
    const char *s = d->js + d->t[i].start;
    size_t n = (size_t)(d->t[i].end - d->t[i].start);
    char *o = malloc(n + 1);   /* unescaping never grows a string */
    if (!o) return NULL;
    size_t w = 0;
    for (size_t k = 0; k < n; k++) {
        if (s[k] != '\\' || k + 1 >= n) { o[w++] = s[k]; continue; }
        char e = s[++k];
        unsigned cp;
        switch (e) {
        case 'n': o[w++] = '\n'; continue;
        case 't': o[w++] = '\t'; continue;
        case 'r': o[w++] = '\r'; continue;
        case 'b': o[w++] = '\b'; continue;
        case 'f': o[w++] = '\f'; continue;
        case 'u':
            if (k + 4 >= n || !hex4(s + k + 1, &cp)) { free(o); return NULL; }
            k += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                unsigned lo;
                if (k + 6 >= n || s[k + 1] != '\\' || s[k + 2] != 'u' || !hex4(s + k + 3, &lo) ||
                    lo < 0xDC00 || lo > 0xDFFF) { free(o); return NULL; }
                cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                k += 6;
            }
            if (cp < 0x80) o[w++] = (char)cp;
            else if (cp < 0x800) { o[w++] = (char)(0xC0 | (cp >> 6)); o[w++] = (char)(0x80 | (cp & 0x3F)); }
            else if (cp < 0x10000) {
                o[w++] = (char)(0xE0 | (cp >> 12));
                o[w++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                o[w++] = (char)(0x80 | (cp & 0x3F));
            } else {
                o[w++] = (char)(0xF0 | (cp >> 18));
                o[w++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                o[w++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                o[w++] = (char)(0x80 | (cp & 0x3F));
            }
            continue;
        default: o[w++] = e; continue;   /* \" \\ \/ */
        }
    }
    o[w] = '\0';
    return o;
}

/* A whole non-negative number at token `i`. */
static int u64(const doc *d, int i, uint64_t *out) {
    if (i < 0 || i >= d->n || d->t[i].type != JSMN_PRIMITIVE) return -1;
    uint64_t v = 0;
    int digits = 0;
    for (int k = d->t[i].start; k < d->t[i].end; k++) {
        char c = d->js[k];
        if (c < '0' || c > '9') return -1;
        v = v * 10 + (uint64_t)(c - '0');
        digits++;
    }
    if (!digits) return -1;
    *out = v;
    return 0;
}

/* The ids in the array at token `arr`. */
static int ids(const doc *d, int arr, uint64_t **out, size_t *n) {
    *out = NULL;
    *n = 0;
    if (arr < 0 || d->t[arr].type != JSMN_ARRAY || !d->t[arr].size) return 0;
    *out = malloc((size_t)d->t[arr].size * sizeof **out);
    if (!*out) return -1;
    int i = arr + 1;
    for (int k = 0; k < d->t[arr].size; k++, i = skip(d, i))
        if (u64(d, i, &(*out)[*n]) == 0) (*n)++;
    return 0;
}

void oc_summary_view_free(oc_summary_view *v) {
    if (!v) return;
    free(v->overview);
    free(v->refs);
    for (size_t i = 0; i < v->n_attention; i++) { free(v->attention[i].text); free(v->attention[i].refs); }
    free(v->attention);
    for (size_t i = 0; i < v->n_more; i++) free(v->more[i]);
    free(v->more);
    for (size_t i = 0; i < v->n_topics; i++) {
        oc_summary_topic *t = &v->topics[i];
        free(t->title);
        free(t->text);
        free(t->refs);
        for (size_t k = 0; k < t->n_people; k++) free(t->people[k]);
        free(t->people);
        for (size_t k = 0; k < t->n_details; k++) {
            free(t->details[k].text);
            free(t->details[k].refs);
        }
        free(t->details);
    }
    free(v->topics);
    for (size_t i = 0; i < v->n_posters; i++) free(v->posters[i].name);
    free(v->posters);
    for (size_t i = 0; i < v->n_sources; i++) {
        free(v->sources[i].author);
        free(v->sources[i].text);
    }
    free(v->sources);
    memset(v, 0, sizeof *v);
}

/* The string at `i`, or "" when there is none; NULL only when out of memory. */
static char *str_or_empty(const doc *d, int i) {
    char *s = str(d, i);
    return s ? s : calloc(1, 1);
}

int oc_summary_view_parse(const char *json, size_t len, oc_summary_view *out) {
    memset(out, 0, sizeof *out);
    if (!json) return -1;
    doc d = { json, NULL, 0 };
    unsigned cap = 256;
    for (;;) {
        jsmntok_t *t = realloc(d.t, cap * sizeof *t);
        if (!t) { free(d.t); return -1; }
        d.t = t;
        jsmn_parser p;
        jsmn_init(&p);
        int n = jsmn_parse(&p, json, len, d.t, cap);
        if (n == JSMN_ERROR_NOMEM && cap < (1u << 20)) { cap *= 4; continue; }
        if (n <= 0) { free(d.t); return -1; }
        d.n = n;
        break;
    }
    int rc = -1;
    int s = get(&d, 0, "summary");
    int arr = get(&d, s, "topics"), ov = get(&d, s, "overview");
    if (s < 0 || arr < 0 || d.t[arr].type != JSMN_ARRAY || ov < 0 || d.t[ov].type != JSMN_OBJECT) goto out;
    if (!(out->overview = str_or_empty(&d, get(&d, ov, "text")))) goto out;
    if (ids(&d, get(&d, ov, "refs"), &out->refs, &out->n_refs) != 0) goto out;
    if (d.t[arr].size && !(out->topics = calloc((size_t)d.t[arr].size, sizeof *out->topics))) goto out;
    int i = arr + 1;
    for (int k = 0; k < d.t[arr].size; k++, i = skip(&d, i)) {
        if (d.t[i].type != JSMN_OBJECT) continue;
        oc_summary_topic *t = &out->topics[out->n_topics++];
        if (!(t->title = str_or_empty(&d, get(&d, i, "title"))) || !(t->text = str_or_empty(&d, get(&d, i, "text"))))
            goto out;
        if (ids(&d, get(&d, i, "refs"), &t->refs, &t->n_refs) != 0) goto out;
        u64(&d, get(&d, i, "count"), &t->count);
        int pe = get(&d, i, "people");
        if (pe >= 0 && d.t[pe].type == JSMN_ARRAY && d.t[pe].size) {
            if (!(t->people = calloc((size_t)d.t[pe].size, sizeof *t->people))) goto out;
            int j = pe + 1;
            for (int e = 0; e < d.t[pe].size; e++, j = skip(&d, j)) {
                char *nm = str(&d, j);
                if (nm) t->people[t->n_people++] = nm;
            }
        }
        int det = get(&d, i, "details");
        if (det < 0 || d.t[det].type != JSMN_ARRAY || !d.t[det].size) continue;
        if (!(t->details = calloc((size_t)d.t[det].size, sizeof *t->details))) goto out;
        int j = det + 1;
        for (int e = 0; e < d.t[det].size; e++, j = skip(&d, j)) {
            if (d.t[j].type != JSMN_OBJECT) continue;
            char *dt = str(&d, get(&d, j, "text"));
            if (!dt) continue;
            oc_summary_detail *x = &t->details[t->n_details++];
            x->text = dt;
            if (ids(&d, get(&d, j, "refs"), &x->refs, &x->n_refs) != 0) goto out;
        }
    }
    int at = get(&d, s, "attention");
    if (at >= 0 && d.t[at].type == JSMN_ARRAY && d.t[at].size) {
        if (!(out->attention = calloc((size_t)d.t[at].size, sizeof *out->attention))) goto out;
        int j = at + 1;
        for (int e = 0; e < d.t[at].size; e++, j = skip(&d, j)) {
            char *tx = str(&d, get(&d, j, "text"));
            if (!tx) continue;
            oc_summary_attention *x = &out->attention[out->n_attention++];
            x->text = tx;
            char *kd = str(&d, get(&d, j, "kind"));
            x->question = kd && !strcmp(kd, "question");
            free(kd);
            if (ids(&d, get(&d, j, "refs"), &x->refs, &x->n_refs) != 0) goto out;
        }
    }
    int mo = get(&d, s, "more");
    if (mo >= 0 && d.t[mo].type == JSMN_ARRAY && d.t[mo].size) {
        if (!(out->more = calloc((size_t)d.t[mo].size, sizeof *out->more))) goto out;
        int j = mo + 1;
        for (int e = 0; e < d.t[mo].size; e++, j = skip(&d, j)) {
            char *tt = str(&d, j);
            if (tt) out->more[out->n_more++] = tt;
        }
    }
    u64(&d, get(&d, 0, "count"), &out->count);
    int ps = get(&d, 0, "posters");
    if (ps >= 0 && d.t[ps].type == JSMN_ARRAY && d.t[ps].size) {
        if (!(out->posters = calloc((size_t)d.t[ps].size, sizeof *out->posters))) goto out;
        int j = ps + 1;
        for (int e = 0; e < d.t[ps].size; e++, j = skip(&d, j)) {
            char *nm = str(&d, get(&d, j, "name"));
            if (!nm) continue;
            oc_summary_poster *p = &out->posters[out->n_posters++];
            p->name = nm;
            u64(&d, get(&d, j, "id"), &p->id);
        }
    }
    int so = get(&d, 0, "sources");
    if (so >= 0 && d.t[so].type == JSMN_OBJECT && d.t[so].size) {
        if (!(out->sources = calloc((size_t)d.t[so].size, sizeof *out->sources))) goto out;
        int j = so + 1;
        for (int e = 0; e < d.t[so].size && j + 1 < d.n; e++) {
            char *key = str(&d, j);
            int val = j + 1;
            uint64_t id = key ? strtoull(key, NULL, 10) : 0;
            free(key);
            if (id && d.t[val].type == JSMN_OBJECT) {
                char *au = str(&d, get(&d, val, "author")), *tx = str(&d, get(&d, val, "text"));
                if (au && tx) {
                    oc_summary_source *x = &out->sources[out->n_sources++];
                    x->id = id;
                    x->author = au;
                    x->text = tx;
                    u64(&d, get(&d, val, "author_id"), &x->author_id);
                    u64(&d, get(&d, val, "at"), &x->at);
                    u64(&d, get(&d, val, "parent"), &x->parent);
                } else {
                    free(au);
                    free(tx);
                }
            }
            j = skip(&d, val);
        }
    }
    rc = 0;
out:
    free(d.t);
    if (rc != 0) oc_summary_view_free(out);
    return rc;
}

const oc_summary_source *oc_summary_source_of(const oc_summary_view *v, uint64_t id) {
    for (size_t i = 0; v && i < v->n_sources; i++)
        if (v->sources[i].id == id) return &v->sources[i];
    return NULL;
}

/* One local date "YYYY-MM-DD" at `*p`: the time its day starts; *p moves past it. */
static int read_date(const char **p, time_t *out) {
    int y, mo, d, n = 0;
    while (**p == ' ') (*p)++;
    if (sscanf(*p, "%4d-%2d-%2d%n", &y, &mo, &d, &n) != 3 || mo < 1 || mo > 12 || d < 1 || d > 31) return -1;
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    if (t == (time_t)-1 || tm.tm_mday != d) return -1;   /* 2026-02-30 is not a day */
    *out = t;
    *p += n;
    return 0;
}

int oc_summary_date_parse(const char *text, uint64_t *day_ms) {
    if (!text) return -1;
    const char *p = text;
    time_t t;
    if (read_date(&p, &t) != 0) return -1;
    while (*p == ' ') p++;
    if (*p) return -1;
    *day_ms = (uint64_t)t * 1000u;
    return 0;
}

int oc_summary_range_parse(const char *text, uint64_t *start_ms, uint64_t *end_ms) {
    if (!text) return -1;
    const char *p = text;
    time_t a, b;
    if (read_date(&p, &a) != 0) return -1;
    while (*p == ' ' || *p == '-') p++;
    if (!strncmp(p, "to ", 3)) p += 3;
    if (read_date(&p, &b) != 0) return -1;
    while (*p == ' ') p++;
    if (*p || b < a) return -1;
    /* The day after the second, by the calendar (a day is not always 24 hours). */
    struct tm tm;
    if (!oc_localtime_r(&b, &tm)) return -1;
    tm.tm_mday += 1;
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
    tm.tm_isdst = -1;
    time_t e = mktime(&tm);
    if (e == (time_t)-1) return -1;
    *start_ms = (uint64_t)a * 1000u;
    *end_ms = (uint64_t)e * 1000u;
    return 0;
}

const char *oc_summary_scope_name(uint8_t scope) {
    switch (scope) {
    case OC_SUM_UNREAD: return "Unread";
    case OC_SUM_TODAY:  return "Today";
    case OC_SUM_DAILY:  return "Since yesterday";
    case OC_SUM_WEEK:   return "Last 7 days";
    default:            return "Custom date range";
    }
}

static const char *const MON[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

/* "Oct 5" for the local day holding `ms`; "" when it cannot be had. */
static void day_name(uint64_t ms, char *out, size_t cap) {
    time_t t = (time_t)(ms / 1000);
    struct tm tm;
    if (!oc_localtime_r(&t, &tm)) { snprintf(out, cap, "%s", ""); return; }
    snprintf(out, cap, "%s %d", MON[tm.tm_mon], tm.tm_mday);
}

void oc_summary_dates(uint64_t start_ms, uint64_t end_ms, char *out, size_t cap) {
    char a[16], b[16];
    day_name(start_ms, a, sizeof a);
    day_name(end_ms > start_ms ? end_ms - 1 : start_ms, b, sizeof b);
    if (!strcmp(a, b)) snprintf(out, cap, "%s", a);
    else snprintf(out, cap, "%s - %s", a, b);
}

void oc_summary_wait_title(uint8_t scope, uint64_t start_ms, uint64_t end_ms, const char *where,
                           char *out, size_t cap) {
    char span[40];
    switch (scope) {
    case OC_SUM_UNREAD: snprintf(span, sizeof span, "unreads"); break;
    case OC_SUM_TODAY:  snprintf(span, sizeof span, "today"); break;
    case OC_SUM_DAILY:  snprintf(span, sizeof span, "since yesterday"); break;
    case OC_SUM_WEEK:   snprintf(span, sizeof span, "the last 7 days"); break;
    default:            oc_summary_dates(start_ms, end_ms, span, sizeof span); break;
    }
    snprintf(out, cap, "Summarizing %s in %s", span, where ? where : "this conversation");
}

void oc_summary_notice_title(uint8_t status, uint8_t scope, uint64_t start_ms, uint64_t end_ms, const char *where,
                             char *out, size_t cap) {
    char span[40];
    if (scope == OC_SUM_RANGE && end_ms > start_ms) oc_summary_dates(start_ms, end_ms, span, sizeof span);
    else snprintf(span, sizeof span, "%s", oc_summary_scope_name(scope));
    if (!where || !*where) where = "this conversation";
    if (status == OC_SUM_OK) snprintf(out, cap, "Your summary of %s (%s) is ready", where, span);
    else snprintf(out, cap, "Couldn't summarize %s (%s)", where, span);
}

const char *oc_summary_fail_title(uint8_t status) {
    switch (status) {
    case OC_SUM_UNAVAILABLE: return "Summaries aren't available right now";
    case OC_SUM_FORBIDDEN:   return "You can't summarize this conversation";
    case OC_SUM_CANCELLED:   return "Summary canceled";
    default:                 return "This summary couldn't be made";
    }
}

void oc_summary_wait_text(int32_t position, char *out, size_t cap) {
    if (position == (int32_t)OC_SUM_POS_STARTING)
        snprintf(out, cap, "Getting ready to summarize\xE2\x80\xA6 Yours starts once the summarizer is up.");
    else if (position < 0)
        snprintf(out, cap, "Sifting through messages…");
    else if (position == 0)
        snprintf(out, cap, "Summarizing now… This can take a few minutes.");
    else
        snprintf(out, cap, "Waiting: %d request%s ahead of yours.", (int)position, position == 1 ? "" : "s");
}

void oc_summary_posters_text(const oc_summary_view *v, char *out, size_t cap) {
    size_t w = 0, n = v ? v->n_posters : 0;
    if (cap) out[0] = '\0';
    for (size_t i = 0; i < n && w < cap; i++) {
        const char *sep = i == 0 ? "" : n == 2 ? " and " : i + 1 == n ? ", and " : ", ";
        int k = snprintf(out + w, cap - w, "%s%s", sep, v->posters[i].name);
        if (k < 0) break;
        w += (size_t)k;
    }
}

void oc_summary_when(uint64_t at_ms, uint64_t now_ms, int h24, char *out, size_t cap) {
    time_t at = (time_t)(at_ms / 1000), now = (time_t)(now_ms / 1000);
    struct tm a, n;
    if (!oc_localtime_r(&at, &a) || !oc_localtime_r(&now, &n)) { snprintf(out, cap, "%s", ""); return; }
    char clock[16];
    if (h24) snprintf(clock, sizeof clock, "%02d:%02d", a.tm_hour, a.tm_min);
    else snprintf(clock, sizeof clock, "%d:%02d %s", a.tm_hour % 12 ? a.tm_hour % 12 : 12, a.tm_min,
                  a.tm_hour < 12 ? "AM" : "PM");
    /* Yesterday: the day before today's, by the calendar. */
    struct tm y = n;
    y.tm_mday -= 1;
    y.tm_hour = 12;
    y.tm_isdst = -1;
    time_t yt = mktime(&y);
    struct tm yd;
    int have_y = yt != (time_t)-1 && oc_localtime_r(&yt, &yd);
    if (a.tm_year == n.tm_year && a.tm_yday == n.tm_yday)
        snprintf(out, cap, "Today at %s", clock);
    else if (have_y && a.tm_year == yd.tm_year && a.tm_yday == yd.tm_yday)
        snprintf(out, cap, "Yesterday at %s", clock);
    else
        snprintf(out, cap, "%s %d at %s", MON[a.tm_mon], a.tm_mday, clock);
}

static int word_byte(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 0x80;
}

size_t oc_summary_mentions(const oc_summary_view *v, const char *text, oc_summary_mention *out, size_t max) {
    size_t n = 0, tl = text ? strlen(text) : 0;
    for (size_t at = 0; at < tl && n < max; ) {
        size_t best = 0, who = 0;
        if (at == 0 || !word_byte((unsigned char)text[at - 1])) {
            for (size_t p = 0; v && p < v->n_posters; p++) {
                size_t nl = strlen(v->posters[p].name);
                if (nl > best && nl <= tl - at && !memcmp(text + at, v->posters[p].name, nl) &&
                    !word_byte((unsigned char)text[at + nl])) {
                    best = nl;
                    who = p;
                }
            }
        }
        if (!best) { at++; continue; }
        out[n].start = at;
        out[n].len = best;
        out[n].poster = who;
        n++;
        at += best;
    }
    return n;
}

/* The local day `y-m-d` (normalized, so day 0 is the month before's last) as ms
 * of its start. */
static uint64_t local_day(int y, int m, int d) {
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    tm.tm_year = y - 1900;
    tm.tm_mon = m - 1;
    tm.tm_mday = d;
    tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    return t == (time_t)-1 ? 0 : (uint64_t)t * 1000u;
}

void oc_sumcal_init(oc_sumcal *c, uint64_t now_ms) {
    memset(c, 0, sizeof *c);
    time_t t = (time_t)(now_ms / 1000);
    struct tm tm;
    if (!oc_localtime_r(&t, &tm)) return;
    c->year = tm.tm_year + 1900;
    c->month = tm.tm_mon + 1;
    c->today = local_day(c->year, c->month, tm.tm_mday);
}

void oc_sumcal_shift(oc_sumcal *c, int delta) {
    int mm = c->year * 12 + (c->month - 1) + delta;
    c->year = mm / 12;
    c->month = mm % 12 + 1;
}

void oc_sumcal_month(const oc_sumcal *c, int which, int *year, int *month, uint64_t cells[42]) {
    int mm = c->year * 12 + (c->month - 1) + which;
    int y = mm / 12, m = mm % 12 + 1;
    *year = y;
    *month = m;
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    tm.tm_year = y - 1900;
    tm.tm_mon = m - 1;
    tm.tm_mday = 1;
    tm.tm_hour = 12;
    tm.tm_isdst = -1;
    mktime(&tm);
    int first = tm.tm_wday;
    /* The days in it: the day before the next month's first. */
    struct tm nx;
    memset(&nx, 0, sizeof nx);
    nx.tm_year = y - 1900;
    nx.tm_mon = m;
    nx.tm_mday = 0;
    nx.tm_hour = 12;
    nx.tm_isdst = -1;
    mktime(&nx);
    int days = nx.tm_mday;
    for (int i = 0; i < 42; i++) {
        int d = i - first + 1;
        cells[i] = d >= 1 && d <= days ? local_day(y, m, d) : 0;
    }
}

void oc_sumcal_pick(oc_sumcal *c, uint64_t day) {
    if (!day || (c->today && day > c->today)) return;
    if (c->start && !c->end && day >= c->start) c->end = day;
    else { c->start = day; c->end = 0; }
}

int oc_sumcal_range(const oc_sumcal *c, uint64_t *start_ms, uint64_t *end_ms) {
    if (!c->start || !c->end) return -1;
    time_t t = (time_t)(c->end / 1000);
    struct tm tm;
    if (!oc_localtime_r(&t, &tm)) return -1;
    *start_ms = c->start;
    *end_ms = local_day(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday + 1);
    return *end_ms ? 0 : -1;
}

void oc_summary_day_text(uint64_t day_ms, char *out, size_t cap) {
    time_t t = (time_t)(day_ms / 1000);
    struct tm tm;
    if (!day_ms || !oc_localtime_r(&t, &tm)) { snprintf(out, cap, "%s", ""); return; }
    snprintf(out, cap, "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
}
