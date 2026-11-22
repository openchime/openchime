#!/usr/bin/env bash
# Calls in the Win32 client, two clients end to end (REQ-150, REQ-301-305,
# ARCH-113, docs/CALLS.md).
#
#   tools/gui/features/calls.sh            # run; the pair and its daemon stop at the end
#   tools/gui/features/calls.sh --keep     # ... and leave them running
#
# alice and bob, each a real client with the synthetic audio devices -- alice's
# microphone a 440 Hz tone, bob's 660 Hz -- over tools/gui/harness/win32/pair.sh, against a real
# daemon and its relay. It asserts that
#
#   - a call started in #general opens the picker -- its subject "general",
#     bob ticked -- and invites bob, who sees it in Calls home with its own Join
#     and gets an invitation notification; his #general shows the call's banner
#     with Join and Decline and the call's message, the header's phone says
#     "Join the call", and he joins from the banner; the call view names its
#     subject;
#   - Mute's tooltip says "You are currently muted" or "...not muted";
#   - the call view shows the call's own chat beside it, a message sent there
#     reaches alice's copy of that chat, and it folds away and back; Settings
#     holds the devices;
#   - alice records (CALLS.md §8): both see the Recording mark, and the call
#     says it is recorded; bob leaves and comes back: he is shown "This call is
#     being recorded." and sends nothing -- alice hears nothing of him -- until
#     he presses Join; alice stops it;
#   - End for everyone asks first: Cancel keeps the call, Confirm ends it; with
#     no call left, the Calls section is gone;
#   - a call nobody takes is in bob's Calls home as missed, and its page opens;
#   - bob joins: each receives the other's audio, every packet decrypting once
#     the first second's keys are in, with both keyed for the current epoch;
#   - noise suppression takes a steady hum out -- which is what a test tone is
#     to it, so the rest runs with it off;
#   - bob mutes: alice stops receiving his audio and sees that he is muted;
#     holding the talk key lets him through again;
#   - a volume set for bob is the one the engine plays him at;
#   - alice shares the synthetic screen: bob's call view shows it, at its size,
#     its frame numbers rising; her bar is up and her view says she is sharing;
#     bob goes full screen and to actual size and back; bob takes over, which
#     stops alice's; bob stops, and the picture goes;
#   - the keys: Ctrl+Shift+M mutes and unmutes, Ctrl+Shift+Space held talks;
#   - away from the call, the strip at the foot of the sidebar mutes and brings
#     you back, and the header's call button opens the call;
#   - bob may not end the call; alice ends it for everyone and it is gone from
#     both Calls sections;
#   - a call nobody takes leaves "Missed call" in bob's history;
#   - an invitation's toast button joins;
#   - with more members than the call holds, the starter picks, the most recently
#     active ticked.
#
# Against a daemon that records calls (make CALLREC=1).
#
# Its own daemon on its own port, wiped each run. The clients reach it at the
# WSL machine's address, not 127.0.0.1: WSL forwards only TCP there, and a
# call's audio is UDP.
#
# platforms: win32 web
# needs: callrec
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
. "$HERE/tools/gui/harness/lib.sh"
gui_begin calls 9620 /tmp/oc-calls "$@"
# The shared screen's size: the synthetic 2560x1600 screen fitted into the
# platform's limit (oc_plat_share_limits): 1920x1080 on a desktop, 960x540 in
# a browser.
SHARE_WH=1728x1080; [ "$PLAT" = web ] && SHARE_WH=864x540

export OC_TEST_HOST="${OC_TEST_HOST:-$(ip -4 -o addr show eth0 | awk '{print $4}' | cut -d/ -f1)}"
export OPENCHIME_DEV_AUDIO=synthetic
export OPENCHIME_DEV_CAPTURE=synthetic   # the synthetic screen, for sharing
export OC_TEST_TONE_A=440 OC_TEST_TONE_B=660
CH=1                                   # #general: both are members

