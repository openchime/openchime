#!/usr/bin/env bash
# The candidate's manifest (docs/RELEASING.md): what a candidate build on
# staging made, signed, so a release on main can take exactly those files and
# nothing else.
#
#   candidate_manifest.sh write  <dir> <build-id> <sha> <run-number> <run-id> [name=digest ...]
#   candidate_manifest.sh sign   <dir>            # with the imported REPO_SIGNING_KEY
#   candidate_manifest.sh verify <dir> [build-id] # signature, then every file's sha256
#
# `write` lists every regular file under <dir> (the manifest and its signature
# aside) with its sha256 and size, and the images the candidate pushed, as
# name=digest pairs. `verify` refuses a manifest whose signature does not
# check, whose build id is not the one asked for, or whose files are missing
# or changed. One script for both ends, so the build and the release cannot
# drift in what they call a candidate.
set -euo pipefail
verb="${1:-}"; dir="${2:-}"
[ -n "$verb" ] && [ -d "$dir" ] || { echo "usage: candidate_manifest.sh write|sign|verify <dir> ..." >&2; exit 2; }
M="$dir/manifest.json"
case "$verb" in
  write)
    id="${3:?build-id}"; sha="${4:?sha}"; run="${5:?run-number}"; rid="${6:?run-id}"; shift 6
    python3 - "$dir" "$id" "$sha" "$run" "$rid" "$@" <<'PY'
import hashlib, json, os, sys, time
d, bid, sha, run, rid, *images = sys.argv[1:]
files = []
for root, _, names in os.walk(d):
    for n in sorted(names):
        p = os.path.join(root, n); rel = os.path.relpath(p, d)
        if rel in ("manifest.json", "manifest.json.asc") or not os.path.isfile(p): continue
        h = hashlib.sha256()
        with open(p, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""): h.update(chunk)
        files.append({"path": rel, "sha256": h.hexdigest(), "size": os.path.getsize(p)})
files.sort(key=lambda f: f["path"])
imgs = dict(i.split("=", 1) for i in images)
m = {"build_id": bid, "sha": sha, "run_number": int(run), "run_id": int(rid),
     "created": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "files": files, "images": imgs}
with open(os.path.join(d, "manifest.json"), "w") as f: json.dump(m, f, indent=2, sort_keys=True); f.write("\n")
print(f"manifest: build {bid} ({sha}), run {run}, {len(files)} files, {len(imgs)} images")
PY
    ;;
  sign)
    [ -f "$M" ] || { echo "candidate_manifest: no $M" >&2; exit 1; }
    rm -f "$M.asc"
    gpg --batch --yes --detach-sign --armor -o "$M.asc" "$M"
    echo "manifest: signed"
    ;;
  verify)
    want="${3:-}"
    [ -f "$M" ] && [ -f "$M.asc" ] || { echo "candidate_manifest: $dir has no signed manifest" >&2; exit 1; }
    gpg --batch --verify "$M.asc" "$M" 2>/dev/null || { echo "candidate_manifest: the manifest's signature does not check" >&2; exit 1; }
    python3 - "$dir" "$want" <<'PY'
import hashlib, json, os, sys
d, want = sys.argv[1:]
m = json.load(open(os.path.join(d, "manifest.json")))
if want and m["build_id"] != want:
    print(f"candidate_manifest: the manifest is for build {m['build_id']}, not {want}", file=sys.stderr); sys.exit(1)
bad = 0
for f in m["files"]:
    p = os.path.join(d, f["path"])
    if not os.path.isfile(p):
        print(f"candidate_manifest: missing {f['path']}", file=sys.stderr); bad += 1; continue
    h = hashlib.sha256()
    with open(p, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""): h.update(chunk)
    if h.hexdigest() != f["sha256"] or os.path.getsize(p) != f["size"]:
        print(f"candidate_manifest: {f['path']} differs from the manifest", file=sys.stderr); bad += 1
listed = {f["path"] for f in m["files"]}
for root, _, names in os.walk(d):
    for n in names:
        rel = os.path.relpath(os.path.join(root, n), d)
        if rel not in listed and rel not in ("manifest.json", "manifest.json.asc"):
            print(f"candidate_manifest: {rel} is not in the manifest", file=sys.stderr); bad += 1
if bad: sys.exit(1)
print(f"manifest: build {m['build_id']} ({m['sha']}), run {m['run_number']}: {len(m['files'])} files verified")
PY
    ;;
  *) echo "candidate_manifest: unknown verb $verb" >&2; exit 2 ;;
esac
