#!/usr/bin/env bash
# Calls in the Win32 client, two clients end to end (REQ-150, REQ-301-305,
# ARCH-113, docs/CALLS.md).
#
#   scripts/gui_calls.sh            # run, and leave the pair running
#   scripts/gui_calls.sh --down     # ... and take it down afterwards
#
# alice and bob, each a real client with the synthetic audio devices -- alice's
# microphone a 440 Hz tone, bob's 660 Hz -- over gui_pair.sh, against a real
# daemon and its relay. It asserts that
#
#   - a call started in #general invites bob, who sees it in his Calls section
#     and gets an invitation notification;
#   - bob joins: each receives the other's audio, every packet decrypting once
#     the first second's keys are in, with both keyed for the current epoch;
#   - noise suppression takes a steady hum out -- which is what a test tone is
#     to it, so the rest runs with it off;
#   - bob mutes: alice stops receiving his audio and sees that he is muted;
#     holding the talk key lets him through again;
#   - a volume set for bob is the one the engine plays him at;
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
# Its own daemon on its own port, wiped each run. The clients reach it at the
# WSL machine's address, not 127.0.0.1: WSL forwards only TCP there, and a
# call's audio is UDP.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PAIR="$HERE/scripts/gui_pair.sh"
START=$SECONDS
fails=0
checks=0

export OC_PAIR_PORT="${OC_PAIR_PORT:-9620}"
export OC_PAIR_DIR="${OC_PAIR_DIR:-/tmp/oc-calls}"
export OC_PAIR_HOST="${OC_PAIR_HOST:-$(ip -4 -o addr show eth0 | awk '{print $4}' | cut -d/ -f1)}"
export OPENCHIME_TEST_AUDIO=synthetic
export OC_PAIR_TONE_A=440 OC_PAIR_TONE_B=660
CH=1                                   # #general: both are members

say()  { printf '%s\n' "$*"; }
fail() { printf '  FAIL %s\n' "$*"; fails=$((fails + 1)); }
ok()   { printf '  ok   %s\n' "$*"; }
check() { local what="$1"; shift; checks=$((checks + 1)); if "$@"; then ok "$what"; else fail "$what"; fi; }

# A dump of one client, into a file this script reads.
dump() { "$PAIR" "$1" dump "calls_$1" > "/tmp/oc-calls-dump-$1.txt" 2>&1; }
# The value of key `k` on the first line starting with `prefix`.
field() {  # field <who> <line-prefix> <key>
  grep -m1 "^$2" "/tmp/oc-calls-dump-$1.txt" | tr ' ' '\n' | grep -m1 "^$3=" | cut -d= -f2- | tr -d '"'
}
# Wait up to `secs` for a condition (a command) over fresh dumps of `who`.
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
lt()  { num "$1" && [ "$1" -lt "$2" ]; }
# Click the centre of a rect the dump gives as l,t,r,b under key `k` of the call line.
click_rect() {  # click_rect <who> <key>
  local r; r="$(field "$1" 'call ' "$2")"
  local l t rr b; IFS=, read -r l t rr b <<< "$r"
  [ -n "${b:-}" ] && [ "${rr%.*}" -gt 0 ] 2>/dev/null || return 1
  "$PAIR" "$1" click $(( (l + rr) / 2 )) $(( (t + b) / 2 )) >/dev/null
}
# Click just inside the top-left of such a rect: the strip's own body, above its buttons.
click_corner() {  # click_corner <who> <key>
  local r; r="$(field "$1" 'call ' "$2")"
  local l t rr b; IFS=, read -r l t rr b <<< "$r"
  [ -n "${b:-}" ] && [ "${rr%.*}" -gt 0 ] 2>/dev/null || return 1
  "$PAIR" "$1" click $(( l + 40 )) $(( t + 10 )) >/dev/null
}
peer() {  # peer <who> <key>: that client's one other participant's value
  grep -m1 "^call.peer" "/tmp/oc-calls-dump-$1.txt" | tr ' ' '\n' | grep -m1 "^$2=" | cut -d= -f2
}

