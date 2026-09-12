#!/usr/bin/env bash
# Does the Win32 client BOOT and RUN? That is the whole question.
#
#   scripts/gui_smoke.sh            # launch, assert, leave the client running
#   scripts/gui_smoke.sh --kill     # ... and shut it down afterwards
#
# THIS IS A SMOKE TEST, AND IT IS THE ONLY GUI HARNESS. It asserts that the
# client starts, reaches a daemon, paints its shell, takes the keyboard, sends a
# message and answers a shortcut — and then it stops. It does NOT cover the
# composer's editing rules, the modal frame, the schedule card, the scaling
# matrix, drafts, threads, emoji, avatars or accessibility, and it is not
# supposed to.
#
# The narrowness is the point, not an apology for it. This runs before every
# push, so it has to be cheap enough that nobody is tempted to skip it, and
# narrow enough that a failure means "the client is broken" rather than "one of
# two hundred assertions moved". When it fails, stop and look.
#
# KEEP IT THIS SIZE. A suite of a few hundred assertions grew here once, took
# seven minutes, and was skipped for it. Adding "just one more check" is how that
# happens again — a feature is verified by driving it, by hand or by a harness of
# its own, not by making the boot check longer.
#
# WHAT IT WILL AND WILL NOT CATCH. It catches a client that will not start, will
# not connect, will not authenticate, paints no shell, cannot be typed into,
# cannot send, or has lost its message-loop shortcuts — the failures that make
# every other test meaningless. It catches nothing subtler, deliberately.
#
# NOT IN CI. The daemon is epoll-based, so it is Linux-only, and GitHub's Windows
# runners cannot host it (no Linux containers). Until a self-hosted Windows box
# exists, this is the pre-push gate — run it and read it, the same discipline as
# reading CI.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DRIVE="$HERE/scripts/gui_drive.sh"
LIN_DIR='/mnt/c/Windows/Temp/octest'
START=$SECONDS
fails=0
checks=0

# --- isolation -----------------------------------------------------
# Its OWN daemon on its OWN port, wiped each run, and it says so rather than
# adopting whatever is listening. gui_drive's staleness check keeps a daemon
# whose start time postdates the binary, which silently adopts an UNRELATED
# daemon if one holds the port — a run once bound to a nine-hour-old daemon
# holding real workspace data and reported confident nonsense. A test that can be
# wrong about which machine it is testing is not a test.
export OC_DEV_PORT="${OC_DEV_PORT:-9500}"
export OC_DEV_DIR="${OC_DEV_DIR:-/tmp/oc-smoke}"
export OC_DEV_WS="${OC_DEV_WS:-Smoke Fixture}"

say()  { printf '%s\n' "$*"; }
fail() { printf '  FAIL %s\n' "$*"; fails=$((fails + 1)); }
ok()   { printf '  ok   %s\n' "$*"; }

# STOP THE DAEMON BEFORE WIPING ITS DIRECTORY. Deleting the directory under a
# running daemon does not reset it: SQLite keeps writing to the unlinked inode,
# so the workspace stays live while its files are gone. And killing it needs the
# ENVIRONMENT, not the command line — the daemon is started as
# `env OPENCHIME_PROTO_PORT=… openchimed`, so its own cmdline carries no port and
# `pkill -f OPENCHIME_PROTO_PORT=9500` matches nothing while looking exactly like
# "nothing to kill".
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

# How long a wait tolerates before calling it a failure. Only failing runs pay it.
WAIT_MS="${OC_SMOKE_WAIT_MS:-6000}"

# --- driving -------------------------------------------------------
# EVERY VERB'S EXIT STATUS IS CHECKED, which the regression suite does not do and
# is the single biggest reason a long run there produces failures that are not
# defects: gui_drive exits non-zero when the client never acked, and a discarded
# status turns a dropped command into the NEXT assertion failing for a reason
# that has nothing to do with it. A verb that does not ack is a broken client,
# and this suite says so at the point it happens.
drive() {
  if ! "$DRIVE" "$@" >/dev/null 2>&1; then
    fail "the client did not answer '$*' — it is wedged or gone"
    checks=$((checks + 1))
    return 1
  fi
  return 0
}

