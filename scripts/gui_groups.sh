#!/usr/bin/env bash
# User groups in the Win32 client (REQ-307-309, ARCH-114): two clients end to end.
#
#   scripts/gui_groups.sh            # run, and leave the pair running
#   scripts/gui_groups.sh --down     # ... and take it down afterwards
#
# Alice (owner) makes a group in Admin > Groups and puts carol and bob in it with
# the people picker, then gives it to a private channel from the channel menu.
# Bob, never invited, is in the channel through the group; a message naming the
# group names him; and his channel menu will not let him leave, because the group
# keeps him in. In the members pane, alice's Remove on bob says why not, the
# group's row takes the group off (asking first), Add group puts it back, and a
# direct member's Remove takes them out. New message's To field, which shares the
# picker, still takes a channel and stops at 8 people. Its own daemon on its own
# port, wiped each run; carol and u1..u9 have no client.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PAIR="$HERE/scripts/gui_pair.sh"
fails=0

export OC_PAIR_PORT="${OC_PAIR_PORT:-9640}"
export OC_PAIR_DIR="${OC_PAIR_DIR:-/tmp/oc-groups}"
export OC_PAIR_USERS="alice:pw:owner,bob:pw:member,carol:pw:member$(for i in 1 2 3 4 5 6 7 8 9; do printf ',u%d:pw:member' "$i"; done)"
# The WSL machine's own address, as gui_calls_tcp.sh uses: Windows' forwarding of
# 127.0.0.1 into WSL does not reliably reach a port it has not seen before.
export OC_PAIR_HOST="${OC_PAIR_HOST:-$(ip -4 -o addr show eth0 | awk '{print $4}' | cut -d/ -f1)}"

say()  { printf '%s\n' "$*"; }
check() { local what="$1"; shift; if "$@"; then say "  ok   $what"; else say "  FAIL $what"; fails=$((fails + 1)); fi; }
dump() { "$PAIR" "$1" dump "groups_$1" > "/tmp/oc-groups-dump-$1.txt" 2>&1; }
line() { grep -m1 "^$2" "/tmp/oc-groups-dump-$1.txt"; }
waitfor() { local who="$1" secs="$2" t=0; shift 2
  while [ "$t" -lt $((secs * 4)) ]; do dump "$who"; if "$@"; then return 0; fi; sleep 0.25; t=$((t + 1)); done; return 1; }
# The picker's line: `pick a chips` is what is chosen, `pick a matches` what is offered.
pick() { line "$1" 'grppick ' | grep -o "$2=\"[^\"]*\"" | cut -d'"' -f2; }
field() { line "$1" "$2" | grep -o "\b$3=[^ ]*" | head -1 | cut -d= -f2; }
# Click the centre of an l,t,r,b rect (DIPs); refuses an empty one.
click_rect() { local l t r b; IFS=, read -r l t r b <<< "$2"; [ "${r%.*}" -gt "${l%.*}" ] || return 1
  "$PAIR" "$1" click $(( (l + r) / 2 )) $(( (t + b) / 2 )) >/dev/null; }
# A member row's rect, by name, from alice's dump.
memrow_of() { grep "^memrow .*name=\"$1\"" /tmp/oc-groups-dump-a.txt | head -1 | grep -o 'r=[0-9,.-]*' | cut -d= -f2; }
press() { "$PAIR" "$1" key "$2" >/dev/null; "$PAIR" "$1" keyup "$2" >/dev/null; }

say "== fixture: a fresh daemon, alice (owner) and bob"
"$PAIR" down >/dev/null 2>&1
rm -rf "$OC_PAIR_DIR"
"$PAIR" up || { say "pair did not come up"; exit 1; }

say "== alice makes @crew in Admin > Groups, and puts carol and bob in it"
"$PAIR" a menu 87 >/dev/null                          # Admin > Groups
sleep 0.5
"$PAIR" a formnext 'crew|The crew|people on call' >/dev/null
"$PAIR" a grpbtn new >/dev/null
has_crew() { line "$1" 'groups ' | grep -q '@crew:'; }
check "alice sees @crew" waitfor a 10 has_crew a
check "so does bob, who did nothing" waitfor b 10 has_crew b
"$PAIR" a grpbtn 0 >/dev/null                          # the first row's "People"
sleep 0.3
"$PAIR" a grppick car >/dev/null                       # a partial name, in lower case
chips_carol() { [ "$(pick a chips)" = "carol" ]; }
check "the picker finds carol from 'car'" waitfor a 5 chips_carol
"$PAIR" a grppick '#gen' >/dev/null                    # a channel is not a person
no_channel() { [ -z "$(pick a matches)" ] && [ "$(pick a chips)" = "carol" ]; }
check "a channel query offers nothing, and Enter adds nobody" waitfor a 5 no_channel
press a esc
"$PAIR" a grppick Bo >/dev/null
two_chips() { [ "$(pick a chips)" = "carol,bob" ]; }
check "a second person joins the first" waitfor a 5 two_chips
"$PAIR" a shot groups_picker >/dev/null
"$PAIR" a grppick - >/dev/null                         # Enter on an empty query adds them
crew_has_two() { line a 'groups ' | grep -q '@crew:2'; }
check "one Add puts both in @crew" waitfor a 10 crew_has_two
check "and the picker is empty again" bash -c '[ -z "$(grep -m1 "^grppick " /tmp/oc-groups-dump-a.txt | grep -o "chips=\"[^\"]*\"" | cut -d\" -f2)" ]'
"$PAIR" a shot groups_admin >/dev/null

