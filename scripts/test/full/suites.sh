#!/usr/bin/env bash
# scripts/test/full/suites.sh
#
# The full suite's plain pass (`build/tests --list plain`): every suite whose
# code starts no thread, the slow ones included, and media, one after another
# in one process, with every timing verdict judged (tests/check.h). The suites
# that start threads run once, under ThreadSanitizer (full/tsan.sh), which does
# not judge speed and skips timer tests (TIMER_ONLY, tests/check.h). The suites
# marked WALK in SUITES[] (tests/main.c) -- the network loop, the client core,
# the database writer, summaries and media -- start threads but run here only,
# plainly: their sanitizer run would cost minutes.
#
# Needs: build/tests; ffprobe (the media suite's recording check, a SKIP and
# so a failure in CI without it).
# Run by: make test-all (PART=plain).
. "$(dirname "$0")/../lib/common.sh"
have build/tests "make build-all PART=plain builds it"
export OC_TEST_ONLY
OC_TEST_ONLY=$(suites plain ./build/tests)
# One after another, in one process, whatever the environment says
# (docs/details/TESTING.md, "One build at a time").
export OC_TEST_JOBS=1
exec ./build/tests
