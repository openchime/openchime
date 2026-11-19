#!/usr/bin/env bash
# A shortcut pressed quickly in the browser is still the shortcut (REQ-264).
#
#   tools/gui/features/chords.sh            # run; the client and daemon stop at the end
#   tools/gui/features/chords.sh --keep     # ... and leave them running
#
# The browser hands the client its keys in batches between frames, so a quick
# Ctrl+, reaches it as Ctrl down, comma down, comma up, Ctrl up all at once.
# The comma must be handled with the Ctrl it was pressed with, not with the
# keyboard's state by the time it is handled, when Ctrl is already up. The
# driver's `press` verb sends a chord the way a keyboard does, through the page
# and SDL, as one batch; the `key` verb would hand the client the chord
# directly and could not tell. Each chord is pressed several times, since the
# race it guards was lost only most of the time. Web only: the Windows driver
# has no keyboard of its own to press.
#
# platforms: web
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
. "$HERE/tools/gui/harness/lib.sh"
gui_begin chords 9750 /tmp/oc-chords "$@"

if [ "$PLAT" != web ]; then
  step "skipped: the browser's keyboard only"
  gui_end
fi

TIMES=5
WAIT_MS="${OC_TEST_WAIT_MS:-6000}"
WAIT_S=$(( (WAIT_MS + 999) / 1000 ))

# The chord is dispatched when `press` acks, and taken on the client's next
# frame: poll for the state it leads to rather than read once after a pause.
modal_is() { gui_snap && [ "$(field "$D" '^modal=' modal)" = "$1" ]; }
is() { waitfor "$WAIT_S" modal_is "$1"; }     # is <modal>
# Nothing opens: still `none` a second on. A slow client could only make this
# pass wrongly, never fail with nothing wrong.
stays_none() {
  local t
  for t in 1 2 3 4 5 6 7 8 9 10; do
    modal_is none || return 1
    # paced: the second of quiet is the check -- nothing may open in it
    sleep 0.1
  done
}

step "fixture: alice in the browser"
gui_launch
check "nothing is open to begin with" is none

step "Ctrl+, opens Preferences, every time"
n=0
for _ in $(seq 1 $TIMES); do
  act press ctrl+comma
  is prefs && n=$((n + 1))
  act press esc
  is none
done
check "Preferences opened $n of $TIMES times" [ "$n" = "$TIMES" ]
check "...and Esc closed it" is none

step "Ctrl+/ opens the shortcuts, every time"
n=0
for _ in $(seq 1 $TIMES); do
  act press ctrl+slash
  is keys && n=$((n + 1))
  act press esc
  is none
done
check "the shortcuts opened $n of $TIMES times" [ "$n" = "$TIMES" ]

step "a bare comma is not the shortcut"
act press comma
check "a comma alone opens nothing" stays_none
gui_end
