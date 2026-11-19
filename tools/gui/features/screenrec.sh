#!/usr/bin/env bash
# Screen recording in the Win32 client, end to end (REQ-162, REQ-166, ARCH-110).
#
#   tools/gui/features/screenrec.sh            # run; the client and daemon stop at the end
#   tools/gui/features/screenrec.sh --keep     # ... and leave them running
#
# Real Windows.Graphics.Capture on this machine, a known picture in the camera
# box (OPENCHIME_DEV_CAPTURE=synthetic-camera: colour bars; the screens are
# real), and known sound (OPENCHIME_DEV_AUDIO=synthetic: the microphone a
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
# Its own daemon on its own port, wiped each run, for the reasons tools/gui/features/smoke.sh
# gives. Not in CI: the daemon cannot run on a Windows runner.
#
# platforms: win32 web
# needs: ffmpeg
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
. "$HERE/tools/gui/harness/lib.sh"
gui_begin screenrec 9530 /tmp/oc-screenrec "Screen Fixture" "$@"
OUT="${GUI_DRIVE_OUT:-/tmp/ocshot}"
# The screen the harness records: Windows names its first monitor, a browser
# offers the one its picker chooses.
SCREEN_ID=screen:0; [ "$PLAT" = web ] && SCREEN_ID=display
# Where the daemon keeps what it is sent (daemon/config.c: <data>/blobs).
BLOBS="$OC_TEST_DIR/data/blobs"

export OPENCHIME_DEV_CAPTURE=synthetic-camera
export OPENCHIME_DEV_AUDIO=synthetic
export OPENCHIME_DEV_VIDEO_CAP_MS=15000

line() { grep -m1 "^$1" "$D"; }
k() { field "$D" "^$1" "$2"; }                         # k <line-prefix> <key>
is() { gui_snap && [ "$(k "$1" "$2")" = "$3" ]; }      # is <line-prefix> <key> <want>
num() { [ -n "$1" ] && [ "$1" -eq "$1" ] 2>/dev/null; }
at_least() { gui_snap && num "$(k "$1" "$2")" && [ "$(k "$1" "$2")" -ge "$3" ]; }   # at_least <prefix> <key> <n>

step "what can be recorded"
gui_launch
act channel general
act vm open
listed() { gui_snap && grep -E "^vmscreen\[[0-9]+\] kind=1 id=\"$SCREEN_ID\"" "$D" >/dev/null; }
check "a screen is listed" waitfor 5 listed
own_listed() { grep -E '^vmscreen\[[0-9]+\] kind=2 .*name="OpenChime"' "$D" >/dev/null; }
[ "$PLAT" = win32 ] && check "OpenChime's own window is listed" own_listed

