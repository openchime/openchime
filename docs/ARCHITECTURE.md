# OpenChime: Architectural Decisions

A consolidated record of the architecture decisions made for OpenChime, a cross-platform business chat application with a self-hosted open-source core and a paid hosted tier.

Business, product, and scope decisions live in [REQUIREMENTS.md](./REQUIREMENTS.md).

## Server / Daemon

- **ARCH-1 (Language):** C daemon.
- **ARCH-2 (Persistence):** SQLite in WAL mode.
- **ARCH-3 (Replication):** Litestream for continuous replication to object storage (R2/B2), run as an unmodified external companion process (Apache 2.0), not embedded or reimplemented. Embedding it was evaluated (Go, `cgo -buildmode=c-shared`) and rejected — it would pull the Go runtime/GC into an otherwise pure-C daemon and complicate Windows/macOS cross-compilation. A from-scratch C reimplementation (via `sqlite3_wal_hook`, avoiding Litestream's external WAL-polling) was also evaluated and rejected for this project's current scope — Litestream's years of production hardening against WAL/checkpoint edge cases aren't worth re-earning for a durability-critical component.
- **ARCH-4 (Deployment):** Fly.io, single-box-per-tenant island model. Cost is driven by concurrent load and data volume, not registered user count. HA (second machine + LiteFS) and heavy media egress are the triggers that change the economics, not user count alone.
- **ARCH-5 (Threading):** Two-thread model. The network thread owns the socket and framing; the DB writer thread owns the single SQLite write connection.
- **ARCH-22 (Network concurrency model):** The network thread is a single-threaded event loop (epoll/kqueue) multiplexing all client connections with non-blocking I/O, not thread-per-connection and not a worker pool. Fits the lean per-tenant memory profile (ARCH-4) and matches the two-thread model literally.
- **ARCH-23 (Durability and RPO):** The daemon acks a client's message send after the local SQLite WAL commit only; it does not block on Litestream's remote shipment (ARCH-3). Litestream's sync interval is set to 15 seconds, so the system's stated RPO is up to 15s of committed-but-unshipped messages on total box loss. This was a deliberate cost/latency tradeoff: at a 1s interval, worst-case replication request cost was estimated at ~$11.66/tenant/month (Litestream batches all changes within an interval into one R2/B2 write request, so cost is bounded by interval length, not message volume); 15s cuts that roughly 15x, with realistic usage costing pennies.
- **ARCH-24 (Cold-start recovery):** On startup, if no local database file is present, the daemon always restores from the latest Litestream snapshot in R2/B2 before serving traffic. If a local file is present (a normal restart on the same disk), it is used as-is and Litestream simply resumes shipping.
- **ARCH-25 (Health check):** A dedicated lightweight HTTP endpoint (`/healthz`), separate from the binary-protocol port, for the deployment platform's orchestrator to poll — not a connection through the custom wire protocol.
- **ARCH-26 (Configuration source):** Per-tenant configuration (OIDC/JWKS endpoints, DNS suffix, R2/B2 credentials) is a static config file read once at daemon startup. No runtime dependency on a reachable control plane, consistent with the single-box-per-tenant island model.
- **ARCH-27 (Schema migrations):** Sequential migrations are embedded in the daemon binary, numbered, and applied automatically against a `schema_version` table on startup, inside a transaction, before the daemon serves traffic. No separate migration tool or manual operator step.
- **ARCH-28 (Packaging and process model):** One binary, multiple processes. The daemon self-forks (or re-execs itself with a mode flag, e.g. `openchimed --mode=audio-sidecar`) to run companion processes like the audio relay (ARCH-18) as separate OS processes. Process isolation comes from `fork()`, not from being a separate build artifact, so this gets the same crash-isolation property as a separate binary while keeping one build artifact and a guaranteed version match between the daemon and its sidecars.
- **ARCH-29 (Upgrade rollout):** Restart-and-reconnect. A new daemon binary replaces the old one and restarts the process; connected clients see a dropped connection and reconnect automatically via the mechanism REQ-100/REQ-101 already require. No graceful connection draining — not worth the added complexity for a single-box, hobby-scale system.

## Wire Protocol

- **ARCH-6 (Decision):** Custom length-prefixed, versioned binary protocol over TLS/TCP. Not HTTP, not WebSocket, not JSON. Justified because both ends (C daemon and C client) are built together, removing any reason to pay HTTP/JSON overhead.
- **ARCH-7 (Serialization):** Explicit field-by-field; no struct memcpy.
- **ARCH-8 (Frame layout):** Fixed header of `length` (u32, network byte order), `version` (u8/u16), `msg_type` (u16), followed by a type-specific payload.
  - `length`: total bytes after this field; makes the stream self-framing over TCP's boundary-less byte stream.
  - `version`: checked on the first frame so incompatible clients are rejected cleanly rather than misparsed.
  - `msg_type`: enum discriminating login, message-send, message-broadcast, presence-update, ack, error, etc.
- **ARCH-9 (Reading discipline):** Network thread reads in two phases (fixed header, then `length` more bytes), each looping to handle partial TCP reads. Everything is network byte order, converted at the edges.
- **ARCH-10 (TLS):** The daemon generates its own self-signed certificate on first run. Clients trust it via TOFU (trust-on-first-connect) pinning, with the fingerprint optionally published in the `.well-known` discovery metadata (ARCH-14) for out-of-band verification. This applies uniformly to hosted and self-hosted tiers — no ACME, no Let's Encrypt, no certificate renewal machinery anywhere in the baseline system. Standard PKI was the original design but was dropped: the client is a native binary under the maintainer's control, not a browser, so it doesn't need CA-chain trust; and mandatory ACME is impossible for a self-hosted, internet-unreachable deployment (e.g. an air-gapped MSP client network), since ACME requires public HTTP reachability or a public DNS API. This is a different axis from the earlier per-operator cert-pinning design that was dropped in favor of a generic client — that was build-time pinning tied to per-operator binaries; TOFU is dynamic, runtime pinning, fully compatible with one generic client. The one exception is the incoming-webhooks endpoint (see "HTTP and Webhooks" below), which does need a CA-signed cert.
- **ARCH-30 (Protocol limits):** Conservative small-team defaults: max frame size and max message body size are ~64KB, and max concurrent connections per tenant is in the low hundreds. Attachments are never sent as protocol frames — they go through the object-storage path (ARCH-17) exclusively.

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
- **ARCH-31 (Audio sidecar IPC):** The daemon and the audio sidecar process (ARCH-18, ARCH-28) communicate over a Unix domain socket — native POSIX IPC via the same `socket()` API already used for TCP, no extra library, no network exposure.

## HTTP and Webhooks

- **ARCH-32 (Embedded HTTP listener):** The daemon embeds an HTTP/1.1 listener on ports 80/443, a separate port from the binary protocol (ARCH-6), fed from the same event loop as the binary-protocol listener (ARCH-22). Parsing uses picohttpparser (MIT/Perl dual-licensed, a single `.c`/`.h` request/header parser with no bundled socket or TLS handling and no competing event loop) — not a general embedded web server. Mongoose was ruled out (GPLv2/commercial dual license, not MIT/BSD); civetweb was ruled out because it brings its own connection-handling loop that would compete with ARCH-22's event loop. This endpoint serves the incoming-webhook receiver (see REQUIREMENTS.md REQ-170) and the health check (ARCH-25).
- **ARCH-33 (No HTTP/2):** The webhook/health HTTP surface is plain HTTP/1.1 only, matching ARCH-6's existing rejection of HTTP overhead for the core protocol. Webhook senders are third parties making single, independent requests; HTTP/2's multiplexing has no benefit for that pattern.
- **ARCH-34 (Webhook TLS):** A CA-signed certificate is obtained on-demand, feature-gated to tenants that enable webhooks — not a baseline daemon requirement (ARCH-10). This is the one surface where TOFU pinning doesn't work: third-party webhook senders (GitHub, Zapier, scripts) are not under the operator's control and validate TLS against a standard CA trust store with no ability to pin a custom cert.

## Authentication

- **ARCH-19 (Architecture):** Client-drives-browser-flow using platform-native auth session APIs (ASWebAuthenticationSession on iOS/macOS, loopback redirect on desktop) with PKCE. The daemon only validates the returned JWT against provider JWKS.

## Packaging and Distribution

- **ARCH-20 (Linux, self-hosted):** apt / `.deb` with a systemd unit, from a GPG-signed repo hosted on R2.
- **ARCH-21 (Windows):** Microsoft Store via MSIX packaging, chosen to avoid an Authenticode cert purchase.

## Local Development and Build

- **ARCH-35 (Build system):** A single hand-written `Makefile` (no CMake, no autotools), mirroring the sibling OpenBounty project's convention.
- **ARCH-36 (Local dev/test harness):** Docker Compose, not QEMU or Firecracker. Fly.io actually runs machines on Firecracker microVMs — Docker only shares the host kernel via namespaces/cgroups, so this doesn't replicate that isolation boundary — but it's the format Fly.io itself ingests as a build artifact (a Dockerfile becomes a Firecracker rootfs via `flyctl deploy`), and was chosen for local iteration speed and simplicity over fidelity.
- **ARCH-37 (Local base image):** Alpine (musl libc), matching the lean 256MB-per-tenant memory target (ARCH-4/REQUIREMENTS.md REQ-210). Litestream's glibc-linked release binary runs on it via the `gcompat` compatibility shim rather than a from-source build.
- **ARCH-38 (Local S3 simulation):** MinIO stands in for R2/B2 in the local Docker Compose environment, over LocalStack (heavier, broader-than-needed AWS emulation) and SeaweedFS (less battle-tested against Litestream specifically). MinIO server is AGPLv3-licensed; this is a dev/test-only dependency run via Compose, never embedded or distributed with OpenChime's own code, so no obligation attaches.
- **ARCH-39 (Local Litestream placement and sequencing):** Litestream runs in the same container as the daemon, matching the real single-box-per-tenant topology (ARCH-3/ARCH-4) where it needs direct filesystem access to the daemon's local SQLite file. The container's entrypoint sequences startup explicitly: attempt restore from the replica (ARCH-24) if no local DB file exists, fall back to initializing a fresh database if no replica exists either, start `litestream replicate` in the background, then exec the daemon in the foreground — avoiding a race where Litestream starts watching a file that doesn't exist yet.
- **ARCH-40 (Dev convenience scripts):** A `Scripts/` directory wraps the Docker Compose workflow: `run.sh` (build + start + block until the health check passes), `stop.sh` (stop, preserving local volumes), `reset.sh` (stop and wipe local volumes, for exercising the restore-on-boot path from a clean slate). These are thin wrappers with no behavior of their own beyond what `docker compose` already does directly — they exist to remove manual steps (creating `.env`, polling `/healthz`, remembering the container-removal-before-volume-removal ordering), not to hide anything.
