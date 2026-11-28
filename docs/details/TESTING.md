# OpenChime — Testing Strategy

How OpenChime is tested, and the conventions any new test follows. Recorded as
a decision in [ARCHITECTURE.md](../ARCHITECTURE.md) (ARCH-48).

## 1. The two suites

Every test is defined in one place and belongs to one of two suites: the
fast one, which the full one includes, or the full one alone. Each suite is a
pair of make targets, and each is the gate for one step of the release
process (CONTRIBUTING.md):

| Suite | Commands | Runs | Gate for |
|---|---|---|---|
| **fast** | `make build`, `make test` | locally, before every push; the pre-push hook refuses a tree `make test` has not passed on | a push, and so a pull request |
| **full** | `make build-all`, `make test-all` | in CI, once per release-candidate, on the staging tip it cuts (`release-candidate.yml`) | the `release-N-rcK` tag |

**The fast suite takes about four minutes**, `make build` and `make test`
from `make clean` on a developer's machine, one compile and one suite at a time
("One build at a time", below); a suite that pushes it well past that
moves to the full suite. **The full suite is a superset of the fast one.** No
test runs on a pull request (`pr-policy.yml` runs policy checks: the title,
the attribution scan, the issue references), in a release or in a promotion.
`make build` and `make test` refuse to run in CI, and `make build-all` and
`make test-all` anywhere else (`CI` unset).

**A documentation-only change is the one exception to the fast suite.** A
branch whose every change is documentation (`scripts/policy/docs_only.sh`, the
one rule) may be pushed without `make test`: the pre-push hook lets it through,
and `pr-policy` still checks it. Once merged, `promote.yml` can put it on
`main` without a release.

**The fast suite** (`make test`, `scripts/test/fast/`):

- `make build`, the fast build: the daemon with no speech, summaries or web
  client (`TTS=0 STT=0 SUM=0 WEB=0`, whatever was asked or remembered before)
  and the Windows GUI and TUI, a job per CPU; `make test` builds the test
  binary, `build/tests`, beside them;
- the source checks (`scripts/test/fast/source.sh`): no two message types
  share an opcode; no file cites an issue (`scripts/policy/refs.sh`); the
  Makefile's targets are the ones its header names; the configuration
  registry is the one source of every `OPENCHIME_*` name; nothing that ships
  runs in development mode; the tests' own rules (§2.3a); every linked
  component has its licence notice; what a release lists, closes and
  sends as shipped; the speech data's archive tool checking, joining and
  unpacking a stand-in archive offline, refusing a changed file by name
  (`scripts/test/fast/speech_sources.sh`); the apt and rpm repositories
  taking a release's packages, the daemon's and the Linux client's side by
  side, one version more of each package name per architecture, and
  refusing a second copy of a version (`scripts/test/fast/repo_update.sh`);
  and the Flatpak repository taking a release's bundles for both
  architectures, its summary and each commit signed, each commit verifying
  against the key as a client verifies it, refusing a changed summary, a
  commit whose signature is missing or another key's, and a key that cannot
  sign, and repairing, with `resign`, a repository published with unsigned
  commits, at the same commits (`scripts/test/fast/flatpak_repo.sh`); both
  on a local-path rclone remote with a throwaway signing key;
- actionlint on the workflows (`scripts/test/fast/actionlint.sh`, fetched
  pinned on first use);
- the suites `tests/main.c` marks `FAST`, one after another (§2.4), timing judged;
- on a pass, the tree it passed on, recorded for the pre-push hook.

**The full suite** (`make test-all`, `scripts/test/full/`), on what `make
build-all` builds: everything a release ships. `PART=` splits both into the
steps CI runs one after another (`mk/full.mk`):

- `plain` — the daemon with every feature and the web client in it, the
  Linux TUI and GUI, the e2e client, the fuzzing harnesses, and sdltext's and
  the Linux GUI's test programs; then the fast suite's
  source checks and actionlint; every suite that starts no thread, the `FULL`
  ones included, and `media`, plainly with timing judged (§2.4); the
  static-link check; the end-to-end run against a natively-run daemon (§3.2);
  the speech round trip; sdltext on FreeType and the Linux GUI's test program
  on a session bus of its own (§2.2c); the web client's objects' header
  dependencies (§3.4); and each fuzzing harness
  for its fixed inputs, one at a time, inside 20 seconds in all (§2.2b);
- `threads` — the suites that start threads, under ThreadSanitizer, their one
  run in the full suite (§2.4), in one process, one after another, nothing
  beside it, about 30 seconds in CI; the end-to-end walks marked `WALK` run
  in the plain pass instead;
- `packages` — the release's daemon for each architecture (gcc 12, glibc 2.35)
  and its `.deb`, `.rpm` and tarball; then the glibc floor, one binary in all
  three, speech on the release binary, the `.deb` installed and speaking, and
  its unit file;
- `image` — the image laid from that tree; then it starts, and its database
  replicates and restores (§3.2);
- `windows` — the Windows binaries, the installer and the MSIX; then their
  version resources, the installer and the MSIX;
- `linux-client` — the Linux client for `ARCH` (x86_64, aarch64), built
  twice with gcc 12: against Ubuntu 24.04's libraries (the machine's for
  x86_64, the pinned arm64 sysroot's for aarch64) and packaged as the `.deb`
  and the `.rpm`, and against the pinned Freedesktop SDK and assembled as the
  Flatpak (VENDORS.md §2); then the desktop entry and the metainfo validate,
  the packages hold the client for `ARCH` needing no glibc newer than 2.39,
  with every library it links among their dependencies, the `.deb`'s client
  prints its version where the machine can run it, and the Flatpak names
  its runtime and its sandbox (`scripts/test/full/linux_client.sh`). arm64
  is cross-compiled with no emulation, and no arm64 package is installed on
  the machine.

The code tests (`plain`, then `threads`, in one job) run in `test-all.yml`,
which `release-candidate.yml` calls as its `test` job: they start the daemon
in development mode, which no release-path workflow may
(`scripts/test/fast/release_lint.sh`). `linux-client` runs there too, after
them, for both architectures: the client's packages build on Ubuntu 24.04,
the job's runner, and reach `stage` as an artifact. The rest runs in
`release-candidate.yml`'s `stage` job, on the artifacts it then stores and
publishes. A check on a binary or an image the runner cannot execute (the
arm64 ones on an x86 runner) says so and skips.

**After a release publishes**, `release.yml` verifies what it published
(`scripts/release/smoke.sh`, every check in `scripts/release/smoke/`: the apt
and dnf repositories install the daemon, and the Linux client on
`ubuntu:24.04` and Rocky Linux 10; each index lists the release once more,
and the Flatpak repository's signed summary its client, each commit
signed with the key; the Flatpak repository adds from its `.flatpakrepo`,
lists the client for both architectures, and pulls each architecture's
commit metadata with signature checking on, as an install does; the released image pulls and runs). That is release verification, not a suite: it checks a
publication, not a change.

**Manual harnesses** live in `tools/` and are run by hand, never by either
suite: the TUI demo (`tools/demo/`), the
capacity benchmark (`tools/bench.sh`) and the toolchain check
(`tools/toolchains.sh`).

### Levels

Within either suite, a behaviour is tested at the lowest level that can
actually observe it:

- **Unit and in-process integration** — the suites of `build/tests`: pure
  logic (the frame codec, migration runner, idempotency/dedup bookkeeping,
  rate limiter, the protocol state machine), and the real epoll server over
  TLS and the headless client app-core, in process (§2).
- **Black-box integration** — the whole daemon running as a process,
  exercised over its real wire protocol against its real SQLite path (§3):
  the deployed image, TLS termination and the protocol vertical end to end.
  Off-box backup is a deployment concern (ARCH-3), but the replicator the
  image carries for it is exercised by the full suite's image part (§3.2).

Codec edge cases belong in unit tests; "two clients see each other's messages
in order" belongs in integration.

**Coverage.** The unit suites cover the codec, framebuf, migrations,
auth/JWT/roles/rate-limiting, the DB-writer handlers, storage/maintenance,
enrollment, push, and the shared @mention scanner (`test_mention` —
deliberately its own suite because the daemon and every client link that one
implementation, ARCH-89), the message-formatting parser (`test_richtext`,
REQ-220/ARCH-100 — same reasoning one level down: both frontends render from
that one parser, and its edge cases are where a dialect eats text somebody
meant literally; tables included), the table grid and its conversions
(`test_table`), the shared URL scanner (`test_url` — the address-boundary
rules the client autolinks by and the daemon unfurls by, shared for the same
reason), the unfurl fetcher's pure halves (`test_unfurl` — the SSRF gate's
per-address verdict, the HTML title/description scan, and the block-page
refusal), and the shared notify evaluator, plus in-process integration suites
that drive the real epoll server over TLS (`itest_netloop`, `itest_tls`,
`itest_slow_blob`) and the headless client app-core (`test_client_core.c`) —
all compiled into one `build/tests` binary. The codec is fuzzed twice over: a
deterministic sweep in every run of the suites and coverage-guided libFuzzer
harnesses in the full suite (§2.2b, "Fuzzing"). A concurrency load test
(`tests/bench_load.c`, driven by `tools/bench.sh`) rounds it out, by hand
(§5).

### Prerequisites

