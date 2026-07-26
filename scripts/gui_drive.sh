#!/usr/bin/env bash
# Drive the Win32 client through its in-app test hook (OPENCHIME_TEST_DIR) — a
# file command channel that works regardless of display state. Screenshots are
# rendered by the app itself (Direct2D DC render target), so no screen-scraping.
#
#   scripts/gui_drive.sh launch [ws] [user:pass]   # start the client with the hook on
#   scripts/gui_drive.sh <cmd...>                  # send one command, wait for ack
#   scripts/gui_drive.sh shot <name>               # render to <scratch>/<name>.bmp
#   scripts/gui_drive.sh kill
#
# Commands: shot <winpath> | send <text> | channel <name> | click x y |
#           rclick x y | members | scroll <dy> | size w h | dump <winpath>
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXE="$HERE/build/openchime.exe"
WIN_DIR='C:\Windows\Temp\octest'
LIN_DIR='/mnt/c/Windows/Temp/octest'
OUT="${GUI_DRIVE_OUT:-/tmp/ocshot}"

mkdir -p "$LIN_DIR" "$OUT"

case "${1:-}" in
  launch)
    ws="${2:-127.0.0.1:8443}"; cred="${3:-alice:pw}"
    rm -f "$LIN_DIR"/cmd "$LIN_DIR"/ack
    # WSLENV is required for the env var to cross into the Windows process.
    WSLENV="${WSLENV:+$WSLENV:}OPENCHIME_TEST_DIR" OPENCHIME_TEST_DIR="$WIN_DIR" \
        setsid "$EXE" "$ws" "$cred" >/dev/null 2>&1 < /dev/null &
    disown; sleep 3; echo "launched"; exit 0 ;;
  kill)
    powershell.exe -NoProfile -Command "Get-Process openchime -EA SilentlyContinue|Stop-Process -Force" >/dev/null 2>&1 || true
    echo killed; exit 0 ;;
  "") echo "usage: gui_drive.sh launch|<cmd...>|kill" >&2; exit 2 ;;
esac

# Convenience: `shot foo` -> render into the scratch dir and copy back as PNG-able BMP.
if [ "$1" = "shot" ] && [ $# -eq 2 ]; then
  name="$2"; cmd="shot ${WIN_DIR}\\${name}.bmp"
else
  cmd="$*"
fi

rm -f "$LIN_DIR/ack"
printf '%s' "$cmd" > "$LIN_DIR/cmd.tmp"
mv "$LIN_DIR/cmd.tmp" "$LIN_DIR/cmd"          # atomic handoff

for _ in $(seq 1 100); do [ -f "$LIN_DIR/ack" ] && break; sleep 0.1; done
ack="$(cat "$LIN_DIR/ack" 2>/dev/null || echo TIMEOUT)"
echo "ack: $ack"

if [ "$1" = "shot" ] && [ $# -eq 2 ]; then
  cp "$LIN_DIR/${2}.bmp" "$OUT/${2}.bmp" 2>/dev/null && echo "$OUT/${2}.bmp"
fi
