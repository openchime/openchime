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
# What a comment should do instead is say the thing: not a pointer at an issue
# but "the daemon exits 0 when it cannot start, which systemd reads as success".
# A
# reference to REQ-N or ARCH-N is fine and expected — REQUIREMENTS.md and
# ARCHITECTURE.md are stable documents that live in this repository, and their
# ids are identifiers rather than positions.
#
# Run by `make check-refs`, by `make test`, and by CI's build job.
#
# Four things fail, described rather than written out, because this file is
# scanned like every other one:
#   - a hash followed by one to four digits, in any file
#   - a tracker URL whose last segment is a number
#   - the retired per-item tag of the old markdown list (its three-letter
#     platform prefix, a dash, digits)
#   - that list's filename, or prose naming an entry in it
#
# Deliberately NOT failures: a bare link to the tracker with no number; the
# ordinary English "backlog" (a queue of pending work — daemon/netloop.c uses it
# correctly); `#define`/`#include`; a markdown anchor; a six-digit hex colour;
# and the Unicode annex's own name, which is a hash and a number.

set -eu

# One pattern per rule, ERE. The hash rule requires the digits to end the token,
# which is what keeps anchors, colours and preprocessor lines out of it.
#
# The two literal strings this check bans are ASSEMBLED here rather than written,
# so that this file passes its own check and can be scanned with everything else.
# A checker that has to exempt itself cannot tell you the tree is clean.
win_tag=$(printf 'W%sN' 'I')
old_list=$(printf 'BACK%sOG' 'L')
pat_num='(^|[^&[:alnum:]_])#[0-9]{1,4}([^0-9A-Za-z-]|$)'
pat_url='issues/[0-9]+'
pat_win="$win_tag-[0-9]+"
pat_bl="$old_list|$(printf 'back%sog item' 'l')|$(printf 'back%sog #' 'l')"

fail=0
# Vendored code is excluded because it is not ours to rewrite: mbedTLS ships a
# ChangeLog full of upstream issue numbers, and editing them would fork a pinned
# dependency to satisfy a house rule. NOTHING ELSE is excluded — not this file.
files=$(git ls-files | grep -v '^third_party/' || true)
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

# A phrase that wraps across a line break is invisible to a line-scoped grep —
# "it is a backlog\nitem" hid in a document through the first version of this
# check. Second pass: fold each file to one line and look again. Reported per
# file, since a folded file has no line numbers left to report.
for f in $files; do
    folded=$(tr '\n' ' ' < "$f" | tr -s ' ')
    if printf '%s' "$folded" | grep -qE "$(printf 'back%sog (item|entry|entries)' 'l')"; then
        fail=1
        echo "check_refs: issue reference wrapped across lines: $f" >&2
    fi
done

if [ "$fail" -ne 0 ]; then
    echo "check_refs: files cite no issue numbers — say the thing, or cite REQ-N / ARCH-N." >&2
    exit 1
fi

n=$(printf '%s\n' "$files" | wc -l | tr -d ' ')
echo "check_refs: $n tracked files, no issue references"
