#!/usr/bin/env bash
# Nothing that ships is a demo build: DEMO=1 appears in no release path (the
# release workflow, the Dockerfile, packaging/), and the release asserts the
# binaries' --version says no `demo`. Run by `make test`.
set -uo pipefail
cd "$(dirname "$0")/../.."
bad=$(grep -nE '(make|MAKE)[^#]*\bDEMO=1' .github/workflows/candidate.yml .github/workflows/release.yml Dockerfile packaging/*.sh packaging/*/* 2>/dev/null || true)
if [ -n "$bad" ]; then echo "release_lint: DEMO=1 in a release path:" >&2; echo "$bad" | sed 's/^/  /' >&2; exit 1; fi
grep -q 'OPENCHIME-DEMO-BUILD' .github/workflows/candidate.yml || { echo "release_lint: candidate.yml does not assert the built binaries are not demo builds" >&2; exit 1; }
echo "release_lint: no demo build ships"
