# Releasing

How a staging commit becomes a **candidate** (`candidate.yml`) and a candidate
becomes a **release** (`release.yml`), and the things about both that are not
obvious from reading the files.

## Build id and release number

A build is identified by its **build id**: the short commit it was built from
(`74359c0`). The candidate build compiles it into the daemon, the web client
the daemon serves, and both Windows clients (`make OC_BUILD_ID=...`, through
`shared/build_id.h`), and it is what `openchimed --version`, the startup log,
`openchime-tui --version`, the Windows file properties (ProductVersion
`build 74359c0`) and the GUI's About OpenChime report. About shows the daemon's
too, which the daemon sends each client at sign-in (PROTOCOL.md `SERVER_BUILD`).
Nothing about a release number is in a binary.

A **release number** is a label assigned to one build id when it is released:
`release-23` is build `74359c0`. It appears only where a public monotonic
version is required: the deb and rpm version fields and filenames, the tarball
and installer filenames, the installer's and the MSIX's version, the image tags
`:23` and `:latest`, the `release-23` tag and the GitHub release, whose notes
name the build id. The tag on the commit is the map from one to the other.

Windows wants a numeric, growing file version. A commit is neither, so the
file version is `1.0.<run>.0`, the run number of the candidate build that made
the files, recorded in the candidate's manifest; the release's installer check
reads it from there.

## Candidates: built on staging, stored by build id

Every push to `staging` runs `candidate.yml`, which waits for the push's CI,
attribution-guard and commit-policy runs to pass and then builds everything a
release ships, exactly as a release used to build it: both daemons with their
data (zig against glibc 2.34, the arm64 one cross-built), the images (buildah,
started with podman and asked `/healthz`), the Windows clients (llvm-mingw).
The packaging scripts run under a placeholder version so a package that cannot
be built, installed or made identical across channels stops the candidate; the
packages themselves are not kept.

What is kept, under `candidates/<build-id>/` in the staging bucket
(`DIST_STAGING_BUCKET`, via the `dist-staging` rclone remote):

    linux-amd64/openchimed-build-<id>-linux-amd64.tar.gz   the binary, tts/, stt/, web/, copyright
    linux-arm64/openchimed-build-<id>-linux-arm64.tar.gz
    windows-x64/openchime.exe, openchime-tui.exe, openchime.debug
    manifest.json, manifest.json.asc

The manifest (`scripts/release/candidate_manifest.sh`) names the build id, the
full commit, the run number and id, every file's sha256 and size, and the
digests of the images the candidate pushed to GHCR as `:build-<id>-amd64`,
`:build-<id>-arm64` and the manifest list `:build-<id>`. It is signed with the
archive signing key. The same files go up as a workflow artifact,
`candidate-<id>`, for anyone who wants to look. Candidates older than thirty
days that no `release-*` tag names are pruned.

One candidate per commit. `make RELEASE=1 ARCH=aarch64 ... OC_BUILD_ID=<id>`
is the same command a developer runs, so a candidate is reproducible by hand.

## Starting a release

A release is a **promotion**. Run

    gh workflow run promote.yml --ref staging

and `promote.yml` does, in order:

1. refuses unless it was started on `staging`;
2. runs the full CI suite on that exact staging commit;
3. confirms the attribution guard and commit policy passed on the push that put
   the commit on `staging`, that `staging` has not moved on since, that
   `main` is its ancestor, and that **a candidate for the commit is stored**
   (the one way there is none is a promotion started before the candidate
   build finished: wait for it, then promote again);
4. fast-forwards `main` to it with the repository's deploy key (`RELEASE_SSH_KEY`
   in the `release` environment).

That push to `main` **is** the trigger: `release.yml` runs on it and publishes.
A ruleset on `main` refuses every update except from that deploy key, so every
push to `main` is a promotion, and `main` always names what was released. A
deploy-key push starts workflows natively; no personal token is involved.

`promote.yml` takes `dry_run`, which does everything except move `main`. A dry
run of the release itself is `gh workflow run release.yml --ref staging -f
dry_run=true`: it fetches staging's own candidate for that commit, packages it
under the next number and publishes nothing. `release.yml` otherwise runs on
`main` only, whether pushed or dispatched — its first job, `branch`, refuses
anything else, and a dry run on any ref but `staging` or `main`.

