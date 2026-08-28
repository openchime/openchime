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
eats text somebody meant literally), the shared URL scanner (`test_url` — the
address-boundary rules the client autolinks by and the daemon unfurls by,
shared for the same reason), the unfurl fetcher's pure halves (`test_unfurl` —
the SSRF gate's per-address verdict, the HTML title/description scan, and the
block-page refusal), and the shared notify evaluator, plus in-process
integration suites that drive the real epoll server over TLS (`itest_netloop`,
`itest_tls`, `itest_slow_blob`) and the headless client app-core
(`test_client_core.c`) — all compiled into one `build/tests` binary by `make test`.
The black-box integration tier drives a natively-run daemon over a real socket
(the `integration` job in `.github/workflows/ci.yml`; §3.2). A deterministic codec fuzzer (45k iterations by default —
30k random + 15k framed; clean under
ASan/UBSan) and a concurrency load test (`tests/bench_load.c`, driven by
`Scripts/bench.sh`) round it out.

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
- The one exception is platform-backend code that cannot execute on the build
  host: sdltext's DirectWrite backend (ARCH-106) has its own console program,
  `make windows-sdltext-test` → `build/sdltext_test.exe`, same
  failure-count-as-exit-code contract. CI cross-compiles it (the compile is
  the header/vtable check mingw keeps honest); a Windows host runs it. The
  portable half of sdltext (the byte↔UTF-16 offset map) stays in `make test`
  like everything else.
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
built as `build/e2e_client` and driven by CI's `integration` job). It speaks
`HELLO`/`WELCOME`, `AUTH_CHALLENGE`/`AUTH`/`AUTH_OK`, and
`SEND`/`SEND_ACK`/`BROADCAST` — the auth-and-message vertical, and nothing
further. `CLIENT_ACK`, backfill, version rejection and session revocation are
exercised in the **in-process** integration suites (`itest_netloop`), not by this
client.

**Client-side TLS.** The wire protocol runs over TLS with TOFU
pinning (ARCH-10, REQ-180); there is no plaintext fallback. The TLS library is
mbedTLS (ARCH-51, [TLS.md](./TLS.md)), used by both the daemon and the test
client; `shared/tls.c` provides the TOFU-pinning client, and
`tests/itest_tls.c` exercises the handshake + pinning end-to-end.

### 3.2 Runner: a natively-run daemon in CI

Integration tests run **against the daemon binary CI just built**, started
directly on the runner and driven by `build/e2e_client`.

**No test anywhere exercises the published container image.** The
release builds it with buildah and pushes it to GHCR; nothing pulls it, starts
it or asserts anything about it. A fault confined to the `Dockerfile`, the
entrypoint (ARCH-39) or the Alpine/musl build would not be caught before it
reached users. The daemon's own behaviour is covered by the assertions
below and by the in-process suites.

**There is no local runner.** The two assertions live inline in
the `integration` job of `.github/workflows/ci.yml`.
`make build/e2e_client` builds the driver, so you can point it at a daemon
you started yourself (`make run` starts one on `127.0.0.1:8443`).

### 3.3 Scenarios

Every scenario below is implemented and runs in CI. **Which tier runs it is the
part that matters**, because the two tiers reach different things: the black-box
tier drives the daemon over a real socket from a separate process, and the
in-process suites reach states a black-box client cannot drive.

Note the scope of the claim: the black-box tier proves that the built *binary*
works. Nothing proves the shipped *image* works (§3.2).

**Black-box, against a natively-run daemon (the `integration` job in
`.github/workflows/ci.yml`) — two checks:**

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
conventions. The `push` trigger runs on **every branch except `main`** — main is
covered by the required checks on pull requests and by the release workflow's
`workflow_call`, so a standalone push run would only cancel its twin in the same
`concurrency` group — and skips doc-only changes via `paths-ignore`. The
`pull_request` trigger targets `main` and deliberately has **no** `paths-ignore`:
the jobs are required status checks, and a job that never triggers never
reports, so a docs-only PR would wait forever on a check that will not run.

Jobs:

- **`build`** — installs `libsqlite3-dev`, then runs `make` and `make test`.
  Fast feedback on compile + unit tests.
