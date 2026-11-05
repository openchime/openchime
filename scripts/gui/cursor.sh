#!/usr/bin/env bash
# The mouse cursor's shape in the GUI client, read from the client itself.
#
#   scripts/gui/cursor.sh          # run, and leave the client running
#   scripts/gui/cursor.sh --kill   # ... and shut it down afterwards
#
# The pointer a detached session reports is suppressed, and the harness's hover
# is synthetic, so the shape is the one the client last set, from the dump's
# `cursor kind=` line. It asserts that hovering
#
#   - the composer's text gives the I-beam;
#   - a link in a message gives the hand;
#   - empty space gives the arrow.
#
# Its own daemon on its own port.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
. "$HERE/scripts/gui/lib.sh"

export OC_TEST_PORT="${OC_TEST_PORT:-9674}"
export OC_TEST_HEALTH_PORT="${OC_TEST_HEALTH_PORT:-9675}"
export OC_TEST_DIR="${OC_TEST_DIR:-/tmp/oc-cursor-fixture}"
export OC_TEST_WS="${OC_TEST_WS:-Cursor Fixture}"

kill_dev_daemon || { say "a daemon still holds :$OC_TEST_PORT; stop it and re-run"; exit 1; }
rm -rf "$OC_TEST_DIR"

D="/tmp/oc-cursor-dump.txt"
snap() { "$DRIVE" dump cursor > "$D" 2>/dev/null; }
waitfor() {  # waitfor <secs> <command...>
  local secs="$1" t=0; shift
  while [ "$t" -lt $((secs * 4)) ]; do if "$@"; then return 0; fi; sleep 0.25; t=$((t + 1)); done
  return 1
}
kind_is() { snap; grep -q "^cursor kind=$1\$" "$D"; }
# The top-left of message row $1's body, from the dump.
body_xy() { grep "^  msgrow $1 " "$D" | grep -o 'body=[0-9]*,[0-9]*' | cut -d= -f2; }

say "== a conversation with a link in it"
"$DRIVE" launch >/dev/null 2>&1 || { say "launch failed"; exit 1; }
for _ in $(seq 1 80); do snap; grep -q 'authed=1' "$D" && break; sleep 0.25; done
"$DRIVE" size 1100 800 >/dev/null
"$DRIVE" channel 1 >/dev/null
"$DRIVE" send "https://example.com/cursor" >/dev/null
drawn() { snap; [ "$(grep -m1 '^msgrows ' "$D" | grep -o 'n=[0-9]*' | cut -d= -f2)" -ge 1 ]; }
check "the message is drawn" waitfor 10 drawn
n=$(grep -m1 '^msgrows ' "$D" | grep -o 'n=[0-9]*' | cut -d= -f2)
IFS=, read -r lx ly <<< "$(body_xy $((n - 1)))"
IFS=, read -r el et er eb <<< "$(grep -m1 '^ed ' "$D" | grep -o 'box=[0-9.,-]*' | cut -d= -f2)"

say "== the composer, a link, and nothing"
"$DRIVE" move $(( (${el%.*} + ${er%.*}) / 2 )) $(( (${et%.*} + ${eb%.*}) / 2 )) >/dev/null
check "the composer's text: the I-beam" waitfor 5 kind_is ibeam
"$DRIVE" move $((lx + 12)) $((ly + 8)) >/dev/null
check "a link: the hand" waitfor 5 kind_is hand
"$DRIVE" move 8 8 >/dev/null
check "empty space: the arrow" waitfor 5 kind_is arrow
rm -f "$D"

say ""
say "gui_cursor: $((checks - fails))/$checks passed in $((SECONDS - START))s"
[ "${1:-}" = "--kill" ] && { "$DRIVE" kill >/dev/null; kill_dev_daemon; }
[ "$fails" = 0 ]
