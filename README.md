# OpenChime

A self-hostable business chat system: a C daemon, a shared C client app-core,
and native frontends.

**Three deployment models** (ARCH-76 in
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)):

| Model | Who runs the daemon | Depends on OpenChime-operated services |
|---|---|---|
| **Self-hosted stand-alone** | you | **none** — fully independent, and air-gappable. Federated-only features are absent, most visibly mobile push. |
| **Self-hosted federated** | you | opt-in, per function: OIDC login, push delivery, the app directory, SCIM, an optional DNS name, package distribution. You still run the daemon and own **all message data**. |
| **Hosted** | OpenChime | all of the above, operated for you. |

In every model, **no OpenChime-operated service is ever in the message path** —
the federated services broker identity, notification, discovery, and
provisioning metadata, and none can read message content. Federating trades
availability independence for capability, never message confidentiality
(REQ-040/041).

For what is actually built vs. specified, see
**[docs/STATUS.md](docs/STATUS.md)** — a reconciliation of every requirement
against the current tree, plus the server-robustness backlog.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md),
[docs/REQUIREMENTS.md](docs/REQUIREMENTS.md), and
[docs/PROTOCOL.md](docs/PROTOCOL.md) (the byte-level wire protocol spec for the
core messaging path), [docs/SCHEMA.md](docs/SCHEMA.md) (the SQLite schema and
migration mechanism), [docs/TLS.md](docs/TLS.md) (the mbedTLS-based transport
and TOFU trust model), [docs/AUTH.md](docs/AUTH.md) (the two-mode
authentication design), [docs/AUDIO.md](docs/AUDIO.md) (the huddle model, media
path, and echo cancellation), [docs/CLIENT.md](docs/CLIENT.md) (the native client
architecture), [docs/TESTING.md](docs/TESTING.md) (the unit +
integration test strategy), and [docs/VENDORS.md](docs/VENDORS.md) (every
third-party dependency, with source, version pinning, and license) for the
project's design.

The daemon is an **early skeleton**. It has the real foundations —
the v1 wire-protocol frame codec (PROTOCOL.md), the two-thread model
(ARCH-5: an epoll network loop + a single DB-writer thread), TLS termination
with self-signed TOFU certs (ARCH-10, mbedTLS), and schema migrations applied
on boot (ARCH-27) — plus the build/container pipeline (ARCH-35–39).
The network loop completes the TLS handshake, negotiates the protocol version
(`HELLO`→`WELCOME`/`REJECT`), authenticates a session, and runs the core
messaging path — a `SEND` is persisted (with a server-assigned monotonic id and
idempotent-retry dedup) and delivered to the channel's connected members as a
`BROADCAST`, with the sender acked; a reconnecting client can `BACKFILL_REQUEST`
the messages it missed and have them replayed. **Authentication is currently
stubbed** (any token is accepted and mapped to a user); the real design — two
modes (local accounts, or OIDC brokered by a central service) converging on a
daemon-issued session — is specified in [docs/AUTH.md](docs/AUTH.md) and is the
next implementation milestone. Channel management is a single auto-provisioned
default channel for now.

## Local build (daemon)

```
make
```

Requires a C toolchain and SQLite development headers (`sqlite3.h`,
`libsqlite3`) on the host.

## Client (app-core + native frontends)

The client ([docs/CLIENT.md](docs/CLIENT.md)) is **one shared C app-core with a
native UI per platform** (the tdlib model, ARCH-74). The app-core
(`client/core/`) is frontend-agnostic — it links the daemon's exact `shared/`
wire code, owns the network thread and the view-model, and is driven headlessly
by `make test` (`tests/test_client_core.c`). A standalone compile check:

```
make core
```

The first frontend is a **TUI** (`make tui`, termbox2 + utf8proc), built on the
host like the daemon. It covers live messaging with history backfill, reactions,
edit/delete, typing, threads, search, channel + DM management, roster + presence,
who-reacted, notification prefs + DND, admin (roles/invite/remove), and logout;
only attachments and webhook management remain to surface. Native GUIs
(Windows/macOS), a web DOM UI, and mobile follow.

## Local Docker environment

Requires Docker with Compose v2 (`docker compose`, not the standalone
`docker-compose` v1).

```
Scripts/run.sh
```

This copies `.env.example` to `.env` if missing, builds and starts the
stack, and waits for the daemon's health check before returning. Use
`Scripts/stop.sh` to stop the platform while keeping local data, or
`Scripts/reset.sh` to stop it and wipe the local DB + MinIO volumes
entirely, for a from-scratch start.

Equivalently, by hand:

```
cp .env.example .env
docker compose up --build
```

This starts three services:
- `minio` — S3-compatible object storage for local dev (ARCH-38). It currently
  has no consumer: the daemon's S3 blob backend (ARCH-70) defaults to the local
  filesystem and is not wired up here.
- `minio-init` — one-shot job that creates the dev bucket
- `daemon` — the daemon, resource-capped to approximate Fly.io's smallest
  instance (256MB / 1 shared vCPU, ARCH-4)

### Verify the health check

```
curl http://localhost:8080/healthz
```

Should return `OK`.

## Known fidelity gaps

- Docker containers share the host kernel; Fly.io actually runs machines on
  Firecracker microVMs. This setup validates build and runtime behavior, not
  kernel-level isolation.
- Docker's `cpus` limit approximates Fly's shared-cpu-1x but doesn't
  reproduce its exact throttling behavior.
