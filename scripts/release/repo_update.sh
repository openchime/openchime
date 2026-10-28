#!/usr/bin/env bash
# Add a release's packages to the apt and rpm repositories in object storage,
# downloading only the indexes, not the pool (ARCH-20).
#
#   repo_update.sh prepare apt|rpm <work> <source> <mirror> <package>...
#   repo_update.sh upload  apt|rpm <work> <dest>
#   repo_update.sh verify  apt|rpm <dest> <expect>
#
# <source>, <mirror> and <dest> are rclone paths to a repository's prefix
# (dist:<bucket>/apt, dist-mirror:<bucket>/rpm). Any rclone remote serves, a
# local-path one included, which is how this is tested off line.
#
# `prepare` reads the source (the primary bucket), writes nothing to it, and
# builds in <work>/tree exactly what is to be uploaded: the indexes, extended
# by the new packages, signed; and the new packages. It prints, and writes to
# <work>/expect, how many versions each architecture's index lists once it is
# published ("amd64=4 arm64=4"). Before anything is published it reconciles
# the mirror bucket to the source's pool, copying any package the
# mirror lacks, so the index uploaded to both names nothing either is missing.
# `upload` copies <work>/tree to one bucket: packages first, then the indexes,
# the signed top file last, so no client ever reads an index naming a package
# not yet there. `rclone copy`, never sync: nothing is ever deleted. `verify`
# reads a published index back and checks it lists <expect> versions per
# architecture, this release's exactly once, and only objects that exist.
#
# The witnesses `prepare` refuses on, before anything is written:
#   - the listing must succeed: an auth failure or a 5xx stops it, and only
#     rclone's "directory not found" reads as an empty prefix;
#   - the old index must name exactly the packages listed in the pool (or the
#     pool also holds this release's own packages: an earlier attempt that
#     uploaded them and failed before the index);
#   - the tag guard: a prefix that exists but holds no package, with PRIOR
#     release tags, is a fetch that did not happen;
#   - a new package already in the index is the same file (a re-run), or the
#     release stops: two packages sharing a version is what apt cannot
#     recover from;
#   - the new index lists every old package plus exactly one new one per
#     architecture.
#
# Two methods. `incremental`, the default: fetch only dists/ or repodata/,
# append the new debs' stanzas, or sign only the new rpms and run createrepo_c
# --update --skip-stat over the listed and the new. `full`, the method this
# replaced and its fallback: fetch the whole prefix, re-sign every rpm and
# regenerate both indexes from the whole pool. rpm takes `full` by itself
# when createrepo_c lacks the flags (they are in 0.17.3, noble's) or its
# incremental result fails the checks above; REPO_UPDATE_MODE=full asks for
# it, which is also the repair for an index that has drifted from its pool.
#
# Environment: VERSION (the release number), PRIOR (release tags before this
# one: the tag guard), GPG_KEY_ID (the imported REPO_SIGNING_KEY);
# REPO_UPDATE_MODE=auto|incremental|full (auto).
set -euo pipefail
# One collation for every sort and comm below.
export LC_ALL=C

APT_ARCHES="amd64 arm64"
APT_POOL="pool/main/o/openchimed"

die()  { echo "::error::repo_update: $*" >&2; exit 1; }
note() { echo "repo_update: $*" >&2; }

