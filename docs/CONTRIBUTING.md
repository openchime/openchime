# OpenChime — Contributing & Workflow

The branch, commit, and CI policy for this repo. The private control-plane repo
(`openchime-saas`) follows the same rules, tailored to its .NET CI.

## Branches & merging

- **Trunk-based.** `main` is always releasable and kept **linear**.
- **Non-trivial changes** (any code, or structural/multi-file docs) happen on a
  **short-lived branch** named descriptively in kebab-case for the change:
  `message-management`, `tuikit-phase-4`.
- **Docs-only changes** may go straight to `main` (they skip CI via
  `paths-ignore`) or use a branch — either is fine.
- **Gate merges on CI.** Push the branch, **wait for its CI to pass**, then
  **fast-forward merge** to `main`. Because a fast-forward puts the *identical*
  commit on `main`, a green branch keeps `main` green.
- **Fast-forward only** (no merge commits) → linear history. After merging,
  **delete the branch** (local + remote).
- **PRs are optional** — use one for a review record or branch protection; merge
  only after checks pass.
- **One logical change per branch.**

## Commits

- `area: imperative summary` — sentence case, **no trailing period**. The prefix
  names the area/subsystem (`TUI:`, `GUI:`, `docs:`, `daemon:`, …), not a fixed
  type set.
- Cite decision ids inline where relevant: `ARCH-N`, `REQ-N`.
- **Never** add a `Co-Authored-By:` or any attribution trailer naming Claude /
  Anthropic. The [`attribution-guard`](../.github/workflows/attribution-guard.yml)
  workflow scans author, committer, and message on every push and **rejects**
  matches (it force-resets `main` / deletes the offending branch).

## CI

- **`build`** — native gcc build + `make test` (unit + in-process integration,
  including the headless client app-core test).
- **`integration`** — the deployed image end-to-end on the Docker Compose stack
  (health check + the protocol vertical over TLS).
- **`core`** — standalone compile check of the client app-core (ARCH-74).
- **`attribution-guard`** — runs on every push.
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
