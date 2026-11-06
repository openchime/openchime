# OpenChime — Contributing & Workflow

The branch, commit, and CI policy for this repo.

**Contributions from outside** are pull requests against `staging`. Each is
reviewed; one that is taken is included in the next release candidate, with its
author kept and their `Signed-off-by` beside the maintainer's, and its pull
request is then closed, since its change reaches `staging` with that candidate.
No automated check runs on a pull request on `openchime/openchime`: the checks
under "CI" run where the maintainers integrate a change, so run the fast suite
(`make test`, below) yourself before opening one.
Most of this document is the maintainers' own workflow; an outside contributor
needs "Before a pull request" and "Commits" (and "Adding a setting" for a new
setting).

## Branches & merging

- **`staging` is the integration branch.** All feature work lands on `staging`
  through a pull request.
- **`main` is what was released, and nothing else.** The path is feature →
  `staging` → release-candidate → release. A contributor's
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
- **Merge with a squash merge** once the required check passes ("CI") — no
  merge commits, history stays linear. What stands between a change and
  `staging` is the fast suite, run locally (below). Delete the branch, local
  and remote, after.
- **Documentation and workflow changes take the same path**: a branch, a pull
  request into `staging`, the same checks. A documentation-only change (`*.md`
  and `docs/`, except `docs/CONFIG.md`, which follows the daemon's settings,
  and `LICENSING.md`; the one rule is `scripts/policy/docs_only.sh`) can be
  pushed without `make test`, and once merged can be put on `main` by the
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
7. `make clean && make build`, so nothing built for other work is linked in
   (`clean` keeps the downloaded speech models).

Before writing code, read the `REQ-N` / `ARCH-N` entries the issue names and
the doc that owns the area. A frame layout change or a reassigned opcode bumps
`OC_PROTOCOL_VERSION` (PROTOCOL.md §2); a schema change is a new numbered
migration, never an edit to an existing one (SCHEMA.md §1).

## Before a pull request — the minimal set

Locally, within the fast suite's budget (TESTING.md), a pull request needs
exactly this:

1. `make test` — the fast suite: it builds (`make build`), runs the source
   checks, actionlint and the fast suites, and records the tree it passed on
   for the pre-push hook. Zero warnings, every suite passing (`OK: all suites
   passed`), then `tested: tree … recorded`. Any change after it means running
   it again; a documentation-only change alone may skip it.
2. One commit ahead of `origin/staging`, signed off, no body, its subject the
   pull request's title: `OC-<n>: <Capitalised imperative summary>`
   (`scripts/policy/subject.sh "<subject>"` checks it as `pr-policy` will).
   To squash:

       git reset --soft $(git merge-base origin/staging HEAD)
       git commit -s -m "OC-<n>: <Capitalised imperative summary>"

3. `git status --porcelain` prints nothing; push. The pre-push hook refuses a
   tree `make test` did not pass on, and a subject `pr-policy` would refuse.
   `git push --force-with-lease` only when step 2 rewrote a branch already
   pushed.
4. `gh pr create --base staging --title "<the commit's subject>" --body ""`.

After a policy failure on the pull request (`pr policy`), fix it, run
step 1 again if anything but the title changed, and push once.

That is all locally. **There are two test suites, and nothing else runs a
test** (TESTING.md): the fast one, `make build` and `make test`, here, before
every push; and the full one, `make build-all` and `make test-all`, in CI,
once per release-candidate. Each refuses to run on the other side.

**Before pushing a GUI change, run `tools/gui/smoke.sh`** (TESTING.md
"Reading the GUI smoke").

## Commits

- `OC-<issue>: Capitalised imperative summary` — **no trailing punctuation**, at
  most 72 characters once GitHub appends the pull request suffix on squash, and
  **no body** beyond the sign-off. The rule is `scripts/policy/subject.sh`, and
  everything that checks a subject runs it: the pre-push hook on the branch's
  tip, `pr-policy` on the title, which the squash commits, and
  `scripts/policy/commit_policy.sh` on the commits themselves, those since the
  last release in the release-candidate's gate and those `promote` puts on
  `main`, where it warns (a subject already on `staging` is fixed only by
  rewriting it).
- Cite decision ids inline where relevant: `ARCH-N`, `REQ-N`.
- **Every commit cites its issue**, in the subject prefix: `OC-N` is issue N on
  `openchime/openchime`, the project's public issue tracker, whose numbers are
  stable identifiers.
  `pr-policy` checks N is open there, and the release closes it.
  `scripts/policy/refs.sh` refuses a tracker prefix with a number in any file.
