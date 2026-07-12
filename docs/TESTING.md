# OpenChime — Testing Strategy

How OpenChime is tested, and the conventions any new test follows. Recorded as
a decision in [ARCHITECTURE.md](./ARCHITECTURE.md) (ARCH-48) and consistent
with the sibling C project openblocks, whose hand-rolled test convention this
mirrors.

**Status.** Strategy defined before implementation. As of this writing the
only code is the placeholder daemon (`src/main.c`); the unit tier below has no
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
  its real wire protocol and its real SQLite + Litestream + object-storage
  path. This is where durability-critical behavior (ARCH-23 replication, ARCH-24
  restore-on-boot) *must* be proven, because none of it is reachable from a unit
  test.

A behavior is tested at the lowest tier that can actually observe it. Codec
edge cases belong in unit tests; "two clients see each other's messages in
order" belongs in integration.

---

## 2. Unit tier

### 2.1 Convention (mirrors openblocks)

- Tests live in `tests/`, one translation unit per subject
  (e.g. `tests/test_protocol.c`).
- A test TU **`#include`s the `.c` under test directly** (e.g.
  `#include "protocol.c"`) so it can reach file-static helpers and state
  without exporting them just for testing. This is the same technique
  openblocks uses to test `game.c`/`input.c` in isolation.
- A single hand-rolled macro, no framework:

  ```c
  static int failures = 0;

  #define CHECK(cond)                                                    \
      do {                                                               \
          if (!(cond)) {                                                 \
              printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);   \
              failures++;                                                \
          }                                                              \
      } while (0)
  ```

- `main()` runs each test group, prints a one-line summary, and returns
  non-zero if `failures > 0`:

  ```c
  int main(void) {
      test_header_roundtrip();
      test_size_limits();
      test_version_negotiation();
      if (failures == 0) { printf("OK: all checks passed\n"); return 0; }
      printf("FAILED: %d check(s)\n", failures);
      return 1;
  }
  ```

- Built and run by `make test`, compiled `-O0 -g` (debuggable, and cheap since
  there is no window/GL/TLS to link). A non-zero exit fails the build and CI.

  ```makefile
  test: build/test_protocol
  	./build/test_protocol

  build/test_protocol: tests/test_protocol.c src/protocol.c src/protocol.h | build
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
- **Migration runner** (ARCH-27) — applying migrations against an empty
  `schema_version`, resuming a partially-migrated DB, and refusing to skip or
  reorder.
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
links the same `src/protocol.c` the daemon uses** — not a re-implementation of
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
client; `src/tls.c` already provides a TOFU-pinning client, so socket-level
integration scenarios are no longer gated. `tests/itest_tls.c` exercises the
handshake + pinning end-to-end today.

### 3.2 Runner: the existing Docker Compose stack

Integration tests run **against the Docker Compose environment already defined
for local dev** (ARCH-36/38/39), not a second bespoke environment — the same
`daemon` + `minio` + `minio-init` topology, so replication and restore are
exercised against a real S3-compatible store and a real Litestream, exactly as
production does. A wrapper script, `Scripts/test-integration.sh`, brings the
stack up, runs the scenario driver, and tears it down with a non-zero exit on
any failed scenario. It sits alongside the existing `run.sh`/`stop.sh`/`reset.sh`
wrappers (ARCH-40).

### 3.3 Scenarios

Grouped by what they prove. The starred (★) ones are reachable **today** with
the placeholder daemon and are what CI runs now; the rest come online with the
real protocol.

- **Durability (must-have, ARCH-23/24):**
  - ★ replication reaches object storage: after startup, the Litestream replica
    prefix in MinIO is non-empty within the sync interval;
  - ★ restore-on-boot: wipe the daemon's local volume, restart, and confirm the
    daemon logs a restore-from-replica (not a fresh init) and comes healthy;
  - ★ health check: `/healthz` returns `200 OK` (ARCH-25).
- **Handshake/versioning:** a client advertising an unsupported range gets a
  `REJECT` with the right `VERSION_TOO_OLD`/`VERSION_TOO_NEW` code and a closed
  connection (REQ-110/111).
- **Auth:** a valid JWT reaches `AUTH_OK`; an invalid one gets
  `AUTH_INVALID_TOKEN` and is closed (REQ-023). (JWKS is stubbed with a
  test-issuer keypair; real provider integration is a separate concern.)
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
- **`integration`** — the durability path end-to-end on the Compose stack:
  build the image, bring the stack up, wait for `/healthz`, assert the MinIO
  replica prefix is non-empty (replication, ARCH-23), then wipe the daemon
  volume, restart, and assert a restore-from-replica log line (ARCH-24). This
  is the automation of the manual steps in the README's "Verify" sections.

Everything runs non-interactively and communicates pass/fail purely through
exit codes, so no scenario depends on a human reading output. New unit test
binaries are added to the `build` job's `make test`; new socket-level
integration scenarios join the `integration` job once the client-TLS decision
(§3.1) is settled.

---

## 5. Layout summary

```
tests/
  test_protocol.c        # unit: frame codec (first to land)
  test_migrations.c      # unit: migration runner
  itest_client.c         # integration: C client reusing src/protocol.c
  ...
Scripts/
  test-integration.sh    # brings up compose, runs scenarios, tears down
.github/workflows/
  ci.yml                 # build + unit tests, and the compose durability path
Makefile
  test:                  # builds + runs the unit binaries (-O0 -g)
```
