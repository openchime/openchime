#!/usr/bin/env bash
# Close the issues a release shipped.
#
#   close_shipped_issues.sh <release-tag> [--dry-run]
#
# An issue closes when its work SHIPS, not when it merges: a merge lands on
# staging, and staging is not a release. Nor can the merge do it -- the squash
# carries no body, so a `Closes` line never reaches staging or main.
#
# Every commit that reaches main names its issue in the subject (commit-policy
# rejects anything else), so the release's commits are the list. The range runs
# from the previous release-N tag to this one; the first release has no
# predecessor and covers only its own commit.
#
# NOTHING HERE FAILS THE RELEASE. By the time this runs the packages are live, and
# an API hiccup must not brand a good release failed -- every path warns and exits 0.
set -uo pipefail

tag="${1:-}"
dry=0
[ "${2:-}" = "--dry-run" ] && dry=1
if [ -z "$tag" ]; then
  echo "usage: close_shipped_issues.sh <release-tag> [--dry-run]" >&2
  exit 2
fi

warn() { echo "::warning::$*"; }

n="${tag#release-}"
if ! printf '%s' "$n" | grep -qE '^[0-9]+$' || ! sha=$(git rev-parse -q --verify "${tag}^{commit}"); then
  warn "$tag is not a release tag in this checkout; closed nothing"
  exit 0
fi

# The newest release below this one, by NUMBER (the same ordering version.sh
# uses) -- not by date, and not by ancestry.
prev=$(git tag -l 'release-*' | sed 's/^release-//' | grep -E '^[0-9]+$' \
       | awk -v n="$n" '$1 < n' | sort -n | tail -1 || true)
if [ -n "$prev" ]; then
  range="release-${prev}..${sha}"
else
  range="${sha}~1..${sha}"
  git rev-parse -q --verify "${sha}~1" >/dev/null || range="$sha"
fi

# Resolved ONCE, and a failure to resolve is reported rather than read as "no
# commits" -- the fail-open trap commit-policy.yml documents.
if ! commits=$(git rev-list --no-merges "$range"); then
  warn "could not resolve $range; closed nothing"
  exit 0
fi
[ -z "$commits" ] && { echo "no commits in $range"; exit 0; }

repo="${GITHUB_REPOSITORY:-$(gh repo view --json nameWithOwner --jq .nameWithOwner 2>/dev/null)}"
[ -z "$repo" ] && { warn "cannot tell which repository this is; closed nothing"; exit 0; }

echo "closing issues shipped in $tag ($range, $(printf '%s\n' "$commits" | wc -l) commits)"
seen=" "
while read -r c; do
  [ -z "$c" ] && continue
  subject=$(git log -1 --format=%s "$c")
  # The pull-request suffix GitHub appends on squash names the PULL REQUEST. Issues and pull
  # requests share one counter, so that number must never be mistaken for the issue.
  core=$(printf '%s' "$subject" | sed -E 's/ \(#[0-9]+\)$//')
  num=$(printf '%s' "$core" | sed -nE 's/^OC-([0-9]+):.*/\1/p')
  [ -z "$num" ] && continue
  case "$seen" in *" $num "*) continue ;; esac
  seen="$seen$num "

  if ! meta=$(gh api "repos/${repo}/issues/${num}" --jq '[.state, (.pull_request != null)] | @tsv' 2>/dev/null); then
    warn "OC-${num}: could not read the issue; left open"
    continue
  fi
  state=$(printf '%s' "$meta" | cut -f1)
  is_pr=$(printf '%s' "$meta" | cut -f2)
  if [ "$is_pr" = "true" ]; then
    warn "OC-${num} is a pull request, not an issue; left alone"
    continue
  fi
  if [ "$state" = "closed" ]; then
    echo "OC-${num} already closed"
    continue
  fi
  if [ "$dry" = 1 ]; then
    echo "would close OC-${num}"
    continue
  fi
  if gh issue close "$num" --repo "$repo" --reason completed \
       --comment "Shipped in ${tag} (\`${sha}\`)." >/dev/null 2>&1; then
    echo "closed OC-${num}"
  else
    warn "OC-${num}: close failed; left open"
  fi
done <<<"$commits"
exit 0
