#!/usr/bin/env bash
# Nothing that ships runs in development mode: no release path (the staging
# build, release-candidate and release workflows, the Dockerfile and its
# entrypoint, packaging/, and the checks on what the release ships and
# publishes, scripts/test/release/ and published/) sets
# OPENCHIME_MODE=development or an OPENCHIME_DEV_* input (CONFIG.md "Mode").
# The end-to-end test runs the daemon in that mode, which is why it is a code
# test (scripts/test/merge/e2e.sh), run from ci.yml.
# Run by scripts/test/local/source.sh.
set -uo pipefail
cd "$(dirname "$0")/../../.."
paths=(.github/workflows/staging-build.yml .github/workflows/release-candidate.yml .github/workflows/release.yml Dockerfile entrypoint.sh packaging/*.sh packaging/*.env packaging/*/* scripts/test/release/*.sh scripts/smoke/*.sh)
bad=$(grep -nE 'OPENCHIME_MODE[=: "'"'"']+development|OPENCHIME_DEV_[A-Z_]+[=:]|\bDEMO=1' "${paths[@]}" 2>/dev/null || true)
if [ -n "$bad" ]; then echo "release_lint: development mode in a release path:" >&2; echo "$bad" | sed 's/^/  /' >&2; exit 1; fi
echo "release_lint: nothing that ships runs in development mode"
