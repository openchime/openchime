#!/usr/bin/env bash
# Machine-checked invariants for the Win32 client's chrome.
#
#   scripts/gui_smoke.sh            # launch, assert, leave the client running
#   scripts/gui_smoke.sh --kill     # ... and shut it down afterwards
#
# WHY THIS EXISTS. Three bugs reached the user in one day (one of them a regression) and every one of them was a *chrome* bug: a native child
# shown in a view it does not belong to, a composer under a surface you cannot
# type into, a hit-box matching the wrong column. None was visible in a Direct2D
# screenshot at the time, and all of them are one boolean each. Booleans belong in
# a script, not in my eyes.
#
# The assertions come from the three predicates in winmain.c — sidebar_kind()
# (what is in the second column), main_is_conversation() (can you type into the
# middle one) and window_is_covered() (does something own the window) — read back
# through the test hook's `natives` line. A new view or overlay must be added here
# as well as there; that is the point.
#
# NOT IN CI. The daemon is epoll-based, so it is Linux-only, and GitHub's Windows
# runners cannot host it (no Linux containers). A hosted GUI smoke therefore needs
# a self-hosted Windows box with the daemon reachable, which does not exist yet.
# Until it does, this is the pre-push gate for any change to Win32 chrome — run it
# and read it, the same discipline as reading CI.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DRIVE="$HERE/scripts/gui_drive.sh"
LIN_DIR='/mnt/c/Windows/Temp/octest'
fails=0
checks=0

# --- isolation -----------------------------------------------------
# The suite runs against its OWN daemon on its OWN port, wiped each run, and it
# says so rather than adopting whatever is listening. gui_drive's staleness check
# keeps a daemon whose start time postdates the binary — which silently adopts an
# UNRELATED daemon if one holds the port. That happened: a run bound to a
# nine-hour-old daemon holding real workspace data and reported confident
# failures for group DMs and @-completion, because the fixture users did not
# exist there. A test that can be wrong about which machine it is testing is not
# a test. Override deliberately if you mean to.
export OC_DEV_PORT="${OC_DEV_PORT:-9500}"
export OC_DEV_DIR="${OC_DEV_DIR:-/tmp/oc-smoke}"
export OC_DEV_WS="${OC_DEV_WS:-Smoke Fixture}"
# A scratch DB per run: sections, channels and emoji are server state that
# survives a run, and a suite that assumes it starts from zero passes exactly
# once. Wiping is cheaper and more honest than every section clearing up after
# itself, and it means a run leaves nothing behind in anyone's workspace.
#
# STOP THE DAEMON FIRST. Deleting the directory under a running daemon does not
# reset it: SQLite keeps writing to the unlinked inode, so the workspace stays
# live while its files are gone, and the next run's uploads land in a file
# nothing can open. That cost a debugging round — the symptom was "custom emoji
# and avatars fail to upload", which looks nothing like the cause.
#
# Defined ABOVE kill_dev_daemon, not below it. The refusal path in that block
# calls say() twice to explain why the run is stopping, and bash resolves
# functions at call time -- so with these defined later, both calls died with
# "say: command not found" and the operator saw the refusal with no reason, at
# the one moment the reason matters. (The debug branch below reaches snap(),
# which is still defined later; that is fine, it only fires for section headers
# starting "==", which the refusal messages are not.)
say()  {
  printf '%s\n' "$*"
  # OC_SMOKE_DEBUG=1 tags each section with the core's error counter, which is how
  # you find the section that BROKE something rather than the one that noticed.
  case "${OC_SMOKE_DEBUG:-0}$*" in
    1==*) printf '       [error_seq=%s last=%s]\n' \
            "$(snap | grep -oE '^error_seq=[0-9]+' | cut -d= -f2)" \
            "$(snap | grep -oE 'last_error="[^"]*"' | head -1)";;
  esac
}
fail() { printf '  FAIL %s\n' "$*"; fails=$((fails + 1)); }
ok()   { printf '  ok   %s\n' "$*"; }

# Killing it needs the ENVIRONMENT, not the command line. The daemon is started
# as `env OPENCHIME_PROTO_PORT=… openchimed`, so its own cmdline is just the
# binary and `pkill -f OPENCHIME_PROTO_PORT=9500` matches NOTHING — it exits 1,
# looks like "nothing to kill", and the daemon keeps running while the directory
# is deleted underneath it. Every upload then fails with an opaque "transfer
# error", which is where an hour went. Match /proc/<pid>/environ instead, so the
# port is compared against the value the process actually has.
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

if [ "${OC_SMOKE_KEEP_DB:-0}" != "1" ]; then
  if ! kill_dev_daemon; then
    say "a daemon is still listening on :$OC_DEV_PORT and would keep running on a"
    say "deleted directory — refusing to wipe underneath it. Stop it and re-run."
    exit 1
  fi
  rm -rf "$OC_DEV_DIR"
fi

# How long a predicate wait will tolerate before calling it a failure. Generous:
# the cost of a high value is paid only by runs that are already failing.
WAIT_MS="${OC_SMOKE_WAIT_MS:-6000}"

# Read one `key=value` out of a fresh dump.
snap() { "$DRIVE" dump smoke >/dev/null 2>&1; cat "$LIN_DIR/smoke.txt" 2>/dev/null; }

# --- waiting on state instead of on a clock ------------------------
# `ack` means the verb's HANDLER RAN, not that its effect is observable: most of
# the interesting dump fields (`natives`, the hit-box rects) are recorded during
# WM_PAINT, and anything that goes to the server lands a round trip later. The
# suite used to bridge that with 96 hand-tuned sleeps, which made it fail ~60% of
# runs for reasons that were never product defects — and, worse, produced FAKE
# PASSES, because asserting a state rather than a transition means "Esc closed
# the pane" passes when the pane never opened.
#
# So: wait for the state being asserted. A true assertion returns on the first
# poll (the suite gets faster); only a real failure pays the timeout.

# key_of <dump> <key> — one value out of a dump
key_of() { printf '%s' "$1" | grep -o "\b$2=[^ ]*" | head -1 | cut -d= -f2; }

# wait_for <key> <want> [ms] — poll fresh dumps until it matches. 0 = matched.
wait_for() {
  local key="$1" want="$2" ms="${3:-$WAIT_MS}" t=0
  while :; do
    [ "$(key_of "$(snap)" "$key")" = "$want" ] && return 0
    [ "$t" -ge "$ms" ] && return 1
    sleep 0.1; t=$((t + 100))
  done
}

# wait_grep <regex> [ms] — poll fresh dumps until one matches the regex.
wait_grep() {
  local re="$1" ms="${2:-$WAIT_MS}" t=0
  while :; do
    snap | grep -qE "$re" && return 0
    [ "$t" -ge "$ms" ] && return 1
    sleep 0.1; t=$((t + 100))
  done
}

# settle <key> <want> — wait without asserting, for blocks that then capture one
# dump and check several keys from that same frame.
settle() { wait_for "$1" "$2" >/dev/null 2>&1 || true; }

# After a `launch`, the new window does not always take the keyboard back from
# whatever had it — and when it does not, every `key`/`chars` verb still acks "ok"
# while going nowhere, so the suite reports "sending did not clear the field" for
# a send that was never typed. That cost a real investigation. Wait for the window
# to actually hold focus, and say plainly when it never does.
# Both halves, because they are different facts and only the second is the one
# `key`/`chars` need: the WINDOW can hold the keyboard while the composer has not
# claimed it (the auto-focus runs on the idle tick and only in a conversation).
# Waiting on the window alone still let a run reach `key enter` with focus=0.
# A freshly launched client must take the keyboard: the window is shown LATE when
# it auto-connects (it waits for its saved geometry), and a window shown by a
# process that does not own the foreground is not activated — so the composer's
# auto-focus, which is gated on having focus, never fired. That is what produced
# three "sending did not clear the field" failures for sends that were never
# typed. The client now activates itself on show; this asserts it rather than
# waiting it out, so a regression is a failure and not a slow pass.
wait_typeable() {
  local t=0
  while :; do
    local d; d=$(snap)
    case "$d" in *"wnd_focus=1"*) case "$d" in *"focus=1"*) return 0;; esac;; esac
    [ "$t" -ge 8000 ] && return 1
    sleep 0.2; t=$((t + 200))
  done
}
expect_typeable() {
  checks=$((checks + 1))
  if wait_typeable; then ok "$1"
  else fail "$1 — the composer never took the keyboard: $(snap | grep -E '^wnd_|^ed ' | tr '\n' ' ')"; fi
}

# expect <dump> <key> <wanted> <label> — same-frame check against a captured dump.
expect() {
  local dump="$1" key="$2" want="$3" label="$4" got
  checks=$((checks + 1))
  got=$(key_of "$dump" "$key")
  if [ "$got" = "$want" ]; then ok "$label ($key=$got)"
  else fail "$label — expected $key=$want, got ${got:-<missing>}"; fi
}

# expect_eventually <key> <want> <label> — the workhorse. Drives no command; call
# it straight after the one whose effect it asserts.
expect_eventually() {
  local key="$1" want="$2" label="$3" got
  checks=$((checks + 1))
  if wait_for "$key" "$want"; then ok "$label ($key=$want)"; return 0; fi
  got=$(key_of "$(snap)" "$key")
  fail "$label — expected $key=$want, got ${got:-<missing>} after ${WAIT_MS}ms"
  return 1
}

# expect_grep <regex> <label> — for the assertions that are not a single key.
expect_grep() {
  local re="$1" label="$2"
  checks=$((checks + 1))
  if wait_grep "$re"; then ok "$label"; return 0; fi
  fail "$label — no dump matched /$re/ within ${WAIT_MS}ms"
  return 1
}

say "== launching"
"$DRIVE" launch >/dev/null 2>&1 || { say "launch failed"; exit 1; }
# Wait for auth rather than sleeping a guess: every assertion below needs a model.
if ! wait_grep 'authed=1 connected=1' 20000; then
  say "   NOT AUTHED — is the dev daemon up? (see gui_drive.sh launch)"; exit 1
fi
say "   authed"

# --- and it is the RIGHT daemon ------------------------------------
# Assert the fixture before asserting anything about the product. Getting this
# wrong does not look like a broken harness, it looks like broken features.
fixture=$(snap | grep -o 'workspace name="[^"]*"' | cut -d'"' -f2)
case "$fixture" in
  "$OC_DEV_WS") say "   workspace \"$fixture\" on :$OC_DEV_PORT";;
  *) say "   WRONG DAEMON — reached \"${fixture:-<none>}\" on :$OC_DEV_PORT, expected \"$OC_DEV_WS\"."
     say "   Refusing to run: every assertion below would be about someone else's data."
     exit 1;;
esac
# The fixture's people, too. The group-DM and completion checks name alice, bob and carol
# and alice; without this, "no such user" and "the feature is broken" are the
# same failure from here — which is exactly how three runs were misread.
if ! wait_grep '^users n=[0-9]+ names="' 10000; then
  say "   no roster in the dump — the client never listed users"; exit 1
fi
roster=$(snap | grep -o '^users n=[0-9]* names="[^"]*"' | cut -d'"' -f2)
for u in alice bob carol; do
  case ",$roster," in
    *",$u,"*) ;;
    *) say "   fixture user '$u' missing — roster is \"$roster\""
       say "   Refusing to run: the named-people checks below would fail for the wrong reason."
       exit 1;;
  esac
done
say "   roster: $roster"
# Settle before asserting. `authed=1` means the model is live, not that the shell
# has painted — and every `natives` value is measured during paint.
"$DRIVE" view 0 >/dev/null 2>&1
"$DRIVE" channel general >/dev/null 2>&1
settle sbkind 1

# --- the view matrix -------------------------------------------------------
# view  sbkind  re  find  ffind  conv     what it is
#  0    1       1   1     0      1        Home: channel list + transcript
#  1    2       0   0     0      0        DMs: conversation list, index in the middle
#  2    3       1   0     0      1        Activity: feed left, transcript stays
#  3    4       0   0     1      0        Files: own column + its own search box
#  4    5       0   0     0      0        Later: own column, Files-shaped
#  5    0       0   0     0      0        Admin: no second column
say "== views"
while read -r v sbkind re find ffind conv name; do
  [ -z "${v:-}" ] && continue
  "$DRIVE" view "$v" >/dev/null 2>&1
  [ "$v" = "home" ] && "$DRIVE" channel general >/dev/null 2>&1
  # The second column identifies the view, so it is the thing to wait on; the
  # other five are same-frame properties of it.
  settle sbkind "$sbkind"
  d=$(snap)
  expect "$d" sbkind "$sbkind" "$name: second column"
  expect "$d" re     "$re"     "$name: composer child"
  expect "$d" find   "$find"   "$name: find box"
  expect "$d" ffind  "$ffind"  "$name: files search box"
  expect "$d" conv   "$conv"   "$name: middle column typeable"
  expect "$d" covered 0        "$name: nothing covering"
done <<'MATRIX'
home     1 1 1 0 1 Home
dms      2 0 0 0 0 DMs
activity 3 1 0 0 1 Activity
files    4 0 0 1 0 Files
later    5 0 0 0 0 Later
drafts   1 0 1 0 0 Drafts
admin    0 0 0 0 0 Admin
MATRIX

# --- overlays --------------------------------------------------------------
# Each of these hid a real bug: the search box surviving a modal, the composer
# living under the Pins tab, a native child punched through a dimmed card.
say "== overlays"
"$DRIVE" view 0 >/dev/null 2>&1; "$DRIVE" channel general >/dev/null 2>&1; settle sbkind 1

