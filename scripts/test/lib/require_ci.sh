#!/usr/bin/env bash
# scripts/test/lib/require_ci.sh <target>
#
# make build-all and make test-all are the full suite, and run in CI only, no
# exceptions: they build every feature and every artifact a release ships and
# drive them, which a developer's machine is not for. GitHub Actions sets CI;
# anywhere else this stops the target before it builds anything. Locally, the
# commands are make build and make test.
#
# Run by: make build-all, make test-all (their first step).
. "$(dirname "$0")/common.sh"
if [ -z "${CI:-}" ]; then
    echo "make ${1:-this target} runs in CI only; locally, run make build and make test" >&2
    exit 2
fi
