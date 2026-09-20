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
- **Video message media** (`tests/test_media.c`, ARCH-110) — the MP4 writer
  and reader round-trip and the reader survives a mutation fuzz; VP9 and Opus
  round-trip against quality floors; the recorder runs against the synthetic
  camera and tone, and `ffprobe` (installed in CI as a validator, never linked)
  checks the file it writes; the player plays, seeks and copes with a slow
  decoder. `tests/test_video_media.c` covers the protocol and daemon side.
- **Screen recording** (`tests/test_media.c`, REQ-162) — the camera box's geometry
  and the frame views that draw it; the screen front end repeating a still screen
  at the frame rate; recordings of the synthetic screen with the camera boxed into
  each corner (checked by colour in a decoded frame), of a window without one, of
  a window that goes away; the computer's sound mixed with the microphone and,
  with the microphone hearing only its echo, present once. The canceller that does
  it is measured in the ERLE harness (`tests/test_voice.c`) at 48 kHz on the same
  rooms as voice input's.
- **Voice input** (ARCH-112) — `tests/test_voice.c`: the segmenter over
  synthetic voiced sound with known pauses (free talk, the cap, push to talk) and
  the ERLE harness (AUDIO.md §6.4: converged, 100 ppm drift, double-talk);
  `tests/test_stt.c`: the tokenizer, spoken mentions and the recognition worker
  against a stub engine (order, a full queue, idle release, the token ceiling);
  `tests/test_protocol.c` round-trips the `STT_*` frames; `itest_netloop` drives
  the daemon's vertical — the capability and cap at auth; push to talk answered to
  the speaker only; free talk posted through the send path, in order, idempotently,
  refused for a non-member or an archived channel; nothing heard, recognition
  failing, over the cap, a chunk out of order; a segment still being heard when
  its speaker leaves never posted; the per-connection rate; and, in loops of their
  own, voice input turned off and with no engine (as when its data is missing);
  `test_client_core` shows free talk arriving as `BROADCAST`s with no client
  `SEND`, push to talk returning words and sending nothing, and one microphone
  owner.
- **Calls** (REQ-150–152, REQ-301–306, ARCH-73/113) — `tests/test_e2e.c`: SFrame
  against RFC 9605's vectors (every header encoding, suite 0x0004's key, salt and
  ciphertext), HPKE Auth mode against RFC 9180 A.1.3 (the ephemeral, receiver and
  sender keys, the sealed message), then what vectors do not cover — altered
  headers, bodies and tags, another key for the same KID, the replay window,
  another receiver or sender, another info or aad, a low-order key refused.
  `tests/test_call_media.c`: the jitter buffer on a simulated network — steady,
  0–120 ms of jitter with reordering, 10% and 20% loss, bursts, duplicates, DTX
  gaps, a network worse than its ceiling — asserting every frame that arrived is
  played, single losses rebuilt from FEC and the target in bounds; and Opus at
  16 kHz with FEC, concealment and DTX. `itest_netloop`: a start with
  invitations, the roster with slots, keys and epochs, sealed keys forwarded only
  between participants for the current epoch, the cap, declining, only the
  starter ending, a leave naming another conversation doing nothing, the missed
  call, one call per connection, a non-member refused, a rejoin's old token
  revoked, and the relay's sweep reported and applied. `test_client_core`: two
  and then three client cores in a call through the daemon and a relay with a
  **tap** in front of it, synthetic tones out and Goertzel measurements in —
  each hears the others at full level and never itself; a joiner misses only the
  grace; the tap sees only SFrame, never a repeating plaintext byte; after a
  leave, only keys the leaver never had; mute (seen by the others), push to talk
  and per-person volume; only the starter ending; the missed call; and the device
  key stored, re-read, upgraded from a version 2 entry and forgotten on sign-out.
- **Screen sharing** (REQ-161, ARCH-86/87) — `tests/test_share_media.c`: a sharer
  and a viewer over a simulated network that loses, delays, reorders and
  duplicates — fragments at the edge sizes, 5% loss recovered by NACKs, a whole
  frame lost and asked for, a frame beyond recovery given up for a keyframe, a
  late joiner's keyframe, the rate's halving, hold, growth, floor and ceiling, the
  size stepping down and back, malformed fragments refused, and the synthetic
  screen through VP9 and a lossy network back to readable, rising frame numbers.
  `itest_netloop`: `CALL_SHARE` start, take-over, a stop that is not the sharer's,
  leave and disconnect clearing it, someone outside the call refused, codecs on
  the roster. `test_client_core`: through the daemon and the tapped relay, dana
  shares the synthetic screen and erik decodes it at its size with rising frame
  numbers, the relay seeing only SFrame; 10% loss at the tap costs NACKs and
  resends, not the picture; faye joins late and has a picture within seconds;
  erik takes over, stopping dana's; erik stops and the picture goes.
