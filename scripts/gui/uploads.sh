#!/usr/bin/env bash
# The upload tray in the Win32 client's message box (REQ-140).
#
#   scripts/gui/uploads.sh          # run, and leave the client running
#   scripts/gui/uploads.sh --kill   # ... and shut it down afterwards
#
# Files wait in the box as chips until Send, which posts them with the text as
# one message. It asserts that
#
#   - attached files appear as chips, an image with its picture, each one's
#     button published to assistive technology, and nothing is uploaded yet;
#   - a chip's x takes it out, before anything has moved;
#   - Send posts what is left and the text as ONE message, and the tray empties;
#   - a file that cannot be read stops the post: the chip comes back marked, the
#     text comes back into the box, and nothing is posted;
#   - Ctrl+V attaches what is not text: a copied file as itself, a copied image
#     as a PNG chip with its picture, while text still pastes as text.
#
# Its own daemon on its own port. The last step sends a large file so the bars
# can be seen moving in the screenshot; that shot is for looking at, and asserts
# nothing -- when a local upload finishes is not something to test against.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
. "$HERE/scripts/gui/lib.sh"
LIN_DIR='/mnt/c/Windows/Temp/octest'
WIN_DIR='C:\Windows\Temp\octest'
# A file's path as the client under test names it: the Windows share for the
# Windows client, this machine's path for the web client (the driver carries
# it onto the page).
cpath() { if [ "$PLAT" = web ]; then printf '%s/%s' "$LIN_DIR" "$1"; else printf '%s\\%s' "$WIN_DIR" "$1"; fi; }

export OC_TEST_PORT="${OC_TEST_PORT:-9630}"
export OC_TEST_DIR="${OC_TEST_DIR:-/tmp/oc-uploads}"
export OC_TEST_WS="${OC_TEST_WS:-Uploads Fixture}"

kill_dev_daemon || { say "a daemon still holds :$OC_TEST_PORT; stop it and re-run"; exit 1; }
rm -rf "$OC_TEST_DIR"

