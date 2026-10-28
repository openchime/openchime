/* Tables as people edit them (REQ-220, MARKDOWN.md §1a): the grid the table
 * editor and a paste work in, and its conversions to and from the stored
 * Markdown. Every route into a message -- typed, pasted from a sheet or from
 * its HTML, built in the editor, reopened from a sent message -- goes through
 * these, so each is checked to come back as it went in. */

#include "check.h"
#include "table.h"

#include <stdlib.h>
#include <string.h>

/* The first table in `md`, as a grid. */
static oc_table *parse(const char *md) {
    oc_rt_table rt;
    if (oc_rt_tables(md, strlen(md), &rt, 1) != 1) return NULL;
    return oc_table_from_markdown(md, strlen(md), &rt);
}

static int same(const oc_table *a, const oc_table *b) {
    if (!a || !b || a->cols != b->cols || a->rows != b->rows || a->header != b->header) return 0;
    if (memcmp(a->align, b->align, a->cols)) return 0;
    for (uint32_t r = 0; r <= a->rows; r++)
        for (uint8_t k = 0; k < a->cols; k++)
            if (strcmp(oc_table_cell(a, r, k), oc_table_cell(b, r, k))) return 0;
    return 1;
}

static void test_markdown_round_trip(void) {
    oc_table *t = oc_table_new(3, 2);
    CHECK(t != NULL);
    oc_table_set(t, 0, 0, "Item"); oc_table_set(t, 0, 1, "Price"); oc_table_set(t, 0, 2, "Notes");
    oc_table_set(t, 1, 0, "*Tacos*"); oc_table_set(t, 1, 1, "3"); oc_table_set(t, 1, 2, "Spicy\nBring napkins");
    oc_table_set(t, 2, 0, "a | b"); oc_table_set(t, 2, 1, "4"); oc_table_set(t, 2, 2, "\xF0\x9F\x8C\xAE :taco:");
    t->align[1] = OC_RT_ALIGN_RIGHT;
    t->align[2] = OC_RT_ALIGN_CENTER;
    char *md = oc_table_to_markdown(t);
    CHECK(md != NULL);
    /* The stored form: pipes escaped, a line break as <br>, alignment in the separator. */
    CHECK(md && strstr(md, "| a \\| b |") != NULL);
    /* contract: the stored form MARKDOWN.md §1a specifies, a line break as <br> */
    CHECK(md && strstr(md, "Spicy<br>Bring napkins") != NULL);
    CHECK(md && strstr(md, "| --- | ---: | :---: |") != NULL);
    /* And back: the same grid. */
    oc_table *back = md ? parse(md) : NULL;
    CHECK(same(t, back));
    oc_table_free(back);
    free(md);

    /* No header: an empty header row, read back as no header. */
    t->header = 0;
    md = oc_table_to_markdown(t);
    back = md ? parse(md) : NULL;
    CHECK(back && back->header == 0 && back->rows == 2);
    CHECK(back && strcmp(oc_table_cell(back, 1, 1), "3") == 0);
    oc_table_free(back);
    free(md);
    oc_table_free(t);

    /* A typed table reads back with its markup as written, every <br>
     * spelling a line break. */
    t = parse("| a | b |\n|:--|---|\n| _x_<BR />y | `c` |");
    CHECK(t && t->align[0] == OC_RT_ALIGN_LEFT);
    CHECK(t && strcmp(oc_table_cell(t, 1, 0), "_x_\ny") == 0);
    CHECK(t && strcmp(oc_table_cell(t, 1, 1), "`c`") == 0);
    oc_table_free(t);
}

static void test_grid_edits(void) {
    oc_table *t = oc_table_new(2, 1);
    oc_table_set(t, 0, 0, "h0"); oc_table_set(t, 0, 1, "h1");
    oc_table_set(t, 1, 0, "a"); oc_table_set(t, 1, 1, "b");
    /* A row appended and one inserted before the first body row. */
    CHECK(oc_table_insert_row(t, 2) == 0 && t->rows == 2 && oc_table_cell(t, 2, 0)[0] == '\0');
    CHECK(oc_table_insert_row(t, 1) == 0 && t->rows == 3);
    CHECK(strcmp(oc_table_cell(t, 2, 0), "a") == 0 && strcmp(oc_table_cell(t, 0, 1), "h1") == 0);
    CHECK(oc_table_remove_row(t, 1) == 0 && strcmp(oc_table_cell(t, 1, 0), "a") == 0);
    /* The header is not a body row to remove; the last body row stays. */
    CHECK(oc_table_remove_row(t, 0) == -1);
    CHECK(oc_table_remove_row(t, 2) == 0 && oc_table_remove_row(t, 1) == -1);
    /* A column inserted in the middle, with its own alignment. */
    t->align[1] = OC_RT_ALIGN_RIGHT;
    CHECK(oc_table_insert_col(t, 1) == 0 && t->cols == 3);
    CHECK(strcmp(oc_table_cell(t, 0, 2), "h1") == 0 && t->align[2] == OC_RT_ALIGN_RIGHT && t->align[1] == 0);
    CHECK(oc_table_remove_col(t, 0) == 0 && t->cols == 2 && t->align[1] == OC_RT_ALIGN_RIGHT);
    CHECK(oc_table_remove_col(t, 1) == 0 && oc_table_remove_col(t, 0) == -1);
    /* Out of bounds. */
    CHECK(oc_table_new(0, 1) == NULL && oc_table_new(OC_TABLE_MAX_COLS + 1, 1) == NULL);
    CHECK(oc_table_new(2, OC_TABLE_MAX_ROWS + 1) == NULL);
    oc_table *c = oc_table_copy(t);
    CHECK(same(t, c));
    oc_table_free(c);
    oc_table_free(t);
}

