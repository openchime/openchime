/*
 * tuikit — tk_textarea. See tk_textarea.h.
 */

#include "tk_textarea.h"
#include "tk_draw.h"

#include "utf8proc.h"
#include <string.h>

void tk_textarea_init(tk_textarea *t, const char *placeholder) {
    memset(t, 0, sizeof *t);
    t->placeholder = placeholder;
}

void tk_textarea_clear(tk_textarea *t) { t->buf[0] = '\0'; }

const char *tk_textarea_value(const tk_textarea *t) { return t->buf; }

static void backspace(char *buf) {
    size_t n = strlen(buf);
    if (!n) return;
    do { n--; } while (n > 0 && (buf[n] & 0xC0) == 0x80);
    buf[n] = '\0';
}

tk_result tk_textarea_handle(tk_textarea *t, const struct tb_event *ev) {
    if (ev->type != TB_EVENT_KEY) return TK_NONE;
    if (ev->key == TB_KEY_ENTER) return TK_SELECT;
    if (ev->key == TB_KEY_ESC)   return TK_CANCEL;
    if (ev->key == TB_KEY_BACKSPACE || ev->key == TB_KEY_BACKSPACE2) {
        backspace(t->buf); return TK_CHANGED;
    }
    uint32_t cp = ev->ch;
    if (ev->key == TB_KEY_SPACE) cp = ' ';
    if (cp >= 0x20) {
        utf8proc_uint8_t enc[4];
        utf8proc_ssize_t k = utf8proc_encode_char((utf8proc_int32_t)cp, enc);
        size_t n = strlen(t->buf);
        if (k > 0 && n + (size_t)k < sizeof t->buf) {
            memcpy(t->buf + n, enc, (size_t)k);
            t->buf[n + k] = '\0';
        }
        return TK_CHANGED;
    }
    return TK_NONE;
}

/* Soft-wrap the buffer to `width` columns, drawing each line; returns the (x,y)
 * just past the last glyph so the caller can place a cursor. Char-level wrap with
 * a preference to break at the last space on the line. */
void tk_textarea_draw(tk_textarea *t, tk_rect r, int focused) {
    for (int j = 0; j < r.h; j++) tk_fill(r.y + j, r.x, r.x + r.w, TB_DEFAULT);

    if (!t->buf[0]) {
        if (t->placeholder) tk_text(r.x, r.y, r.x + r.w, t->placeholder, TB_BLACK | TB_BOLD, TB_DEFAULT);
        if (focused) tb_set_cell(r.x, r.y, ' ', TB_DEFAULT, TB_REVERSE);
        return;
    }

    const char *s = t->buf;
    utf8proc_ssize_t len = (utf8proc_ssize_t)strlen(s), off = 0;
    int col = 0, row = 0, endx = r.x, endy = r.y;
    while (off < len && row < r.h) {
        int32_t cp;
        utf8proc_ssize_t n = utf8proc_iterate((const utf8proc_uint8_t *)s + off, len - off, &cp);
        if (n <= 0) break;
        off += n;
        if (cp == '\n') { row++; col = 0; endx = r.x; endy = r.y + row; continue; }
        int w = tk_cp_width(cp);
        if (w == 0) continue;
        if (col + w > r.w) { row++; col = 0; if (row >= r.h) break; }
        tb_set_cell(r.x + col, r.y + row, (uint32_t)cp, TB_WHITE, TB_DEFAULT);
        col += w;
        endx = r.x + col; endy = r.y + row;
    }
    if (focused && endy < r.y + r.h && endx < r.x + r.w)
        tb_set_cell(endx, endy, ' ', TB_DEFAULT, TB_REVERSE);
}
