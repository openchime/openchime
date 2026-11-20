#!/usr/bin/env bash
# Deleting a draft from Drafts, scheduled & sent (REQ-223, REQ-228), driven and
# observed in the Win32 client.
#
#   tools/gui/features/drafts.sh          # run; the client and daemon stop at the end
#   tools/gui/features/drafts.sh --keep   # ... and leave them running
#
# It asserts that
#
#   - each draft's row carries a Delete button, published to assistive technology;
#   - Delete removes the draft at once, and its notice offers Undo, which puts
#     it back -- a draft is writing, and the safeguard is taking the delete back,
#     not a question asked before every one;
#   - deleted, the draft is gone -- from the list, the count and the sidebar --
#     and it stays gone: the composer, which still holds that conversation's text
#     while the Drafts pane is up, does not write it back on the way out;
#   - the row's context menu offers the same Delete, which does the same;
#   - every context menu -- a draft's, a message's, a sidebar conversation's, a
#     member's -- keeps the row it was opened on lit while it is open, and only
#     while: read from the screen, since the pointer moving onto the menu is
#     exactly what used to take the row's highlight away.
#
# Its own daemon on its own port, wiped each run.
#
# platforms: win32
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
. "$HERE/tools/gui/harness/lib.sh"
gui_begin drafts 9560 /tmp/oc-drafts "Drafts Fixture" "$@"

key() { grep -o "\b$1=[^ ]*" "$D" | head -1 | cut -d= -f2; }
key_is() { gui_snap; [ "$(key "$1")" = "$2" ]; }
waitkey() { waitfor "${3:-6}" key_is "$1" "$2"; }  # waitkey <key> <want> [secs]
has_aid() { grep -q "^a11yitem $1 " "$D"; }
# The conversation of Drafts row `i`.
row_cid() { grep -m1 '^draftrows ' "$D" | tr ' ' '\n' | grep '@' | sed -n "$(( $1 + 1 ))p" | cut -d: -f1; }

step "two drafts"
gui_launch
"$DRIVE" size 1100 800 >/dev/null
"$DRIVE" mkchan quiet >/dev/null
waitkey channels 2 10 || gui_abort "the second conversation was not made"
"$DRIVE" channel 2 >/dev/null || gui_abort "the client did not open quiet"
"$DRIVE" typekeys "the draft in quiet" >/dev/null || gui_abort "the client did not take quiet's draft"
"$DRIVE" channel 1 >/dev/null              # leaving quiet writes its draft
"$DRIVE" typekeys "the draft in general" >/dev/null || gui_abort "the client did not take general's draft"
"$DRIVE" channel 2 >/dev/null              # ...and leaving general writes its own
"$DRIVE" channel 1 >/dev/null              # back in general: the composer holds its draft
check "both are drafts" waitkey draftn 2
"$DRIVE" view drafts >/dev/null || gui_abort "the client did not open Drafts"
both_published() { gui_snap; has_aid drafts.delete.0 && has_aid drafts.delete.1; }
check "each row has a Delete button, published" waitfor 5 both_published
"$DRIVE" shot drafts_rows >/dev/null

step "Delete removes it at once; Undo puts it back"
# Find general's row: it is the one the composer still holds.
gi=0; [ "$(row_cid 0)" = 1 ] || gi=1
# The newest notice, and the action it offers.
toast_now() { gui_snap; grep -E '^toast\[[0-9]+\] ' "$D" | tail -1 | grep -F "action=\"$2\" \"$1\"" >/dev/null; }
toast_says() { waitfor 3 toast_now "$1" "$2"; }  # toast_says <text> <action>
press "drafts.delete.$gi" || gui_abort "general's row has no Delete to click"
check "Delete removes it, with no question first" waitkey draftn 1
check "...and nothing is asked" waitkey modal none
check "its notice says so and offers Undo" toast_says "Draft deleted." "Undo"
"$DRIVE" shot drafts_deleted >/dev/null
"$DRIVE" key ctrl+shift+z >/dev/null       # the newest notice's action
check "Undo puts it back" waitkey draftn 2
restored() { gui_snap; [ "$(row_cid 0)" = 1 ] || [ "$(row_cid 1)" = 1 ]; }
check "...general's, as it was" waitfor 5 restored
check "...and says so" toast_says "Draft restored." ""

step "deleted, it is gone and stays gone"
gui_snap
gi=0; [ "$(row_cid 0)" = 1 ] || gi=1
press "drafts.delete.$gi" || gui_abort "general's row has no Delete to click"
check "the list has one draft left" waitkey draftn 1
remaining() { gui_snap; [ "$(row_cid 0)" = 2 ]; }
check "...and it is quiet's" waitfor 5 remaining
"$DRIVE" view home >/dev/null
"$DRIVE" channel 1 >/dev/null
gui_snap
emptied() { [ "$(field "$D" '^ed ' len)" = 0 ]; }
check "general's composer is empty, not holding the deleted text" emptied
check "general is not marked as having a draft" waitkey drafthere 0
"$DRIVE" channel 2 >/dev/null              # leaving general must not write it back
# The write-back under test would show as a second draft; nothing marks that it
# will not come, so it is given a second to.
sleep 1  # paced: the time a write-back has to show
check "leaving general did not write the draft back" key_is draftn 1

