# OpenChime — Daemon Configuration Reference

Every environment variable the daemon reads. **There is no configuration file**
(ARCH-26): the daemon is configured entirely from its process environment, read
once at startup into a single `oc_config` singleton (`daemon/config.c`). The one
file the loader opens is the OIDC public-key PEM named by
`OPENCHIME_OIDC_PUBKEY_FILE`.

Under systemd this is an `Environment=` / `EnvironmentFile=` block (ARCH-20);
in the container image it is the container environment (ARCH-4). Nothing here is
ever fetched from a control plane at runtime, in any deployment model — a
federated deployment contacts its opted-in services for their own function, never
to learn how to run (ARCH-26/76).

**The packages ship an `EnvironmentFile`, and it is not decoration.** The `.deb`,
`.rpm` and tarball install `/etc/openchime/openchimed.env` and the unit reads it.
It is marked as configuration (`conffiles` / `%config(noreplace)`), so an
upgrade preserves your edits. It **must** override the path defaults below: those
point at `/data`, which suits the container image, whereas the systemd unit runs
with `ProtectSystem=strict` and a `StateDirectory`, making `/var/lib/openchime`
the only writable path. The shipped file repoints the database, blobs and TLS
identity there. The table's defaults are therefore what the *binary* does, not
what a packaged install does.

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
| `OPENCHIME_TRUSTED_PROXIES` | *(none)* | Addresses and CIDR blocks, comma-separated, of TCP forwarders in front of the daemon. A connection from one must begin with a **PROXY protocol v2** header, read before TLS, and the client address it names is what the per-address connection cap and the sign-in limiter count; a trusted peer that sends none is closed. Nobody else's header is read. A list the daemon cannot parse stops the boot. |
| `OPENCHIME_DEPLOYMENT_MODE` | `standalone` | `standalone` \| `federated` \| `managed` — reported to clients in `WORKSPACE_INFO` (ARCH-76). Does **not** by itself enable federated services; those are gated on their own URLs. |
| `OPENCHIME_WORKSPACE_NAME` | *(empty)* | Human-readable workspace name, reported in `WORKSPACE_INFO`. |

## Authentication

| Variable | Default | Meaning |
|---|---|---|
| `OPENCHIME_AUTH_MODE` *(alias)* | `local` | Which built-in identity sources are on (ARCH-55): `local` (daemon-managed password accounts), `relay` (the central relay), or `local,relay`. `oidc` reads as `relay`. A name the daemon does not know stops the boot. |
| `OPENCHIME_MAX_USERS` | `0` (unlimited) | Registered-user cap; new-user creation is refused at the cap with `ERROR USER_LIMIT` across redeem / register / bootstrap / OIDC-JIT. Active users only — removing a member frees a seat (CP-7). |
| `OPENCHIME_BOOTSTRAP_USERS` *(alias)* | *(none)* | Dev/test seeding of local accounts. In normal operation the first owner comes from the one-time setup token logged at first run (ARCH-59). |
| `OPENCHIME_OIDC_ISSUER` *(alias)* | *(none)* | Expected `iss` on the relay-issued ES256 JWT. |
| `OPENCHIME_OIDC_AUDIENCE` *(alias)* | *(none)* | Expected `aud`. Normally left unset — the enrolled audience from the `enrollment` table wins (ARCH-84). |
| `OPENCHIME_OIDC_PUBKEY` *(alias)* | *(none)* | Central's pinned ES256 public keys, inline PEM — one block, or several during a rotation; a token's `kid` chooses among them. |
| `OPENCHIME_OIDC_PUBKEY_FILE` *(alias)* | *(none)* | The same, read from a file. **The only file the config loader reads.** |
| `OPENCHIME_OIDC_ALLOW` | *(none)* | Who may join by OIDC: a comma-separated list of `owner:<email>`, `tenant:google:<hosted domain>`, `tenant:microsoft:<tenant id>` and `domain:<domain>` rules (AUTH.md §8.4). Empty admits nobody new; a rule the daemon does not understand stops the boot. |

## Attachments and blob storage

Selection is by configuration, not a mode flag: **if the S3 credentials below are
present the S3 backend is used, otherwise the local filesystem is** (ARCH-70).

