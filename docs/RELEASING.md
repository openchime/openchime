# Releasing

How a staging commit becomes a **release-candidate** (`release-candidate.yml`)
and a release-candidate becomes a **release** (`release.yml`), and the things
about both that are not obvious from reading the files.

Both are tagged and published in this repository. A release-candidate is a
staging commit, tagged `release-N-rcK` on that commit and published as a
prerelease with the files built from it. A release takes one
release-candidate, fast-forwards `main` to its commit, tags `release-N` there
and publishes the same files, packaged under N. The images are in Fly's
registry, `registry.fly.io/openchime-daemon-<environment>`. At the very end,
each release-candidate and each release is copied to `openchime/openchime`
(its commits, its tag and its GitHub release); the copy is a convenience, and
nothing reads it back or depends on it.

## Build id and release number

A build is identified by its **build id**: the short commit it was built from
(`74359c0`). The release-candidate build compiles it into the daemon, the web client
the daemon serves, and both Windows clients (`make OC_BUILD_ID=...`, through
`shared/build_id.h`), and it is what `openchimed --version`, the startup log,
`openchime-tui --version`, the Windows file properties (ProductVersion
`build 74359c0`) and the GUI's About OpenChime report. About shows the daemon's
too, which the daemon sends each client at sign-in (PROTOCOL.md `SERVER_BUILD`).
Nothing about a release number is in a binary.

A **release number** is a label assigned to one build id when it is released:
`release-1` is build `74359c0`. It appears only where a public monotonic
version is required: the deb and rpm version fields and filenames, the tarball
and installer filenames, the installer's and the MSIX's version, the image tags
`:1` and `:latest`, the `release-1` tag and the GitHub release, whose notes
name the build id. The tag on the commit is the map from one to the other.

Windows wants a numeric, growing file version. A commit is neither, so the
file version is `1.0.<run>.0`, the run number of the release-candidate build
that made the files plus the `RUN_NUMBER_OFFSET` variable (0 when unset),
recorded in the build's manifest; the release's installer check reads it from
there. The offset exists because a workflow's run numbers start again from 1
when it is renamed or moved, and the file version must keep growing past every
build already shipped: set it to at least the highest run number released so
far.

## Release-candidates: built from staging, stored by build id

A release-candidate is cut from the tip of `staging`:

    gh workflow run release-candidate.yml --ref staging

`release-candidate.yml` refuses any other ref, waits for (or finds finished) the
CI, attribution-guard and commit-policy runs of the push that put the commit on
`staging`, and then builds everything a release ships: both daemons with their
data (zig against glibc 2.34, the arm64 one cross-built), the images (buildah,
started with podman and asked `/healthz`), the Windows clients (llvm-mingw).
The packaging scripts run under a placeholder version so a package that cannot
be built, installed or made identical across channels stops the
release-candidate; the packages themselves are not kept.

CI runs on every push to `staging`, a documentation-only one included (its
build and test jobs skip), so the gate waits for CI, the attribution guard and
the commit policy on every commit (`scripts/release/wait_checks.sh`).

What is kept, under `candidates/<build-id>/` in the staging bucket
(`DIST_STAGING_BUCKET`, via the `dist-staging` rclone remote):

    linux-amd64/openchimed-build-<id>-linux-amd64.tar.gz   the binary, tts/, stt/, web/, copyright
    linux-arm64/openchimed-build-<id>-linux-arm64.tar.gz
    windows-x64/openchime.exe, openchime-tui.exe, openchime.debug
    manifest.json, manifest.json.asc

The manifest (`scripts/release/candidate_manifest.sh`) names the build id, the
full commit, the run number and id, every file's sha256 and size, and the
digests of the images the build pushed to the staging registry,
`registry.fly.io/openchime-daemon-staging`, as `:build-<id>-amd64`,
`:build-<id>-arm64` and the manifest list `:build-<id>`. It is signed with the
archive signing key. The same files go up as a workflow artifact,
`candidate-<id>`, for anyone who wants to look. Builds older than thirty days
that no `release-*` tag names (a release, or a standing release-candidate) are
pruned.