- **Rate limiter** (REQ-190/191) and the **connection state machine**
  ([PROTOCOL.md](./PROTOCOL.md) §10) — legal transitions accepted, illegal
  frames rejected with the expected reason code.

### 2.2a When the binary itself crashes

A crash used to leave nothing to work with: the binary's output is block-buffered
into CI's pipe, so the suite that was running went down with the process, and
there was no backtrace — one crash on CI was knowable only as "Segmentation fault
(core dumped)". `tests/main.c` now line-buffers its output, names each suite as it
starts it, and handles the fatal signals: a crash prints the signal, the suite,
how far into the run it was, the faulting address, the crashing thread's stack
resolved to file and line by `addr2line`, and — where `gdb` is installed, as it is
on CI — every thread's stack. Then it raises the signal again, so the exit status
is the one it would have been.

Two switches help hunt a crash that appears once in fifty runs, rather than
paying four minutes of unrelated suites per attempt:

- `OC_TEST_ONLY=audio,media` runs only the suites whose names contain those;
- `OC_TEST_REPEAT=20` runs the selection that many times.

Unset, nothing changes. A repeated suite runs **in one process**, so a suite that
leaves a fixture switched must put it back or the second round fails somewhere
that has nothing to do with the cause: `itest_netloop` sets a flag that makes the
audio sidecar refuse to start, to prove a join is refused openly when the relay
cannot be brought back, and while it stayed set the second round died at
`adaemon >= 0` in the suite's *setup*.

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
- **A real-time rate is reported, not asserted narrowly.** Capture and recording
  run in real time, so how many frames a second of them yields is a property of
  the machine: a host that cannot encode 720p at 30 fps produces fewer frames,
  which is the recorder behaving correctly. A narrow band around the nominal count
  therefore tests the host — two such checks in `test_media.c` failed on two
  pinned CPUs and under ThreadSanitizer with nothing wrong in the tree. What a
  rate check may assert is what belongs to the code: that it never runs *faster*
  than it was asked to (the count, and the median gap between frames), that the
  timestamps go forwards, that there is no hole in the run (a whole second with no
  frame is a stop, which no load explains), and a **generous floor** — a fifth of
  the rate — below which there is nothing to watch. `check_frame_rate` in
  `test_media.c` is that check, and it prints the measured rate, which is the
  number a benchmark wants and a unit suite cannot assert. The same applies to a
  recording's *length*: stopping early loses what was being recorded and is a
  defect, stopping a little late is a busy machine.
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
conventions. The `push` trigger runs on **every branch except `main`** — `main`
moves only by promotion, and both `promote.yml` (on the staging commit being
promoted) and `release.yml` (on `main`) run the suite through `workflow_call`, so
a standalone push run would only cancel its twin in the same `concurrency` group
— and skips doc-only changes via `paths-ignore`. Because `promote.yml` runs the
suite itself, a docs-only staging tip is still tested before it is released.
There is no `pull_request` trigger: a pull request into `staging` is the same
commit its feature-branch push already ran.

Jobs:

- **`build`** — installs `libsqlite3-dev`, then runs `make` and `make test`.
  Fast feedback on compile + unit tests.
- **`integration`** — the daemon end-to-end, natively: build it and the e2e
  client, start the daemon on the runner, wait for `/healthz`, then drive the
  protocol vertical over TLS with the e2e client (§3.2 — the published image is
  tested by nothing).
- **`core`** — a standalone compile-check of the client app-core (ARCH-74).
- **`second-compiler`** — `make CC=clang test`, so an assumption only gcc
  accepts fails in CI rather than on someone's machine. The `build` job also
  compiles the daemon's sources with the release's own compiler (`make
  check-release-cc`: zig's clang, targeting the release's glibc), which is
  stricter than either and was not checked before a release failed on it.
