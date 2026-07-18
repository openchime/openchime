# OpenChime — Third-Party Dependencies

Every package that goes into building or running OpenChime, how it is pinned, and
its license. Kept current whenever a dependency is added, removed, or bumped.

OpenChime deliberately keeps its dependency surface small and permissive. There
are four ways a dependency enters the build; each row below says which:

| Class | How it's managed | Reproducible? |
|-------|------------------|---------------|
| **Vendored (committed)** | Single-file source committed under `third_party/` | Yes — frozen in git |
| **Fetched at build** | Downloaded + built by a `scripts/build_*.sh` (gitignored output) | Version-pinned in the script |
| **System / OS package** | Provided by the host or base image, linked at build | Pinned by the OS, not by us |
| **Container image** | Pulled by Docker / Compose for deployment or dev | Per the image tag |

**License posture:** everything linked into the shipping **daemon** and **TUI**
is permissive — MIT, Apache-2.0, Public Domain, or (for the *optional, dynamically
linked* libsecret/glib) LGPL-2.1. No GPL/AGPL code is linked into any shipped
binary. The only AGPL component (MinIO) is **dev/test infrastructure only** — a
separate process the daemon talks to over S3, never linked.

---

## 1. Vendored source — committed under `third_party/`

Single-file (or few-file) libraries committed directly to the repo, so local and
CI builds share byte-identical sources with zero transitive dependencies
(ARCH-75). "Pinning" = the file is frozen in git; bumping means replacing it.

| Package | Version | Purpose | Used by | Source | License |
|---------|---------|---------|---------|--------|---------|
| **termbox2** | v2.5.0 | Terminal cell grid + input | TUI (`client/tui`) | https://github.com/termbox/termbox2 | MIT |
| **utf8proc** | v2.11.3 | Unicode width + grapheme segmentation (correct emoji/CJK width) | TUI | https://github.com/JuliaStrings/utf8proc | MIT (bundled Unicode data under the Unicode license) |
| **jsmn** | commit-pinned (upstream has no release tags) | Minimal JSON tokenizer | Daemon (OIDC/webhook JSON) | https://github.com/zserge/jsmn | MIT |