Once stored, the release-candidate is **published**. `N` is the number the
next release would take (`scripts/release/version.sh next`, which counts only
`release-<N>` tags) and `K` is one past the highest release-candidate already
tagged for N, both read from this repository's tags. The job tags the commit
`release-N-rcK` and creates a prerelease of that tag carrying the two Linux
trees, the Windows exes and `openchime.debug`, with notes naming the build id
and the commit, both with the job's own token. Earlier release-candidates for
the same N stay, retitled `<tag> (superseded by release-N-rcK)`. The step
summary names the tag. Re-running the job finishes a release-candidate already
tagged on the commit rather than numbering it again.

Last, `sync-public` copies it to `openchime/openchime`: the commit to its
`staging` (a plain push, never forced), the tag, and a prerelease with the same
files, title and notes. It reads nothing there but whether the tag and the
release already exist, so a re-run finishes it, and a failure is a
`::warning::`, never a failed run.

One build per commit. `make RELEASE=1 ARCH=aarch64 ... OC_BUILD_ID=<id>`
is the same command a developer runs, so a build is reproducible by hand.

## Starting a release

A release is made from a release-candidate:

    gh workflow run release.yml --ref staging -f release_candidate=release-1-rc1

and `release.yml`:

1. refuses unless it was started on `staging`;
2. resolves the release-candidate: the tag must be `release-N-rcK`, exist, and
   have a published prerelease; its commit is what is released, and N is the
   number it gets;
3. fetches and verifies that commit's stored build (below);
4. refuses if `release-N` already exists or N is not the next number, and
   reserves `release-N` on the commit;
5. packages, then **fast-forwards `main`** to the commit (a plain push, never
   forced: if `main` is not its ancestor the release stops with nothing
   published and the number is returned);
6. publishes, closes the issues it shipped, and removes every `release-N-rcK`
   prerelease and tag: `release-N` carries the same files on the same commit;
7. copies the release to `openchime/openchime` (`sync-public`, below).

`main` moves only here, so it always names what was released. A ruleset on it
refuses every update except from the repository's deploy key
(`RELEASE_SSH_KEY`), which the release pushes with.

A release-candidate whose number has since been released cannot be released;
cut a new one from `staging`.

## Promotion: main without a release

A release moves `main` and takes a number. Documentation and workflows change
nothing in the packages, so `promote.yml` moves `main` without one. It runs
after every push to `staging` (a merged pull request) and:

1. lists `main..staging`. When every commit touches only `*.md`, `.gitignore`,
   `.github/` or `.claude/`, it goes on; when any touches code, it stops and
   says which: `main` moves with the next release, which carries the
   documentation with it.
2. waits for CI, the attribution guard and the commit policy on the `staging`
   tip, as a release-candidate does;
3. fast-forwards `main` with `RELEASE_SSH_KEY`;
4. copies `main` and `staging` to `openchime/openchime` with `PUBLIC_DEPLOY_KEY`
   (a failure there is a warning);
5. closes the OC issues the promoted commits name, with `PUBLIC_TOKEN`
   (`close_shipped_issues.sh --range`).

No tag, no number, no packages, no images. `main` only fast-forwards, so the
hashes are the same in both repositories, and the next release's range starts
at the previous `release-N` as before: issues a promotion already closed are
reported as closed.

## Shape

`release-candidate.yml`, dispatched on staging:

    branch ─▶ id ──────┬─▶ packages (amd64, arm64) ─▶ image ─▶ image-manifest ─┐
          ├─▶ gate ────┤                                                       ├─▶ store ─┬─▶ prune
          └─▶ preflight└─▶ windows-build ─▶ windows-package ───────────────────┘          └─▶ publish ─▶ sync-public

`release.yml`, dispatched on staging with a release-candidate:

    branch ─▶ guard ─────────────────────────────┐
          ├─▶ resolve ─────────────┐             │
          └─▶ preflight ───────────┴─▶ candidate ┴─▶ version ─┬─▶ package (amd64, arm64) ─┬─▶ main ─▶ publish ─┬─▶ smoke
                                                              └─▶ windows ────────────────┘                    ├─▶ close-issues
                                                                                                               ├─▶ release-candidate-cleanup
                                                                                                               ├─▶ unreserve (on failure)
                                                                                                               └─▶ sync-public (after all the others)

