# OpenChime

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) and
[docs/REQUIREMENTS.md](docs/REQUIREMENTS.md) for the project's design.

This repo currently contains a **placeholder daemon** only — enough to
prove the build/container/replication pipeline works end-to-end (ARCH-35
through ARCH-39), not the real wire protocol or chat features (ARCH-6
onward, not yet implemented).

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
