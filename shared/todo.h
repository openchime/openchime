/* To-do lists in messages: how a body makes one. A message whose body starts
 * "/todo" and a title is a list; each following line that is not blank is a
 * task, a leading '-' or '*' ignored, text after the first ": " its
 * description. The daemon parses the body when it stores the message and keeps
 * the list in its own tables from then on; the clients use the same rules to
 * build the body a compose form sends. The body stays the author's plain text.
 *
 * Built into shared/richtext.c, beside the rest of the text rules. */
#ifndef OPENCHIME_TODO_H
#define OPENCHIME_TODO_H

#include <stddef.h>
#include <stdint.h>

#include "protocol.h"   /* OC_TODO_* */

typedef struct { uint32_t off, len; } oc_todo_span;
typedef struct {
    oc_todo_span title;
    uint16_t     n_tasks;
    oc_todo_span name[OC_TODO_MAX_TASKS], desc[OC_TODO_MAX_TASKS];
} oc_todo_parsed;

/* What oc_todo_parse found. */
enum { OC_TODO_NONE = 0,     /* not a to-do list: an ordinary message */
       OC_TODO_OK = 1,       /* a list, in *out */
       OC_TODO_BAD = -1,     /* a list past a limit, or with no title */
       OC_TODO_DUP = -2 };   /* a list naming one task twice */

/* Parse `n` bytes of `body`. Spans are offsets into `body`, trimmed of spaces
 * and tabs. */
int oc_todo_parse(const char *body, size_t n, oc_todo_parsed *out);

/* Whether a task name or a title is blank: empty, or only spaces and tabs. */
int oc_todo_blank(const char *s, size_t n);

#endif
