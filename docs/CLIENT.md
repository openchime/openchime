# OpenChime — Client

The client's architecture and how it's built. Cross-referenced from
ARCHITECTURE.md (ARCH-11, **ARCH-74**, and the superseded ARCH-12/62/63/65) and
PROTOCOL.md (the wire flow the core drives).

**Direction (ARCH-74).** The client is **one shared C app-core with a native UI
per platform** — the *tdlib* model — chosen for real native feel over a
custom-rendered single UI. The earlier raylib + `gfx.h` + HarfBuzz plan is
dropped; there is no shared rendering layer, because each platform's OS shapes
its own text and provides its own widgets.

**Status.** Building the app-core + a **TUI** frontend first (the fastest usable,
CI-testable client). Native GUIs (Win32/WinUI, AppKit, Android, a web DOM UI)
and mobile follow.

---

## 1. Layers

```
shared/        wire contract: protocol, tls, framebuf, sock   (daemon + client)
client/core/   the app-core — frontend-agnostic, headless-testable C:
               net thread + session + (later) SQLite store + the view-model
               (channels, messages, roster, presence, unread) + reducers
client/tui/    terminal frontend over the core                (first frontend)
[client/win, client/mac, ...]   native GUIs over the same core (later)
```

The **app-core is the one shared asset.** It holds *all* logic and state; a
frontend is pure view + input — it renders the view-model and emits intents. Get
this boundary right and each new frontend is small; get it wrong and you rewrite
the client per platform. The core lives in the same repo and links the daemon's
exact `shared/` wire source, so client and server can't drift (the same reason
`tests/e2e_client.c` reuses `shared/protocol.c`).

## 2. The app-core (`client/core/`)

- **`net.c` — the network thread.** A blocking-socket TLS connection (the client
  is one connection on its own thread, so no epoll): `dial` → `oc_tls_handshake`
  → `HELLO`/`WELCOME` → `AUTH_CHALLENGE` → `AUTH` → `AUTH_OK`, then a serve loop
  that interleaves sending queued intents with reading + dispatching server
  frames. Lifted from the Phase-1 skeleton (which lifted it from
  `tests/e2e_client.c`).
- **`queue.c` — two thread-safe queues** (mutex+condvar FIFO, the daemon's
  net↔dbwriter shape): **intents in** (send, later: switch/join-call) and
  **events out** (connected, auth-ok, message, presence, channel, disconnected,
  error).
- **`event.h` — the core's wire-agnostic vocabulary:** the `oc_ev` (net→UI) and
  `oc_cmd` (UI→net) types crossing the queues.
- **`model.c` — the view-model + reducers.** `oc_model` holds the connection
  state, the channel/DM list, a per-channel message buffer (with the ARCH-45
  high-water dedup mark), roster/presence, and unread counts. `oc_model_apply`
  folds one `oc_ev` into that state. A frontend owns an `oc_model`, drains events
  each tick, applies them, and renders — the "read events at frame start" shape,
  fed by the net thread. Single-threaded on the frontend, so no locking.
- **`client.c` — the facade.** `oc_client_start(host, port, cred)` /
  `oc_client_tick()` (drain + apply) / `oc_client_model()` / `oc_client_send()` /
  `oc_client_stop()`. A frontend uses only this.

**Headless-testable.** A test starts the daemon's netloop in-process (like the
itest), drives an `oc_client` against it, sends a message, and asserts the
view-model updated — so the core is fully CI-covered with no UI.

## 3. Frontends

Each frontend is thin: create a client, loop { `oc_client_tick`; render the
model; translate input to intents }, stop.

- **TUI (`client/tui/`, first):** a terminal cell grid — a channel sidebar (with
  presence), a scrolling wrapped message pane, and an input line — built on
  **notcurses** (ARCH-75). notcurses is chosen for one reason that matters to a
  chat client: **correct Unicode wide-character / grapheme width**, so emoji
  reactions (REQ-070/071) and wrapped messages don't corrupt the layout the way
  they do under ncurses/termbox2. It is **vendored + pinned from source** like
  mbedTLS (identical lib across local/CI), built `--disable-multimedia`. **The
  TUI is text-only and never renders graphics — no sixel/kitty images, ever** —
  so the multimedia disable is permanent and the dependency footprint stays at
  notcurses + libunistring. Built on the host like the daemon.
- **Windows (later):** Win32/WinUI (C++/WinRT or C#) over the C core.
- **macOS/iOS (later):** AppKit/UIKit (Swift) over the core.
- **Android (later):** Android views (Kotlin) over the core.
- **Web (later):** a DOM UI — WASM can't use native desktop widgets; the core
  compiles to WASM and drives a JS/TS view.

## 4. The wire layer (reused, already tested)

The core links `shared/protocol.c` (every `oc_encode_*`/`oc_decode_*` for both
directions exists), `shared/tls.c` (client TLS + TOFU), `shared/framebuf.c`
(reassembly), and `shared/sock.h` (POSIX/Winsock shim). The wire sequence is
PROTOCOL.md §3–§6 and the §10 state machine. **TOFU pinning (ARCH-10):** Phase 1
trusts the presented cert (`pin=NULL`); persisting + pinning the fingerprint
arrives with the store phase.

## 5. Local store (later)

The core bundles SQLite and reuses the daemon's migration-runner pattern with its
own client migration set: cached history per channel + the high-water/backfill
cursors (ARCH-45/46), the **offline outbox** (messages composed offline, resent
on reconnect with their idempotency tokens — the concrete answer to REQ-102), the
session token (ARCH-58), per-instance TOFU pins, and config. Platform config dir;
keychains are noted future hardening. The core is in-memory until this lands.

## 6. Auth + reconnect/offline (later)

Auth UX (ARCH-19): a **local** username+password form; **OIDC** via the system
browser to central's authorize URL with a loopback `127.0.0.1` redirect catching
the ES256 JWT (`ASWebAuthenticationSession` on iOS/macOS); **session** silent
reconnect with the stored token. Instance+email → DNS resolution (SRV port >
`.well-known` > 443), failures surfaced distinctly (REQ-010/011). Reconnect
(REQ-100/101/102): the net thread auto-reconnects with the session token,
`BACKFILL_REQUEST`s per-channel cursors, and flushes the outbox.

## 7. Build

The core + TUI build on the host (a `make tui` target linking `client/core/*.c` +
`client/tui/*.c` + `shared/*` + notcurses), like the daemon — no cross-compile,
no container. notcurses is vendored + pinned from source (a `scripts/`-built
tarball into `third_party/`, `--disable-multimedia`), the same discipline as
mbedTLS (ARCH-51/75), so local and CI share one library. Native GUIs build with
their platform toolchains over the core; release artifacts come from CI/CD, never
a dev machine.

## 8. Roadmap

- **Now:** app-core + TUI — connect, auth, live messages across channels,
  presence, send; headless core test in CI.
- **Next:** store + reconnect/offline; auth completeness (local + OIDC);
  attachments (chunked up/download) and the **audio client** (Opus encode/decode
  + UDP to the sidecar — the deferred half of REQ-150/151).
- **Then:** native desktop GUIs (Windows/macOS), a web DOM UI, and mobile.
