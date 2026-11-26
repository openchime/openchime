#!/usr/bin/env bash
# The workflows keep the billed-minutes rules a reader cannot be trusted to
# remember (the repository's CONTRIBUTING.md, "Billed minutes"):
#   - every job has timeout-minutes, so a hang bills a bounded time;
#   - every step that reaches the network has its own timeout-minutes, so one
#     stalled call does not hold the job to its limit;
#   - every apt install says on the line before why the runner image lacks the
#     package (a comment holding "apt:"); tools come pinned, not from apt;
#   - every deployable with an image of its own (deploy/<name>/Dockerfile, or a
#     target of deploy/Dockerfile a deploy/fly/<name>-*.toml builds) has,
#     in release-candidate.yml, a step "Build the <name> image" and a step
#     "Deploy the <name>", and in release.yml a step "Promote the <name> image"
#     and a step "Deploy the <name>"; both run scripts/release/smoke.sh. A
#     repository with no such image has nothing to check.
# A step reaches the network when its script calls curl, wget, skopeo, flyctl,
# rclone, dnf, podman, buildah, docker, rsync, ssh, scp, gh, git push, fetch,
# clone, ls-remote or pull, dotnet restore, apt, make test-full (which restores),
# or a scripts/release/ or deploy/ script.
# The same check, word for word, in all three repositories.
#
# Needs: python3 with PyYAML (on the runner image).
# Run by: the fast suite and the full suite (make test; make test-all or
# make test-full).
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
python3 - "$@" <<'PY'
import glob, re, sys, yaml
NET = re.compile(r'\b(curl|wget|skopeo|flyctl|rclone|dnf|podman|buildah|docker|rsync|ssh|scp)\b'
                 r'|scripts/release/|scripts/build/apt\.sh|apt-get|\bgh\s'
                 r'|git\s+(push|fetch|clone|ls-remote|pull)|dotnet\s+restore'
                 r'|\bdeploy/\S+\.sh|make\s+test-full')
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

def steps(text):
    w = yaml.safe_load(text) or {}
    return [st for j in (w.get('jobs') or {}).values() for st in (j.get('steps') or [])]

def deployables(names, rc, release):
    out = []
    for f, text, wanted in (('release-candidate.yml', rc, ['Build the {} image', 'Deploy the {}']),
                            ('release.yml', release, ['Promote the {} image', 'Deploy the {}'])):
        have = steps(text)
        named = {st.get('name') for st in have}
        for n in names:
            for w in wanted:
                if w.format(n) not in named:
                    out.append(f"{f}: no step '{w.format(n)}'")
        if not any('scripts/release/smoke.sh' in (st.get('run') or '') for st in have):
            out.append(f"{f}: no step runs scripts/release/smoke.sh")
    return out

# The same for this rule: a deployable no workflow names gives one finding per step missing.
if len(deployables(['x'], 'jobs: {}', 'jobs: {}')) != 6:
    print("workflow_rules: the check misses a deployable no workflow ships; fix the check", file=sys.stderr)
    sys.exit(1)

files = sys.argv[1:] or sorted(glob.glob('.github/workflows/*.yml'))
bad = []
for f in files:
    bad += check(f, open(f).read())
# A deployable is a deploy/<name>/Dockerfile, or a target of the one shared
# deploy/Dockerfile that a deploy/fly/<name>-*.toml builds (build-target).
names = {p.split('/')[1] for p in glob.glob('deploy/*/Dockerfile')}
for p in glob.glob('deploy/fly/*.toml'):
    m = re.search(r'^\s*build-target\s*=\s*"([^"]+)"', open(p).read(), re.M)
    if m:
        names.add(m.group(1))
names = sorted(names)
if names and not sys.argv[1:]:
    bad += deployables(names, open('.github/workflows/release-candidate.yml').read(),
                       open('.github/workflows/release.yml').read())
for b in bad:
    print(f"workflow_rules: {b}", file=sys.stderr)
if bad:
    sys.exit(1)
print(f"workflow_rules: ok: {len(files)} workflows keep the billed-minutes rules"
      + (f"; each of {', '.join(names)} is built, deployed and promoted" if names else ""))
PY
