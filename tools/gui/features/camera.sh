#!/usr/bin/env bash
# Cameras in the call view, two clients end to end (REQ-160, docs/details/CALLS.md §2, §7).
#
#   tools/gui/features/camera.sh            # run; the pair and its daemon stop at the end
#   tools/gui/features/camera.sh --keep     # ... and leave them running
#
# alice and bob, each a real client with the synthetic audio devices and the
# synthetic camera, over tools/gui/harness/win32/pair.sh, against a real daemon and its relay.
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
#   - self-view: shrunk to a corner, hidden, shown again, each kept with the
#     preferences;
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
#
# platforms: win32 web
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
. "$HERE/tools/gui/harness/lib.sh"
gui_begin camera 9676 /tmp/oc-camera "$@"
export OC_TEST_HOST="${OC_TEST_HOST:-$(ip -4 -o addr show eth0 | awk '{print $4}' | cut -d/ -f1)}"
export OPENCHIME_DEV_AUDIO=synthetic
export OPENCHIME_DEV_CAPTURE=synthetic   # the synthetic camera: frame numbers in its pixels
export OC_TEST_TONE_A=440 OC_TEST_TONE_B=660
CH=1                                   # #general: both are members

# act, dfile and seen: lib.sh.
f() { field "$(dfile "$1")" "^$2" "$3"; }   # f <who> <line-prefix> <key>
num() { [ -n "$1" ] && [ "$1" -eq "$1" ] 2>/dev/null; }
ge()  { num "$1" && [ "$1" -ge "$2" ]; }
gt()  { num "$1" && [ "$1" -gt "$2" ]; }
has_aid() { grep "^a11yitem $2 " "$(dfile "$1")" >/dev/null; }
# The card or tile <who>'s view drew for <uid>: the value of <key> on its line.
tile() { field "$(dfile "$1")" "^camera\.tile uid=$2 " "$3"; }     # tile <who> <uid> <key>
cpeer() { field "$(dfile "$1")" "^camera\.peer uid=$2 " "$3"; }    # cpeer <who> <uid> <key>: the engine's view of <uid>'s camera
has() { grep -E "$2" "$(dfile "$1")" >/dev/null; }
nbig() { grep -c "^camera\.tile .* big=$2 " "$(dfile "$1")"; }    # nbig <who> 0|1: cards (not) the large one

step "a fresh daemon, alice and bob at $OC_TEST_HOST:$OC_TEST_PORT"
gui_pair_up

a_in() { [ "$(f a 'call ' in)" = 1 ]; }

step "alice starts a call in #general, and bob joins"
act a call start $CH
# The call button opens New Call, its subject and people filled in; Start starts it.
picker_up() { grep -q '^callpick ' "$(dfile a)"; }
waitfor 10 seen a picker_up || gui_abort "alice's call button did not open New Call"
act a call pick-go
act a call ns off
waitfor 10 seen a a_in || gui_abort "alice is not in the call she started"
act b call join $CH
pj_up() { grep -q '^prejoin open=1' "$(dfile b)"; }   # every join goes through the pre-join screen
waitfor 10 seen b pj_up || gui_abort "bob's pre-join screen did not open"
act b call prejoin-join
act b call ns off
two_a() { [ "$(f a 'call ' parts)" = 2 ]; }
two_b() { [ "$(f b 'call ' parts)" = 2 ]; }
check "bob is in it, with alice" waitfor 10 seen b two_b
check "alice sees two in it" waitfor 10 seen a two_a
ua="$(f a 'call ' self)"; ub="$(f b 'call ' self)"
[ -n "$ua" ] && [ -n "$ub" ] && [ "$ua" != 0 ] && [ "$ub" != 0 ] || gui_abort "the dumps do not say who alice and bob are in the call (self=$ua, $ub)"
act a call open $CH
act b call open $CH

step "the controls"
check "the call view offers a camera button (call.camera)" waitfor 5 seen a has_aid a call.camera
act a call settings
check "Settings offers a camera picker (call.camera.picker)" waitfor 5 seen a has_aid a call.camera.picker
cams() { ge "$(f a 'camera ' cams)" 1; }
check "...which has the synthetic camera to pick" cams
act a call settings

step "alice turns her camera on"
act a call camera on
own() { [ "$(f a 'camera ' on)" = 1 ] && [ "$(tile a "$ua" cam)" = 1 ] && ge "$(tile a "$ua" fno)" 0; }
check "her own card shows her camera at once" waitfor 10 seen a own
sending() { [ "$(f a 'camera ' state)" = 2 ]; }
check "the roster marks it on, and it goes out" waitfor 10 seen a sending
act a shot camera_self

