#!/usr/bin/env bash
# User groups in the Win32 client (REQ-307-309, ARCH-114): two clients end to end.
#
#   tools/gui/features/groups.sh            # run; the pair and its daemon stop at the end
#   tools/gui/features/groups.sh --keep     # ... and leave them running
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
#
# platforms: win32
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
. "$HERE/tools/gui/harness/lib.sh"
gui_begin groups 9640 /tmp/oc-groups "$@"
export OC_TEST_USERS="alice:pw:owner,bob:pw:member,carol:pw:member$(for i in 1 2 3 4 5 6 7 8 9; do printf ',u%d:pw:member' "$i"; done)"
# The WSL machine's own address, as tools/gui/features/calls_tcp.sh uses: Windows' forwarding of
# 127.0.0.1 into WSL does not reliably reach a port it has not seen before.
export OC_TEST_HOST="${OC_TEST_HOST:-$(ip -4 -o addr show eth0 | awk '{print $4}' | cut -d/ -f1)}"

# acked <who> <verb...>: the verb, answered "ok" -- for a button that is only
# there to press once the client has drawn it.
acked() { [ "$(ack_of "$@")" = ok ]; }
has() { grep -E "$2" "$(dfile "$1")" >/dev/null; }    # has <who> <ERE>
line() { grep -m1 "^$2" "$(dfile "$1")"; }
# The picker's line: `pick a chips` is what is chosen, `pick a matches` what is offered.
pick() { fieldq "$(dfile "$1")" '^grppick ' "$2"; }
f() { field "$(dfile "$1")" "^$2" "$3"; }             # f <who> <line-prefix> <key>
# A member row's rect, by name, from alice's dump; its Remove, when shown; and
# whether that Remove is dimmed because a group keeps them in.
memrow_of() { grep "^memrow .*name=\"$1\"" "$DA" | head -1 | grep -o ' r=[0-9,.-]*' | cut -d= -f2; }
rm_of()  { local r; r="$(grep "^memrow .*name=\"$1\"" "$DA" | head -1 | grep -o 'rm=[0-9,.-]*' | cut -d= -f2)"
           [ "$r" = "0,0,0,0" ] || printf '%s' "$r"; }
via_of() { grep "^memrow .*name=\"$1\"" "$DA" | head -1 | grep -o 'via=[0-9]*' | cut -d= -f2; }
tap() { act "$1" key "$2"; act "$1" keyup "$2"; }   # tap <who> <key>: down and up
# Admin > Groups, drawn: its tab is in what the client last painted.
admin_groups() {
  act a menu 87
  waitfor 5 seen a has a '^a11yitem admin\.tab\.groups ' || gui_abort "Admin > Groups was not drawn"
}
crew_n() { has a "^groups .*@crew:$1( |\$)"; }

step "a fresh daemon, alice (owner) and bob"
gui_pair_up

step "alice makes @crew in Admin > Groups, and puts carol and bob in it"
admin_groups
act a formnext 'crew|The crew|people on call'
waitfor 5 acked a grpbtn new || gui_abort "Admin > Groups has no New group to press"
has_crew() { has "$1" '^groups .*@crew:'; }
check "alice sees @crew" waitfor 10 seen a has_crew a
check "so does bob, who did nothing" waitfor 10 seen b has_crew b
waitfor 5 acked a grpbtn 0 || gui_abort "@crew's row has no People to press"   # the first row's "People"
grp_picker() { [ "$(f a 'grppick ' host)" = 1 ]; }
waitfor 5 seen a grp_picker || gui_abort "@crew's people did not open, with their picker"
act a grppick car                                      # a partial name, in lower case
chips_carol() { [ "$(pick a chips)" = "carol" ]; }
check "the picker finds carol from 'car'" waitfor 5 seen a chips_carol
act a grppick '#gen'                                   # a channel is not a person
no_channel() { [ -z "$(pick a matches)" ] && [ "$(pick a chips)" = "carol" ]; }
check "a channel query offers nothing, and Enter adds nobody" waitfor 5 seen a no_channel
tap a esc
act a grppick Bo
two_chips() { [ "$(pick a chips)" = "carol,bob" ]; }
check "a second person joins the first" waitfor 5 seen a two_chips
act a shot groups_picker
act a grppick -                                        # Enter on an empty query adds them
check "one Add puts both in @crew" waitfor 10 seen a crew_n 2
empty_chips() { has a '^grppick ' && [ -z "$(pick a chips)" ]; }
check "and the picker is empty again" waitfor 5 seen a empty_chips
act a shot groups_admin

