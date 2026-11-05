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
# Which issues a release shipped is scripts/release/shipped_issues.sh's: the
# issue each of its commits names, between the previous release-N tag and this
# one, and those of the documentation its Release N snapshot carried onto main.
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

here=$(cd "$(dirname "$0")" && pwd)
if [ -n "$range" ]; then
  # shipped_issues.sh refuses a range that does not end at a commit.
  sha=$(git rev-parse -q --verify "${range##*..}^{commit}" || true)
  what="${sha:0:7}"
  note="Closed by hand at \`${sha}\`."
  issues=$("$here/shipped_issues.sh" --range "$range") || { warn "could not list the issues $range shipped; closed nothing"; exit 0; }
else
  sha=$(git rev-parse -q --verify "${tag}^{commit}" || true)
  what="$tag"
  note="Shipped in ${tag} (\`${sha}\`)."
  issues=$("$here/shipped_issues.sh" "$tag") || { warn "could not list the issues $tag shipped; closed nothing"; exit 0; }
fi
[ -z "$issues" ] && { echo "no issues named in $what"; exit 0; }

oc_repo="${ISSUES_REPO:-openchime/openchime}"
token="${GH_TOKEN:-}"

echo "closing issues shipped in $what ($(printf '%s\n' "$issues" | wc -l) issues)"
while read -r num; do
  [ -z "$num" ] && continue
  ref="OC-${num}"
  repo="$oc_repo"

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
done <<<"$issues"
exit 0