D="/tmp/oc-uploads-dump.txt"
snap() { "$DRIVE" dump uploads 2>/dev/null | tail -n +2 > "$D"; }
tray() { snap; grep -m1 '^ftray ' "$D" | grep -o "\b$1=[^ ]*" | head -1 | cut -d= -f2; }
chip() { grep "^  fchip $1 " "$D" | grep -o "\b$2=[^ ]*" | head -1 | cut -d= -f2 | tr -d '"'; }
waitfor() {  # waitfor <secs> <command...>
  local secs="$1" t=0; shift
  while [ "$t" -lt $((secs * 4)) ]; do if "$@"; then return 0; fi; sleep 0.25; t=$((t + 1)); done
  return 1
}
click_x() {  # click chip $1's x
  local l t r b; IFS=, read -r l t r b <<< "$(chip "$1" x)"
  [ -n "${b:-}" ] && [ "${r%.*}" -gt 0 ] || return 1
  "$DRIVE" click $(( (l + r) / 2 )) $(( (t + b) / 2 )) >/dev/null
}
# A 48x32 PNG in two colours, so the chip has a real picture to show.
make_png() {
  python3 - "$1" <<'PY'
import struct, sys, zlib
w, h = 48, 32
rows = b''.join(b'\0' + b''.join((b'\x3b\x82\xf6' if x < w // 2 else b'\xf5\x9e\x0b') for x in range(w)) for _ in range(h))
def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
open(sys.argv[1], 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                              + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))
PY
}

say "== three files wait in the box"
"$DRIVE" launch >/dev/null 2>&1 || { say "launch failed"; exit 1; }
for _ in $(seq 1 80); do snap; grep -q 'authed=1' "$D" && break; sleep 0.25; done
"$DRIVE" size 1200 820 >/dev/null
"$DRIVE" channel 1 >/dev/null
make_png "$LIN_DIR/up-picture.png"
printf 'the notes\n' > "$LIN_DIR/up-notes.txt"
head -c 300000 /dev/urandom > "$LIN_DIR/up-data.bin"
for f in up-picture.png up-notes.txt up-data.bin; do "$DRIVE" attach "$(cpath $f)" >/dev/null; done
three() { [ "$(tray here)" = 3 ] && [ "$(tray height)" != 0 ]; }
check "three chips, and the box grew to hold them" waitfor 5 three
check "the picture shows itself" bash -c "grep -q '^  fchip 0 name=\"up-picture.png\" here=1 pic=1 ' $D"
waiting() { [ "$(chip 0 state)" = -1 ] && [ "$(chip 1 state)" = -1 ] && [ "$(chip 2 state)" = -1 ]; }
check "nothing has moved yet" waiting
# Published from the paint that places the buttons, so after the next frame.
published() { snap; grep -q '^a11yitem composer.file.0 ' "$D" && grep -q '^a11yitem composer.file.1 ' "$D" &&
              grep -q '^a11yitem composer.file.2 ' "$D"; }
check "each chip's button is published to assistive technology" waitfor 5 published
"$DRIVE" shot uploads_waiting >/dev/null

say "== a chip's x takes it out"
click_x 1
two_left() { [ "$(tray here)" = 2 ] && [ "$(chip 0 name)" = up-picture.png ] && [ "$(chip 1 name)" = up-data.bin ]; }
check "the notes are gone, the others stay" waitfor 5 two_left

say "== Send posts them with the text as one message"
"$DRIVE" send "two files" >/dev/null
landed() { [ "$(tray here)" = 0 ] && [ "$(tray posts)" = 0 ] && [ "$(tray height)" = 0 ]; }
check "the tray empties once they land" waitfor 20 landed
one_msg() {
  snap
  local a b
  a=$(grep 'attach msg=.* name="up-picture.png"' "$D" | grep -o 'msg=[0-9]*' | head -1)
  b=$(grep 'attach msg=.* name="up-data.bin"' "$D" | grep -o 'msg=[0-9]*' | head -1)
  [ -n "$a" ] && [ "$a" = "$b" ] && ! grep -q 'name="up-notes.txt"' "$D"
}
check "both files are on one message, and the removed one on none" waitfor 10 one_msg
check "...which carries the text" bash -c "grep -q '^  ch 1 .*prev=\"two files\"' $D"

if [ "$PLAT" = win32 ]; then   # a browser copies the file when it is attached: it cannot go away under the post
say "== a file that cannot be read stops the post"
printf 'soon gone\n' > "$LIN_DIR/up-gone.txt"
"$DRIVE" attach "$(cpath up-gone.txt)" >/dev/null
rm -f "$LIN_DIR/up-gone.txt"
"$DRIVE" send "comes back" >/dev/null
returned() { [ "$(tray here)" = 1 ] && [ "$(chip 0 failed)" = 1 ] && [ "$(chip 0 state)" = -1 ] && [ "$(tray posts)" = 0 ]; }
check "the chip is back, marked" waitfor 10 returned
check "the text is back in the box" bash -c "grep -Eq '^ed len=([1-9][0-9]*) ' $D"
check "its button says it was not sent" bash -c "grep -q '^a11yitem composer.file.0 ' $D"
"$DRIVE" shot uploads_failed >/dev/null
click_x 0
"$DRIVE" send "after the failure" >/dev/null
anchored() { snap; grep -q '^  ch 1 .*prev="after the failure"' "$D"; }
check "nothing was posted for it (the next message is the last)" waitfor 10 anchored
check "...and no such file was shared" bash -c "! grep -q 'name=\"up-gone.txt\"' $D"
fi

if [ "$PLAT" = win32 ]; then   # the clipboard is set from PowerShell: Windows alone
say "== pasting attaches what is not text"
# The Windows clipboard, set the way other programs set it: Explorer's copy puts a
# file list on it (CF_HDROP); an image copied out of a program puts a bitmap.
clip() { powershell.exe -NoProfile -STA -Command "Add-Type -AssemblyName System.Windows.Forms,System.Drawing; $1" >/dev/null 2>&1; }
clip "\$c = New-Object Collections.Specialized.StringCollection; [void]\$c.Add('$WIN_DIR\\up-picture.png'); [Windows.Forms.Clipboard]::SetFileDropList(\$c)"
"$DRIVE" key ctrl+v >/dev/null
pasted_file() { [ "$(tray here)" = 1 ] && [ "$(chip 0 name)" = up-picture.png ]; }
check "a copied file pastes as itself" waitfor 5 pasted_file
clip "[Windows.Forms.Clipboard]::SetImage([Drawing.Image]::FromFile('$WIN_DIR\\up-picture.png'))"
"$DRIVE" key ctrl+v >/dev/null
pasted_image() { [ "$(tray here)" = 2 ] && chip 1 name | grep -Eq '^pasted-image-[0-9]{8}-[0-9]{6}\.png$' && [ "$(chip 1 pic)" = 1 ]; }
check "a copied image pastes as a PNG with its picture" waitfor 5 pasted_image
"$DRIVE" send "pasted" >/dev/null
check "the pasted ones land" waitfor 20 landed
pasted_msg() { snap; grep -q 'attach msg=.* name="pasted-image-' "$D"; }
check "...the image under its pasted name" waitfor 10 pasted_msg
clip "[Windows.Forms.Clipboard]::SetText('plain words')"
"$DRIVE" key ctrl+v >/dev/null
as_text() { [ "$(tray here)" = 0 ] && grep -Eq '^ed len=11 ' "$D"; }
check "text still pastes as text, and adds no chip" waitfor 5 as_text
"$DRIVE" key ctrl+a >/dev/null; "$DRIVE" key delete >/dev/null
clip "[Windows.Forms.Clipboard]::Clear()"
fi

say "== a large one, to watch (not asserted)"
head -c $((120 * 1024 * 1024)) /dev/urandom > "$LIN_DIR/up-large.bin"
"$DRIVE" attach "$(cpath up-picture.png)" >/dev/null
"$DRIVE" attach "$(cpath up-large.bin)" >/dev/null
"$DRIVE" send "a large one" >/dev/null
sleep 0.6; "$DRIVE" shot uploads_moving >/dev/null
check "the large one lands" waitfor 120 landed
rm -f "$D" "$LIN_DIR"/up-*.png "$LIN_DIR"/up-*.txt "$LIN_DIR"/up-*.bin

say ""
say "gui_uploads: $((checks - fails))/$checks passed in $((SECONDS - START))s"
[ "${1:-}" = "--kill" ] && { "$DRIVE" kill >/dev/null; kill_dev_daemon; }
[ "$fails" = 0 ]