# The value of key `k` on the first line starting with `prefix` (act, dfile,
# seen, click_rect: lib.sh).
f() { field "$(dfile "$1")" "^$2" "$3"; }   # f <who> <line-prefix> <key>
has() { grep -E "$2" "$(dfile "$1")" >/dev/null; }   # has <who> <ERE>
num() { [ -n "$1" ] && [ "$1" -eq "$1" ] 2>/dev/null; }
ge()  { num "$1" && [ "$1" -ge "$2" ]; }
gt()  { num "$1" && [ "$1" -gt "$2" ]; }
lt()  { num "$1" && [ "$1" -lt "$2" ]; }
# Click just inside the top-left of such a rect: the strip's own body, above its buttons.
click_corner() {  # click_corner <who> <key>
  local r; r="$(f "$1" 'call ' "$2")"
  local l t rr b; IFS=, read -r l t rr b <<< "$r"
  [ -n "${b:-}" ] && [ "${rr%.*}" -gt 0 ] 2>/dev/null || return 1
  act "$1" click $(( l + 40 )) $(( t + 10 ))
}
peer() { f "$1" 'call\.peer' "$2"; }   # peer <who> <key>: that client's one other participant's value

step "a fresh daemon, alice (440 Hz) and bob (660 Hz) at $OC_TEST_HOST:$OC_TEST_PORT"
gui_pair_up
offered() { gui_snap a && gui_snap b && ge "$(f a 'call ' cap)" 2 && ge "$(f b 'call ' cap)" 2; }
check "the daemon offers calls to both" waitfor 5 offered

step "alice starts a call in #general, which asks bob"
act a call start $CH
# The picker: what the call is about, and who #general's call asks, ticked.
pick_up() { has a '^callpick ch='"$CH"' invite=0 n=1 subject="general" on=[0-9]*:1$'; }
check "alice's call button opens the picker: subject \"general\", bob ticked" waitfor 10 seen a pick_up
act a shot calls_picker
act a call pick-go
a_alone()     { [ "$(f a 'call ' in)" = 1 ] && [ "$(f a 'call ' parts)" = 1 ]; }
b_invited()   { has b "^calls\[0\] ch=$CH .*me_invited=1"; }
b_notified()  { ge "$(f b tray_live toasts_raised)" 1; }
check "alice is in it, alone, as its starter" waitfor 10 seen a a_alone
check "bob's Calls section lists it, with him invited" waitfor 10 seen b b_invited
check "bob was notified" waitfor 5 seen b b_notified
gui_snap a || gui_abort "alice's client did not dump"
ID="$(f a 'call ' id)"
[ -n "$ID" ] && [ "$ID" != 0 ] || gui_abort "alice's dump names no call id"
act b shot calls_invited

