#!/usr/bin/env bash
# The New message pane (REQ-229), driven: does the field that LOOKS focused get
# the keys, does the message go where the pane says, and does what you typed
# survive leaving the pane?
#
#   scripts/gui_newmsg.sh
#
# It exists because 35 defects were found in this pane by reading it, and the two
# that mattered most were invisible to every other harness: characters typed into
# the To: field were applied to the message body behind it, and Enter in the
# message sent it to the channel that happened to be open before. Both look
# perfectly normal on screen. The dump's `newmsg` line (focus= chips= q= caret=
# sel= matches= body= pending=) is what makes them assertable.
#
# NOT IN CI, for the reason gui_smoke.sh gives: the daemon is Linux-only and
# GitHub's Windows runners cannot host it. Run it before pushing a change to the
# pane, the picker or the composer's key routing.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DRIVE="$HERE/scripts/gui_drive.sh"
LIN_DIR='/mnt/c/Windows/Temp/octest'

# Its own daemon, port and directory, wiped each run.
export OC_DEV_PORT="${OC_DEV_PORT:-9520}"
export OC_DEV_DIR="${OC_DEV_DIR:-/tmp/oc-newmsg}"
export OC_DEV_WS="${OC_DEV_WS:-Newmsg Fixture}"

fails=0
checks=0
say()  { printf '%s\n' "$*"; }
fail() { printf '  FAIL %s\n' "$*"; fails=$((fails + 1)); checks=$((checks + 1)); }
ok()   { printf '  ok   %s\n' "$*"; checks=$((checks + 1)); }

kill_dev_daemon() {
  local p
  for p in $(pgrep -x openchimed 2>/dev/null); do
    tr '\0' '\n' < "/proc/$p/environ" 2>/dev/null |
      grep -qx "OPENCHIME_PROTO_PORT=$OC_DEV_PORT" && kill "$p" 2>/dev/null
  done
  for _ in 1 2 3 4 5 6 7 8 9 10; do
    (exec 3<>/dev/tcp/127.0.0.1/"$OC_DEV_PORT") 2>/dev/null || return 0
    exec 3<&- 3>&-; sleep 0.3
  done
  return 1
}
if ! kill_dev_daemon; then
  say "a daemon is still listening on :$OC_DEV_PORT — refusing to wipe underneath it."
  exit 1
fi
rm -rf "$OC_DEV_DIR"

drive() { "$DRIVE" "$@" >/dev/null 2>&1; }
# A press: down AND up, as a keyboard does. The `key` verb sends the down only,
# deliberately, so a held key can be tested -- and a handler that re-arms on the
# release then ignores every press after the first. Backspace in the To: field is
# one: it takes one recipient per press and re-arms on WM_KEYUP, so a run that
# never released it deleted one chip and silently no-opped every Backspace after.
# Anything pressed more than once in a run goes through this.
press() { drive key "$1"; drive keyup "$1"; }
snap()  { "$DRIVE" dump nm >/dev/null 2>&1; cat "$LIN_DIR/nm.txt" 2>/dev/null; }
key_of()    { printf '%s' "$1" | grep -o "\b$2=[^ ]*" | head -1 | cut -d= -f2; }
quoted_of() { printf '%s' "$1" | grep -o "\b$2=\"[^\"]*\"" | head -1 | sed "s/^$2=\"//; s/\"\$//"; }
nm_of()     { printf '%s' "$1" | grep -o "^newmsg .*" | grep -o "\b$2=[^ ]*" | head -1 | cut -d= -f2; }
nmq_of()    { printf '%s' "$1" | grep -o "^newmsg .*" | grep -o "\b$2=\"[^\"]*\"" | head -1 | sed "s/^$2=\"//; s/\"\$//"; }
# How many recipients the pane holds. The dump joins them with commas, so an
# empty list is 0 and "@bob,@carol" is 2.
nchips_of() { local c; c="$(nmq_of "$1" chips)"; [ -z "$c" ] && { echo 0; return; }
              printf '%s' "$c" | tr ',' '\n' | grep -c . ; }

