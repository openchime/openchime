# OpenChime — Testing Strategy

How OpenChime is tested, and the conventions any new test follows. Recorded as
a decision in [ARCHITECTURE.md](./ARCHITECTURE.md) (ARCH-48) and consistent
with the sibling C project openblocks, whose hand-rolled test convention this
mirrors.

**Status.** **Both tiers are built and green in CI.** The unit tier below is
implemented across the codec, framebuf, migrations, auth/JWT/roles/rate-limiting,
the DB-writer handlers, storage/maintenance, enrollment, push, and the shared
@mention scanner (`test_mention` — deliberately its own suite because the daemon
and every client link that one implementation, ARCH-89), the message-formatting
parser (`test_richtext`, REQ-220/ARCH-100 — same reasoning one level down: both
frontends render from that one parser, and its edge cases are where a dialect
eats text somebody meant literally), plus in-process
integration suites that drive the real epoll server over TLS (`itest_netloop`,
`itest_tls`, `itest_slow_blob`) and the headless client app-core
(`test_client_core.c`) — all compiled into one `build/tests` binary by `make test`.
The integration tier drives the deployed container over the compose stack
(`make integration`). A deterministic codec fuzzer (45k iterations by default —
30k random + 15k framed; clean under
ASan/UBSan) and a concurrency load test (`tests/bench_load.c`, driven by
`Scripts/bench.sh`) round it out.

*Note: the replication/restore pipeline this document once exercised is gone —
ARCH-3 withdrew replication from this repo entirely; off-box durability is a
deployment concern (hosted lives in `openchime-saas`).*

---

## 1. Two tiers

The codebase splits cleanly by testability:

- **Unit tier** — pure logic with no sockets, threads, or real disk: the frame
  codec, migration runner, idempotency/dedup bookkeeping, rate limiter, and the
  protocol state machine. Fast, deterministic, run on every build.
- **Integration tier** — the whole daemon running as a process, exercised over
  its real wire protocol against its real SQLite path. This is where behavior
  that is unreachable from a unit test — the deployed image, TLS termination,
  and the protocol vertical end to end — is proven. Off-box backup is **not**
  exercised here: it is a deployment concern (ARCH-3), and in the hosted model
  it lives in the `openchime-saas` repo.

A behavior is tested at the lowest tier that can actually observe it. Codec
edge cases belong in unit tests; "two clients see each other's messages in
order" belongs in integration.

---

## 2. Unit tier

### 2.1 Convention

- All unit and in-process integration suites compile into **one** binary,
  `build/tests`, run by `make test`. There is deliberately no per-test binary:
  the tests exercise public APIs, so each `tests/*.c` links the real module
  objects rather than `#include`-ing the `.c` under test.
- A subject's tests live in one translation unit exposing a single entry point,
  `int run_<subject>_tests(void)`, which runs its groups and returns its
  failure count. `tests/main.c` calls each and sums; a non-zero total exits
  non-zero and fails CI.
- A single hand-rolled `CHECK` macro, no framework, shared via `tests/check.h`:

  ```c
  #define CHECK(cond)                                                    \
      do {                                                               \
          if (!(cond)) {                                                 \
              printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);   \
              failures++;                                                \
          }                                                              \
      } while (0)
  ```

- If a future test genuinely needs a file-static helper, that one TU can
  `#include` the `.c` under test directly (the openblocks technique) — but none
  currently do, so all link the public API instead.

- Built and run by `make test`, compiled `-O0 -g` (debuggable). A non-zero exit
  fails the build and CI. One rule builds the single binary from every
  `tests/*.c` plus the daemon and app-core objects it links:

  ```makefile
  test: $(TEST_BIN)
  	./$(TEST_BIN)

  $(TEST_BIN): $(TEST_SRC) $(APP_SRC) $(CORE_SRC) $(HDRS) $(MBEDTLS_A) | build
  	$(CC) $(CFLAGS) -O0 -g $(INC) $(CORE_INC) -Itests \
  	    $(TEST_SRC) $(APP_SRC) $(CORE_SRC) $(MBEDTLS_LIBS) \
  	    -lsqlite3 -lresolv -lpthread -o $@
  ```

  Adding a suite is therefore two steps: drop in `tests/test_<subject>.c`
  exposing `run_<subject>_tests()`, and call it from `tests/main.c`.

