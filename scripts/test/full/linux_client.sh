#!/usr/bin/env bash
# scripts/test/full/linux_client.sh <dist> <version> <arch>
#
# The Linux client's three packages for <arch> (x86_64, aarch64), as
# `make build-all PART=linux-client` left them in <dist>, hold what a desktop
# needs:
#   - the desktop entry passes desktop-file-validate, in the .deb and as the
#     Flatpak exports it, and names the openchime:// scheme; the AppStream
#     metainfo passes `appstreamcli validate --no-net`;
#   - the .deb and the .rpm carry one binary, for <arch>, needing no glibc
#     newer than 2.39 (Ubuntu 24.04's, the client's floor: ARCH-20), and every
#     library it links is one their dependencies name: the .deb's Depends, and
#     the .rpm's requirements, each of which resolves to a declared package;
#   - the .deb's dependencies are installed on this machine, at the version it
#     asks of glibc, and its client, unpacked into a scratch root, prints its
#     version, where this machine can run it;
#   - the Flatpak bundle imports into a repository and is
#     app/io.openchime.OpenChime/<arch>/stable, on org.freedesktop.Platform at
#     the branch pinned in packaging/linux-client/freedesktop-runtime.conf,
#     with the sandbox the client needs (Wayland, X11 where there is no
#     Wayland, IPC, the GPU, the network, PipeWire, the portals and
#     notifications on the session bus); its binary is for <arch>, and every
#     library it links is in that runtime.
# An arm64 binary on an x86 machine is read, not run.
#
# Needs: dpkg-deb, dpkg-query, rpm, rpm2cpio, cpio, readelf,
# desktop-file-validate (desktop-file-utils), appstreamcli (appstream),
# flatpak, ostree; the runtime the Flatpak names, fetched
# (scripts/build/freedesktop_sdk.sh, which the build ran).
# Run by: make test-all (PART=linux-client).
. "$(dirname "$0")/../lib/common.sh"
need dpkg-deb "dpkg"; need dpkg-query "dpkg"; need rpm "rpm"; need rpm2cpio "rpm"; need cpio "cpio"
need readelf "binutils"; need desktop-file-validate "desktop-file-utils"; need appstreamcli "appstream"
need flatpak "flatpak"; need ostree "ostree"
dist=${1:?usage: linux_client.sh <dist> <version> <arch>} version=${2:?} arch=${3:?}
case "$arch" in
    x86_64)  deb_arch=amd64; machine='Advanced Micro Devices X86-64' ;;
    aarch64) deb_arch=arm64; machine=AArch64 ;;
    *) fail "unsupported arch $arch (x86_64, aarch64)" ;;
esac
app=io.openchime.OpenChime
deb="$dist/openchime_${version}_${deb_arch}.deb"
rpm="$dist/openchime-${version}-1.${arch}.rpm"
bundle="$dist/${app}-${version}-${arch}.flatpak"
for f in "$deb" "$rpm" "$bundle"; do have "$f" "make build-all PART=linux-client makes it"; done
. packaging/linux-client/freedesktop-runtime.conf

v=$(mktemp -d)
trap 'rm -rf "$v"' EXIT
mkdir -p "$v/deb" "$v/rpm"
dpkg-deb -x "$deb" "$v/deb"
rpm2cpio "$rpm" > "$v/rpm.cpio"
( cd "$v/rpm" && cpio -idm --quiet < ../rpm.cpio )

# The libraries the client links, by the name the loader looks for.
needed() { readelf -d "$1" | sed -n 's/.*(NEEDED).*\[\(.*\)\]$/\1/p'; }
# machine <binary> <what>: built for <arch>.
machine() {
    local m
    m=$(readelf -h "$1" | sed -n 's/^ *Machine: *//p')
    [ "$m" = "$machine" ] || fail "$2 is built for '$m', not $arch"
}

# --- The desktop entry and the metainfo -----------------------------------------
desktop="$v/deb/usr/share/applications/$app.desktop"
metainfo="$v/deb/usr/share/metainfo/$app.metainfo.xml"
have "$desktop" "the .deb installs it"; have "$metainfo" "the .deb installs it"
desktop-file-validate "$desktop" || fail "the .deb's $app.desktop does not validate"
grep -qx 'MimeType=x-scheme-handler/openchime;' "$desktop" || fail "$app.desktop does not name x-scheme-handler/openchime"
appstreamcli validate --no-net "$metainfo" >/dev/null || fail "$app.metainfo.xml does not validate: $(appstreamcli validate --no-net "$metainfo" 2>&1 | tail -n 3)"
for p in deb rpm; do
    for f in usr/bin/openchime "usr/share/applications/$app.desktop" "usr/share/metainfo/$app.metainfo.xml" \
             "usr/share/icons/hicolor/256x256/apps/$app.png" "usr/share/icons/hicolor/48x48/apps/$app.png"; do
        [ -f "$v/$p/$f" ] || fail "the .$p has no /$f"
    done