- **`windows`** — the Windows cross-compile of the TUI and GUI
  (`make windows-tui windows-gui`), so the ported client stays building.

Everything runs non-interactively and communicates pass/fail purely through
exit codes, so no scenario depends on a human reading output.

**Audio.** `tests/test_audio.c` covers the **relay sidecar** — forwarding, call
isolation, and `REVOKE`. Echo cancellation is measured by the ERLE harness in
`tests/test_voice.c` (AUDIO.md §6.4), which voice input built: a synthetic room
impulse response over a far-end signal, near-end speech mixed in, clock drift
injected by resampling one side, and ERLE in dB asserted.

**Speech.** The `build` job renders a sentence with read-aloud and requires
`openchimed --stt-hear` to hear its words, so each speech feature checks the other
by content; the release repeats it on the stripped binary and on the installed
`.deb`. The same job builds `make TTS=0 STT=0` and starts it.

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

**Video messages drive on a synthetic camera.** Launch with
`WSLENV=OPENCHIME_TEST_CAPTURE:OPENCHIME_TEST_AUDIO OPENCHIME_TEST_CAPTURE=synthetic
OPENCHIME_TEST_AUDIO=synthetic scripts/gui_drive.sh launch` (add
`OPENCHIME_TEST_VIDEO_CAP_MS` to shorten the five-minute cap, or
`OPENCHIME_TEST_CAPTURE=denied` for the blocked-camera path). The `vm` verb opens,
records, stops, sends, plays and closes by name, and `dump` reports the overlay on
its `vm=` line. The synthetic source never opens a real camera, so a run cannot
turn on the camera of the machine it happens to be on.

**A real camera needs the client on a local disk.** Windows refuses camera
activation to an executable started from the WSL share (`\\wsl.localhost\…`):
`ActivateObject` fails with `0x80070490`, element not found, and the card says
the recording could not start, with that step in brackets. `gui_drive.sh launch`
runs `build/openchime.exe` from the share, which is fine for everything else;
for a camera test, copy it to a Windows directory and start it from there —
`OC_DRIVE_LOCAL=1` makes `launch` do exactly that.

**Two verbs put text in the composer, and they are not interchangeable.**
`type` sets the buffer directly (`ed_set`), bypassing the editor's own rules;
`typekeys` sends each character as a real `WM_CHAR` through the editor, so the
rich-mode typing rules (pending styles, continuation, delimiter handling) run
exactly as they do for a user. Testing an editor behaviour with `type` proves
nothing about typing.

**`key` presses a key down; it does not let it go.** The release is its own verb
(`keyup`), so a harness can hold a key — push to talk needs exactly that. The cost
is that a handler which re-arms on the release sees the first press and then
nothing: Backspace in the To: field takes one recipient per press and clears its
latch in `WM_KEYUP`, so a script that never released it deleted one chip and
silently did nothing on every Backspace after, while the checks that followed
asserted against a state the run never reached. **Anything pressed more than once
in a run must be released**, which is what `gui_newmsg.sh`'s `press` helper does:
`drive key <k>; drive keyup <k>`. Hold a key only where the hold is the thing
under test.

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

## Reading the startup harness

`scripts/gui_startup.sh` answers the question the smoke suite deliberately
stops short of: **what does the client do between launch and its first frame.**
It launches the client four ways — a workspace that signs in, one that does not
resolve (`nosuch.invalid`, reserved never to), one that is malformed, and one
that resolves but where nothing listens — and asserts on the client's own dump
for each: that the window is **visible**, that **exactly one** client started,
which view it landed on, and that a failure **says why**. Fifteen checks, on its
own fixture daemon and port (9510).

It exists because two defects went out through that stretch and were caught
only by a person noticing: a workspace that did not resolve left a running
process with an invisible window and no message, and the fix for it started the
client twice. The dump's `startup` line (`visible= started= si_ws= si_err=`) is
what makes both assertable. It was proved to catch both: with the double start
put back and the sign-in reason dropped, four of its checks fail, each naming
the defect.

Like the smoke suite it is not in CI, for the same reason, and it is not the
pre-push gate — run it when a change touches how the client starts.

## Reading the New message harness

`scripts/gui_newmsg.sh` asks the questions the New message pane (REQ-229)
answered wrongly for its whole life, in 32 checks on its own fixture daemon and
port (9520):