step "bob joins"
# A test tone is a steady hum, which noise suppression is there to take out; the
# audio checks below turn it off, and one check turns it on to see it do that.
act a call ns off
act b call ns off
# From the conversation: its banner names who is in the call, with Join, and
# Decline since bob was asked; the Calls row has its own Join.
act b view home
act b channel $CH
banner() { [ "$(f b 'callui ' banner)" = 1 ] && has b '^a11yitem call\.banner\.join ' && has b '^a11yitem call\.banner\.decline '; }
check "bob's #general shows the call's banner, with Join and Decline" waitfor 10 seen b banner
msg_join() { has b "^a11yitem call\.msg\.$ID "; }
check "...and the call's message, with its Join" msg_join
hdrtip() { has b '^callui .*hdrtip="Join the call"'; }
check "the header's phone says \"Join the call\"" hdrtip
act b shot calls_banner
act b call home
row_join() { has b "^a11yitem calls\.join\.$ID "; }
check "bob's Calls home lists it, with its own Join" waitfor 10 seen b row_join
act b shot calls_home_invited
act b view home
act b channel $CH
act b call banner-join
two_a()     { [ "$(f a 'call ' parts)" = 2 ]; }
two_b()     { [ "$(f b 'call ' parts)" = 2 ]; }
a_hears_b() { gt "$(peer a packets)" 50 && gt "$(peer a level)" 2000; }
b_hears_a() { gt "$(peer b packets)" 50 && gt "$(peer b level)" 2000; }
keyed()     { [ "$(peer a keyed)" = 1 ] && [ "$(peer b keyed)" = 1 ]; }
check "bob is in it, with alice" waitfor 10 seen b two_b
subject_b() { has b "^callobj view=$ID subject=\"general\" chat=[1-9]"; }
check "bob's call view names it \"general\", its chat beside it" waitfor 10 seen b subject_b
CHAT="$(grep -m1 '^callobj ' "$DB" | grep -o 'chat=[0-9]*' | cut -d= -f2)"
check "alice sees two in it" waitfor 10 seen a two_a
check "alice receives bob, and hears him" waitfor 15 seen a a_hears_b
check "bob receives alice, and hears her" waitfor 15 seen b b_hears_a
gui_snap a && gui_snap b || gui_abort "a client of the pair did not dump"
check "each holds the other's key for this epoch" keyed
# Then a hundred more of bob's packets (two seconds of them), none of which
# fails to decrypt.
u1="$(peer a undecryptable)"; p1="$(peer a packets)"
more() { ge "$(peer a packets)" $(( ${p1:-0} + 100 )); }
waitfor 10 seen a more || gui_abort "bob's packets stopped reaching alice"
same_u() { [ -n "$u1" ] && [ "$(peer a undecryptable)" = "$u1" ]; }
check "nothing fails to decrypt once the keys are in (${u1:-?} before, $(peer a undecryptable) a hundred packets on)" same_u
act a shot calls_incall

step "noise suppression"
act b call ns on
hum_gone() { lt "$(peer a level)" 1000; }
check "on at bob's end, his steady 660 Hz hum is taken out before it is sent" waitfor 10 seen a hum_gone
act b call ns off
check "off again, it comes back" waitfor 10 seen a a_hears_b

step "bob mutes, then holds the talk key"
act b call mute
a_sees_muted() { [ "$(peer a muted)" = 1 ] && lt "$(peer a level)" 300; }
a_hears_ptt()  { gt "$(peer a level)" 2000; }
check "alice sees bob muted, and hears nothing of him" waitfor 8 seen a a_sees_muted
act b call ptt-down
check "held, bob is heard again" waitfor 8 seen a a_hears_ptt
act b call ptt-up
act b call unmute

step "the keys"
act b key ctrl+shift+m
b_muted() { [ "$(f b 'call ' muted)" = 1 ]; }
b_unmuted() { [ "$(f b 'call ' muted)" = 0 ]; }
check "Ctrl+Shift+M mutes" waitfor 4 seen b b_muted
tip_muted() { has b '^callui .*mutetip="You are currently muted"'; }
check "Mute's tooltip says \"You are currently muted\"" tip_muted
act b key ctrl+shift+space
check "Ctrl+Shift+Space held, bob is heard though muted" waitfor 8 seen a a_hears_ptt
act b keyup ctrl+shift+space
a_silent() { lt "$(peer a level)" 300; }
check "let go, he is not" waitfor 8 seen a a_silent
act b key ctrl+shift+m
check "Ctrl+Shift+M again unmutes" waitfor 4 seen b b_unmuted
tip_unmuted() { has b '^callui .*mutetip="You are currently not muted"'; }
check "...and the tooltip says \"You are currently not muted\"" tip_unmuted