static void test_from_tsv(void) {
    /* As Excel and Sheets put it on the clipboard: a quoted cell holds a newline
     * and a doubled quote, and the trailing newline is no extra row. */
    const char *x = "Region\tQ1\tNote\r\nEast\t120\t\"Up \"\"a lot\"\"\r\nsince May\"\r\nWest\t90\t\r\n";
    oc_table *t = oc_table_from_tsv(x, strlen(x));
    CHECK(t && t->cols == 3 && t->rows == 2 && t->header == 1);
    /* contract: the cell exactly as the sheet held it, its quoting undone */
    CHECK(t && strcmp(oc_table_cell(t, 1, 2), "Up \"a lot\"\nsince May") == 0);
    CHECK(t && strcmp(oc_table_cell(t, 2, 2), "") == 0);
    oc_table_free(t);

    /* Not tabular: one row; one column; ragged rows; a column empty top to
     * bottom (code indented with tabs); no tab at all. */
    CHECK(oc_table_from_tsv("a\tb\tc", 5) == NULL);
    CHECK(oc_table_from_tsv("a\nb\nc", 5) == NULL);
    CHECK(oc_table_from_tsv("a\tb\nc", 5) == NULL);
    const char *code = "\tif (x)\n\t\treturn;";
    CHECK(oc_table_from_tsv(code, strlen(code)) == NULL);
    const char *ind = "\tfoo\n\tbar";
    CHECK(oc_table_from_tsv(ind, strlen(ind)) == NULL);
    CHECK(oc_table_from_tsv("plain words", 11) == NULL);

    /* Too wide for a table. */
    char wide[256] = "";
    for (int i = 0; i <= OC_TABLE_MAX_COLS; i++) strcat(wide, i ? "\tc" : "c");
    strcat(wide, "\n");
    for (int i = 0; i <= OC_TABLE_MAX_COLS; i++) strcat(wide, i ? "\td" : "d");
    CHECK(oc_table_from_tsv(wide, strlen(wide)) == NULL);
}

static void test_from_html(void) {
    /* Sheets' shape: bold by style, a link, an entity, a line break, a
     * colspan, whitespace collapsed, a style block and a comment ignored. */
    const char *h =
        "<html><head><style>td{color:red}</style></head><body><!-- x -->"
        "<table><tbody>"
        "<tr><td style=\"font-weight:bold\">Name</td><td><b>Site</b></td><td>Note</td></tr>"
        "<tr><td>Ada &amp; co</td><td><a href=\"https://example.com/a\">home</a></td>"
        "<td>line one<br>line   two</td></tr>"
        "<tr><td colspan=\"2\">wide</td><td><a href=\"javascript:x\">bad</a> <i>it</i></td></tr>"
        "</tbody></table></body></html>";
    oc_table *t = oc_table_from_html(h, strlen(h));
    CHECK(t && t->cols == 3 && t->rows == 2);
    CHECK(t && strcmp(oc_table_cell(t, 0, 0), "*Name*") == 0);
    CHECK(t && strcmp(oc_table_cell(t, 0, 1), "*Site*") == 0);
    /* contract: the entity decoded, the cell otherwise as copied */
    CHECK(t && strcmp(oc_table_cell(t, 1, 0), "Ada & co") == 0);
    CHECK(t && strcmp(oc_table_cell(t, 1, 1), "[home](https://example.com/a)") == 0);
    /* contract: <br> a line break, a run of spaces one space */
    CHECK(t && strcmp(oc_table_cell(t, 1, 2), "line one\nline two") == 0);
    /* colspan: its column, then an empty one; a link to anything but http(s)
     * keeps its label and loses the address. */
    CHECK(t && strcmp(oc_table_cell(t, 2, 0), "") == 0 && strcmp(oc_table_cell(t, 2, 1), "wide") == 0);
    /* contract: the label kept without its address, italic as the dialect writes it */
    CHECK(t && strcmp(oc_table_cell(t, 2, 2), "bad _it_") == 0);
    /* And as Markdown it is a table the scanner reads back. */
    char *md = t ? oc_table_to_markdown(t) : NULL;
    oc_table *back = md ? parse(md) : NULL;
    CHECK(same(t, back));
    oc_table_free(back);
    free(md);
    oc_table_free(t);

    CHECK(oc_table_from_html("<p>no table</p>", 15) == NULL);
    CHECK(oc_table_from_html("&#x1F600;", 9) == NULL);
    /* Numeric entities decode to UTF-8. */
    const char *e = "<table><tr><td>&#x1F600;&#233;</td><td>x</td></tr></table>";
    t = oc_table_from_html(e, strlen(e));
    CHECK(t && strcmp(oc_table_cell(t, 0, 0), "\xF0\x9F\x98\x80\xC3\xA9") == 0);
    oc_table_free(t);
}

int run_table_tests(void) {
    failures = 0;
    test_markdown_round_trip();
    test_grid_edits();
    test_from_tsv();
    test_from_html();
    printf("test_table: Markdown round trips with alignment, pipes and line breaks; grid edits; "
           "spreadsheet text with quoting; clipboard HTML with links, bold and entities\n");
    return failures;
}
