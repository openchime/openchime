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
| **Container image** | Base of the published deployment image; not used for dev or test | Per the image tag |

**License posture:** our own code is AGPL-3.0-or-later throughout
([LICENSING.md](../LICENSING.md)); every *third-party* component linked into the
shipping **daemon** and **TUI** is permissive — MIT, Apache-2.0, Public Domain,
or (for the *optional, dynamically linked* libsecret/glib) LGPL-2.1-or-later. No
third-party GPL/AGPL code is linked into any shipped binary, and every one of
these is compatible with the AGPL.

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

**Why the daemon uses system SQLite rather than vendoring it (ARCH-20; no client links SQLite at all, ARCH-88).**
Packaging the daemon raised the question, since a statically linked SQLite would
leave the `.deb` and `.rpm` depending on nothing but glibc. It stays dynamic
deliberately: mbedTLS is already static, so **we** already own TLS CVE response,
and static-linking SQLite would double that — every SQLite CVE would need an
OpenChime release and an operator upgrade, where today `apt upgrade` or `dnf
update` fixes it with no action from us. For software distributed *through* those
package managers, the distribution's security path is the feature. `libsqlite3-0`
(Debian) / `sqlite-libs` (RHEL) is present on essentially every system, so the
dependency costs close to nothing. The counter-argument is real and recorded: the
control-plane repo had to pin SQLite forward past CVE-2025-6965 — but that was a
native library bundled into a container, which has no distribution update path,
which is exactly the situation apt and dnf avoid.

**Attribution when shipping binaries.** Building from source never triggered the
vendored licences' notice requirements; distributing packages does. `openchimed`
links **mbedTLS** (Apache-2.0) and **jsmn** (MIT), and every channel ships both
texts in full — `/usr/share/doc/openchimed/copyright` on Debian,
`/usr/share/licenses/openchimed/copyright` on RHEL, `COPYRIGHT` in the tarball.
`packaging/licenses.sh` reads them out of the trees the build actually used, so
the notice cannot drift from what was linked, and it **fails the build** if a
licence file is missing rather than shipping a binary without one. termbox2,
utf8proc and lucide are deliberately absent from that file: they are TUI and
client components, and none is linked into the daemon.

Since **ARCH-83**, `tuikit/` is the in-tree toolbox wrapping termbox2 + utf8proc
(terminal layer + width handling), which the TUI builds on. `client/tui/main.c`
also calls both libraries **directly** — `utf8proc_iterate` for width, and the
raw `tb_*` grid calls — so the toolbox is the shared layer rather than an
exclusive one.

Committed files: `third_party/{termbox2/termbox2.h, utf8proc/utf8proc.{c,h},
utf8proc/utf8proc_data.c, jsmn/jsmn.h}`, and **each carries its licence file**:
termbox2's `LICENSE`, utf8proc's `LICENSE.md`, and jsmn's `LICENSE` (extracted
verbatim from the notice in `jsmn.h`, which is where upstream keeps it). `.gitignore` ignores
`third_party/*` and whitelists **four** paths: `jsmn/`, `termbox2/`,
`utf8proc/` and `lucide/` (committed, see below). Everything else under
`third_party/` — the fetched mbedTLS trees among it — stays ignored.

## 2. Fetched at build time — `scripts/build_*.sh` (gitignored output)

| Package | Version | Purpose | Used by | Source | License |
|---------|---------|---------|---------|--------|---------|
| **Mbed TLS** | 3.6.2 | TLS transport (client + daemon), TOFU cert handling, SHA-256/PBKDF2, ES256 verify | Daemon + client (`shared/tls.c`) | https://github.com/Mbed-TLS/mbedtls | Apache-2.0 **OR** GPL-2.0-or-later (dual; we use it under Apache-2.0) |
| **SDL3** | 3.4.14 | Windowing, input, GPU-accelerated 2D renderer for the graphical clients | GUI client (`client/gui/gfx/`, oc_gfx) | https://github.com/libsdl-org/SDL | zlib (no notice required in binary distributions) |

Fetched + built by `scripts/build_mbedtls.sh` from the official GitHub release
tarball into `third_party/mbedtls-3.6.2/` (gitignored). Version pinned by
`MBEDTLS_VERSION` (default `3.6.2`). Built as **static** libs
(`libmbedtls/libmbedx509/libmbedcrypto.a`) with **`MBEDTLS_THREADING`** enabled —
the client runs a TLS network thread per connection and the tests drive several
TLS clients concurrently, which races without it (see docs/TLS.md).
**The tarball is checksum-verified before it is unpacked.** Both fetch scripts
carry the known-good SHA-256 for the pinned version — the value upstream
publishes in the `mbedtls-<version>-sha256sum.txt` asset beside the release — and
refuse a mismatch, deleting the bad file rather than extracting it. Bumping
`MBEDTLS_VERSION` without supplying a matching sum is **refused**, not fetched
unverified: the script prints the `curl` that retrieves the upstream sum and
exits. Override for a new version with `MBEDTLS_SHA256=<sum>`.