- **Does the field that looks focused get the keys?** Characters, and the keys
  that are not characters — Delete, Ctrl+A, Ctrl+V — while the To: field has
  focus. They used to fall through to the message body behind it, so Ctrl+V
  pasted into a message nobody could see and Ctrl+A then Delete wiped it.
- **Does the message go where the pane says?** It posts into `#general` first, so
  a wrong send has somewhere visible to land, then addresses a message to bob and
  presses Enter. `#general`'s message count must not move and the DM's must.
- **Does what you typed survive?** Leaving and returning must bring back the words
  **and** the recipients, and the pane's text must never become the selected
  conversation's draft.
- **Does Backspace take one recipient per press?** One press takes one; the key
  held — pressed again without a release — takes no more, which is the rule that
  stopped a held key walking backwards through the whole list.
- **Does a send with nobody addressed refuse, and say so?** The recipients being
  gone is asserted *before* Enter is pressed, the message must still be in the box
  afterwards, and a toast must say why. This check used to run with a recipient
  still attached — the message was sent and the check passed or failed on which
  dump line was read first — because the harness pressed Backspace without
  releasing it (see the `key`/`keyup` rule above).

It reads the dump's `newmsg` line — `focus= chips= q= caret= sel= matches= body=
pending=` — which exists so those states can be asserted rather than described.
Both halves of the pane are in it: which field owns the keyboard, who the message
is addressed to, what the query holds and where its caret sits.

Like the other GUI harnesses it is not in CI (the daemon is Linux-only and
GitHub's Windows runners cannot host it). Run it when a change touches the pane,
the target picker, or the composer's key routing.

## Reading the GUI smoke

`scripts/gui_smoke.sh` answers one question —
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

## Reading the screen-recording harness

`scripts/gui_screenrec.sh` records with real Windows.Graphics.Capture against its
own fixture daemon (port 9530), with a known picture in the camera box
(`OPENCHIME_TEST_CAPTURE=synthetic-camera`) and known sound
(`OPENCHIME_TEST_AUDIO=synthetic`: the microphone 440 Hz, the computer 660 Hz). It
asserts the screens and windows listed, the box in the chosen corner of the preview
(colour bars at the rectangle the dump's `vmsrc` line and `oc_inset_rect` put it),
the card stepping aside for the recording bar, the bar kept out of the capture and
read and pressed through UI Automation (`scripts/uia_recbar.ps1`), Stop ending the
take at the fitted size, the file the daemon stores — found in the fixture's blob
directory — being VP9 and Opus at that size with both tones in it (`ffprobe`,
`ffmpeg`), and a window closed mid-take keeping what came before.

**Keep the Remote Desktop window open while it runs.** A session that is not drawn
captures its screens as black; a window is captured either way. `gui_drive.sh
launch` with `OC_DRIVE_LOCAL=1` runs the client from a local copy, which a real
camera needs.

## Reading the reactions harness

`scripts/gui_reactions.sh` drives the who-reacted pane (REQ-070/071) over
`gui_pair.sh` (port 9580): bob reacts, so alice's pane has somebody else's
reaction to join. It asserts a chip per distinct emoji with its count, marked
when it is yours; that pressing one adds your reaction and pressing it again
takes it back, with the message agreeing; and that each chip is published to
assistive technology saying which it would do. The take-back check requires the
chip to have BEEN yours, or it would pass on a chip that was never pressed. It
then covers the strip of quick reactions on the message under the pointer: six
cells, published to assistive technology, one press to react and another to take
it back, and gone once the pointer leaves the row. It reads the dump's
`reactors`, `rxnchip` and `hoverreact` lines. The window is sized wide enough
for the context pane the list lives in, or nothing draws. Not in CI, for the
smoke's reason.

## Reading the shortcuts harness

`scripts/gui_shortcuts.sh` drives the Win32 client's keyboard shortcuts over
`gui_pair.sh` (port 9570) and asserts the EFFECT of each key, not that it
dispatched: two clients, so bob can leave something unread for alice. Today it
covers Shift+Esc — every conversation read, as the reference client binds it —
and that bare Esc still closes what is open and marks nothing; and Ctrl+<digit>,
which goes to that workspace counting from 1 in the rail's order, with a digit
past the last one doing nothing. The workspace half starts a **second daemon** on
the next port, because a client keys a workspace by its address and signing in
twice to one address is one workspace; and it sets the starting point through the
switcher's own path before each key, so a check cannot begin where it means to
end. It reads the dump's `marks ch … unread=`, `toast[n]` and
`workspaces=/active=` lines. Not in CI, for the smoke's reason.

## Reading the composer harness

`scripts/gui_composer.sh` drives the Win32 message box against its own fixture
daemon (port 9540) and asserts that the box is always tall enough for its text,
however the text got there: a draft long enough to wrap, restored when the client
starts again; leaving for a conversation with no draft and coming back; a
narrower and a wider window. It reads the dump's `ed` line — `lines=` (what the
text wraps to at the width it is drawn at) against `fit_lines=` (what the box
holds), which must agree up to the box's four-line maximum, past which the text
scrolls.
It also asserts that the formatting row and the action row are built to one
measure — the same button size, left edge and pitch, from the dump's `fmtbar`
and `actrow` lines — and that resting the pointer on each action button shows a
tooltip naming it, as the formatting buttons do (`actrow tip=`). Not in CI, for
the smoke's reason.

## Reading the drafts harness

`scripts/gui_drafts.sh` deletes drafts in the Win32 client against its own fixture
daemon (port 9560): each Drafts row's Delete, published to assistive technology;
the confirmation, where Cancel keeps the draft; a confirmed delete gone from the
list, the count and the sidebar, and not written back by the composer, which still
holds that conversation's text; the row's context menu doing the same. It then
checks that every context menu — a message's, a sidebar conversation's, a
member's, a draft's — keeps its row lit while it is open and only while, by
reading the row's pixels from a screenshot with the pointer moved off both the
menu and the row: hover alone could not, since a menu being open is what clears
or freezes it. It reads the dump's `draftn=`, `draftrows` (each row's
conversation, thread and Delete rect), `modal=`, `memrow` and `a11yitem` lines.
Not in CI, for the smoke's reason.

## Reading the files harness

`scripts/gui_files.sh` pages the Win32 files view (REQ-143) against its own
fixture daemon (port 9590) with `OPENCHIME_FILE_PAGE=2`, so three uploads are
enough to see paging a real daemon would only show past two hundred files. It
asserts that the first page holds its two rows and says more remain; that "Load
more" is published to assistive technology; that pressing it APPENDS the next
page rather than replacing what is shown — three rows, each file once — and that
the button then goes, there being no more. It reads the dump's `files` line:
`n=` (rows held), `more=` (the daemon says more remain), `loading=`, `rows=`
(rows drawn) and `more_btn=` (the button's rect, `0,0,0,0` when it is not there).
The paging underneath is proven without a screen in `test_client_core.c`, which
asks the daemon for pages of two and checks the second continues from the first
rather than repeating it. Not in CI, for the smoke's reason.

## Reading the members harness

`scripts/gui_members.sh` scrolls the members pane (REQ-031) against its own
fixture daemon (port 9600) bootstrapped with sixty people — `OC_DEV_USERS`, all of
whom land in `#general` on registration — so the roster is three times what the
pane holds. It asserts that only what fits is drawn and the pane says the rest is
below it; that the wheel reaches **every** member, counted as the distinct people
drawn on the way down (sixty of sixty); that the bottom row is then somebody the
first screen never showed, and answers a click by opening that person's profile;
that the offset stops at both ends rather than running past them; and that
switching channel puts the pane back at its own top. It reads the dump's
`members n= rows= scroll= max=` line and the `memrow uid= r=` lines, and drives
the `wheel` verb in detents of 120.

