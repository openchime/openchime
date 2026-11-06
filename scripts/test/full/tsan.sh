#!/usr/bin/env bash
# scripts/test/full/tsan.sh
#
# The suites SUITES[] marks THREADS or TIMED (tests/main.c, `--list
# threads`) under ThreadSanitizer (docs/TESTING.md §2.4); a data race between
# our threads fails it (tests/tsan.supp). This is the only run of a THREADS
# suite in the full suite: the plain pass (full/suites.sh) leaves it out. The
# vendored libraries are not instrumented, so a race inside one is not seen.
# Address randomization is off (setarch -R): ThreadSanitizer maps its shadow
# memory at fixed addresses, and recent kernels' randomization places the
# binary where it has to go.
#
# One process, the suites one after another, nothing beside it. Under the
# sanitizer no test sits out a set time (tests/check.h, TIMER_ONLY) and loops
# run fewer turns (TSAN_FEW); the end-to-end walks (WALK in SUITES[]) run
# plainly instead (full/suites.sh), so the pass takes about 30 seconds in CI.
# It prints how long it took; time never fails it.
#
# Needs: setarch; build/tests-tsan.
# Run by: make test-all (PART=threads).
. "$(dirname "$0")/../lib/common.sh"
need setarch "util-linux"
have build/tests-tsan "make build-all PART=threads builds it"
if [ -z "${OC_TEST_ONLY:-}" ]; then OC_TEST_ONLY=$(suites threads setarch "$(uname -m)" -R ./build/tests-tsan); fi
export OC_TEST_ONLY OC_TEST_JOBS=1
export TSAN_OPTIONS="halt_on_error=1 second_deadlock_stack=1 suppressions=$PWD/tests/tsan.supp"
out=$(mktemp)
trap 'rm -f "$out"' EXIT

start=$(date +%s%N)
rc=0
setarch "$(uname -m)" -R ./build/tests-tsan 2>&1 | tee "$out" || rc=$?
took_ms=$(( ($(date +%s%N) - start) / 1000000 ))
took="$(( took_ms / 1000 )).$(( took_ms % 1000 / 100 ))s"
[ "$rc" = 0 ] || fail "under ThreadSanitizer, a suite failed (above), after $took"
ok "the threaded suites passed under ThreadSanitizer, one after another, in $took"
