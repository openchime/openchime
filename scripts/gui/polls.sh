#!/usr/bin/env bash
# Polls, made and changed in the poll composer and voted in (REQ-225).
#
#   scripts/gui/polls.sh          # run, and leave the pair running
#   scripts/gui/polls.sh --down   # ... and take it down afterwards
#
# A poll is a message whose body is its question, with a card of options under
# it. The composer makes one and changes one: a field for the question, one per
# option with rows added and removed, two checks, and a deadline picked as a
# day and a time to the minute in local time. It asserts that
#
#   - the composer posts a poll: the question, options added and removed a row
#     at a time, and a deadline at an odd minute two days off, which the card
#     carries to the millisecond the local time names; both clients draw it;
#   - its author can change it before anyone votes -- the question, and an
#     option more -- and both clients see the change; nobody else is offered to;
#   - a vote from one client moves the count on the other, which does not learn
#     whose it was, and marks the option on the voter's own;
#   - pressing an option row votes, pressing it again takes the vote back, and
#     each row is published to assistive technology;
#   - "See who voted" lists the people and what they picked;
#   - once anyone has voted, the composer shows the options and the checks but
#     takes no change to them, and the deadline still moves;
#   - a poll of several choices and no names says so, and offers no list;
#   - Close poll now closes it: no row takes a vote any more, on either client;
#   - one option, no question or a deadline already past is refused at its
#     field, and nothing is posted.
#
# Two clients over the pair (scripts/gui/pair.sh, or scripts/web/pair.sh on the
# web): alice makes the polls, bob votes in them.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
. "$HERE/scripts/gui/lib.sh"

export OC_TEST_PORT="${OC_TEST_PORT:-9710}"
export OC_TEST_DIR="${OC_TEST_DIR:-/tmp/oc-polls}"
CH=1

DA="/tmp/oc-polls-a.txt"
DB="/tmp/oc-polls-b.txt"
snap_a() { "$PAIR" a dump polls > "$DA" 2>&1; }
snap_b() { "$PAIR" b dump polls > "$DB" 2>&1; }
waitfor() {  # waitfor <secs> <command...>
  local secs="$1" t=0; shift
  while [ "$t" -lt $((secs * 4)) ]; do if "$@"; then return 0; fi; sleep 0.25; t=$((t + 1)); done
  return 1
}
# Field `k` of a dump line: line <file> <pattern> <k>
field() { grep -m1 -E "$2" "$1" | grep -o "\b$3=[^ ]*" | head -1 | cut -d= -f2 | tr -d '"'; }
polls_in() { grep -c '^  poll msg=' "$1"; }
newest_poll() { grep '^  poll msg=' "$1" | tail -1 | grep -o 'msg=[0-9]*' | cut -d= -f2; }
opt() {  # opt <file> <mid> <i> <field>
  field "$1" "^    pollopt msg=$2 i=$3 " "$4"
}
pc() { field "$DA" '^pollcomposer ' "$1"; }       # a field of alice's composer
row() { field "$DA" "^  pcrow i=$1 " "$2"; }      # ... and of one of its rows
ack_a() { "$PAIR" a "$@" 2>/dev/null | grep -o 'ack: [a-z]*' | head -1 | cut -d' ' -f2; }
# Press the centre of rect "l,t,r,b" on alice's client.
press() {
  local l t rr b
  IFS=, read -r l t rr b <<< "$1"
  [ -n "${b:-}" ] && [ "${rr:-0}" -gt 0 ] || return 1
  "$PAIR" a click $(( (l + rr) / 2 )) $(( (t + b) / 2 )) >/dev/null
}
# The instant a local date and time names, in ms, as the client computes it.
local_ms() { echo "$(date -d "$1 $2" +%s)000"; }

say "== fixture: alice and bob in one channel"
"$PAIR" down >/dev/null 2>&1
rm -rf "$OC_TEST_DIR"
"$PAIR" up || { say "pair did not come up"; exit 1; }
# Wide enough for the context pane the who-voted list lives in, and tall
# enough for the composer at ten rows.
"$PAIR" a size 1200 820 >/dev/null
"$PAIR" a channel $CH >/dev/null
"$PAIR" b channel $CH >/dev/null