- **No file cites an issue number — a commit message is the only place one
  belongs.** Not source, not scripts, not the workflows, not the documents.
  Comments explain themselves, or cite a `REQ-N` / `ARCH-N`: REQUIREMENTS.md and
  ARCHITECTURE.md are stable documents living in this repository, and their ids
  identify rather than position. An issue number in a file is a pointer with no
  integrity — it rots the moment the issue is closed, renamed, merged or
  superseded, and nothing in the file can tell the reader that it has.

  Instead of pointing at an issue, say the thing: "the daemon exits 0 when it
  cannot start, which systemd reads as success". The prose survives the tracker.

  **This is enforced.** `scripts/policy/refs.sh` fails on a hash followed by
  digits and on a tracker URL ending in a number. Both suites run it before
  their tests, and `pr-policy` over every pull request's tree.
  A bare link to the tracker carrying no number is fine — and note the rule
  applies to this file too, which is why the patterns above are described rather
  than written out.
- **Never** add a `Co-Authored-By:` or any attribution trailer naming Claude /
  Anthropic. The attribution scan (`scripts/policy/attribution_scan.sh`)
  checks author, committer, and message on every pull request (a step of
  `pr-policy`, so a match fails the required check `pr policy`), over the commits since
  the last release in the release-candidate's gate, and over what `promote` puts on
  `main`, and **rejects** matches: the pull request cannot merge, and the
  release-candidate or the promotion is refused. `main` is never force-pushed, and
  `staging` only in this one case: a violation on `staging` (the squash
  GitHub commits can add a trailer the title never had). An admin rewrites
  `staging` from the commit before it, keeping every later commit (an
  interactive rebase that rewords or drops only that commit), and pushes it
  with `git push --force-with-lease`, relaxing `staging`'s protection for that
  push alone. No release-candidate of the violating commit can be made, so it
  cannot reach `main`; documentation already promoted from rewritten commits is still
  recognised by its patch-id.

## Adding a setting

A daemon setting is one row in `daemon/config_table.c`: its name, kind, range
or values, default, class (operator, managed, or development) and one line of
meaning. Nothing else: the loader reads it from the row, CONFIG.md's reference
and `packaging/openchimed.env` are regenerated from it
(`scripts/test/fast/config.sh --write`), `openchimed --check-config` reports it, and
`make test` fails if a `getenv` appears outside the loader or a name appears
anywhere the registry does not know. A client setting goes through
`client/core/env.c` and a `CLIENT` row, so the name is known. A test input is a
`CFG_DEV` row named `OPENCHIME_DEV_*`, read through `oc_env_dev` on a client: it
does nothing unless `OPENCHIME_MODE=development` (CONFIG.md "Mode").

## Keeping CI small

Two suites, and nothing else runs a test (TESTING.md): the fast one locally,
before every push; the full one in CI, once per release-candidate, on the
staging tip. Each test is a script under `scripts/test/fast/` or
`scripts/test/full/`; the make targets call the scripts, and the workflows call
only the targets. No test runs on a pull request, in a release or in a
promotion. The release's check of what it published is release verification,
not a test: `scripts/release/smoke.sh`, which runs every check in
`scripts/release/smoke/`.

A compile error or a failing fast test is found locally, not in CI: `make
test` runs before the commit, and `.githooks/pre-push` refuses a tree it did
not pass on (a documentation-only change excepted). The hooks are on once per
clone: `git config core.hooksPath .githooks` ("Starting a feature"). Every job
has a timeout, set a little above its longest run, so a hang is stopped
rather than billed.

## Billed minutes

GitHub bills every job rounded up to a whole minute, plus its set-up. So each workflow has the fewest jobs its environments allow: `release-candidate.yml` runs `test`, `stage` and `publish`; `release.yml` runs `prepare`, `deploy` and `finish`, and `unreserve` only after a failure; `pr-policy.yml` and `promote.yml` run one job each. There is no dry run: `prepare` refuses before anything changes. A failure is found locally first: the fast suite runs before every push.

## CI

One workflow runs on a pull request, and nothing runs on a push.
`pr-policy.yml` runs the policy checks, not tests: the pull request's title
(`scripts/policy/subject.sh`) and empty body, that the title names an open
issue, the tree's issue references (`scripts/policy/refs.sh`, from the base
branch), and the attribution scan, all steps of one job, `pr policy`, the
required check on `staging`; the branch need not be up to date to merge.

The full suite runs in the **release-candidate** (`release-candidate.yml`),
dispatched by hand on `staging` for its tip once the maintainers have graded
it and approved it:

    gh workflow run release-candidate.yml --ref staging

It runs three jobs, each its steps one after another on one runner:

| Job | Environment | What it does |
|---|---|---|
| `test` (`test-all.yml`) | none | The gate, then the full suite's code tests (below). |
| `stage` | `release` | Reports the release credentials; builds what a release ships — both daemons with their data and packages, the images and their manifest list, and the Windows clients with their installer and MSIX — each checked by `make test-all` (PART=packages, image, windows); stores the daemon trees, the Windows binaries and the images by commit. |
| `publish` | `release` | Reads the stored build back and verifies it, tags the commit `release-N-rcK`, tags the image, publishes the prerelease, and copies all of it to the public repository. |