"$DRIVE" search "" >/dev/null 2>&1
settle srch 1
d=$(snap)
expect "$d" srch 1 "search overlay: query box shown"
expect "$d" re   0 "search overlay: composer hidden"
"$DRIVE" search "" >/dev/null 2>&1; settle srch 0    # toggle off

# The search box is a native child, so it composites ABOVE the Direct2D output
# and shows up as a bare white rectangle over whatever is really there. It was
# gated on `search_open` alone — a MODEL flag that outlives the view — so
# leaving search open and walking to DMs floated it over the third person in
# the "New direct message" picker (reported from a screenshot of the
# running client). The view matrix above could not catch it: it visits each view
# from a clean state, and this needs a surface left open in the PREVIOUS one.
"$DRIVE" search "" >/dev/null 2>&1; settle srch 1
"$DRIVE" view 1 >/dev/null 2>&1; settle sbkind 2
"$DRIVE" dmcompose >/dev/null 2>&1
expect_eventually srch 0 "search left open does not follow you to the DM picker"
"$DRIVE" dmcompose >/dev/null 2>&1
# Coming back restores it — the fix has to be "not while that view is up", not
# "closed for good", and only asserting the first half would pass either way.
# No `channel` here: PICKING a conversation closes search, which is the app
# behaving correctly and would hide what this is asking about.
"$DRIVE" view 0 >/dev/null 2>&1; settle sbkind 1
expect_eventually srch 1 "and it comes back when you return to the transcript"
"$DRIVE" search "" >/dev/null 2>&1; settle srch 0
"$DRIVE" channel general >/dev/null 2>&1

"$DRIVE" tab 2 >/dev/null 2>&1                       # Pins
settle re 0
expect_eventually re 0 "pins tab: composer hidden"
"$DRIVE" tab 0 >/dev/null 2>&1; settle re 1

"$DRIVE" prefs >/dev/null 2>&1
settle modal prefs
d=$(snap)
expect "$d" covered 1 "preferences modal: window covered"
expect "$d" re      0 "preferences modal: composer hidden"
expect "$d" find    0 "preferences modal: find box hidden"
"$DRIVE" keys 0 >/dev/null 2>&1                  # close whatever is open
"$DRIVE" view 0 >/dev/null 2>&1; settle modal none

"$DRIVE" palette >/dev/null 2>&1
settle pal 1
d=$(snap)
expect "$d" pal     1 "command palette: its box shown"
expect "$d" covered 1 "command palette: window covered"
"$DRIVE" palette >/dev/null 2>&1; settle pal 0

# --- global shortcuts ------------------------------------------------------
# Every one of these was DEAD while the composer had focus, which is nearly
# always: they were handled in the main window's WM_KEYDOWN, and a native child
# consumes what it does not recognise. The shortcut sheet advertised them anyway.
# They are dispatched from the message loop now, so the assertions are made with
# the caret sitting in the message box — the case that was broken.
#
# These are asserted as TRANSITIONS, not states. "Esc closes the palette" checked
# against `pal=0` passes when the palette never opened — so a broken Ctrl+K used
# to be reported as one failure and one *success*, which is worse than a plain
# failure because it inflates the pass count. `chord` proves the closed→open→closed
# round trip and refuses to credit the close if the open did not happen.
say "== global shortcuts (composer focused)"
"$DRIVE" view 0 >/dev/null 2>&1; "$DRIVE" channel general >/dev/null 2>&1; settle sbkind 1
"$DRIVE" type "typing when the shortcut arrives" >/dev/null 2>&1

# chord <key> <dumpkey> <open-value> <label>
chord() {
  local k="$1" key="$2" open="$3" label="$4" closed
  closed=$([ "$key" = "modal" ] && echo none || echo 0)
  # Start from a known-closed state, or the transition means nothing.
  if [ "$(key_of "$(snap)" "$key")" != "$closed" ]; then
    "$DRIVE" key esc >/dev/null 2>&1; settle "$key" "$closed"
  fi
  "$DRIVE" key "$k" >/dev/null 2>&1
  if ! expect_eventually "$key" "$open" "$label"; then
    checks=$((checks + 1)); fail "$label — close not attempted (it never opened)"
    "$DRIVE" key esc >/dev/null 2>&1; return 1
  fi
  "$DRIVE" key esc >/dev/null 2>&1
  expect_eventually "$key" "$closed" "... and Esc closes it"
}

chord ctrl+k     pal   1    "Ctrl+K opens the palette from the composer"
chord ctrl+f     srch  1    "Ctrl+F opens search from the composer"
chord ctrl+slash modal keys "Ctrl+/ opens the shortcut sheet"

# --- the modal frame -------------------------------------------------------
# Explicit commit is the whole design (ARCH-94 / docs/CLIENT.md): Save persists,
# Cancel and Esc put the snapshot back. A Cancel that silently behaved like Save
# would look identical on screen, so it is asserted rather than eyeballed. The
# clicks are the frame's own footer, whose geometry the frame owns.
say "== modal frame"

# Click the centre of a preference chip, located from the dump rather than from a
# screenshot. Hardcoded coordinates were wrong twice while writing this: the card
# geometry shifted under the new frame, and a chip's width depends on its label.
# The app already reports where it drew each hit-box, so ask it.
# Preferences is two-paned, so a row only has hit-boxes while ITS category
# is showing — and the sheet remembers the pane you left it on. Select the category
# before reaching for a chip.
click_cat() {                     # click_cat <index>
  local i="$1" r l t rr b
  r=$(snap | grep -oE "prefcat $i name=[A-Za-z]* r=[0-9,.-]*" | sed 's/.*r=//')
  [ -n "$r" ] || { fail "no prefcat $i rect"; return 1; }
  IFS=, read -r l t rr b <<<"$r"
  "$DRIVE" click $(( (${l%.*} + ${rr%.*}) / 2 )) $(( (${t%.*} + ${b%.*}) / 2 )) >/dev/null 2>&1
}

click_pref() {                    # click_pref <row> <val>
  local row="$1" val="$2" r
  r=$(snap | grep -o "prefhit row=$row val=$val r=[0-9,]*" | head -1 | sed 's/.*r=//')
  [ -n "$r" ] || { fail "no prefhit row=$row val=$val in the dump"; return 1; }
  local l t rt b
  IFS=, read -r l t rt b <<<"$r"
  "$DRIVE" click $(( (l + rt) / 2 )) $(( (t + b) / 2 )) >/dev/null 2>&1
}

"$DRIVE" prefs >/dev/null 2>&1
settle modal prefs
d=$(snap)
expect "$d" modal prefs "preferences opens as a modal"
click_cat 1; settle prefcat 1     # Messages — where the time format lives
before=$(key_of "$d" time24)
other=$([ "$before" = "1" ] && echo 0 || echo 1)

click_pref 1 "$other"
expect_eventually time24 "$other" "a change applies while the sheet is open"
"$DRIVE" key esc >/dev/null 2>&1
settle modal none
d=$(snap)
expect "$d" closed_by esc       "Esc closes it"
expect "$d" time24    "$before" "Esc RESTORES the snapshot"
expect "$d" modal     none      "nothing left open"

"$DRIVE" prefs >/dev/null 2>&1; settle modal prefs
click_cat 1; settle prefcat 1
click_pref 1 "$other"; settle time24 "$other"
"$DRIVE" key enter >/dev/null 2>&1
settle modal none
d=$(snap)
expect "$d" closed_by save     "Enter commits (primary is Save)"
expect "$d" time24    "$other" "the change SURVIVES a commit"

# Leave the preference as it was found: a smoke run that mutates settings is a
# smoke run you stop trusting.
"$DRIVE" prefs >/dev/null 2>&1; settle modal prefs
click_cat 1; settle prefcat 1
click_pref 1 "$before"; settle time24 "$before"
"$DRIVE" key enter >/dev/null 2>&1; settle modal none
expect_eventually time24 "$before" "the run left the setting as it found it"

# --- channel visibility (REQ-031) -------------------------------------------
# The two directions are not symmetric — private narrows the audience, public
# discloses the whole history — so both are driven, and the confirm is asserted to
# be there: this is the one channel action that cannot be undone by repeating it.
say "== channel visibility"
"$DRIVE" view 0 >/dev/null 2>&1; settle sbkind 1
# `mkchan` is a no-op when the channel already exists, so this run may inherit one
# left either way by the last. The checks are therefore about the FLIP — the value
# changes, and changes back — not about an absolute state a previous run decided.
"$DRIVE" mkchan smokevis 0 >/dev/null 2>&1
wait_grep '^  ch [0-9]+ "smokevis"' >/dev/null 2>&1 || true
"$DRIVE" channel smokevis >/dev/null 2>&1; "$DRIVE" tab 3 >/dev/null 2>&1
wait_grep 'aboutvis=[0-9]' >/dev/null 2>&1 || true

# Anchored on the name and then on `pub=` separately, rather than on the two
# being adjacent: they stopped being adjacent the moment `peers=` was added to
# the line, and a matcher that encodes field ORDER breaks on every new
# field. This one only requires that both are on the smokevis row.
vispub() { snap | grep -E '^  ch [0-9]+ "smokevis" ' | grep -oE 'pub=[01]' | head -1 | cut -d= -f2; }

click_about() {                    # click_about <field>
  local r l t rr b
  r=$(snap | grep -oE "$1=[0-9,.-]+" | head -1 | cut -d= -f2)
  [ -n "$r" ] || { fail "no $1 rect in the dump"; return 1; }
  IFS=, read -r l t rr b <<<"$r"
  [ "${rr%.*}" -gt "${l%.*}" ] || { fail "$1 is not drawn"; return 1; }
  "$DRIVE" click $(( (${l%.*} + ${rr%.*}) / 2 )) $(( (${t%.*} + ${b%.*}) / 2 )) >/dev/null 2>&1
}

was=$(vispub)
checks=$((checks + 1))
[ -n "$was" ] && ok "the channel is there (pub=$was)" || fail "no smokevis channel in the dump"

click_about aboutvis
expect_eventually modal confirm "changing visibility asks first"
"$DRIVE" key enter >/dev/null 2>&1
settle modal none
# The flip is a server round trip, so wait for the value to actually change
# rather than for the dialog to close.
t=0; while [ "$(vispub)" = "$was" ] && [ $t -lt $WAIT_MS ]; do sleep 0.1; t=$((t+100)); done
now=$(vispub)
checks=$((checks + 1))
if [ -n "$now" ] && [ "$now" != "$was" ]; then ok "the flip lands (pub=$was -> $now)"
else fail "visibility did not change (still pub=${now:-?})"; fi
# A member keeps the channel in ANY direction — private narrows who can read it,
# it does not remove the people who are in it.
checks=$((checks + 1))
if snap | grep -qE '^  ch [0-9]+ "smokevis"'; then ok "a member still sees it"
else fail "the channel disappeared for its own member"; fi
# Deliberately the `ch` line and not `sbrow`: whether a member still HAS the channel
# is a fact about the channel list. Asserting it through the sidebar made the check
# depend on which section a previous run had filed it under, and on whether that
# section was collapsed.

click_about aboutvis
settle modal confirm
"$DRIVE" key enter >/dev/null 2>&1
t=0; while [ "$(vispub)" != "$was" ] && [ $t -lt $WAIT_MS ]; do sleep 0.1; t=$((t+100)); done
checks=$((checks + 1))
back=$(vispub)
if [ "$back" = "$was" ]; then ok "and flips back (pub=$back)"
else fail "did not come back: pub=${back:-?}, started at $was"; fi
"$DRIVE" tab 0 >/dev/null 2>&1; settle re 1

# --- custom emoji (REQ-072) -------------------------------------------------
# The claim is that the catalogue arrives and a shortcode becomes an IMAGE. The
# image itself is checked by the thumb cache carrying its attachment id: a
# screenshot proves the pixels, this proves the wiring on every run.
say "== custom emoji"
if [ -f "$LIN_DIR/shipit.png" ]; then
  # The delete must LAND before the add: the name is the identity (REQ-072), so an
  # add racing its own delete is a duplicate-name rejection. The old script hid
  # this behind a sleep; waiting on the catalogue says what is actually required.
  "$DRIVE" emoji_del smoketest >/dev/null 2>&1
  t=0; while snap | grep -q 'cemoji smoketest ' && [ $t -lt $WAIT_MS ]; do sleep 0.1; t=$((t+100)); done
  # An upload needs a conversation to upload INTO — the hook falls back to the
  # selected channel — so say which, rather than inheriting whatever the previous
  # section happened to leave selected.
  "$DRIVE" channel general >/dev/null 2>&1; settle sel 1
  [ "${OC_SMOKE_DEBUG:-0}" = "1" ] && say "      pre-add:  $(snap | grep -oE 'error_seq=[0-9]+ last_error=\"[^\"]*\"')"
  "$DRIVE" emoji_add smoketest 'C:\Windows\Temp\octest\shipit.png' >/dev/null 2>&1
  if [ "${OC_SMOKE_DEBUG:-0}" = "1" ]; then
    for i in 1 2 3 4; do sleep 1
      say "      +${i}s:      $(snap | grep -oE 'error_seq=[0-9]+ last_error=\"[^\"]*\"') cemoji=$(snap | grep -c cemoji)"
    done
  fi
  # An upload plus a catalogue fan-out: wait for the row, do not guess at 2.5s.
  if ! expect_grep '^  cemoji smoketest attach=[0-9]+' ":smoketest: is in the catalogue"; then
    say "      state: $(snap | grep -E '^authed|^users|^error_seq' | tr '\n' ' ')"
  fi
  aid=$(snap | grep -oE '^  cemoji smoketest attach=[0-9]+' | grep -oE '[0-9]+$')
  # Post it and let the transcript ask for the image.
  "$DRIVE" channel general >/dev/null 2>&1
  "$DRIVE" send "smoke :smoketest: check" >/dev/null 2>&1
  if [ -n "${aid:-}" ]; then
    expect_grep "^  thumb [0-9]+ id=$aid " "its image decoded, so the shortcode renders as a picture"
  else
    checks=$((checks + 1)); fail "no decoded bitmap — no attachment id to look for"
  fi
  # Leave the workspace as we found it.
  "$DRIVE" emoji_del smoketest >/dev/null 2>&1
  checks=$((checks + 1))
  t=0; while snap | grep -q 'cemoji smoketest ' && [ $t -lt $WAIT_MS ]; do sleep 0.1; t=$((t+100)); done
  if ! snap | grep -q 'cemoji smoketest '; then ok "deleting it removes it everywhere"
  else fail "smoketest survived a delete"; fi
