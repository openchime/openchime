#!/usr/bin/env bash
# scripts/test/fast/flatpak_repo.sh
#
# The Flatpak repository takes a release's bundles (scripts/release/
# flatpak_repo.sh). On a local-path rclone remote, with a throwaway signing
# key and bundles of a stand-in application built here (its metadata written
# by hand, so no runtime is needed: `flatpak build-finish`, `build-export`
# and `build-bundle` only arrange files), for x86_64 and aarch64:
#   1. a first release is published to an empty prefix, and a second over
#      it; after each, `verify` reads both summaries back, signed, with each
#      architecture's ref at the release's commit, and the .flatpakrepo and
#      .flatpakref name the repository;
#   2. the second release prepared again, the same bundles, is a re-run and
#      is taken; a different bundle of the published version is refused;
#   3. a summary changed in the bucket is refused, by `verify` and by
#      `prepare`, where the same bucket was taken before the change.
#
# Needs: flatpak, ostree, rclone, gpg.
# Run by: scripts/test/fast/source.sh (make test; make test-all).
. "$(dirname "$0")/../lib/common.sh"
need flatpak "flatpak"
need ostree "ostree"
need rclone "rclone"
need gpg "gnupg"
tool="$PWD/scripts/release/flatpak_repo.sh"
tmp=$(mktemp -d)
export GNUPGHOME="$tmp/gnupg"
# The agent the key starts is stopped with the test: nothing is left running.
trap 'gpgconf --kill all >/dev/null 2>&1 || true; rm -rf "$tmp"' EXIT
mkdir -m 700 "$GNUPGHOME"
: > "$tmp/rclone.conf"
export RCLONE_CONFIG="$tmp/rclone.conf"
# No installation of the user's or the machine's is read or written.
export FLATPAK_USER_DIR="$tmp/flatpak-user"
export DIST_BASE_URL="https://dist.example.invalid"

gpg --batch --quiet --passphrase '' --quick-gen-key "Repository test <test@example.invalid>" rsa2048 sign never 2>/dev/null \
    || fail "cannot make a throwaway key"
GPG_KEY_ID=$(gpg --batch --list-keys --with-colons 2>/dev/null | awk -F: '/^fpr:/ { print $10; exit }')
export GPG_KEY_ID

# bundle <version> <arch> [note]: the stand-in client of <version>, exported
# to a build repository of its own, as a release-candidate's build is, and
# bundled; a note makes another build of the same version. Prints the commit.
bundle() {
    local v="$1" a="$2" note="${3:-}" app="$tmp/app-$1-$2${3:-}" build="$tmp/build-$1${3:-}"
    mkdir -p "$app/files/bin"
    printf '[Application]\nname=io.openchime.OpenChime\nruntime=org.freedesktop.Platform/%s/26.08\nsdk=org.freedesktop.Sdk/%s/26.08\n' \
        "$a" "$a" > "$app/metadata"
    printf '#!/bin/sh\necho %s%s\n' "$v" "$note" > "$app/files/bin/openchime"
    chmod +x "$app/files/bin/openchime"
    { flatpak build-finish --command=openchime --share=network "$app" &&
      flatpak build-export --arch="$a" --subject="OpenChime $v" "$build" "$app" stable &&
      flatpak build-bundle --arch="$a" "$build" "$tmp/client-$v-$a$note.flatpak" io.openchime.OpenChime stable
    } > "$tmp/bundle.log" 2>&1 || { cat "$tmp/bundle.log" >&2; fail "cannot build the $v $a bundle"; }
    ostree --repo="$build" rev-parse "app/io.openchime.OpenChime/$a/stable"
}

bucket="$tmp/bucket/flatpak"

# release <version>: prepare, upload, verify; the expect line is each
# architecture's ref at the commit its bundle holds.
release() {
    local v="$1" w="$tmp/work-$1" want
    want="aarch64=$(bundle "$v" aarch64) x86_64=$(bundle "$v" x86_64)"
    VERSION="$v" "$tool" prepare "$w" "$bucket" "$tmp/client-$v-aarch64.flatpak" "$tmp/client-$v-x86_64.flatpak" > "$w.log" 2>&1 \
        || { cat "$w.log" >&2; fail "release $v: prepare refused"; }
    [ "$(cat "$w/expect")" = "$want" ] || fail "release $v: expect is '$(cat "$w/expect")', not '$want'"
    "$tool" upload "$w" "$bucket" > "$w.up.log" 2>&1 || { cat "$w.up.log" >&2; fail "release $v: upload failed"; }
    VERSION="$v" "$tool" verify "$bucket" "$want" > "$w.verify.log" 2>&1 \
        || { cat "$w.verify.log" >&2; fail "release $v: verify refused '$want'"; }
    for f in io.openchime.OpenChime.flatpakrepo io.openchime.OpenChime.flatpakref; do
        grep -qxF "Url=$DIST_BASE_URL/flatpak/" "$bucket/$f" || fail "release $v: $f does not name the repository"
        grep -qxF "GPGKey=$(gpg --batch --export "$GPG_KEY_ID" | base64 -w0)" "$bucket/$f" || fail "release $v: $f does not carry the key"
    done
}

# 1. Two releases in a row, the first to an empty prefix.
release 1
release 2
# Nothing is deleted: the first release's commits are still there.
for c in $(cat "$tmp/work-1/expect"); do
    c=${c#*=}
    [ -f "$bucket/objects/${c:0:2}/${c:2}.commit" ] || fail "release 1's commit $c is gone"
done

# 2. A re-run is taken; another build of version 2 is refused.
VERSION=2 "$tool" prepare "$tmp/rerun" "$bucket" "$tmp/client-2-aarch64.flatpak" "$tmp/client-2-x86_64.flatpak" > "$tmp/rerun.log" 2>&1 \
    || { cat "$tmp/rerun.log" >&2; fail "a re-run of release 2 with its own bundles was refused"; }
other=$(bundle 2 x86_64 -other)
VERSION=2 "$tool" prepare "$tmp/dup" "$bucket" "$tmp/client-2-aarch64.flatpak" "$tmp/client-2-x86_64-other.flatpak" > "$tmp/dup.log" 2>&1 \
    && fail "another build of version 2 ($other) was taken over the published one"
grep -qF "$other" "$tmp/dup.log" || { cat "$tmp/dup.log" >&2; fail "another build of version 2 was refused, but not for clashing with the published one"; }

# 3. The summary changed in the bucket: one byte of a ref's name.
python3 -I -c 'import sys; p = sys.argv[1]; b = bytearray(open(p, "rb").read()); i = b.find(b"stable"); b[i] = ord("S"); open(p, "wb").write(b)' \
    "$bucket/summary"
VERSION=2 "$tool" verify "$bucket" "$(cat "$tmp/work-2/expect")" > "$tmp/tampered.log" 2>&1 \
    && fail "verify took a summary changed after it was signed"
VERSION=2 "$tool" prepare "$tmp/tampered" "$bucket" "$tmp/client-2-aarch64.flatpak" "$tmp/client-2-x86_64.flatpak" > "$tmp/tampered.log" 2>&1 \
    && fail "prepare built on a summary changed after it was signed"
ok "two releases in a row, each summary signed and at the release's commits; a re-run taken, a second build of a version and a changed summary refused"
