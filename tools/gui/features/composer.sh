#!/usr/bin/env bash
# The Win32 composer's layout (REQ-220, REQ-223), driven and observed.
#
#   tools/gui/features/composer.sh          # run; the client and daemon stop at the end
#   tools/gui/features/composer.sh --keep   # ... and leave them running
#
# It asserts that the message box is always tall enough for its text, however the
# text got there:
#
#   - a draft long enough to wrap, saved, is restored at the right height when the
#     client starts again -- not laid out for one line and clipped;
#   - leaving for a conversation with no draft shrinks the box to one line, and
#     coming back grows it again;
#   - a narrower or wider window re-wraps it, and the box follows;
#   - the formatting row and the action row are built to one measure -- the same
#     button size, the same left edge, the same pitch -- and every action button,
#     rested on, shows a tooltip saying what it is, as the formatting buttons do.
#
# "Tall enough" is read from the dump's `ed` line: `lines=` is how many lines the
# text wraps to at the width it is drawn at, `fit_lines=` how many the box holds.
# The box grows to four lines and then scrolls, so it must hold min(lines, 4).
#
# Its own daemon on its own port, wiped each run.
#
# platforms: win32
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
. "$HERE/tools/gui/harness/lib.sh"
gui_begin composer 9540 /tmp/oc-composer "Composer Fixture" "$@"

ed_key() { field "$D" '^ed ' "$1"; }
MAX_LINES=4                                 # the box's tallest; past it the text scrolls
# The box holds its text, with `lines` at least <min>: fits_now looks once,
# fits waits up to six seconds and says what it saw last.
l=""; f=""
fits_now() {  # fits_now <min lines>
  local want
  gui_snap; l="$(ed_key lines)"; f="$(ed_key fit_lines)"
  want="$l"; [ -n "$l" ] && [ "$l" -gt "$MAX_LINES" ] && want="$MAX_LINES"
  [ -n "$l" ] && [ "$l" -ge "$1" ] && [ "$want" = "$f" ]
}
fits() { waitfor 6 fits_now "$1" || { say "     lines=${l:-?} fit_lines=${f:-?}"; return 1; }; }
# The box's drawn width, from its l,t,r,b.
box_w() { local bl bt br bb; IFS=, read -r bl bt br bb <<< "$(ed_key box)"; echo $(( ${br%.*} - ${bl%.*} )); }

LONG="A draft long enough to wrap across several lines of the message box, so that the box has to be measured for more than one line when it comes back after the client starts again, which is when it used to be laid out for one line and clipped."

step "a long draft, saved"
gui_launch
"$DRIVE" size 1100 800 >/dev/null
"$DRIVE" mkchan quiet >/dev/null           # channel 2: a conversation with no draft
two() { gui_snap && grep '^channels=2' "$D" >/dev/null; }
waitfor 10 two || gui_abort "the second conversation was not made"
"$DRIVE" channel 1 >/dev/null || gui_abort "the client did not open the channel"
"$DRIVE" typekeys "$LONG" >/dev/null || gui_abort "the client did not take the draft"
check "typed, it wraps and the box holds it" fits 3
"$DRIVE" channel 2 >/dev/null              # leaving writes the draft
one_now() { fits_now 1 && [ "$l" = 1 ]; }
one() { waitfor 6 one_now || { say "     lines=${l:-?} fit_lines=${f:-?}"; return 1; }; }
check "in a conversation with no draft, the box is one line" one

step "the client starts again"
OC_TEST_NO_BUILD=1 OC_TEST_NO_DAEMON=1 gui_launch
"$DRIVE" size 1100 800 >/dev/null
"$DRIVE" channel 1 >/dev/null || gui_abort "the client did not open the channel"
restored_now() { fits_now 3 && [ "$(ed_key len)" -gt 100 ] 2>/dev/null; }
restored() { waitfor 6 restored_now || { say "     len=$(ed_key len) lines=${l:-?} fit_lines=${f:-?}"; return 1; }; }
check "the draft comes back at its height, not clipped to one line" restored
"$DRIVE" shot composer_restored >/dev/null

