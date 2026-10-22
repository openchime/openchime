# OpenChime — Contributing & Workflow

The branch, commit, and CI policy for this repo.

**Contributions from outside** are pull requests against `staging`. Each is
reviewed; one that is taken is included in the next release candidate, with its
author kept and their `Signed-off-by` beside the maintainer's, and its pull
request is then closed, since its change reaches `staging` with that candidate.

## Branches & merging

- **`staging` is the integration branch.** All feature work lands on `staging`
  through a pull request.
- **`main` is what was released, and nothing else.** A **release-candidate**
  is cut from `staging` (`gh workflow run release-candidate.yml --ref staging`):
  the staging tip, once its CI, attribution guard and commit policy have
  passed, is built, tagged `release-N-rcK` and published as a prerelease. A
  **release** is made from a release-candidate (`gh workflow run release.yml
  --ref staging -f release_candidate=release-N-rcK`): it fast-forwards `main`
  to that exact commit, tags it `release-N` and publishes the same files (see
  [RELEASING.md](./RELEASING.md)). Both are tagged and published in this
  repository, their images in Fly's registry
  (`registry.fly.io/openchime-daemon-<environment>`), and each is copied to
  `openchime/openchime` at the end; the copy is a convenience, and nothing
  depends on it. A ruleset refuses every update to `main` except the release's
  own, made with the repository's deploy key, so `main` is always an ancestor
  of `staging`, and never takes a direct push or a pull request.
- **Every feature has exactly one GitHub issue**, and its branch is named for
  it: `feature/oc-<issue number>-<short-kebab-description>`, cut from a clean,
  current `staging`. The repo's `/oc-feature-start` command runs the preflight
  (clean tree, `staging` in sync with origin, no branch already carrying the
  issue), refuses rather than repairs, and makes one fresh build on the new
  branch.
- **Land by pull request to `staging`,** squashed to a **single** house-format
  commit whose subject names the issue. The issue **closes when its release
  ships**: the release reads the issue from each shipped commit's subject and
  closes it with a comment naming the release. A merge does not close it — merged
  is not shipped. The PR body stays **empty**: the issue and the commit already
  say everything. `/oc-feature-finish` runs the sequence (below).
- **CI gates the merge.** Staging's branch protection requires every CI job and
  the two policy checks to pass on the pull request before it can merge. Merge
  with a **squash merge** — no merge commits, history stays linear. Delete the
  branch, local and remote, after.
- **Documentation and workflow changes take the same path**: a branch, a pull
  request into `staging`, the same checks. CI's build and test jobs skip when
  only documentation changed (`*.md`, `.gitignore`, `.claude/`), and a skipped
  job counts as passed, so the pull request can merge in a minute. Once merged,
  `promote` moves `main` to it with no release, when everything waiting on
  `staging` is documentation or workflows (RELEASING.md "Promotion"); when code
  is waiting, it goes out with the next release.
- **Sign off every commit** (`git commit -s`) — the DCO applies to every path
  into the tree, including a cherry-pick.
- **One logical change per branch.**

## Before a pull request — the minimal set

Locally, and in about 2 minutes, a pull request needs exactly this, which
`/oc-feature-finish` runs:

1. `make && make test` — zero warnings, every suite passing.
2. `scripts/check/pr.sh "<title>"` — the title's format and length.
3. One commit, signed off, no body; pushed; the pull request raised into
   `staging` with an empty body.

That is all. **The thread sanitizer, the release compiler, the Windows builds
and the end-to-end run are CI's**, on the pull request, and staging will not merge
until they pass; running them locally as well repeats the gate and costs the
time the gate exists to save. `make test-ci` refuses to run outside CI.

**A push that touches the GUI runs `scripts/gui/smoke.sh` first** (TESTING.md
"Reading the GUI smoke"): about ten seconds, and the only check of the Windows
client CI cannot run.

## Commits

- `OC-<issue>: Capitalised imperative summary` — **no trailing punctuation**, at
  most 72 characters once GitHub appends the pull request suffix on squash, and
  **no body** beyond the sign-off. `commit-policy` enforces all of it on every
  push to `staging` and `main`.
- Cite decision ids inline where relevant: `ARCH-N`, `REQ-N`.
- **Every commit cites its issue**, in the subject prefix: `OC-N` is issue N on
  `openchime/openchime`, the project's issue list, whose numbers are stable
  identifiers. `pr-policy` checks N is open there, and the release closes it.
