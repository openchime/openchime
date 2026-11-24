#!/usr/bin/env bash
# A call in the Win32 client when UDP cannot reach the relay (PROTOCOL.md §5.17,
# AUDIO.md §4): two clients end to end, their media carried by the connection.
#
#   tools/gui/features/calls_tcp.sh            # run; the pair and its daemon stop at the end
#   tools/gui/features/calls_tcp.sh --keep     # ... and leave them running
#
# The daemon tells the clients the relay is at a UDP port nothing listens on
# (OPENCHIME_MANAGED_AUDIO_ADVERTISE_PORT), so every datagram they send is lost and none
# comes back -- a network that blocks UDP, as far as either can tell. It asserts
# that each client finds that within seconds and moves to the connection, that
# each then hears the other's tone, and that the call view says where the audio
# is going. Its own daemon on its own port, wiped each run.
#
# platforms: win32 web
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
. "$HERE/tools/gui/harness/lib.sh"
gui_begin calls_tcp 9670 /tmp/oc-calls-tcp "$@"
export OC_TEST_HOST="${OC_TEST_HOST:-$(ip -4 -o addr show eth0 | awk '{print $4}' | cut -d/ -f1)}"
export OC_TEST_ADVERTISE_PORT=9   # discard: nothing answers there
export OPENCHIME_DEV_AUDIO=synthetic
export OC_TEST_TONE_A=440 OC_TEST_TONE_B=660
CH=1

cfield() { field "$(dfile "$1")" '^call ' "$2"; }      # cfield <who> <key>: the call line
peer() { field "$(dfile "$1")" '^call\.peer' "$2"; }   # peer <who> <key>: the other's
num() { [ -n "$1" ] && [ "$1" -eq "$1" ] 2>/dev/null; }
gt()  { num "$1" && [ "$1" -gt "$2" ]; }

step "a fresh daemon whose relay the clients cannot reach over UDP"
gui_pair_up

step "alice starts a call, bob joins"
"$PAIR" a call start $CH >/dev/null || gui_abort "alice's client did not take 'call start'"
# The call button opens New Call, its subject and people filled in; Start starts it.
picker_up() { gui_snap a && grep -q '^callpick ' "$DA"; }
waitfor 10 picker_up || gui_abort "alice's call button did not open New Call"
"$PAIR" a call pick-go >/dev/null || gui_abort "alice's client did not take 'call pick-go'"
"$PAIR" a call ns off >/dev/null || gui_abort "alice's client did not turn noise suppression off"
"$PAIR" b call ns off >/dev/null || gui_abort "bob's client did not turn noise suppression off"
a_in() { gui_snap a && [ "$(cfield a in)" = 1 ]; }
waitfor 10 a_in || gui_abort "alice is not in the call she started"
"$PAIR" b call join $CH >/dev/null || gui_abort "bob's client did not take 'call join'"
# Every join goes through the pre-join screen: its Join.
pj_up() { gui_snap b && grep -q '^prejoin open=1' "$DB"; }
waitfor 10 pj_up || gui_abort "bob's pre-join screen did not open"
"$PAIR" b call prejoin-join >/dev/null || gui_abort "bob's client did not take 'call prejoin-join'"
a_tcp() { gui_snap a && [ "$(cfield a transport)" = tcp ]; }
b_tcp() { gui_snap b && [ "$(cfield b transport)" = tcp ]; }
check "alice finds UDP does not get through, and goes by the connection" waitfor 15 a_tcp
check "so does bob" waitfor 15 b_tcp
a_hears_b() { gui_snap a && gt "$(peer a packets)" 50 && gt "$(peer a level)" 2000; }
b_hears_a() { gui_snap b && gt "$(peer b packets)" 50 && gt "$(peer b level)" 2000; }
check "alice hears bob's tone" waitfor 20 a_hears_b
check "bob hears alice's tone" waitfor 20 b_hears_a
say "  alice from bob: packets=$(peer a packets) level=$(peer a level) lost=$(peer a lost) late=$(peer a late)"

step "the call view"
"$PAIR" a call open $CH >/dev/null || gui_abort "alice's client did not open the call view"
a_view() { gui_snap a && [ "$(cfield a inview)" = 1 ]; }
waitfor 5 a_view || gui_abort "alice's call view did not open"
"$PAIR" a shot callstcp_incall >/dev/null
# The route is the call's business: the view gives the usual hint, and says
# nothing of the network blocking the call's own route. Read from alice's own
# dump, taken with her call view open and the call on the connection.
quiet() { [ "$(cfield a transport)" = tcp ] && ! grep 'blocks the call' "$DA" >/dev/null; }
check "the call view, on the connection, says nothing of a blocked route" quiet
"$PAIR" a call leave >/dev/null
"$PAIR" b call leave >/dev/null
gui_end