step "away and back, narrower and wider"
"$DRIVE" channel 2 >/dev/null
check "away: one line" one
"$DRIVE" channel 1 >/dev/null
check "back: the draft's height again" fits 3
w0="$(box_w)"
"$DRIVE" size 900 800 >/dev/null
# Each width read from a dump laid out at it -- the box a different width than
# at the last size (narrower windows drop the side panes, so it can be wider) --
# not the last size's dump read again before the client has re-wrapped.
narrower_now() { fits_now 3 && [ "$(box_w)" != "$w0" ]; }
narrower() { waitfor 6 narrower_now || { say "     lines=${l:-?} fit_lines=${f:-?} w=$(box_w) (was $w0)"; return 1; }; }
check "narrower, the box follows the re-wrap" narrower
n1="$l"; w1="$(box_w)"
"$DRIVE" size 1500 800 >/dev/null
wider_now() { fits_now 2 && [ "$(box_w)" -gt "$w1" ] && [ "$l" -lt "${n1:-0}" ]; }
wider() { waitfor 6 wider_now || { say "     lines=${l:-?} (was ${n1:-?}) fit_lines=${f:-?} w=$(box_w) (was $w1)"; return 1; }; }
check "wider, fewer lines, and the box follows" wider
"$DRIVE" size 1100 800 >/dev/null

step "the two rows of buttons"
"$DRIVE" channel 2 >/dev/null
gui_snap
# fmtbar: seven l,t,r,b after hover=; actrow: thirteen after tip="..." (ACT_* order).
fmt=( $(grep -m1 '^fmtbar ' "$D" | cut -d' ' -f3-) )
act=( $(grep -m1 '^actrow ' "$D" | sed 's/.*tip="[^"]*" //') )
w_of() { local l t r b; IFS=, read -r l t r b <<< "$1"; echo $((r - l)); }
l_of() { echo "${1%%,*}"; }
same_size() { [ "$(w_of "${fmt[0]:-}")" = "$(w_of "${act[0]:-}")" ] && [ "$(w_of "${fmt[0]:-}")" -gt 0 ]; }
same_left() { [ -n "${fmt[0]:-}" ] && [ "$(l_of "${fmt[0]}")" = "$(l_of "${act[0]:-}")" ]; }
# The action row's pitch between drawn buttons (the first four ACT_* that exist)
# against the formatting row's first four.
same_pitch() {
  local fp=$(( $(l_of "${fmt[1]:-0}") - $(l_of "${fmt[0]:-0}") )) prev="" i ok=1 n=0
  for i in 0 1 2 3 4 5; do
    [ "$(w_of "${act[$i]:-}")" -gt 0 ] || continue
    local x; x="$(l_of "${act[$i]}")"
    [ -n "$prev" ] && { [ $((x - prev)) = "$fp" ] || ok=0; n=$((n + 1)); }
    prev="$x"
  done
  [ "$ok" = 1 ] && [ "$n" -ge 2 ]
}
check "the two rows' buttons are one size (${fmt[0]:-} vs ${act[0]:-})" same_size
check "...start at one left edge" same_left
check "...and step at one pitch" same_pitch
# Rest on each drawn action button in turn: its tooltip must name it.
NAMES=("Attach a file" "Record a video message" "Emoji" "Mention someone" "Hold to talk" "Free talk" "Send" "Send later")
tip_is() { gui_snap && grep -m1 '^actrow ' "$D" | grep "tip=\"$1\"" >/dev/null; }
for i in 0 1 2 3 4 5 6 7; do
  wd="$(w_of "${act[$i]:-}")"; [ "$wd" -gt 0 ] || continue
  IFS=, read -r l t r b <<< "${act[$i]}"
  "$DRIVE" move $(( (l + r) / 2 )) $(( (t + b) / 2 )) >/dev/null
  check "resting on it, the ${NAMES[$i]} button says so" waitfor 3 tip_is "${NAMES[$i]}"
done
IFS=, read -r l t r b <<< "${fmt[0]:-0,0,0,0}"
"$DRIVE" move $(( (l + r) / 2 )) $(( (t + b) / 2 )) >/dev/null
check "and the formatting row still does (Bold)" waitfor 3 tip_is "Bold"
"$DRIVE" move 10 10 >/dev/null
check "away from them, no tooltip" waitfor 3 tip_is ""
"$DRIVE" shot composer_rows >/dev/null
gui_end