say "== alice gives @crew to a private channel"
"$PAIR" a mkchan crewroom 0 >/dev/null
sleep 1
"$PAIR" a channel crewroom >/dev/null
"$PAIR" a formnext '0' >/dev/null                      # the only group it lacks: @crew
"$PAIR" a chmenu 8 >/dev/null
a_chan_has_group() { line a 'chgroups ' | grep -Eq ' n=1 '; }
check "the channel has one group" waitfor a 10 a_chan_has_group
"$PAIR" a view home >/dev/null
"$PAIR" a channel crewroom >/dev/null
"$PAIR" a members >/dev/null
sleep 1

say "== bob is in the channel through @crew"
b_sees() { "$PAIR" b channel crewroom >/dev/null; line b 'chgroups ' | grep -q 'via="crew"'; }
check "bob, never invited, is in crewroom through @crew" waitfor b 15 b_sees
"$PAIR" a shot groups_members >/dev/null
"$PAIR" b view home >/dev/null
"$PAIR" b channel crewroom >/dev/null
"$PAIR" b members >/dev/null
"$PAIR" a send "@crew standup in five" >/dev/null
b_named() { line b 'chgroups ' | grep -q 'last_names_me=1'; }
check "a message naming @crew names bob" waitfor b 10 b_named
"$PAIR" b shot groups_bob >/dev/null

say "== bob cannot leave: the group keeps him in"
"$PAIR" b chmenu 3 >/dev/null                          # Leave, were it offered
still_in() { line b 'chgroups ' | grep -q 'via="crew"'; }
refused() { line b 'error_seq' | grep -q 'through a group'; }
check "the daemon refuses, and says why" waitfor b 10 refused
check "and bob is still in" still_in

say "== in alice's members pane: bob's Remove says why not"
"$PAIR" a view home >/dev/null
"$PAIR" a channel crewroom >/dev/null
dump a
[ "$(field a 'authed=' members)" = "1" ] || "$PAIR" a members >/dev/null   # open the pane if shut
# The pane yields to the conversation in a narrow window, and the pair's windows
# are narrow; widened, it is drawn, and its buttons are there to press.
"$PAIR" a size 2400 1400 >/dev/null
sleep 0.5
bob_row() { [ -n "$(memrow_of bob)" ]; }
waitfor a 10 bob_row >/dev/null
r="$(memrow_of bob)"; IFS=, read -r l t rr bb <<< "$r"
"$PAIR" a move $(( (l + rr) / 2 )) $(( (t + bb) / 2 )) >/dev/null
bob_rm() { [ "$(field a 'memgrp ' memrm | cut -d: -f1)" != "0" ]; }
check "hovered, bob's row offers Remove" waitfor a 5 bob_rm
"$PAIR" a shot groups_pane_via >/dev/null
click_rect a "$(field a 'memgrp ' memrm | cut -d: -f2)"
told_why() { grep -q '^toast.*bob is in this channel through @crew' /tmp/oc-groups-dump-a.txt; }
check "clicked, it says bob is in through @crew" waitfor a 5 told_why
b_in() { "$PAIR" b channel crewroom >/dev/null; line b 'chgroups ' | grep -q 'via="crew"'; }
check "and bob is still in" waitfor b 5 b_in

say "== carol, also added directly, stays when the group comes off"
"$PAIR" a formnext 'carol' >/dev/null
"$PAIR" a chmenu 6 >/dev/null                          # Add someone
sleep 1
grp_row() { [ -n "$(field a 'memgrp ' rows)" ]; }
waitfor a 5 grp_row >/dev/null
click_rect a "$(field a 'memgrp ' rows | cut -d';' -f1 | cut -d: -f2)"
asks() { [ "$(field a 'modal=' modal)" = "confirm" ]; }
check "the group row's Remove asks first" waitfor a 5 asks
"$PAIR" a shot groups_pane_confirm >/dev/null
press a enter
no_group() { line a 'chgroups ' | grep -Eq ' n=0 '; }
check "confirmed, the channel has no group" waitfor a 10 no_group
b_out() { "$PAIR" b channel crewroom >/dev/null; line b 'chgroups ' | grep -q 'joined=0'; }
check "bob, in only through it, is out" waitfor b 10 b_out
carol_stays() { [ -n "$(memrow_of carol)" ] && [ -z "$(memrow_of bob)" ]; }
check "carol, in directly too, stays" waitfor a 10 carol_stays