step "away from the call"
act b view home
act b channel $CH
strip_up() { gt "$(f b 'call ' strip | cut -d, -f3)" 0; }
check "the in-call strip is at the foot of bob's sidebar" waitfor 5 seen b strip_up
click_rect b "$(f b 'call ' stripmute)"
check "its Mute mutes" waitfor 4 seen b b_muted
click_rect b "$(f b 'call ' stripmute)"
waitfor 4 seen b b_unmuted || gui_abort "the strip's Mute did not unmute bob again"
click_corner b strip
b_inview() { [ "$(f b 'call ' inview)" = 1 ]; }
check "its body goes back to the call" waitfor 4 seen b b_inview
act b view home
act b channel $CH
b_away() { [ "$(f b 'call ' inview)" = 0 ] && gt "$(f b 'call ' hdr | cut -d, -f3)" 0; }
waitfor 5 seen b b_away || gui_abort "bob's #general, with the header's call button, did not come back"
click_rect b "$(f b 'call ' hdr)"
check "the header's call button opens the call going on" waitfor 4 seen b b_inview

step "the call's chat beside it"
# Wide enough for the call and the conversation side by side: narrower, the
# panel gives the call the room, as it is meant to.
act b size 1600 900
act b call open $CH
chat_on()  { [ "$(f b 'callui ' chat)" = 1 ]; }
chat_off() { [ "$(f b 'callui ' chat)" = 0 ]; }
check "bob's call view shows the call's chat beside it" waitfor 5 seen b chat_on
act b shot calls_view_chat
act b send "said from the call view"
a_got() { has a "^  ch $CHAT .*prev=\"said from the call view\""; }
check "a message sent from it reaches alice's copy of the call's chat" waitfor 10 seen a a_got
not_general() { ! has a '^  ch 1 .*prev="said from the call view"'; }
check "...and not #general" not_general
act b call chat
check "folded away, it is gone" waitfor 5 seen b chat_off
act b call chat
check "...and back" waitfor 5 seen b chat_on
act b call settings
settings_up() { [ "$(f b 'callui ' settings)" = 1 ] && has b '^a11yitem call\.mic ' && has b '^a11yitem call\.noise '; }
check "Settings holds the microphone, the speaker and noise suppression" waitfor 5 seen b settings_up
act b shot calls_settings
act b call settings

step "recording"
act a call open $CH
act a call record on
marked() { has "$1" "^callobj view=$ID .* recording=1 mark=[0-9]*,[0-9]*,[1-9]"; }
check "alice records: her call view shows the Recording mark" waitfor 10 seen a marked a
check "...and so does bob's" waitfor 10 seen b marked b
act a shot calls_recording
act b shot calls_recording_bob
# bob, already in it, was not held back; out and back in, he is.
act b call leave
b_gone() { [ "$(f b 'call ' in)" = 0 ]; }
waitfor 8 seen b b_gone || gui_abort "bob did not leave"
act b call join $CH
noticed() { has b "^callobj view=$ID .* notice=1 notice_id=$ID"; }
check "joining again, bob is shown \"This call is being recorded.\" first" waitfor 10 seen b noticed
act b shot calls_recorded_notice
not_yet() { [ "$(f b 'call ' in)" = 0 ]; }
check "...and is not in it until he says so" not_yet
a_no_b() { [ "$(f a 'call ' parts)" = 2 ]; }   # alice and the Recorder
check "...so alice's call has nobody of his: herself and the Recorder" seen a a_no_b
act b call rec-join
three_b() { [ "$(f b 'call ' parts)" = 3 ]; }   # alice, bob and the Recorder
check "his Join puts him in it" waitfor 10 seen b three_b
# Joined from the notice, he said he saw it: he is heard.
check "...and, having said he saw it, alice hears him" waitfor 15 seen a a_hears_b
ungated() { has b "^callobj .* gated=0 "; }
check "...his engine sends" ungated
act a call record off
unmarked() { has "$1" "^callobj view=$ID .* recording=0 mark=0,0,0,0"; }
check "alice stops it: the mark is gone for both" waitfor 10 seen a unmarked a
check "...for bob too" waitfor 10 seen b unmarked b

