#!/usr/bin/env bash
# The OC issues a release shipped, one number per line, in commit order.
#
#   scripts/release/shipped_issues.sh <release-tag>
#   scripts/release/shipped_issues.sh --range <from>..<to>
#
# The rules close_shipped_issues.sh closes by, and the list send_notes.sh
# reports: every commit on staging names its issue in the subject (pr-policy
# refuses a title that does not, and the squash commits the title), so the
# release's commits are the list -- the staging commits from the previous
# release-N tag to this one (the tags are on staging's commits; main holds
# snapshots). The first release has no predecessor and covers only its own
# commit. Beside them, the documentation the release carries onto main (the
# Docs: trailers of its Release N snapshot, on MAIN_REF, origin/main by default)
# shipped with it too.
#
# What it could not read it says on stderr. Exits 1 when the range itself
# cannot be resolved -- never read as "nothing shipped" -- and 0 otherwise.
set -uo pipefail

warn() { echo "::warning::$*" >&2; }
usage() {
  echo "usage: scripts/release/shipped_issues.sh <release-tag>" >&2
  echo "       scripts/release/shipped_issues.sh --range <from>..<to>" >&2
  exit 2
}

range="" n=""
if [ "${1:-}" = "--range" ]; then
  range="${2:-}"
  [ -n "$range" ] || usage
  git rev-parse -q --verify "${range##*..}^{commit}" >/dev/null || { warn "$range does not end at a commit in this checkout"; exit 1; }
else
  tag="${1:-}"
  [ -n "$tag" ] || usage
  n="${tag#release-}"
  if ! printf '%s' "$n" | grep -qE '^[0-9]+$' || ! sha=$(git rev-parse -q --verify "${tag}^{commit}"); then
    warn "$tag is not a release tag in this checkout"
    exit 1
  fi
  # The newest release below this one, by NUMBER (the same ordering
  # scripts/release/version.sh uses) -- not by date, and not by ancestry.
  prev=$(git tag -l 'release-*' | sed 's/^release-//' | grep -E '^[0-9]+$' \
         | awk -v n="$n" '$1 < n' | sort -n | tail -1 || true)
  if [ -n "$prev" ]; then
    range="release-${prev}..${sha}"
  else
    range="${sha}~1..${sha}"
    git rev-parse -q --verify "${sha}~1" >/dev/null || range="$sha"
  fi
fi

# Resolved ONCE, and a failure to resolve is reported rather than read as "no
# commits" -- the fail-open trap scripts/release/commit_policy.sh documents.
if ! commits=$(git rev-list --no-merges --reverse "$range"); then
  warn "could not resolve $range"
  exit 1
fi
# Documentation the release carried onto main, beyond its candidate (the
# Docs: lines of its `Release N` commit there), shipped with it too.
if [ -n "$n" ]; then
  snap=$(git log "${MAIN_REF:-origin/main}" -1 --format=%H --grep="^Release ${n}\$" 2>/dev/null || true)
  if [ -n "$snap" ]; then
    carried=$(git log -1 --format='%(trailers:key=Docs,valueonly,separator=%x0A)' "$snap" | awk 'NF {print $1}')
    for d in $carried; do
      if git cat-file -e "${d}^{commit}" 2>/dev/null; then
        echo "carried documentation: $(git log -1 --format='%h %s' "$d")" >&2
        commits="${commits:+${commits}
}${d}"
      else
        warn "carried documentation commit ${d:0:7} is not in this checkout; its issue is not listed"
      fi
    done
  else
    warn "no Release ${n} commit on ${MAIN_REF:-origin/main}; only the candidate's own issues are listed"
  fi
fi

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
  echo "$num"
done <<<"$commits"
exit 0