say "== the pane's Add group puts it back; a direct member's Remove takes them out"
"$PAIR" a formnext '0' >/dev/null
click_rect a "$(field a 'memgrp ' add)"
has_group() { line a 'chgroups ' | grep -Eq ' n=1 '; }
check "Add group gives the channel @crew again" waitfor a 10 has_group
check "and bob is back in" waitfor b 10 b_in
"$PAIR" a formnext 'u1' >/dev/null
"$PAIR" a chmenu 6 >/dev/null                          # u1, directly and only so
u1_in() { [ -n "$(memrow_of u1)" ]; }
waitfor a 10 u1_in >/dev/null
r="$(memrow_of u1)"; IFS=, read -r l t rr bb <<< "$r"
"$PAIR" a move $(( (l + rr) / 2 )) $(( (t + bb) / 2 )) >/dev/null
u1_rm() { [ "$(field a 'memgrp ' memrm | cut -d: -f1)" != "0" ]; }
waitfor a 5 u1_rm >/dev/null
click_rect a "$(field a 'memgrp ' memrm | cut -d: -f2)"
u1_out() { [ -z "$(memrow_of u1)" ] && [ -n "$(memrow_of bob)" ]; }
check "u1's Remove takes u1 out, and nobody else" waitfor a 10 u1_out

say "== New message still takes a channel, and stops at 8 people"
"$PAIR" a view newmsg >/dev/null
sleep 0.5
"$PAIR" a typekeys '#crewr' >/dev/null
press a enter
nm_chan() { [ "$(pick a chips)" = "#crewroom" ] && [ "$(field a 'grppick ' host)" = "0" ]; }
check "the To field takes #crewroom" waitfor a 5 nm_chan
for i in 1 2 3 4 5 6 7 8 9; do "$PAIR" a typekeys "u$i" >/dev/null; press a enter; done
eight() { [ "$(pick a chips)" = "u1,u2,u3,u4,u5,u6,u7,u8" ]; }
check "people replace the channel, and the ninth is refused" waitfor a 5 eight
capped() { grep -q '^toast.*at most 8 people' /tmp/oc-groups-dump-a.txt; }
check "and it says so" waitfor a 5 capped
# A body, so there is a draft to keep: an unaddressed draft with no text is no
# draft at all (the model drops it), recipients or not.
press a tab
"$PAIR" a typekeys 'for the eight' >/dev/null

say "== the group's picker does not take New message's people"
"$PAIR" a menu 87 >/dev/null
sleep 0.3
"$PAIR" a grpbtn 0 >/dev/null
own() { [ "$(field a 'grppick ' host)" = "1" ] && [ -z "$(pick a chips)" ]; }
check "opening @crew's people starts an empty picker of its own" waitfor a 5 own
"$PAIR" a grppick ali >/dev/null
me_offered() { [ "$(pick a chips)" = "alice" ]; }
check "alice can choose herself" waitfor a 5 me_offered
"$PAIR" a grppick - >/dev/null
crew_three() { line a 'groups ' | grep -q '@crew:3'; }
check "and is in @crew" waitfor a 10 crew_three
"$PAIR" a view newmsg >/dev/null
sleep 0.5
still_eight() { [ "$(pick a chips)" = "u1,u2,u3,u4,u5,u6,u7,u8" ] && [ "$(field a 'grppick ' host)" = "0" ] &&
               [ "$(field a 'newmsg focus' body)" = "13" ]; }
check "New message's 8 people, and its text, are still there" waitfor a 5 still_eight
press a esc

say "== out of the group, out of the channel"
"$PAIR" a menu 87 >/dev/null
sleep 0.3
"$PAIR" a grpbtn 0 >/dev/null
sleep 0.3
for _ in 1 2 3; do "$PAIR" a grpbtn 0 >/dev/null; sleep 0.5; done   # every member row's "Remove"
b_gone() { "$PAIR" b channel crewroom >/dev/null; line b 'chgroups ' | grep -q 'via="" .*joined=0'; }
check "bob is out of crewroom, and told" waitfor b 10 b_gone

[ "${1:-}" = "--down" ] && "$PAIR" down >/dev/null 2>&1
if [ "$fails" -eq 0 ]; then say "gui_groups: all passed"; else say "gui_groups: $fails FAILED"; fi
exit "$fails"
