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
| **termbox2** | v2.5.0 | Terminal cell grid + input | tuikit (→ TUI) | https://github.com/termbox/termbox2 | MIT |
| **utf8proc** | v2.11.3 | Unicode width + grapheme segmentation (correct emoji/CJK width) | tuikit (→ TUI) | https://github.com/JuliaStrings/utf8proc | MIT (bundled Unicode data under the Unicode license) |
| **jsmn** | commit-pinned (upstream has no release tags) | Minimal JSON tokenizer | Daemon (OIDC/webhook JSON) | https://github.com/zserge/jsmn | MIT |
| ~~**SQLite (amalgamation)**~~ | — | **Removed (ARCH-88).** It was vendored so the Windows *client* could have a store; no client embeds a database engine any more, and `third_party/sqlite` is deleted. The daemon still uses system SQLite. | — | — | — |

Since **ARCH-83**, `tuikit/` is the in-tree toolbox wrapping termbox2 + utf8proc
(terminal layer + width handling), which the TUI builds on — `client/tui`
consumes them through tuikit, not directly.

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
| **SQLite** (`libsqlite3`) | The daemon's database (ARCH-2). **Not linked by any client** (ARCH-88) | Daemon only | `-lsqlite3` | https://sqlite.org | Public Domain |
| **glibc `resolv`** (`libresolv`) | DNS **SRV** lookup for workspace resolution (REQ-010) | TUI only (Linux) | `-lresolv` | glibc | LGPL-2.1 (glibc) |
| **pthreads** | Threads (net thread, queues, dbwriter) | Daemon + client | `-lpthread` | glibc / musl | LGPL-2.1 / MIT |
| **libsecret** *(optional)* | OS keyring (Secret Service) backend for the session token on Linux (`client/shared/secret_libsecret.c`); Windows uses Credential Manager via `advapi32`, no vendored dependency | Linux TUI/GUI credential store | `pkg-config libsecret-1`, gated by `-DOC_HAVE_LIBSECRET` | https://gitlab.gnome.org/GNOME/libsecret | LGPL-2.1 |
| **GLib** *(optional)* | Pulled in transitively by libsecret | TUI (only when libsecret present) | via libsecret | https://gitlab.gnome.org/GNOME/glib | LGPL-2.1 |

`libsecret`/`glib` are **optional and dynamically linked**: `make tui` detects
libsecret via `pkg-config` and compiles the keyring backend, else compiles a stub
and that machine then persists no credential at all (headless / no D-Bus). They are
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

The self-rendered GUI direction (Clay layout + raylib/OpenGL) was tried and
rejected in favour of native per-platform UIs (**ARCH-82**): a self-rendered UI
is non-native and perpetually lags. **raylib and Clay have been removed** — the
`clay/`, `raylib-6.0-win`, `raylib-install-*` trees, the `build_raylib_*.sh`
scripts, the **old self-rendered `client/gui` (Clay+raylib)**, and the comctl32
`client/win32` client are all deleted. (The **current** native Win32 GUI lives at
`client/gui/win32/` — ARCH-82, pure Win32 + Direct2D — and is unrelated to the
removed self-rendered `client/gui`.) Retained Windows artifacts below are for the
**Windows TUI and GUI** cross-compile (ARCH-81/82).

| Item | Version | Note | License |
|------|---------|------|---------|
| `scripts/build_mbedtls_windows.sh`, `third_party/mbedtls-3.6.2-win` | 3.6.2 | Windows cross-compile (mingw); used by `make windows-tui` and `make windows-gui` | Apache-2.0 |

## 7. Planned — not yet a dependency

- **libopus** — Opus encode/decode for the **audio client** (REQ-150/151).
  BSD-3-Clause. Not yet linked; the server-relayed audio path carries opaque
  Opus payloads and does not link libopus (ARCH-73). Client-side only.
- **miniaudio** — planned single-header device I/O (capture + playback) for the
  audio client, wrapping ALSA/PulseAudio/PipeWire/CoreAudio/WASAPI.
  MIT-0/public-domain, vendored like termbox2/utf8proc/jsmn (ARCH-75). Chosen
  for its **duplex** mode, which AUDIO.md §2 requires so capture and playback
  share one clock. Not yet vendored.
