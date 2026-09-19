#!/usr/bin/env bash
# Deleting a draft from Drafts, scheduled & sent (REQ-223, REQ-228), driven and
# observed in the Win32 client.
#
#   scripts/gui_drafts.sh          # run, and leave the client running
#   scripts/gui_drafts.sh --kill   # ... and shut it down afterwards
#
# It asserts that
#
#   - each draft's row carries a Delete button, published to assistive technology;
#   - Delete asks first, and Cancel keeps the draft;
#   - confirmed, the draft is gone -- from the list, the count and the sidebar --
#     and it stays gone: the composer, which still holds that conversation's text
#     while the Drafts pane is up, does not write it back on the way out;
#   - the row's context menu offers the same, and asks the same;
#   - every context menu -- a draft's, a message's, a sidebar conversation's, a
#     member's -- keeps the row it was opened on lit while it is open, and only
#     while: read from the screen, since the pointer moving onto the menu is
#     exactly what used to take the row's highlight away.
#
# Its own daemon on its own port, wiped each run.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DRIVE="$HERE/scripts/gui_drive.sh"
LIN_DIR='/mnt/c/Windows/Temp/octest'
START=$SECONDS
fails=0
checks=0

export OC_DEV_PORT="${OC_DEV_PORT:-9560}"
export OC_DEV_DIR="${OC_DEV_DIR:-/tmp/oc-drafts}"
export OC_DEV_WS="${OC_DEV_WS:-Drafts Fixture}"

say()  { printf '%s\n' "$*"; }
fail() { printf '  FAIL %s\n' "$*"; fails=$((fails + 1)); }
ok()   { printf '  ok   %s\n' "$*"; }
check() { local what="$1"; shift; checks=$((checks + 1)); if "$@"; then ok "$what"; else fail "$what"; fi; }

# Stop this port's daemon before wiping its directory: SQLite would keep writing
# to the unlinked files. Found by its environment, which is where the port is.
kill_dev_daemon() {
  local p
  for p in $(pgrep -x openchimed 2>/dev/null); do
    tr '\0' '\n' < "/proc/$p/environ" 2>/dev/null |
      grep -qx "OPENCHIME_PROTO_PORT=$OC_DEV_PORT" && kill "$p" 2>/dev/null
  done
  for _ in 1 2 3 4 5 6 7 8 9 10; do
    (exec 3<>/dev/tcp/127.0.0.1/"$OC_DEV_PORT") 2>/dev/null || return 0
    exec 3<&- 3>&-; sleep 0.3
  done
  return 1
}
kill_dev_daemon || { say "a daemon still holds :$OC_DEV_PORT; stop it and re-run"; exit 1; }
rm -rf "$OC_DEV_DIR"

D="/tmp/oc-drafts-dump.txt"
snap() { "$DRIVE" dump drafts >/dev/null 2>&1; cp "$LIN_DIR/drafts.txt" "$D" 2>/dev/null; }
key() { grep -o "\b$1=[^ ]*" "$D" | head -1 | cut -d= -f2; }
waitkey() {  # waitkey <key> <want> [secs]
  local t=0 n=$(( ${3:-6} * 4 ))
  while [ "$t" -lt "$n" ]; do snap; [ "$(key "$1")" = "$2" ] && return 0; sleep 0.25; t=$((t + 1)); done
  return 1
}
# Click the centre of an accessibility item, by its id: its rect is in device
# pixels, the click verb takes DIPs.
click_aid() {
  local r dpi l t rr b
  r="$(grep -m1 "^a11yitem $1 " "$D" | cut -d' ' -f3-)"; [ -n "$r" ] || return 1
  dpi="$(key dpi)"; [ -n "$dpi" ] || dpi=96
  read -r l t rr b <<< "$r"
  "$DRIVE" click $(( (l + rr) / 2 * 96 / dpi )) $(( (t + b) / 2 * 96 / dpi )) >/dev/null
}
has_aid() { grep -q "^a11yitem $1 " "$D"; }
# The conversation of Drafts row `i`.
row_cid() { grep -m1 '^draftrows ' "$D" | tr ' ' '\n' | grep '@' | sed -n "$(( $1 + 1 ))p" | cut -d: -f1; }

say "== two drafts"
"$DRIVE" launch >/dev/null 2>&1 || { say "launch failed"; exit 1; }
waitkey authed 1 20 || { say "never signed in"; exit 1; }
"$DRIVE" size 1100 800 >/dev/null
"$DRIVE" mkchan quiet >/dev/null
waitkey channels 2 10 >/dev/null
"$DRIVE" channel 2 >/dev/null
"$DRIVE" typekeys "the draft in quiet" >/dev/null
"$DRIVE" channel 1 >/dev/null              # leaving quiet writes its draft
"$DRIVE" typekeys "the draft in general" >/dev/null
"$DRIVE" channel 2 >/dev/null              # ...and leaving general writes its own
"$DRIVE" channel 1 >/dev/null              # back in general: the composer holds its draft
check "both are drafts" waitkey draftn 2
"$DRIVE" view drafts >/dev/null
snap
check "each row has a Delete button, published" bash -c "grep -q '^a11yitem drafts.delete.0 ' $D && grep -q '^a11yitem drafts.delete.1 ' $D"
"$DRIVE" shot drafts_rows >/dev/null

say "== Delete asks first; Cancel keeps it"
# Find general's row: it is the one the composer still holds.
gi=0; [ "$(row_cid 0)" = 1 ] || gi=1
click_aid "drafts.delete.$gi"
check "Delete opens a confirmation" waitkey modal confirm
"$DRIVE" shot drafts_confirm >/dev/null
snap; click_aid modal.button.cancel
check "Cancel closes it" waitkey modal none
check "...and the draft is still there" waitkey draftn 2