- **`integration`** — the daemon end-to-end, natively: build it and the e2e
  client, start the daemon on the runner, wait for `/healthz`, then drive the
  protocol vertical over TLS with the e2e client (§3.2 — the published image is
  tested by nothing).
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
concurrent connections does it hold" — measured, not assumed. Reproduced by
`Scripts/bench.sh`, which drives the
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
for: **does a slow blob backend stall message delivery?** Every other test runs
against the local filesystem, where a blob operation completes in microseconds —
so without this one, the ~100 ms/op regime the pool exists for would go
unexercised.

The test points the daemon's S3 backend at a loopback endpoint that dribbles a
download 16 KB at a time with a 120 ms delay between pieces, then measures one
client's message round-trips while another client's download crawls through it.

```
bob round-trip: idle median 57ms | during slow-download median 52ms, max 62ms
backend 120 ms/op, 3 slow segment(s) served DURING the measurement
  (= 360 ms of backend stall the loop did not absorb)
```

**The test discriminates.** Reverting `download_pump` to read inline
on the epoll thread (the behaviour ARCH-69 forbids) makes the run **time out entirely** —
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

### Harness conventions worth knowing

- `bench_load`'s read timeout is 180 s, so the serialized PBKDF2 auth ramp
  (~2 logins/sec, ≈500 ms each) is never the limit — a burst of N clients takes
  N/2 seconds to drain, and a short timeout would count slow-but-fine clients
  as connection failures.
- `Scripts/bench.sh` prints the whole result line, `connections_ok=` included —
  a failed connection reports `rtt 0.00`, so hiding the count makes a degenerate
  run read like an outstanding result.
- The memory table reports **requested vs connected** and divides by the
  connections that actually established.

**The real constraint** is that connection setup is bounded at
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

### Run it — scripted, against a control plane you started yourself

**Bringing up Postgres and the control plane is your job** — there is no
one-command stack for it (ARCH-36).

Start the control plane directly (e.g. `dotnet run` with `Push:Log:Enabled=true`
against a Postgres you started), then:

```sh
scripts/demo-federated.sh http://localhost:5176
```

It scripts the real console reserve — no dev endpoint needed — and drives the
same flow. Then drive a client against the daemon it left running:

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

### OIDC-relay login (the identity wire)

`scripts/demo-oidc.sh` proves the other daemon↔central wire (ARCH-56/57): the control
plane **mints** an ES256 identity token and a daemon in **OIDC mode verifies** it against
the central key it pins. It generates an ES256 keypair (private → the control plane's
signing key, public → the daemon), starts both — **it needs a Postgres already
listening on `localhost:5432`** — and mints a token via the dev endpoint
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

## A death always leaves evidence

The client writes a crash report and a minidump from an unhandled-exception
filter — but a filter only runs if the process gets to run code on the way out,
and three classes of death do not. `__fastfail`, which the CRT's buffer-overrun
and heap-corruption checks raise, goes past every handler by design. A stack
overflow may have no stack left to run one on. `TerminateProcess` from outside
runs nothing at all. Any library installing its own filter after ours wins, too.
Each of those leaves a process that is simply gone, which is not a bug report.

So the **breadcrumb ring is a file-backed memory mapping**, not process memory.
Nothing has to run at death time: the pages are the file, and the memory manager
writes them back whatever killed us. A clean exit marks the ring and deletes it
at `WM_DESTROY`; anything that does not reach there leaves it behind, and the
next start turns it into a `postmortem-<pid>.txt` naming what the app was doing.
An absent fault address is itself the finding — it means no handler ran.

The CRT's two exits are routed in as well: `abort()` and an invalid-parameter
call both raise a real exception now, so they produce the ordinary report
instead of vanishing.

**Prove it rather than wait for it.** `gui_drive.sh die <av|abort|badparam|fastfail|kill>`
forces one class each. `av`, `abort` and `badparam` produce a crash report;
`fastfail` and `kill` produce none, by construction, and are caught by the ring.

**The harness closes the client gracefully** (`CloseMainWindow`, falling back to
force after three seconds) for a reason that matters here: `Stop-Process -Force`
is a `TerminateProcess`, which is correctly reported as a death that ran no
handler. Force-killing on every launch would fill the test directory with
post-mortems the harness caused on purpose and bury the one that matters.

## Symbolicating a Win32 crash