say "== Create a poll, in the composer"
"$PAIR" a pollnew >/dev/null
composer_new() { snap_a; [ "$(pc open)" = 1 ] && [ "$(pc mode)" = new ] && [ "$(pc rows)" = 2 ]; }
check "the composer opens with two empty rows" waitfor 10 composer_new
check "...its controls published to assistive technology" \
  bash -c "grep -q '^a11yitem pollc.question ' $DA && grep -q '^a11yitem pollc.option.1 ' $DA && grep -q '^a11yitem pollc.add ' $DA"
"$PAIR" a polltype q Lunch today? >/dev/null
"$PAIR" a polltype 0 Tacos >/dev/null
"$PAIR" a polltype 1 Soup >/dev/null
"$PAIR" a polladd >/dev/null
"$PAIR" a polltype 2 Salad >/dev/null
three_rows() { snap_a; [ "$(pc rows)" = 3 ] && [ "$(row 2 text)" = Salad ]; }
check "Add option gives it a third row" waitfor 10 three_rows
check "...each row with a remove" [ -n "$(row 2 rm | grep -v '^0,0,0,0$')" ]
"$PAIR" a pollrm 1 >/dev/null
removed() { snap_a; [ "$(pc rows)" = 2 ] && [ "$(row 1 text)" = Salad ] && [ "$(row 0 rm)" = 0,0,0,0 ]; }
check "removing Soup moves Salad up, and two rows have no remove" waitfor 10 removed
# Re-added by pressing Add option, as a person does.
press "$(pc add)"
readded() { snap_a; [ "$(pc rows)" = 3 ]; }
check "pressing Add option adds the row back" waitfor 10 readded
"$PAIR" a polltype 2 Soup >/dev/null
DAY="$(date -d '+2 days' +%F)"
EXP="$(local_ms "$DAY" 14:37)"
"$PAIR" a polldeadline "$DAY 2:37 PM" >/dev/null
deadline_set() { snap_a; [ "$(pc dl)" = 1 ] && [ "$(pc deadline)" = "$EXP" ]; }
check "a deadline typed as 2:37 PM, two days off, is that local minute" waitfor 10 deadline_set
check "...and is said in words" bash -c "grep -qE '^pollcomposer .*when=\"Closes [A-Za-z]+day, .*(2:37 PM|14:37)\"' $DA"
check "...with the days to pick it from published" bash -c "grep -q '^a11yitem pollc.day.$(date -d "$DAY" +%Y%m%d) ' $DA"
"$PAIR" a shot polls_composer >/dev/null
press "$(pc submit)"
one_poll_a() { snap_a; [ "$(polls_in "$DA")" = 1 ] && [ "$(pc open)" = 0 ]; }
check "Post poll posts it, and alice's client draws it" waitfor 15 one_poll_a
MID="$(newest_poll "$DA")"
check "...with three options, nobody having voted, closing at that minute" \
  bash -c "grep -q '^  poll msg=$MID multi=0 anon=0 closes=$EXP closed=0 voters=0 mine=0 opts=3 ' $DA"
in_order() { [ "$(opt "$DA" "$MID" 0 label)" = Tacos ] && [ "$(opt "$DA" "$MID" 1 label)" = Salad ] \
               && [ "$(opt "$DA" "$MID" 2 label)" = Soup ]; }
check "...in the order typed: Tacos, Salad, Soup" in_order
one_poll_b() { snap_b; [ "$(polls_in "$DB")" = 1 ] && [ "$(newest_poll "$DB")" = "$MID" ]; }
check "bob's client draws the same poll" waitfor 15 one_poll_b
check "...and offers bob no Edit on alice's poll" [ "$(field "$DB" "^  poll msg=$MID " edit)" = 0,0,0,0 ]
# Published as it is drawn, which can be a frame after the poll arrives.
offers_edit() { snap_a; grep -q "^a11yitem poll.$MID.edit " "$DA"; }
check "alice's card offers her Edit" waitfor 10 offers_edit
"$PAIR" a shot polls_new >/dev/null

