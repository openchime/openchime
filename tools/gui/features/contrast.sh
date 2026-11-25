#!/usr/bin/env bash
# High contrast (REQ-262): the Appearance preference, Off, On or Follow system.
#
#   tools/gui/features/contrast.sh          # run; the client and daemon stop at the end
#   tools/gui/features/contrast.sh --keep   # ... and leave them running
#
# Read from the dump's `contrast` line: `pref=` (0 Off, 1 On, 2 Follow
# system), `sys=` (what the client takes the system's setting to be: 1 on,
# 0 off, -1 unknown) and `on=` (whether the high-contrast palette is the one in
# force); and from the `palette` line, every token by name, `on_accent=`
# included. The system's answer is stood in for with the test-only
# `syscontrast 0|1|-1` verb, so both branches of Follow system run on any
# machine. It asserts that:
#
#   - the default is Follow system;
#   - under Follow system, a system that says on is on and one that says off
#     is off; On is on whatever the system says, Off never is;
#   - with high contrast on, in Dark and in Light, the palette as dumped
#     clears 7:1 for every text ink (text, muted, faint, accent, danger) on
#     every surface (base, sidebar, header, input, select, hover), rail_icon
#     on rail and on_accent on accent, accent_dim and danger; and 4.5:1 for
#     every control ink (border, accent_dim, online, away, notice) on every
#     surface;
#   - clicking another colour scheme while it is on leaves the palette as it
#     was, and turning it off shows that scheme's rail and accent;
#   - the Appearance row has the three choices; one clicked is put back by
#     Cancel; Reset returns it to Follow system;
#   - the choice saved is there again after the client starts again.
#
# Its own daemon on its own port, wiped each run.
#
# platforms: win32 web
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
. "$HERE/tools/gui/harness/lib.sh"
gui_begin contrast 9685 /tmp/oc-contrast "Contrast Fixture" "$@"

hc() { field "$D" '^contrast ' "$1"; }
pal() { field "$D" '^palette ' "$1"; }
seen_contrast() { say "     contrast: $(grep -m1 '^contrast ' "$D")"; }
# contrast_is <pref> <sys> <on>, from a fresh look.
contrast_now() { gui_snap; [ "$(hc pref)" = "$1" ] && [ "$(hc sys)" = "$2" ] && [ "$(hc on)" = "$3" ]; }
contrast_is() { waitfor 5 contrast_now "$@" || { seen_contrast; return 1; }; }
# The rectangle of Preferences' choice <row> <val>, as the dump places it.
hit() { grep -m1 "^  prefhit row=$1 val=$2 " "$D" | grep -o 'r=[-0-9,]*' | cut -d= -f2; }
# The whole palette as dumped, its tokens only, for comparing two looks.
palette_now() { grep -m1 '^palette ' "$D" | cut -d' ' -f5-; }

# short_pairs <text|controls>: from the dumped palette, each pair under its
# ratio, one per line, then `checked=<n>`. The same arithmetic as
# oc_theme_contrast (WCAG's relative luminance), in awk without gawk.
short_pairs() {
  grep -m1 '^palette ' "$D" | awk -v kind="$1" '
    function hx(s,  i, n) { n = 0; s = tolower(s)
      for (i = 1; i <= length(s); i++) n = n * 16 + index("0123456789abcdef", substr(s, i, 1)) - 1
      return n }
    function ch(v) { v = v / 255; return v <= 0.03928 ? v / 12.92 : ((v + 0.055) / 1.055) ^ 2.4 }
    function lum(s) { return 0.2126 * ch(hx(substr(s, 1, 2))) + 0.7152 * ch(hx(substr(s, 3, 2))) + 0.0722 * ch(hx(substr(s, 5, 2))) }
    function cr(a, b,  x, y, t) { x = lum(a); y = lum(b); if (x < y) { t = x; x = y; y = t }; return (x + 0.05) / (y + 0.05) }
    function pair(ink, surf, want,  c) { n++; c = cr(P[ink], P[surf])
      if (P[ink] == "" || P[surf] == "" || c < want) printf "%s on %s: %s on %s is %.2f:1, under %.1f:1\n", ink, surf, P[ink], P[surf], c, want }
    { for (i = 1; i <= NF; i++) { split($i, kv, "="); P[kv[1]] = kv[2] } }
    END {
      ns = split("base sidebar header input select hover", S, " ")
      if (kind == "text") {
        ni = split("text muted faint accent danger", I, " ")
        for (i = 1; i <= ni; i++) for (s = 1; s <= ns; s++) pair(I[i], S[s], 7.0)
        pair("rail_icon", "rail", 7.0)
        nf = split("accent accent_dim danger", F, " ")
        for (f = 1; f <= nf; f++) pair("on_accent", F[f], 7.0)
      } else {
        ni = split("border accent_dim online away notice", I, " ")
        for (i = 1; i <= ni; i++) for (s = 1; s <= ns; s++) pair(I[i], S[s], 4.5)
      }
      print "checked=" n
    }'
}
# clears <text|controls> <pairs expected>: none short, and all of them checked.
clears() {
  local out; out="$(short_pairs "$1")"
  [ "$(grep -vc '^checked=' <<<"$out")" = 0 ] && grep -qx "checked=$2" <<<"$out" && return 0
  say "     $(grep -m1 '^palette ' "$D")"
  while IFS= read -r l; do say "     $l"; done <<<"$out"
  return 1
}