# Parse the indexes. Python, as candidate_manifest.sh is: one reader of each
# format, used by `prepare` and `verify` both.
py() {
  python3 -I - "$@" <<'PY'
import bz2, gzip, hashlib, lzma, os, subprocess, sys
import xml.etree.ElementTree as ET

def opened(path):
    if path.endswith(".gz"):  return gzip.open(path, "rb").read()
    if path.endswith(".xz"):  return lzma.open(path, "rb").read()
    if path.endswith(".bz2"): return bz2.open(path, "rb").read()
    if path.endswith(".zst"): return subprocess.run(["zstd", "-dc", path], check=True, capture_output=True).stdout
    return open(path, "rb").read()

REPO = "{http://linux.duke.edu/metadata/repo}"
COMMON = "{http://linux.duke.edu/metadata/common}"

def repomd(repodata):
    """The primary file repomd.xml names, and its recorded sha256."""
    root = ET.parse(os.path.join(repodata, "repomd.xml")).getroot()
    for d in root.findall(REPO + "data"):
        if d.get("type") == "primary":
            loc = d.find(REPO + "location").get("href")
            ck = d.find(REPO + "checksum")
            return loc, ck.get("type"), ck.text
    sys.exit("repo_update: repomd.xml names no primary")

def primary(repodata):
    """href, arch, name, version-release, checksum for each package; the declared count."""
    loc, _, _ = repomd(repodata)
    path = os.path.join(os.path.dirname(repodata.rstrip("/")), loc)
    root = ET.fromstring(opened(path))
    rows = []
    for p in root.findall(COMMON + "package"):
        v = p.find(COMMON + "version")
        rows.append((p.find(COMMON + "location").get("href"), p.findtext(COMMON + "arch"),
                     p.findtext(COMMON + "name"), f"{v.get('ver')}-{v.get('rel')}",
                     p.findtext(COMMON + "checksum")))
    return rows, int(root.get("packages", "-1"))

def stanzas(path):
    if not os.path.exists(path): return []
    text = open(path, encoding="utf-8").read()
    out = []
    for block in text.split("\n\n"):
        f = {}
        for line in block.splitlines():
            if line and not line[0].isspace() and ":" in line:
                k, v = line.split(":", 1); f[k] = v.strip()
        if f: out.append(f)
    return out

mode, args = sys.argv[1], sys.argv[2:]
if mode == "primary":            # <repodata>  -> href arch name ver-rel checksum
    rows, declared = primary(args[0])
    if declared != len(rows):
        sys.exit(f"repo_update: primary.xml declares {declared} packages and holds {len(rows)}")
    for r in rows: print("\t".join(r))
elif mode == "packages":         # <Packages>  -> Filename Architecture Package Version SHA256
    for f in stanzas(args[0]):
        print("\t".join(f.get(k, "") for k in ("Filename", "Architecture", "Package", "Version", "SHA256")))
elif mode == "release-check":    # <dists/stable>: every index Release names has the hash it records
    base = args[0]; bad = 0; seen = 0; sect = None
    for line in open(os.path.join(base, "Release"), encoding="utf-8"):
        if not line.startswith(" "):
            sect = line.split(":", 1)[0]; continue
        if sect != "SHA256": continue
        h, size, name = line.split()
        p = os.path.join(base, name)
        if not os.path.exists(p): continue          # Release may name what a mirror lacks
        seen += 1
        got = hashlib.sha256(open(p, "rb").read()).hexdigest()
        if got != h or os.path.getsize(p) != int(size):
            print(f"repo_update: {name} does not match Release", file=sys.stderr); bad = 1
    if not seen: sys.exit("repo_update: Release names none of the fetched indexes")
    sys.exit(bad)
elif mode == "repomd-check":     # <repodata>: the primary file has the checksum repomd.xml records
    loc, kind, want = repomd(args[0])
    path = os.path.join(os.path.dirname(args[0].rstrip("/")), loc)
    got = hashlib.new(kind, open(path, "rb").read()).hexdigest()
    if got != want: sys.exit(f"repo_update: {loc} is {got}, repomd.xml says {want}")
else:
    sys.exit(f"repo_update: no mode {mode}")
PY
}

# The listing of every object under a prefix, relative to it, one per line.
# NOT `|| true`: that cannot tell "the prefix does not exist yet" from
# "credentials rejected" or "endpoint returned 503", and an empty pool read
# from a failed listing publishes an index of one release. An object store
# lists an absent prefix as empty, with exit 0; a local path reports it as
# rclone's exit 3, "directory not found", which is the one failure read as
# empty.
list_all() {   # <rclone path> <out file>
  local rc=0
  rclone lsf -R --files-only --fast-list "$1" > "$2" 2>"$2.err" || rc=$?
  if [ "$rc" -eq 3 ]; then
    : > "$2"
  elif [ "$rc" -ne 0 ]; then
    cat "$2.err" >&2
    die "cannot list $1 (rclone exit $rc); refusing to build an index from an unread pool"
  fi
  rm -f "$2.err"
  sort -o "$2" "$2"
}

pkgs_of() {   # <kind> <listing>: the packages in a listing, sorted
  if [ "$1" = apt ]; then
    awk '/^pool\/.*\.deb$/' "$2"
  else
    awk '/\.rpm$/ && !/^repodata\//' "$2"
  fi
}