No external test dependency is vendored. The projects that share this
convention deliberately keep the harness to ~15 lines rather than pull in a
framework, and OpenChime follows suit.

### 2.2 What the unit tier covers

- **Frame codec** ([PROTOCOL.md](./PROTOCOL.md) §2, §7) — the first and most
  important unit target:
  - encode→decode round-trips for every v1 frame (§9 registry);
  - the primitive encoders (`u8/16/32/64`, `str`, `lstr`, `bytes`, `idem`),
    including empty strings and max-length values;
  - boundary sizes: a body at exactly `MAX_BODY_SIZE`, a frame at exactly
    `MAX_FRAME_SIZE`, and rejection one byte over each;
  - malformed input: a `length` shorter than the header requires, a truncated
    payload, a `str`/`lstr` whose declared length overruns the buffer, an
    unknown `msg_type`, a frame whose `version` differs from the negotiated
    one;
  - version-negotiation math (§3): overlapping ranges pick
    `min(server_max, client_max)`; disjoint ranges yield the correct
    `VERSION_TOO_OLD` vs `VERSION_TOO_NEW` outcome.
- **Frame reassembler** (`framebuf`, ARCH-9) — feeding a frame one byte at a
  time yields nothing until complete; two frames in one push come out in order;
  a frame split across pushes reassembles; bad/oversized lengths error rather
  than buffer forever.
- **Migration runner** (ARCH-27) — applying migrations against an empty
  `schema_version`, resuming a partially-migrated DB, and refusing to skip or
  reorder. The **DB-writer thread** (`dbwriter`) is tested for migrate-on-boot,
  clean start/stop, and its AUTH/SEND job processing (user upsert, monotonic
  ids, idempotent replay, membership gate, broadcast fan-out list) driven
  directly through the job queue. The **event loop** (`itest_netloop`) is tested
  end-to-end: two TLS clients authenticate, one `SEND`s, and both receive the
  `BROADCAST` while the sender is acked; and a reconnecting client
  `BACKFILL_REQUEST`s and receives its missed messages replayed in order.
- **Idempotency + dedup** (ARCH-44/45) — a repeated `(channel, token)` returns
  the original id without a second insert; the client high-water mark
  suppresses a `message_id` at or below the mark.
- **Rate limiter** (REQ-190/191) and the **connection state machine**
  ([PROTOCOL.md](./PROTOCOL.md) §10) — legal transitions accepted, illegal
  frames rejected with the expected reason code.

### 2.3 Determinism rules

Unit tests must be reproducible and independent of wall-clock or environment:

- Any use of `rand()` seeds it explicitly (openblocks seeds `srand(12345)`).
- No test asserts an exact wall-clock timestamp. Tests assert *relative* facts —
  ids strictly increase, a timestamp falls inside a window the test itself
  bounded — never a literal `now()`. **There is no injectable clock or id
  source**: `dbw_now_ms` calls `clock_gettime` directly with no override seam, so
  determinism here comes from asserting relative facts rather than from
  scripting time. What *is* injectable is the interval a periodic job runs at
  (`oc_dbwriter_set_idem_retention`, `OPENCHIME_MAINT_INTERVAL_MS`,
  `OPENCHIME_SCHED_TICK_MS`), which is what lets a suite compress a clock it
  cannot fake.
- Unit tests touch no network. **Several suites use real files on disk**, not
  `:memory:` — `test_dbwriter`, `test_push`, `test_client_core`, `itest_netloop`
  and `itest_slow_blob` each open a database under `build/`, because they
  exercise migration-on-boot, WAL behaviour and multi-connection paths that an
  in-memory database does not reproduce. They clean up after themselves; the
  purely-logical suites (codec, framebuf, mention, richtext, searchq) touch
  nothing.

---

## 3. Integration tier

### 3.1 A C test client that reuses `protocol.c`

