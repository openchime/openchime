#!/bin/sh
# Would `pr-policy` accept this title? Ask before pushing, not after.
#
#   scripts/check_pr.sh "OC-38: Raise the tray balloon"     # a title about to be used
#   scripts/check_pr.sh                                      # HEAD's subject line
#
# The rules are .github/workflows/pr-policy.yml's, restated here so they can be
# checked locally. The one that keeps catching people is the LENGTH: GitHub
# appends " (#<pr>)" on squash, and the PR number does not exist until the PR
# does — so a title that fits in isolation fails once opened. The number is
# assumed to be four digits, which is the widest this repo will plausibly reach
# and the only safe direction to round.
#
# Exit 0 = it would pass. Exit 1 = it would fail, with the reason.
set -eu

TITLE="${1:-$(git log -1 --pretty=%s)}"
# A squashed subject keeps the " (#N)" GitHub already added; do not count it twice.
TITLE=$(printf '%s' "$TITLE" | sed -E 's/ \(#[0-9]+\)$//')
fail=0

if ! printf '%s' "$TITLE" \
     | grep -qE '^OC-[0-9]+: [A-Z]([^[:cntrl:]]*[^.,;:!?[:space:]])?$'; then
  echo "check_pr: title must read 'OC-<issue>: Capitalised description' with no trailing punctuation"
  echo "          got: $TITLE"
  fail=1
fi

WIDEST='9999'
SUBJECT="$TITLE (#${WIDEST})"
if [ "${#SUBJECT}" -gt 72 ]; then
  echo "check_pr: ${#SUBJECT} chars once GitHub appends the number, limit 72 -- shorten by $((${#SUBJECT} - 72))"
  fail=1
fi

N=$(printf '%s' "$TITLE" | sed -nE 's/^OC-([0-9]+):.*/\1/p')
if [ -n "$N" ] && command -v gh >/dev/null 2>&1; then
  state=$(gh issue view "$N" --json state,number -q .state 2>/dev/null || echo "")
  if [ -z "$state" ]; then
    echo "check_pr: OC-$N is not an issue in this repository"
    fail=1
  elif [ "$state" != OPEN ]; then
    echo "check_pr: OC-$N is not an open issue (it is $state)"
    fail=1
  fi
fi

# The body is the other half of the policy and has no local form to check: it
# must be EMPTY, which is `gh pr create --body ""`.
[ "$fail" = 0 ] && echo "check_pr: ok -- ${#SUBJECT} chars with the widest number, OC-$N open" || exit 1
