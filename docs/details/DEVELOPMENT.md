# Developing OpenChime

Building the daemon and the clients from source, and running them on this
machine. The process for a change — branches, commits, the fast suite before
every push — is [CONTRIBUTING.md](./CONTRIBUTING.md); the suites are
[TESTING.md](./TESTING.md).

## Building

```
make build      # the fast build (a bare `make`): the daemon alone, and the Windows clients
make test       # the fast suite, before every push (TESTING.md)
make run TTS=1 STT=1 SUM=1 WEB=1   # every feature: read-aloud, voice input, summaries, the web client
```

Requires a C toolchain, Python 3, and curl with network access on the first
build: SQLite and mbedTLS are in `third_party/`, and both are built once.
Python 3 generates the sources the daemon embeds (its web page and password
list). `make build` also cross-builds the Windows clients with the pinned
llvm-mingw it fetches (`mk/toolchains.mk`); nasm builds libvpx's x86 assembly.
`make build` and `make test` always build the one configuration, whatever was
asked before, with a job per CPU.

Anything else is built by its path. `TTS` (read-aloud) and `STT` (voice
input) need nothing more: their models run in C inside the daemon and their
data is committed. `SUM` needs nothing more either: its model runs in C
too, and the daemon fetches and converts it on first start. `WEB` needs CMake,
a C++17 compiler, git and xz besides, and the build fetches its own pinned Emscripten. `make run` builds the daemon with the data
read-aloud and voice input need beside it, and the Linux TUI, and starts it
(below); `make openchimed` builds the daemon alone. The switches are
remembered in `build/.features`, so a later `make openchimed` or `make run`
builds the same daemon; `make distclean` forgets them. `make ARCH=aarch64
build/aarch64/openchimed` cross-builds the daemon for arm64 (build only), and
`make openchimed RELEASE=1` builds the host's as the release does, with gcc 12
(`gcc-12`, `g++-12`), stripped; the arm64 build needs Ubuntu's gcc 12 cross
compiler (`gcc-12-aarch64-linux-gnu`, `g++-12-aarch64-linux-gnu`,
`libc6-dev-arm64-cross`). `make openchimed OC_BUILD_ID=<commit>` stamps the build
id into `openchimed --version`; an unstamped build reports `dev`. There is one
build: the tests and the demo scripts run it with
`OPENCHIME_MODE=development`, which a deployment never sets (CONFIG.md,
"Mode"). `make build-all` and `make test-all`, the full build and suite, run
in CI only.

To build the distributable packages from an already-built binary:

```sh
packaging/build-deb.sh     <version> amd64 ./openchimed dist
packaging/build-rpm.sh     <version> amd64 ./openchimed dist   # needs rpmbuild
packaging/build-tarball.sh <version> amd64 ./openchimed dist
```

Note the release binaries are built on **Ubuntu 22.04**, with its own gcc 12,
because a binary linked against a newer glibc will not start on an older one:
built there, they need glibc 2.35 at most, the oldest supported. The arm64
daemon comes off the same x86 runner, cross-compiled with Ubuntu's
`aarch64-linux-gnu-gcc-12` against Ubuntu 22.04's arm64 glibc (`make ARCH=aarch64`).
gcc 12 rather than the release's default gcc 11, which leaves read-aloud's
kernels unvectorised and about five times slower.
The full suite asserts the result rather than trusting it: if the built binary
needs a symbol above `GLIBC_2.35`, the build fails. A binary built on a newer
machine, this one say, links and runs there and is refused by that check. Linux runners build everything, the Windows installer and MSIX
included (NSIS and makemsix, signed with jsign when configured); nothing in the
pipeline uses Docker or Windows.

## The clients

The client ([CLIENT.md](./CLIENT.md)) is **one shared C app-core under one
portable graphical client and a terminal client** (the tdlib model, ARCH-74). The app-core
(`client/core/`) is frontend-agnostic — it links the daemon's exact `shared/`
wire code, owns the network thread and the view-model, and is driven headlessly
by the full suite (`tests/test_client_core.c`, `make test-all`).

The **web client** (`WEB.md`) is the same graphical client -- one
portable application over a per-platform directory (`client/gui/app/`,
`client/gui/platform/`) -- compiled for the browser with Emscripten, with text
through a canvas backend and the protocol inside a WebSocket. **The daemon
serves it**: open the workspace's address in a browser, sign in on the daemon's
own page, and the client is there -- the page and loader are compiled into the
daemon and the wasm ships beside it, so a daemon always serves the client that
matches it. Building the web client needs Emscripten, which `make openchimed
WEB=1` fetches pinned; `make build` leaves the web client out, and the
released packages carry it.

