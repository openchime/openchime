#!/usr/bin/env bash
# An email's link, in the GUI client: openchime://w/<audience>/c/<channel>/m/<message>
# (CLIENT.md, "Links").
#
#   tools/gui/features/permalink_w.sh          # run; the client and daemon stop at the end
#   tools/gui/features/permalink_w.sh --keep   # ... and leave them running
#
# It asserts that
#
#   - a link naming this workspace's audience opens its conversation, at the
#     message the link names;
#   - a link naming an audience this device does not know opens nothing, says
#     it is for a workspace you're not signed into, and leaves you where you were.
#
# Its own daemon on its own port, started with an audience as a managed box is.
#
# platforms: win32
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
. "$HERE/tools/gui/harness/lib.sh"
export OC_TEST_HEALTH_PORT="${OC_TEST_HEALTH_PORT:-9675}"
AUD="ws_PermalinkFixture01"
export OPENCHIME_MANAGED_AUDIENCE="$AUD"
gui_begin permalink_w 9674 /tmp/oc-permalink-w-fixture "Link Fixture" "$@"

hist() { field "$D" '^hist ' "$1"; }
chan_id() { grep -m1 "^  ch [0-9]* \"$1\"" "$D" | awk '{print $2}'; }

step "a channel with a message to link to"
gui_launch
"$DRIVE" size 1100 800 >/dev/null
"$DRIVE" mkchan "elsewhere 1" >/dev/null || gui_abort "the client did not take mkchan"
made() { gui_snap; [ -n "$(chan_id elsewhere)" ]; }
waitfor 10 made || gui_abort "the channel elsewhere was not made"
A=$(chan_id general); B=$(chan_id elsewhere)
"$DRIVE" channel elsewhere >/dev/null || gui_abort "the client did not open elsewhere"
"$DRIVE" send "The line the email points at" >/dev/null || gui_abort "the client did not send"
drawn() { gui_snap; grep '^  msgrow 0 mid=' "$D" >/dev/null && [ "$(hist cid)" = "$B" ]; }
waitfor 10 drawn || gui_abort "the message in elsewhere was not drawn"
MB=$(grep -m1 '^  msgrow 0 mid=' "$D" | grep -o 'mid=[0-9]*' | cut -d= -f2)
"$DRIVE" channel general >/dev/null
in_general() { gui_snap; [ "$(hist cid)" = "$A" ]; }
waitfor 10 in_general || gui_abort "the client did not go back to general"

step "a link naming this workspace's audience"
"$DRIVE" palette "openchime://w/$AUD/c/$B/m/$MB" >/dev/null || gui_abort "the client did not open the palette"
"$DRIVE" key enter >/dev/null
opened() { gui_snap; [ "$(hist cid)" = "$B" ]; }
check "the link opens its conversation" waitfor 10 opened
"$DRIVE" shot permalink_w_opened >/dev/null

step "a link naming a workspace this device does not know"
"$DRIVE" channel general >/dev/null
waitfor 10 in_general || gui_abort "the client did not go back to general"
"$DRIVE" palette "openchime://w/ws_SomeoneElse0000/c/$B/m/$MB" >/dev/null || gui_abort "the client did not open the palette"
"$DRIVE" key enter >/dev/null
told() { gui_snap; grep -i "toast\[[0-9]\].*not signed into" "$D" >/dev/null; }
check "it says the link is for a workspace you're not signed into" waitfor 10 told
check "...and stays where it was" in_general
gui_end
