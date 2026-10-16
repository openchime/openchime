#!/usr/bin/env bash
# Prove the web client is the Win32 client: walk every scene of the audit's
# list (scripts/gui/audit/scenes.tsv) on both, side by side, against one
# fixture, at one size, theme, scale and text size, and compare what each
# painted.
#
#   scripts/gui/parity.sh                 # every scene, in the state matrix below
#   scripts/gui/parity.sh --quick         # light, 96 dpi, text size 1 only
#   scripts/gui/parity.sh --scene home    # one scene
#   scripts/gui/parity.sh --keep          # leave both clients running afterwards
#
# The two clients are one application: the same layout code draws both, so a
# scene's two paint ledgers should hold the same primitives at the same places
# (scripts/gui/audit/parity.py). Where they do not, a platform differs -- text
# measured by another engine, a service the browser lacks, a size the page
# decides -- and the side-by-side picture (<out>/<state>.<scene>.side.png) is
# for the eye, for what no ledger reports.
#
# THE FONTS. The web client asks the browser for Segoe UI first (sdltext's
# canvas backend), as the Win32 client asks DirectWrite, so in a Windows browser
# both draw with one face. The headless Chromium this drives is a Linux one, with
# no Segoe UI, and would fall back to a wider face and report every text as
# different. So it is given the fonts of this machine's own Windows (fontconfig,
# for this run only, OC_TEST_FONTS); nothing is bundled or installed.
#
# Needs the Win32 harness (WSL, scripts/gui/drive.sh) and the web one
# (node, a Chromium, scripts/web/drive.sh).
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# Its own daemon, on its own port, from nothing each run: a scene that changes
# the workspace (the emoji picker reacts) otherwise changes the next run's.
export OC_TEST_PORT="${OC_TEST_PORT:-8453}" OC_TEST_DIR="${OC_TEST_DIR:-/tmp/oc-parity-dev}" OC_TEST_HEALTH_PORT=0
WIN="$HERE/scripts/gui/drive.sh"
WEB="$HERE/scripts/web/drive.sh"
SCENES="$HERE/scripts/gui/audit/scenes.tsv"
PARITY="$HERE/scripts/gui/audit/parity.py"
WIN_DIR='C:\Windows\Temp\octest'
LIN_DIR='/mnt/c/Windows/Temp/octest'
WEB_DIR="$OC_TEST_DIR/web-a"
OUT="${OC_PARITY_OUT:-/tmp/oc-parity}"
FONTS="${OC_TEST_FONTS:-/mnt/c/Windows/Fonts}"

quick=0 keep=0 only=""
while [ $# -gt 0 ]; do
  case "$1" in
    --quick) quick=1 ;;
    --keep)  keep=1 ;;
    --scene) only="${2:-}"; shift ;;
    *) echo "usage: scripts/gui/parity.sh [--quick] [--keep] [--scene <id>]" >&2; exit 2 ;;
  esac
  shift
