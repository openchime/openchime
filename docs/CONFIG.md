# OpenChime — Daemon Configuration Reference

Every environment variable the daemon reads. **There is no configuration file**
(ARCH-26): the daemon is configured entirely from its process environment, read
once at startup into a single `oc_config` singleton (`daemon/config.c`). The one
file the loader opens is the OIDC public-key PEM named by
`OPENCHIME_OIDC_PUBKEY_FILE`.

Under systemd this is an `Environment=` / `EnvironmentFile=` block (ARCH-20);
under Docker/Fly it is the container environment (ARCH-4/36). Nothing here is
ever fetched from a control plane at runtime, in any deployment model — a
federated deployment contacts its opted-in services for their own function, never
to learn how to run (ARCH-26/76).

**Deprecated aliases.** Variables marked *alias* also accept an older `OC_`-
prefixed spelling. The alias still works and logs a one-line deprecation warning
to stderr; prefer the `OPENCHIME_` name.

---

## Core

| Variable | Default | Meaning |
|---|---|---|
| `OPENCHIME_DB_PATH` | `/data/openchime.db` | SQLite database file (WAL, ARCH-2). |
| `OPENCHIME_PROTO_PORT` | `8443` | The binary-protocol + ALPN-demuxed HTTP port. Production is 443 (ARCH-54). |
| `OPENCHIME_HEALTH_PORT` | `8080` | Plaintext `/healthz` + landing-page port (ARCH-25). |
| `OPENCHIME_TLS_CERT` | `/data/cert.pem` | Self-signed certificate path. Generated on first run; also persisted in the DB so the TOFU pin survives a restore (ARCH-10/66b). |
| `OPENCHIME_TLS_KEY` | `/data/key.pem` | Private key for the above. |
| `OPENCHIME_MAX_CONNS_PER_IP` | `256` | Accept-loop cap on concurrent connections from one source IP. |
| `OPENCHIME_DEPLOYMENT_MODE` | `standalone` | `standalone` \| `federated` \| `managed` — reported to clients in `WORKSPACE_INFO` (ARCH-76). Does **not** by itself enable federated services; those are gated on their own URLs. |
| `OPENCHIME_WORKSPACE_NAME` | *(empty)* | Human-readable workspace name, reported in `WORKSPACE_INFO`. |

## Authentication

| Variable | Default | Meaning |
|---|---|---|
| `OPENCHIME_AUTH_MODE` *(alias)* | `local` | `local` (daemon-managed password accounts) or `oidc` (central relay). One mode per tenant (ARCH-55). |
| `OPENCHIME_MAX_USERS` | `0` (unlimited) | Registered-user cap; new-user creation is refused at the cap with `ERROR USER_LIMIT` across redeem / register / bootstrap / OIDC-JIT. Active users only — removing a member frees a seat (CP-7). |
| `OPENCHIME_BOOTSTRAP_USERS` *(alias)* | *(none)* | Dev/test seeding of local accounts. In normal operation the first owner comes from the one-time setup token logged at first run (ARCH-59). |
| `OPENCHIME_OIDC_ISSUER` *(alias)* | *(none)* | Expected `iss` on the relay-issued ES256 JWT. |
| `OPENCHIME_OIDC_AUDIENCE` *(alias)* | *(none)* | Expected `aud`. Normally left unset — the enrolled audience from the `enrollment` table wins (ARCH-84). |
| `OPENCHIME_OIDC_PUBKEY` *(alias)* | *(none)* | Central's pinned ES256 public key, inline PEM. |
| `OPENCHIME_OIDC_PUBKEY_FILE` *(alias)* | *(none)* | Same key, read from a file. **The only file the config loader reads.** |
| `OPENCHIME_OIDC_PARAMS` *(alias)* | *(empty)* | Extra parameters advertised to the client in `AUTH_CHALLENGE`. |

## Attachments and blob storage

Selection is by configuration, not a mode flag: **if the S3 credentials below are
present the S3 backend is used, otherwise the local filesystem is** (ARCH-70).

| Variable | Default | Meaning |
|---|---|---|
| `OPENCHIME_BLOB_DIR` | `/data/blobs` | Local-filesystem blob root. |
| `OPENCHIME_BLOB_BACKEND` | *(auto)* | Forces a backend (`fs` / `s3`) instead of inferring from the credentials. Mainly for tests. |
| `OPENCHIME_MAX_ATTACHMENT_SIZE` | `OC_MAX_ATTACHMENT_SIZE` | Per-attachment byte ceiling, declared up front on `UPLOAD_BEGIN`. |
| `OPENCHIME_XFER_WORKERS` | `2` | Transfer-pool worker threads; blob I/O runs here, never on the net loop (ARCH-69). |
| `OPENCHIME_S3_ENDPOINT` | *(none)* | S3-compatible endpoint. An `https://` scheme (or any non-443 port with `http://`) selects the transport; HTTPS is CA-verified with hostname checking. |
| `OPENCHIME_S3_BUCKET` | *(none)* | Bucket name (path-style addressing). |
| `OPENCHIME_S3_ACCESS_KEY` | *(none)* | SigV4 access key. |
| `OPENCHIME_S3_SECRET_KEY` | *(none)* | SigV4 secret key. |
| `OPENCHIME_S3_REGION` | `us-east-1` | SigV4 region. |
| `OPENCHIME_S3_CA_BUNDLE` | *(system)* | CA bundle for the S3 TLS client — the one place OpenChime consults a CA store (ARCH-10/70). |

