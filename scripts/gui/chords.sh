#!/usr/bin/env bash
# A shortcut pressed quickly in the browser is still the shortcut (REQ-264).
#
#   scripts/gui/chords.sh            # run, and leave the client running
#   scripts/gui/chords.sh --down     # ... and take it down afterwards
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
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
. "$HERE/scripts/gui/lib.sh"

if [ "$PLAT" != web ]; then
  echo "gui_chords: skipped (the browser's keyboard only)"; exit 0
fi

export OC_TEST_PORT="${OC_TEST_PORT:-9700}"
export OC_TEST_DIR="${OC_TEST_DIR:-/tmp/oc-chords}"
TIMES=5

drive() { "$DRIVE" "$@" >/dev/null; }
modal() { "$DRIVE" dump chords 2>/dev/null | grep -m1 '^modal=' | sed 's/^modal=\([^ ]*\).*/\1/'; }
is()    { [ "$(modal)" = "$1" ]; }

say "== fixture: alice in the browser"
"$DRIVE" kill >/dev/null 2>&1
kill_dev_daemon || { say "the old daemon did not stop"; exit 1; }
rm -rf "$OC_TEST_DIR"
"$DRIVE" launch >/dev/null || { say "the client did not launch"; exit 1; }
check "nothing is open to begin with" is none

say "== Ctrl+, opens Preferences, every time"
n=0
for _ in $(seq 1 $TIMES); do
  drive press ctrl+comma
  is prefs && n=$((n + 1))
  drive press esc
done
check "Preferences opened $n of $TIMES times" [ "$n" = "$TIMES" ]
check "...and Esc closed it" is none

say "== Ctrl+/ opens the shortcuts, every time"
n=0
for _ in $(seq 1 $TIMES); do
  drive press ctrl+slash
  is keys && n=$((n + 1))
  drive press esc
done
check "the shortcuts opened $n of $TIMES times" [ "$n" = "$TIMES" ]

say "== a bare comma is not the shortcut"
drive press comma
check "a comma alone opens nothing" is none

[ "${1:-}" = --down ] && { "$DRIVE" kill >/dev/null 2>&1; kill_dev_daemon; }
echo "gui_chords: $((checks - fails))/$checks passed ($((SECONDS - START))s)"
[ "$fails" = 0 ]