- **speexdsp** — planned acoustic echo canceller (`speex_echo_state`) for the
  audio client, BSD-3-Clause, behind the processor vtable in AUDIO.md §3.3 so it
  is swappable. Not yet vendored. See AUDIO.md §6.2 for why it is preferred over
  WebRTC AEC3 as a first implementation despite being the weaker canceller.
- **libvpx** — planned **VP9** encode/decode for **screenshare** (REQ-161,
  ARCH-87), **BSD-3-Clause**, with screen-content tuning
  (`VP9E_SET_TUNE_CONTENT`). Not yet fetched. It belongs in the **fetched at
  build** class (§2) beside mbedTLS, *not* the committed-single-file class — it is
  the **first genuinely large dependency** in the tree and does not fit the
  jsmn/termbox2/utf8proc pattern. **Licence note:** BSD-3-Clause is not on the
  §"License summary" list today because nothing has needed it; it is squarely
  within this repo's *permissive* rule (mbedTLS is already Apache-2.0, not MIT),
  so this is a policy addition rather than an exception. **Patent note:** the
  reason it is preferred to the alternatives is as much patent as copyright —
  **openh264** is BSD-licensed but Cisco's royalty arrangement covers only the
  binaries *Cisco itself distributes*, so building from source leaves us exposed;
  **x264/x265** are GPL; **AV1** (SVT-AV1 + dav1d, BSD) is the designated
  successor once realtime software encode is cheaper. Client-side only — the
  daemon links no video codec, exactly as it links no libopus (ARCH-73/86).

---

## Lucide (icons, ISC)

Most of the graphical clients' icons come from [Lucide](https://lucide.dev)
(ISC License). We vendor **only the handful of SVGs we use**
(`third_party/lucide/icons/*.svg` — 13 of them) plus the license
(`third_party/lucide/LICENSE`).

**Four icons are ours, not Lucide's**, and live outside `third_party/` for exactly
that reason: `send`, `smile`, `at-sign` and `download`, hand-authored in Lucide's
conventions (24×24, 2px round-capped strokes) under `client/shared/icons_src/`.
Keeping them out of the vendored tree is what keeps the ISC attribution truthful —
it covers Lucide's work and nothing else.
A dev-time script (`scripts/gen_icons.py`, uses `svgelements`) flattens those SVGs
into platform-agnostic cubic-bezier path data baked into `client/shared/icons.{h,c}`
— so there is **no icon-font dependency** and each native client strokes the same
geometry with its own 2D API (Win32 uses Direct2D `ID2D1PathGeometry`). The
generated `icons.c` carries the ISC attribution in its header. Only the baked
paths ship; nothing is fetched at runtime.

---

## License summary

| License | Packages | Notes |
|---------|----------|-------|
| **MIT** | termbox2, utf8proc, jsmn | Vendored, committed |
| **ISC** | Lucide (icon path data) | Baked into client/shared/icons.c; 13 SVGs + LICENSE vendored. The other 4 icons in that file are our own work (`client/shared/icons_src/`), not ISC-licensed material |
| **Apache-2.0** | Mbed TLS (chosen from its dual license) | Static-linked |
| **BSD-3-Clause** | libvpx (VP9) — **planned, not yet fetched** | Screenshare codec (REQ-161, ARCH-87). Client-side only; the daemon links no codec. Permissive, within this repo's posture — see §7 |
| **Public Domain** | SQLite | System-linked, **daemon only** — no client links it (ARCH-88) |
| **LGPL-2.1** | libsecret, glib, glibc (resolv/pthreads) | Dynamically linked / optional — LGPL satisfied by dynamic linking |
| **zlib/libpng** | raylib | Dropped self-rendered-GUI toolkit (ARCH-82); unused, not linked |
| **zlib** | Clay | Dropped GUI layout engine (ARCH-82); unused, not linked |
| **AGPL-3.0** | MinIO, mc | **Dev/test infra only — never linked into a shipped binary** |
| **Unicode license** | utf8proc bundled data tables | Alongside utf8proc's MIT code |

**Bottom line:** the shipped daemon and TUI are permissively licensed
(MIT/Apache/Public-Domain, plus optional dynamic LGPL). AGPL exists only in the
optional MinIO dev container, isolated behind the S3 API.
