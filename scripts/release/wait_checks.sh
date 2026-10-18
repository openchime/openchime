#!/usr/bin/env bash
# Wait for the push checks on a staging commit, and fail unless all passed.
#
#   GH_TOKEN=... scripts/release/wait_checks.sh <sha>
#
# ci, attribution-guard and commit-policy run on every push to staging (ci with
# its build and test jobs skipped when only documentation changed), so each has
# a run to wait for. release-candidate.yml waits before it builds anything, and
# promote.yml before it moves main. Up to 55 minutes each: ci from a cold cache
# takes most of that. The token needs `actions: read`.
set -euo pipefail

sha="${1:?usage: scripts/release/wait_checks.sh <sha>}"
repo="${GITHUB_REPOSITORY:?GITHUB_REPOSITORY is not set}"

for wf in ci attribution-guard commit-policy; do
  for _ in $(seq 1 110); do
    state=$(gh api "repos/${repo}/actions/runs?head_sha=${sha}&event=push&branch=staging" \
              --jq "[.workflow_runs[] | select(.name == \"${wf}\") | .conclusion] | first // \"pending\"")
    [ "$state" = "pending" ] || [ "$state" = "null" ] || break
    sleep 30
  done
  [ "$state" = "success" ] || {
    echo "::error::${wf} has not passed on ${sha:0:7} (state: ${state})"; exit 1; }
  echo "${wf}: success on ${sha:0:7}"
done
