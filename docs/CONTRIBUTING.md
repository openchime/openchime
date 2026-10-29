# OpenChime — Contributing & Workflow

The branch, commit, and CI policy for this repo.

**Contributions from outside** are pull requests against `staging`. Each is
reviewed; one that is taken is included in the next release candidate, with its
author kept and their `Signed-off-by` beside the maintainer's, and its pull
request is then closed, since its change reaches `staging` with that candidate.
No automated check runs on a pull request on `openchime/openchime`: the checks
under "CI" run where the maintainers integrate a change, so run the local
checks below yourself before opening one.
Most of this document is the maintainers' own workflow; an outside contributor
needs "Before a pull request" and "Commits" (and "Adding a setting" for a new
setting).

## Branches & merging

- **`staging` is the integration branch.** All feature work lands on `staging`
  through a pull request.
- **`main` is what was released, and nothing else.** The path is feature →
  `staging` → staging build → release-candidate → release. A contributor's
  part ends at the merge into `staging`; the maintainers start every step
  after it by hand ("Releases", below). `main` is a line of snapshot commits
  the release pipeline writes, and only fast-forwards; `staging` holds the
  real commits. `main` never takes a pull request.
- **Every feature has exactly one GitHub issue**, and its branch is named for
  it: `feature/oc-<issue number>-<short-kebab-description>`, cut from a clean,
  current `staging` ("Starting a feature").
- **Land by pull request to `staging`,** squashed to a **single** house-format
  commit whose subject names the issue. The issue **closes when its release
  ships**: the release reads the issue from each shipped commit's subject, and
  from the documentation it carries onto `main`, and closes it with a comment
  naming the release. A merge does not close it — merged
  is not shipped. The PR body stays **empty**: the issue and the commit already
  say everything ("Before a pull request").
- **Merge with a squash merge** once the required checks pass ("CI") — no
  merge commits, history stays linear. What stands between a change and
  `staging` is the local test (below). Delete the branch, local and remote,
  after.
