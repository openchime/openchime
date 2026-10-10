/*
 * OpenChime — a channel or DM summary as a client reads it (REQ-310,
 * docs/SUMMARIES.md §3).
 *
 * The daemon answers SUMMARIZE with the summary as JSON (docs/SUMMARIES.md §1, §3):
 *   {"summary": {"overview": {"text", "refs"},
 *                "topics": [{"title", "text", "refs", "count", "people": [names],
 *                            "details": [{"text", "refs"}]}],
 *                "attention": [{"kind": "action|question", "text", "refs"}],
 *                "more": ["title", ...]},
 *    "posters": [{"id": <user id, 0 an integration>, "name"}], "count": <messages>,
 *    "sources": {"<msg id>": {"author", "author_id", "at", "parent", "text"}}}
 * This turns that into what a frontend draws: the overview; topics, each with
 * what happened, who was in it, how many messages it rests on, and details
 * citing the messages they came from; what needs attention; the titles of more
 * topics; who posted and how much; and the cited messages themselves, for a
 * preview of each citation.
 */
#ifndef OC_CLIENT_SUMMARY_H
#define OC_CLIENT_SUMMARY_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char     *text;        /* heap */
    uint64_t *refs;        /* heap; the messages it came from, in the order given */
    size_t    n_refs;
} oc_summary_detail;

typedef struct {
    char              *title;   /* heap; "" when there is none */
    char              *text;    /* heap; the paragraph, "" when there is none */
    uint64_t          *refs;    /* heap */
    size_t             n_refs;
    uint64_t           count;   /* the messages it rests on */
    char             **people;  /* heap; who wrote them */
    size_t             n_people;
    oc_summary_detail *details; /* heap */
    size_t             n_details;
} oc_summary_topic;

/* What needs attention: an action someone has, or a question not answered. */
typedef struct {
    uint8_t   question;    /* 0 an action */
    char     *text;        /* heap */
    uint64_t *refs;        /* heap */
    size_t    n_refs;
} oc_summary_attention;

typedef struct {
    uint64_t id;           /* user id; 0 for an integration */
    char    *name;         /* heap */
} oc_summary_poster;

/* A message a detail cites, as it was when the summary was sent. */
typedef struct {
    uint64_t id;
    char    *author;       /* heap */
    uint64_t author_id;    /* 0 for an integration */
    uint64_t at;           /* ms */
    uint64_t parent;       /* its thread's root, 0 when not in a thread */
    char    *text;         /* heap; plain, whole */
} oc_summary_source;

typedef struct {
    char                 *overview;     /* heap; "" when there is none */
    uint64_t             *refs;         /* heap; what the overview cites */
    size_t                n_refs;
    oc_summary_topic     *topics;       /* heap */
    size_t                n_topics;
    oc_summary_attention *attention;    /* heap */
    size_t                n_attention;
    char                **more;         /* heap; titles of further topics */
    size_t                n_more;
    oc_summary_poster *posters;    /* heap; most messages first */
    size_t             n_posters;
    uint64_t           count;      /* messages in the span */
    oc_summary_source *sources;    /* heap */
    size_t             n_sources;
} oc_summary_view;

/* Read `len` bytes of a SUMMARY body into `out` (zeroed first). 0, or -1 when it
 * is not a summary (out is then empty). */
int  oc_summary_view_parse(const char *json, size_t len, oc_summary_view *out);
void oc_summary_view_free(oc_summary_view *v);

/* The message `id` among the sources, or NULL. */
const oc_summary_source *oc_summary_source_of(const oc_summary_view *v, uint64_t id);

/* A range the person typed as two local dates, "2026-09-04 2026-09-14" (or
 * with "to" between), into [start of the first day, start of the day after
 * the second) in ms. 0, or -1 when it is not two dates in order. */
int  oc_summary_range_parse(const char *text, uint64_t *start_ms, uint64_t *end_ms);

