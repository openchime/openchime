/*
 * OpenChime — message formatting (REQ-220, ARCH-100).
 *
 * The parser for the dialect [MARKDOWN.md](../docs/MARKDOWN.md) specifies:
 * a Slack-compatible subset of inline emphasis extended with real lists.
 *
 * Shared by every frontend and by the daemon. Rendering formatting needs no
 * server knowledge, but reading a message aloud does: the daemon turns a body
 * into speakable text before it is synthesized (ARCH-111, shared/speakable.c),
 * and it must agree with the frontends about what is a code block and what is a
 * delimiter. One implementation is what stops the TUI, the GUI and the spoken
 * version drifting about what counts as bold.
 *
 * It returns SPANS OVER THE ORIGINAL BYTES and never a rewritten string. The
 * body a client renders is byte-identical to the body the daemon stored, which
 * keeps FTS5 search, migration 0021's mention offsets and message editing all
 * addressing the same text.
 */

#ifndef OC_RICHTEXT_H
#define OC_RICHTEXT_H

#include <stddef.h>
#include <stdint.h>

/* Styles. A span carries exactly one construct; OC_RT_DELIM is OR-ed in to say
 * "these bytes are the construct's delimiter, not its content" so a frontend
 * can dim `*`…`*` while keeping a `- ` bullet visible. */
enum {
    OC_RT_BOLD      = 0x0001,   /* *bold* or **bold** */
    OC_RT_ITALIC    = 0x0002,   /* _italic_ */
    OC_RT_STRIKE    = 0x0004,   /* ~struck~ */
    OC_RT_CODE      = 0x0008,   /* `inline code` */
    OC_RT_CODEBLOCK = 0x0010,   /* ```fenced```, may span lines */
    OC_RT_QUOTE     = 0x0020,   /* > quoted */
    OC_RT_BULLET    = 0x0040,   /* - item */
    OC_RT_ORDERED   = 0x0080,   /* 1. item */
    OC_RT_LINK      = 0x0100,   /* a bare http(s) URL, autolinked */
    OC_RT_LABELLED  = 0x0200,   /* [label](https://...): the label; its address
                                 * is in the `](...)` delimiter after it, found
                                 * with oc_rt_target */
    OC_RT_TABLE     = 0x0400,   /* a pipe table, the whole of it: one span over
                                 * every byte; its rows and cells come from
                                 * oc_rt_tables, and no other span from
                                 * oc_rt_scan falls inside it */
    OC_RT_BREAK     = 0x0800,   /* `<br>` in a table cell: a line break. Only
                                 * oc_rt_scan_cell reports it; the bytes are the
                                 * whole construct, drawn as a new line */

    OC_RT_DELIM     = 0x8000    /* the delimiter bytes of the construct above */
};

typedef struct {
    size_t   start;   /* byte offset into the body */
    size_t   len;     /* bytes */
    uint16_t style;   /* one OC_RT_* construct, optionally | OC_RT_DELIM */
} oc_rt_span;

/* Per message. Generous for real use; a bound so a pathological body cannot
 * make a keystroke do unbounded work — the composer re-scans on every one. */
#define OC_RT_MAX 128

/* Scan `body` (`len` bytes, not necessarily NUL-terminated) and write up to
 * `max` spans to `out`. Returns how many were found, which MAY EXCEED `max` so
 * a caller can tell it was truncated. `out` may be NULL when `max` is 0.
 *
 * Spans NEST rather than partition: `*bold with _italic_ inside*` yields a bold
 * span and an italic span inside it, and the style at a byte is the OR of every
 * span containing it. They arrive sorted by `start` ascending, and where two
 * share a start the enclosing one comes first.
 *
 * The rules, stated once so both frontends inherit them (MARKDOWN.md §2):
 *   - An INLINE opening delimiter must be preceded by whitespace, a line start,
 *     an opening bracket or another delimiter, and followed by a non-space; a
 *     closing one must be preceded by a non-space. So `2 * 3 * 4` is
 *     arithmetic and `a_variable_name` is an identifier.
 *   - Inline emphasis must close ON THE SAME LINE. A fenced code block need
 *     not; it is the only multi-line construct.
 *   - `\*` `\_` `\~` `` \` `` `\>` `\-` `\\` produce the literal character. The
 *     backslash is reported as a lone OC_RT_DELIM span so a frontend can hide
 *     it; the character after it is plain text.
 *   - Code suppresses everything: no markup, and no escapes, inside a code span
 *     or a fenced block.
 *   - Block markers (`>`, `- `, `1. `) are matched at the start of a line,
 *     after optional indentation, and are exempt from the inline rules.
 *   - A bare `http://` or `https://` URL is autolinked: it yields one
 *     OC_RT_LINK span over the address, with NO delimiter span, because there
 *     is no markup to hide — the address is its own label (MARKDOWN.md §4).
 *     Trailing sentence punctuation and an unbalanced closing bracket are left
 *     outside the span, so `(see https://example.com/a).` links the address and
 *     not the full stop. Only those two schemes are recognised: a link span is
 *     what a frontend turns into something the OS opens, so widening the set is
 *     a security decision and it is taken HERE rather than per frontend.
 *   - `[label](https://address)` is a labelled link: an OC_RT_LABELLED span
 *     over the label, which may itself carry emphasis, with OC_RT_DELIM |
 *     OC_RT_LABELLED spans over the `[` and over `](address)`. The address
 *     follows the autolink rules exactly -- http or https, the same end -- and
 *     must be followed by the closing `)`; the label is one line, non-empty,
 *     with no brackets of its own. Anything else is literal text, in which the
 *     bare address still autolinks. Because the label can say anything, a
 *     frontend shows the address before opening one (MARKDOWN.md §4).
 *

 * Anything unmatched or ambiguous degrades to its literal source: an unclosed
 * delimiter yields no span at all, so a half-typed `*` never restyles the rest
 * of the message.
 */
