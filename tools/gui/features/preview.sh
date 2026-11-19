#!/usr/bin/env bash
# The composer's Preview: the draft shown as the conversation will show it.
#
#   tools/gui/features/preview.sh          # run; the client and daemon stop at the end
#   tools/gui/features/preview.sh --keep   # ... and leave them running
#
# A draft with bold, a mention, a link and a table is previewed: the dump's
# `cpreview` line says the transcript's renderer laid it out, the table
# included, and the field is hidden. While it is shown, typing edits nothing
# and no frame is written (the dump's frames= count): a custom emoji with no
# image yet is not fetched, and the draft's save waits. Edit brings the field
# back with the caret where it was; previewing again, Enter sends the draft from
# the preview, which then closes. In plain text, the same button is there. Its
# own daemon on its own port, wiped each run.
#
# platforms: win32 web
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
. "$HERE/tools/gui/harness/lib.sh"
gui_begin preview 9535 /tmp/oc-preview "Preview Fixture" "$@"

pv() { field "$D" '^cpreview ' "$1"; }
ed() { field "$D" '^ed ' "$1"; }
msgs() { field "$D" '^  ch 1 ' msgs; }

step "a draft with bold, a mention, a link, a custom emoji and a table"
gui_launch
"$DRIVE" size 1200 820 >/dev/null
"$DRIVE" channel 1 >/dev/null || gui_abort "the client did not open the channel"
# In the catalogue, its image never drawn here, so not fetched yet.
mkdir -p "$LIN_DIR"
make_png "$LIN_DIR/em-prv.png" 32x32 f59e0b
"$DRIVE" emoji_add "prv $(cpath em-prv.png)" >/dev/null || gui_abort "the client did not take emoji_add"
catalogued() { gui_snap; grep '^cemoji=1$' "$D" >/dev/null; }
waitfor 30 catalogued || gui_abort "the custom emoji did not reach the catalogue"
"$DRIVE" editor rich >/dev/null
"$DRIVE" type "*Bold* for @alice :prv: see https://example.com/notes " >/dev/null
"$DRIVE" tablenew >/dev/null
"$DRIVE" tablecell 0 0 Item >/dev/null
"$DRIVE" tablecell 0 1 Qty >/dev/null
"$DRIVE" tablecell 1 0 Pear >/dev/null
"$DRIVE" tablecell 1 1 4 >/dev/null
"$DRIVE" tableok >/dev/null || gui_abort "the client did not take the table"
gui_snap
before_msgs="$(msgs)"; before_len="$(ed len)"; before_caret="$(ed caret)"
check "the draft is in the box ($before_len units, caret $before_caret)" [ "${before_len:-0}" -gt 0 ]
check "Preview is on the formatting row, and published" grep -q '^a11yitem composer.preview ' "$D"

# What the typing above sent (its typing notice) is written by the network
# thread up to half a second after the verb returned: read at once, frames=
# then moved under the preview with nothing of the preview's doing. So the
# count is taken once it has held for 0.75 s -- well inside the draft's 2 s
# debounce, whose save is what previewing must hold back.
quiet() {
  gui_snap
  local now=${EPOCHREALTIME/./}
  if [ "$(pv frames)" != "$QF" ]; then QF="$(pv frames)"; QT=$now; return 1; fi
  [ $((now - QT)) -ge 750000 ]
}
QF=""; QT=0
waitfor 5 quiet || gui_abort "the client's frames= never held still"
[ "$(pv draft)" = 1 ] || gui_abort "the draft was saved before it could be previewed (draft=$(pv draft))"

step "Preview shows it rendered"
"$DRIVE" preview >/dev/null || gui_abort "the client did not take preview"
shown() { gui_snap; [ "$(pv on)" = 1 ] && [ "$(pv lines)" -ge 1 ] && [ "$(pv tables)" = 1 ]; }
check "the renderer laid the draft out, its table included" waitfor 5 shown
"$DRIVE" shot preview_on >/dev/null
frames_on="$(pv frames)"
# The save must still be waiting as the preview opens, or there is nothing for
# it to hold back (a window that loses focus writes its draft at once).
[ "$(pv draft)" != 0 ] || gui_abort "the draft was written before the preview opened: nothing left to hold back"
wanted() { gui_snap; [ "$(pv held)" -ge 1 ]; }
check "the custom emoji's image is wanted, and not asked for" waitfor 5 wanted
"$DRIVE" chars "xyz" >/dev/null
"$DRIVE" key back >/dev/null 2>&1
# The save the typing above started comes due, and is held back.
# Due (2) and still unsaved: 0 would mean it was written after all.
settled() { gui_snap; [ "$(pv draft)" = 2 ]; }
check "the draft's save comes due" waitfor 5 settled
check "typing edits nothing while previewing" [ "$(ed len)" = "$before_len" ]
check "and nothing was posted" [ "$(msgs)" = "$before_msgs" ]
check "and no frame was written ($frames_on, $(pv frames))" [ "$(pv frames)" = "$frames_on" ]

step "Edit brings the field back as it was"
"$DRIVE" preview >/dev/null
back() { gui_snap;  [ "$(pv on)" = 0 ] && [ "$(ed caret)" = "$before_caret" ] && [ "$(ed len)" = "$before_len" ]; }
check "the field is back, its caret where it was" waitfor 5 back

step "Enter sends from the preview"
"$DRIVE" preview >/dev/null
on_again() { gui_snap; [ "$(pv on)" = 1 ]; }
check "previewing again" waitfor 5 on_again
"$DRIVE" key enter >/dev/null
sent() { gui_snap; [ "$(msgs)" -gt "${before_msgs:-0}" ] && [ "$(pv on)" = 0 ] && [ "$(ed len)" = 0 ]; }
check "the draft is sent, the box is empty, and the preview has closed" waitfor 10 sent
check "what was sent is the draft" grep -q '^  ch 1 .*prev="[^"]*Bold[^"]*alice' "$D"
check "and the send is in the count: it counts what is written" [ "$(pv frames)" -gt "$frames_on" ]

step "in plain text, the same button"
"$DRIVE" editor plain >/dev/null
"$DRIVE" type "plain *draft*" >/dev/null
btn() { gui_snap; local b; b="$(pv btn)"; [ -n "$b" ] && [ "$b" != "0,0,0,0" ]; }
check "Preview is drawn" waitfor 5 btn
check "and published" grep -q '^a11yitem composer.preview ' "$D"
press_btn() { click_rect "$(pv btn)"; }
press_btn
plain_on() { gui_snap; [ "$(pv on)" = 1 ] && [ "$(pv lines)" -ge 1 ]; }
check "its click shows the draft rendered" waitfor 5 plain_on
press_btn
plain_off() { gui_snap; [ "$(pv on)" = 0 ]; }
check "and its click again brings the field back" waitfor 5 plain_off
rm -f "$LIN_DIR/em-prv.png"
gui_end