## Shape

`candidate.yml`, on every push to staging:

    branch ─▶ id ──────┬─▶ packages (amd64, arm64) ─▶ image ─▶ image-manifest ─┐
          ├─▶ gate ────┤                                                       ├─▶ store ─▶ prune
          └─▶ preflight└─▶ windows-build ─▶ windows-package ───────────────────┘

`release.yml`, on the promotion:

    branch ─▶ guard ──────────────────┐
          └─▶ preflight ─▶ candidate ─┴─▶ version ─┬─▶ package (amd64, arm64) ─┬─▶ publish ─┬─▶ smoke
                                                  └─▶ windows ────────────────┘            ├─▶ close-issues
                                                                                           └─▶ unreserve (on failure)

`candidate` fetches `candidates/<id>/` for `main`'s new head, verifies the
manifest's signature and every file against it, unpacks the amd64 daemon and
asks it `--version` (the id in the manifest is the id the code carries), and
checks the image `:build-<id>` is in the registry with the digest the manifest
recorded. `version` reserves the number. `package` runs the packaging scripts
under it and proves the deb, the rpm and the tarball carry the candidate's
binary. `windows` signs the candidate's exes and wraps them in the installer
and the MSIX. `publish` writes the repositories, re-tags the image, creates the
GitHub release and moves `:latest`. No compiler runs; a release takes minutes.

`guard` is the attribution guard, gated via `workflow_call`. CI is not run
again on main: the candidate exists only because CI passed on the commit on
staging, and `promote.yml` gated on it once more before moving `main`.

`close-issues` closes the issues the release shipped: every commit on `main`
names its issue in the subject, and `scripts/release/close_shipped_issues.sh` reads the
commits between the previous `release-N` and this one. An issue closes when it
ships, not when it merges. The job only ever warns — the release is already live.

## Verification is terminal, and publishing is not interrupted by it

`smoke` — install the daemon from the repository that was just published — is
the last job, not a step in the middle of `publish`.

It covers all three channels: apt on the runner, dnf in a `rockylinux:9`
container, and the container image, pulled by its released tag **with no
credentials** and run with `--version`. A GHCR package is private until it is
made public, so an image only a logged-in account can pull would otherwise pass
every release. The image check is not behind `skip_repo_smoke` — the registry
does not depend on `/dist` — and runs even when a repository step above it fails.

As a step sitting between writing the repositories and finishing the
release, it would be a postcondition aborting the steps after it: a failed smoke
would skip the GitHub release, the tarballs and `:latest`, while apt and dnf were
already live. Packages published with nothing
naming them is the worst reachable state.

Once the apt index is written the release is irreversible, so **completing it is
strictly better than stopping half way**. A red run that says "released, and the
public URL does not serve it" is true, actionable, and leaves nothing to fix by
hand. `skip_repo_smoke` still turns the check off; the default is still a hard
failure, for the reason below.

## The control plane serving `/dist` is an external dependency

`DIST_BASE_URL` is `https://openchime.io/dist`, and the **control plane** serves
it by reading the bucket. Nothing in this repository can make that true. When
the route is absent, `openchime.io` answers but `/dist/*`
returns the application's own 404 — the same body as any unrouted path.

The smoke fails in that case by default, deliberately. Auto-detecting the outage
would mask a genuinely broken repository.

What `preflight` adds is only *timing*: one HEAD request says at minute one what
the smoke would otherwise say at minute nine, after every build has run. It
**warns and continues** — failing there would make releases impossible whenever an
external dependency is down, and a release is perfectly publishable without it.
Only the public URL is unreachable.

## The credentials are checked before anything is built

`preflight` asserts that every required secret and variable is set, and it takes
seconds. Each one is otherwise read only at its point of use, and every step that
reads one is skipped on a dry run — so without it the first time a missing value could
surface is a real release, after the CI gate, both package builds, the image
and the Windows binaries have all run.

