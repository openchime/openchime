#!/usr/bin/env bash
# Screen recording in the Win32 client, end to end (REQ-162, REQ-166, ARCH-110).
#
#   scripts/gui_screenrec.sh            # run, and leave the client running
#   scripts/gui_screenrec.sh --kill     # ... and shut it down afterwards
#
# Real Windows.Graphics.Capture on this machine, a known picture in the camera
# box (OPENCHIME_TEST_CAPTURE=synthetic-camera: colour bars; the screens are
# real), and known sound (OPENCHIME_TEST_AUDIO=synthetic: the microphone a
# 440 Hz tone, the computer's sound 660 Hz). It asserts that
#
#   - the screens and windows are listed, OpenChime's own among them;
#   - the camera box is in the corner chosen, in the preview;
#   - recording steps the card aside for the recording bar, and the bar is kept
#     out of the capture and reachable through UI Automation;
#   - Stop (pressed through UI Automation) ends it at the size the screen fits;
#   - Send posts it, and the file the daemon stored is VP9 and Opus at that size,
#     carrying both the microphone and the computer's sound;
#   - a window recorded until it closes keeps what came before.
#
# WHOLE-SCREEN CAPTURE NEEDS A DRAWN DESKTOP. Over Remote Desktop, a session
# whose window is minimised or covered is not drawn, and a screen captures as
# black; keep the Remote Desktop window open while this runs. A window is
# captured either way.
#
# Its own daemon on its own port, wiped each run, for the reasons gui_smoke.sh
# gives. Not in CI: the daemon cannot run on a Windows runner.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DRIVE="$HERE/scripts/gui_drive.sh"
LIN_DIR='/mnt/c/Windows/Temp/octest'
START=$SECONDS
fails=0
checks=0

export OC_DEV_PORT="${OC_DEV_PORT:-9530}"
export OC_DEV_DIR="${OC_DEV_DIR:-/tmp/oc-screenrec}"
export OC_DEV_WS="${OC_DEV_WS:-Screen Fixture}"

say()  { printf '%s\n' "$*"; }
fail() { printf '  FAIL %s\n' "$*"; fails=$((fails + 1)); }
ok()   { printf '  ok   %s\n' "$*"; }

kill_dev_daemon() {
  local p
  for p in $(pgrep -x openchimed 2>/dev/null); do
    tr '\0' '\n' < "/proc/$p/environ" 2>/dev/null |
      grep -qx "OPENCHIME_PROTO_PORT=$OC_DEV_PORT" && kill "$p" 2>/dev/null
  done
  for _ in 1 2 3 4 5 6 7 8 9 10; do
    (exec 3<>/dev/tcp/127.0.0.1/"$OC_DEV_PORT") 2>/dev/null || return 0
    exec 3<&- 3>&-; sleep 0.3
  done
  return 1
}

