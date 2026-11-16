#!/usr/bin/env bash
# scripts/test/lib/clean_run.sh <command> [args...]
#
# Runs a build or test command under the rules in docs/TESTING.md, "One build
# at a time":
#
#   - One build at a time. Before the command starts, this takes an exclusive
#     lock on one file shared by every checkout on the machine
#     (/tmp/openchime-build.lock); a second run says it is waiting and starts
#     when the first ends. The lock is closed in the command, so nothing the
#     command leaves behind holds it. It is not one of the vendored
#     libraries' own locks (scripts/build/mbedtls.sh), so the two never wait
#     on each other.
#   - One compile at a time, checked while it runs. The command and everything
#     it starts carry this run's mark in their environment (OC_CLEAN_RUN).
#     If two compiler or assembler processes that carry it run at once,
#     neither started by the other, both are named, the run is stopped, and it
#     fails. A compiler and what it starts itself count as one: cc and its
#     cc1 and as, a wrapper script and the compiler it runs, emcc and its
#     clang.
#   - Nothing left running. When the command ends, a process that still
#     carries the mark is named and stopped, wherever it moved itself (a new
#     session included), and the run fails. Interrupted with Ctrl-C or a TERM,
#     the command's process group and whatever else carries the mark are
#     stopped.
#
# Otherwise it exits with the command's own status (130 on INT, 143 on TERM).
# Only processes carrying this run's mark are ever counted or stopped: a
# compile or a daemon a person started themselves is not ours.
#
# Needs: flock, ps, and /proc (Linux), which is where a process's environment
# is read.
# Run by: the Makefile, once for whatever goals were asked (its first lines).
# Tested by: scripts/test/fast/clean_run.sh.
set -uo pipefail
[ $# -gt 0 ] || { echo "usage: scripts/test/lib/clean_run.sh <command> [args...]" >&2; exit 2; }

lock="${OC_CLEAN_RUN_LOCK:-/tmp/openchime-build.lock}"
exec 9>>"$lock" || { echo "clean_run: cannot open the lock $lock" >&2; exit 1; }
if ! flock -n 9; then
  echo "clean_run: another build holds $lock; waiting for it to end" >&2
  flock 9
fi

CMD="$*"
# The mark: given to the command alone, not to this script, so this script's
# own helpers never carry it.
id="$$.$(date +%s%N)"
mark="OC_CLEAN_RUN=$id"

# The pids of this user's processes that carry the mark, one per line. One
# grep over /proc: another user's environment cannot be read and is skipped.
marked() {
  grep -lzxF -- "$mark" /proc/[0-9]*/environ 2>/dev/null | cut -d/ -f3 | sort -n
}

# The compile commands running now: marked processes that are a compiler or an
# assembler and were not started by another one. One line each: pid and
# command line. A script run by an interpreter (a wrapper named cc, emcc.py)
# is known by the script's name.
names='^(cc|c[+][+]|gcc|g[+][+]|clang|clang[+][+]|cc1|cc1plus|as|nasm|yasm|zig|emcc|em[+][+])(-[0-9][0-9.]*)?$|-(cc|c[+][+]|gcc|g[+][+]|clang|clang[+][+]|as)(-[0-9][0-9.]*)?$'
compiles() {
  local pids
  pids="$(marked | tr '\n' ',')"; pids="${pids%,}"
  [ -n "$pids" ] || return 0
  ps -o pid=,ppid=,args= -p "$pids" 2>/dev/null | awk -v re="$names" '
    function base(p,   n, parts) { n = split(p, parts, "/"); return parts[n] }
    { pid = $1; parent[pid] = $2; line[pid] = $0
      name = base($3)
      if (name ~ /^(sh|bash|dash|env|python[0-9.]*)$/ && NF >= 4) { name = base($4); sub(/[.]py$/, "", name) }
      compiler[pid] = (name ~ re) ? 1 : 0 }
    END {
      for (p in compiler) if (compiler[p]) {
        top = 1; q = parent[p]
        for (hops = 0; (q in parent) && hops < 64; hops++) { if (compiler[q]) { top = 0; break } q = parent[q] }
        if (top) { sub(/^ +/, "", line[p]); print line[p] }
      } }' | sort -n
}

child=""
watcher=""
status=""
interrupted=0
done_cleaning=0
caught="$(mktemp)"

stop_pids() {  # TERM, a few seconds, then KILL
  local pids="$1" pid
  [ -n "$pids" ] || return 0
  kill -TERM $pids 2>/dev/null
  for _ in $(seq 1 30); do
    local alive=0
    for pid in $pids; do kill -0 "$pid" 2>/dev/null && alive=1; done
    [ "$alive" = 0 ] && return 0
    sleep 0.1
  done
  kill -KILL $pids 2>/dev/null
}

cleanup() {
  [ "$done_cleaning" = 1 ] && return
  done_cleaning=1
  trap '' INT TERM
  [ -n "$watcher" ] && kill "$watcher" 2>/dev/null && wait "$watcher" 2>/dev/null
  if [ -s "$caught" ]; then
    echo "clean_run: two compiles ran at once during '$CMD'; a build compiles one file at a time:" >&2
    cat "$caught" >&2
    status=1
  fi
  local left pid
  if [ "$interrupted" = 0 ] && [ ! -s "$caught" ]; then
    # The command ended by itself: what it started should have ended too. A
    # moment for a process that is on its way out, then whatever still carries
    # the mark was left running.
    for _ in $(seq 1 20); do
      left="$(marked)"
      [ -z "$left" ] && break
      sleep 0.1
    done
    if [ -n "$left" ]; then
      echo "clean_run: still running after '$CMD' ended:" >&2
      for pid in $left; do
        printf '  %s %s\n' "$pid" "$(ps -o args= -p "$pid" 2>/dev/null | cut -c1-160)" >&2
      done
      echo "clean_run: stopping them; failing the run" >&2
      status=1
    fi
  fi
  rm -f "$caught"
  # The command's own group first, so nothing in it starts more work; then
  # whatever else carries the mark (a process that took a session of its own).
  if [ -n "$child" ]; then
    kill -TERM -- "-$child" 2>/dev/null
    for _ in $(seq 1 20); do
      kill -0 -- "-$child" 2>/dev/null || break
      sleep 0.1
    done
    kill -KILL -- "-$child" 2>/dev/null
  fi
  stop_pids "$(marked | tr '\n' ' ')"
}

on_signal() { interrupted=1; status="$1"; cleanup; exit "$1"; }
trap 'on_signal 130' INT
trap 'on_signal 143' TERM
trap 'cleanup; exit "${status:-1}"' EXIT

# Its own process group (job control), so cleanup can stop all of it and
# nothing else; stdin from /dev/null, since a background group cannot read
# the terminal; the lock's descriptor closed, so nothing it leaves behind
# holds the lock; the mark in its environment and in no other.
set -m
OC_CLEAN_RUN="$id" "$@" < /dev/null 9>&- &
child=$!
set +m

# The watch: two compiles at once are named in $caught, and the command's
# group stopped.
(
  while kill -0 "$child" 2>/dev/null; do
    now="$(compiles)"
    if [ "$(printf '%s' "$now" | grep -c .)" -ge 2 ]; then
      printf '%s\n' "$now" | sed 's/^/  /' | cut -c1-200 > "$caught"
      kill -TERM -- "-$child" 2>/dev/null
      exit 0
    fi
    sleep 0.2
  done
) 9>&- &
watcher=$!

wait "$child"
rc=$?
cleanup
status="${status:-$rc}"
[ "$status" = 1 ] && [ "$rc" != 0 ] && [ "$rc" -lt 128 ] && status=$rc
exit "$status"