wait_for() {
  local pred="$1" i
  for i in $(seq 1 40); do
    d="$(snap)"
    [ -n "$d" ] && "$pred" && return 0
    sleep 0.25
  done
  return 1
}

say "build"
make -C "$HERE" >/dev/null 2>&1 || { say "  FAIL daemon build"; exit 1; }
make -C "$HERE" windows-gui >/dev/null 2>&1 || { say "  FAIL gui build"; exit 1; }

OC_DRIVE_NO_BUILD=1 "$DRIVE" launch "127.0.0.1:$OC_DEV_PORT" "alice:pw" >/dev/null 2>&1
signed_in() { [ "$(key_of "$d" authed)" = "1" ]; }
wait_for signed_in || { say "  FAIL never signed in"; exit 1; }

# --- 1. the keys go where the caret is -----------------------------------------
say "1. the field that looks focused gets the keys"
drive view newmsg
d="$(snap)"
[ "$(nm_of "$d" focus)" = "to" ] && ok "opens with the To: field focused" \
  || fail "opened focused on '$(nm_of "$d" focus)'"
drive typekeys "bob"
d="$(snap)"
[ "$(nmq_of "$d" q)" = "bob" ] && ok "typing lands in the To: query" \
  || fail "the To: query holds '$(nmq_of "$d" q)' after typing bob"
[ "$(nm_of "$d" body)" = "0" ] && ok "and NOT in the message body" \
  || fail "$(nm_of "$d" body) characters leaked into the message body"
drive key enter
d="$(snap)"
[ "$(nmq_of "$d" chips)" = "@bob" ] && ok "Enter accepts the highlighted match" \
  || fail "chips are '$(nmq_of "$d" chips)' after Enter"
[ "$(nmq_of "$d" q)" = "" ] && ok "and clears the query" || fail "query left as '$(nmq_of "$d" q)'"

# The message box, reached with Tab, takes the characters — and the To: field
# stops taking them. This is the defect a person found by typing.
drive key tab
d="$(snap)"
[ "$(nm_of "$d" focus)" = "body" ] && ok "Tab moves to the message" \
  || fail "Tab left focus on '$(nm_of "$d" focus)'"
drive typekeys "hello there"
d="$(snap)"
[ "$(nm_of "$d" body)" = "11" ] && ok "typing lands in the message" \
  || fail "the message holds $(nm_of "$d" body) characters, not 11"
[ "$(nmq_of "$d" q)" = "" ] && ok "and not in the To: query" \
  || fail "the To: query took '$(nmq_of "$d" q)' while the message had focus"
drive key shift+tab
d="$(snap)"
[ "$(nm_of "$d" focus)" = "to" ] && ok "Shift+Tab goes back to the recipients" \
  || fail "Shift+Tab left focus on '$(nm_of "$d" focus)' — a keyboard trap"

# The keys that are NOT characters are the ones that used to leak: with the To:
# field focused, Delete deleted from the message, Ctrl+A selected it and Ctrl+V
# pasted into it — none of it visible, because the message was not on screen.
before_body="$(nm_of "$d" body)"
powershell.exe -NoProfile -Command "Set-Clipboard -Value 'PASTED'" >/dev/null 2>&1
drive key delete
drive key ctrl+v
d="$(snap)"
[ "$(nm_of "$d" body)" = "$before_body" ] \
  && ok "Delete, Ctrl+V and Ctrl+A do not reach the message" \
  || fail "the message changed from $before_body to $(nm_of "$d" body) characters while the To: field had the keys"
[ "$(nmq_of "$d" q)" = "PASTED" ] && ok "Ctrl+V pastes into the To: field" \
  || fail "Ctrl+V put '$(nmq_of "$d" q)' in the To: field"
drive key ctrl+a
drive key delete
d="$(snap)"
[ "$(nmq_of "$d" q)" = "" ] && ok "Ctrl+A then Delete clears the To: field" \
  || fail "the To: field still holds '$(nmq_of "$d" q)'"
drive key tab

