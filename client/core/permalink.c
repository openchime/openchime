/* client/core/permalink.c -- see permalink.h. */
#include "permalink.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* A decimal id after `tag` ("/c/", "/m/", "/t/") at *p: 1 with *p moved past it,
 * 0 when the tag is not there; -1 when it is there with no number, or with 0. */
static int read_id(const char **p, const char *tag, uint64_t *out) {
    size_t n = strlen(tag);
    if (strncmp(*p, tag, n) != 0) return 0;
    const char *s = *p + n;
    if (!isdigit((unsigned char)*s)) return -1;
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 10);
    if (!end || end == s || v == 0) return -1;
    *out = (uint64_t)v;
    *p = end;
    return 1;
}

/* An audience id as one is minted: `ws_` and base64url characters (AUTH.md §3.3). */
static int audience_ok(const char *a, size_t n) {
    if (n < 6 || n > 128 || strncmp(a, "ws_", 3) != 0) return 0;
    for (size_t i = 3; i < n; i++) {
        unsigned char c = (unsigned char)a[i];
        if (!(isalnum(c) || c == '-' || c == '_')) return 0;
    }
    return 1;
}

int oc_permalink_parse(const char *text, oc_permalink *out) {
    if (!text || !out) return 0;
    memset(out, 0, sizeof *out);
    const char *p = text;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (strncmp(p, "openchime://", 12) != 0) return 0;
    p += 12;

    if (!strncmp(p, "w/", 2)) {
        p += 2;
        size_t n = 0;
        while (p[n] && p[n] != '/') n++;
        if (!audience_ok(p, n) || n >= sizeof out->where) return 0;
        memcpy(out->where, p, n);
        out->where[n] = '\0';
        out->kind = OC_PERMALINK_AUDIENCE;
        p += n;
    } else {
        size_t n = 0;
        while (p[n] && p[n] != '/' && n + 1 < sizeof out->where) n++;
        memcpy(out->where, p, n);
        out->where[n] = '\0';
        out->kind = OC_PERMALINK_HOST;
        p += n;
    }

    if (read_id(&p, "/c/", &out->channel) != 1) return 0;
    if (read_id(&p, "/m/", &out->message) < 0) return 0;
    /* A thread is the email link's alone: a copied permalink has never had one. */
    if (out->kind == OC_PERMALINK_AUDIENCE && read_id(&p, "/t/", &out->thread) < 0) return 0;
    /* The host form kept its old leniency: anything after its ids is ignored,
     * as it always was. The email form is exact. */
    if (out->kind == OC_PERMALINK_AUDIENCE) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (*p) return 0;
    }
    return 1;
}
