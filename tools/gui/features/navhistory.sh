#!/usr/bin/env bash
# Back and forward through the places you have been, in the GUI client.
#
#   tools/gui/features/navhistory.sh          # run; the client and daemon stop at the end
#   tools/gui/features/navhistory.sh --keep   # ... and leave them running
#
# A place is a conversation, its open thread, and where in it you were. It
# asserts that
#
#   - reading a channel scrolled up, then following a permalink elsewhere,
#     leaves two places, the first remembering the message at its top;
#   - Alt+Left returns to the first at that same message, and Alt+Right goes
#     forward to the second;
#   - the header's Back button does what Alt+Left does;
#   - going somewhere new after going back drops the forward entry;
#   - the DMs view's person list has a header of its own ("Direct messages",
#     Back, New message), a banner sits directly under it, and opening a
#     conversation shows that conversation's header instead.
#
# Its own daemon on its own port.
#
# platforms: win32 web
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
. "$HERE/tools/gui/harness/lib.sh"
export OC_TEST_HEALTH_PORT="${OC_TEST_HEALTH_PORT:-9673}"
gui_begin navhistory 9672 /tmp/oc-navhistory-fixture "History Fixture" "$@"

hist() { field "$D" '^hist ' "$1"; }
chan_id() { grep -m1 "^  ch [0-9]* \"$1\"" "$D" | awk '{print $2}'; }
at() {  # at <pos> <cid> [mid]: the current entry is that one, settled
  gui_snap
  [ "$(hist pos)" = "$1" ] && [ "$(hist cid)" = "$2" ] && [ "$(hist restoring)" = 0 ] &&
    { [ -z "${3:-}" ] || [ "$(hist mid)" = "$3" ]; }
}

step "a long channel and a short one"
gui_launch
"$DRIVE" size 1100 800 >/dev/null
"$DRIVE" mkchan "elsewhere 1" >/dev/null || gui_abort "the client did not take mkchan"
"$DRIVE" mkchan "third 1" >/dev/null || gui_abort "the client did not take mkchan"
made() { gui_snap; [ -n "$(chan_id elsewhere)" ] && [ -n "$(chan_id third)" ]; }
waitfor 10 made || gui_abort "the channels elsewhere and third were not made"
A=$(chan_id general); B=$(chan_id elsewhere); C=$(chan_id third)
"$DRIVE" channel elsewhere >/dev/null || gui_abort "the client did not open elsewhere"
for i in 1 2 3; do "$DRIVE" send "Elsewhere, message $i" >/dev/null || gui_abort "the client did not send"; done
"$DRIVE" channel general >/dev/null || gui_abort "the client did not open general"
# One at a time, each back from the daemon before the next, so general holds
# forty in the order sent.
for i in $(seq 1 40); do
  act send "General, message $i of forty"
  waitfor 10 newest_is "$A" "General, message $i of forty" || gui_abort "general's message $i did not come back from the daemon"
done
"$DRIVE" channel elsewhere >/dev/null
first_b() { gui_snap; grep '^  msgrow 0 mid=' "$D" >/dev/null && [ "$(hist cid)" = "$B" ]; }
waitfor 10 first_b || gui_abort "elsewhere was not drawn"
MB=$(grep -m1 '^  msgrow 0 mid=' "$D" | grep -o 'mid=[0-9]*' | cut -d= -f2)

step "read general scrolled up, then follow a link away"
"$DRIVE" channel general >/dev/null
in_a() { gui_snap; [ "$(hist cid)" = "$A" ] && [ "$(hist restoring)" = 0 ] && grep '^  msgrow 0 mid=' "$D" >/dev/null; }
waitfor 10 in_a || gui_abort "general was not drawn"
"$DRIVE" scroll 500 >/dev/null
up() { gui_snap; [ "$(hist cid)" = "$A" ] && [ "$(hist mid)" != 0 ]; }
check "general's place remembers the message at its top" waitfor 10 up
MA=$(hist mid)
POS0=$(hist pos)
"$DRIVE" palette "openchime:///c/$B/m/$MB" >/dev/null || gui_abort "the client did not open the palette"
"$DRIVE" key enter >/dev/null
check "the link is a new place" waitfor 10 at $((POS0 + 1)) "$B"
"$DRIVE" shot navhistory_away >/dev/null