Integration tests drive the daemon with a small in-repo **C test client that
links the same `shared/protocol.c` the daemon uses** — not a re-implementation of
the wire format in another language. This is a deliberate choice: a second
protocol implementation (e.g. a Python harness) would be free to silently drift
from the C encoder, so a bug that affects both in the same way would pass. One
source of truth for the frames means the integration tests also dogfood the
codec the real client will ship.

The test client lives under `tests/` (`tests/e2e_client.c`, the black-box client
built as `build/e2e_client` and driven by `make integration`). It speaks
`HELLO`/`WELCOME`, `AUTH_CHALLENGE`/`AUTH`/`AUTH_OK`, and
`SEND`/`SEND_ACK`/`BROADCAST` — the auth-and-message vertical, and nothing
further. `CLIENT_ACK`, backfill, version rejection and session revocation are
exercised in the **in-process** integration suites (`itest_netloop`), not by this
client.

**Client-side TLS (settled).** The wire protocol runs over TLS with TOFU
pinning (ARCH-10, REQ-180); there is no plaintext fallback. The TLS library is
mbedTLS (ARCH-51, [TLS.md](./TLS.md)), used by both the daemon and the test
client; `shared/tls.c` already provides a TOFU-pinning client, so socket-level
integration scenarios are no longer gated. `tests/itest_tls.c` exercises the
handshake + pinning end-to-end today.

### 3.2 Runner: the existing Docker Compose stack

Integration tests run **against the Docker Compose environment already defined
for local dev** (ARCH-36/38/39), not a second bespoke environment — the same
`daemon` + `minio` + `minio-init` topology, so what CI exercises is the image
that ships. A wrapper script, `Scripts/test-integration.sh`, brings the
stack up, runs the scenario driver, and tears it down with a non-zero exit on
any failed scenario. It sits alongside the existing `run.sh`/`stop.sh`/`reset.sh`
wrappers (ARCH-40).

### 3.3 Scenarios

Every scenario below is implemented and runs in CI. **Which tier runs it is the
part that matters**, because the compose stack proves something the in-process
suites cannot — that the shipped *image* works — and the in-process suites prove
things the compose stack does not reach.

**Against the deployed container (`make integration`,
`Scripts/test-integration.sh`) — two checks:**

- **Liveness:** `/healthz` returns `200 OK` (ARCH-25).
- **The auth-and-message vertical over TLS:** `build/e2e_client` handshakes,
  authenticates in local mode, sends, and observes its own `SEND_ACK` and the
  `BROADCAST` (REQ-023, REQ-090/092).

**In-process, over the real epoll server and TLS (`make test` —
`itest_netloop`, `itest_tls`, `itest_slow_blob`, `test_client_core`):**

- **Handshake/versioning:** a client advertising an unsupported range gets a
  `REJECT` with the right `VERSION_TOO_OLD`/`VERSION_TOO_NEW` code and a closed
  connection (REQ-110/111).
- **Auth (AUTH.md):** *local mode* — a correct username+password reaches
  `AUTH_OK`, a wrong password gets `AUTH_INVALID_TOKEN` and is rate-limited after
  repeats; *OIDC mode* — an ES256 JWT signed by a **test issuer** keypair
  (standing in for the central service) reaches `AUTH_OK`, and a bad-signature /
  wrong-algorithm / wrong-audience / expired token is rejected; *session* — a
  reconnect with a stored session token resumes without re-auth, and a revoked
  session is refused (REQ-023, REQ-100, REQ-182).
- **Messaging:** two clients in a channel — a `SEND` from one produces a
  `SEND_ACK` to the sender and a `BROADCAST` to both; ordering within the
  channel matches send order (REQ-092).
- **Idempotency:** re-sending with the same token after a simulated drop yields
  the same `message_id` and no duplicate row (REQ-093).
- **Reconnect/backfill:** a client that disconnects, misses messages, then
  reconnects and issues `BACKFILL_REQUEST` receives exactly the missed messages
  and a `BACKFILL_DONE` (REQ-100/101).
