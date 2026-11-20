#!/usr/bin/env bash
# The expanded image view pages through the conversation's media.
#
#   tools/gui/features/lightbox.sh          # run; the client and daemon stop at the end
#   tools/gui/features/lightbox.sh --keep   # ... and leave them running
#
# Three images are posted with a text file between them. A click opens one;
# the first is 1 of 3 with no previous arrow. Right moves to the next image,
# the text file never in the sequence; Left comes back; the next arrow does
# what Right does; at the last, Right changes nothing and there is no next
# arrow; Esc closes the view. Its own daemon on its own port, wiped each run.
#
# platforms: win32 web
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
. "$HERE/tools/gui/harness/lib.sh"
gui_begin lightbox 9710 /tmp/oc-lightbox "Lightbox Fixture" "$@"

lb() { field "$D" '^lightbox=' "$1"; }

step "three images, a text file among them"
gui_launch
"$DRIVE" size 1200 900 >/dev/null
"$DRIVE" channel 1 >/dev/null || gui_abort "the client did not open the channel"
mkdir -p "$LIN_DIR"
make_png "$LIN_DIR/lb-one.png" 64x48 f59e0b
make_png "$LIN_DIR/lb-two.png" 64x48 22c55e
make_png "$LIN_DIR/lb-three.png" 64x48 3b82f6
printf 'notes\n' > "$LIN_DIR/lb-notes.txt"
# One at a time, each back from the daemon as a message's attachment before the
# next, so the conversation holds them in this order.
for f in lb-one.png lb-notes.txt lb-two.png lb-three.png; do
  upload_stored "$f"
done
three() { gui_snap; [ "$(lb thumb_hits)" -ge 3 ] 2>/dev/null; }
check "the three images are drawn" waitfor 30 three

step "opened, and paged"
# The earliest image wholly below the conversation's header: its hit-box,
# "  thumbhit[i] id=N L,T WxH", with T past 100.
read -r x y w h <<<"$(grep '^  thumbhit\[' "$D" | sed -E 's/.* ([0-9.-]+),([0-9.-]+) ([0-9.]+)x([0-9.]+).*/\1 \2 \3 \4/' |
                      awk '$2 > 100' | sort -k2 -n | head -1)"
[ -n "${h:-}" ] || gui_abort "no image is drawn below the header to click"
"$DRIVE" click $(( ${x%.*} + ${w%.*} / 2 )) $(( ${y%.*} + ${h%.*} / 2 )) >/dev/null
opened() { gui_snap; [ "$(lb lightbox)" != 0 ] && [ "$(lb lightbox_pos)" != 0/0 ]; }
check "a click on an image opens the view" waitfor 5 opened
"$DRIVE" key left >/dev/null; "$DRIVE" key left >/dev/null
at1() { gui_snap; [ "$(lb lightbox_pos)" = 1/3 ] && [ "$(lb prev)" = 0 ] && [ "$(lb next)" = 1 ]; }
check "Left to the first: 1 of 3, with only a next arrow" waitfor 5 at1
ONE="$(lb lightbox)"
"$DRIVE" shot lightbox_first >/dev/null
"$DRIVE" key right >/dev/null
at2() { gui_snap; [ "$(lb lightbox_pos)" = 2/3 ] && [ "$(lb lightbox)" != "$ONE" ]; }
check "Right moves to the next image, past the text file" waitfor 5 at2
"$DRIVE" key left >/dev/null
back1() { gui_snap; [ "$(lb lightbox_pos)" = 1/3 ] && [ "$(lb lightbox)" = "$ONE" ]; }
check "Left comes back" waitfor 5 back1
click_rect "$(lb next_r)"
check "the next arrow does what Right does" waitfor 5 at2
"$DRIVE" key right >/dev/null
at3() { gui_snap; [ "$(lb lightbox_pos)" = 3/3 ] && [ "$(lb next)" = 0 ] && [ "$(lb prev)" = 1 ]; }
check "the last is 3 of 3, with no next arrow" waitfor 5 at3
LAST="$(lb lightbox)"
"$DRIVE" key right >/dev/null || gui_abort "the client did not answer key right"
# A key is handled before its ack: the look after it is the view after Right.
gui_snap
unchanged() { [ -n "$LAST" ] && [ "$(lb lightbox)" = "$LAST" ] && [ "$(lb lightbox_pos)" = 3/3 ]; }
check "Right at the last changes nothing" unchanged
"$DRIVE" shot lightbox_last >/dev/null
"$DRIVE" key esc >/dev/null
closed() { gui_snap; [ "$(lb lightbox)" = 0 ]; }
check "Esc closes the view" waitfor 5 closed
rm -f "$LIN_DIR"/lb-*
gui_end
