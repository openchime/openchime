#!/usr/bin/env bash
# The interface font: each choice in the Appearance preference, read from the dump.
#
#   tools/gui/features/fonts.sh          # run; the pair and daemon stop at the end
#   tools/gui/features/fonts.sh --keep   # ... and leave them running
#
# Each font on offer (Default, Verdana, Calibri, Sitka) is chosen in turn and the
# dump is read: the family asked for and the family used (`fontpick`, `fontfam`),
# every search box's face, both UI weights on one line box (`ui_mh_eq=1`), the
# find box's EDIT one line tall inside its chrome, and, at the largest text size,
# larger formats than at the default. Under Default the pinned pairs are the ones
# ARCH-97 states. On the web a family the browser lacks is reported missing and
# the default used, which is all a browser without Windows' fonts can show. Then a family no system has is asked for by name (the test-only
# `fontfam` verb): the client says it is missing and uses the default family.
# Its own daemon on its own port, wiped each run; only alice's client is driven.
#
# platforms: win32 web
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
. "$HERE/tools/gui/harness/lib.sh"
export OC_TEST_HOST="${OC_TEST_HOST:-$(ip -4 -o addr show eth0 | awk '{print $4}' | cut -d/ -f1)}"
gui_begin fonts 9655 /tmp/oc-fonts "$@"

a()      { "$PAIR" a "$@" >/dev/null; }
ts()     { field "$DA" "^textsize=" "$1"; }      # the text-size line
tsq()    { fieldq "$DA" "^textsize=" "$1"; }
tm()     { field "$DA" "^textmetrics " "$1"; }
sb()     { field "$DA" "^searchbox $1 " "$2"; }
dpi()    { ts dpi; }

# Every search box the client has made wears the family it says it uses.
faces_ok() {
  local fam want n=0
  want="$(tsq fontfam)"; [ -n "$want" ] || return 1
  while IFS= read -r fam; do
    n=$((n + 1)); [ "$fam" = "$want" ] || return 1
  done < <(grep -E '^searchbox [a-z]+ made=1 ' "$DA" | grep -o 'face="[^"]*"' | cut -d'"' -f2)
  [ "$n" -gt 0 ]
}
# The find box: shown, at least the UI size, its EDIT holding a line of the
# font, inside its chrome and centred in it (as search_boxes.sh reads it).
find_ok() {
  local el et er eb bl bt br bb lh lf min
  [ "$(sb find vis)" = 1 ] || return 1
  lf="$(sb find lf)"; lh="$(sb find lh)"; min=$(( 14 * $(dpi) / 96 ))
  [ "${lf#-}" -ge "$min" ] || return 1
  IFS=, read -r el et er eb <<< "$(sb find edit)"
  IFS=, read -r bl bt br bb <<< "$(sb find box)"
  [ $((eb - et)) -ge "$lh" ] || return 1
  [ "$el" -gt "$bl" ] && [ "$er" -lt "$br" ] && [ "$et" -ge "$bt" ] && [ "$eb" -le "$bb" ] || return 1
  local above=$((et - bt)) below=$((bb - eb)); local d=$((above - below)); [ "${d#-}" -le 1 ]
}
# picked <n> <ts>: alice's client shows font <n> at text size <ts>.
picked() { gui_snap a; [ "$(ts font)" = "$1" ] && [ "$(ts textsize)" = "$2" ]; }
# The UI token's line box height (the `ui=` pair's first half).
ui_lh() { tm ui | cut -d/ -f1; }

FAMS=("Segoe UI" "Verdana" "Calibri" "Sitka Text")
NAMES=("Default" "Verdana" "Calibri" "Sitka")

step "fixture: a fresh daemon, alice"
gui_pair_up
"$PAIR" a size 1600 1000 >/dev/null
a dpi 96 || gui_abort "alice's client did not take dpi"
a textsize 1 || gui_abort "alice's client did not take textsize"
a view home; a channel general

DEFAULT_FAM=""
for n in 0 1 2 3; do
  name="${NAMES[$n]}"
  step "$name"
  a font "$n" || gui_abort "alice's client did not take font $n"
  waitfor 5 picked "$n" 1 || gui_abort "alice's client is not on font $n"
  pick="$(tsq fontpick)"; fam="$(tsq fontfam)"
  say "  asked for \"$pick\", using \"$fam\" (missing=$(ts fontmissing) ascent=$(ts fontasc) cap=$(ts fontcap))"
  if [ "$n" = 0 ]; then
    # Default is the platform's UI family; on the web the browser's own.
    DEFAULT_FAM="$fam"
    case "$fam" in "Segoe UI"|"Segoe UI Variable Text") ok "$name: the platform's UI family" ;;
      *) if [ "$PLAT" = web ]; then ok "$name: the browser's UI family"; else fail "$name: the family is \"$fam\""; fi ;; esac
    check "$name: the pinned pairs are ARCH-97's" \
      test "$(tm scale) $(tm display) $(tm title) $(tm ui) $(tm meta) $(tm micro)" = "1.00 26/19 23/17 22/16 19/14 15/11"
  else
    check "$name: the family asked for is ${FAMS[$n]}" test "$pick" = "${FAMS[$n]}"
    if [ "$PLAT" = web ] && [ "$(ts fontmissing)" = 1 ]; then
      # A browser without the family (fonts are not part of parity, WEB.md):
      # it says so, and uses the default rather than whatever the browser picks.
      check "$name: missing in this browser, the default is used" test "$fam" = "$DEFAULT_FAM"
    else
      check "$name: and is the one used" test "$fam" = "${FAMS[$n]}" -a "$(ts fontmissing)" = 0
    fi
  fi
  check "$name: every search box wears it" faces_ok
  check "$name: both UI weights on one line box" test "$(tm ui_mh_eq)" = 1
  check "$name: the find box holds its line" find_ok
  [ "$n" = 1 ] && a shot font_verdana
  UI_DEFAULT="$(ui_lh)"; LF_DEFAULT="$(sb find lf)"

  a textsize 3 || gui_abort "alice's client did not take textsize"
  waitfor 5 picked "$n" 3 || gui_abort "alice's client is not at the largest text size"
  check "$name, largest: the formats are larger" test "$(ui_lh)" -gt "$UI_DEFAULT" -a "$(sb find lf)" -gt "$LF_DEFAULT"
  check "$name, largest: both UI weights on one line box" test "$(tm ui_mh_eq)" = 1
  check "$name, largest: the find box holds its line" find_ok
  a textsize 1 || gui_abort "alice's client did not take textsize"
done

step "a family the system lacks"
a fontfam NoSuchFamilyOpenChime || gui_abort "alice's client did not take fontfam"
asked() { gui_snap a; [ "$(tsq fontpick)" = NoSuchFamilyOpenChime ]; }
waitfor 5 asked || gui_abort "alice's client did not ask for the family"
say "  asked for \"$(tsq fontpick)\", using \"$(tsq fontfam)\" (missing=$(ts fontmissing))"
check "missing: reported missing" test "$(ts fontmissing)" = 1
check "missing: the default family is used" test "$(tsq fontfam)" = "$DEFAULT_FAM"
check "missing: every search box wears the default" faces_ok
a fontfam
a font 0
gui_end
