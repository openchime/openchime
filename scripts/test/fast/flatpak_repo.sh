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
#      architecture's ref at the release's commit, each commit verifies
#      against the key as a client verifies it (OSTree pulling its detached
#      metadata from the bucket), and the .flatpakrepo and .flatpakref name
#      the repository;
#   2. the second release prepared again, the same bundles, is a re-run and
#      is taken; a different bundle of the published version is refused;
#   3. `verify` refuses a published commit whose .commitmeta is removed, and
#      one whose .commitmeta is replaced by another key's signature;
#   4. `resign` of the signed repository uploads no new object; a repository
#      published with unsigned commits (built here as releases were before
#      commits were signed) is `resign`ed, uploaded and verified, its refs at
#      the same commits and each commit verifying against the key;
#   5. `prepare` given a key that cannot sign (its secret half deleted)
#      refuses, and writes nothing;
#   6. a summary changed in the bucket is refused, by `verify` and by
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
# OSTree checks a signature in a keyring it makes under TMPDIR, and may leave
# that keyring's agent running after it removes the keyring.
export TMPDIR="$tmp"
# The agents the keys start are stopped with the test: nothing is left running.
trap 'gpgconf --kill all >/dev/null 2>&1 || true; pkill -f "^gpg-agent --homedir $tmp/" || true; rm -rf "$tmp"' EXIT
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
gpg --batch --export "$GPG_KEY_ID" > "$tmp/key.gpg"
# Another key: a signature not the repository's, then a key that cannot sign.
gpg --batch --quiet --passphrase '' --quick-gen-key "Other test <other@example.invalid>" ed25519 sign never 2>/dev/null \
    || fail "cannot make a second throwaway key"
OTHER_KEY_ID=$(gpg --batch --list-keys --with-colons 2>/dev/null | awk -F: '/^fpr:/ { print $10 }' | grep -vxF "$GPG_KEY_ID")

# signed <repo> <commit>: the commit verifies against the key as a client
# verifies it, OSTree pulling it and its detached metadata with gpg-verify on.
signed() {
    rm -rf "$tmp/check"
    ostree --repo="$tmp/check" init --mode=archive-z2
    ostree --repo="$tmp/check" remote add --set=gpg-verify=true --set=gpg-verify-summary=false \
        --gpg-import="$tmp/key.gpg" pub "file://$1" >/dev/null
    ostree --repo="$tmp/check" pull --commit-metadata-only pub "$2" > "$tmp/check.log" 2>&1
}

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
    for c in $want; do
        signed "$bucket" "${c#*=}" || { cat "$tmp/check.log" >&2; fail "release $v: ${c%%=*}'s commit does not verify against the key"; }
    done
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

# 3. A missing or another key's signature on a published commit is refused
# by `verify`, on copies of the bucket.
c=$(sed 's/.*x86_64=//' "$tmp/work-2/expect")
meta="objects/${c:0:2}/${c:2}.commitmeta"
cp -r "$bucket" "$tmp/nosig"
rm "$tmp/nosig/$meta"
VERSION=2 "$tool" verify "$tmp/nosig" "$(cat "$tmp/work-2/expect")" > "$tmp/nosig.log" 2>&1 \
    && fail "verify took a commit with no .commitmeta"
grep -qF "$c is not signed" "$tmp/nosig.log" || { cat "$tmp/nosig.log" >&2; fail "verify refused a commit with no .commitmeta, but not for its signature"; }
cp -r "$bucket" "$tmp/othersig"
ostree --repo="$tmp/other" init --mode=archive-z2
mkdir -p "$tmp/other/objects/${c:0:2}"
cp "$bucket/objects/${c:0:2}/${c:2}.commit" "$tmp/other/objects/${c:0:2}/"
ostree --repo="$tmp/other" gpg-sign --gpg-homedir="$GNUPGHOME" "$c" "$OTHER_KEY_ID" || fail "cannot sign with the other key"
cp "$tmp/other/$meta" "$tmp/othersig/$meta"
VERSION=2 "$tool" verify "$tmp/othersig" "$(cat "$tmp/work-2/expect")" > "$tmp/othersig.log" 2>&1 \
    && fail "verify took a commit signed with another key"
grep -qF "$c has no valid signature" "$tmp/othersig.log" || { cat "$tmp/othersig.log" >&2; fail "verify refused a commit signed with another key, but not for its signature"; }

# 4. resign: of a signed repository, no new object; of one published with
# unsigned commits, every commit signed and none moved.
find "$bucket/objects" -type f | sort > "$tmp/objects.before"
"$tool" resign "$tmp/resign-signed" "$bucket" > "$tmp/resign-signed.log" 2>&1 \
    || { cat "$tmp/resign-signed.log" >&2; fail "resign of a signed repository refused"; }
