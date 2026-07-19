/*
 * termbox2 Windows Console backend (ARCH-81) — the implementation of the
 * contract in termbox2_win.h, so client/tui/main.c runs unchanged on Windows.
 *
 * Output is VT/ANSI, not the classic CHAR_INFO console buffer: Windows 10+
 * consoles interpret the same escape sequences termbox2 already emits once
 * ENABLE_VIRTUAL_TERMINAL_PROCESSING is set, and Windows Terminal renders wide
 * glyphs (emoji, CJK) far better than legacy conhost. The tradeoff, stated
 * plainly: this expects a VT-capable console — Windows Terminal, or Windows 10
 * 1511+ conhost. On a truly ancient console it degrades to garbage escape
 * codes; that is an accepted limitation, not a bug to work around.
 *
 * Input is the Console API (ReadConsoleInput), translated into tb_event. The
 * fiddly part is the key mapping; it is done explicitly below.
 *
 * Rendering strategy: a back buffer of cells, presented by a **full-frame
 * diff** against a front buffer, so only changed cells are redrawn — the same
 * approach termbox2 uses, and what keeps a 30 Hz redraw flicker-free.
 *
 * Wide glyphs: a codepoint whose display width is 2 (measured with utf8proc,
 * exactly as main.c measures for layout) occupies one cell here and the terminal
 * advances the cursor by two, so the trailing cell is skipped on present. Getting
 * this wrong shifts every wide row right by one; it is the single most likely
 * rendering bug and is handled explicitly.
 */

#ifdef _WIN32

#include "termbox2_win.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utf8proc.h"   /* width measurement, same lib main.c uses for layout */

typedef struct {
    uint32_t   ch;
    uintattr_t fg;
    uintattr_t bg;
} tb_cell;

static struct {
    HANDLE   hin, hout;
    DWORD    old_in_mode, old_out_mode;
    int      inited;
    int      w, h;
    tb_cell *back;      /* what tb_set_cell writes */
    tb_cell *front;     /* what is currently on screen (for the diff) */
    int      mouse;     /* TB_INPUT_MOUSE enabled */
    uint32_t pending_resize_w, pending_resize_h;
    int      have_resize;
} T;

/* --- helpers -------------------------------------------------------------- */

static void query_size(int *w, int *h) {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(T.hout, &csbi)) {
        *w = csbi.srWindow.Right  - csbi.srWindow.Left + 1;
        *h = csbi.srWindow.Bottom - csbi.srWindow.Top  + 1;
    } else {
        *w = 80; *h = 24;
    }
    if (*w < 1) *w = 1;
    if (*h < 1) *h = 1;
}

static int alloc_buffers(int w, int h) {
    tb_cell *nb = calloc((size_t)w * h, sizeof *nb);
    tb_cell *nf = calloc((size_t)w * h, sizeof *nf);
    if (!nb || !nf) { free(nb); free(nf); return TB_ERR; }
    free(T.back); free(T.front);
    T.back = nb; T.front = nf;
    T.w = w; T.h = h;
    /* Fill the back buffer with blanks and the front with an impossible cell so
     * the first present redraws everything. */
    for (int i = 0; i < w * h; i++) {
        T.back[i].ch = ' '; T.back[i].fg = TB_DEFAULT; T.back[i].bg = TB_DEFAULT;
        T.front[i].ch = 0xffffffffu;   /* forces a diff on the first frame */
    }
    return TB_OK;
}

static void out(const char *s, size_t n) {
    DWORD wr;
    WriteConsoleA(T.hout, s, (DWORD)n, &wr, NULL);
}
static void outs(const char *s) { out(s, strlen(s)); }