| Variable | Default | Meaning |
|---|---|---|
| `OPENCHIME_BLOB_DIR` | `/data/blobs` | Local-filesystem blob root. |
| `OPENCHIME_BLOB_BACKEND` | *(auto)* | Forces a backend (`fs` / `s3`) instead of inferring from the credentials. Mainly for tests. |
| `OPENCHIME_MAX_ATTACHMENT_SIZE` | `OC_MAX_ATTACHMENT_SIZE` (200 MiB) | Per-attachment byte ceiling, declared up front on `UPLOAD_BEGIN`. |
| `OPENCHIME_MAX_VIDEO_MESSAGE_SIZE` | `OC_MAX_VIDEO_MESSAGE_SIZE` (160 MiB) | Byte ceiling on a video message (REQ-164), checked by `ATTACH_MEDIA_SET`; clamped to the attachment ceiling. This, not the duration, is what bounds one: the daemon links no codec, so the length a client reports is shown, not verified. |
| `OPENCHIME_FILE_PAGE` | `OC_MAX_FILE_LIST` (200) | Rows in one page of a `LIST_FILES` answer (PROTOCOL.md §5.9b), clamped to 1–200. The client asks for the next page with the cursor the last row gives it, so this changes how many arrive at a time, not how many can be browsed. Mainly for tests, which want a page small enough to reach a second one. |
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
| `OPENCHIME_SCHED_TICK_MS` | `15000` (15 s) | Scheduled-send sweep interval (REQ-224/ARCH-102), separate from the maintenance pass because five minutes is not a send time. Values below 50 are ignored and the default stands, so a bad value cannot busy-loop the writer. |
| `OPENCHIME_AUDIT_MAX_DAYS` | `365` | Audit-log retention, applied **per family** so a flood of security noise cannot age out administrative history (REQ-251b). |

## Federated services (ARCH-84/85)

Both are **outbound only** — the daemon calls central, central never dials a
daemon (ARCH-56). Each is independently declinable; declining all of them is
exactly the self-hosted stand-alone model (ARCH-76).

| Variable | Default | Meaning |
|---|---|---|
| `OPENCHIME_ENROLL_URL` *(alias)* | *(none)* | Control-plane base URL. Setting it enables enrollment: the daemon generates a keypair + opaque audience, prints an `oce1.` code, and performs the challenge/confirm proof-of-possession. |
| `OPENCHIME_ENROLL_CODE_FILE` *(alias)* | *(none)* | Also write the `oce1.` code to this path, so orchestration can pick it up instead of scraping stderr. |
| `OPENCHIME_ENROLL_WAIT_SECS` *(alias)* | `0` | Seconds to wait for the operator to reserve the code before giving up for this boot. A managed box claiming with a ticket waits 120 when this is unset. |
| `OPENCHIME_ENROLL_TICKET` | *(none)* | A managed box's one-time ticket (AUTH.md §8.7). With it and `OPENCHIME_OIDC_AUDIENCE` set, the daemon adopts that audience, generates its key, and claims the binding at `OPENCHIME_ENROLL_URL` instead of printing a code. |
| `OPENCHIME_ENROLL_CA_BUNDLE` *(alias)* | *(system)* | CA bundle for the enrollment HTTPS client. |
| `OPENCHIME_PUSH_URL` *(alias)* | *(none)* | Push-gateway base URL. Push requires **both** this and an active enrollment, which is why it is absent in stand-alone deployments (ARCH-16/85). |
| `OPENCHIME_PUSH_CA_BUNDLE` *(alias)* | *(system)* | CA bundle for the push HTTPS client. |

## Link unfurls (REQ-222, ARCH-105)

Always on — there is deliberately **no on/off variable**. Every fetch passes an
SSRF gate (no loopback, private, link-local, CGNAT, multicast or reserved
destination — checked on every resolved address and re-checked per redirect)
and is capped in bytes, redirects and time; an air-gapped box's fetches simply
fail, bounded and silent.

| Variable | Default | Meaning |
|---|---|---|
| `OPENCHIME_UNFURL_CA_BUNDLE` | *(system)* | CA bundle for the fetcher's HTTPS client — the fourth CA consumer beside S3, enrollment and push (ARCH-10). |
| `OPENCHIME_WELLKNOWN_CA_BUNDLE` | *(system)* | Client. CA bundle for the `.well-known` discovery fetch (REQ-010, ARCH-10). Unset probes the usual system locations, which exist on Linux and the BSDs; Windows has none, so a client there consults the metadata only when this names a bundle. Without one the fetch is refused rather than made unverified, and resolution falls back to 443. |

