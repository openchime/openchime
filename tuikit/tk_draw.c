/*
 * tuikit — tk_draw. See tk_draw.h.
 */

#include "tk_draw.h"
#include "tk_theme.h"

#include "utf8proc.h"
#include <string.h>

int tk_cp_width(int32_t cp) {
    int w = utf8proc_charwidth(cp);
    if (w < 0) return 0;         /* control/combining: no advance */
    return w > 2 ? 2 : w;
}

int tk_str_width(const char *s) {
    utf8proc_ssize_t len = (utf8proc_ssize_t)strlen(s), off = 0;
    int w = 0;
    while (off < len) {
        int32_t cp;
        utf8proc_ssize_t n = utf8proc_iterate((const utf8proc_uint8_t *)s + off, len - off, &cp);
        if (n <= 0) break;
        off += n;
        w += tk_cp_width(cp);
    }
    return w;
}

int tk_text(int x, int y, int xmax, const char *s, uintattr_t fg, uintattr_t bg) {
    utf8proc_ssize_t len = (utf8proc_ssize_t)strlen(s), off = 0;
    while (off < len && x < xmax) {
        int32_t cp;
        utf8proc_ssize_t n = utf8proc_iterate((const utf8proc_uint8_t *)s + off, len - off, &cp);
        if (n <= 0) break;
        off += n;
        int w = tk_cp_width(cp);
        if (w == 0) continue;                 /* skip zero-width (combining/control) */
        if (x + w > xmax) break;              /* a wide glyph won't fit the last cell */
        tb_set_cell(x, y, (uint32_t)cp, fg, bg);
        x += w;
    }
    return x;
}

void tk_fill(int y, int x0, int x1, uintattr_t bg) {
    for (int x = x0; x < x1; x++) tb_set_cell(x, y, ' ', TB_DEFAULT, bg);
}

int tk_text_right(int y, int x0, int x1, const char *s, uintattr_t fg, uintattr_t bg) {
    int w = tk_str_width(s);
    int start = x1 - w;
    if (start < x0) start = x0;   /* too wide: left-clip */
    tk_text(start, y, x1, s, fg, bg);
    return start;
}
