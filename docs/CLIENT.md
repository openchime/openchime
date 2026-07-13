# OpenChime — Client

The native client's architecture and how it's built. Cross-referenced from
ARCHITECTURE.md (ARCH-11/12/13, ARCH-61–ARCH-66) and PROTOCOL.md (the wire flow
the client drives).

**Status.** Design + Phase 1 skeleton. The daemon has a working core; this is
the first client. Phase 1 is a Linux raylib skeleton that connects to a running
daemon, completes the handshake + (stubbed) auth, and shows/sends messages in a
bare UI — the client-side analogue of the daemon's first end-to-end proof. Later
phases (real text UI, local cache, reconnect/offline, real auth, cross-platform
packaging) are a roadmap in §8.

---

## 1. Repository layout (shared / daemon / client)

The client lives in the **same repo** and links the daemon's exact wire source,
so the two can't drift on the protocol (the same reason `tests/e2e_client.c`
reuses `shared/protocol.c`). The tree is split into three concerns:

```
shared/   protocol, tls, framebuf    — the wire contract (daemon + client)
daemon/   dbwriter, netloop, migrate, main
client/   gfx, ui, app, net, queue, main
```

A separate client repo was rejected: it would need a submodule or a vendored
copy of the wire code, reintroducing drift risk and cross-repo release
coordination for zero benefit.

## 2. Loop + threads (openblocks pattern + the daemon's threading)

The client follows the sibling raylib project **openblocks** for its loop
structure and adds threading for the network (which openblocks has none of):

- **One `AppCtx` struct + one `app_frame()`**, driven by a swappable loop driver
  (`while(!WindowShouldClose())` on desktop now; emscripten / iOS callbacks
  later) — exactly openblocks' `main.c` shape, so cross-platform drivers slot in
  without reshaping the loop.
- **UI thread** runs `app_frame()`: input + render at frame rate; it never
  touches a socket.
- **Network thread** owns the TLS socket end to end: connect → handshake →
  auth → the read loop → dispatch, and it drains an outbound queue to send
  frames. Because it's its own thread, it uses **blocking** sockets (no epoll) —
  simpler than the daemon's non-blocking loop and sufficient for one connection.
- **Two thread-safe queues** (a mutex+condvar linked list, like the daemon's
  net↔dbwriter queue): **UI→net** carries user actions (send, switch channel,
  reconnect); **net→UI** carries events (connected, auth-ok, message-received,
  disconnected, error). The UI thread **drains the net→UI queue at the top of
  `app_frame()`** and updates view state — the same "read events at frame start"
  shape openblocks uses, just fed by another thread.

## 3. Rendering through the `gfx.h` seam (ARCH-12)

Drawing goes through a **narrow `gfx.h` primitive seam** — `gfx_begin_frame` /
`gfx_end_frame` / `gfx_clear` / `gfx_rect` / `gfx_line` / `gfx_text` /
`gfx_measure_text` (openblocks' proven ARCH-12 seam) — implemented by
`client/gfx_raylib.c`. A later iOS build can swap the whole graphics stack by
providing a second implementation of that one header (openblocks does exactly
this with a Metal backend), and the seam is widened where a chat UI needs it
(clip/scroll regions, avatar images, glyph quads). App state
(`client/app.{c,h}`) is kept **raylib-free** and separate from rendering
(`client/ui.c`), so it stays testable and the text upgrade is contained. Per-
platform capabilities are gated through `client/platform.h`.

**Text.** raylib's built-in font is used in **Phase 1** to get a running UI
fast. The real message view needs **HarfBuzz** shaping + a **Unicode line-break**
library for wrapping (ARCH-12) — greenfield (openblocks renders only ASCII
pixel-font) — which lands in Phase 2 behind the `gfx.h`/state seams.

## 4. The wire layer (reused, already tested)

The client links `shared/protocol.c` (every `oc_encode_*`/`oc_decode_*` for both
directions already exists), `shared/tls.c` (client TLS + TOFU), and
`shared/framebuf.c` (incremental reassembly). The network thread's connect/read/
write logic is **lifted from `tests/e2e_client.c`**: `dial()` →
`oc_tls_client_init(pin)` → `oc_tls_conn_init` → drive `oc_tls_handshake` →
`read_frame` (framebuf + `oc_parse_frame`) for reads, `oc_encode_*` +
`oc_tls_write` for writes. The wire sequence the client drives is
PROTOCOL.md §3–§6 and the §10 state machine.

**TOFU pinning (ARCH-10).** On first connect to an instance the client trusts
the presented self-signed cert, captures its fingerprint via
`oc_tls_peer_fingerprint`, and stores it; on later connects it passes that
fingerprint as the `pin` so a changed cert fails the handshake. (A first-connect
trust prompt + `.well-known` out-of-band verification is Phase 4.)

## 5. Local store: SQLite (Phase 3)

The client bundles SQLite and reuses the daemon's migration-runner pattern
(`daemon/migrate.c`) with its **own** client migration set, holding: cached
message history per channel (offline view + the per-channel high-water marks
that drive dedup, ARCH-45, and backfill cursors, ARCH-46); the **offline outbox**
(messages composed while disconnected, resent on reconnect with their
idempotency tokens — this is the concrete answer to REQ-102); the session token
(reconnect without re-login, ARCH-58); per-instance TOFU fingerprints; and config
(known instances, email, last used). Stored under the platform config dir (XDG /
AppData / `~/Library`). Session token + pins sit in SQLite with tight file perms
for v1; platform keychains (macOS Keychain / Windows DPAPI / libsecret) are noted
future hardening. (Phase 1 keeps everything in memory; the store lands in
Phase 3.)

## 6. Authentication UX (ARCH-19)

**Sequencing:** the revised auth frames (`AUTH_CHALLENGE`, method-discriminated
`AUTH`, `AUTH_OK` with role + session token) are *documented* but **not yet in
code** — `shared/protocol.c` still has the stub-era `oc_auth`/`oc_auth_ok`. So
Phase 1 authenticates against today's stub exactly as `e2e_client.c` does (send a
token, get `AUTH_OK`). The real login UI lands with the daemon's auth-core
milestone, which updates `shared/protocol.c` for both sides at once. Then:

