# OpenChime — Daemon Configuration Reference

Everything that controls how the daemon runs: every environment variable it
reads, then the state in its database that governs it, the SQLite settings it
fixes in code, its command line and its build switches. **There is no
configuration file** (ARCH-26): the daemon is configured entirely from its
process environment, read once at startup into a single `oc_config` singleton
(`daemon/config.c`) from one registry (`daemon/config_table.c`), which is also
where the reference below, the packaged environment file and
`openchimed --check-config` come from. The files the loader opens are the ones
the data directory holds and the files the environment names (any extra CA
roots, a direct connection's secret).

**The surface is small, on purpose.** An operator sees two kinds of row:
**features**, each a flag that turns a capability on or off with its one value
where there is anything to configure (`OPENCHIME_TLS=acme:chat.example.com`,
`OPENCHIME_S3=s3://…`, `OPENCHIME_SUMMARY=cloud:<url>,<model>`), and
**settings**, values with no feature behind them (the name, the data
directory, the ports, who may join, the mode). Twenty in all. Every path is under one
`OPENCHIME_DATA_DIR` at a fixed name, the deployment model is derived from
what the box was started with rather than declared, and everything nobody
varies — session life, worker counts, queue depths, storage watermarks, rate
limits, the call cap — is a constant in `daemon/config.h`, not a row. Seven
more rows are the control plane's, written when it provisions a managed box
("A managed box", below); set without its ticket they refuse the start.
Four more, and twelve the clients read, are development inputs, which only
development mode reads ("Mode", below).

**The daemon is strict about its environment.** A value that does not parse,
a value outside its range, a name it does not know (a typo, or a name it no
longer reads), a managed row without the ticket and, outside development
mode, a development input each stop the daemon before it opens anything, with one
line naming the variable, the value and what is accepted. An empty value is
unset. There is no lenient mode:
`openchimed --check-config --env-file /etc/openchime/openchimed.env` runs the
same loader over a file and prints every effective value with its source, so
an edit is tested before a restart (the packages run it on install).

Under systemd this is an `Environment=` / `EnvironmentFile=` block (ARCH-20);
in the container image it is the container environment (ARCH-4). Nothing here is
ever fetched from a control plane at runtime, in any deployment model — a
federated deployment contacts its opted-in services for their own function, never
to learn how to run (ARCH-26/76).

**The packages ship an `EnvironmentFile`.** The `.deb`, `.rpm` and tarball
install `/etc/openchime/openchimed.env` and the unit reads it. It is marked as
configuration (`conffiles` / `%config(noreplace)`), so an upgrade preserves
your edits. It is mode `0600`, owned by root — it can hold an S3 key and a
summary API key, and it is systemd that reads it, not the daemon's own user —
and an install tightens a file an older one left wider. It sets the one thing
a packaged install must: `OPENCHIME_DATA_DIR=/var/lib/openchime`, the unit's
`StateDirectory`, since the binary's own default is `/data`, which suits the
container image and exists nowhere else.

**The data directory's layout** is fixed: `openchime.db` (the database, which
the daemon creates and migrates), `blobs/` (attachments on local disk, unless
`OPENCHIME_S3`), `cert.pem` and `key.pem` (the certificate presented, written
by the daemon or, with `OPENCHIME_TLS=file`, yours), `factor.key` (the key
local accounts' second-step secrets are sealed under: back it up with the
database) and `summary/` (the local summary model, fetched at the first start with
`OPENCHIME_SUMMARY=local`). The
daemon makes the directory if it is absent (not its parents), and everything
inside it. The speech features'
data and the web client are not configuration: they are found at
`/usr/share/openchime/{tts,stt,web}`, else beside the executable.

**No aliases.** A name that was a setting once is not read; a daemon started
with one says what replaced it, or the constant it became, and stops (the
table at the end lists them).

## Mode

`OPENCHIME_MODE` says what a box is for, and it is the only thing that does:
there is one build, and the same binary is a deployment, a test fixture or
the public demo by this setting alone. The daemon and every client read it.

