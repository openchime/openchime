#!/usr/bin/env bash
# scripts/test/lib/require_local.sh <target>
#
# make build and make test are the fast suite, and run locally only: they are
# what a person runs before every push, and their pass is recorded for the
# pre-push hook in this clone. In CI, which sets CI, the commands are make
# build-all and make test-all; this stops the target before it builds anything.
#
# Run by: make build, make test (their first step).
. "$(dirname "$0")/common.sh"
if [ -n "${CI:-}" ]; then
    echo "make ${1:-this target} runs locally only; in CI, run make build-all and make test-all" >&2
    exit 2
fi