say "== alice changes it before anyone votes"
# Pressed where the card is NOW: the transcript can still settle (a read
# receipt arriving) between a dump and the click, so a miss is pressed again.
press_edit() { snap_a; [ "$(pc open)" = 1 ] || press "$(field "$DA" "^  poll msg=$MID " edit)"; snap_a; [ "$(pc open)" = 1 ]; }
waitfor 10 press_edit || true
composer_edit() { snap_a; [ "$(pc open)" = 1 ] && [ "$(pc mode)" = edit ] && [ "$(pc mid)" = "$MID" ] \
                    && [ "$(pc locked)" = 0 ] && [ "$(pc rows)" = 3 ] && [ "$(row 1 text)" = Salad ]; }
check "her Edit opens the composer on it, filled in" waitfor 10 composer_edit
check "...its deadline as it was" [ "$(pc deadline)" = "$EXP" ]
"$PAIR" a polltype q Lunch today, team? >/dev/null
"$PAIR" a polladd >/dev/null
"$PAIR" a polltype 3 Noodles >/dev/null
check "Save is taken" [ "$(ack_a pollsubmit)" = ok ]
changed_a() { snap_a; [ "$(field "$DA" "^  poll msg=$MID " opts)" = 4 ] && [ "$(opt "$DA" "$MID" 3 label)" = Noodles ] \
                && grep -q "^  poll msg=$MID .*question=\"Lunch today, team?\"" "$DA"; }
check "alice's client shows the new option and question" waitfor 15 changed_a
changed_b() { snap_b; [ "$(field "$DB" "^  poll msg=$MID " opts)" = 4 ] && [ "$(opt "$DB" "$MID" 3 label)" = Noodles ] \
                && grep -q "^  poll msg=$MID .*question=\"Lunch today, team?\"" "$DB"; }
check "...and so does bob's" waitfor 15 changed_b
check "...the deadline where it was" [ "$(field "$DB" "^  poll msg=$MID " closes)" = "$EXP" ]

say "== bob votes Soup"
"$PAIR" b vote "$MID 2" >/dev/null
bob_mine() { snap_b; [ "$(opt "$DB" "$MID" 2 mine)" = 1 ] && [ "$(opt "$DB" "$MID" 2 count)" = 1 ]; }
check "bob's client marks Soup his" waitfor 15 bob_mine
alice_sees() { snap_a; [ "$(opt "$DA" "$MID" 2 count)" = 1 ] && [ "$(opt "$DA" "$MID" 2 mine)" = 0 ]; }
check "alice's client counts it, and it is not hers" waitfor 15 alice_sees

say "== alice presses Tacos, then takes it back"
snap_a
R="$(opt "$DA" "$MID" 0 r)"
IFS=, read -r l t rr b <<< "$R"
on_screen() { [ -n "${b:-}" ] && [ "${rr:-0}" -gt 0 ]; }
check "the Tacos row is on screen" on_screen
check "...and published to assistive technology" bash -c "grep -q '^a11yitem poll.$MID.0 ' $DA"
"$PAIR" a click $(( (l + rr) / 2 )) $(( (t + b) / 2 )) >/dev/null
alice_tacos() { snap_a; [ "$(opt "$DA" "$MID" 0 mine)" = 1 ] && [ "$(opt "$DA" "$MID" 0 count)" = 1 ] \
                  && [ "$(field "$DA" "^  poll msg=$MID " voters)" = 2 ]; }
check "Tacos is hers, two people have voted" waitfor 15 alice_tacos
"$PAIR" a shot polls_voted >/dev/null
"$PAIR" a click $(( (l + rr) / 2 )) $(( (t + b) / 2 )) >/dev/null
alice_back() { snap_a; [ "$(opt "$DA" "$MID" 0 mine)" = 0 ] && [ "$(opt "$DA" "$MID" 0 count)" = 0 ]; }
check "pressed again, the vote is taken back" waitfor 15 alice_back