- **local** — a username+password form → `AUTH{local}`.
- **oidc** — open the system browser to central's authorize URL (from
  `AUTH_CHALLENGE.oidc_params`); catch the redirect with a **loopback HTTP
  listener** on `127.0.0.1:port` (desktop) to receive the ES256 JWT →
  `AUTH{oidc}`. iOS/macOS use `ASWebAuthenticationSession`.
- **session** — automatic on reconnect using the stored token → `AUTH{session}`.

At sign-in the client collects the **instance + email** (ARCH-14) and resolves
the instance to a host+port by DNS (SRV port > `.well-known` > 443), surfacing
resolution failures distinctly from auth/network errors (REQ-010/011).

## 7. Reconnect / offline (REQ-100/101/102)

The network thread auto-reconnects with the stored session token, then issues a
`BACKFILL_REQUEST` with per-channel cursors from the local store, replaying
missed messages (REQ-101). Offline-composed messages wait in the SQLite outbox
and are flushed on reconnect with their original idempotency tokens (REQ-102).
Client-side dedup is the per-channel high-water mark on `message_id` (ARCH-45,
REQ-091), held in the store.

## 8. Build

The client is a GUI app whose raylib build pulls in the X11/GL toolchain
(`libgl1-mesa-dev`, `libx11-dev`, `libxrandr/xinerama/xcursor/xi-dev`,
`libasound2-dev`), plus cross-compile toolchains later. To keep that off the
host it builds in a **container**: `Dockerfile.client` (a Debian base) installs
the toolchain, vendors raylib via `scripts/build_raylib_linux.sh` into
`third_party/raylib-install/` (the openblocks vendoring pattern) and mbedTLS via
`scripts/build_mbedtls.sh`, and compiles `client/*.c` + `shared/{protocol,tls,
framebuf}.c` + raylib + mbedTLS + pthread. The container-built binary dynamically
links X11/GL, whose runtime is present on a desktop/WSLg host, so it runs on the
host for GUI verification without a display in the container.

## 9. Phase roadmap

- **Phase 1 (this):** Linux raylib skeleton — connect, handshake, stub-auth,
  bare UI (connection status + a channel's live messages + a composer that
  sends), built in the container.
- **Phase 2 — real UI + text:** HarfBuzz + Unicode line-break message view,
  channel list, layout, scrolling.
- **Phase 3 — store + reconnect/offline:** SQLite cache (history, cursors,
  outbox), auto-reconnect, backfill, offline outbox flush.
- **Phase 4 — auth completeness:** the `AUTH_CHALLENGE` flow, local + OIDC
  loopback browser flow, session persistence, TOFU first-connect trust UX.
- **Phase 5 — cross-platform + packaging:** Windows/macOS then iOS/Android,
  following openblocks' Makefile/`build_raylib_*.sh`/per-object-dir pattern.
  Note the store packaging (ARCH-20 `.deb`+systemd, ARCH-21 MSIX, App-Store
  signing) is greenfield beyond the openblocks template (which ships plain
  archives and an unsigned `.ipa`).
