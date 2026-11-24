#!/usr/bin/env bash
# Preferences' choices, as assistive technology is given them (REQ-269,
# REQ-290, ARCH-99).
#
#   tools/gui/features/prefs_a11y.sh          # run; the client and daemon stop at the end
#   tools/gui/features/prefs_a11y.sh --keep   # ... and leave them running
#
# Every chip and swatch a Preferences row draws is published to the
# accessibility tree with an automation id `prefs.<row>.<value>`: the row's
# fixed name and the value the choice stores. A choice in a row of several is
# a tab named "<row label>: <choice>", the current one adding ", selected"; a
# row with one action is a button named for the row. It reads the dump's
# `prefhit` lines (each choice's rect, empty when scrolled out of the pane),
# `a11yitem` (the tree), `a11yname` (what each of those items is called),
# `motion` and `spoken`, and asserts that:
#
#   - in Appearance, Messages, Notifications, System and Accessibility, the
#     choices published are as many as the on-screen `prefhit` lines, and no
#     id in the tree appears twice;
#   - the ids prefs.theme.0-2, prefs.motion.0-2, prefs.enter.1, prefs.reset.0
#     and prefs.say.order.1 are there;
#   - Reduce motion's three choices are named, left to right, "Reduce motion:
#     Off", "Reduce motion: On" and "Reduce motion: Follow system, selected";
#     Reset is a button named "Reset preferences";
#   - pressing prefs.motion.1 where it is published chooses On, and its name
#     then carries ", selected" in place of Follow system's;
#   - on win32, invoking prefs.say.order.1 through UI Automation sets Text
#     first, and Cancel puts it back; a choice of another category cannot be
#     invoked;
#   - Accessibility's ids are the same at widths of 1000 and 1400.
#
# Its own daemon on its own port, wiped each run.
#
# platforms: win32 web
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
. "$HERE/tools/gui/harness/lib.sh"
gui_begin prefs_a11y 9615 /tmp/oc-prefs-a11y "Prefs A11y Fixture" "$@"

# The choices on screen, by the dump's prefhit lines, and as published.
hits_on() { grep '^  prefhit ' "$D" | grep -vc 'r=0,0,0,0'; }
choice_ids() { grep -oE '^a11yitem prefs\.[^ ]+' "$D" | cut -d' ' -f2 | grep -vE '^prefs\.(cat|quick)\.'; }
items_on() { choice_ids | grep -c .; }
dup_ids() { grep '^a11yitem ' "$D" | cut -d' ' -f2 | sort | uniq -d; }
has_id() { grep -q "^a11yitem $1 " "$D"; }
# name_of <aid>: what that item is called, quotes off.
name_of() { grep -m1 "^a11yname $1 " "$D" | sed 's/^a11yname [^ ]* "//; s/"$//'; }
seen_counts() { say "     prefhit on screen: $(hits_on), choices published: $(items_on)"; }
# shown <cat-index>: that category is drawn, with its choices published as
# many as are on screen.
shown_now() { gui_snap && grep -q "^prefcat=$1\$" "$D" && [ "$(hits_on)" -gt 0 ] && [ "$(items_on)" = "$(hits_on)" ]; }
shown() { waitfor 5 shown_now "$1" || { seen_counts; return 1; }; }
no_dups() { local d; d="$(dup_ids)"; [ -z "$d" ] || { say "     twice: $d"; return 1; }; }
mo() { field "$D" '^motion ' "$1"; }
sp() { field "$D" '^spoken ' "$1"; }

step "fixture"
gui_launch
"$DRIVE" size 1100 900 >/dev/null
act channel 1
act prefs 0

