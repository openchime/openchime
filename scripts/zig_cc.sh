#!/bin/sh
# Run zig's C or C++ compiler for a target, as a drop-in for gcc-style flags:
#
#   scripts/zig_cc.sh cc|c++ TARGET [FLAG...] -- compiler arguments
#
# The release builds ONNX Runtime with this as CC/CXX (ARCH-111). Everything
# passes through untouched except -march on arm64, which zig does not accept:
# ONNX Runtime and KleidiAI ask for instruction sets as -march=armv8.2-a+fp16,
# and zig takes the same request as -mcpu=generic+v8_2a+fullfp16 (plus the
# original, for the assembler). Unrecognised, the flag is an error ("unknown
# CPU"), and ONNX Runtime refuses to configure.
set -eu

tool="$1"
target="$2"
shift 2
pre=""
while [ $# -gt 0 ] && [ "$1" != "--" ]; do
  pre="$pre $1"
  shift
done
[ $# -gt 0 ] && shift

for a do
  shift
  case "$a" in
    -march=armv8*|-march=armv9*)
      spec="${a#-march=armv}"            # 8.2-a+fp16+dotprod
      base="${spec%%+*}"                 # 8.2-a
      feats="${spec#"$base"}"            # +fp16+dotprod
      ver="v$(printf '%s' "${base%-a}" | tr . _)a"   # v8_2a
      # GCC's names to LLVM's: fp16 is fullfp16, and +noX removes X.
      feats="$(printf '%s' "$feats" | sed -E 's/\+fp16(\+|$)/+fullfp16\1/g; s/\+fp16(\+|$)/+fullfp16\1/g; s/\+no([a-z0-9_]+)/-\1/g')"
      # zig hands -mcpu to the compiler but not to the assembler, so a .S file
      # would still be assembled for the baseline; the assembler takes -march
      # as GCC spells it.
      set -- "$@" "-mcpu=generic+${ver}${feats}" "-Wa,$a"
      ;;
    *) set -- "$@" "$a" ;;
  esac
done

# shellcheck disable=SC2086 # $pre is a list of flags
exec "${ZIG:-/opt/zig/zig}" "$tool" -target "$target" $pre "$@"
