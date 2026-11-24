#!/usr/bin/env bash
# New Call, the one dialog a call starts from (CALLS.md §2), two clients end to
# end.
#
#   tools/gui/features/newcall.sh            # run; the pair and its daemon stop at the end
#   tools/gui/features/newcall.sh --keep     # ... and leave them running
#
# alice (owner), bob, carol and dave; a call holds three (OC_TEST_CALL_MAX). It
# asserts that
#
#   - every way in opens the same dialog: the Calls panel's New, the sidebar's
#     +, a conversation's call button (its subject and people filled in, nothing
#     focused but Start), and Ctrl+Shift+L; Esc closes it;
#   - starting, it previews the camera and the microphone;
#   - its search adds a person by part of a name, a group (@crew: carol) and a
#     channel (#general: everyone in it) -- and past the call's room it refuses,
#     saying how many the call holds;
#   - a chip's x takes its person off; a subject left blank becomes "Call
#     with" and the people asked;
#   - from a conversation, Enter starts the call it filled in;
#   - Invite in a call opens it to invite, and invites who is picked.
#
# platforms: win32 web
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
. "$HERE/tools/gui/harness/lib.sh"
gui_begin newcall 9810 /tmp/oc-newcall "$@"

export OC_TEST_HOST="${OC_TEST_HOST:-$(ip -4 -o addr show eth0 | awk '{print $4}' | cut -d/ -f1)}"
export OPENCHIME_DEV_AUDIO=synthetic
export OPENCHIME_DEV_CAPTURE=synthetic
export OC_TEST_USERS="alice:pw:owner,bob:pw:member,carol:pw:member,dave:pw:member" OC_TEST_CALL_MAX=3
CH=1                                    # #general: everyone is in it

f() { field "$(dfile "$1")" "^$2" "$3"; }
has() { grep -E "$2" "$(dfile "$1")" >/dev/null; }
# A person's id, as the daemon said when it made them.
uid_of() { grep -m1 "bootstrap user '$1' -> id" "$OC_TEST_DIR/daemon.log" | grep -o 'id [0-9]*' | cut -d' ' -f2; }
acked() { [ "$(ack_of "$@")" = ok ]; }
num() { [ -n "$1" ] && [ "$1" -eq "$1" ] 2>/dev/null; }
gt()  { num "$1" && [ "$1" -gt "$2" ]; }
modal_is() { [ "$(f a 'modal=' modal)" = "$1" ]; }
closed() { modal_is none && ! has a '^callpick '; }
tap() { act "$1" key "$2"; act "$1" keyup "$2"; }

step "a fresh daemon, alice and bob; four people in all"
gui_pair_up
BOB="$(uid_of bob)"; CAROL="$(uid_of carol)"; DAVE="$(uid_of dave)"
[ -n "$BOB" ] && [ -n "$CAROL" ] && [ -n "$DAVE" ] || gui_abort "alice's dump names not everyone (bob=$BOB carol=$CAROL dave=$DAVE)"

step "@crew, with carol in it"
act a menu 87
waitfor 5 seen a has a '^a11yitem admin\.tab\.groups ' || gui_abort "Admin > Groups was not drawn"
act a formnext 'crew|The crew|people on call'
waitfor 5 acked a grpbtn new || gui_abort "Admin > Groups has no New group to press"
crew() { has a '^groups .*@crew:'; }
waitfor 10 seen a crew || gui_abort "@crew was not made"
waitfor 5 acked a grpbtn 0 || gui_abort "@crew's row has no People to press"
act a grppick car
act a grppick -
crew1() { has a '^groups .*@crew:1( |$)'; }
waitfor 10 seen a crew1 || gui_abort "carol was not put in @crew"
tap a esc
act a view home

step "every way in opens the one dialog"
up_from() { has a "^callpick ch=$1 invite=0 "; }
act a call home
press a calls.new || gui_abort "the Calls panel's New was not there"
check "the Calls panel's New" waitfor 10 seen a up_from 0
tap a esc
check "...and Esc closes it" waitfor 5 seen a closed
press a sidebar.calls.new || gui_abort "the sidebar's + was not there"
check "the sidebar's +" waitfor 10 seen a up_from 0
previewing() { has a '^newcall .* preview=1 '; }
check "starting, it previews the camera" waitfor 10 seen a previewing
miclevel() { gt "$(f a newcall level)" 500; }
check "...and the microphone" waitfor 10 seen a miclevel
act a shot newcall_empty
tap a esc
waitfor 5 seen a closed || gui_abort "the dialog did not close"
act a view home
act a channel $CH
press a header.call || gui_abort "#general's call button was not there"
filled() { has a "^callpick ch=$CH invite=0 n=[0-9]+ subject=\"general\" on=" && [ "$(f a newcall ticked)" = 2 ] &&
           has a '^newcall .* focus_subject=0 focus_q=0'; }
