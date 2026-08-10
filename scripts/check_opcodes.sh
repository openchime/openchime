#!/bin/sh
# Fail if two OC_MSG_* message types share an opcode.
#
# The wire contract is "one value, one meaning". Two collisions reached main
# unnoticed (BACKLOG.md, "Two message-type opcodes are used twice each") because
# nothing checked: each pair happened to be direction-disjoint, so both dispatch
# chains held exactly one branch for the value and nothing misdecoded. That is a
# property of today's branches, not of the protocol, and the next inbound branch
# for the opposite direction removes it silently.
#
# Run by `make check-opcodes` and by CI's build job.
#
# KNOWN_SHARED lists the opcodes that are still shared and are tracked as open
# backlog items. An entry here is a defect with a number, not an exemption:
# delete the value when the item is fixed, and the check starts enforcing it.
set -eu

HDR="${1:-shared/protocol.h}"

# 0x0070 — SET_PROFILE (C->S) and SET_PRESENCE (C->S). NOT direction-disjoint:
# editing a profile drops the connection and saves nothing. Backlog item 114.
KNOWN_SHARED="0x0070"

[ -r "$HDR" ] || { echo "check_opcodes: cannot read $HDR" >&2; exit 2; }

# All reporting and the exit status live in awk, so there is no pipeline for a
# missing trailing newline to swallow a finding in.
awk -v known="$KNOWN_SHARED" '
    BEGIN { split(known, k, /[ \t]+/); for (i in k) allowed[k[i]] = 1 }

    # The enum lines only: OC_MSG_<NAME> = 0x<hex>
    match($0, /OC_MSG_[A-Z0-9_]+[ \t]*=[ \t]*0x[0-9A-Fa-f]+/) {
        split(substr($0, RSTART, RLENGTH), part, /[ \t]*=[ \t]*/)
        name = part[1]
        code = part[2]
        sub(/^0[xX]/, "", code)
        # Numeric, not textual. Normalising case alone left zero-padding
        # significant, so 0x70 and 0x0070 -- the same opcode -- produced
        # different keys and a genuine duplicate written with different padding
        # was never detected. The header promises "one value, one meaning"; this
        # is what enforces it rather than "one spelling, one meaning".
        #
        # NOTE the variable names. This script already uses `n` to COUNT opcodes
        # and `i` in its BEGIN loop; an accumulator called `n` silently corrupts
        # the count (a 2-opcode fixture reported "113 opcodes").
        val = 0
        for (ci = 1; ci <= length(code); ci++) {
            ch = tolower(substr(code, ci, 1))
            val = val * 16 + index("0123456789abcdef", ch) - 1
        }
        code = sprintf("0x%04X", val)
        if (code in seen) {
            if (code in allowed)
                printf "check_opcodes: known shared opcode %s (%s, %s) — tracked in BACKLOG.md\n",
                       code, seen[code], name
            else {
                printf "check_opcodes: DUPLICATE opcode %s used by %s and %s\n",
                       code, seen[code], name > "/dev/stderr"
                bad++
            }
        } else seen[code] = name
        n++
    }

    END {
        if (n == 0) {
            print "check_opcodes: no OC_MSG_* definitions found — wrong file?" > "/dev/stderr"
            exit 2
        }
        if (bad) {
            print "check_opcodes: a message type'\''s opcode is not unique. One value, one meaning." > "/dev/stderr"
            exit 1
        }
        printf "check_opcodes: %d OC_MSG_* opcodes, all unique (known-shared: %s)\n", n, known
    }
' "$HDR"
