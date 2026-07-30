/*
 * Search query parsing (REQ-081, WIN-39).
 *
 * Deliberately in shared/ and linked by BOTH the daemon and the client core, for the
 * same reason as shared/mention.c: "what does this query mean" is answered in two
 * places — the client shows the user what it understood, the daemon executes it — and
 * if those two ever disagree the product is broken in a way neither side can see.
 *
 * This is pure text parsing. It splits a query into FILTERS (`from:`, `in:`, `has:`,
 * `before:`, `after:`) and the remaining free TEXT. It resolves nothing: a name is
 * left as text because only the caller knows the roster, and dates are left as the
 * user typed them because only the caller knows the timezone.
 *
 * The operators are filters, not search terms, which is why they cannot simply be
 * passed to FTS: `from:alice` is a predicate on the author column, and MATCHing the
 * literal string "from:alice" would find messages that mention it instead.
 */

#ifndef OC_SEARCHQ_H
#define OC_SEARCHQ_H

#include <stddef.h>
#include <stdint.h>

#define OC_SQ_NAME_MAX 64
#define OC_SQ_TEXT_MAX 512

/* `has:` values, as a mask so "has:link has:file" is expressible. */
#define OC_SQ_HAS_FILE  0x01u
#define OC_SQ_HAS_LINK  0x02u
#define OC_SQ_HAS_IMAGE 0x04u

typedef struct {
    char     text[OC_SQ_TEXT_MAX];      /* what is left for FTS; may be empty */
    char     from[OC_SQ_NAME_MAX];      /* author name as typed ("" = any) */
    char     in[OC_SQ_NAME_MAX];        /* channel name as typed ("" = any) */
    unsigned has;                       /* OC_SQ_HAS_* mask; 0 = no constraint */
    char     before[16];                /* YYYY-MM-DD as typed ("" = none) */
    char     after[16];
    uint8_t  n_filters;                 /* how many operators were recognised */
} oc_searchq;

/* Parse `q` into `out`. Always succeeds: anything unrecognised stays in `text`, so a
 * query is never rejected for containing a colon. */
void oc_searchq_parse(const char *q, oc_searchq *out);

/* Render the parsed form back to a human string ("from:alice in:general fix"), for a
 * client to show what it understood. Truncates safely. */
void oc_searchq_describe(const oc_searchq *sq, char *out, size_t cap);

/* "YYYY-MM-DD" -> epoch ms at LOCAL midnight (start) or 23:59:59.999 (end); 0 when
 * unparseable. Local, not UTC, because "after:2026-01-01" means the user's day — and
 * that is why the client resolves dates rather than shipping the string to a daemon
 * that does not know the timezone. */
uint64_t oc_day_start_ms(const char *ymd);
uint64_t oc_day_end_ms(const char *ymd);

#endif /* OC_SEARCHQ_H */
