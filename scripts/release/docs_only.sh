#!/usr/bin/env bash
# Is a commit documentation only? Exit 0 when every path it changes is a
# Markdown file (*.md) or under docs/; exit 1 otherwise, printing the first
# other path; exit 2 when the commit cannot be read, which a caller treats as
# fatal. Not documentation, whatever their names: .github/ and .claude/
# (workflows and commands change how the repository builds and works),
# third_party/ (vendored sources), LICENSING.md (what the packages carry), and
# docs/CONFIG.md (its reference is generated from the daemon's settings and
# checked against them by scripts/check/config.sh, so it ships with the code).
# Used by promote, which copies such commits onto main without a release, and
# by scripts/release/main_next.sh.
#
#   scripts/release/docs_only.sh <commit>
set -uo pipefail
c="${1:?usage: scripts/release/docs_only.sh <commit>}"
list=$(mktemp)
trap 'rm -f "$list"' EXIT
# NUL-separated, so a path is read exactly as it is, whatever its bytes.
git diff-tree -z --no-commit-id --name-only -r --root "$c" > "$list" || {
  echo "docs_only: cannot read ${c}" >&2; exit 2; }
while IFS= read -r -d '' f; do
  case "$f" in
    .github/*|.claude/*|third_party/*|LICENSING.md|docs/CONFIG.md) printf '%s\n' "$f"; exit 1 ;;
    docs/*|*.md) ;;
    *) printf '%s\n' "$f"; exit 1 ;;
  esac
done < "$list"
exit 0
