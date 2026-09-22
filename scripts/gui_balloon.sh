#!/usr/bin/env bash
# Is the tray balloon actually DRAWN? Raise one through the real delivery chain
# and photograph the desktop.
#
#   scripts/gui_balloon.sh           # launch, raise, capture, leave it running
#   scripts/gui_balloon.sh --kill    # ... and shut the client down afterwards
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
# is disconnected or locked, nothing is being composited, and the run says so
# rather than reporting a missing balloon. Focus Assist / Do Not Disturb also
# hides it while still accepting it; the capture is how you tell.
#
# NOT IN CI, for the reason gui_smoke.sh gives, and because it needs a desktop
# somebody is connected to.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DRIVE="$HERE/scripts/gui_drive.sh"
LIN_DIR='/mnt/c/Windows/Temp/octest'
OUT_WIN='C:\Windows\Temp\ocballoon.png'
OUT_LIN='/mnt/c/Windows/Temp/ocballoon.png'
DEST="${GUI_BALLOON_DEST:-/tmp/ocballoon.png}"

"$DRIVE" launch >/dev/null
"$DRIVE" wintoast 0 >/dev/null
"$DRIVE" toast "tray balloon check" >/dev/null
"$DRIVE" dump balloon >/dev/null
by=$(grep -a -o '^notify deliver=[0-9]* by=[0-9]*' "$LIN_DIR/balloon.txt" | sed 's/.*by=//')
# The shell animates it in; a capture in the same instant catches an empty corner.
sleep 3
shot=$(powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(wslpath -w "$HERE/scripts/gui_balloon.ps1")" -Out "$OUT_WIN" | tr -d '\r' | tail -1)
cp "$OUT_LIN" "$DEST" 2>/dev/null || true
"$DRIVE" wintoast 1 >/dev/null
[ "${1:-}" = "--kill" ] && "$DRIVE" kill >/dev/null 2>&1 || true

if [ "$by" != "2" ]; then
  echo "gui_balloon: FAIL -- the balloon did not carry it (by=${by:-?}; 1 toast, 2 balloon, 3 own window)"
  exit 1
fi
case "$shot" in
  ok*)    echo "gui_balloon: accepted by the shell; look at $DEST (${shot#ok })" ;;
  blank*) echo "gui_balloon: accepted by the shell, NOT OBSERVED -- the desktop is blank (session disconnected or locked)"; exit 2 ;;
  *)      echo "gui_balloon: accepted by the shell, capture failed: $shot"; exit 2 ;;
esac