`resolve` turns the release-candidate tag into its commit and N. `candidate`
fetches `candidates/<id>/` for that commit, verifies the manifest's signature
and every file against it, checks the manifest names the same commit, unpacks
the amd64 daemon and asks it `--version` (the id in the manifest is the id the
code carries), and checks the image `:build-<id>` is in the staging registry
with the digest the manifest recorded. It passes the verified files on as one
artifact per platform (`candidate-linux-amd64`, `candidate-linux-arm64`,
`candidate-windows-x64`), and `package` and `windows` each download only their
own. `version` reserves the number. `package` runs the
packaging scripts under it and proves the deb, the rpm and the tarball carry
the build's binary. `windows` signs the build's exes and wraps them in the
installer and the MSIX. `main` fast-forwards `main`. `publish` writes the
repositories, creates the GitHub release, copies the image to `:N` and moves
`:latest`. No compiler runs; a release takes minutes.

`guard` is the attribution guard, gated via `workflow_call`. CI is not run
again: the release-candidate exists only because CI passed on its commit on
staging.

`close-issues` closes the issues the release shipped: every commit on `main`
names its issue in the subject, and `scripts/release/close_shipped_issues.sh` reads the
commits between the previous `release-N` and this one. The first release has
no previous one and covers only its own commit: the issues named by the
commits below it are not closed, and are closed by hand. The issues are on
`openchime/openchime`, closed with `PUBLIC_TOKEN`. An issue closes when it
ships, not when it merges. The job only ever warns — the release is already live.

`release-candidate-cleanup` deletes the release-candidates of the number just
released, prereleases and tags. It also only ever warns.

`sync-public` runs last, after every other job of a release that published.
It copies the release to `openchime/openchime`: the commit to its `main` and
its `staging` (plain pushes, never forced; a `staging` there that already
contains the commit is left as it is), the `release-N` tag, and a release with
the files, title and notes of `release-N` here (`gh release download`), then
deletes the release-candidates of N there. It reads nothing there but whether
a ref, the tag or a release already exists. It is `continue-on-error`: a
failure is a `::warning::` and never fails the run, and a re-run of the job
completes it.

## Verification is terminal, and publishing is not interrupted by it

`smoke` — install the daemon from the repository that was just published — is
the last job, not a step in the middle of `publish`.

It covers all three channels: apt on the runner, dnf in a `rockylinux:9`
container, and the container image, pulled from the staging registry by its
released tag `:N` (logged in with `FLY_REGISTRY_TOKEN_STAGING`, as a deploy
is) and run with `--version`. The image check is not behind `skip_repo_smoke`,
since the registry does not depend on `/dist`, and runs even when a repository
step above it fails.

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
release-candidate build: their job is to prove the build, and failing it would stop
anyone verifying one on a fork with no secrets — but surfacing the gap is
exactly what was impossible before.

Authenticode signing is checked separately because it is optional (see *Windows
signing is optional* below). The state worth naming is a **partial** trio: `SIGN`
requires all three values, so one or two set reads as configured and silently
ships unsigned. Preflight warns on that specifically.

## The release number is reserved, not assigned at the end

`version` takes N from the release-candidate, checks it against the tags, and
**pushes `release-N` immediately**, once the build is verified and before
anything is packaged.

The number goes into `.deb` and `.rpm` filenames and into the apt/dnf indexes:
if the tag push were last and it failed, release N would already be live and
untagged, and the next run would compute N again, package different content and
publish it under the same version. Two `.deb`s sharing a version is the one
thing apt cannot recover from.

Reserving inverts the failure: the worst case is a **burnt number**, which costs
nothing. The terminal `unreserve` job deletes the tag when `publish` did not
succeed, which is safe precisely because nothing names it — no index, no GitHub
release, no `:latest` (`main` may already have moved, which is harmless: it
names a commit the next release-candidate descends from). One guard sharpens
that: `publish` sets an output the moment the apt index lands, and
`unreserve` requires it to be **unset** — returning a number is only safe
while nothing has been published under it, and a failure *after* the
repositories go live must burn the number rather than hand it back.

`publish` **holds** the number as its first step, before anything is written.
The tag on the release-candidate's commit is the ordinary case. The tag absent is what
re-running only the failed jobs produces — `unreserve` returned the number and
`version`, having succeeded, does not run again — so `publish` reserves it again,
provided it is still exactly the next number. A tag on another commit, or a
later release already tagged, means the run no longer owns the number, and it
stops with nothing published. The tagging step near the end still **asserts**
the tag rather than creating it; if that ever fires, something deleted the
reservation mid-job.

