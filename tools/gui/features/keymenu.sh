#!/usr/bin/env bash
# The keyboard's route to the context menu in the Win32 client (REQ-264).
#
#   tools/gui/features/keymenu.sh          # run; the client and daemon stop at the end
#   tools/gui/features/keymenu.sh --keep   # ... and leave them running
#
# Pin, save, forward, copy link, edit, delete, mark unread and react were
# reachable only by right-clicking, so without a pointer the richest action set
# in the app could not be reached at all. It asserts that
#
#   - Ctrl+Down puts the keyboard on a message, and Ctrl+Up/Ctrl+Down move it
#     between messages;
#   - Shift+F10 opens THAT message's actions, with the first item already
#     highlighted so Enter means something;
#   - the arrows move the highlight through the menu and Enter runs the item that
#     is highlighted -- proven by its EFFECT, not by the menu closing: "Copy link"
#     puts a link to THAT message on the clipboard, pasted back to be read;
#   - Esc closes the menu, and Esc again takes the keyboard off the row;
#   - with no message focused, Shift+F10 opens the conversation's own menu;
#   - with a message focused, the single keys e r t p f s u m l each run their
#     menu item on it (`kbfocus ran=`) and are never typed into the composer,
#     each read back by its effect; with Ctrl held a letter is its shortcut, and
#     with no message focused it is typed;
#   - "Unread from here" moves the "New" line to that message at once (REQ-235),
#     without the conversation having to be opened again.
#
# Its own daemon on its own port.
#
# platforms: win32
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
. "$HERE/tools/gui/harness/lib.sh"
gui_begin keymenu 9610 /tmp/oc-keymenu "Keymenu Fixture" "$@"

MENU_MSG=6            # the enum's value, as the dump prints it
MENU_CHANNEL=8
CMD_COPY_LINK=105     # "Copy link" -- its effect can be read back
CMD_UNREAD=107        # "Unread from here"
CMD_REMIND_FIRST=115  # "In 20 minutes", the first of Remind me's choices

# Every key press, with its ACK CHECKED. gui_drive exits non-zero when the client
# did not answer within ten seconds, and a dropped verb that nobody notices
# reports itself as the next assertion failing -- a key that never arrived looks
# exactly like a key that did nothing. This is the whole script's key path, so a
# drop stops the script where it happened.
k() { act key "$1"; }

# The dump: an unanswered `dump` leaves an empty $D (gui_snap), so nothing read
# after it is an earlier look's.
kb()   { gui_snap; field "$D" '^kbfocus ' "$1"; }
top()  { gui_snap; grep -m1 "^$1=" "$D" | cut -d= -f2 | cut -d' ' -f1; }
menu() { gui_snap; grep -m1 '^menu=' "$D" | grep -o '^menu=[0-9-]*' | cut -d= -f2; }

step "three messages, and no pointer"
gui_launch
"$DRIVE" size 1100 800 >/dev/null
"$DRIVE" channel 1 >/dev/null || gui_abort "the client did not open the channel"
# One at a time, each back from the daemon before the next.
for i in 1 2 3; do send_stored "message number $i"; done
# The pointer is parked off every row, so nothing here can be the mouse's doing.
"$DRIVE" move 20 20 >/dev/null
three_rows() { gui_snap; [ "$(field "$D" '^msgrows ' n)" -ge 3 ] 2>/dev/null; }
check "three messages are drawn" waitfor 10 three_rows

step "Ctrl+Down puts the keyboard on a message"
nofocus() { [ "$(kb mid)" = 0 ]; }
check "nothing is focused to begin with" nofocus
k ctrl+down
focused() { [ "$(kb mid)" != 0 ]; }
check "Ctrl+Down focuses one" waitfor 5 focused
newest="$(kb mid)"
k ctrl+up
moved() { [ "$(kb mid)" != "$newest" ] && [ "$(kb mid)" != 0 ]; }
check "Ctrl+Up moves to the one before it" waitfor 5 moved
older="$(kb mid)"
k ctrl+down
back() { [ "$(kb mid)" = "$newest" ]; }
check "...and Ctrl+Down comes back" waitfor 5 back
"$DRIVE" shot keymenu_focus >/dev/null

