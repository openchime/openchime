#!/usr/bin/env bash
# Jump to a date: a conversation opened at the first message of a day.
#
#   tools/gui/jumpdate.sh          # run, and leave the client running
#   tools/gui/jumpdate.sh --kill   # ... and shut it down afterwards
#
# A few messages are sent today; "jumpdate" with today's date lands on the
# first of today's, with a date divider on screen. A date after the newest
# lands on the newest, marked not exact. Its own daemon on its own port, wiped
# each run.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
. "$HERE/tools/gui/lib.sh"

export OC_TEST_PORT="${OC_TEST_PORT:-9710}"
export OC_TEST_DIR="${OC_TEST_DIR:-/tmp/oc-jumpdate}"
export OC_TEST_WS="${OC_TEST_WS:-Jump Fixture}"

kill_dev_daemon || { say "a daemon still holds :$OC_TEST_PORT; stop it and re-run"; exit 1; }
rm -rf "$OC_TEST_DIR"

D="/tmp/oc-jumpdate-dump.txt"
snap() { "$DRIVE" dump jumpdate 2>/dev/null | tail -n +2 > "$D"; }
dj() { grep -m1 '^datejump ' "$D" | grep -o "\b$1=[^ ]*" | head -1 | cut -d= -f2; }
waitfor() {  # waitfor <secs> <command...>
  local secs="$1" t=0; shift
  while [ "$t" -lt $((secs * 4)) ]; do if "$@"; then return 0; fi; sleep 0.25; t=$((t + 1)); done
  return 1
}

say "== a conversation opened at today's first message"
"$DRIVE" launch >/dev/null 2>&1 || { say "launch failed"; exit 1; }
for _ in $(seq 1 80); do snap; grep -q 'authed=1' "$D" && break; sleep 0.25; done
"$DRIVE" size 1200 900 >/dev/null
"$DRIVE" channel 1 >/dev/null
for i in 1 2 3; do "$DRIVE" send "jump fixture $i" >/dev/null; sleep 0.3; done
"$DRIVE" jumpdate "$(date +%Y-%m-%d)" >/dev/null
landed() { snap; [ "$(dj pending)" = 0 ] && [ "$(dj found)" != 0 ] && [ "$(dj exact)" = 1 ] && [ "$(dj jump)" = 0 ]; }
check "today's first message is found, and the transcript is on it" waitfor 10 landed
check "the conversation stays the one asked about" [ "$(dj sel)" = 1 ]
"$DRIVE" shot jumpdate_today >/dev/null

say "== after the newest: the newest, said so"
"$DRIVE" jumpdate "2099-01-01" >/dev/null
last() { snap; [ "$(dj pending)" = 0 ] && [ "$(dj exact)" = 0 ] && [ "$(dj found)" != 0 ]; }
check "the last message before it, not exact" waitfor 10 last
"$DRIVE" shot jumpdate_after >/dev/null

say "== the date divider is there to click"
check "a date divider is on screen" [ "$(dj seps)" -ge 1 ]
rm -f "$D"

say ""
say "gui_jumpdate: $((checks - fails))/$checks passed in $((SECONDS - START))s"
[ "${1:-}" = "--kill" ] && { "$DRIVE" kill >/dev/null; kill_dev_daemon; }
[ "$fails" = 0 ]
