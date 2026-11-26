#!/usr/bin/env bash
# Add a release's Flatpak bundles to the Flatpak repository in object storage,
# downloading only its summary, config, refs and the commit objects the refs
# name, never the objects behind them (ARCH-20). The model is repo_update.sh:
# the same verbs, the same rule that nothing is deleted.
#
#   flatpak_repo.sh prepare <work> <source> <bundle>...
#   flatpak_repo.sh resign  <work> <source>
#   flatpak_repo.sh upload  <work> <dest>
#   flatpak_repo.sh verify  <dest> <expect>
#
# <source> and <dest> are rclone paths to the repository's prefix
# (dist:<bucket>/flatpak). Any rclone remote serves, a local-path one
# included, which is how this is tested off line.
#
# The repository is an OSTree archive, as `flatpak build-export` makes one,
# holding app/io.openchime.OpenChime/<arch>/stable for x86_64 and aarch64.
# Its objects are named by their content, so a release only ever adds files;
# what changes is the ref files, the summary that lists the refs and their
# commits, and its signatures. Flatpak reads the indexed summary (summary.idx,
# summaries/), OSTree and older Flatpak the plain one (summary); both are
# signed with the repository signing key, the apt and rpm repositories' key.
# Each commit is signed with the same key, in its detached metadata
# (<commit>.commitmeta, beside the commit object; signing adds it and leaves
# the commit's ID as it was): the .flatpakrepo and .flatpakref carry the key,
# so a remote added from them verifies every commit it pulls, and refuses one
# with no signature from the key.
#
# `prepare` reads the source, writes nothing to it, and builds in
# <work>/repo exactly what is to be uploaded: the published refs at their
# commits (the commit objects alone, with their signatures), each bundle
# imported (`flatpak build-import-bundle`) and its commit signed
# (`flatpak build-sign`), and the summary regenerated and signed
# (`flatpak build-update-repo --gpg-sign`), with
# io.openchime.OpenChime.flatpakrepo and .flatpakref beside it. It prints,
# and writes to <work>/expect, the commit each architecture's ref is at once
# it is published ("aarch64=<commit> x86_64=<commit>"). No static deltas: a
# delta from a release's parent needs the parent's objects, which are not
# fetched, and the client is small enough to pull object by object.
# `resign` repairs a published repository whose commits are not signed, as
# one published before commits were signed is: it fetches what `prepare`
# fetches, signs each commit the summary lists that has no valid signature
# from the key, and regenerates and signs the summary, the refs at the same
# commits; <work>/expect names them, and its output is uploaded and verified
# as a release's is (VERSION the release the refs are at). Over a repository
# already signed it adds no object.
# `upload` copies <work>/repo to one bucket: the objects and the indexed
# summary's parts first, then the refs, then the summaries, their signatures
# last, so no client ever reads a summary naming a commit not yet there.
# `rclone copy`, never sync: nothing is ever deleted. `verify` reads the
# published summaries back, checks their signatures, and checks the refs are
# at <expect>'s commits, each commit there, this release's (VERSION), and
# signed with the key.
#
# The witnesses `prepare` refuses on, before anything is written:
#   - the listing must succeed: only rclone's "directory not found" reads as
#     an empty prefix, and an empty prefix is the first publish;
#   - a prefix that is not empty holds a summary, its signature and a config:
#     objects with no summary are a publish that did not finish, or a fetch
#     that did not happen;
#   - the published summary's signature verifies against the signing key;
#   - each ref the summary lists has its commit object in the prefix, and its
#     ref file names that commit, or this release's (an earlier attempt that
#     uploaded the refs and failed before the summary);
#   - there is exactly one bundle per architecture, each holding
#     app/io.openchime.OpenChime/<arch>/stable at a commit whose subject is
#     this release's ("OpenChime <VERSION>", as packaging/build-client-flatpak.sh
#     exports it);
#   - a ref the summary has at another commit of this same version is a
#     second copy of the version, and the release stops; at this release's
#     commit, it is a re-run, and left as it is;
#   - the new summary, read back with the key, lists every ref the old one
#     did, and this release's at its commits;
#   - each commit the new summary lists has a valid signature from the key
#     (one left unsigned by an earlier publish is `resign`'s to repair).
# `resign` refuses on the same witnesses of the published repository, and
# when the new summary does not list every ref at the commit the old one did.
#
# Environment: VERSION (the release number), GPG_KEY_ID (the imported
# REPO_SIGNING_KEY; the keyring is gpg's own, GNUPGHOME if set), and for
# `prepare` DIST_BASE_URL, the published base: the repository is served at
# ${DIST_BASE_URL}/flatpak/, and the .flatpakrepo and .flatpakref name it.
set -euo pipefail
export LC_ALL=C

