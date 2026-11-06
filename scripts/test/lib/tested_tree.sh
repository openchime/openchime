#!/usr/bin/env bash
# scripts/test/lib/tested_tree.sh record | has <tree>
#
# The trees `make test` passed on, newest first, ten at most, in this clone's
# git directory (oc-tested-tree). `record` adds the working tree as it is now,
# untracked files included, computed in an index of its own so the real one is
# untouched: what the commit that follows will hold. `has <tree>` exits 0 when
# <tree> is one of them, which is how the pre-push hook (.githooks/pre-push)
# tells a tested push from an untested one.
#
# Run by: make test (record, its last step, on a pass); .githooks/pre-push (has).
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
stamp="$(git rev-parse --git-dir)/oc-tested-tree"
case "${1:-}" in
    record)
        index=$(mktemp)
        trap 'rm -f "$index"' EXIT
        GIT_INDEX_FILE="$index" git read-tree HEAD
        GIT_INDEX_FILE="$index" git add -A
        tree=$(GIT_INDEX_FILE="$index" git write-tree)
        { printf '%s\n' "$tree"; if [ -f "$stamp" ]; then head -n 9 "$stamp"; fi; } > "$stamp.new"
        mv "$stamp.new" "$stamp"
        echo "tested: tree ${tree:0:12} recorded for the pre-push hook"
        ;;
    has)
        [ -n "${2:-}" ] && grep -qxF "$2" "$stamp" 2>/dev/null
        ;;
    *)
        echo "usage: scripts/test/lib/tested_tree.sh record | has <tree>" >&2
        exit 2
        ;;
esac