**`make build` and `make test`** (Linux, x86-64 or arm64): a C compiler
(`cc`), `make`, `nasm` (libvpx's assembly), `curl` and `xz` (the pinned
llvm-mingw for the Windows clients) and `python3`. The vendored libraries and the toolchain build themselves on first
use (`scripts/build/*.sh`, `mk/toolchains.mk`); no Emscripten is
needed. actionlint is fetched once into
`~/.cache/openchime/`. The repository checks (`repo_update.sh`,
`flatpak_repo.sh`) need `rclone`, `gpg`, `dpkg-deb`, `apt-ftparchive`
(`apt-utils`), `rpmbuild` and `rpmsign` (`rpm`), `flatpak` and `ostree`.
Five things are checked only where present, and a
missing one is a `SKIP` (a failure where `CI` is set): `ffprobe` (package
`ffmpeg`, the media suite's recording check, a full-suite suite),
`createrepo_c` (package `createrepo-c`, the rpm half of `repo_update.sh`), an
IPv6 loopback (`::1`), a non-loopback LAN address (`test_client_core`'s
certificate-trust checks), and a kernel that leaves a connection attempt
pending (`test_sock`'s connect bound, skipped when the kernel answers every
SYN). `gdb` is optional and makes a crash print every thread's stack.

**`make build-all` and `make test-all`** run in CI only and refuse anywhere
else. What they need is installed by the workflows: `cmake` 3.28+ for SDL3's own
builds, `setarch` for the
ThreadSanitizer part, a clang with libFuzzer for the fuzzing harnesses,
`ffmpeg`; for the Linux GUI and its test programs (§2.2c) the development
packages of FreeType, HarfBuzz, fontconfig, GLib, libsecret and libpipewire,
the X11, Wayland, xkbcommon, EGL, OpenGL and libdecor headers SDL3's Linux
build needs, the DejaVu fonts, `dbus-run-session`, `pipewire` and
`wireplumber` (DEVELOPMENT.md, "The Linux GUI", names the Ubuntu packages); for
the Linux client's packages gcc 12 and Ubuntu's gcc 12 cross compiler for
arm64, `flatpak` and `ostree`, `desktop-file-validate`
(`desktop-file-utils`), `appstreamcli` (`appstream`), `rpm` and `cpio`, with
the Freedesktop SDK and the Ubuntu arm64 sysroot, which the build fetches
pinned (VENDORS.md §2); and for the release's artifacts
`podman`, `buildah`, `rpm`, `cpio`, `nsis`, `python3-pefile`, and clang and
the pinned CMake `scripts/build/makemsix.sh` fetches.

**TUI script** (`tools/demo/tui.sh`): `tmux` and `curl`.

### One build at a time

Three rules hold for every build and every test run, on every machine, CI
included. They are enforced, not only configured: a limit kept in a setting is
a limit something on a command line overrides, and nobody sees it until the
build is measured.

- **One build at a time.** The `Makefile` runs whatever goals were asked for
  inside one run of `scripts/test/lib/clean_run.sh`, which first takes an
  exclusive lock on `/tmp/openchime-build.lock`. A second run, from this
  checkout or any other, says it is waiting and starts when the first ends.
  That is every goal: `make build`, `make test`, the full suite's parts, and a
  build by path (`make openchimed TTS=1`, the `make daemon` a harness under
  `tools/` runs). A make started inside a run does not start another; a dry
  run (`make -n`) takes no lock; `make run` builds inside the run and starts
  the daemon after it has ended.
- **One compile and one suite at a time.** The `Makefile` is `.NOTPARALLEL`:
  one recipe at a time whatever `-j` or `MAKEFLAGS` it is given. Every script
  under `scripts/build/` gives the library it builds a literal `-j1` (or
  `--parallel 1`) and clears a calling make's flags, and Emscripten is told one
  core (`EMCC_CORES=1`). There is no job-count variable to set. The suites run
  one after another, in one process: both `suites.sh` set `OC_TEST_JOBS=1`.
  `clean_run.sh` checks it while the build runs: everything the run starts
  carries the run's mark in its environment (`OC_CLEAN_RUN`), and if two
  compiler or assembler processes carrying it run at once, neither started by
  the other, it names both, stops the run, and fails it. A compiler and what
  it starts itself count as one.
- **Nothing left running.** When the command ends, `clean_run.sh` names and
  stops any process that still carries the run's mark, wherever it moved
  itself, and fails the run; interrupted, it stops the command's process group
  and whatever else carries the mark. A process without the mark is never
  counted or stopped: a compile or a daemon you started yourself is not the
  run's. Nor is one marked process: the container runtime's pause process
  (`catatonit -P`), which rootless podman starts with its first command, to
  hold the user's namespace for every podman command after it, and leaves
  running. Started inside a run (the image tests are the first to use podman),
  it carries the mark, and is neither counted nor stopped.

`scripts/test/fast/clean_run.sh` tests the wrapper with stub commands, in both
suites. What is not held: threads inside one process (a suite's own, a
compiler's, a linker's); and what the manual harnesses under
`tools/` start besides the `make` they run.

---

## 2. The test binary: unit and in-process suites

### 2.1 Convention

- All unit and in-process integration suites compile into **one** binary,
  `build/tests`, run by both suites, and that binary runs in **development
  mode** (it sets `OPENCHIME_MODE=development` for itself, CONFIG.md "Mode"):
  the suites set the development inputs, which no other mode reads; they sign in
  as a person does, through the daemon's page (`tests/testsignin.h`). There is deliberately no per-test binary:
  the tests exercise public APIs, so each `tests/*.c` links the real module
  objects rather than `#include`-ing the `.c` under test.
- Platform-backend code that cannot execute on the build host (sdltext's
  DirectWrite backend, ARCH-106, and the gfx layer's SDL backend) has no test
  program of its own: the fast build cross-compiles it with the client (the
  compile is the header/vtable check mingw keeps honest), and the Windows
  client is tested by hand. The portable half of sdltext (the byte↔UTF-16
  offset map) is a suite like any other.
- A subject's tests live in one translation unit exposing a single entry point,
  `int run_<subject>_tests(void)`, which runs its groups and returns its
  failure count. `tests/main.c` calls each and sums; a non-zero total exits
  non-zero and fails the suite.
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

- A test that genuinely needs a file-static helper can have its one TU
  `#include` the `.c` under test directly (a long-standing C testing technique); every
  other test links the public API instead.

- Compiled `-O0 -g` (debuggable). A non-zero exit fails the suite. One rule
  builds the single binary from every `tests/*.c` except the tool programs
  (`e2e_client`, `demo_client`, `bench_load`), plus the
  daemon and app-core objects it links (`mk/tests.mk`), each source compiled
  to its own object under the object tree.

- **`SUITES[]` in `tests/main.c` is the one list of the suites and of where
  each runs**: a row per suite, `S(run_<subject>_tests, FAST|FULL,
  PLAIN|THREADS|TIMED|WALK)`. `FAST` runs in both suites, `FULL` in the full suite
  only (one too slow for the fast suite's budget); `THREADS` marks a suite
  whose code starts threads, which the full suite runs under ThreadSanitizer
  and only there, and `TIMED` one that the full suite also runs plainly, for
  the timer tests the sanitizer does not run (§2.4), and `WALK` one that starts threads but which the full suite
  runs plainly and not under ThreadSanitizer: an end-to-end walk of the daemon
  or the client, or real-time media, whose sanitizer run would cost minutes. The scripts never
  keep a list of their own: they ask the binary, `build/tests --list fast`,
  `--list full` (every suite), `--list plain` (the full suite's plain pass:
  all but `THREADS`) or `--list threads` (`THREADS` and `TIMED`, not `WALK`).

  Adding a suite is therefore two steps: drop in `tests/test_<subject>.c`
  exposing `run_<subject>_tests()`, and give it a row in `SUITES[]` (and a
  declaration beside the others).

No external test dependency is vendored: the whole harness is `tests/check.h`
(the `CHECK` macro, the waits and `SKIP`) and `tests/testenv.h`, rather than a
framework.

### 2.2 What the unit suites cover

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
- **The blob cache in front of S3** (`test_blob_cache`, ARCH-70) — against a
  fake S3 endpoint that holds many objects, lists them a page at a time and
  fails every request on command: an upload accepted while the store is down,
  readable from disk, and sent once it is back; failures counted and retried
  with a wait that grows; a read fetched into the cache on a miss and served
  from it after; a missing object told apart from an unreachable store; a
  delete made while the store is down reaching it later; eviction down to a
  budget that never takes a blob the store does not have; a read and a write
  with no room on the disk going to the store directly; blobs already on disk
  sent once to a newly given store and not again; the queue surviving a
  restart; and the sweep deleting only old objects no row names, under the
  store's own prefix, across several listing pages.
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
- **The HTTP stack** (ARCH-32, `tests/test_http.c` and `tests/test_ioloop.c`) —
  parsing over picohttpparser: request line, headers, body framing, partial
  input, and what is refused (a chunked body, two `Content-Length`s that
  disagree, a length that is not a number, a head over 8 KiB or 32 headers); the
  router (exact and prefix paths, the query ignored, `404`, `405`, a fallback, a
  named path beating the fallback); the response writer. Through the real I/O
  threads: a request for the loop's route sent in pieces is reported once,
  whole; a static route and every refusal (`400`, `404`, `405`, `413` before the
  body is sent, `408`) are answered on the I/O thread and never reported; a
  second request on the connection gets nothing; a plaintext socket is served
  its own site. Each guard — the timeout sweep, the per-listener site, the body
  limit, the chunked refusal, the one-request cut, the `405`, the per-address
  cap on the health port, the disagreeing lengths, the head cap — was shown to
  fail its test when reverted.
- **Video message media** (`tests/test_media.c`, ARCH-110) — the MP4 writer
  and reader round-trip and the reader survives a mutation fuzz; VP9 and Opus
  round-trip against quality floors; the recorder runs against the synthetic
  camera and tone, and `ffprobe` (installed in CI as a validator, never linked)
  checks the file it writes; the player plays, seeks and copes with a slow
  decoder. `tests/test_video_media.c` covers the protocol and daemon side.
- **Export of a video message** (ARCH-135) — in `make test`,
  `tests/test_export.c`: the H.264 + MP3 writer's layout, parameter sets, sync
  samples, offsets and audio edit; the render, with stub codecs, reading a source
  once front to back and handing every sample over in file order, and stopping
  cleanly when cancelled or when a read fails; the worker's read-aloud gate,
  joining of duplicate requests, three requests a person, order, keys and the
  temporary files it never leaves; and the cache's refusals, hits by version,
  reclaim first under pressure and with its video. `itest_export_wire` (in
  `tests/itest_netloop.c`, also in `make test`) runs a daemon with export off, on
  without codecs, and on with the stub: the capability, the refusal by name with
  the connection kept, and the copy streamed byte for byte. In the full suite,
  `tests/test_export_media.c` (`export_media`) exports a synthetic 1080p
  recording with the real codecs and has `ffprobe` and `ffmpeg` check it: one
  H.264 Constrained Baseline and one MP3 stream at the source's size and
  duration, a flash and a click within a frame of each other, and the transcode's
  peak memory under 96 MB. Run it here with `OC_TEST_ONLY=export_media`.
- **A call's recording** (CALLS.md §8, ARCH-136) — in `make test`,
  `tests/test_callrec.c`: a spool written as the Recorder writes one, made into a
  recording with stub codecs and read back — the voices mixed, each where it was
  said (after a silence, and after a device counted its frames afresh), the
  speaker large and the others in the strip, a shared screen large while shared;
  a cancelled, a damaged, an empty and an over-long spool; the file-backed MP4
  writer byte for byte against the in-memory one; and the worker storing the
  recording under its key, removing the spool, and finishing on its next start one
  it was stopped in the middle of. `tests/test_dbwriter.c`: a recording made (an
  attachment of the call's chat with its media row and digest, and the message
  carrying it) and failed, the list a start resumes, and the retention.
  `tests/test_client_core.c` (full): the Recorder through the real daemon and
  relay at 10% loss, the spool, the recording made and sent to the call's people,
  the call history and page through the core, a transcript and its delete, and a
  joiner held back until it acknowledges. In the full suite,
  `tests/test_callrec_media.c` (`callrec_media`) makes a recording of a synthetic
  two-person call with libvpx and libopus and decodes it: the picture and the
  voices where they were. Run it here with `OC_TEST_ONLY=callrec_media`.
- **Screen recording** (`tests/test_media.c`, REQ-162) — the camera box's geometry
  and the frame views that draw it; the screen front end repeating a still screen
  at the frame rate; recordings of the synthetic screen with the camera boxed into
  each corner (checked by colour in a decoded frame), of a window without one, of
  a window that goes away; the computer's sound mixed with the microphone and,
  with the microphone hearing only its echo, present once. The canceller that does
  it is measured in the ERLE harness (`tests/test_voice.c`) at 48 kHz on the same
  rooms as voice input's.
- **The speech kernels** (nnkit, TTSKIT.md §5b, STTKIT.md §4) — `tests/test_nnkit.c`,
  in the fast suite: each kernel read-aloud's and voice input's models run on
  against a naive version of its operator on random inputs.
- **Read-aloud's voice** (ARCH-111, TTSKIT.md §9) — `tests/test_ttskit.c`, in
  the fast suite: the duration rounding and the token-to-frame
  alignment, `voice.bin`'s loader on a file built byte by byte (a wrong magic,
  version or length refused), its converter on a small model built in the
  upstream formats, and the committed `ttskit/data/voice.bin` loading with every
  voice and refused when damaged. `voice_parity` in `tests/test_voice.c`, a
  `FULL` suite: every fixture in `tests/data/voice/` rendered from the committed
  `voice.bin` with the model's noise off has the reference's sample count and
  at least 40 dB SNR against the reference's noise-off rendering (136–141 dB
  measured), and with the noise on the noise's variance is within 10% of what
  the model's operators specify. The fixtures — six sentences in two voices,
  their token ids and the reference's raw output with the noise on and off, and a
  `manifest` — were made once, before the engine they were made with was withdrawn; they cannot be made again;
  the committed files are the reference.
- **Voice input** (ARCH-112) — `tests/test_voice.c`: the segmenter over
  synthetic voiced sound with known pauses (free talk, the cap, push to talk) and
  the ERLE harness (AUDIO.md §6.4: converged, 100 ppm drift, double-talk);
  `tests/test_stt.c`: spoken mentions and the recognition worker against a stub
  engine (order, a full queue, idle release); `tests/test_sttkit.c`, in the fast
  suite: the tokenizer and the token ceiling, `recognizer.bin`'s loader on a file
  built from the format's description (a wrong magic, version, length or shape, or
  a missing tensor, refused), the committed file loading, and the converter
  refusing what is not the published model; `recognizer_parity` in the same file,
  a `FULL` suite: every fixture in `tests/data/recognizer/` heard from the
  committed `recognizer.bin` gives the reference's token ids, with its encoder
  output and first decoding step's logits within 30 dB SNR of the reference's
  (measured: the encoder output identical, the logits 129–137 dB). The fixtures —
  seven utterances spoken by read-aloud's voice and silence, with the reference's
  tokens, encoder output and first logits — were made once, before the engine they were made with was withdrawn; they cannot be made again;
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
- **Encrypted DMs** (PRIVATE-DM.md), `tests/test_pdm_crypto.c`: the Noise XX handshake
  against the published `Noise_XX_25519_AESGCM_SHA256` vectors (every handshake and
  transport message, the handshake hash), then what vectors do not cover: each altered
  message and a disagreeing prologue fail, turns are enforced; the Double Ratchet, which
  has no published vectors, by its rules: a key per message, a new ratchet key per turn,
  out of order within 256 skipped and refused beyond, replays and used keys refused, an
  altered body or header refused with the session unchanged, another session's message
  refused, and a wiped state all zeros.
  `tests/test_protocol.c` round-trips every `PDM_*` frame and refuses one out of
  size; migration 0063 adds `channels.e2e`.
  `itest_netloop`'s `test_pdm_relay` drives the daemon with raw frames: refused to
  oneself; the opener given `PDM_OPENED` and then the channel's `CHANNEL_INFO`, a DM
  with the peer marked `e2e`; an invitation to each of the peer's devices, taken by
  the first that answers (which is given the channel too) and refused to the others;
  the opener's first ratchet message relayed as `PDM_MSG`; then `SEND`s as in any DM,
  their bodies kept and sent on as given, to the two devices only; a `SEND` from the
  peer's other device refused with `FORBIDDEN` and nothing sent to it, and a third
  person refused; an upload refused with `FORBIDDEN`; the channel list's entry
  flagged `e2e` with no preview; ended, after which a `SEND` is `CHANNEL_ARCHIVED`;
  declined; a new channel each time; and kept for its session while a side is away,
  a fresh sign-in sent nothing of it. `test_client_core`'s `test_pdm_e2e` opens one
  between two clients through the real daemon: refused to oneself; nothing on the
  peer until it accepts; the same safety number on both; the channel a DM marked
  `e2e` on both; text both ways, the sender reading its own; an edit, a reaction and
  a delete as in any DM; closed, nothing sent after it, and forgotten with its text;
  and a declined invitation. It also checks the credential store's remembered peer
  keys.