- **Slow-backend isolation:** a download crawling through a deliberately slow S3
  endpoint does not stall message round-trips (ARCH-69,
  [TESTING.md §5](./TESTING.md)).

---

## 4. Continuous integration

CI is GitHub Actions (`.github/workflows/ci.yml`), mirroring openblocks'
conventions: it triggers on **every branch push** — deliberately unfiltered, so
the branch gate CONTRIBUTING.md's merge policy depends on is real — and on pull
requests targeting `main`. It skips doc-only changes via `paths-ignore` and uses
a `concurrency` group to cancel superseded runs.

Jobs:

- **`build`** — installs `libsqlite3-dev`, then runs `make` and `make test`.
  Fast feedback on compile + unit tests.
- **`integration`** — the deployed image end-to-end on the Compose stack:
  build the image, bring the stack up, wait for `/healthz`, then drive the
  protocol vertical over TLS with the e2e client. This is the automation of the
  manual steps in the README's "Verify" section.
- **`core`** — a standalone compile-check of the client app-core (ARCH-74).
- **`windows`** — the Windows cross-compile of the TUI and GUI
  (`make windows-tui windows-gui`), so the ported client stays building.

Everything runs non-interactively and communicates pass/fail purely through
exit codes, so no scenario depends on a human reading output.

**Audio.** `tests/test_audio.c` covers the **relay sidecar only** — forwarding,
call isolation, and `REVOKE`. Echo cancellation has **no harness**: AUDIO.md §6.4
designs one (a synthetic room impulse response convolved with a far-end signal,
near-end speech mixed in, and **ERLE** in dB as the measured output, with clock
drift injected by resampling one side) and sequences it with the audio client,
which does not exist. It is a design, not a test that runs.

New unit-test binaries are added to the `build` job's `make test`.

---

---

## 5. Capacity benchmark (REQ-210/211)

Measured answers to "how much memory does the daemon use, and how many
concurrent connections does it hold" — the two requirements that were previously
plausible-but-unmeasured. Reproduced by `Scripts/bench.sh`, which drives the
running daemon with the `tests/bench_load.c` load client and samples the
daemon's resident memory (`/proc/<pid>/status` `VmRSS`) while the load runs.

**Environment.** Localhost, single box, one daemon process (single-threaded net
loop + one DB-writer thread), mbedTLS, glibc. These are therefore an *upper
bound on latency* and a *lower bound on capacity per unit RAM*: a real
deployment on dedicated hardware with clients over a network sees no worse
memory behavior, and network latency dominates the sub-millisecond local
scheduling costs measured here.

### How to run

```
Scripts/bench.sh                 # default: idle memory at 50, 100, 200 conns + latency
Scripts/bench.sh 50 100 200 400  # custom connection counts
```

It bootstraps a set of local accounts (auth is a 600k-iteration PBKDF2, so it
keeps the count modest), starts a throwaway daemon, and prints a table of
connections vs. peak RSS vs. round-trip latency, tearing everything down after.

### Results

Representative figures (they vary a few KB/ms run to run):

| Metric | Value |
|--------|-------|
| Baseline daemon RSS (0 connections) | **~5 MB** |
| RSS per idle authenticated connection | **~50–60 KB** |
| 100 idle connections | ~10–12 MB total |
| 200 idle connections | ~15–18 MB total |
| Message round-trip (persist + `SEND_ACK`), 32 concurrent senders | **p50 ~2–3 ms, p90 ~80 ms, p99 ~130 ms** |
| Connection *setup* throughput | **~2 logins/sec** (see bottleneck below) |

> **Corrected 2026-07-19.** The two figures above were previously recorded as
> *p99 ~20–40 ms* and *~6–7 logins/sec*. Both were measured against a harness
> that was silently dropping most of its connections: `bench_load` used a 10 s
> read timeout while authentication is a 600k-iteration PBKDF2 serialized on the
> single writer, so a 32-connection burst only ever landed ~10 authenticated
> clients — and `Scripts/bench.sh` stripped the `connections_ok=` prefix that
> would have shown it. The latency percentiles were therefore measured at about a
> third of the intended concurrency, which flattered them by roughly 5×. With the
> timeout raised so all 32 connect, p99 is ~130 ms. The per-connection memory
> figure survived the correction largely unchanged (~58–60 KB), because peak RSS
> had included the connections that later timed out.

