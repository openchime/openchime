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

**Where this differs from the reference products.** Per-message read receipts
("seen by", REQ-090) exist here and in neither Slack nor Pumble, which both
decline them deliberately. Self-hosting, data residency and the three deployment
models are ours alone — both references are single-cloud and SaaS-only. The
clients are native C rather than Electron or web-tech. Two things are **not**
differentiators and are recorded as such: unlimited history is matched by
Pumble's free tier, so the argument there is data ownership rather than
retention; and on breadth of client platforms both references are ahead, since
this project ships a Windows GUI and a Linux/Windows TUI and no macOS, Linux
GUI, web or mobile client. On single sign-on the position is weaker still — see
REQ-027.

## The documents

Sixteen, and each has one job. **Start with the first three**: they answer what
the product is meant to do, what is wrong with it today, and why it is built the
way it is.

| Document | What it is |
|---|---|
| [REQUIREMENTS.md](docs/REQUIREMENTS.md) | The product specification — every `REQ-NNN`, written as a contract in present-perfect. Each requirement carries a marker saying whether it is built, so a shipped guarantee reads differently from an intention. |
| [BACKLOG.md](docs/BACKLOG.md) | **The project's only issue list.** 118 numbered items covering the Win32 client and the daemon, ordered lowest impact to highest, each with the evidence that verified it. |
| [ARCHITECTURE.md](docs/ARCHITECTURE.md) | The `ARCH-N` decision record: every architectural choice, its rationale, and what later decisions amended or withdrew it. Design decisions live here; product scope lives in REQUIREMENTS.md. |
| [PROTOCOL.md](docs/PROTOCOL.md) | The byte-level wire protocol — frame layout, the handshake, every message type and its payload, the error codes, and the connection state machine. Its §9 registry is generated from the codec and is the authority on which opcodes are taken. |
| [SCHEMA.md](docs/SCHEMA.md) | The SQLite schema and the migration mechanism, documenting all 36 migrations and why each table is shaped the way it is. |
| [CLIENT.md](docs/CLIENT.md) | The client architecture: one shared C app-core with a native UI per platform, and the internals of the Win32 GUI — its self-drawn composer, modal frame, and native-child rules. |
| [AUTH.md](docs/AUTH.md) | The two authentication modes — local PBKDF2 accounts and OIDC brokered by a central relay — and the daemon-issued session both converge on. |
| [TLS.md](docs/TLS.md) | How the daemon terminates TLS and how a client trusts it: trust-on-first-use certificate pinning against a self-signed cert, with no CA anywhere on the client-facing path. |
| [CONFIG.md](docs/CONFIG.md) | Every environment variable the daemon reads, with its default and meaning. There is no configuration file. |
| [TESTING.md](docs/TESTING.md) | The test strategy and its two tiers, what CI runs, the measured capacity benchmark, and how to bring the federated stack up by hand. |
| [MARKDOWN.md](docs/MARKDOWN.md) | The message-formatting dialect — a Slack-compatible subset plus real lists — where it is parsed, and the places it deliberately differs. |
| [AUDIO.md](docs/AUDIO.md) | The design for server-relayed audio calls: the huddle model, client-side mixing, and echo cancellation. The server half is built; the client half is not. |
| [VIDEO.md](docs/VIDEO.md) | The screenshare design: why the codec is a wire contract rather than a per-platform choice, and why it is sequenced behind the audio client. Not started. |
| [TUIKIT.md](docs/TUIKIT.md) | The in-tree TUI widget toolbox the terminal client is built on — deliberately generic, and knowing nothing about chat. |
| [VENDORS.md](docs/VENDORS.md) | Every third-party dependency, how it enters the build, and its licence. |
| [CONTRIBUTING.md](docs/CONTRIBUTING.md) | Branch, commit and CI policy, including the attribution guard that runs on every push. |

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

For what is known to be wrong with it, see
**[docs/BACKLOG.md](docs/BACKLOG.md)**. Three daemon defects are open there, two
of them reproduced against a running daemon: incoming webhooks are unreachable by
ordinary HTTPS clients (the daemon advertises only the `oc/1` ALPN protocol), a
webhook that is reachable can post into an archived channel, and two message
types share an opcode so editing a profile drops the connection. Remaining
server-side scope includes a CA-signed certificate for the webhook endpoint
(REQ-171).

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
All of them already exist in the app-core, so closing the gap is TUI work alone.
The frontend order is fixed — all of Win32 first — so the TUI's gap is not
tracked as backlog. Some frames the daemon speaks reach no client at all yet —
see [docs/CLIENT.md](docs/CLIENT.md) §3. The app-core **writes nothing to disk** (ARCH-88/REQ-201): one credential per
workspace in the OS credential store carries the session token, the TOFU pin and
the workspace book, so it reconnects silently across restarts and queues sends
made while disconnected — in memory, for the life of the process. History comes
from the server's own read cursor rather than a local cache (REQ-100/101/102).

A native **Windows GUI** (Win32 + Direct2D/DirectWrite, pure C — ARCH-82) is the
most complete client: every tracked engine feature is reachable, and it leads the
TUI by a wide margin. Accessibility is built (REQ-269/ARCH-99: a UI Automation
provider over the self-drawn UI, a system caret and spoken notifications), with
an automation id and an invoke pattern on every actionable element (REQ-290),
both verified by a real UIA client from outside the process. Rich text and its
toolbar (REQ-220/ARCH-100), drafts, scheduled send, the notification schedule
with keywords and priority people, cross-channel threads and the People directory
all shipped with their daemon halves on 2026-08-01/02.

Its `scripts/gui_smoke.sh` gate reported **247 of 249 checks passing** on
2026-08-02. The two failures, two further defects the gate structurally cannot
see, and the verification gap underneath them are in
[docs/BACKLOG.md](docs/BACKLOG.md). Next is **TUI catch-up**, then GTK (Linux),
AppKit (macOS), a web DOM UI, and mobile.

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
