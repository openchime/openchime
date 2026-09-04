# Releasing

How `release.yml` publishes a version, and the things about it that are not
obvious from reading the file.

## Shape

    guard ────┐
    test ─────┼─▶ version ─┬─▶ packages (amd64, arm64) ─┐
    preflight ┘            │                            │
                           ├─▶ image                    ├─▶ publish ─┬─▶ winget ─▶ smoke
                           └─▶ windows-build ─▶ windows-package      │
                                                        └─▶ unreserve (on failure)

`test` is `ci.yml` reused, not re-implemented: a release that ran a weaker suite
than CI would be worse than no gate, because it would look like one. `guard` is
the attribution guard, gated via `workflow_call`.

## Verification is terminal, and publishing is not interrupted by it

`smoke` — install the daemon from the repository that was just published — is
the last job, not a step in the middle of `publish`.

It used to be a step, sitting between writing the repositories and finishing the
release. That made a postcondition abort the steps after it: a failed smoke
skipped the GitHub release, the tarballs, `:latest` and the `winget`
submission, while apt and dnf were already live. Packages published with nothing
naming them is the worst reachable state, and it needed a manual repair twice.

Once the apt index is written the release is irreversible, so **completing it is
strictly better than stopping half way**. A red run that says "released, and the
public URL does not serve it" is true, actionable, and leaves nothing to fix by
hand. `skip_repo_smoke` still turns the check off; the default is still a hard
failure, for the reason below.

## The control plane serving `/dist` is an external dependency

`DIST_BASE_URL` is `https://openchime.io/dist`, and the **control plane** serves
it by reading the bucket. Nothing in this repository can make that true, and it
has not been true for any release so far: `openchime.io` answers, but `/dist/*`
returns the application's own 404 — the same body as any unrouted path — so the
route does not exist in the deployed build.

The smoke therefore fails by default, deliberately. Auto-detecting the outage
would mask a genuinely broken repository once serving is live.

What `preflight` adds is only *timing*: one HEAD request says at minute one what
the smoke would otherwise say at minute nine, after every build has run. It
**warns and continues** — failing there would make releases impossible until an
external dependency lands, and a release is perfectly publishable without it.
Only the public URL is unreachable.

## The credentials are checked before anything is built

`preflight` asserts that every required secret and variable is set, and it takes
seconds. Each one used to be read only at its point of use, and every step that
reads one is skipped on a dry run — so the first time a missing value could
surface was a real release, after the CI gate, both package builds, the image
and the Windows binaries had all run.

It gates **`version`**, not merely `publish`, and that is the point: the release
number is reserved before anything is built and only returned when nothing was
published, so a run that cannot possibly publish would otherwise burn a number
for nothing.

A **dry run reports and continues** rather than failing. Its job is to prove the
build, and failing it would stop anyone verifying one on a fork with no secrets —
but surfacing the gap in a dry run is exactly what was impossible before.

Authenticode signing is checked separately because it is optional (see *Windows
signing is optional* below). The state worth naming is a **partial** trio: `SIGN`
requires all three values, so one or two set reads as configured and silently
ships unsigned. Preflight warns on that specifically.

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
release, no `:latest`. One guard sharpens that: `publish` sets an output the
moment the apt index lands, and `unreserve` requires it to be **unset** —
returning a number is only safe while nothing has been published under it, and
a failure *after* the repositories go live must burn the number rather than
hand it back.

`publish` therefore **asserts** the tag exists rather than creating it. If that
assertion ever fires, something deleted the reservation mid-run.

## `:latest` moves last

`image` needs only `version`, so it runs beside `windows-package`. It builds one
image per architecture with **buildah** on a native runner of that architecture;
a separate `image-manifest` job then publishes the `:VERSION` manifest list that
indexes them. The per-architecture tags (`:VERSION-amd64`, `:VERSION-arm64`) are
an implementation detail — nothing is documented as pullable until the manifest
exists.

`:VERSION` **only**. If `:latest` moved there and a later job failed, `publish`
would be skipped and `:latest` would point at a version with no packages, no tag
and no release — anyone pulling `:latest` would be on a build that officially
does not exist.

`publish` moves it at the end with `skopeo copy --all`, which re-points the tag
registry-side without pulling, preserving the digest. `--all` copies the whole
manifest list rather than just the runner's architecture.