The per-connection cost is dominated by the mbedTLS session buffers plus the
per-connection frame reassembler and output buffer — flat and predictable, with
no per-connection growth beyond that at rest.

### What this means for capacity

Working from ~5 MB baseline + ~50 KB per idle connection:

- **By memory alone:** `(256 − 5) MB ÷ 50 KB ≈ ~5,000 idle connections` before
  RAM is the constraint in the lean profile (REQ-210).
- **The binding limit is the fd cap, not memory:** `OC_NETLOOP_MAX_FD = 4096`
  (a compile-time constant in `daemon/netloop.c`) caps the daemon at ~4,000
  connections regardless of RAM — and ~4,000 idle connections is still only
  ~200 MB, within the lean profile.
- **Comfortable planning number:** budget **~2,000–3,000 concurrent connections**
  on a 256 MB box, reserving headroom for SQLite's page cache, attachment
  transfer buffers, and the per-connection output buffers that grow (up to a
  1 MiB cap) while a connection drains a broadcast burst — the ~50 KB figure is
  measured on *idle* connections.
- **In users:** at ~1–2 connections per user (laptop + phone) and the fact that
  not everyone is online at once, that is roughly **1,500–3,000 concurrent
  users**, backing a registered tenant of **5,000–10,000+**.

Relative to REQ-211's stated target — "low-hundreds of concurrent connections…
sufficient for the 50–100 target customer scale" — a single 256 MB box clears it
by **one to two orders of magnitude**. At the 50-user scale specifically
(~100 connections ≈ 10 MB total, ~4% of the lean profile), the daemon is nowhere
near any limit; it would run on a 64 MB box.

Scaling further needs no architecture change (REQ-210): raise
`OC_NETLOOP_MAX_FD` and move to the ~512 MB standard profile.

### The one bottleneck: connection setup

Steady-state connection *count* is cheap; connection *establishment* is not. Each
login runs a **600k-iteration PBKDF2-HMAC-SHA256** password verification
(OWASP-tier, `OC_PW_ITERATIONS` in `daemon/auth.h`), and — like all mutations —
it runs on the **single DB-writer thread** (ARCH-5). That serializes logins at
**~2 per second** (≈500 ms each), the figure the results table and the
harness-limitations section below both carry.

This is a deliberate security cost paid once per login, not a memory or
steady-state limit: a box holding thousands of connections is fine, but a
**thundering-herd reconnect** (e.g. every client re-authenticating at once after
a daemon restart) drains at ~2/sec — ~25 minutes for 3,000 clients. If that ever
matters at scale, the fix is to parallelize the PBKDF2 across a small worker pool
off the single writer (the auth verification is CPU-bound and independent per
login), not to add RAM. It is a modest cost at the low-hundreds target scale
(50 logins ≈ 25 seconds), and session-token reconnect (ARCH-58) skips PBKDF2
entirely, which is what a real reconnect storm uses.

### Caveats

- Idle-connection memory; active fan-out uses more (bounded by the 1 MiB
  per-connection output cap and recoverable via reconnect + backfill).
- Localhost measurement — excludes real network RTT (which would dominate the
  low-single-millisecond local p50) and NIC/kernel socket-buffer memory under
  real load.
- No periodic large-scale soak test yet; these are point-in-time measurements.

### Slow-backend isolation (ARCH-69)

`itest_slow_blob` (in `make test`) answers the question the transfer pool exists
for and that nothing previously tested: **does a slow blob backend stall message
delivery?** Every earlier test ran against the local filesystem, where a blob
operation completes in microseconds — the ~100 ms/op regime the pool was built
for was never exercised.

The test points the daemon's S3 backend at a loopback endpoint that dribbles a
download 16 KB at a time with a 120 ms delay between pieces, then measures one
client's message round-trips while another client's download crawls through it.

