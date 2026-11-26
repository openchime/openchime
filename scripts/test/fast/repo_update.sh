#!/usr/bin/env bash
# scripts/test/fast/repo_update.sh
#
# A release's apt and rpm repositories take the daemon's and the Linux
# client's packages side by side (scripts/release/repo_update.sh). On a
# local-path rclone remote, a source bucket and its mirror, with a throwaway
# signing key and packages built here that hold nothing but their names:
#   1. a first release of `openchimed` alone is published to empty prefixes;
#   2. a second release adds `openchimed` and the first `openchime`, and a
#      third adds one more version of both; after each, `expect` counts one
#      more version per package name per architecture, and `verify` reads
#      each bucket back to that count;
#   3. the full method, asked for, makes the same index from the same
#      repository as the incremental one;
#   4. the third release prepared again, the same files, is a re-run and is
#      accepted; a different package carrying a version already published,
#      under its file name or under another, is refused.
# The rpm half needs createrepo_c; without it that half is a SKIP (a failure
# where CI is set), and the apt half runs regardless.
#
# Needs: rclone, gpg, dpkg-deb, apt-ftparchive (apt-utils), rpmbuild, rpm,
# rpmsign, createrepo_c (createrepo-c), python3.
# Run by: scripts/test/fast/source.sh (make test; make test-all).
. "$(dirname "$0")/../lib/common.sh"
need rclone "rclone"
need gpg "gnupg"
need dpkg-deb "dpkg"
need apt-ftparchive "apt-utils"
need rpmbuild "rpm"
need rpmsign "rpm"
root=$(pwd)
tool="$root/scripts/release/repo_update.sh"
tmp=$(mktemp -d)
export GNUPGHOME="$tmp/gnupg"
# The agent the key starts is stopped with the test: nothing is left running.
trap 'gpgconf --kill all >/dev/null 2>&1 || true; rm -rf "$tmp"' EXIT
mkdir -m 700 "$GNUPGHOME"
: > "$tmp/rclone.conf"
export RCLONE_CONFIG="$tmp/rclone.conf"

kinds="apt rpm"
if ! command -v createrepo_c >/dev/null 2>&1; then
    [ -z "${CI:-}" ] || fail "createrepo_c is missing (createrepo-c); the rpm half cannot run"
    echo "$TEST: SKIP the rpm half: no createrepo_c (createrepo-c)"
    kinds="apt"
fi

gpg --batch --quiet --passphrase '' --quick-gen-key "Repository test <test@example.invalid>" rsa2048 sign never 2>/dev/null \
    || fail "cannot make a throwaway key"
GPG_KEY_ID=$(gpg --batch --list-keys --with-colons 2>/dev/null | awk -F: '/^fpr:/ { print $10; exit }')
export GPG_KEY_ID

# deb <name> <version> <arch> [note]: a package holding nothing but its
# control file; a note makes another package of the same name and version.
deb() {
    local d="$tmp/src/$1-$2-$3${4:-}" note="${4:-}"
    mkdir -p "$d/DEBIAN" "$tmp/pkg$note"
    printf 'Package: %s\nVersion: %s\nArchitecture: %s\nMaintainer: Test <test@example.invalid>\nDescription: fixture%s\n' \
        "$1" "$2" "$3" "$note" > "$d/DEBIAN/control"
    dpkg-deb --root-owner-group -Zgzip -b "$d" "$tmp/pkg$note/${1}_${2}_$3.deb" >/dev/null || fail "cannot build the $1 $2 $3 deb"
}

# rpm <name> <version> <arch> [note]: the same, as an rpm, release 1.
rpm_fixture() {
    local top="$tmp/rpmbuild-$1-$2-$3${4:-}" note="${4:-}"
    mkdir -p "$top" "$tmp/pkg$note"
    cat > "$top/f.spec" <<EOF
Name: $1
Version: $2
Release: 1
Summary: fixture$note
License: MIT
%description
fixture$note
%files
EOF
    rpmbuild -bb --target "$3" --define "_topdir $top" --define "_rpmdir $top/out" \
        --define "debug_package %{nil}" "$top/f.spec" >/dev/null 2>&1 || fail "cannot build the $1 $2 $3 rpm"
    cp "$top/out/$3/$1-$2-1.$3.rpm" "$tmp/pkg$note/"
}

# release <version> <name>...: each named package of <version> for both
# architectures, in both formats.
release() {
    local v="$1" n; shift
    for n in "$@"; do
        deb "$n" "$v" amd64; deb "$n" "$v" arm64
        case " $kinds " in *" rpm "*) rpm_fixture "$n" "$v" x86_64; rpm_fixture "$n" "$v" aarch64 ;; esac
    done
}

