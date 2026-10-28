/*
 * tuikit — terminal layer (tk_term). The toolbox is written against termbox2's
 * API, but termbox2 itself is POSIX-only (it drives /dev/tty via termios). So
 * this header routes: on Windows it pulls in tk_term_win.h, which re-declares the
 * exact subset of the termbox2 contract we use (implemented over the Windows
 * Console API in tk_term.c); on POSIX it pulls in termbox2.h for the declarations.
 *
 * Unlike the old tb_compat.h, this header carries NO implementation gate — the
 * single TB_IMPL instantiation now lives in tk_term.c, so any translation unit
 * can include tk_term.h (or tuikit.h) and just get declarations. The public
 * constants and struct tb_event layout are termbox2 v2.5.0 verbatim, so both
 * backends are semantically identical.
 */

#ifndef TK_TERM_H
#define TK_TERM_H

#ifdef _WIN32
#  include "tk_term_win.h"
#else
#  include "termbox2.h"
#endif

#endif /* TK_TERM_H */
