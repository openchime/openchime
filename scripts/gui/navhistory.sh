#!/usr/bin/env bash
# Back and forward through the places you have been, in the GUI client.
#
#   scripts/gui/navhistory.sh          # run, and leave the client running
#   scripts/gui/navhistory.sh --kill   # ... and shut it down afterwards
#
# A place is a conversation, its open thread, and where in it you were. It
# asserts that
#
#   - reading a channel scrolled up, then following a permalink elsewhere,
#     leaves two places, the first remembering the message at its top;
#   - Alt+Left returns to the first at that same message, and Alt+Right goes
#     forward to the second;
#   - the header's Back button does what Alt+Left does;
#   - going somewhere new after going back drops the forward entry.
#
# Its own daemon on its own port.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
. "$HERE/scripts/gui/lib.sh"

export OC_TEST_PORT="${OC_TEST_PORT:-9672}"
export OC_TEST_HEALTH_PORT="${OC_TEST_HEALTH_PORT:-9673}"
export OC_TEST_DIR="${OC_TEST_DIR:-/tmp/oc-navhistory-fixture}"
export OC_TEST_WS="${OC_TEST_WS:-History Fixture}"

kill_dev_daemon || { say "a daemon still holds :$OC_TEST_PORT; stop it and re-run"; exit 1; }
rm -rf "$OC_TEST_DIR"

D="/tmp/oc-navhistory-dump.txt"
snap() { "$DRIVE" dump navhist > "$D" 2>/dev/null; }
waitfor() {  # waitfor <secs> <command...>
  local secs="$1" t=0; shift
  while [ "$t" -lt $((secs * 4)) ]; do if "$@"; then return 0; fi; sleep 0.25; t=$((t + 1)); done
  return 1
}
hist() { grep -m1 '^hist ' "$D" | grep -o "\b$1=[^ ]*" | cut -d= -f2; }
chan_id() { grep -m1 "^  ch [0-9]* \"$1\"" "$D" | awk '{print $2}'; }
at() {  # at <pos> <cid> [mid]: the current entry is that one, settled
  snap
  [ "$(hist pos)" = "$1" ] && [ "$(hist cid)" = "$2" ] && [ "$(hist restoring)" = 0 ] &&
    { [ -z "${3:-}" ] || [ "$(hist mid)" = "$3" ]; }
}

say "== a long channel and a short one"
"$DRIVE" launch >/dev/null 2>&1 || { say "launch failed"; exit 1; }
for _ in $(seq 1 80); do snap; grep -q 'authed=1' "$D" && break; sleep 0.25; done
"$DRIVE" size 1100 800 >/dev/null
"$DRIVE" mkchan "elsewhere 1" >/dev/null
"$DRIVE" mkchan "third 1" >/dev/null
made() { snap; [ -n "$(chan_id elsewhere)" ] && [ -n "$(chan_id third)" ]; }
check "both channels exist" waitfor 10 made
A=$(chan_id general); B=$(chan_id elsewhere); C=$(chan_id third)
"$DRIVE" channel elsewhere >/dev/null
for i in 1 2 3; do "$DRIVE" send "Elsewhere, message $i" >/dev/null; done
"$DRIVE" channel general >/dev/null
for i in $(seq 1 40); do "$DRIVE" send "General, message $i of forty" >/dev/null; sleep 0.15; done
forty() { snap; grep -m1 "^  ch $A " "$D" | grep -q 'prev="General, message 40 of forty"'; }
check "forty messages in general" waitfor 20 forty
"$DRIVE" channel elsewhere >/dev/null
first_b() { snap; grep -q '^  msgrow 0 mid=' "$D" && [ "$(hist cid)" = "$B" ]; }
check "elsewhere is drawn" waitfor 10 first_b
MB=$(grep -m1 '^  msgrow 0 mid=' "$D" | grep -o 'mid=[0-9]*' | cut -d= -f2)

say "== read general scrolled up, then follow a link away"
"$DRIVE" channel general >/dev/null
sleep 0.6
"$DRIVE" scroll 500 >/dev/null
up() { snap; [ "$(hist cid)" = "$A" ] && [ "$(hist mid)" != 0 ]; }
check "general's place remembers the message at its top" waitfor 10 up
MA=$(hist mid)
POS0=$(hist pos)
"$DRIVE" palette "openchime:///c/$B/m/$MB" >/dev/null
"$DRIVE" key enter >/dev/null
check "the link is a new place" waitfor 10 at $((POS0 + 1)) "$B"
"$DRIVE" shot navhistory_away >/dev/null

say "== Alt+Left and Alt+Right"
"$DRIVE" key alt+left >/dev/null
check "Alt+Left returns to general" waitfor 10 at "$POS0" "$A"
back_where() { snap; [ "$(hist mid)" = "$MA" ] && awk '/^hist /{for(i=1;i<=NF;i++) if($i ~ /^scroll=/){split($i,a,"="); exit !(a[2]>0)}}' "$D"; }
check "...at the message it left from" waitfor 10 back_where
"$DRIVE" shot navhistory_back >/dev/null
"$DRIVE" key alt+right >/dev/null
check "Alt+Right goes forward again" waitfor 10 at $((POS0 + 1)) "$B"

say "== the header's Back button"
IFS=, read -r bl bt _ bb <<< "$(hist btn)"
"$DRIVE" click $((bl + 10)) $(( (bt + bb) / 2 )) >/dev/null
check "Back in the header returns to general" waitfor 10 at "$POS0" "$A"

say "== somewhere new drops what was forward"
"$DRIVE" channel third >/dev/null
check "third is the newest place" waitfor 10 at $((POS0 + 1)) "$C"
nofwd() { snap; [ "$(hist fwd)" = 0 ] && [ "$(hist n)" = $((POS0 + 2)) ]; }
check "...and there is nothing forward" waitfor 5 nofwd
rm -f "$D"

say ""
say "gui_navhistory: $((checks - fails))/$checks passed in $((SECONDS - START))s"
[ "${1:-}" = "--kill" ] && { "$DRIVE" kill >/dev/null; kill_dev_daemon; }
[ "$fails" = 0 ]
