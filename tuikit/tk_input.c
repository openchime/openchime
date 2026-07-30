/*
 * tuikit — tk_input. See tk_input.h. Cursor is at the end of the buffer (a full
 * left/right-cursor editor is a later refinement); this covers login/search/
 * composer entry, which append + backspace.
 */

#include "tk_input.h"
#include "tk_theme.h"
#include "tk_draw.h"

#include "utf8proc.h"
#include <stdio.h>      /* snprintf, used below; it was missing and warned */
#include <string.h>

void tk_input_init(tk_input *in, int mask, const char *placeholder) {
    memset(in, 0, sizeof *in);
    in->mask = mask;
    in->placeholder = placeholder;
}

void tk_input_clear(tk_input *in) { in->buf[0] = '\0'; }

void tk_input_set(tk_input *in, const char *text) {
    snprintf(in->buf, sizeof in->buf, "%s", text ? text : "");
}

const char *tk_input_value(const tk_input *in) { return in->buf; }

/* Remove the last UTF-8 codepoint. */
static void backspace(char *buf) {
    size_t n = strlen(buf);
    if (!n) return;
    do { n--; } while (n > 0 && (buf[n] & 0xC0) == 0x80);
    buf[n] = '\0';
}

tk_result tk_input_handle(tk_input *in, const struct tb_event *ev) {
    if (ev->type != TB_EVENT_KEY) return TK_NONE;
    if (ev->key == TB_KEY_ENTER) return TK_SELECT;
    if (ev->key == TB_KEY_ESC)   return TK_CANCEL;
    if (ev->key == TB_KEY_BACKSPACE || ev->key == TB_KEY_BACKSPACE2) {
        backspace(in->buf); return TK_CHANGED;
    }
    uint32_t cp = ev->ch;
    if (ev->key == TB_KEY_SPACE) cp = ' ';
    if (cp >= 0x20) {
        utf8proc_uint8_t enc[4];
        utf8proc_ssize_t k = utf8proc_encode_char((utf8proc_int32_t)cp, enc);
        size_t n = strlen(in->buf);
        if (k > 0 && n + (size_t)k < sizeof in->buf) {
            memcpy(in->buf + n, enc, (size_t)k);
            in->buf[n + k] = '\0';
        }
        return TK_CHANGED;
    }
    return TK_NONE;
}

void tk_input_draw(tk_input *in, tk_rect r, int focused) {
    const tk_theme *th = tk_theme_active();
    int xmax = r.x + r.w;
    tk_fill(r.y, r.x, xmax, th->bg);
    int cx;
    if (in->buf[0]) {
        if (in->mask) {
            char dots[TK_INPUT_CAP]; size_t j = 0;
            for (const char *p = in->buf; *p && j < sizeof dots - 1; p++)
                if ((*p & 0xC0) != 0x80) dots[j++] = '*';
            dots[j] = '\0';
            cx = tk_text(r.x, r.y, xmax, dots, th->fg, th->bg);
        } else {
            cx = tk_text(r.x, r.y, xmax, in->buf, th->fg, th->bg);
        }
    } else {
        if (in->placeholder) tk_text(r.x, r.y, xmax, in->placeholder, th->muted, th->bg);
        cx = r.x;   /* cursor sits at start when empty */
    }
    if (focused && cx < xmax) tb_set_cell(cx, r.y, ' ', TB_DEFAULT, TB_REVERSE);
}