# The pools of source and mirror must hold the same packages before the one
# index goes to both. What the mirror lacks is copied from the source, file by
# file (only those); what the mirror has and the source does not is reported
# and left, since nothing will name it. Run before anything is published.
reconcile() {   # <kind> <work> <source> <mirror>
  local kind="$1" w="$2" src="$3" mirror="$4"
  list_all "$mirror" "$w/mirror.all"
  pkgs_of "$kind" "$w/mirror.all" > "$w/mirror.pkgs"
  # This release's own packages are left to the upload, which writes them to
  # both; a copy of them from an earlier attempt is not wanted.
  comm -23 "$w/pool" "$w/mirror.pkgs" | comm -23 - "$w/new.paths" > "$w/mirror.missing"
  comm -13 "$w/pool" "$w/mirror.pkgs" | comm -23 - "$w/new.paths" > "$w/mirror.extra"
  if [ -s "$w/mirror.extra" ]; then
    echo "::warning::$mirror holds $(wc -l < "$w/mirror.extra") package(s) $src does not; left in place, and no index names them"
  fi
  if [ -s "$w/mirror.missing" ]; then
    note "$mirror lacks $(wc -l < "$w/mirror.missing") of the source's packages; copying them"
    rclone copy "$src" "$mirror" --files-from-raw "$w/mirror.missing" --fast-list
    list_all "$mirror" "$w/mirror.all"
    pkgs_of "$kind" "$w/mirror.all" > "$w/mirror.pkgs"
    [ -z "$(comm -23 "$w/pool" "$w/mirror.pkgs" | comm -23 - "$w/new.paths")" ] \
      || die "$mirror still lacks packages after the copy"
  fi
  note "$mirror holds the source's $(wc -l < "$w/pool") package(s)"
}

# The tag guard, as it was: a prefix that EXISTS and yields no package means
# the listing or the fetch silently did not complete, and an index published
# from it delists every prior release. No objects at all is a bucket nothing
# has published into, which is a first publish.
tag_guard() {   # <kind> <work>
  local n
  n=$(wc -l < "$2/pool")
  if [ -s "$2/all" ] && [ "$n" -eq 0 ] && [ "${PRIOR}" -gt 0 ]; then
    die "$1 pool is empty but ${PRIOR} release tag(s) exist -- refusing to publish an index that would delist them"
  fi
  [ -s "$2/all" ] || echo "::notice::no existing $1 objects in the source; treating as first publish"
}

# The old index must name what the pool holds. The one difference allowed is
# this release's own packages, listed but not indexed: an earlier attempt
# uploaded them (packages go first) and failed before the index.
witness() {   # <kind> <work> <indexed paths, sorted>
  local w="$2" over under
  over=$(comm -13 "$w/pool" "$3")
  under=$(comm -23 "$w/pool" "$3" | comm -23 - "$w/new.paths")
  [ -z "$over" ] || die "the $1 index names packages the pool does not hold: $(tr '\n' ' ' <<<"$over")"
  [ -z "$under" ] || die "the $1 pool holds packages its index does not name: $(tr '\n' ' ' <<<"$under")-- run with REPO_UPDATE_MODE=full to rebuild the index from the pool"
  note "$1 witness: the index names the $(wc -l < "$3") package(s) the pool holds"
}

sign_apt() {   # <dists/stable>
  local d="$1"
  rm -f "$d/Release" "$d/Release.gpg" "$d/InRelease"
  cat > "$WORK/apt-release.conf" <<'EOF'
APT::FTPArchive::Release::Origin "OpenChime";
APT::FTPArchive::Release::Label "OpenChime";
APT::FTPArchive::Release::Suite "stable";
APT::FTPArchive::Release::Codename "stable";
APT::FTPArchive::Release::Architectures "amd64 arm64";
APT::FTPArchive::Release::Components "main";
EOF
  ( cd "$(dirname "$d")/.." && apt-ftparchive -c "$WORK/apt-release.conf" release dists/stable > "$WORK/Release" )
  mv "$WORK/Release" "$d/Release"
  gpg --batch --yes --local-user "$GPG_KEY_ID" -abs -o "$d/Release.gpg" "$d/Release"
  gpg --batch --yes --local-user "$GPG_KEY_ID" --clearsign -o "$d/InRelease" "$d/Release"
}

