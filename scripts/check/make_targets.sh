#!/usr/bin/env bash
# The Makefile's typed targets are the ones its header names, and no other
# phony target exists: PUBLIC (the header's list) and INTERNAL (CI's and
# make's own) in the Makefile must together name every phony rule in Makefile
# and mk/*.mk, and the header's "make <target>" lines must be exactly PUBLIC.
# Run by `make test` with the other source checks; keeps the target list from
# growing back one convenience at a time.
set -uo pipefail
cd "$(dirname "$0")/../.."
public=$(sed -n 's/^PUBLIC *:= *//p' Makefile | tr ' ' '\n' | grep . | sort)
internal=$(sed -n 's/^INTERNAL *:= *//p' Makefile | tr ' ' '\n' | grep . | sort)
# The header: every "#   make <target>   description" line before the first
# non-comment line; a bare "make" names the default goal.
header=$(awk '/^[^#]/{exit} /^#   make( |$)/{ t=$3; if (match($0, /^#   make  /)) t="all"; print t }' Makefile | sort -u)
fail=0
if [ "$header" != "$public" ]; then
    echo "make_targets: the Makefile header and PUBLIC differ:" >&2
    diff <(echo "$header") <(echo "$public") | sed 's/^/  /' >&2
    fail=1
fi
# Every rule whose target is a plain word (no slash, no dot, no variable, no %)
# is phony by this tree's convention: files are always under build/ or a path.
rules=$(grep -h -E '^[A-Za-z][A-Za-z0-9_-]*:([^=]|$)' Makefile mk/*.mk | sed 's/:.*//' | sort -u)
listed=$(printf '%s\n%s\n' "$public" "$internal" | sort -u)
unlisted=$(comm -23 <(echo "$rules") <(echo "$listed") | grep -vx 'build')
if [ -n "$unlisted" ]; then
    echo "make_targets: phony rules in neither PUBLIC nor INTERNAL: $(echo $unlisted)" >&2
    echo "make_targets: a target a person types goes in PUBLIC and the header; one only make or CI calls goes in INTERNAL; a tool is built by its path." >&2
    fail=1
fi
[ $fail = 0 ] && echo "make_targets: $(echo "$public" | wc -l) typed targets, as the header says"
exit $fail