else
  say "   (no $LIN_DIR/shipit.png — skipping; create one to cover REQ-072)"
fi

# --- group DMs (REQ-056) ----------------------------------------------------
# A group DM is a DM with more than two participants, so the claims worth asserting
# are that it appears ONCE with a computed title, and that reopening the same set
# returns the same conversation rather than a second one.
say "== group DMs"
"$DRIVE" view 0 >/dev/null 2>&1; settle sbkind 1
# Section collapse is per-user state and it PERSISTS on the server: a collapsed
# Direct-messages section emits its header and no children, so every row assertion
# below would fail for a reason that has nothing to do with group DMs. Ask for the
# state this section needs rather than inheriting whatever the last run left.
"$DRIVE" section expand 1 >/dev/null 2>&1
"$DRIVE" groupdm bob,carol >/dev/null 2>&1
expect_grep '^  sbrow sec=1 header=0 cid=[0-9]+ label="alice, bob, carol"' \
            "the group is titled by everyone in it, the reader included"
d=$(snap)
gid=$(printf '%s' "$d" | grep -oE '^  sbrow sec=1 header=0 cid=[0-9]+ label="alice, bob, carol"' | grep -oE 'cid=[0-9]+' | cut -d= -f2 | head -1)
checks=$((checks + 1))
n=$(printf '%s' "$d" | grep -cE 'sbrow .*label="alice, bob, carol"' || true)
if [ "$n" = "1" ]; then ok "and exactly once"
else fail "the group appears $n times"; fi

# Reopening the same set must not make a second conversation. There is nothing to
# wait FOR here — the assertion is that nothing new appears — so this one settles
# on the round trip completing (the selection lands on the reopened DM) and then
# counts.
"$DRIVE" groupdm carol,bob >/dev/null 2>&1
[ -n "${gid:-}" ] && settle sel "$gid"
checks=$((checks + 1))
n=$(snap | grep -cE 'sbrow .*label="alice, bob, carol"' || true)
if [ "$n" = "1" ]; then ok "reopening the same set reuses it"
else fail "reopening produced $n rows"; fi

# The header names the group, and a DM's tab strip has no About tab.
if [ -n "${gid:-}" ]; then
  "$DRIVE" channel "$gid" >/dev/null 2>&1
  expect_eventually sel "$gid" "selecting it works by id"
fi

# --- the group-DM picker -------------------------------------------
# The engine half is asserted above through the `groupdm` verb. This is the
# AFFORDANCE: gathering people by ticking them, which is what replaced a form
# asking for "two to eight usernames, comma separated". Driven through the rects
# the app reports, because the rows MOVE when the gathering bar appears — the
# first attempt at this clicked a measured pixel and toggled the same person
# twice.
say "== group DM, through the New Message pane"
# The tick-list card this used to drive is retired (REQ-229): a group DM is now
# made the same way as any other message — address it to two people and send.
# The assertions moved with the feature rather than being deleted with the card.
"$DRIVE" menu 83 >/dev/null 2>&1
expect_eventually view 6 "the group action opens the New Message pane"
expect_eventually tofocus 1 "with the To: field ready"

# Whoever the fixture's other two people are — naming them would hardcode ids
# into a check about the picker.
"$DRIVE" key ctrl+a >/dev/null 2>&1; "$DRIVE" key backspace >/dev/null 2>&1
"$DRIVE" chars "b" >/dev/null 2>&1
expect_eventually cand 1 "the picker offers somebody"
"$DRIVE" key enter >/dev/null 2>&1
expect_eventually chips 1 "one person gathered"
"$DRIVE" chars "c" >/dev/null 2>&1
"$DRIVE" key enter >/dev/null 2>&1
expect_eventually chips 2 "and a second alongside them"
# You are never offered to yourself: you are in every conversation you start.
checks=$((checks + 1))
snap | grep -qE '^newmsg .*cand=' && ok "the picker answers without offering yourself" \
                                  || fail "picker state missing from the dump"

"$DRIVE" key enter >/dev/null 2>&1          # done addressing
"$DRIVE" chars "three of us" >/dev/null 2>&1
sendpt=$(snap | awk '/^newmsg /{ for (i=1;i<=NF;i++) if ($i ~ /^send=/) { sub(/^send=/,"",$i);
  split($i, b, ","); print int((b[1]+b[3])/2), int((b[2]+b[4])/2) } }')
"$DRIVE" click $sendpt >/dev/null 2>&1
# Creating the conversation is a round trip, so this waits on the RESULT: the
# message present in a DM channel, which is the only proof it went anywhere.
expect_grep '^  ch [0-9]+ DM .*prev="three of us"' "sending creates the group and posts to it"
"$DRIVE" view home >/dev/null 2>&1; "$DRIVE" channel general >/dev/null 2>&1; settle sbkind 1

# --- avatars -------------------------------------------------------
# Two things are asserted, because the second was invisible for an hour: that the
# avatar is set, AND that a screenshot can see an image at all. Every capture used
# to suppress images (a D2D bitmap belongs to the target that made it), so the
# avatars were drawing correctly on screen and no capture could show it.
say "== avatars"
"$DRIVE" view 0 >/dev/null 2>&1; "$DRIVE" channel general >/dev/null 2>&1; settle sbkind 1
if [ -f "$LIN_DIR/face.png" ]; then
  "$DRIVE" avatar 'C:\Windows\Temp\octest\face.png' >/dev/null 2>&1
  expect_grep 'myavatar=[1-9][0-9]*' "the avatar is set"
  mine=$(snap | grep -oE 'myavatar=[0-9]+' | cut -d= -f2)
  # The bytes come back and decode: one cached thumbnail whose id is the avatar's.
  if [ -n "${mine:-}" ] && [ "$mine" != "0" ]; then
    expect_grep "^  thumb [0-9]+ id=$mine " "its image decoded into the cache"
  else
    checks=$((checks + 1)); fail "no avatar id to look for a bitmap with"
  fi
  "$DRIVE" avatar 0 >/dev/null 2>&1
  expect_eventually myavatar 0 "clearing it works"
else
  say "   (no $LIN_DIR/face.png — skipping; create one to cover avatar upload)"
fi

# --- user-defined sidebar sections ---------------------------------
# The interesting property is the appear-ONCE rule: a conversation in a custom
# section leaves Channels, and a starred one leaves the custom section too. That is
# a claim about the sidebar the core builds, so it is asserted from the row list
# rather than from a screenshot.
say "== sidebar sections"
"$DRIVE" view 0 >/dev/null 2>&1; "$DRIVE" channel general >/dev/null 2>&1; settle sbkind 1
"$DRIVE" section expand 0 >/dev/null 2>&1              # Channels, for the same reason

rows_have() {                     # rows_have <label> <count>
  local want="$1" n="$2" got
  checks=$((checks + 1))
  got=$(snap | grep -cE "^  sbrow .*label=\"$want\"" || true)
  if [ "$got" = "$n" ]; then ok "#$want appears $n time(s) in the sidebar"
  else fail "#$want appears $got time(s), expected $n"; fi
}

# Sections are per-user state on the SERVER, so they survive a run. Clear them all
# first: a test that assumes it starts from zero passes exactly once.
for _ in 1 2 3 4 5 6 7 8; do
  n=$(snap | grep -oE '^sections=[0-9]+' | cut -d= -f2)
  [ "${n:-0}" = "0" ] && break
  "$DRIVE" section rm 0 >/dev/null 2>&1
  settle sections $(( ${n:-1} - 1 ))
done
"$DRIVE" section add SmokeSec >/dev/null 2>&1
expect_eventually sections 1 "a section is created"
"$DRIVE" section put 1 0 >/dev/null 2>&1
expect_grep 'section 0 name="SmokeSec" n=1 collapsed=0 ids=1' "the conversation is in it"
# ... and it is in the SECTION, not in Channels as well.
rows_have SmokeSec 1
rows_have general 1
expect_grep '^  sbrow sec=16 header=0 cid=1 ' "#general sits under section 16"

# Removing the section returns the conversation rather than losing it.
"$DRIVE" section rm 0 >/dev/null 2>&1
expect_eventually sections 0 "removing the section leaves none"

# --- appearance: text size, zoom, accent, density ------------------
# Each of these rebuilds the DirectWrite table or the palette, so "it did not take
# effect" is a real failure mode and none of it is visible in a boolean. The dump
# reports the three scale inputs ARCH-97 keeps apart plus their product.
say "== appearance"
"$DRIVE" view 0 >/dev/null 2>&1; "$DRIVE" channel general >/dev/null 2>&1; settle sbkind 1

# Zoom is asserted as a round trip for the same reason as the chords: "Ctrl+0
# resets it" against zoom=0 passes when Ctrl+= never zoomed, so the reset used to
# be credited for free on exactly the runs where the zoom failed.
base=$(snap | grep -oE 'scale=[0-9.]+' | head -1 | cut -d= -f2)
"$DRIVE" key ctrl+= >/dev/null 2>&1
if expect_eventually zoom 1 "Ctrl+= zooms in"; then
  checks=$((checks + 1))
  got=$(snap | grep -oE 'scale=[0-9.]+' | head -1 | cut -d= -f2)
  if [ "$got" != "$base" ]; then ok "the font scale actually moved ($base -> $got)"
  else fail "scale unchanged at $got — the zoom did not reach fonts_build()"; fi
  "$DRIVE" key ctrl+0 >/dev/null 2>&1
  expect_eventually zoom 0 "Ctrl+0 resets it"
  checks=$((checks + 1))
  got=$(snap | grep -oE 'scale=[0-9.]+' | head -1 | cut -d= -f2)
  if [ "$got" = "$base" ]; then ok "back to $base"
  else fail "scale did not return: $got vs $base"; fi
else
  checks=$((checks + 3)); fail "zoom did not engage — skipping the scale and reset checks"
  "$DRIVE" key ctrl+0 >/dev/null 2>&1
fi

# Ctrl+, opens Preferences, and it opens on a category rather than a flat list.
"$DRIVE" key ctrl+, >/dev/null 2>&1
expect_eventually modal prefs "Ctrl+, opens Preferences"
# It reopens on the pane you left it on, which is why this selects rather than
# asserts a fixed category — an earlier section in this run leaves it on Messages.
click_cat 0
expect_eventually prefcat 0 "the Appearance pane selects"

# Switch to Advanced by clicking the row the app reported.
r=$(snap | grep -oE 'prefcat 3 name=Advanced r=[0-9,.-]*' | sed 's/.*r=//')
if [ -n "${r:-}" ]; then
  IFS=, read -r l t rt2 b <<<"$r"
  "$DRIVE" click $(( (${l%.*} + ${rt2%.*}) / 2 )) $(( (${t%.*} + ${b%.*}) / 2 )) >/dev/null 2>&1
  expect_eventually prefcat 3 "the category list switches panes"
else
  checks=$((checks + 1)); fail "no prefcat rects in the dump"
fi

# Text size and accent apply LIVE, and Cancel puts BOTH back — the whole point of
# the snapshot rule, and neither is a local variable: one rebuilds every font, the
# other re-resolves the palette.
"$DRIVE" key esc >/dev/null 2>&1; settle modal none
"$DRIVE" prefs >/dev/null 2>&1; settle modal prefs
# Back to Appearance FIRST: the sheet remembers the pane you left it on (which is
# right — you came back for the same thing), so the chips below are not on screen
# after the Advanced check above.
click_cat 0; settle prefcat 0
d=$(snap)
ts0=$(key_of "$d" textsize)
ac0=$(key_of "$d" scheme)
rail0=$(key_of "$d" railcol)
click_pref 7 2; settle textsize 2   # Text size -> Large
click_pref 6 2; settle scheme   2   # Colour scheme -> the third swatch
d=$(snap)
expect "$d" textsize 2 "a text size applies while the sheet is open"
expect "$d" scheme   2 "so does a colour scheme"
# A scheme is a PAIR: the rail has to move with the accent, or the picker is
# changing half of what it shows.
checks=$((checks + 1))
rail1=$(key_of "$d" railcol)
if [ -n "$rail1" ] && [ "$rail1" != "$rail0" ]; then ok "and the rail colour with it ($rail0 -> $rail1)"
else fail "the rail did not change with the scheme (still $rail1)"; fi
"$DRIVE" key esc >/dev/null 2>&1
settle modal none
settle textsize "$ts0"
d=$(snap)
expect "$d" textsize "$ts0" "Cancel RESTORES the text size"
expect "$d" scheme   "$ac0" "Cancel RESTORES the scheme"
checks=$((checks + 1))
if [ "$(key_of "$d" railcol)" = "$rail0" ]; then
  ok "... and the rail with it"