# One fresh dump. `ack` means the handler ran, not that its effect is painted —
# most of the interesting fields are recorded during WM_PAINT — so nothing here
# reads a dump once and hopes; it waits for the state it is about to assert.
snap() { "$DRIVE" dump smoke >/dev/null 2>&1; cat "$LIN_DIR/smoke.txt" 2>/dev/null; }

key_of() { printf '%s' "$1" | grep -o "\b$2=[^ ]*" | head -1 | cut -d= -f2; }

wait_for() {                       # wait_for <key> <want> [ms]
  local key="$1" want="$2" ms="${3:-$WAIT_MS}" t=0
  while :; do
    [ "$(key_of "$(snap)" "$key")" = "$want" ] && return 0
    [ "$t" -ge "$ms" ] && return 1
    sleep 0.05; t=$((t + 50))
  done
}

wait_grep() {                      # wait_grep <regex> [ms]
  local re="$1" ms="${2:-$WAIT_MS}" t=0
  while :; do
    snap | grep -qE "$re" && return 0
    [ "$t" -ge "$ms" ] && return 1
    sleep 0.05; t=$((t + 50))
  done
}

settle() { wait_for "$1" "$2" >/dev/null 2>&1 || true; }

expect() {                         # expect <dump> <key> <want> <label>
  local dump="$1" key="$2" want="$3" label="$4" got
  checks=$((checks + 1))
  got=$(key_of "$dump" "$key")
  if [ "$got" = "$want" ]; then ok "$label ($key=$got)"
  else fail "$label — expected $key=$want, got ${got:-<missing>}"; fi
}

expect_eventually() {              # expect_eventually <key> <want> <label>
  local key="$1" want="$2" label="$3" got
  checks=$((checks + 1))
  if wait_for "$key" "$want"; then ok "$label ($key=$want)"; return 0; fi
  got=$(key_of "$(snap)" "$key")
  fail "$label — expected $key=$want, got ${got:-<missing>} after ${WAIT_MS}ms"
  return 1
}

expect_grep() {                    # expect_grep <regex> <label>
  checks=$((checks + 1))
  if wait_grep "$1"; then ok "$2"; return 0; fi
  fail "$2 — no dump matched /$1/ within ${WAIT_MS}ms"
  return 1
}

# --- 1. it starts, and it reaches a daemon ---------------------------------
say "== it starts"
"$DRIVE" launch >/dev/null 2>&1 || { say "launch failed — see gui_drive.sh launch"; exit 1; }
if ! wait_grep 'authed=1 connected=1' 20000; then
  say "   NOT AUTHED — the client started and never reached :$OC_DEV_PORT"
  exit 1
fi
ok "the client launches, connects and authenticates"
checks=$((checks + 1))

# Assert the fixture before asserting anything about the product: reaching the
# wrong daemon does not look like a broken harness, it looks like broken features.
fixture=$(snap | grep -o 'workspace name="[^"]*"' | cut -d'"' -f2)
checks=$((checks + 1))
case "$fixture" in
  "$OC_DEV_WS") ok "and it is the smoke fixture on :$OC_DEV_PORT, not somebody's workspace" ;;
  *) fail "WRONG DAEMON — reached \"${fixture:-<none>}\", expected \"$OC_DEV_WS\""
     say "   Refusing to go on: every assertion below would be about the wrong data."
     exit 1 ;;
esac

# --- 2. it paints a shell you can use --------------------------------------
# The three predicates in winmain.c, in the one view that has to work: a channel
# list on the left, a conversation in the middle, nothing covering the window.
# The full seven-view matrix is the regression suite's; this asks only whether
# the shell came up at all.
say "== it paints"
drive view home && drive channel general
settle sbkind 1
d=$(snap)
expect "$d" sbkind  1 "the channel list is the second column"
expect "$d" conv    1 "the middle column is a conversation"
expect "$d" covered 0 "nothing is covering the window"
expect "$d" re      1 "the composer has a box to draw into"