- **Tables** (REQ-220, ARCH-100, MARKDOWN.md §1a) — `tests/test_richtext.c`'s
  `test_tables` covers what is a table and what is not: the ordinary case with
  every alignment, no outer pipes, no header, short and long rows, `\|`, the
  ends (a blank line, a line with no pipe), a header alone, CRLF, two tables,
  and the refusals -- no separator, mismatched cells, a separator that is not
  dashes, no pipe in the header, a fenced block, a quote or a list, past the
  column limit -- each scanned as text, with no inline span reaching across
  cells. `test_table_cells` covers a cell's spans (emphasis, `<br>` in every
  spelling and nowhere outside a cell, the escaped pipe, no block markers) and
  `test_table_flatten` the one-line excerpt, cut only at a character boundary.
  `tests/test_table.c` round-trips the grid through Markdown (pipes, line breaks,
  alignment, no header, typed markup kept), edits it (rows and columns in and
  out, the header and the last row kept), reads spreadsheet text (Excel's
  quoting; and what is not a table -- one row, one column, ragged rows, code
  indented with tabs) and clipboard HTML (bold by tag and by style, links kept
  only to http(s), entities, line breaks, colspan, comments and styles ignored).
  `tests/test_speakable.c` checks a table is announced with its row count and
  its cells skipped, and `tests/test_client_core.c`'s `test_table_preview` that
  the sidebar preview and a message's one-line text read "Table: N rows × M
  columns".
- **Polls** (REQ-225, ARCH-119) — `tests/test_protocol.c`'s `test_poll_frames`
  round-trips all six frames and refuses an option count past ten, and
  `tests/test_fuzz.c` feeds their decoders. `tests/test_dbwriter.c`'s `test_polls`
  covers the writer: a poll posted with its tally read back; one option, an empty
  or oversized one, no question and a past deadline refused; single choice moved,
  refused a second pick and withdrawn; several picks counted, a repeat refused; who
  voted, by option, and refused for an anonymous poll; a vote past the deadline
  refused; an edit by its author changing everything while nobody has voted,
  refused to anyone else and for one option, refused for options or settings once
  someone has voted, the deadline still moved, and closed now so the next vote is
  refused; a vote on an ordinary message refused; a backfill carrying each poll
  with the asker's own selection; a poll in a thread and its list; the poll's rows
  gone with a delete; and a poll refused in an encrypted DM. `tests/itest_netloop.c`'s
  `test_poll_vertical` runs it over TLS: the `polls` capability; `POLL` right after
  the message's `ACTION`; a vote reaching the conversation without the voter's
  selection and the voter with it; a bad vote refused to the voter alone; who
  voted, and an anonymous poll refused; an edit reaching the conversation, and
  refused to someone not its author; a second connection's backfill carrying
  the voter's choice. `tests/test_client_core.c`'s `test_poll_model` folds the
  frames into the model -- the selection kept when a tally arrives without it, the
  poll gone with a tombstone and not brought back by a late tally, the who-voted
  rows for the poll asked about only -- and the live run posts, votes and asks who
  voted between two clients through the real daemon.
- **What a message says to a screen reader** (REQ-269, ARCH-99) —
  `tests/test_spoken.c` (fast) builds a message's spoken name from its parts:
  the defaults reproduce the transcript's two forms exactly, present and
  deleted; each switch on its own adds or removes only its part; reactions in
  the message's order, replies at none, one and many, the text first, nobody
  named read as "someone", and the cut at a small buffer on a whole UTF-8
  character.
- **Calls** (REQ-150–152, REQ-301–306, ARCH-73/113/136) — `tests/test_e2e.c`: SFrame
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
  revoked, and the relay's sweep reported, applied, and told to the swept
  connection; a disconnected participant's seat held (marked away in a new
  epoch, nothing more until the grace), taken back by a join from a new connection in the same call at a new epoch, dropped
  at the grace, and a call whose members all disconnect surviving their return
  and ending at the grace when they do not return. `test_callsig`: getting back
  into a call, frame by frame with a fake engine and clock — after a reconnect,
  a sweep or a moved path; given up when the call ended, another took its place,
  none was reported, the join was refused, the window passed or three rejoins
  came within it; not after a leave, a quit or a move to another device. `test_client_core`: two
  and then three client cores in a call through the daemon and a relay with a
  **tap** in front of it, synthetic tones out and Goertzel measurements in —
  each hears the others at full level and never itself; a joiner misses only the
  grace; the tap sees only SFrame, never a repeating plaintext byte; after a
  leave, only keys the leaver never had; mute (seen by the others), push to talk
  and per-person volume; only the starter ending; the missed call; and the device
  key stored, re-read, upgraded from a version 2 entry and forgotten on sign-out;
  and getting back in — someone talking alone for twice the lost-path time
  staying put on UDP; one client's connection cut by a forwarder in front of
  the daemon, marked away on the other's roster, her seat held and the same call
  rejoined, heard again from her new engine, the other never moving; then every UDP path moved by the tap, each engine reporting its path
  lost and rejoining, and heard again by UDP, not left on the connection.
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
- **IPv6 and address literals** (ARCH-14/54) — `test_client_core`: every
  literal form (`192.0.2.7`, `:port`, `[v6]`, `[v6]:port`, a bare v6, a zone)
  resolves to its canonical host and port with **the SRV, DNS and `.well-known`
  counters (`oc_resolve_counts`) unmoved**, malformed forms are refused, keys fold
  spellings, and `oc_hostport`, `oc_addr_is_loopback` and
  `oc_workspace_takes_suffix` — the managed suffix beside a bare name or nothing
  typed, never beside an address, a port, a dotted name or `localhost` — hold to
  their tables; a
  client resolves `[::1]:<port>`, signs in and sends over IPv6 with nothing else
  looked up; the call engine picks the relay's family IPv4 first and IPv6 when
  that is all there is, starts on the connection when it has no UDP, and dana, on
  `127.0.0.1`, and faye, signed in by `[::1]`, hear each other over UDP through a
  dual-stack tap that sees faye arrive over IPv6. `test_sock`: a connect to a
  listener whose accept queue is full gives up at its bound, and a name moves on
  to the address that answers. `test_url`: `[v6]:port` authorities and the
  bracketed Host header. `test_push`, `test_enroll` and `test_blob_s3` each run
  their fake server on `[::1]` too. `itest_tls`: a certificate naming
  `127.0.0.1` passes for that address, with no SNI sent, and fails for `::1` and
  for a certificate with no IP-address name, while a host name still goes as SNI.
  `test_unfurl`: each IPv6 block the SSRF gate refuses, the embedded IPv4 of a
  6to4 or IPv4-compatible address judged, public controls let through.
  `test_srccount`, `test_dbwriter` and `itest_netloop`: an IPv6 source counts by
  its /64 — sign-in attempts sprayed across one /64 share a limit, and `::1`
  meets the per-address cap on its own key while `127.0.0.1` holds its own.
  `test_proxyproto`: an IPv4-mapped PROXY source is written as its IPv4 address.
  `test_signin`: the loopback listener, with IPv4 refused, binds `[::1]` and
  redirects there. The literal check, the bracket parsing, the /64 key, the 6to4
  judgement, the IPv4-first order, the connect bound and the IP-address name
  check were each shown to fail their test when reverted.
- **Rate limiter** (REQ-190/191) and the **connection state machine**
  ([PROTOCOL.md](./PROTOCOL.md) §10) — legal transitions accepted, illegal
  frames rejected with the expected reason code.

### 2.2a When the binary itself crashes

A crash would otherwise leave nothing to work with: the binary's output is block-buffered
into CI's pipe, so the suite that was running goes down with the process, and
there is no backtrace — a crash on CI is knowable only as "Segmentation fault
(core dumped)". `tests/main.c` therefore line-buffers its output, names each suite as it
starts it, and handles the fatal signals: a crash prints the signal, the suite,
how far into the run it was, the faulting address, the crashing thread's stack
resolved to file and line by `addr2line`, and — where `gdb` is installed, as it is
on CI — every thread's stack. Then it raises the signal again, so the exit status
is the one it would have been.

Two switches help hunt a crash that appears once in fifty runs, rather than
paying for unrelated suites per attempt:

- `OC_TEST_ONLY=audio,media` runs only the suites whose names contain those,
  `OC_TEST_EXCEPT` all but those; a selection that names no suite fails rather
  than passing empty;
- `OC_TEST_REPEAT=20` runs the selection that many times.

Unset, nothing changes. The suites run **in one process** (§2.4), a repeated
suite with the rest, so a suite that
leaves a fixture switched must put it back or the second round fails somewhere
that has nothing to do with the cause: a suite that swaps the loop's call relay
socket, as `itest_netloop`'s routed-call test does, must hand it back, or the next
round's loop starts without one.

### 2.2b Fuzzing

**Two speeds.** `tests/test_fuzz.c` is a deterministic sweep, in both
suites: 5,000 random and 2,000 framed inputs through `oc_parse_frame` and
every payload decoder, from a fixed seed (raise them with
`-DOC_FUZZ_RANDOM_ITERS=` and `-DOC_FUZZ_FRAMED_ITERS=` for a deep run). The
coverage-guided tier is libFuzzer harnesses under `tests/fuzz/`, each built
under ASan and UBSan and run by the full suite. Both use one list of decoders,
`tests/fuzz/decoders.c`, so a new message type's decoder is added there once,
and its encoder to `tests/fuzz/seeds.c`.

**The harnesses** are `FUZZ_HARNESSES` in `mk/sources.mk`, the one list of
them, each `tests/fuzz/<harness>.c`, built as `build/fuzz/<harness>`; the
header of each file says what it fuzzes and what must hold. The dictionary a
harness takes is `FUZZ_DICT_<harness>` beside the list (`tests/fuzz/*.dict`),
none where unset, and the number of inputs the full suite runs it for is
`FUZZ_RUNS_<harness>`. The full suite's pass is given all three by make
(`scripts/test/full/fuzz.sh`). It does the same work every run: one harness
at a time, each for its fixed inputs from a fixed seed (`FUZZ_SEED`, 1), from
its checked-in corpus. The counts are sized from CI's measured speed so the
whole pass fits inside `FUZZ_SECONDS` (20), which is also a gate: one deadline
for the whole pass, which each harness may run up to, and one stopped there is
named as cut short.
A longer run is by hand. A new harness is its file, a word in that list, its
input count, and a line for its sources in `mk/tests.mk`.

A harness that takes several inputs reads them with `oc_fuzz_split`
(`tests/fuzz/fuzz.h`): each part but the last is prefixed by its length, a
big-endian u16.

**Prerequisites.** A clang with libFuzzer and the sanitizer runtimes: on
Ubuntu, `clang` and `libclang-rt-18-dev` (or the version your clang is). Only
the harnesses need it; the fast suite never does. Make uses `/usr/bin/clang`
(`FUZZ_CC`): the pinned llvm-mingw make puts first on `PATH` has no Linux
runtimes.

**Building and running one.** Harnesses are tools built by path, not a
target:

```
make build/fuzz/frame
mkdir -p /tmp/frame && build/fuzz/frame -dict=tests/fuzz/frame.dict /tmp/frame tests/fuzz/corpus/frame
```