step "Shift+F10 opens that message's actions"
# Put something ELSE on the clipboard first. Windows keeps it across runs, so a
# link copied by an earlier run would satisfy the paste check below whether or not
# this run ever copied anything -- two checks passing on yesterday's work.
"$DRIVE" typekeys "clipboard-was-here" >/dev/null || gui_abort "the client did not take the text to copy"
k ctrl+a
k ctrl+c
k ctrl+a
k delete
seeded() { k ctrl+v; gui_snap; grep -m1 '^ed ' "$D" | grep 'clipboard-was-here' >/dev/null; }
check "the clipboard starts with something that is not a link" waitfor 5 seeded
k ctrl+a
k delete
k shift+f10
msg_menu() { [ "$(menu)" = "$MENU_MSG" ]; }
check "the message menu is open" waitfor 5 msg_menu
ready() { [ "$(kb menuhover)" -ge 0 ] 2>/dev/null; }
check "...with an item already highlighted, so Enter means something" waitfor 5 ready
"$DRIVE" shot keymenu_open >/dev/null

step "the arrows walk it and Enter runs what is highlighted"
first="$(kb menuhovercmd)"
k down
stepped() { [ "$(kb menuhovercmd)" != "$first" ]; }
check "Down moves the highlight" waitfor 5 stepped
k up
stepped_back() { [ "$(kb menuhovercmd)" = "$first" ]; }
check "...and Up moves it back" waitfor 5 stepped_back
# Walk to "Copy link" and choose it. What it did is then READ BACK: the link goes
# on the clipboard, so pasting it into the composer says both that the highlighted
# item ran and that it ran on the focused message -- the id is in the link. A menu
# that merely closed would prove neither.
found=0
for _ in $(seq 1 30); do
  [ "$(kb menuhovercmd)" = "$CMD_COPY_LINK" ] && { found=1; break; }
  k down
done
check "the menu offers \"Copy link\" to the keyboard" [ "$found" = 1 ]
k enter
closed() { [ "$(menu)" = 0 ]; }
check "Enter closes the menu" waitfor 5 closed
said() { gui_snap; grep -qi 'toast\[[0-9]\].*[Ll]ink copied' "$D"; }
check "...and it says the link was copied" waitfor 5 said
k ctrl+a
k delete
k ctrl+v
# The link ends in the message's id: text="openchime://<host>/c/<c>/m/<id>".
pasted() { gui_snap; grep -m1 '^ed ' "$D" | grep "/m/$newest\"" >/dev/null; }
has_link() { gui_snap; grep -m1 '^ed ' "$D" | grep -i 'openchime://' >/dev/null; }
check "the clipboard holds a link" waitfor 5 has_link
check "...to the message the keyboard was on ($newest)" pasted
k ctrl+a
k delete

step "Esc takes the keyboard back off"
k shift+f10
check "the menu opens again" waitfor 5 msg_menu
k esc
check "Esc closes it" waitfor 5 closed
check "the keyboard is still on the message" focused
k esc
check "Esc again takes it off the row" waitfor 5 nofocus