else fail "the rail did not come back: $(key_of "$d" railcol)"; fi

# --- the generic form on the modal frame ---------------------------
# Sixteen call sites went through a native GDI popup with its own window class and
# its own message loop. Now it is the app's modal frame with native EDITs on it, so
# the things that were previously unassertable are asserted: that Cancel does not
# commit, that Enter does, and that a checkbox and a choice chip actually change
# the value the caller receives.
say "== form"
"$DRIVE" view 0 >/dev/null 2>&1; "$DRIVE" channel general >/dev/null 2>&1; settle sbkind 1

"$DRIVE" form 3 >/dev/null 2>&1
settle modal form
d=$(snap)
expect "$d" modal   form "form opens on the modal frame"
expect "$d" form    1    "the form flag is set"
expect "$d" nfields 3    "all three fields are up"
expect "$d" covered 1    "the window is covered"
expect "$d" re      0    "the composer is hidden under it"
checks=$((checks + 1))
if printf '%s' "$d" | grep -q 'formfield 0 kind=0 edit=1'; then ok "the text field has a native EDIT"
else fail "no native EDIT for field 0"; fi

# Esc must NOT commit. The value is read back from the caller's array via `last`.
"$DRIVE" key esc >/dev/null 2>&1
expect_eventually modal none "Esc closes the form"
expect_grep 'last=cancel' "Esc means CANCEL"

# Enter commits, and a chip click reaches the caller's value.
"$DRIVE" form 3 >/dev/null 2>&1; settle modal form
"$DRIVE" key enter >/dev/null 2>&1
expect_eventually modal none "Enter closes the form"
expect_grep 'last=ok text="initial"' "Enter COMMITS the field values"

# --- pane headers close with a ✕, not a caption --------------------
say "== pane header"
"$DRIVE" view 0 >/dev/null 2>&1; "$DRIVE" channel general >/dev/null 2>&1; settle sbkind 1
"$DRIVE" search "" >/dev/null 2>&1
expect_eventually srch 1 "search pane is up"
# The ✕'s rect is frame-owned and reported in the dump, so the click comes from
# the app's own geometry rather than from arithmetic that breaks the moment the
# members pane is open and the middle column stops ending at the window edge.
r=$(snap | grep -o 'paneclose=[0-9,.-]*' | head -1 | cut -d= -f2)
if [ -n "${r:-}" ] && [ "${r%%,*}" != "0" ]; then
  IFS=, read -r pl pt pr pb <<<"$r"
  "$DRIVE" click $(( (${pl%.*} + ${pr%.*}) / 2 )) $(( (${pt%.*} + ${pb%.*}) / 2 )) >/dev/null 2>&1
  expect_eventually srch 0 "the pane close button closes it"
else
  checks=$((checks + 1)); fail "no paneclose= rect in the dump"
  "$DRIVE" key esc >/dev/null 2>&1
fi

# --- the composer is ours ------------------------------------------
# Every one of these was the RichEdit's job until today, which means every one of
# them is now code that can be wrong. They are asserted through `chars` (real
# WM_CHARs) rather than `type` (which sets the text), because typing is the path a
# user runs.
say "== composer"
"$DRIVE" view 0 >/dev/null 2>&1; "$DRIVE" channel general >/dev/null 2>&1; settle sbkind 1
ed() { snap | grep -oE "^ed .*" | head -1; }
edf() {                            # edf <field>  -> its value
  ed | grep -oE "$1=[-0-9]+" | head -1 | cut -d= -f2
}
# This section is a CHAIN — backspace assumes the typing landed, undo assumes the
# backspace did — so a single early race used to report five or six failures and
# bury the one that mattered. Each step now waits for its own effect, and a step
# that never lands stops the chain instead of cascading.
ed_wait() {                        # ed_wait <field> <want> — 0 if it got there
  local f="$1" want="$2" t=0
  while :; do
    [ "$(edf "$f")" = "$want" ] && return 0
    [ "$t" -ge "$WAIT_MS" ] && return 1
    sleep 0.1; t=$((t + 100))
  done
}
ed_step() {                        # ed_step <field> <want> <label>
  local f="$1" want="$2" label="$3"
  checks=$((checks + 1))
  if ed_wait "$f" "$want"; then ok "$label"; return 0; fi
  fail "$label — $f=$(edf "$f") wanted $want: $(ed)"
  return 1
}

"$DRIVE" chars "hello world" >/dev/null 2>&1
if ed_wait len 11 && [ "$(edf caret)" = "11" ]; then
  checks=$((checks + 1)); ok "typing inserts and moves the caret"

  "$DRIVE" key backspace  >/dev/null 2>&1; ed_step len   10 "Backspace deletes"
  "$DRIVE" key ctrl+left  >/dev/null 2>&1; ed_step caret 6  "Ctrl+Left jumps a word"
  "$DRIVE" key shift+left >/dev/null 2>&1; ed_step sel   1  "Shift+Left selects"
  "$DRIVE" key ctrl+z     >/dev/null 2>&1; ed_step len   11 "Ctrl+Z undoes the delete"
  "$DRIVE" key ctrl+a     >/dev/null 2>&1; ed_step sel   1  "Ctrl+A selects all"
else
  checks=$((checks + 6))
  fail "typing did not reach the composer: $(ed) — skipping the editing chain it feeds"
fi

# A click INSIDE the field places the caret — the field is drawn, not a child, so
# this goes through the same WM_LBUTTONDOWN a user generates.
r=$(ed | grep -oE 'box=[0-9,.-]+' | cut -d= -f2)
if [ -n "${r:-}" ]; then
  IFS=, read -r bl bt br bb <<<"$r"
  "$DRIVE" click $(( ${bl%.*} + 14 )) $(( (${bt%.*} + ${bb%.*}) / 2 )) >/dev/null 2>&1
  checks=$((checks + 1))
  t=0; while c=$(edf caret); [ -n "$c" ] && [ "$c" -ge 5 ] && [ $t -lt $WAIT_MS ]; do
    sleep 0.1; t=$((t + 100)); done
  if [ -n "$c" ] && [ "$c" -lt 5 ]; then ok "a click places the caret (caret=$c)"
  else fail "click-to-caret: $(ed)"; fi
else
  checks=$((checks + 1)); fail "no ed box in the dump"
fi

# The SYSTEM caret (REQ-269 / ARCH-99) — invisible by design, so the only way to
# know it is there is to ask. It is what screen readers and magnifiers follow, and
# a self-drawn field that never creates one leaves them unable to track typing at
# all. Asserted as a TRANSITION: that it exists, and that it MOVES with the text
# caret, because a caret pinned at the box origin looks identical to a working one
# in any single sample.
checks=$((checks + 1))
if snap | grep -q '^caret owned=1 '; then ok "a system caret exists while the composer has focus"
else fail "no system caret: $(snap | grep '^caret ')"; fi
cx() { snap | grep -oE '^caret owned=[0-9] x=[0-9-]+' | grep -oE 'x=[0-9-]+' | cut -d= -f2; }
"$DRIVE" key ctrl+a >/dev/null 2>&1; "$DRIVE" key backspace >/dev/null 2>&1; ed_wait len 0
x0=$(cx)
"$DRIVE" chars "system caret tracks" >/dev/null 2>&1; ed_wait len 19
x1=$(cx)
checks=$((checks + 1))
if [ -n "${x0:-}" ] && [ -n "${x1:-}" ] && [ "$x1" -gt "$x0" ]; then
  ok "and it follows the text caret ($x0 -> $x1)"
else fail "the system caret did not move with the caret ($x0 -> $x1)"; fi
"$DRIVE" key ctrl+a >/dev/null 2>&1; "$DRIVE" key backspace >/dev/null 2>&1; ed_wait len 0

# The mention popover still tracks the caret, and Tab accepts. `alice` is asserted
# to exist in the fixture at launch, so a miss here is the completion, not the
# roster — which is a distinction three earlier runs got wrong.
"$DRIVE" key ctrl+a >/dev/null 2>&1; "$DRIVE" key backspace >/dev/null 2>&1
ed_wait len 0
"$DRIVE" chars "hey @al" >/dev/null 2>&1
ed_wait len 7
"$DRIVE" key tab >/dev/null 2>&1
expect_grep '^ed .*text="hey @alice "' "Tab accepts a mention completion"

# Enter sends and clears; the draft machinery still works across a switch.
"$DRIVE" key enter >/dev/null 2>&1
ed_step len 0 "Enter sends and clears the field"

# A channel of its own for this check: asserting against whatever second channel a
# workspace happens to have made the result depend on the fixture, and a test that
# passes because of the fixture is a test that fails when someone gives you a clean
# one — which is exactly what happened.
"$DRIVE" mkchan smokedrafts 1 >/dev/null 2>&1
wait_grep '^  ch [0-9]+ "smokedrafts"' >/dev/null 2>&1 || true
"$DRIVE" chars "a draft" >/dev/null 2>&1
ed_wait len 7
"$DRIVE" channel smokedrafts >/dev/null 2>&1
ed_step len 0 "the other channel starts empty"
"$DRIVE" channel general >/dev/null 2>&1
expect_grep '^ed .*text="a draft"' "and the draft comes back"
"$DRIVE" key ctrl+a >/dev/null 2>&1; "$DRIVE" key backspace >/dev/null 2>&1

# --- formatting: the toolbar and its chords (REQ-220) ---------------
# Two paths into one operation — the buttons and the keys — so both are driven.
# Every assertion is against the TEXT, because what the toolbar produces has to
# be the plain markup a person could have typed (ARCH-100 §5); if these ever
# start asserting a style instead, the constraint has been lost.
say "== formatting"
fmtbar=$(snap | grep -m1 '^fmtbar ')
nbtns=$(echo "$fmtbar" | awk '{print NF - 2}')
checks=$((checks + 1))
[ "${nbtns:-0}" = "7" ] && ok "the composer publishes seven formatting buttons" \
                        || fail "formatting toolbar — expected 7 buttons, got ${nbtns:-<none>}"

fmt_rect() {                       # fmt_rect <index> -> "x y" at its centre
  echo "$fmtbar" | awk -v i="$1" '{ split($(i + 3), r, ","); print int((r[1]+r[3])/2), int((r[2]+r[4])/2) }'
}

"$DRIVE" chars "ship it" >/dev/null 2>&1; ed_wait len 7
"$DRIVE" key ctrl+a >/dev/null 2>&1; "$DRIVE" key ctrl+b >/dev/null 2>&1
expect_grep '^ed .*text="\*ship it\*"' "Ctrl+B wraps the selection"
"$DRIVE" key ctrl+b >/dev/null 2>&1
expect_grep '^ed .*text="ship it"' "and pressing it again takes it off"
"$DRIVE" key ctrl+a >/dev/null 2>&1; "$DRIVE" key ctrl+i >/dev/null 2>&1
expect_grep '^ed .*text="_ship it_"' "Ctrl+I italicises"
"$DRIVE" key ctrl+a >/dev/null 2>&1; "$DRIVE" key ctrl+shift+x >/dev/null 2>&1
expect_grep '^ed .*text="~_ship it_~"' "Ctrl+Shift+X strikes through, around it"
"$DRIVE" key ctrl+a >/dev/null 2>&1; "$DRIVE" key backspace >/dev/null 2>&1; ed_wait len 0
"$DRIVE" chars "run it" >/dev/null 2>&1; ed_wait len 6
"$DRIVE" key ctrl+a >/dev/null 2>&1; "$DRIVE" key ctrl+shift+c >/dev/null 2>&1
expect_grep '^ed .*text="`run it`"' "Ctrl+Shift+C marks it as code"

# The transcript's Ctrl+C used to claim the key whether or not it had a
# selection, so the FIELD's copy never ran and Ctrl+C there put nothing on the
# clipboard. Asserted as a round trip through the real clipboard.
"$DRIVE" key ctrl+a >/dev/null 2>&1; "$DRIVE" key ctrl+c >/dev/null 2>&1
"$DRIVE" key ctrl+a >/dev/null 2>&1; "$DRIVE" key backspace >/dev/null 2>&1; ed_wait len 0
"$DRIVE" key ctrl+v >/dev/null 2>&1
expect_grep '^ed .*text="`run it`"' "Ctrl+C in the composer copies"

# Block markers apply to every line the selection touches, and number themselves.
"$DRIVE" key ctrl+a >/dev/null 2>&1; "$DRIVE" key backspace >/dev/null 2>&1; ed_wait len 0
"$DRIVE" chars "one" >/dev/null 2>&1
"$DRIVE" key shift+enter >/dev/null 2>&1
"$DRIVE" chars "two" >/dev/null 2>&1
ed_wait len 7
"$DRIVE" key ctrl+a >/dev/null 2>&1; "$DRIVE" key ctrl+shift+8 >/dev/null 2>&1
ed_step len 11 "Ctrl+Shift+8 bullets every line in the selection"
"$DRIVE" key ctrl+shift+8 >/dev/null 2>&1
ed_step len 7 "and takes them all off again"
"$DRIVE" key ctrl+shift+7 >/dev/null 2>&1
expect_grep '^ed .*text="1\. one' "Ctrl+Shift+7 numbers them from one"
"$DRIVE" key ctrl+shift+7 >/dev/null 2>&1
"$DRIVE" key ctrl+shift+9 >/dev/null 2>&1
expect_grep '^ed .*text="> one' "Ctrl+Shift+9 quotes them"
"$DRIVE" key ctrl+shift+9 >/dev/null 2>&1; ed_wait len 7