**SDL3** follows the identical arrangement one script over:
`scripts/build_sdl3_windows.sh` fetches the pinned release tarball
(SHA-256-verified, refuses an unpinned bump), cross-builds a **static-only**
`libSDL3.a` with mingw into `third_party/sdl3-3.4.14-win/` (gitignored), and
`make windows-gfx-test` links it. SDL's own build is CMake; the script invoking
it is the same arrangement as invoking mbedTLS's make — the dependency's build
system stays its own business, and this tree's stays make. It is the first
genuinely large dependency in the tree, which is exactly why it sits in this
class and not the committed-single-file one (the reasoning §7 recorded for
libvpx, now applied). zlib-licensed: no notice needs to travel with a shipped
binary, so `packaging/licenses.sh` (daemon-only regardless) is untouched.

## 3. System / OS packages (linked at build)

Provided by the host toolchain (local dev / CI) or the Alpine base of the
published container image; their versions track the OS, not this repo.

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
- **CI installs:** `libsqlite3-dev` + `bzip2` (for the mbedTLS tarball). CI builds
  the **Windows** TUI and GUI (`make windows-tui windows-gui`) but not the Linux
  `make tui` target, so it needs neither libsecret nor libresolv: the mingw build
  reaches DNS through `DnsQuery` and the credential store through Credential
  Manager.

## 4. Container images — deployment only

| Image / tool | Version | Purpose | License | Source |
|--------------|---------|---------|---------|--------|
| **Alpine Linux** | `3.20` | Build + runtime base of the published image | mixed (base OS) | https://alpinelinux.org |

One image, and it is an **output** rather than a tool: the OCI image published to
GHCR for the hosted model (ARCH-20/76). Runtime Alpine packages: `sqlite-libs`,
`ca-certificates`.

The project runs no containers for development or testing (ARCH-36), so this
table holds exactly one row. The sole image reference in the repo is the
`FROM alpine:3.20` in `Dockerfile` — a tag rather than a digest, which is the
upstream's own stability contract for a base OS.

## 5. Build & dev tooling (not linked into the product)

- **C99 toolchain** — gcc or clang (+ `ar`); the whole tree builds with
  `-std=c99 -Wall -Wextra`.
- **make** — the only build system (no CMake/autotools for OpenChime itself).
- **bash, curl, tar, bzip2** — used by `scripts/build_mbedtls.sh` and the image
  build.
- **pkg-config** — detects libsecret for the TUI.
- **zig** (`0.13.0`, MIT) — **release only**, not needed for a local build.
  `zig cc` builds the shipped Linux daemon against glibc 2.34, the oldest
  supported target (ARCH-20). Pinned by SHA-256 per architecture in
  `.github/workflows/release.yml`.
- **buildah / skopeo** — **release only**. Build and push the OCI image and move
  the `:latest` tag, daemonlessly.
- **Docker and Docker Compose are NOT used**, anywhere, by anything — not to
  build, not to test, not locally, not in CI. This is a standing constraint;
  see ARCH-36 and the header of `.github/workflows/release.yml`.

## 6. Windows cross-compile artifacts

The native Win32 GUI lives at `client/gui/win32/` (ARCH-82, pure Win32 +
Direct2D). Its cross-compile, and the Windows TUI's (ARCH-81), use:

| Item | Version | Note | License |
|------|---------|------|---------|
| `scripts/build_mbedtls_windows.sh`, `third_party/mbedtls-3.6.2-win` | 3.6.2 | Windows cross-compile (mingw); used by `make windows-tui` and `make windows-gui` | Apache-2.0 |
| `scripts/build_sdl3_windows.sh`, `third_party/sdl3-3.4.14-win` | 3.4.14 | Windows cross-compile (mingw, static, CMake invoked by the script); used by `make windows-gfx-test` | zlib |

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
(`third_party/lucide/icons/*.svg` — 15 of them) plus the license
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
| **ISC** | Lucide (icon path data) | Baked into client/shared/icons.c; 15 SVGs + LICENSE vendored. The other 4 icons in that file are our own work (`client/shared/icons_src/`), not ISC-licensed material |
| **Apache-2.0** | Mbed TLS (chosen from its dual license) | Static-linked |
| **BSD-3-Clause** | libvpx (VP9) — **planned, not yet fetched** | Screenshare codec (REQ-161, ARCH-87). Client-side only; the daemon links no codec. Permissive, within this repo's posture — see §7 |
| **Public Domain** | SQLite | System-linked, **daemon only** — no client links it (ARCH-88) |
| **LGPL-2.1** | libsecret, glib, glibc (resolv/pthreads) | Dynamically linked / optional — LGPL satisfied by dynamic linking |
| **Unicode license** | utf8proc bundled data tables | Alongside utf8proc's MIT code |

**Bottom line:** every third-party dependency in the shipped daemon and TUI is
permissively licensed (MIT/Apache/Public-Domain, plus optional dynamic LGPL), so
all of them are compatible with the AGPL-3.0-or-later terms our own code carries.
No third-party AGPL code exists anywhere in the build.