step "alice turns bob down"
gui_snap a || gui_abort "alice's client did not dump"
bob_id="$(peer a uid)"
[ -n "$bob_id" ] || gui_abort "alice's dump names no other person in the call"
act a call volume "$bob_id" 25
vol25() { [ "$(peer a volume)" = 25 ]; }
check "the engine plays bob at 25%" waitfor 5 seen a vol25
act a call volume "$bob_id" 100

step "alice shares her screen"
act b call open $CH
act a call open $CH
has_aid() { has "$1" "^a11yitem $2 "; }
check "alice's call view offers Share screen (call.share)" waitfor 5 seen a has_aid a call.share
act a call share "screen:synthetic"
sfield() { f "$1" 'share ' "$2"; }
# The floating bar is a second window, which a web page has none of (WEB.md):
# there the call view's own banner is the sharer's control.
a_sharing()   { [ "$(sfield a state)" = 2 ] && [ "$(sfield a bar)" = "$([ "$PLAT" = web ] && echo 0 || echo 1)" ]; }
b_sees_a()    { [ "$(sfield b sharer)" = "$(f a 'call ' self)" ] && ge "$(sfield b view_frames)" 10; }
b_picture()   { [ "$(sfield b tex)" = "$SHARE_WH" ] && ge "$(sfield b px_fno)" 1; }
check "alice's share is confirmed, and her bar is up" waitfor 10 seen a a_sharing
check "bob receives it" waitfor 15 seen b b_sees_a
check "bob's call view shows it at $SHARE_WH (2560x1600 fitted into the platform's limit), with a readable frame number" waitfor 10 seen b b_picture
f1="$(sfield b px_fno)"
rising() { gt "$(sfield b px_fno)" "${f1:-0}"; }
check "...which keeps moving on (from frame ${f1:-?})" waitfor 5 seen b rising
check "alice's view offers Stop sharing (call.share.stop)" waitfor 5 seen a has_aid a call.share.stop
stage() { has_aid b call.share.stage && has_aid b call.share.fullscreen && has_aid b call.share.actualsize; }
check "bob's names the picture and offers Full screen and Actual size" waitfor 5 seen b stage
act b shot share_viewer
act a shot share_sharer
act b call share full
full() { [ "$(sfield b full)" = 1 ]; }
notfull() { [ "$(sfield b full)" = 0 ]; }
check "bob goes full screen" waitfor 4 seen b full
act b shot share_full
act b key esc
check "Esc brings him back" waitfor 4 seen b notfull
act b call share actual
actual() { [ "$(sfield b actual)" = 1 ]; }
check "actual size" waitfor 4 seen b actual
act b call share actual

step "bob takes over, then stops"
act b call share "window:synthetic"
a_taken() { [ "$(sfield a on)" = 0 ] && [ "$(sfield a bar)" = 0 ] && [ "$(sfield a sharer)" = "$(f b 'call ' self)" ]; }
a_sees_b() { ge "$(sfield a view_frames)" 5 && [ "$(sfield a tex)" = "$SHARE_WH" ]; }
check "alice's share stops, and bob is the sharer" waitfor 10 seen a a_taken
check "alice sees bob's" waitfor 15 seen a a_sees_b
act b call share stop
nobody() { [ "$(sfield "$1" sharer)" = 0 ] && [ "$(sfield "$1" tex)" = 0x0 ]; }
check "bob stops: nobody shares, and alice's picture is gone" waitfor 10 seen a nobody a
bar_down() { [ "$(sfield b bar)" = 0 ]; }
check "...and bob's bar is down" waitfor 4 seen b bar_down