# Each rpm is signed individually: rpm verifies package signatures, unlike apt,
# which verifies the index. The macros are given on the command line rather
# than in ~/.rpmmacros, so nothing of the runner's home is rewritten; %__gpg
# among them, since rpm's default names gpg2, which a runner need not have.
# Then each is checked against the key's public half, in a database of its
# own: a signature that did not happen is found here, not by dnf.
sign_rpms() {   # <rpm>...
  [ "$#" -gt 0 ] || return 0
  local db out f
  rpmsign --define "_gpg_name ${GPG_KEY_ID}" --define "__gpg $(command -v gpg)" \
    --define "__gpg_sign_cmd %{__gpg} gpg --batch --no-armor --no-secmem-warning -u \"%{_gpg_name}\" -sbo %{__signature_filename} %{__plaintext_filename}" \
    --addsign "$@" </dev/null >/dev/null
  db=$(mktemp -d)
  gpg --batch --export --armor "$GPG_KEY_ID" > "$db/key.asc"
  rpmkeys --dbpath "$db" --import "$db/key.asc"
  for f in "$@"; do
    out=$(rpmkeys --dbpath "$db" --checksig "$f" 2>&1) || die "$f does not verify after signing: $out"
    [[ "$out" == *"signatures OK"* ]] || die "$f carries no signature after signing: $out"
  done
  rm -rf "$db"
}

sign_repomd() {   # <repodata>
  rm -f "$1/repomd.xml.asc"
  gpg --batch --yes --local-user "$GPG_KEY_ID" --detach-sign --armor -o "$1/repomd.xml.asc" "$1/repomd.xml"
}

# An index entry's version is this release's: the deb's Version is N, the
# rpm's version-release is N-<release>.
IS_V='function isv(x) { return x == v || index(x, v "-") == 1 }'

# Per architecture, how many entries an index holds, as "arch=n ..." sorted.
counts() {   # <rows file: path TAB arch ...>
  awk -F'\t' '{ n[$2]++ } END { for (a in n) print a "=" n[a] }' "$1" | sort | tr '\n' ' ' | sed 's/ $//'
}

# The new index's own check: every old entry, plus exactly one entry of this
# version for each architecture this release has a package for (read from the
# packages given, <work>/new.arches, not from the index being checked).
# Writes <work>/expect.
check_new_index() {   # <kind> <old rows> <new rows> <version string>
  local kind="$1" old="$2" new="$3" ver="$4" a want got
  : > "$WORK/expect.parts"
  for a in $(cat "$WORK/new.arches"); do
    want=$(( $(awk -F'\t' -v a="$a" -v v="$ver" "$IS_V"' $2 == a && !isv($4)' "$old" | wc -l) + 1 ))
    got=$(awk -F'\t' -v a="$a" '$2 == a' "$new" | wc -l)
    [ "$got" -eq "$want" ] || { note "$kind $a: the new index lists $got, expected $want"; return 1; }
    [ "$(awk -F'\t' -v a="$a" -v v="$ver" "$IS_V"' $2 == a && isv($4)' "$new" | wc -l)" -eq 1 ] \
      || { note "$kind $a: the new index does not list version $ver exactly once"; return 1; }
    echo "$a=$want" >> "$WORK/expect.parts"
  done
  # Nothing old is lost: every old path is still there.
  [ -z "$(comm -23 <(cut -f1 "$old" | sort) <(cut -f1 "$new" | sort))" ] \
    || { note "$kind: the new index dropped packages the old one listed"; return 1; }
  sort "$WORK/expect.parts" | tr '\n' ' ' | sed 's/ $//' > "$WORK/expect"
}

# --- apt ----------------------------------------------------------------------
apt_rows() {   # <Packages> <arch>: its rows, the architecture that of the index
  py packages "$1" | awk -F'\t' -v a="$2" 'BEGIN { OFS = "\t" } { $2 = a; print }'
}