## Audio

| Variable | Default | Meaning |
|---|---|---|
| `OPENCHIME_AUDIO_PORT` | `0` (ephemeral) | UDP port for the forked audio-relay sidecar (ARCH-28/73). **`0` means the kernel picks a free port, not that audio is off** — the daemon binds the socket, reads the assigned port back with `getsockname`, forks the sidecar unconditionally, and advertises that port in `CALL_JOINED`. Setting a value pins the port. The relay forwards opaque payloads — SFrame ciphertext (ARCH-113) — and the daemon never decodes a codec. A client behind NAT reaches it over UDP, so the port has to be reachable as the protocol port is. |
| `OPENCHIME_CALL_MAX` | `10` | The most people in one call (REQ-305), clamped to 2–32 and announced to clients on `WORKSPACE_INFO`. Every participant receives and mixes every other's stream (AUDIO.md §1.1), so this bounds each client's download and decoding as well as the relay's fan-out. |

## Read-aloud

Speech is built into the daemon (ARCH-111), and its voice model and pronunciation
data are files shipped beside it in the same package, so the feature is on unless it
is turned off or its data is missing. A daemon built with `make TTS=0` has no read-aloud
whatever these say, and tells its clients so.

| Variable | Default | Meaning |
|---|---|---|
| `OPENCHIME_TTS` | `1` | Read messages aloud. `0` turns it off: no renderings are made or served, and clients are told the feature is absent (REQ-295). |
| `OPENCHIME_TTS_QUEUE` | `256` | Renderings that may wait for the worker. A full queue answers `TTS_UNAVAILABLE` rather than growing; clamped to 1–4096. |
| `OPENCHIME_TTS_IDLE_SECS` | `300` | How long the model stays loaded with nothing to render. It loads on the first request (about a quarter of a second) and is released after this, so an idle tenant holds none of its working memory; clamped to 5–86400. |
| `OPENCHIME_TTS_RATE` | `60` | `AUDIO_GET`s one connection may make a minute. A listener plays messages one at a time, so this bounds a client asking for a whole history at once; clamped to 1–6000. |
| `OPENCHIME_TTS_DATA_DIR` | *(search)* | Where read-aloud's voice data is. Unset, the daemon uses `/usr/share/openchime/voices` (where the packages install it), else `voices/` beside its own executable (where the tarball and a source build put it). Set, it uses exactly that directory and never another. The directory's manifest is checked at startup: data that is absent, from a different build, or altered turns read-aloud off with the reason logged — the daemon starts either way. |
| `OPENCHIME_TTS_LANG` | `en-US` | The language messages are read in, as a BCP 47 tag. One language is built into the binary, and a value it cannot speak is refused at startup rather than fallen back from — reading every message in a language nobody asked for is worse than not starting. The tag is stamped in the pronunciation data and announced with every voice, so a client knows what it is hearing. |

**What it costs.** Nothing while nobody is listening: measured on one core, an
idle daemon with read-aloud built in holds 1.7 MB against 1.6 MB without it, and
the ~110 MB of model and pronunciation data stays on disk. While rendering, peak
memory is about 210 MB for a typical sentence and up to 315 MB for a long one, and
a rendering takes about six tenths of the time it takes to speak. A rendering is
about 3 KB a second of speech, stored until the disk needs the room.

## Voice input

Speech recognition is built into the daemon (ARCH-112) on the same engine as
read-aloud, and its recognizer's files ship beside it in the same package, so the
feature is on unless it is turned off or its data is missing. A daemon built with
`make STT=0` has no voice input whatever these say, and tells its clients so.