say "== See who voted"
"$PAIR" a vote "$MID 1" >/dev/null
# Asked once the vote has landed: the list is a snapshot, read beside the
# writer that stores the vote, so asking at once can miss it.
alice_salad() { snap_a; [ "$(opt "$DA" "$MID" 1 mine)" = 1 ]; }
waitfor 15 alice_salad || fail "alice's vote for Salad did not land"
"$PAIR" a voters "$MID" >/dev/null
listed() { snap_a; [ "$(field "$DA" '^voters ' open)" = 1 ] && [ "$(field "$DA" '^voters ' rows)" = 2 ] \
             && [ "$(field "$DA" '^voters ' pane)" = 1 ]; }
check "the pane lists two people" waitfor 15 listed
check "...bob for Soup" bash -c "grep -q '^  voter option=2 name=\"bob\"' $DA"
check "...alice for Salad" bash -c "grep -q '^  voter option=1 name=\"alice\"' $DA"
"$PAIR" a shot polls_voters >/dev/null

say "== once there are votes, only the question and the deadline change"
"$PAIR" a polledit "$MID" >/dev/null
locked() { snap_a; [ "$(pc open)" = 1 ] && [ "$(pc locked)" = 1 ] && [ "$(row 0 enabled)" = 0 ]; }
check "the composer shows the options locked" waitfor 10 locked
no_rows_tools() { [ "$(row 0 rm)" = 0,0,0,0 ] && [ "$(pc add)" = 0,0,0,0 ]; }
check "...with no remove and no Add option" no_rows_tools
check "an option cannot be typed into" [ "$(ack_a polltype 0 Burritos)" = err ]
check "...nor a check changed" [ "$(ack_a pollcheck multi 1)" = err ]
check "...nor a row added" [ "$(ack_a polladd)" = err ]
"$PAIR" a shot polls_locked >/dev/null
DAY2="$(date -d '+3 days' +%F)"
EXP2="$(local_ms "$DAY2" 09:05)"
"$PAIR" a polldeadline "$DAY2 09:05" >/dev/null
check "the deadline moves, and Save is taken" [ "$(ack_a pollsubmit)" = ok ]
moved() { snap_b; [ "$(field "$DB" "^  poll msg=$MID " closes)" = "$EXP2" ] && [ "$(field "$DB" "^  poll msg=$MID " opts)" = 4 ] \
            && [ "$(opt "$DB" "$MID" 2 count)" = 1 ]; }
check "bob's client has the new deadline, the options and votes as they were" waitfor 15 moved

say "== several choices, and no names"
"$PAIR" a pollnew >/dev/null
"$PAIR" a polltype q Which days? >/dev/null
"$PAIR" a polltype 0 Monday >/dev/null
"$PAIR" a polltype 1 Tuesday >/dev/null
"$PAIR" a polladd >/dev/null
"$PAIR" a polltype 2 Wednesday >/dev/null
"$PAIR" a pollcheck multi 1 >/dev/null
"$PAIR" a pollcheck anon 1 >/dev/null
DAY3="$(date -d '+1 day' +%F)"
"$PAIR" a polldeadline "$DAY3 17:15" >/dev/null
"$PAIR" a pollsubmit >/dev/null
two_polls() { snap_a; [ "$(polls_in "$DA")" = 2 ]; }
check "the second poll is drawn" waitfor 15 two_polls
M2="$(newest_poll "$DA")"
check "...multiple choice and anonymous, closing tomorrow at 17:15" \
  bash -c "grep -qE '^  poll msg=$M2 multi=1 anon=1 closes=$(local_ms "$DAY3" 17:15) closed=0 ' $DA"
