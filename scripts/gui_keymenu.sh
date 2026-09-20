#!/usr/bin/env bash
# The keyboard's route to the context menu in the Win32 client (REQ-264).
#
#   scripts/gui_keymenu.sh          # run, and leave the client running
#   scripts/gui_keymenu.sh --kill   # ... and shut it down afterwards
#
# Pin, save, forward, copy link, edit, delete, mark unread and react were
# reachable only by right-clicking, so without a pointer the richest action set
# in the app could not be reached at all. It asserts that
#
#   - Ctrl+Down puts the keyboard on a message, and Ctrl+Up/Ctrl+Down move it
#     between messages;
#   - Shift+F10 opens THAT message's actions, with the first item already
#     highlighted so Enter means something;
#   - the arrows move the highlight through the menu and Enter runs the item that
#     is highlighted -- proven by its EFFECT, not by the menu closing: "Copy link"
#     puts a link to THAT message on the clipboard, pasted back to be read;
#   - Esc closes the menu, and Esc again takes the keyboard off the row;
#   - with no message focused, Shift+F10 opens the conversation's own menu.
#
# Its own daemon on its own port.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DRIVE="$HERE/scripts/gui_drive.sh"
LIN_DIR='/mnt/c/Windows/Temp/octest'
START=$SECONDS
fails=0
checks=0

export OC_DEV_PORT="${OC_DEV_PORT:-9610}"
export OC_DEV_DIR="${OC_DEV_DIR:-/tmp/oc-keymenu}"
export OC_DEV_WS="${OC_DEV_WS:-Keymenu Fixture}"

MENU_MSG=6            # the enum's value, as the dump prints it
MENU_CHANNEL=8
CMD_COPY_LINK=105     # "Copy link" -- its effect can be read back

# Every key press, with its ACK CHECKED. gui_drive exits non-zero when the client
# did not answer within ten seconds, and a dropped verb that nobody notices
# reports itself as the next assertion failing -- a key that never arrived looks
# exactly like a key that did nothing. This is the whole script's key path, so a
# drop is named where it happened.
k() {
  if ! "$DRIVE" key "$1" >/dev/null 2>&1; then
    printf '  FAIL the client did not answer "key %s"\n' "$1"; fails=$((fails + 1)); checks=$((checks + 1))
  fi
}
say()  { printf '%s\n' "$*"; }
fail() { printf '  FAIL %s\n' "$*"; fails=$((fails + 1)); }
ok()   { printf '  ok   %s\n' "$*"; }
check() { local what="$1"; shift; checks=$((checks + 1)); if "$@"; then ok "$what"; else fail "$what"; fi; }

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

D="/tmp/oc-keymenu-dump.txt"
# The same rule for the dump: an unanswered `dump` leaves the PREVIOUS file on
# disk, so every value read after it is yesterday's -- which is how a check comes
# to fail (or pass) for a reason that is not in the client at all.
snap() {
  if ! "$DRIVE" dump km >/dev/null 2>&1; then
    printf '  FAIL the client did not answer "dump"\n'; fails=$((fails + 1)); checks=$((checks + 1))
    return 1
  fi
  cp "$LIN_DIR/km.txt" "$D" 2>/dev/null
}
kb()   { snap; grep -m1 '^kbfocus ' "$D" | grep -o "\b$1=[^ ]*" | head -1 | cut -d= -f2; }
top()  { snap; grep -m1 "^$1=" "$D" | cut -d= -f2 | cut -d' ' -f1; }
menu() { snap; grep -m1 '^menu=' "$D" | grep -o '^menu=[0-9-]*' | cut -d= -f2; }
waitfor() {  # waitfor <secs> <command...>
  local secs="$1" t=0; shift
  while [ "$t" -lt $((secs * 4)) ]; do if "$@"; then return 0; fi; sleep 0.25; t=$((t + 1)); done
  return 1
}

say "== three messages, and no pointer"
"$DRIVE" launch >/dev/null 2>&1 || { say "launch failed"; exit 1; }
for _ in $(seq 1 80); do snap; grep -q 'authed=1' "$D" && break; sleep 0.25; done
"$DRIVE" size 1100 800 >/dev/null
"$DRIVE" channel 1 >/dev/null
for i in 1 2 3; do "$DRIVE" send "message number $i" >/dev/null; sleep 0.4; done
# The pointer is parked off every row, so nothing here can be the mouse's doing.
"$DRIVE" move 20 20 >/dev/null
three_rows() { [ "$(snap; grep -m1 '^msgrows ' "$D" | grep -o 'n=[0-9]*' | cut -d= -f2)" -ge 3 ]; }
check "three messages are drawn" waitfor 10 three_rows

