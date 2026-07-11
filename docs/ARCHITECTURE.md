# OpenChime: Architectural Decisions

A consolidated record of the architecture decisions made for OpenChime, a cross-platform business chat application with a self-hosted open-source core and a paid hosted tier.

Business, product, and scope decisions live in [REQUIREMENTS.md](./REQUIREMENTS.md).

## Server / Daemon

- **ARCH-1 (Language):** C daemon.
- **ARCH-2 (Persistence):** SQLite in WAL mode.
- **ARCH-3 (Replication):** Litestream for continuous replication to object storage (R2/B2).
- **ARCH-4 (Deployment):** Fly.io, single-box-per-tenant island model. Cost is driven by concurrent load and data volume, not registered user count. HA (second machine + LiteFS) and heavy media egress are the triggers that change the economics, not user count alone.
- **ARCH-5 (Threading):** Two-thread model. The network thread owns the socket and framing; the DB writer thread owns the single SQLite write connection.

## Wire Protocol

- **ARCH-6 (Decision):** Custom length-prefixed, versioned binary protocol over TLS/TCP. Not HTTP, not WebSocket, not JSON. Justified because both ends (C daemon and C client) are built together, removing any reason to pay HTTP/JSON overhead.
- **ARCH-7 (Serialization):** Explicit field-by-field; no struct memcpy.
- **ARCH-8 (Frame layout):** Fixed header of `length` (u32, network byte order), `version` (u8/u16), `msg_type` (u16), followed by a type-specific payload.
  - `length`: total bytes after this field; makes the stream self-framing over TCP's boundary-less byte stream.
  - `version`: checked on the first frame so incompatible clients are rejected cleanly rather than misparsed.
  - `msg_type`: enum discriminating login, message-send, message-broadcast, presence-update, ack, error, etc.
- **ARCH-9 (Reading discipline):** Network thread reads in two phases (fixed header, then `length` more bytes), each looping to handle partial TCP reads. Everything is network byte order, converted at the edges.
- **ARCH-10 (TLS):** Standard PKI / Let's Encrypt, replacing the earlier per-operator cert-pinning design after the per-operator binary model was dropped in favor of a generic client.

## Client

- **ARCH-11 (Language):** Native C, explicitly rejecting Electron.
- **ARCH-12 (Rendering path):** raylib for window/paint/input, with HarfBuzz for text shaping and a Unicode line-break library for the message view. Lexbor evaluated but ruled out as a text-rendering engine (its layout/paint pipeline is not production-ready; only the HTML/CSS parser and DOM are stable).
- **ARCH-13 (Generic client):** A single generic client replaced the earlier per-operator custom-binary approach.

## Discovery

- **ARCH-14 (Rule):** Dot-based. Bare names (e.g. `acme`) resolve via the hosted service's DNS suffix; names containing a domain (e.g. `acme.com`) use SRV records plus optional `.well-known` metadata for self-hosters.

## Feature Implementation

- **ARCH-15 (Search):** FTS5 full-text search, positioned as a competitive wedge against Slack's history caps.
- **ARCH-16 (Notifications delivery):** Push via APNs/FCM (free from providers). Self-hosted deployments need a small push gateway since the app is published under the maintainer's accounts.
- **ARCH-17 (Attachments):** Via object storage.
- **ARCH-18 (Audio conferencing):** Server-relay (no P2P/ICE), Opus codec, delivered as an isolated UDP-based sidecar process rather than embedded in the daemon's TCP loop.

## Authentication

- **ARCH-19 (Architecture):** Client-drives-browser-flow using platform-native auth session APIs (ASWebAuthenticationSession on iOS/macOS, loopback redirect on desktop) with PKCE. The daemon only validates the returned JWT against provider JWKS.

## Packaging and Distribution

- **ARCH-20 (Linux, self-hosted):** apt / `.deb` with a systemd unit, from a GPG-signed repo hosted on R2.
- **ARCH-21 (Windows):** Microsoft Store via MSIX packaging, chosen to avoid an Authenticode cert purchase.
