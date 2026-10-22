#!/usr/bin/env bash
# The local test before a commit (/oc-feature-finish): build the daemon and run
# the suites once (make && make test), and, when both pass, record the tree that passed, so the
# pre-push hook (.githooks/pre-push) can tell a tested push from an untested
# one. CI is the confirmation, not where a failure is found: a red run costs
# billed minutes, a red local run costs none.
#
#   scripts/check/test.sh
#
# The tree recorded is the working tree as it is now, untracked files
# included, computed in an index of its own so the real one is untouched: what
# the commit that follows will hold.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

make
make test

index=$(mktemp)
trap 'rm -f "$index"' EXIT
GIT_INDEX_FILE="$index" git read-tree HEAD
GIT_INDEX_FILE="$index" git add -A
tree=$(GIT_INDEX_FILE="$index" git write-tree)
stamp="$(git rev-parse --git-dir)/oc-tested-tree"
# The newest ten trees, this one first.
{ printf '%s\n' "$tree"; if [ -f "$stamp" ]; then head -n 9 "$stamp"; fi; } > "$stamp.new"
mv "$stamp.new" "$stamp"
echo "tested: tree ${tree:0:12} recorded for the pre-push hook"
