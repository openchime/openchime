#!/usr/bin/env bash
# Spoilers (MARKDOWN.md §1d): text hidden until a reader chooses to see it.
#
#   scripts/gui/spoiler.sh          # run, and leave the client running
#   scripts/gui/spoiler.sh --kill   # ... and shut it down afterwards
#
# A message with an inline spoiler and a labelled block is sent. Both are drawn
# covered, each a button for assistive technology, and the sidebar's preview
# says "[spoiler]" rather than what it hides. A click reveals the first, and a
# second click covers it again; the labelled block reveals the same way. From
# the keyboard, the message's menu (Ctrl+Down, Shift+F10) shows them all on
# Enter. Its own daemon on its own port, wiped each run.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
. "$HERE/scripts/gui/lib.sh"

export OC_TEST_PORT="${OC_TEST_PORT:-9690}"
export OC_TEST_DIR="${OC_TEST_DIR:-/tmp/oc-spoiler}"
export OC_TEST_WS="${OC_TEST_WS:-Spoiler Fixture}"

kill_dev_daemon || { say "a daemon still holds :$OC_TEST_PORT; stop it and re-run"; exit 1; }
rm -rf "$OC_TEST_DIR"

D="/tmp/oc-spoiler-dump.txt"
snap() { "$DRIVE" dump spoiler 2>/dev/null | tail -n +2 > "$D"; }
spl() { grep "^  spoiler .* k=$1 " "$D" | head -1 | grep -o "\b$2=[^ ]*" | cut -d= -f2; }
waitfor() {  # waitfor <secs> <command...>
  local secs="$1" t=0; shift
  while [ "$t" -lt $((secs * 4)) ]; do if "$@"; then return 0; fi; sleep 0.25; t=$((t + 1)); done
  return 1
}
click_rect() {  # click the centre of an l,t,r,b rect; refuses an empty one
  local l t r b; IFS=, read -r l t r b <<< "$1"
  [ -n "${b:-}" ] && [ "${r%.*}" -gt "${l%.*}" ] || return 1
  "$DRIVE" click $(( (l + r) / 2 )) $(( (t + b) / 2 )) >/dev/null
}

say "== a message with two spoilers"
"$DRIVE" launch >/dev/null 2>&1 || { say "launch failed"; exit 1; }
for _ in $(seq 1 80); do snap; grep -q 'authed=1' "$D" && break; sleep 0.25; done
"$DRIVE" size 1200 820 >/dev/null
"$DRIVE" channel 1 >/dev/null
"$DRIVE" sendraw 'The killer is ||the butler|| of course.\n```spoiler Ending\nShe lives.\n```' >/dev/null
covered() { snap; [ "$(grep -m1 '^spoilers ' "$D" | grep -o 'n=[0-9]*' | cut -d= -f2)" = 2 ] &&
            [ "$(spl 0 shown)" = 0 ] && [ "$(spl 1 shown)" = 0 ]; }
check "both are drawn covered" waitfor 10 covered
check "each is a button for assistive technology" \
  bash -c "grep -q '^a11yitem message\.[0-9]*\.spoiler\.0 ' '$D' && grep -q '^a11yitem message\.[0-9]*\.spoiler\.1 ' '$D'"
check "the preview names it and gives nothing away" grep -q '^  ch 1 .*prev="The killer is \[spoiler\] of course' "$D"
check "and nothing anywhere says what it hides" bash -c "! grep -q 'prev=\"[^\"]*butler' '$D'"
"$DRIVE" shot spoiler_covered >/dev/null

say "== revealed, and covered again"
click_rect "$(spl 0 r)"
shown0() { snap; [ "$(spl 0 shown)" = 1 ] && [ "$(spl 1 shown)" = 0 ]; }
check "a click reveals the first, and only it" waitfor 5 shown0
"$DRIVE" shot spoiler_shown >/dev/null
click_rect "$(spl 0 r)"
hidden0() { snap; [ "$(spl 0 shown)" = 0 ]; }
check "a second click covers it again" waitfor 5 hidden0
click_rect "$(spl 1 r)"
shown1() { snap; [ "$(spl 1 shown)" = 1 ]; }
check "the labelled block reveals the same way" waitfor 5 shown1

say "== from the keyboard"
kb() { snap; grep -m1 '^kbfocus ' "$D" | grep -o "\b$1=[^ ]*" | head -1 | cut -d= -f2; }
"$DRIVE" move 20 20 >/dev/null
"$DRIVE" key ctrl+down >/dev/null           # onto the newest message: this one
"$DRIVE" key shift+f10 >/dev/null           # its actions
found=0
for _ in $(seq 1 20); do
  [ "$(kb menuhovercmd)" = 110 ] && { found=1; break; }
  "$DRIVE" key down >/dev/null
done
check "the message's menu offers its spoilers to the keyboard" [ "$found" = 1 ]
"$DRIVE" key enter >/dev/null
# One was shown (the block): Enter takes them all to the other state of the
# first, which was covered -- so both show.
both() { snap; [ "$(spl 0 shown)" = 1 ] && [ "$(spl 1 shown)" = 1 ]; }
check "Enter on it shows every spoiler in the message" waitfor 5 both
say "== however many, and however long"
many=""; for i in $(seq 1 10); do many="${many}||secret$i|| "; done
"$DRIVE" sendraw "$many" >/dev/null
tenth() { snap; MID="$(grep '^  spoiler mid=[0-9]* k=9 ' "$D" | head -1 | grep -o 'mid=[0-9]*' | cut -d= -f2)"; [ -n "$MID" ]; }
check "the tenth spoiler in a message is drawn" waitfor 10 tenth
check "and all ten are covered" [ "$(grep -c "^  spoiler mid=$MID k=[0-9]* shown=0 " "$D")" = 10 ]
blk='```spoiler Long\nline 1\nline 2\nline 3\nline 4\nline 5\nline 6\nline 7\nline 8\n| a | b |\n|---|---|\n| 1 | 2 |\n```'
"$DRIVE" sendraw "$blk" >/dev/null
eight() { snap; grep -q "^  spoiler mid=[0-9]* k=0 shown=0 lines=1[01] " "$D"; }
check "every line of a long block has its cover" waitfor 10 eight
BLK="$(grep -m1 "^  spoiler mid=[0-9]* k=0 shown=0 lines=1[01] " "$D" | grep -o 'mid=[0-9]*' | cut -d= -f2)"
check "and a table inside it is not drawn as a grid" bash -c "! grep -q '^  table msg=$BLK ' '$D'"
"$DRIVE" shot spoiler_many >/dev/null
rm -f "$D"

say ""
say "gui_spoiler: $((checks - fails))/$checks passed in $((SECONDS - START))s"
[ "${1:-}" = "--kill" ] && { "$DRIVE" kill >/dev/null; kill_dev_daemon; }
[ "$fails" = 0 ]