# The list FLOATS over the composer, so the composer must not take its clicks:
# it did, and clicking a match put the caret in the message box instead of adding
# the recipient.
say "1b. a match is chosen by clicking it"
drive key shift+tab
drive key ctrl+a
drive key delete
press backspace              # drop the chip from case 1, so the list has room
drive typekeys "car"
# The rects come from the PAINT, so wait for the list to be drawn rather than
# reading the frame before it.
row_drawn() { [ "$(printf '%s' "$d" | grep -m1 '^newmsgrow0' | cut -d' ' -f2)" != "0,0,0,0" ]; }
wait_for row_drawn
row="$(printf '%s' "$d" | grep -m1 '^newmsgrow0' | cut -d' ' -f2)"
rx=$(( ( $(printf '%s' "$row" | cut -d, -f1) + $(printf '%s' "$row" | cut -d, -f3) ) / 2 ))
ry=$(( ( $(printf '%s' "$row" | cut -d, -f2) + $(printf '%s' "$row" | cut -d, -f4) ) / 2 ))
before_chips="$(nmq_of "$d" chips)"
[ "$rx" -gt 0 ] && [ "$ry" -gt 0 ] && ok "the list is on screen with a row to click" \
  || fail "no suggestion row is drawn (rect '$row')"
drive click "$rx $ry"
d="$(snap)"
[ "$(nmq_of "$d" chips)" != "$before_chips" ] && ok "clicking a match adds it: $(nmq_of "$d" chips)" \
  || fail "clicking the match at $rx,$ry changed nothing (chips '$(nmq_of "$d" chips)')"
[ "$(nm_of "$d" focus)" = "to" ] && ok "and the keys stay in the To: field" \
  || fail "the click moved focus to '$(nm_of "$d" focus)' — it fell through to the composer"

# --- 2. the To: field is a text field ------------------------------------------
say "2. the To: field edits like a text field"
drive key shift+tab
drive typekeys "carol"
drive key left
drive key left
d="$(snap)"
[ "$(nm_of "$d" caret)" = "3" ] && ok "Left moves the caret inside the query" \
  || fail "caret is at $(nm_of "$d" caret) after two Lefts in 'carol'"
# "carol" with the caret between r and o: Backspace takes the r, not the l.
press backspace
d="$(snap)"
[ "$(nmq_of "$d" q)" = "caol" ] && ok "Backspace deletes at the caret, not at the end" \
  || fail "query is '$(nmq_of "$d" q)' after a mid-string Backspace"
drive key esc
d="$(snap)"
[ "$(nmq_of "$d" q)" = "" ] && ok "Escape clears the query" || fail "query survived Escape"

# --- 3. the message goes where the pane says -----------------------------------
say "3. Enter sends to the recipients, not to the last channel"
# A message in #general first, so g_sel names a channel a wrong send would reach.
drive view home
drive channel general
drive type "in the channel"
drive key enter
drive view newmsg
drive typekeys "bob"
drive key enter
drive key tab
drive typekeys "for bob only"
drive key enter
sent_to_bob() { [ "$(nm_of "$d" pending)" = "0" ] && [ "$(nm_of "$d" body)" = "" ]; }
d="$(snap)"
sleep 1
d="$(snap)"
gen="$(printf '%s' "$d" | grep 'ch [0-9]* "general"' | head -1)"
gmsgs="$(printf '%s' "$gen" | grep -o 'msgs=[0-9]*' | cut -d= -f2)"
[ "$gmsgs" = "1" ] && ok "#general still holds only its own message" \
  || fail "#general holds $gmsgs messages — the new message went to the wrong conversation"
case "$gen" in
  *'for bob only'*) fail "#general's newest message is the one addressed to bob" ;;
  *) ok "and its newest message is not the addressed one" ;;
esac
dm="$(printf '%s' "$d" | grep 'ch [0-9]* DM' | head -1)"
case "$dm" in
  *'for bob only'*) ok "the DM received it" ;;
  *) fail "the DM did not receive it (dm row: $dm)" ;;
