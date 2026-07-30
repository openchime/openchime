#!/usr/bin/env bash
# Turn a crash report's RVA into a file and line.
#
#   scripts/crash_resolve.sh /mnt/c/Windows/Temp/octest/crash-1234.txt
#   scripts/crash_resolve.sh 0x283a9                    # just an RVA
#
# The client writes crash-<pid>.txt on an unhandled exception (see crash_filter in
# winmain.c): the exception code, the faulting address, the module's load base, the
# RVA between them, and the last 64 breadcrumbs. The RVA is the part that needs a
# build to interpret, and it only resolves against the SAME binary that crashed —
# rebuild in between and the answer is fiction.
#
# Uses objdump's decoded line table rather than addr2line: addr2line returns "??"
# for this PE + DWARF combination even when the line table plainly covers the
# address (verified — the instruction at the RVA was the expected one and the table
# spanned it), so this walks the table and takes the nearest entry at or below the
# target. Slower, but it answers.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXE="${OC_CRASH_EXE:-$HERE/build/openchime.exe}"
OBJDUMP="${OBJDUMP:-x86_64-w64-mingw32-objdump}"

[ $# -ge 1 ] || { echo "usage: $0 <crash-report.txt | 0xRVA>" >&2; exit 2; }
[ -f "$EXE" ] || { echo "no $EXE — set OC_CRASH_EXE" >&2; exit 2; }

arg="$1"
if [ -f "$arg" ]; then
  rva=$(grep -o 'rva=0x[0-9a-f]*' "$arg" | head -1 | cut -d= -f2)
  [ -n "$rva" ] || { echo "no rva= line in $arg" >&2; exit 1; }
  echo "--- report"; cat "$arg"; echo
else
  rva="$arg"
fi

# PE ImageBase, read from the binary rather than assumed: a different link could
# change it, and a wrong base gives a confidently wrong line.
# No early `exit` in awk here: it closes the pipe, objdump takes SIGPIPE, and
# `set -o pipefail` then kills this script before it prints anything — which it did.
base=$("$OBJDUMP" -p "$EXE" | awk '/ImageBase/{b="0x"$2} END{print b}')
target=$(( base + rva ))
printf -- "--- resolving rva=%s against %s (ImageBase=%s) -> 0x%x\n" "$rva" "$(basename "$EXE")" "$base" "$target"

"$OBJDUMP" --dwarf=decodedline "$EXE" 2>/dev/null | awk -v t="$target" '
  { for (i = 1; i <= NF; i++)
      if ($i ~ /^0x[0-9a-f]+$/) {
        a = strtonum($i)
        if (a <= t && a > best) { best = a; line = $0 }
      }
  }
  END {
    if (best == 0) { print "  no line-table entry at or below the target — built without -g?"; exit 1 }
    printf "  %s\n", line
  }'