```
bob round-trip: idle median 57ms | during slow-download median 52ms, max 62ms
backend 120 ms/op, 3 slow segment(s) served DURING the measurement
  (= 360 ms of backend stall the loop did not absorb)
```

**The test discriminates.** Temporarily reverting `download_pump` to read inline
on the epoll thread (pre-ARCH-69 behavior) makes the run **time out entirely** —
the loop freezes on the slow reads and the daemon stops serving anyone. A test
that cannot fail proves nothing, so this was verified rather than assumed.

Two implementation notes worth keeping, both learned by getting it wrong first:

- The slow path must be a **download**, not an upload. Making a *write* block
  requires exceeding the sender's socket send buffer (~2.5 MB), so an
  upload-based version needs a multi-megabyte payload and runs for minutes. A
  slow *reader* blocks trivially — there is no data yet.
- The concurrent client must run on **its own thread**. Ticking it between the
  measured client's sends makes the two barely overlap, and the measurement
  becomes a no-op that passes either way.

### Maintenance-pass overhead (ARCH-78)

Running the storage maintenance pass every 200 ms — 25× more often than the
5-minute default — against the same load shows no measurable cost:

| | baseline | maint every 200 ms |
|---|---|---|
| Daemon RSS | 5.0 MB | 5.1 MB |
| KB per connection | 55–57 | 52–65 |
| Round-trip latency | unchanged | unchanged |

### Known harness limitations

**Diagnosed and fixed.** The shortfall was `bench_load`'s own 10-second read
timeout, not a daemon limit. Authentication is a 600k-iteration PBKDF2 that runs
**serialized on the single writer thread**, measured here at **~2 logins/sec**
(≈500 ms each — the expected cost of 600k iterations). A burst of N clients
therefore takes N/2 seconds to drain, so everything past roughly the 20th client
gave up mid-authentication and was counted as a connection failure.

The knee is sharp and reproducible: 8/8 and 16/16 connect, 24 drops to 22/24.

Three fixes, all in the harness — the daemon was behaving exactly as designed:

- `bench_load`'s read timeout is now 180 s, so the auth ramp is never the limit.
- `Scripts/bench.sh` prints the whole result line. It previously piped through a
  `sed` that kept only `rtt_ms...` and dropped the `connections_ok=` prefix —
  and failed connections report `rtt 0.00`, so a degenerate run printed
  `p50=0.00 p90=0.00 p99=0.00`, which reads like an outstanding result rather
  than no result at all.
- The memory table now reports **requested vs connected** and divides by the
  connections that actually established, rather than by the number requested.

**The real constraint this exposes** is that connection setup is bounded at
~2/sec by design (REQ-191 wants PBKDF2 expensive). A server restart with a few
hundred clients reconnecting takes minutes to fully re-authenticate them, and
session-token reconnect (ARCH-58) — which skips PBKDF2 entirely — is what makes
that tolerable in practice. Worth remembering before quoting a connection-count
capacity number: the daemon *holds* thousands of connections, but *establishes*
them at two per second.

---

## 6. Running the federated stack by hand

Runs the whole federated system on one machine — the C daemon enrolled against the
.NET control plane (`openchime-saas`), with a client driving the two daemon→central
outbound wires (enrollment, ARCH-84; push, ARCH-85). Verified end to end.

### What it proves

1. **Enrollment** — the daemon generates a keypair + opaque audience, prints an `oce1.`
   code, the operator reserves it in the console, and the daemon proves possession and
   activates (ARCH-84; the control plane ratifies).
2. **Push** — a client registers a device token; a message send drives the daemon's push
   emitter, which signs a contentless batch with the enrollment key and POSTs it to the
   control-plane gateway, which verifies the request signature and relays it (ARCH-85;
   control-plane side). The cross-language crypto (mbedTLS sign ↔ .NET verify) matches by construction.

### Run it — one command (recommended)

Assuming the sibling layout `../openchime-saas`:

```sh
docker compose -f docker-compose.federated.yml up --build
```