step "alice adds carol to a private channel with the people picker, then gives it @crew"
act a mkchan crewroom 0
made() { has a '^  ch [0-9]+ [^"]*"crewroom"'; }
waitfor 10 seen a made || gui_abort "the private channel crewroom did not come back from the daemon"
act a channel crewroom
# The members pane yields to the conversation in a narrow window, and the pair's
# windows are narrow; widened, the pane is drawn and its buttons can be pressed.
act a size 2400 1400
act a chmenu 6                                         # Add someone: the pane's picker
adding() { [ "$(f a 'grppick ' memmode)" = 1 ] && [ "$(f a 'grppick ' host)" = 2 ]; }
check "Add someone opens the pane's people picker" waitfor 5 seen a adding
act a grppick CAR                                      # a partial name, upper case
check "it finds carol from 'CAR'" waitfor 5 seen a chips_carol
act a shot channel_picker
act a grppick -                                        # Enter on an empty query adds
carol_direct() { [ -n "$(memrow_of carol)" ] && [ "$(f a 'grppick ' memmode)" = 0 ]; }
check "carol is in the channel, and the picker has closed" waitfor 10 seen a carol_direct
act a formnext '0'                                     # the only group it lacks: @crew
act a chmenu 8
a_chan_has_group() { has a '^chgroups .* n=1 '; }
check "the channel has one group" waitfor 10 seen a a_chan_has_group
act a view home
act a channel crewroom
act a members
crewroom_open() { [ "$(f a 'authed=' sel)" = "$(grep -m1 -E '^  ch [0-9]+ [^"]*"crewroom"' "$DA" | awk '{print $2}')" ] && a_chan_has_group; }
waitfor 5 seen a crewroom_open || gui_abort "alice's crewroom did not open again"

step "bob is in the channel through @crew"
b_in() { "$PAIR" b channel crewroom >/dev/null && gui_snap b && has b '^chgroups .*via="crew"'; }
check "bob, never invited, is in crewroom through @crew" waitfor 15 b_in
act a shot groups_members
act b view home
act b channel crewroom
act b members
act a send "@crew standup in five"
b_named() { has b '^chgroups .*last_names_me=1'; }
check "a message naming @crew names bob" waitfor 10 seen b b_named
act b shot groups_bob

step "bob cannot leave: the group keeps him in"
act b chmenu 3                                         # Leave, were it offered
still_in() { has b '^chgroups .*via="crew"'; }
refused() { has b '^error_seq.*through a group'; }
check "the daemon refuses, and says why" waitfor 10 seen b refused
check "and bob is still in" still_in

step "in alice's members pane: bob's Remove says why not"
act a view home
act a channel crewroom
gui_snap a || gui_abort "alice's client did not dump"
[ "$(f a 'authed=' members)" = "1" ] || act a members   # open the pane if shut
bob_row() { [ -n "$(memrow_of bob)" ]; }
waitfor 10 seen a bob_row || gui_abort "alice's members pane does not list bob"
r="$(memrow_of bob)"; IFS=, read -r l t rr bb <<< "$r"
act a move $(( (l + rr) / 2 )) $(( (t + bb) / 2 ))
bob_rm() { [ "$(f a 'memgrp ' memrm | cut -d: -f1)" != "0" ]; }
check "hovered, bob's row offers Remove" waitfor 5 seen a bob_rm
act a shot groups_pane_via
click_rect a "$(f a 'memgrp ' memrm | cut -d: -f2)"
told_why() { has a '^toast.*bob is in this channel through @crew'; }
check "clicked, it says bob is in through @crew" waitfor 5 seen a told_why
check "and bob is still in" waitfor 5 b_in

step "the picker leaves out who is in already; carol, in directly too, stays when the group comes off"
# Esc takes the newest notice first, whatever has the keys (Remove's "bob is in
# this channel through @crew" is still up): gone, before the picker's Escapes.
no_toasts() { gui_snap a && ! grep -E '^toast\[[0-9]+\] ' "$DA" >/dev/null; }
for _ in 1 2 3 4; do no_toasts && break; tap a esc; done
no_toasts || gui_abort "the notices did not go: each would take an Escape meant for the picker"
act a chmenu 6
act a grppick bo                                       # bob, in through @crew
no_bob() { [ "$(f a 'grppick ' memmode)" = 1 ] && [ -z "$(pick a matches)" ] && [ -z "$(pick a chips)" ]; }
check "bob, already in, is not offered" waitfor 5 seen a no_bob
tap a esc                                            # clears the query
tap a esc                                            # and closes the picker
closed() { [ "$(f a 'grppick ' memmode)" = 0 ]; }
check "Escape closes the picker" waitfor 5 seen a closed
grp_row() { [ -n "$(f a 'memgrp ' rows)" ]; }
waitfor 5 seen a grp_row || gui_abort "alice's members pane shows no group row"
click_rect a "$(f a 'memgrp ' rows | cut -d';' -f1 | cut -d: -f2)"
asks() { [ "$(f a 'modal=' modal)" = "confirm" ]; }
check "the group row's Remove asks first" waitfor 5 seen a asks
act a shot groups_pane_confirm
tap a enter
no_group() { has a '^chgroups .* n=0 '; }
check "confirmed, the channel has no group" waitfor 10 seen a no_group
b_out() { "$PAIR" b channel crewroom >/dev/null && gui_snap b && has b '^chgroups .*joined=0'; }
check "bob, in only through it, is out" waitfor 10 b_out
carol_stays() { [ -n "$(memrow_of carol)" ] && [ -z "$(memrow_of bob)" ]; }
check "carol, in directly too, stays" waitfor 10 seen a carol_stays

step "the pane's Add group puts it back; a direct member's Remove takes them out"
act a formnext '0'
click_rect a "$(f a 'memgrp ' plus)"                  # the GROUPS subheading's +
check "Add group gives the channel @crew again" waitfor 10 seen a a_chan_has_group
check "and bob is back in" waitfor 10 b_in
gui_snap a || gui_abort "alice's client did not dump"
click_rect a "$(f a 'memppl ' add)"                   # the header's Add people
act a grppick u1
act a grppick u2
act a grppick -                                        # both in one Add
u1_in() { [ -n "$(memrow_of u1)" ] && [ -n "$(memrow_of u2)" ]; }
check "the pane's Add people adds two at once" waitfor 10 seen a u1_in

step "Remove someone: the roster, where each person's Remove is on their row"
act a chmenu 7                                         # Remove someone
roster() { [ "$(f a 'grppick ' memmode)" = 0 ] && [ -n "$(memrow_of u1)" ] && [ -z "$(f a 'memppl ' pickadd | tr -d '0,')" ]; }
check "it opens the roster, with no mode and no picker" waitfor 5 seen a roster
r="$(memrow_of u1)"; IFS=, read -r l t rr bb <<< "$r"
act a move $(( (l + rr) / 2 )) $(( (t + bb) / 2 ))
u1_rm() { [ -n "$(rm_of u1)" ] && [ "$(via_of u1)" = 0 ] && [ -z "$(rm_of u2)" ]; }
check "hovered, u1's row alone offers Remove" waitfor 5 seen a u1_rm
click_rect a "$(rm_of u1)"
u1_out() { [ -z "$(memrow_of u1)" ] && [ -n "$(memrow_of u2)" ] && [ -n "$(memrow_of bob)" ]; }
check "u1's Remove takes u1 out, and nobody else" waitfor 10 seen a u1_out

step "no rows of buttons; the two icons are named on hover"
flat() { [ -n "$(f a 'memgrp ' plus | tr -d '0,')" ] && [ -n "$(f a 'memppl ' add | tr -d '0,')" ]; }
check "the header has Add people and GROUPS has its +" waitfor 5 seen a flat
act a shot channel_pane_flat
hover_on() { local l t rr bb; IFS=, read -r l t rr bb <<< "$1"; act a move $(( (l + rr) / 2 )) $(( (t + bb) / 2 )); }
hover_on "$(f a 'memppl ' add)"
tip_ppl() { [ "$(f a 'memppl ' tip)" = 1 ]; }
check "resting on the header's icon shows \"Add people\"" waitfor 5 seen a tip_ppl
act a shot channel_tip_addpeople
hover_on "$(f a 'memgrp ' plus)"
tip_grp() { [ "$(f a 'memppl ' tip)" = 2 ]; }
check "resting on the + shows \"Add a group\"" waitfor 5 seen a tip_grp
act a move 5 500

step "a click in the composer takes the keys back from the picker"
click_rect a "$(f a 'memppl ' add)"                   # the picker, with the keys
has_keys() { [ "$(f a 'grppick ' chanfocus)" = 1 ]; }
waitfor 5 seen a has_keys || gui_abort "the pane's picker did not take the keys"
click_rect a "$(line a 'ed len=' | grep -o 'box=[0-9,.-]*' | cut -d= -f2)"
act a typekeys 'zz'
composer_has() { [ "$(f a 'ed len=' len)" = 2 ] && [ "$(f a 'grppick ' chanfocus)" = 0 ]; }
check "what is typed next goes to the composer, not the picker" waitfor 5 seen a composer_has
act a type ''
click_rect a "$(f a 'memppl ' pickcancel)"            # Cancel

step "New message still takes a channel, and stops at 8 people"
act a view newmsg
nm_up() { has a '^newmsg focus='; }
waitfor 5 seen a nm_up || gui_abort "New message did not open"
act a typekeys '#crewr'
tap a enter
nm_chan() { [ "$(pick a chips)" = "#crewroom" ] && [ "$(f a 'grppick ' host)" = "0" ]; }
check "the To field takes #crewroom" waitfor 5 seen a nm_chan
for i in 1 2 3 4 5 6 7 8 9; do act a typekeys "u$i"; tap a enter; done
eight() { [ "$(pick a chips)" = "u1,u2,u3,u4,u5,u6,u7,u8" ]; }
check "people replace the channel, and the ninth is refused" waitfor 5 seen a eight
capped() { has a '^hint.*at most 8 people'; }   # said at the composer
check "and it says so" waitfor 5 seen a capped
# A body, so there is a draft to keep: an unaddressed draft with no text is no
# draft at all (the model drops it), recipients or not.
tap a tab
act a typekeys 'for the eight'

step "the group's picker does not take New message's people"
admin_groups
waitfor 5 acked a grpbtn 0 || gui_abort "@crew's row has no People to press"
own() { [ "$(f a 'grppick ' host)" = "1" ] && [ -z "$(pick a chips)" ]; }
check "opening @crew's people starts an empty picker of its own" waitfor 5 seen a own
act a grppick ali
me_offered() { [ "$(pick a chips)" = "alice" ]; }
check "alice can choose herself" waitfor 5 seen a me_offered
act a grppick -
check "and is in @crew" waitfor 10 seen a crew_n 3
act a view newmsg
still_eight() { [ "$(pick a chips)" = "u1,u2,u3,u4,u5,u6,u7,u8" ] && [ "$(f a 'grppick ' host)" = "0" ] &&
               [ "$(f a 'newmsg focus' body)" = "13" ]; }
check "New message's 8 people, and its text, are still there" waitfor 5 seen a still_eight
tap a esc

step "out of the group, out of the channel"
admin_groups
waitfor 5 acked a grpbtn 0 || gui_abort "@crew's row has no People to press"
waitfor 5 seen a grp_picker || gui_abort "@crew's people did not open"
# Every member row's "Remove", each once the one before has gone.
for n in 2 1 0; do
  waitfor 5 acked a grpbtn 0 || gui_abort "@crew's people show no member row's Remove"
  waitfor 10 seen a crew_n $n || gui_abort "a Remove in @crew's people did not take anyone out (@crew should have $n)"
done
b_gone() { "$PAIR" b channel crewroom >/dev/null && gui_snap b && has b '^chgroups .*via="" .*joined=0'; }
check "bob is out of crewroom, and told" waitfor 10 b_gone
gui_end