done

# --- The .deb and the .rpm ---------------------------------------------------------
bin="$v/deb/usr/bin/openchime"
cmp -s "$bin" "$v/rpm/usr/bin/openchime" || fail "the .deb and the .rpm carry different binaries"
machine "$bin" "the .deb's and the .rpm's client"
floor=GLIBC_2.39
max=$(readelf --dyn-syms --wide "$bin" | grep -oE 'GLIBC_[0-9]+\.[0-9]+' | sort -Vu | tail -1)
[ -n "$max" ] || fail "the packaged client requires no versioned glibc symbol: not the dynamically linked client"
[ "$(printf '%s\n%s\n' "$max" "$floor" | sort -V | tail -1)" = "$floor" ] ||
    fail "the packaged client requires $max, above the floor ${floor#GLIBC_}"

# Which package of each distribution holds each library the client may link.
deb_pkg() {
    case "$1" in
        libc.so.6|libm.so.6|libresolv.so.2|libpthread.so.0|libdl.so.2|ld-linux*) echo libc6 ;;
        libfreetype.so.6) echo libfreetype6 ;;
        libharfbuzz.so.0) echo libharfbuzz0b ;;
        libfontconfig.so.1) echo libfontconfig1 ;;
        libpipewire-0.3.so.0) echo libpipewire-0.3-0t64 ;;
        libglib-2.0.so.0|libgobject-2.0.so.0|libgio-2.0.so.0) echo libglib2.0-0t64 ;;
        libsecret-1.so.0) echo libsecret-1-0 ;;
    esac
}
rpm_pkg() {
    case "$1" in
        libc.so.6|libm.so.6|libresolv.so.2|libpthread.so.0|libdl.so.2|ld-linux*) echo glibc ;;
        libfreetype.so.6) echo freetype ;;
        libharfbuzz.so.0) echo harfbuzz ;;
        libfontconfig.so.1) echo fontconfig ;;
        libpipewire-0.3.so.0) echo pipewire-libs ;;
        libglib-2.0.so.0|libgobject-2.0.so.0|libgio-2.0.so.0) echo glib2 ;;
        libsecret-1.so.0) echo libsecret ;;
    esac
}
depends=$(dpkg-deb -f "$deb" Depends)
deb_names=$(tr ',' '\n' <<<"$depends" | sed 's/^ *//; s/ .*//' | grep .)
rpm_reqs=$(rpm -qpR "$rpm" 2>/dev/null)
for want in freetype harfbuzz fontconfig pipewire-libs glib2 libsecret; do
    grep -qx "$want" <<<"$rpm_reqs" || fail "the .rpm does not require $want"
done
for want in libc6 libfreetype6 libharfbuzz0b libfontconfig1 libpipewire-0.3-0t64 libglib2.0-0t64 libsecret-1-0; do
    grep -qx "$want" <<<"$deb_names" || fail "the .deb does not depend on $want"
done
links=$(needed "$bin")
for lib in $links; do
    p=$(deb_pkg "$lib")
    [ -n "$p" ] || fail "the client links $lib, which no package of its dependencies is known to hold"
    grep -qx "$p" <<<"$deb_names" || fail "the client links $lib, of $p, and the .deb does not depend on $p"
done
# Every library the .rpm requires, which rpm read from the binary, resolves to
# a package it declares (glibc's to the C library every system has).
while read -r req; do
    lib=${req%%(*}
    p=$(rpm_pkg "$lib")
    [ -n "$p" ] || fail "the .rpm requires $req, which no package it declares is known to hold"
    if [ "$p" != glibc ] && ! grep -qx "$p" <<<"$rpm_reqs"; then
        fail "the .rpm requires $req, of $p, and does not declare $p"
    fi
done < <(grep -E '^[^ (]+\.so[.0-9]*\(' <<<"$rpm_reqs")
ok "the .deb and the .rpm carry one $arch client, needing glibc ${max#GLIBC_}, and declare the $(wc -w <<<"$links") libraries it links"

if runnable "$bin"; then
    # What dpkg would refuse the .deb for: a dependency missing on this
    # machine, or glibc older than the client needs.
    for p in $deb_names; do
        have_v=$(dpkg-query -W -f='${Version}' "$p:$deb_arch" 2>/dev/null || dpkg-query -W -f='${Version}' "$p" 2>/dev/null || true)
        [ -n "$have_v" ] || fail "the .deb depends on $p, which this machine does not have"
    done
    libc_min=$(sed -n 's/.*libc6 (>= \([^)]*\)).*/\1/p' <<<"$depends")
    dpkg --compare-versions "$(dpkg-query -W -f='${Version}' libc6)" ge "$libc_min" ||
        fail "the .deb asks for libc6 $libc_min, newer than this machine's"
    said=$("$bin" --version) || fail "the .deb's client did not print its version"
    case "$said" in "openchime "*" (protocol "*) ;; *) fail "the .deb's client said '$said' for its version" ;; esac
    ok "the .deb's dependencies are on this machine, and its client says: $said"
