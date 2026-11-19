#!/usr/bin/env bash
# Cameras in the call view, two clients end to end (REQ-160, docs/CALLS.md §2, §7).
#
#   tools/gui/camera.sh            # run, and leave the pair running
#   tools/gui/camera.sh --down     # ... and take it down afterwards
#
# alice and bob, each a real client with the synthetic audio devices and the
# synthetic camera, over tools/gui/pair.sh, against a real daemon and its relay.
# It asserts that
#
#   - the call view offers a camera button (call.camera), and Settings a camera
#     picker (call.camera.picker) listing the synthetic camera;
#   - alice turns hers on: her own card shows it at once, mirrored, and the
#     roster marks it on for bob;
#   - both on: each card shows the other's camera, its frame numbers rising;
#   - the grid: a card each, and the view takes every camera;
#   - the speaker layout: bob large on alice's view, alice in the strip; and
#     back to the grid with one click, remembered;
#   - away from the call view nothing is drawn and no camera is taken; back,
#     the view takes the cameras of the cards it draws again;
#   - bob turns his off: his card on alice's view is his face again;
#   - with the camera refused (devenv OPENCHIME_DEV_CAPTURE denied, in the
#     same pair), turning it on says so, in the words the video recorder uses,
#     and leaves it off.
#
# Its own daemon on its own port, wiped each run. The clients reach it at the
# WSL machine's address, not 127.0.0.1: WSL forwards only TCP there, and a
# call's media is UDP.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
. "$HERE/tools/gui/lib.sh"

export OC_TEST_PORT="${OC_TEST_PORT:-9676}"
export OC_TEST_DIR="${OC_TEST_DIR:-/tmp/oc-camera}"
export OC_TEST_HOST="${OC_TEST_HOST:-$(ip -4 -o addr show eth0 | awk '{print $4}' | cut -d/ -f1)}"
export OPENCHIME_DEV_AUDIO=synthetic
export OPENCHIME_DEV_CAPTURE=synthetic   # the synthetic camera: frame numbers in its pixels
export OC_TEST_TONE_A=440 OC_TEST_TONE_B=660
CH=1                                   # #general: both are members

dump() { "$PAIR" "$1" dump "camera_$1" > "/tmp/oc-camera-dump-$1.txt" 2>&1; }
field() {  # field <who> <line-prefix> <key>
  grep -m1 "^$2" "/tmp/oc-camera-dump-$1.txt" | tr ' ' '\n' | grep -m1 "^$3=" | cut -d= -f2- | tr -d '"'
}
waitfor() {  # waitfor <who> <secs> <command...>
  local who="$1" secs="$2" t=0; shift 2
  while [ "$t" -lt $((secs * 4)) ]; do
    dump "$who"
    if "$@"; then return 0; fi
    sleep 0.25; t=$((t + 1))
  done
  return 1
}
num() { [ -n "$1" ] && [ "$1" -eq "$1" ] 2>/dev/null; }
ge()  { num "$1" && [ "$1" -ge "$2" ]; }
gt()  { num "$1" && [ "$1" -gt "$2" ]; }
has_aid() { grep -q "^a11yitem $2 " "/tmp/oc-camera-dump-$1.txt"; }
# The card or tile <who>'s view drew for <uid>: the value of <key> on its line.
tile() {  # tile <who> <uid> <key>
  grep -m1 "^camera.tile uid=$2 " "/tmp/oc-camera-dump-$1.txt" | tr ' ' '\n' | grep -m1 "^$3=" | cut -d= -f2
}
cpeer() {  # cpeer <who> <uid> <key>: the engine's view of <uid>'s camera
  grep -m1 "^camera.peer uid=$2 " "/tmp/oc-camera-dump-$1.txt" | tr ' ' '\n' | grep -m1 "^$3=" | cut -d= -f2
}
ntiles() { grep -c '^camera.tile ' "/tmp/oc-camera-dump-$1.txt"; }

say "== fixture: a fresh daemon, alice and bob at $OC_TEST_HOST:$OC_TEST_PORT"
"$PAIR" down >/dev/null 2>&1
rm -rf "$OC_TEST_DIR"
"$PAIR" up || { say "pair did not come up"; exit 1; }

a_in() { [ "$(field a 'call ' in)" = 1 ]; }

say "== alice starts a call in #general, and bob joins"
"$PAIR" a call start $CH >/dev/null
"$PAIR" a call ns off >/dev/null
waitfor a 10 a_in >/dev/null
"$PAIR" b call join $CH >/dev/null
"$PAIR" b call ns off >/dev/null
two_a() { [ "$(field a 'call ' parts)" = 2 ]; }
two_b() { [ "$(field b 'call ' parts)" = 2 ]; }
check "bob is in it, with alice" waitfor b 10 two_b
check "alice sees two in it" waitfor a 10 two_a
ua="$(field a 'call ' self)"; ub="$(field b 'call ' self)"
"$PAIR" a call open $CH >/dev/null
"$PAIR" b call open $CH >/dev/null

say "== the controls"
dump a
check "the call view offers a camera button (call.camera)" has_aid a call.camera
"$PAIR" a call settings >/dev/null
dump a
check "Settings offers a camera picker (call.camera.picker)" has_aid a call.camera.picker
check "...which has the synthetic camera to pick" ge "$(field a 'camera ' cams)" 1
"$PAIR" a call settings >/dev/null