step "both cameras on"
act b call camera on
b_sees_a() { [ "$(tile b "$ua" cam)" = 1 ] && ge "$(tile b "$ua" fno)" 0 && ge "$(cpeer b "$ua" frames)" 10; }
a_sees_b() { [ "$(tile a "$ub" cam)" = 1 ] && ge "$(tile a "$ub" fno)" 0 && ge "$(cpeer a "$ub" frames)" 10; }
check "bob's card for alice shows her camera" waitfor 15 seen b b_sees_a
check "alice's card for bob shows his" waitfor 15 seen a a_sees_b
f1="$(tile a "$ub" fno)"
rising() { gt "$(tile a "$ub" fno)" "$f1"; }
check "and his picture keeps coming, its frame numbers rising" waitfor 10 seen a rising
size640() { [ "$(cpeer a "$ub" w)" = 640 ] && [ "$(cpeer a "$ub" h)" = 480 ]; }
check "it is at the size he sends (640x480)" size640
act a shot camera_grid

step "the grid"
gui_snap a || gui_abort "alice's client did not dump"
two_small() { [ "$(nbig a 0)" = 2 ]; }
check "a card each, neither the large one" two_small
check "the layout is the grid" test "$(f a 'camera ' layout)" = grid
check "the view takes every camera: every other card is drawn" test "$(f a 'camera ' sub_all)" = 1
check "...and the engine takes bob's" test "$(cpeer a "$ub" subscribed)" = 1

step "the speaker layout"
act a call layout speaker
speaker() { [ "$(f a 'camera ' layout)" = speaker ] && [ "$(tile a "$ub" big)" = 1 ] && [ "$(tile a "$ua" big)" = 0 ]; }
check "bob is large on alice's view, alice in the strip" waitfor 10 seen a speaker
check "...with his camera on it" test "$(tile a "$ub" cam)" = 1
check "the view still takes his camera, the one it draws" test "$(f a 'camera ' sub_all)" = 1
check "the layout offers its way back (call.layout)" has_aid a call.layout
act a shot camera_speaker
act a call layout grid
grid() { [ "$(f a 'camera ' layout)" = grid ] && [ "$(nbig a 1)" = 0 ]; }
check "one click, and it is the grid again" waitfor 10 seen a grid

step "self-view"
press a call.selfview || gui_abort "the call view has no self-view button"
small() { has a '^selfview mode=1 small=[0-9]+,[0-9]+,[1-9]' && [ "$(f a selfview stored)" = 1 ] &&
          [ "$(tile a "$ua" rect)" = "$(f a selfview small)" ] && [ "$(grep -c "^camera\.tile uid=$ua " "$(dfile a)")" = 1 ]; }
check "shrunk: alice's own picture small in the corner, bob's card alone in the grid, kept in her preferences" waitfor 10 seen a small
act a shot camera_selfview_small
press a call.selfview || gui_abort "the self-view button went away"
hidden() { has a '^selfview mode=2 small=0,0,0,0' && [ "$(nbig a 0)" = 1 ] && [ "$(f a selfview stored)" = 2 ]; }
check "hidden: no picture of herself at all, and kept" waitfor 10 seen a hidden
press a call.selfview || gui_abort "the self-view button went away"
shown() { has a '^selfview mode=0 ' && [ "$(nbig a 0)" = 2 ] && [ "$(f a selfview stored)" = 0 ]; }
check "shown again: her card back beside bob's" waitfor 10 seen a shown

step "the subscriptions follow what is drawn"
act a view home
none() { [ "$(f a 'camera ' drawn)" = 0 ] && [ "$(f a 'camera ' sub_all)" = 0 ] &&
         [ "$(f a 'camera ' subs)" = - ] && [ "$(cpeer a "$ub" subscribed)" = 0 ]; }
check "away from the call view, no card is drawn and no camera taken" waitfor 10 seen a none
act a call open $CH
again() { [ "$(f a 'camera ' sub_all)" = 1 ] && [ "$(cpeer a "$ub" subscribed)" = 1 ] && [ "$(tile a "$ub" cam)" = 1 ]; }
check "back in it, his card is drawn, his camera taken, and his picture back" waitfor 15 seen a again

step "bob turns his camera off"
act b call camera off
b_off() { [ "$(tile a "$ub" cam)" = 0 ] && [ "$(cpeer a "$ub" on)" = 0 ]; }
check "his card on alice's view is his face again" waitfor 10 seen a b_off
act a shot camera_off
act a call camera off
a_off() { [ "$(f a 'camera ' on)" = 0 ] && [ "$(f a 'camera ' state)" = 0 ] && [ "$(tile a "$ua" cam)" = 0 ]; }
check "alice turns hers off: off, and her own card her face" waitfor 10 seen a a_off

step "a camera the system refuses"
# The same pair: the client is told its camera is refused from here on
# (devenv, development mode only), rather than a second pair being started.
act a devenv OPENCHIME_DEV_CAPTURE denied
act a call camera on
refused() { [ "$(f a 'camera ' on)" = 0 ] && [ "$(f a 'camera ' shown_err)" = -1 ]; }
check "turning it on says the camera is blocked, and it stays off" waitfor 10 seen a refused
told() { grep 'blocking camera access for OpenChime' "$DA" >/dev/null; }
check "...in the video recorder's words" told
act a shot camera_denied
act a devenv OPENCHIME_DEV_CAPTURE synthetic
act a call end
gui_end
