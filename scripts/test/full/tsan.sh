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
# Each suite runs as its own process, TSAN_JOBS of them at once (default: one
# per CPU): a race is between one suite's threads, so nothing is lost, and the
# pass takes about as long as its slowest suite rather than all of them
# together. Each suite's output is printed whole, in list order, once all have
# finished; any failing fails the pass, named.
#
# Needs: setarch; build/tests-tsan.
# Run by: make test-all (PART=threads).
. "$(dirname "$0")/../lib/common.sh"
need setarch "util-linux"
have build/tests-tsan "make build-all PART=threads builds it"
only=${OC_TEST_ONLY:-$(suites threads setarch "$(uname -m)" -R ./build/tests-tsan)}
jobs=${TSAN_JOBS:-$(nproc 2>/dev/null || echo 2)}
export TSAN_OPTIONS="halt_on_error=1 second_deadlock_stack=1 suppressions=$PWD/tests/tsan.supp"
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT

IFS=, read -r -a list <<<"$only"
for s in "${list[@]}"; do
    while [ "$(jobs -rp | wc -l)" -ge "$jobs" ]; do wait -n || true; done
    ( OC_TEST_ONLY="$s" setarch "$(uname -m)" -R ./build/tests-tsan >"$scratch/$s.log" 2>&1
      echo $? >"$scratch/$s.status" ) &
done
wait

failed=()
for s in "${list[@]}"; do
    echo "=== $s"
    cat "$scratch/$s.log"
    [ "$(cat "$scratch/$s.status" 2>/dev/null)" = 0 ] || failed+=("$s")
done
[ "${#failed[@]}" = 0 ] || fail "under ThreadSanitizer: ${failed[*]} failed"
ok "${#list[@]} threaded suites passed under ThreadSanitizer, ${jobs} at a time"
