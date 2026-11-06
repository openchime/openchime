#!/usr/bin/env bash
# The expanded image view pages through the conversation's media.
#
#   tools/gui/lightbox.sh          # run, and leave the client running
#   tools/gui/lightbox.sh --kill   # ... and shut it down afterwards
#
# Three images are posted with a text file between them. A click opens one;
# the first is 1 of 3 with no previous arrow. Right moves to the next image,
# the text file never in the sequence; Left comes back; the next arrow does
# what Right does; at the last, Right changes nothing and there is no next
# arrow; Esc closes the view. Its own daemon on its own port, wiped each run.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
. "$HERE/tools/gui/lib.sh"
LIN_DIR='/mnt/c/Windows/Temp/octest'
WIN_DIR='C:\Windows\Temp\octest'
# A file's path as the client under test names it (tools/gui/uploads.sh).
cpath() { if [ "$PLAT" = web ]; then printf '%s/%s' "$LIN_DIR" "$1"; else printf '%s\\%s' "$WIN_DIR" "$1"; fi; }

export OC_TEST_PORT="${OC_TEST_PORT:-9710}"
export OC_TEST_DIR="${OC_TEST_DIR:-/tmp/oc-lightbox}"
export OC_TEST_WS="${OC_TEST_WS:-Lightbox Fixture}"

kill_dev_daemon || { say "a daemon still holds :$OC_TEST_PORT; stop it and re-run"; exit 1; }
rm -rf "$OC_TEST_DIR"

D="/tmp/oc-lightbox-dump.txt"
snap() { "$DRIVE" dump lightbox 2>/dev/null | tail -n +2 > "$D"; }
lb() { grep -m1 '^lightbox=' "$D" | grep -o "\b$1=[^ ]*" | head -1 | cut -d= -f2; }
waitfor() {  # waitfor <secs> <command...>
  local secs="$1" t=0; shift
  while [ "$t" -lt $((secs * 4)) ]; do if "$@"; then return 0; fi; sleep 0.25; t=$((t + 1)); done
  return 1
}
make_png() {  # a 64x48 PNG in one colour
  python3 - "$1" "$2" <<'PY'
import struct, sys, zlib
w, h = 64, 48
px = bytes.fromhex(sys.argv[2])
rows = b''.join(b'\0' + px * w for _ in range(h))
def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
open(sys.argv[1], 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                              + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))
PY
}

say "== three images, a text file among them"
"$DRIVE" launch >/dev/null 2>&1 || { say "launch failed"; exit 1; }
for _ in $(seq 1 80); do snap; grep -q 'authed=1' "$D" && break; sleep 0.25; done
"$DRIVE" size 1200 900 >/dev/null
"$DRIVE" channel 1 >/dev/null
mkdir -p "$LIN_DIR"
make_png "$LIN_DIR/lb-one.png" f59e0b
make_png "$LIN_DIR/lb-two.png" 22c55e
make_png "$LIN_DIR/lb-three.png" 3b82f6
printf 'notes\n' > "$LIN_DIR/lb-notes.txt"
for f in lb-one.png lb-notes.txt lb-two.png lb-three.png; do
  "$DRIVE" upload "$(cpath $f)" >/dev/null; sleep 1.2
done
three() { snap; [ "$(lb thumb_hits)" -ge 3 ]; }
check "the three images are drawn" waitfor 30 three

say "== opened, and paged"
# The earliest image wholly below the conversation's header: its hit-box,
# "  thumbhit[i] id=N L,T WxH", with T past 100.
read -r x y w h <<<"$(grep '^  thumbhit\[' "$D" | sed -E 's/.* ([0-9.-]+),([0-9.-]+) ([0-9.]+)x([0-9.]+).*/\1 \2 \3 \4/' |
                      awk '$2 > 100' | sort -k2 -n | head -1)"
[ -n "${h:-}" ] && "$DRIVE" click $(( ${x%.*} + ${w%.*} / 2 )) $(( ${y%.*} + ${h%.*} / 2 )) >/dev/null
opened() { snap; [ "$(lb lightbox)" != 0 ] && [ "$(lb lightbox_pos)" != 0/0 ]; }
check "a click on an image opens the view" waitfor 5 opened
"$DRIVE" key left >/dev/null; "$DRIVE" key left >/dev/null
at1() { snap; [ "$(lb lightbox_pos)" = 1/3 ] && [ "$(lb prev)" = 0 ] && [ "$(lb next)" = 1 ]; }
check "Left to the first: 1 of 3, with only a next arrow" waitfor 5 at1
ONE="$(lb lightbox)"
"$DRIVE" shot lightbox_first >/dev/null
"$DRIVE" key right >/dev/null
at2() { snap; [ "$(lb lightbox_pos)" = 2/3 ] && [ "$(lb lightbox)" != "$ONE" ]; }
check "Right moves to the next image, past the text file" waitfor 5 at2
"$DRIVE" key left >/dev/null
back1() { snap; [ "$(lb lightbox_pos)" = 1/3 ] && [ "$(lb lightbox)" = "$ONE" ]; }
check "Left comes back" waitfor 5 back1
IFS=, read -r l t r b <<<"$(lb next_r)"
"$DRIVE" click $(( (${l%.*} + ${r%.*}) / 2 )) $(( (${t%.*} + ${b%.*}) / 2 )) >/dev/null
check "the next arrow does what Right does" waitfor 5 at2
"$DRIVE" key right >/dev/null
at3() { snap; [ "$(lb lightbox_pos)" = 3/3 ] && [ "$(lb next)" = 0 ] && [ "$(lb prev)" = 1 ]; }
check "the last is 3 of 3, with no next arrow" waitfor 5 at3
LAST="$(lb lightbox)"
"$DRIVE" key right >/dev/null
sleep 0.5; snap
check "Right at the last changes nothing" [ "$(lb lightbox)" = "$LAST" ]
"$DRIVE" shot lightbox_last >/dev/null
"$DRIVE" key esc >/dev/null
closed() { snap; [ "$(lb lightbox)" = 0 ]; }
check "Esc closes the view" waitfor 5 closed
rm -f "$D" "$LIN_DIR"/lb-*

say ""
say "gui_lightbox: $((checks - fails))/$checks passed in $((SECONDS - START))s"
[ "${1:-}" = "--kill" ] && { "$DRIVE" kill >/dev/null; kill_dev_daemon; }
[ "$fails" = 0 ]