step "the default"
gui_launch
"$DRIVE" size 1100 800 >/dev/null
act channel 1
gui_snap
check "High contrast is Follow system" [ "$(hc pref)" = 2 ]

step "Follow system, with the system's answer stood in for"
act syscontrast 1
check "a system that says on: on" contrast_is 2 1 1
act syscontrast 0
check "a system that says off: off" contrast_is 2 0 0

step "On and Off, whatever the system says"
act contrast 1
act syscontrast 0
check "On, with a system that says off: on" contrast_is 1 0 1
act contrast 0
act syscontrast 1
check "Off, with a system that says on: off" contrast_is 0 1 0
act syscontrast -1

step "the palette, high contrast in Dark and in Light"
act contrast 1
for mode in 0 1; do
  name=Dark; [ "$mode" = 1 ] && name=Light
  act theme "$mode"
  in_mode() { gui_snap; [ "$(hc on)" = 1 ] && [ "$(pal light)" = "$1" ]; }
  need "$name, with high contrast on" waitfor 5 in_mode "$mode"
  check "$name: every text ink clears 7:1, and the inks on the rail and on an accent" clears text 34
  check "$name: every control ink clears 4.5:1" clears controls 30
done
act theme 0

step "a colour scheme picked while high contrast is on"
act prefs 0
drawn() { gui_snap; [ "$(grep -c '^  prefhit row=131 ' "$D")" = 3 ] && [ -n "$(hit 6 2)" ]; }
check "Appearance has High contrast with three choices" waitfor 5 drawn
"$DRIVE" shot contrast_prefs >/dev/null
before="$(palette_now)"
click_rect "$(hit 6 2)" || gui_abort "the dump gives no rectangle for the Teal scheme"
teal_kept() { gui_snap; [ "$(field "$D" '^textsize=' scheme)" = 2 ]; }
check "the scheme is chosen" waitfor 5 teal_kept
unchanged() { [ "$(palette_now)" = "$before" ]; }
check "...and the palette is as it was" unchanged
click_rect "$(hit 131 0)" || gui_abort "the dump gives no rectangle for High contrast's Off"
teal_shown() {
  gui_snap; [ "$(hc pref)" = 0 ] && [ "$(hc on)" = 0 ] &&
    [ "$(field "$D" '^textsize=' railcol)" = 0B322D ] && [ "$(field "$D" '^textsize=' accentcol)" = 2DD4BF ]
}
check "turned off, Teal's rail and accent are shown" waitfor 5 teal_shown

step "Cancel and Reset"
press modal.button.cancel || gui_abort "Preferences has no Cancel to press"
put_back() {
  gui_snap; ! grep -q '^modal=prefs ' "$D" && [ "$(hc pref)" = 1 ] && [ "$(hc on)" = 1 ] &&
    [ "$(field "$D" '^textsize=' scheme)" = 0 ]
}
check "Cancel puts On back, and the scheme with it" waitfor 5 put_back
act prefs 3
reset_drawn() { gui_snap; grep -q '^  prefhit row=9 val=0 ' "$D"; }
waitfor 5 reset_drawn || gui_abort "System drew no Reset"
click_rect "$(hit 9 0)" || gui_abort "the dump gives no rectangle for Reset"
system_now() { gui_snap; [ "$(hc pref)" = 2 ]; }
check "Reset returns it to Follow system" waitfor 5 system_now
press modal.button.save || gui_abort "Preferences has no Save to press"
saved() { gui_snap; ! grep -q '^modal=prefs ' "$D" && [ "$(hc pref)" = 2 ]; }
check "and Save keeps it" waitfor 5 saved

step "the client starts again"
act contrast 1
OC_TEST_NO_BUILD=1 OC_TEST_NO_DAEMON=1 gui_launch
"$DRIVE" size 1100 800 >/dev/null
act channel 1
kept() { gui_snap; [ "$(hc pref)" = 1 ] && [ "$(hc on)" = 1 ]; }
check "On is still chosen: it is kept in the account's preferences" waitfor 10 kept
act contrast 2
gui_end
