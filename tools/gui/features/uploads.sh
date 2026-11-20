#!/usr/bin/env bash
# The upload tray in the Win32 client's message box (REQ-140).
#
#   tools/gui/features/uploads.sh          # run; the client and daemon stop at the end
#   tools/gui/features/uploads.sh --keep   # ... and leave them running
#
# Files wait in the box as chips until Send, which posts them with the text as
# one message. It asserts that
#
#   - in the Windows client, the real file dialog "+" opens is driven: the hook
#     answers while it is open (dump `picker=1`), Cancel closes it adding
#     nothing, and a file picked in it comes back as a chip;
#   - attached files appear as chips, an image with its picture, each one's
#     button published to assistive technology, and nothing is uploaded yet;
#   - a chip's x takes it out, before anything has moved;
#   - Send posts what is left and the text as ONE message, and the tray empties;
#   - a file that cannot be read stops the post: the chip comes back marked, the
#     text comes back into the box, and nothing is posted;
#   - Ctrl+V attaches what is not text: a copied file as itself, a copied image
#     as a PNG chip with its picture, while text still pastes as text. In the
#     browser, what was copied outside the client: an image, then text, through
#     the browser's own paste; and Ctrl+C puts the box's text, and a message's
#     Copy text puts the message, on the browser's clipboard.
#
# Its own daemon on its own port. The last step sends a large file so the bars
# can be seen moving in the screenshot; that shot is for looking at, and asserts
# nothing -- when a local upload finishes is not something to test against.
#
# platforms: win32 web
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
. "$HERE/tools/gui/harness/lib.sh"
gui_begin uploads 9630 /tmp/oc-uploads "Uploads Fixture" "$@"

tray() { gui_snap; field "$D" '^ftray ' "$1"; }
chip() { field "$D" "^  fchip $1 " "$2"; }
click_x() { click_rect "$(chip "$1" x)"; }   # click chip $1's x

step "three files wait in the box"
gui_launch
"$DRIVE" size 1200 820 >/dev/null
"$DRIVE" channel 1 >/dev/null || gui_abort "the client did not open the channel"
# 48x32 in two colours, so the chip has a real picture to show.
make_png "$LIN_DIR/up-picture.png" 48x32 3b82f6 f59e0b
printf 'the notes\n' > "$LIN_DIR/up-notes.txt"
head -c 300000 /dev/urandom > "$LIN_DIR/up-data.bin"
if [ "$PLAT" = win32 ]; then   # the browser's picker is the page's, not a dialog on the client's thread
step "the file dialog, driven: cancelled, then a file picked"
# "+" opens the system's own Open dialog, whose modal loop holds the client's
# thread; the hook is answered from inside it all the same. The click is
# acked before it is made (clickform), since the dialog does not return.
plus() {  # click the composer's "+", by the rect it publishes (lib.sh's aid_xy)
  local xy
  gui_snap; xy="$(aid_xy "$D" composer.attach)" || return 1
  "$DRIVE" clickform $xy >/dev/null
}
picker_is() { gui_snap; grep -q "^modal=[^ ]* picker=$1 " "$D"; }
no_dialog() { [ "$("$DRIVE" picker cancel 2>/dev/null)" = "ack: none" ]; }
empty_tray() { [ "$(tray here)" = 0 ]; }
check "with no dialog open, the picker verb changes nothing" no_dialog
check "\"+\" opens the file dialog" plus
check "the hook answers while it is open, and the dump says so" waitfor 10 picker_is 1
check "a screenshot is taken with it open" "$DRIVE" shot uploads_picker
"$DRIVE" picker cancel >/dev/null
check "cancelled, it closes" waitfor 10 picker_is 0
check "...and adds nothing" empty_tray
check "\"+\" opens it again" plus
waitfor 10 picker_is 1 || gui_abort "the file dialog did not open again"
"$DRIVE" picker pick "$(cpath up-notes.txt)" >/dev/null
picked() { [ "$(tray here)" = 1 ] && [ "$(chip 0 name)" = up-notes.txt ]; }
check "a file picked in it comes back as a chip" waitfor 10 picked
check "...and the dialog is gone" picker_is 0
click_x 0
waitfor 5 empty_tray || gui_abort "the picked file's chip did not come out"
fi
for f in up-picture.png up-notes.txt up-data.bin; do
  "$DRIVE" attach "$(cpath $f)" >/dev/null || gui_abort "the client did not take attach $f"