## The image is the build's, copied; `:latest` moves last

The release-candidate build's `image` job lays each architecture's tree onto a glibc
base with **buildah** (`Dockerfile`, `packaging/image-context.sh`; no compile,
no `RUN`, so the arm64 image is built on the x86 runner with `--arch arm64` and
no emulation), together with the pinned, checksummed Litestream for that
architecture (ARCH-3; VENDORS.md §4), starts the amd64 one with **podman** (daemonless, reading
buildah's store) and asks it `/healthz`, then starts it again given the
`replica-*` files for a bucket on the runner and proves it replicates under
`<prefix>/db/`, restores onto an empty volume and refuses to boot on a bucket
it cannot read (TESTING.md §3.2), so a fault in the `Dockerfile`, the
entrypoint or the binary on that base stops the release-candidate rather than reaching
the registry (the arm64 image is build-only, as its binary is), and pushes
`:build-<id>-<arch>` to the staging registry,
`registry.fly.io/openchime-daemon-staging`; `image-manifest` pushes the
manifest list `:build-<id>` there. Nothing deploys those tags; `:N` and
`:latest` are written only by a release. Fly's registry takes basic auth: user
`x`, a Fly deploy token as the password (`FLY_REGISTRY_TOKEN_STAGING`,
`FLY_REGISTRY_TOKEN_PRODUCTION`).

Everything Linux-hosted runs on one image, `ubuntu-24.04`, with every compiler
fetched pinned by make (VENDORS.md §2, "Toolchains"). The arm64 daemon is
**cross-built** there with zig (`make RELEASE=1 ARCH=aarch64`) and is build-only:
the render, the hearing check and the package install run for amd64; arm64 ships
the same sources through the same compiler, asserted for its glibc floor and
packaged, but not run.

The release copies the manifest list `:build-<id>` with
`skopeo copy --all --preserve-digests`, registry to registry, without pulling
or rebuilding: to `:N` in the staging registry, then to `:N` in the production
registry, `registry.fly.io/openchime-daemon-production`. In each, the digest
of `:N` is checked against the build's, and only then `:N` becomes `:latest`.
If `:latest` moved earlier and a later step failed, `:latest` would point at a
version with no packages, no tag and no release. `--all` copies the whole
manifest list rather than just the runner's architecture.

## The apt/dnf pool guard

Each release **adds** its packages to both repositories; the index must go on
naming every prior release, or it silently drops them.
`scripts/release/repo_update.sh` does it without downloading the pool, so the
time a release takes does not grow with the number of releases:

- **apt:** fetch `dists/` only; generate stanzas for the new debs alone
  (`apt-ftparchive packages` over a scratch tree of the pool's layout), append
  them, regenerate `Release` and sign it.
- **rpm:** fetch `repodata/` only; sign only the new rpms, then
  `createrepo_c --update --skip-stat --error-exit-val --pkglist` over the
  listed and the new packages, so the listed ones, absent on the runner, are
  taken from the old index by their path; sign `repomd.xml`.

Both are prepared, checked and signed before either is written. The upload is
`rclone copy`, never `sync`: the packages, then the index, then its signed top
file, so no client reads an index naming a package not yet there.

The pool is **listed**, and the listing must not swallow failure: a `|| true`
cannot distinguish "prefix does not exist yet" from "credentials rejected" or
"endpoint returned 503" — on any failure the pool reads as empty, the index is
built from one release, and the upload replaces `Release`/`InRelease` in the
bucket. The old objects survive; the index naming them does not. Only rclone's
"directory not found" (a local path; an object store lists an absent prefix as
empty) reads as empty.

Then the witnesses, any of which stops the release with nothing published:

- **The old index names exactly what the pool lists**, per architecture. The
  one difference allowed is this release's own packages listed but not
  indexed: an earlier attempt that uploaded them and failed before the index.
- **The tag count**: a prefix that exists but holds no package, with prior
  release tags, is a hard stop. No objects at all is a first publish.
- **No version twice**: a new package already in the index must be the same
  file (a re-run, which leaves it as it is: by sha256 for a deb, by header
  digest for an rpm, which signing does not change); its version under
  another file is refused.
- **The new index** lists every old entry and this release exactly once per
  architecture.

Every release publishes to both buckets from production's index. Staging's
pool is reconciled to production's before anything is written: what it lacks
is copied across, file by file, and what it has beyond production's is
reported and left. Afterwards `smoke` reads each index back from each bucket
and checks it lists the previous count + 1 versions per architecture, every
package it names present, and the signed top matching the files it names.

**The full method is the fallback**: fetch the whole prefix, re-sign every rpm
and regenerate both indexes from the whole pool, as every release did before.
The rpm half takes it by itself when `createrepo_c` lacks the flags (noble's
0.17.3 has them) or its incremental index fails the checks; setting
`REPO_UPDATE_MODE=full` in the step's environment asks for it, and is also the
repair for an index that has drifted from its pool, which the witness refuses.

**Offline parity**, worth running after any change to the script: build tiny
packages of versions 1, 2 and 3; publish 1 and 2 the full way into a local
rclone remote (an `alias` of a directory); add 3 both ways, into two copies;
diff the sorted `primary.xml` package sets and the sorted `Packages` stanzas,
which must be identical; and run `verify` on both.

The guard was verified against a real Tigris endpoint, four cases:

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
both**: the indexes are built once, from production's, staging's pool is
reconciled to production's, and the same files are uploaded to each, so the
two stay identical.

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

The release-candidate build runs `systemd-analyze verify` against it, directly on the runner —
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
  The release-candidate build does it with the placeholder package; the release does
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
removes stays trusted until a release carries the change. Before cutting a
release-candidate, take
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
| `RELEASE_SSH_KEY` | secret, `release` environment | the private half of this repository's write deploy key, the one actor `main`'s ruleset lets through: fast-forwards `main` |
| `FLY_REGISTRY_TOKEN_STAGING` | secret, `release` environment | a Fly deploy token for `registry.fly.io/openchime-daemon-staging`: pushes the build's images, reads their digests, writes `:N` and `:latest` there, and the smoke pull |
| `FLY_REGISTRY_TOKEN_PRODUCTION` | secret, `release` environment | a Fly deploy token for `registry.fly.io/openchime-daemon-production`: writes `:N` and `:latest` there |
| `PUBLIC_DEPLOY_KEY` | secret, `release` environment | the private half of a write deploy key on `openchime/openchime`: `sync-public` pushes `staging`, `main` and the tags there, and removes release-candidate tags; optional, the copy warns without it |
| `PUBLIC_TOKEN` | secret, `release` environment | a fine-grained token with Contents and Issues write on `openchime/openchime`: `sync-public` creates the prereleases and releases there and removes the release-candidates, and `close-issues` closes the shipped issues there. `check release credentials` proves it can write issues there before anything is built or published: a release refuses without it, a release-candidate warns; optional otherwise, as above |
| `TRUSTED_SIGNING_ACCOUNT` / `_ENDPOINT` / `_PROFILE` | secret | Authenticode; **optional** |
| `AZURE_TENANT_ID` / `_CLIENT_ID` / `_CLIENT_SECRET` | secret | Authenticode signing auth; **optional** |
| `DIST_S3_ENDPOINT` / `DIST_BUCKET` / `DIST_STAGING_BUCKET` | variable | rclone configuration |
| `RUN_NUMBER_OFFSET` | variable | added to the run number in the Windows file version; optional, 0 when unset |

`GITHUB_TOKEN` pushes the `release-N-rcK` and `release-N` tags and creates,
edits and deletes the GitHub releases, all in this repository; the jobs that
do so ask for `contents: write`. Every job that reads a `release` environment secret declares
that environment.

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

A release-candidate is the rehearsal of the build: it proves the daemons, the
images and the Windows clients build, install, start and report their build
id, and leaves the result stored and published as a prerelease. A release dry
run (`release.yml` dispatched on staging with a `release_candidate` and
`dry_run: true`) rehearses the other half against that release-candidate: it
resolves it, fetches and verifies its build, checks its number without
reserving it, packages both architectures under it, builds the installer
unsigned, and skips `main`, `publish` (apt, dnf, the GitHub release, the image
copy and `:latest`), `smoke`, `unreserve`, `close-issues`,
`release-candidate-cleanup` and `sync-public`. Those are exercised only by a real release.
