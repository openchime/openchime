# Releasing

How `release.yml` publishes a version, and the things about it that are not
obvious from reading the file. Written after the pipeline's first real run, so
it records what was learned by executing it rather than what was intended.

## Shape

    guard ─┐
           ├─▶ version ─┬─▶ packages (amd64, arm64) ─┐
    test ──┘            │                            │
                        ├─▶ image                    ├─▶ publish ─▶ winget
                        └─▶ windows-build ─▶ windows-package
                                                     └─▶ unreserve (on failure)

`test` is `ci.yml` reused, not re-implemented: a release that ran a weaker suite
than CI would be worse than no gate, because it would look like one. `guard` is
the attribution guard, gated via `workflow_call`.

## The release number is reserved, not assigned at the end

`version` allocates N from the tags and **pushes `release-N` immediately**,
before anything is built.

This is the opposite of the control plane, which tags after deploying, and the
asymmetry is deliberate. Here the number is baked into `.deb` and `.rpm`
filenames and into the apt/dnf indexes: if the tag push were last and it failed,
release N would already be live and untagged, and the next run would compute N
again, build different content and publish it under the same version. Two `.deb`s
sharing a version is the one thing apt cannot recover from.

Reserving inverts the failure: the worst case is a **burnt number**, which costs
nothing. The terminal `unreserve` job deletes the tag when `publish` did not
succeed, which is safe precisely because nothing names it — no index, no GitHub
release, no `:latest`.

`publish` therefore **asserts** the tag exists rather than creating it. If that
assertion ever fires, something deleted the reservation mid-run.

## `:latest` moves last

`image` needs only `version`, so it runs beside `windows-package`. It pushes
`:VERSION` **only**. If `:latest` moved there and a later job failed, `publish`
would be skipped and `:latest` would point at a version with no packages, no tag
and no release — anyone running Compose against `:latest` would be on a build
that officially does not exist.

`publish` moves it at the end with `docker buildx imagetools create`, which
re-points the tag registry-side without pulling, preserving the digest.

## The apt/dnf pool guard

Both repositories are regenerated from the **full** pool, not from this release's
packages alone, or the index silently drops every prior release.

The pool is fetched with `rclone`, and the fetch used to end in `|| true`. That
cannot distinguish "prefix does not exist yet" from "credentials rejected" or
"endpoint returned 503" — on any failure the pool was empty, the index was built
from one release, and the upload replaced `Release`/`InRelease` in the bucket.
The old objects survive; the index naming them does not.

It is now two independent checks: `rclone lsjson` proves the prefix is reachable,
and the **tag count** is a second witness of what the pool should contain. An
empty pool with prior release tags is a hard stop.

Verified against a real Tigris endpoint, four cases:

| case | result |
|---|---|
| prefix absent, no prior releases | pass (first publish) |
| prefix absent, prior releases exist | **fail** |
| valid endpoint, bad credentials | **fail** |
| prefix populated, prior releases exist | pass |

The third case is the bug: with `|| true` it passed, with `pool_count=0`, and
would have published a one-version index.

## Object storage

Tigris, provisioned through Fly (`fly storage create`) and billed on the Fly
account, so it is not a separate vendor relationship. It speaks S3; `rclone` is
configured from four values:

| name | kind | value |
|---|---|---|
| `DIST_S3_ACCESS_KEY` | secret | |
| `DIST_S3_SECRET_KEY` | secret | |
| `DIST_S3_ENDPOINT` | variable | `https://fly.storage.tigris.dev` |
| `DIST_BUCKET` | variable | `openchime-dist` |

**The control plane shares this keypair.** It only reads — it serves the bucket
at `openchime.io/dist` — but it holds write credentials, and neither side can be
revoked without breaking the other. Splitting them needs a read-only key from the
Tigris dashboard; `flyctl storage` has no key management (create, list, status,
update, destroy, and nothing else).

**The keys cannot be read back** from GitHub or Fly. `destroy && create` was the
free way to rotate while the bucket was empty; once it holds the repositories,
that throws them away. Rotation is a dashboard operation from here.

## The archive signing key

`REPO_SIGNING_KEY` signs the apt indexes (`Release.gpg`, `InRelease`), each
`.rpm` individually, and `repomd.xml`. Its public half is published to the bucket
as `openchime-archive-keyring.{asc,gpg}` and ends up in every user's
`/usr/share/keyrings/`.

    OpenChime Archive Signing Key <legal@bronzeventure.com>
    Ed25519, sign-only, no expiry
    47A4 ECF0 F41F D8E0 8480  9285 FA07 1DC4 0E2F 491A

Properties worth knowing before touching it:

- **It is effectively unrotatable.** The public half is on every installation.
  Rotating means every existing install stops trusting the repository until it
  manually re-imports.
- **It exists in two places**: the GitHub secret, and paper. There is no third
  copy and the secret cannot be read back.
- **Ed25519 signatures need rpm ≥ 4.16.** Verified working on Rocky 9
  (rpm 4.16.1.3): `Header V4 EdDSA/SHA512 Signature ... OK`. **RHEL/CentOS 8 ships
  rpm 4.14 and cannot verify them.** That is a distribution decision already baked
  into an unrotatable key — EL8 support would require a different algorithm and a
  new key.

## The unit file is validated, because nothing else validates it

`openchimed.service` is 60 lines of `ProtectSystem`, `DynamicUser` and
`RestrictAddressFamilies` that CI produces and, until recently, nothing ever
parsed: both maintainer scripts gate the enable path on `[ -d /run/systemd/system ]`,
which a plain `docker run` container does not have.