done
mkdir -p "$OUT"
rm -f "$OUT"/*.tsv "$OUT"/*.bmp "$OUT"/*.png "$OUT"/*.txt "$OUT"/*.pixels 2>/dev/null

# The audit's axes. The browser's scale is its device pixel ratio, so a dpi is a
# ratio there (96 is 1, 144 is 1.5) and the viewport is the same 1024x768 of
# CSS pixels the Win32 client area is set to.
if [ "$quick" = 1 ]; then
  STATES=("light 96 1")
else
  STATES=("light 96 1" "dark 96 1" "light 144 1" "light 96 3")
fi

# --- the browser's fonts: this run's fontconfig, the system's plus Windows' ----
FC="$OUT/fonts.conf"
if [ -d "$FONTS" ]; then
  printf '<?xml version="1.0"?>\n<!DOCTYPE fontconfig SYSTEM "fonts.dtd">\n<fontconfig>\n  <include ignore_missing="yes">/etc/fonts/fonts.conf</include>\n  <dir>%s</dir>\n  <cachedir>%s/fccache</cachedir>\n</fontconfig>\n' \
    "$FONTS" "$OUT" > "$FC"
  export FONTCONFIG_FILE="$FC"
else
  echo "parity: no Windows fonts at $FONTS (OC_TEST_FONTS): text widths will differ by the font" >&2
fi

# The drivers are kept off stdin: the scene loop reads its list from there.
w() { "$WIN" "$@" >/dev/null 2>&1 </dev/null; }
b() { "$WEB" "$@" >/dev/null 2>&1 </dev/null; }

# Poll each client for the scene's settled state (the audit's rule: a verb acks
# when its handler ran, not when what it shows has painted).
settle() {   # settle <win32|web> <key> <want>
  local who="$1" key="$2" want="$3" t=0 got f
  while [ "$t" -lt 4000 ]; do
    if [ "$who" = win32 ]; then w dump settle; f="$LIN_DIR/settle.txt"; else b dump settle; f="$WEB_DIR/settle.txt"; fi
    # The key as a line of its own first (`view=1`): the same name is a field
    # of other lines too (the call line's `view=`), and the first of those is
    # not the scene's.
    got=$( { grep -o "^$key=[^ ]*" "$f"; grep -o "\b$key=[^ ]*" "$f"; } 2>/dev/null | head -1 | cut -d= -f2)
    # `!value`: anything but it -- a dialog is settled when some modal is up.
    case "$want" in
      !*) [ -n "$got" ] && [ "$got" != "${want#!}" ] && return 0 ;;
      *)  [ "$got" = "$want" ] && return 0 ;;
    esac
    sleep 0.05; t=$((t + 50))
  done
  return 1
}

launch_web() {   # launch_web <dpr>
  b kill
  OC_TEST_NO_BUILD=1 OC_TEST_NO_DAEMON=1 OC_TEST_VIEWPORT=1024x768 OC_TEST_DPR="$1" \
    "$WEB" launch >/dev/null 2>&1 || { echo "parity: the web client did not start" >&2; exit 1; }
}

# --- fixture: the audit's ---------------------------------------------------
echo "parity: launching on :$OC_TEST_PORT"
for p in $(pgrep -x openchimed || true); do
  tr '\0' '\n' < "/proc/$p/environ" 2>/dev/null | grep -qx "OPENCHIME_PORT=$OC_TEST_PORT" || continue
  kill "$p"; for _ in $(seq 1 120); do kill -0 "$p" 2>/dev/null || break; sleep 0.25; done
done
rm -rf "$OC_TEST_DIR/data"
w kill; b kill   # a client left running is on whatever daemon it was
"$WIN" launch >/dev/null 2>&1 || { echo "parity: the Win32 client did not start" >&2; exit 1; }
# Built every run, incrementally: one left from before a protocol change fails
# its handshake, and the fixture is then a workspace with no messages in it.
make -C "$HERE" build/demo_client >/dev/null 2>&1 || { echo "parity: build/demo_client did not build" >&2; exit 1; }
for c in engineering design releases; do w mkchan "$c 1"; done
sleep 1
send() { "$HERE/build/demo_client" 127.0.0.1 "$OC_TEST_PORT" "$1" pw send "$2" "$3" >/dev/null 2>&1 || { echo "parity: the fixture message from $1 was not sent" >&2; exit 1; }; }
send alice 1 "Welcome to the audit fixture."
send bob   1 "A reply, so a row has a neighbour."
send carol 1 "And a third, so grouping has something to group."
send bob   2 "A message long enough to wrap at every text size in the matrix, which is the state that finds a box somebody sized for one of them."
send alice 2 "Short."
send carol 3 "One more channel with content."
w groupdm bob,carol
w groupdm bob
sleep 1

same=0 differ=0 failed=0
REPORT="$OUT/report.txt"; : > "$REPORT"
for state in "${STATES[@]}"; do
  set -- $state
  theme="$1"; dpi="$2"; ts="$3"
  dpr=$(python3 -c "print($dpi / 96)")
  launch_web "$dpr"
  # The Win32 client restarted too, as the web one is: a pass that inherits the
  # last one's state (an Admin tab, an open picker) differs from a fresh page
  # by what the previous pass left, not by platform.
  [ "$state" = "${STATES[0]}" ] || "$WIN" launch >/dev/null 2>&1 </dev/null || { echo "parity: the Win32 client did not restart" >&2; exit 1; }
  w dpi "$dpi"; w size "$(( 1024 * dpi / 96 ))" "$(( 768 * dpi / 96 ))"
  for c in w b; do $c theme "$([ "$theme" = dark ] && echo 0 || echo 1)"; $c textsize "$ts"; done
  sleep 0.5
  tag="$theme-${dpi}dpi-ts$ts"

  while IFS=$'\t' read -r id verbs key want; do
    case "$id" in ''|\#*) continue ;; esac
    [ -n "$only" ] && [ "$only" != "$id" ] && continue
    ok=1
    for who in win32 web; do
      if [ "$who" = win32 ]; then d="$WIN"; else d="$WEB"; fi
      "$d" key esc >/dev/null 2>&1 </dev/null
      # A dialog the last scene left open runs its own loop, and unwinds only
      # after the Esc is answered: opening the next one before then is refused
      # (one form at a time), so wait for the modal to be gone.
      settle "$who" modal none || "$d" key esc >/dev/null 2>&1 </dev/null
      "$d" view home >/dev/null 2>&1 </dev/null
      IFS=';' read -ra steps <<< "$verbs"
      for v in "${steps[@]}"; do
        "$d" $v >/dev/null 2>&1 </dev/null || { echo "  FAIL $tag/$id ($who): no answer to '$v'"; ok=0; break; }
      done
      [ "$ok" = 1 ] && ! settle "$who" "$key" "$want" && { echo "  FAIL $tag/$id ($who): never reached $key=$want"; ok=0; }
      # And the open conversation's members, where it has any (a scene with
      # no conversation never has them, so this waits and goes on).
      [ "$ok" = 1 ] && settle "$who" chanmem_ready 1
      [ "$ok" = 1 ] || break
    done
    [ "$ok" = 1 ] || { failed=$((failed + 1)); continue; }

    # The pointer parked on the rail's empty stretch (between Admin and New) on
    # both: the Win32 window is on a real desktop, where a mouse resting over it
    # draws a hover the browser's never has.
    w move "$(( 35 * dpi / 96 ))" "$(( 450 * dpi / 96 ))"; b move 35 450
    w "ledger ${WIN_DIR}\\par.tsv"; w shot par
    cp "$LIN_DIR/par.tsv" "$OUT/$tag.$id.win32.tsv" 2>/dev/null
    cp "/tmp/ocshot/par.bmp" "$OUT/$tag.$id.win32.bmp" 2>/dev/null
    b ledger par; b shot par
    cp "$WEB_DIR/par.tsv" "$OUT/$tag.$id.web.tsv" 2>/dev/null
    cp "$WEB_DIR/par.bmp" "$OUT/$tag.$id.web.bmp" 2>/dev/null

    python3 - "$OUT/$tag.$id" <<'PY' 2>/dev/null
import sys
from PIL import Image, ImageChops
p = sys.argv[1]
a, c = Image.open(p + ".win32.bmp").convert("RGB"), Image.open(p + ".web.bmp").convert("RGB")
from PIL import ImageFilter
diff = ImageChops.difference(a.resize(c.size) if a.size != c.size else a, c).point(lambda v: 255 if v > 24 else 0)
s = Image.new("RGB", (a.width + c.width + diff.width + 20, max(a.height, c.height)), (255, 0, 255))
s.paste(a, (0, 0)); s.paste(c, (a.width + 10, 0)); s.paste(diff, (a.width + c.width + 20, 0))
s.save(p + ".side.png")
# What differs to the eye, which no ledger row says (a glyph the other font
# draws, a sign clipped off): pixels unlike anything within a pixel of them
# on the other side, so antialiasing a fraction apart does not count.
if a.size == c.size:
    la, lc = a.convert("L"), c.convert("L")
    d = ImageChops.difference(la, lc)
    lo = ImageChops.difference(la, lc.filter(ImageFilter.MinFilter(3)))
    hi = ImageChops.difference(la, lc.filter(ImageFilter.MaxFilter(3)))
    m = ImageChops.darker(ImageChops.darker(d, lo), hi).point(lambda v: 255 if v > 60 else 0)
    open(p + ".pixels", "w").write("%d %s\n" % (m.histogram()[255], m.getbbox()))
PY
    if res=$(python3 "$PARITY" "$OUT/$tag.$id.win32.tsv" "$OUT/$tag.$id.web.tsv" 2>&1); then
      same=$((same + 1))
      echo "$tag/$id: same" >> "$REPORT"
    else
      differ=$((differ + 1))
      { echo "$tag/$id:"; echo "$res" | sed 's/^/  /'; } >> "$REPORT"
      echo "  $tag/$id: $(echo "$res" | head -1 | sed 's/^parity: //')"
    fi
  done < "$SCENES"
done

# The pictures, by how much differs to the eye, most first: the ones to look at.
{ echo; echo "pixels differing (look at these pictures first):"
  for f in "$OUT"/*.pixels; do [ -f "$f" ] && echo "  $(cut -d' ' -f1 "$f")  $(basename "$f" .pixels)"; done | sort -rn | head -15; } >> "$REPORT"

[ "$keep" = 1 ] || { w kill; b kill; }
echo
echo "parity: $((same + differ)) scenes compared, $same the same, $differ different, $failed not reached"
echo "report in $REPORT; side-by-side pictures in $OUT/*.side.png"
[ "$differ" = 0 ] && [ "$failed" = 0 ]
