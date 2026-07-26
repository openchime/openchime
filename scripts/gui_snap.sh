#!/usr/bin/env bash
# Screenshot feedback loop for the native Win32 GUI, driven from WSL.
#
#   scripts/gui_snap.sh launch [ws] [user:pass]   # start build/openchime.exe, then snap
#   scripts/gui_snap.sh                            # snap the running window
#   scripts/gui_snap.sh kill                       # close the running client
#
# The PNG lands in the scratchpad path echoed on stdout (readable from Linux).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXE="$HERE/build/openchime.exe"
PS1="$HERE/scripts/gui_snap.ps1"
OUT_WIN='C:\Windows\Temp\ocsnap.png'
OUT_LIN='/mnt/c/Windows/Temp/ocsnap.png'
DEST="${GUI_SNAP_DEST:-/tmp/ocsnap.png}"

cmd="${1:-snap}"
case "$cmd" in
  launch)
    ws="${2:-127.0.0.1:8443}"; cred="${3:-alice:pw}"
    [ -x "$EXE" ] || { echo "build first: make windows-gui" >&2; exit 1; }
    # WSL can exec the PE directly; detach so this script returns. Pass no creds
    # to exercise the login dialog / cached-session path.
    if [ -n "$cred" ]; then
      setsid "$EXE" "$ws" "$cred" >/dev/null 2>&1 < /dev/null &
    else
      setsid "$EXE" >/dev/null 2>&1 < /dev/null &
    fi
    disown || true
    sleep 3
    ;;
  kill)
    powershell.exe -NoProfile -Command "Get-Process openchime -ErrorAction SilentlyContinue | Stop-Process -Force" >/dev/null 2>&1 || true
    echo "killed"; exit 0
    ;;
  snap) ;;
  *) echo "usage: gui_snap.sh [launch|snap|kill]" >&2; exit 2 ;;
esac

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(wslpath -w "$PS1")" \
  -Title OpenChime -Out "$OUT_WIN" 2>&1 | tr -d '\r'
cp "$OUT_LIN" "$DEST"
echo "$DEST"