APP_ID=io.openchime.OpenChime
BRANCH=stable
ARCHES="aarch64 x86_64"
# What a client fetches to read the repository; uploaded last, in this order.
SUMMARY="summary summary.idx"
SIGS="summary.sig summary.idx.sig"

die()  { echo "::error::flatpak_repo: $*" >&2; exit 1; }
note() { echo "flatpak_repo: $*" >&2; }

# The listing of every object under a prefix, as repo_update.sh's: a failed
# listing is never read as an empty one; rclone's exit 3, "directory not
# found", from a local path, is.
list_all() {   # <rclone path> <out file>
  local rc=0
  rclone lsf -R --files-only --fast-list "$1" > "$2" 2>"$2.err" || rc=$?
  if [ "$rc" -eq 3 ]; then
    : > "$2"
  elif [ "$rc" -ne 0 ]; then
    cat "$2.err" >&2
    die "cannot list $1 (rclone exit $rc); refusing to build a summary from an unread repository"
  fi
  rm -f "$2.err"
  sort -o "$2" "$2"
}

# The signing key's public half, which every check below reads a summary with.
public_key() {   # <out file>
  gpg --batch --export "$GPG_KEY_ID" > "$1"
  [ -s "$1" ] || die "no public key for $GPG_KEY_ID in the keyring"
}

# The refs a repository's plain summary lists, "ref<TAB>commit" sorted, read
# with its signature checked: an OSTree remote of a scratch repository, at
# file://<dir>, verifying the summary against <key> alone.
summary_refs() {   # <dir holding summary and summary.sig> <key> <scratch> <out>
  local dir="$1" key="$2" s="$3"
  rm -rf "$s"
  ostree --repo="$s" init --mode=archive-z2
  ostree --repo="$s" remote add --set=gpg-verify-summary=true --set=gpg-verify=false \
    --gpg-import="$key" pub "file://$dir" >/dev/null
  ostree --repo="$s" remote refs --revision pub > "$4.raw" 2>"$4.err" || {
    cat "$4.err" >&2; return 1; }
  sed 's/^pub://' "$4.raw" | sort > "$4"
  rm -f "$4.raw" "$4.err"
}

# The same refs read the way Flatpak 1.9 and later reads them, from the
# indexed summary and its signature, in a Flatpak installation of its own.
# Flatpak lists one architecture at a time, and a commit shortened.
indexed_refs() {   # <dir> <key> <scratch> <out>
  local a
  rm -rf "$3"
  FLATPAK_USER_DIR="$3" flatpak remote-add --user --gpg-import="$2" pub "file://$1" 2>"$4.err" \
    || { cat "$4.err" >&2; return 1; }
  for a in $ARCHES; do
    FLATPAK_USER_DIR="$3" flatpak remote-ls --user --arch="$a" --columns=ref,commit pub 2>"$4.err" \
      || { cat "$4.err" >&2; return 1; }
  done | sort > "$4"
  rm -f "$4.err"
}

# The subject of a commit: the first line after the header ostree shows.
subject() {   # <repo> <commit>
  ostree --repo="$1" show "$2" | awk 'body && NF { sub(/^ +/, ""); print; exit } /^$/ { body = 1 }'
}

obj() { echo "objects/${1:0:2}/${1:2}.${2:-commit}"; }   # <checksum> [kind]: its object's path

# The objects of a commit's root directory, which `flatpak build-update-repo`
# reads for every ref it lists: the root's dirtree and dirmeta checksums, the
# last two fields of the commit, (a{sv}aya(say)sstayay). The last is 32 bytes
# between the end of the one before it and the tuple's framing offsets: six
# of them, one per variable-sized field but the last, little-endian, as wide
# as the size of the whole needs.
root_objects() {   # <commit object file>
  python3 -I - "$1" <<'PY'
import sys
b = open(sys.argv[1], "rb").read()
n = len(b)
w = 1 if n < 1 << 8 else 2 if n < 1 << 16 else 4 if n < 1 << 32 else 8
end = int.from_bytes(b[n - 6 * w : n - 5 * w], "little")
tree, meta = b[end - 32 : end], b[end : n - 6 * w]
if len(meta) != 32 or end < 32:
    sys.exit(f"flatpak_repo: {sys.argv[1]} is not a commit object")
print(f"objects/{tree.hex()[:2]}/{tree.hex()[2:]}.dirtree")
print(f"objects/{meta.hex()[:2]}/{meta.hex()[2:]}.dirmeta")
PY
}