step "single keys act on the message the keyboard is on"
# Each letter runs its menu item on the focused message, read back from the
# dump's `kbfocus ran=` (the command it ran), and is never typed: `keychar`
# sends the key AND the text the platform sends for it, as a person's press
# does, so a letter that leaked into the composer would show in its text.
ran()    { [ "$(kb ran)" = "$1" ]; }
edtext() { gui_snap; fieldq "$D" '^ed ' text; }
untyped() { [ "$(edtext)" = "" ]; }
# On the newest message again, from wherever the last key left the keyboard.
on_newest() {
  local i
  for i in 1 2 3; do [ "$(kb mid)" = 0 ] && break; k esc; done
  k ctrl+down
  waitfor 5 back
}
# press <letter> <command>: on the newest message, the letter runs <command>
# and leaves the composer as it was.
press1() {
  act keychar "$1"
  check "'$1' runs its item ($2)" waitfor 5 ran "$2"
  check "...and is not typed into the composer" untyped
}
check "the composer is empty to begin with" untyped
need "the keyboard is on the newest message" on_newest
press1 p 103
press1 s 104
saved() { gui_snap; grep -qi 'toast\[[0-9]\].*[Ss]aved to Later' "$D"; }
check "'s' saves it for later" waitfor 5 saved
press1 u 107
unread_newest() { [ "$(top unread_from)" = "$older" ]; }
check "'u' moves the \"New\" line to this message" waitfor 5 unread_newest
press1 l 105
# What it copied is read back: a link to THIS message, pasted into the composer.
k ctrl+v
check "'l' puts this message's link on the clipboard" waitfor 5 pasted
k ctrl+a
k delete
check "the composer is emptied again" waitfor 5 untyped
press1 P 103
check "upper case is the same key: 'P' unpins it again" focused
press1 m 114
remind_open() { [ "$(menu)" = "$MENU_MSG" ] && [ "$(kb menuhovercmd)" = "$CMD_REMIND_FIRST" ]; }
check "'m' opens the reminder choices, the first highlighted" waitfor 5 remind_open
k esc
check "Esc closes them" waitfor 5 closed
press1 r 7
picker_up() { gui_snap; [ "$(field "$D" '^pickstate ' open)" = 1 ]; }
check "'r' opens the emoji picker" waitfor 5 picker_up
picker_down() { gui_snap; [ "$(field "$D" '^pickstate ' open)" = 0 ]; }
for _ in 1 2 3; do picker_down && break; k esc; done
check "Esc closes the picker" waitfor 5 picker_down
need "the keyboard is back on the newest message" on_newest
press1 f 106
pal_up() { gui_snap; [ "$(field "$D" '^natives ' pal)" = 1 ]; }
pal_down() { gui_snap; [ "$(field "$D" '^natives ' pal)" = 0 ]; }
# Esc takes the keyboard off the message and dismisses each notice the keys
# above raised before it reaches the palette, so it is pressed until it does.
check "'f' opens the palette to pick where it goes" waitfor 5 pal_up
for _ in $(seq 1 8); do pal_down && break; k esc; done
check "Esc closes the palette" waitfor 5 pal_down
need "the keyboard is back on the newest message" on_newest
act keychar t
check "'t' runs its item (100)" waitfor 5 ran 100
thread_open() { gui_snap; [ "$(field "$D" '^hist ' thread)" = "$newest" ]; }
check "...and opens this message's thread" waitfor 10 thread_open
check "...and leaves the keyboard to the reply" nofocus
check "...with nothing typed" untyped
k esc
thread_shut() { gui_snap; [ "$(field "$D" '^hist ' thread)" = 0 ]; }
check "Esc closes the thread" waitfor 5 thread_shut
need "the keyboard is back on the newest message" on_newest
act keychar e
check "'e' runs its item (21)" waitfor 5 ran 21
editing() { [ "$(edtext)" = "message number 3" ]; }
check "...and the composer holds the message to edit, and no 'e'" waitfor 5 editing
check "...and leaves the keyboard to the edit" nofocus
k esc
check "Esc cancels the edit" waitfor 5 untyped
need "the keyboard is back on the newest message" on_newest
k ctrl+k
check "with Ctrl held, a letter is still its shortcut (Ctrl+K)" waitfor 5 pal_up
check "...and runs no single key" ran 0
for _ in $(seq 1 8); do pal_down && break; k esc; done
check "Esc closes the palette" waitfor 5 pal_down
for _ in 1 2 3; do nofocus && break; k esc; done
check "the keyboard is off the messages" nofocus
act keychar p
typed() { [ "$(edtext)" = "p" ]; }
check "with no message focused, 'p' is typed into the composer" waitfor 5 typed
k ctrl+a
k delete
check "...and taken out again" waitfor 5 untyped
"$DRIVE" shot keymenu_keys >/dev/null

step "\"Unread from here\" shows where reading resumes"
k ctrl+down
k ctrl+up
on_older() { [ "$(kb mid)" = "$older" ]; }
check "...the one before the newest ($older)" waitfor 5 on_older
k shift+f10
check "its menu opens" waitfor 5 msg_menu
found=0
for _ in $(seq 1 30); do
  [ "$(kb menuhovercmd)" = "$CMD_UNREAD" ] && { found=1; break; }
  k down
done
check "the menu offers \"Unread from here\"" [ "$found" = 1 ]
k enter
check "choosing it closes the menu" waitfor 5 closed
marked() { gui_snap; grep -qi 'toast\[[0-9]\].*[Mm]arked unread' "$D"; }
check "...and says so" waitfor 5 marked
# The divider sits after the message before this one: the "from" is that
# earlier message's id -- set now, in the open conversation.
from_ok() { local f; f="$(top unread_from)"; [ -n "$f" ] && [ "$f" -gt 0 ] && [ "$f" -lt "$older" ]; }
check "the \"New\" line moves to this message at once" waitfor 5 from_ok
"$DRIVE" shot keymenu_unread >/dev/null
k esc

step "with no message focused, it is the conversation's menu"
k shift+f10
chan_menu() { [ "$(menu)" = "$MENU_CHANNEL" ]; }
check "the conversation's own menu opens" waitfor 5 chan_menu
k esc
gui_end
