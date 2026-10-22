#!/usr/bin/env bash
# Did the pull request that this push merged already pass CI on this exact
# tree? Asked by ci.yml's `changes` job on a push to staging, so that a merge
# whose tree its pull request already tested is not built and tested again.
#
#   GH_TOKEN=... GITHUB_REPOSITORY=owner/repo \
#     scripts/release/tested_tree.sh <tree> <pushed-sha>
#
# Prints, on stdout, one line per job and the run that tested it:
#   build=true|false  tsan=true|false  windows=true|false  run=<id or empty>
# and on stderr why. It never fails: any error, a missing answer or a doubt is
# `false` for every job, and the job runs in full. A wrong `true` would skip a
# check, so every one of these must hold for it:
#   1. the pushed commit is the merge commit of a merged pull request into
#      staging, whose head is on this repository (not a fork);
#   2. a ci.yml run of that pull request's own head concluded success, on a
#      pull_request event, from this repository;
#   3. that run uploaded tested-<tree>-build, -tsan and -windows, unexpired:
#      each job uploads its own as its last step, only when everything before
#      it passed, named for the tree of the merge it tested.
# Only the merged pull request's own run counts, so a record another branch's
# run uploaded for the same tree is never read. The token needs
# `pull-requests: read` and `actions: read`.
set -uo pipefail

tree="${1:-}"; sha="${2:-}"
repo="${GITHUB_REPOSITORY:-}"
jobs=(build tsan windows)

answer() {   # <true|false> <run> <why>
  for j in "${jobs[@]}"; do echo "$j=$1"; done
  echo "run=$2"
  echo "tested_tree: $3" >&2
  exit 0
}

[ -n "$tree" ] && [ -n "$sha" ] && [ -n "$repo" ] \
  || answer false "" "usage: tested_tree.sh <tree> <pushed-sha>, with GITHUB_REPOSITORY set"
command -v gh >/dev/null 2>&1 || answer false "" "gh is not installed"

# 1. The pull request this push merged. The association can lag the push by a
# few seconds, so an empty answer is asked again before it is believed.
pr=""
for attempt in 1 2 3; do
  if out=$(gh api "repos/${repo}/commits/${sha}/pulls" --jq "
        [.[] | select(.merged_at != null and .merge_commit_sha == \"${sha}\"
                      and .base.ref == \"staging\"
                      and .head.repo.full_name == \"${repo}\")
         | \"\(.number) \(.head.sha)\"] | first // empty" 2>&1); then
    pr="$out"
    [ -n "$pr" ] && break
  else
    echo "tested_tree: looking up the pull request for ${sha:0:7} failed: ${out}" >&2
  fi
  [ "$attempt" = 3 ] || sleep 5
done
[ -n "$pr" ] || answer false "" "${sha:0:7} is not the merge commit of a pull request into staging from this repository"
number="${pr%% *}"; head="${pr#* }"
echo "tested_tree: ${sha:0:7} merged pull request ${number}, head ${head:0:7}" >&2

# 2. Its successful pull-request runs of ci.yml, newest first.
runs=$(gh api "repos/${repo}/actions/workflows/ci.yml/runs?head_sha=${head}&event=pull_request&status=success&per_page=20" \
         --jq "[.workflow_runs[] | select(.conclusion == \"success\"
                                          and .head_sha == \"${head}\"
                                          and .event == \"pull_request\"
                                          and .head_repository.full_name == \"${repo}\")
                | .id] | .[]" 2>&1) \
  || answer false "" "listing ci runs of ${head:0:7} failed: ${runs}"
[ -n "$runs" ] || answer false "" "no successful pull_request ci run of ${head:0:7}"

# 3. The first of them that recorded this tree for all three jobs.
for run in $runs; do
  names=$(gh api "repos/${repo}/actions/runs/${run}/artifacts?per_page=100" \
            --jq '.artifacts[] | select(.expired == false) | .name' 2>&1) || {
    echo "tested_tree: listing the artifacts of run ${run} failed: ${names}" >&2; continue; }
  missing=""
  for j in "${jobs[@]}"; do
    grep -qxF "tested-${tree}-${j}" <<<"$names" || missing="${missing} ${j}"
  done
  [ -z "$missing" ] && answer true "$run" "run ${run} tested tree ${tree:0:12} in every job"
  echo "tested_tree: run ${run} has no unexpired record of tree ${tree:0:12} for:${missing}" >&2
done
answer false "" "no successful run of ${head:0:7} recorded tree ${tree:0:12}"
