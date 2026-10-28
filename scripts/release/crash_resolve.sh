#!/usr/bin/env bash
# Turn a crash report's RVA into a file and line.
#
#   scripts/release/crash_resolve.sh /mnt/c/Windows/Temp/octest/crash-1234.txt
#   scripts/release/crash_resolve.sh 0x283a9                    # just an RVA
#
# The client writes crash-<pid>.txt on an unhandled exception (see crash_filter in
# client/gui/platform/win32/plat_win32.c): the exception code, the faulting address, the module's load base, the
# RVA between them, and the last 64 breadcrumbs. The RVA is the part that needs a
# build to interpret, and it only resolves against the SAME binary that crashed —
# rebuild in between and the answer is fiction.
#
# Resolves with the toolchain's addr2line (llvm-symbolizer behind llvm-mingw's
# x86_64-w64-mingw32-addr2line, third_party/llvm-mingw-*/bin), which reads
# PE + DWARF; GNU addr2line returned "??" for the same binary, which is why an
# earlier version of this walked the line table itself.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
EXE="${OC_CRASH_EXE:-$HERE/build/openchime.exe}"
TOOLS="$(ls -d "$HERE"/third_party/llvm-mingw-*/bin 2>/dev/null | sort | tail -1)"
OBJDUMP="${OBJDUMP:-${TOOLS:+$TOOLS/}x86_64-w64-mingw32-objdump}"
ADDR2LINE="${ADDR2LINE:-${TOOLS:+$TOOLS/}x86_64-w64-mingw32-addr2line}"

[ $# -ge 1 ] || { echo "usage: $0 <crash-report.txt | 0xRVA>" >&2; exit 2; }
[ -f "$EXE" ] || { echo "no $EXE: set OC_CRASH_EXE" >&2; exit 2; }

arg="$1"
if [ -f "$arg" ]; then
  rva=$(grep -o 'rva=0x[0-9a-f]*' "$arg" | head -1 | cut -d= -f2)
  [ -n "$rva" ] || { echo "no rva= line in $arg" >&2; exit 1; }
  echo "--- report"; cat "$arg"; echo
else
  rva="$arg"
fi

# PE ImageBase, read from the binary rather than assumed: a different link could
# change it, and a wrong base gives a confidently wrong line. llvm-objdump prints
# it as 0x..., GNU's as bare hex; both are taken.
# No early `exit` in awk here: it closes the pipe, objdump takes SIGPIPE, and
# `set -o pipefail` then kills this script before it prints anything — which it did.
base=$("$OBJDUMP" -p "$EXE" | awk '/ImageBase/{b=$2} END{sub(/^0x/,"",b); print "0x" b}')
target=$(( base + rva ))
printf -- "--- resolving rva=%s against %s (ImageBase=%s) -> 0x%x\n" "$rva" "$(basename "$EXE")" "$base" "$target"
"$ADDR2LINE" -e "$EXE" -f -C -i "$(printf '0x%x' "$target")" | sed 's/^/  /'
