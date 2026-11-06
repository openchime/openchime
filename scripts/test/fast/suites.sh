#!/usr/bin/env bash
# scripts/test/fast/suites.sh
#
# The fast suites, before every push: build/tests running the suites SUITES[]
# marks FAST (tests/main.c, `build/tests --list fast`), as many at once as
# there are CPUs. The suites marked FULL, too slow for the fast suite's
# budget, run in the full suite with every other (scripts/test/full/suites.sh).
# Timing verdicts are judged (tests/check.h).
#
# Needs: build/tests.
# Run by: make test.
. "$(dirname "$0")/../lib/common.sh"
have build/tests "make test builds it"
export OC_TEST_ONLY
OC_TEST_ONLY=$(suites fast ./build/tests)
export OC_TEST_JOBS="${OC_TEST_JOBS:-$(nproc 2>/dev/null || echo 4)}"
exec ./build/tests