`make windows-gui` ships a **stripped** `build/openchime.exe` and keeps the
symbols beside it in `build/openchime.debug`, linked by a `.gnu_debuglink`. The
crash report the client writes carries a module-relative `rva=`, which stripping
does not move, so a crash is resolved without the shipped binary having symbols
in it:

```
base=$(x86_64-w64-mingw32-objdump -p build/openchime.debug | awk '/ImageBase/{print $2}')
va=$(python3 -c "print(hex(0x$base + 0xRVA))")
x86_64-w64-mingw32-addr2line -e build/openchime.debug -f -C -i "$va"
```

Keep the `.debug` file for any binary handed to somebody else; without it a
minidump from that build resolves to nothing.

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

**Two verbs put text in the composer, and they are not interchangeable.**
`type` sets the buffer directly (`ed_set`), bypassing the editor's own rules;
`typekeys` sends each character as a real `WM_CHAR` through the editor, so the
rich-mode typing rules (pending styles, continuation, delimiter handling) run
exactly as they do for a user. Testing an editor behaviour with `type` proves
nothing about typing.

> **The smoke owns its own daemon.** It defaults to
> port **9500** and `/tmp/oc-smoke`, wipes that directory, and **verifies the
> workspace it reached is the fixture** — name plus the presence of alice, bob
> and carol — refusing to run otherwise. Silently adopting whatever daemon is
> listening means asserting fixture users against somebody's real workspace and
> reporting confident nonsense, which is exactly what the fixture check refuses.
>
> **Kill a dev daemon by its environment, not its command line.** It is started
> as `env OPENCHIME_PROTO_PORT=… openchimed`, so the port never appears in the
> process's *cmdline* and `pkill -f OPENCHIME_PROTO_PORT=9500` matches nothing —
> exiting 1, looking exactly like "nothing to kill". The daemon then keeps
> running on a directory that has been deleted underneath it (SQLite happily
> writes to the unlinked inode), and every upload fails with an opaque
> `transfer error`. Match `/proc/<pid>/environ`, as both harnesses do.

## Reading the GUI smoke

`scripts/gui_smoke.sh` is the only GUI harness, and it answers one question —
**does the client boot and run** — in about ten seconds across fourteen checks.
It drives the client through the test hook (`OPENCHIME_TEST_DIR`) and stands up
its own fixture daemon on its own port. It is the pre-push gate; it is not a
feature suite and does not aspire to be one.

**Assertions wait on state, never on a clock.** `expect_eventually`, `wait_grep`
and `settle` poll for the state being asserted, so a true assertion returns on
the first poll and only a real failure pays the timeout; the palette is asserted
as a whole closed→open→closed **round trip**, refusing to credit a close whose
open never happened. Tune the patience with `OC_SMOKE_WAIT_MS` (default 6000).

**Its failures can be believed on sight, which is a property that was built in
rather than hoped for.** It checks the **exit status of every verb**: gui_drive
exits non-zero when the client never acked, and a caller that discards that
status turns a dropped command into the *next* assertion failing for an
unrelated reason. That is not hypothetical — it is what made a previous, much
larger suite's failures need interpretation before they could be trusted, and
half of them turned out to be dropped verbs rather than defects.

**Its checks cannot pass by accident either.** The message it sends carries a
per-run unique string, so no assertion can be satisfied by something a previous
run left in the database; and the run ends by making the client answer one more
time, because every earlier check reads a dump file that looks identical whether
the client is alive or died on the last keystroke.

**Keeping it small is the maintenance rule.** The suite that preceded it held a
few hundred assertions and ran for seven minutes, so it was skipped, so it
caught nothing — the failure mode of a slow gate is not that it is slow, it is
that it stops being run. Verify a feature by driving it, through
`scripts/gui_drive.sh` or a harness scoped to that feature. Do not add it here.

*One caveat when reading a failure message:* the dump's `comp=` field is the
**IME composition length**, not the autocomplete popover — the dump exposes no
popover state, so `comp=0` beside a completion failure means nothing. The
fields worth reading are `error_seq` and `last_error`: without them a failed
intent and an intent that was never sent look identical. Prefer asserting on
`error_seq`, which only ever increments — `last_error` is cleared on
`OC_EV_CONNECTED`/`OC_EV_AUTH_OK`, so a reconnect between the failure and the
check can erase the evidence a wait is polling for.