step "the screen, with the camera boxed top left, and the computer's sound"
act vm "source $SCREEN_ID"
act vm "corner tl"
act vm "sound on"
check "the choices took" waitfor 10 is vmsrc src "$SCREEN_ID"
check "top left, sound on" test "$(k vmsrc corner)" = 3 -a "$(k vmsrc sound)" = 1
check "the preview is running" waitfor 10 is vm= recphase 0
# The box in the preview: colour bars, where the page around it is whatever the
# screen shows. The client reports the video area and the frame's size; the box
# is where oc_inset_rect puts it, and a line across it must cross several
# strongly coloured bars -- which screen content almost never does. Read from
# the BMP the client wrote (32-bit BGRA; rows top-down when its height is
# negative), by lib.sh's bmp_px.
hues() {
  local x0 y0 n
  read -r x0 y0 n <<<"$(python3 -I -c '
import sys
l, t, r, b = (float(v) for v in sys.argv[1].split(","))
fw, fh = int(sys.argv[2]), int(sys.argv[3])
# The frame letterboxed into the video area, as vm_draw_video draws it.
sc = min((r - l) / fw, (b - t) / fh)
ox, oy = (l + r) / 2 - fw * sc / 2, (t + b) / 2 - fh * sc / 2
# oc_inset_rect, top left, for the 640x360 synthetic camera.
bw = (fw // 5) & ~1; bh = (bw * 360 // 640) & ~1; m = (fw // 50) & ~1
print(ox + (m + 4) * sc, oy + (m + bh / 2) * sc, max(0, int((bw - 8) * sc)))
' "$(k vmsrc vbox)" "$(k vmsrc frame_w)" "$(k vmsrc frame_h)" 2>/dev/null)"
  [ -n "${n:-}" ] || { echo 0; return; }
  # The colours across the box, and how many strong hues among them.
  bmp_px "$OUT/screenrec_preview.bmp" "$x0" "$y0" "$n" | python3 -I -c '
import sys, colorsys
hues = set()
for c in sys.stdin.read().split():
    h, s, v = colorsys.rgb_to_hsv(int(c[0:2], 16) / 255, int(c[2:4], 16) / 255, int(c[4:6], 16) / 255)
    if s > 0.5 and v > 0.3: hues.add(int(h * 12))
print(len(hues))
'
}
colour=""
bars() { act shot screenrec_preview; gui_snap || return 1; colour="$(hues)"; [ "${colour:-0}" -ge 5 ]; }
check "the camera box, top left, shows the camera's colour bars (five hues or more across it)" waitfor 5 bars
say "  ${colour:-no} hues across the box"

step "recording"
act vm record
if [ "$PLAT" = win32 ]; then
  check "the card steps aside for the recording bar" waitfor 6 is vmsrc away 1
  check "the recording bar is up" test "$(k vmsrc bar)" = 1
  check "and kept out of the capture" test "$(k vmsrc excluded)" = 1
  bar=$(win_uia_recbar)
  counting() { grep 'bar=1 text="Recording in.*buttons=Cancel,Discard' <<<"$bar" >/dev/null; }
  check "UI Automation finds the bar counting down, with Cancel and Discard ($bar)" counting
fi
check "it is recording" waitfor 6 is vm= recphase 2
if [ "$PLAT" = win32 ]; then
  timing() { bar=$(win_uia_recbar); grep 'bar=1 text=".*Recording  0:0.*buttons=Stop,Discard' <<<"$bar" >/dev/null; }
  check "then with the time, and Stop and Discard" waitfor 8 timing
  say "  $bar"
fi
# Two seconds of it, by the recorder's own clock, before Stop.
waitfor 10 at_least vm= rec_elapsed 2000 || gui_abort "the recording did not run two seconds"
if [ "$PLAT" = win32 ]; then
  press=$(win_uia_recbar -Press Stop)
  stopped() { grep 'pressed Stop' <<<"$press" >/dev/null; }
  check "Stop, pressed through UI Automation ($press)" stopped
else
  act vm stop
fi
check "the card comes back to review" waitfor 10 is vm= recphase 4
check "the bar is gone" test "$(k vmsrc bar)" = 0 -a "$(k vmsrc away)" = 0
check "1920x1080 fitted into 720p" test "$(k vmsrc rec_w)" = 1280 -a "$(k vmsrc rec_h)" = 720
ms=$(k vm= take_ms)
check "Stop ended it, well before the cap (${ms} ms of 15 s)" test "${ms:-0}" -ge 1500 -a "${ms:-0}" -le 9000

step "sending it"
act vm send
mp4=""
stored() {
  local f
  for f in $(ls -t "$BLOBS" 2>/dev/null); do
    if ffprobe -v error -show_entries stream=codec_name "$BLOBS/$f" 2>/dev/null | grep vp9 >/dev/null; then
      mp4="$BLOBS/$f"; return 0
    fi
  done
  return 1
}
check "the daemon stored the video" waitfor 30 stored
if [ -n "$mp4" ]; then
  probe=$(ffprobe -v error -show_entries stream=codec_name,width,height -of compact "$mp4")
  vp9_opus() { grep 'codec_name=vp9|width=1280|height=720' <<<"$probe" >/dev/null && grep 'codec_name=opus' <<<"$probe" >/dev/null; }
  check "VP9 at 1280x720, and Opus ($(printf '%s' "$probe" | tr '\n' ' '))" vp9_opus
  tones=$(ffmpeg -v error -i "$mp4" -f s16le -ac 1 -ar 48000 - 2>/dev/null | python3 -I -c '
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

if [ "$PLAT" = win32 ]; then
step "a window, until it closes"
win_open_program notepad
act vm open
# The recorder lists the windows open when it opens: until Notepad's is among
# them, open it again.
np=""
np_listed() {
  gui_snap || return 1
  np=$(grep '^vmscreen' "$D" | grep -i 'notepad' | head -1 | sed 's/.* id="\([^"]*\)".*/\1/')
  [ -n "$np" ] && return 0
  "$DRIVE" vm close >/dev/null 2>&1; "$DRIVE" vm open >/dev/null 2>&1; return 1
}
check "the new window is listed" waitfor 10 np_listed
say "  ${np:-none}"
if [ -n "$np" ]; then
  act vm "source $np"
  act vm "nocam on"
  act vm "sound off"
  check "its preview is running" waitfor 10 is vm= recphase 0
  act vm record
  check "recording" waitfor 6 is vm= recphase 2
  # A second of it, by the recorder's own clock, before the window goes.
  waitfor 10 at_least vm= rec_elapsed 1000 || gui_abort "the window's recording did not run a second"
  win_stop_program notepad
  check "closing the window ends the recording in review" waitfor 10 is vm= recphase 4
  check "and says the window went" test "$(k vmsrc gone)" = 1
  ms=$(k vm= take_ms)
  check "what came before is kept (${ms} ms)" test "${ms:-0}" -ge 500
  act vm discard
fi
fi   # a window: the Windows client's alone; a browser's picker is its own
gui_end
