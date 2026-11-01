#!/usr/bin/env bash
# scripts/test/merge/tsan.sh
#
# The threaded suites under ThreadSanitizer, one process, one suite after the
# other, as the staging build runs them (docs/TESTING.md §2.4); a data race
# between our threads fails it (tests/tsan.supp). The vendored libraries are
# not instrumented, so a race inside one is not seen. Address randomization is
# off (setarch -R): ThreadSanitizer maps its shadow memory at fixed addresses,
# and recent kernels' randomization places the binary where it has to go.
#
# Needs: setarch; build/tests-tsan.
# Run by: make test-merge (PART=threads).
. "$(dirname "$0")/../lib/common.sh"
need setarch "util-linux"
have build/tests-tsan "make test-merge builds it"
export OC_TEST_ONLY="${OC_TEST_ONLY:-$TSAN_SUITES}"
export TSAN_OPTIONS="halt_on_error=1 second_deadlock_stack=1 suppressions=$PWD/tests/tsan.supp"
exec setarch "$(uname -m)" -R ./build/tests-tsan
