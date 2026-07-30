/*
 * Search query parsing (REQ-081, WIN-39). See searchq.h.
 */

#include "searchq.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static int sq_lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* Case-insensitive prefix test; returns the length matched, else 0. */
static size_t sq_prefix(const char *s, const char *pfx) {
    size_t i = 0;
    while (pfx[i]) {
        if (!s[i] || sq_lower((unsigned char)s[i]) != sq_lower((unsigned char)pfx[i])) return 0;
        i++;
    }
    return i;
}

/* Copy one token's value into `dst`, stopping at whitespace. Quotes are honoured so
 * `in:"design docs"` works — a channel cannot contain a space today, but a display
 * name can, and the parser should not be the thing that forbids it. */
static const char *sq_take(const char *p, char *dst, size_t cap) {
    size_t n = 0;
    if (*p == '"') {
        p++;
        while (*p && *p != '"') { if (n + 1 < cap) dst[n++] = *p; p++; }
        if (*p == '"') p++;
    } else {
        while (*p && *p != ' ' && *p != '\t') { if (n + 1 < cap) dst[n++] = *p; p++; }
    }
    dst[n] = '\0';
    return p;
}

void oc_searchq_parse(const char *q, oc_searchq *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!q) return;

    size_t tn = 0;
    const char *p = q;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        size_t k;
        if ((k = sq_prefix(p, "from:"))) {
            p = sq_take(p + k, out->from, sizeof out->from);
            /* A bare "from:" is not a filter — it constrains nothing, so leaving it
             * counted would make the client claim it understood something. */
            if (out->from[0]) out->n_filters++;
            continue;
        }
        if ((k = sq_prefix(p, "in:"))) {
            /* "#design" and "design" mean the same channel; strip the sigil so the
             * caller does not have to know which the user typed. */
            p = sq_take(p + k, out->in, sizeof out->in);
            if (out->in[0] == '#') memmove(out->in, out->in + 1, strlen(out->in));
            if (out->in[0]) out->n_filters++;
            continue;
        }
        if ((k = sq_prefix(p, "has:"))) {
            char v[24];
            p = sq_take(p + k, v, sizeof v);
            unsigned bit = 0;
            if      (!strcmp(v, "file") || !strcmp(v, "files")) bit = OC_SQ_HAS_FILE;
            else if (!strcmp(v, "link") || !strcmp(v, "links")) bit = OC_SQ_HAS_LINK;
            else if (!strcmp(v, "image") || !strcmp(v, "images") ||
                     !strcmp(v, "pic") || !strcmp(v, "pics"))   bit = OC_SQ_HAS_IMAGE;
            if (bit) { out->has |= bit; out->n_filters++; }
            else {
                /* An unknown has: value is kept as TEXT rather than dropped: silently
                 * ignoring part of a query is how a search lies about its results. */
                if (tn && tn + 1 < sizeof out->text) out->text[tn++] = ' ';
                int w = snprintf(out->text + tn, sizeof out->text - tn, "has:%s", v);
                if (w > 0) tn += (size_t)w;
            }
            continue;
        }
        if ((k = sq_prefix(p, "before:"))) {
            p = sq_take(p + k, out->before, sizeof out->before);
            if (out->before[0]) out->n_filters++;
            continue;
        }
        if ((k = sq_prefix(p, "after:"))) {
            p = sq_take(p + k, out->after, sizeof out->after);
            if (out->after[0]) out->n_filters++;
            continue;
        }

        /* Not an operator: it is search text. */
        {
            char word[OC_SQ_TEXT_MAX];
            p = sq_take(p, word, sizeof word);
            if (word[0]) {
                if (tn && tn + 1 < sizeof out->text) out->text[tn++] = ' ';
                int w = snprintf(out->text + tn, sizeof out->text - tn, "%s", word);
                if (w > 0) tn += (size_t)w;
            }
        }
    }
    out->text[tn < sizeof out->text ? tn : sizeof out->text - 1] = '\0';
}

void oc_searchq_describe(const oc_searchq *sq, char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!sq) return;
    size_t n = 0;
    #define SQ_ADD(...) do { \
        if (n < cap) { int w = snprintf(out + n, cap - n, __VA_ARGS__); if (w > 0) n += (size_t)w; } \
    } while (0)
    if (sq->from[0])   SQ_ADD("%sfrom:%s",   n ? " " : "", sq->from);
    if (sq->in[0])     SQ_ADD("%sin:%s",     n ? " " : "", sq->in);
    if (sq->has & OC_SQ_HAS_FILE)  SQ_ADD("%shas:file",  n ? " " : "");
    if (sq->has & OC_SQ_HAS_LINK)  SQ_ADD("%shas:link",  n ? " " : "");
    if (sq->has & OC_SQ_HAS_IMAGE) SQ_ADD("%shas:image", n ? " " : "");
    if (sq->after[0])  SQ_ADD("%safter:%s",  n ? " " : "", sq->after);
    if (sq->before[0]) SQ_ADD("%sbefore:%s", n ? " " : "", sq->before);
    if (sq->text[0])   SQ_ADD("%s%s",        n ? " " : "", sq->text);
    #undef SQ_ADD
}

/* Local midnight for a YYYY-MM-DD, as epoch ms. mktime interprets the struct tm in
 * LOCAL time, which is exactly the intent. */
static uint64_t sq_day_ms(const char *ymd, int end) {
    int y = 0, mo = 0, d = 0;
    if (!ymd || sscanf(ymd, "%d-%d-%d", &y, &mo, &d) != 3) return 0;
    if (y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
    struct tm tv;
    memset(&tv, 0, sizeof tv);
    tv.tm_year = y - 1900; tv.tm_mon = mo - 1; tv.tm_mday = d;
    tv.tm_hour = end ? 23 : 0;
    tv.tm_min  = end ? 59 : 0;
    tv.tm_sec  = end ? 59 : 0;
    tv.tm_isdst = -1;                      /* let the library decide, DST included */
    time_t t = mktime(&tv);
    if (t == (time_t)-1) return 0;
    return (uint64_t)t * 1000ull + (end ? 999ull : 0ull);
}

uint64_t oc_day_start_ms(const char *ymd) { return sq_day_ms(ymd, 0); }
uint64_t oc_day_end_ms(const char *ymd)   { return sq_day_ms(ymd, 1); }
