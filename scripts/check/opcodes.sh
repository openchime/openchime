#!/bin/sh
# Fail if two OC_MSG_* message types share an opcode.
#
# The wire contract is "one value, one meaning".
#
# Run by `make test` and `make test-ci` (mk/tests.mk SOURCE_CHECKS).
#
# KNOWN_SHARED lists opcodes that are still shared. An entry here is a defect on
# the record, not an exemption: delete the value when it is fixed, and the check
# starts enforcing it. KNOWN_SHARED is empty: every opcode carries exactly one
# message type.
set -eu

HDR="${1:-shared/protocol.h}"

KNOWN_SHARED=""

[ -r "$HDR" ] || { echo "check_opcodes: cannot read $HDR" >&2; exit 2; }

# All reporting and the exit status live in awk, so there is no pipeline for a
# missing trailing newline to swallow a finding in.
awk -v known="$KNOWN_SHARED" '
    BEGIN { split(known, k, /[ \t]+/); for (i in k) allowed[k[i]] = 1 }

    # A line that defines an OC_MSG_ name -- it starts with one, or assigns one
    # -- must be exactly one enum line, OC_MSG_<NAME> = 0x<hex>, or the check
    # cannot vouch for it: a decimal value, an implicit one, or two on a line
    # would otherwise be skipped and their opcodes never compared.
    /^[ \t]*OC_MSG_[A-Z0-9_]+/ || /OC_MSG_[A-Z0-9_]+[ \t]*=/ {
        line = $0
        sub(/\/\*.*$/, "", line); sub(/\/\/.*$/, "", line)
        defs = gsub(/OC_MSG_[A-Z0-9_]+[ \t]*=/, "&", line)
        if (line !~ /^[ \t]*#[ \t]*define[ \t]/ &&
            (defs != 1 || line !~ /^[ \t]*OC_MSG_[A-Z0-9_]+[ \t]*=[ \t]*0[xX][0-9A-Fa-f]+[ \t]*,?[ \t]*$/)) {
            printf "check_opcodes: %s:%d: cannot parse this OC_MSG_ line: %s\n", FILENAME, FNR, $0 > "/dev/stderr"
            unparsed++
            next
        }
    }

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
                printf "check_opcodes: known shared opcode %s (%s, %s): a tracked defect\n",
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
            print "check_opcodes: no OC_MSG_* definitions found: wrong file?" > "/dev/stderr"
            exit 2
        }
        if (unparsed) {
            print "check_opcodes: an OC_MSG_ line is not one `OC_MSG_<NAME> = 0x<hex>,` definition." > "/dev/stderr"
            exit 1
        }
        if (bad) {
            print "check_opcodes: a message type'\''s opcode is not unique. One value, one meaning." > "/dev/stderr"
            exit 1
        }
        printf "check_opcodes: %d OC_MSG_* opcodes, all unique (known-shared: %s)\n", n, known
    }
' "$HDR"
