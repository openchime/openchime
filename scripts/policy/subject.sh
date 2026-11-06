#!/usr/bin/env bash
# The one rule for a commit's subject, which is a pull request's title before
# the squash: `OC-<issue>: Capitalised description`, no trailing punctuation,
# and at most 72 characters once GitHub has appended the pull request's
# " (#<n>)" on the squash (CONTRIBUTING.md "Commits"). Exit 0 when <subject>
# meets it; 1 with the reason on stderr (as a GitHub annotation under
# GitHub Actions) when it does not.
#
#   scripts/policy/subject.sh <subject> [<pull request number>]
#
# A subject that already ends in " (#<n>)" -- a squash's -- is measured as it
# is. One that does not is measured with the suffix the squash will add: the
# number given, or, before the pull request exists, the widest one this
# repository will plausibly reach (four digits), the only safe direction to
# round.
#
# Used by pr-policy (a pull request's title), scripts/policy/commit_policy.sh
# (the commits a release or a promotion brings) and the pre-push hook (the
# tip of a pushed branch).
set -uo pipefail
subject=${1?usage: scripts/policy/subject.sh <subject> [<pull request number>]}
pr=${2:-9999}
say() { if [ -n "${GITHUB_ACTIONS:-}" ]; then echo "::error::$*"; else echo "subject: $*" >&2; fi; }
if [[ "$subject" =~ \ \(\#[0-9]+\)$ ]]; then
    core=${subject% (#*}
    squashed=$subject
else
    core=$subject
    squashed="$subject (#$pr)"
fi
fail=0
grep -qE '^OC-[0-9]+: [A-Z]([^[:cntrl:]]*[^.,;:!?[:space:]])?$' <<<"$core" || {
    say "the subject must read 'OC-<issue>: Capitalised description' with no trailing punctuation; got: $subject"
    fail=1
}
[ "${#squashed}" -le 72 ] || {
    say "the subject is ${#squashed} characters once squashed ('$squashed'), limit 72: shorten it by $(( ${#squashed} - 72 ))"
    fail=1
}
exit "$fail"
