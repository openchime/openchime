#!/usr/bin/env bash
# Is a change documentation only? The one rule, for every place that asks:
# every path changed is a Markdown file (*.md) or under docs/; and not
# documentation, whatever their names: .github/ (workflows change how the
# repository builds and works), third_party/ (vendored sources),
# LICENSING.md (what the packages carry), and docs/CONFIG.md (its reference is
# generated from the daemon's settings and checked against them by
# scripts/test/fast/config.sh, so it ships with the code).
#
#   scripts/policy/docs_only.sh <commit>           what <commit> changes
#   scripts/policy/docs_only.sh <base> <commit>    what <base>..<commit> changes
#
# Exit 0 when documentation only (nothing changed counts); 1 otherwise,
# printing the first other path; 2 when the change cannot be read, which a
# caller treats as not documentation.
#
# Used by the pre-push hook (a push of documentation alone needs no `make
# test`), promote (which copies such commits onto main without a release),
# and scripts/release/main_next.sh.
set -uo pipefail
[ $# -ge 1 ] && [ $# -le 2 ] || { echo "usage: scripts/policy/docs_only.sh [<base>] <commit>" >&2; exit 2; }
list=$(mktemp)
trap 'rm -f "$list"' EXIT
# NUL-separated, so a path is read exactly as it is, whatever its bytes.
if [ $# -eq 2 ]; then
  git diff -z --name-only "$1" "$2" -- > "$list" || { echo "docs_only: cannot read ${1}..${2}" >&2; exit 2; }
else
  git diff-tree -z --no-commit-id --name-only -r --root "$1" > "$list" || {
    echo "docs_only: cannot read ${1}" >&2; exit 2; }
fi
while IFS= read -r -d '' f; do
  case "$f" in
    .github/*|third_party/*|LICENSING.md|docs/CONFIG.md) printf '%s\n' "$f"; exit 1 ;;
    docs/*|*.md) ;;
    *) printf '%s\n' "$f"; exit 1 ;;
  esac
done < "$list"
exit 0