check "a conversation's call button: its subject and two of its people asked, Start next" waitfor 10 seen a filled
act a shot newcall_from_channel
tap a esc
waitfor 5 seen a closed || gui_abort "the dialog did not close"
act a key ctrl+shift+l
act a keyup ctrl+shift+l
check "Ctrl+Shift+L, from #general, the same" waitfor 10 seen a filled
tap a esc
waitfor 5 seen a closed || gui_abort "the dialog did not close"

step "its search: a person, a group, a channel, and the room"
act a call home
press a calls.new || gui_abort "the Calls panel's New was not there, again"
waitfor 10 seen a up_from 0 || gui_abort "New call did not open"
act a call pick-search bo
found_bob() { has a "^newcall.res kind=user id=$BOB$"; }
check "\"bo\" finds bob" waitfor 5 seen a found_bob
press a "call.pick.$BOB" || gui_abort "bob was not there to pick"
asked1() { [ "$(f a newcall ticked)" = 1 ] && has a "^callpick .*on=.*$BOB:1"; }
check "...and asks him" waitfor 5 seen a asked1
act a call pick-search crew
GROUP="$(seen a true; grep -m1 '^newcall.res kind=group ' "$DA" | grep -o 'id=[0-9]*' | cut -d= -f2)"
[ -n "$GROUP" ] || gui_abort "\"crew\" found no group"
press a "call.pick.group.$GROUP" || gui_abort "@crew was not there to pick"
asked2() { [ "$(f a newcall ticked)" = 2 ] && has a "^callpick .*on=.*$CAROL:1"; }
check "@crew asks carol" waitfor 5 seen a asked2
act a shot newcall_chips
act a call pick-search gen
press a "call.pick.conv.$CH" || gui_abort "#general was not there to pick"
refused() { [ "$(f a newcall ticked)" = 2 ] && has a '^newcall .* msg="The call holds 3 people, you among them."'; }
check "#general's people would be more than the call holds: refused, and said so" waitfor 10 seen a refused
act a shot newcall_refused

step "a subject left blank"
# carol's chip taken off, so the call has room for one more later.
press a "call.pick.chip.$CAROL" || gui_abort "carol's chip was not there"
unasked() { [ "$(f a newcall ticked)" = 1 ]; }
waitfor 5 seen a unasked || gui_abort "carol's chip did not take her off"
act a call subject ""
act a call pick-go
blank_named() { [ "$(f a 'call ' in)" = 1 ] && has a '^callobj view=[1-9][0-9]* subject="Call with bob" '; }
check "it becomes \"Call with bob\"" waitfor 10 seen a blank_named

step "Invite, in the call"
press a call.invite || gui_abort "the call's Invite was not there"
inviting() { has a '^callpick ch=[0-9]+ invite=1 '; }
check "Invite opens it to invite" waitfor 10 seen a inviting
act a shot newcall_invite
act a call pick-search dav
press a "call.pick.$DAVE" || gui_abort "dave was not there to pick"
act a call pick-go
dave_asked() { gui_snap a && [ "$(f a 'call ' invited)" = 2 ] && closed; }
check "...and invites dave" waitfor 10 dave_asked
act a call leave
out() { [ "$(f a 'call ' in)" = 0 ]; }
waitfor 10 seen a out || gui_abort "alice did not leave"

step "Enter, from a conversation"
act a view home
act a channel $CH
press a header.call || gui_abort "#general's call button was not there, again"
waitfor 10 seen a filled || gui_abort "the call button did not open it filled in"
tap a enter
started() { [ "$(f a 'call ' in)" = 1 ] && has a '^callobj view=[1-9][0-9]* subject="general" '; }
check "Enter starts the call it filled in" waitfor 10 seen a started
act a call leave
gui_end