done
three() { [ "$(tray here)" = 3 ] && [ "$(tray height)" != 0 ]; }
check "three chips, and the box grew to hold them" waitfor 5 three
check "the picture shows itself" grep -q '^  fchip 0 name="up-picture.png" here=1 pic=1 ' "$D"
waiting() { [ "$(chip 0 state)" = -1 ] && [ "$(chip 1 state)" = -1 ] && [ "$(chip 2 state)" = -1 ]; }
check "nothing has moved yet" waiting
# Published from the paint that places the buttons, so after the next frame.
published() { gui_snap; grep -q '^a11yitem composer.file.0 ' "$D" && grep -q '^a11yitem composer.file.1 ' "$D" &&
              grep -q '^a11yitem composer.file.2 ' "$D"; }
check "each chip's button is published to assistive technology" waitfor 5 published
"$DRIVE" shot uploads_waiting >/dev/null

step "a chip's x takes it out"
click_x 1
two_left() { [ "$(tray here)" = 2 ] && [ "$(chip 0 name)" = up-picture.png ] && [ "$(chip 1 name)" = up-data.bin ]; }
check "the notes are gone, the others stay" waitfor 5 two_left

step "Send posts them with the text as one message"
"$DRIVE" send "two files" >/dev/null || gui_abort "the client did not send"
landed() { [ "$(tray here)" = 0 ] && [ "$(tray posts)" = 0 ] && [ "$(tray height)" = 0 ]; }
check "the tray empties once they land" waitfor 20 landed
one_msg() {
  gui_snap
  local a b
  a=$(grep 'attach msg=.* name="up-picture.png"' "$D" | grep -o 'msg=[0-9]*' | head -1)
  b=$(grep 'attach msg=.* name="up-data.bin"' "$D" | grep -o 'msg=[0-9]*' | head -1)
  [ -n "$a" ] && [ "$a" = "$b" ] && ! grep -q 'name="up-notes.txt"' "$D"
}
check "both files are on one message, and the removed one on none" waitfor 10 one_msg
check "...which carries the text" grep -q '^  ch 1 .*prev="two files"' "$D"

if [ "$PLAT" = win32 ]; then   # a browser copies the file when it is attached: it cannot go away under the post
step "a file that cannot be read stops the post"
printf 'soon gone\n' > "$LIN_DIR/up-gone.txt"
"$DRIVE" attach "$(cpath up-gone.txt)" >/dev/null || gui_abort "the client did not take attach up-gone.txt"
rm -f "$LIN_DIR/up-gone.txt"
"$DRIVE" send "comes back" >/dev/null
returned() { [ "$(tray here)" = 1 ] && [ "$(chip 0 failed)" = 1 ] && [ "$(chip 0 state)" = -1 ] && [ "$(tray posts)" = 0 ]; }
check "the chip is back, marked" waitfor 10 returned
check "the text is back in the box" grep -Eq '^ed len=([1-9][0-9]*) ' "$D"
check "its button says it was not sent" grep -q '^a11yitem composer.file.0 ' "$D"
"$DRIVE" shot uploads_failed >/dev/null
click_x 0
"$DRIVE" send "after the failure" >/dev/null
check "nothing was posted for it (the next message is the last)" waitfor 10 newest_is 1 "after the failure"
no_gone() { ! grep -q 'name="up-gone.txt"' "$D"; }
check "...and no such file was shared" no_gone
fi

