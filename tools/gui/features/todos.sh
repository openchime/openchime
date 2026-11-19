#!/usr/bin/env bash
# To-do lists in messages, made, marked, added to and renamed.
#
#   tools/gui/features/todos.sh          # run; the pair and daemon stop at the end
#   tools/gui/features/todos.sh --keep   # ... and leave them running
#
# A to-do list is a message whose body starts "/todo" and a title, a task to a
# line after it. The client draws it as a card -- a box per task, who marked
# each done, "Add a task", and Rename for its author -- in place of the body.
# It asserts that
#
#   - a "/todo" message is drawn as its list on both clients, its title and
#     tasks in order, a description after a task's ": ", and not as its text;
#   - pressing a task's box on one client marks it done on both, saying who
#     marked it; pressing it again marks it not done;
#   - both clients marking the same task done at once agree: done, once;
#   - "Add a task" adds one at the end, for anyone who reads the list, and a
#     name the list already has is refused, the list unchanged;
#   - only the author is offered Rename, and renaming changes the title on
#     both clients;
#   - a to-do list's message offers no Edit;
#   - "New to-do list" makes the same message from a title and its tasks;
#   - a "/todo" body past a limit, or naming a task twice, is not posted.
#
# Two clients over the pair (tools/gui/harness/win32/pair.sh, or tools/gui/harness/web/pair.sh on the
# web): alice makes the lists, bob works them.
#
# platforms: win32 web
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
. "$HERE/tools/gui/harness/lib.sh"

gui_begin todos 9740 /tmp/oc-todos "$@"
CH=1

lists_in() { grep -c '^  todo msg=' "$1"; }
newest_list() { grep '^  todo msg=' "$1" | tail -1 | grep -o 'msg=[0-9]*' | cut -d= -f2; }
list() { field "$1" "^  todo msg=$2 " "$3"; }           # list <file> <mid> <field>
task() { field "$1" "^    todotask msg=$2 i=$3 " "$4"; }  # task <file> <mid> <i> <field>
# Whatever either client has sent so far has been through the daemon and back
# to both: alice says something, then bob, each seen by both. A change that
# was going to happen (a mark undone, a task added) has happened by then, so
# its absence can be checked without waiting on a clock.
SETTLES=0
settle() {
  local w
  for w in a b; do
    SETTLES=$((SETTLES + 1))
    "$PAIR" $w send "settle $SETTLES" >/dev/null || gui_abort "client $w did not take a message"
    waitfor 10 newest_is a $CH "settle $SETTLES" && waitfor 10 newest_is b $CH "settle $SETTLES" ||
      gui_abort "client $w's message did not come back to both clients"
  done
}

step "fixture: alice and bob in one channel"
gui_pair_up
for c in a b; do
  "$PAIR" $c size 1200 820 >/dev/null && "$PAIR" $c channel $CH >/dev/null ||
    gui_abort "client $c did not take its size and channel"
done

step "alice sends a /todo message"
"$PAIR" a sendraw '/todo Launch checklist\n- Write the notes: for the release\n* Tag it\nTell people' >/dev/null
one_a() { gui_snap a; [ "$(lists_in "$DA")" = 1 ]; }
check "alice's client draws one list" waitfor 15 one_a
MID="$(newest_list "$DA")"
check "...titled from the first line, three tasks, none done" \
  grep -q "^  todo msg=$MID .*total=3 done=0 .*title=\"Launch checklist\"" "$DA"
in_order() { grep -q "^    todotask msg=$MID i=0 .*name=\"Write the notes\" desc=\"for the release\"" "$DA" \
               && grep -q "^    todotask msg=$MID i=1 .*name=\"Tag it\" desc=\"\"" "$DA" \
               && grep -q "^    todotask msg=$MID i=2 .*name=\"Tell people\" desc=\"\"" "$DA"; }
check "...in order, a leading - or * dropped, the description after ': '" in_order
check "...drawn as its card, not as its /todo text" [ "$(list "$DA" "$MID" body)" = 0 ]
one_b() { gui_snap b; [ "$(lists_in "$DB")" = 1 ] && [ "$(newest_list "$DB")" = "$MID" ]; }
check "bob's client draws the same list" waitfor 15 one_b
check "...offering bob Add a task" [ "$(list "$DB" "$MID" add)" != 0,0,0,0 ]
check "...and no Rename on alice's list" [ "$(list "$DB" "$MID" rename)" = 0,0,0,0 ]
offers_rename() { gui_snap a; [ "$(list "$DA" "$MID" rename)" != 0,0,0,0 ] && [ -n "$(list "$DA" "$MID" rename)" ]; }
check "alice's card offers her Rename" waitfor 10 offers_rename
check "a to-do message offers no Edit" [ "$(ack_of a editmsg "$MID")" = err ]
"$PAIR" a shot todos_new >/dev/null

step "bob marks a task done, then not done"
gui_snap b
click_rect b "$(task "$DB" "$MID" 1 r)"
marked_b() { gui_snap b; [ "$(task "$DB" "$MID" 1 done)" = 1 ] && [ "$(task "$DB" "$MID" 1 byname)" = bob ]; }
check "pressing Tag it's box marks it done, by bob" waitfor 15 marked_b
marked_a() { gui_snap a; [ "$(task "$DA" "$MID" 1 done)" = 1 ] && [ "$(task "$DA" "$MID" 1 byname)" = bob ] \
               && [ "$(list "$DA" "$MID" done)" = 1 ]; }