A third clean-room container installs systemd and runs `systemd-analyze verify`.
Two things about that check are measured, not assumed:

1. **`systemd-analyze verify` exits 0 for a directive it does not recognise.** Its
   status is worthless; the output must be grepped.
2. **The pattern is `Unknown key`, not `Unknown key name`.** Bookworm words it
   `Unknown key 'ProtectSystm' in section [Service], ignoring.` A first draft
   matching `Unknown key name` passed a unit with a typo'd `ProtectSystem` — the
   exact bug the step exists to catch.

Tested against three fixtures: real unit passes, typo'd key fails, bad `Type=`
value fails.

## Windows signing is optional

`SIGN` requires a real release **and** all three `TRUSTED_SIGNING_*` secrets to
be non-empty.

The second half is load-bearing. It used to be `dry_run == false` alone, so a
real release invoked `azure/trusted-signing-action` against an empty endpoint
whether or not credentials existed. That fails `windows-package`, and `publish`
needs it — so with no certificate the first real release would have published
**nothing at all**: not the Windows installer, which cannot be signed, but also
not apt, dnf, the image or the GitHub release, none of which needs a certificate.

Without the secrets the release ships an unsigned installer and says so twice: a
`::warning::` plus step summary in the job, and a paragraph in the release notes
for whoever meets SmartScreen. Setting the three secrets turns signing back on
with no edit.

## Downloads are pinned

`wingetcreate` is pinned to a versioned asset with a `Get-FileHash` check.
`https://aka.ms/wingetcreate/latest` is a mutable redirect, and it was being
executed with `WINGET_TOKEN` in scope. Bump `WINGETCREATE_VERSION` and
`WINGETCREATE_SHA256` together; the hash is Microsoft's own, published beside the
asset as `wingetcreate.exe.txt`.

Same rule as `build_mbedtls.sh`, which refuses to fetch without a known SHA-256.

## Secrets and variables

| name | kind | required for |
|---|---|---|
| `DIST_S3_ACCESS_KEY` / `DIST_S3_SECRET_KEY` | secret | apt + dnf publish |
| `REPO_SIGNING_KEY` | secret | signing the indexes and packages |
| `WINGET_TOKEN` | secret | the WinGet submission PR |
| `TRUSTED_SIGNING_ACCOUNT` / `_ENDPOINT` / `_PROFILE` | secret | Authenticode; **optional** |
| `DIST_S3_ENDPOINT` / `DIST_BUCKET` | variable | rclone configuration |

`GITHUB_TOKEN` covers GHCR; nothing extra is needed for the image.

## What the first real release found

It took **four attempts**, and every failure was in the release machinery rather
than in anything being released. Every build, on every platform, was green from
the first attempt onward. Recorded in order, because the pattern matters more
than any single fault: each one was invisible to review and obvious the moment
the pipeline ran.

**Attempt 1 — the pool guard counted its own reservation.** `version` reserves
`release-N` before anything is built; the guard counts `release-*` tags to tell
"first publish" from "the fetch failed". On a first release those meet — pool
empty, one tag — and it refused itself. Both mechanisms correct alone, wrong
together. The count now excludes the current version.

**Attempt 2 — the corrected count killed its own step, silently.** Once this
release's tag is filtered out, a first release leaves `grep -vx` with no match.
grep exits 1, `pipefail` propagates, `set -e` aborts the assignment. The step
died in under a second having printed nothing at all: no notice, no error. This
is the same defect as the dead error path in `winget/render.sh`, fixed hours
earlier and reintroduced here — a no-match grep is not an error, but under
`set -euo pipefail` it reads as one.

**Attempt 3 — apt and dnf published, then the release step could not find the
installer.** `no matches found for dist/windows-x64/*-setup.exe`. It had been
built and uploaded correctly: `pattern: windows-*` matches exactly one artifact
and unpacked it flat into `dist/`, while `pattern: linux-*` matches two and nests
each in a named directory. The windows artifact is now downloaded by name.

That attempt exposed a second, worse fault. It failed *after* the repositories
were live, and `unreserve` dutifully handed the number back — leaving release 1
visible to apt clients with no tag naming it. That is the published-but-untagged
state reserving the number exists to prevent, reached from the other direction.
`publish` now sets an output the moment the apt index lands, and `unreserve`
requires it to be unset. **Returning a number is only safe while nothing has been
published under it.**

**Alongside all this — a docs-only pull request could not merge at all.**
`ci.yml`'s `pull_request` trigger carried `paths-ignore: '**.md'` while those four
jobs are required status checks, and a job that never triggers never *reports*,
so the PR waited forever on a check that would not run. Found by opening one to
write this file. `paths-ignore` stays on `push` and is gone from `pull_request`.

**Attempt 4 — green.** Tag, GitHub release with six assets, apt and dnf live and
smoked, `:latest` moved to `1`, and the WinGet pull request opened.

The lesson worth keeping: **every one of these fixes was reviewed and statically
sound, and three of the four faults were interactions between two correct
changes.** No amount of re-reading either half would have surfaced them. A dry
run would not have either — it withholds exactly the steps that failed.

## Dry runs

`dry_run: true` builds and verifies everything and withholds exactly two things:
the tag and the GitHub release. Note what that means for testing — a dry run
**cannot** exercise the version reservation, the `:latest` move or the WinGet
submission, because those are the parts it skips. The first real release is the
first test of them.