if [ "$PLAT" = win32 ]; then   # the clipboard is set from PowerShell: Windows alone
step "pasting attaches what is not text"
# The Windows clipboard, set the way other programs set it: Explorer's copy puts a
# file list on it (CF_HDROP); an image copied out of a program puts a bitmap.
# clip_put <kind> <set...>: set it (a win_clipboard_set_* of win.sh), then read
# back that the clipboard holds that kind -- another program holding the
# clipboard open makes a set fail quietly, and the paste then has nothing of the
# kind to attach. Tried again until it holds.
clip_put() {
  local what="$1"; shift
  put() { "$@"; win_clipboard_holds "$what"; }
  waitfor 10 put "$@" || gui_abort "the Windows clipboard would not take the $what"
}
clip_put FileDropList win_clipboard_set_files "$WIN_DIR\\up-picture.png"
"$DRIVE" key ctrl+v >/dev/null
pasted_file() { [ "$(tray here)" = 1 ] && [ "$(chip 0 name)" = up-picture.png ]; }
check "a copied file pastes as itself" waitfor 5 pasted_file
clip_put Image win_clipboard_set_png "$WIN_DIR\\up-picture.png"
"$DRIVE" key ctrl+v >/dev/null
# A chip named as a pasted image is: pasted-image-<date>-<time>.png.
pasted_name() { [[ "$1" =~ ^pasted-image-[0-9]{8}-[0-9]{6}\.png$ ]]; }
pasted_image() { [ "$(tray here)" = 2 ] && pasted_name "$(chip 1 name)" && [ "$(chip 1 pic)" = 1 ]; }
check "a copied image pastes as a PNG with its picture" waitfor 5 pasted_image
"$DRIVE" send "pasted" >/dev/null
check "the pasted ones land" waitfor 20 landed
pasted_msg() { gui_snap; grep -q 'attach msg=.* name="pasted-image-' "$D"; }
check "...the image under its pasted name" waitfor 10 pasted_msg
clip_put Text win_clipboard_set_text 'plain words'
"$DRIVE" key ctrl+v >/dev/null
as_text() { [ "$(tray here)" = 0 ] && grep -Eq '^ed len=11 ' "$D"; }
check "text still pastes as text, and adds no chip" waitfor 5 as_text
"$DRIVE" key ctrl+a >/dev/null; "$DRIVE" key delete >/dev/null
win_clipboard_clear
else   # the browser's clipboard, set from outside the client as another page or program sets it
step "pasting what was copied outside the client"
# The keyboard's Ctrl+V, through the page: the browser's paste, not the hook's.
"$DRIVE" sysclip png "$LIN_DIR/up-picture.png" >/dev/null
"$DRIVE" syspaste >/dev/null
pasted_name() { [[ "$1" =~ ^pasted-image-[0-9]{8}-[0-9]{6}\.png$ ]]; }
pasted_image() { [ "$(tray here)" = 1 ] && pasted_name "$(chip 0 name)" && [ "$(chip 0 pic)" = 1 ]; }
check "a copied image pastes as a PNG with its picture" waitfor 5 pasted_image
"$DRIVE" send "pasted" >/dev/null
check "the pasted one lands" waitfor 20 landed
"$DRIVE" sysclip text 'plain words' >/dev/null
"$DRIVE" syspaste >/dev/null
as_text() { [ "$(tray here)" = 0 ] && grep -Eq '^ed len=11 ' "$D"; }
check "copied text pastes into the box as text, and adds no chip" waitfor 5 as_text
step "copying out of the client"
"$DRIVE" key ctrl+a >/dev/null; "$DRIVE" key ctrl+c >/dev/null
clip_is() { [ "$("$DRIVE" sysclipread 2>/dev/null)" = "ack: ok $1" ]; }
copied_out() { clip_is 'plain words'; }
check "Ctrl+C in the box leaves its text on the browser's clipboard" waitfor 5 copied_out
"$DRIVE" key ctrl+a >/dev/null; "$DRIVE" key delete >/dev/null
# A message's own Copy text, from its menu: Ctrl+Down to the newest message,
# Shift+F10, the highlight walked to Copy text (command 20), Enter.
"$DRIVE" send "copy this message" >/dev/null
check "the message to copy is there" waitfor 10 newest_is 1 "copy this message"
"$DRIVE" move 20 20 >/dev/null
"$DRIVE" key ctrl+down >/dev/null
hovercmd() { gui_snap; field "$D" '^kbfocus ' menuhovercmd; }
"$DRIVE" key shift+f10 >/dev/null
menu_open() { local c; c="$(hovercmd)"; [ -n "$c" ] && [ "$c" != -1 ]; }
waitfor 5 menu_open || gui_abort "Shift+F10 opened no menu on the message"
found=0
for _ in $(seq 1 30); do
  [ "$(hovercmd)" = 20 ] && { found=1; break; }
  "$DRIVE" key down >/dev/null
done
check "its menu offers Copy text" [ "$found" = 1 ]
"$DRIVE" key enter >/dev/null
msg_copied() { clip_is 'copy this message'; }
check "Copy text leaves the message on the browser's clipboard" waitfor 5 msg_copied
fi

step "a large one, to watch (not asserted)"
head -c $((120 * 1024 * 1024)) /dev/urandom > "$LIN_DIR/up-large.bin"
"$DRIVE" attach "$(cpath up-picture.png)" >/dev/null
"$DRIVE" attach "$(cpath up-large.bin)" >/dev/null
"$DRIVE" send "a large one" >/dev/null || gui_abort "the client did not send"
# The shot once a chip is moving (state 1), or once they have landed already.
moving() { [ "$(tray here)" = 0 ] || grep -q '^  fchip [0-9]* .* state=1 ' "$D"; }
waitfor 10 moving
"$DRIVE" shot uploads_moving >/dev/null
check "the large one lands" waitfor 120 landed
rm -f "$LIN_DIR"/up-*.png "$LIN_DIR"/up-*.txt "$LIN_DIR"/up-*.bin
gui_end
