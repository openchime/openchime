#!/usr/bin/env bash
# scripts/test/merge/web_deps.sh
#
# The web client's objects follow their headers (mk/web.mk): touching
# client/core/event.h makes `make -n web` recompile every web object whose
# dependency file names it, and no other; touching one source recompiles that
# object alone. Without this, a struct that grows leaves the client mixing two
# layouts, which shows only as random traps at run time. Nothing is built: the
# touched files get their times back, so the tree stays as built.
#
# Needs: the web client built (its .d files under build/wasm/); MAKE, with the
# switches make passes it.
# Run by: make test-merge (PART=plain).
. "$(dirname "$0")/../lib/common.sh"
have build/web/openchime.js "make web builds it"
MAKE=${MAKE:-make}

# The objects `make -n web` would compile, one per line. app.o is left out:
# it depends on the build-id stamp (build/.build_id), which is always remade and
# only rewritten when the id changes, so a dry run always lists it.
APP_O=build/wasm/gui/client/gui/app/app.o
would_build() {
    $MAKE -n web 2>/dev/null | grep -oE -- '-o build/wasm/(gui|core)/[^ ]+\.o' | cut -c4- | grep -vxF "$APP_O" | sort -u
}

# Touch $1, list what would be built, and give $1 its time back.
after_touch() {
    local f=$1 t
    t=$(stat -c %Y "$f")
    touch "$f"
    would_build
    touch -d "@$t" "$f"
}

[ -z "$(would_build)" ] || fail "make web has objects to build before anything is touched: build the web client first"

hdr=client/core/event.h
want=$(grep -l -F "$hdr" $(find build/wasm/gui build/wasm/core -name '*.d') | sed 's/\.d$/.o/' | grep -vxF "$APP_O" | sort -u)
[ -n "$want" ] || fail "no web object's dependency file names $hdr"
got=$(after_touch "$hdr")
[ "$got" = "$want" ] || fail "touching $hdr: make -n web would build
$got
where the dependency files name
$want"
ok "touching $hdr recompiles the $(wc -l <<<"$want") web objects that include it, and no other"

src=client/gui/platform/web/plat_web.c
got=$(after_touch "$src")
[ "$got" = "build/wasm/gui/${src%.c}.o" ] || fail "touching $src: make -n web would build
$got
where only build/wasm/gui/${src%.c}.o should be"
ok "touching $src recompiles its object alone"