check "...with no list of who voted to offer" bash -c "grep -q '^  poll msg=$M2 .* who=0,0,0,0' $DA"
"$PAIR" b vote "$M2 0" >/dev/null
# The second pick is added to the selection the client holds, so it waits
# for the first to come back, as a person's second press would.
bob_monday() { snap_b; [ "$(opt "$DB" "$M2" 0 mine)" = 1 ]; }
waitfor 15 bob_monday || fail "bob's first pick did not land"
"$PAIR" b vote "$M2 2" >/dev/null
both_days() { snap_b; [ "$(opt "$DB" "$M2" 0 mine)" = 1 ] && [ "$(opt "$DB" "$M2" 2 mine)" = 1 ] \
                && [ "$(field "$DB" "^  poll msg=$M2 " voters)" = 1 ]; }
check "bob picks two days, and counts as one voter" waitfor 15 both_days

say "== Close poll now"
"$PAIR" a polledit "$MID" >/dev/null
closable() { snap_a; [ "$(pc open)" = 1 ] && [ "$(pc close)" != 0,0,0,0 ] && [ -n "$(pc close)" ]; }
check "the composer offers Close poll now on an open poll" waitfor 10 closable
press "$(pc close)"
closed_a() { snap_a; [ "$(pc open)" = 0 ] && [ "$(field "$DA" "^  poll msg=$MID " closed)" = 1 ] \
               && [ "$(opt "$DA" "$MID" 0 r)" = 0,0,0,0 ]; }
check "pressing it closes the poll, and its rows take no vote" waitfor 15 closed_a
closed_b() { snap_b; [ "$(field "$DB" "^  poll msg=$MID " closed)" = 1 ] && [ "$(opt "$DB" "$MID" 1 r)" = 0,0,0,0 ]; }
check "...on bob's client too" waitfor 15 closed_b
check "...where a vote is refused" [ "$("$PAIR" b vote "$MID 1" 2>/dev/null | grep -o 'ack: [a-z]*' | cut -d' ' -f2)" = err ]
reopened_closed() { "$PAIR" a polledit "$MID" >/dev/null; snap_a
                    grep -q '^pollcomposer open=1 .*close=0,0,0,0' "$DA" && grep -q '^pollcomposer .*when="Closed ' "$DA"; }
check "...and its composer says Closed, with no Close poll now" reopened_closed
"$PAIR" a pollcancel >/dev/null
"$PAIR" a shot polls_closed >/dev/null

say "== what the composer refuses"
"$PAIR" a pollnew >/dev/null
"$PAIR" a polltype 0 Yes >/dev/null
"$PAIR" a polltype 1 No >/dev/null
check "no question: refused" [ "$(ack_a pollsubmit)" = err ]
snap_a
at_question() { [ "$(pc errat)" = question ] && [ "$(pc open)" = 1 ]; }
check "...at the question, the card still open" at_question
"$PAIR" a polltype q Just one? >/dev/null
"$PAIR" a polltype 1 "" >/dev/null
check "one option: refused" [ "$(ack_a pollsubmit)" = err ]
snap_a
at_options() { [ "$(pc errat)" = options ] && grep -q '^pollcomposer .*err="A poll needs at least two options."' "$DA"; }
check "...at the options, saying so" at_options
"$PAIR" a shot polls_refused >/dev/null
"$PAIR" a polltype 1 No >/dev/null
"$PAIR" a polldeadline "$(date -d '-1 day' +%F) 10:00" >/dev/null
check "a deadline already past: refused" [ "$(ack_a pollsubmit)" = err ]
snap_a
check "...at the time" [ "$(pc errat)" = time ]
"$PAIR" a pollcancel >/dev/null
sleep 1
snap_a
nothing_posted() { [ "$(polls_in "$DA")" = 2 ] && [ "$(pc open)" = 0 ]; }
check "nothing was posted, and the card is gone" nothing_posted

rm -f "$DA" "$DB"
[ "${1:-}" = --down ] && "$PAIR" down >/dev/null 2>&1
if [ "$fails" -eq 0 ]; then say "gui_polls: all passed ($checks checks)"; else say "gui_polls: $fails FAILED"; fi
exit "$fails"
