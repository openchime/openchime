#!/usr/bin/env bash
# To-do lists in messages, made, marked, added to and renamed.
#
#   tools/gui/todos.sh          # run, and leave the pair running
#   tools/gui/todos.sh --down   # ... and take it down afterwards
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
# Two clients over the pair (tools/gui/pair.sh, or tools/web/pair.sh on the
# web): alice makes the lists, bob works them.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
. "$HERE/tools/gui/lib.sh"

export OC_TEST_PORT="${OC_TEST_PORT:-9740}"
export OC_TEST_DIR="${OC_TEST_DIR:-/tmp/oc-todos}"
CH=1

DA="/tmp/oc-todos-a.txt"
DB="/tmp/oc-todos-b.txt"
snap_a() { "$PAIR" a dump todos > "$DA" 2>&1; }
snap_b() { "$PAIR" b dump todos > "$DB" 2>&1; }
waitfor() {  # waitfor <secs> <command...>
  local secs="$1" t=0; shift
  while [ "$t" -lt $((secs * 4)) ]; do if "$@"; then return 0; fi; sleep 0.25; t=$((t + 1)); done
  return 1
}
# Field `k` of the first dump line matching a pattern: field <file> <pattern> <k>
field() { grep -m1 -E "$2" "$1" | grep -o "\b$3=[^ ]*" | head -1 | cut -d= -f2 | tr -d '"'; }
lists_in() { grep -c '^  todo msg=' "$1"; }
newest_list() { grep '^  todo msg=' "$1" | tail -1 | grep -o 'msg=[0-9]*' | cut -d= -f2; }
list() { field "$1" "^  todo msg=$2 " "$3"; }           # list <file> <mid> <field>
task() { field "$1" "^    todotask msg=$2 i=$3 " "$4"; }  # task <file> <mid> <i> <field>
# Press the centre of rect "l,t,r,b" on a client: press <a|b> <rect>
press() {
  local l t rr b
  IFS=, read -r l t rr b <<< "$2"
  [ -n "${b:-}" ] && [ "${rr:-0}" -gt 0 ] || return 1
  "$PAIR" "$1" click $(( (l + rr) / 2 )) $(( (t + b) / 2 )) >/dev/null
}

say "== fixture: alice and bob in one channel"
"$PAIR" down >/dev/null 2>&1
rm -rf "$OC_TEST_DIR"
"$PAIR" up || { say "pair did not come up"; exit 1; }
"$PAIR" a size 1200 820 >/dev/null
"$PAIR" b size 1200 820 >/dev/null
"$PAIR" a channel $CH >/dev/null
"$PAIR" b channel $CH >/dev/null

say "== alice sends a /todo message"
"$PAIR" a sendraw '/todo Launch checklist\n- Write the notes: for the release\n* Tag it\nTell people' >/dev/null
one_a() { snap_a; [ "$(lists_in "$DA")" = 1 ]; }
check "alice's client draws one list" waitfor 15 one_a
MID="$(newest_list "$DA")"
check "...titled from the first line, three tasks, none done" \
  bash -c "grep -q '^  todo msg=$MID .*total=3 done=0 .*title=\"Launch checklist\"' $DA"
in_order() { grep -q "^    todotask msg=$MID i=0 .*name=\"Write the notes\" desc=\"for the release\"" "$DA" \
               && grep -q "^    todotask msg=$MID i=1 .*name=\"Tag it\" desc=\"\"" "$DA" \
               && grep -q "^    todotask msg=$MID i=2 .*name=\"Tell people\" desc=\"\"" "$DA"; }
check "...in order, a leading - or * dropped, the description after ': '" in_order
check "...drawn as its card, not as its /todo text" [ "$(list "$DA" "$MID" body)" = 0 ]
one_b() { snap_b; [ "$(lists_in "$DB")" = 1 ] && [ "$(newest_list "$DB")" = "$MID" ]; }
check "bob's client draws the same list" waitfor 15 one_b
check "...offering bob Add a task" [ "$(list "$DB" "$MID" add)" != 0,0,0,0 ]
check "...and no Rename on alice's list" [ "$(list "$DB" "$MID" rename)" = 0,0,0,0 ]
offers_rename() { snap_a; [ "$(list "$DA" "$MID" rename)" != 0,0,0,0 ] && [ -n "$(list "$DA" "$MID" rename)" ]; }
check "alice's card offers her Rename" waitfor 10 offers_rename
check "a to-do message offers no Edit" [ "$("$PAIR" a editmsg "$MID" 2>/dev/null | grep -o 'ack: [a-z]*' | cut -d' ' -f2)" = err ]
"$PAIR" a shot todos_new >/dev/null

