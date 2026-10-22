#!/usr/bin/env bash
# Check, once, that the push checks on a staging commit passed; fail unless all did.
#
#   GH_TOKEN=... scripts/release/wait_checks.sh <sha> [workflow ...]
#
# It looks once and never waits: a runner held while another workflow finishes
# is billed for every minute of it. A check missing or still running is a
# failure that says so, and the answer is to start again once it has passed --
# which is why ci dispatches promote rather than having it wait, and why a
# release-candidate is dispatched after ci has passed on the tip.
#
# ci runs on every push to staging, the commit policy and the attribution scan
# among its steps (its build and test jobs skipped when only documentation and
# other workflows changed, or when the merged pull request already passed them
# on the same tree), so it is the default. A workflow is named by its file,
# .github/workflows/<workflow>.yml, and its runs are found by that path, never
# by the display name. The token needs `actions: read`.
set -euo pipefail

sha="${1:?usage: scripts/release/wait_checks.sh <sha> [workflow ...]}"
shift
[ "$#" -gt 0 ] || set -- ci
repo="${GITHUB_REPOSITORY:?GITHUB_REPOSITORY is not set}"

fail=0
for wf in "$@"; do
  state=$(gh api "repos/${repo}/actions/runs?head_sha=${sha}&event=push&branch=staging" \
            --jq "[.workflow_runs[] | select((.path | split(\"@\") | first) == \".github/workflows/${wf}.yml\") | (.conclusion // .status)] | first // \"missing\"")
  case "$state" in
    success) echo "${wf}: success on ${sha:0:7}" ;;
    queued|in_progress|waiting|pending|requested)
      echo "::error::${wf} is still ${state} on ${sha:0:7}: start this again once it has passed"; fail=1 ;;
    missing)
      echo "::error::${wf} has no run on ${sha:0:7}"; fail=1 ;;
    *)
      echo "::error::${wf} did not pass on ${sha:0:7} (${state})"; fail=1 ;;
  esac
done
exit "$fail"
