#!/usr/bin/env bash
# The composer's Preview: the draft shown as the conversation will show it.
#
#   tools/gui/preview.sh          # run, and leave the client running
#   tools/gui/preview.sh --kill   # ... and shut it down afterwards
#
# A draft with bold, a mention, a link and a table is previewed: the dump's
# `cpreview` line says the transcript's renderer laid it out, the table
# included, and the field is hidden. While it is shown, typing edits nothing
# and no frame is written (the dump's frames= count): a custom emoji with no
# image yet is not fetched, and the draft's save waits. Edit brings the field
# back with the caret where it was; previewing again, Enter sends the draft from
# the preview, which then closes. In plain text, the same button is there. Its
# own daemon on its own port, wiped each run.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
. "$HERE/tools/gui/lib.sh"
LIN_DIR='/mnt/c/Windows/Temp/octest'
WIN_DIR='C:\Windows\Temp\octest'
# A file's path as the client under test names it (tools/gui/uploads.sh).
cpath() { if [ "$PLAT" = web ]; then printf '%s/%s' "$LIN_DIR" "$1"; else printf '%s\\%s' "$WIN_DIR" "$1"; fi; }

export OC_TEST_PORT="${OC_TEST_PORT:-9670}"
export OC_TEST_DIR="${OC_TEST_DIR:-/tmp/oc-preview}"
export OC_TEST_WS="${OC_TEST_WS:-Preview Fixture}"

kill_dev_daemon || { say "a daemon still holds :$OC_TEST_PORT; stop it and re-run"; exit 1; }
rm -rf "$OC_TEST_DIR"

D="/tmp/oc-preview-dump.txt"
snap() { "$DRIVE" dump preview 2>/dev/null | tail -n +2 > "$D"; }
pv() { grep -m1 '^cpreview ' "$D" | grep -o "\b$1=[^ ]*" | head -1 | cut -d= -f2; }
ed() { grep -m1 '^ed ' "$D" | grep -o "\b$1=[^ ]*" | head -1 | cut -d= -f2; }
msgs() { grep -m1 "^  ch 1 " "$D" | grep -o 'msgs=[0-9]*' | cut -d= -f2; }
waitfor() {  # waitfor <secs> <command...>
  local secs="$1" t=0; shift
  while [ "$t" -lt $((secs * 4)) ]; do if "$@"; then return 0; fi; sleep 0.25; t=$((t + 1)); done
  return 1
}

say "== a draft with bold, a mention, a link, a custom emoji and a table"
"$DRIVE" launch >/dev/null 2>&1 || { say "launch failed"; exit 1; }
for _ in $(seq 1 80); do snap; grep -q 'authed=1' "$D" && break; sleep 0.25; done
"$DRIVE" size 1200 820 >/dev/null
"$DRIVE" channel 1 >/dev/null
# In the catalogue, its image never drawn here, so not fetched yet.
mkdir -p "$LIN_DIR"
python3 - "$LIN_DIR/em-prv.png" <<'PY'
import struct, sys, zlib
rows = b''.join(b'\0' + bytes.fromhex('f59e0b') * 32 for _ in range(32))
def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
open(sys.argv[1], 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 32, 32, 8, 2, 0, 0, 0))
                              + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))
PY
"$DRIVE" emoji_add "prv $(cpath em-prv.png)" >/dev/null
catalogued() { snap; grep -q '^cemoji=1$' "$D"; }
check "a custom emoji is in the catalogue" waitfor 30 catalogued
"$DRIVE" editor rich >/dev/null
"$DRIVE" type "*Bold* for @alice :prv: see https://example.com/notes " >/dev/null
"$DRIVE" tablenew >/dev/null
"$DRIVE" tablecell 0 0 Item >/dev/null
"$DRIVE" tablecell 0 1 Qty >/dev/null
"$DRIVE" tablecell 1 0 Pear >/dev/null
"$DRIVE" tablecell 1 1 4 >/dev/null
"$DRIVE" tableok >/dev/null
snap
before_msgs="$(msgs)"; before_len="$(ed len)"; before_caret="$(ed caret)"
check "the draft is in the box ($before_len units, caret $before_caret)" [ "${before_len:-0}" -gt 0 ]
check "Preview is on the formatting row, and published" grep -q '^a11yitem composer.preview ' "$D"

say "== Preview shows it rendered"
"$DRIVE" preview >/dev/null
shown() { snap; [ "$(pv on)" = 1 ] && [ "$(pv lines)" -ge 1 ] && [ "$(pv tables)" = 1 ]; }
check "the renderer laid the draft out, its table included" waitfor 5 shown
"$DRIVE" shot preview_on >/dev/null
frames_on="$(pv frames)"
wanted() { snap; [ "$(pv held)" -ge 1 ]; }
check "the custom emoji's image is wanted, and not asked for" waitfor 5 wanted
"$DRIVE" chars "xyz" >/dev/null
"$DRIVE" key back >/dev/null 2>&1
# The save the typing above started comes due, and is held back.
settled() { snap; [ "$(pv draft)" != 1 ]; }
check "the draft's save comes due" waitfor 5 settled
check "typing edits nothing while previewing" [ "$(ed len)" = "$before_len" ]
check "and nothing was posted" [ "$(msgs)" = "$before_msgs" ]
check "and no frame was written ($frames_on, $(pv frames))" [ "$(pv frames)" = "$frames_on" ]

say "== Edit brings the field back as it was"
"$DRIVE" preview >/dev/null
back() { snap;  [ "$(pv on)" = 0 ] && [ "$(ed caret)" = "$before_caret" ] && [ "$(ed len)" = "$before_len" ]; }
check "the field is back, its caret where it was" waitfor 5 back

say "== Enter sends from the preview"
"$DRIVE" preview >/dev/null
on_again() { snap; [ "$(pv on)" = 1 ]; }
check "previewing again" waitfor 5 on_again
"$DRIVE" key enter >/dev/null
sent() { snap; [ "$(msgs)" -gt "${before_msgs:-0}" ] && [ "$(pv on)" = 0 ] && [ "$(ed len)" = 0 ]; }
check "the draft is sent, the box is empty, and the preview has closed" waitfor 10 sent
check "what was sent is the draft" grep -q '^  ch 1 .*prev="[^"]*Bold[^"]*alice' "$D"
check "and the send is in the count: it counts what is written" [ "$(pv frames)" -gt "$frames_on" ]

say "== in plain text, the same button"
"$DRIVE" editor plain >/dev/null
"$DRIVE" type "plain *draft*" >/dev/null
btn() { snap; b="$(pv btn)"; [ -n "$b" ] && [ "$b" != "0,0,0,0" ]; }
check "Preview is drawn" waitfor 5 btn
check "and published" grep -q '^a11yitem composer.preview ' "$D"
press_btn() { local l t r bb; IFS=, read -r l t r bb <<< "$(pv btn)"; "$DRIVE" click $(( (l + r) / 2 )) $(( (t + bb) / 2 )) >/dev/null; }
press_btn
plain_on() { snap; [ "$(pv on)" = 1 ] && [ "$(pv lines)" -ge 1 ]; }
check "its click shows the draft rendered" waitfor 5 plain_on
press_btn
plain_off() { snap; [ "$(pv on)" = 0 ]; }
check "and its click again brings the field back" waitfor 5 plain_off
rm -f "$D" "$LIN_DIR/em-prv.png"

say ""
say "gui_preview: $((checks - fails))/$checks passed in $((SECONDS - START))s"
[ "${1:-}" = "--kill" ] && { "$DRIVE" kill >/dev/null; kill_dev_daemon; }
[ "$fails" = 0 ]
