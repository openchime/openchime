#!/usr/bin/env bash
# The Win32 client's six search boxes: one control, the app's own text.
#
#   tools/gui/features/search_boxes.sh          # run; the pair and daemon stop at the end
#   tools/gui/features/search_boxes.sh --keep   # ... and leave them running
#
# Find a conversation, search messages, files, people, Jump to and emoji are each
# a native EDIT over chrome the client draws. Each is opened in turn, at the
# default text size, at the largest, and at 200% DPI, and the dump's `searchbox`
# line is checked: the box wears the UI font (form_font()) in the UI face at no
# less than the UI size; its EDIT is exactly the font's line height, inside its
# chrome and centred in it; and all six share one font size. Before, every one
# wore the stock dialog font, a fixed 11 px face whatever the DPI or text size.
# Its own daemon on its own port, wiped each run; only alice's client is driven.
#
# platforms: win32 web
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
. "$HERE/tools/gui/harness/lib.sh"
export OC_TEST_HOST="${OC_TEST_HOST:-$(ip -4 -o addr show eth0 | awk '{print $4}' | cut -d/ -f1)}"
gui_begin search_boxes 9660 /tmp/oc-sbox "$@"

a()     { "$PAIR" a "$@" >/dev/null; }
line()  { grep -m1 "^searchbox $1 " "$DA"; }
sb()    { field "$DA" "^searchbox $1 " "$2"; }   # sb <box> <key>
face()  { fieldq "$DA" "^searchbox $1 " face; }
dpi()   { grep -m1 -o '\bdpi=[0-9]*' "$DA" | cut -d= -f2; }
# shown <box>: that box is open, in a fresh look at alice's client.
shown() { gui_snap a; [ "$(sb "$1" vis)" = 1 ]; }

# The one box open now: the UI font and face, at least the UI size (14 DIP at
# this DPI), and an EDIT exactly one line tall, inside its chrome, centred.
box_ok() {
  local n="$1" el et er eb bl bt br bb lh lf min
  [ "$(sb "$n" vis)" = 1 ] && [ "$(sb "$n" uifont)" = 1 ] || return 1
  # The UI face: Segoe on Windows; the browser's system font on the web,
  # whatever it is called there (fonts are not part of parity, WEB.md).
  case "$(face "$n")" in "Segoe UI"|"Segoe UI Variable Text") ;; "") return 1 ;; *) [ "$PLAT" = web ] || return 1 ;; esac
  lf="$(sb "$n" lf)"; lh="$(sb "$n" lh)"; min=$(( 14 * $(dpi) / 96 ))
  [ "${lf#-}" -ge "$min" ] || return 1
  IFS=, read -r el et er eb <<< "$(sb "$n" edit)"
  IFS=, read -r bl bt br bb <<< "$(sb "$n" box)"
  # The field's box is the chrome's full height (the text is centred inside it
  # by field_draw); it must hold at least a line.
  [ $((eb - et)) -ge "$lh" ] || return 1
  [ "$el" -gt "$bl" ] && [ "$er" -lt "$br" ] && [ "$et" -ge "$bt" ] && [ "$eb" -le "$bb" ] || return 1
  local above=$((et - bt)) below=$((bb - eb)); local d=$((above - below)); [ "${d#-}" -le 1 ]
}

SIZES=""
# open <box> <verb...>: the verbs that open it, then wait until it shows.
open() {
  local n="$1"; shift
  while [ $# -gt 0 ]; do
    local v=(); while [ $# -gt 0 ] && [ "$1" != ";" ]; do v+=("$1"); shift; done; [ $# -gt 0 ] && shift
    a "${v[@]}" || gui_abort "alice's client did not take '${v[*]}'"
  done
  waitfor 5 shown "$n" || say "  (the $n box did not show)"
}
open_each() {   # open every box in turn and check it; remember each one's size
  local tag="$1"
  open find view home \; channel general
  check "$tag: find a conversation" box_ok find;   SIZES="$SIZES $(sb find lf)"
  [ "$tag" = default ] && a shot sbox_find
  open srch search
  check "$tag: search messages" box_ok srch;       SIZES="$SIZES $(sb srch lf)"
  [ "$tag" = default ] && a shot sbox_search
  open files key esc \; view files
  check "$tag: search files" box_ok files;         SIZES="$SIZES $(sb files lf)"
  [ "$tag" = default ] && a shot sbox_files
  open people view people
  check "$tag: search people" box_ok people;       SIZES="$SIZES $(sb people lf)"
  [ "$tag" = default ] && a shot sbox_people
  open pal view home \; channel general \; palette
  check "$tag: jump to" box_ok pal;                SIZES="$SIZES $(sb pal lf)"
  [ "$tag" = default ] && a shot sbox_palette
  open pick key esc \; emoji 0
  check "$tag: search emoji" box_ok pick;          SIZES="$SIZES $(sb pick lf)"
  [ "$tag" = default ] && a shot sbox_emoji
  a key esc
}
uniform() { [ "$(printf '%s\n' $SIZES | sort -u | wc -l)" -eq 1 ]; }

step "fixture: a fresh daemon, alice"
gui_pair_up
"$PAIR" a size 1600 1000 >/dev/null
a dpi 96 || gui_abort "alice's client did not take dpi"
a textsize 1 || gui_abort "alice's client did not take textsize"

step "default text size, 96 DPI"
SIZES=""; open_each default
check "default: all six share one font size" uniform
LF_DEFAULT="$(sb pick lf)"

step "the largest text size"
a textsize 3 || gui_abort "alice's client did not take textsize"
# The find box is always there: it wears the larger text before any box is
# read at this size (else "all six share one size" could be six at the default).
grown() { gui_snap a; local f; f="$(sb find lf)"; [ -n "$f" ] && [ "${f#-}" -gt "${LF_DEFAULT#-}" ]; }
check "largest: the boxes' text is larger than at the default" waitfor 5 grown
SIZES=""; open_each largest
check "largest: all six share one font size" uniform
a textsize 1

step "200% DPI"
a dpi 192 || gui_abort "alice's client did not take dpi"
at_dpi() { gui_snap a; [ "$(dpi)" = 192 ]; }
waitfor 5 at_dpi || gui_abort "alice's client is not at 192 DPI"
SIZES=""; open_each hidpi
check "200%: all six share one font size" uniform
a dpi 96
gui_end
