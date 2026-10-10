# OpenChime — Contributing & Workflow

The branch, commit, and CI policy for this repo. The private control-plane repo
(`openchime-saas`) follows the same rules, tailored to its .NET CI.

## Branches & merging

- **`staging` is the integration branch.** All feature work lands on `staging`
  through a pull request.
- **`main` is what was released, and nothing else.** It moves only when
  `staging` is **promoted**: `gh workflow run promote.yml --ref staging` runs CI
  on the staging tip, confirms its attribution guard and commit policy passed,
  and fast-forwards `main` to that exact commit with the repository's deploy key.
  That push is the release (`release.yml`; see [RELEASING.md](./RELEASING.md)).
  A ruleset refuses every other update to `main`, from anyone — so `main` is
  always an ancestor of `staging`, and never takes a direct push or a pull
  request.
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
- **Docs-only changes go straight to `staging`**, without a pull request, and
  reach `main` with the next promotion (they skip the build jobs via
  `paths-ignore`; the attribution guard runs on every push regardless).
- **Sign off every commit** (`git commit -s`) — the DCO applies to every path
  into the tree, including a cherry-pick.
- **One logical change per branch.**

## Before a pull request — the minimal set

Locally, and in about 2 minutes, a pull request needs exactly this, which
`/oc-feature-finish` runs:

1. `make && make test` — zero warnings, every suite passing.
2. `scripts/check_pr.sh "<title>"` — the title's format and length.
3. One commit, signed off, no body; pushed; the pull request raised into
   `staging` with an empty body.

That is all. **The thread sanitizer, the release compiler, the Windows builds
and the end-to-end run are CI's**, on the pull request, and staging will not merge
until they pass; running them locally as well repeats the gate and costs the
time the gate exists to save. `make test-ci` refuses to run outside CI.

## Commits

- `OC-<issue>: Capitalised imperative summary` — **no trailing punctuation**, at
  most 72 characters once GitHub appends the pull request suffix on squash, and
  **no body** beyond the sign-off. `commit-policy` enforces all of it on every
  push to `staging` and `main`.
- Cite decision ids inline where relevant: `ARCH-N`, `REQ-N`.
- **Every commit cites its issue**, in the subject prefix. GitHub Issues is the
  project's only issue list, and its numbers are stable identifiers.
- **No file cites an issue number — a commit message is the only place one
  belongs.** Not source, not scripts, not the workflows, not the documents.
  Comments explain themselves, or cite a `REQ-N` / `ARCH-N`: REQUIREMENTS.md and
  ARCHITECTURE.md are stable documents living in this repository, and their ids
  identify rather than position. An issue number in a file is a pointer with no
  integrity — it rots the moment the issue is closed, renamed, merged or
  superseded, and nothing in the file can tell the reader that it has.

  Instead of pointing at an issue, say the thing: "the daemon exits 0 when it
  cannot start, which systemd reads as success". The prose survives the tracker.

  **This is enforced.** `make check-refs` (`scripts/check_refs.sh`) fails on a
  hash followed by digits and on a tracker URL ending in a number.
  It is a prerequisite of `make test` and its own step in CI's build job.
  A bare link to the tracker carrying no number is fine — and note the rule
  applies to this file too, which is why the patterns above are described rather
  than written out.
- **Never** add a `Co-Authored-By:` or any attribution trailer naming Claude /
  Anthropic. The [`attribution-guard`](../.github/workflows/attribution-guard.yml)
  workflow scans author, committer, and message on every push and **rejects**
  matches (it deletes an offending branch or tag; a violation can only reach
  `main` through a promotion, and promotion refuses a commit the guard did not
  pass).

## CI

`ci.yml` runs on every pull request into `staging` and every push to `staging`,
and is the gate `promote.yml` and `release.yml` call. It checks nothing twice:
each test suite runs once, one clang compile serves as both the release-compiler
and the second-compiler check, and what the release checks itself is left to the
release. Every vendored library is cached by the script that builds it, and
staging's runs save the caches pull requests restore. Three jobs, on three
machines at once:

- **`build`** — builds the daemon; checks it links only libc and libm; starts
  it and drives it over TLS with the e2e client; compiles every Linux
  translation unit with the release's clang under `-Werror`
  (`check-release-cc`); runs the suites that start no threads
  (`make test-ci PART=plain`, with `check-opcodes` and `check-refs`).
- **`thread-sanitizer`** — the suites that start threads, under
  ThreadSanitizer (`make test-ci PART=threads`); their only run in CI.
- **`windows`** — cross-compiles the Windows TUI + GUI, and builds the Linux
  TUI with that runner's newer gcc, which warns where gcc 11 and clang do not.
- **`pr policy`** and **`guard`** — the pull request's title and body, and the
  attribution guard ([`attribution-guard`](../.github/workflows/attribution-guard.yml)).
  The guard has **no `paths-ignore`**, so it runs on every push including
  docs-only ones, since what it rejects lives in commit messages and author
  lines. A promotion refuses a staging commit it has not passed on.

**All five are required checks on `staging`.** Docs-only pushes skip the build
jobs (`paths-ignore: ['**.md', ...]`), which is why docs go straight to
`staging` rather than through a pull request that would wait on checks that
never run.

See [TESTING.md](./TESTING.md) for the full test strategy.

## Cross-repo

- This public repo holds the daemon, the client app-core, the wire protocol, and
  the shared **requirements/specs** (`REQ-N`) + architecture (`ARCH-N`). The
  hosted control plane and federated-service **implementation** live in the
  separate **private** `openchime-saas` repo — **share the contract, keep the
  implementation**.
- Control-plane work sometimes creates daemon-side dependencies here; they land as
  ordinary `ARCH-N` / `REQ-N` items (e.g. a max-registered-users config cap, and
  the daemon's first outbound federated-services client for enrollment/push).
