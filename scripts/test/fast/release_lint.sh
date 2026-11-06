#!/usr/bin/env bash
# Nothing that ships runs in development mode: no release path (the
# release-candidate and release workflows, the Dockerfile and its entrypoint,
# packaging/, the full suite's checks on the artifacts a release ships, and
# the release's verification of what it published, scripts/release/smoke/)
# sets OPENCHIME_MODE=development or an OPENCHIME_DEV_* input (CONFIG.md
# "Mode"). The end-to-end test and the web client's checks run the daemon in
# that mode, which is why they are code tests (scripts/test/full/e2e.sh,
# web.sh), run from test-all.yml, which release-candidate.yml calls.
# Run by scripts/test/fast/source.sh.
set -uo pipefail
cd "$(dirname "$0")/../../.."
# The full suite's checks on the release's artifacts (make test-all
# PART=packages, image and windows), one name each: the rest of
# scripts/test/full/ is code tests.
artifacts=(glibc_floor same_binary speech package unit_file image db_restore windows)
paths=(.github/workflows/release-candidate.yml .github/workflows/release.yml Dockerfile entrypoint.sh
       packaging/*.sh packaging/*.env packaging/*/* scripts/release/smoke/*.sh)
for a in "${artifacts[@]}"; do paths+=("scripts/test/full/$a.sh"); done
missing=""
for p in "${paths[@]}"; do [ -e "$p" ] || missing="$missing $p"; done
if [ -n "$missing" ]; then echo "release_lint: a release path named here does not exist:$missing" >&2; exit 1; fi
bad=$(grep -nE 'OPENCHIME_MODE[=: "'"'"']+development|OPENCHIME_DEV_[A-Z_]+[=:]|\bDEMO=1' "${paths[@]}" 2>/dev/null || true)
if [ -n "$bad" ]; then echo "release_lint: development mode in a release path:" >&2; echo "$bad" | sed 's/^/  /' >&2; exit 1; fi
echo "release_lint: nothing that ships runs in development mode"