It gates **`version`**, not merely `publish`, and that is the point: the release
number is reserved before anything is packaged and only returned when nothing was
published, so a run that cannot possibly publish would otherwise burn a number
for nothing.

A **dry run reports and continues** rather than failing, and so does the
candidate build: their job is to prove the build, and failing it would stop
anyone verifying one on a fork with no secrets — but surfacing the gap is
exactly what was impossible before.

Authenticode signing is checked separately because it is optional (see *Windows
signing is optional* below). The state worth naming is a **partial** trio: `SIGN`
requires all three values, so one or two set reads as configured and silently
ships unsigned. Preflight warns on that specifically.

## The release number is reserved, not assigned at the end

`version` allocates N from the tags and **pushes `release-N` immediately**,
once the candidate is verified and before anything is packaged.

The number goes into `.deb` and `.rpm` filenames and into the apt/dnf indexes:
if the tag push were last and it failed, release N would already be live and
untagged, and the next run would compute N again, package different content and
publish it under the same version. Two `.deb`s sharing a version is the one
thing apt cannot recover from.

Reserving inverts the failure: the worst case is a **burnt number**, which costs
nothing. The terminal `unreserve` job deletes the tag when `publish` did not
succeed, which is safe precisely because nothing names it — no index, no GitHub
release, no `:latest`. One guard sharpens that: `publish` sets an output the
moment the apt index lands, and `unreserve` requires it to be **unset** —
returning a number is only safe while nothing has been published under it, and
a failure *after* the repositories go live must burn the number rather than
hand it back.

`publish` **holds** the number as its first step, before anything is written.
The tag on the run's commit is the ordinary case. The tag absent is what
re-running only the failed jobs produces — `unreserve` returned the number and
`version`, having succeeded, does not run again — so `publish` reserves it again,
provided it is still exactly the next number. A tag on another commit, or a
later release already tagged, means the run no longer owns the number, and it
stops with nothing published. The tagging step near the end still **asserts**
the tag rather than creating it; if that ever fires, something deleted the
reservation mid-job.

## The image is the candidate's, re-tagged; `:latest` moves last