say "== alice turns her camera on"
"$PAIR" a call camera on >/dev/null
own() { [ "$(field a 'camera ' on)" = 1 ] && [ "$(tile a "$ua" cam)" = 1 ] && ge "$(tile a "$ua" fno)" 0; }
check "her own card shows her camera at once" waitfor a 10 own
sending() { [ "$(field a 'camera ' state)" = 2 ]; }
check "the roster marks it on, and it goes out" waitfor a 10 sending
"$PAIR" a shot camera_self >/dev/null

say "== both cameras on"
"$PAIR" b call camera on >/dev/null
f1=""
b_sees_a() { [ "$(tile b "$ua" cam)" = 1 ] && ge "$(tile b "$ua" fno)" 0 && ge "$(cpeer b "$ua" frames)" 10; }
a_sees_b() { [ "$(tile a "$ub" cam)" = 1 ] && ge "$(tile a "$ub" fno)" 0 && ge "$(cpeer a "$ub" frames)" 10; }
check "bob's card for alice shows her camera" waitfor b 15 b_sees_a
check "alice's card for bob shows his" waitfor a 15 a_sees_b
f1="$(tile a "$ub" fno)"
rising() { gt "$(tile a "$ub" fno)" "$f1"; }
check "and his picture keeps coming, its frame numbers rising" waitfor a 10 rising
check "it is at the size he sends (640x480)" bash -c "[ \"\$(grep -m1 '^camera.peer uid=$ub ' /tmp/oc-camera-dump-a.txt | grep -o 'w=[0-9]* h=[0-9]*')\" = 'w=640 h=480' ]"
"$PAIR" a shot camera_grid >/dev/null

say "== the grid"
dump a
check "a card each, neither the large one" bash -c "[ \"\$(grep -c '^camera.tile .* big=0 ' /tmp/oc-camera-dump-a.txt)\" = 2 ]"
check "the layout is the grid" test "$(field a 'camera ' layout)" = grid
check "the view takes every camera: every other card is drawn" test "$(field a 'camera ' sub_all)" = 1
check "...and the engine takes bob's" test "$(cpeer a "$ub" subscribed)" = 1

say "== the speaker layout"
"$PAIR" a call layout speaker >/dev/null
speaker() { [ "$(field a 'camera ' layout)" = speaker ] && [ "$(tile a "$ub" big)" = 1 ] && [ "$(tile a "$ua" big)" = 0 ]; }
check "bob is large on alice's view, alice in the strip" waitfor a 10 speaker
check "...with his camera on it" test "$(tile a "$ub" cam)" = 1
check "the view still takes his camera, the one it draws" test "$(field a 'camera ' sub_all)" = 1
check "the layout offers its way back (call.layout)" has_aid a call.layout
"$PAIR" a shot camera_speaker >/dev/null
"$PAIR" a call layout grid >/dev/null
grid() { [ "$(field a 'camera ' layout)" = grid ] && [ "$(grep -c '^camera.tile .* big=1 ' /tmp/oc-camera-dump-a.txt)" = 0 ]; }
check "one click, and it is the grid again" waitfor a 10 grid

say "== the subscriptions follow what is drawn"
"$PAIR" a view home >/dev/null
none() { [ "$(field a 'camera ' drawn)" = 0 ] && [ "$(field a 'camera ' sub_all)" = 0 ] &&
         [ "$(field a 'camera ' subs)" = - ] && [ "$(cpeer a "$ub" subscribed)" = 0 ]; }
check "away from the call view, no card is drawn and no camera taken" waitfor a 10 none
"$PAIR" a call open $CH >/dev/null
again() { [ "$(field a 'camera ' sub_all)" = 1 ] && [ "$(cpeer a "$ub" subscribed)" = 1 ] && [ "$(tile a "$ub" cam)" = 1 ]; }
check "back in it, his card is drawn, his camera taken, and his picture back" waitfor a 15 again

say "== bob turns his camera off"
"$PAIR" b call camera off >/dev/null
b_off() { [ "$(tile a "$ub" cam)" = 0 ] && [ "$(cpeer a "$ub" on)" = 0 ]; }
check "his card on alice's view is his face again" waitfor a 10 b_off
"$PAIR" a shot camera_off >/dev/null
"$PAIR" a call camera off >/dev/null
a_off() { [ "$(field a 'camera ' on)" = 0 ] && [ "$(field a 'camera ' state)" = 0 ] && [ "$(tile a "$ua" cam)" = 0 ]; }
check "alice turns hers off: off, and her own card her face" waitfor a 10 a_off

say "== a camera the system refuses"
# The same pair: the client is told its camera is refused from here on
# (devenv, development mode only), rather than a second pair being started.
"$PAIR" a devenv OPENCHIME_DEV_CAPTURE denied >/dev/null
"$PAIR" a call camera on >/dev/null
refused() { [ "$(field a 'camera ' on)" = 0 ] && [ "$(field a 'camera ' shown_err)" = -1 ]; }
check "turning it on says the camera is blocked, and it stays off" waitfor a 10 refused
told() { grep -q 'blocking camera access for OpenChime' /tmp/oc-camera-dump-a.txt; }
check "...in the video recorder's words" told
"$PAIR" a shot camera_denied >/dev/null
"$PAIR" a devenv OPENCHIME_DEV_CAPTURE synthetic >/dev/null
"$PAIR" a call end >/dev/null

say ""
say "gui_camera: $((checks - fails))/$checks passed in $((SECONDS - START))s"
[ "${1:-}" = "--down" ] && "$PAIR" down >/dev/null
[ "$fails" = 0 ]
