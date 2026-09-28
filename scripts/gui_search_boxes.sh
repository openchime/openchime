#!/usr/bin/env bash
# The Win32 client's six search boxes: one control, the app's own text.
#
#   scripts/gui_search_boxes.sh            # run, and leave the pair running
#   scripts/gui_search_boxes.sh --down     # ... and take it down afterwards
#
# Find a conversation, search messages, files, people, Jump to and emoji are each
# a native EDIT over chrome the client draws. Each is opened in turn, at the
# default text size, at the largest, and at 200% DPI, and the dump's `searchbox`
# line is checked: the box wears the UI font (form_font()) in the UI face at no
# less than the UI size; its EDIT is exactly the font's line height, inside its
# chrome and centred in it; and all six share one font size. Before, every one
# wore the stock dialog font, a fixed 11 px face whatever the DPI or text size.
# Its own daemon on its own port, wiped each run; only alice's client is driven.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PAIR="$HERE/scripts/gui_pair.sh"
fails=0

export OC_PAIR_PORT="${OC_PAIR_PORT:-9660}"
export OC_PAIR_DIR="${OC_PAIR_DIR:-/tmp/oc-sbox}"
export OC_PAIR_HOST="${OC_PAIR_HOST:-$(ip -4 -o addr show eth0 | awk '{print $4}' | cut -d/ -f1)}"

say()   { printf '%s\n' "$*"; }
check() { local what="$1"; shift; if "$@"; then say "  ok   $what"; else say "  FAIL $what"; fails=$((fails + 1)); fi; }
D=/tmp/oc-sbox-dump.txt
dump()  { "$PAIR" a dump sbox > "$D" 2>&1; }
a()     { "$PAIR" a "$@" >/dev/null; }
settle(){ sleep 0.6; dump; }
line()  { grep -m1 "^searchbox $1 " "$D"; }
field() { line "$1" | grep -o "\b$2=[^ ]*" | head -1 | cut -d= -f2 | tr -d '"'; }
face()  { line "$1" | grep -o 'face="[^"]*"' | cut -d'"' -f2; }
dpi()   { grep -m1 -o '\bdpi=[0-9]*' "$D" | cut -d= -f2; }

# The one box open now: the UI font and face, at least the UI size (14 DIP at
# this DPI), and an EDIT exactly one line tall, inside its chrome, centred.
box_ok() {
  local n="$1" e b el et er eb bl bt br bb lh lf min
  [ "$(field "$n" vis)" = 1 ] && [ "$(field "$n" uifont)" = 1 ] || return 1
  case "$(face "$n")" in "Segoe UI"|"Segoe UI Variable Text") ;; *) return 1 ;; esac
  lf="$(field "$n" lf)"; lh="$(field "$n" lh)"; min=$(( 14 * $(dpi) / 96 ))
  [ "${lf#-}" -ge "$min" ] || return 1
  IFS=, read -r el et er eb <<< "$(field "$n" edit)"
  IFS=, read -r bl bt br bb <<< "$(field "$n" box)"
  [ $((eb - et)) -eq "$lh" ] || return 1
  [ "$el" -gt "$bl" ] && [ "$er" -lt "$br" ] && [ "$et" -ge "$bt" ] && [ "$eb" -le "$bb" ] || return 1
  local above=$((et - bt)) below=$((bb - eb)); local d=$((above - below)); [ "${d#-}" -le 1 ]
}

SIZES=""
open_each() {   # open every box in turn and check it; remember each one's size
  local tag="$1"
  a view home; a channel general; settle
  check "$tag: find a conversation" box_ok find;   SIZES="$SIZES $(field find lf)"
  [ "$tag" = default ] && a shot sbox_find
  a search; settle
  check "$tag: search messages" box_ok srch;       SIZES="$SIZES $(field srch lf)"
  [ "$tag" = default ] && a shot sbox_search
  a key esc; a view files; settle
  check "$tag: search files" box_ok files;         SIZES="$SIZES $(field files lf)"
  [ "$tag" = default ] && a shot sbox_files
  a view people; settle
  check "$tag: search people" box_ok people;       SIZES="$SIZES $(field people lf)"
  [ "$tag" = default ] && a shot sbox_people
  a view home; a channel general; a palette; settle
  check "$tag: jump to" box_ok pal;                SIZES="$SIZES $(field pal lf)"
  [ "$tag" = default ] && a shot sbox_palette
  a key esc; a emoji 0; settle
  check "$tag: search emoji" box_ok pick;          SIZES="$SIZES $(field pick lf)"
  [ "$tag" = default ] && a shot sbox_emoji
  a key esc
}
uniform() { [ "$(printf '%s\n' $SIZES | sort -u | wc -l)" -eq 1 ]; }

say "== fixture: a fresh daemon, alice"
"$PAIR" down >/dev/null 2>&1
rm -rf "$OC_PAIR_DIR"
"$PAIR" up || { say "pair did not come up"; exit 1; }
"$PAIR" a size 1600 1000 >/dev/null
a dpi 96; a textsize 1

say "== default text size, 96 DPI"
SIZES=""; open_each default
check "default: all six share one font size" uniform

say "== the largest text size"
a textsize 3
SIZES=""; open_each largest
check "largest: all six share one font size" uniform
a textsize 1

say "== 200% DPI"
a dpi 192
SIZES=""; open_each hidpi
check "200%: all six share one font size" uniform
a dpi 96

[ "${1:-}" = "--down" ] && "$PAIR" down >/dev/null 2>&1
if [ "$fails" -eq 0 ]; then say "gui_search_boxes: all passed"; else say "gui_search_boxes: $fails FAILED"; fi
exit "$fails"