say "== fixture: a fresh daemon, alice (440 Hz) and bob (660 Hz) at $OC_PAIR_HOST:$OC_PAIR_PORT"
"$PAIR" down >/dev/null 2>&1
rm -rf "$OC_PAIR_DIR"
"$PAIR" up || { say "pair did not come up"; exit 1; }
offered() { ge "$(field a 'call ' cap)" 2 && ge "$(field b 'call ' cap)" 2; }
dump a; dump b
check "the daemon offers calls to both" offered

say "== alice starts a call in #general, which asks bob"
"$PAIR" a call start $CH >/dev/null
a_alone()     { [ "$(field a 'call ' in)" = 1 ] && [ "$(field a 'call ' parts)" = 1 ]; }
b_invited()   { grep -q "^calls\[0\] ch=$CH .*me_invited=1" /tmp/oc-calls-dump-b.txt; }
b_notified()  { ge "$(field b tray_live toasts_raised)" 1; }
check "alice is in it, alone, as its starter" waitfor a 10 a_alone
check "bob's Calls section lists it, with him invited" waitfor b 10 b_invited
check "bob was notified" b_notified
"$PAIR" b shot calls_invited >/dev/null

say "== bob joins"
# A test tone is a steady hum, which noise suppression is there to take out; the
# audio checks below turn it off, and one check turns it on to see it do that.
"$PAIR" a call ns off >/dev/null
"$PAIR" b call ns off >/dev/null
"$PAIR" b call join $CH >/dev/null
two_a()     { [ "$(field a 'call ' parts)" = 2 ]; }
two_b()     { [ "$(field b 'call ' parts)" = 2 ]; }
a_hears_b() { gt "$(peer a packets)" 50 && gt "$(peer a level)" 2000; }
b_hears_a() { gt "$(peer b packets)" 50 && gt "$(peer b level)" 2000; }
keyed()     { [ "$(peer a keyed)" = 1 ] && [ "$(peer b keyed)" = 1 ]; }
check "bob is in it, with alice" waitfor b 10 two_b
check "alice sees two in it" waitfor a 10 two_a
check "alice receives bob, and hears him" waitfor a 15 a_hears_b
check "bob receives alice, and hears her" waitfor b 15 b_hears_a
check "each holds the other's key for this epoch" keyed
u1="$(peer a undecryptable)"; sleep 2; dump a
same_u() { [ "$(peer a undecryptable)" = "$u1" ]; }
check "nothing fails to decrypt once the keys are in (${u1:-?} in the first second)" same_u
"$PAIR" a shot calls_incall >/dev/null

say "== noise suppression"
"$PAIR" b call ns on >/dev/null
hum_gone() { lt "$(peer a level)" 1000; }
check "on at bob's end, his steady 660 Hz hum is taken out before it is sent" waitfor a 10 hum_gone
"$PAIR" b call ns off >/dev/null
check "off again, it comes back" waitfor a 10 a_hears_b

say "== bob mutes, then holds the talk key"
"$PAIR" b call mute >/dev/null
a_sees_muted() { [ "$(peer a muted)" = 1 ] && lt "$(peer a level)" 300; }
a_hears_ptt()  { gt "$(peer a level)" 2000; }
check "alice sees bob muted, and hears nothing of him" waitfor a 8 a_sees_muted
"$PAIR" b call ptt-down >/dev/null
check "held, bob is heard again" waitfor a 8 a_hears_ptt
"$PAIR" b call ptt-up >/dev/null
"$PAIR" b call unmute >/dev/null

say "== the keys"
"$PAIR" b key ctrl+shift+m >/dev/null
b_muted() { [ "$(field b 'call ' muted)" = 1 ]; }
b_unmuted() { [ "$(field b 'call ' muted)" = 0 ]; }
check "Ctrl+Shift+M mutes" waitfor b 4 b_muted
"$PAIR" b key ctrl+shift+space >/dev/null
check "Ctrl+Shift+Space held, bob is heard though muted" waitfor a 8 a_hears_ptt
"$PAIR" b keyup ctrl+shift+space >/dev/null
a_silent() { lt "$(peer a level)" 300; }
check "let go, he is not" waitfor a 8 a_silent
"$PAIR" b key ctrl+shift+m >/dev/null
check "Ctrl+Shift+M again unmutes" waitfor b 4 b_unmuted

