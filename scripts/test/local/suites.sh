#!/usr/bin/env bash
# scripts/test/local/suites.sh
#
# The fast suites, before every push: build/tests running every suite but the
# slow ones (SLOW_SUITES, scripts/test/lib/common.sh), as many at once as
# there are CPUs. The slow suites run in make test-merge, on every pull
# request, so nothing goes untested; this keeps the local run near a minute.
#
# Needs: build/tests.
# Run by: make test.
. "$(dirname "$0")/../lib/common.sh"
have build/tests "make test builds it"
# A suite that starts a thread must be on TSAN_SUITES: the binary checks.
export OC_TSAN_SUITES="$TSAN_SUITES"
export OC_TEST_JOBS="${OC_TEST_JOBS:-$(nproc 2>/dev/null || echo 4)}"
export OC_TEST_EXCEPT="$SLOW_SUITES"
exec ./build/tests