step "Alt+Left and Alt+Right"
"$DRIVE" key alt+left >/dev/null
check "Alt+Left returns to general" waitfor 10 at "$POS0" "$A"
# Scrolled up, not merely at the bottom with the same message named.
back_where() { gui_snap; local s; s="$(hist scroll)"; [ "$(hist mid)" = "$MA" ] && [ -n "$s" ] && awk -v s="$s" 'BEGIN { exit !(s > 0) }'; }
check "...at the message it left from" waitfor 10 back_where
"$DRIVE" shot navhistory_back >/dev/null
"$DRIVE" key alt+right >/dev/null
check "Alt+Right goes forward again" waitfor 10 at $((POS0 + 1)) "$B"

step "the header's Back button"
IFS=, read -r bl bt _ bb <<< "$(hist btn)"
[ -n "${bb:-}" ] || gui_abort "the dump gives no rectangle for the header's Back button"
"$DRIVE" click $((bl + 10)) $(( (bt + bb) / 2 )) >/dev/null
check "Back in the header returns to general" waitfor 10 at "$POS0" "$A"

step "somewhere new drops what was forward"
"$DRIVE" channel third >/dev/null
check "third is the newest place" waitfor 10 at $((POS0 + 1)) "$C"
nofwd() { gui_snap; [ "$(hist fwd)" = 0 ] && [ "$(hist n)" = $((POS0 + 2)) ]; }
check "...and there is nothing forward" waitfor 5 nofwd

step "the DMs person list has a header of its own"
# The header line and the banner's rectangle, from the dump.
hdr_line() { grep -m1 '^header ' "$D"; }
hdr_r()    { hdr_line | grep -o ' r=[0-9,.-]*' | cut -d= -f2; }
hdr_new()  { hdr_line | grep -o ' new=[0-9,.-]*' | cut -d= -f2; }
ban_r()    { grep -m1 '^banner ' "$D" | grep -o ' r=[0-9,.-]*' | cut -d= -f2; }
"$DRIVE" view dms >/dev/null || gui_abort "the client did not open the DMs view"
"$DRIVE" dmcompose 0 >/dev/null
dms_header() { gui_snap; local h; h="$(hdr_line)"; grep '^header title="Direct messages" .* dm_index=1 ' <<<"$h" >/dev/null; }
check "the DMs list is headed 'Direct messages'" waitfor 10 dms_header
can_back() { gui_snap; [ "$(hist back)" = 1 ]; }
check "...with Back to go where you were" waitfor 5 can_back
"$DRIVE" banner demo >/dev/null || gui_abort "the client did not raise the banner"
under() {
  gui_snap
  local hb bt
  hb="$(hdr_r | cut -d, -f4)"; bt="$(ban_r | cut -d, -f2)"
  [ -n "$hb" ] && [ -n "$bt" ] && [ "$hb" -gt 0 ] && [ "$bt" = "$hb" ]
}
check "a banner sits directly under that header, nothing blank above it" waitfor 10 under
"$DRIVE" shot navhist_dms_banner >/dev/null
"$DRIVE" banner off >/dev/null
click_rect "$(hdr_new)" || gui_abort "the dump gives no rectangle for the header's New message"
# New message, as the sidebar's compose button does: the New message pane (view 6).
newmsg() { gui_snap; [ "$(grep -m1 '^view=' "$D" | cut -d' ' -f1)" = view=6 ]; }
check "its New message opens the New message pane" waitfor 10 newmsg
"$DRIVE" view dms >/dev/null
"$DRIVE" groupdm bob,carol >/dev/null || gui_abort "the client did not take groupdm"
# A group DM has no name: it is the conversation newer than every channel made here.
newest_id() { grep '^  ch [0-9]' "$D" | awk '{print $2}' | sort -n | tail -1; }
made_dm() { gui_snap; [ "$(newest_id)" -gt "$C" ] 2>/dev/null; }
waitfor 15 made_dm || gui_abort "the group DM was not made"
"$DRIVE" channel "$(newest_id)" >/dev/null
in_dm() { gui_snap; local h; h="$(hdr_line)"; grep ' dm_index=0 ' <<<"$h" >/dev/null && ! grep 'title="Direct messages"' <<<"$h" >/dev/null; }
check "opening a conversation shows its own header instead" waitfor 15 in_dm
gui_end
