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
#     chat is beside it, and the recording -- made by the daemon -- plays;
#   - the Calls panel (CALLS.md §2): eight calls bob misses mark the sidebar's
#     Calls row until he opens the panel; Past is under a "Today" heading, each
#     row with the faces of who took part; the Missed and Joined filters; Active
#     stays where it is while Past scrolls, and older calls load as it does (a
#     page of three, OPENCHIME_DEV_CALL_HISTORY_PAGE); on a wide window a call's
#     page opens beside the list, on a narrow one in its place with Back; and a
#     missed call's Call back opens New Call with its subject and people.
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
export OPENCHIME_DEV_CALL_HISTORY_PAGE=3      # a page of three: the list's next page is a few calls away

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
pj_up() { has b '^prejoin open=1'; }   # every join goes through the pre-join screen
waitfor 10 seen b pj_up || gui_abort "bob's pre-join screen did not open"
act b call prejoin-join
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
# Beside the list, the page offers its chat: Chat opens the page with it.
seen b has b '^view=11 ' && { press b callpage.chat || gui_abort "the page's Chat was not there"; }
chat_beside() { [ "$(f b 'callui ' chat)" = 1 ] && has b '^view=12 '; }
check "...the call's chat beside it" waitfor 10 seen b chat_beside
ready() { act b call page "$ID"; gui_snap b && has b '^callpage\.rec id=[0-9]+ state=3 .* ms=[1-9][0-9]{3,} attach=[1-9]'; }
check "...and the recording, made: ready, seconds long, a video to play" waitfor 30 ready
order() { has b '^call\.strip .* heads="Recording\|Who was there when\|People\|Timeline"'; }
check "the page: the recording first, then who was there when, then the people, then the timeline" waitfor 5 seen b order
strip() { has b '^call\.strip people=2 bars=2 rec=[0-9]+,' && [ "$(f b 'call\.strip' rec | cut -d, -f3)" != 0 ]; }
check "...a bar for each of the two, and the recording's span marked over them" strip
act b shot callhome_page
act b call play 0
playing() { [ "$(f b vm= play_loading)" = 0 ] && gt "$(f b vm= play_dur)" 1000 && gt "$(f b vm= play_pos)" 0; }
check "Play plays it, as a video message is played" waitfor 15 seen b playing
act b shot callhome_playing
act b key esc

step "missed calls, and the Calls panel"
# Eight calls alice makes that bob never takes.
for k in 1 2 3 4 5 6 7 8; do
  act a call new
  waitfor 10 seen a picking || gui_abort "alice's New call did not open (call $k)"
  act a call subject "Missed $k"
  press a "call.pick.$BOB" || gui_abort "bob was not in alice's picker (call $k)"
  act a call pick-go
  in_call() { [ "$(f a 'call ' in)" = 1 ]; }
  waitfor 10 seen a in_call || gui_abort "alice's call $k did not start"
  act a call leave
  left() { [ "$(f a 'call ' in)" = 0 ]; }
  waitfor 10 seen a left || gui_abort "alice did not leave call $k"
done
act b view home
missed_marked() { [ "$(f b callhome missed)" = 8 ] && [ "$(f b callhome missed_mark | cut -d, -f3)" != 0 ]; }
check "the sidebar's Calls row marks eight missed calls bob has not seen" waitfor 15 seen b missed_marked
act b shot callhome_missed_mark
act b size 1600 900
act b call home
seen_now() { [ "$(f b callhome missed)" = 0 ] && [ "$(f b callhome missed_mark | cut -d, -f3)" = 0 ]; }
check "opening Calls marks them seen: the mark is gone" waitfor 10 seen b seen_now
today() { has b '^callrow id=[0-9]+ head="Today" '; }
check "Past is under a \"Today\" heading" waitfor 10 seen b today
faces() { has b "^callhist id=$ID .* faces=2$"; }
act b call filter-reset
press b calls.filter.missed || gui_abort "the Missed filter was not there"
only_missed() { has b '^callhome .* filter=2 ' && ! grep -q "^callrow id=$ID " "$DB" && has b '^callrow '; }
check "Missed shows only the missed calls" waitfor 10 seen b only_missed
act b shot callhome_filter_missed
press b calls.filter.joined || gui_abort "the Joined filter was not there"
only_joined() { has b "^callrow id=$ID " && [ "$(grep -c '^callrow ' "$DB")" = 1 ]; }
check "Joined shows only the call bob was in" waitfor 10 seen b only_joined
press b calls.filter.all || gui_abort "the All filter was not there"
# A short window, so Past is longer than it, and the list asked for afresh: a page.
act b size 1600 480
act b call home
y0() { f b callhome active_y; }
waitfor 5 seen b has b '^callhome loaded=1 .* filter=0 ' || gui_abort "the list did not come back"
AY="$(y0)"; HN="$(f b callhome hist)"
# The wheel in the list, at its middle.
LX=0; LY=0
IFS=, read -r l t r bb <<< "$(f b callhome list)"
LX=$(( (${l%.*} + ${r%.*}) / 2 )); LY=$(( (${t%.*} + ${bb%.*}) / 2 ))
act b wheel "$LX" "$LY" -1200
act b wheel "$LX" "$LY" -1200
scrolled() { gt "$(f b callhome scroll)" 0 && [ "$(y0)" = "$AY" ]; }
check "Past scrolls, and Active stays where it was" waitfor 10 seen b scrolled
more_loaded() { gt "$(f b callhome hist)" "${HN:-0}"; }
check "older calls load as it scrolls (${HN:-?} held before)" waitfor 15 seen b more_loaded
check "each row has the faces of who took part" seen b faces
act b size 1600 900
# The call bob was in, alone under Joined, so its row is on screen.
press b calls.filter.joined || gui_abort "the Joined filter was not there, again"
press b "calls.row.$ID" || gui_abort "the call's row was not there"
beside() { has b '^view=11 ' && has b "^callpage id=$ID shown=$ID " && has b '^callhome .* split=1 '; }
check "on a wide window its page opens beside the list" waitfor 10 seen b beside
act b shot callhome_split
act b size 860 900
waitfor 5 seen b has b '^callhome .* split=0 ' || gui_abort "the panel did not go narrow"
press b "calls.row.$ID" || gui_abort "the call's row was not there, narrow"
alone() { has b '^view=12 ' && has b '^a11yitem callpage\.back '; }
check "on a narrow one it opens in the list's place, with Back" waitfor 10 seen b alone
press b callpage.back || gui_abort "Back was not there"
back_home() { has b '^view=11 '; }
check "Back returns to the list" waitfor 5 seen b back_home
act b size 1600 900
press b calls.filter.missed || gui_abort "the Missed filter was not there, a third time"
waitfor 10 seen b only_missed || gui_abort "the Missed filter did not apply"
MID="$(grep -m1 '^callrow ' "$DB" | grep -o 'id=[0-9]*' | cut -d= -f2)"
press b "calls.callback.$MID" || gui_abort "a missed call's Call back was not there"
called_back() { has b '^callpick ch=0 invite=0 n=1 subject="Missed [0-9]" on=[0-9]+:1$'; }
check "Call back opens New Call with the call's subject, and alice asked" waitfor 10 seen b called_back
act b shot callhome_callback
gui_end
