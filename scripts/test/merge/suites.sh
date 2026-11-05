#!/usr/bin/env bash
# scripts/test/merge/suites.sh
#
# Every suite but the threaded ones, which merge/tsan.sh runs under
# ThreadSanitizer, so make test-merge runs each suite exactly once. Wall-clock
# verdicts are not judged in CI (OC_TEST_TIMING=0, tests/check.h).
#
# Needs: build/tests.
# Run by: make test-merge (PART=plain).
. "$(dirname "$0")/../lib/common.sh"
have build/tests "make test-merge builds it"
# A suite that starts a thread must be on TSAN_SUITES: the binary checks.
export OC_TSAN_SUITES="$TSAN_SUITES"
export OC_TEST_JOBS="${OC_TEST_JOBS:-$(nproc 2>/dev/null || echo 4)}"
export OC_TEST_EXCEPT="$TSAN_SUITES"
export OC_TEST_TIMING="${OC_TEST_TIMING:-0}"
exec ./build/tests