- **No file cites an issue number — a commit message is the only place one
  belongs.** Not source, not scripts, not the workflows, not the documents.
  Comments explain themselves, or cite a `REQ-N` / `ARCH-N`: REQUIREMENTS.md and
  ARCHITECTURE.md are stable documents living in this repository, and their ids
  identify rather than position. An issue number in a file is a pointer with no
  integrity — it rots the moment the issue is closed, renamed, merged or
  superseded, and nothing in the file can tell the reader that it has.

  Instead of pointing at an issue, say the thing: "the daemon exits 0 when it
  cannot start, which systemd reads as success". The prose survives the tracker.

  **This is enforced.** `scripts/check/refs.sh` fails on a hash followed by
  digits and on a tracker URL ending in a number. `make test` and CI's build
  job run it before the suite.
  A bare link to the tracker carrying no number is fine — and note the rule
  applies to this file too, which is why the patterns above are described rather
  than written out.
- **Never** add a `Co-Authored-By:` or any attribution trailer naming Claude /
  Anthropic. The [`attribution-guard`](../.github/workflows/attribution-guard.yml)
  workflow scans author, committer, and message on every push and **rejects**
  matches (it deletes an offending branch or tag; a violation can only reach
  `main` through a release, and a release-candidate is refused for a commit
  the guard did not pass).

## Adding a setting

A daemon setting is one row in `daemon/config_table.c`: its name, kind, range
or values, default, class (operator, managed, or development) and one line of
meaning. Nothing else: the loader reads it from the row, CONFIG.md's reference
and `packaging/openchimed.env` are regenerated from it
(`scripts/check/config.sh --write`), `openchimed --check-config` reports it, and
`make test` fails if a `getenv` appears outside the loader or a name appears
anywhere the registry does not know. A client setting goes through
`client/core/env.c` and a `CLIENT` row, so the name is known. A test input is a
`CFG_DEV` row named `OPENCHIME_DEV_*`, read through `oc_env_dev` on a client: it
does nothing unless `OPENCHIME_MODE=development` (CONFIG.md "Mode").

## CI

`ci.yml` runs on every pull request into `staging` and every push to `staging`,
and the push's run is the gate `release-candidate.yml` waits for. It checks nothing twice:
each test suite runs once, one clang compile serves as both the release-compiler
and the second-compiler check, and what the release checks itself is left to the
release. Every vendored library is cached by the script that builds it, and
staging's runs save the caches pull requests restore. Three jobs, on three
machines at once:

- **`build`** — builds the daemon; checks it links only libc and libm; starts
  it and drives it over TLS with the e2e client; compiles every Linux
  translation unit with the release's clang under `-Werror`
  (`check-release-cc`); runs the suites that start no threads
  (`make test-ci PART=plain`, which runs the source checks first:
  `scripts/check/opcodes.sh`, `refs.sh`, `make_targets.sh`, `config.sh`,
  `release_lint.sh` and `tests_lint.sh`).
- **`thread-sanitizer`** — the suites that start threads, under
  ThreadSanitizer (`make test-ci PART=threads`); their only run in CI.
- **`windows`** — cross-compiles the Windows TUI + GUI with the pinned
  llvm-mingw (make fetches it).
- **`pr policy`** and **`guard`** — the pull request's title and body (and that the
  title names an open issue), and the attribution guard
  ([`attribution-guard`](../.github/workflows/attribution-guard.yml)).
  The guard runs on every push including documentation-only ones, since what
  it rejects lives in commit messages and author lines. A release-candidate is refused for a staging commit it has not passed
  on.

**All five are required checks on `staging`.** CI runs on every pull request
and push; its first job, `classify the change`, skips the three build jobs when
only documentation changed, and GitHub counts a skipped job as passed, so a
documentation pull request reports every required check.

**Read the run's result before asking for a merge**: the `build` job's TLS run
of the shipped `main()` and the thread-sanitizer suites are checks a local
`make test` does not make.

See [TESTING.md](./TESTING.md) for the full test strategy.

## Cross-repo

- This public repo holds the daemon, the client app-core, the wire protocol, and
  the shared **requirements/specs** (`REQ-N`) + architecture (`ARCH-N`). The
  hosted control plane and federated-service **implementation** live elsewhere:
  **share the contract, keep the implementation**.
- Control-plane work sometimes creates daemon-side dependencies here; they land as
  ordinary `ARCH-N` / `REQ-N` items (e.g. a max-registered-users config cap, and
  the daemon's first outbound federated-services client for enrollment/push).