check "...on alice's client too, saying bob marked it" waitfor 15 marked_a
"$PAIR" a shot todos_marked >/dev/null
gui_snap b
click_rect b "$(task "$DB" "$MID" 1 r)"
unmarked() { gui_snap a; [ "$(task "$DA" "$MID" 1 done)" = 0 ] && [ "$(task "$DA" "$MID" 1 by)" = 0 ]; }
check "pressed again, it is not done, on alice's client" waitfor 15 unmarked

step "both mark the same task done at once"
# At once: each presses while neither has heard of the other's mark. A press
# sends the state its box does not show (done, for a box not ticked), so a
# press made after the other's mark had reached that client would send "not
# done" -- a different case, and which one ran was down to timing. The
# daemon is held (SIGSTOP) while both press, each mark waiting unanswered in
# its connection, and let go once both are made: two marks that agree, the
# second finding the task already done (daemon/dbwriter.c sets, it does not
# toggle). lib.sh's teardown lets a held daemon go, however this stops.
gui_snap a; gui_snap b
RA="$(task "$DA" "$MID" 2 r)"; RB="$(task "$DB" "$MID" 2 r)"
HELD="$(dev_daemon_pids)"
[ -n "$HELD" ] || gui_abort "no daemon on :$OC_TEST_PORT to hold"
kill -STOP $HELD
click_rect a "$RA" || gui_abort "alice's card has no box for Tell people"
click_rect b "$RB" || gui_abort "bob's card has no box for Tell people"
neither() { gui_snap a && gui_snap b && [ "$(task "$DA" "$MID" 2 done)" = 0 ] && [ "$(task "$DB" "$MID" 2 done)" = 0 ]; }
neither || gui_abort "a client shows Tell people marked while the daemon was held: the presses were not at once"
kill -CONT $HELD
agreed() { gui_snap a; gui_snap b; [ "$(task "$DA" "$MID" 2 done)" = 1 ] && [ "$(task "$DB" "$MID" 2 done)" = 1 ] \
             && [ "$(task "$DA" "$MID" 2 by)" = "$(task "$DB" "$MID" 2 by)" ]; }
check "both clients show Tell people done, marked by the same person" waitfor 15 agreed
settle
still() { gui_snap a; [ "$(task "$DA" "$MID" 2 done)" = 1 ]; }
check "...and it stays done: the two marks did not cancel out" still

step "bob adds a task"
"$PAIR" b formnext "Book the room|the big one" >/dev/null || gui_abort "bob's client did not take the form's answer"
gui_snap b
click_rect b "$(list "$DB" "$MID" add)"
added() { gui_snap a; [ "$(list "$DA" "$MID" total)" = 4 ] && grep -q "^    todotask msg=$MID i=3 .*name=\"Book the room\" desc=\"the big one\"" "$DA"; }
check "Add a task adds it at the end, on alice's client" waitfor 15 added
"$PAIR" b formnext "Tag it|" >/dev/null || gui_abort "bob's client did not take the form's answer"
gui_snap b
click_rect b "$(list "$DB" "$MID" add)"
settle
not_added() { gui_snap a; gui_snap b; [ "$(list "$DA" "$MID" total)" = 4 ] && [ "$(list "$DB" "$MID" total)" = 4 ]; }
check "a name the list already has is refused, the list unchanged" not_added

step "alice renames the list"
"$PAIR" a formnext "Launch day" >/dev/null || gui_abort "alice's client did not take the form's answer"
gui_snap a
click_rect a "$(list "$DA" "$MID" rename)"
renamed() { gui_snap b; grep -q "^  todo msg=$MID .*title=\"Launch day\"" "$DB"; }
check "Rename changes the title on bob's client" waitfor 15 renamed
check "...the tasks as they were" [ "$(list "$DB" "$MID" total)" = 4 ]
"$PAIR" a shot todos_renamed >/dev/null

step "New to-do list"
# The palette's "Create a to-do list", answered with a title and one task.
"$PAIR" a formnext "Groceries|Milk: two litres" >/dev/null || gui_abort "alice's client did not take the form's answer"
"$PAIR" a menu 97 >/dev/null || gui_abort "alice's client did not take the palette's command"
two_lists() { gui_snap b; [ "$(lists_in "$DB")" = 2 ]; }
check "the form posts a second list" waitfor 15 two_lists
M2="$(newest_list "$DB")"
groceries() { grep -q "^  todo msg=$M2 .*total=1 .*title=\"Groceries\"" "$DB" \
                && grep -q "^    todotask msg=$M2 i=0 .*name=\"Milk\" desc=\"two litres\"" "$DB"; }
check "...titled Groceries, with its one task and description" groceries

step "what the daemon refuses at send"
LONG="$(head -c 251 /dev/zero | tr '\0' x)"
"$PAIR" a sendraw "/todo $LONG\\n- one" >/dev/null
"$PAIR" a sendraw '/todo Twice\n- Same\n- Same' >/dev/null
settle
refused() { gui_snap a; gui_snap b; [ "$(lists_in "$DA")" = 2 ] && [ "$(lists_in "$DB")" = 2 ] \
              && ! grep -q 'title="Twice"' "$DB"; }
check "a title past 250 bytes and a task named twice post nothing" refused
gui_end