Committed files: `third_party/{termbox2/termbox2.h, utf8proc/utf8proc.{c,h},
utf8proc/utf8proc_data.c, jsmn/jsmn.h}` (+ each project's LICENSE). These three
are whitelisted in `.gitignore`; everything else under `third_party/` is ignored.

## 2. Fetched at build time — `scripts/build_*.sh` (gitignored output)

| Package | Version | Purpose | Used by | Source | License |
|---------|---------|---------|---------|--------|---------|
| **Mbed TLS** | 3.6.2 | TLS transport (client + daemon), TOFU cert handling, SHA-256/PBKDF2, ES256 verify | Daemon + client (`shared/tls.c`) | https://github.com/Mbed-TLS/mbedtls | Apache-2.0 **OR** GPL-2.0-or-later (dual; we use it under Apache-2.0) |

Fetched + built by `scripts/build_mbedtls.sh` from the official GitHub release
tarball into `third_party/mbedtls-3.6.2/` (gitignored). Version pinned by
`MBEDTLS_VERSION` (default `3.6.2`). Built as **static** libs
(`libmbedtls/libmbedx509/libmbedcrypto.a`) with **`MBEDTLS_THREADING`** enabled —
the client runs a TLS network thread per connection and the tests drive several
TLS clients concurrently, which races without it (see docs/TLS.md).
**Supply-chain note:** the fetch trusts GitHub over TLS and does **not** verify a
pinned checksum — a hardening opportunity.

## 3. System / OS packages (linked at build)

Provided by the host toolchain (local dev / CI) or the Alpine base image
(Docker); their versions track the OS, not this repo.

| Package | Purpose | Used by | Link | Source | License |
|---------|---------|---------|------|--------|---------|
| **SQLite** (`libsqlite3`) | The daemon DB and the client store (schema + migrations) | Daemon + client | `-lsqlite3` | https://sqlite.org | Public Domain |
| **glibc `resolv`** (`libresolv`) | DNS **SRV** lookup for workspace resolution (REQ-010) | TUI only (Linux) | `-lresolv` | glibc | LGPL-2.1 (glibc) |
| **pthreads** | Threads (net thread, queues, dbwriter) | Daemon + client | `-lpthread` | glibc / musl | LGPL-2.1 / MIT |
| **libsecret** *(optional)* | OS keyring (Secret Service) backend for the session token | TUI credential cache | `pkg-config libsecret-1`, gated by `-DOC_HAVE_LIBSECRET` | https://gitlab.gnome.org/GNOME/libsecret | LGPL-2.1 |
| **GLib** *(optional)* | Pulled in transitively by libsecret | TUI (only when libsecret present) | via libsecret | https://gitlab.gnome.org/GNOME/glib | LGPL-2.1 |

`libsecret`/`glib` are **optional and dynamically linked**: `make tui` detects
libsecret via `pkg-config` and compiles the keyring backend, else compiles a stub
and the client falls back to the SQLite store (headless / no D-Bus). They are
*not* linked into the daemon, the test binary, or `make core`.

- **Local build headers:** `libsqlite3-dev` (Ubuntu) / `sqlite-dev` (Alpine), and
  `libsecret-1-dev` for the keyring backend.
- **CI installs:** `libsqlite3-dev` + `bzip2` (for the mbedTLS tarball). CI does
  not build the TUI, so it needs neither libsecret nor libresolv.

## 4. Container images — deployment & dev (Docker / Compose)

| Image / tool | Version | Purpose | License | Source |
|--------------|---------|---------|---------|--------|
| **Alpine Linux** | `3.20` | Build + runtime base image | mixed (base OS) | https://alpinelinux.org |
| **MinIO** | `minio/minio:latest` | S3-compatible object storage, **dev/test only** | AGPL-3.0 | https://github.com/minio/minio |
| **MinIO Client (`mc`)** | `minio/mc:latest` | One-shot bucket init (compose `minio-init`) | AGPL-3.0 | https://github.com/minio/mc |

- **MinIO** runs as its own container in `docker-compose.yml` to simulate a
  managed S3-compatible store (ARCH-38). It is **not part of the daemon** and its
  AGPL does not reach any OpenChime binary. It currently has no consumer — the
  daemon's S3 blob backend (ARCH-70) defaults to the local filesystem and is not
  wired into compose.
- Runtime Alpine packages: `sqlite-libs`, `sqlite`, `ca-certificates`.
- **Pinning gap:** the MinIO images use `:latest` (not version-pinned) — worth
  pinning to a digest for reproducible dev/test.

## 5. Build & dev tooling (not linked into the product)

- **C99 toolchain** — gcc or clang (+ `ar`); the whole tree builds with
  `-std=c99 -Wall -Wextra`.
- **make** — the only build system (no CMake/autotools for OpenChime itself).
- **bash, curl, tar, bzip2** — used by `scripts/build_mbedtls.sh` and the Docker
  build.
- **pkg-config** — detects libsecret for the TUI.
- **Docker + Docker Compose** — local platform + e2e integration harness.

## 6. Vestigial — present locally but in NO current build

These are gitignored and **not linked by any build target**. The raylib-based GUI
client plan was superseded by the shared-core + native-UI-per-platform model
(ARCH-74), so raylib is retained only as an artifact of that dropped direction.

| Item | Version | Note | License |
|------|---------|------|---------|
| **raylib** (`third_party/raylib-6.0-win`, `raylib-install-win`) | 6.0.0 | Dropped GUI-client toolkit (ARCH-74); unused | zlib/libpng |
| `scripts/build_raylib_windows.sh`, `scripts/build_mbedtls_windows.sh`, `third_party/mbedtls-3.6.2-win` | — | Experimental Windows cross-compile artifacts | (per package) |

## 7. Planned — not yet a dependency

- **libopus** — Opus encode/decode for the deferred **audio client**
  (REQ-150/151). BSD-3-Clause. Not yet linked; the server-relayed audio path
  carries opaque Opus payloads and does not link libopus.

---

## License summary

| License | Packages | Notes |
|---------|----------|-------|
| **MIT** | termbox2, utf8proc, jsmn | Vendored, committed |
| **Apache-2.0** | Mbed TLS (chosen from its dual license) | Static-linked |
| **Public Domain** | SQLite | System-linked |
| **LGPL-2.1** | libsecret, glib, glibc (resolv/pthreads) | Dynamically linked / optional — LGPL satisfied by dynamic linking |
| **zlib/libpng** | raylib | Vestigial, unused |
| **AGPL-3.0** | MinIO, mc | **Dev/test infra only — never linked into a shipped binary** |
| **Unicode license** | utf8proc bundled data tables | Alongside utf8proc's MIT code |

**Bottom line:** the shipped daemon and TUI are permissively licensed
(MIT/Apache/Public-Domain, plus optional dynamic LGPL). AGPL exists only in the
optional MinIO dev container, isolated behind the S3 API.