# The BUTTON is the other path in, and clicking one must not be what takes the
# caret away from the text it is about to wrap.
"$DRIVE" key ctrl+a >/dev/null 2>&1; "$DRIVE" key backspace >/dev/null 2>&1; ed_wait len 0
"$DRIVE" chars "by hand" >/dev/null 2>&1; ed_wait len 7
"$DRIVE" key ctrl+a >/dev/null 2>&1
"$DRIVE" click $(fmt_rect 0) >/dev/null 2>&1
expect_grep '^ed .*text="\*by hand\*"' "clicking B does what Ctrl+B does"
"$DRIVE" key ctrl+a >/dev/null 2>&1; "$DRIVE" key backspace >/dev/null 2>&1; ed_wait len 0
# With no selection it opens a pair and puts the caret between them, so what you
# type next is already inside the markup.
"$DRIVE" key ctrl+b >/dev/null 2>&1
"$DRIVE" chars "inside" >/dev/null 2>&1
expect_grep '^ed .*text="\*inside\*"' "with no selection it opens a pair to type into"
"$DRIVE" key ctrl+a >/dev/null 2>&1; "$DRIVE" key backspace >/dev/null 2>&1; ed_wait len 0

# --- the field shows formatting, never markup ---------------------
# The delimiters are still in the buffer — every assertion here is on the TEXT,
# which is what gets sent — but they are drawn with no ink and no width. What
# that costs is caret arithmetic, and these are the rules that pay it.
say "== composer WYSIWYG"
fmt_reset() {
  "$DRIVE" key ctrl+a >/dev/null 2>&1; "$DRIVE" key backspace >/dev/null 2>&1; ed_wait len 0
  "$DRIVE" chars "$1" >/dev/null 2>&1; ed_wait len "$2"
}
# An arrow key crosses an invisible marker in the SAME press: nine visible steps
# from the start of "say *bold* now" land after the space, at buffer index 11.
fmt_reset 'say *bold* now' 14
"$DRIVE" key home >/dev/null 2>&1
for _ in 1 2 3 4 5 6 7 8 9; do "$DRIVE" key right >/dev/null 2>&1; done
ed_step caret 11 "arrow keys step over invisible delimiters, never into them"

# The caret canonicalises left, which is what makes emphasis sticky at the back
# edge and open at the front — the two behaviours a rich editor has.
fmt_reset 'a *bold*' 8
"$DRIVE" key end >/dev/null 2>&1; "$DRIVE" chars "X" >/dev/null 2>&1
expect_grep '^ed .*text="a \*boldX\*"' "typing at the end of emphasis continues it"
fmt_reset '*bold* b' 8
"$DRIVE" key home >/dev/null 2>&1; "$DRIVE" chars "X" >/dev/null 2>&1
expect_grep '^ed .*text="\*Xbold\* b"' "typing at the front of emphasis joins it"

# Markup must never APPEAR without being typed. Deleting the space before a bold
# word stops it parsing (MARKDOWN.md §2.1), so the delimiters go with it rather
# than showing up on screen as two asterisks nobody wrote.
fmt_reset 'a *x* b' 7
for _ in 1 2 3; do "$DRIVE" key left >/dev/null 2>&1; done
"$DRIVE" key back >/dev/null 2>&1
expect_grep '^ed .*text="ax b"' "breaking a construct drops the emphasis, not the disguise"

# And emptying one takes both delimiters, or ** would be left behind — which no
# longer parses either, so it would come back visible.
fmt_reset 'a *x* b' 7
for _ in 1 2; do "$DRIVE" key left >/dev/null 2>&1; done
"$DRIVE" key back >/dev/null 2>&1
expect_grep '^ed .*text="a  b"' "deleting the last character removes the emphasis whole"
"$DRIVE" key ctrl+a >/dev/null 2>&1; "$DRIVE" key backspace >/dev/null 2>&1; ed_wait len 0

# --- the editor preference ----------------------------------------
# Two modes, and BOTH are driven here: a preference with one tested path has an
# untested path, and this one changes what every keystroke does.
say "== editor preference"
"$DRIVE" editor plain >/dev/null 2>&1
fmt_reset 'say *bold* now' 14
"$DRIVE" key home >/dev/null 2>&1
for _ in 1 2 3 4 5; do "$DRIVE" key right >/dev/null 2>&1; done
ed_step caret 5 "plain text: the caret steps one character at a time, markup included"
# And nothing edits your markup for you — the rich mode's repair is inert here.
fmt_reset 'a *x* b' 7
for _ in 1 2 3; do "$DRIVE" key left >/dev/null 2>&1; done
"$DRIVE" key back >/dev/null 2>&1
expect_grep '^ed .*text="a \*\* b"' "plain text: the delimiters are left exactly as typed"
# The toolbar belongs to the rich editor: in plain text it would offer buttons
# for markup already on screen, and its row must give back the height too.
checks=$((checks + 1))
snap | grep -qE '^fmtbar hover=-?[0-9]+ (0,0,0,0 ?){7}$' \
  && ok "plain text: the formatting toolbar is collapsed" \
  || fail "plain text: toolbar still has hit-boxes — $(snap | grep '^fmtbar ')"

# Switching modes must not EDIT what you wrote. It did: the repair pass measures
# against a snapshot of what was invisible, so after a switch to plain — where
# nothing is hidden — the first keystroke read every delimiter as one that had
# stopped parsing and deleted it. Reported as "text jumping".
fmt_reset 'a *bold* b' 10
"$DRIVE" editor rich >/dev/null 2>&1
"$DRIVE" editor plain >/dev/null 2>&1
"$DRIVE" chars "X" >/dev/null 2>&1
expect_grep '^ed .*text="a \*bold\* bX"' "switching modes leaves the markup alone"

"$DRIVE" editor rich >/dev/null 2>&1
checks=$((checks + 1))
snap | grep -qE '^fmtbar hover=-?[0-9]+ (0,0,0,0 ?){7}$' \
  && fail "rich text: the toolbar did not come back" \
  || ok "rich text: the toolbar comes back"
fmt_reset 'say *bold* now' 14
"$DRIVE" key home >/dev/null 2>&1
for _ in 1 2 3 4 5; do "$DRIVE" key right >/dev/null 2>&1; done
ed_step caret 6 "rich text: the same five presses cross the invisible delimiter"
"$DRIVE" key ctrl+a >/dev/null 2>&1; "$DRIVE" key backspace >/dev/null 2>&1; ed_wait len 0

# The caret spans the LINE, whatever runs are on it. It used to take its height
# from the cluster it sat against, so the moment a hidden delimiter was next to
# it the caret became 0.1 DIP tall and sat on the baseline — reported as "the
# cursor is in the wrong alignment when formatting appears". Asserted
# as an equality rather than a number, so it holds at any DPI or text size.
caret_h() { snap | awk '/^edcaret /{ for (i=1;i<=NF;i++) if ($i ~ /^h=/) { sub(/^h=/,"",$i); print $i } }'; }
fmt_reset 'plain text' 10
plain_h=$(caret_h)
fmt_reset 'plain _text_' 12
checks=$((checks + 1))
[ -n "$plain_h" ] && [ "$(caret_h)" = "$plain_h" ] \
  && ok "the caret spans the line, with formatting or without (h=$plain_h)" \
  || fail "caret height changed with formatting — plain $plain_h, formatted $(caret_h)"
"$DRIVE" key ctrl+a >/dev/null 2>&1; "$DRIVE" key backspace >/dev/null 2>&1; ed_wait len 0

# The field's box must be at least as tall as a line, and the caret must not
# move when nothing has changed. Both are the same defect: the box was
# a hardcoded 20 DIP while a line is 22 x the text scale, so every line was
# clipped AND the caret-visibility scroll had no satisfiable answer — its two
# branches alternated on every repaint and the line oscillated by the
# difference. Reported as "text jumping up and down"; measured at 3.8 DIP with
# one zoom step applied, which is why the zoomed case is the one asserted.
fmt_reset 'i like you' 10
"$DRIVE" key ctrl+= >/dev/null 2>&1          # the case that made it worst
caret_top() { snap | awk '/^edcaret /{ split($2, a, ","); print a[2] }'; }
box_h() { snap | awk '/^ed /{ for (i=1;i<=NF;i++) if ($i ~ /^box=/) { sub(/^box=/,"",$i);
  split($i, b, ","); print b[4] - b[2] } }'; }
checks=$((checks + 1))
bh=$(box_h); ch=$(caret_h)
# Emptiness first. awk coerces an empty string to 0, so a dump missing `ed box=`
# or `edcaret h=` made the test 0 + 0.5 >= 0 -- true -- and the check reported ok
# with a blank measurement printed. A missing measurement is a failure, not a pass.
if [ -z "$bh" ] || [ -z "$ch" ]; then
  fail "no composer geometry in the dump (box='$bh' caret='$ch') — nothing was measured"
elif awk -v b="$bh" -v c="$ch" 'BEGIN{ exit !(b + 0.5 >= c) }'; then
  ok "the field's box is at least a line tall (box $bh >= caret $ch)"
else
  fail "composer box $bh is shorter than a line $ch — clipping, and the scroll cannot settle"
fi
before=$(caret_top)
"$DRIVE" shot _osc1 >/dev/null 2>&1; "$DRIVE" shot _osc2 >/dev/null 2>&1
checks=$((checks + 1))
[ "$(caret_top)" = "$before" ] \
  && ok "and the caret does not drift when nothing changes ($before)" \
  || fail "caret moved across repaints: $before -> $(caret_top) (caret oscillation)"
"$DRIVE" key ctrl+0 >/dev/null 2>&1
"$DRIVE" key ctrl+a >/dev/null 2>&1; "$DRIVE" key backspace >/dev/null 2>&1; ed_wait len 0

# --- send later (REQ-224) ---------------------------------------------------
# The queue, the sweep and the Scheduled tab were all built and NOTHING could
# create a row: the feature was unreachable from the product. This drives the
# chevron that fixed that.
say "== send later"
"$DRIVE" view home >/dev/null 2>&1; "$DRIVE" channel general >/dev/null 2>&1; settle conv 1
"$DRIVE" key ctrl+a >/dev/null 2>&1; "$DRIVE" key backspace >/dev/null 2>&1; ed_wait len 0
"$DRIVE" chars "for later" >/dev/null 2>&1; ed_wait len 9
schpt=$(snap | awk '{ if ($1 == "sched") { sub(/^btn=/,"",$2); split($2, b, ",");
  if (b[3] > b[1]) print int((b[1]+b[3])/2), int((b[2]+b[4])/2) } }')
checks=$((checks + 1))
[ -n "$schpt" ] && ok "the composer offers send-later beside Send" \
                || fail "no send-later chevron with text in the composer"
if [ -n "$schpt" ]; then
  "$DRIVE" click $schpt >/dev/null 2>&1
  expect_eventually menu 10 "and it opens its menu"
  # The first item sits below the section header; the menu anchors up-left of
  # the chevron, which is why this is computed rather than measured.
  mx=$(echo $schpt | cut -d' ' -f1); my=$(echo $schpt | cut -d' ' -f2)
  "$DRIVE" click $(( mx + 23 )) $(( my - 72 )) >/dev/null 2>&1
  ed_step len 0 "choosing a time takes the message out of the composer"
  expect_grep '^sched .* n=[1-9]' "and it is waiting in the queue"
fi

# --- New message, as a pane (REQ-229) ---------------------------------------
# The old picker asked WHO first and let you write second. This addresses and
# composes in one place, so the assertions follow that order: filter, pick,
# write, send — and the message that arrives must be exactly what was typed,
# once.
say "== new message"
"$DRIVE" newmsg >/dev/null 2>&1
expect_eventually tofocus 1 "the To: field takes the keys first"
"$DRIVE" key ctrl+a >/dev/null 2>&1; "$DRIVE" key backspace >/dev/null 2>&1
"$DRIVE" chars "bo" >/dev/null 2>&1
expect_eventually cand 1 "typing filters people and channels together"
"$DRIVE" key enter >/dev/null 2>&1
expect_eventually chips 1 "Enter takes the highlighted one"
# Enter with nothing left to pick is "done addressing", not another pick — it
# used to keep taking whoever was first in the roster.
"$DRIVE" key enter >/dev/null 2>&1
d=$(snap)
expect "$d" chips 1 "and a second Enter does not add another"
expect "$d" tofocus 0 "it moves to the message instead"
"$DRIVE" chars "from the pane" >/dev/null 2>&1
expect_grep '^ed .*text="from the pane"' "typing now goes to the message"

# Send resolves the recipient into a conversation and posts there.
sendpt=$(snap | awk '/^newmsg /{ for (i=1;i<=NF;i++) if ($i ~ /^send=/) { sub(/^send=/,"",$i);
  split($i, b, ","); print int((b[1]+b[3])/2), int((b[2]+b[4])/2) } }')
