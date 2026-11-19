#!/usr/bin/env bash
# The mouse cursor's shape in the GUI client, read from the client itself.
#
#   tools/gui/features/cursor.sh          # run; the client and daemon stop at the end
#   tools/gui/features/cursor.sh --keep   # ... and leave them running
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
#
# platforms: win32 web
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
. "$HERE/tools/gui/harness/lib.sh"
gui_begin cursor 9505 /tmp/oc-cursor-fixture "Cursor Fixture" "$@"
export OC_TEST_HEALTH_PORT="${OC_TEST_HEALTH_PORT:-9506}"

kind_is() { gui_snap && grep -q "^cursor kind=$1\$" "$D"; }
# The top-left of message row $1's body, from the dump.
body_xy() { field "$D" "^  msgrow $1 " body; }

step "a conversation with a link in it"
gui_launch
"$DRIVE" size 1100 800 >/dev/null
"$DRIVE" channel 1 >/dev/null || gui_abort "the client did not open the channel"
# Back from the daemon, and drawn: the link's row is the newest.
send_stored "https://example.com/cursor"
drawn() { gui_snap && [ "$(field "$D" '^msgrows ' n)" -ge 1 ] 2>/dev/null; }
check "the message is drawn" waitfor 10 drawn
n="$(field "$D" '^msgrows ' n)"
IFS=, read -r lx ly <<< "$(body_xy $(( ${n:-1} - 1 )))"
IFS=, read -r el et er eb <<< "$(field "$D" '^ed ' box)"
[ -n "${ly:-}" ] && [ -n "${eb:-}" ] || gui_abort "the dump gives no message body or composer box to hover"

step "the composer, a link, and nothing"
"$DRIVE" move $(( (${el%.*} + ${er%.*}) / 2 )) $(( (${et%.*} + ${eb%.*}) / 2 )) >/dev/null
check "the composer's text: the I-beam" waitfor 5 kind_is ibeam
"$DRIVE" move $((lx + 12)) $((ly + 8)) >/dev/null
check "a link: the hand" waitfor 5 kind_is hand
"$DRIVE" move 8 8 >/dev/null
check "empty space: the arrow" waitfor 5 kind_is arrow
gui_end