One frontend is a **TUI** (`make build/openchime-tui`, built on the in-tree `tuikit`
toolbox over termbox2 + utf8proc, ARCH-83), built on the host like the daemon and
also shipping on Windows (ARCH-81). It is menu- and screen-driven — panels, context
menus, dialogs, and a Ctrl+K command palette; there are no slash commands. It
covers live messaging with history backfill, reactions, edit/delete, typing,
threads, search, channel + DM management, roster + presence, who-reacted,
notification prefs + DND, admin (roles/invite/remove), webhooks, attachments,
storage and audit overlays, multiple workspaces, and logout. The app-core **writes nothing to disk** (ARCH-88/REQ-201): one credential per
workspace in the OS credential store carries the session token, the TOFU pin and
the workspace book, so it reconnects silently across restarts and queues sends
made while disconnected — in memory, for the life of the process. History comes
from the server's own read cursor rather than a local cache (REQ-100/101/102).

A native **Windows GUI**, pure C, presents through an **SDL3 renderer on its own
Win32 window**: every primitive is drawn by the in-tree `oc_gfx` layer and every
glyph is laid out and rasterized by **sdltext** over DirectWrite
(ARCH-80/106/107). The window class, message loop, tray, IME and the
accessibility provider stay Win32. Direct2D is used only as sdltext's
rasterizer on this platform — it is not the client's presentation layer.

It is the reference client: every engine feature is reachable from it.
Accessibility is first-class (REQ-269/ARCH-99: a UI Automation
provider over the self-drawn UI, a system caret and spoken notifications), with
an automation id and an invoke pattern on every actionable element (REQ-290).
Rich text and its
toolbar (REQ-220/ARCH-100), drafts, scheduled send, the notification schedule
with keywords and priority people, cross-channel threads, the People directory
and link-unfurl cards all run end to end with their daemon halves.

Every desktop shares one portable self-rendered application layer over SDL3 and
sdltext, with a thin native shim per platform (ARCH-80), so Linux and macOS are
ports of that layer rather than GTK and AppKit rewrites, as the web client
already is. Mobile has frontends of its own over the same core.

## Local development environment

Nothing in this project runs in a container. Build the daemon and run it
directly:

```
make run
```

This builds the daemon and the TUI, then starts a dev daemon on port 8443
(health on 8080) with its database and blobs under `/tmp/openchime-dev` and two
bootstrap users (`alice/pw`, `bob/pw`). It listens on `127.0.0.1` only
(`OPENCHIME_DEV_LOOPBACK=on`), so those fixed passwords are not reachable from
the network. Connect with the TUI it just built,
and sign in as `alice` / `pw` (in the browser or with a device code):

```
build/openchime-tui 127.0.0.1 8443
```

`make run LEAN=1` runs the same daemon under the lean profile's ceiling (ARCH-4,
REQ-210): at most 256 MB of memory with no swap, and one CPU, in a transient
systemd user scope (`systemd-run --user --scope`). Past the ceiling the daemon is
killed, as a small box would kill it, instead of swapping. It is an
approximation of a small machine, not a copy of one: the disk, the network and
the kernel are still this machine's. It needs a systemd user session with the
memory and cpu controllers delegated; where it cannot make the scope, it stops
and says so rather than running the daemon without limits.

Stop it with Ctrl-C. To start from scratch, delete `/tmp/openchime-dev` — there
are no volumes and no daemon holding state anywhere else.

### Verify the health check

The dev daemon serves `/healthz` on `OPENCHIME_HEALTH_PORT` (8080 by default):

```
curl http://localhost:8080/healthz
```

Should return `OK`.

## Known fidelity gaps

- **Local development only approximates the deployment target.**
  `make run` applies no resource limits; `make run LEAN=1` applies the lean
  profile's memory and CPU ceiling (ARCH-4, REQ-210) in a systemd user scope,
  but on this machine's disk, network and kernel, so constrained-resource
  behaviour is approximated rather than reproduced.
- The container image is exercised by the full suite's image checks
  (TESTING.md §3.2: started, asked `/healthz`, replicated and restored) and by
  the release's smoke, which pulls the released tag and asks it `--version`.
  The end-to-end suite drives a natively-built daemon, not the image.