That stands up Postgres + the control plane (with the dev log push provider + the dev
enrollment-reserve shortcut) + a daemon that **enrolls hands-off**: the daemon writes its
`oce1` code to a shared volume (`OPENCHIME_ENROLL_CODE_FILE`), an `enroll-init` step reserves it
via the dev endpoint, and the daemon activates (`OPENCHIME_ENROLL_WAIT_SECS` lets it wait for the
reserve, then come up Active with push enabled). The daemon serves on `localhost:8443`.

Then drive a client against it:

```sh
make demo-client
build/demo_client 127.0.0.1 8443 bob   pw token apns tok-bob-demo
build/demo_client 127.0.0.1 8443 alice pw send  1 "hello from the federated stack"
```

Watch the control-plane log for:

```
push[Apns] would notify token tok-bob-demo (channel 1, ...)
```

That is the daemon→central push wire firing end to end (signed, contentless).

### Run it — scripted, against a control plane you started yourself

If you're running the control plane directly (e.g. `dotnet run` with
`Push:Log:Enabled=true`), `scripts/demo-federated.sh http://localhost:5176` does the same
flow against it (it scripts the real console reserve, no dev endpoint needed).

### OIDC-relay login (the identity wire)

`scripts/demo-oidc.sh` proves the other daemon↔central wire (ARCH-56/57): the control
plane **mints** an ES256 identity token and a daemon in **OIDC mode verifies** it against
the central key it pins. It generates an ES256 keypair (private → the control plane's
signing key, public → the daemon), starts both, mints a token via the dev endpoint
`POST /api/dev/oidc/token` (gated on `Oidc:DevMintEnabled` — never in prod), and runs:

```sh
build/demo_client 127.0.0.1 18444 --oidc "$JWT" whoami
# -> demo_client: authenticated as uid 1
```

The browser upstream-IdP flow (Google) is bypassed on purpose — this exercises the
daemon's *verification*, the untested half. A real deployment supplies real Google
credentials to the relay instead of the dev mint endpoint.

### TUI runtime smoke (the interactive client)

`scripts/demo-tui.sh` runtime-verifies the **TUI** (ARCH-75) against a live daemon — the
headless check the project lacked (it was only ever build-verified). It drives the real
interactive client in a **tmux** pane: connect + auto-login as alice, send a message from
the composer, receive a broadcast from bob (via `demo_client`), and assert both render in
the transcript. Requires `tmux`. To connect the TUI by hand instead:

```sh
make tui
build/openchime-tui 127.0.0.1 8443 alice:pw    # <host> <port> [user:pass] direct connect
```

### Notes / gotchas

- **`OPENCHIME_BLOB_DIR`** must point at a writable directory. Outside the container the
  default `/data/blobs` doesn't exist, and `oc_netloop_run` refuses to start without a
  usable blob store — the daemon logs `blob storage = local disk` then exits before
  `netloop: listening`. The script sets it to a temp dir.
- The demo uses **local auth** for the client (bootstrap users), independent of push.
  Push only needs the box **enrolled** (active audience + key) and `OPENCHIME_PUSH_URL` set.
- **OIDC-relay login** against a live daemon is *not* covered here — it needs real Google
  credentials on the control plane. The mint↔verify contract is unit-tested on both sides.
- `demo_client` (`make demo-client`) is the flexible black-box tool: `token <apns|fcm>
  <tok>` or `send <channel> <text>` against a running daemon.

## Driving the Win32 GUI

`scripts/gui_drive.sh launch` **builds both sides and restarts the daemon if the
running one predates the binary it just built.** That is not convenience: the
client and daemon share a wire and ship together (ARCH-61), so a client built
from source N against a daemon still running N-1 decodes garbage and reports only
"connection lost — reconnecting", which points nowhere near the cause. It cost
real time three times in a single day before the guard existed.

`make` is incremental, so it is free when nothing changed. `OC_DRIVE_NO_BUILD=1`
skips the build and `OC_DRIVE_NO_DAEMON=1` leaves the daemon alone — for when a
mismatched pair is the thing under test, such as the version-reject path.