say "== Ctrl+Down puts the keyboard on a message"
nofocus() { [ "$(kb mid)" = 0 ]; }
check "nothing is focused to begin with" nofocus
k ctrl+down
focused() { [ "$(kb mid)" != 0 ]; }
check "Ctrl+Down focuses one" waitfor 5 focused
newest="$(kb mid)"
k ctrl+up
moved() { [ "$(kb mid)" != "$newest" ] && [ "$(kb mid)" != 0 ]; }
check "Ctrl+Up moves to the one before it" waitfor 5 moved
older="$(kb mid)"
k ctrl+down
back() { [ "$(kb mid)" = "$newest" ]; }
check "...and Ctrl+Down comes back" waitfor 5 back
"$DRIVE" shot keymenu_focus >/dev/null

say "== Shift+F10 opens that message's actions"
# Put something ELSE on the clipboard first. Windows keeps it across runs, so a
# link copied by an earlier run would satisfy the paste check below whether or not
# this run ever copied anything -- two checks passing on yesterday's work.
"$DRIVE" typekeys "clipboard-was-here" >/dev/null
k ctrl+a
k ctrl+c
k ctrl+a
k delete
seeded() { k ctrl+v; snap; grep -m1 '^ed ' "$D" | grep -q 'clipboard-was-here'; }
check "the clipboard starts with something that is not a link" waitfor 5 seeded
k ctrl+a
k delete
k shift+f10
msg_menu() { [ "$(menu)" = "$MENU_MSG" ]; }
check "the message menu is open" waitfor 5 msg_menu
ready() { [ "$(kb menuhover)" -ge 0 ] 2>/dev/null; }
check "...with an item already highlighted, so Enter means something" waitfor 5 ready
"$DRIVE" shot keymenu_open >/dev/null

say "== the arrows walk it and Enter runs what is highlighted"
first="$(kb menuhovercmd)"
k down
stepped() { [ "$(kb menuhovercmd)" != "$first" ]; }
check "Down moves the highlight" waitfor 5 stepped
k up
stepped_back() { [ "$(kb menuhovercmd)" = "$first" ]; }
check "...and Up moves it back" waitfor 5 stepped_back
# Walk to "Copy link" and choose it. What it did is then READ BACK: the link goes
# on the clipboard, so pasting it into the composer says both that the highlighted
# item ran and that it ran on the focused message -- the id is in the link. A menu
# that merely closed would prove neither.
found=0
for _ in $(seq 1 30); do
  [ "$(kb menuhovercmd)" = "$CMD_COPY_LINK" ] && { found=1; break; }
  k down
done
check "the menu offers \"Copy link\" to the keyboard" [ "$found" = 1 ]
k enter
closed() { [ "$(menu)" = 0 ]; }
check "Enter closes the menu" waitfor 5 closed
said() { snap; grep -qi 'toast\[[0-9]\].*[Ll]ink copied' "$D"; }
check "...and it says the link was copied" waitfor 5 said
k ctrl+a
k delete
k ctrl+v
pasted() { snap; grep -m1 '^ed ' "$D" | grep -q "/m/$newest\""; }
has_link() { snap; grep -m1 '^ed ' "$D" | grep -qi 'openchime://'; }
check "the clipboard holds a link" waitfor 5 has_link
check "...to the message the keyboard was on ($newest)" pasted
k ctrl+a
k delete

say "== Esc takes the keyboard back off"
k shift+f10
check "the menu opens again" waitfor 5 msg_menu
k esc
check "Esc closes it" waitfor 5 closed
check "the keyboard is still on the message" focused
k esc
check "Esc again takes it off the row" waitfor 5 nofocus

say "== with no message focused, it is the conversation's menu"
k shift+f10
chan_menu() { [ "$(menu)" = "$MENU_CHANNEL" ]; }
check "the conversation's own menu opens" waitfor 5 chan_menu
k esc
rm -f "$D"

say ""
say "gui_keymenu: $((checks - fails))/$checks passed in $((SECONDS - START))s"
[ "${1:-}" = "--kill" ] && { "$DRIVE" kill >/dev/null; kill_dev_daemon; }
[ "$fails" = 0 ]