say "== bob marks a task done, then not done"
snap_b
press b "$(task "$DB" "$MID" 1 r)"
marked_b() { snap_b; [ "$(task "$DB" "$MID" 1 done)" = 1 ] && [ "$(task "$DB" "$MID" 1 byname)" = bob ]; }
check "pressing Tag it's box marks it done, by bob" waitfor 15 marked_b
marked_a() { snap_a; [ "$(task "$DA" "$MID" 1 done)" = 1 ] && [ "$(task "$DA" "$MID" 1 byname)" = bob ] \
               && [ "$(list "$DA" "$MID" done)" = 1 ]; }
check "...on alice's client too, saying bob marked it" waitfor 15 marked_a
"$PAIR" a shot todos_marked >/dev/null
snap_b
press b "$(task "$DB" "$MID" 1 r)"
unmarked() { snap_a; [ "$(task "$DA" "$MID" 1 done)" = 0 ] && [ "$(task "$DA" "$MID" 1 by)" = 0 ]; }
check "pressed again, it is not done, on alice's client" waitfor 15 unmarked

say "== both mark the same task done at once"
snap_a; snap_b
RA="$(task "$DA" "$MID" 2 r)"; RB="$(task "$DB" "$MID" 2 r)"
press a "$RA" & press b "$RB"; wait
agreed() { snap_a; snap_b; [ "$(task "$DA" "$MID" 2 done)" = 1 ] && [ "$(task "$DB" "$MID" 2 done)" = 1 ] \
             && [ "$(task "$DA" "$MID" 2 by)" = "$(task "$DB" "$MID" 2 by)" ]; }
check "both clients show Tell people done, marked by the same person" waitfor 15 agreed
sleep 1
still() { snap_a; [ "$(task "$DA" "$MID" 2 done)" = 1 ]; }
check "...and it stays done: the two marks did not cancel out" still

say "== bob adds a task"
"$PAIR" b formnext "Book the room|the big one" >/dev/null
snap_b
press b "$(list "$DB" "$MID" add)"
added() { snap_a; [ "$(list "$DA" "$MID" total)" = 4 ] && grep -q "^    todotask msg=$MID i=3 .*name=\"Book the room\" desc=\"the big one\"" "$DA"; }
check "Add a task adds it at the end, on alice's client" waitfor 15 added
"$PAIR" b formnext "Tag it|" >/dev/null
snap_b
press b "$(list "$DB" "$MID" add)"
sleep 2
not_added() { snap_a; snap_b; [ "$(list "$DA" "$MID" total)" = 4 ] && [ "$(list "$DB" "$MID" total)" = 4 ]; }
check "a name the list already has is refused, the list unchanged" not_added

say "== alice renames the list"
"$PAIR" a formnext "Launch day" >/dev/null
snap_a
press a "$(list "$DA" "$MID" rename)"
renamed() { snap_b; grep -q "^  todo msg=$MID .*title=\"Launch day\"" "$DB"; }
check "Rename changes the title on bob's client" waitfor 15 renamed
check "...the tasks as they were" [ "$(list "$DB" "$MID" total)" = 4 ]
"$PAIR" a shot todos_renamed >/dev/null

say "== New to-do list"
# The palette's "Create a to-do list", answered with a title and one task.
"$PAIR" a formnext "Groceries|Milk: two litres" >/dev/null
"$PAIR" a menu 97 >/dev/null
two_lists() { snap_b; [ "$(lists_in "$DB")" = 2 ]; }
check "the form posts a second list" waitfor 15 two_lists
M2="$(newest_list "$DB")"
check "...titled Groceries, with its one task and description" \
  bash -c "grep -q '^  todo msg=$M2 .*total=1 .*title=\"Groceries\"' $DB && grep -q '^    todotask msg=$M2 i=0 .*name=\"Milk\" desc=\"two litres\"' $DB"

say "== what the daemon refuses at send"
LONG="$(head -c 251 /dev/zero | tr '\0' x)"
"$PAIR" a sendraw "/todo $LONG\\n- one" >/dev/null
"$PAIR" a sendraw '/todo Twice\n- Same\n- Same' >/dev/null
sleep 2
refused() { snap_a; snap_b; [ "$(lists_in "$DA")" = 2 ] && [ "$(lists_in "$DB")" = 2 ] \
              && ! grep -q 'title="Twice"' "$DB"; }
check "a title past 250 bytes and a task named twice post nothing" refused

rm -f "$DA" "$DB"
[ "${1:-}" = --down ] && "$PAIR" down >/dev/null 2>&1
if [ "$fails" -eq 0 ]; then say "gui_todos: all passed ($checks checks)"; else say "gui_todos: $fails FAILED"; fi
exit "$fails"