prepare_apt() {   # <source> <deb>...
  local src="$1"; shift
  local T="$WORK/tree" d f a base sha old_sha clash
  local -a publish=()
  d="$T/dists/stable"
  if [ "$MODE" = full ]; then
    if [ -s "$WORK/all" ]; then rclone copy "$src" "$T" --fast-list; fi
    [ -z "$( (cd "$T" && find pool -name '*.deb' 2>/dev/null || true) | sort | comm -3 - "$WORK/pool")" ] \
      || die "the apt pool fetched is not the pool listed; the copy did not complete"
  elif grep -q '^dists/' "$WORK/all"; then
    rclone copy "$src/dists" "$T/dists" --fast-list
  fi
  mkdir -p "$d"
  : > "$WORK/old.rows"
  for a in $APT_ARCHES; do
    apt_rows "$d/main/binary-$a/Packages" "$a" >> "$WORK/old.rows"
  done
  [ -z "$(cut -f1,2 "$WORK/old.rows" | sort | uniq -d)" ] || die "the old apt index names a package twice"
  cut -f1 "$WORK/old.rows" | sort -u > "$WORK/old.paths"
  [ "$MODE" = full ] || witness apt "$WORK" "$WORK/old.paths"

  # Each new deb: published already (a re-run: the same file, by its sha256,
  # left as it is), or new.
  mkdir -p "$WORK/scratch/$APT_POOL"
  for f in "$@"; do
    base="$APT_POOL/$(basename "$f")"
    sha=$(sha256sum "$f" | cut -d' ' -f1)
    old_sha=$(awk -F'\t' -v p="$base" '$1 == p { print $5 }' "$WORK/old.rows" | sort -u)
    if [ -n "$old_sha" ] && [ "$MODE" != full ]; then
      [ "$old_sha" = "$sha" ] || die "$base is in the index with sha256 $old_sha, and this release's is $sha"
      note "$base is already published, the same file; left as it is"
    else
      cp "$f" "$WORK/scratch/$APT_POOL/"
      publish+=("$f")
    fi
  done
  # The new debs' stanzas, from a scratch tree of the same layout, so each
  # names its file exactly as a regeneration over the whole pool would.
  : > "$WORK/new.rows"
  for a in $APT_ARCHES; do
    ( cd "$WORK/scratch" && apt-ftparchive --arch "$a" packages pool ) > "$WORK/new.$a"
    apt_rows "$WORK/new.$a" "$a" >> "$WORK/new.rows"
  done
  # Refuse duplicates: this package and version for this architecture under
  # another file is two debs sharing a version.
  if [ "$MODE" != full ]; then
    while IFS=$'\t' read -r base a pkg v _; do
      clash=$(awk -F'\t' -v p="$base" -v a="$a" -v n="$pkg" -v v="$v" \
                '$1 != p && $2 == a && $3 == n && $4 == v { print $1 }' "$WORK/old.rows")
      [ -z "$clash" ] || die "$pkg $v ($a) is already published as another file: $clash"
    done < "$WORK/new.rows"
  fi

  if [ "${#publish[@]}" -gt 0 ]; then
    for a in $APT_ARCHES; do
      mkdir -p "$d/main/binary-$a"
      f="$d/main/binary-$a/Packages"
      if [ "$MODE" = full ]; then
        # The method this replaced: every stanza regenerated from the whole pool.
        mkdir -p "$T/$APT_POOL"
        cp "${publish[@]}" "$T/$APT_POOL/"
        ( cd "$T" && apt-ftparchive --arch "$a" packages pool ) > "$f"
      elif [ -s "$WORK/new.$a" ]; then
        # Appended after the old stanzas, one blank line between, as
        # apt-ftparchive separates them.
        if [ -s "$f" ]; then
          python3 -I -c 'import sys; p = sys.argv[1]; t = open(p).read().rstrip("\n"); open(p, "w").write(t + "\n\n")' "$f"
        fi
        cat "$WORK/new.$a" >> "$f"
      fi
      touch "$f"
      gzip -9kf "$f"
    done
  fi
  : > "$WORK/new.rows.all"
  for a in $APT_ARCHES; do
    apt_rows "$d/main/binary-$a/Packages" "$a" >> "$WORK/new.rows.all"
  done
  check_new_index apt "$WORK/old.rows" "$WORK/new.rows.all" "$VERSION" || die "the new apt index fails its own check"
  # A re-run that finds everything in place re-signs nothing and adds nothing.
  [ "${#publish[@]}" -gt 0 ] || return 0
  sign_apt "$d"
  mkdir -p "$T/$APT_POOL"
  cp "${publish[@]}" "$T/$APT_POOL/"
}

# --- rpm ----------------------------------------------------------------------
rpm_incremental_ok() {
  local help flag
  command -v createrepo_c >/dev/null || { note "no createrepo_c"; return 1; }
  help=$(createrepo_c --help 2>&1) || return 1
  for flag in --update --skip-stat --pkglist --error-exit-val; do
    [[ "$help" == *"$flag"* ]] || { note "createrepo_c has no $flag"; return 1; }
  done
}