size_t oc_rt_scan(const char *body, size_t len, oc_rt_span *out, size_t max);

/* The address a labelled link opens: for an OC_RT_LABELLED span (the label,
 * not a delimiter) from oc_rt_scan over the same `body`, its offset and length.
 * Returns 1, or 0 when `span` is not a labelled link's label. */
int oc_rt_target(const char *body, size_t len, const oc_rt_span *span,
                 size_t *off, size_t *tlen);

/* ---- tables (MARKDOWN.md §1a) ----------------------------------------------
 *
 * A GitHub-style pipe table: a header row, a separator row of `---` cells
 * (`:---` left, `:---:` centre, `---:` right), then body rows, each a line with
 * at least one unescaped `|`. Leading and trailing pipes are optional and `\|`
 * is a literal pipe. The header and separator must have the same number of
 * cells, at most OC_RT_TABLE_MAX_COLS; a body row with fewer cells has empty
 * ones, and extra cells are ignored. The table ends at a blank line or the first
 * line with no pipe. Never inside a fenced code block, and a line that begins
 * with a block marker (`>`, `- `, `1. `) does not start one. Anything else is
 * not a table and is scanned as text, so nothing typed is lost. */
#define OC_RT_TABLE_MAX_COLS 20

enum { OC_RT_ALIGN_NONE = 0, OC_RT_ALIGN_LEFT, OC_RT_ALIGN_CENTER, OC_RT_ALIGN_RIGHT };

typedef struct {
    size_t   start, len;     /* the table's bytes: the header row's first byte to
                              * the end of its last row, the final newline not
                              * included -- the same range as its OC_RT_TABLE span */
    uint8_t  cols;
    uint8_t  header;         /* 0 when every header cell is empty: no header row */
    uint32_t rows;           /* body rows, the header not counted */
    uint8_t  align[OC_RT_TABLE_MAX_COLS];   /* OC_RT_ALIGN_* per column */
} oc_rt_table;

/* A cell's content: its bytes with the surrounding spaces trimmed. len 0 for an
 * empty cell, or one a short row does not have. */
typedef struct { size_t start, len; } oc_rt_cell;

/* The tables in `body`, in order, up to `max` written to `out`. Returns how many
 * there are, which may exceed `max`, as oc_rt_scan does. */
size_t oc_rt_tables(const char *body, size_t len, oc_rt_table *out, size_t max);

/* Walk a table's rows: the header first (row 0, whether or not t->header), then
 * each body row; the separator is never a row. oc_rt_row_next fills `cells`
 * with t->cols entries and the row's index in *row, and returns 0 after the
 * last. Linear in the table's size however it is walked. */
typedef struct {
    const char *b;
    size_t      pos, end;
    uint32_t    row;
    uint8_t     cols;
} oc_rt_rowit;
void oc_rt_rows_begin(oc_rt_rowit *it, const char *body, size_t len, const oc_rt_table *t);
int  oc_rt_row_next(oc_rt_rowit *it, oc_rt_cell cells[OC_RT_TABLE_MAX_COLS], uint32_t *row);

/* The inline spans of one cell (`len` bytes at `off` in `body`), with offsets
 * into `body`: emphasis, code, links and escapes as anywhere, plus OC_RT_BREAK
 * for each `<br>` (`<br/>`, `<br />`, any case). No block markers: a cell is
 * one line of inline text. Returns the count, which may exceed `max`. */
size_t oc_rt_scan_cell(const char *body, size_t off, size_t len, oc_rt_span *out, size_t max);

/* `body` on one line for an excerpt -- a sidebar preview, a toast, a search
 * hit: each table becomes "Table: N rows × M columns" and every other byte is
 * kept. Writes at most `cap` bytes including the NUL, cutting only at a UTF-8
 * boundary, and returns the length written. */
size_t oc_rt_flatten(const char *body, size_t len, char *out, size_t cap);

#endif /* OC_RICHTEXT_H */