else
    echo "$TEST: the $arch client on $(uname -m); read, not run"
fi

# --- The Flatpak ------------------------------------------------------------------
repo="$v/repo"
ostree --repo="$repo" init --mode=archive >/dev/null
# flatpak 1.14 prints GLib criticals importing a bundle into a repository with
# no remotes, and imports it all the same: what it says is shown only when the
# import fails, and the import is judged by the commit it leaves below.
out=$(flatpak build-import-bundle --no-update-summary "$repo" "$bundle" 2>&1) || fail "the bundle does not import into a repository: $out"
ref="app/$app/$arch/stable"
ostree --repo="$repo" rev-parse "$ref" >/dev/null 2>&1 || fail "the bundle is not $ref: $(ostree --repo="$repo" refs | tr '\n' ' ')"
meta=$(ostree --repo="$repo" cat "$ref" /metadata)
# value <group> <key>: one key of the metadata, as the keyfile has it.
value() { awk -v g="[$1]" -v k="$2" '/^\[/{in_g=($0==g)} in_g && index($0, k"=")==1 {print substr($0, length(k)+2)}' <<<"$meta"; }
[ "$(value Application name)" = "$app" ] || fail "the Flatpak's metadata names '$(value Application name)', not $app"
want_runtime="org.freedesktop.Platform/$arch/$FREEDESKTOP_BRANCH"
[ "$(value Application runtime)" = "$want_runtime" ] || fail "the Flatpak runs on '$(value Application runtime)', not the pinned $want_runtime"
[ "$(value Application command)" = openchime ] || fail "the Flatpak's command is '$(value Application command)', not openchime"
# has <group> <key> <item>: <item> is one of the key's ;-separated values.
has() { local vals; vals=$(value "$1" "$2"); grep -qx -- "$3" <<<"${vals//;/$'\n'}"; }
for s in wayland fallback-x11; do has Context sockets "$s" || fail "the Flatpak's sandbox has no $s socket"; done
for s in ipc network; do has Context shared "$s" || fail "the Flatpak's sandbox does not share $s"; done
has Context devices dri || fail "the Flatpak's sandbox has no GPU (devices=dri)"
has Context filesystems xdg-run/pipewire-0 || fail "the Flatpak's sandbox cannot reach PipeWire (xdg-run/pipewire-0)"
for n in org.freedesktop.portal.Desktop org.freedesktop.Notifications; do
    [ "$(value 'Session Bus Policy' "$n")" = talk ] || fail "the Flatpak cannot talk to $n on the session bus"
done
for f in "export/share/applications/$app.desktop" "files/share/metainfo/$app.metainfo.xml" \
         "export/share/icons/hicolor/256x256/apps/$app.png"; do
    ostree --repo="$repo" ls "$ref" "/$f" >/dev/null 2>&1 || fail "the Flatpak has no /$f"
done
ostree --repo="$repo" cat "$ref" "/export/share/applications/$app.desktop" > "$v/$app.desktop"
desktop-file-validate "$v/$app.desktop" || fail "the Flatpak's exported $app.desktop does not validate"
ostree --repo="$repo" cat "$ref" /files/bin/openchime > "$v/flatpak-openchime"
machine "$v/flatpak-openchime" "the Flatpak's client"
runtime="third_party/flatpak-freedesktop-$FREEDESKTOP_BRANCH/runtime/org.freedesktop.Platform/$arch/$FREEDESKTOP_BRANCH/active/files"
[ -d "$runtime" ] || fail "the runtime is not at $runtime (scripts/build/freedesktop_sdk.sh $arch)"
for lib in $(needed "$v/flatpak-openchime"); do
    [ -e "$runtime/lib/$arch-linux-gnu/$lib" ] || [ -e "$runtime/lib/$lib" ] ||
        fail "the Flatpak's client links $lib, which org.freedesktop.Platform $FREEDESKTOP_BRANCH does not hold"
done
ok "the Flatpak is $ref on $want_runtime, its $arch client's libraries in the runtime, with the sandbox it needs"