say "== confirmed, it is gone and stays gone"
snap; click_aid "drafts.delete.$gi"
waitkey modal confirm >/dev/null
snap; click_aid modal.button.delete
check "the list has one draft left" waitkey draftn 1
remaining() { snap; [ "$(row_cid 0)" = 2 ]; }
check "...and it is quiet's" remaining
"$DRIVE" view home >/dev/null
"$DRIVE" channel 1 >/dev/null
snap
emptied() { [ "$(grep -m1 '^ed ' "$D" | grep -o 'len=[0-9]*' | cut -d= -f2)" = 0 ]; }
check "general's composer is empty, not holding the deleted text" emptied
check "general is not marked as having a draft" waitkey drafthere 0
"$DRIVE" channel 2 >/dev/null              # leaving general must not write it back
sleep 1
check "leaving general did not write the draft back" waitkey draftn 1

say "== the row's context menu"
"$DRIVE" view drafts >/dev/null
snap
IFS=, read -r l t r b <<< "$(grep -m1 '^draftrows ' "$D" | tr ' ' '\n' | grep '@' | head -1 | cut -d@ -f2)"
"$DRIVE" rclick $(( l - 200 )) $(( (t + b) / 2 )) >/dev/null
menu_on() { snap; [ "$(key menu)" != 0 ] && [ "$(grep -m1 '^draftrows ' "$D" | grep -o 'menu_cid=[0-9]*' | cut -d= -f2)" = 2 ]; }
check "right-clicking quiet's row opens its menu" menu_on
"$DRIVE" shot drafts_menu >/dev/null
"$DRIVE" key esc >/dev/null
"$DRIVE" menu 2601 >/dev/null              # the menu's "Delete draft…"
check "its Delete asks too" waitkey modal confirm
snap; click_aid modal.button.delete
check "confirmed, no drafts are left" waitkey draftn 0
say "== a context menu keeps its row lit"
# The colour at (x, y) in a fresh screenshot.
px() {  # px <x> <y>
  "$DRIVE" shot drafts_px >/dev/null
  python3 -c "from PIL import Image; im = Image.open('$LIN_DIR/drafts_px.bmp').convert('RGB'); print('%02X%02X%02X' % im.getpixel(($1, $2)))"
}
# Right-click at (rx, ry), move onto the menu as a person does, read (sx, sy);
# then Esc and read it again. Lit while open, back as it was after.
lit_while_open() {  # lit_while_open <label> <rx> <ry> <sx> <sy>
  local base open closed
  "$DRIVE" move 700 250 >/dev/null; sleep 0.3; base="$(px "$4" "$5")"
  "$DRIVE" move "$2" "$3" >/dev/null; "$DRIVE" rclick "$2" "$3" >/dev/null
  # Onto the menu, and then away from both it and the row: where the pointer
  # happens to rest must not decide whether the row says what the menu is for.
  "$DRIVE" move $(( $2 + 30 )) $(( $3 + 40 )) >/dev/null
  "$DRIVE" move 700 250 >/dev/null; sleep 0.4; open="$(px "$4" "$5")"
  "$DRIVE" key esc >/dev/null; "$DRIVE" move 700 250 >/dev/null; sleep 0.4; closed="$(px "$4" "$5")"
  checks=$((checks + 1))
  if [ "$open" != "$base" ] && [ "$closed" = "$base" ]; then ok "$1 ($base, $open with its menu, $closed after)"
  else fail "$1 -- $base, then $open with its menu, then $closed after"; fi
}
"$DRIVE" view home >/dev/null
"$DRIVE" channel 1 >/dev/null
"$DRIVE" send "a message to right-click" >/dev/null
sleep 1; snap
by="$(grep -m1 '^  msgrow ' "$D" | grep -o 'body=[0-9,]*' | cut -d, -f2)"
# Each row is right-clicked near its right end and read at its left edge: a menu
# opens rightward from the pointer, and reading under it would read the menu. The
# message's right end is also where the "Draft deleted" toast from a moment ago sits.
mx0="$(grep -m1 '^msgrows ' "$D" | grep -o 'x=[0-9]*' | cut -d= -f2)"
lit_while_open "a message" 700 "$by" $(( mx0 + 8 )) $(( by + 10 ))
read -r l t r b <<< "$(grep -m1 '^a11yitem conv.2 ' "$D" | cut -d' ' -f3-)"
lit_while_open "a sidebar conversation" $(( r - 30 )) $(( (t + b) / 2 )) $(( l + 12 )) $(( (t + b) / 2 ))
IFS=, read -r l t r b <<< "$(grep '^memrow ' "$D" | sed -n 2p | grep -o 'r=[0-9,]*' | cut -d= -f2)"
lit_while_open "a member" $(( r - 30 )) $(( (t + b) / 2 )) $(( l + 8 )) $(( (t + b) / 2 ))
"$DRIVE" channel 2 >/dev/null
"$DRIVE" typekeys "another draft" >/dev/null
"$DRIVE" channel 1 >/dev/null
"$DRIVE" view drafts >/dev/null
sleep 0.5; snap
read -r l t r b <<< "$(grep -m1 '^a11yitem drafts.row.0 ' "$D" | cut -d' ' -f3-)"
lit_while_open "a draft" $(( r - 150 )) $(( (t + b) / 2 )) $(( l + 8 )) $(( (t + b) / 2 ))
rm -f "$D" "$LIN_DIR/drafts_px.bmp"

say ""
say "gui_drafts: $((checks - fails))/$checks passed in $((SECONDS - START))s"
[ "${1:-}" = "--kill" ] && { "$DRIVE" kill >/dev/null; kill_dev_daemon; }
[ "$fails" = 0 ]