The candidate build's `image` job lays each architecture's tree onto a glibc
base with **buildah** (`Dockerfile`, `packaging/image-context.sh`; no compile,
no `RUN`, so the arm64 image is built on the x86 runner with `--arch arm64` and
no emulation), starts the amd64 one with **podman** (daemonless, reading
buildah's store) and asks it `/healthz`, so a fault in the `Dockerfile`, the
entrypoint or the binary on that base stops the candidate rather than reaching
the registry (the arm64 image is build-only, as its binary is), and pushes
`:build-<id>-<arch>`; `image-manifest` pushes the manifest list `:build-<id>`.
Nothing is documented as pullable under those tags.

Everything Linux-hosted runs on one image, `ubuntu-24.04`, with every compiler
fetched pinned by make (VENDORS.md §2, "Toolchains"). The arm64 daemon is
**cross-built** there with zig (`make RELEASE=1 ARCH=aarch64`) and is build-only:
the render, the hearing check and the package install run for amd64; arm64 ships
the same sources through the same compiler, asserted for its glibc floor and
packaged, but not run.

The release re-tags with `skopeo copy`, which re-points tags registry-side
without pulling or rebuilding, preserving the digest: `:build-<id>-<arch>`
becomes `:N-<arch>`, the manifest list becomes `:N`, and only then `:N`
becomes `:latest`. If `:latest` moved earlier and a later step failed,
`:latest` would point at a version with no packages, no tag and no release.
`--all` copies the whole manifest list rather than just the runner's
architecture.

## The apt/dnf pool guard

Both repositories are regenerated from the **full** pool, not from this release's
packages alone, or the index silently drops every prior release.

The pool is fetched with `rclone`, and the fetch must not swallow failure: a
`|| true` cannot distinguish "prefix does not exist yet" from "credentials
rejected" or "endpoint returned 503" — on any failure the pool reads as empty,
the index is built from one release, and the upload replaces
`Release`/`InRelease` in the bucket. The old objects survive; the index naming
them does not.

So it is two independent checks: counting the objects under the prefix proves it
is reachable, and the **tag count** is a second witness of what the pool should
contain. An
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

Tigris, provisioned through Fly and billed on the Fly account, so it is not a
separate vendor relationship. Each environment of the control plane has its own
package bucket in its own Fly organisation, and **every release publishes to
both**: the repositories are built once, from the pool read from production's
bucket, and uploaded to each, so the two stay identical.

| name | kind | value |
|---|---|---|
| `DIST_S3_ENDPOINT` | variable | `https://fly.storage.tigris.dev` |
| `DIST_BUCKET` | variable | `openchime-packages-production` |
| `DIST_S3_ACCESS_KEY` / `DIST_S3_SECRET_KEY` | secret | publishing key for `DIST_BUCKET` |
| `DIST_STAGING_BUCKET` | variable | `openchime-packages-staging` |
| `DIST_S3_STAGING_ACCESS_KEY` / `DIST_S3_STAGING_SECRET_KEY` | secret | publishing key for `DIST_STAGING_BUCKET` |

**The operators' URL and the verification URL differ.** `DIST_BASE_URL`,
`https://openchime.io/dist`, is written into `openchime.repo` and every
operator's configuration, and is permanent. The release checks what it published
at `DIST_VERIFY_URL`, `https://staging.openchime.io/dist`, which serves the same
packages from the staging bucket; the dnf check reads the published `.repo` file
and points it there.

**Keys are issued by the control plane.** Its `deploy/storage-keys.sh` creates the
buckets, issues each key through Tigris's IAM-compatible API at `iam.storage.dev`
with a policy naming one bucket, and sets the secrets and variables above in this
repository. The control plane reads each bucket with a different, read-only key.
A new key starts with **no** permissions, and `AttachUserPolicy` takes the access
key **ID** as its `UserName`; rotation is running that script again, then
`DeleteAccessKey` on the old keys.

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

`openchimed.service` is 70 lines of `ProtectSystem`, `DynamicUser` and
`RestrictAddressFamilies` that nothing at install time parses strictly: both
maintainer scripts gate the enable path on `[ -d /run/systemd/system ]`.

The candidate build runs `systemd-analyze verify` against it, directly on the runner —
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
   units. The job runs on `ubuntu-24.04` now; when this was measured, on
   `ubuntu-22.04`, two of that image's units tripped the patterns above —
   `snapd.service` had an `Unknown key name 'RestartMode'`, and
   `netplan-ovs-cleanup.service` gave `Failed to open ...: Permission denied`.
   Grepping for `openchimed`
   first is what makes the check about us; every message that concerns a unit is
   prefixed with its name or its path and line.

Tested against three fixtures: real unit passes, typo'd key fails, bad `Type=`
value fails.

## Install coverage, and the rpm gap

A package which builds but does not install is precisely the failure a release
pipeline exists to catch, and only an install finds it. Coverage, without
containers (ARCH-36):

- **The `.deb`** is installed on the runner itself and the binary is executed —
  Ubuntu rather than Debian, and a runner that is not pristine, but it proves
  the package unpacks, its dependencies resolve, and what it lays down runs.
  The candidate build does it with the placeholder package; the release does
  it again with the real one and asserts the installed daemon reports the
  build id.
- **The `.rpm` is installed only by the published-repository smoke test**, in
  a `rockylinux:9` container with `dnf`, after publishing. Before that, the
  identical-binary check covers its contents and the apt half of the smoke
  covers the signing key and object layout the two repositories share.

## Windows signing is optional

`SIGN` requires a real release **and** all three `TRUSTED_SIGNING_*` secrets
**and** all three Azure service-principal secrets (`AZURE_TENANT_ID` /
`_CLIENT_ID` / `_CLIENT_SECRET`) to be non-empty — six in total. The first
three name the signing profile; the last three are what let the action's
`DefaultAzureCredential` prove the caller may use it. Naming the profile
without proving identity is not enough — the profile inputs are not a
credential.

The second half is load-bearing: gating on a real release alone would invoke
`Azure/artifact-signing-action` against an empty endpoint whenever credentials
are absent. That fails `windows`, and `publish` needs it — so with no
certificate a release would publish **nothing at all**: not the Windows
installer, which cannot be signed, but also not apt, dnf, the image or the
GitHub release, none of which needs a certificate.

Without the secrets the release ships an unsigned installer and says so twice: a
`::warning::` plus step summary in the job, and a paragraph in the release notes
for whoever meets SmartScreen. Setting all six secrets turns signing back on
with no edit.

## Downloads are pinned

A tool the release downloads is pinned to a versioned asset with a known SHA-256,
never fetched from a mutable "latest" redirect — the same rule as
`scripts/build/mbedtls.sh`, which refuses to fetch without one.

## The CA roots are a snapshot

The roots every outbound HTTPS connection trusts are compiled in
(`third_party/ca-roots/ca_roots.c`, [TLS.md](./TLS.md)), so a root Mozilla
removes stays trusted until a release carries the change. Before promoting, take
the newest dated extract and its sum from https://curl.se/docs/caextract.html,
set `CA_ROOTS_DATE` and `CA_ROOTS_SHA256` in `scripts/gen/ca_roots.sh`, run it,
and land the regenerated file on `staging` like any other change. Its diff is the
list of roots added and removed.

## Secrets and variables

| name | kind | required for |
|---|---|---|
| `DIST_S3_ACCESS_KEY` / `DIST_S3_SECRET_KEY` | secret | apt + dnf publish (production's bucket) |
| `DIST_S3_STAGING_ACCESS_KEY` / `DIST_S3_STAGING_SECRET_KEY` | secret | apt + dnf publish (staging's bucket); storing, checking and fetching candidates |
| `REPO_SIGNING_KEY` | secret | signing the indexes and packages, and the candidate manifest |
| `RELEASE_SSH_KEY` | secret, `release` environment | `promote.yml`'s fast-forward of `main`; the private half of the repository's write deploy key, the ruleset's one bypass actor |
| `TRUSTED_SIGNING_ACCOUNT` / `_ENDPOINT` / `_PROFILE` | secret | Authenticode; **optional** |
| `AZURE_TENANT_ID` / `_CLIENT_ID` / `_CLIENT_SECRET` | secret | Authenticode signing auth; **optional** |
| `DIST_S3_ENDPOINT` / `DIST_BUCKET` / `DIST_STAGING_BUCKET` | variable | rclone configuration |

`GITHUB_TOKEN` covers GHCR; nothing extra is needed for the image. It is passed
to `buildah login` and `skopeo`'s `--src-creds` and `--dest-creds`.

## Upgrading changes what the environment may say

The daemon is strict about its settings (CONFIG.md): an unknown `OPENCHIME_*`
name, one that was a setting once (the table at the end of CONFIG.md), a value that does not
parse or is out of its range stops it. A box upgraded with an old environment
file can therefore fail to come up, by design, rather than run with a setting
it misread. The packages run `openchimed --check-config --env-file
/etc/openchime/openchimed.env` on install and print what the new daemon would
refuse; an operator runs the same before restarting. The release notes of the
first release carrying this say so.

Nothing that ships runs in development mode. There is one build, and the
tests run the binary that ships; what keeps a deployment ordinary is
`OPENCHIME_MODE`, which defaults to `normal` (CONFIG.md "Mode"). `make test`'s
`scripts/check/release_lint.sh` keeps `OPENCHIME_MODE=development` and every
development input out of the workflows, the Dockerfile and `packaging/`.

## Dry runs

The candidate build is the rehearsal of the build: every staging push proves
the daemons, the images and the Windows clients build, install, start and
report their build id, and leaves the result stored. A release dry run
(`release.yml` dispatched on staging with `dry_run: true`) rehearses the other
half against that candidate: it fetches and verifies it, computes the next
number without reserving it, packages both architectures under it, builds the
installer unsigned, and skips `publish` (apt, dnf, the GitHub release, the
re-tagging and `:latest`), `smoke`, `unreserve` and `close-issues`. Those are
exercised only by a real release.

A push is never a dry run: `inputs.*` is empty on a push, so every dry-run
condition tests `github.event_name == 'workflow_dispatch' && inputs.dry_run`
rather than the input alone.