| Mode | For | What it changes |
|---|---|---|
| `normal` (the default) | every deployment, self-hosted or managed | Nothing: the constants hold, and a development input refuses the daemon's start (a client ignores one). |
| `development` | the tests, CI, the GUI and web harnesses, the scripts, a box of your own | The development inputs below are read: fixture accounts, a smaller call or file page, no per-address connection cap, and the clients' synthetic devices, test hook and sign-in typing. Enrolment and certificates through central reach a loopback test service only; the managed rows are taken without the ticket, so a fixture can stand in for a managed box. The startup log says so in capitals. |
| `demo` | the public demo workspace | A development input refuses the start, as in `normal`. Needs `OPENCHIME_AUTH=local,relay`. Visitors sign up with the control plane's signed invite; daily limits per person and per workspace; one-day sessions; sample content at first start; every client says it is a public demo (AUTH.md §8.12). |

Nothing that ships sets `development`: `make test` checks that no release
path (the workflows, the Dockerfile, `packaging/`) names it or a development
input (`scripts/check/release_lint.sh`). The test code behind the inputs is
in every binary and does nothing outside development mode.

**The web client takes its mode from the daemon that serves it.** A browser
has no environment, so the page builds one, and its own address is not a
source the mode may come from: a link must not be able to turn the harness on
in a visitor's browser. The daemon writes its mode into the page it serves at
`/app/`; only a page from a development daemon (or from
`scripts/web/serve.py`, which serves one the same way) honours `?env=`,
`?hook=`, `?cmd=`, `?shot=` and `?mic=`, and `?env=` never sets
`OPENCHIME_MODE`.

---

<!-- config-reference -->
<!-- Generated by `openchimed --config-reference` from daemon/config_table.c; do not edit by hand (scripts/check/config.sh). -->

## Settings and features

Everything an operator may set. A value that does not parse, is out of its range, or names a setting that does not exist stops the daemon before it opens anything; an empty value is unset. `openchimed --check-config` tests a file before a restart.

### Settings

| Variable | Default | Meaning |
|---|---|---|
| `OPENCHIME_MODE` | `normal` | What the box is for: `normal` (a deployment), `development` (the tests and harnesses; the only mode that reads the Development rows) or `demo` (the public demo workspace). One of `normal`, `development`, `demo`. |
| `OPENCHIME_WORKSPACE_NAME` | *(empty)* | The workspace's display name, shown to clients; unset, a client derives one from the host. |
| `OPENCHIME_MAX_VIDEO_SIZE` | `167772160` | The most bytes one video message may hold (REQ-164), at most the attachment cap of 209715200; clients learn it at sign-in and record within it. 160 MiB by default. Integer 1048576..209715200. |
| `OPENCHIME_DATA_DIR` | `/data` | Where the daemon keeps everything: `openchime.db`, `blobs/`, `cert.pem`, `key.pem`, `factor.key`, `summary/`. The packages set `/var/lib/openchime`. |
| `OPENCHIME_PORT` | `8443` | The TLS port the protocol, the web client and the sign-in pages are served on, and the UDP port calls use (443 for a real deployment, ARCH-54). Integer 1..65535. |
| `OPENCHIME_HEALTH_PORT` | `8080` | The plaintext port for `/healthz` and the landing page (ARCH-25); 0 opens none. Integer 0..65535. |
| `OPENCHIME_JOIN` | *(unset)* | Who may join by signing in through a provider: `domain:`, `tenant:`, `subject:` and `owner:` rules, comma-separated (AUTH.md §8.4). Unset: only by invitation. |
| `OPENCHIME_OIDC_CONNECT_<n>` | *(unset)* | A direct connection to your own provider, 1 to 4 (SSO.md): `label=…;issuer=https://…;client_id=…;secret_file=…;subject=sub|oid;redirect=127.0.0.1|localhost`. |
| `OPENCHIME_EXTRA_CA` | *(unset)* | PEM roots trusted beside the built-in ones for every outbound connection (push, enrollment, unfurls, S3): a service behind a private CA. |
| `OPENCHIME_TRUSTED_PROXIES` | *(unset)* | Addresses or CIDRs whose PROXY protocol v2 header is believed, comma-separated; the connection's address is then the one the proxy saw. |

### Features

