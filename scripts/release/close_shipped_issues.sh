#!/usr/bin/env bash
# Close the issues a release shipped.
#
#   scripts/release/close_shipped_issues.sh <release-tag> [--dry-run]
#   scripts/release/close_shipped_issues.sh --range <from>..<to> [--dry-run]
#
# --range closes the issues the commits <from>..<to> name; no workflow uses it
# (promote closes nothing: an issue closes when a release ships it), it is for
# closing by hand.
#
# An issue closes when its work SHIPS, not when it merges: a merge lands on
# staging, and staging is not a release. Nor can the merge do it -- the squash
# carries no body, so a `Closes` line never reaches staging or main.
#
# Every commit on staging names its issue in the subject (pr-policy refuses a
# title that does not, and the squash commits the title), so the release's
# commits are the list: the staging commits from the previous release-N tag
# to this one (the tags are on staging's commits; main holds snapshots). The
# first release has no predecessor and covers only its own commit. Beside
# them, the documentation the release carries onto main (the Docs: trailers of
# its Release N snapshot) closes its issues too.
#
# A subject names its issue by its prefix: OC-N is an issue on
# openchime/openchime, closed with GH_TOKEN (which must be able to close issues
# there; ISSUES_REPO overrides the repository).
#
# NOTHING HERE FAILS THE RELEASE. By the time this runs the packages are live, and
# an API hiccup must not brand a good release failed -- every path warns and exits 0.
set -uo pipefail

warn() { echo "::warning::$*"; }
usage() {
  echo "usage: scripts/release/close_shipped_issues.sh <release-tag> [--dry-run]" >&2
  echo "       scripts/release/close_shipped_issues.sh --range <from>..<to> [--dry-run]" >&2
  exit 2
}

range=""
if [ "${1:-}" = "--range" ]; then
  range="${2:-}"
  [ -n "$range" ] || usage
  shift
fi
tag="${1:-}"
[ -n "$tag" ] || usage
dry=0
[ "${2:-}" = "--dry-run" ] && dry=1

if [ -n "$range" ]; then
  if ! sha=$(git rev-parse -q --verify "${range##*..}^{commit}"); then
    warn "$range does not end at a commit in this checkout; closed nothing"
    exit 0
  fi
  what="${sha:0:7}"
  note="Closed by hand at \`${sha}\`."
else
  n="${tag#release-}"
  if ! printf '%s' "$n" | grep -qE '^[0-9]+$' || ! sha=$(git rev-parse -q --verify "${tag}^{commit}"); then
    warn "$tag is not a release tag in this checkout; closed nothing"
    exit 0
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
  what="$tag"
  note="Shipped in ${tag} (\`${sha}\`)."
fi

# Resolved ONCE, and a failure to resolve is reported rather than read as "no
# commits" -- the fail-open trap scripts/release/commit_policy.sh documents.
if ! commits=$(git rev-list --no-merges "$range"); then
  warn "could not resolve $range; closed nothing"
  exit 0
fi
# Documentation the release carried onto main, beyond its candidate (the
# Docs: lines of its `Release N` commit there), shipped with it too.
if [ -n "${n:-}" ]; then
  snap=$(git log "${MAIN_REF:-origin/main}" -1 --format=%H --grep="^Release ${n}\$" 2>/dev/null || true)
  if [ -n "$snap" ]; then
    carried=$(git log -1 --format='%(trailers:key=Docs,valueonly,separator=%x0A)' "$snap" | awk 'NF {print $1}')
    for d in $carried; do
      if git cat-file -e "${d}^{commit}" 2>/dev/null; then
        echo "carried documentation: $(git log -1 --format='%h %s' "$d")"
        commits="${commits:+${commits}
}${d}"
      else
        warn "carried documentation commit ${d:0:7} is not in this checkout; its issue is left open"
      fi
    done
  else
    warn "no Release ${n} commit on ${MAIN_REF:-origin/main}; only the candidate's own issues are closed"
  fi
fi
[ -z "$commits" ] && { echo "no commits in $range"; exit 0; }

oc_repo="${ISSUES_REPO:-openchime/openchime}"

echo "closing issues shipped in $what ($range, $(printf '%s\n' "$commits" | wc -l) commits)"
seen=" "
while read -r c; do
  [ -z "$c" ] && continue
  subject=$(git log -1 --format=%s "$c")
  # The pull-request suffix GitHub appends on squash names the PULL REQUEST. Issues and pull
  # requests share one counter, so that number must never be mistaken for the issue.
  core=$(printf '%s' "$subject" | sed -E 's/ \(#[0-9]+\)$//')
  prefix=$(printf '%s' "$core" | sed -nE 's/^(OC)-[0-9]+:.*/\1/p')
  num=$(printf '%s' "$core" | sed -nE 's/^OC-([0-9]+):.*/\1/p')
  [ -z "$num" ] && continue
  ref="${prefix}-${num}"
  case "$seen" in *" $ref "*) continue ;; esac
  seen="$seen$ref "

  repo="$oc_repo"; token="${GH_TOKEN:-}"

  if ! meta=$(GH_TOKEN="$token" gh api "repos/${repo}/issues/${num}" --jq '[.state, (.pull_request != null)] | @tsv' 2>/dev/null); then
    warn "${ref}: could not read the issue; left open"
    continue
  fi
  state=$(printf '%s' "$meta" | cut -f1)
  is_pr=$(printf '%s' "$meta" | cut -f2)
  if [ "$is_pr" = "true" ]; then
    warn "${ref} is a pull request, not an issue; left alone"
    continue
  fi
  if [ "$state" = "closed" ]; then
    echo "${ref} already closed"
    continue
  fi
  if [ "$dry" = 1 ]; then
    echo "would close ${ref}"
    continue
  fi
  if GH_TOKEN="$token" gh issue close "$num" --repo "$repo" --reason completed \
       --comment "$note" >/dev/null 2>&1; then
    echo "closed ${ref}"
  else
    warn "${ref}: close failed; left open"
  fi
done <<<"$commits"
exit 0
