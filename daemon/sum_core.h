/*
 * OpenChime — channel and DM summaries: the engine-free core (REQ-310, ARCH-116,
 * docs/SUMMARIES.md).
 *
 * A summary is built from small pieces and rolled up. This file is everything
 * about that which needs neither the database nor a model:
 *   - cutting a channel's messages into pieces (chunks of whole messages, a whole
 *     thread where it fits, a big thread cut into chunks of its own);
 *   - rendering a piece, or a list of child summaries, as the text a model reads,
 *     with short ids (P1 for a person, m1 for a message, i1 for a child's item);
 *   - the grammar that holds a local model to the summary's one shape, with
 *     only the ids of that piece allowed;
 *   - reading a model's answer back, dropping any item that cites what is not
 *     there or states a number its sources do not contain, and writing the kept
 *     items with the real message and user ids.
 * Pure C99: the tests drive it without a model.
 */
#ifndef OC_SUM_CORE_H
#define OC_SUM_CORE_H

#include <stddef.h>
#include <stdint.h>

/* The two design-time constants. A piece, a thread or a list of child summaries
 * larger than SUM_THRESHOLD_TOKENS is cut up and rolled up; top-level messages
 * further apart than SUM_GAP_MS start a new chunk. */
#ifndef SUM_THRESHOLD_TOKENS
#define SUM_THRESHOLD_TOKENS 1000
#endif
#ifndef SUM_GAP_MS
#define SUM_GAP_MS (45ull * 60ull * 1000ull)
#endif
/* The one size estimate every backend shares, so where pieces are cut never
 * depends on the model: a token per this many bytes of rendered text. */
#define SUM_BYTES_PER_TOKEN 4
/* Bumped whenever the prompts, the shape or the grammar change. */
#define SUM_PROMPT_VERSION "s1"

/* At most this many items of each kind, so an answer always fits the token
 * budget: four kinds of three items of SUM_TEXT_MAX characters, and the overview. */
#define SUM_MAX_ITEMS 3
#define SUM_MAX_REFS  4
#define SUM_TEXT_MAX  160

/* One message as the core sees it: plain text (oc_speakable), its author's
 * name, and its thread (parent_id: the root's id, 0 for a top-level message). */
typedef struct {
    int64_t     id, parent_id, author_id;
    int64_t     created_ms, edited_ms;
    const char *author;
    const char *text;
} oc_sum_msg;

/* A piece of a channel to summarize: the indices (into the caller's message
 * array, in order) of the messages it holds. `root_id` is the thread's root when
 * the piece is all or part of one thread. `end_ms` is its last activity. */
typedef struct {
    int    *idx;
    int     n;
    int64_t root_id;
    int64_t end_ms;
} oc_sum_chunk;

/* A unit of the cut: one chunk, or a thread too big for one, held as chunks of
 * its own (n_chunks > 1, rolled up to one thread summary). */
typedef struct {
    oc_sum_chunk *chunks;
    int           n_chunks;
    int           is_big_thread;
    int64_t       root_id;
    int64_t       end_ms;
} oc_sum_piece;

typedef struct {
    oc_sum_piece *pieces;
    int           n;
} oc_sum_cut;

/* The estimated tokens of one message as rendered. */
size_t oc_sum_msg_tokens(const oc_sum_msg *m);

/* Cut `msgs` (n, sorted by id; replies carry their root's id in parent_id, and a
 * thread's root must be present when any reply is) into pieces, in order of each
 * piece's last activity. `threshold` and `gap_ms` are the two constants, passed
 * so the tests and the evaluation tool can vary them. 0, or -1 out of memory. */
int  oc_sum_cut_build(const oc_sum_msg *msgs, int n, size_t threshold, uint64_t gap_ms, oc_sum_cut *out);
void oc_sum_cut_free(oc_sum_cut *c);

/* A growing string. */
typedef struct { char *p; size_t n, cap; int oom; } oc_sum_buf;
void oc_sum_buf_add(oc_sum_buf *b, const char *s, size_t n);
void oc_sum_buf_puts(oc_sum_buf *b, const char *s);
void oc_sum_buf_printf(oc_sum_buf *b, const char *fmt, ...);
/* `s` as a JSON string, quotes included. */
void oc_sum_buf_json(oc_sum_buf *b, const char *s);
void oc_sum_buf_free(oc_sum_buf *b);

/* The ids a rendered prompt uses, and what they stand for. */
typedef struct {
    int64_t  *person;          /* P1.. → user id */
    char    **person_name;
    int       n_person;
    int64_t  *msg;             /* m1.. → message id (a leaf) */
    const char **msg_text;     /* the cited text, for the number check */
    int       n_msg;
    /* A rollup's i1.. items: each stands for the message ids its item cited. */
    char    **item_text;
    int64_t **item_refs;
    int      *item_nrefs;
    int       n_item;
} oc_sum_ids;
void oc_sum_ids_free(oc_sum_ids *ids);

/* Render a chunk as the model reads it (`channel` is its display name), and
 * fill `ids`. 0 or -1. */
int oc_sum_render_chunk(const char *channel, const oc_sum_msg *msgs, const oc_sum_chunk *c,
                        oc_sum_buf *out, oc_sum_ids *ids);

/* A child summary, stored form (see oc_sum_check): its JSON body and the names
 * of the people it cites, by user id. */
typedef struct {
    const char *body;
    int64_t     start_ms, end_ms;
} oc_sum_child;
/* Resolve a user id to a display name for a rollup's roster. */
typedef const char *(*oc_sum_name_fn)(void *ctx, int64_t user_id);

/* Render child summaries, in order, as a rollup's input. 0 or -1. */
int oc_sum_render_rollup(const char *channel, const oc_sum_child *kids, int n, oc_sum_name_fn name,
                         void *ctx, oc_sum_buf *out, oc_sum_ids *ids);

/* The estimated tokens a list of children renders to (to decide on sections). */
size_t oc_sum_rollup_tokens(const oc_sum_child *kids, int n);

/* The system prompts. */
extern const char *const OC_SUM_SYSTEM_LEAF;
extern const char *const OC_SUM_SYSTEM_ROLLUP;

/* The GBNF grammar for `ids`: the summary's shape, with only its P# and m# (a
 * leaf) or i# (a rollup) allowed, and at most `max_items` (1 to SUM_MAX_ITEMS)
 * of each kind. 0 or -1. */
int oc_sum_grammar(const oc_sum_ids *ids, int rollup, int max_items, oc_sum_buf *out);

/* Read a model's answer (`json`, `len` bytes) against `ids` and write the stored
 * form into `out`: the same shape with refs as message ids and people as user
 * ids. An item is dropped when any ref or person is not one of `ids`, or when a
 * number in its text appears in none of its sources. Returns how many items were
 * kept (0 is a valid, empty summary), or -1 if the answer is not the shape at
 * all. `dropped` (may be NULL) counts the items dropped. */
int oc_sum_check(const char *json, size_t len, const oc_sum_ids *ids, int rollup, oc_sum_buf *out,
                 int *dropped);

/* 1 when a stored body says nothing: no overview, no items. */
int oc_sum_body_empty(const char *body);

/* The overview of a stored body, unescaped, for logs and tests. 0 or -1. */
int oc_sum_body_overview(const char *body, char *out, size_t cap);

#endif
