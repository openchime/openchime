/* client/core/permalink.h -- an openchime:// link, read (ARCH-96, CLIENT.md "Links").
 *
 * Two forms name a conversation and, optionally, a line in it:
 *
 *   openchime://<host>/c/<channel>[/m/<message>]                 a copied permalink
 *   openchime://w/<audience>/c/<channel>[/m/<message>][/t/<thread>]   an email link
 *
 * The first names the workspace by the address it was copied from. The second
 * names it by its audience id with OpenChime's services, so the app opens only a
 * workspace it is already signed into or remembers, and never connects to a
 * server a link names (the email cannot point anywhere else). `/t/<thread>` is
 * the thread's root message. */
#ifndef OC_PERMALINK_H
#define OC_PERMALINK_H

#include <stddef.h>
#include <stdint.h>

#define OC_PERMALINK_HOST     1   /* openchime://<host>/c/... */
#define OC_PERMALINK_AUDIENCE 2   /* openchime://w/<audience>/c/... */

typedef struct {
    int      kind;            /* OC_PERMALINK_HOST or OC_PERMALINK_AUDIENCE */
    char     where[256];      /* the host, or the audience id */
    uint64_t channel;         /* never 0 */
    uint64_t message;         /* 0: the conversation, not a line in it */
    uint64_t thread;          /* the thread's root; 0: none named */
} oc_permalink;

/* Reads `text` (leading whitespace allowed). 1 and *out filled for either form;
 * 0 for anything else: another scheme, a missing or zero channel, a malformed
 * id, an audience id that is not `ws_` and URL-safe characters, or anything
 * after the last part. */
int oc_permalink_parse(const char *text, oc_permalink *out);

#endif /* OC_PERMALINK_H */