"$DRIVE" click $sendpt >/dev/null 2>&1
ed_step len 0 "sending clears the pane"
expect_grep '^  ch [0-9]+ DM .*prev="from the pane"' "and the message arrives, once"
# Reopening starts clean: the unaddressed draft went with the send. It did not,
# once — the clear used the addressed API, which refuses channel 0 outright.
"$DRIVE" newmsg >/dev/null 2>&1
ed_step len 0 "reopening the pane starts empty"
# Put the app back where the sections after this one expect it: they type into
# the CONVERSATION composer, and leaving the pane up pointed them at its field.
"$DRIVE" view home >/dev/null 2>&1; "$DRIVE" channel general >/dev/null 2>&1; settle conv 1

# --- selecting text with the mouse --------------------------------
# The self-drawn field replaced a native EDIT, and word selection left
# with the control: the window class never asked for CS_DBLCLKS, so Windows was
# not even sending WM_LBUTTONDBLCLK and a double-click just moved the caret
# twice. Nothing caught it because the harness could not express a gesture at
# all — `drag`, `dblclick` and `tripleclick` exist as of this section.
say "== selecting with the mouse"
ed_pt() {                          # ed_pt <dx> <dy> -> a point inside the field
  snap | awk -v dx="$1" -v dy="$2" '/^ed /{
    for (i = 1; i <= NF; i++) if ($i ~ /^box=/) { sub(/^box=/, "", $i); split($i, b, ",");
      if (b[3] > b[1]) print int(b[1] + dx), int(b[2] + dy); exit } }'
}
"$DRIVE" key ctrl+a >/dev/null 2>&1; "$DRIVE" key backspace >/dev/null 2>&1; ed_wait len 0
"$DRIVE" chars "select some of this text" >/dev/null 2>&1; ed_wait len 24
"$DRIVE" dblclick $(ed_pt 44 10) >/dev/null 2>&1
ed_step sel 1 "double-click selects a word in the composer"
# What it selected, asserted through the operation it exists to serve: the word
# and nothing but the word, or the delimiters land in the wrong place.
"$DRIVE" key ctrl+b >/dev/null 2>&1
expect_grep '^ed .*text="select \*some\* of this text"' "and it is the word under the pointer, exactly"
"$DRIVE" key ctrl+b >/dev/null 2>&1
"$DRIVE" tripleclick $(ed_pt 44 10) >/dev/null 2>&1
"$DRIVE" key ctrl+b >/dev/null 2>&1
expect_grep '^ed .*text="\*select some of this text\*"' "triple-click takes the whole field"
"$DRIVE" key ctrl+a >/dev/null 2>&1; "$DRIVE" key backspace >/dev/null 2>&1; ed_wait len 0
"$DRIVE" chars "drag across me" >/dev/null 2>&1; ed_wait len 14
"$DRIVE" drag $(ed_pt 2 10) $(ed_pt 60 10) >/dev/null 2>&1
ed_step sel 1 "dragging in the composer selects a range"
"$DRIVE" key ctrl+a >/dev/null 2>&1; "$DRIVE" key backspace >/dev/null 2>&1; ed_wait len 0

# The transcript has the same two gestures, and its selection is what Ctrl+C
# copies — so this asserts the round trip rather than the internal range.
"$DRIVE" send "the quick brown fox jumps" >/dev/null 2>&1
wait_grep '^tsel ' >/dev/null 2>&1 || true
body=$(snap | awk '/^  msgrow /{ for (i=1;i<=NF;i++) if ($i ~ /^body=/) { sub(/^body=/,"",$i); split($i,b,","); x=b[1]; y=b[2]; seen=1 } } END{ if (seen) print int(x)+12, int(y)+8 }')
# Empty means no msgrow was in the dump. The awk used to fall through to
# `print int(x)+12, int(y)+8` with x and y unset, i.e. the literal point "12 8",
# so the clicks landed in the corner and the selection assertions failed blaming
# the selection feature for an absent message row.
if [ -z "$body" ]; then
  fail "no msgrow in the dump — cannot locate a message to select"
else
"$DRIVE" dblclick $body >/dev/null 2>&1
expect_grep '^tsel has=1 a=[0-9]+:0 f=[0-9]+:3' "double-click selects a word in a message"
"$DRIVE" tripleclick $body >/dev/null 2>&1
expect_grep '^tsel has=1 a=[0-9]+:0 f=[0-9]+:25' "triple-click selects the whole message"
fi
"$DRIVE" key ctrl+c >/dev/null 2>&1
"$DRIVE" click $(ed_pt 44 10) >/dev/null 2>&1
"$DRIVE" key ctrl+v >/dev/null 2>&1
expect_grep '^ed .*text="the quick brown fox jumps"' "and Ctrl+C copies what was selected"
"$DRIVE" key ctrl+a >/dev/null 2>&1; "$DRIVE" key backspace >/dev/null 2>&1; ed_wait len 0

# --- clickable links (REQ-220) --------------------------------------
# The autolink BOUNDARY rules are unit-tested where they belong
# (tests/test_richtext.c, which can enumerate them). What only this suite can
# show is that the client agrees with the parser about which bytes are the link
# — the span has to survive a byte->UTF-16 remap and a DirectWrite hit test, and
# either of those being off by one is invisible in a screenshot and produces a
# link that opens the wrong address.
#
# Hence the trailing full stop below: it is in the MESSAGE and must not be in
# the LINK. That single character is the whole assertion — a hover that reported
# the URL with it attached would be a client that navigates somewhere else.
say "== links"
"$DRIVE" channel general >/dev/null 2>&1; settle conv 1
"$DRIVE" send "https://example.com/runbook." >/dev/null 2>&1
wait_grep '^tsel ' >/dev/null 2>&1 || true
lbody=$(snap | awk '/^  msgrow /{ for (i=1;i<=NF;i++) if ($i ~ /^body=/) { sub(/^body=/,"",$i); split($i,b,","); x=b[1]; y=b[2]; seen=1 } } END{ if (seen) print int(x)+12, int(y)+8 }')
if [ -z "$lbody" ]; then
  fail "no msgrow in the dump — cannot locate the link to hover"
else
"$DRIVE" move $lbody >/dev/null 2>&1
expect_grep '^link hover="https://example\.com/runbook"$' \
  "hovering a URL reports it, without the sentence's full stop"
fi
# Off the transcript entirely: the rail. This is the direction that was broken
# first time round — the hover was computed in the branch that owns the
# transcript's x range, so nothing ever cleared it on the way out.
"$DRIVE" move 5 5 >/dev/null 2>&1
expect_grep '^link hover=""$' "and leaving the transcript clears it"

# --- drafts that outlive the process (REQ-223, ARCH-101) -------------
# Drafts used to be 24 slots in memory: a switch survived them, a restart did
# not. They now live on the daemon, so the assertion that matters is the one the
# old ones could never pass — kill the client and find the text still there.
say "== drafts"
"$DRIVE" channel general >/dev/null 2>&1; settle conv 1
"$DRIVE" key ctrl+a >/dev/null 2>&1; "$DRIVE" key backspace >/dev/null 2>&1; ed_wait len 0
"$DRIVE" chars "unsent thoughts" >/dev/null 2>&1; ed_wait len 15
# The write is debounced (~2s) rather than sent per keystroke; wait for the
# state, do not sleep a guess.
expect_eventually drafthere 1 "typing a draft records it against this conversation"
d=$(snap)
expect "$d" draftn    1 "drafts: the model holds one"
expect "$d" draftrail 1 "drafts: the Home sidebar carries the destination"

# The whole point: a NEW PROCESS finds it.
"$DRIVE" launch >/dev/null 2>&1
if ! wait_grep 'authed=1 connected=1' 20000; then fail "relaunch did not authenticate"; fi
"$DRIVE" channel general >/dev/null 2>&1; settle conv 1
expect_typeable "a relaunched client takes the keyboard"
expect_grep '^ed .*text="unsent thoughts"' "and it is still there after a restart"

# Sending it is what ends it — on the daemon, in the send's own transaction.
"$DRIVE" key enter >/dev/null 2>&1
ed_step len 0 "sending clears the field"
expect_eventually drafthere 0 "and the draft with it"
d=$(snap)
expect "$d" draftn    0 "drafts: none left"
# The ROW stays — Slack's "Drafts & sent" is always in the sidebar and it is the
# COUNT that comes and goes. Asserting the row disappears would be asserting the
# mistake this branch corrected.
expect "$d" draftrail 1 "drafts: the destination stays, only the count clears"

# --- mentioning someone who is not in the channel (REQ-287) ------------------
# The defect this covers is a FALSE CONFIRMATION: the highlight is syntactic, so
# a mention that notified nobody looked exactly like one that worked. Asserted as
# a round trip — the notice appears, the remedy works, and afterwards the same
# mention is silent because it now resolves.
say "== mention of a non-member"
"$DRIVE" view 0 >/dev/null 2>&1; settle sbkind 1
"$DRIVE" mkchan solo 1 >/dev/null 2>&1
wait_grep '^  ch [0-9]+ "solo"' >/dev/null 2>&1 || true
"$DRIVE" channel solo >/dev/null 2>&1; settle re 1
useq=$(snap | grep -oE '^unres seq=[0-9]+' | cut -d= -f2)
"$DRIVE" send "hello @bob are you there" >/dev/null 2>&1
t=0; while [ "$(snap | grep -oE '^unres seq=[0-9]+' | cut -d= -f2)" = "$useq" ] && [ $t -lt $WAIT_MS ]; do
  sleep 0.1; t=$((t+100)); done
expect_grep '^unres seq=[0-9]+ n=1 can_add=1 priv=0 names="bob"' \
            "naming a non-member is reported to the sender, with a remedy"

# REQ-288: in a PUBLIC channel the mention still reaches them, so the notice
# offers membership rather than claiming they were not notified. The delivery
# itself needs a second account and is covered by hand (a mention of bob lands in
# bob's activity feed; a private channel leaves it empty) — asserted here is the
# half that lives in one session: that the sender is told, and told the truth.
checks=$((checks + 1))
if snap | grep -q '^unres .*priv=0 '; then ok "a public channel reports it as a membership gap, not a delivery failure"
else fail "public/private not distinguished: $(snap | grep '^unres ')"; fi
expect_eventually modal confirm "and it is offered as a confirmation, not a silent drop"

# Accepting it adds them — proven by the mention going quiet afterwards rather
# than by reading a members list, because resolution is the thing under test.
"$DRIVE" key enter >/dev/null 2>&1; settle modal none
useq=$(snap | grep -oE '^unres seq=[0-9]+' | cut -d= -f2)
"$DRIVE" channel solo >/dev/null 2>&1; settle re 1
"$DRIVE" send "second ping @bob" >/dev/null 2>&1
sleep 2
checks=$((checks + 1))
if [ "$(snap | grep -oE '^unres seq=[0-9]+' | cut -d= -f2)" = "$useq" ]; then
  ok "after adding them the same mention resolves and says nothing"
else fail "still reported as unresolved after the add"; fi

# A PRIVATE channel must say what adding someone discloses (cf. REQ-036a).
"$DRIVE" mkchan hush 0 >/dev/null 2>&1
wait_grep '^  ch [0-9]+ "hush"' >/dev/null 2>&1 || true
"$DRIVE" channel hush >/dev/null 2>&1; settle re 1
"$DRIVE" send "quiet word @carol" >/dev/null 2>&1
expect_grep '^unres seq=[0-9]+ n=1 can_add=1 priv=1 names="carol"' \
            "a private channel reports the disclosure it would make"
"$DRIVE" key esc >/dev/null 2>&1; settle modal none
"$DRIVE" view 0 >/dev/null 2>&1; "$DRIVE" channel general >/dev/null 2>&1; settle sbkind 1

# --- accessibility (REQ-269, ARCH-99) ---------------------------------------
# Two claims, and they are NOT the same claim. The dump proves what the app
# published; only a real UIA client proves anything can see it. The second is the
# feature, so the second is the one that gates.
say "== accessibility"
"$DRIVE" view 0 >/dev/null 2>&1; "$DRIVE" channel general >/dev/null 2>&1; settle sbkind 1
expect_grep '^a11y avail=1 ' "the UIA provider resolved"
checks=$((checks + 1))
n=$(snap | grep -oE '^a11y avail=[0-9] items=[0-9]+' | grep -oE 'items=[0-9]+' | cut -d= -f2)
if [ -n "${n:-}" ] && [ "$n" -ge 2 ]; then ok "the paint pass described $n elements"
else fail "nothing published to the accessibility tree (items=${n:-?})"; fi

# Announcements. What is SAID cannot be observed from here — the same honest
# limit as the tray balloon — so what is asserted is that the path runs:
# a failure raises one, and your own message does not, because being read your
# own words back is noise.
ann() { snap | grep -oE 'announced=[0-9]+' | cut -d= -f2; }
a0=$(ann)
"$DRIVE" send "smoke: my own message is not announced" >/dev/null 2>&1
settle re 1; sleep 1
checks=$((checks + 1))
if [ "$(ann)" = "$a0" ]; then ok "your own message raises no announcement"
else fail "a self-authored message was announced ($a0 -> $(ann))"; fi
"$DRIVE" mkchan general 1 >/dev/null 2>&1          # duplicate name -> a real failure toast
t=0; while [ "$(ann)" = "$a0" ] && [ $t -lt $WAIT_MS ]; do sleep 0.1; t=$((t+100)); done
checks=$((checks + 1))
if [ "$(ann)" != "$a0" ]; then ok "a failure is announced ($a0 -> $(ann))"
else fail "a failure raised no announcement (still $a0)"; fi