# The header digests of an rpm: unchanged by signing, so a package signed on
# an earlier attempt and the unsigned one it was made from compare equal.
rpm_digest() {
  rpm -qp --nosignature --qf '%{NEVRA} %{SHA256HEADER} %{SHA1HEADER}\n' "$1" 2>/dev/null
}

# An incremental result createrepo_c made that fails the checks is not a
# witness failure but a tool behaving differently: say so in <work>/retry,
# and the caller takes the full method. Never `return` non-zero here: this
# runs under set -e, which a caller's `||` would switch off for the whole
# function.
retry_full() { note "$*"; touch "$WORK/retry"; }

prepare_rpm() {   # <source> <rpm>...
  local src="$1"; shift
  local T="$WORK/tree" f base name arch vr pub clash
  local -a publish=()
  if [ "$MODE" = full ]; then
    if [ -s "$WORK/all" ]; then rclone copy "$src" "$T" --fast-list; fi
    [ -z "$( (cd "$T" && find . -name '*.rpm' -not -path './repodata/*' || true) | sed 's|^\./||' | sort | comm -3 - "$WORK/pool")" ] \
      || die "the rpm pool fetched is not the pool listed; the copy did not complete"
  elif grep -q '^repodata/' "$WORK/all"; then
    rclone copy "$src/repodata" "$T/repodata" --fast-list
  fi
  if [ -f "$T/repodata/repomd.xml" ]; then
    py primary "$T/repodata" > "$WORK/old.rows"
  else
    : > "$WORK/old.rows"
  fi
  [ -z "$(cut -f1 "$WORK/old.rows" | sort | uniq -d)" ] || die "the old rpm index names a package twice"
  cut -f1 "$WORK/old.rows" | sort > "$WORK/old.paths"
  [ "$MODE" = full ] || witness rpm "$WORK" "$WORK/old.paths"

  for f in "$@"; do
    base=$(basename "$f")
    IFS=$'\t' read -r name arch vr < <(rpm -qp --nosignature --qf '%{NAME}\t%{ARCH}\t%{VERSION}-%{RELEASE}\n' "$f" 2>/dev/null)
    [ -n "$name" ] && [ -n "$arch" ] || die "$f is not a readable rpm"
    if [ "$MODE" != full ] && grep -qxF "$base" "$WORK/old.paths"; then
      # A re-run: the published object must be this package, signed.
      mkdir -p "$WORK/published"
      rclone copyto "$src/$base" "$WORK/published/$base"
      pub=$(rpm_digest "$WORK/published/$base")
      [[ "$pub" != *"(none) (none)"* ]] && [ "$pub" = "$(rpm_digest "$f")" ] \
        || die "$base is in the index and is not this release's package"
      note "$base is already published, the same package; left as it is"
      continue
    fi
    if [ "$MODE" != full ]; then
      # Refuse duplicates: this name, architecture and version under another file.
      clash=$(awk -F'\t' -v p="$base" -v n="$name" -v a="$arch" -v v="$vr" \
                '$1 != p && $3 == n && $2 == a && $4 == v { print $1 }' "$WORK/old.rows")
      [ -z "$clash" ] || die "$name $vr ($arch) is already published as another file: $clash"
    fi
    cp "$f" "$T/"
    publish+=("$T/$base")
  done

  if [ "$MODE" = full ]; then
    # The method this replaced: every rpm re-signed, the index regenerated.
    mapfile -t all_rpms < <(cd "$T" && find . -name '*.rpm' -not -path './repodata/*' | sed 's|^\./||' | sort)
    ( cd "$T" && sign_rpms "${all_rpms[@]}" )
    createrepo_c --update "$T" >/dev/null
  elif [ "${#publish[@]}" -gt 0 ]; then
    sign_rpms "${publish[@]}"
    # The listed packages and the new: --pkglist instead of a walk of the
    # directory, which holds only the new; --skip-stat so the listed, absent
    # here, are taken from the old index by their path; --error-exit-val so a
    # package neither indexed nor present fails the run instead of dropping out.
    { cat "$WORK/pool"; for f in "${publish[@]}"; do basename "$f"; done; } | sort -u > "$WORK/pkglist"
    if ! createrepo_c --update --skip-stat --error-exit-val --pkglist "$WORK/pkglist" "$T" >/dev/null; then
      retry_full "createrepo_c --update --skip-stat failed"
      return 0
    fi
  fi
  [ -f "$T/repodata/repomd.xml" ] || die "no rpm index was made"
  if ! py primary "$T/repodata" > "$WORK/new.rows.all"; then
    retry_full "createrepo_c's index cannot be read back"
    return 0
  fi
  if ! check_new_index rpm "$WORK/old.rows" "$WORK/new.rows.all" "$VERSION"; then
    [ "$MODE" = incremental ] || die "the rpm index fails its own check"
    retry_full "createrepo_c's incremental index fails its own check"
    return 0
  fi
  # A re-run that finds everything in place re-signs nothing.
  [ "$MODE" = full ] || [ "${#publish[@]}" -gt 0 ] || return 0
  sign_repomd "$T/repodata"
}

