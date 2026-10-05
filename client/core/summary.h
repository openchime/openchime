/*
 * OpenChime — a channel or DM summary as a client reads it (REQ-310,
 * docs/SUMMARIES.md §3).
 *
 * The daemon answers SUMMARIZE with the summary as JSON:
 *   {"summary": {"overview": "...", "refs": [<msg ids>],
 *                "decisions": [{"text", "refs"}], "actions": [{"who"?, "what", "refs", "status"?}],
 *                "problems": [{"text", "refs", "status"?}], "facts": [{"text", "refs"}]},
 *    "people": {"<user id>": "<display name>", ...}}
 * This turns that into an overview and a list of items a frontend draws, each
 * with the messages it came from, so the frontend can jump to them.
 */
#ifndef OC_CLIENT_SUMMARY_H
#define OC_CLIENT_SUMMARY_H

#include <stddef.h>
#include <stdint.h>

/* What an item is, in the order a summary shows them. */
enum { OC_SUMI_DECISION = 0, OC_SUMI_ACTION = 1, OC_SUMI_PROBLEM = 2, OC_SUMI_FACT = 3 };

typedef struct {
    uint8_t   kind;        /* OC_SUMI_* */
    char     *text;        /* heap */
    char     *who;         /* heap; an action's owner ("Team" for the team), else NULL */
    char      status[12];  /* "open", "done", "resolved", or "" when none was given */
    uint64_t *refs;        /* heap; the messages it came from, oldest first as given */
    size_t    n_refs;
} oc_summary_item;

typedef struct {
    char            *overview;   /* heap; "" when there is none */
    uint64_t        *refs;       /* heap; every message the summary covers */
    size_t           n_refs;
    oc_summary_item *items;      /* heap; decisions, then actions, problems, facts */
    size_t           n_items;
} oc_summary_view;

/* Read `len` bytes of a SUMMARY body into `out` (zeroed first). 0, or -1 when it
 * is not a summary (out is then empty). */
int  oc_summary_view_parse(const char *json, size_t len, oc_summary_view *out);
void oc_summary_view_free(oc_summary_view *v);

/* A range the person typed as two local dates, "2026-09-04 2026-09-14" (or
 * with "to" between), into [start of the first day, start of the day after
 * the second) in ms. 0, or -1 when it is not two dates in order. */
int  oc_summary_range_parse(const char *text, uint64_t *start_ms, uint64_t *end_ms);

/* What a span is called: "Unread", "Last 7 days", "Since yesterday", or the
 * range's local dates. `scope` is OC_SUM_*; start and end matter for a range. */
void oc_summary_span_label(uint8_t scope, uint64_t start_ms, uint64_t end_ms, char *out, size_t cap);

/* What the pane says while a summary is coming: `position` is how many requests
 * are ahead of it (SUMMARY_QUEUED), 0 once it is being made, -1 before the
 * daemon has said. */
void oc_summary_wait_text(int32_t position, char *out, size_t cap);

/* What an item kind is called, as a heading: "Decisions", ... */
const char *oc_summary_kind_heading(uint8_t kind);

#endif
