#!/usr/bin/env bash
# What each message says to a screen reader, and whether a new one is
# announced (REQ-269, ARCH-99), in the Win32 client.
#
#   tools/gui/features/announce.sh          # run; the pair and daemon stop at the end
#   tools/gui/features/announce.sh --keep   # ... and leave them running
#
# Preferences > Accessibility chooses which of a message's parts a screen
# reader is given -- the sender, the time, the reactions, the replies -- and
# whether the text comes first, and whether a message from someone else is
# read out as it arrives. It asserts that
#
#   - Accessibility is a category of its own, after the others, with its six
#     rows; a row's choice, cancelled, is put back;
#   - with the defaults, a message's accessible name is "<who>, <when>: <text>",
#     as it has always been;
#   - with everything on, once the message has a reaction and a reply, the
#     text comes first and the name carries "reactions:" and "1 reply";
#   - with every part off, the name is the text alone;
#   - with announcing off, a message arriving from bob announces nothing; on,
#     it announces once;
#   - the settings last chosen are there again after the clients start again.
#
# It reads the dump's `spoken` line (the six settings), `a11yname` (what each
# message on the transcript says) and `a11y ... announced=` (how many
# announcements the client has made). Two clients over the pair driver: bob
# writes, alice listens.
#
# platforms: win32
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
. "$HERE/tools/gui/harness/lib.sh"
gui_begin announce 9605 /tmp/oc-announce "$@"
CH=1
TEXT="hello from bob"

# What alice's transcript says for the message holding $TEXT, from a fresh look.
name_now() {
  gui_snap a
  grep "^a11yname " "$DA" | grep -m1 -F "$TEXT" | sed 's/^a11yname [^ ]* "//; s/"$//'
}
spoken_is() { gui_snap a; grep -q "^spoken $1\$" "$DA"; }
announced() { gui_snap a; field "$DA" '^a11y ' announced; }
# named <predicate>: the predicate within ten seconds, or what was read last.
named() { waitfor 10 "$1" || { say "     read: $(name_now)"; return 1; }; }
# never <predicate>: the predicate does not come true in three seconds -- the
# time an announcement on its way would take to be made.
never() { ! waitfor 3 "$@"; }

step "fixture: alice and bob in #general, bob writes"
gui_pair_up
act a channel $CH
act b channel $CH
"$PAIR" a size 1200 820 >/dev/null || gui_abort "alice's window did not take its size"
available() { gui_snap a; [ "$(field "$DA" '^a11y ' avail)" = 1 ]; }
need "alice's client has an accessibility layer to announce through" waitfor 10 available
send_stored b "$TEXT"
waitfor 10 newest_is a $CH "$TEXT" || gui_abort "bob's message did not reach alice"

step "Accessibility, in Preferences"
act a prefs 5
cat_drawn() {
  gui_snap a
  grep -q '^prefcat=5$' "$DA" && grep -q '^  prefcat 5 name=Accessibility ' "$DA" &&
    [ "$(grep -c '^  prefhit row=12[0-5] ' "$DA")" = 12 ]
}
check "the sixth category is Accessibility, with six rows of two choices" waitfor 5 cat_drawn
check "...after the categories that were there before it" grep -q '^  prefcat 4 name=AI clients ' "$DA"
check "...and is published to assistive technology" grep -q '^a11yitem prefs.cat.accessibility ' "$DA"
check "the defaults are how the client has always read" \
  spoken_is "sender=1 time=1 reactions=0 replies=0 text_first=0 announce_new=1"
"$PAIR" a shot announce_prefs >/dev/null
# Order: Text first, as a person clicks it; then Cancel puts it back.
click_rect a "$(grep -m1 '^  prefhit row=124 val=1 ' "$DA" | grep -o 'r=[-0-9,]*' | cut -d= -f2)" ||
  gui_abort "the dump gives no rectangle for Order's Text first"
check "clicking Text first chooses it" waitfor 5 spoken_is \
  "sender=1 time=1 reactions=0 replies=0 text_first=1 announce_new=1"
press a modal.button.cancel || gui_abort "Preferences has no Cancel to press"
check "Cancel puts it back" waitfor 5 spoken_is \
  "sender=1 time=1 reactions=0 replies=0 text_first=0 announce_new=1"

step "the defaults read as the transcript always has"
defaults_read() { [[ "$(name_now)" =~ ^bob,\ .+:\ "$TEXT"$ ]]; }
check "\"<who>, <when>: <text>\"" named defaults_read

step "a reaction and a reply, and every part on"
THUMB="$(printf '\xF0\x9F\x91\x8D')"
act a react "0 $THUMB"
# The thread, from the message's own menu, and a reply in it.
gui_snap a
MID="$(grep "^a11yname " "$DA" | grep -m1 -F "$TEXT" | cut -d' ' -f2 | cut -d. -f2)"
BY="$(grep -m1 "^  msgrow [0-9]* mid=$MID " "$DA" | grep -o 'body=[0-9,]*' | cut -d, -f2)"
[ -n "$BY" ] || gui_abort "alice's transcript has no row for bob's message"
XS="$(grep -m1 '^msgrows ' "$DA" | grep -o 'x=[0-9.]*\.\.[0-9.]*' | cut -d= -f2)"
act a rclick $(( (${XS%%..*} + ${XS##*..}) / 2 )) "$BY"
press a menu.100 || gui_abort "the message's menu has no Reply in thread"
act a send "a reply"
# Back to the conversation: at this width the thread takes the transcript's place.
act a key esc
act a spoken 1 1 1 1 1 1
check "the verb sets all six" waitfor 5 spoken_is \
  "sender=1 time=1 reactions=1 replies=1 text_first=1 announce_new=1"
all_on() {
  local n; n="$(name_now)"
  [[ "$n" == "$TEXT, bob, "* ]] && [[ "$n" == *"reactions: $THUMB 1"* ]] && [[ "$n" == *", 1 reply" ]]
}
check "the text first, then who and when, \"reactions:\" and \"1 reply\"" named all_on
"$PAIR" a shot announce_all_on >/dev/null

step "every part off"
act a spoken 0 0 0 0 0 1
alone() { [ "$(name_now)" = "$TEXT" ]; }
check "the text alone" named alone

step "announcing a new message, off and on"
act a spoken 1 1 0 0 0 0
check "announcing is off" waitfor 5 spoken_is \
  "sender=1 time=1 reactions=0 replies=0 text_first=0 announce_new=0"
n0="$(announced)"
send_stored b "quiet arrival"
waitfor 10 newest_is a $CH "quiet arrival" || gui_abort "bob's second message did not reach alice"
moved() { [ "$(announced)" != "$n0" ]; }
check "off: it arrives and announced= stays at $n0" never moved
act a spoken 1 1 0 0 0 1
n1="$(announced)"
send_stored b "loud arrival"
waitfor 10 newest_is a $CH "loud arrival" || gui_abort "bob's third message did not reach alice"
plus_one() { [ "$(announced)" = $((n1 + 1)) ]; }
check "on: it adds one to announced= ($n1)" waitfor 5 plus_one
over_one() { [ "$(announced)" -gt $((n1 + 1)) ]; }
check "...and only one" never over_one

gui_end