# The commits of a repository, checked as a client checks them before it
# takes one: a scratch repository's OSTree remote at file://<repo>, verifying
# commits against <key> alone, which pulls a commit and its detached metadata
# (<commit>.commitmeta) and nothing below them.
sig_remote() {   # <repo> <key> <scratch>
  rm -rf "$3"
  ostree --repo="$3" init --mode=archive-z2
  ostree --repo="$3" remote add --set=gpg-verify=true --set=gpg-verify-summary=false \
    --gpg-import="$2" pub "file://$1" >/dev/null
}
commit_signed() {   # <scratch> <commit>: whether it has a valid signature from the key
  ostree --repo="$1" pull --commit-metadata-only pub "$2" > "$1.log" 2>&1
}

# A ref's commit signed with the key and the keyring the summary is signed
# with (`flatpak build-sign`; the commit's ID is unchanged).
gpg_home=()
[ -z "${GNUPGHOME:-}" ] || gpg_home=(--gpg-homedir="$GNUPGHOME")
sign_ref() {   # <repo> <ref>
  local kind id a br
  local -a runtime=()
  IFS=/ read -r kind id a br <<<"$2"
  [ "$kind" != runtime ] || runtime=(--runtime)
  flatpak build-sign "${runtime[@]}" --arch="$a" --gpg-sign="$GPG_KEY_ID" "${gpg_home[@]}" "$1" "$id" "$br" > "$W/sign.log" 2>&1 \
    || { cat "$W/sign.log" >&2; die "cannot sign $2's commit with $GPG_KEY_ID"; }
}

# A fresh <work>, its repository <work>/repo, and the signing key's public
# half beside it.
init_work() {   # <work>
  rm -rf "$1" && mkdir -p "$1"
  W=$(cd "$1" && pwd)
  public_key "$W/key.gpg"
  R="$W/repo"
  ostree --repo="$R" init --mode=archive-z2
}

# The published repository taken into <work>/repo: its config, and its refs
# at the commits the signed summary lists, each commit object with its
# signature and its root directory, and nothing below it. <work>/old is the
# summary's refs ("ref<TAB>commit"), empty for an empty prefix.
fetch_published() {   # <source>
  local src="$1" ref c f
  list_all "$src" "$W/all"
  : > "$W/old"
  if [ ! -s "$W/all" ]; then
    echo "::notice::no Flatpak repository at $src; treating as first publish"
    return 0
  fi
  for f in summary summary.sig config; do
    grep -qxF "$f" "$W/all" || die "$src holds $(wc -l < "$W/all") object(s) and no $f: a publish that did not finish, or a listing that did not complete"
  done
  mkdir -p "$W/published"
  printf '%s\n' summary summary.sig config > "$W/fetch"
  grep '^refs/heads/' "$W/all" >> "$W/fetch" || true
  rclone copy "$src" "$W/published" --files-from-raw "$W/fetch" --fast-list
  summary_refs "$W/published" "$W/key.gpg" "$W/check-old" "$W/old" \
    || die "$src: the published summary does not verify against the signing key"
  cp "$W/published/config" "$R/config"
  # The commit each ref is at and its root directory: what the new summary
  # is made from, and nothing below it. A commit's signature, where it has
  # one, comes with it.
  while IFS=$'\t' read -r ref c; do
    grep -qxF "$(obj "$c")" "$W/all" || die "$src: the summary lists $ref at $c, which is not there"
    obj "$c"
  done < "$W/old" > "$W/commits"
  if [ -s "$W/commits" ]; then
    rclone copy "$src" "$R" --files-from-raw "$W/commits" --fast-list
    while read -r f; do root_objects "$R/$f" || die "$src: $f does not read as a commit"; done < "$W/commits" | sort -u > "$W/roots"
    [ -z "$(comm -23 "$W/roots" "$W/all")" ] || die "$src: the published commits' root directories are not all there"
    sed 's/\.commit$/.commitmeta/' "$W/commits" | sort | comm -12 - "$W/all" >> "$W/roots"
    rclone copy "$src" "$R" --files-from-raw "$W/roots" --fast-list
  fi
  while IFS=$'\t' read -r ref c; do
    ostree --repo="$R" refs --create="$ref" "$c"
  done < "$W/old"
  note "$src: the signed summary lists $(wc -l < "$W/old") ref(s), each commit present"
}

