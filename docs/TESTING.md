# OpenChime — Testing Strategy

How OpenChime is tested, and the conventions any new test follows. Recorded as
a decision in [ARCHITECTURE.md](./ARCHITECTURE.md) (ARCH-48) and consistent
with the sibling C project openblocks, whose hand-rolled test convention this
mirrors.

**Status.** Strategy defined before implementation. As of this writing the
only code is the placeholder daemon (`daemon/main.c`); the unit tier below has no
tests yet, and the integration tier exercises the placeholder's
build → replicate → restore pipeline (ARCH-35–39). Both tiers grow as the real
daemon lands, starting with the frame codec.

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

- Built and run by `make test`, compiled `-O0 -g` (debuggable, and cheap since
  there is no window/GL/TLS to link). A non-zero exit fails the build and CI.

  ```makefile
  test: build/test_protocol
  	./build/test_protocol

  build/test_protocol: tests/test_protocol.c shared/protocol.c shared/protocol.h | build
  	$(CC) $(CFLAGS) -O0 -g -Isrc tests/test_protocol.c -o $@
  ```

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
- No test asserts an exact wall-clock timestamp. Server time and
  `message_id` assignment are reached through an **injectable clock/id source**
  so a test can supply a scripted sequence — the same technique openblocks uses
  to drive `input.c` from a fake 60 Hz clock. Tests assert *relative* facts
  (ids strictly increase; a timestamp is within a supplied fake range), never a
  literal now().
- Tests touch no network and no real disk. Where a subject needs SQLite, it
  uses an in-memory database (`:memory:`), not a file.

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

The test client lives under `tests/` (e.g. `tests/itest_client.c` plus a
scenario driver) and is built by a dedicated make target. It speaks real
frames: `HELLO`/`WELCOME`, `AUTH`, `SEND`/`SEND_ACK`/`BROADCAST`/`CLIENT_ACK`,
`BACKFILL_REQUEST`/`BACKFILL_DONE`.

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

Grouped by what they prove. The starred (★) ones are reachable **today** with
the placeholder daemon and are what CI runs now; the rest come online with the
real protocol.

- **Liveness:**
  - ★ health check: `/healthz` returns `200 OK` (ARCH-25).
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

---

## 4. Continuous integration

CI is GitHub Actions (`.github/workflows/ci.yml`), mirroring openblocks'
conventions: it triggers on pushes to `main` and on pull requests targeting
`main`, skips doc-only changes via `paths-ignore`, and uses a `concurrency`
group to cancel superseded runs.

Jobs:

- **`build`** — installs `libsqlite3-dev`, runs `make` (and `make test` once the
  first `tests/` TU exists). Fast feedback on compile + unit tests.
- **`integration`** — the deployed image end-to-end on the Compose stack:
  build the image, bring the stack up, wait for `/healthz`, then drive the
  protocol vertical over TLS with the e2e client. This is the automation of the
  manual steps in the README's "Verify" section.

Everything runs non-interactively and communicates pass/fail purely through
exit codes, so no scenario depends on a human reading output. New unit test
binaries are added to the `build` job's `make test`; new socket-level
integration scenarios join the `integration` job once the client-TLS decision
(§3.1) is settled.

---

## 5. Layout summary

```
tests/
  check.h                # shared CHECK macro
  main.c                 # calls each run_<suite>_tests(), sums failures
  test_protocol.c        # frame codec
  test_framebuf.c        # incremental frame reassembler
  test_migrate.c         # migration runner
  test_dbwriter.c        # DB-writer thread: migrate-on-boot, AUTH/SEND jobs
  itest_tls.c            # in-process: TLS handshake + TOFU pinning
  itest_netloop.c        # in-process: two-client AUTH + SEND + BROADCAST
  e2e_client.c           # black-box client for the harness (standalone binary)
Scripts/
  test-integration.sh    # brings up compose, runs the 4 e2e checks, tears down
.github/workflows/
  ci.yml                 # build + `make test`, and the compose e2e harness
Makefile
  test:                  # one binary (build/tests): unit + in-process integration
  integration:           # Scripts/test-integration.sh (containerized daemon)
```

All unit and in-process integration suites compile into a **single** binary
(`build/tests`): each `tests/*.c` links the module's public API (no per-test
binary, no unity `#include` of a `.c`) and exposes `run_<suite>_tests()`, which
`tests/main.c` aggregates. The only separate artifact is `e2e_client`, which by
definition talks to a *deployed* daemon rather than in-process modules.
