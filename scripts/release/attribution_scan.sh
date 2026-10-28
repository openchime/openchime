#!/usr/bin/env bash
# The attribution scan: no commit this event brings, and no tag it pushes, may
# name the banned pattern in its author, committer or message (CONTRIBUTING.md).
# One script, run where a job already is: pr-policy on a pull request, and,
# as a push of a range, the staging build's gate (the commits since the last
# release) and promote (what it puts on main). Exits 1 on a violation, and writes
# violation=true|false to $GITHUB_OUTPUT when set.
#
#   EVENT BEFORE AFTER REF BASE_REF  scripts/release/attribution_scan.sh
#
# EVENT is push, pull_request or anything else (nothing to scan: passes).
set -u
PATTERN='claude|anthropic'
ZERO=0000000000000000000000000000000000000000

# Only push/pull_request events carry new commits to judge; any other EVENT passes.
if [ "$EVENT" != "push" ] && [ "$EVENT" != "pull_request" ]; then
  echo "violation=false" >> "${GITHUB_OUTPUT:-/dev/null}"; exit 0
fi
# Ref deletion: nothing to scan.
if [ "$EVENT" = "push" ] && { [ "$AFTER" = "$ZERO" ] || [ -z "$AFTER" ]; }; then
  echo "violation=false" >> "${GITHUB_OUTPUT:-/dev/null}"; exit 0
fi

if [ "$EVENT" = "pull_request" ]; then
  commits=$(git rev-list "origin/$BASE_REF..HEAD")
elif [ "$BEFORE" != "$ZERO" ] && git cat-file -e "$BEFORE" 2>/dev/null; then
  commits=$(git rev-list "$BEFORE..$AFTER")
else
  # New ref (or force push whose old tip is gone): scan commits not reachable from
  # any other ref.
  #
  # Both the pushed branch AND the pushed tag have to be excluded from the "other
  # refs" set. Excluding only the branch meant that on a TAG push the tag itself
  # stayed in the set, so `--not <the tag>` cancelled out `$AFTER` and rev-list
  # returned nothing -- a tag pushed at a commit reachable from no other ref was
  # never scanned. (When the tag points at a commit already on a branch, both
  # variants correctly scan nothing: no commit is new.)
  self_branch="refs/remotes/origin/${REF#refs/heads/}"
  self_tag="${REF}"
  others=$(git for-each-ref --format='%(refname) %(objectname)' refs/remotes/origin refs/tags \
           | awk -v b="$self_branch" -v t="$self_tag" '$1 != b && $1 != t {print $2}')
  # $others is a whitespace-separated list of SHAs and MUST word-split into
  # separate rev-list arguments; quoting it would pass one bogus multi-line ref.
  # shellcheck disable=SC2086
  commits=$(git rev-list "$AFTER" --not $others --)
fi

: > /tmp/violations
for c in $commits; do
  # Captured first, then matched: piped, grep -q stops at its first
  # match and `git log` dies on the closed pipe, which under pipefail
  # (or a later `set -o pipefail`) reads as no match.
  meta=$(git log -1 --format='%an%n%ae%n%cn%n%ce%n%B' "$c")
  if grep -qiE "$PATTERN" <<<"$meta"; then
    {
      echo "commit $c"
      git log -1 --format='%an <%ae> / %cn <%ce>%n%B' "$c" | grep -iE "$PATTERN" | sed 's/^/  /'
    } >> /tmp/violations
  fi
done

# Annotated tag messages count too. This check is independent of the commit scan
# above and applies to every tag push.
case "$REF" in refs/tags/*)
  note=$(git tag -l --format='%(contents)' "${REF#refs/tags/}")
  if grep -qiE "$PATTERN" <<<"$note"; then
    echo "tag ${REF#refs/tags/} annotation" >> /tmp/violations
  fi
esac

if [ -s /tmp/violations ]; then
  echo "violation=true" >> "${GITHUB_OUTPUT:-/dev/null}"
  { echo '## Attribution guard: banned reference found'
    echo '```'
    cat /tmp/violations
    echo '```'
  } >> "${GITHUB_STEP_SUMMARY:-/dev/null}"
  cat /tmp/violations
else
  echo "violation=false" >> "${GITHUB_OUTPUT:-/dev/null}"
fi
[ ! -s /tmp/violations ]