/* Encode a codepoint as UTF-8 into buf (>=4). Returns bytes written. */
static int cp_to_utf8(uint32_t cp, char *buf) {
    if (cp < 0x80) { buf[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        buf[0] = (char)(0xE0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    buf[0] = (char)(0xF0 | (cp >> 18));
    buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    buf[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static int cell_width(uint32_t cp) {
    if (cp == 0 || cp == ' ') return 1;
    int w = utf8proc_charwidth((utf8proc_int32_t)cp);
    return w <= 0 ? 1 : w;   /* treat zero/neg-width as 1 so layout never desyncs */
}

/* Append the SGR sequence for (fg,bg) to buf. termbox color 1..8 → ANSI 30..37;
 * TB_DEFAULT(0) → 39/49; TB_BOLD → 1; TB_REVERSE → 7. */
static size_t sgr(char *buf, uintattr_t fg, uintattr_t bg) {
    int fcol = fg & 0x00FF, bcol = bg & 0x00FF;
    int fansi = fcol ? (30 + (fcol - 1)) : 39;
    int bansi = bcol ? (40 + (bcol - 1)) : 49;
    const char *bold = (fg & TB_BOLD) ? ";1" : "";
    const char *rev  = (fg & TB_REVERSE) ? ";7" : "";
    return (size_t)snprintf(buf, 32, "\x1b[0;%d;%d%s%sm", fansi, bansi, bold, rev);
}

/* --- lifecycle ------------------------------------------------------------ */

int tb_init(void) {
    if (T.inited) return TB_OK;
    T.hin  = GetStdHandle(STD_INPUT_HANDLE);
    T.hout = GetStdHandle(STD_OUTPUT_HANDLE);
    if (T.hin == INVALID_HANDLE_VALUE || T.hout == INVALID_HANDLE_VALUE) return TB_ERR;

    if (!GetConsoleMode(T.hin, &T.old_in_mode) ||
        !GetConsoleMode(T.hout, &T.old_out_mode)) return TB_ERR;

    /* Output: interpret VT sequences. */
    DWORD om = T.old_out_mode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    if (!SetConsoleMode(T.hout, om)) return TB_ERR;

    /* Input: raw. Crucially clear ENABLE_PROCESSED_INPUT so Ctrl+C / Ctrl+Q are
     * delivered as key events rather than signals, and clear QUICK_EDIT so mouse
     * events reach us. ENABLE_EXTENDED_FLAGS is required for those to stick. */
    DWORD im = T.old_in_mode;
    im &= ~(DWORD)(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT | ENABLE_QUICK_EDIT_MODE);
    im |=  (DWORD)(ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS);
    /* Mouse defaults on; tb_set_input_mode toggles it. */
    im |= ENABLE_MOUSE_INPUT;
    T.mouse = 1;
    if (!SetConsoleMode(T.hin, im)) { SetConsoleMode(T.hout, T.old_out_mode); return TB_ERR; }

    int w, h; query_size(&w, &h);
    if (alloc_buffers(w, h) != TB_OK) {
        SetConsoleMode(T.hin, T.old_in_mode);
        SetConsoleMode(T.hout, T.old_out_mode);
        return TB_ERR;
    }

    outs("\x1b[?1049h");   /* alternate screen buffer */
    outs("\x1b[?25l");     /* hide cursor */
    outs("\x1b[2J");       /* clear */
    T.inited = 1;
    return TB_OK;
}

int tb_shutdown(void) {
    if (!T.inited) return TB_OK;
    outs("\x1b[0m");       /* reset attrs */
    outs("\x1b[?25h");     /* show cursor */
    outs("\x1b[?1049l");   /* leave alternate screen */
    SetConsoleMode(T.hin, T.old_in_mode);
    SetConsoleMode(T.hout, T.old_out_mode);
    free(T.back);  T.back  = NULL;
    free(T.front); T.front = NULL;
    T.inited = 0;
    return TB_OK;
}

int tb_width(void)  { return T.w; }
int tb_height(void) { return T.h; }

int tb_clear(void) {
    for (int i = 0; i < T.w * T.h; i++) {
        T.back[i].ch = ' '; T.back[i].fg = TB_DEFAULT; T.back[i].bg = TB_DEFAULT;
    }
    return TB_OK;
}

int tb_set_cell(int x, int y, uint32_t ch, uintattr_t fg, uintattr_t bg) {
    if (x < 0 || y < 0 || x >= T.w || y >= T.h) return TB_OK;
    tb_cell *c = &T.back[y * T.w + x];
    c->ch = ch; c->fg = fg; c->bg = bg;
    return TB_OK;
}

int tb_set_input_mode(int mode) {
    /* main.c passes TB_INPUT_ESC | (mouse ? TB_INPUT_MOUSE : 0). Only the mouse
     * bit changes console state; ESC handling is inherent to raw input. */
    DWORD im;
    if (!GetConsoleMode(T.hin, &im)) return TB_ERR;
    int want_mouse = (mode & TB_INPUT_MOUSE) ? 1 : 0;
    if (want_mouse) im |= ENABLE_MOUSE_INPUT;
    else            im &= ~(DWORD)ENABLE_MOUSE_INPUT;
    im |= ENABLE_EXTENDED_FLAGS;   /* keep the mouse/quick-edit changes sticky */
    SetConsoleMode(T.hin, im);
    T.mouse = want_mouse;
    return TB_OK;
}

/* --- present (diff) ------------------------------------------------------- */

int tb_present(void) {
    /* Build the frame into a growable buffer, then one WriteConsole. Only cells
     * that changed are emitted; each emitted run repositions the cursor and sets
     * SGR once. Wide glyphs consume the following cell. */
    static char *fb = NULL;
    static size_t fbcap = 0;
    size_t need = (size_t)T.w * T.h * 24 + 64;   /* generous upper bound */
    if (need > fbcap) { char *n = realloc(fb, need); if (!n) return TB_ERR; fb = n; fbcap = need; }
    size_t p = 0;

    int cur_x = -2, cur_y = -2;          /* force a move on the first emit */
    uintattr_t cur_fg = 0xffff, cur_bg = 0xffff;

    for (int y = 0; y < T.h; y++) {
        for (int x = 0; x < T.w; x++) {
            int idx = y * T.w + x;
            tb_cell *b = &T.back[idx], *f = &T.front[idx];
            int wide = cell_width(b->ch) >= 2;

            if (b->ch == f->ch && b->fg == f->fg && b->bg == f->bg) {
                if (wide) x++;           /* skip the covered trailing cell */
                continue;
            }

            if (y != cur_y || x != cur_x) {
                p += (size_t)snprintf(fb + p, 32, "\x1b[%d;%dH", y + 1, x + 1);
                cur_x = x; cur_y = y;
            }
            if (b->fg != cur_fg || b->bg != cur_bg) {
                p += sgr(fb + p, b->fg, b->bg);
                cur_fg = b->fg; cur_bg = b->bg;
            }
            char u8[4];
            int n = cp_to_utf8(b->ch ? b->ch : ' ', u8);
            memcpy(fb + p, u8, (size_t)n); p += (size_t)n;
            *f = *b;
            cur_x += wide ? 2 : 1;       /* terminal advanced 1 or 2 columns */

            if (wide) {
                /* Mark the trailing front cell consumed so a later frame that
                 * only changes it forces a redraw of this pair. */
                if (x + 1 < T.w) { T.front[idx + 1].ch = 0xfffffffeu; x++; }
            }
        }
    }
    if (p > 0) out(fb, p);
    return TB_OK;
}

/* --- input ---------------------------------------------------------------- */

static uint8_t mods_of(DWORD ks) {
    uint8_t m = 0;
    if (ks & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED))     m |= TB_MOD_ALT;
    if (ks & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED))   m |= TB_MOD_CTRL;
    if (ks & SHIFT_PRESSED)                              m |= TB_MOD_SHIFT;
    return m;
}

/* Translate one KEY_EVENT_RECORD into `ev`. Returns 1 if it produced an event,
 * 0 if it should be ignored (key-up, dead modifier press, etc.). */
static int key_event(const KEY_EVENT_RECORD *k, struct tb_event *ev) {
    if (!k->bKeyDown) return 0;
    memset(ev, 0, sizeof *ev);
    ev->type = TB_EVENT_KEY;
    ev->mod  = mods_of(k->dwControlKeyState);

    switch (k->wVirtualKeyCode) {
    case VK_UP:     ev->key = TB_KEY_ARROW_UP;   return 1;
    case VK_DOWN:   ev->key = TB_KEY_ARROW_DOWN; return 1;
    case VK_PRIOR:  ev->key = TB_KEY_PGUP;       return 1;
    case VK_NEXT:   ev->key = TB_KEY_PGDN;       return 1;
    case VK_RETURN: ev->key = TB_KEY_ENTER;      return 1;
    case VK_ESCAPE: ev->key = TB_KEY_ESC;        return 1;
    case VK_TAB:    ev->key = TB_KEY_TAB;        return 1;
    case VK_BACK:   ev->key = TB_KEY_BACKSPACE2; return 1;   /* main.c checks both */
    case VK_SHIFT: case VK_CONTROL: case VK_MENU:
    case VK_LWIN: case VK_RWIN: case VK_CAPITAL:
        return 0;   /* bare modifier press */
    default: break;
    }

    WCHAR wc = k->uChar.UnicodeChar;
    if (wc == 0) return 0;

    if (wc < 0x20) {
        /* Control code — Ctrl+letter arrives here as 1..26 with PROCESSED_INPUT
         * off. main.c's TB_KEY_CTRL_* equal exactly these values. Space is 0x20
         * (not < 0x20) so it takes the printable path below. */
        ev->key = (uint16_t)wc;
        return 1;
    }
    if (wc == 0x7f) { ev->key = TB_KEY_BACKSPACE2; return 1; }

    /* Printable. UTF-16 surrogate halves for astral codepoints (typed emoji) are
     * rare in a composer and not paired here; BMP covers everything the TUI's
     * input path needs, and picker-inserted emoji never come through the console
     * as keystrokes. */
    ev->ch = (uint32_t)wc;
    return 1;
}

static int mouse_to_ev(const MOUSE_EVENT_RECORD *m, struct tb_event *ev) {
    memset(ev, 0, sizeof *ev);
    ev->type = TB_EVENT_MOUSE;
    ev->x = m->dwMousePosition.X;
    ev->y = m->dwMousePosition.Y;
    ev->mod = mods_of(m->dwControlKeyState);

    if (m->dwEventFlags & MOUSE_WHEELED) {
        /* High word of dwButtonState is the signed wheel delta. */
        short delta = (short)HIWORD(m->dwButtonState);
        ev->key = delta > 0 ? TB_KEY_MOUSE_WHEEL_UP : TB_KEY_MOUSE_WHEEL_DOWN;
        return 1;
    }
    if (m->dwEventFlags == 0 && (m->dwButtonState & FROM_LEFT_1ST_BUTTON_PRESSED)) {
        ev->key = TB_KEY_MOUSE_LEFT;
        return 1;
    }
    return 0;   /* movement / button-up: ignored, matching the TUI's use of clicks */
}

/* Wait up to timeout_ms (<0 = forever) for one translatable event. */
static int read_event(struct tb_event *ev, int timeout_ms) {
    for (;;) {
        if (T.have_resize) {
            memset(ev, 0, sizeof *ev);
            ev->type = TB_EVENT_RESIZE;
            ev->w = (int)T.pending_resize_w;
            ev->h = (int)T.pending_resize_h;
            T.have_resize = 0;
            return TB_OK;
        }

        DWORD wr = WaitForSingleObject(T.hin, timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms);
        if (wr == WAIT_TIMEOUT) return TB_ERR;      /* no event (peek timed out) */
        if (wr != WAIT_OBJECT_0) return TB_ERR;

        INPUT_RECORD rec;
        DWORD n = 0;
        if (!ReadConsoleInput(T.hin, &rec, 1, &n) || n == 0) return TB_ERR;

        switch (rec.EventType) {
        case KEY_EVENT:
            if (key_event(&rec.Event.KeyEvent, ev)) return TB_OK;
            break;
        case MOUSE_EVENT:
            if (T.mouse && mouse_to_ev(&rec.Event.MouseEvent, ev)) return TB_OK;
            break;
        case WINDOW_BUFFER_SIZE_EVENT: {
            int w = rec.Event.WindowBufferSizeEvent.dwSize.X;
            int h = rec.Event.WindowBufferSizeEvent.dwSize.Y;
            /* dwSize is the buffer, not the window; re-query the window rect. */
            query_size(&w, &h);
            if (w != T.w || h != T.h) {
                alloc_buffers(w, h);
                outs("\x1b[2J");
                memset(ev, 0, sizeof *ev);
                ev->type = TB_EVENT_RESIZE; ev->w = w; ev->h = h;
                return TB_OK;
            }
            break;
        }
        default: break;
        }
        /* An ignored record: loop, but if this was a bounded peek and time is up,
         * WaitForSingleObject on the next pass returns quickly. For a poll
         * (timeout < 0) we simply keep waiting, which is correct. */
        if (timeout_ms == 0) return TB_ERR;
    }
}

int tb_poll_event(struct tb_event *event) {
    return read_event(event, -1);
}

int tb_peek_event(struct tb_event *event, int timeout_ms) {
    return read_event(event, timeout_ms);
}

#endif /* _WIN32 */