# The real gate: walk the tree from OUTSIDE the process, as a screen reader does.
# Always re-stage: `[ -f ] ||` left a stale copy in place, so a changed probe
# silently did not run — the same trap as a path-relative dump name.
cp "$HERE/scripts/uia_probe.ps1" "$HERE/scripts/uia_invoke.ps1" "$LIN_DIR/" 2>/dev/null
if [ -f "$LIN_DIR/uia_probe.ps1" ]; then
  checks=$((checks + 1))
  if powershell.exe -NoProfile -ExecutionPolicy Bypass \
       -File 'C:\Windows\Temp\octest\uia_probe.ps1' -Assert 2>/dev/null | grep -q 'uia_probe: OK'; then
    ok "a real UIA client can read the tree, and every control has a unique id"
  else
    fail "uia_probe failed — run it directly to see the walk"
  fi

  # ...and DRIVE it by id, which is the whole point of the identifiers (REQ-290).
  # Coordinates are not involved: this is the same route a UIA-based suite would
  # take, so a pane moving cannot break it.
  checks=$((checks + 1))
  if powershell.exe -NoProfile -ExecutionPolicy Bypass \
       -File 'C:\Windows\Temp\octest\uia_invoke.ps1' -Aid 'shelf.threads' 2>/dev/null |
       grep -q 'uia_invoke: OK'; then
    if wait_grep '^threads n=[0-9]+ '; then
      ok "a UIA client can PRESS a control by id, not only find it"
    else fail "invoking shelf.threads did not open the pane"; fi
  else fail "could not invoke shelf.threads through UI Automation"; fi
  "$DRIVE" view 0 >/dev/null 2>&1; settle sbkind 1
else
  say "   (could not stage uia_probe.ps1 — skipping the external check)"
fi

# --- keywords, priority people, schedule (REQ-135/136) -------------
# All three are SERVER state reached through a modal form, which is why the
# harness learned `formnext`: a form blocks the command loop, so every setting
# behind one was undrivable and therefore untested end to end.
say "== keywords, priority people and the schedule"
"$DRIVE" view 0 >/dev/null 2>&1; settle sbkind 1
"$DRIVE" menu 71 >/dev/null 2>&1          # the notifications overlay
kwb=$(snap | grep -oE '^kwbtn=[0-9]+,[0-9]+' | cut -d= -f2 | tr ',' ' ')
vipb=$(snap | grep -oE 'vipbtn=[0-9]+,[0-9]+' | cut -d= -f2 | tr ',' ' ')
checks=$((checks + 1))
if [ -n "$kwb" ] && [ -n "$vipb" ]; then ok "the overlay offers both editors"
else fail "no keyword/priority buttons in the dump"; fi

"$DRIVE" formnext "deploy, release train" >/dev/null 2>&1
[ -n "$kwb" ] && "$DRIVE" click $kwb >/dev/null 2>&1
expect_grep '^keywords="deploy,release train"' "keywords save through the real button"

"$DRIVE" formnext "bob" >/dev/null 2>&1
[ -n "$vipb" ] && "$DRIVE" click $vipb >/dev/null 2>&1
expect_grep '^keywords=.* nvip=1 ' "a priority person resolves by name and saves"

# A name that is nobody is REPORTED, not silently dropped — the list is a
# promise about who reaches you, so a typo must not fail quietly.
# Every toast is also ANNOUNCED (ARCH-99), and the announcement counter is the
# one the dump carries — `toasts_raised` counts OS tray balloons, which is a
# different thing and stays at zero here.
a0=$(ann)
"$DRIVE" formnext "bob, nosuchperson" >/dev/null 2>&1
[ -n "$vipb" ] && "$DRIVE" click $vipb >/dev/null 2>&1
t=0; while [ "$(ann)" = "$a0" ] && [ $t -lt $WAIT_MS ]; do sleep 0.1; t=$((t+100)); done
checks=$((checks + 1))
if [ "$(ann)" != "$a0" ]; then ok "an unknown name is reported rather than dropped"
else fail "a typo in the priority list was swallowed (announcements still $a0)"; fi
# ...and the people who DO resolve are still saved: reporting the miss must not
# discard the rest of what was typed.
expect_grep '^keywords=.* nvip=1 ' "the names that resolve are saved anyway"

# The schedule (REQ-136) is inline in the SAME overlay, as Slack keeps it: one
# row of modes, and Custom expands the seven days in place. Driven by clicking
# the real chips and the real time dropdown, which is the only way to know the
# hit-boxes and the dropdown's scroll arithmetic are right.
mode_xy() { snap | grep -oE '^schedmodes=.*' | cut -d= -f2 | cut -d" " -f$(( $1 + 1 )) | tr ',' ' '; }
"$DRIVE" click $(mode_xy 1) >/dev/null 2>&1     # Every day
expect_grep '^keywords=.* schedmode=1 schedwin=480-1320' "Every day saves as allowed hours, 08:00-22:00"
"$DRIVE" click $(mode_xy 2) >/dev/null 2>&1     # Weekdays
expect_grep '^keywords=.* schedmode=2 ' "Weekdays is its own mode, not a second window"

# The time dropdown: open it on the base window's start and pick a value. The
# list scrolls, so the row a click lands on is computed from its position rather
# than its index — the arithmetic that would silently set the wrong hour.
basestart=$(snap | grep -oE '^schedbase=[0-9]+,[0-9]+' | cut -d= -f2 | tr ',' ' ')
[ -n "$basestart" ] && "$DRIVE" click $basestart >/dev/null 2>&1
expect_grep '^schedbase=.* tpopen=1 tprows=[1-9]' "a time field opens the dropdown"
"$DRIVE" key esc >/dev/null 2>&1
expect_grep '^schedbase=.* tpopen=0 ' "escape closes it without changing anything"

# Custom expands seven days, pre-filled as a working week — every day off would
# be a schedule that silences everything the moment it is chosen.
"$DRIVE" click $(mode_xy 3) >/dev/null 2>&1
expect_grep '^keywords=.* schedmode=3 .*scheddays=7' "Custom expands the whole week"
expect_grep '^  schedrow wd=1 on=1 win=540-1020' "Monday is on, 09:00-17:00"
expect_grep '^  schedrow wd=0 on=0 ' "Sunday is off"
# A day toggles through its own checkbox.
satchk=$(snap | grep -E '^  schedday d=6 ' | grep -oE 'chk=[0-9]+,[0-9]+' | cut -d= -f2 | tr ',' ' ')
[ -n "$satchk" ] && "$DRIVE" click $satchk >/dev/null 2>&1
expect_grep '^  schedrow wd=6 on=1 ' "a day switches on from its checkbox"

# All of it is the daemon's, so a new process finds it — the check the client's
# own memory could never fail.
"$DRIVE" kill >/dev/null 2>&1
"$DRIVE" launch >/dev/null 2>&1
wait_for authed 1 >/dev/null 2>&1
expect_typeable "the client takes the keyboard after a restart"
expect_grep '^keywords="deploy,release train" nvip=1 schedmode=3 .*scheddays=7' \
            "keywords, priority people and the whole custom week survive a restart"

"$DRIVE" menu 71 >/dev/null 2>&1
"$DRIVE" click $(mode_xy 0) >/dev/null 2>&1     # Any time
expect_grep '^keywords=.* schedmode=0 ' "turning the schedule off leaves nothing behind"
"$DRIVE" key esc >/dev/null 2>&1; settle modal none

# --- the chrome survives scaling ---------------------------------
# The defects this covers appear ONLY when something scales, which is why they
# went unseen: at 100% the shell is clean. `chromefit` is computed from the same
# element rects the automation surface publishes (REQ-290) — siblings that
# overlap, and anything drawn entirely off its own surface. A healthy layout is
# 0 0 at every combination.
say "== chrome fit across dpi x zoom x text size"
"$DRIVE" view 0 >/dev/null 2>&1; "$DRIVE" channel general >/dev/null 2>&1; settle sbkind 1
fitcheck() {   # dpi zoom textsize label
  "$DRIVE" dpi "$1" >/dev/null 2>&1
  "$DRIVE" zoom "$2" >/dev/null 2>&1
  "$DRIVE" textsize "$3" >/dev/null 2>&1
  sleep 0.4
  checks=$((checks + 1))
  local line; line=$(snap | grep -E '^chromefit ')
  case "$line" in
    *"overlaps=0 outside=0"*) ok "$4" ;;
    *) fail "$4 — $line" ;;
  esac
}
# WITH TEXT IN THE COMPOSER. The first version of this matrix ran against an
# EMPTY composer, which is one line tall and fits at every scale — so it reported
# a clean shell while the typed text was being drawn straight through the +,
# emoji and @ icons. The composer is the one control whose height depends on its
# contents; testing it empty tests the case that cannot fail.
"$DRIVE" type "twenty twentyone twentytwo twentythree twentyfour" >/dev/null 2>&1
sleep 0.3
fitcheck  96  0 1 "the shell fits at 100%"
fitcheck  96  0 3 "...at the largest text size"
fitcheck  96  4 3 "...and zoomed in on top of it"
fitcheck 192  0 1 "...at 200% DPI"
fitcheck 240  4 3 "...and at everything at once, where it used to collide"
# The panes and the densest overlay, at the size that broke them.
for v in threads people drafts activity; do
  "$DRIVE" view "$v" >/dev/null 2>&1; sleep 0.4
  checks=$((checks + 1))
  line=$(snap | grep -E '^chromefit ')
  case "$line" in
    *"overlaps=0 outside=0"*) ok "$v fits at the largest scale" ;;
    *) fail "$v does not fit — $line" ;;
  esac
done
# A composer that has grown to its four-line maximum, at the largest scale: the
# tallest the field ever gets against the row it collided with.
"$DRIVE" view 0 >/dev/null 2>&1; settle sbkind 1
"$DRIVE" type " and now a much longer paragraph that has to wrap several times over so the field grows to the maximum height it is ever allowed to reach" >/dev/null 2>&1
sleep 0.4
checks=$((checks + 1))
line=$(snap | grep -E '^chromefit ')
case "$line" in
  *"overlaps=0 outside=0"*) ok "a grown composer stays clear of its action row" ;;
  *) fail "the composer collides — $line" ;;
esac
"$DRIVE" key ctrl+a >/dev/null 2>&1; "$DRIVE" key backspace >/dev/null 2>&1
"$DRIVE" menu 71 >/dev/null 2>&1; sleep 0.5
checks=$((checks + 1))
line=$(snap | grep -E '^chromefit ')
case "$line" in
  *"overlaps=0 outside=0"*) ok "the notifications card keeps its content inside itself" ;;
  *) fail "the modal spills — $line" ;;
esac

# --- "The Notifications card clips its own content at high DPI" -------------
# Cited by title, not by number: an issue number in a comment rots as soon as
# the issue is closed, renamed or superseded.
# `chromefit` above cannot see this: it compares published rectangles, and the
# card's own rows were published at coordinates well past its bottom edge while
# every element it does know about sat inside. The card's content is a fixed
# ~420 DIP tall; a window's DIP extent shrinks in inverse proportion to DPI, so
# at 240 DPI the body is ~250 DIP and the weekday rows were drawn outside the
# clip — invisible, and still hit-testable, so a click on empty card background
# toggled a weekday nobody could see.
say "== the notifications card at high dpi"
"$DRIVE" dpi 240 >/dev/null 2>&1
settle modal notify
# A paint has to have HAPPENED at the new scale before any rectangle is read.
# `dpi` acks when its handler ran, and the hit rects are recorded during
# WM_PAINT, so reading them straight after the verb returns the previous scale's
# geometry. That is how this block failed the first time it ran.
sleep 0.5
cardr=$(snap | grep -oE 'card=[0-9]+,[0-9]+,[0-9]+,[0-9]+' | head -1 | cut -d= -f2)
cl=$(echo "$cardr" | cut -d, -f1); ct=$(echo "$cardr" | cut -d, -f2)
cr=$(echo "$cardr" | cut -d, -f3); cb=$(echo "$cardr" | cut -d, -f4)
cmx=$(( (cl + cr) / 2 )); cmy=$(( (ct + cb) / 2 ))

# reach <sed-program> — reads a published "x,y" out of the dump, wheeling DOWN a
# notch at a time until it stops reporting 0,0. Every rectangle in this card is
# now clamped to what was painted, so 0,0 means "below the fold", and the way to
# a control below the fold is to scroll to it. That is also the assertion: this
# block arrives at the matrix's largest scale (dpi 240, zoom 4, text size 3),
# where even the mode chips start below the fold — before the scroller existed
# there was no sequence of inputs that could reach them at all.
# The step is 96 DIP (a -240 wheel delta is two notches of 48), deliberately
# SMALLER than the card's visible body at the scale this runs at — which measures
# ~119 DIP. A coarser step can carry a control from below the fold to above it
# between two samples and report it unreachable when it is not; that happened with
# a 240 DIP step, and it looks exactly like the defect being tested for.
reach() {
  local v i
  for i in $(seq 0 16); do
    v=$(snap | sed -nE "$1" | head -1 | tr -d '\r')
    [ -n "$v" ] && [ "$v" != "0,0" ] && { printf '%s' "$v"; return 0; }
    "$DRIVE" wheel "$cmx" "$cmy" -240 >/dev/null 2>&1; sleep 0.25
  done
  return 1
}

# Custom is the mode that expands the seven weekday rows, and so the mode that
# makes the card outgrow the window.
chip=$(reach 's/^schedmodes=.*[ ]([0-9]+,[0-9]+)$/\1/p') || chip=""
checks=$((checks + 1))
if [ -n "$chip" ]; then
  "$DRIVE" click "${chip%,*}" "${chip#*,}" >/dev/null 2>&1
  if wait_for schedmode 3; then ok "the Custom chip is reachable and selects Custom"
  else fail "the Custom chip was published at $chip but clicking it did nothing"; fi
