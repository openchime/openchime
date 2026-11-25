#!/usr/bin/env bash
# scripts/test/fast/vendored.sh
#
# mbedTLS and SDL3 are committed in third_party/mbedtls and third_party/SDL3
# (docs/details/VENDORS.md §1) and built from there with nothing downloaded.
#
# mbedTLS: Its version is written in six places
# that must agree: the tree's own include/mbedtls/build_info.h, the build
# directories the makefiles name (MBEDTLS_DIR, MBEDTLS_WIN) and the three
# scripts that build it. A tree replaced without the rest following it, or a
# script that goes back to fetching, fails here.
#
# First each matcher is shown a planted line, so a pattern that has stopped
# matching anything fails here rather than passing the tree unchecked.
#
# Needs: make.
# Run by: scripts/test/fast/source.sh (make test; make test-all).
. "$(dirname "$0")/../lib/common.sh"

tree_dir=third_party/mbedtls
scripts=(scripts/build/mbedtls.sh scripts/build/mbedtls_windows.sh scripts/build/mbedtls_wasm.sh)
download='(^|[^[:alnum:]_])(curl|wget)([^[:alnum:]_]|$)|https?://|git +clone'
header_version() { sed -n 's/^#define MBEDTLS_VERSION_STRING  *"\(.*\)"$/\1/p' "$@"; }
script_version() { sed -n 's/^MBEDTLS_VERSION=\([0-9][0-9.]*\)$/\1/p' "$@"; }

grep -qE "$download" <<<'curl -fsSL -o "${tmp}" "${URL}"' || fail "the pattern no longer matches a planted curl"
grep -qE "$download" <<<'URL="https://example.org/mbedtls.tar.bz2"' || fail "the pattern no longer matches a planted URL"
! grep -qE "$download" <<<'cp -a "${TREE}" "${SRC}.new"' || fail "the pattern matches a copy of the committed tree"
[ "$(header_version <<<'#define MBEDTLS_VERSION_STRING         "1.2.3"')" = 1.2.3 ] || fail "the header's version line is no longer read"
[ "$(script_version <<<'MBEDTLS_VERSION=1.2.3')" = 1.2.3 ] || fail "a script's version line is no longer read"

[ -f "$tree_dir/include/mbedtls/build_info.h" ] || fail "$tree_dir is missing: mbedTLS is committed there (docs/details/VENDORS.md)"
tree=$(header_version "$tree_dir/include/mbedtls/build_info.h")
[ -n "$tree" ] || fail "$tree_dir/include/mbedtls/build_info.h names no version"

# Not the calling make's flags or variables, and not the tree's remembered
# switches: a fresh make's own values (scripts/test/fast/notices.sh).
unset MAKEFLAGS MFLAGS MAKELEVEL
vars=$(make -s -p -n -f Makefile REMEMBER=0 FORCE 2>/dev/null || true)
bad=0
for v in MBEDTLS_DIR MBEDTLS_WIN; do
    want="$tree_dir-$tree"
    [ "$v" = MBEDTLS_WIN ] && want="$want-win"
    got=$(sed -n "s/^$v := //p" <<<"$vars" | head -1)
    [ "$got" = "$want" ] || { error "$v is '$got' and the committed mbedTLS is $tree: it should be $want"; bad=1; }
done
for s in "${scripts[@]}"; do
    got=$(script_version "$s")
    [ "$got" = "$tree" ] || { error "$s builds mbedTLS '$got' and the committed tree is $tree"; bad=1; }
    hit=$(grep -nE "$download" "$s" || true)
    [ -z "$hit" ] || { error "$s downloads, and mbedTLS is built from $tree_dir alone: $hit"; bad=1; }
done
[ "$bad" = 0 ] || exit 1
ok "mbedTLS $tree is built from $tree_dir, by the makefiles and its three scripts, with nothing downloaded"

# SDL3: its version is written in four places that must agree: the tree's own
# include/SDL3/SDL_version.h (three numbers), the build directories the
# makefiles name (SDL3_VERSION in mk/sources.mk, SDL3_WIN in mk/windows.mk and
# SDL3_LINUX in mk/linux.mk) and the three scripts that build it. No script may
# download.
sdl_dir=third_party/SDL3
sdl_scripts=(scripts/build/sdl3_windows.sh scripts/build/sdl3_wasm.sh scripts/build/sdl3_linux.sh)
sdl_header_version() { sed -n 's/^#define SDL_\(MAJOR\|MINOR\|MICRO\)_VERSION  *\([0-9]*\)$/\2/p' "$@" | paste -sd.; }
sdl_script_version() { sed -n 's/^SDL3_VERSION=\([0-9][0-9.]*\)$/\1/p' "$@"; }

[ "$(sdl_header_version <<<$'#define SDL_MAJOR_VERSION   1\n#define SDL_MINOR_VERSION   2\n#define SDL_MICRO_VERSION   3')" = 1.2.3 ] ||
    fail "SDL's version lines are no longer read"
[ "$(sdl_script_version <<<'SDL3_VERSION=1.2.3')" = 1.2.3 ] || fail "an SDL3 script's version line is no longer read"
grep -qE "$download" <<<'curl -fsSL -o "$TARBALL" "https://github.com/libsdl-org/SDL/releases/download/x"' ||
    fail "the pattern no longer matches a planted SDL3 download"

[ -f "$sdl_dir/include/SDL3/SDL_version.h" ] || fail "$sdl_dir is missing: SDL3 is committed there (docs/details/VENDORS.md)"
sdl=$(sdl_header_version "$sdl_dir/include/SDL3/SDL_version.h")
[ -n "$sdl" ] || fail "$sdl_dir/include/SDL3/SDL_version.h names no version"
bad=0
got=$(sed -n 's/^SDL3_VERSION := //p' <<<"$vars" | head -1)
[ "$got" = "$sdl" ] || { error "SDL3_VERSION is '$got' and the committed SDL3 is $sdl"; bad=1; }
got=$(sed -n 's/^SDL3_WIN := //p' <<<"$vars" | head -1)
[ "$got" = "third_party/sdl3-$sdl-win" ] || { error "SDL3_WIN is '$got' and the committed SDL3 is $sdl: it should be third_party/sdl3-$sdl-win"; bad=1; }
got=$(sed -n 's/^SDL3_LINUX := //p' <<<"$vars" | head -1)
[ "$got" = "third_party/sdl3-$sdl" ] || { error "SDL3_LINUX is '$got' and the committed SDL3 is $sdl: it should be third_party/sdl3-$sdl"; bad=1; }
for s in "${sdl_scripts[@]}"; do
    got=$(sdl_script_version "$s")
    [ "$got" = "$sdl" ] || { error "$s builds SDL3 '$got' and the committed tree is $sdl"; bad=1; }
    hit=$(grep -nE "$download" "$s" || true)
    [ -z "$hit" ] || { error "$s downloads, and SDL3 is built from $sdl_dir alone: $hit"; bad=1; }
done
[ "$bad" = 0 ] || exit 1
ok "SDL3 $sdl is built from $sdl_dir, by the makefiles and its three scripts, with nothing downloaded"
