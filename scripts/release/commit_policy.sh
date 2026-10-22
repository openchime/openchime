#!/usr/bin/env bash
# The commit policy for a push to staging or main: every commit it brings has a
# house subject, no body but its sign-off, and no merge among them. pr-policy
# checks the title before the squash; this checks what the squash committed,
# which is where a Co-authored-by trailer GitHub adds shows up. Run as a step of
# ci on a push to staging, and by commit-policy on main.
#
#   BEFORE=<the tip before>  scripts/release/commit_policy.sh   (on $GITHUB_SHA)
set -uo pipefail
ZERO=0000000000000000000000000000000000000000

if [ -n "${BEFORE:-}" ] && [ "$BEFORE" != "$ZERO" ] && git cat-file -e "$BEFORE" 2>/dev/null; then
  RANGE="${BEFORE}..${GITHUB_SHA}"
elif [ -z "$(git log -1 --format=%P "$GITHUB_SHA")" ]; then
  RANGE="$GITHUB_SHA"
else
  RANGE="${GITHUB_SHA}~1..${GITHUB_SHA}"
fi
echo "checking $RANGE"

# Resolve the range ONCE, and fail if it cannot be resolved. The
# fail-open trap: `if [ -n "$(git rev-list ...)" ]` and `for c in
# $(git rev-list ...)` don't trip `set -e` on a failed command
# substitution, so an unresolvable range yields an empty list, zero
# loop iterations, and "commit policy ok" having checked nothing.
if ! MERGE_LIST=$(git rev-list --merges "$RANGE"); then
  echo "::error::could not resolve $RANGE; refusing to report success"
  exit 1
fi
if ! COMMIT_LIST=$(git rev-list --no-merges "$RANGE"); then
  echo "::error::could not resolve $RANGE; refusing to report success"
  exit 1
fi
MERGES=()
COMMITS=()
[ -n "$MERGE_LIST" ] && mapfile -t MERGES <<<"$MERGE_LIST"
[ -n "$COMMIT_LIST" ] && mapfile -t COMMITS <<<"$COMMIT_LIST"

# A merge produces a tree no single commit produced, and the policy
# below is keyed to one commit's subject. Reject them outright.
if [ "${#MERGES[@]}" -gt 0 ]; then
  echo "::error::This push contains merge commits. Squash, or fast-forward."
  printf '  %s\n' "${MERGES[@]}"
  exit 1
fi

echo "checking ${#COMMITS[@]} commit(s)"

fail=0
for c in "${COMMITS[@]}"; do
  subject=$(git log -1 --format=%s "$c")

  # A root commit predates any issue to scope it to.
  [ -z "$(git log -1 --format=%P "$c")" ] && { echo "  ${c:0:8} root, exempt"; continue; }

  # GitHub appends the pull request number in parentheses on squash;
  # strip it before matching so a trailing full stop before the
  # suffix is still caught.
  core=$(printf '%s' "$subject" | sed -E 's/ \(#[0-9]+\)$//')
  grep -qE '^OC-[0-9]+: [A-Z]([^[:cntrl:]]*[^.,;:!?[:space:]])?$' <<<"$core" \
    || { echo "::error::${c:0:8} subject must read 'OC-<issue>: Capitalised description' — got: ${subject}"; fail=1; }

  if [ "${#subject}" -gt 72 ]; then
    echo "::error::${c:0:8} subject is ${#subject} chars, limit 72"
    fail=1
  fi

  # THE REASON THIS WORKFLOW EXISTS. The one line a body may hold is
  # a DCO sign-off: every commit must carry one (CONTRIBUTING.md), and
  # GitHub copies it into the squash even with a BLANK message. Anything
  # else, a Co-authored-by trailer included, still fails. The `|| true`
  # is load-bearing: a body that is ONLY the sign-off leaves grep with no
  # output, grep exits 1, and under the runner's `bash -e` plus pipefail
  # that kills the step silently -- a failure with no error printed.
  body=$(git log -1 --format=%b "$c" \
         | { grep -vE '^Signed-off-by: [^<]+ <[^>]+>$' || true; } \
         | sed '/^[[:space:]]*$/d')
  if [ -n "$body" ]; then
    echo "::error::${c:0:8} has a commit body; subjects here stand alone."
    printf '%s\n' "$body" | head -2 | sed 's/^/    /'
    fail=1
  fi
done

[ "$fail" = 0 ] && echo "commit policy ok" || exit 1
