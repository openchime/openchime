/* What a new password must be (pwpolicy.h). */
#include "pwpolicy.h"
#include "utf8proc.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* The list (scripts/build/pw_common.py): lowercase, sorted bytewise. */
extern const char *const oc_pw_common[];
extern const size_t      oc_pw_common_n;

int oc_pw_normalize(const char *pw, char *out, size_t cap) {
    if (!pw || !out || !cap) return -1;
    utf8proc_uint8_t *n = NULL;
    utf8proc_ssize_t len = utf8proc_map((const utf8proc_uint8_t *)pw, 0, &n,
                                        UTF8PROC_NULLTERM | UTF8PROC_STABLE | UTF8PROC_COMPOSE | UTF8PROC_COMPAT);
    if (len < 0 || !n) return -1;
    int bad = (size_t)len >= cap;
    for (utf8proc_ssize_t i = 0; !bad && i < len; i++)
        if (n[i] < 0x20 || n[i] == 0x7f) bad = 1;      /* a control character */
    if (!bad) memcpy(out, n, (size_t)len + 1);
    free(n);
    return bad ? -1 : 0;
}

static size_t chars(const char *s) {
    size_t n = 0;
    for (; *s; s++) if (((unsigned char)*s & 0xC0) != 0x80) n++;
    return n;
}

static int cmp(const void *k, const void *e) { return strcmp((const char *)k, *(const char *const *)e); }

/* `s` lowercased (ASCII letters only: the list and the names are compared as
 * typed otherwise), into `out`. */
static void lower(const char *s, char *out, size_t cap) {
    size_t i = 0;
    for (; s[i] && i + 1 < cap; i++) out[i] = (char)tolower((unsigned char)s[i]);
    out[i] = '\0';
}

/* Whether `pw` (lowercased) contains `word` (lowercased), for a word long
 * enough to mean something. */
static int contains(const char *pw, const char *word) {
    if (!word || strlen(word) < 4) return 0;
    char w[256];
    lower(word, w, sizeof w);
    return strstr(pw, w) != NULL;
}

const char *oc_pw_verdict_text(oc_pw_verdict v) {
    switch (v) {
    case OC_PW_OK:        return "";
    case OC_PW_BAD_CHARS: return "That password has characters it can't hold. Use letters, numbers, spaces and symbols.";
    case OC_PW_TOO_SHORT: return "Use at least 15 characters. A short phrase works well.";
    case OC_PW_TOO_LONG:  return "Use at most 128 characters.";
    case OC_PW_COMMON:    return "That password is too common. Choose one that isn't in lists of leaked passwords.";
    case OC_PW_PERSONAL:  return "Don't build the password on your username, the workspace's name or OpenChime.";
    }
    return "";
}

oc_pw_verdict oc_pw_check(const char *pw, const char *username, const char *workspace,
                          char *out, size_t cap) {
    if (oc_pw_normalize(pw, out, cap) != 0) return OC_PW_BAD_CHARS;
    size_t n = chars(out);
    if (n < OC_PW_MIN_CHARS) return OC_PW_TOO_SHORT;
    if (n > OC_PW_MAX_CHARS) return OC_PW_TOO_LONG;
    char lw[OC_PW_NORM_MAX];
    lower(out, lw, sizeof lw);
    int same = 1;
    for (size_t i = 1; lw[i]; i++) if (lw[i] != lw[0]) { same = 0; break; }
    if (same || bsearch(lw, oc_pw_common, oc_pw_common_n, sizeof *oc_pw_common, cmp)) return OC_PW_COMMON;
    if (contains(lw, username) || contains(lw, workspace) || strstr(lw, "openchime")) return OC_PW_PERSONAL;
    return OC_PW_OK;
}