> **The smoke owns its own daemon (WIN-88, fixed 2026-07-31).** It defaults to
> port **9500** and `/tmp/oc-smoke`, wipes that directory, and **verifies the
> workspace it reached is the fixture** — name plus the presence of alice, bob
> and carol — refusing to run otherwise. Before that it silently adopted whatever
> was listening: during the 2026-07-30 review it bound to a nine-hour-old daemon
> holding real workspace data and reported confident failures for group DMs and
> `@`-completion because the fixture users did not exist there. It also no longer
> leaves `smokevis` / `smokedrafts` channels in anyone's workspace, since it
> starts from an empty one.
>
> **Kill a dev daemon by its environment, not its command line.** It is started
> as `env OPENCHIME_PROTO_PORT=… openchimed`, so the port never appears in the
> process's *cmdline* and `pkill -f OPENCHIME_PROTO_PORT=9500` matches nothing —
> exiting 1, looking exactly like "nothing to kill". The daemon then keeps
> running on a directory that has been deleted underneath it (SQLite happily
> writes to the unlinked inode), and every upload fails with an opaque
> `transfer error`. Match `/proc/<pid>/environ`, as `gui_smoke.sh` now does.

## Known flakiness

*None currently known in the GUI smoke.*

**Resolved 2026-07-31 — the suite was flaky and is now deterministic (WIN-87).**
Five consecutive runs used to give 2 failures, clean, clean, 2 failures, 4
failures, with the failing assertions *moving between runs*. Every failure was a
harness artifact, not a product defect. Two causes, both fixed:

1. **It slept instead of waiting.** 96 hand-tuned `sleep`s guarded 91
   assertions. `ack` means the verb's *handler ran*, not that its effect is
   observable — most dump fields are recorded during `WM_PAINT`, and anything
   that reaches the server lands a round trip later. The suite now waits on the
   state it is about to assert (`expect_eventually`, `wait_grep`, `settle`), so
   a true assertion returns on the first poll and only a real failure pays the
   timeout. Seven `sleep`s remain, all inside those poll loops.
2. **It asserted states, not transitions** — so "Esc closes the palette" passed
   when the palette never opened, inflating the pass count on exactly the runs
   where something was broken. Chords and zoom now assert the whole
   closed→open→closed round trip and refuse to credit the close if the open
   never happened.

After the fix the suite went six consecutive runs clean. It has grown a great
deal since — it reported **249 checks** on 2026-08-02 — so read the total the run
prints rather than quoting a number from here. Tune the patience with
`OC_SMOKE_WAIT_MS` (default 6000).

**Not every `sleep` is inside a poll loop.** The WIN-87 rewrite replaced the
sleeps that *guarded assertions*, and those are gone; the suite still uses plain
inter-action sleeps between driving steps (roughly two dozen), which are settling
time rather than assertion timing. A new assertion waits on the state it is about
to assert — it does not add a sleep.

*One caveat when reading a failure message:* the dump's `comp=` field is the
**IME composition length**, not the autocomplete popover — the dump exposes no
popover state, so `comp=0` beside a completion failure means nothing. The fields
worth reading are `error_seq` and `last_error`, added 2026-07-31: without them a
failed intent and an intent that was never sent look identical.

**Resolved 2026-07-29 — the one intermittent assertion.** `test_client_core`'s
live two-client section failed occasionally on
`WAIT_FOR(a, m->last_error[0] != '\0')` after a deliberately-rejected password
change, and passed on the retry.

The cause is a **fragile assertion, not a slow one**: `last_error` is *cleared*
on `OC_EV_CONNECTED` and `OC_EV_AUTH_OK`, so any reconnect between the rejection
arriving and the check erases the evidence the test is waiting for. The wait is
5 s (500 × 10 ms), which was never the problem.

It now asserts on **`error_seq`**, which only ever increments — the model's own
comment says that field exists precisely so a repeated failure cannot be
un-observed. An assertion on monotonic state cannot be raced by a clear.

**Honesty about the diagnosis:** this was not reproduced on demand — 12
consecutive runs stayed green, including six under 8-way CPU load — so the
*trigger* for the reconnect is unproven. What is proven is that the old
assertion could be defeated by one, and the new one cannot. If something in this
area fails again, that is new information and worth chasing rather than retrying.
