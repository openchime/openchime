# OpenChime: Architectural Decisions

A consolidated record of the architecture decisions made for OpenChime, a cross-platform business chat application with a self-hosted open-source core and a paid hosted tier.

## Product and Positioning

- **Model:** Open-core / COSS. The chat core is fully open source; the hosted service charges for operational convenience; MSP-facing features (multi-tenant console, white-label, bulk provisioning API, SSO/SCIM) form the monetizable tier.
- **Target:** 50 to 100 paying customers. This scale deliberately sidesteps the market-saturation and switching-cost arguments that apply to larger ambitions.
- **Distribution:** MSP channel identified as the most promising path, borrowing MSP trust relationships with client firms. Commercial structure is wholesale pricing MSPs mark up themselves plus white-labeling, not revenue share.
- **Licensing:** AGPL or source-available strongly recommended over BSD/MIT to prevent strip-mining by cloud providers. LICENSE decision (AGPL vs MIT) flagged as the item to settle before the first real commit.
- **Name/repo:** github.com/openchime org claimed; openchime.dev is the natural domain pick.

## Server / Daemon

- **Language:** C daemon.
- **Persistence:** SQLite in WAL mode.
- **Replication:** Litestream for continuous replication to object storage (R2/B2).
- **Deployment:** Fly.io, single-box-per-tenant island model.
  - Lean config (256MB, no dedicated IP): ~$2.50 to $3.50/month.
  - Comfortable config (512MB, dedicated IPv4): ~$6 to $7/month.
  - Cost is driven by concurrent load and data volume, not registered user count. HA (second machine + LiteFS) and heavy media egress are the triggers that change the economics, not user count alone.
- **Threading:** Two-thread model. The network thread owns the socket and framing; the DB writer thread owns the single SQLite write connection.

## Wire Protocol

- **Decision:** Custom length-prefixed, versioned binary protocol over TLS/TCP. Not HTTP, not WebSocket, not JSON. Justified because both ends (C daemon and C client) are built together, removing any reason to pay HTTP/JSON overhead.
- **Serialization:** Explicit field-by-field; no struct memcpy.
- **Frame layout:** Fixed header of `length` (u32, network byte order), `version` (u8/u16), `msg_type` (u16), followed by a type-specific payload.
  - `length`: total bytes after this field; makes the stream self-framing over TCP's boundary-less byte stream.
  - `version`: checked on the first frame so incompatible clients are rejected cleanly rather than misparsed.
  - `msg_type`: enum discriminating login, message-send, message-broadcast, presence-update, ack, error, etc.
- **Reading discipline:** Network thread reads in two phases (fixed header, then `length` more bytes), each looping to handle partial TCP reads. Everything is network byte order, converted at the edges.
- **TLS:** Standard PKI / Let's Encrypt, replacing the earlier per-operator cert-pinning design after the per-operator binary model was dropped in favor of a generic client.

## Client

- **Language:** Native C, explicitly rejecting Electron.
- **Rendering path:** raylib for window/paint/input, with HarfBuzz for text shaping and a Unicode line-break library for the message view. Lexbor evaluated but ruled out as a text-rendering engine (its layout/paint pipeline is not production-ready; only the HTML/CSS parser and DOM are stable).
- **Generic client:** A single generic client replaced the earlier per-operator custom-binary approach.

## Discovery

- **Rule:** Dot-based. Bare names (e.g. `acme`) resolve via the hosted service's DNS suffix; names containing a domain (e.g. `acme.com`) use SRV records plus optional `.well-known` metadata for self-hosters.

## Feature Set (Slack-comparable)

- **Threads:** Non-negotiable.
- **Search:** FTS5 full-text search, positioned as a competitive wedge against Slack's history caps.
- **Reactions:** Emoji reactions.
- **Notifications:** Per-channel settings and DND; push via APNs/FCM (free from providers). Self-hosted deployments need a small push gateway since the app is published under the maintainer's accounts.
- **Attachments:** Via object storage.
- **Other:** Message edit/delete, presence/typing indicators, incoming webhooks.
- **Audio conferencing:** Server-relay (no P2P/ICE), Opus codec, delivered as an isolated UDP-based sidecar process rather than embedded in the daemon's TCP loop.
- **Video:** Explicitly ruled out.

## Authentication

- **Architecture:** Client-drives-browser-flow using platform-native auth session APIs (ASWebAuthenticationSession on iOS/macOS, loopback redirect on desktop) with PKCE. The daemon only validates the returned JWT against provider JWKS.
- **Providers:** Microsoft Entra and Google are priority; Apple required for iOS App Store compliance; Facebook flagged as non-standard and low-value for this audience.

## Packaging and Distribution

- **Linux (self-hosted):** apt / `.deb` with a systemd unit, from a GPG-signed repo hosted on R2.
- **Windows:** Microsoft Store via MSIX packaging, chosen to avoid an Authenticode cert purchase.