| Variable | Default | Meaning |
|---|---|---|
| `OPENCHIME_TLS` | `self` | The certificate the daemon presents (TLS.md): `self` (its own, made at first run), `file` (`cert.pem` and `key.pem` in the data directory), `acme:<names>[;email=…][;directory=…]` (RFC 8555 for the comma-separated names), or `central` (through the control plane; the default on a managed box). |
| `OPENCHIME_AUTH` | `local` | Where accounts come from: `local` (passwords held here), `relay` (sign-in through the control plane this box is enrolled with, ARCH-56), or `local,relay`. |
| `OPENCHIME_MFA` | `optional` | Whether local accounts are asked for a second step (AUTH.md §8.6). One of `off`, `optional`, `required`. |
| `OPENCHIME_S3` | *(unset)* | Attachments in S3 instead of under the data directory (ARCH-70): `s3://KEY:SECRET@host[:port]/bucket[?region=…]`, or `http+s3://` for a plaintext endpoint; percent-encode `/`, `+` and `@` in the key and secret. Unset: local disk. |
| `OPENCHIME_TTS` | `on` | Read-aloud (ARCH-111), in a build that carries it; off saves the voice model's memory. `on` or `off`. |
| `OPENCHIME_STT` | `on` | Voice input (ARCH-112), in a build that carries it. `on` or `off`. |
| `OPENCHIME_SUMMARY` | `off` | Summaries (REQ-310): `off`, `local` (the model runs inside the daemon, fetched on first start), or `cloud:<url>,<model>` (a hosted API's base URL and the model there). |
| `OPENCHIME_SUMMARY_API_KEY` | *(empty)* | The hosted API's key, for `cloud`. |
| `OPENCHIME_ENROLL_URL` | *(unset)* | The control plane to enrol with (ARCH-84), which makes the box federated: https, or http to loopback for a test. Prints an `oce1.` code to the log on first run. |
| `OPENCHIME_PUSH_URL` | *(unset)* | The mobile push relay (ARCH-16/85), contentless; needs an enrollment. https, or http to loopback for a test. |
| `OPENCHIME_INVITE_MAIL` | `off` | Whether invitations bound to an address are reported to central for it to mail (REQ-280); needs an active enrollment. One of `off`, `on`. |

## Managed

Written by the control plane when it provisions a managed box ("A managed box", below), with `OPENCHIME_MANAGED_TICKET`; set without the ticket, each refuses the start.

### Managed

| Variable | Default | Meaning |
|---|---|---|
| `OPENCHIME_MANAGED_TICKET` | *(unset)* | The one-time claim ticket; with it the box is managed and the other rows here may be set. |
| `OPENCHIME_MANAGED_AUDIENCE` | *(unset)* | The audience central minted for this workspace; the relay's tokens carry it. |
| `OPENCHIME_MANAGED_RELAY_KEY` | *(unset)* | A relay public key, PEM, pinned beneath the keys the relay publishes (AUTH.md §8.3). |
| `OPENCHIME_MANAGED_MAX_USERS` | `0` | The most registered users (CP-7); 0 is unlimited. Integer 0..1000000. |
| `OPENCHIME_MANAGED_ADDRESS` | *(empty)* | The workspace's canonical address, when it moved and still answers at the old one; clients re-file it under this. |
| `OPENCHIME_MANAGED_AUDIO_ADVERTISE_PORT` | `0` | The UDP port clients are told, where a front door maps it (AUDIO.md §4); 0 is the bound one. Integer 0..65535. |
| `OPENCHIME_MANAGED_AUDIO_TOKEN_PREFIX` | *(unset)* | Hex bytes (at most 16) that lead every relay token, so a front door can route by them. |

## Development mode only

Read by the daemon or a client only when `OPENCHIME_MODE=development` (TESTING.md): the tests, the harnesses and the scripts. In any other mode each refuses the daemon's start, and a client ignores it.

### Development

| Variable | Default | Meaning |
|---|---|---|
| `OPENCHIME_DEV_USERS` | *(unset)* | Accounts made at first start, `name:password:role,…`, for a fixture; a real deployment's first owner comes from the setup token. |
| `OPENCHIME_DEV_FILE_PAGE` | `200` | How many files one LIST_FILES page carries, so a test sees a second page. Integer 1..200. |
| `OPENCHIME_DEV_CALL_MAX` | `10` | The most people in one call, so a test can fill one. Integer 2..32. |
| `OPENCHIME_DEV_CONNS_PER_IP` | `1024` | Connections one address may hold; 0 is no cap, for a load test from one host. Integer 0..1000000. |
| `OPENCHIME_DEV_DIR` | *(unset)* | GUI: the directory of the automation hook's command channel and dumps (TESTING.md). |
| `OPENCHIME_DEV_AUDIO` | *(unset)* | Clients: `synthetic` audio devices, or `mic-denied`. |
| `OPENCHIME_DEV_MIC` | *(unset)* | Clients: a WAV the synthetic microphone speaks. |
| `OPENCHIME_DEV_MIC_LEAD_MS` | *(unset)* | Clients: silence before the WAV, in ms. |
| `OPENCHIME_DEV_TONE` | *(unset)* | Clients: the synthetic microphone's tone in Hz (20 to 7000; 440 unset). |
| `OPENCHIME_DEV_MIC_ECHO` | *(unset)* | Clients: the speaker echoed into the microphone (`1`), or alone (`only`). |
| `OPENCHIME_DEV_CAPTURE` | *(unset)* | Clients: `synthetic` camera and screen, `synthetic-camera`, or `denied`. |
| `OPENCHIME_DEV_SCREEN_GONE_MS` | *(unset)* | Clients: the synthetic screen ends after this many ms. |
| `OPENCHIME_DEV_VIDEO_CAP_MS` | *(unset)* | Clients: a shorter video message cap, in ms. |
| `OPENCHIME_DEV_CALL_TIMERS` | *(unset)* | Clients: `probe:<ms>,udp_wait:<ms>,udp_lost:<ms>,tcp_probe:<ms>`, any subset. |
| `OPENCHIME_DEV_SUFFIX` | *(unset)* | Clients: the DNS suffix appended to a bare workspace name instead of the service's (ARCH-14). |
| `OPENCHIME_DEV_SIGNIN` | *(unset)* | Web client: `user:password` the harness types into the daemon's sign-in page for the person (TESTING.md). |

## No longer read

A name that was a setting once. Set, it stops the daemon with the line below; the constants it names are in `daemon/config.h`.

| Name | Instead |
|---|---|
| `OPENCHIME_DEPLOYMENT_MODE` | derived: managed with OPENCHIME_MANAGED_TICKET, federated with OPENCHIME_ENROLL_URL, else standalone |
| `OPENCHIME_DB_PATH` | the database is openchime.db under OPENCHIME_DATA_DIR |
| `OPENCHIME_BLOB_DIR` | attachments are blobs/ under OPENCHIME_DATA_DIR |
| `OPENCHIME_TLS_CERT` | cert.pem under OPENCHIME_DATA_DIR |
| `OPENCHIME_TLS_KEY` | key.pem under OPENCHIME_DATA_DIR |
| `OPENCHIME_FACTOR_KEY_FILE` | factor.key under OPENCHIME_DATA_DIR |
| `OPENCHIME_TTS_DATA_DIR` | the voice data is found at /usr/share/openchime/tts, else tts/ beside the executable |
| `OPENCHIME_STT_DATA_DIR` | the recognizer's data is found at /usr/share/openchime/stt, else stt/ beside the executable |
| `OPENCHIME_PROTO_PORT` | renamed OPENCHIME_PORT |
| `OPENCHIME_AUDIO_PORT` | the call relay's UDP port is OPENCHIME_PORT |
| `OPENCHIME_WORKSPACE_ADDRESS` | renamed OPENCHIME_MANAGED_ADDRESS (the control plane's) |
| `OPENCHIME_MAX_USERS` | renamed OPENCHIME_MANAGED_MAX_USERS (the control plane's) |
| `OPENCHIME_MAX_CONNS_PER_IP` | a constant, 1024; development mode reads OPENCHIME_DEV_CONNS_PER_IP |
| `OPENCHIME_XFER_WORKERS` | a constant, 2 |
| `OPENCHIME_SESSION_DAYS` | a constant, 30 |
| `OPENCHIME_SESSION_IDLE_DAYS` | a constant: no idle limit |
| `OPENCHIME_LOCAL_MFA` | renamed OPENCHIME_MFA |
| `OPENCHIME_AUTH_MODE` | renamed OPENCHIME_AUTH |
| `OPENCHIME_OIDC_ISSUER` | the relay is the control plane: its issuer is OPENCHIME_ENROLL_URL's origin |
| `OPENCHIME_OIDC_AUDIENCE` | an enrolled box's audience is its enrollment's; a managed box is started with OPENCHIME_MANAGED_AUDIENCE |
| `OPENCHIME_OIDC_PUBKEY` | the relay's keys are the ones it publishes; a managed box may pin one with OPENCHIME_MANAGED_RELAY_KEY |
| `OPENCHIME_OIDC_PUBKEY_FILE` | the relay's keys are the ones it publishes |
| `OPENCHIME_OIDC_ALLOW` | renamed OPENCHIME_JOIN |
| `OPENCHIME_OIDC_EMAIL_LINK` | a constant: an emailed code signs into a person a provider knows only through that provider |
| `OPENCHIME_TLS_SOURCE` | OPENCHIME_TLS=self|file|acme:<names>|central |
| `OPENCHIME_TLS_NAME` | OPENCHIME_TLS=acme:<names> |
| `OPENCHIME_ACME_DIRECTORY` | OPENCHIME_TLS=acme:<names>;directory=<url> |
| `OPENCHIME_ACME_EMAIL` | OPENCHIME_TLS=acme:<names>;email=<address> |
| `OPENCHIME_MAX_ATTACHMENT_SIZE` | a constant: the wire's cap, 200 MiB |
| `OPENCHIME_MAX_VIDEO_MESSAGE_SIZE` | renamed OPENCHIME_MAX_VIDEO_SIZE |
| `OPENCHIME_S3_ENDPOINT` | OPENCHIME_S3=s3://KEY:SECRET@host[:port]/bucket[?region=…] |
| `OPENCHIME_S3_BUCKET` | OPENCHIME_S3=s3://KEY:SECRET@host[:port]/bucket[?region=…] |
| `OPENCHIME_S3_ACCESS_KEY` | OPENCHIME_S3=s3://KEY:SECRET@host[:port]/bucket[?region=…] |
| `OPENCHIME_S3_SECRET_KEY` | OPENCHIME_S3=s3://KEY:SECRET@host[:port]/bucket[?region=…] |
| `OPENCHIME_S3_REGION` | OPENCHIME_S3=s3://…/bucket?region=<region> |
| `OPENCHIME_ATTACH_MAX_AGE_DAYS` | a constant: attachments are kept |
| `OPENCHIME_EVICT` | a constant: on, on local disk |
| `OPENCHIME_EVICT_GRACE_HOURS` | a constant, 24 |
| `OPENCHIME_DB_RESERVE_MB` | a constant, 256 |
| `OPENCHIME_PRESSURE_MB` | a constant, 512 |
| `OPENCHIME_MAINT_BATCH` | a constant, 64 |
| `OPENCHIME_AUDIT_MAX_DAYS` | a constant, 365 |
| `OPENCHIME_MAINT_INTERVAL_MS` | a constant, five minutes |
| `OPENCHIME_SCHED_TICK_MS` | a constant, fifteen seconds |
| `OPENCHIME_ENROLL_CODE_FILE` | the enrollment code is in the log |
| `OPENCHIME_ENROLL_WAIT_SECS` | a constant: one attempt at first start, then each boot; a managed box waits two minutes for its claim |
| `OPENCHIME_ENROLL_TICKET` | renamed OPENCHIME_MANAGED_TICKET |
| `OPENCHIME_AUDIO_ADVERTISE_PORT` | renamed OPENCHIME_MANAGED_AUDIO_ADVERTISE_PORT (the control plane's) |
| `OPENCHIME_AUDIO_TOKEN_PREFIX` | renamed OPENCHIME_MANAGED_AUDIO_TOKEN_PREFIX (the control plane's) |
| `OPENCHIME_CALL_MAX` | a constant, 10; development mode reads OPENCHIME_DEV_CALL_MAX |
| `OPENCHIME_TTS_LANG` | the binary carries one language |
| `OPENCHIME_TTS_QUEUE` | a constant, 256 |
| `OPENCHIME_TTS_IDLE_SECS` | a constant, 300 |
| `OPENCHIME_TTS_RATE` | a constant, 60 a minute |
| `OPENCHIME_STT_QUEUE` | a constant, 64 |
| `OPENCHIME_STT_IDLE_SECS` | a constant, 300 |
| `OPENCHIME_STT_RATE` | a constant, 60 a minute |
| `OPENCHIME_STT_MAX_SECS` | a constant, 30: the model's limit |
| `OPENCHIME_SUMMARY_URL` | OPENCHIME_SUMMARY=cloud:<url>,<model> |
| `OPENCHIME_SUMMARY_MODEL` | OPENCHIME_SUMMARY=cloud:<url>,<model> |
| `OPENCHIME_TEST_PASSWORD_AUTH` | removed: a password is taken only on the sign-in page; tests sign in through it (tests/testsignin.h) |
| `OPENCHIME_DEMO_PASSWORD_FRAMES` | removed: a password is taken only on the sign-in page; tests sign in through it (tests/testsignin.h) |
| `OPENCHIME_BOOTSTRAP_USERS` | renamed OPENCHIME_DEV_USERS (OPENCHIME_MODE=development) |
| `OPENCHIME_FILE_PAGE` | renamed OPENCHIME_DEV_FILE_PAGE (OPENCHIME_MODE=development) |
| `OPENCHIME_UNFURL_ALLOW_PRIVATE` | nothing read it: the unfurl fetcher never reaches a private address |
| `OPENCHIME_TEST_OS_ROOTS` | nothing read it: the operating system's roots, with OPENCHIME_EXTRA_CA beside them |
| `OPENCHIME_DEMO_USERS` | renamed OPENCHIME_DEV_USERS (OPENCHIME_MODE=development) |
| `OPENCHIME_DEMO_FILE_PAGE` | renamed OPENCHIME_DEV_FILE_PAGE (OPENCHIME_MODE=development) |
| `OPENCHIME_DEMO_CALL_MAX` | renamed OPENCHIME_DEV_CALL_MAX (OPENCHIME_MODE=development) |
| `OPENCHIME_DEMO_CONNS_PER_IP` | renamed OPENCHIME_DEV_CONNS_PER_IP (OPENCHIME_MODE=development) |
| `OPENCHIME_DEMO_MAINT_MS` | nothing read it: the maintenance pass runs every five minutes |
| `OPENCHIME_DEMO_SCHED_MS` | nothing read it: scheduled messages are swept every fifteen seconds |
| `OPENCHIME_DEMO_PRIVATE_UNFURL` | nothing read it: the unfurl fetcher never reaches a private address |
| `OPENCHIME_DEMO_OS_ROOTS` | nothing read it: the operating system's roots, with OPENCHIME_EXTRA_CA beside them |
| `OPENCHIME_TEST_DIR` | renamed OPENCHIME_DEV_DIR (OPENCHIME_MODE=development) |
| `OPENCHIME_TEST_AUDIO` | renamed OPENCHIME_DEV_AUDIO (OPENCHIME_MODE=development) |
| `OPENCHIME_TEST_MIC` | renamed OPENCHIME_DEV_MIC (OPENCHIME_MODE=development) |
| `OPENCHIME_TEST_MIC_LEAD_MS` | renamed OPENCHIME_DEV_MIC_LEAD_MS (OPENCHIME_MODE=development) |
| `OPENCHIME_TEST_TONE` | renamed OPENCHIME_DEV_TONE (OPENCHIME_MODE=development) |
| `OPENCHIME_TEST_MIC_ECHO` | renamed OPENCHIME_DEV_MIC_ECHO (OPENCHIME_MODE=development) |
| `OPENCHIME_TEST_CAPTURE` | renamed OPENCHIME_DEV_CAPTURE (OPENCHIME_MODE=development) |
| `OPENCHIME_TEST_SCREEN_GONE_MS` | renamed OPENCHIME_DEV_SCREEN_GONE_MS (OPENCHIME_MODE=development) |
| `OPENCHIME_TEST_VIDEO_CAP_MS` | renamed OPENCHIME_DEV_VIDEO_CAP_MS (OPENCHIME_MODE=development) |
| `OPENCHIME_TEST_CALL_TIMERS` | renamed OPENCHIME_DEV_CALL_TIMERS (OPENCHIME_MODE=development) |
| `OPENCHIME_TEST_SUFFIX` | renamed OPENCHIME_DEV_SUFFIX (OPENCHIME_MODE=development) |
| `OPENCHIME_TEST_SIGNIN` | renamed OPENCHIME_DEV_SIGNIN (OPENCHIME_MODE=development) |
| `OPENCHIME_SUFFIX` | the service suffix is built in; a client in development mode reads OPENCHIME_DEV_SUFFIX |
| `OPENCHIME_RECOVER_MB` | nothing read it |
| `OPENCHIME_BLOB_BACKEND` | OPENCHIME_S3 set is S3; unset is local disk |
| `OPENCHIME_STATE` | the TUI keeps its state in the credential store (ARCH-88) |
<!-- /config-reference -->

## A managed box

The `OPENCHIME_MANAGED_*` rows above are what the control plane writes into a
daemon's environment when it provisions a managed workspace (ARCH-76, model 3).
An operator never sets them; a daemon started with one of them and no ticket
refuses to start, naming this section. What the rows carry, beyond the table:

- **`OPENCHIME_MANAGED_TICKET`** is the one-time claim ticket (AUTH.md §8.7).
  Its presence is what makes the box managed, and it needs
  `OPENCHIME_ENROLL_URL` and `OPENCHIME_MANAGED_AUDIENCE` beside it.
- **`OPENCHIME_MANAGED_AUDIENCE`** is adopted: the daemon generates its own key
  and claims the binding. A database holding a different audience stops the
  boot rather than becoming two workspaces.
- **`OPENCHIME_MANAGED_RELAY_KEY`** is optional; the keys the relay publishes at
  `<enroll origin>/oidc/jwks` alone are the ordinary case.
- **`OPENCHIME_MANAGED_ADDRESS`** is how a moved workspace is re-filed by clients
  (PROTOCOL.md §4.3b); during the move, the certificate through central is
  asked for both names (AUTH.md §8.9).

With them, the control plane sets the operator rows a managed box needs as any
deployment would: `OPENCHIME_ENROLL_URL` (central's own origin, which is also
the relay's issuer), `OPENCHIME_AUTH=relay`, `OPENCHIME_JOIN` (an `owner:` or
`subject:` rule from the owner's console sign-in, AUTH.md §8.4),
`OPENCHIME_PUSH_URL`, `OPENCHIME_INVITE_MAIL=on`, `OPENCHIME_WORKSPACE_NAME`,
`OPENCHIME_DATA_DIR`, `OPENCHIME_PORT` and `OPENCHIME_HEALTH_PORT`.

**What is derived.**

- **The deployment model** is `managed` when the ticket is set, `federated`
  when only `OPENCHIME_ENROLL_URL` is, else `standalone`; nothing declares it.
- **The relay** needs no issuer, key file or audience of its own: the issuer
  is the enroll URL's origin, the keys are fetched from it, and the audience is
  the enrollment's — which on a managed box is the one above.
- **The certificate** comes through central (`OPENCHIME_TLS=central`) unless
  told otherwise.
- **The claim** is made once the daemon serves and retried for two minutes
  (`OC_CLAIM_WAIT_SECS`) while central cannot be reached; a refused ticket is
  not retried.

**A demo workspace.** The public demo in each environment (AUTH.md §8.12) is a
managed box like any other, provisioned with `OPENCHIME_MODE=demo` and
`OPENCHIME_AUTH=local,relay`: visitors make local accounts with the control
plane's signed invite, checked against the relay's published keys, and the
operator signs in through the relay as its owner. The control plane rebuilds it
every night, from the image, on a new volume.

**Development mode** accepts the managed rows without the ticket, so the
fixtures can stand in for a managed box (a user cap in the menu, a relay port
nothing answers on); it still never enrols anywhere but loopback.

## Link unfurls (REQ-222, ARCH-105)

Always on — there is deliberately **no on/off variable**. Every fetch passes an
SSRF gate (no loopback, private, link-local, CGNAT, multicast or reserved
destination — checked on every resolved address and re-checked per redirect)
and is capped in bytes, redirects and time; an air-gapped box's fetches simply
fail, bounded and silent.

---

## Database-held state

None of this is set by an operator directly; the daemon writes it and reads it
back at startup, and it outranks the environment where the two overlap. Full
column detail is in [SCHEMA.md](./SCHEMA.md).

| Where | Written | What it controls |
|---|---|---|
| `schema_version` | By the migrator, one row per applied migration | Which migrations have run. At each boot every migration above the highest row is applied in its own transaction; a failure rolls that one back and stops the boot (ARCH-27). |
| `server_identity` (one row) | On the first run that generates a TLS identity | The daemon's certificate and private key. When present it is written over `cert.pem` / `key.pem` in the data directory at startup, so a restored database keeps the fingerprint clients pinned (ARCH-10/66b). |
| `local_issuer` (one row) | At first start | The key and issuer name that sign the ID token a browser sign-in on the daemon's own pages ends with (AUTH.md §8.10, migration 0050): a restored database still verifies what it signed. A private key. |
| `acme_account`, `tls_certificate` (one row each) | When a CA-issued certificate is first obtained | The ACME account (with its CA's directory) and the certificate presented, with its source, names and expiry; presented at once on a restart that still has that source and those names (migration 0049). |
| `enrollment` (one row) | When `OPENCHIME_ENROLL_URL` first takes effect | The enrollment key, the audience and `pending` / `active`. The stored audience is the relay's; a managed box's must match `OPENCHIME_MANAGED_AUDIENCE`, and push and invitation mail need `active` (ARCH-84/85). |
| `invites` with no `created_by` | At boot, in `local` auth mode, while no owner who can sign in exists | The first-run setup token, printed once to stderr; redeeming it creates the owner (REQ-024, ARCH-59). A newer token spends the older ones, and none is redeemed once an owner can sign in. |
| `users.role` | By owners and admins | `owner` / `admin` / `member` — who may administer the workspace (ARCH-60). |
| `users.disabled` | By member removal | A removed member cannot sign in and does not count against `OPENCHIME_MANAGED_MAX_USERS` (CP-7). |
| `webhooks` | By owners and admins | Incoming webhooks and the channel each posts to (REQ-170). |
| `custom_emoji` | By members | The workspace's own emoji (REQ-072). |

Each person's preferences are also stored here and steer what the daemon sends
them: notification level and mute per channel (`notification_prefs`), the default
level, schedule, pause and time zone (`users.notify_default`, `dnd_mode`,
`allow_start_min` / `allow_end_min`, `dnd_until_ms`, `tz_offset_min`,
`notify_schedule`), keywords and priority people (`notify_keywords`,
`priority_people`), thread follows (`thread_follows`), the voice they are read in
(`users.voice_id`) and each client's synced settings (`client_settings`).

## SQLite settings

Fixed in code (`daemon/dbwriter.c`); nothing in the environment changes them.

| Setting | Value | Why |
|---|---|---|
| `journal_mode` | `WAL` | Readers run beside the single writer without blocking it (ARCH-2/66). |
| `foreign_keys` | `ON` | The schema's references and cascades are enforced. |
| `query_only` (read connections) | `1` | Queries go through three further connections that cannot write (ARCH-66). |
| `busy_timeout` (read connections) | 5000 ms | How long a read waits on a lock before failing. |

## Command line

Configuration never comes from arguments; these are the only ones the daemon
accepts, and any other stops it with exit status 2.

| Argument | What it does |
|---|---|
| `--version`, `-V` | Prints the version and protocol version, then exits. |
| `--check-config [--env-file PATH]` | Loads the configuration from the environment, with PATH's `NAME=value` lines applied over it (systemd's `EnvironmentFile` grammar), prints every setting's effective value and source and the mode it would start in, and exits 0 only when the daemon would start. |
| `--config-reference` | Prints the settings reference above, as markdown, from the registry. |
| `--config-env` | Prints the packaged environment file (`packaging/openchimed.env`), from the registry. |
| `--tts-say VOICE TEXT OUT` | Renders TEXT in VOICE to an MP4 at OUT with the built-in model, then exits. Only in a build with read-aloud. |
| `--tts-manifest DIR` | Writes the manifest for the read-aloud data in DIR, then exits. In every build, with read-aloud or not. |
| `--stt-hear IN.wav` | Prints the text recognized in a 16-bit mono WAV, then exits. Only in a build with voice input. |
| `--stt-manifest DIR` | Writes the manifest for the recognizer data in DIR, then exits. In every build, with voice input or not. |

## Build switches

`make` variables, fixed into the binary (the Makefile's header lists them; the
feature switches are remembered in `build/.features`).

| Variable | Default | What it controls |
|---|---|---|
| `TTS` | `0` | `1` builds read-aloud in; without it `OPENCHIME_TTS` is accepted and has no effect. `make full` sets it. |
| `STT` | `0` | `1` builds voice input in; likewise `OPENCHIME_STT`. |
| `SUM` | `0` | `1` builds llama.cpp in for `OPENCHIME_SUMMARY=local`; `cloud:…` works without it. |
| `WEB` | `0` | `1` compiles the web client's loader into the daemon, which then serves it at `/app/`. |
| `OC_BUILD_ID` | *(unset: `dev`)* | The build id `--version` prints: the short commit the candidate build compiled (ARCH-20, RELEASING.md). A release number is assigned to a build afterwards and is not compiled in. |
