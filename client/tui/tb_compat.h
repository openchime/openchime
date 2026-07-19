/*
 * Terminal-backend router (ARCH-81). The TUI is written against termbox2's API,
 * but termbox2 itself is POSIX-only (it includes <termios.h>, <sys/ioctl.h>,
 * <sys/select.h> unconditionally and drives /dev/tty). So on Windows we do not
 * include it at all: `termbox2_win.h` re-declares the exact subset of the
 * termbox2 contract the TUI uses, and `termbox2_win.c` implements it over the
 * Windows Console API. `client/tui/main.c` includes THIS header instead of
 * termbox2.h, and nothing else in the TUI changes.
 *
 * The public constants and the `struct tb_event` layout are copied verbatim
 * from termbox2 (v2.5.0) so the two backends are semantically identical — the
 * TUI compares against the same TB_KEY / TB_MOD / color values on both.
 */

#ifndef OC_TB_COMPAT_H
#define OC_TB_COMPAT_H

#ifdef _WIN32
#  include "termbox2_win.h"
#else
   /* POSIX: the single-header library, implementation compiled into the TU that
    * defines OC_TB_IMPL (main.c), exactly as before this shim existed. */
#  ifdef OC_TB_IMPL
#    define TB_IMPL
#  endif
#  include "termbox2.h"
#endif

#endif /* OC_TB_COMPAT_H */