The first directory is where libFuzzer writes what it finds. Keep it out of the
tree. `-max_total_time=<s>` stops a run; `-runs=<n> -seed=<s>` makes it the
same run every time. The full suite runs each harness for its
`FUZZ_RUNS_<harness>` inputs from seed 1 (`make test-all PART=plain`,
`scripts/test/full/fuzz.sh`). A finding fails the job, with the sanitizer's
report and the input, base64, in the log.

**The corpora** under `tests/fuzz/corpus/<harness>/` are real frames:
`build/fuzz/seeds <dir>` writes one per encoder in `shared/protocol.c` (each
over a zeroed message) and a few streams of them for `framebuf`. They are kept
whole rather than minimized with `-merge=1`, which would keep about one in five
and lose the frame for most message types. `tests/fuzz/frame.dict` holds the
protocol version and every message type id. Regenerate both when the protocol
changes. The `image` harness's corpus is images rather than frames: a small
JPEG, PNG and WebP of each kind the preview decoders take, made with the same
libraries (docs/details/IMAGE-PREVIEWS.md §5).

**A crash.** Reproduce it with `build/fuzz/<harness> <file>`, and minimize it
with `build/fuzz/<harness> -minimize_crash=1 -runs=10000 <file>`. Check the
minimized input in under `tests/fuzz/crashes/<harness>/`:
`run_fuzz_replay_tests` (`tests/test_fuzz_replay.c`) runs every corpus and
reproducer file through its harness in both suites, so the crash fails there
until it is fixed, and stays a regression test afterwards. A missing
`crashes/` directory counts as no files.

### 2.2c The Linux GUI's test programs

The Linux GUI's own code cannot be in `build/tests`: it compiles against
system development packages (FreeType, HarfBuzz, fontconfig, GLib's GIO,
libsecret, libpipewire) that `make build` and `make test` must not need, so a
host without them still runs the fast suite. Its tests are two programs of
their own, built by path, each with its failure count as its exit code, and
run by the full suite's `plain` part, where `test-all.yml` installs the
packages (§1, "Prerequisites"). Built without the packages, each stops before
compiling, naming the ones `pkg-config` cannot find.

- **`build/sdltext_test`** (`sdltext/st_test.c`) is sdltext's one test
  program for every backend; here it runs on FreeType, HarfBuzz and
  fontconfig (`sdltext/st_ft.c`), and `make build/sdltext_test.exe` builds the
  same program against DirectWrite for Windows. It covers measurement and
  wrapping, range styling over multibyte text, the three hit tests, inline
  boxes, line metrics, a uniform line height across weights, a right-to-left
  run hit-testing to cluster boundaries, a code point the primary family lacks
  still drawn from fontconfig's fallback, and the pixels a memory sink
  receives. Every assertion is a relation the API promises whatever the font,
  except a few bounds on measured widths and line heights, which are DejaVu
  Sans's on Linux (`fonts-dejavu-core`) and Segoe UI's on Windows.
  `scripts/test/full/sdltext.sh` runs it, and fails at once if fontconfig
  finds no DejaVu Sans.