esac

# --- 4. leaving and coming back keeps both halves ------------------------------
say "4. the draft keeps the words AND the people"
drive view newmsg
drive key ctrl+a
drive key delete
drive typekeys "bob"
drive key enter
drive key tab
drive typekeys "half written"
sleep 3                      # the debounce writes it
drive view home
drive view newmsg
d="$(snap)"
[ "$(nm_of "$d" body)" = "12" ] && ok "the words come back" \
  || fail "the message came back with $(nm_of "$d" body) characters, not 12"
[ "$(nmq_of "$d" chips)" = "@bob" ] && ok "the people come back" \
  || fail "recipients came back as '$(nmq_of "$d" chips)'"

# --- 5. nothing is written into another conversation's draft -------------------
# `drafthere` is whether the SELECTED conversation has a draft. The pane's text
# used to be filed under whatever was open before, on every way out of it.
say "5. the pane's text is nobody else's draft"
drive view home
drive channel general
d="$(snap)"
[ "$(key_of "$d" drafthere)" = "0" ] && ok "#general has no draft of the new message" \
  || fail "the unaddressed message was filed as #general's draft"
[ "$(key_of "$d" draftn)" = "1" ] && ok "exactly one draft exists — the pane's own" \
  || fail "$(key_of "$d" draftn) drafts exist, not 1"

# --- 6. Backspace takes one recipient per press ---------------------------------
# The rule the client enforces on the key's release, which is why a press in this
# script is a press and not a hold. Two recipients, so taking too many shows.
say "6. Backspace takes one recipient per press"
drive view newmsg
drive typekeys "carol"
drive key enter              # the top match, beside the restored @bob
d="$(snap)"
[ "$(nchips_of "$d")" = "2" ] && ok "two recipients: $(nmq_of "$d" chips)" \
  || fail "the pane holds '$(nmq_of "$d" chips)', not two recipients"
press backspace
d="$(snap)"
[ "$(nchips_of "$d")" = "1" ] && ok "one press takes one: $(nmq_of "$d" chips)" \
  || fail "one Backspace left '$(nmq_of "$d" chips)'"
# Two again, and now the key is HELD -- pressed twice without a release. The
# second recipient must survive, which is what the latch is for: a held key used
# to walk backwards through the whole list and take the people behind the one
# aimed at.
drive typekeys "carol"
drive key enter
d="$(snap)"
[ "$(nchips_of "$d")" = "2" ] && ok "two again, to hold the key against" \
  || fail "the pane holds '$(nmq_of "$d" chips)', not two"
drive key backspace
drive key backspace
d="$(snap)"
[ "$(nchips_of "$d")" = "1" ] && ok "a held Backspace takes only the one it started on: $(nmq_of "$d" chips)" \
  || fail "holding Backspace left $(nchips_of "$d") recipients ('$(nmq_of "$d" chips)'), not 1"
drive keyup backspace
press backspace              # released and re-armed: the last one goes

# --- 7. sending needs a recipient, and says so ---------------------------------
say "7. a refusal says why"
# The recipients are gone BEFORE Enter is pressed, asserted here: this check used
# to run with the restored recipient still attached, so the message was sent and
# whether the check passed depended on which dump line was read first.
d="$(snap)"
[ "$(nchips_of "$d")" = "0" ] && ok "nobody is addressed" \
  || fail "'$(nmq_of "$d" chips)' is still addressed -- the refusal cannot be tested"
drive key tab
drive typekeys "nobody is addressed"
before_body="$(nm_of "$(snap)" body)"
drive key enter
d="$(snap)"
[ "$(nm_of "$d" body)" = "$before_body" ] && ok "the message is still in the box ($before_body characters)" \
  || fail "the message went from $before_body characters to $(nm_of "$d" body) with nobody to send it to"
case "$d" in
  *'Who is this for?'*) ok "and a toast says why" ;;
  *) fail "nothing said why the send did not happen" ;;
esac

say ""
say "$checks checks, $fails failed"
[ "$fails" -eq 0 ] || exit 1