- **Documentation and workflow changes take the same path**: a branch, a pull
  request into `staging`, the same checks. Once merged, a documentation-only
  commit (`*.md` and `docs/`, except `docs/CONFIG.md`, which follows the
  daemon's settings, and `LICENSING.md`) can be put on `main` by the
  maintainers without a release (`promote.yml`); anything else, workflows
  included, goes out with the next release.
- **Sign off every commit** (`git commit -s`) — the DCO applies to every path
  into the tree, including a cherry-pick.
- **One logical change per branch.**

## Starting a feature

Stop at the first check that fails and fix the cause; nothing here stashes,
resets or works around a dirty state.

1. `git config core.hooksPath` prints `.githooks` (if not, run
   `git config core.hooksPath .githooks`: the pre-push hook is what holds a
   push to a tree tested locally).
2. `git fetch origin --prune`; `git status --porcelain` prints nothing.
3. Local `staging` is not ahead of or diverged from `origin/staging`; bring it
   current with `git checkout staging && git pull --ff-only origin staging`.
4. The issue, on `openchime/openchime`: `gh issue view <n> -R openchime/openchime`
   for an existing one (not closed). For new work, search first —
   `gh issue list -R openchime/openchime --state open --search "<words>"` —
   and only then create one: a title stating what is wrong or missing as a
   fact, a body saying what it costs and citing `REQ-N` / `ARCH-N`, and an
   existing label pair where one fits (`win32`, `daemon`, `testing`,
   `packaging-release` × `missing-feature`, `defect`, `verification-gap`).
   `gh issue list` shows 30 by default and the tracker holds more:
   `gh issue list -R openchime/openchime --state open --limit 500` lists them
   all (add `--label <name>` or `--search "<words>"` to filter).
5. No branch already carries the issue:
   `git branch -a --list '*feature/oc-<n>-*'` prints nothing.
6. `git checkout -b feature/oc-<n>-<short-kebab-description> staging` — `oc`
   lowercase, the description a few words naming the change (the issue "The
   daemon exits 0, silently, when it cannot start" becomes
   `feature/oc-<n>-daemon-exit-nonzero`).
7. `make clean && make`, so nothing built for other work is linked in
   (`clean` keeps the downloaded speech models).

Before writing code, read the `REQ-N` / `ARCH-N` entries the issue names and
the doc that owns the area. A frame layout change or a reassigned opcode bumps
`OC_PROTOCOL_VERSION` (PROTOCOL.md §2); a schema change is a new numbered
migration, never an edit to an existing one (SCHEMA.md §1).

## Before a pull request — the minimal set

Locally, and in about 2 minutes, a pull request needs exactly this:

1. `scripts/check/test.sh` — `make && make test`, then it records the tree it
   passed on for the pre-push hook. Zero warnings, every suite passing
   (`OK: all suites passed`), then `tested: tree … recorded`. Any change after
   it means running it again; a change to `*.md`, `.gitignore` or `.claude/`
   alone may skip it.
2. `scripts/check/pr.sh "OC-<n>: <Capitalised imperative summary>"` — the
   title's format and length, and that the issue is open.
3. One commit ahead of `origin/staging`, signed off, no body. To squash:

       git reset --soft $(git merge-base origin/staging HEAD)
       git commit -s -m "OC-<n>: <the title from step 2>"

4. `git status --porcelain` prints nothing; push. `git push --force-with-lease`
   only when step 3 rewrote a branch already pushed.
5. `gh pr create --base staging --title "<the commit's subject>" --body ""`.

After a policy failure on the pull request (`pr policy`, `guard`), fix it, run
step 1 again if anything but the title changed, and push once.

That is all. **The thread sanitizer, the release compiler, the Windows builds
and the end-to-end run are the staging build's** ("CI"); running them locally as
well repeats that and costs the time it exists to save. `make test-ci` refuses
to run outside CI. A change that risks them (threads, a new compiler idiom,
Windows code) may run `make check-release-cc` or `make windows` locally first,
by choice: a failure in the staging build holds back everything merged with
the change.

**Before pushing a GUI change, run `scripts/gui/smoke.sh`** (TESTING.md
"Reading the GUI smoke").

## Commits

- `OC-<issue>: Capitalised imperative summary` — **no trailing punctuation**, at
  most 72 characters once GitHub appends the pull request suffix on squash, and
  **no body** beyond the sign-off. `pr-policy` enforces the title, which the
  squash commits; `scripts/release/commit_policy.sh` checks the commits
  themselves, those since the last release in the staging build and those
  `promote` puts on `main`, and warns (a subject already on `staging` is fixed only by rewriting it).
- Cite decision ids inline where relevant: `ARCH-N`, `REQ-N`.
- **Every commit cites its issue**, in the subject prefix: `OC-N` is issue N on
  `openchime/openchime`, the project's public issue tracker, whose numbers are
  stable identifiers.
  `pr-policy` checks N is open there, and the release closes it.
  `scripts/check/refs.sh` refuses a tracker prefix with a number in any file.
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
  digits and on a tracker URL ending in a number. `make test` and the staging
  build's tests run it before the suite.
  A bare link to the tracker carrying no number is fine — and note the rule
  applies to this file too, which is why the patterns above are described rather
  than written out.
- **Never** add a `Co-Authored-By:` or any attribution trailer naming Claude /
  Anthropic. The attribution scan (`scripts/release/attribution_scan.sh`)
  checks author, committer, and message on every pull request (a step of
  `pr-policy`, which posts the required check `guard`), over the commits since
  the last release in the staging build, and over what `promote` puts on
  `main`, and **rejects** matches: the pull request cannot merge, and the staging
  build or the promotion is refused. `main` is never force-pushed, and
  `staging` only in this one case: a violation on `staging` (the squash
  GitHub commits can add a trailer the title never had). An admin rewrites
  `staging` from the commit before it, keeping every later commit (an
  interactive rebase that rewords or drops only that commit), and pushes it
  with `git push --force-with-lease`, relaxing `staging`'s protection for that
  push alone. No build of the violating commit can be made, so it cannot reach
  `main`; documentation already promoted from rewritten commits is still
  recognised by its patch-id.

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

## Keeping CI small

Nothing runs on its own but what must ("CI"). The cheap checks are steps of a job a workflow already has, never jobs
of their own; no job waits on another workflow (a gate checks once and fails
fast); every job has a timeout. A failure is found locally, not in CI:
`scripts/check/test.sh` (`make && make test`) runs before the commit, and
`.githooks/pre-push` refuses a tree it did not pass on (a change to `*.md`,
`.gitignore` or `.claude/` alone excepted) and workflows `actionlint` rejects.
The hooks are on once per clone: `git config core.hooksPath .githooks`
("Starting a feature").

## CI

Nothing runs on a pull request but **`pr policy`**, and nothing runs on a push.
`pr-policy.yml` checks the pull request's title and body, that the title
names an open issue, and the tree's issue references (`scripts/check/refs.sh`,
from the base branch); the attribution scan is a step of it that posts
**`guard`**. These two are the required checks on `staging`, and the branch
need not be up to date to merge.