## The apt/dnf pool guard

Both repositories are regenerated from the **full** pool, not from this release's
packages alone, or the index silently drops every prior release.

The pool is fetched with `rclone`, and the fetch must not swallow failure: a
`|| true` cannot distinguish "prefix does not exist yet" from "credentials
rejected" or "endpoint returned 503" — on any failure the pool reads as empty,
the index is built from one release, and the upload replaces
`Release`/`InRelease` in the bucket. The old objects survive; the index naming
them does not.

So it is two independent checks: `rclone lsjson` proves the prefix is reachable,
and the **tag count** is a second witness of what the pool should contain. An
empty pool with prior release tags is a hard stop.

Verified against a real Tigris endpoint, four cases:

| case | result |
|---|---|
| prefix absent, no prior releases | pass (first publish) |
| prefix absent, prior releases exist | **fail** |
| valid endpoint, bad credentials | **fail** |
| prefix populated, prior releases exist | pass |

The third case is the one the guard exists for: swallowed, it publishes a
one-version index.

## Object storage

Tigris, provisioned through Fly (`fly storage create`) and billed on the Fly
account, so it is not a separate vendor relationship. It speaks S3; `rclone` is
configured from four values:

| name | kind | value |
|---|---|---|
| `DIST_S3_ACCESS_KEY` | secret | |
| `DIST_S3_SECRET_KEY` | secret | |
| `DIST_S3_ENDPOINT` | variable | `https://fly.storage.tigris.dev` |
| `DIST_BUCKET` | variable | `openchime-dist-prod` |

**The control plane holds a different key, and a read-only one.** It serves the
bucket at `openchime.io/dist` by reading it, with a pair scoped by IAM policy to
`s3:GetObject` and `s3:ListBucket` on this bucket alone -- verified by what it
cannot do: a `PutObject` returns `403`, as does a read of any other bucket in the
organisation. The pair here is the write half and is not shared with it.

That key is issued through Tigris's IAM-compatible API at `iam.storage.dev`
(`CreateAccessKey`, `CreatePolicy`, `AttachUserPolicy`), not through the console.
`flyctl storage` has no key management -- create, list, status, update, destroy,
and nothing else -- and `create` refuses a bucket that already exists, which is
what makes the IAM API the only scriptable route.

**Both sides sit in the `openchime` Fly organisation**, and must. A Tigris key is
scoped to the organisation that owns the bucket, so a key issued elsewhere is
refused with `403 AccessDenied`. Note what that does *not* mean: the caller's own
organisation is irrelevant, and this workflow proves it every release by writing
here from a GitHub-hosted runner that belongs to no Fly organisation at all.
Authorization follows the key, never the caller.

**The keys cannot be read back** from GitHub or Fly, and `destroy && create`
would throw the repositories away. Rotation goes through Tigris's IAM API
instead: `CreateAccessKey` for the replacement, set it in both places, then
`DeleteAccessKey` on the old one. Two undocumented details, learned by doing it
-- a new key starts with **no** permissions rather than inheriting any, and
`AttachUserPolicy` takes the access key **ID** as its `UserName`, not the
friendly name, which otherwise fails with `Access key not found`.

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
`RestrictAddressFamilies` that nothing at install time parses strictly: both
maintainer scripts gate the enable path on `[ -d /run/systemd/system ]`.

The release runs `systemd-analyze verify` against it, directly on the runner —
which boots systemd for real, so no container is needed to have something that
parses the unit.

Three things about the check are measured, not assumed:

1. **`systemd-analyze verify` exits 0 for a directive it does not recognise.** Its
   status is worthless; the output must be grepped.
2. **The pattern is `Unknown key`, not `Unknown key name`.** systemd words it
   `Unknown key 'ProtectSystm' in section [Service], ignoring.` — match the
   wrong phrase and a typo'd `ProtectSystem` sails through, the exact bug the
   step exists to catch.
3. **The output must be scoped to our unit before it is matched.** `verify` walks
   the dependency graph and reports on everything it loads. In the container that
   was only `openchimed.service`; on the runner it is also the runner's own
   units, and `ubuntu-22.04` ships two that trip the patterns above —
   `snapd.service` has an `Unknown key name 'RestartMode'`, and
   `netplan-ovs-cleanup.service` gives `Failed to open ...: Permission denied`.
   Grepping for `openchimed`
   first is what makes the check about us; every message that concerns a unit is
   prefixed with its name or its path and line.