The gate refuses any ref but staging's tip, a commit already released or
already a release-candidate, and one `main` cannot take next; it checks the
commits since the last release (the attribution scan refuses; the commit
policy warns). Then the full suite's code tests, in the same job:

- **`make build-all PART=plain` and `make test-all PART=plain`** — the daemon
  with every feature; the source checks and actionlint; every suite that
  starts no thread, and `media`, with its timing judged; the static-link
  check; the daemon started and driven over TLS with the e2e client; every
  Linux translation unit through the release's clang under `-Werror`; the
  speech round trip; the web client in a headless Chrome and the GUI smoke on
  the web (TESTING.md §3.4); the fuzzing pass.
- **then PART=threads** — the suites that start threads, under
  ThreadSanitizer, their one run (TESTING.md §2.4).

Only when both pass does `stage` build anything. Everything runs on Linux,
the Windows packaging included (NSIS, makemsix). Every vendored library and
toolchain is cached by the script that builds it, one cache per library keyed
by its version, its own script and the compiler that builds it, so a change to
one script rebuilds that library alone; a cache is saved as soon as the step
that builds it has run, and a cache unused for seven days is evicted.
`tools/toolchains.sh` says, when run by hand, whether a pinned toolchain is
behind upstream.

**A failure there holds back the release-candidate.** Find the commit, fix it
on a branch like any other change, and dispatch the release-candidate again on
the new tip.

See [TESTING.md](./TESTING.md) for the two suites.

**A library the daemon links needs its licence notice.** A component added to
the daemon's build under `third_party/` needs an `emit` line in
`packaging/licenses.sh`, under the same `TTS`, `STT` or `SUM` condition the
build uses, so the notice travels with every binary that links it.
`scripts/test/fast/notices.sh`, one of the source checks both suites run, compares the
two for all eight combinations and names any component without one.

**Every network call retries a failed connection.** Every workflow sets `BASH_ENV` to `.github/net-retry.bash`, so every bash step, and every bash script it runs, gets it: `git`, `gh` and `ssh-keyscan` (GitHub); `skopeo`, `docker`, `buildah` and `flyctl` (Fly's registry, API and builder); `curl`; and `dotnet restore`, while `rclone`, `npm` and NuGet are told through their own settings to retry harder and give up on a stall. Each attempt is bounded, so a stalled connection fails instead of hanging, and a call that failed in the connection (a timeout, a dropped, reset or refused connection, a 5xx) is retried with backoff, up to four times. A real refusal, such as a rejected push, a missing ref or image, a 4xx or a failed health check, fails at once.

## Releases

The maintainers cut releases; a contributor does nothing for one. A
release-candidate runs the full suite on the `staging` tip, builds and stores
what a release ships, by commit, and tags it `release-N-rcK`, published as a
prerelease; a release takes one release-candidate and publishes the same
binaries, numbered N, to apt, dnf, the container registries and the GitHub
release (the tarballs and the Windows installer), appends a `Release N`
snapshot to `main`, closes the issues it shipped, those of the
documentation it carries included, and sends the list of them, each as filed,
to the control plane for the release notes (`release-candidate.yml`,
`release.yml`; its `finish` job sends to the
address the `CONTROL_PLANE_URL` repository variable names). Each release-candidate, release and
promotion is then copied to the public repository (`openchime/openchime`): its
`staging`, the tag, the GitHub release and `main`, fast-forward only. Between releases, `promote.yml`
appends `Documentation` snapshots to `main`. No compiler and no test runs
after the release-candidate. A build is identified by its commit and a release by its number;
[UPGRADING.md](./UPGRADING.md) says how the two relate and how a self-hoster
upgrades. A change that alters what an operator must do on upgrade adds a
section to UPGRADING.md in the same pull request.

A release takes one release-candidate, by its tag:

    gh workflow run release.yml --ref release-N-rcK

| Job | Environment | What it does |
|---|---|---|
| `prepare` | `release` | Refuses any ref but a release-candidate's tag; resolves it to its commit and number; checks every credential and the public tracker's token; fetches the stored build and verifies it; checks the number is the next one and that `main` can take the commit; then, last, reserves the number as the `release-N` tag. |
| `deploy` | `release` | Packages the build under N (`.deb`, `.rpm`, tarballs), builds the Windows installer and MSIX (signed when configured), moves `main`, publishes the apt and dnf repositories to both buckets, the GitHub release and the images, then verifies what it published (`scripts/release/smoke.sh`). |
| `finish` | `release` | Closes the shipped issues, sends the release notes, removes the release-candidates of N, and copies `main`, `staging`, the tag and the release to the public repository; a failure in any part is reported and the rest still run. |
| `unreserve` | `release` | Only after a failure before anything was published: returns the number. |

There is no dry run. Everything a release must be is checked in `prepare`
before the number is reserved, so a release that cannot go ahead stops there
with nothing changed; one that fails later, before the apt index is written,
gives its number back, and one that fails after it is finished by re-running
its failed jobs. The Store's `.msix` is the run's `windows-x64` artifact.

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
