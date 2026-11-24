/* The scope catalog: see scopes.h. The names and descriptions are
 * docs/plans/APPS.md §4's, word for word. */

#include "scopes.h"

#include <string.h>

static const oc_scope CATALOG[] = {
    { "chat:write",        "Post and edit its own messages in channels it is in" },
    { "channels:read",     "See the names and members of public channels it is in" },
    { "channels:history",  "Read messages in public channels it is in" },
    { "groups:read",       "See the names and members of private channels it is in" },
    { "groups:history",    "Read messages in private channels it is in" },
    { "im:history",        "Read direct messages it is part of" },
    { "users:read",        "See people's names, titles and time zones" },
    { "users:read.email",  "See people's email addresses" },
    { "reactions:read",    "See reactions in channels it is in" },
    { "reactions:write",   "Add and remove its own reactions" },
    { "app_mentions:read", "See messages that mention it" },
    { "commands",          "Add slash commands and shortcuts" },
    { "links:read",        "See links to its own domains" },
    { "links:write",       "Show previews of links to its own domains" },
};
#define N_SCOPES (sizeof CATALOG / sizeof CATALOG[0])

size_t oc_scope_count(void) { return N_SCOPES; }

const oc_scope *oc_scope_at(size_t i) { return i < N_SCOPES ? &CATALOG[i] : NULL; }

static long scope_index(const char *name, size_t len) {
    for (size_t i = 0; i < N_SCOPES; i++)
        if (strlen(CATALOG[i].name) == len && memcmp(CATALOG[i].name, name, len) == 0) return (long)i;
    return -1;
}

const oc_scope *oc_scope_find(const char *name, size_t len) {
    long i = name ? scope_index(name, len) : -1;
    return i < 0 ? NULL : &CATALOG[i];
}

int oc_scopes_normalize(const char *list, size_t len, char *out, size_t cap) {
    if (!out || cap == 0) return -1;
    unsigned char seen[N_SCOPES] = { 0 };
    size_t i = 0;
    while (i < len) {
        size_t j = i;
        while (j < len && list[j] != ' ') j++;
        if (j == i) return -1;                           /* empty name: two spaces, or one at an end */
        long k = scope_index(list + i, j - i);
        if (k < 0) return -1;
        seen[k] = 1;
        if (j == len) break;
        i = j + 1;
        if (i == len) return -1;                         /* a trailing space */
    }
    size_t at = 0;
    for (size_t k = 0; k < N_SCOPES; k++) {
        if (!seen[k]) continue;
        size_t n = strlen(CATALOG[k].name);
        if (at + (at ? 1 : 0) + n + 1 > cap) return -1;
        if (at) out[at++] = ' ';
        memcpy(out + at, CATALOG[k].name, n);
        at += n;
    }
    out[at] = '\0';
    return 0;
}