files() {   # <kind> <version>: the packages of a version
    if [ "$1" = apt ]; then ls "$tmp/pkg"/*_"$2"_*.deb; else ls "$tmp/pkg"/*-"$2"-1.*.rpm; fi
}

src="$tmp/bucket/dist"
mirror="$tmp/bucket/mirror"

# prepare <kind> <version> <prior> <work> <package>...: 0 if prepare passes,
# its output kept in <work>.log.
prepare() {
    local kind="$1" v="$2" prior="$3" w="$4"; shift 4
    VERSION="$v" PRIOR="$prior" "$tool" prepare "$kind" "$w" "$src/$kind" "$mirror/$kind" "$@" > "$w.log" 2>&1
}

# publish <kind> <version> <prior> <expect> <bucket>... -- <package>...:
# prepare, then upload to each bucket and verify it.
publish() {
    local kind="$1" v="$2" prior="$3" want="$4" w="$tmp/work-$1-$2" b
    local -a buckets=()
    shift 4
    while [ "$1" != -- ]; do buckets+=("$1"); shift; done
    shift
    prepare "$kind" "$v" "$prior" "$w" "$@" || { cat "$w.log" >&2; fail "$kind release $v: prepare refused"; }
    [ "$(cat "$w/expect")" = "$want" ] || fail "$kind release $v: expect is '$(cat "$w/expect")', not '$want'"
    for b in "${buckets[@]}"; do
        "$tool" upload "$kind" "$w" "$b/$kind" > "$w.up.log" 2>&1 \
            || { cat "$w.up.log" >&2; fail "$kind release $v: upload to $b failed"; }
        VERSION="$v" "$tool" verify "$kind" "$b/$kind" "$want" > "$w.verify.log" 2>&1 \
            || { cat "$w.verify.log" >&2; fail "$kind release $v: verify of $b refused '$want'"; }
    done
}

# refused <kind> <work> <published file> <package>: prepare refuses <package>,
# naming the published file it clashes with.
refused() {
    prepare "$1" 3 2 "$2" "$4" && fail "$1: $4 was taken, though $3 carries its name and version"
    grep -qF "$3" "$2.log" || { cat "$2.log" >&2; fail "$1: $4 was refused, but not for clashing with $3"; }
}

# Per kind, the index's architecture names.
arches() { if [ "$1" = apt ]; then echo "amd64 arm64"; else echo "aarch64 x86_64"; fi; }
want() {   # <kind> <openchime count> <openchimed count>
    local a out=""
    if [ "$2" -gt 0 ]; then for a in $(arches "$1"); do out+="openchime:$a=$2 "; done; fi
    for a in $(arches "$1"); do out+="openchimed:$a=$3 "; done
    echo "${out% }"
}

release 1 openchimed
release 2 openchimed openchime
release 3 openchimed openchime

# Each rclone call is most of the time this takes, so each step publishes to
# the buckets it needs and no more: the first to the source alone, which the
# second's prepare then copies to the mirror.
for kind in $kinds; do
    # 1 and 2: the daemon alone, as the repositories hold it before the
    # client; then the client's first release beside the daemon's second.
    mapfile -t p < <(files "$kind" 1)
    publish "$kind" 1 0 "$(want "$kind" 0 1)" "$src" -- "${p[@]}"
    mapfile -t p < <(files "$kind" 2)
    # The full method, over the same repository, makes the same index.
    REPO_UPDATE_MODE=full prepare "$kind" 2 1 "$tmp/full-$kind" "${p[@]}" \
        || { cat "$tmp/full-$kind.log" >&2; fail "$kind release 2: the full method refused"; }
    [ "$(cat "$tmp/full-$kind/expect")" = "$(want "$kind" 1 2)" ] \
        || fail "$kind release 2, full method: expect is '$(cat "$tmp/full-$kind/expect")'"
    publish "$kind" 2 1 "$(want "$kind" 1 2)" "$src" "$mirror" -- "${p[@]}"

    # 3: one release of each takes a second of both.
    mapfile -t p < <(files "$kind" 3)
    publish "$kind" 3 2 "$(want "$kind" 2 3)" "$src" -- "${p[@]}"

    # 4: the same files again are a re-run, and are taken; a different
    # package of a published version is refused, under its file name or, for
    # apt, under another.
    prepare "$kind" 3 2 "$tmp/rerun-$kind" "${p[@]}" \
        || { cat "$tmp/rerun-$kind.log" >&2; fail "$kind: a re-run of release 3 with its own packages was refused"; }
    if [ "$kind" = apt ]; then
        deb openchime 3 amd64 -other
        # apt-ftparchive --arch takes only a deb named *_<arch>.deb.
        cp "$tmp/pkg-other/openchime_3_amd64.deb" "$tmp/pkg-other/openchime_3+rebuilt_amd64.deb"
        refused apt "$tmp/dup-renamed" openchime_3_amd64.deb "$tmp/pkg-other/openchime_3+rebuilt_amd64.deb"
        refused apt "$tmp/dup-apt" openchime_3_amd64.deb "$tmp/pkg-other/openchime_3_amd64.deb"
    else
        rpm_fixture openchime 3 x86_64 -other
        refused rpm "$tmp/dup-rpm" openchime-3-1.x86_64.rpm "$tmp/pkg-other/openchime-3-1.x86_64.rpm"
    fi
done
ok "$kinds: one version more per package name per architecture, the full method alike, a second copy of a version refused"
