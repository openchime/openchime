#!/usr/bin/env bash
# scripts/release/smoke.sh [--skip-repos]
#
# The smoke test of a published release: every script in scripts/release/smoke/, one
# after another. Each runs even when an earlier one failed, so a broken
# repository cannot hide whether the indexes or the image are right; each is
# reported, and the run exits 1 if any failed. Adding a check is adding a file
# to scripts/release/smoke/.
#
# --skip-repos skips the repository installs (apt.sh, dnf.sh), for while the
# published address is not yet served; the release says so in its summary.
#
# Needs: what each check's header names: the release's addresses, bucket keys
# and registry token, podman, rclone, sudo and apt-get.
# Run by: the release workflow, after publishing, and nothing else.
set -uo pipefail
cd "$(dirname "$0")/../.."
skip_repos=
[ "${1:-}" = --skip-repos ] && skip_repos=1
failed=() passed=() skipped=()
for check in scripts/release/smoke/*.sh; do
    name=$(basename "$check" .sh)
    if [ -n "$skip_repos" ] && { [ "$name" = apt ] || [ "$name" = dnf ]; }; then
        skipped+=("$name"); echo "=== $name: skipped (--skip-repos)"; continue
    fi
    echo "=== $name"
    if "$check"; then passed+=("$name"); else failed+=("$name"); fi
done
echo "smoke: passed: ${passed[*]:-none}; failed: ${failed[*]:-none}; skipped: ${skipped[*]:-none}"
[ ${#failed[@]} -eq 0 ]
