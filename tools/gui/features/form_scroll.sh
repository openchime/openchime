#!/usr/bin/env bash
# Edit profile's rows: measured as drawn, and scrolled when they do not fit.
#
#   tools/gui/features/form_scroll.sh          # run; the client and daemon stop at the end
#   tools/gui/features/form_scroll.sh --keep   # ... and leave them running
#
# A form's rows are measured (form_rowh) to size the card and to decide
# whether they scroll, and drawn (draw_form) where that measure put them. At
# each text size, Small to Larger, Edit profile is opened from the signed-in
# person's own profile and the window's height swept from 900 down to 480 at
# 1100 wide. At each height every drawn row lies within the height measured
# for it (the dump's `formrow` lines), and every field is drawn whole inside
# the form's body or not drawn at all, and when one is not drawn the rows
# scroll (`formscroll max` above 0). In the shortest window, at Small and at
# Larger, the wheel brings the last field into view, and so does Tab from the
# first field to the last; Esc closes the form from whichever field has the
# keyboard. Its own daemon on its own port, wiped each run.
#
# platforms: win32
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
. "$HERE/tools/gui/harness/lib.sh"
gui_begin form_scroll 9635 /tmp/oc-formscroll "$@"

NAMES=(Small Default Large Larger)
fs()   { field "$D" '^formscroll ' "$1"; }
card() { field "$D" '^closed_by=' card; }
nf()   { field "$D" '^form=' nfields; }

# Every drawn row within the height measured for it: label top to hint or box
# bottom, no taller than form_rowh (a row not drawn reads 0,0).
rows_fit() {
  awk '/^  formrow / {
         split($3, t, "="); split($4, b, "="); split($5, h, "=")
         if (!(t[2] == 0 && b[2] == 0) && b[2] - t[2] > h[2] + 0.01) { print "     row " $2 ": drawn " b[2] - t[2] " in " h[2]; bad = 1 }
       } END { exit bad }' "$D"
}
# Every field drawn whole inside the body, or not drawn; and one not drawn
# only when the rows scroll.
fields_whole() {
  awk -v body="$(fs body)" -v max="$(fs max)" '
       BEGIN { split(body, B, ",") }
       /^  formfield / {
         match($0, /r=[-0-9.]+,[-0-9.]+,[-0-9.]+,[-0-9.]+/); split(substr($0, RSTART + 2, RLENGTH - 2), r, ",")
         if (r[3] <= r[1]) { hidden = 1; next }
         if (r[2] < B[2] - 0.5 || r[4] > B[4] + 0.5) { print "     field " $2 ": " r[2] ".." r[4] " outside " B[2] ".." B[4]; bad = 1 }
       } END { if (hidden && max + 0 <= 0) { print "     a field is not drawn and max=" max; bad = 1 }; exit bad }' "$D"
}
# The last field drawn, inside the body.
last_shown() {
  local n r l t rr b bl bt br bb
  n=$(( $(nf) - 1 )); r="$(field "$D" "^  formfield $n " r)"
  IFS=, read -r l t rr b <<< "$r"; IFS=, read -r bl bt br bb <<< "$(fs body)"
  awk -v l="$l" -v t="$t" -v r="$rr" -v b="$b" -v bt="$bt" -v bb="$bb" \
      'BEGIN { exit !(r > l && t >= bt - 0.5 && b <= bb + 0.5) }'
}

open_form() {
  _btn() { gui_snap && [ -n "$(field "$D" '^  selfcardbtn cmd=53 ' r)" ]; }
  waitfor 10 _btn || gui_abort "the profile has no Edit profile button"
  local l t r b
  IFS=, read -r l t r b <<< "$(field "$D" '^  selfcardbtn cmd=53 ' r)"
  act clickform $(( (l + r) / 2 )) $(( (t + b) / 2 ))
  _open() { gui_snap && [ "$(field "$D" '^form=' form)" = 1 ] && [ -n "$(fs body)" ] && [ "$(nf)" -gt 1 ] 2>/dev/null; }
  waitfor 10 _open || gui_abort "Edit profile did not open"
}
close_form() {
  act key esc; act keyup esc
  _shut() { gui_snap && [ "$(field "$D" '^form=' form)" = 0 ]; }
  need "Esc closes Edit profile" waitfor 10 _shut
}
# A height, and a dump painted at it: the card is centred in the window, so
# its rect moves when the height does. A height the card cannot follow leaves
# it where it was, and the look after the wait is as good as any.
at_height() {
  local was; was="$(card)"
  act size 1100 "$1"
  _moved() { gui_snap && [ "$(card)" != "$was" ]; }
  waitfor 3 _moved || gui_snap
}
key() { act key "$1"; act keyup "$1"; }

step "the fixture"
gui_launch
act size 1100 900
ME="$(field "$D" '^me ' id)"
need "the signed-in person is known" [ -n "$ME" ]

for ts in 0 1 2 3; do
  step "${NAMES[$ts]} text: the rows from 900 down to 480"
  ok_ts() { gui_snap && [ "$(field "$D" '^textsize=' textsize)" = "$ts" ]; }
  act textsize "$ts"
  waitfor 5 ok_ts || gui_abort "the client did not take textsize $ts"
  act size 1100 900
  act profile "$ME"
  open_form
  bad_fit=""; bad_whole=""
  for h in $(seq 900 -20 480); do
    at_height "$h"
    rows_fit     || bad_fit="$bad_fit $h"
    fields_whole || bad_whole="$bad_whole $h"
  done
  check "every drawn row within its measured height${bad_fit:+ (not at$bad_fit)}" [ -z "$bad_fit" ]
  check "every field whole in the body or not drawn, and scrolled when not${bad_whole:+ (not at$bad_whole)}" [ -z "$bad_whole" ]

  if [ "$ts" = 0 ] || [ "$ts" = 3 ]; then
    step "${NAMES[$ts]} text, 480 high: the last field reached"
    IFS=, read -r bl bt br bb <<< "$(fs body)"
    cx=$(( (${bl%.*} + ${br%.*}) / 2 )); cy=$(( (${bt%.*} + ${bb%.*}) / 2 ))
    if ! last_shown; then
      need "the rows scroll" awk -v m="$(fs max)" 'BEGIN { exit !(m > 0) }'
      _wheeled() { act wheel "$cx" "$cy" -120; gui_snap && last_shown; }
      check "the wheel brings the last field into view" waitfor 10 _wheeled
      _top() { act wheel "$cx" "$cy" 1200; gui_snap && [ "$(fs scroll)" = 0.00 ]; }
      need "the wheel takes the rows back to the top" waitfor 10 _top
      need "the first field is drawn" click_rect "$(field "$D" '^  formfield 0 ' r)"
      _first() { gui_snap && grep -q '^formscroll scroll=0.00 ' "$D"; }
      waitfor 5 _first
      for _ in $(seq 2 "$(nf)"); do key tab; done
      _tabbed() { gui_snap && last_shown; }
      check "Tab to the last field brings it into view" waitfor 5 _tabbed
    else
      fail "at 480 high every field fits, so there is nothing to scroll to"
    fi
  fi
  close_form
done
gui_end