The build and the tests run in the **staging build** (`staging-build.yml`),
dispatched by hand on `staging` for its tip, usually after a few changes have
merged:

    gh workflow run staging-build.yml --ref staging

Its gate checks the commits since the last release (the attribution scan refuses; the
commit policy warns) and refuses a commit already built. Then the tests, by
calling `ci.yml`, two jobs on two machines at once:

- **`build + unit tests`** — builds the daemon; checks it links only libc and
  libm; starts it and drives it over TLS with the e2e client; compiles every
  Linux translation unit with the release's clang under `-Werror`
  (`check-release-cc`); runs the suites that start no threads
  (`make test-ci PART=plain`, which runs `mk/tests.mk`'s `SOURCE_CHECKS`
  first); and the web client in a headless Chrome (TESTING.md §3.4).
- **`thread sanitizer`** — the suites that start threads, under
  ThreadSanitizer (`make test-ci PART=threads`); their only run in CI.

Only when both pass does it build what a release ships — both daemons with
their data, the images, and the Windows clients (with the two Windows test
programs) — and store the daemon trees, the Windows binaries and the images by
commit; a stored build is what a release-candidate is made from. The packages
and installers are built too, under a placeholder version, only to prove they
build; a release makes them again under its number. Every vendored library and
toolchain is cached by the script that builds it, and the staging build's runs
save the caches the next one restores; a cache unused for seven days is
evicted. Once a month `toolchains.yml` files an issue when a pinned toolchain
is behind upstream.

**A failure there is everyone's.** It holds back every change merged since the
last staging build: find the commit, fix it on a branch like any other change,
and run the staging build again on the new tip.

See [TESTING.md](./TESTING.md) for the full test strategy.

**Every network call retries a failed connection.** Every workflow sets `BASH_ENV` to `.github/net-retry.bash`, so every bash step, and every bash script it runs, gets it: `git`, `gh` and `ssh-keyscan` (GitHub); `skopeo`, `docker`, `buildah` and `flyctl` (Fly's registry, API and builder); `curl`; and `dotnet restore`, while `rclone`, `npm` and NuGet are told through their own settings to retry harder and give up on a stall. Each attempt is bounded, so a stalled connection fails instead of hanging, and a call that failed in the connection (a timeout, a dropped, reset or refused connection, a 5xx) is retried with backoff, up to four times. A real refusal, such as a rejected push, a missing ref or image, a 4xx or a failed health check, fails at once.

## Releases

The maintainers cut releases; a contributor does nothing for one. A staging
build tests the `staging` tip and stores what a release ships, by commit; a
release-candidate is a stored build, tagged `release-N-rcK` and published as a
prerelease; a release takes one release-candidate and publishes the same
binaries, numbered N, to apt, dnf, the container registries and the GitHub
release (the tarballs and the Windows installer), appends a `Release N`
snapshot to `main`, and closes the issues it shipped, those of the
documentation it carries included (`staging-build.yml`,
`release-candidate.yml`, `release.yml`). Each release-candidate, release and
promotion is then copied to the public repository (`openchime/openchime`): its
`staging`, the tag, the GitHub release and `main`, fast-forward only. Between releases, `promote.yml`
appends `Documentation` snapshots to `main`. No compiler runs after the staging
build. A build is identified by its commit and a release by its number;
[UPGRADING.md](./UPGRADING.md) says how the two relate and how a self-hoster
upgrades. A change that alters what an operator must do on upgrade adds a
section to UPGRADING.md in the same pull request.

## Security

Report a vulnerability privately, through GitHub's private vulnerability
reporting on [`openchime/openchime`](https://github.com/openchime/openchime/security)
("Report a vulnerability"), not in a public issue. Once its fix is ready it
gets a plainly worded `OC` issue, which the fixing commit cites.

## Cross-repo

- This public repo holds the daemon, the client app-core, the wire protocol, and
  the shared **requirements/specs** (`REQ-N`) + architecture (`ARCH-N`). The
  federated services' **implementation** lives elsewhere: **share the
  contract, keep the implementation**.
- A federated service sometimes needs something of the daemon; that lands here
  as ordinary `ARCH-N` / `REQ-N` items (e.g. a max-registered-users cap, and
  the daemon's outbound client for enrollment and push).
