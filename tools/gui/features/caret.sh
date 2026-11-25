#!/usr/bin/env bash
# The Win32 client's system caret (REQ-269, ARCH-99), driven and observed.
#
#   tools/gui/features/caret.sh          # run; the client and daemon stop at the end
#   tools/gui/features/caret.sh --keep   # ... and leave them running
#
# The client draws its own caret; screen readers and magnifiers follow the
# system caret, which it creates and moves but never shows. Being invisible by
# design, it is read from the dump's `caret` line: `owned= x= y=` is the
# client's record of where it put it, `sys= sx= sy= sh=` what Windows says the
# thread's caret is. It asserts that:
#
#   - with the message box focused there is a system caret, at the drawn one;
#   - it follows the caret as text is typed, and as the arrow keys move it;
#   - it moves to another drawn field (the search box) when that takes focus,
#     and back to the message box when Escape leaves the search box;
#   - with no field focused (the Files view), there is none.
#
# Its own daemon on its own port, wiped each run.
#
# platforms: win32
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
. "$HERE/tools/gui/harness/lib.sh"
gui_begin caret 9600 /tmp/oc-caret "Caret Fixture" "$@"

ck() { field "$D" '^caret ' "$1"; }
ed_key() { field "$D" '^ed ' "$1"; }
srch() { field "$D" '^searchbox srch ' "$1"; }
# The system caret exists, where the client put it, and is a line tall.
at_drawn() { [ "$(ck owned)" = 1 ] && [ "$(ck sys)" = 1 ] && [ "$(ck sx)" = "$(ck x)" ] && [ "$(ck sy)" = "$(ck y)" ] && [ "$(ck sh)" -gt 1 ]; }
seen_caret() { say "     caret: $(grep -m1 '^caret ' "$D")"; }

step "the message box focused"
gui_launch
"$DRIVE" size 1100 800 >/dev/null
"$DRIVE" channel 1 >/dev/null || gui_abort "the client did not open the channel"
composer_now() { gui_snap; [ "$(ed_key focus)" = 1 ] && at_drawn; }
composer() { waitfor 6 composer_now || { seen_caret; return 1; }; }
check "a system caret, at the drawn caret" composer
x0="$(ck sx)"; y0="$(ck sy)"

step "typing, and the arrow keys"
"$DRIVE" typekeys "where the caret goes" >/dev/null || gui_abort "the client did not take the keys"
typed_now() { gui_snap; [ "$(ed_key len)" -gt 0 ] && at_drawn && [ "$(ck sx)" -gt "$x0" ]; }
typed() { waitfor 6 typed_now || { seen_caret; return 1; }; }
check "it follows the caret as text is typed" typed
x1="$(ck sx)"
"$DRIVE" key left >/dev/null
"$DRIVE" key left >/dev/null
"$DRIVE" key left >/dev/null
left_now() { gui_snap; at_drawn && [ "$(ck sx)" -lt "$x1" ] && [ "$(ck sy)" = "$y0" ]; }
left() { waitfor 6 left_now || { seen_caret; return 1; }; }
check "Left moves it back with the caret" left
x2="$(ck sx)"
"$DRIVE" key right >/dev/null
right_now() { gui_snap; at_drawn && [ "$(ck sx)" -gt "$x2" ] && [ "$(ck sx)" -lt "$x1" ]; }
right() { waitfor 6 right_now || { seen_caret; return 1; }; }
check "Right moves it on with the caret" right

step "another field takes focus: the search box"
"$DRIVE" search >/dev/null || gui_abort "the client did not open search"
# Inside the search box's drawn text (its l,t,r,b, device pixels), not the composer's.
in_srch() {
  local l t r b; IFS=, read -r l t r b <<< "$(srch edit)"
  [ -n "$b" ] && [ "$(ck sx)" -ge "${l%.*}" ] && [ "$(ck sx)" -le "${r%.*}" ] &&
    [ "$(ck sy)" -ge "${t%.*}" ] && [ "$(ck sy)" -lt "${b%.*}" ]
}
search_now() { gui_snap; [ "$(srch vis)" = 1 ] && [ "$(ed_key focus)" = 0 ] && at_drawn && in_srch; }
search() { waitfor 6 search_now || { seen_caret; say "     $(grep -m1 '^searchbox srch ' "$D")"; return 1; }; }
check "the system caret moves to the search box" search
"$DRIVE" shot caret_search >/dev/null

step "Escape: back to the message box"
"$DRIVE" key esc >/dev/null
# Escape gives the keyboard back to the message box, and the caret goes with it.
back_now() { gui_snap; [ "$(srch vis)" = 0 ] && [ "$(ed_key focus)" = 1 ] && at_drawn && ! in_srch; }
back() { waitfor 6 back_now || { seen_caret; return 1; }; }
check "Escape out of the search box: the system caret is back in the message box" back

step "no field focused"
"$DRIVE" view files >/dev/null || gui_abort "the client did not open Files"
none_now() { gui_snap; [ "$(ed_key focus)" = 0 ] && [ "$(ck owned)" = 0 ] && [ "$(ck sys)" = 0 ]; }
none() { waitfor 6 none_now || { seen_caret; return 1; }; }
check "in Files, with no field focused: no system caret" none
gui_end
