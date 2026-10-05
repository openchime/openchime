/*
 * OpenChime — channel and DM summaries: the engine-free core (REQ-310, ARCH-116,
 * docs/SUMMARIES.md).
 *
 * One summarize step at every level. Its input is numbered lines -- messages at
 * the bottom, the lines of child summaries above -- and its output is labelled
 * plain text (an overview, then decisions, actions, problems and facts, each
 * bullet citing the lines it comes from), parsed by code into the stored
 * summary. This file is everything about that which needs neither the database
 * nor a model:
 *   - cutting a channel's messages into chunks of whole messages (a whole
 *     thread where it fits; a big thread into chunks of its own);
 *   - splitting one message too big for a chunk into parts;
 *   - the lines a chunk, a part or a stored summary reads as;
 *   - the prompt, and the grammar that holds a model to the answer's shape and
 *     to the line numbers and people of its input;
 *   - reading an answer back, dropping a bullet that cites a line not there,
 *     names a person not there, states a number its lines do not contain, or
 *     repeats one already kept, and writing the stored summary with the message
 *     and user ids its lines stand for.
 * Pure C99: the tests drive it without a model.
 */
#ifndef OC_SUM_CORE_H
#define OC_SUM_CORE_H

#include <stddef.h>
#include <stdint.h>

/* The two design-time constants. Anything larger than SUM_THRESHOLD_TOKENS --
 * a thread, a message, a list of summaries -- is cut into pieces that fit, each
 * summarized, and the summaries summarized in turn. Top-level messages further
 * apart than SUM_GAP_MS start a new chunk. */
#ifndef SUM_THRESHOLD_TOKENS
#define SUM_THRESHOLD_TOKENS 1000
#endif
#ifndef SUM_GAP_MS
#define SUM_GAP_MS (45ull * 60ull * 1000ull)
#endif
/* The one size estimate every backend shares, so where pieces are cut never
 * depends on the model: a token per this many bytes of text. */
#define SUM_BYTES_PER_TOKEN 4
/* The words a summary is asked to stay within, as a share of its input's:
 * written chat summaries run at 20-30% of the conversation (SUMMARIES.md §2).
 * Guidance to the model only; no answer is ever cut. */
#define SUM_WORDS_PCT 30
/* Bumped whenever the prompt, the shape or the grammar change. */
#define SUM_PROMPT_VERSION "s2"

/* One message as the core sees it: plain text (oc_speakable_full), its author's
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
 * its own (summarized, and the summaries summarized, to one thread summary). */
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

/* The estimated tokens of one message as a line. */
size_t oc_sum_msg_tokens(const oc_sum_msg *m);

/* Cut `msgs` (n, sorted by id; replies carry their root's id in parent_id, and a
 * thread's root must be present when any reply is) into pieces, in order of each
 * piece's last activity. A message larger than `threshold` is a chunk of its
 * own. `threshold` and `gap_ms` are the two constants, passed so the tests and
 * the evaluation tool can vary them. 0, or -1 out of memory. */
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

/* The input of one summarize step: numbered lines, each standing for the
 * message ids it came from. `label`, when set, is printed above the line (the
 * date of the summaries that follow). */
typedef struct {
    char    *text;
    int      indent;          /* a reply in a thread */
    char    *label;
    int64_t *refs;
    int      n_refs;
} oc_sum_line;
typedef struct {
    oc_sum_line *v;
    int          n, cap;
} oc_sum_lines;
/* Append a line (copies everything). 0 or -1. */
int  oc_sum_lines_add(oc_sum_lines *l, const char *text, int indent, const char *label,
                      const int64_t *refs, int n_refs);
void oc_sum_lines_free(oc_sum_lines *l);
/* The estimated tokens the lines render to. */
size_t oc_sum_lines_tokens(const oc_sum_lines *l);

/* The people an answer may give an action to, by display name. */
typedef struct {
    int64_t *id;
    char   **name;
    int      n;
} oc_sum_people;
/* Add a person once (by id). 0 or -1. */
int  oc_sum_people_add(oc_sum_people *p, int64_t id, const char *name);
void oc_sum_people_free(oc_sum_people *p);

/* The lines of a chunk of messages: "Name: text", replies indented. The
 * authors go into `people`. 0 or -1. */
int oc_sum_chunk_lines(const oc_sum_msg *msgs, const oc_sum_chunk *c, oc_sum_lines *l, oc_sum_people *people);

/* Split `text` into parts of at most `max_tokens` each: at paragraphs, then
 * lines, then sentences, then words, and only as a last resort mid-word (never
 * mid-character). Every byte of the text is in exactly one part. *parts (heap
 * array of heap strings) and their count. 0 or -1. */
int  oc_sum_split_text(const char *text, size_t max_tokens, char ***parts, int *n);
void oc_sum_parts_free(char **parts, int n);

/* Resolve a user id to a display name. */
typedef const char *(*oc_sum_name_fn)(void *ctx, int64_t user_id);

/* Append the lines of a stored summary: its overview, then one line per item.
 * `label` (may be NULL) is printed above its first line. The people its actions
 * name go into `people`. 0, or -1 (out of memory, or not a summary). */
int oc_sum_body_lines(const char *body, oc_sum_name_fn name, void *ctx, const char *label,
                      oc_sum_lines *l, oc_sum_people *people);

/* The system prompt: the same at every level. */
extern const char *const OC_SUM_SYSTEM;

/* The prompt for `lines`: `intro` (what the lines are), the numbered lines, and
 * what to write, within SUM_WORDS_PCT of the lines' words. 0 or -1. */
int oc_sum_prompt(const char *intro, const oc_sum_lines *l, oc_sum_buf *out);

/* The GBNF grammar for an answer over `n_lines` lines: the headings in order,
 * bullets that end in citations of existing line numbers, actions given to one
 * of `people` or the team. No counts and no lengths. 0 or -1. */
int oc_sum_grammar(int n_lines, const oc_sum_people *people, oc_sum_buf *out);

/* Read an answer against its lines and people, and write the stored summary
 * into `out`:
 *   {"overview":"...","decisions":[{"text":"...","by":[],"refs":[<msg ids>]}],
 *    "actions":[{"who":<user id, 0 the team>,"what":"...","refs":[...],"status":"open|done"}],
 *    "problems":[{"text":"...","refs":[...],"status":"open|resolved"}],
 *    "facts":[{"text":"...","refs":[...]}]}
 * A bullet is dropped when it cites a line not there, gives an action to
 * someone not there, states a number none of its cited lines contains, or
 * repeats one already kept; the overview, when it states a number no line
 * contains. Returns how many items were kept (0 is a valid, empty summary), or
 * -1 when the answer is not the shape at all. `dropped` (may be NULL) counts
 * what was dropped. */
int oc_sum_parse(const char *answer, const oc_sum_lines *l, const oc_sum_people *people, oc_sum_buf *out,
                 int *dropped);

/* 1 when a stored body says nothing: no overview, no items. */
int oc_sum_body_empty(const char *body);

/* The overview of a stored body, unescaped, for logs and tests. 0 or -1. */
int oc_sum_body_overview(const char *body, char *out, size_t cap);

/* Words in `s` (runs of non-space). */
size_t oc_sum_words(const char *s);

#endif
