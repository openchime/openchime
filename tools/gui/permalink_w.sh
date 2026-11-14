#!/usr/bin/env bash
# An email's link, in the GUI client: openchime://w/<audience>/c/<channel>/m/<message>
# (CLIENT.md, "Links").
#
#   tools/gui/permalink_w.sh          # run, and leave the client running
#   tools/gui/permalink_w.sh --kill   # ... and shut it down afterwards
#
# It asserts that
#
#   - a link naming this workspace's audience opens its conversation, at the
#     message the link names;
#   - a link naming an audience this device does not know opens nothing, says
#     it is for a workspace you're not signed into, and leaves you where you were.
#
# Its own daemon on its own port, started with an audience as a managed box is.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
. "$HERE/tools/gui/lib.sh"

export OC_TEST_PORT="${OC_TEST_PORT:-9674}"
export OC_TEST_HEALTH_PORT="${OC_TEST_HEALTH_PORT:-9675}"
export OC_TEST_DIR="${OC_TEST_DIR:-/tmp/oc-permalink-w-fixture}"
export OC_TEST_WS="${OC_TEST_WS:-Link Fixture}"
AUD="ws_PermalinkFixture01"
export OPENCHIME_MANAGED_AUDIENCE="$AUD"

kill_dev_daemon || { say "a daemon still holds :$OC_TEST_PORT; stop it and re-run"; exit 1; }
rm -rf "$OC_TEST_DIR"

D="/tmp/oc-permalink-w-dump.txt"
snap() { "$DRIVE" dump permalink_w > "$D" 2>/dev/null; }
waitfor() {  # waitfor <secs> <command...>
  local secs="$1" t=0; shift
  while [ "$t" -lt $((secs * 4)) ]; do if "$@"; then return 0; fi; sleep 0.25; t=$((t + 1)); done
  return 1
}
hist() { grep -m1 '^hist ' "$D" | grep -o "\b$1=[^ ]*" | cut -d= -f2; }
chan_id() { grep -m1 "^  ch [0-9]* \"$1\"" "$D" | awk '{print $2}'; }

say "== a channel with a message to link to"
"$DRIVE" launch >/dev/null 2>&1 || { say "launch failed"; exit 1; }
for _ in $(seq 1 80); do snap; grep -q 'authed=1' "$D" && break; sleep 0.25; done
"$DRIVE" size 1100 800 >/dev/null
"$DRIVE" mkchan "elsewhere 1" >/dev/null
made() { snap; [ -n "$(chan_id elsewhere)" ]; }
check "the channel exists" waitfor 10 made
A=$(chan_id general); B=$(chan_id elsewhere)
"$DRIVE" channel elsewhere >/dev/null
"$DRIVE" send "The line the email points at" >/dev/null
drawn() { snap; grep -q '^  msgrow 0 mid=' "$D" && [ "$(hist cid)" = "$B" ]; }
check "elsewhere is drawn" waitfor 10 drawn
MB=$(grep -m1 '^  msgrow 0 mid=' "$D" | grep -o 'mid=[0-9]*' | cut -d= -f2)
"$DRIVE" channel general >/dev/null
in_general() { snap; [ "$(hist cid)" = "$A" ]; }
check "back in general" waitfor 10 in_general

say "== a link naming this workspace's audience"
"$DRIVE" palette "openchime://w/$AUD/c/$B/m/$MB" >/dev/null
"$DRIVE" key enter >/dev/null
opened() { snap; [ "$(hist cid)" = "$B" ]; }
check "the link opens its conversation" waitfor 10 opened
"$DRIVE" shot permalink_w_opened >/dev/null

say "== a link naming a workspace this device does not know"
"$DRIVE" channel general >/dev/null
check "back in general again" waitfor 10 in_general
"$DRIVE" palette "openchime://w/ws_SomeoneElse0000/c/$B/m/$MB" >/dev/null
"$DRIVE" key enter >/dev/null
told() { snap; grep -qi "toast\[[0-9]\].*not signed into" "$D"; }
check "it says the link is for a workspace you're not signed into" waitfor 10 told
check "...and stays where it was" in_general

rm -f "$D"

say ""
say "gui_permalink_w: $((checks - fails))/$checks passed in $((SECONDS - START))s"
[ "${1:-}" = "--kill" ] && { "$DRIVE" kill >/dev/null; kill_dev_daemon; }
[ "$fails" = 0 ]
