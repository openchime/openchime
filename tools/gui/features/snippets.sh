#!/usr/bin/env bash
# Snippets (REQ-226): one posted, shown as a card with its first lines, and
# opened whole, coloured.
#
#   tools/gui/features/snippets.sh          # run; the client and daemon stop at the end
#   tools/gui/features/snippets.sh --keep   # ... and leave them running
#
# The test hook's "snippet" verb posts one as Create snippet does; the card
# fetches its text; a click on the card opens the viewer, whose spans are the
# lexer's; Copy and Close are its buttons. Its own daemon on its own port, wiped
# each run.
#
# platforms: win32
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
. "$HERE/tools/gui/harness/lib.sh"
gui_begin snippets 9720 /tmp/oc-snippets "Snippet Fixture" "$@"

card() { field "$D" '^snipcard ' "$1"; }
view() { field "$D" '^snipview ' "$1"; }

step "a snippet posted is a card with its first lines"
gui_launch
"$DRIVE" size 1200 900 >/dev/null
"$DRIVE" channel 1 >/dev/null || gui_abort "the client did not open the channel"
"$DRIVE" snippet 'rotate.py|python|import os\n# rotate the keys\ndef rotate(n):\n    return n + 1\n' >/dev/null ||
  gui_abort "the client did not take the snippet"
loaded() { gui_snap; [ "$(card loaded)" = 1 ]; }
check "the card is drawn, and its text fetched" waitfor 15 loaded
"$DRIVE" shot snippets_card >/dev/null

step "a click opens it whole, coloured"
click_rect "$(card rect)" || gui_abort "no snippet card to click"
opened() { gui_snap; [ "$(view open)" = 1 ] && [ "$(view spans)" != 0 ]; }
check "the viewer opens on it" waitfor 5 opened
check "its words are coloured (spans $(view spans))" [ "$(view spans)" -ge 4 ]
check "as Python" [ "$(view lang)" = 2 ]
"$DRIVE" shot snippets_view >/dev/null
# Its Copy puts the whole text on the clipboard, read back from outside the
# client as another program reads it (Windows: snippets are not run on the web).
if [ "$PLAT" = win32 ]; then
  win_clipboard_set_text before
  press modal.button.copy
  copied() {
    local clip; clip="$(win_clipboard_read)"
    grep '^def rotate(n):$' <<<"$clip" >/dev/null
  }
  check "Copy puts its whole text on the clipboard" waitfor 5 copied
fi
"$DRIVE" key escape >/dev/null
closed() { gui_snap; [ "$(view open)" = 0 ]; }
check "Esc closes it" waitfor 5 closed
gui_end