step "ending"
act b call end
refused() { [ "$(f b 'call ' err)" = 3029 ]; }
check "bob may not end it for everyone" waitfor 5 seen b refused
# End for everyone asks first: Cancel keeps the call, Confirm ends it.
act a call open-id "$ID"
act a call end-press
asks() { [ "$(f "$1" 'modal=' modal)" = confirm ]; }
check "alice's End for everyone asks before it acts" waitfor 5 seen a asks a
act a shot calls_end_confirm
act a key esc
still_in() { [ "$(f a 'call ' in)" = 1 ] && [ "$(f a 'modal=' modal)" = none ]; }
check "Cancel keeps the call" waitfor 5 seen a still_in
act a call end-press
waitfor 5 seen a asks a || gui_abort "End for everyone did not ask a second time"
act a key enter
a_out() { [ "$(f a 'call ' in)" = 0 ] && [ "$(f a calls n)" = 0 ]; }
b_out() { [ "$(f b 'call ' in)" = 0 ] && [ "$(f b calls n)" = 0 ]; }
check "confirmed, alice ends it: she is in no call and lists none" waitfor 8 seen a a_out
check "...and neither is bob" waitfor 8 seen b b_out
none_going() { has b '^calls n=0 '; }
check "with no call going on, bob's Calls row counts none" waitfor 5 seen b none_going

step "a call nobody takes"
act a call start $CH
waitfor 10 seen a pick_up || gui_abort "alice's picker did not open"
act a call pick-go
b_listed() { has b "^calls\[0\] ch=$CH"; }
waitfor 10 seen b b_listed || gui_abort "bob's Calls row did not count alice's new call"
gui_snap a || gui_abort "alice's client did not dump"
MID="$(f a 'call ' id)"
act a call leave
act b call home
# OC_CALL_HIST_MISSED (2): asked, never joined nor declined, and over.
missed() { grep -E "^callhist id=$MID .* flags=[0-9]+ " "$DB" | grep -oE 'flags=[0-9]+' | cut -d= -f2 |
           { read -r fl; [ -n "$fl" ] && [ $(( fl & 2 )) = 2 ]; }; }
check "bob's Calls home has it, missed" waitfor 10 seen b missed
act b shot calls_missed
press b "calls.row.$MID" || gui_abort "the missed call's row was not there to press"
paged() { has b "^callpage id=$MID shown=$MID .* stays=1 "; }
check "...and its page opens: alice alone was in it" waitfor 10 seen b paged

step "an invitation's Join"
act a call start $CH
waitfor 10 seen a pick_up || gui_abort "alice's picker did not open"
act a call pick-go
waitfor 10 seen b b_invited || gui_abort "bob was not invited to alice's new call"
# The toast names the workspace the call is in, as a real one does: bob's active
# one, which on a Windows client with saved workspaces need not be the first.
BWS="$(grep -m1 '^workspaces=' "$DB" | grep -o 'active=[0-9]*' | cut -d= -f2)"
act b toastaction "join|${BWS:-0}|$CH|"
check "the toast's Join puts bob in the call" waitfor 10 seen b two_b
act a call end
waitfor 8 seen b b_out || gui_abort "alice's End did not end the call for bob"

step "more members than the call holds"
# A second fixture: three members, a call of two.
gui_teardown || gui_abort "the first fixture did not stop: a daemon still holds :$OC_TEST_PORT"
rm -rf "$OC_TEST_DIR"; mkdir -p "$OC_TEST_DIR"
export OC_TEST_USERS="alice:pw:owner,bob:pw:member,carol:pw:member" OC_TEST_CALL_MAX=2
gui_pair_up
act a call start $CH
picker() { has a '^callpick .* n=2 ' && [ "$(grep -o '^callpick .*on=.*' "$DA" | grep -o ':1' | wc -l)" = 1 ]; }
check "alice picks: two members offered, one ticked, as a call of two holds" waitfor 10 seen a picker
act a call pick-go
one_invited() { [ "$(f a 'call ' in)" = 1 ] && [ "$(f a 'call ' invited)" = 1 ]; }
check "the call starts with the one she picked invited" waitfor 10 seen a one_invited
gui_end
