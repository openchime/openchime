# OpenChime — Contributing & Workflow

The branch, commit, and CI policy for this repo. The private control-plane repo
(`openchime-saas`) follows the same rules, tailored to its .NET CI.

## Branches & merging

- **`staging` is the integration branch.** All feature work lands on `staging`
  through a pull request; `main` receives staging's history when it is
  promoted. *(This supersedes the trunk-on-`main` flow this file used to
  describe. The old text outlived the practice, and a session followed it
  faithfully into the wrong branch — which is exactly the failure a stale
  workflow document produces, and why the change is now written down.)*
  Promotion of `staging` to `main` is not yet mechanised; until it is, nothing
  lands on `main` directly except docs-only changes.
- **Every feature has exactly one GitHub issue**, and its branch is named for
  it: `feature/oc-<issue number>-<short-kebab-description>`, cut from a clean,
  current `staging`. The repo's `/oc-feature-start` command runs the preflight
  (clean tree, `staging` in sync with origin, no branch already carrying the
  issue) and refuses rather than repairs.
- **Land by pull request to `staging`,** squashed to a **single** house-format
  commit whose message cites the issue — `Closes` plus its number when the
  merge should close it. Note GitHub auto-closes only from the default branch,
  so closing the issue on a merge to `staging` is a deliberate act, done with a
  short comment saying what landed. The PR body stays **empty**: the issue and
  the commit already say everything. `/oc-feature-pr` runs the sequence: tests
  clean with zero warnings, squash, push (`--force-with-lease` after a squash
  is the one acceptable force-push, and only ever on a `feature/*` branch),
  raise the PR.
- **Gate merges on CI.** Merge only when every check on the pull request is
  green, with a **rebase merge** — no merge commits, history stays linear.
  Delete the branch, local and remote, after.
- **Docs-only changes still go straight to `main`** (they skip the build jobs
  via `paths-ignore`; the attribution guard runs on every push regardless).
- **Sign off every commit** (`git commit -s`) — the DCO applies to every path
  into the tree, including a cherry-pick.
- **One logical change per branch.**

## Commits

- `area: imperative summary` — sentence case, **no trailing period**. The prefix
  names the area/subsystem (`TUI:`, `GUI:`, `docs:`, `daemon:`, …), not a fixed
  type set.
- Cite decision ids inline where relevant: `ARCH-N`, `REQ-N`.
- **A commit that resolves an issue cites it** — its number, or `Closes` plus
  its number when the merge should close it. Issues are the project's only issue list; the
  numbers are stable identifiers, unlike the positional numbering of the
  markdown issue list they replaced (migrated 2026-08-19).
- **No file cites an issue number — a commit message is the only place one
  belongs.** Not source, not scripts, not the workflows, not the documents.
  Comments explain themselves, or cite a `REQ-N` / `ARCH-N`: REQUIREMENTS.md and
  ARCHITECTURE.md are stable documents living in this repository, and their ids
  identify rather than position. An issue number in a file is a pointer with no
  integrity — it rots the moment the issue is closed, renamed, merged or
  superseded, and nothing in the file can tell the reader that it has. The
  markdown backlog these issues replaced was the stronger case: its numbers were
  *positions*, so every citation drifted whenever an item above it closed.

  Instead of pointing at an issue, say the thing: "the daemon exits 0 when it
  cannot start, which systemd reads as success". The prose survives the tracker.

  **This is enforced.** `make check-refs` (`scripts/check_refs.sh`) fails on a
  hash followed by digits, on a tracker URL ending in a number, on the retired
  per-item tags of the old markdown list, and on any mention of that list by
  name. It is a prerequisite of `make test` and its own step in CI's build job.
  A bare link to the tracker carrying no number is fine — and note the rule
  applies to this file too, which is why the patterns above are described rather
  than written out.
- **Never** add a `Co-Authored-By:` or any attribution trailer naming Claude /
  Anthropic. The [`attribution-guard`](../.github/workflows/attribution-guard.yml)
  workflow scans author, committer, and message on every push and **rejects**
  matches (it force-resets `main` / deletes the offending branch).

## CI

- **`build`** — native gcc build + `make test` (unit + in-process integration,
  including the headless client app-core test), plus the two source checks:
  `make check-opcodes` (no two message types share an opcode) and
  `make check-refs` (no file cites an issue by number).
- **`integration`** — the daemon end-to-end, started natively on the runner
  (health check + the protocol vertical over TLS). It used to drive the deployed
  image on a Docker Compose stack; the project no longer uses Docker anywhere,
  and there is no local equivalent of this job (docs/TESTING.md §3.2).
- **`core`** — standalone compile check of the client app-core (ARCH-74).
- **`windows`** — cross-compiles the Windows TUI + GUI (`make windows-tui windows-gui`).
- **`guard`** — the job in the separate [`attribution-guard`](../.github/workflows/attribution-guard.yml)
  workflow. Unlike the four above it has **no `paths-ignore`**, so it runs on
  every push including docs-only ones — which is the point, since the thing it
  rejects lives in commit messages and author lines. It is also the **required
  status check** on `main`, so a direct docs push to `main` reports a bypass
  until it reports green.
- Docs-only pushes skip the build jobs (`paths-ignore: ['**.md', ...]`).

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
