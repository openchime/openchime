#!/usr/bin/env bash
# A call in the Win32 client when UDP cannot reach the relay (PROTOCOL.md §5.17,
# AUDIO.md §4): two clients end to end, their media carried by the connection.
#
#   scripts/gui_calls_tcp.sh            # run, and leave the pair running
#   scripts/gui_calls_tcp.sh --down     # ... and take it down afterwards
#
# The daemon tells the clients the relay is at a UDP port nothing listens on
# (OPENCHIME_AUDIO_ADVERTISE_PORT), so every datagram they send is lost and none
# comes back -- a network that blocks UDP, as far as either can tell. It asserts
# that each client finds that within seconds and moves to the connection, that
# each then hears the other's tone, and that the call view says where the audio
# is going. Its own daemon on its own port, wiped each run.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PAIR="$HERE/scripts/gui_pair.sh"
fails=0

export OC_PAIR_PORT="${OC_PAIR_PORT:-9630}"
export OC_PAIR_DIR="${OC_PAIR_DIR:-/tmp/oc-calls-tcp}"
export OC_PAIR_HOST="${OC_PAIR_HOST:-$(ip -4 -o addr show eth0 | awk '{print $4}' | cut -d/ -f1)}"
export OC_PAIR_ADVERTISE_PORT=9   # discard: nothing answers there
export OPENCHIME_TEST_AUDIO=synthetic
export OC_PAIR_TONE_A=440 OC_PAIR_TONE_B=660
CH=1

say()  { printf '%s\n' "$*"; }
check() { local what="$1"; shift; if "$@"; then say "  ok   $what"; else say "  FAIL $what"; fails=$((fails + 1)); fi; }
dump() { "$PAIR" "$1" dump "callstcp_$1" > "/tmp/oc-callstcp-dump-$1.txt" 2>&1; }
field() { grep -m1 "^$2" "/tmp/oc-callstcp-dump-$1.txt" | tr ' ' '\n' | grep -m1 "^$3=" | cut -d= -f2- | tr -d '"'; }
peer()  { grep -m1 "^call.peer" "/tmp/oc-callstcp-dump-$1.txt" | tr ' ' '\n' | grep -m1 "^$2=" | cut -d= -f2; }
waitfor() { local who="$1" secs="$2" t=0; shift 2
  while [ "$t" -lt $((secs * 4)) ]; do dump "$who"; if "$@"; then return 0; fi; sleep 0.25; t=$((t + 1)); done; return 1; }
num() { [ -n "$1" ] && [ "$1" -eq "$1" ] 2>/dev/null; }
gt()  { num "$1" && [ "$1" -gt "$2" ]; }

say "== fixture: a fresh daemon whose relay the clients cannot reach over UDP"
"$PAIR" down >/dev/null 2>&1
rm -rf "$OC_PAIR_DIR"
"$PAIR" up || { say "pair did not come up"; exit 1; }

say "== alice starts a call, bob joins"
"$PAIR" a call start $CH >/dev/null
"$PAIR" a call ns off >/dev/null
"$PAIR" b call ns off >/dev/null
sleep 1
"$PAIR" b call join $CH >/dev/null
a_tcp() { [ "$(field a 'call ' transport)" = tcp ]; }
b_tcp() { [ "$(field b 'call ' transport)" = tcp ]; }
check "alice finds UDP does not get through, and goes by the connection" waitfor a 15 a_tcp
check "so does bob" waitfor b 15 b_tcp
a_hears_b() { gt "$(peer a packets)" 50 && gt "$(peer a level)" 2000; }
b_hears_a() { gt "$(peer b packets)" 50 && gt "$(peer b level)" 2000; }
check "alice hears bob's tone" waitfor a 20 a_hears_b
check "bob hears alice's tone" waitfor b 20 b_hears_a
lost_a="$(peer a lost)"; late_a="$(peer a late)"
say "  alice from bob: packets=$(peer a packets) level=$(peer a level) lost=$lost_a late=$late_a"
"$PAIR" a shot callstcp_incall >/dev/null
"$PAIR" a call leave >/dev/null
"$PAIR" b call leave >/dev/null

[ "${1:-}" = "--down" ] && "$PAIR" down >/dev/null 2>&1
if [ "$fails" -eq 0 ]; then say "gui_calls_tcp: all passed"; else say "gui_calls_tcp: $fails FAILED"; fi
exit "$fails"
