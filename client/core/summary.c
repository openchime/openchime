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

/* The display name the body's people map gives `uid`, on the heap. */
static char *person(const doc *d, int people, uint64_t uid) {
    char key[24];
    snprintf(key, sizeof key, "%llu", (unsigned long long)uid);
    char *s = str(d, get(d, people, key));
    if (!s) {
        s = malloc(32);
        if (s) snprintf(s, 32, "user %llu", (unsigned long long)uid);
    }
    return s;
}

void oc_summary_view_free(oc_summary_view *v) {
    if (!v) return;
    free(v->overview);
    free(v->refs);
    for (size_t i = 0; i < v->n_items; i++) {
        free(v->items[i].text);
        free(v->items[i].who);
        free(v->items[i].refs);
    }
    free(v->items);
    memset(v, 0, sizeof *v);
}

int oc_summary_view_parse(const char *json, size_t len, oc_summary_view *out) {
    static const char *const KINDS[4] = { "decisions", "actions", "problems", "facts" };
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
        if (n == JSMN_ERROR_NOMEM && cap < 65536) { cap *= 4; continue; }
        if (n <= 0) { free(d.t); return -1; }
        d.n = n;
        break;
    }
    int rc = -1;
    int s = get(&d, 0, "summary"), people = get(&d, 0, "people");
    if (s < 0 || d.t[s].type != JSMN_OBJECT) goto out;
    out->overview = str(&d, get(&d, s, "overview"));
    if (!out->overview && !(out->overview = calloc(1, 1))) goto out;
    if (ids(&d, get(&d, s, "refs"), &out->refs, &out->n_refs) != 0) goto out;
    size_t total = 0;
    for (int k = 0; k < 4; k++) {
        int arr = get(&d, s, KINDS[k]);
        if (arr >= 0 && d.t[arr].type == JSMN_ARRAY) total += (size_t)d.t[arr].size;
    }
    if (total && !(out->items = calloc(total, sizeof *out->items))) goto out;
    for (int k = 0; k < 4; k++) {
        int arr = get(&d, s, KINDS[k]);
        if (arr < 0 || d.t[arr].type != JSMN_ARRAY) continue;
        int i = arr + 1;
        for (int e = 0; e < d.t[arr].size; e++, i = skip(&d, i)) {
            if (d.t[i].type != JSMN_OBJECT) continue;
            oc_summary_item *it = &out->items[out->n_items];
            it->kind = (uint8_t)k;
            it->text = str(&d, get(&d, i, k == OC_SUMI_ACTION ? "what" : "text"));
            if (!it->text) continue;
            out->n_items++;
            uint64_t who;
            if (k == OC_SUMI_ACTION && u64(&d, get(&d, i, "who"), &who) == 0) {
                it->who = who ? person(&d, people, who) : strdup("Team");
                if (!it->who) goto out;
            }
            char *st = str(&d, get(&d, i, "status"));
            if (st) {
                snprintf(it->status, sizeof it->status, "%s", st);
                free(st);
            }
            if (ids(&d, get(&d, i, "refs"), &it->refs, &it->n_refs) != 0) goto out;
        }
    }
    rc = 0;
out:
    free(d.t);
    if (rc != 0) oc_summary_view_free(out);
    return rc;
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

void oc_summary_span_label(uint8_t scope, uint64_t start_ms, uint64_t end_ms, char *out, size_t cap) {
    if (scope == OC_SUM_UNREAD) { snprintf(out, cap, "Unread"); return; }
    if (scope == OC_SUM_WEEK) { snprintf(out, cap, "Last 7 days"); return; }
    if (scope == OC_SUM_DAILY) { snprintf(out, cap, "Since yesterday"); return; }
    char a[16] = "", b[16] = "";
    time_t ts = (time_t)(start_ms / 1000), te = (time_t)((end_ms ? end_ms - 1 : 0) / 1000);
    struct tm tm;
    if (oc_localtime_r(&ts, &tm)) strftime(a, sizeof a, "%Y-%m-%d", &tm);
    if (oc_localtime_r(&te, &tm)) strftime(b, sizeof b, "%Y-%m-%d", &tm);
    if (!strcmp(a, b)) snprintf(out, cap, "%s", a);
    else snprintf(out, cap, "%s to %s", a, b);
}

const char *oc_summary_kind_heading(uint8_t kind) {
    static const char *const H[4] = { "Decisions", "Actions", "Problems", "Facts" };
    return kind < 4 ? H[kind] : "";
}
