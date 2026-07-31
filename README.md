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
path, and echo cancellation), [docs/VIDEO.md](docs/VIDEO.md) (screenshare — the
codec-as-wire-contract decision and why it is sequenced behind
audio), [docs/CLIENT.md](docs/CLIENT.md) (the native client
architecture), [docs/WIN32_BACKLOG.md](docs/WIN32_BACKLOG.md) (the numbered
Windows GUI work list), [docs/MARKDOWN.md](docs/MARKDOWN.md) (the message-formatting dialect and where
it is parsed), [docs/CONFIG.md](docs/CONFIG.md) (every environment
variable the daemon reads), [docs/TESTING.md](docs/TESTING.md) (the unit +
integration test strategy), and [docs/VENDORS.md](docs/VENDORS.md) (every
third-party dependency, with source, version pinning, and license) for the
project's design. See [docs/CONTRIBUTING.md](docs/CONTRIBUTING.md) for the branch,
commit, and CI workflow.

The daemon is a **feature-complete v1 chat core**. On the foundations — the v1
wire-protocol frame codec (PROTOCOL.md), the two-thread model (ARCH-5: an epoll
network loop + a single DB-writer thread, refined by ARCH-66 into a third
read-only query thread), TLS termination with self-signed TOFU certs (ARCH-10,
mbedTLS), and schema migrations applied on boot (ARCH-27) — it runs the whole
messaging path end to end: **two-mode authentication** (local PBKDF2 accounts or
an OIDC token re-issued by the central relay, converging on a daemon-issued
session, [docs/AUTH.md](docs/AUTH.md)), roles and full tenant administration,
public/private channels and DMs, edit/delete, reactions, threads, FTS5 search,
presence and typing, notification preferences and DND, **@mentions** and
**pinned messages**, attachments proxied to object storage with per-channel file
and member listings, incoming webhooks, an audit log, and reconnect backfill. The
daemon also emits **mobile push** to the control-plane gateway (ARCH-85) and
**enrolls** with it for federated deployments (ARCH-84). Server-relayed **audio**
signaling and the UDP sidecar are built; the client-side codec is not
([docs/AUDIO.md](docs/AUDIO.md)).

For exactly what is built vs specified — including the gaps — see
**[docs/STATUS.md](docs/STATUS.md)**. Known remaining server-side work: a
CA-signed certificate for the webhook endpoint (REQ-171). The `MENTIONS`
notification level is no longer pending — @mentions (REQ-221) built it.

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

The first frontend is a **TUI** (`make tui`, built on the in-tree `tuikit`
toolbox over termbox2 + utf8proc, ARCH-83), built on the host like the daemon and
also shipping on Windows (ARCH-81). It is menu- and screen-driven — panels, context
menus, dialogs, and a Ctrl+K command palette; there are no slash commands. It
covers live messaging with history backfill, reactions, edit/delete, typing,
threads, search, channel + DM management, roster + presence, who-reacted,
notification prefs + DND, admin (roles/invite/remove), webhooks, attachments,
storage and audit overlays, multiple workspaces, and logout. It reached nearly
every capability the app-core exposes until the July 2026 engine and GUI work;
it is now behind the Windows GUI by **more than twenty** features — among them
@mentions, pins, saved items, the channel files listing, the per-channel member
roster, channel topic/rename/archive, mute, star, mark-unread, the activity feed,
in-app preferences and themes — plus webhook *deletion* and log-out-everywhere.
All of them already exist in the app-core, so closing the gap is TUI work alone;
the list and a proposed order are in
[docs/CLIENT_GAP_ANALYSIS.md](docs/CLIENT_GAP_ANALYSIS.md) §4–§5. Some frames the
daemon speaks reach no client at all yet — see [docs/CLIENT.md](docs/CLIENT.md)
§3. The app-core **writes nothing to disk** (ARCH-88/REQ-201): one credential per
workspace in the OS credential store carries the session token, the TOFU pin and
the workspace book, so it reconnects silently across restarts and queues sends
made while disconnected — in memory, for the life of the process. History comes
from the server's own read cursor rather than a local cache (REQ-100/101/102).

A native **Windows GUI** (Win32 + Direct2D/DirectWrite, pure C — ARCH-82) is the
most complete client: every tracked engine feature is reachable, its numbered
depth backlog is closed, and it now *leads* the TUI by more than twenty features.
An adversarial review on 2026-07-30 — then a 135-check smoke suite plus
hand-driven probing — found no defect in the closed backlog. Since then
accessibility shipped (REQ-269/ARCH-99: a UI Automation provider over the
self-drawn UI, a system caret and spoken notifications, verified by a real UIA
client from outside the process), the avatar and harness defects were fixed, and
the unreproduced typing crash was closed as living in a composer that no longer
exists. What remains in [docs/WIN32_BACKLOG.md](docs/WIN32_BACKLOG.md) is rich
text (dialect now settled, ARCH-100) and three items waiting on a daemon
requirement. The four-way surface analysis behind it is
[docs/CLIENT_GAP_ANALYSIS.md](docs/CLIENT_GAP_ANALYSIS.md). Next is **TUI
catch-up**, then GTK (Linux), AppKit (macOS), a web DOM UI, and mobile.

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