say "== away from the call"
"$PAIR" b view home >/dev/null
"$PAIR" b channel $CH >/dev/null
dump b
strip_up() { gt "$(field b 'call ' strip | cut -d, -f3)" 0; }
check "the in-call strip is at the foot of bob's sidebar" strip_up
click_rect b stripmute
check "its Mute mutes" waitfor b 4 b_muted
click_rect b stripmute
waitfor b 4 b_unmuted >/dev/null
click_corner b strip
b_inview() { [ "$(field b 'call ' inview)" = 1 ]; }
check "its body goes back to the call" waitfor b 4 b_inview
"$PAIR" b view home >/dev/null
"$PAIR" b channel $CH >/dev/null
dump b
click_rect b hdr
check "the header's call button opens the call going on" waitfor b 4 b_inview

say "== alice turns bob down"
bob_id="$(peer a uid)"
"$PAIR" a call volume "$bob_id" 25 >/dev/null
dump a
vol25() { [ "$(peer a volume)" = 25 ]; }
check "the engine plays bob at 25%" vol25
"$PAIR" a call volume "$bob_id" 100 >/dev/null

say "== ending"
"$PAIR" b call end >/dev/null
refused() { [ "$(field b 'call ' err)" = 3029 ]; }
check "bob may not end it for everyone" waitfor b 5 refused
"$PAIR" a call end >/dev/null
a_out() { [ "$(field a 'call ' in)" = 0 ] && [ "$(field a calls n)" = 0 ]; }
b_out() { [ "$(field b 'call ' in)" = 0 ] && [ "$(field b calls n)" = 0 ]; }
check "alice ends it: she is in no call and lists none" waitfor a 8 a_out
check "...and neither is bob" waitfor b 8 b_out

say "== a call nobody takes"
"$PAIR" a call start $CH >/dev/null
b_listed() { grep -q "^calls\[0\] ch=$CH" /tmp/oc-calls-dump-b.txt; }
waitfor b 10 b_listed
"$PAIR" a call leave >/dev/null
missed() { ge "$(field b callevents n)" 1 && grep -q '^callevents .*last="Missed call"' /tmp/oc-calls-dump-b.txt; }
check "bob's history has a missed call" waitfor b 10 missed
"$PAIR" b view home >/dev/null
"$PAIR" b channel $CH >/dev/null
sleep 0.5
"$PAIR" b shot calls_missed >/dev/null

say "== an invitation's Join"
"$PAIR" a call start $CH >/dev/null
waitfor b 10 b_invited >/dev/null
"$PAIR" b toastaction "join|0|$CH|" >/dev/null
check "the toast's Join puts bob in the call" waitfor b 10 two_b
"$PAIR" a call end >/dev/null
waitfor b 8 b_out >/dev/null

say "== more members than the call holds"
"$PAIR" down >/dev/null 2>&1
rm -rf "$OC_PAIR_DIR"
OC_PAIR_USERS="alice:pw:owner,bob:pw:member,carol:pw:member" OC_PAIR_CALL_MAX=2 "$PAIR" up >/dev/null 2>&1
"$PAIR" a call start $CH >/dev/null
picker() { grep -q '^callpick .* n=2 ' /tmp/oc-calls-dump-a.txt && [ "$(grep -o '^callpick .*on=.*' /tmp/oc-calls-dump-a.txt | grep -o ':1' | wc -l)" = 1 ]; }
check "alice picks: two members offered, one ticked, as a call of two holds" waitfor a 10 picker
"$PAIR" a call pick-go >/dev/null
one_invited() { [ "$(field a 'call ' in)" = 1 ] && [ "$(field a 'call ' invited)" = 1 ]; }
check "the call starts with the one she picked invited" waitfor a 10 one_invited

say ""
say "gui_calls: $((checks - fails))/$checks passed in $((SECONDS - START))s"
[ "${1:-}" = "--down" ] && "$PAIR" down >/dev/null
[ "$fails" = 0 ]
