/* OpenChime client core -- a table as people edit it (REQ-220, MARKDOWN.md §1a).
 *
 * A message stores a table as Markdown; this is the same table as a grid of
 * strings, which is what the table editor edits and what a paste produces. The
 * conversions here are the only place either form is turned into the other, so
 * a table pasted, typed, built in the editor or reopened from a sent message
 * all round-trip through one pair of functions.
 *
 * A cell's text is what the person sees and types: inline Markdown as written,
 * a real newline for a line break, a real `|` for a pipe. Escaping to the
 * stored form (`\|`, `<br>`) happens in oc_table_to_markdown and is undone in
 * oc_table_from_markdown, and nowhere else. */
#ifndef OC_TABLE_H
#define OC_TABLE_H

#include <stddef.h>
#include <stdint.h>

#include "richtext.h"

/* What the editor and a paste allow, beyond the dialect's column limit: enough
 * for any table a chat message should carry, bounded so a pasted sheet cannot
 * make one that no body could hold. */
#define OC_TABLE_MAX_COLS OC_RT_TABLE_MAX_COLS
#define OC_TABLE_MAX_ROWS 200

typedef struct {
    uint8_t  cols;
    uint32_t rows;                         /* body rows; the header is row 0 of `cells` */
    uint8_t  header;                       /* 0: no header row (written as an empty one) */
    uint8_t  align[OC_TABLE_MAX_COLS];     /* OC_RT_ALIGN_* */
    char   **cells;                        /* (rows + 1) * cols heap strings, never NULL */
} oc_table;

/* A table of `cols` columns and `rows` body rows, every cell empty; a header
 * row by default. NULL when out of bounds or out of memory. */
oc_table *oc_table_new(uint8_t cols, uint32_t rows);
void      oc_table_free(oc_table *t);
oc_table *oc_table_copy(const oc_table *t);

/* The cell at `row` (0 = the header) and `col`; "" out of range. */
const char *oc_table_cell(const oc_table *t, uint32_t row, uint8_t col);
/* Replace a cell's text (copied). Returns 0, or -1 out of range or memory. */
int  oc_table_set(oc_table *t, uint32_t row, uint8_t col, const char *text);

/* Grow or shrink: a row inserted before body row `at` (1-based; rows + 1
 * appends) or removed; a column inserted before `at` (0-based) or removed. The
 * header row is never removed, and at least one column and one body row stay.
 * Returns 0, or -1 when the change is not allowed. */
int  oc_table_insert_row(oc_table *t, uint32_t at);
int  oc_table_remove_row(oc_table *t, uint32_t at);
int  oc_table_insert_col(oc_table *t, uint8_t at);
int  oc_table_remove_col(oc_table *t, uint8_t at);

/* The table as Markdown: a header row (empty cells when !header), the separator
 * with each column's alignment, then the body rows. `|` is written `\|` and a
 * newline `<br>`. Heap; NULL on failure. */
char *oc_table_to_markdown(const oc_table *t);

/* The table `rt` found in `body` by oc_rt_tables, as a grid: `\|` read as `|`
 * and `<br>` as a newline, everything else as written. */
oc_table *oc_table_from_markdown(const char *body, size_t len, const oc_rt_table *rt);

/* Cells copied from a spreadsheet: rows on lines, cells on tabs, a quoted cell
 * ("...", "" for a quote) holding tabs and newlines as Excel and Sheets write
 * them. The first row is the header. NULL unless the text is tabular -- at
 * least two rows and two columns, within the limits above -- which is the
 * test for offering "Paste as table" at all. */
oc_table *oc_table_from_tsv(const char *text, size_t len);

/* The first <table> in clipboard HTML: its rows and cells, a link to an http(s)
 * address kept as [label](address), bold and italic (as tags or as a
 * font-weight or font-style in a style attribute) as *x* and _x_, <br> as a
 * newline, entities decoded, every other tag dropped. The first row is the
 * header. NULL when there is no table, or it is out of bounds. */
oc_table *oc_table_from_html(const char *html, size_t len);

#endif /* OC_TABLE_H */