drive() {
  if ! "$DRIVE" "$@" >/dev/null 2>&1; then
    checks=$((checks + 1)); fail "the client did not answer '$*'"; return 1
  fi
  return 0
}
snap() { "$DRIVE" dump screenrec >/dev/null 2>&1; cat "$LIN_DIR/screenrec.txt" 2>/dev/null; }
line() { snap | grep "^$1"; }
key_of() { printf '%s' "$1" | grep -o "\b$2=[^ ]*" | head -1 | cut -d= -f2 | tr -d '"'; }
check() {
  local label="$1"; shift
  checks=$((checks + 1))
  if "$@"; then ok "$label"; else fail "$label"; fi
}
wait_key() {                          # wait_key <line-prefix> <key> <want> [ms]
  local t=0 ms="${4:-10000}"
  while :; do
    [ "$(key_of "$(line "$1")" "$2")" = "$3" ] && return 0
    [ "$t" -ge "$ms" ] && return 1
    sleep 0.1; t=$((t + 100))
  done
}
uia_bar() { powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(wslpath -w "$HERE/scripts/uia_recbar.ps1")" "$@" 2>&1 | tr -d '\r'; }

# --- launch -------------------------------------------------------------------
kill_dev_daemon || { say "a daemon still holds :$OC_DEV_PORT; stop it and re-run"; exit 1; }
rm -rf "$OC_DEV_DIR"
export OPENCHIME_TEST_CAPTURE=synthetic-camera
export OPENCHIME_TEST_AUDIO=synthetic
export OPENCHIME_TEST_VIDEO_CAP_MS=15000
"$DRIVE" launch >/dev/null 2>&1 || { say "launch failed"; exit 1; }
t=0; until snap | grep -q 'authed=1 connected=1'; do
  [ "$t" -ge 200 ] && { say "the client never authenticated on :$OC_DEV_PORT"; exit 1; }
  sleep 0.1; t=$((t + 1))
done
drive channel general

say "== what can be recorded"
drive vm open
sleep 1
screens=$(snap | grep '^vmscreen')
check "a screen is listed" sh -c "printf '%s' \"\$1\" | grep -q 'kind=1 id=\"screen:0\"'" _ "$screens"
check "OpenChime's own window is listed" sh -c "printf '%s' \"\$1\" | grep -q 'kind=2 .*name=\"OpenChime\"'" _ "$screens"

say "== the screen, with the camera boxed top left, and the computer's sound"
drive vm "source screen:0"
drive vm "corner tl"
drive vm "sound on"
check "the choices took" wait_key vmsrc src screen:0
d=$(line vmsrc)
check "top left, sound on" test "$(key_of "$d" corner)" = 3 -a "$(key_of "$d" sound)" = 1
check "the preview is running" wait_key vm= recphase 0
# The box in the preview: colour bars, where the page around it is whatever the
# screen shows. The client reports the video area and the frame's size; the box
# is where oc_inset_rect puts it, and a line across it must cross several
# strongly coloured bars -- which screen content almost never does.
sleep 1
drive shot screenrec_preview
d=$(line vmsrc)
colour=$(python3 - "$LIN_DIR/screenrec_preview.bmp" "$(key_of "$d" vbox)" "$(key_of "$d" frame_w)" "$(key_of "$d" frame_h)" <<'PY'
import sys, colorsys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB')
l, t, r, b = (float(v) for v in sys.argv[2].split(','))
fw, fh = int(sys.argv[3]), int(sys.argv[4])
# The frame letterboxed into the video area, as vm_draw_video draws it.
sc = min((r - l) / fw, (b - t) / fh)
ox, oy = (l + r) / 2 - fw * sc / 2, (t + b) / 2 - fh * sc / 2
# oc_inset_rect, top left, for the 640x360 synthetic camera.
bw = (fw // 5) & ~1; bh = (bw * 360 // 640) & ~1; m = (fw // 50) & ~1
x0, y0 = ox + (m + 4) * sc, oy + (m + bh / 2) * sc
hues = set()
for i in range(0, int((bw - 8) * sc)):
    rr, gg, bb = im.getpixel((int(x0 + i), int(y0)))
    h, s, v = colorsys.rgb_to_hsv(rr / 255, gg / 255, bb / 255)
    if s > 0.5 and v > 0.3: hues.add(int(h * 12))
print(len(hues))
PY
)
check "the camera box, top left, shows the camera's colour bars (${colour:-0} hues)" test "${colour:-0}" -ge 5

say "== recording"
drive vm record
check "the card steps aside for the recording bar" wait_key vmsrc away 1 6000
d=$(line vmsrc)
check "the recording bar is up" test "$(key_of "$d" bar)" = 1
check "and kept out of the capture" test "$(key_of "$d" excluded)" = 1
bar=$(uia_bar)
check "UI Automation finds the bar counting down, with Cancel and Discard ($bar)" \
  sh -c "printf '%s' \"\$1\" | grep -q 'bar=1 text=\"Recording in.*buttons=Cancel,Discard'" _ "$bar"
check "it is recording" wait_key vm= recphase 2 6000
sleep 1
bar=$(uia_bar)
check "then with the time, and Stop and Discard ($bar)" \
  sh -c "printf '%s' \"\$1\" | grep -q 'bar=1 text=\".*Recording  0:0.*buttons=Stop,Discard'" _ "$bar"
sleep 2
press=$(uia_bar -Press Stop)
check "Stop, pressed through UI Automation" sh -c "printf '%s' \"\$1\" | grep -q 'pressed Stop'" _ "$press"
check "the card comes back to review" wait_key vm= recphase 4 10000
d=$(line vmsrc)
check "the bar is gone" test "$(key_of "$d" bar)" = 0 -a "$(key_of "$d" away)" = 0
check "1920x1080 fitted into 720p" test "$(key_of "$d" rec_w)" = 1280 -a "$(key_of "$d" rec_h)" = 720
v=$(line vm=)
ms=$(key_of "$v" take_ms)
check "Stop ended it, well before the cap (${ms} ms of 15 s)" test "${ms:-0}" -ge 1500 -a "${ms:-0}" -le 9000

say "== sending it"
before=$(ls "$OC_DEV_DIR/blobs" 2>/dev/null | wc -l)
drive vm send
t=0; mp4=""
while [ "$t" -lt 300 ] && [ -z "$mp4" ]; do
  for f in $(ls -t "$OC_DEV_DIR/blobs" 2>/dev/null); do
    if ffprobe -v error -show_entries stream=codec_name "$OC_DEV_DIR/blobs/$f" 2>/dev/null | grep -q vp9; then
      mp4="$OC_DEV_DIR/blobs/$f"; break
    fi
  done
  sleep 0.1; t=$((t + 1))
done
check "the daemon stored the video" test -n "$mp4"
if [ -n "$mp4" ]; then
  probe=$(ffprobe -v error -show_entries stream=codec_name,width,height -of compact "$mp4")
  check "VP9 at 1280x720, and Opus ($(printf '%s' "$probe" | tr '\n' ' '))" \
    sh -c "printf '%s' \"\$1\" | grep -q 'codec_name=vp9|width=1280|height=720' && printf '%s' \"\$1\" | grep -q 'codec_name=opus'" _ "$probe"
  tones=$(ffmpeg -v error -i "$mp4" -f s16le -ac 1 -ar 48000 - 2>/dev/null | python3 -c '
import sys, struct, math
d = sys.stdin.buffer.read()
x = struct.unpack("<%dh" % (len(d) // 2), d)[-48000:]
def amp(hz):
    w = 2 * math.pi * hz / 48000; c = 2 * math.cos(w); s1 = s2 = 0.0
    for v in x: s0 = v + c * s1 - s2; s2 = s1; s1 = s0
    return 2 * math.sqrt((s1 - s2 * math.cos(w)) ** 2 + (s2 * math.sin(w)) ** 2) / len(x)
print("%d %d" % (amp(440), amp(660)))')
  a440=${tones% *}; a660=${tones#* }
  check "the microphone (440 Hz: ${a440:-?}) and the computer's sound (660 Hz: ${a660:-?}) are both in it" \
    test "${a440:-0}" -ge 4000 -a "${a660:-0}" -ge 3000
fi

say "== a window, until it closes"
powershell.exe -NoProfile -Command "Start-Process notepad" >/dev/null 2>&1
sleep 2
drive vm open
sleep 1
np=$(snap | grep '^vmscreen' | grep -i 'notepad' | head -1 | sed 's/.* id="\([^"]*\)".*/\1/')
check "the new window is listed (${np:-none})" test -n "$np"
if [ -n "$np" ]; then
  drive vm "source $np"
  drive vm "nocam on"
  drive vm "sound off"
  check "its preview is running" wait_key vm= recphase 0
  drive vm record
  check "recording" wait_key vm= recphase 2 6000
  sleep 1
  powershell.exe -NoProfile -Command "Get-Process notepad -EA SilentlyContinue | Stop-Process -Force" >/dev/null 2>&1
  check "closing the window ends the recording in review" wait_key vm= recphase 4 10000
  check "and says the window went" test "$(key_of "$(line vmsrc)" gone)" = 1
  ms=$(key_of "$(line vm=)" take_ms)
  check "what came before is kept (${ms} ms)" test "${ms:-0}" -ge 500
  drive vm discard
fi

[ "${1:-}" = "--kill" ] && "$DRIVE" kill >/dev/null 2>&1
echo
if [ "$fails" -eq 0 ]; then
  say "gui_screenrec: OK — $checks checks in $((SECONDS - START))s"
else
  say "gui_screenrec: $fails of $checks checks FAILED"
  exit 1
fi