**Counting the people is the check; reading the bottom row is not.** A first
version asserted that the last drawn row was a real member and that the offset
reached its maximum — both of which hold with the scroll offset removed entirely,
because the pane still draws a full first screen and the maximum is computed from
the roster either way. It passed 11/11 against a deliberately broken build. What
distinguishes the two is whether anybody past the first screen is ever drawn.

## Reading the calls harness

`scripts/gui_calls.sh` runs two Win32 clients in a call over `gui_pair.sh`
against its own fixture daemon (port 9620): alice's synthetic microphone at
440 Hz, bob's at 660 Hz (`OPENCHIME_TEST_TONE`). The clients reach the daemon at
the WSL machine's own address rather than 127.0.0.1, because WSL forwards only TCP
there and a call's audio is UDP. It asserts the start and bob's invitation (his
Calls section and a notification), both in the call hearing each other with
every packet decrypting once the keys are in, noise suppression taking a steady
hum out, bob muted and seen muted, push to talk, a per-person volume, only the
starter ending, the missed-call line, and screen sharing — alice shares the
synthetic screen (`OPENCHIME_TEST_CAPTURE=synthetic`), bob's view shows it with
readable, rising frame numbers, full screen and Esc, actual size, the
accessibility names, bob taking over and stopping. It reads the dump's `call` line — `in=
parts= epoch= sent= keepalives= tx_epoch= self= slot=` — its `call.peer` lines —
`packets= lost= level= keyed= muted= undecryptable= volume=` — its `share` line —
`sharer= on= state= bar= view_frames= tex= px_fno= full= actual=`, `px_fno` being
the synthetic screen's frame number read from the decoded pixels — and `calls[n]`
and `callevents`. The `call` verb drives it: `call start|join|open|leave|end|decline
[ch]`, `call mute|unmute|ptt-down|ptt-up`, `call ns on|off`, `call invite <uid...>`,
`call volume <uid> <percent>`, `call share <device id or name>|pick|stop|full|actual`.
Not in CI, for the smoke's reason.

## Reading the voice harness

`scripts/gui_voice.sh` drives voice input in the Win32 client end to end against
its own fixture daemon (port 9510), with real recognition: read-aloud renders a
sentence, and `OPENCHIME_TEST_AUDIO=synthetic` with `OPENCHIME_TEST_MIC=<wav>`
makes the client's microphone speak it. It asserts push to talk by the held key,
by UI Automation and by the mouse (words in the composer, nothing sent), free talk
(the words posted, the composer untouched), that leaving the conversation and
opening the video recorder end a session, that a blocked microphone
(`OPENCHIME_TEST_AUDIO=mic-denied`) is reported, and that with `OPENCHIME_STT=0`
there are no controls at all. It reads the dump's `dictate` line — `avail=
offered= on= mode= hold= sent= answered= words= err=` and the button rects — and
its `toast[n]` lines. Not in CI, for the smoke's reason. Run it when a change
touches voice input, the composer's controls or the device layer.

## The visual audit — checking a render with no render to compare it to

`scripts/gui_audit.sh` walks every surface of the Win32 client across themes,
DPI settings and text sizes, and checks each captured scene against properties
it must hold **on its own**. It is not the smoke and must not become one: the
smoke asks "does the client boot" in ten seconds and runs before every push;
this runs a few hundred states, takes minutes, and is for when GUI chrome
changes.

**Why there is no reference image.** The first version of this audit diffed the
SDL render against the Direct2D binary it was ported from, ranked the pairs by
PSNR and walked the ranking. That rig did the job a diff can do and is finished.
A diff finds *divergence*, so it is blind by construction to anything both
renderers did the same way — and the defects left are exactly those. Three of
them were logged against that audit while it was still running, each found by
eye and none by the diff, and the note on the third says why in one line: *the
old client has identical geometry.* A picture that looks fine to a diff and
wrong to a person is the whole remaining category.

So the reference is gone. What replaced it is the client's own account of what
it drew.

**The paint ledger** (`gfx_ledger_dump`, verb `ledger <path>`) is one row per
primitive, in draw order, with the rect, the clip in force, the colour and a
tag — see the note in `client/gui/gfx/gfx.h`. It is allocated only when
`OPENCHIME_TEST_DIR` is set. From it, `scripts/audit/oracles.py` asks questions
a single frame can answer:

- a string whose raster runs past its clip is **truncated**, and nothing on
  screen says so;
- two strings sharing pixels with no opaque fill between them are
  **overprinted**;
- ink measured against the surface beneath it is a **contrast** ratio;
- a rect drawn outside the window is **unreachable chrome**;
- a colour that resolves to no palette token is a surface that **will not
  follow the theme**.

**Read the asymmetries, they are deliberate.** A text row's *width* is the
advance width of the glyphs, so a rect wider than its clip is a cut string.
Its *height* is the line box — ascent, descent and leading, pinned per size
token (ARCH-108) — which legitimately overhangs its seat, and a row half below
the fold of a scrolling list is ordinary rather than broken. So horizontal
clipping is reported and vertical clipping is not, and a clip that removes an
element *entirely* is not reported at all: that is how scrolling works, and
from a ledger row a scrolled-out row and a stray one are the same thing. The
instrument that can tell those apart is the fit check inside the client, which
is built from the accessibility tree and therefore only ever sees what is
reachable.

The same discipline decides the rest. A scrim is a layer boundary — a modal
owns the window, the accessibility publisher already skips the shell behind
one, and the ledger checks do too, or a sidebar label two hundred pixels away
comes back "overprinted" by a modal row. A colour tagged `content:` is data
rather than a theme choice (an avatar disc derived from a user id, a swatch for
a scheme you have not picked) and is exempt from the palette check by that
declaration rather than by a list of exceptions here.

**Every one of these started as a false positive and was fixed rather than
muted.** The first run reported sixty-five findings on a healthy screen. A
check that fires on correct code is one people learn to skip, which is how the
client's own fit check came to report phantom overlaps for months without
anybody reading it.

**`scripts/audit/pair.py`** compares two captures of one surface in two states
and reports what MOVED. Some defects are invisible in one frame: a label that
shifts a pixel when its row goes semibold looks correct in both screenshots and
reads as a twitch in use. The property is that changing a state must not change
a position.

**Two accounts of one frame, cross-checked.** The client also publishes its
accessibility tree into the dump, one line per element. An element published at
coordinates the ledger painted nothing at is a **phantom** — a screen reader is
invited to activate something that is not there, and every check built from the
tree alone compares it against real elements and reports collisions nobody can
see. Nothing internal to the tree can catch that, because the tree is
consistent with itself; only the other account knows. This is what found the
Admin view still offering conversation rows for a sidebar it does not draw.

**The checks that need pixels** (`scripts/audit/pixels.py`) close the gap
between what the app asked for and what the renderer produced. The stroke
tessellator once blew its vertex budget and discarded nine icons whole: the draw
call happened, the ledger would have recorded it, and nothing appeared. So a
filled shape must *be* its colour (holes mean uncovered tessellation), an icon
box must not come back one flat colour, and a disc's rim must be a ramp rather
than a step. Each is asked only of shapes nothing was drawn over afterwards —
without that filter every button in the app is a finding.

**The states that must agree** (`scripts/audit/consistency.py`) compare the
client against itself. A theme reached by a live switch must render as a cold
start in that theme does; 96 → 192 → 96 must equal a cold 96; a conversation
reached twice must look the same both times. The first version made the theme
case a *round trip* — X to Y and back to X — and it passed while the defect it
was written for was live in the build, because a cache whose key forgets the
theme is stale in both directions and lands on the right colours by being wrong
twice. It is one switch now, from a cold start in the other theme.

**Contact sheets** (`scripts/audit/sheets.py`) are for the findings no check
will ever make. "A 13px marker on an 18px tile looks bad" is a judgement, not a
property. The sheets do not replace the eye; they change what it is asked to
do — every icon at every size on one page, every avatar composite on another,
the corner arc of every rounded rect on a third. A thing that is wrong is then
sitting beside eleven things that are right, which is a glance rather than a
hunt through 224 screenshots.

**`scripts/audit/selftest.py` proves every check can fail**, with a synthetic
scene per check and, for the pixel ones, a real capture handed a ledger that
lies about it. Run it after touching anything in that directory; it needs no
client. A check that has never failed has not been shown to check anything, and
this project has the scars: the client's own fit check spent months reporting
phantoms nobody read.

**Three tags a call site can declare**, all of them narrowing what the checks
may ask rather than excusing a result:

- `content:` — this colour is DATA, not a theme choice (an avatar disc derived
  from a user id, a swatch for a scheme you have not picked).
- `content:emoji` — a COLOUR glyph: its raster is not the ink it was asked for
  and its advance is not its ink, so neither contrast nor truncation applies.
- `transient:` — present in some frames and not others by design (the composer
  caret blinks on a 530ms phase), so frame-equality checks skip it.

**Reading a run.** Scenes and how to reach them live in
`scripts/audit/scenes.tsv`, one row per surface with the verbs and — not
optional — a dump key to wait for. A verb acks when its handler *ran*, not when
the frame showing its effect has been painted, and several of these views paint
"Loading…" until the daemon answers; an earlier pass slept a fixed time instead
and captured Admin mid-load, which read as a layout defect and cost a round to
explain.