step "Appearance"
check "every choice on screen is published, one item each" shown 0
check "no id in the tree appears twice" no_dups
check "Theme's three choices: prefs.theme.0 to 2" eval 'has_id prefs.theme.0 && has_id prefs.theme.1 && has_id prefs.theme.2'
check "Reduce motion's three: prefs.motion.0 to 2" eval 'has_id prefs.motion.0 && has_id prefs.motion.1 && has_id prefs.motion.2'
in_order() { [ "$(grep '^a11yitem prefs\.motion\.' "$D" | cut -d' ' -f2 | tr '\n' ' ')" = "prefs.motion.0 prefs.motion.1 prefs.motion.2 " ]; }
check "...published left to right, as drawn" in_order
check "\"Reduce motion: Off\"" eval '[ "$(name_of prefs.motion.0)" = "Reduce motion: Off" ]'
check "\"Reduce motion: On\"" eval '[ "$(name_of prefs.motion.1)" = "Reduce motion: On" ]'
check "\"Reduce motion: Follow system, selected\"" eval '[ "$(name_of prefs.motion.2)" = "Reduce motion: Follow system, selected" ]'
"$DRIVE" shot prefs_a11y_appearance >/dev/null

step "pressing a choice where it is published"
press prefs.motion.1 || gui_abort "prefs.motion.1 was never published"
on_chosen() {
  gui_snap && [ "$(mo pref)" = 1 ] &&
    [ "$(name_of prefs.motion.1)" = "Reduce motion: On, selected" ] &&
    [ "$(name_of prefs.motion.2)" = "Reduce motion: Follow system" ]
}
check "it chooses On, and On is now the one selected" waitfor 5 on_chosen
press modal.button.cancel || gui_abort "Preferences has no Cancel to press"
put_back() { gui_snap && ! grep -q '^modal=prefs ' "$D" && [ "$(mo pref)" = 2 ]; }
check "Cancel puts Follow system back" waitfor 5 put_back
act prefs 0

step "Messages"
press prefs.cat.messages || gui_abort "Messages was never published"
check "every choice on screen is published, one item each" shown 1
check "no id in the tree appears twice" no_dups
check "When I press Enter's Start a new line: prefs.enter.1" has_id prefs.enter.1

step "Notifications"
press prefs.cat.notifications || gui_abort "Notifications was never published"
check "every choice on screen is published, one item each" shown 2
check "no id in the tree appears twice" no_dups

step "System"
press prefs.cat.system || gui_abort "System was never published"
check "every choice on screen is published, one item each" shown 3
check "no id in the tree appears twice" no_dups
check "Reset: prefs.reset.0, a button named for its row" eval \
  'has_id prefs.reset.0 && [ "$(name_of prefs.reset.0)" = "Reset preferences" ]'

step "Accessibility"
press prefs.cat.accessibility || gui_abort "Accessibility was never published"
check "every choice on screen is published, one item each" shown 5
check "no id in the tree appears twice" no_dups
check "Order's Text first: prefs.say.order.1" has_id prefs.say.order.1

if [ "$PLAT" = win32 ]; then
step "invoking a choice through UI Automation"
check "Text first is Order's choice, not yet chosen" eval '[ "$(sp text_first)" = 0 ]'
check "invoking prefs.say.order.1 is accepted" win_uia_invoke prefs.say.order.1
text_first() { gui_snap && [ "$(sp text_first)" = "$1" ]; }
check "it sets Text first" waitfor 5 text_first 1
check "...and names it selected" eval '[ "$(name_of prefs.say.order.1)" = "Order: Text first, selected" ]'
check "a choice of another category is not there to invoke" eval '! win_uia_invoke prefs.motion.1'
check "...and Reduce motion is as it was" eval '[ "$(mo pref)" = 2 ]'
press modal.button.cancel || gui_abort "Preferences has no Cancel to press"
check "Cancel puts Sender first back" waitfor 5 text_first 0
act prefs 5
fi

step "Accessibility's ids at two widths"
ids_at() {
  "$DRIVE" size "$1" 900 >/dev/null
  waitfor 5 shown_now 5 || return 1
  choice_ids | sort | tr '\n' ' '
}
at1000="$(ids_at 1000)"
at1400="$(ids_at 1400)"
say "     at 1000: $at1000"
check "all twelve choices are published at 1000" eval '[ "$(wc -w <<<"$at1000")" = 12 ]'
check "...and the same ids at 1400" eval '[ -n "$at1000" ] && [ "$at1000" = "$at1400" ]'
gui_end