- **`build/linux_gui_test`** (`tests/linux_gui_test.c`) covers the Linux
  platform (`client/gui/platform/linux/`), desktop notifications
  (`client/shared/osnotify_linux.c`) and capture through the portals
  (`client/core/media/cap_portal.c`), linked from the GUI's own objects and
  none of the application layer, in seven groups: **platform** (the data
  directory, the crash ring's mapping, a URL handed over from a second
  process, the autostart and URL-scheme `.desktop` files, all in temporary XDG
  directories), **image** (each format decoded to known pixels, and a header
  over the pixel cap refused before anything is allocated), **notify**
  (`Notify`'s arguments, replacing, withdrawing, a click, a button and a
  typed reply reaching the callback, and no server), **a11y** (the AT-SPI tree
  read back over the bus, the composer's text and caret, and `DoAction`
  posting the item's token), **capture** (the portal's requests granted,
  refused and dismissed, no portal, and the frame conversion from synthetic
  buffers in every format it accepts), **stream** (the PipeWire stream
  itself: a source the program publishes, BGRx with a crop region as a
  screen and NV12 as a camera, with a frame counter in its pattern, captured
  through the backends -- the format agreed, each frame converted to the
  expected I420 and the crop honoured, a screen's frame reported only when
  the picture changed and not for a buffer that carries none, the camera in
  the list by its node, and the source going away ending the capture,
  `OC_CAP_GONE` for a screen) and **settings** (colour scheme,
  contrast and animations from the Settings portal, and each absent one
  answering -1). The desktop it talks to is its own: a notification server,
  an accessibility bus and registry, and a portal, served in C over GDBus on
  a session bus the run starts for itself, `dbus-run-session`, so nothing on
  the machine's own bus is touched; and for the stream group a real
  `pipewire` and `wireplumber`, started as the program's children with
  runtime, configuration and state directories under its temporary directory
  (no device monitors, so no camera or sound card of the machine's is seen),
  waited for by their socket and WirePlumber's metadata appearing, and
  stopped at its end. The stand-in portal's `OpenPipeWireRemote` answers with
  a connection to that PipeWire, and `Start` with the source's node.
  `scripts/test/full/linux_gui.sh` runs it, and needs `pipewire` and
  `wireplumber` installed as it needs `dbus-run-session`; run by hand
  without them, the stream group is a `SKIP`.
  A check that would need the bus to have no real server, where the bus
  would start one, is a `SKIP`, and so a failure in CI.

To run them locally, with the packages installed (DEVELOPMENT.md, "The Linux
GUI"):

    make build/sdltext_test && build/sdltext_test
    make build/linux_gui_test && dbus-run-session build/linux_gui_test

### 2.2d The UI harness

The desktop client, driven by a test against a real daemon, step by step,
with every screen and its accessibility tree compared exactly against a
reference. `make test-ui` runs it, here only: it needs `build/openchime.exe`
and a machine that runs Windows programs (WSL), so it is in neither suite.
Its suite, `run_ui_harness_tests` (`tests/ui/ui_harness.c`), is in
`build/tests` marked `UI`, which `--list` never names and a run takes only
when `OC_TEST_ONLY` names it.

**Nothing in it waits.** No step sleeps, polls a time or has a timeout: a
step ends when its command is answered, and one that needs the daemon ends on
the daemon's barrier (`sync`). A harness test that needs a wait is wrong.

**The pieces.**

- The **driver** (`tests/ui/driver.c`, `driver.h`) starts a daemon in the
  test binary's process (`oc_netloop_run_ex`) on a fresh database and data
  directory, with its test hooks on (`OPENCHIME_DEV_TEST_HOOKS`), its writer's
  clock held (`oc_dbwriter_hold_clock_ms`), a workspace name, and alice
  (owner), bob and carol. It listens for each client, launches it, signs it
  in through the daemon's page (`oc_test_signin`) and hands it the result.
- The **client** with `--harness <port> --harness-token <t>` connects to the
  driver (`--harness-host`, an IPv4 address; 127.0.0.1 unless given) and
  presents the token; a wrong one is refused and the client ends. It then
  takes one JSON command per line and answers each with one JSON line. Its
  transport is `client/gui/app/harness.c`; the commands are `app.c`'s
  (`harness_step`), because it is the app's state they change. It touches
  nothing of a person's installation: no remembered workspace, no stored
  session, no tray, toast or link registration. It draws with SDL's software
  renderer, and with `--harness-headless` on SDL's offscreen video driver.
- **One command, one frame.** The loop is the client's own `pump()`: in
  harness mode each turn reads one command, applies it, takes in what the
  core has, ticks, draws exactly one frame, and answers with that frame's
  number. A command that opens something modal (a form, with its own loop) is
  answered once it is drawn, and the driver drives the modal next.
- **The clocks are held.** Every clock the client reads -- `now_ms()`, its
  wall clock, the core's -- is one source the driver sets (`clock
  {set_ms}`) and steps (`clock {advance_ms}`); outside harness mode it reads
  the system's. The daemon's writer clock is held too; `ui_daemon_advance`
  steps it and `ui_daemon_sweep` runs its timed sweeps (scheduled messages,
  reminders, expiring mutes) now.
- **The barrier.** `sync` sends `SYNC` (PROTOCOL.md §5.16s) and answers once
  `SYNC_DONE` is folded into the model: everything the client sent before is
  done and its results and fan-out taken in. What arrives can prompt the
  client to ask for more (a conversation drawn asks for its members), so a
  round is taken in, ticked and drawn, and rounds repeat until one prompted
  no request. Not covered: work off the daemon's writer, such as a link's
  preview or a push to a phone.

**Settings are the run's.** `settings` takes `dpi` (the display scale: 96 is
100 %, 144 is 150 %), `w` and `h` (the window, in pixels), `tz`, `theme`
(`dark`, `light`), `textsize` (0-3), `density` (0 compact, 1 cozy), `motion`
(`on` as designed, `reduced`, `system`) and `front` (whether the window is in
front of a person: 1 unless set). They are kept over whatever a workspace's
stored preferences say once it signs in. Nothing is read from the desktop:
not its scale, its reduced-motion setting, nor which window has the
keyboard, so a run is the same headless or visible.

**The commands.**

| Command | Members | Does |
|---|---|---|
| `settings` | as above | sets them |
| `clock` | `set_ms`, or `advance_ms` once set | holds or steps the clock |
| `signin` | `ws`, `host`, `port`, `fp`, `source`, `verifier`, `token` | signs in with a browser sign-in's result; `fp` pins the daemon's certificate |
| `sync` | | the barrier, above |
| `click` | `id`, or `x` and `y`; `button` (`left`, `right`), `clicks` | by automation id runs what the accessibility invoke runs (a left single click), else a click at the item's centre; by position, in the frame's pixels |
| `move` | `x`, `y` | the pointer |
| `key` | `key` (an SDL key name), `ctrl`, `shift`, `alt` | one press, its text with it when it types one |
| `text` | `s` | typed text |
| `wheel` | `x`, `y`, `dy` | the wheel |
| `frame` | | one frame, nothing else |
| `tree` | | the accessibility items of the last frame (kind, id, name, rect in pixels) and the keyboard's place (`focus`); no frame |
| `shot` | | the last frame's pixels: `{w, h, bytes}`, then that many bytes of RGBA; no frame |
| `quit` | | the client ends |

**References.** `ui_check(client, test, step, settings)` compares the tree
and the frame with `tests/ui/ref/<test>/<step>@<settings>.tree.json` and
`.png`, exactly; a reference belongs to its settings, so the same step at 96
and 144 DPI is two. A difference, or a reference missing, fails naming the
file, and what was seen is left under `build/ui-out/`. `OC_UI_UPDATE=1`
writes the references instead; a reference is reviewed like code.

**Running it.**

    make test-ui                        # headless
    OC_UI_VISIBLE=1 make test-ui        # each client in a window, to watch
    OC_UI_UPDATE=1 make test-ui         # write the references
    OC_UI_TRACE=1 make test-ui          # each command and its answer

Under WSL the driver meets the client on WSL's own address, which Windows
reaches directly: WSL's forwarding of 127.0.0.1 takes up a newly opened port
only some time later, and a client that connects first is refused.
`OC_UI_HOST` names another address.

### 2.3 Determinism rules

Unit tests must be reproducible and independent of wall-clock or environment:

- Any use of `rand()` seeds it explicitly (e.g. `srand(12345)`).
- No test asserts an exact wall-clock timestamp. Tests assert *relative* facts —
  ids strictly increase, a timestamp falls inside a window the test itself
  bounded — never a literal `now()`. **Where time passing is the behaviour
  under test, the test moves the clock the code reads** rather than sitting the
  time out: `oc_dbwriter_advance_clock_ms` (the writer's: a token ageing, a
  restore window closing), `oc_netloop_advance_clock_ms` (the loop's: a
  session's last-seen interval, a device code's life), the certificate
  worker's `now_ms` in `oc_certs_opts` (an ARI window), and the media clock
  (`oc_media_clock_hold` and `oc_media_clock_advance_us`, `oc_media.h`), which
  the synthetic camera, screen and microphone, the recorder and the player all
  read and wait on: held, it moves only when the test moves it, so a recording
  holds every frame the camera made however slow the host, and a test moves it
  a frame or a step at a time, waiting each time on what the code did
  (`rec_sweep`, `play_sweep` in `test_media.c`). Each puts the clock back when
  nothing reads it any more. Also injectable is the interval a periodic job
  runs at (`oc_dbwriter_set_idem_retention`, `oc_netloop_set_relay_silence_ms`,
  `oc_netloop_set_presence_rate_ms`, `OPENCHIME_DEV_CALL_TIMERS`,
  `OPENCHIME_DEV_VIDEO_CAP_MS`).
- **A check that something does NOT happen never waits out a timeout to say
  so.** It waits for something that must come *after* the thing it rules out,
  and then looks: the daemon answers a connection's requests in order, so a
  channel-list request is a fence behind which everything already queued for
  that connection has arrived (`count_frames` in `itest_netloop.c`); the relay
  handles one socket's datagrams in order, so a revoked token sent before a
  valid one would reach the listener first; a queue plays in order, so what is
  offered next shows what was queued before it. Where there is nothing to order
  against, the wait is short and says why a short one is a full one (a relay
  pumped on the test's own thread has already delivered). A negative check that
  sits out a five-second read timeout costs five seconds on every run and
  proves no more.
- **A recording is as long as its checks need, not longer:** the shared one
  crosses the 2 s keyframe the poster and the slow-decoder check need; the
  screen recording is two seconds, enough for the narrator's pause and a
  second of talking; a frame-rate check needs about fifteen frames.
- **Every `test_dbwriter` database starts as a copy of one migrated template**
  (`start_db` in `test_dbwriter.c`) rather than migrating from nothing; the
  other suites start their writers on fresh paths, which migrate on boot.
- **A real-time rate is reported, not asserted narrowly.** Capture and recording
  run in real time unless a test holds the media clock, so how many frames a
  second of them yields is then a property of the machine: a host that cannot encode 720p at 30 fps produces fewer frames,
  which is the recorder behaving correctly. A narrow band around the nominal count
  therefore tests the host — two such checks in `test_media.c` failed on two
  pinned CPUs and under ThreadSanitizer with nothing wrong in the tree. What a
  rate check may assert is what belongs to the code: that it never runs *faster*
  than it was asked to (the count, and the median gap between frames), that the
  timestamps go forwards, that there is no hole in the run (a whole second with no
  frame is a stop, which no load explains), and a **generous floor** — a fifth of
  the rate — below which there is nothing to watch. `check_frame_rate` in
  `test_media.c` is that check, and it prints the measured rate, which is the
  number a benchmark wants and a unit suite cannot assert. A test that asserts a
  recording's frame count, its length or how its tracks end holds the media
  clock instead, and then those are the recorder's.
- Unit tests touch no network. **Several suites use real files on disk**, not
  `:memory:` — `test_dbwriter`, `test_push`, `test_client_core`, `itest_netloop`
  and `itest_slow_blob` each open a database, because they exercise
  migration-on-boot, WAL behaviour and multi-connection paths that an in-memory
  database does not reproduce. Each suite's files live in **a directory of its
  own**, made under `$TMPDIR` by `oc_test_mkdtemp` (`tests/testenv.h`) and
  removed at its end — never a fixed name under `build/` or `/tmp/`, which two
  runs of the same checkout, or two checkouts, would share. The
  purely-logical suites (codec, framebuf, mention, richtext, searchq) touch
  nothing.

### 2.3a Brittle tests

**A flaky test is a bug.** A test that gives a different result on the same
code -- fails once and passes the next time, with nothing changed -- gets an
issue when it is first seen, and is fixed or removed before the next merge to
`staging`. A failed run, local or in CI, is never re-run to get a pass: a pass
on a re-run counts for nothing, and the failure is the one to explain.

**A test is brittle if its result can change without a change in behaviour a
person, an operator or an integrator could see — or if it can stay green while
that behaviour regresses.** Both are defects in the test: the first fails a
run in which nothing broke and teaches everyone to re-run; the second passes
one in which something did. Every rule below follows from that one.

**What is compared exactly, and what is not.** Exact matching is kept for
machine contracts only: wire and protocol bytes and opcodes, error codes and
HTTP statuses, configuration names (`OPENCHIME_*`), the version line, JSON
field names, and the hashes of committed data. **No C test matches a person's
words** — a page's sentence, an error's text, an e-mail's or an invitation's
wording, a prompt — not even where the words are part of a contract: text that
is registered or legal is checked through the constant that holds it, or its
hash, in one place. Everything else is asserted as an outcome (the account
exists, the token works, the message arrived), as parsed structure (JSON
through jsmn, a page through its markers), or as a stable identifier. Where a
test needs to know a state the code only said in words, the code gains a
**stable machine marker** and the test reads that:

- The daemon's pages (`daemon/webpages.h`) name their screen on
  `<main data-oc-page="…">` (`signin`, `security-on`, `device-confirm`, …),
  report an outcome as `data-oc-result="…"` (`password-changed`,
  `password-set`, `approved`, `denied`, `step-on`, `step-off`,
  `passkey-added`) and a refusal's kind as `data-oc-error="…"` on its message
  (`pw-mismatch`, `pw-short`, `bad-credentials`, `code-wrong`, `rate-limited`,
  … — `oc_page_err`, one code per kind, its sentence beside it in one table).
  The static pages carry `data-oc-page="invalid"` and `"unavailable"` on
  `<body>`. `tests/testsignin.h` reads them: `oc_test_page_is`,
  `oc_test_page_result`, `oc_test_page_error`, `oc_test_page_any_error`.
- The password policy (`daemon/pwpolicy.h`) returns a verdict
  (`OC_PW_TOO_SHORT`, `OC_PW_COMMON`, …), not a sentence;
  `oc_pw_verdict_text` words it.
- The client's loopback page (`client/core/signin.h`) carries
  `data-oc-result="signed-in"`, `"refused"` or `"pending"`.
- The image's entrypoint says what its boot did as `entrypoint: <key>=<value>`
  lines (§3.2), and the workflows read those.

A literal that is the test's own data — given to the code and looked for again
— is an identity, not the code's words, and is compared exactly. A machine
contract whose shape is not obvious says so, on the line or the line before:
`/* contract: <why> */` (an HTTP header, a signature, a decoded claim).

**Waiting.** A test waits for an outcome it can observe — a callback's
counter, a model's field, a stat — until a deadline on `CLOCK_MONOTONIC`, and
never sleeps a fixed time before asserting, nor counts loop iterations as a
measure of time. `tests/check.h` holds the one way to wait and is the one file
in `tests/` that sleeps:

- `WAIT_UNTIL(cond, ms)` polls `cond` until it holds or `WAIT_MS(ms)` pass,
  and is whether it held; `WAIT_UNTIL_DO(step, cond, ms)` runs `step` (tick a
  client, pump a relay) before each look.
- `oc_test_deadline(ms)` and `oc_test_pause(deadline)` are the pieces, for a
  loop with more to do than one condition; `oc_test_mono_ms()` measures.
- Elapsed time that is itself the behaviour under test — a window that must
  close, a timestamp that must move on — is the code's clock moved by the test
  (§2.3), never real time sat out.
- A fake that models a slow peer on purpose says so:
  `/* slow-on-purpose: <why> */`.

**Negatives.** Every check that something did not happen is paired with a
positive control that shows the check could have seen it: a fence that must
come after the thing ruled out (§2.3), or the positive half asserted first
(the three jobs were taken in and are counted as waiting; then none came
back). An absence on its own passes as readily when the code is broken.

**Speed and time.** No verdict depends on how fast code runs or on real time
passing: a loaded machine, or the sanitizer, changes nothing a test asserts.
A limit on speed is asserted as the mechanism behind it (the work was
batched, the cache was hit, no blob operation ran on the loop's thread, a
message was delivered while a backend read was held); a window, a moment or a
rate is asserted on a clock the test injects and moves (§2.3), or by an
explicit sweep through it, a step at a time, each waited on.

**Isolation.** Each test has its own data, paths and ports (§2.3, §2.4):
`tests/testenv.h`'s `oc_test_mkdtemp` and `oc_test_rmtree` for files,
`oc_test_free_port` or a port-0 bind for sockets, `oc_test_tree_path` for the
tree's own files, found from the test binary rather than the working
directory. A test that changes a process-wide knob (a call grace, a dev limit)
restores it, and a test that fails does not stop the ones after it from running
unless what failed was their setup.

**Skips.** A check that cannot run here — a tool not installed, no IPv6 —
calls `SKIP("why")` (`tests/check.h`): it prints a `SKIP` line and is counted,
and `build/tests` prints the total at the end. **Where `CI` is set, a skip is a
failure**: the runner is meant to have everything, and a check that silently
stops running is a check that passes while what it covered regresses.

**Enforced.** `scripts/test/fast/tests_lint.sh` runs with the other source checks
before the suites (in both suites) and fails on: a sleep call in
`tests/` outside `tests/check.h` (or a `slow-on-purpose` line); a comparison
(`strstr`, `strcmp`, `memcmp`, …) whose literal argument has a space and
lowercase words, unless it is the test's own data or marked
`/* contract: … */`; a `"/tmp/` literal in `tests/`; any of the removed
helpers that judged speed or let real time pass, or the variable that switched
them off, named anywhere in `tests/` (the lint lists them); and, in the workflows,
`| grep -q` in a step under `pipefail` — `grep -q` exits at its first match,
the writer dies on the closed pipe, and pipefail turns the match into a
failure, so the output is captured first (`grep -q … <<<"$out"`, or a file).
`tests/manual/`, which a person runs by hand, is not held to it.

### 2.4 Which suites run where

Each suite's row in `SUITES[]` (`tests/main.c`) says where it runs (§2.1).
The fast suite runs the suites marked `FAST` one after another, in one process
(`scripts/test/fast/suites.sh` sets `OC_TEST_JOBS=1`; "One build at a time",
§1). It ends by printing each suite's time, slowest first; the run takes the
sum of them, about a minute for the fast list. The suites marked `FULL` (the
network loop, the client core, the database writer and media, chosen by those
times, and the voice model's parity, which renders whole sentences) run in the
full suite only; a suite that pushes the fast suite well
past its time (§1) is marked `FULL`. `tests/main.c` can still run suites side
by side, each in a process of its own (`OC_TEST_JOBS=N`), for a person hunting
something by hand; neither suite does.

In one process, a suite puts back whatever it changes there -- the
environment, a test knob, a signal handler -- because the next suite inherits
it: a suite that clears the settings it loaded, the mode the run was given
among them, fails a check in a later suite that has nothing to do with it.
And suites must not share anything outside the process: each
suite's files are in its own `oc_test_mkdtemp` directory, and a suite that
listens takes its port from the kernel — it binds port 0 and reads the port
back (`getsockname`; for the in-process daemon, `oc_netloop_run_ex` on port 0
and the port its `oc_netloop_view` reports), or, where the listener is not its
own to bind, takes one from `oc_test_free_port` and retries on `EADDRINUSE` —
never a number derived from a process id or a fixed range, which another
suite or another run can be holding.

Password hashing in the tests is at test strength: every test writer is given
2 MiB of Argon2id memory (`oc_dbwriter_set_pw_mem`), including the check an
unknown name gets and a new password's key, where production uses 19 MiB.
Passwords a test sets on the pages must pass the policy (AUTH.md §2): fifteen
characters or more, not common, not built on the username.

The full suite runs every suite once: the suites that start no thread
plainly, one after another as the fast suite does (`PART=plain`), and then, on the
same machine, the suites marked `THREADS` under ThreadSanitizer
(`PART=threads`):

- the suites whose code runs threads — the loop and its I/O threads, the
  writer and readers, the worker pools, the client, the recorder and player —
  under ThreadSanitizer (address randomization off, `tests/tsan.supp` for the
  one test-only reconfiguration), in one process, one suite after another,
  nothing beside it (`scripts/test/full/tsan.sh`). No test sits out a set time
  under the sanitizer: a test whose subject is a timer firing or a window
  closing returns at once there (`TIMER_ONLY()`, `tests/check.h`) and runs in
  the ordinary build, and a long loop runs a tenth of its turns there
  (`TSAN_FEW`). `WAIT_MS` stretches deadlines only, never a timer a test sets.
  The pass takes about 30 seconds in CI and prints its time; time never fails
  it. A suite that starts a thread is marked
  `THREADS`, and that is checked at run time, not read from the source: the
  plain test binary is linked with `-Wl,--wrap=pthread_create`, and a suite
  that starts a thread and is not marked fails, naming itself and the table
  (`tests/main.c`), in either suite. The sanitizer run judges races, and no
  verdict anywhere depends on speed (§2.3a, "Speed and time"), so the
  sanitizer's slowing, five to fifteen times and uneven, changes none. A
  timeout that must fire, checked with a wide margin, is not a speed limit and
  stays a `CHECK`. And a test must not read an order the
  code does not promise — a count, a status or a flag taken to mean something
  happened before or after another thread's step — since the sanitizer's
  slowing is exactly what brings the other order out.
- every suite that starts no thread plainly. A suite with no thread cannot
  race, so instrumenting it would only make it slower. A threaded suite is not
  run plainly as well: its checks are judged under the sanitizer, and the
  timer tests of a `FAST` one by the fast suite, before every push. The one
  end-to-end walks -- netloop, client_core, dbwriter, summary and media -- are
  marked `WALK`: they run in the plain pass only, with every timer test
  judged, and not under the sanitizer, whose run of them cost
  minutes. Their threads' parts are tested under it by the suites of those
  threads (ioloop, authpool, xferpool, the workers).

---

## 3. Black-box integration

### 3.1 A C test client that reuses `protocol.c`

Integration tests drive the daemon with a small in-repo **C test client that
links the same `shared/protocol.c` the daemon uses** — not a re-implementation of
the wire format in another language. This is a deliberate choice: a second
protocol implementation (e.g. a Python harness) would be free to silently drift
from the C encoder, so a bug that affects both in the same way would pass. One
source of truth for the frames means the integration tests also dogfood the
codec the real client will ship.

The test client lives under `tests/` (`tests/e2e_client.c`, the black-box client
built as `build/e2e_client` and driven by the full suite,
`scripts/test/full/e2e.sh`). It speaks
`HELLO`/`WELCOME`, `AUTH_CHALLENGE`/`AUTH`/`AUTH_OK`, and
`SEND`/`SEND_ACK`/`BROADCAST` — the auth-and-message vertical, and nothing
further. `CLIENT_ACK`, backfill, version rejection and session revocation are
exercised in the **in-process** integration suites (`itest_netloop`), not by this
client.
The same script then registers a push token with `build/demo_client`
(`tests/demo_client.c`) and requires its acknowledgement, which the daemon sends
after whatever it pushes once a person signs in; then it sends a message and
replies to it in a thread, and requires the reply's acknowledgement too.

**Client-side TLS.** The wire protocol runs over TLS, verified as ARCH-10 says
(REQ-180); there is no plaintext fallback. The TLS library is mbedTLS (ARCH-51,
[TLS.md](./TLS.md)), used by both the daemon and the test client;
`tests/itest_tls.c` exercises the handshake and the fingerprint check, and
`test_client_core` the client's judgement of a certificate at this machine's LAN
address — a root's, the person's trust, a change, a wrong name, and the probe —
and a connected client across its daemon's certificate re-issued with a new key
under the same name, on a box restarted with it: a full handshake, no session
resumed, and back in with nobody asked, whatever it had pinned before.

**Local sign-in in the browser** (AUTH.md §8.10) is proven at every layer, and
it is also how every suite signs in: `tests/testsignin.h` plays the browser --
the form POSTed to the daemon's page over TLS, the token read off the 303 --
and the client presents that token, so a suite's sign-in is a person's. The
same helper links into the e2e, demo and bench clients.
`test_jwt`: a token the daemon's issuer mints verifies against its key, name and
audience, and not with a byte changed, for another audience, from another key or
past its time; a `sub` names a user only as `local|<id>`. `itest_netloop`
(`test_web_signin`, a daemon of its own): the pages and their headers, escaping,
the setup token signing up the owner, a sign-in's token presented on `AUTH`
once and only with its verifier, a forged token, `Origin` and a non-loopback
redirect refused, a password change, a removed member, `AUTH{local}` and the
retired opcodes refused, and the account limiter in front of the page. `test_signin`: the tunnel against a
fake daemon — the pages only, its own `Host` only, `Host`, `Origin` and a
`Location` rewritten to the daemon's origin, a certificate other than the
accepted one refused, and an answer slower than the budget for reaching the
daemon waited for — but not past the answer's own budget. `test_client_core` (`test_local_browser`): the whole client
both ways in — **directly**, at a daemon a test root vouches for, with a browser
that verifies the root and the name; and **through the tunnel**, at a
self-signed daemon on loopback — sign-up with the setup token, the password
page, a wrong password leaving it waiting, cancel, and a session kept by
Remember me. Mutation proofs cover the verifier, `Origin`, direct-versus-tunnel both ways,
the tunnel's certificate and `Host` checks, the page's limiter and the subject.

**A device code** (AUTH.md §8.11): `test_protocol`
round-trips its four frames. `test_devicecodes` drives the table alone — pending,
slow down, collected once, denied once, gone, a code typed in lower case or
without its dash, the per-source cap, and expiry. `itest_netloop`
(`test_device_signin`, a daemon of its own): the code's shape, `PENDING` and
`SLOW_DOWN` with the interval growing, the `/device` page naming the requester's
address, a wrong password and then approval, the token collected once and good
only with the verifier, a refusal, the user code refused as a device code, the
cap, the lookup limiter (429) and — on a second loop with a short life — expiry.
`test_client_core` (`test_device_client`): the whole client at a self-signed
daemon (the fingerprint shown) and at a test-root daemon (none, and its own
name), approval signing it in, cancel and expiry, and an approval slower than a
connection is given to sign in (a short `unauthed_ms`) signing in on the
connection that polled, with nothing said meanwhile. `test_tkqr`: the QR code's size,
quiet zone and finder pattern as half blocks. Mutation proofs cover the slow
down, expiry, the refusal, the lookup limiter, the per-source cap, polling by the
user code, single collection and the challenge binding.

### 3.2 Runner: a natively-run daemon in CI

Integration tests run **against the daemon binary the full suite just
built**, started directly on the runner and driven by `build/e2e_client`
(`scripts/test/full/e2e.sh`, in `make test-all PART=plain`).

**The container image is exercised by `make test-all PART=image`, not
here.** The release-candidate's `image` job builds it with buildah (`make
build-all PART=image`) and, before pushing it, starts the amd64 image with
podman twice over (`scripts/test/full/image.sh` and `db_restore.sh`):

- **Plain:** no bucket and no key, so no replicator; `/healthz` answers.
- **Replicated (ARCH-3, ARCH-39):** a bucket served as S3 on the runner by a pinned
  `rclone serve s3`, a process rather than a container (ARCH-38). A box given
  the deployment's six required `replica-*` files under `/etc/openchime`
  (CONFIG.md) starts on an empty bucket, and its database and its identity
  reach the bucket under `<prefix>/db/` and `<identity prefix>/db/`, and
  nothing outside them; after it is stopped, a box given the same files on an
  empty volume restores both before the daemon starts and presents the first
  box's certificate — the identity (ARCH-66b) came back; a box given a data
  prefix never used beside the same identity prefix begins its data again and
  presents that certificate on an empty database; a restore asked for to a time
  before any copy stops the boot rather than starting empty; a box given the
  files with a wrong secret exits non-zero at the restore without creating a
  database; a box given the files without `replica-identity-prefix`, or
  without `replica-endpoint`, stops as misconfigured; and a box whose database
  a newer daemon wrote stops with status 4 before Litestream starts. What the boot did is read from
  `entrypoint.sh`'s stable tokens, one per line (`entrypoint: replica=on`,
  `replica=misconfigured`, `restore=`/`identity-restore=` `empty`, `ok` or
  `failed`, `data=too-new`), with exit status 1, 3 and 4 for the refusals,
  never from its sentences; the outcomes are checked as well — replica files in
  the bucket, `cert.pem` byte-identical, no database after a refused boot.

What it cannot show: the arm64 image is build-only, and no real S3 provider is
reached (the S3 backend's real-provider round-trip is ARCH-70's). The daemon's own
behaviour is covered by the assertions below and by the in-process suites.

**By hand.** The end-to-end run is the full suite's, and `make test-all`
runs only in CI; `scripts/test/full/e2e.sh` is the whole of it. `make
build/e2e_client` builds the driver, so you can point it at a daemon you
started yourself with the script's environment, which bootstraps the users it
signs in as:

```sh
OPENCHIME_MODE=development OPENCHIME_DATA_DIR=$(mktemp -d) \
  OPENCHIME_DEV_USERS='e2e-alice:e2e-pw:owner,e2e-bob:e2e-pw:member' ./openchimed
build/e2e_client 127.0.0.1 8443
```

### 3.3 Scenarios

Every scenario below runs in the full suite, and the in-process ones in the
fast suite too unless their suite is marked `FULL`. **Which level runs it is
the part that matters**, because the two levels reach different things: the
black-box run drives the daemon over a real socket from a separate process,
and the in-process suites reach states a black-box client cannot drive.

Note the scope of the claim: the black-box run proves that the built *binary*
works. The shipped *image* is the full suite's image part to prove (§3.2).

**Black-box, against a natively-run daemon (`scripts/test/full/e2e.sh`) — two
checks:**

- **Liveness:** `/healthz` returns `200 OK` (ARCH-25).
- **The auth-and-message vertical over TLS:** `build/e2e_client` handshakes,
  signs in on the daemon's page as a person does and presents its token on
  `AUTH`, sends, and observes its own `SEND_ACK` and the `BROADCAST` (REQ-023,
  REQ-090/092).

**In-process, over the real epoll server and TLS (`itest_netloop`,
`itest_tls`, `itest_slow_blob`, `test_client_core`; the first and the last in
the full suite only):**

- **Handshake/versioning:** a client advertising an unsupported range gets a
  `REJECT` with the right `VERSION_TOO_OLD`/`VERSION_TOO_NEW` code and a closed
  connection (REQ-110/111).
- **Auth (AUTH.md):** *local accounts* — signed in on the daemon's page,
  whose token reaches `AUTH_OK`; a wrong password is refused on the page and
  counted by the account limiter; a password sent in an `AUTH` frame is refused
  with `AUTH_SOURCE_UNAVAILABLE` and the connection closed; *OIDC mode* — an ES256 JWT signed by a **test issuer** keypair
  (standing in for the central service) reaches `AUTH_OK`, and a bad-signature /
  wrong-algorithm / wrong-audience / expired token is rejected; *session* — a
  reconnect with a stored session token resumes without re-auth, and a revoked
  session is refused; with three live connections of one user, a password
  change by frame closes the other two and keeps the changer's (which a fresh
  sign-in marks as its own), one on the password page closes every one, signing
  out this device leaves the others open, and signing out everywhere closes
  them all (REQ-023, REQ-100, REQ-182).
- **Messaging:** two clients in a channel — a `SEND` from one produces a
  `SEND_ACK` to the sender and a `BROADCAST` to both; ordering within the
  channel matches send order (REQ-092).
- **The first owner (`test_dbwriter`, `test_setup_invite`):** the setup token
  makes the owner once; only the newest one works, and none once an owner can
  sign in; a removed owner neither satisfies the last-owner guard nor stops a
  new token when every owner is removed (REQ-024, REQ-030).
- **Someone joins (`itest_netloop` `test_admin_vertical`):** with the owner
  connected, an invitation redeemed on a second connection reaches the owner as
  an unasked `USER_LIST` of exactly the new member, before anything else.
- **What the browser is told (`test_signin`, `test_client_core`
  `test_browser_signin`):** the listener holds the callback until it is
  answered, and answers once; the tab says "You are signed in" only after the
  daemon has accepted the token, and for a person the join rules refuse, "You
  are not signed in" with the client's words for why.
- **Removing a member (`test_dbwriter`, `test_remove_user_integrations`):** the
  member's webhook posts nothing, even turned back on, and their push device
  tokens are gone (REQ-033, REQ-170).
- **Push device tokens (`test_protocol`, `test_push`, `itest_netloop`):** a token
  too long, with a quote or a NUL is refused on the wire and by the writer, and a
  stored string that is no token is left out of the push body rather than written
  into its JSON (ARCH-85).
- **Before a sign-in (`test_dbwriter`, `itest_netloop` `test_unauthed_closed`):**
  a name with no account is held on the auth pool like a wrong password; a
  refused `AUTH` closes its connection; a connection that never says `HELLO`, or
  never signs in, is closed once its time is up, and a signed-in one is kept
  (REQ-191).
- **Where signed requests go (`test_url`, `test_push`, `test_enroll`):** plain
  `http` to anything but loopback stops the boot for `OPENCHIME_ENROLL_URL` and
  `OPENCHIME_PUSH_URL`, and is refused by the push emitter and the enrollment
  client before any connection is made — 127.0.0.2, which reaches the host but
  is no loopback name, included (ARCH-84, ARCH-85).
- **Refusals in the client's words (`test_protocol` `test_error_texts`,
  `test_client_core`):** every `OC_ERR_*` the header defines has words, read from
  the header so a new code without them fails; a refused role change reaches
  `last_error` as its code's words, not the server's text.
- **Idempotency:** re-sending with the same token after a simulated drop yields
  the same `message_id` and no duplicate row (REQ-093).
- **A password change on the page (`test_dbwriter` `test_change_password`):** a
  wrong current password is audited as a failed sign-in and counts with the
  sign-in limiter, which then stops both the change and a sign-in (REQ-191).
- **Owner by identity (`test_joinrules`, `test_dbwriter` `test_oidc_join_rules`):**
  `subject:` admits that exact `<issuer>|<subject>` as owner with no verified
  address, and no other subject, issuer or letter case.
- **Never downward (`test_dbwriter` `test_oidc_no_downgrade`):** an emailed
  code for a Google user's address is refused with `AUTH_USE_PROVIDER` and makes
  no identity; Microsoft for a person known by emailed code links; with
  the writer's email-link switch on (which the daemon never sets) the emailed code links too.
- **The relay's published keys (`test_relaykeys`; `test_dbwriter`
  `test_relay_published_keys`):** a JWKS yields its P-256 signing keys as PEM and
  passes over another curve or type, a key for encryption or another algorithm,
  and a point off the curve; at most eight are taken and none is cut short. A
  published key is trusted beside the pinned one, the next set retires it, and the
  pinned key survives every set. The worker asks again when the relay is down and
  hands the writer what it answers.
- **Direct connections (`test_idtoken`; `test_dbwriter`
  `test_direct_identities`; `itest_netloop` `test_direct_signin`):** ID tokens
  signed in the test with RSA, RSA-PSS and P-256 keys pass, and each of a wrong
  issuer, audience, `azp`, nonce or key, an expired or future token, `alg=none`
  and HS256 is refused. A direct identity never links by address either way, the
  relay may not deliver a direct issuer, and a relay identity does not link onto a
  direct one. Against a fake provider in the test: the source is offered by its
  label, the authorize URL carries the daemon's own PKCE challenge, the code
  exchanges into a member; a wrong `state`, someone else's verifier, or a `state`
  used once already is refused with no request reaching the token endpoint; a
  person no rule admits is refused after the exchange; a provider that is down
  makes the source unavailable.
- **A second step (`test_totp`; `itest_netloop` `test_web_signin`,
  `test_device_signin`):** RFC 6238's SHA1 vectors, one step either side and no
  replay; base32; a sealed secret opens with its key and account only; the
  factor key is made 0600 and a file that is not one is refused. Through the
  pages: a right password on an account with TOTP shows the step page, a wrong
  code shows it again, the right one ends the sign-in, and neither the ticket nor
  the code is good twice; a recovery code, typed any way, works once; a sign-in
  waiting at its step when the password changes is refused; the password page
  changes nothing until the step; five wrong codes end the ticket; no factor key
  refuses the step; a device approval waits for its code (AUTH.md §8.6).
- **The identity apart from the data (`test_migrate`, `test_dbwriter`):**
  migration 0064 moves the identity's rows into its own database and drops them
  from the data; a fresh database beside a kept identity takes the kept one;
  the factor key is made once and kept in the identity, taken in from an older
  daemon's file (which goes), and not made anew while sealed second steps are
  kept; a database a newer daemon wrote is refused, before anything is written.
- **Setting the step up (`itest_netloop` `test_web_signin`):** the page takes the
  password, then shows a key and a QR code; a wrong first code is refused, the
  right one turns the step on and shows ten recovery codes once, with a download
  link holding the same codes and a copy button whose script matches the hash
  the page names (`test_client_core` fetches it through the tunnel); a sign-in then
  asks for a code and takes a recovery code; a code turns it off and leaves no
  secret or code behind; `required` refuses an account with no step, `off` asks
  for none and closes the page.
- **Resetting a password (`test_dbwriter` `test_reset_credential`, `itest_netloop`
  `test_reset_frame` and `test_web_signin`):** a member resets nobody and an
  admin no owner; the link sets a new password once, within its day, signs the
  account out everywhere and clears its second step when asked; over the wire an
  owner gets a 64-hex token and a member `FORBIDDEN`; the page refuses a token of
  the wrong shape, a mismatch, and a spent reset; opened with a sign-in's
  redirect it carries it and, the password set, goes on to that sign-in, and a
  redirect not a sign-in's is no link. A spent reset, opened from its link or
  from a code typed into a client, is called a reset and never a link. `test_client_core`: the reset text names
  the workspace and the code, and a link only where given; a reset sign-in opens
  the reset page through the client's tunnel.
- **Bringing a member back (`test_dbwriter` `test_enable_user`, `itest_netloop`
  `test_reset_frame`):** only a removed member, only by whoever may remove them,
  not at the seat cap; with a reset with which they sign in again; over the
  wire, removed then back, the owner gets the link.
- **Sessions (`test_dbwriter` `test_session_policy`, `itest_netloop`
  `test_revoke_one_session`):** a session lives the configured days; one unused
  past the idle limit is refused and deleted at its next use, one marked in use
  goes on; one device signed out on its own closes its connection and leaves
  the asker's, another user's session id changes nothing; a connection in use
  has its session's last_seen_ms written once the interval is up, an idle one
  does not (REQ-181, REQ-182).
- **An account's own address (`test_dbwriter` `test_set_email`):** set
  lower-cased, cleared, one address only; a provider's sign-in with the same
  address is somebody else; a provider's account keeps its provider's address.
- **Passkeys (`test_webauthn`; `itest_netloop` `test_web_signin`):** the CBOR
  reader refuses truncation at every byte, indefinite lengths, reserved forms
  and nesting past its bound; registration and assertion against an in-process
  authenticator, refused for the wrong type, challenge, origin, relying party,
  a missing user-presence flag, a changed signature or data, another key, a
  counter that did not rise, and a field named twice. Through the pages, on a
  trusted name: a code allows adding one, the step page then offers it and it
  signs in once; the script is served with the hash the page names; off the
  trusted name none is offered and no script runs; turning the step off removes
  them.
- **Reconnect/backfill:** a client that disconnects, misses messages, then
  reconnects and issues `BACKFILL_REQUEST` receives exactly the missed messages
  and a `BACKFILL_DONE` (REQ-100/101).
- **Slow-backend isolation:** a download crawling through a deliberately slow S3
  endpoint does not stall message round-trips (ARCH-69, §5
  "Slow-backend isolation").
- **The HTTP stack over the wire** (`test_http_stack`, ARCH-25/32): on a loop
  with a health port, `/healthz` answers `OK` and every other path the landing
  page, in plaintext; on the TLS port an HTTP client gets `404` and `405` where
  nothing is served; a client that stalls on either port is answered `408`; the
  health port counts against the same per-address cap as the TLS port; and once
  the loop stops nothing answers the health port. The webhook post itself is
  `test_webhook_vertical`, unchanged.

### 3.4 The web client

The browser build (WEB.md) is built by the full suite (`make build-all
PART=plain`, which fetches Emscripten itself), and checked there for what a
build can show: `scripts/test/full/web_deps.sh`, that its objects follow their
headers (touching a header makes `make -n web` recompile every web object whose
dependency file names it, and no other), since a struct that grows would
otherwise leave the client mixing two layouts, which shows only as random traps
at run time.

The WebSocket transport itself is covered natively, in `itest_netloop`'s HTTP
stack scenario: the upgrade's accept value and echoed subprotocol, HELLO's
answer in frames, a ping's pong, and an unmasked frame ending the connection.

---

## 4. Continuous integration

**Two suites, and every test runs through one of them** (§1). Each test is a
self-contained script under `scripts/test/fast/` or `scripts/test/full/`, shared
helpers in `scripts/test/lib/`; the make targets build what the scripts need
and call them (`mk/tests.mk`, `mk/full.mk`), and the workflows call only the
targets. Nothing runs a test on a pull request: `pr-policy.yml` checks
policy. The full suite runs in `release-candidate.yml`, once per
release-candidate, the code tests through `test-all.yml`; the workflows and
what each job covers are CONTRIBUTING.md "CI".

Nothing is checked twice: each suite runs once, plainly or, if it starts
threads, under the sanitizer, which judges races and not speed (§2.4);
the release compiles with the same gcc as the fast build, so there is no
second release-compiler pass, and the daemon's release compile is `stage`'s
alone; and the end-to-end run (§3.2) is the one thing the in-process suites cannot
show, since they link the daemon's code without its `main()`.

The tests run on `ubuntu-24.04`; the release builds on `ubuntu-22.04`
(`stage`), whose glibc 2.35 is the release binary's floor. The native build,
the tests use the runner's gcc, the release Ubuntu 22.04's gcc 12 and the
arm64 release its gcc 12 cross compiler, installed from the runner's packages; Emscripten and llvm-mingw are
fetched pinned by make (VENDORS.md §2, "Toolchains"). The Linux GUI's
system development packages, and the headers SDL3's Linux build needs, come
from the runner's packages too, installed by `test-all.yml` beside the
tools it lacks (§2.2c). Everything runs non-interactively and
communicates pass/fail purely through exit codes.

**Audio.** `tests/test_audio.c` covers the **call relay** (`daemon/relay.c`),
driving it directly as the event loop does — forwarding, call isolation, the
address binding, revoke, the keepalive echo, a full call's fan-out and the
silence sweep. Echo cancellation is measured by the ERLE harness in
`tests/test_voice.c` (AUDIO.md §6.4): a synthetic room
impulse response over a far-end signal, near-end speech mixed in, clock drift
injected by resampling one side, and ERLE in dB asserted.

**Speech.** `scripts/test/full/speech.sh` renders a sentence with read-aloud
and requires `openchimed --stt-hear` to hear its words, so each speech feature
checks the other by content, in the full suite: on the full-feature build
(`PART=plain`), and on the stripped release binary and the installed `.deb`
(`PART=packages`); the speech suites (`tts_worker`, `stt`, `ttskit`) run in
both suites with their stub engines, and `voice_parity` holds read-aloud's
voice model to its reference renderings in the full suite (§2.2, "Read-aloud's
voice").

A new suite is its declaration and one row in `SUITES[]` in `tests/main.c`
(§2.1): `FAST` unless it is too slow for the fast suite's budget, and
`THREADS` if its code starts a thread (either suite fails it otherwise, §2.4)
-- `TIMED` instead if it is also `FULL` and has a timer test (`TIMER_ONLY`), `WALK` if it
is an end-to-end walk too slow for the sanitizer pass's 30 seconds.

---

## 5. Capacity benchmark (REQ-210/211)

Measured answers to "how much memory does the daemon use, and how many
concurrent connections does it hold" — measured, not assumed. Reproduced by
`tools/bench.sh`, which starts a daemon, drives it with the
`tests/bench_load.c` load client and samples the daemon's resident memory
(`/proc/<pid>/status` `VmRSS`) while the load runs.

**Environment.** Localhost, single box, one daemon process (single-threaded net
loop + one DB-writer thread), mbedTLS, glibc. These are therefore an *upper
bound on latency* and a *lower bound on capacity per unit RAM*: a real
deployment on dedicated hardware with clients over a network sees no worse
memory behavior, and network latency dominates the sub-millisecond local
scheduling costs measured here.

### How to run

```
tools/bench.sh                 # default: idle memory at 50, 100, 200 conns + latency
tools/bench.sh 50 100 200 400  # custom connection counts
```

It bootstraps a set of local accounts (auth is a 19 MiB Argon2id, so it
keeps the count modest), starts a throwaway daemon, and prints a table of
connections vs. peak RSS vs. round-trip latency, tearing everything down after.

### Results

Measured on a 12-core WSL2 host, the fast build's daemon, two runs (`tools/bench.sh
50 100 200`); figures vary run to run and by machine, so re-run it on the
machine in question:

| Metric | Value |
|--------|-------|
| Baseline daemon RSS (0 connections) | **~4 MB** |
| Peak RSS, 50 / 100 / 200 idle connections | ~50 / ~56 / ~64–67 MB |
| Added RSS per idle connection, 50 → 200 | **~90–110 KB** |
| Connection *setup* throughput | **~25–110 logins/sec**, by machine — not measured by the script: two auth threads at 18–70 ms an Argon2id (see bottleneck below) |

The jump from the baseline to the first figure is mostly sign-in, not
connections: each password sign-in holds a 19 MiB Argon2id buffer on the
two-thread auth pool, and the memory stays mapped after the burst. The script's
`kb_per_conn` column divides that jump by the connection count, so it reads high
at small counts; the slope between counts is the per-connection cost (mbedTLS
session buffers, the frame reassembler and the output buffer). The script's
latency line reports `sends=0`: its measurement does not run; the loop's
turn and client latency under load are below.

### What this means for capacity

Working from ~45 MB after a sign-in burst + ~100 KB per idle connection:

- **By memory:** `(256 − 45) MB ÷ 100 KB ≈ 2,000 idle connections` before RAM
  is the constraint in the lean profile (REQ-210). `make run LEAN=1` runs the
  development daemon under that ceiling locally — 256 MB, no swap, one CPU, in
  a systemd user scope ([DEVELOPMENT.md](./DEVELOPMENT.md), "Local development environment") — an
  approximation of a small box, not a copy of one.
- **The fd cap:** `OC_NETLOOP_MAX_FD = 4096` (a compile-time constant in
  `daemon/netloop.c`) caps the daemon at ~4,000 connections whatever the RAM;
  that many idle connections need about 450 MB, so on a 256 MB box memory binds
  first.
- **Planning number:** budget **~1,000 concurrent connections** on a 256 MB box,
  reserving headroom for SQLite's page cache, attachment transfer buffers, and
  the per-connection output buffers that grow (up to a 1 MiB cap) while a
  connection drains a broadcast burst — the ~100 KB figure is measured on
  *idle* connections.

REQ-211 asks for a low-hundreds count of concurrent connections, enough for a
workspace of 50–100 people; a 256 MB box clears it several times over. At that
scale (~100–200 connections, under 70 MB in the runs above) the daemon is
nowhere near any limit.

Scaling further needs no architecture change (REQ-210): raise
`OC_NETLOOP_MAX_FD` and move to the ~512 MB standard profile.

### The one bottleneck: connection setup

Steady-state connection *count* is cheap; connection *establishment* is not. Each
login runs an **Argon2id** password verification at OWASP's floor (19 MiB, two
passes, one lane: `OC_PW_MEM_KIB` and its neighbours in `daemon/auth.h`), 18 ms
on a 12-core development machine and about 70 ms on a small box, on the
**two-thread auth pool** (ARCH-5): about 110 logins a second at the one end, 25
at the other.

This is a deliberate security cost paid once per login, not a memory or
steady-state limit: a box holding thousands of connections is fine, but a
**thundering-herd reconnect** (e.g. every client re-authenticating at once after
a daemon restart) drains at 25 to 110 a second: on a small box, two minutes for 3,000 clients.
It is a modest cost at REQ-211's low-hundreds scale, and session-token
reconnect (ARCH-58) skips the hash entirely, which is what a real reconnect
storm uses.

### Caveats

- Idle-connection memory; active fan-out uses more (bounded by the 1 MiB
  per-connection output cap and recoverable via reconnect + backfill).
- Localhost measurement — excludes real network RTT (which would dominate the
  low-single-millisecond local p50) and NIC/kernel socket-buffer memory under
  real load.
- These are point-in-time measurements, not a periodic large-scale soak test.

### Slow-backend isolation (ARCH-69)

`itest_slow_blob` (in `make test`) answers the question the transfer pool exists
for: **does a slow blob backend stall message delivery?** Every other test runs
against the local filesystem, where a blob operation completes in microseconds —
so without this one, the ~100 ms/op regime the pool exists for would go
unexercised.

The test points the daemon's S3 backend at a loopback endpoint that dribbles a
download 16 KB at a time, each piece held until the test releases it, with the
blob cache in front of it given no room (`oc_netloop_set_blob_cache_floor`), so
every write and read goes to the slow store itself. In each of four rounds the
endpoint holds a piece of one client's download -- the daemon's read of it is
stalled for as long as the test likes -- and another client sends a message,
which must be delivered while the piece is still held; then the test releases
it.

```
  bob: 4 message(s) delivered, each while a backend read was held; 3 segment(s) served during the rounds
  blob operations on the loop thread during the transfer: 0
```

**The claim is checked by count, not by timing.** The loop marks its own thread
while it serves, and the blob store counts every operation that runs on a marked
thread (`oc_blobstore_loop_ops`). The test requires that count not to move while
the slow transfer streams: zero blob operations on the loop, whatever the
runner's load. A single blob call added to the loop's download path makes it
count dozens and fail. The effect is checked by construction, not by timing:
a stall in a blob read on the loop's thread would hold the loop until the test
released the piece, so the message sent meanwhile would not arrive -- no
round-trip is timed, and nothing depends on the runner's speed.

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
  other client's sends makes the two barely overlap, and the check becomes a
  no-op that passes either way.

The same endpoint has a **gate**, which proves cancelling a post of files
(REQ-140) by construction rather than by timing. While it is shut, a PUT is read
whole and not answered, and the daemon's commit waits on that answer — so an
upload that has sent its last byte cannot finish, however long the test takes.
Cancelling it then is cancelling a running upload; a post queued behind it
cannot start, since it holds the connection's one transfer slot, so cancelling
that is cancelling a queued one. Neither posts, and the post queued after both
does once the gate opens. Making the cancel a no-op fails the run.

### Maintenance-pass overhead (ARCH-78)

Running the storage maintenance pass every 200 ms — 1500× more often than the
5-minute default — against the same load shows no measurable change in daemon
RSS, per-connection memory or round-trip latency.

### The event loop's turns (ARCH-22)

Measured with the daemon in process — writer, readers, auth pool, the event
loop with its I/O threads and call relay — driven by real TLS clients on
loopback through six loads: **chat** (100 connected, one posting 5/s),
**fanout** (100 connected, ten posting 2/s to all of them), **storm** (100 clients
connecting and signing in at once), **backfill** (20 clients each replaying 500
messages), **call** (ten on the relay at 50 audio packets/s, one sharing a screen
at ~300/s, beside 40 chatting) and **call-tcp** (the same with five of the ten on
the connection transport). For each, the loop's turn percentiles
(`oc_netloop_stats`) beside what the clients measured. The harness is not in
the tree; a new measurement drives the same loads.

Measured on a 12-core WSL2 host, three runs of each, before the loop work and
after it (a turn is the work between one `epoll_wait` returning and the next):

| Load | Worst turn p99, before | Worst turn max, before | p99, after | max, after |
|---|---|---|---|---|
| chat | 6–16 ms | 18–43 ms | 0.1 ms | 0.8–1.8 ms |
| fanout | 3–5 ms | 15–25 ms | 0.1 ms | 0.5–1.8 ms |
| storm | 20–65 ms | 69–123 ms | 0.1 ms | 0.2–2.5 ms |
| backfill | 4–20 ms | 4–20 ms | 0.6–0.9 ms | 0.8–1.0 ms |
| call (relay now in the loop) | 4–7 ms | 4–7 ms | 1.3–1.5 ms | 2.1–2.6 ms |
| call-tcp (five of ten by the connection) | — | — | 0.5 ms | 1.2–1.4 ms |

The turn is short now because handshakes and encryption are on the I/O threads,
nothing is found by scanning the connection table, and a backfill is replayed a
slice at a time; the storm's worst turn was a hundred handshakes on the loop.

**Call media.** Relayed in the loop, audio's forwarding latency is the same as
it was from the separate process: p99 0.9–1.4 ms, against 1.3–2.0 ms before, and
video's 0.5–0.9 ms. With five of ten on the connection transport it is 1.2 ms.

**Memory.** The I/O threads and the auth pool add about a megabyte to the idle
daemon, fixed whatever the load (current figures under "Results" above). In a
build with read-aloud or voice input, measure with `OPENCHIME_TTS=off
OPENCHIME_STT=off`: the voice model's memory drowns every other figure.

**Client latency is the noisy measure on this host.** SEND→SEND_ACK and
SEND→BROADCAST are 4–7 ms at p50 throughout, but their p99 swings from 10 ms to
several hundred between identical runs while the host is loaded — and swung the
same way for the build before the I/O threads, run alternately with the one after.
The loop's own turn is the figure to compare.

### Harness conventions worth knowing

- `bench_load`'s read timeout is 180 s, so the Argon2id auth ramp
  (about 25 logins a second) is never the limit, and a short timeout would count
  slow-but-fine clients as connection failures.
- `tools/bench.sh` prints the whole result line, `connections_ok=` included —
  a failed connection reports `rtt 0.00`, so hiding the count makes a degenerate
  run read like an outstanding result.
- The memory table reports **requested vs connected** and divides by the
  connections that actually established.

### Speech models' memory

Read-aloud and voice input each load a model on first use and release it after
five idle minutes (`OC_TTS_IDLE_SECS`, `OC_STT_IDLE_SECS`). While one is loaded it
is most of the daemon's memory. Measured on a 256 MB Fly machine (about 210 MB
usable once the kernel takes its share), on the full build's daemon, sampling
`VmRSS` every 0.2 s; summaries are off by default and were not loaded:

| What the daemon did | Peak RSS | Free on the machine at the worst | Time |
|---|---|---|---|
| Idle | 23 MB | | |
| Read aloud a 138-character message | 145 MB | 33 MB | 12.7 s |
| Read aloud three full 120-character pieces | 168 MB | 23 MB | 100 s for 20 s of speech |
| Heard a 5 s segment | 147 MB | 22 MB | 1.1 s |
| Heard a 20 s segment, in one piece | 156 MB | 3 MB | 40 s |
| Heard a 30 s segment, in one piece | 162 MB | 3 MB | 413 s |

These rows were measured when both models ran on their previous engine. They now run in plain
C: in ttskit one render peaks at about 156 MB resident on a laptop core, about 86 MB of
it the model's mapped pages (READ-ALOUD.md §4), and in sttkit hearing a 6 s segment
peaks at about 53 MB, 40 MB of it the recognizer's mapped pages (VOICE-INPUT.md §6);
neither yet measured on the 256 MB machine. Both models are mapped from their files and
used in place (`daemon/tts_data.c`; ttskit and sttkit read `voice.bin`'s and
`recognizer.bin`'s pages where they lie), so under pressure the kernel drops their
pages and reads them back rather than killing the daemon: memory stays under the
machine's, and the time goes instead. Nothing was killed in these runs. The
recognizer's working memory grows with what it hears at once, which is why the
long segments left nothing free; the daemon now hears a segment longer than
`OC_STT_WINDOW_SECS` (6 s) in windows of at most that, each cut at the quietest
moment within `OC_STT_CUT_SEARCH_MS` of its limit (`daemon/stt_worker.c`), so a
30 s segment holds no more at once than a 6 s one; the windowed figures are
not yet measured on the 256 MB machine. Read-aloud already renders a message in
pieces of at most 120 characters (`OC_TTS_SEGMENT_CHARS`).

---

## 6. Running the daemon by hand

`make run` starts a development daemon on `127.0.0.1:8443`, loopback only (health
`8080`, data `/tmp/openchime-dev`, users alice/pw and bob/pw).

The federation wires — enrollment (ARCH-84), push (ARCH-85) and the OIDC relay
(ARCH-56/57) — are proven in-process by `test_enroll`, `test_push` and
`test_jwt`, against fixtures of the central side.

### TUI runtime smoke (the interactive client)

`tools/demo/tui.sh` runtime-verifies the **TUI** (ARCH-75) against a live daemon — the
headless check the project lacked (it was only ever build-verified). It drives the real
interactive client in a **tmux** pane: connect and sign in as alice (the TUI's device-code sign-in, which a person approves while the script waits), send a message from
the composer, receive a broadcast from bob (via `demo_client`), and assert both render in
the transcript. Requires `tmux`. To connect the TUI by hand instead:

```sh
make build/openchime-tui
build/openchime-tui 127.0.0.1 8443             # <host> <port>: a device-code sign-in, or the browser
```

### Notes / gotchas

- **`OPENCHIME_DATA_DIR`** must be writable, or creatable. Outside the container the
  default `/data` is neither, and the daemon exits before `netloop: listening`.
  `tools/demo/tui.sh` sets it to a temp dir.
- `demo_client` (`make build/demo_client`) is the flexible black-box tool:
  `demo_client <host> <port> <user> <pass> token <apns|fcm> <tok> | send <channel> <text> | reply <channel> <parent> <text>`
  against a running daemon (its usage lists the rest).

## A death always leaves evidence

The client writes a crash report and a minidump from an unhandled-exception
filter (stacks, threads, modules and handles only — no data segments and no
memory the stacks point at, which hold session tokens and message text) — but a filter only runs if the process gets to run code on the way out,
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
call both raise a real exception, so they produce the ordinary report
instead of vanishing.

## Symbolicating a Win32 crash

`make build` ships a **stripped** `build/openchime.exe` and keeps the
symbols beside it in `build/openchime.debug`, linked by a `.gnu_debuglink`. The
crash report the client writes carries a module-relative `rva=`, which stripping
does not move, so a crash is resolved without the shipped binary having symbols
in it:

```
scripts/release/crash_resolve.sh 0xRVA        # or the crash-<pid>.txt itself
```

which reads the ImageBase with the toolchain's `objdump -p` and resolves the
address with its `addr2line` (llvm-symbolizer, which reads PE + DWARF; both in
`third_party/llvm-mingw-*/bin`).

Keep the `.debug` file for any binary handed to somebody else; without it a
minidump from that build resolves to nothing.
