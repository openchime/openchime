#!/usr/bin/env bash
# The workflows keep the billed-minutes rules a reader cannot be trusted to
# remember (docs/CONTRIBUTING.md, "Billed minutes"):
#   - every job has timeout-minutes, so a hang bills a bounded time;
#   - every step that reaches the network has its own timeout-minutes, so one
#     stalled call does not hold the job to its limit;
#   - every apt install says on the line before why the runner image lacks the
#     package (a comment holding "apt:"); tools come pinned, not from apt.
# A step reaches the network when its script calls curl, wget, skopeo, flyctl,
# rclone, dnf, podman, buildah, docker, rsync, ssh, scp, gh, git push, fetch,
# clone, ls-remote or pull, dotnet restore, apt, or a scripts/release/ script.
# The same check, word for word, in all three repositories.
#
# Needs: python3 with PyYAML (on the runner image).
# Run by: make test, make test-all.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
python3 - "$@" <<'PY'
import glob, re, sys, yaml
NET = re.compile(r'\b(curl|wget|skopeo|flyctl|rclone|dnf|podman|buildah|docker|rsync|ssh|scp)\b'
                 r'|scripts/release/|scripts/build/apt\.sh|apt-get|\bgh\s'
                 r'|git\s+(push|fetch|clone|ls-remote|pull)|dotnet\s+restore')
APT = re.compile(r'scripts/build/apt\.sh|apt-get\s+install')
def check(f, text):
    w = yaml.safe_load(text) or {}
    lines = text.split('\n')
    out = []
    for jn, j in (w.get('jobs') or {}).items():
        if 'uses' in j:
            continue
        if 'timeout-minutes' not in j:
            out.append(f"{f}: job {jn} has no timeout-minutes")
        for st in j.get('steps') or []:
            run = st.get('run') or ''
            name = st.get('name') or (run.strip().splitlines()[0][:50] if run.strip() else st.get('uses', '?'))
            if NET.search(run) and 'timeout-minutes' not in st:
                out.append(f"{f}: job {jn}, step '{name}' reaches the network with no timeout-minutes")
    for i, ln in enumerate(lines):
        if APT.search(ln) and not ln.lstrip().startswith('#'):
            if 'apt:' not in (lines[i - 1] if i else ''):
                out.append(f"{f}:{i + 1}: an apt install with no '# apt: <why>' on the line before")
    return out

# The check checks itself first: a workflow made to break each rule must give
# one finding per rule, or the check is broken, not the workflows clean.
BROKEN = """on: push
jobs:
  a:
    runs-on: ubuntu-latest
    steps:
      - run: curl -fsS https://example.com
      - run: scripts/build/apt.sh jq
"""
if len(check('<broken>', BROKEN)) != 4:
    print("workflow_rules: the check misses a broken workflow; fix the check", file=sys.stderr)
    sys.exit(1)

files = sys.argv[1:] or sorted(glob.glob('.github/workflows/*.yml'))
bad = []
for f in files:
    bad += check(f, open(f).read())
for b in bad:
    print(f"workflow_rules: {b}", file=sys.stderr)
if bad:
    sys.exit(1)
print(f"workflow_rules: ok: {len(files)} workflows keep the billed-minutes rules")
PY
