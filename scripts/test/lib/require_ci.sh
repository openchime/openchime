#!/usr/bin/env bash
# scripts/test/lib/require_ci.sh <target>
#
# make test-merge and make test-release run in CI only, no exceptions: they
# build every feature and drive the release's artifacts, which a developer's
# machine is not for. GitHub Actions sets CI; anywhere else this stops the
# target before it builds anything. Locally, the test is `make test`.
#
# Run by: make test-merge, make test-release (their first step).
. "$(dirname "$0")/common.sh"
if [ -z "${CI:-}" ]; then
    echo "make ${1:-this target} runs in CI only; locally, run make test" >&2
    exit 2
fi
