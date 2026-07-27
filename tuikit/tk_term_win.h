/*
 * termbox2 contract for Windows (ARCH-81). termbox2 itself is POSIX-only, so on
 * Windows the TUI is built against this instead: the exact subset of the
 * termbox2 v2.5.0 public API that client/tui/main.c uses, with values copied
 * verbatim so the two backends are semantically identical. The implementation
 * is tk_term.c, over the Windows Console API.
 *
 * Attribute model matches the termbox2 default build (TB_OPT_ATTR_W unset → 16):
 * uintattr_t is 16-bit, colors are 0..8, TB_BOLD/TB_REVERSE are the 0x0100/0x0400
 * flags. Do not "upgrade" these without changing the POSIX build to match.
 */

#ifndef OC_TERMBOX2_WIN_H
#define OC_TERMBOX2_WIN_H

#include <stdint.h>
#include <stddef.h>

typedef uint16_t uintattr_t;

struct tb_event {
    uint8_t  type;   /* TB_EVENT_*                     */
    uint8_t  mod;    /* TB_MOD_* bitset                */
    uint16_t key;    /* TB_KEY_* (0 if a printable ch) */
    uint32_t ch;     /* Unicode codepoint             */
    int32_t  w;      /* resize width                  */
    int32_t  h;      /* resize height                 */
    int32_t  x;      /* mouse x                       */
    int32_t  y;      /* mouse y                       */
};

/* Return codes */
#define TB_OK    0
#define TB_ERR  -1

/* Event types */
#define TB_EVENT_KEY     1
#define TB_EVENT_RESIZE  2
#define TB_EVENT_MOUSE   3

/* Modifier bits */
#define TB_MOD_ALT     1
#define TB_MOD_CTRL    2
#define TB_MOD_SHIFT   4
#define TB_MOD_MOTION  8

/* Input modes (bitset) */
#define TB_INPUT_CURRENT 0
#define TB_INPUT_ESC     1
#define TB_INPUT_ALT     2
#define TB_INPUT_MOUSE   4

/* Keys (control range == the raw control codes) */
#define TB_KEY_CTRL_C     0x03
#define TB_KEY_CTRL_F     0x06
#define TB_KEY_BACKSPACE  0x08
#define TB_KEY_TAB        0x09
#define TB_KEY_CTRL_K     0x0b
#define TB_KEY_ENTER      0x0d
#define TB_KEY_CTRL_Q     0x11
#define TB_KEY_CTRL_R     0x12
#define TB_KEY_CTRL_W     0x17
#define TB_KEY_ESC        0x1b
#define TB_KEY_SPACE      0x20
#define TB_KEY_BACKSPACE2 0x7f

/* Special keys (top of the u16 space, same as termbox2) */
#define TB_KEY_PGUP             (0xffff - 16)
#define TB_KEY_PGDN             (0xffff - 17)
#define TB_KEY_ARROW_UP         (0xffff - 18)
#define TB_KEY_ARROW_DOWN       (0xffff - 19)
#define TB_KEY_ARROW_LEFT       (0xffff - 20)
#define TB_KEY_ARROW_RIGHT      (0xffff - 21)
#define TB_KEY_MOUSE_LEFT       (0xffff - 23)
#define TB_KEY_MOUSE_WHEEL_UP   (0xffff - 27)
#define TB_KEY_MOUSE_WHEEL_DOWN (0xffff - 28)

/* Colors + attributes (16-bit / normal mode) */
#define TB_DEFAULT  0x0000
#define TB_BLACK    0x0001
#define TB_RED      0x0002
#define TB_GREEN    0x0003
#define TB_YELLOW   0x0004
#define TB_BLUE     0x0005
#define TB_MAGENTA  0x0006
#define TB_CYAN     0x0007
#define TB_WHITE    0x0008
#define TB_BOLD     0x0100
#define TB_REVERSE  0x0400

/* Output modes (termbox2 values): 8-color vs xterm-256. */
#define TB_OUTPUT_CURRENT 0
#define TB_OUTPUT_NORMAL  1
#define TB_OUTPUT_256     2

/* The functions client/tui/main.c + tuikit call. */
int tb_init(void);
int tb_shutdown(void);
int tb_width(void);
int tb_height(void);
int tb_clear(void);
int tb_present(void);
int tb_set_cell(int x, int y, uint32_t ch, uintattr_t fg, uintattr_t bg);
int tb_set_input_mode(int mode);
int tb_set_output_mode(int mode);
int tb_poll_event(struct tb_event *event);
int tb_peek_event(struct tb_event *event, int timeout_ms);

#endif /* OC_TERMBOX2_WIN_H */
