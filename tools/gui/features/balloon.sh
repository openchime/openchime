#!/usr/bin/env bash
# Is the tray balloon actually DRAWN? Raise one through the real delivery chain
# and photograph the desktop.
#
#   tools/gui/features/balloon.sh           # launch, raise, capture; the client and daemon stop at the end
#   tools/gui/features/balloon.sh --keep    # ... and leave them running
#
# The balloon is the middle of the chain -- WinRT toast, else balloon, else the
# client's own window -- and on any machine where toasts work it is never
# reached, so nothing had ever seen it. This turns the toast off for the run
# (`wintoast 0`, nothing saved), sends one notification, and checks two things:
#
#   1. the chain says the balloon carried it (notify by=2 in the dump) -- that
#      Shell_NotifyIconW accepted NIF_INFO;
#   2. a capture of the whole desktop, taken while it should be up, for a person
#      to look at. The shell draws the balloon, so only the desktop shows it.
#
# THREE OUTCOMES, NOT TWO. "accepted" and a picture is the pass. "not accepted"
# is a failure of ours. "accepted" with a BLANK desktop is neither: the session
# is disconnected or locked, nothing is being composited, and the run stops
# saying so rather than reporting a missing balloon. Focus Assist / Do Not
# Disturb also hides it while still accepting it; the capture is how you tell.
#
# Its own daemon on its own port, wiped each run. NOT IN CI, for the reason
# tools/gui/features/smoke.sh gives, and because it needs a desktop somebody is
# connected to.
#
# platforms: win32
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
. "$HERE/tools/gui/harness/lib.sh"
gui_begin balloon 9786 /tmp/oc-balloon "$@"
OUT_WIN='C:\Windows\Temp\ocballoon.png'
OUT_LIN='/mnt/c/Windows/Temp/ocballoon.png'
DEST="${GUI_BALLOON_DEST:-/tmp/ocballoon.png}"

step "one notification, with the toast off"
gui_launch
"$DRIVE" wintoast 0 >/dev/null || gui_abort "the client did not turn its toast off"
"$DRIVE" toast "tray balloon check" >/dev/null || gui_abort "the client did not take the notification"
by() { field "$D" '^notify ' by; }
carried() { gui_snap && [ "$(by)" = 2 ]; }
check "the balloon carried it (1 toast, 2 balloon, 3 own window)" waitfor 5 carried
say "  notify by=$(by)"

step "the desktop, for a person to look at"
# paced: the shell animates the balloon in, and nothing the client can dump says when it is drawn; a capture in the same instant catches an empty corner
sleep 3
rm -f "$OUT_LIN"
shot=$(win_capture_desktop "$OUT_WIN")
"$DRIVE" wintoast 1 >/dev/null || true
case "$shot" in
  ok*)    cp "$OUT_LIN" "$DEST" || gui_abort "the capture was not written"
          say "  look at $DEST (${shot#ok })" ;;
  blank*) gui_abort "NOT OBSERVED: the desktop is blank (session disconnected or locked)" ;;
  *)      gui_abort "the desktop capture failed: $shot" ;;
esac
gui_end