[ "$(cat "$tmp/resign-signed/expect")" = "$(cat "$tmp/work-2/expect")" ] || fail "resign of a signed repository moved its refs"
grep -qF "0 commit(s) signed" "$tmp/resign-signed.log" || { cat "$tmp/resign-signed.log" >&2; fail "resign signed a commit already signed"; }
"$tool" upload "$tmp/resign-signed" "$bucket" > "$tmp/resign-signed.up.log" 2>&1 \
    || { cat "$tmp/resign-signed.up.log" >&2; fail "upload of a resign failed"; }
find "$bucket/objects" -type f | sort | diff "$tmp/objects.before" - > "$tmp/objects.diff" \
    || { cat "$tmp/objects.diff" >&2; fail "resign of a signed repository uploaded new objects"; }
unsigned="$tmp/unsigned/flatpak"
mkdir -p "$unsigned" && ostree --repo="$unsigned" init --mode=archive-z2
for a in aarch64 x86_64; do
    flatpak build-import-bundle --no-update-summary "$unsigned" "$tmp/client-1-$a.flatpak" > "$tmp/old-way.log" 2>&1 \
        || { cat "$tmp/old-way.log" >&2; fail "cannot import the $a bundle the old way"; }
done
flatpak build-update-repo --no-update-appstream --gpg-sign="$GPG_KEY_ID" --gpg-homedir="$GNUPGHOME" "$unsigned" > "$tmp/old-way.log" 2>&1 \
    || { cat "$tmp/old-way.log" >&2; fail "cannot sign the old way's summary"; }
cp "$tmp/work-1/repo/io.openchime.OpenChime.flatpakrepo" "$tmp/work-1/repo/io.openchime.OpenChime.flatpakref" "$unsigned/"
old_refs=$(cat "$unsigned"/refs/heads/app/io.openchime.OpenChime/*/stable)
"$tool" resign "$tmp/resign" "$unsigned" > "$tmp/resign.log" 2>&1 \
    || { cat "$tmp/resign.log" >&2; fail "resign of an unsigned repository refused"; }
[ "$(cat "$tmp/resign/expect")" = "$(cat "$tmp/work-1/expect")" ] \
    || fail "resign moved the refs: '$(cat "$tmp/resign/expect")', not '$(cat "$tmp/work-1/expect")'"
"$tool" upload "$tmp/resign" "$unsigned" > "$tmp/resign.up.log" 2>&1 || { cat "$tmp/resign.up.log" >&2; fail "upload of a resign failed"; }
VERSION=1 "$tool" verify "$unsigned" "$(cat "$tmp/work-1/expect")" > "$tmp/resign.verify.log" 2>&1 \
    || { cat "$tmp/resign.verify.log" >&2; fail "verify refused the resigned repository"; }
[ "$(cat "$unsigned"/refs/heads/app/io.openchime.OpenChime/*/stable)" = "$old_refs" ] || fail "the resigned repository's ref files moved"
for c in $(cat "$tmp/work-1/expect"); do
    signed "$unsigned" "${c#*=}" || { cat "$tmp/check.log" >&2; fail "the resigned ${c%%=*} commit does not verify against the key"; }
done

# 5. A key that cannot sign: its public half alone.
gpg --batch --yes --delete-secret-keys "$OTHER_KEY_ID" 2>/dev/null || fail "cannot delete the other key's secret half"
GPG_KEY_ID="$OTHER_KEY_ID" VERSION=1 "$tool" prepare "$tmp/nokey" "$tmp/nokey-bucket/flatpak" \
    "$tmp/client-1-aarch64.flatpak" "$tmp/client-1-x86_64.flatpak" > "$tmp/nokey.log" 2>&1 \
    && fail "prepare took a key that cannot sign"
grep -qF "cannot sign app/io.openchime.OpenChime/" "$tmp/nokey.log" || { cat "$tmp/nokey.log" >&2; fail "prepare refused a key that cannot sign, but not for signing a commit"; }
[ ! -e "$tmp/nokey-bucket" ] || fail "prepare with a key that cannot sign wrote to the bucket"

# 6. The summary changed in the bucket: one byte of a ref's name.
python3 -I -c 'import sys; p = sys.argv[1]; b = bytearray(open(p, "rb").read()); i = b.find(b"stable"); b[i] = ord("S"); open(p, "wb").write(b)' \
    "$bucket/summary"
VERSION=2 "$tool" verify "$bucket" "$(cat "$tmp/work-2/expect")" > "$tmp/tampered.log" 2>&1 \
    && fail "verify took a summary changed after it was signed"
VERSION=2 "$tool" prepare "$tmp/tampered" "$bucket" "$tmp/client-2-aarch64.flatpak" "$tmp/client-2-x86_64.flatpak" > "$tmp/tampered.log" 2>&1 \
    && fail "prepare built on a summary changed after it was signed"
ok "two releases in a row, each summary and commit signed and at the release's commits; a re-run taken; an unsigned repository resigned at its commits, a signed one given no new object; a second build of a version, a missing or foreign commit signature, a key that cannot sign and a changed summary refused"