# The summary regenerated and signed, then read back as a client will read
# it: signed, every old ref still there, <work>/new's refs at their commits,
# and every commit it lists signed with the key.
update_summary() {
  local ref c n
  flatpak build-update-repo --no-update-appstream --gpg-sign="$GPG_KEY_ID" "${gpg_home[@]}" "$R" > "$W/update.log" 2>&1 \
    || { cat "$W/update.log" >&2; die "flatpak build-update-repo failed"; }
  summary_refs "$R" "$W/key.gpg" "$W/check-new" "$W/got" || die "the new summary does not verify"
  [ -z "$(comm -23 <(cut -f1 "$W/old") <(cut -f1 "$W/got"))" ] || die "the new summary dropped refs the old one listed"
  while IFS=$'\t' read -r ref c n; do
    grep -qxF "$ref"$'\t'"$c" "$W/got" || die "the new summary does not list $ref at $c"
  done < "$W/new"
  indexed_refs "$R" "$W/key.gpg" "$W/check-idx" "$W/got-idx" || die "the new indexed summary does not verify"
  while IFS=$'\t' read -r ref c n; do
    grep -qxF "$ref"$'\t'"${c:0:12}" "$W/got-idx" || die "the new indexed summary does not list $ref at $c"
  done < "$W/new"
  sig_remote "$R" "$W/key.gpg" "$W/check-sig"
  while IFS=$'\t' read -r ref c; do
    commit_signed "$W/check-sig" "$c" \
      || { cat "$W/check-sig.log" >&2; die "the new summary lists $ref at $c, which has no valid signature from $GPG_KEY_ID"; }
  done < "$W/got"
}

# <work>/expect: the commit each architecture's ref is at once published.
write_expect() {
  awk -F'\t' '{ split($1, p, "/"); print p[3] "=" $2 }' "$W/new" | sort | tr '\n' ' ' | sed 's/ $//' > "$W/expect"
}

cmd_prepare() {   # <work> <source> <bundle>...
  local src="$2" b a ref c n old old_c f
  W="$1"
  shift 2
  : "${VERSION:?VERSION}" "${GPG_KEY_ID:?GPG_KEY_ID}" "${DIST_BASE_URL:?DIST_BASE_URL}"
  for b in "$@"; do [ -f "$b" ] || die "no bundle $b"; done
  init_work "$W"
  fetch_published "$src"

  # Each bundle, in a scratch repository of its own first: the one ref it
  # holds and its commit are read from it, not from what the import prints.
  : > "$W/new"
  n=0
  for b in "$@"; do
    n=$((n + 1))
    ostree --repo="$W/bundle-$n" init --mode=archive-z2
    flatpak build-import-bundle --no-update-summary "$W/bundle-$n" "$b" > "$W/bundle-$n.log" 2>&1 \
      || { cat "$W/bundle-$n.log" >&2; die "$b does not import"; }
    ref=$(ostree --repo="$W/bundle-$n" refs)
    case "$ref" in
      "app/$APP_ID/"*"/$BRANCH") a=${ref#app/$APP_ID/}; a=${a%/$BRANCH} ;;
      *) die "$b holds $(tr '\n' ' ' <<<"$ref"), not app/$APP_ID/<arch>/$BRANCH" ;;
    esac
    [[ " $ARCHES " == *" $a "* ]] || die "$b is for $a, which the repository does not carry"
    c=$(ostree --repo="$W/bundle-$n" rev-parse "$ref")
    [ "$(subject "$W/bundle-$n" "$c")" = "OpenChime $VERSION" ] \
      || die "$b is '$(subject "$W/bundle-$n" "$c")', not this release's (OpenChime $VERSION)"
    printf '%s\t%s\t%s\n' "$ref" "$c" "$n" >> "$W/new"
  done
  sort -o "$W/new" "$W/new"
  [ "$(cut -f1 "$W/new" | tr '\n' ' ')" = "$(for a in $ARCHES; do printf 'app/%s/%s/%s ' "$APP_ID" "$a" "$BRANCH"; done)" ] \
    || die "the bundles hold $(cut -f1 "$W/new" | tr '\n' ' ')-- one per architecture ($ARCHES) is a release"

  local -i added=0
  while IFS=$'\t' read -r ref c n; do
    old_c=$(awk -F'\t' -v r="$ref" '$1 == r { print $2 }' "$W/old")
    if [ "$old_c" = "$c" ]; then
      note "$ref is already published at $c, this release's; left as it is"
      continue
    fi
    if [ -n "$old_c" ]; then
      old=$(subject "$R" "$old_c")
      [ "$old" != "OpenChime $VERSION" ] || die "$ref is published at $old_c, also OpenChime $VERSION, and this release's is $c"
    fi
    # A ref file left at this release's commit by an earlier attempt is
    # allowed; at any other commit than the summary's, the bucket is not what
    # its summary says.
    if [ -s "$W/old" ] && [ -f "$W/published/refs/heads/$ref" ]; then
      f=$(cat "$W/published/refs/heads/$ref")
      [ "$f" = "$old_c" ] || [ "$f" = "$c" ] || die "$src: refs/heads/$ref is $f, and the summary lists $old_c"
    fi
    ostree --repo="$R" pull-local "$W/bundle-$n" "$ref" > "$W/pull-$n.log" 2>&1 \
      || { cat "$W/pull-$n.log" >&2; die "cannot take $ref from its bundle"; }
    sign_ref "$R" "$ref"
    added+=1
  done < "$W/new"
  # The ref files a summary lists and this release does not replace are
  # checked as well.
  while IFS=$'\t' read -r ref c; do
    [ -z "$(awk -F'\t' -v r="$ref" '$1 == r' "$W/new")" ] || continue
    f=$(cat "$W/published/refs/heads/$ref" 2>/dev/null || true)
    [ -z "$f" ] || [ "$f" = "$c" ] || die "$src: refs/heads/$ref is $f, and the summary lists $c"
  done < "$W/old"

  update_summary
  write_refs "$R"
  write_expect
  note "ready to upload, $added ref(s) moved; once published the repository lists $(cat "$W/expect")"
  cat "$W/expect"
}

