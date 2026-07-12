# OpenChime

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md),
[docs/REQUIREMENTS.md](docs/REQUIREMENTS.md), and
[docs/PROTOCOL.md](docs/PROTOCOL.md) (the byte-level wire protocol spec for the
core messaging path), [docs/SCHEMA.md](docs/SCHEMA.md) (the SQLite schema and
migration mechanism), [docs/TLS.md](docs/TLS.md) (the mbedTLS-based transport
and TOFU trust model), and [docs/TESTING.md](docs/TESTING.md) (the unit +
integration test strategy) for the project's design.

The daemon is an **early skeleton**. It has the real foundations —
the v1 wire-protocol frame codec (PROTOCOL.md), the two-thread model
(ARCH-5: an epoll network loop + a single DB-writer thread), TLS termination
with self-signed TOFU certs (ARCH-10, mbedTLS), and schema migrations applied
on boot (ARCH-27) — plus the build/container/replication pipeline (ARCH-35–39).
The network loop completes the TLS handshake, negotiates the protocol version
(`HELLO`→`WELCOME`/`REJECT`), authenticates a session, and runs the core
messaging path — a `SEND` is persisted (with a server-assigned monotonic id and
idempotent-retry dedup) and delivered to the channel's connected members as a
`BROADCAST`, with the sender acked. **Authentication is currently stubbed** (the
token is accepted and mapped to a user; real OIDC/JWT validation against a
provider's JWKS is the next milestone), and channel management is a single
auto-provisioned default channel for now.

## Local build

```
make
```

Requires a C toolchain and SQLite development headers (`sqlite3.h`,
`libsqlite3`) on the host.

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
entirely (useful before a from-scratch restore-on-boot test below).

Equivalently, by hand:

```
cp .env.example .env
docker compose up --build
```

This starts three services:
- `minio` — S3-compatible object storage simulating R2/B2 (ARCH-38)
- `minio-init` — one-shot job that creates the replication bucket
- `daemon` — the placeholder daemon + Litestream, resource-capped to
  approximate Fly.io's smallest instance (256MB / 1 shared vCPU, ARCH-4)

### Verify the health check

```
curl http://localhost:8080/healthz
```

Should return `OK`.

### Verify replication reached MinIO

Open the MinIO console at [http://localhost:9001](http://localhost:9001)
(login with the credentials from `.env`) and check the `openchime-dev`
bucket for an `openchime/` prefix — Litestream should have shipped at least
one snapshot/WAL segment within 15 seconds of the daemon starting
(ARCH-23), and roughly every 5 seconds thereafter as the placeholder
daemon's heartbeat writes accumulate.

Or from the command line:

```
docker compose exec minio-init sh -c "mc ls local/openchime-dev/openchime"
```

### Verify restore-on-boot

This is the actual end-to-end proof that recovery works (ARCH-24):

```
docker compose stop daemon
docker compose rm -f daemon
docker volume rm openchime_daemon-data
docker compose up -d daemon
```

(Or `Scripts/reset.sh` for a full wipe of both the daemon's DB and MinIO's
data, then `Scripts/run.sh` again.)

Watch the daemon's logs — it should print `no local DB found, attempting
restore from replica...` and successfully restore from MinIO before
`healthz` starts responding, rather than starting fresh.

## Known fidelity gaps

- Docker containers share the host kernel; Fly.io actually runs machines on
  Firecracker microVMs. This setup validates build/replication/recovery
  behavior, not kernel-level isolation.
- Docker's `cpus` limit approximates Fly's shared-cpu-1x but doesn't
  reproduce its exact throttling behavior.
