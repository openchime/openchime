#!/usr/bin/env bash
# scripts/test/fast/clean_run.sh
#
# scripts/test/lib/clean_run.sh holds every build to the rules in
# docs/details/TESTING.md, "One build at a time". With stub commands, and a lock of
# its own, this shows:
#   1. the command's own status comes back;
#   2. two compilers at once fail the run and are named and stopped, and a
#      compiler that starts another (cc and its cc1) does not;
#   3. a process left running when the command ends fails the run and is
#      named and stopped, one that took a session of its own included; the
#      container runtime's pause process (catatonit -P) is not the command's,
#      and is neither counted nor stopped;
#   4. a second run waits for the first to end, and says so.
# The stubs are `sleep` under a compiler's name (exec -a): the watch knows a
# process by its command line, as it would a real cc.
#
# Needs: flock, setsid, pgrep, mkfifo.
# Run by: scripts/test/fast/source.sh (make test; make test-all).
. "$(dirname "$0")/../lib/common.sh"
need flock "util-linux"
need setsid "util-linux"
run="$PWD/scripts/test/lib/clean_run.sh"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
export OC_CLEAN_RUN_LOCK="$tmp/lock"
bad=0
no() { error "$*"; bad=1; }

# 1. The status.
"$run" true > /dev/null 2>&1 || no "a passing command failed under clean_run"
rc=0; "$run" sh -c 'exit 7' > /dev/null 2>&1 || rc=$?
[ "$rc" = 7 ] || no "a command's status 7 came back as $rc"

# 2. Two compilers at once, well before the stubs' 30 seconds.
start=$SECONDS
if "$run" bash -c '(exec -a cc sleep 30.171) & (exec -a x86_64-w64-mingw32-clang sleep 30.172) & wait' > /dev/null 2> "$tmp/two.err"; then
    no "a run with two compilers at once passed"
fi
[ $((SECONDS - start)) -lt 15 ] || no "two compilers at once were not caught while the run went on"
grep -q "two compiles ran at once" "$tmp/two.err" || no "the failure does not say two compiles ran at once"
grep -q "sleep 30.171" "$tmp/two.err" && grep -q "sleep 30.172" "$tmp/two.err" || no "the failure does not name both compilers"
if pgrep -f 'sleep 30[.]17[12]' > /dev/null; then
    no "the two compilers were left running"
    pkill -f 'sleep 30[.]17[12]' || true
fi
# One compiler and the one it starts are one compile.
"$run" bash -c 'exec -a cc bash -c "(exec -a cc1 sleep 1.5) & wait"' > /dev/null 2> "$tmp/chain.err" ||
    no "a compiler that started another was taken for two: $(tr '\n' ' ' < "$tmp/chain.err")"
# So are a wrapper script and the compiler it runs.
mkdir -p "$tmp/bin"
printf '#!/usr/bin/env bash\n(exec -a clang sleep 1.5) & wait\n' > "$tmp/bin/cc"
chmod +x "$tmp/bin/cc"
"$run" "$tmp/bin/cc" > /dev/null 2> "$tmp/wrap.err" ||
    no "a wrapper and its compiler were taken for two: $(tr '\n' ' ' < "$tmp/wrap.err")"

# 3. Left running: in the command's group, and in a session of its own.
if "$run" bash -c 'sleep 30.173 & exit 0' > /dev/null 2> "$tmp/left.err"; then
    no "a run that left a process running passed"
fi
grep -q "still running" "$tmp/left.err" && grep -q "sleep 30.173" "$tmp/left.err" || no "the failure does not name the process left running"
if "$run" bash -c 'setsid sleep 30.174 & exit 0' > /dev/null 2> "$tmp/away.err"; then
    no "a run that left a process in a session of its own passed"
fi
grep -q "sleep 30.174" "$tmp/away.err" || no "the failure does not name the process that took a session of its own"
if pgrep -f 'sleep 30[.]17[34]' > /dev/null; then
    no "a process left running was not stopped"
    pkill -f 'sleep 30[.]17[34]' || true
fi
# The container runtime's pause process, left by a run, is not the run's. The
# stand-in is a shell under that name, waiting on a pipe nothing writes to.
mkfifo "$tmp/pipe"
"$run" bash -c 'setsid bash -c "echo \$\$ > \"\$2\"; exec -a catatonit bash -P <> \"\$1\"" _ "$1" "$2" & exit 0' _ "$tmp/pipe" "$tmp/pause.pid" > /dev/null 2> "$tmp/pause.err" ||
    no "a run that left the container runtime's pause process failed: $(tr '\n' ' ' < "$tmp/pause.err")"
pause="$(cat "$tmp/pause.pid" 2>/dev/null || true)"
if [ -n "$pause" ] && kill -0 "$pause" 2>/dev/null; then
    kill "$pause" 2>/dev/null || true
else
    no "the container runtime's pause process was stopped"
fi
# Only that: the same name doing anything else is the command's.
if "$run" bash -c '(exec -a catatonit sleep 30.175) & exit 0' > /dev/null 2> "$tmp/init.err"; then
    no "a run that left catatonit running, not as the pause process, passed"
fi
grep -q "sleep 30.175" "$tmp/init.err" || no "the failure does not name the catatonit left running"
if pgrep -f 'sleep 30[.]175' > /dev/null; then
    no "the catatonit left running was not stopped"
    pkill -f 'sleep 30[.]175' || true
fi

# 4. One run at a time.
: > "$tmp/order"
"$run" sh -c "echo first start >> '$tmp/order'; sleep 2; echo first end >> '$tmp/order'" > /dev/null 2>&1 &
first=$!
for _ in $(seq 1 50); do grep -q "first start" "$tmp/order" && break; sleep 0.1; done
"$run" sh -c "echo second start >> '$tmp/order'; echo second end >> '$tmp/order'" > /dev/null 2> "$tmp/second.err" || no "the second run failed"
wait "$first" || no "the first run failed"
order="$(tr '\n' ' ' < "$tmp/order")"
[ "$order" = "first start first end second start second end " ] || no "two runs overlapped: $order"
grep -q "waiting" "$tmp/second.err" || no "the second run did not say it was waiting"

[ "$bad" = 0 ] || exit 1
ok "the status comes back, two compiles at once and a process left running fail the run, the runtime's pause process does not, one run at a time"