# --- 3. it takes the keyboard ----------------------------------------------
# A window shown by a process that does not own the foreground is not activated,
# and the composer's auto-focus is gated on having focus — so a client that fails
# this looks fine in a screenshot and ignores every key. Both halves are checked
# because they are different facts: the WINDOW can hold the keyboard while the
# composer has not claimed it.
say "== it takes the keyboard"
checks=$((checks + 1))
t=0; got=""
while :; do
  d=$(snap)
  case "$d" in *"wnd_focus=1"*) case "$d" in *$'\ned '*"focus=1"*) got=1;; esac;; esac
  [ -n "$got" ] && break
  [ "$t" -ge 8000 ] && break
  sleep 0.1; t=$((t + 100))
done
if [ -n "$got" ]; then ok "the window is focused and the composer holds the caret"
else fail "the composer never took the keyboard: $(snap | grep -E '^wnd_|^ed ' | tr '\n' ' ')"; fi

# --- 4. it sends a message -------------------------------------------------
# The end-to-end path in one assertion: real WM_CHARs into the self-drawn field,
# Enter, and the message coming back from the daemon into the transcript. If this
# works, the wire, the composer, the send path and the fan-out are all alive.
#
# The text is UNIQUE PER RUN, so the assertions below cannot be satisfied by a
# message an earlier run left behind. The default wipe would give that too, but
# only as a side effect — and OC_SMOKE_KEEP_DB=1 removes it, at which point a
# fixed string would make this check pass without anything being sent. A test
# that can pass for the wrong reason is worse than one that is absent.
say "== it sends"
MSG="smoke $$ $(date +%s)"
drive key ctrl+a; drive key backspace
drive chars "$MSG"
expect_grep "^ed .*text=\"$MSG\"" "typing reaches the composer"
drive key enter
expect_eventually len 0 "Enter sends and clears the field"
# Two separate facts, because they fail separately: the daemon accepted it and
# said so (the channel's preview is server state), and the transcript actually
# painted a row for it. Do not fold them into one alternation: the `msgrow` line
# carries geometry and no body, so a pattern that would accept either really only
# ever tests the preview, while reading as though it tested both.
expect_grep "prev=\"$MSG\"" "the daemon stored it and sent it back"
expect_grep '^msgrows n=[1-9]' "and the transcript painted a row for it"

# --- 5. its shortcuts still work -------------------------------------------
# Asserted as a round trip, not a state: "Esc closed the palette" checked against
# pal=0 passes when the palette never opened. And asserted with the caret in the
# composer, which is the case that used to be dead — the chords are dispatched
# from the message loop precisely so a focused field cannot eat them.
say "== its shortcuts work"
drive key ctrl+k
if expect_eventually pal 1 "Ctrl+K opens the command palette from the composer"; then
  drive key esc
  expect_eventually pal 0 "and Esc closes it"
else
  drive key esc
fi

# --- 6. it is still alive --------------------------------------------------
# Everything above could pass against a client that crashed on the last keystroke,
# because a stale dump file reads exactly like a fresh one. So the last thing this
# does is make the client answer.
say "== it survives"
checks=$((checks + 1))
if "$DRIVE" dump smoke >/dev/null 2>&1; then ok "the client is still answering at the end of the run"
else fail "the client stopped answering — it died during the run"; fi

[ "${1:-}" = "--kill" ] && "$DRIVE" kill >/dev/null 2>&1

say ""
say "(a boot check, not a feature suite — see the header)"
if [ "$fails" -eq 0 ]; then say "gui_smoke: OK — $checks checks in $((SECONDS - START))s"; exit 0; fi
say "gui_smoke: FAILED — $fails of $checks checks in $((SECONDS - START))s"; exit 1
