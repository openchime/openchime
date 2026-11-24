#!/usr/bin/env bash
# The pre-join screen every join goes through (CALLS.md §2, §8.4), two clients
# end to end.
#
#   tools/gui/features/prejoin.sh            # run; the pair and its daemon stop at the end
#   tools/gui/features/prejoin.sh --keep     # ... and leave them running
#
# alice starts calls in #general; bob joins them. It asserts that
#
#   - joining opens the pre-join screen, not the call: it names who is in it,
#     says the call is end-to-end encrypted, previews the camera and the
#     microphone, and offers the microphone, speaker and camera pickers; bob is
#     not in the call until he presses Join, and Cancel leaves him out of it;
#   - with the microphone off there, bob joins muted;
#   - with the camera on there, he joins with it on;
#   - a camera that cannot start (OPENCHIME_DEV_CAPTURE=denied) is said there,
#     the camera cannot be turned on, and he joins with audio only, heard.
#
# platforms: win32 web
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
. "$HERE/tools/gui/harness/lib.sh"
gui_begin prejoin 9820 /tmp/oc-prejoin "$@"

export OC_TEST_HOST="${OC_TEST_HOST:-$(ip -4 -o addr show eth0 | awk '{print $4}' | cut -d/ -f1)}"
export OPENCHIME_DEV_AUDIO=synthetic
export OPENCHIME_DEV_CAPTURE=synthetic
export OC_TEST_TONE_A=440 OC_TEST_TONE_B=660
CH=1

f() { field "$(dfile "$1")" "^$2" "$3"; }
has() { grep -E "$2" "$(dfile "$1")" >/dev/null; }
num() { [ -n "$1" ] && [ "$1" -eq "$1" ] 2>/dev/null; }
gt()  { num "$1" && [ "$1" -gt "$2" ]; }
tap() { act "$1" key "$2"; act "$1" keyup "$2"; }
peer() { f "$1" 'call\.peer' "$2"; }
in_call() { [ "$(f "$1" 'call ' in)" = 1 ]; }
out_of() { [ "$(f "$1" 'call ' in)" = 0 ]; }
pj_up() { has b '^prejoin open=1 '; }

# alice starts a call in #general from its call button, and waits in it.
start_call() {
  act a call start $CH
  picker_up() { has a '^callpick '; }
  waitfor 10 seen a picker_up || gui_abort "alice's call button did not open New Call"
  act a call pick-go
  waitfor 10 seen a in_call a || gui_abort "alice's call did not start"
  act a call ns off
}
end_call() {
  act a call end
  waitfor 10 seen a out_of a || gui_abort "alice's call did not end"
  waitfor 10 seen b out_of b || gui_abort "bob was still in the ended call"
}

step "a fresh daemon, alice and bob"
gui_pair_up

step "joining opens the pre-join screen"
start_call
act b call join $CH
check "bob's Join opens the pre-join screen" waitfor 10 seen b pj_up
check "...not the call" seen b out_of b
check "it names who is in it: alice" seen b has b '^prejoin open=1 .* people=1$'
check "it says the call is end-to-end encrypted" seen b has b '^prejoin open=1 .* recorded=0 '
pickers() { has b '^a11yitem call\.mic ' && has b '^a11yitem call\.speaker ' && has b '^a11yitem call\.camera\.picker '; }
check "it offers the microphone, speaker and camera pickers" waitfor 5 seen b pickers
act b call prejoin-cam
previewing() { has b '^prejoin open=1 .* cam=1 cam_err=0 preview=1 '; }
check "with the camera on, it previews it" waitfor 10 seen b previewing
act b shot prejoin_screen
act b call prejoin-cam
tap b esc
closed() { has b '^prejoin open=0 '; }
check "Cancel leaves bob out of the call" waitfor 5 seen b closed
check "...still out" seen b out_of b

step "joining muted"
act b call join $CH
waitfor 10 seen b pj_up || gui_abort "the pre-join screen did not open again"
act b call prejoin-mic
mic_off() { has b '^prejoin open=1 .* mic=0 '; }
check "the microphone, off" waitfor 5 seen b mic_off
act b call prejoin-join
muted_in() { in_call b && [ "$(f b 'call ' muted)" = 1 ]; }
check "Join: bob is in the call, muted" waitfor 10 seen b muted_in
a_silent() { [ -z "$(peer a level)" ] || [ "$(peer a level)" -lt 300 ]; }
check "...and alice hears nothing of him" waitfor 10 seen a a_silent
act b call leave
waitfor 10 seen b out_of b || gui_abort "bob did not leave"
act b call unmute

step "joining with the camera on"
act b call join $CH
waitfor 10 seen b pj_up || gui_abort "the pre-join screen did not open again"
act b call prejoin-mic
act b call prejoin-cam
act b call prejoin-join
cam_in() { in_call b && [ "$(f b camera on)" = 1 ]; }
check "Join: bob is in the call with his camera on" waitfor 15 seen b cam_in
act b shot prejoin_joined_camera
act b call camera off
end_call

step "a camera that cannot start"
gui_teardown || gui_abort "the first fixture did not stop"
rm -rf "$OC_TEST_DIR"; mkdir -p "$OC_TEST_DIR"
export OPENCHIME_DEV_CAPTURE=denied
gui_pair_up
start_call
act b call join $CH
waitfor 10 seen b pj_up || gui_abort "bob's pre-join screen did not open"
said() { has b '^prejoin open=1 .* cam_err=-?[1-9]'; }
check "the pre-join screen says the camera could not start" waitfor 10 seen b said
act b shot prejoin_no_camera
act b call prejoin-cam
cam_stays_off() { has b '^prejoin open=1 .* cam=0 '; }
check "...and the camera cannot be turned on" seen b cam_stays_off
act b call ns off
act b call prejoin-join
audio_only() { in_call b && [ "$(f b camera on)" = 0 ]; }
check "Join: bob is in with audio only" waitfor 10 seen b audio_only
heard() { gt "$(peer a packets)" 50 && gt "$(peer a level)" 2000; }
check "...and alice hears him" waitfor 15 seen a heard
gui_end