step "the row's context menu"
"$DRIVE" view drafts >/dev/null
row_drawn() { gui_snap; grep -m1 '^draftrows ' "$D" | grep '@' >/dev/null; }
waitfor 5 row_drawn || gui_abort "the Drafts pane shows no row"
IFS=, read -r l t r b <<< "$(grep -m1 '^draftrows ' "$D" | tr ' ' '\n' | grep '@' | head -1 | cut -d@ -f2)"
"$DRIVE" rclick $(( l - 200 )) $(( (t + b) / 2 )) >/dev/null
menu_on() { gui_snap; [ "$(key menu)" != 0 ] && [ "$(field "$D" '^draftrows ' menu_cid)" = 2 ]; }
check "right-clicking quiet's row opens its menu" waitfor 5 menu_on
"$DRIVE" shot drafts_menu >/dev/null
"$DRIVE" key esc >/dev/null
"$DRIVE" menu 2601 >/dev/null              # the menu's "Delete draft"
check "its Delete removes it the same way: no drafts are left" waitkey draftn 0
check "...asking nothing" waitkey modal none
check "...and offering Undo" toast_says "Draft deleted." "Undo"

step "a context menu keeps its row lit"
# The notices the deletes raised are dismissed first: one still up sits over the
# rows this part right-clicks and reads.
no_toasts() { gui_snap; ! grep -qE '^toast\[[0-9]+\] ' "$D"; }
for _ in 1 2 3 4; do no_toasts && break; "$DRIVE" key esc >/dev/null; done
no_toasts || gui_abort "the notices did not go: they would cover the rows"
# The colour at (x, y) in a fresh screenshot (lib.sh's bmp_px reads it).
px() { "$DRIVE" shot drafts_px >/dev/null && bmp_px "$LIN_DIR/drafts_px.bmp" "$1" "$2"; }   # px <x> <y>
# The pixel, read until it settles: two shots in a row the same.
PX=""
px_settled() { local a; a="$(px "$1" "$2")"; [ -n "$a" ] && [ "$a" = "$PX" ]; local r=$?; PX="$a"; return $r; }
px_not() { PX="$(px "$1" "$2")"; [ -n "$PX" ] && [ "$PX" != "$3" ]; }
px_is() { PX="$(px "$1" "$2")"; [ "$PX" = "$3" ]; }
menu_open() { gui_snap; [ "$(key menu)" != 0 ]; }
menu_shut() { gui_snap; [ "$(key menu)" = 0 ]; }
# Right-click at (rx, ry), move onto the menu as a person does, read (sx, sy);
# then Esc and read it again. Lit while open, back as it was after.
lit_while_open() {  # lit_while_open <label> <rx> <ry> <sx> <sy>
  local base open closed
  "$DRIVE" move 700 250 >/dev/null; PX=""; waitfor 3 px_settled "$4" "$5"; base="$PX"
  "$DRIVE" move "$2" "$3" >/dev/null; "$DRIVE" rclick "$2" "$3" >/dev/null
  waitfor 3 menu_open
  # Onto the menu, and then away from both it and the row: where the pointer
  # happens to rest must not decide whether the row says what the menu is for.
  "$DRIVE" move $(( $2 + 30 )) $(( $3 + 40 )) >/dev/null
  "$DRIVE" move 700 250 >/dev/null; waitfor 2 px_not "$4" "$5" "$base"; open="$PX"
  "$DRIVE" key esc >/dev/null; "$DRIVE" move 700 250 >/dev/null
  waitfor 3 menu_shut; waitfor 2 px_is "$4" "$5" "$base"; closed="$PX"
  checks=$((checks + 1))
  if [ -n "$base" ] && [ "$open" != "$base" ] && [ "$closed" = "$base" ]; then ok "$1 ($base, $open with its menu, $closed after)"
  else fail "$1 -- $base, then $open with its menu, then $closed after"; fi
}
"$DRIVE" view home >/dev/null
"$DRIVE" channel 1 >/dev/null
send_stored "a message to right-click"
rowed() { gui_snap && grep '^  msgrow ' "$D" >/dev/null; }
waitfor 10 rowed || gui_abort "the message came back and no row was drawn for it"
by="$(field "$D" '^  msgrow ' body | cut -d, -f2)"
# Each row is right-clicked near its right end and read at its left edge: a menu
# opens rightward from the pointer, and reading under it would read the menu. The
# message's right end is also where the "Draft deleted" toast from a moment ago sits.
mx0="$(field "$D" '^msgrows ' x)"; mx0="${mx0%%.*}"   # x=<left>..<right>
lit_while_open "a message" 700 "$by" $(( mx0 + 8 )) $(( by + 10 ))
read -r l t r b <<< "$(grep -m1 '^a11yitem conv.2 ' "$D" | cut -d' ' -f3-)"
lit_while_open "a sidebar conversation" $(( r - 30 )) $(( (t + b) / 2 )) $(( l + 12 )) $(( (t + b) / 2 ))
IFS=, read -r l t r b <<< "$(grep '^memrow ' "$D" | sed -n 2p | grep -o 'r=[0-9,]*' | cut -d= -f2)"
lit_while_open "a member" $(( r - 30 )) $(( (t + b) / 2 )) $(( l + 8 )) $(( (t + b) / 2 ))
"$DRIVE" channel 2 >/dev/null
"$DRIVE" typekeys "another draft" >/dev/null
"$DRIVE" channel 1 >/dev/null
"$DRIVE" view drafts >/dev/null
drafts_row() { gui_snap && grep '^a11yitem drafts.row.0 ' "$D" >/dev/null; }
waitfor 5 drafts_row || gui_abort "the Drafts pane shows no row for the new draft"
read -r l t r b <<< "$(grep -m1 '^a11yitem drafts.row.0 ' "$D" | cut -d' ' -f3-)"
lit_while_open "a draft" $(( r - 150 )) $(( (t + b) / 2 )) $(( l + 8 )) $(( (t + b) / 2 ))
gui_end
