#!/usr/bin/env bash
# Nothing that ships runs in development mode: no release path (the staging
# build, release-candidate and release workflows, the Dockerfile and its
# entrypoint, packaging/) sets OPENCHIME_MODE=development or an OPENCHIME_DEV_*
# input (CONFIG.md "Mode"). The staging build's tests run the daemon in that
# mode, which is why they stay in ci.yml, which it calls.
# Run by `make test`.
set -uo pipefail
cd "$(dirname "$0")/../.."
paths=(.github/workflows/staging-build.yml .github/workflows/release-candidate.yml .github/workflows/release.yml Dockerfile entrypoint.sh packaging/*.sh packaging/*.env packaging/*/*)
bad=$(grep -nE 'OPENCHIME_MODE[=: "'"'"']+development|OPENCHIME_DEV_[A-Z_]+[=:]|\bDEMO=1' "${paths[@]}" 2>/dev/null || true)
if [ -n "$bad" ]; then echo "release_lint: development mode in a release path:" >&2; echo "$bad" | sed 's/^/  /' >&2; exit 1; fi
echo "release_lint: nothing that ships runs in development mode"