cmd_resign() {   # <work> <source>
  local src="$2" ref c a
  W="$1"
  : "${GPG_KEY_ID:?GPG_KEY_ID}"
  init_work "$W"
  fetch_published "$src"
  [ -s "$W/old" ] || die "$src holds no Flatpak repository to re-sign"
  for a in $ARCHES; do
    grep -q "^app/$APP_ID/$a/$BRANCH"$'\t' "$W/old" || die "$src: the summary does not list app/$APP_ID/$a/$BRANCH"
  done
  sig_remote "$R" "$W/key.gpg" "$W/check-old-sig"
  local -i signed=0
  while IFS=$'\t' read -r ref c; do
    if commit_signed "$W/check-old-sig" "$c"; then
      note "$ref is at $c, signed; left as it is"
      continue
    fi
    sign_ref "$R" "$ref"
    signed+=1
  done < "$W/old"
  # The refs stay where they are: the new summary lists each at its old commit.
  cp "$W/old" "$W/new"
  update_summary
  [ "$(cat "$W/got")" = "$(cat "$W/old")" ] || die "the new summary does not list exactly the refs the old one did"
  grep "^app/$APP_ID/[^/]*/$BRANCH"$'\t' "$W/old" > "$W/new"
  write_expect
  note "ready to upload, $signed commit(s) signed; once published the repository lists $(cat "$W/expect")"
  cat "$W/expect"
}

# The two files a user adds the repository or installs the client from, with
# the key the summary is signed with (Flatpak's GPGKey, the binary key in
# base64), beside the summary.
write_refs() {   # <repo>
  local url="${DIST_BASE_URL%/}/flatpak/" key
  key=$(base64 -w0 < "$W/key.gpg")
  cat > "$1/$APP_ID.flatpakrepo" <<EOF
[Flatpak Repo]
Title=OpenChime
Url=$url
Homepage=https://openchime.io
Comment=The OpenChime client
GPGKey=$key
EOF
  cat > "$1/$APP_ID.flatpakref" <<EOF
[Flatpak Ref]
Name=$APP_ID
Branch=$BRANCH
Title=OpenChime
Url=$url
Homepage=https://openchime.io
IsRuntime=false
SuggestRemoteName=openchime
RuntimeRepo=https://dl.flathub.org/repo/flathub.flatpakrepo
GPGKey=$key
EOF
}