/* One local date "YYYY-MM-DD" into the time its day starts, in ms. 0 or -1. */
int  oc_summary_date_parse(const char *text, uint64_t *day_ms);

/* What each span is called in the menu, by OC_SUM_* scope: "Unread", "Today",
 * "Since yesterday", "Last 7 days", "Custom date range". */
const char *oc_summary_scope_name(uint8_t scope);

/* The local days [start, end) covers: "Sep 28 - Oct 5", or "Oct 5" for one day. */
void oc_summary_dates(uint64_t start_ms, uint64_t end_ms, char *out, size_t cap);

/* The title while a summary is coming: "Summarizing unreads in #pf-alerts",
 * "Summarizing today in ...", "... since yesterday ...", "... the last 7 days
 * ...", or the dates of a range. `where` is "#name" or "@name". */
void oc_summary_wait_title(uint8_t scope, uint64_t start_ms, uint64_t end_ms, const char *where,
                           char *out, size_t cap);

/* What the pane says under it: `position` is how many requests are ahead of it
 * (SUMMARY_QUEUED), 0 once it is being made, -1 before the daemon has said,
 * OC_SUM_POS_STARTING while the daemon's summarizer is still starting. */
void oc_summary_wait_text(int32_t position, char *out, size_t cap);

/* A notice's line: "Your summary of #kudos (Today) is ready", or "Couldn't
 * summarize #kudos (Today)" when it was not made. `where` is "#name" or
 * "@name". */
void oc_summary_notice_title(uint8_t status, uint8_t scope, uint64_t start_ms, uint64_t end_ms, const char *where,
                             char *out, size_t cap);

/* The headline when a summary was not made, by its SUMMARY status; the daemon's
 * own sentence goes under it. */
const char *oc_summary_fail_title(uint8_t status);

/* Who posted, as the participants' hover reads: "A", "A and B", "A, B, and C". */
void oc_summary_posters_text(const oc_summary_view *v, char *out, size_t cap);

/* When a source was posted: "Today at 11:22 AM", "Yesterday at 9:05 PM", or
 * "Sep 28 at 4:15 PM" (24-hour clock when `h24`). */
void oc_summary_when(uint64_t at_ms, uint64_t now_ms, int h24, char *out, size_t cap);

/* The posters' names in `text`, drawn as mentions: each found where a whole
 * name stands (not inside a word), the longest name first where two overlap,
 * in order of where they start. Up to `max` into `out`; how many. */
typedef struct { size_t start, len; size_t poster; } oc_summary_mention;
size_t oc_summary_mentions(const oc_summary_view *v, const char *text, oc_summary_mention *out, size_t max);

/* The custom range picker both frontends draw: two months side by side, the
 * first click picking the first day and the second the last, days after today
 * not pickable. Days are local, as ms of their start. */
typedef struct {
    int      year, month;      /* the left month shown (month 1-12) */
    uint64_t start, end;       /* the days picked, 0 when not yet */
    uint64_t today;
} oc_sumcal;
void oc_sumcal_init(oc_sumcal *c, uint64_t now_ms);
/* Show months `delta` later (earlier when negative). */
void oc_sumcal_shift(oc_sumcal *c, int delta);
/* The month `which` (0 left, 1 right): its year and month, and its 42 cells,
 * Sunday first, each the day's start in ms or 0 outside the month. */
void oc_sumcal_month(const oc_sumcal *c, int which, int *year, int *month, uint64_t cells[42]);
/* Pick `day`: the first day, or (when one is picked and `day` is not before
 * it) the last; picking again starts over. A day after today is not picked. */
void oc_sumcal_pick(oc_sumcal *c, uint64_t day);
/* The range picked, [first day, the day after the last) in ms: 0, or -1 while
 * both days are not picked. */
int  oc_sumcal_range(const oc_sumcal *c, uint64_t *start_ms, uint64_t *end_ms);
/* A day as "YYYY-MM-DD", as the fields show it. */
void oc_summary_day_text(uint64_t day_ms, char *out, size_t cap);

#endif
