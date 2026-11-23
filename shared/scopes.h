/*
 * The scope catalog (docs/plans/APPS.md §4): the kinds of access a token for an
 * agent or a bot may be given, each with what an owner or admin is told when
 * choosing it. One table, in shared/ because both ends read it: the daemon
 * refuses a mint naming anything outside it (docs/details/MCP.md §3.1), and the
 * clients show each scope's description when a token is minted.
 *
 * A token's scopes travel and are stored as names separated by single spaces.
 * No allocation.
 */
#ifndef OC_SCOPES_H
#define OC_SCOPES_H

#include <stddef.h>

typedef struct {
    const char *name;   /* "chat:write" */
    const char *desc;   /* "Post and edit its own messages in channels it is in" */
} oc_scope;

/* The catalog, in the document's order. */
size_t          oc_scope_count(void);
const oc_scope *oc_scope_at(size_t i);

/* The scope named by the `len` bytes at `name`, or NULL. */
const oc_scope *oc_scope_find(const char *name, size_t len);

/* `list` (`len` bytes, names separated by single spaces; empty is no scopes)
 * written to `out` in the catalog's order, each once, NUL-terminated. Returns
 * 0, or -1 when a name is not in the catalog, a separator is not one space, or
 * `out` is too small. */
int oc_scopes_normalize(const char *list, size_t len, char *out, size_t cap);

#endif