else fail "the Custom chip is unreachable at any scroll offset"; fi

# Scroll until Monday is on screen. Selecting Custom leaves the card at the top,
# where the weekday rows it just added are still below the fold — so this has to
# happen BEFORE the count below, or that count reads zero rows and the check that
# follows is vacuous.
monchk=$(reach 's/^  schedday d=1 .*chk=([0-9]+,[0-9]+).*/\1/p') || monchk=""

# A row published outside the card is a row that can be clicked but not seen.
outside=0
for yy in $(snap | grep -E '^  schedday d=[0-6] ' | sed -E 's/.*chk=[0-9]+,([0-9]+).*/\1/'); do
  [ "$yy" = 0 ] && continue
  [ "$yy" -gt "$cb" ] && outside=$((outside + 1))
done
# Guarded, because this passes for the WRONG reason when no weekday row is drawn
# at all: "none of them is outside the card" is then true and meaningless. It
# passed exactly that way against the unfixed client, where Custom was never
# reached — the fake pass this suite's header is about.
nrows=$(snap | grep -cE '^  schedday d=[0-6] chk=[1-9]')
mode=$(key_of "$(snap)" schedmode)
checks=$((checks + 1))
if [ "$mode" != 3 ]; then
  fail "no weekday rows were ever drawn (schedmode=$mode) — this check verified nothing"
elif [ "$nrows" = 0 ]; then
  fail "Custom is selected but no weekday row could be scrolled into view"
elif [ "$outside" = 0 ]; then
  ok "no weekday row is hit-testable outside the card ($nrows on screen)"
else
  fail "$outside weekday rows are live below the card bottom ($cb) — invisible and clickable"
fi

# The control, and it is the important one: clamping every rectangle to nothing
# would satisfy every check here. A row that IS visible must still toggle. Done
# now, while Monday is known to be on screen.
monbefore=$(snap | grep -E '^  schedrow wd=1 ' | sed -E 's/.*on=([01]).*/\1/' | tr -d '\r')
[ -n "$monchk" ] && "$DRIVE" click "${monchk%,*}" "${monchk#*,}" >/dev/null 2>&1
sleep 0.4
monafter=$(snap | grep -E '^  schedrow wd=1 ' | sed -E 's/.*on=([01]).*/\1/' | tr -d '\r')
checks=$((checks + 1))
if [ -n "$monbefore" ] && [ "$monbefore" != "$monafter" ]; then
  ok "a visible weekday still toggles (on=$monbefore -> on=$monafter)"
  "$DRIVE" click "${monchk%,*}" "${monchk#*,}" >/dev/null 2>&1   # put it back
else fail "clicking a VISIBLE weekday changed nothing — the clamp is too wide"; fi

# ... and the rows that are now hidden must be REACHABLE, or the fix is just a
# thinner kind of broken. Sunday is the last row the card ever draws.
checks=$((checks + 1))
if reach 's/^  schedday d=0 .*chk=([0-9]+,[0-9]+).*/\1/p' >/dev/null; then
  ok "the card scrolls far enough to reach Sunday"
else fail "Sunday is unreachable at every scroll offset"; fi


"$DRIVE" key esc >/dev/null 2>&1; settle modal none
# Back to the defaults, since the text size is a persisted preference.
"$DRIVE" dpi 96 >/dev/null 2>&1; "$DRIVE" zoom 0 >/dev/null 2>&1; "$DRIVE" textsize 1 >/dev/null 2>&1
sleep 0.4

# --- pausing notifications (REQ-278) -------------------------------
# The pause is SERVER state, not a client mood: the restart below is the point of
# the section — a client that only remembered it locally would pass every other
# check here and still show "not paused" to somebody who is.
say "== pause notifications"
"$DRIVE" view 0 >/dev/null 2>&1; settle sbkind 1
"$DRIVE" menu 57 >/dev/null 2>&1          # start from resumed, whatever a previous run left
expect_grep '^snoozed=0 ' "starts unpaused"
"$DRIVE" menu 58 >/dev/null 2>&1          # for 30 minutes
checks=$((checks + 1))
if wait_grep '^snoozed=1 snooze_until=1[0-9]{12}'; then
  ok "pausing for 30 minutes sets an absolute end instant"
else fail "pause did not take: $(snap | grep '^snoozed=')"; fi
until0=$(snap | grep -oE '^snoozed=1 snooze_until=[0-9]+' | cut -d= -f3)

# It outlives the process, because the daemon holds it and tells the client at
# auth — the frame is unconditional, so "not paused" is never inferred from
# silence.
"$DRIVE" kill >/dev/null 2>&1
"$DRIVE" launch >/dev/null 2>&1
wait_for authed 1 >/dev/null 2>&1
expect_typeable "the client takes the keyboard after a restart"
checks=$((checks + 1))
if wait_grep "^snoozed=1 snooze_until=$until0"; then
  ok "the pause survives a restart, to the same instant"
else fail "pause lost across a restart: $(snap | grep '^snoozed=')"; fi

"$DRIVE" menu 57 >/dev/null 2>&1          # resume
expect_grep '^snoozed=0 snooze_until=0 ' "resuming ends it, and ends it now"

# --- Threads, across channels (REQ-062) ---------------------------
# The claim is that this is a SERVER answer: threads I am in wherever they are,
# with unread counts a channel cursor cannot produce. So it is seeded from a
# second account and driven through the shelf row.
say "== threads"
"$DRIVE" view 0 >/dev/null 2>&1; "$DRIVE" channel general >/dev/null 2>&1; settle conv 1
gid=$(snap | grep -oE '^  ch [0-9]+ "general"' | grep -oE '[0-9]+' | head -1)
[ -z "$gid" ] && gid=1
"$DRIVE" send "a thread starts here" >/dev/null 2>&1
sleep 1
root=$(snap | grep -oE '^  msgrow [0-9]+ mid=[0-9]+' | tail -1 | grep -oE 'mid=[0-9]+' | cut -d= -f2)
checks=$((checks + 1))
if [ -n "$root" ]; then ok "a root message to hang a thread on"
else fail "no message id to reply to"; fi
# bob replies, so the thread has a reply from somebody else — the unread case.
#
# demo_client is BUILT here, not merely tested for. Nothing built it before this
# point -- the activity section does, 50 lines below -- so on a clean checkout the
# guard skipped the setup while the two assertions under it ran unconditionally.
# bob never replied and both failed, blaming the product for a missing fixture.
if make -C "$HERE" demo-client >/dev/null 2>&1 && [ -n "$root" ] && [ -x "$HERE/build/demo_client" ]; then
  "$HERE/build/demo_client" 127.0.0.1 "$OC_DEV_PORT" bob pw reply "$gid" "$root" "and bob answers" >/dev/null 2>&1
  sleep 1
  "$DRIVE" view threads >/dev/null 2>&1
  expect_grep '^threads n=[1-9]' "the thread appears in the cross-channel list"
  expect_grep "^  thread root=$root .*unread=1 follow=1" "with bob's reply counted unread, and me following"
else
  say "   (demo_client unavailable — skipping the cross-channel unread checks)"
  "$DRIVE" view threads >/dev/null 2>&1
fi

# Opening it is reading it: the unread count is about replies you have not seen.
card=$(snap | grep -E "^  thread root=$root " | grep -oE 'at=[0-9]+,[0-9]+' | cut -d= -f2 | tr ',' ' ')
[ -n "$card" ] && "$DRIVE" click $card >/dev/null 2>&1
sleep 1
"$DRIVE" view threads >/dev/null 2>&1
expect_grep "^  thread root=$root .*unread=0 " "opening it clears its unread replies"

# Unread-only re-asks the server rather than filtering what is loaded.
ubtn=$(snap | grep -E '^threads ' | grep -oE 'btn=[0-9]+,[0-9]+' | cut -d= -f2 | tr ',' ' ')
[ -n "$ubtn" ] && "$DRIVE" click $ubtn >/dev/null 2>&1
expect_grep '^threads n=0 unreadonly=1' "unreads-only asks again and comes back empty"
[ -n "$ubtn" ] && "$DRIVE" click $ubtn >/dev/null 2>&1
expect_grep '^threads n=[1-9] unreadonly=0' "and back"

# --- People (REQ-289) ---------------------------------------------
# The directory exists because the roster now carries title/timezone/status. The
# check that matters is that it filters, since the pane is otherwise a list.
say "== people"
"$DRIVE" view people >/dev/null 2>&1
expect_grep '^people n=[3-9] q="" box=1' "the directory lists the workspace, with its search box up"
"$DRIVE" dirfind bob >/dev/null 2>&1
expect_grep '^people n=1 q="bob"' "typing a name filters to one person"
"$DRIVE" dirfind "" >/dev/null 2>&1
expect_grep '^people n=[3-9] q=""' "clearing it restores everyone"
"$DRIVE" view 0 >/dev/null 2>&1; settle sbkind 1

# --- one selected row at a time ----------------------------------
# Reported from a screenshot: the Drafts pane lit its own shelf row AND left the
# conversation lit underneath it, so two places claimed to be where you were.
say "== sidebar selection"
"$DRIVE" view 0 >/dev/null 2>&1; "$DRIVE" channel general >/dev/null 2>&1; settle sbkind 1
expect_grep '^sbsel=1' "Home: the open conversation is the one selected row"
"$DRIVE" view drafts >/dev/null 2>&1; settle sbkind 1
expect_grep '^sbsel=1' "Drafts: its shelf row is selected and the conversation is not"
"$DRIVE" view 0 >/dev/null 2>&1; settle sbkind 1

# --- Activity: the three unread tabs ------------------------------
# These tabs are a different QUESTION, not a client-side filter of the involved-me
# feed, so they are asserted end to end: a message arrives from SOMEBODY ELSE
# (demo_client as bob — one session cannot make itself an unread), and the tab is
# reached by CLICKING its chip, which is the path that broke when the chips
# wrapped onto a second row.
say "== activity unread filters"
if make -C "$HERE" demo-client >/dev/null 2>&1 && [ -x "$HERE/build/demo_client" ]; then
  # Read somewhere else first: reading #general is what clears its unread.
  "$DRIVE" view 0 >/dev/null 2>&1; "$DRIVE" channel solo >/dev/null 2>&1; settle sbkind 1
  gid=$(snap | grep -oE '^  ch [0-9]+ "general"' | grep -oE '[0-9]+' | head -1)
  if [ -z "$gid" ]; then gid=1; fi
  "$HERE/build/demo_client" 127.0.0.1 "$OC_DEV_PORT" bob pw send "$gid" "unread from bob" >/dev/null 2>&1
  "$DRIVE" view activity >/dev/null 2>&1; settle sbkind 3
  chip() { snap | grep -E "^  actchip i=$1 " | grep -oE 'c[xy]=[0-9]+' | cut -d= -f2 | tr '\n' ' '; }
  # 4 = Unreads, 5 = DMs, 6 = Channels.
  "$DRIVE" click $(chip 4) >/dev/null 2>&1
  expect_grep '^acttab=4 actwire=1 ' "clicking Unreads selects it and asks the server the unread question"
  checks=$((checks + 1))
  if wait_grep '^acttab=4 actwire=1 actrows=[1-9]'; then ok "an unread from another person is listed"
  else fail "Unreads is empty after bob posted to #general: $(snap | grep '^acttab=')"; fi
  "$DRIVE" click $(chip 6) >/dev/null 2>&1
  checks=$((checks + 1))
  if wait_grep '^acttab=6 actwire=3 actrows=[1-9]'; then ok "the same message is a Channels unread"
  else fail "Channels missed a channel unread: $(snap | grep '^acttab=')"; fi
  "$DRIVE" click $(chip 5) >/dev/null 2>&1
  expect_grep '^acttab=5 actwire=2 actrows=0' "DMs does not claim a channel message"
  # Reading it clears it — the cursor, not a client-side dismissal.
  "$DRIVE" view 0 >/dev/null 2>&1; "$DRIVE" channel general >/dev/null 2>&1; settle re 1
  sleep 1
  "$DRIVE" view activity >/dev/null 2>&1; settle sbkind 3
  "$DRIVE" click $(chip 4) >/dev/null 2>&1
  expect_grep '^acttab=4 actwire=1 actrows=0' "reading the channel clears it from Unreads"
  # And the involved-me tabs stay a LOCAL filter: All must not have been re-asked
  # into an unread answer.
  "$DRIVE" click $(chip 0) >/dev/null 2>&1
  expect_grep '^acttab=0 actwire=0 ' "All goes back to the involved-me question"
else
  say "   (demo_client unavailable — skipping the unread-tab checks)"
fi

# --- the composer cue tracks the conversation ------------------------------
# It was a cached global that went stale on a channel switch ("Message bob" while
# reading alice), so it is asserted rather than eyeballed.
say "== composer cue"
"$DRIVE" view 0 >/dev/null 2>&1; "$DRIVE" channel general >/dev/null 2>&1; settle sbkind 1
expect_grep 'composer_cue="Message #general"' "cue names the channel"

[ "${1:-}" = "--kill" ] && "$DRIVE" kill >/dev/null 2>&1

say ""
if [ "$fails" -eq 0 ]; then say "gui_smoke: OK — $checks checks"; exit 0; fi
say "gui_smoke: FAILED — $fails of $checks checks"; exit 1