## Storage pressure and maintenance (ARCH-77/78)

Local-backend concerns; with external S3 only the database grows locally.

| Variable | Default | Meaning |
|---|---|---|
| `OPENCHIME_DB_RESERVE_MB` | `256` | Free space reserved for SQLite and never spent on attachments. Inviolable — this is what keeps messaging alive when attachment storage is exhausted (REQ-212). |
| `OPENCHIME_PRESSURE_MB` | `512` | Watermark below which reclamation starts. |
| `OPENCHIME_RECOVER_MB` | `1024` | Target free space reclamation works back up to (stops oscillation at a single boundary). |
| `OPENCHIME_EVICT` | on | Set to `off` to disable automatic oldest-first eviction (REQ-215), accepting upload refusal instead. |
| `OPENCHIME_EVICT_GRACE_HOURS` | `24` | Attachments younger than this are never evicted, so a file shared into a live conversation cannot vanish mid-discussion. |
| `OPENCHIME_ATTACH_MAX_AGE_DAYS` | `0` (keep forever) | Standing age policy; expiry runs every pass regardless of pressure (REQ-217). |
| `OPENCHIME_MAINT_INTERVAL_MS` | `300000` (5 min) | Maintenance-pass interval, driven off the net loop tick so an idle box is maintained too (REQ-218). |
| `OPENCHIME_MAINT_BATCH` | `64` | Maximum blobs reclaimed per pass. |
| `OPENCHIME_AUDIT_MAX_DAYS` | `365` | Audit-log retention, applied **per family** so a flood of security noise cannot age out administrative history (REQ-251b). |

## Federated services (ARCH-84/85)

Both are **outbound only** — the daemon calls central, central never dials a
daemon (ARCH-56). Each is independently declinable; declining all of them is
exactly the self-hosted stand-alone model (ARCH-76).

| Variable | Default | Meaning |
|---|---|---|
| `OPENCHIME_ENROLL_URL` *(alias)* | *(none)* | Control-plane base URL. Setting it enables enrollment: the daemon generates a keypair + opaque audience, prints an `oce1.` code, and performs the challenge/confirm proof-of-possession. |
| `OPENCHIME_ENROLL_CODE_FILE` *(alias)* | *(none)* | Also write the `oce1.` code to this path, so orchestration can pick it up instead of scraping stderr. |
| `OPENCHIME_ENROLL_WAIT_SECS` *(alias)* | `0` | Seconds to wait for the operator to reserve the code before giving up for this boot. |
| `OPENCHIME_ENROLL_CA_BUNDLE` *(alias)* | *(system)* | CA bundle for the enrollment HTTPS client. |
| `OPENCHIME_PUSH_URL` *(alias)* | *(none)* | Push-gateway base URL. Push requires **both** this and an active enrollment, which is why it is absent in stand-alone deployments (ARCH-16/85). |
| `OPENCHIME_PUSH_CA_BUNDLE` *(alias)* | *(system)* | CA bundle for the push HTTPS client. |

## Audio

| Variable | Default | Meaning |
|---|---|---|
| `OPENCHIME_AUDIO_PORT` | `0` (disabled) | UDP port for the forked audio-relay sidecar (ARCH-28/73). The relay forwards opaque payloads; the daemon never decodes a codec. |

---

## Client-side

The clients read a small, separate set. Machine-local preferences otherwise live
in `~/.config/openchime/config` (TUI), layered under the daemon's per-`(user,
client_type)` settings bucket — see [CLIENT.md](./CLIENT.md) §3.

| Variable | Used by | Meaning |
|---|---|---|
| `OPENCHIME_STATE` | core store | Overrides the client store path (default `$HOME/.local/state/openchime/state.db`; `%LOCALAPPDATA%` on Windows). |
| `OPENCHIME_SUFFIX` | `resolve.c` | DNS suffix appended to a bare workspace name (`acme` → `acme.<suffix>`), ARCH-14. |
| `OPENCHIME_CRED` | TUI | Credential passed as `user:password`, so a password never lands in the process arguments. |
| `OPENCHIME_TEST_DIR` | Win32 GUI | Enables the in-app automation hook (screenshot / state-dump drop directory) used by `scripts/gui_snap.sh`. Dev only. |

## Compose-only

| Variable | Default | Meaning |
|---|---|---|
| `OPENCHIME_BUCKET` | `openchime-dev` | Read by `docker-compose.yml` to create a MinIO bucket in the dev stack. **The daemon never reads it** — its bucket is `OPENCHIME_S3_BUCKET`. Per ARCH-38 the MinIO service currently has no consumer, since the blob backend defaults to the local filesystem and is not wired into compose. |

## Test-only knobs

Compile-time or test-harness values, listed so they are not mistaken for
deployment configuration: `OC_FUZZ_RANDOM_ITERS` / `OC_FUZZ_FRAMED_ITERS` (fuzz
depth, defaults 30000 / 15000), `OC_NETLOOP_MAX_FD` (4096, a compile-time
constant, **not** an environment variable), and `OC_AUDIO_SILENCE_MS` (sidecar
UDP silence sweep).
