#!/usr/bin/env bash
# scripts/test/full/suites.sh
#
# Every suite, plainly (`build/tests --list full`), the slow ones included,
# as many at once as there are CPUs, with every timing verdict judged
# (tests/check.h): the suites that start threads run here once as written and
# again under ThreadSanitizer (full/tsan.sh), which does not judge speed.
#
# Needs: build/tests; ffprobe (the media suite's recording check, a SKIP and
# so a failure in CI without it).
# Run by: make test-all (PART=plain).
. "$(dirname "$0")/../lib/common.sh"
have build/tests "make build-all PART=plain builds it"
export OC_TEST_ONLY
OC_TEST_ONLY=$(suites full ./build/tests)
export OC_TEST_JOBS="${OC_TEST_JOBS:-$(nproc 2>/dev/null || echo 4)}"
exec ./build/tests
