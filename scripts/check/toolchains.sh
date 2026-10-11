#!/usr/bin/env bash
# Are the pinned toolchains behind upstream? Reads each script's pin, asks
# upstream for its latest release, and prints a table; exits 1 when any is
# behind, with the new tarball's SHA-256 already computed for the bump. Run
# monthly by .github/workflows/toolchains.yml, which files the table as an
# issue; by hand any time. Nothing is changed here: a bump is a version and a
# sum in scripts/build/{zig,emsdk,llvm_mingw}.sh and a pull request CI builds
# with, which is the whole point of pinning. Needs curl and python3.
set -uo pipefail
cd "$(dirname "$0")/../.."
behind=0
row() { printf '| %s | %s | %s | %s |\n' "$@"; }
sha() { curl -fsSL --retry 3 "$1" | sha256sum | cut -d" " -f1; }
echo "| Toolchain | Pinned | Latest | SHA-256 of the latest (Linux x86_64) |"
echo "|---|---|---|---|"

pin=$(sed -n 's/^ZIG_VERSION="${ZIG_VERSION:-\(.*\)}"/\1/p' scripts/build/zig.sh)
# zig's index carries each release's tarball and its sum; the tarball's name
# changed shape after 0.13, so the index is the only place to read it from.
read -r latest zsum < <(curl -fsSL https://ziglang.org/download/index.json | python3 -c 'import json,sys; d=json.load(sys.stdin); v=max((k for k in d if k!="master"), key=lambda v: tuple(int(x) for x in v.split("."))); print(v, d[v]["x86_64-linux"]["shasum"])' 2>/dev/null)
if [ -n "${latest:-}" ] && [ "$latest" != "$pin" ]; then row zig "$pin" "$latest" "$zsum (its tarball name is in the index: zig.sh's URL changes with it)"; behind=1
else row zig "$pin" "${latest:-?}" "-"; fi

pin=$(sed -n 's/^EMSDK_VERSION="${EMSDK_VERSION:-\(.*\)}"/\1/p' scripts/build/emsdk.sh)
latest=$(curl -fsSL https://api.github.com/repos/emscripten-core/emsdk/tags?per_page=5 | python3 -c 'import json,sys; t=[x["name"] for x in json.load(sys.stdin) if x["name"][0].isdigit()]; print(t[0] if t else "")' 2>/dev/null)
if [ -n "$latest" ] && [ "$latest" != "$pin" ]; then row emsdk "$pin" "$latest" "(a git tag; emsdk pins its own binaries)"; behind=1
else row emsdk "$pin" "${latest:-?}" "-"; fi

pin=$(sed -n 's/^LLVM_MINGW_VERSION="${LLVM_MINGW_VERSION:-\(.*\)}"/\1/p' scripts/build/llvm_mingw.sh)
latest=$(curl -fsSL https://api.github.com/repos/mstorsjo/llvm-mingw/releases/latest | python3 -c 'import json,sys; print(json.load(sys.stdin)["tag_name"])' 2>/dev/null)
if [ -n "$latest" ] && [ "$latest" != "$pin" ]; then
  row llvm-mingw "$pin" "$latest" "$(sha "https://github.com/mstorsjo/llvm-mingw/releases/download/${latest}/llvm-mingw-${latest}-ucrt-ubuntu-22.04-x86_64.tar.xz")"; behind=1
else row llvm-mingw "$pin" "${latest:-?}" "-"; fi
exit $behind