# --- the verbs ----------------------------------------------------------------
cmd_prepare() {   # <kind> <work> <source> <mirror> <package>...
  local kind="$1" src="$3" mirror="$4" f asked
  WORK="$2"; shift 4
  : "${VERSION:?VERSION}" "${PRIOR:?PRIOR}" "${GPG_KEY_ID:?GPG_KEY_ID}"
  case "$kind" in apt|rpm) ;; *) die "kind is apt or rpm, not $kind" ;; esac
  asked="${REPO_UPDATE_MODE:-auto}"
  case "$asked" in
    auto|incremental) MODE=incremental ;;
    full) MODE=full ;;
    *) die "REPO_UPDATE_MODE is auto, incremental or full, not $asked" ;;
  esac
  if [ "$kind" = rpm ] && [ "$MODE" = incremental ] && ! rpm_incremental_ok; then
    [ "$asked" = auto ] || die "createrepo_c cannot update incrementally"
    echo "::warning::createrepo_c cannot update incrementally; publishing the rpm repository the full way"
    MODE=full
  fi
  for f in "$@"; do [ -f "$f" ] || die "no package $f"; done
  rm -rf "$WORK" && mkdir -p "$WORK/tree"
  WORK=$(cd "$WORK" && pwd)
  pkgs=()
  for f in "$@"; do pkgs+=("$(cd "$(dirname "$f")" && pwd)/$(basename "$f")"); done
  for f in "${pkgs[@]}"; do
    if [ "$kind" = apt ]; then echo "$APT_POOL/$(basename "$f")"; else basename "$f"; fi
  done | sort > "$WORK/new.paths"
  [ -z "$(uniq -d "$WORK/new.paths")" ] || die "two packages given share a file name"
  # The architectures this release has a package for, from the packages.
  for f in "${pkgs[@]}"; do
    if [ "$kind" = apt ]; then dpkg-deb -f "$f" Architecture; else rpm -qp --nosignature --qf '%{ARCH}\n' "$f" 2>/dev/null; fi
  done | sort > "$WORK/new.arches"
  [ -z "$(uniq -d "$WORK/new.arches")" ] || die "more than one package for one architecture"
  if [ "$kind" = apt ]; then
    while read -r f; do
      [[ " $APT_ARCHES " == *" $f "* ]] || die "a deb for $f, which the repository does not carry"
    done < "$WORK/new.arches"
  fi

  list_all "$src" "$WORK/all"
  pkgs_of "$kind" "$WORK/all" > "$WORK/pool"
  note "$kind: $src lists $(wc -l < "$WORK/all") object(s), $(wc -l < "$WORK/pool") package(s); method: $MODE"
  tag_guard "$kind" "$WORK"

  if [ "$kind" = apt ]; then
    prepare_apt "$src" "${pkgs[@]}"
  else
    prepare_rpm "$src" "${pkgs[@]}"
    if [ -f "$WORK/retry" ]; then
      [ "$asked" = auto ] || die "the incremental rpm index fails its checks"
      echo "::warning::createrepo_c's incremental index failed its checks; publishing the rpm repository the full way"
      MODE=full
      rm -rf "$WORK/tree" "$WORK/published" "$WORK/retry" && mkdir -p "$WORK/tree"
      prepare_rpm "$src" "${pkgs[@]}"
      [ ! -f "$WORK/retry" ] || die "the rpm index fails its own check"
    fi
  fi
  # Only once the index is built and has passed every check, and still
  # before anything is published: the mirror's pool made the source's.
  reconcile "$kind" "$WORK" "$src" "$mirror"
  echo "$MODE" > "$WORK/mode"
  note "$kind: ready to upload ($MODE), $(cd "$WORK/tree" && find . -type f | wc -l) file(s); once published the index lists $(cat "$WORK/expect")"
}