cmd_upload() {   # <work> <dest>
  local W="$1" dest="$2" f
  [ -f "$W/expect" ] || die "$W was not prepared"
  # Objects and the indexed summary's parts are named by their content: one
  # already there is this file, so it is not written again.
  rclone copy "$W/repo" "$dest" --fast-list --ignore-existing \
    --filter "+ objects/**" --filter "+ summaries/**" --filter "- **"
  rclone copy "$W/repo" "$dest" --fast-list --filter "+ /config" --filter "+ refs/heads/**" --filter "- **"
  # The summaries, then their signatures. A client that reads in between
  # finds a summary its signature does not match and refuses it, for the
  # moment; never a summary naming what is not there.
  for f in $SUMMARY; do rclone copyto "$W/repo/$f" "$dest/$f"; done
  for f in $SIGS; do rclone copyto "$W/repo/$f" "$dest/$f"; done
  rclone copy "$W/repo" "$dest" --fast-list --filter "+ /$APP_ID.flatpakrepo" --filter "+ /$APP_ID.flatpakref" --filter "- **"
  note "uploaded to $dest"
}

cmd_verify() {   # <dest> <expect>
  local dest="$1" expect="$2" a c f ref got
  : "${VERSION:?VERSION}" "${GPG_KEY_ID:?GPG_KEY_ID}"
  W=$(mktemp -d)
  trap 'rm -rf "$W"' EXIT
  public_key "$W/key.gpg"
  list_all "$dest" "$W/all"
  for f in $SUMMARY $SIGS config "$APP_ID.flatpakrepo" "$APP_ID.flatpakref"; do
    grep -qxF "$f" "$W/all" || die "$dest: no $f"
  done
  mkdir -p "$W/pub"
  rclone copy "$dest" "$W/pub" --fast-list \
    --filter "+ /summary*" --filter "+ /config" --filter "+ summaries/**" --filter "- **"
  summary_refs "$W/pub" "$W/key.gpg" "$W/check" "$W/refs" || die "$dest: the summary does not verify against the signing key"
  indexed_refs "$W/pub" "$W/key.gpg" "$W/check-idx" "$W/refs-idx" || die "$dest: the indexed summary does not verify against the signing key"
  got=""
  for a in $ARCHES; do
    ref="app/$APP_ID/$a/$BRANCH"
    c=$(awk -F'\t' -v r="$ref" '$1 == r { print $2 }' "$W/refs")
    [ -n "$c" ] || die "$dest: the summary does not list $ref"
    grep -qxF "$ref"$'\t'"${c:0:12}" "$W/refs-idx" || die "$dest: the indexed summary does not list $ref at $c"
    grep -qxF "$(obj "$c")" "$W/all" || die "$dest: $ref is at $c, which is not there"
    got+="$a=$c "
  done
  got=${got% }
  [ "$got" = "$expect" ] || die "$dest: the summary lists $got, expected $expect"
  # Each commit is this release's.
  # Each commit is this release's, and signed with the key: its detached
  # metadata there, and read as a client reads it.
  ostree --repo="$W/commits" init --mode=archive-z2
  for c in $expect; do
    c=${c#*=}
    grep -qxF "$(obj "$c" commitmeta)" "$W/all" || die "$dest: $c is not signed (no $(obj "$c" commitmeta))"
    obj "$c"; obj "$c" commitmeta
  done > "$W/commits.list"
  rclone copy "$dest" "$W/commits" --files-from-raw "$W/commits.list" --fast-list
  sig_remote "$W/commits" "$W/key.gpg" "$W/check-sig"
  for c in $expect; do
    c=${c#*=}
    [ "$(subject "$W/commits" "$c")" = "OpenChime $VERSION" ] || die "$dest: $c is '$(subject "$W/commits" "$c")', not OpenChime $VERSION"
    commit_signed "$W/check-sig" "$c" \
      || { cat "$W/check-sig.log" >&2; die "$dest: $c has no valid signature from $GPG_KEY_ID"; }
  done
  note "$dest lists $got, OpenChime $VERSION, both summaries and each commit signed"
}

verb="${1:-}"; shift || true
case "$verb" in
  prepare) [ "$#" -ge 3 ] || die "usage: prepare <work> <source> <bundle>..."; cmd_prepare "$@" ;;
  resign)  [ "$#" -eq 2 ] || die "usage: resign <work> <source>"; cmd_resign "$@" ;;
  upload)  [ "$#" -eq 2 ] || die "usage: upload <work> <dest>"; cmd_upload "$@" ;;
  verify)  [ "$#" -eq 2 ] || die "usage: verify <dest> <expect>"; cmd_verify "$@" ;;
  *) echo "usage: flatpak_repo.sh prepare|resign|upload|verify ..." >&2; exit 2 ;;
esac