Tested against three fixtures: real unit passes, typo'd key fails, bad `Type=`
value fails.

## Install coverage, and the rpm gap

A package which builds but does not install is precisely the failure a release
pipeline exists to catch, and only an install finds it. Coverage today, without
containers (ARCH-36):

- **The `.deb`** is installed on the runner itself and the binary is executed —
  Ubuntu rather than Debian, and a runner that is not pristine, but it proves
  the package unpacks, its dependencies resolve, and what it lays down runs.
- **The `.rpm` is never installed by anything.** Nothing on a hosted runner can
  install one. Between the identical-binary check and the apt repository smoke
  test, the *binary* and the *signing and object layout* are both covered — but
  nothing exercises `dnf`, so an rpm that builds and signs correctly yet fails to
  install on the RHEL family will not be caught before it is published.
- **The published-repository smoke test has no dnf half** for the same reason.
  The apt half runs on the runner and shares the signing key and object layout
  with the rpm repository, so faults in those still surface there.

The rpm gap is tracked as an open issue rather than treated as settled.

## Windows signing is optional

`SIGN` requires a real release **and** all three `TRUSTED_SIGNING_*` secrets to
be non-empty.

The second half is load-bearing: gating on a real release alone would invoke
`azure/trusted-signing-action` against an empty endpoint whenever credentials
are absent. That fails `windows-package`, and `publish` needs it — so with no
certificate a release would publish **nothing at all**: not the Windows
installer, which cannot be signed, but also not apt, dnf, the image or the
GitHub release, none of which needs a certificate.

Without the secrets the release ships an unsigned installer and says so twice: a
`::warning::` plus step summary in the job, and a paragraph in the release notes
for whoever meets SmartScreen. Setting the three secrets turns signing back on
with no edit.

## Downloads are pinned

`wingetcreate` is pinned to a versioned asset with a `Get-FileHash` check —
`https://aka.ms/wingetcreate/latest` is a mutable redirect, and the tool runs
with `WINGET_TOKEN` in scope. Bump `WINGETCREATE_VERSION` and
`WINGETCREATE_SHA256` together; the hash is Microsoft's own, published beside the
asset as `wingetcreate.exe.txt`.

Same rule as `build_mbedtls.sh`, which refuses to fetch without a known SHA-256.

## The WinGet fork syncs itself

`wingetcreate` pushes a branch to a fork of `microsoft/winget-pkgs` under the
token's account, and refuses outright when GitHub will not fast-forward that
fork. Upstream takes hundreds of commits a day and ours is touched once per
release, so the fork falls thousands of commits behind between releases — 10,650
when release 7 measured it. Releases 6 and 7 both died at this one job, with
every other channel already published.

The `winget` job now runs `gh repo sync` against the fork immediately before
submitting. It is unconditional and needs no judgement, because the fork carries
nothing of ours: each submission is a branch that lives only until its PR merges
upstream, so the default branch is always 0 ahead. The step logs the drift
before and after, and cannot fail the release — if the sync really did not work,
`wingetcreate` fails with the message it always did.

## Secrets and variables

| name | kind | required for |
|---|---|---|
| `DIST_S3_ACCESS_KEY` / `DIST_S3_SECRET_KEY` | secret | apt + dnf publish |
| `REPO_SIGNING_KEY` | secret | signing the indexes and packages |
| `WINGET_TOKEN` | secret | the WinGet submission PR |
| `TRUSTED_SIGNING_ACCOUNT` / `_ENDPOINT` / `_PROFILE` | secret | Authenticode; **optional** |
| `DIST_S3_ENDPOINT` / `DIST_BUCKET` | variable | rclone configuration |

`GITHUB_TOKEN` covers GHCR; nothing extra is needed for the image. It is passed
to `buildah login` and `skopeo --creds`.

## Dry runs

`dry_run: true` builds and verifies everything and withholds exactly two things:
the tag and the GitHub release. Note what that means for testing — a dry run
**cannot** exercise the version reservation, the `:latest` move or the WinGet
submission, because those are the parts it skips. The first real release is the
first test of them.
