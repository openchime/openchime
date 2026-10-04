/*
 * OpenChime — summaries in the database (REQ-310, ARCH-116, docs/SUMMARIES.md §4).
 *
 * The SQL of summaries, apart from the rest of the writer: reading a channel's
 * messages for a period (the summary worker's own read connection), finding a
 * node built before, storing new nodes (the writer, under the guard that their
 * messages are still as they were), and purging the nodes a changed message
 * was built into (the writer, in the same transaction as the change).
 */
#ifndef OC_SUM_STORE_H
#define OC_SUM_STORE_H

#include <stddef.h>
#include <stdint.h>

#include "sqlite3.h"
#include "sum_core.h"

#define OC_SUM_KIND_CHUNK   0
#define OC_SUM_KIND_THREAD  1
#define OC_SUM_KIND_SECTION 2
#define OC_SUM_KIND_PERIOD  3

/* A channel's messages for a period: every top-level message and every thread
 * whose last activity is in [start, end), people only (no integration posts,
 * call events or deleted messages), as plain text, sorted by id. */
typedef struct {
    oc_sum_msg *msgs;
    int         n;
    char        channel[128];
} oc_sum_window;
int  oc_sum_load_window(sqlite3 *db, int64_t channel, int64_t start_ms, int64_t end_ms, oc_sum_window *w);
void oc_sum_window_free(oc_sum_window *w);

/* A display name for `user_id`, into `out` ("" when unknown). */
void oc_sum_user_name(sqlite3 *db, int64_t user_id, char *out, size_t cap);

/* A node with these inputs (ikey) built with `version`: 1 and its id and body
 * (heap) if there is one, else 0. */
int oc_sum_find_ikey(sqlite3 *db, const char *ikey, const char *version, int64_t *id, char **body);

/* The period summary of [start, end) in `channel` for `tz_offset_min`: the one
 * built with `version` if there is one, else (when `allow_old`) the newest built
 * with any version, its version into `got_version`. 1 and id/body (heap) if
 * found, else 0. */
int oc_sum_find_period(sqlite3 *db, int64_t channel, int64_t start_ms, int64_t end_ms, int tz_offset_min,
                       const char *version, int allow_old, int64_t *id, char **body,
                       char *got_version, size_t vcap);

/* One input of a node about to be stored: a message (with its edited-at stamp
 * as read), a node already stored, or a node earlier in the same store. */
#define OC_SUM_IN_MSG  0
#define OC_SUM_IN_NODE 1
#define OC_SUM_IN_NEW  2
typedef struct {
    int     kind;
    int64_t id;          /* MSG: message id; NODE: node id; NEW: index into the batch */
    int64_t stamp;       /* MSG: edited_at_ms as read (0 never edited) */
} oc_sum_input;

typedef struct {
    int           kind;              /* OC_SUM_KIND_* */
    char          ikey[72];
    int64_t       root_id, first_msg_id, last_msg_id;
    int64_t       start_ms, end_ms;
    int           tz_offset_min;
    char         *body;              /* heap, owned by the batch */
    uint32_t      tokens_in, cpu_ms;
    oc_sum_input *in;                /* heap */
    int           n_in;
    int64_t       id;                /* filled by oc_sum_store */
} oc_sum_new;

/* Store `n` nodes in one transaction, after checking every message input still
 * exists, undeleted, with the stamp it was read with, and every node input still
 * exists. 0 stored (ids filled), 1 refused because something changed meanwhile,
 * -1 on a database error. */
int oc_sum_store(sqlite3 *db, int64_t channel, const char *version, oc_sum_new *nodes, int n);

/* Free the batch's heap parts. */
void oc_sum_new_free(oc_sum_new *nodes, int n);

/* A message changed: `msg_id` in `channel` was sent, edited, deleted, restored
 * or removed, in the thread rooted at `root_id` (its own id when top-level), at
 * `created_ms`. Deletes every node built on it or on its thread, every period
 * summary whose span holds that moment, and every node built on those, up the
 * tree. Returns the number of nodes deleted, or -1. For the writer, inside the
 * transaction that changed the message. */
int oc_sum_purge(sqlite3 *db, int64_t channel, int64_t msg_id, int64_t root_id, int64_t created_ms);

/* Remove nodes nothing is built on any more and nobody has needed for
 * `max_age_ms`. Returns how many. */
int oc_sum_collect(sqlite3 *db, int64_t now_ms, int64_t max_age_ms);

/* A stable name for a list of inputs ("<tag>:" and 16 hex digits). */
void oc_sum_ikey(char tag, const oc_sum_input *in, int n, char *out, size_t cap);

#endif
