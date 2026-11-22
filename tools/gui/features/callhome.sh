#!/usr/bin/env bash
# Calls home and a call's page, two clients end to end (CALLS.md §1, §8).
#
#   tools/gui/features/callhome.sh            # run; the pair and its daemon stop at the end
#   tools/gui/features/callhome.sh --keep     # ... and leave them running
#
# alice and bob, each a real client with the synthetic audio devices (440 Hz and
# 660 Hz), over the pair harness, against a real daemon that records calls. It
# asserts that
#
#   - Calls home, opened from the sidebar's Calls row, says no call is going on;
#   - alice's New call asks for a subject and the people: "Quarterly plan", bob
#     ticked; the call starts from no conversation, and her call view names it;
#   - bob's Calls home lists it going on, with him asked and its own Join, which
#     puts him in it;
#   - alice records a few seconds and stops; she ends the call;
#   - bob's Calls home lists it before now, joined and recorded; its page names
#     both people with their times, the timeline has the recording, the call's
#     chat is beside it, and the recording -- made by the daemon -- plays.
#
# Its own daemon on its own port, wiped each run. The clients reach it at the
# WSL machine's address: a call's audio is UDP, which WSL forwards only there.
#
# platforms: win32 web
# needs: callrec
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
. "$HERE/tools/gui/harness/lib.sh"
gui_begin callhome 9800 /tmp/oc-callhome "$@"

export OC_TEST_HOST="${OC_TEST_HOST:-$(ip -4 -o addr show eth0 | awk '{print $4}' | cut -d/ -f1)}"
export OPENCHIME_DEV_AUDIO=synthetic
export OC_TEST_TONE_A=440 OC_TEST_TONE_B=660

f() { field "$(dfile "$1")" "^$2" "$3"; }             # f <who> <line-prefix> <key>
has() { grep -E "$2" "$(dfile "$1")" >/dev/null; }    # has <who> <ERE>
num() { [ -n "$1" ] && [ "$1" -eq "$1" ] 2>/dev/null; }
ge()  { num "$1" && [ "$1" -ge "$2" ]; }
gt()  { num "$1" && [ "$1" -gt "$2" ]; }
peer() { f "$1" 'call\.peer' "$2"; }

step "a fresh daemon, alice and bob at $OC_TEST_HOST:$OC_TEST_PORT"
gui_pair_up
rec_offered() { gui_snap a && has a '^callobj .* rec_cap=1$'; }
check "the daemon records calls" waitfor 5 rec_offered

step "Calls home"
act b call home
home_empty() { has b '^callhome loaded=1 ' && has b '^calls n=0 ' && has b '^a11yitem calls\.new '; }
check "bob's Calls home: nothing going on, and New call" waitfor 10 seen b home_empty
act b shot callhome_empty

step "alice's New call"
act a call home
act a call new
picking() { has a '^callpick ch=0 invite=0 n=[1-9]'; }
check "New call opens the picker: a subject to give, people to ask" waitfor 10 seen a picking
BOB="$(grep -m1 '^callpick ' "$DA" | grep -o 'on=[0-9]*' | cut -d= -f2)"
[ -n "$BOB" ] || gui_abort "alice's picker offers nobody"
act a call subject "Quarterly plan"
press a "call.pick.$BOB" || gui_abort "bob's row in the picker was not there to tick"
ticked() { has a '^callpick .*subject="Quarterly plan" on='"$BOB"':1'; }
check "...\"Quarterly plan\", bob ticked" waitfor 5 seen a ticked
act a shot callhome_new
act a call pick-go
started() { [ "$(f a 'call ' in)" = 1 ] && [ "$(f a 'call ' ch)" = 0 ] && has a '^callobj view=[1-9][0-9]* subject="Quarterly plan" chat=[1-9]'; }
check "the call starts, from no conversation, and her view names it" waitfor 10 seen a started
ID="$(f a 'call ' id)"
act a call ns off

step "bob, from Calls home"
act b call home
listed() { has b "^a11yitem calls\\.join\\.$ID " && has b "^calls\\[0\\] .*id=$ID .*me_invited=1"; }
check "bob's Calls home lists it going on, him asked, with its Join" waitfor 10 seen b listed
act b shot callhome_going
press b "calls.join.$ID" || gui_abort "the row's Join was not there"
two() { [ "$(f "$1" 'call ' parts)" = 2 ]; }
check "its Join puts bob in it" waitfor 10 seen b two b
act b call ns off
hears() { gt "$(peer a packets)" 50 && gt "$(peer a level)" 2000; }
check "alice hears him" waitfor 15 seen a hears

step "recorded, and ended"
act a call record on
marked() { has a "^callobj view=$ID .* recording=1 "; }
check "alice records" waitfor 10 seen a marked
# Some seconds of both of them.
p0="$(peer a packets)"
enough() { ge "$(peer a packets)" $(( ${p0:-0} + 200 )); }
waitfor 15 seen a enough || gui_abort "bob's audio stopped reaching alice while recorded"
act a call record off
act a call end
out() { [ "$(f "$1" 'call ' in)" = 0 ]; }
check "she ends it for both" waitfor 10 seen b out b

step "the call's page"
act b call home
# OC_CALL_HIST_JOINED (1) and OC_CALL_HIST_RECORDING (16): the recording is
# made once the call stops being recorded, so Calls home is asked again.
past() { act b call home; gui_snap b &&
         grep -E "^callhist id=$ID subject=\"Quarterly plan\" flags=[0-9]+ .* ended=1 " "$DB" |
         grep -oE 'flags=[0-9]+' | cut -d= -f2 | { read -r fl; [ -n "$fl" ] && [ $(( fl & 17 )) = 17 ]; }; }
check "bob's Calls home lists it before now: he was in it, and it is recorded" waitfor 30 past
act b shot callhome_past
# Wide enough for the page and the call's chat beside it.
act b size 1600 900
press b "calls.row.$ID" || gui_abort "the call's row in Calls home was not there to press"
paged() { has b "^callpage id=$ID shown=$ID subject=\"Quarterly plan\" stays=2 .* recordings=1 "; }
check "its page: both people, and the recording" waitfor 10 seen b paged
timeline() { has b '^callpage\.ev kind=5 ' && has b '^callpage\.ev kind=6 '; }
check "...the timeline has the recording started and stopped" timeline
chat_beside() { [ "$(f b 'callui ' chat)" = 1 ]; }
check "...the call's chat beside it" chat_beside
ready() { act b call page "$ID"; gui_snap b && has b '^callpage\.rec id=[0-9]+ state=3 .* ms=[1-9][0-9]{3,} attach=[1-9]'; }
check "...and the recording, made: ready, seconds long, a video to play" waitfor 30 ready
act b shot callhome_page
act b call play 0
playing() { [ "$(f b vm= play_loading)" = 0 ] && gt "$(f b vm= play_dur)" 1000 && gt "$(f b vm= play_pos)" 0; }
check "Play plays it, as a video message is played" waitfor 15 seen b playing
act b shot callhome_playing
gui_end