| Variable | Default | Meaning |
|---|---|---|
| `OPENCHIME_STT` | `1` | Let users speak into conversations. `0` turns it off: no speech is accepted, and clients are told the feature is absent (REQ-300). |
| `OPENCHIME_STT_QUEUE` | `64` | Segments that may wait for the recognizer, across every connection. A full queue answers `STT_UNAVAILABLE` rather than growing; clamped to 1–1024. |
| `OPENCHIME_STT_IDLE_SECS` | `300` | How long the recognizer stays loaded with nothing to hear. It loads on the first segment (under a tenth of a second) and is released after this; clamped to 5–86400. |
| `OPENCHIME_STT_RATE` | `60` | Segments one connection may send a minute. Free talk sends one at every pause, so this bounds a client sending far faster than anyone speaks; clamped to 1–6000. |
| `OPENCHIME_STT_MAX_SECS` | `30` | The longest segment accepted, announced to clients so they cut speech inside it. Moonshine recommends staying under about 30 seconds; clamped to 5–60. |
| `OPENCHIME_STT_DATA_DIR` | *(search)* | Where the recognizer's files are. Unset, the daemon uses `/usr/share/openchime/stt` (where the packages install them), else `stt/` beside its own executable (where the tarball and a source build put them). Set, it uses exactly that directory and never another. The manifest is checked at startup: data that is absent, from a different build, or altered turns voice input off with the reason logged, and read-aloud and the rest of the daemon start either way. |

**What it costs.** An idle daemon holds 0.1 MB more with voice input built in, and
the ~45 MB of recognizer files stay on disk until the first segment. Recognizing
takes about a tenth of the time the speech lasted, on one core; peak memory is
about 175 MB for a short sentence, 190 MB for six seconds and 360 MB for twenty.
No audio is kept: a segment is freed once answered.

---

## Client-side

The clients read a small, separate set. Machine-local TUI preferences otherwise
live in a hand-edited config file — `$XDG_CONFIG_HOME/openchime/config` (else
`~/.config/openchime/config`) on Linux/macOS, `%LOCALAPPDATA%\openchime\config`
on Windows — layered under the daemon's per-`(user, client_type)` settings bucket
(see [CLIENT.md](./CLIENT.md) §3). It holds only display preferences (`mouse`,
`members_panel`, `channels_width`, `members_width`, `time`) plus a default
`workspace`; it is the one file a client writes, and it is the *user's* document
rather than client state (ARCH-88/REQ-201).

| Variable | Used by | Meaning |
|---|---|---|
| `OPENCHIME_STATE` | TUI | A vestigial path (default `$HOME/.local/state/openchime/state`), read only as a **persistence on/off flag**: the client writes no file there (ARCH-88), and the store keeps everything in the OS credential store. An unset `HOME` with no override resolves to nothing, which disables persistence for that run. Resolving the path also **deletes** any `state.db`/`-wal`/`-shm` a pre-ARCH-88 build left behind. |
| `OPENCHIME_SUFFIX` | `resolve.c` | Overrides the DNS suffix appended to a bare workspace name (`acme` → `acme.<suffix>`), ARCH-14. **Defaults to `workspace.openchime.io`** (`OC_SERVICE_SUFFIX` in `client/core/resolve.h`, returned by `oc_default_suffix()`) — the hosted case is the common one, so a bare name resolves under the service domain unless this says otherwise. A dotted domain or an explicit `:port` bypasses suffixing entirely. |
| `OPENCHIME_CRED` | TUI | Credential passed as `user:password`, so a password never lands in the process arguments. |
| `OPENCHIME_TEST_DIR` | Win32 GUI | Enables the in-app automation hook — a file command channel plus the screenshot / state-dump drop directory — set by `scripts/gui_drive.sh` (which exports it across the WSL boundary via `WSLENV`) and used through it by `scripts/gui_smoke.sh` (the boot check). `scripts/gui_snap.sh` neither sets nor reads it: that script captures the window from outside with `PrintWindow`. Dev only. |

## Test-only knobs

Compile-time or test-harness values, listed so they are not mistaken for
deployment configuration: `OPENCHIME_UNFURL_ALLOW_PRIVATE` (disables the unfurl
fetcher's SSRF gate so a test can fetch a loopback fixture — never set it in a
deployment), `OC_FUZZ_RANDOM_ITERS` / `OC_FUZZ_FRAMED_ITERS` (fuzz
depth, defaults 30000 / 15000), `OC_NETLOOP_MAX_FD` (4096, a compile-time
constant, **not** an environment variable), `OC_AUDIO_SILENCE_MS` (sidecar
UDP silence sweep, which a test shortens with `oc_audio_sidecar_set_silence_ms`),
and, in the clients, `OPENCHIME_TEST_AUDIO`, `OPENCHIME_TEST_MIC` and
`OPENCHIME_TEST_TONE` — the synthetic audio devices, what the synthetic
microphone speaks, and its tone (440 Hz unless set), which `scripts/gui_calls.sh`
gives each of its two clients so either can tell whom it hears.
