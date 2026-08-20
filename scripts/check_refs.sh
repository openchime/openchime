#!/bin/sh
# Fail if any tracked file cites an issue by number.
#
# The rule (CONTRIBUTING.md): a commit message cites the issue it resolves; a
# FILE never does. An issue number in a comment is a pointer with no integrity —
# it rots the moment the issue is closed, renamed, merged into another or
# superseded, and nothing in the file can tell the reader that it has. The
# project already lived through the stronger version of this: the markdown
# backlog numbered its items by POSITION, so every citation drifted whenever an
# item above it closed, and hundreds of comments ended up naming the wrong item.
#
# What a comment should do instead is say the thing: not "see #144" but "the
# daemon exits 0 when it cannot start, which systemd reads as success". A
# reference to REQ-N or ARCH-N is fine and expected — REQUIREMENTS.md and
# ARCHITECTURE.md are stable documents that live in this repository, and their
# ids are identifiers rather than positions.
#
# Run by `make check-refs`, by `make test`, and by CI's build job.
#
# Patterns, all of which are failures:
#   #123            an issue citation, in any file
#   issues/123      the same thing as a URL
#   WIN-12          the retired per-item tag of the old markdown backlog
#   BACKLOG         that file, which no longer exists
#   backlog item    prose naming an entry in it
#
# Deliberately NOT failures: a bare link to the tracker with no number; the
# ordinary English "backlog" (a queue of pending work — daemon/netloop.c uses it
# correctly); `#define`/`#include`; a markdown anchor (#3-delivery); a six-digit
# hex colour; and `UAX #29`, which is the Unicode annex's own name.
set -eu

self='scripts/check_refs.sh'
# One pattern per rule, ERE. `#N` requires the digits to end the token, which is
# what keeps anchors, colours and preprocessor lines out of it.
pat_num='(^|[^&[:alnum:]_])#[0-9]{1,4}([^0-9A-Za-z-]|$)'
pat_url='issues/[0-9]+'
pat_win='WIN-[0-9]+'
pat_bl='BACKLOG|backlog item|backlog #'

fail=0
files=$(git ls-files | grep -v '^third_party/' | grep -v "^$self\$" || true)
[ -n "$files" ] || { echo "check_refs: no tracked files to scan" >&2; exit 2; }

for pat in "$pat_num" "$pat_url" "$pat_win" "$pat_bl"; do
    # UAX #NN is the one legitimate "#number" in the tree; drop it before judging.
    hits=$(printf '%s\n' "$files" | xargs grep -nE "$pat" 2>/dev/null | grep -v 'UAX #' || true)
    if [ -n "$hits" ]; then
        fail=1
        printf '%s\n' "$hits" | while IFS= read -r line; do
            echo "check_refs: issue reference in a file: $line" >&2
        done
    fi
done

if [ "$fail" -ne 0 ]; then
    echo "check_refs: files cite no issue numbers — say the thing, or cite REQ-N / ARCH-N." >&2
    exit 1
fi

n=$(printf '%s\n' "$files" | wc -l | tr -d ' ')
echo "check_refs: $n tracked files, no issue references"
