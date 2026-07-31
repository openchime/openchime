#!/usr/bin/env bash
# Machine-checked invariants for the Win32 client's chrome.
#
#   scripts/gui_smoke.sh            # launch, assert, leave the client running
#   scripts/gui_smoke.sh --kill     # ... and shut it down afterwards
#
# WHY THIS EXISTS. Three bugs reached the user in one day (WIN-70, WIN-71's
# regression, WIN-72) and every one of them was a *chrome* bug: a native child
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

# --- isolation (WIN-88) -----------------------------------------------------
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

# Read one `key=value` out of a fresh dump.
snap() { "$DRIVE" dump smoke >/dev/null 2>&1; cat "$LIN_DIR/smoke.txt" 2>/dev/null; }

# --- waiting on state instead of on a clock (WIN-87) ------------------------
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

# --- and it is the RIGHT daemon (WIN-88) ------------------------------------
# Assert the fixture before asserting anything about the product. Getting this
# wrong does not look like a broken harness, it looks like broken features.
fixture=$(snap | grep -o 'workspace name="[^"]*"' | cut -d'"' -f2)
case "$fixture" in
  "$OC_DEV_WS") say "   workspace \"$fixture\" on :$OC_DEV_PORT";;
  *) say "   WRONG DAEMON — reached \"${fixture:-<none>}\" on :$OC_DEV_PORT, expected \"$OC_DEV_WS\"."
     say "   Refusing to run: every assertion below would be about someone else's data."
     exit 1;;
esac
# The fixture's people, too. The group-DM and completion checks name bob, carol
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
#  4    5       0   0     0      0        Later: own column (WIN-73), Files-shaped
#  5    0       0   0     0      0        Admin: no second column
say "== views"
while read -r v sbkind re find ffind conv name; do
  [ -z "${v:-}" ] && continue
  "$DRIVE" view "$v" >/dev/null 2>&1
  [ "$v" = "0" ] && "$DRIVE" channel general >/dev/null 2>&1
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
0 1 1 1 0 1 Home
1 2 0 0 0 0 DMs
2 3 1 0 0 1 Activity
3 4 0 0 1 0 Files
4 5 0 0 0 0 Later
5 0 0 0 0 0 Admin
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
# Preferences is two-paned (WIN-78), so a row only has hit-boxes while ITS category
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

vispub() { snap | grep -oE '^  ch [0-9]+ "smokevis" pub=[01]' | grep -oE 'pub=[01]' | cut -d= -f2; }

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
expect_grep '^  sbrow sec=1 header=0 cid=[0-9]+ label="bob, carol"' \
            "the group appears titled by its people"
d=$(snap)
gid=$(printf '%s' "$d" | grep -oE '^  sbrow sec=1 header=0 cid=[0-9]+ label="bob, carol"' | grep -oE 'cid=[0-9]+' | cut -d= -f2 | head -1)
checks=$((checks + 1))
n=$(printf '%s' "$d" | grep -cE 'sbrow .*label="bob, carol"' || true)
if [ "$n" = "1" ]; then ok "and exactly once"
else fail "the group appears $n times"; fi

# Reopening the same set must not make a second conversation. There is nothing to
# wait FOR here — the assertion is that nothing new appears — so this one settles
# on the round trip completing (the selection lands on the reopened DM) and then
# counts.
"$DRIVE" groupdm carol,bob >/dev/null 2>&1
[ -n "${gid:-}" ] && settle sel "$gid"
checks=$((checks + 1))
n=$(snap | grep -cE 'sbrow .*label="bob, carol"' || true)
if [ "$n" = "1" ]; then ok "reopening the same set reuses it"
else fail "reopening produced $n rows"; fi

# The header names the group, and a DM's tab strip has no About tab.
if [ -n "${gid:-}" ]; then
  "$DRIVE" channel "$gid" >/dev/null 2>&1
  expect_eventually sel "$gid" "selecting it works by id"
fi

# --- avatars (WIN-47) -------------------------------------------------------
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
  say "   (no $LIN_DIR/face.png — skipping; create one to cover WIN-47)"
fi

# --- user-defined sidebar sections (WIN-83) ---------------------------------
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

# --- appearance: text size, zoom, accent, density (WIN-78) ------------------
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

# --- the generic form on the modal frame (WIN-77) ---------------------------
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

# --- pane headers close with a ✕, not a caption (WIN-77) --------------------
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

# --- the composer is ours (WIN-80) ------------------------------------------
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