cmd_upload() {   # <kind> <work> <dest>
  local kind="$1" w="$2" dest="$3" top
  [ -f "$w/expect" ] || die "$w was not prepared"
  if [ "$kind" = apt ]; then top=dists/stable; else top=repodata; fi
  # Packages, then the indexes, then the signed top of the index.
  rclone copy "$w/tree" "$dest" --fast-list --filter "- $top/**" --filter "- dists/**" --filter "+ **"
  if [ "$kind" = apt ]; then
    rclone copy "$w/tree" "$dest" --fast-list \
      --filter "- $top/Release" --filter "- $top/Release.gpg" --filter "- $top/InRelease" \
      --filter "+ dists/**" --filter "- **"
    rclone copy "$w/tree" "$dest" --fast-list \
      --filter "+ $top/Release" --filter "+ $top/Release.gpg" --filter "+ $top/InRelease" --filter "- **"
  else
    rclone copy "$w/tree" "$dest" --fast-list --filter "- $top/repomd.xml*" --filter "+ $top/**" --filter "- **"
    rclone copy "$w/tree" "$dest" --fast-list --filter "+ $top/repomd.xml*" --filter "- **"
  fi
  note "$kind: uploaded to $dest"
}

cmd_verify() {   # <kind> <dest> <expect>
  local kind="$1" dest="$2" expect="$3" v a got
  : "${VERSION:?VERSION}"
  WORK=$(mktemp -d)
  trap 'rm -rf "$WORK"' EXIT
  list_all "$dest" "$WORK/all"
  pkgs_of "$kind" "$WORK/all" > "$WORK/pool"
  if [ "$kind" = apt ]; then
    rclone copy "$dest/dists" "$WORK/dists" --fast-list
    py release-check "$WORK/dists/stable" || die "$dest: Release does not match the indexes it names"
    : > "$WORK/rows"
    for a in $APT_ARCHES; do
      [ -f "$WORK/dists/stable/main/binary-$a/Packages.gz" ] || die "$dest: no Packages.gz for $a"
      gzip -dc "$WORK/dists/stable/main/binary-$a/Packages.gz" | cmp -s - "$WORK/dists/stable/main/binary-$a/Packages" \
        || die "$dest: Packages and Packages.gz differ for $a"
      py packages "$WORK/dists/stable/main/binary-$a/Packages" | awk -F'\t' -v a="$a" '{ $2 = a; print }' OFS='\t' >> "$WORK/rows"
    done
  else
    rclone copy "$dest/repodata" "$WORK/repodata" --fast-list
    py repomd-check "$WORK/repodata" || die "$dest: repomd.xml does not match its primary"
    py primary "$WORK/repodata" > "$WORK/rows"
  fi
  got=$(counts "$WORK/rows")
  [ "$got" = "$expect" ] || die "$dest: the $kind index lists $got, expected $expect (the previous count + 1 per architecture)"
  for a in $(cut -f2 "$WORK/rows" | sort -u); do
    [ "$(awk -F'\t' -v a="$a" -v v="$VERSION" "$IS_V"' $2 == a && isv($4)' "$WORK/rows" | wc -l)" -eq 1 ] \
      || die "$dest: the $kind index does not list version $VERSION for $a exactly once"
  done
  missing=$(cut -f1 "$WORK/rows" | sort -u | comm -23 - "$WORK/pool")
  [ -z "$missing" ] || die "$dest: the $kind index names objects that are not there: $(tr '\n' ' ' <<<"$missing")"
  note "$kind: $dest lists $got, version $VERSION once per architecture, every package present"
}

verb="${1:-}"; shift || true
case "$verb" in
  prepare) [ "$#" -ge 5 ] || die "usage: prepare apt|rpm <work> <source> <mirror> <package>..."; cmd_prepare "$@" ;;
  upload)  [ "$#" -eq 3 ] || die "usage: upload apt|rpm <work> <dest>"; cmd_upload "$@" ;;
  verify)  [ "$#" -eq 3 ] || die "usage: verify apt|rpm <dest> <expect>"; cmd_verify "$@" ;;
  *) echo "usage: repo_update.sh prepare|upload|verify ..." >&2; exit 2 ;;
esac
