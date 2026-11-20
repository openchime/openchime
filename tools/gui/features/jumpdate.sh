#!/usr/bin/env bash
# Jump to a date: a conversation opened at the first message of a day.
#
#   tools/gui/features/jumpdate.sh          # run; the client and daemon stop at the end
#   tools/gui/features/jumpdate.sh --keep   # ... and leave them running
#
# A few messages are sent today; "jumpdate" with today's date lands on the
# first of today's, with a date divider on screen. A date after the newest
# lands on the newest, marked not exact. Its own daemon on its own port, wiped
# each run.
#
# platforms: win32
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
. "$HERE/tools/gui/harness/lib.sh"
gui_begin jumpdate 9760 /tmp/oc-jumpdate "Jump Fixture" "$@"

dj() { field "$D" '^datejump ' "$1"; }

step "a conversation opened at today's first message"
gui_launch
"$DRIVE" size 1200 900 >/dev/null
"$DRIVE" channel 1 >/dev/null
# One at a time, each back from the daemon before the next, so they are
# today's in the order sent.
for i in 1 2 3; do send_stored "jump fixture $i"; done
"$DRIVE" jumpdate "$(date +%Y-%m-%d)" >/dev/null
landed() { gui_snap; [ "$(dj pending)" = 0 ] && [ "$(dj found)" != 0 ] && [ "$(dj exact)" = 1 ] && [ "$(dj jump)" = 0 ]; }
check "today's first message is found, and the transcript is on it" waitfor 10 landed
check "the conversation stays the one asked about" [ "$(dj sel)" = 1 ]
"$DRIVE" shot jumpdate_today >/dev/null

step "after the newest: the newest, said so"
"$DRIVE" jumpdate "2099-01-01" >/dev/null
last() { gui_snap; [ "$(dj pending)" = 0 ] && [ "$(dj exact)" = 0 ] && [ "$(dj found)" != 0 ]; }
check "the last message before it, not exact" waitfor 10 last
"$DRIVE" shot jumpdate_after >/dev/null

step "the date divider is there to click"
check "a date divider is on screen" [ "$(dj seps)" -ge 1 ]
gui_end
