#!/usr/bin/env bash
# scripts/test/full/suites.sh
#
# The full suite's plain pass (`build/tests --list plain`): every suite whose
# code starts no thread, the slow ones included, and media, as many at once as
# there are CPUs, with every timing verdict judged (tests/check.h). The suites
# that start threads run once, under ThreadSanitizer (full/tsan.sh), which does
# not judge speed; media is marked TIMED in SUITES[] (tests/main.c), so it runs
# here as well, for the speed verdicts nothing else judges.
#
# Needs: build/tests; ffprobe (the media suite's recording check, a SKIP and
# so a failure in CI without it).
# Run by: make test-all (PART=plain).
. "$(dirname "$0")/../lib/common.sh"
have build/tests "make build-all PART=plain builds it"
export OC_TEST_ONLY
OC_TEST_ONLY=$(suites plain ./build/tests)
export OC_TEST_JOBS="${OC_TEST_JOBS:-$(nproc 2>/dev/null || echo 4)}"
exec ./build/tests
