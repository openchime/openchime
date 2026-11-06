# OpenChime — Testing Strategy

How OpenChime is tested, and the conventions any new test follows. Recorded as
a decision in [ARCHITECTURE.md](./ARCHITECTURE.md) (ARCH-48).

## 1. The two suites

Every test is defined in one place and belongs to one of two suites: the
fast one, which the full one includes, or the full one alone. Each suite is a
pair of make targets, and each is the gate for one step of the release
process (CONTRIBUTING.md):

| Suite | Commands | Runs | Gate for |
|---|---|---|---|
| **fast** | `make build`, `make test` | locally, before every push; the pre-push hook refuses a tree `make test` has not passed on | a push, and so a pull request |
| **full** | `make build-all`, `make test-all` | in CI, once per release-candidate, on the staging tip it cuts (`release-candidate.yml`) | the `release-N-rcK` tag |

**The fast suite has a budget of two minutes**, `make build` and `make test`
from `make clean` on a developer's machine; a suite that pushes it past them
moves to the full suite. **The full suite is a superset of the fast one.** No
test runs on a pull request (`pr-policy.yml` runs policy checks: the title,
the attribution scan, the issue references), in a release or in a promotion.
`make build` and `make test` refuse to run in CI, and `make build-all` and
`make test-all` anywhere else (`CI` unset).

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
  component has its licence notice; the web harness's driver; and what a
  release lists, closes and sends as shipped;
- actionlint on the workflows (`scripts/test/fast/actionlint.sh`, fetched
  pinned on first use);
- the suites `tests/main.c` marks `FAST`, side by side (§2.4), timing judged;
- on a pass, the tree it passed on, recorded for the pre-push hook.

**The full suite** (`make test-all`, `scripts/test/full/`), on what `make
build-all` builds: everything a release ships. `PART=` splits both across
CI's parallel jobs (`mk/full.mk`):

- `plain` — the daemon with every feature and the web client in it, the
  Linux TUI, the e2e client and the fuzzing harnesses; then the fast suite's
  source checks and actionlint; every suite, the `FULL` ones included, plainly
  with timing judged; the static-link check; the end-to-end run against a
  natively-run daemon (§3.2); every Linux source through the release's
  compiler under `-Werror`; the speech round trip; the web client in a
  headless Chrome, its objects' header dependencies and the GUI smoke on the
  web (§3.4); and each fuzzing harness for `FUZZ_SECONDS` (§2.2b);
- `threads` — the suites marked `THREADS`, under ThreadSanitizer (§2.4);
- `packages` — the release's daemon for each architecture (zig, glibc 2.34)
  and its `.deb`, `.rpm` and tarball; then the glibc floor, one binary in all
  three, speech on the release binary, the `.deb` installed and speaking, and
  its unit file;
- `image` — the image laid from that tree; then it starts, and its database
  replicates and restores (§3.2);
- `windows` — the Windows binaries, the installer and the MSIX; then their
  version resources, the installer and the MSIX.

The code tests (`plain`, `threads`) run in `test-all.yml`, which
`release-candidate.yml` calls: they start the daemon in development mode,
which no release-path workflow may (`scripts/test/fast/release_lint.sh`). The
rest runs in `release-candidate.yml` itself, on the artifacts it then stores
and publishes. A check on a binary or an image the runner cannot execute (the
arm64 ones on an x86 runner) says so and skips.

**After a release publishes**, `release.yml` verifies what it published
(`scripts/release/smoke.sh`, every check in `scripts/release/smoke/`: the apt
and dnf repositories install it, each index lists it once more, the released
image pulls and runs). That is release verification, not a suite: it checks a
publication, not a change.

**Manual harnesses** live in `tools/` and are run by hand: the GUI and web
harnesses (`tools/gui/`, `tools/web/`), the TUI demo (`tools/demo/`), the
capacity benchmark (`tools/bench.sh`) and the toolchain check
(`tools/toolchains.sh`). One of them is also the full suite's: the GUI smoke,
`tools/gui/smoke.sh`, which `scripts/test/full/gui_web_smoke.sh` runs on the
web; and the fast suite checks the web driver's process handling
(`tools/web/drive.sh`, `scripts/test/fast/web_driver.sh`).

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
(`cc`), `make`, `curl` and `bzip2` (the pinned mbedTLS tarball), `nasm`
(libvpx's assembly), `xz` (the pinned llvm-mingw for the Windows clients) and
`python3`. The vendored libraries and the toolchain build themselves on first
use (`scripts/build/*.sh`, `mk/toolchains.mk`); no ONNX Runtime, llama.cpp or
Emscripten is needed. actionlint is fetched once into
`~/.cache/openchime/`. Four things are checked only where present, and a
missing one is a `SKIP` (a failure where `CI` is set): `ffprobe` (package
`ffmpeg`, the media suite's recording check, a full-suite suite), an IPv6
loopback (`::1`), a non-loopback LAN address (`test_client_core`'s
certificate-trust checks), and a kernel that leaves a connection attempt
pending (`test_sock`'s connect bound, skipped when the kernel answers every
SYN). `gdb` is optional and makes a crash print every thread's stack.

**`make build-all` and `make test-all`** run in CI only and refuse anywhere
else. What they need is installed by the workflows: `cmake` 3.28+ and a C++17
compiler for the speech and summary builds, `setarch` for the
ThreadSanitizer part, a clang with libFuzzer for the fuzzing harnesses,
`ffmpeg`, Chrome and Node for the web client, and for the release's artifacts
`podman`, `buildah`, `rpm`, `cpio`, `nsis`, `python3-pefile`, and clang and
the pinned CMake `scripts/build/makemsix.sh` fetches.

**The web harness by hand** (`tools/web/*`,
`tools/gui/run.sh --web`): `git` (make fetches Emscripten, about 400 MB, and
its Node), Chrome (`CHROME=/path`, else the installed `google-chrome` on
`PATH`; nothing is downloaded), `npm` to install `playwright-core` from
`package.json` (the scripts do it when it is missing), `openssl` and `python3`.
`tools/gui/run.sh` builds nothing by default (`OC_TEST_NO_BUILD=1`): build
the web client and a daemon that serves it (`make openchimed WEB=1`) first.

**The Win32 GUI harness** (`tools/gui/*.sh`): WSL2 on a Windows host with an
interactive desktop session (`powershell.exe` and `/mnt/c` reachable); `make
build` builds the client. `screenrec.sh` also needs `ffmpeg`, and `voice.sh`
a daemon built with `TTS=1 STT=1`.

**TUI script** (`tools/demo/tui.sh`): `tmux` and `curl`.

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
  (`e2e_client`, `demo_client`, `bench_load`, `testsignin_main`), plus the
  daemon and app-core objects it links (`mk/tests.mk`), each source compiled
  to its own object under the object tree.

- **`SUITES[]` in `tests/main.c` is the one list of the suites and of where
  each runs**: a row per suite, `S(run_<subject>_tests, FAST|FULL,
  PLAIN|THREADS)`. `FAST` runs in both suites, `FULL` in the full suite only
  (one too slow for the fast suite's budget); `THREADS` marks a suite whose
  code starts threads, which the full suite also runs under ThreadSanitizer
  (§2.4). The scripts never keep a list of their own: they ask the binary,
  `build/tests --list fast`, `--list full` (every suite) or `--list threads`.

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
  keys. `tools/gui/pdm.sh` (on Win32, and in the browser with
  `tools/gui/run.sh --web pdm`) drives two GUI clients: New message with
  "End-to-end encrypted" ticked, bob accepting on his device, the same safety number
  and no file, Send later or Files tab in the conversation, what alice wrote in New
  message arriving once it is open and bob's answer from the normal composer reaching
  her, ended and put away, and a second one declined.
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
  voted between two clients through the real daemon. `tools/gui/polls.sh`
  drives the card in both GUI clients (below).
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
  `oc_workspace_takes_suffix` — the hosted suffix beside a bare name or nothing
  typed, never beside an address, a port, a dotted name or `localhost` — hold to
  their tables (`tools/gui/web_signin.sh` checks the Win32 field shows it for
  `acme` and not for the daemon's address); a
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

Unset, nothing changes. Run directly (`./build/tests`, where `OC_TEST_JOBS`
defaults to 1) or with `OC_TEST_JOBS=1` (§2.4), a repeated suite runs **in one process**, so a suite that
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
none where unset. The full suite's pass is given both by make
(`scripts/test/full/fuzz.sh`). A new harness is its file and a word in that
list, and a line for its sources in `mk/tests.mk`.

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
tree. `-max_total_time=<s>` stops a run. The full suite runs each harness for
`FUZZ_SECONDS` (60 by default) from its corpus (`make test-all PART=plain`,
`scripts/test/full/fuzz.sh`). A finding fails the job, with the sanitizer's
report and the input, base64, in the log.

**The corpora** under `tests/fuzz/corpus/<harness>/` are real frames:
`build/fuzz/seeds <dir>` writes one per encoder in `shared/protocol.c` (each
over a zeroed message) and a few streams of them for `framebuf`. They are kept
whole rather than minimized with `-merge=1`, which would keep about one in five
and lose the frame for most message types. `tests/fuzz/frame.dict` holds the
protocol version and every message type id. Regenerate both when the protocol
changes.

**A crash.** Reproduce it with `build/fuzz/<harness> <file>`, and minimize it
with `build/fuzz/<harness> -minimize_crash=1 -runs=10000 <file>`. Check the
minimized input in under `tests/fuzz/crashes/<harness>/`:
`run_fuzz_replay_tests` (`tests/test_fuzz_replay.c`) runs every corpus and
reproducer file through its harness in both suites, so the crash fails there
until it is fixed, and stays a regression test afterwards. A missing
`crashes/` directory counts as no files.

### 2.3 Determinism rules

Unit tests must be reproducible and independent of wall-clock or environment:

- Any use of `rand()` seeds it explicitly (e.g. `srand(12345)`).
- No test asserts an exact wall-clock timestamp. Tests assert *relative* facts —
  ids strictly increase, a timestamp falls inside a window the test itself
  bounded — never a literal `now()`. **There is no injectable clock or id
  source**: `dbw_now_ms` calls `clock_gettime` directly with no override seam, so
  determinism here comes from asserting relative facts rather than from
  scripting time. What *is* injectable is the interval a periodic job runs at
  (`oc_dbwriter_set_idem_retention`, `oc_netloop_set_relay_silence_ms`,
  `oc_netloop_set_presence_rate_ms`, `OPENCHIME_DEV_CALL_TIMERS`,
  `OPENCHIME_DEV_VIDEO_CAP_MS`), which is what lets a suite compress a clock it
  cannot fake.
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
  and `itest_slow_blob` each open a database, because they exercise
  migration-on-boot, WAL behaviour and multi-connection paths that an in-memory
  database does not reproduce. Each suite's files live in **a directory of its
  own**, made under `$TMPDIR` by `oc_test_mkdtemp` (`tests/testenv.h`) and
  removed at its end — never a fixed name under `build/` or `/tmp/`, which two
  runs of the same checkout, or two checkouts, would share. The
  purely-logical suites (codec, framebuf, mention, richtext, searchq) touch
  nothing.

### 2.3a Brittle tests

**A test is brittle if its result can change without a change in behaviour a
person, an operator or an integrator could see — or if it can stay green while
that behaviour regresses.** Both are defects in the test: the first fails a
run in which nothing broke and teaches everyone to re-run; the second passes
one in which something did. Every rule below follows from that one.

**What is compared exactly, and what is not.** Exact matching is kept for
machine contracts only: wire and protocol bytes and opcodes, error codes and
HTTP statuses, configuration names (`OPENCHIME_*`), the version line, JSON
field names, and the hashes of committed data. **No C test matches a person's
words** (the GUI harnesses still read a few `si_err` texts; a marker would
replace them) — a page's sentence, an error's text, an e-mail's or an invitation's
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
- `oc_test_let_time_pass(ms)` is for when elapsed time is itself the behaviour
  under test — a silence that must outlast a window, a timestamp that must move
  on — never a stand-in for waiting on work.
- A fake that models a slow peer on purpose says so:
  `/* slow-on-purpose: <why> */`.

**Negatives.** Every check that something did not happen is paired with a
positive control that shows the check could have seen it: a fence that must
come after the thing ruled out (§2.3), or the positive half asserted first
(the three jobs were taken in and are counted as waiting; then none came
back). An absence on its own passes as readily when the code is broken.

**Speed.** A limit on how fast code runs is a `CHECK_SPEED` (§2.4), never a
`CHECK`; better still, the test asserts the mechanism (the work was batched,
the cache was hit) rather than the clock.

**Timing.** A verdict that races the wall clock — work that must land inside a
short real-time window, or a count that depends on where a window's edge falls
— is a `CHECK_TIMING` (`tests/check.h`), and so is every `CHECK_SPEED`. Both
suites judge them, the full one on CI's runners too; `OC_TEST_TIMING=0`
leaves them unjudged, an escape hatch for a machine too loaded to judge them,
which no suite sets. (The web client's checks have none: they wait on the
client's state, §3.4.)
The code around them still runs everywhere, ThreadSanitizer included. Each is
a debt: the fix is an injected clock, an explicit sweep or a wait on the
outcome, after which it is a `CHECK` again. A new test is written that way
from the start.

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
`/* contract: … */`; a `"/tmp/` literal in `tests/`; and, in the workflows,
`| grep -q` in a step under `pipefail` — `grep -q` exits at its first match,
the writer dies on the closed pipe, and pipefail turns the match into a
failure, so the output is captured first (`grep -q … <<<"$out"`, or a file).
`tests/manual/`, which a person runs by hand, is not held to it.

### 2.4 Which suites run where

Each suite's row in `SUITES[]` (`tests/main.c`) says where it runs (§2.1).
The fast suite runs the suites marked `FAST` side by side: as many at once as
there are CPUs, each in a process of its own (`OC_TEST_JOBS`, `tests/main.c`),
its output printed whole when it ends. It ends by printing each suite's time,
slowest first. The suites are nearly all waiting -- on timers, sockets,
real-time audio and video -- so the run takes about as long as the slowest
suite it runs. The suites marked `FULL` (the network loop, the client core,
the database writer and media, chosen by those times) run in the full suite
only; a suite that pushes the fast suite past its budget (§1) is marked
`FULL`. `OC_TEST_JOBS=1` runs the suites one after another in one process.

Side by side, suites must not share anything outside their process: each
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

The full suite runs every suite plainly, side by side as the fast suite does
(`PART=plain`), and, on a machine of its own at the same time, the suites
marked `THREADS` once more under ThreadSanitizer (`PART=threads`):

- the suites whose code runs threads — the loop and its I/O threads, the
  writer and readers, the worker pools, the client, the recorder and player —
  under ThreadSanitizer (address randomization off, `tests/tsan.supp` for the
  one test-only reconfiguration). A suite that starts a thread is marked
  `THREADS`, and that is checked at run time, not read from the source: the
  plain test binary is linked with `-Wl,--wrap=pthread_create`, and a suite
  that starts a thread and is not marked fails, naming itself and the table
  (`tests/main.c`), in either suite. The sanitizer run judges races, never
  speed: a limit on how fast code runs — wall-clock time, or one timing
  against another — is a `CHECK_SPEED` (`tests/check.h`), asserted in the
  ordinary build and not under the sanitizer, which slows code five to fifteen
  times and unevenly. A timeout that must fire, checked with a wide margin, is
  not a speed limit and stays a `CHECK`. And a test must not read an order the
  code does not promise — a count, a status or a flag taken to mean something
  happened before or after another thread's step — since the sanitizer's
  slowing is exactly what brings the other order out.
- every suite plainly, the threaded ones included, with its timing verdicts
  judged. A suite with no thread cannot race, so instrumenting it would only
  make it slower.

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
it is also how every suite signs in, and a real browser is held to it too
(`scripts/test/full/web.sh`, below): `tests/testsignin.h` plays the browser --
the form POSTed to the daemon's page over TLS, the token read off the 303 --
and the client presents that token, so a suite's sign-in is a person's. The
same helper is the `signin` command (`build/signin`) and links into the e2e,
demo and bench clients; the GUI harness presses the card's Sign in and plays
the browser from the URL the client publishes under its hook (`sign_in_card`
and `play_browser`, `tools/gui/lib.sh`), and the web client, its own browser,
types for the harness with `OPENCHIME_DEV_SIGNIN=user:password` (development mode).
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
`tools/gui/web_signin.sh` drives it on Windows: the Win32 client, with no
credentials, against a self-signed daemon at the WSL address —
certificate trusted, the sign-in link offered to copy while the browser is
awaited and copied by the card's button, sign-up through the tunnel with the setup token, password
changed on its page, and, the workspace forgotten, the old password refused and
the new one signing in; then, forgotten again, the card checked to say nothing
under a heading that already names the workspace, a reset's code entered on the
card, the reset page opened through the client's own tunnel, the password set
and the sign-in it goes on to signing in; PowerShell plays the browser from
`signin_url.txt`. The invitation and the reset code go through the card's real
dialogs — opened by a click on the link (`clickform`, acked before the dialog's
nested loop starts), typed into (`formtype`) and answered with Enter or Esc —
and the card is checked to stay the view under each and after it, since the
attempt a dialog starts is watched only while the card is on screen.

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

### 3.4 The web client (the full suite)

The browser build (WEB.md) is checked by the full suite, which runs
`scripts/test/full/web.sh` and the GUI smoke on the web platform
(`scripts/test/full/gui_web_smoke.sh`) in the runner's Chrome, as part of
`make test-all PART=plain` (§1; make fetches Emscripten itself). Every check
in it waits on the client's state, never a schedule, so all of them are
judged there as by hand. The daemon it starts runs in development mode,
and so serves its own `/app/` page that way: the check signs in by password on
the daemon's page and bootstraps its user.
`scripts/test/full/web.sh` runs the first two, as its two parts:

- **client** — a daemon on a fresh database with one bootstrapped
  owner, and a headless Chromium with a fresh profile loading the client from
  the daemon's own `/app/` page (`openchime.html`, the Windows application
  compiled for the browser); it signs in on the daemon's page and comes back,
  the page keeping the test's instructions for the tab, as it keeps `?hook=`;
  the page polls its test hook until the model reads `authed=1 connected=1`,
  and logs that line, which the test checks; the browser logs are left beside
  the daemon log.

  Then a video message is recorded through the client's own `vm` verbs with
  the synthetic camera and the browser's fake microphone, each verb taken once
  the last has happened (the page's `?script=`, `verb~pattern`: its ack, then
  the dump polled until the state comes; a sign-in that fails ends the run at
  once): the dump must show the file taken (`take_bytes`) and the console that the audio ran on the
  worklet (`oc-web: audio worklet`, not the fallback), and the client's
  `sttest` verb must pass: the text layer's right-to-left order, cluster
  hit-testing and line breaking through the API the views use; and the
  client's `about` verb must answer both builds on one line (`OpenChime client
  <build> (web), server <build>, protocol <n>`, the daemon's from
  `SERVER_BUILD`). Last, a Japanese
  composition is played into the page's hidden input for the input method
  (`?ime=1`), the page polling for it composing (`comp=2`) and then for the
  composer holding its result (`len=2`). Each of these is reported on a line
  of its own.
- **pages** — the daemon's sign-in pages from a real browser.
  A headless Chromium (`scripts/test/lib/web/pages.mjs`, Playwright) types into the
  forms and submits them: the first owner signed up with the setup token, a
  wrong password refused on the page, the right one signing in, the password
  changed on its page, the old one refused and the new one signing in; each
  sign-in's 303 lands on a loopback callback the script serves, as the
  client's own listener is, and the token read off that address is presented
  on `AUTH` by `build/signin present`. What the browser itself sends -- its
  `Origin` under the pages' referrer policy, its handling of the redirect -- is
  what this proves, and what no scripted post can. Then an invitation link
  (REQ-026): the owner mints an invite anyone may use (`build/demo_client
  ... invite 0`) and a second browser opens `/app/?invite=<hex>` cold
  (`scripts/test/lib/web/invite.mjs`), lands on the sign-up form before the client has
  been fetched, chooses a name and a password and comes back signed in to the
  web client, which its own dump says; the tab is closed and the client opened
  again in a new one, and it is still signed in. Last, the client's load (`scripts/test/lib/web/load.mjs`, WEB.md "Loading"),
  cold and then warm in one browser that trusts the daemon's certificate by
  its key (a browser caches nothing from one it was told to overlook): the load
  screen counts the download in bytes against its size, shows Downloading,
  Starting and Opening in that order and comes down once the client has
  drawn; the wasm and the loader arrive gzipped; and the second load brings
  the page, the loader and the wasm under 2 KB each, every one a `304`. Needs
  the web client beside the daemon (`make build-all PART=plain` puts it there)
  and `openssl`. It runs after
  the client part.
- **`tools/gui/run.sh --web all`** — the Windows feature scripts against the browser
  ("Driving the web client" below): the smoke, chords, voice input, screen recording,
  uploads, files, the two call scripts, the search boxes, encrypted DMs (`pdm`),
  polls and tables, each on its own
  fixture daemon, each printing its `gui_<name>:` result line. The checks are the
  Windows scripts' own; what is the Windows platform's alone (UI Automation,
  its windows, the clipboard set from PowerShell) is skipped there.

The WebSocket transport itself is covered natively, in `itest_netloop`'s HTTP
stack scenario: the upgrade's accept value and echoed subprotocol, HELLO's
answer in frames, a ping's pong, and an unmasked frame ending the connection.

---

## 4. Continuous integration

**Two suites, and every test runs through one of them** (§1). Each test is a
standalone script under `scripts/test/fast/` or `scripts/test/full/`, shared
helpers in `scripts/test/lib/`; the make targets build what the scripts need
and call them (`mk/tests.mk`, `mk/full.mk`), and the workflows call only the
targets. Nothing runs a test on a pull request: `pr-policy.yml` checks
policy. The full suite runs in `release-candidate.yml`, once per
release-candidate, the code tests through `test-all.yml`; the workflows and
what each job covers are CONTRIBUTING.md "CI".

Nothing is checked twice within a suite: each suite runs once plainly and the
threaded ones once more under the sanitizer, which judges races and not speed;
one clang compile is both the release-compiler and the second-compiler check,
and the end-to-end run (§3.2) is the one thing the in-process suites cannot
show, since they link the daemon's code without its `main()`. Of the GUI
scripts only the smoke runs in a suite, the full one, on the web platform
(§3.4); the longer web scripts (uploads, voice, calls, …) take minutes each
and are run by hand, and so is `chords`.

All run on `ubuntu-24.04`, the image the release builds on too. The native
build and the tests use the runner's gcc; zig (the release's clang), Emscripten
and llvm-mingw are fetched pinned by make (VENDORS.md §2, "Toolchains"), never
installed from the runner's packages. Everything runs non-interactively and
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
both suites with their stub engines.

A new suite is its declaration and one row in `SUITES[]` in `tests/main.c`
(§2.1): `FAST` unless it is too slow for the fast suite's budget, and
`THREADS` if its code starts a thread (either suite fails it otherwise, §2.4).

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
  a systemd user scope (README, "Local development environment") — an
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
download 16 KB at a time with a 120 ms delay between pieces, with the blob
cache in front of it given no room (`oc_netloop_set_blob_cache_floor`), so
every write and read goes to the slow store itself; it then measures one
client's message round-trips while another client's download crawls through it.

```
  bob round-trip: idle median 57ms | during slow-download median 52ms, max 62ms
  backend 120 ms/op, 3 slow segment(s) served DURING the measurement (= 360 ms of backend stall the loop did not absorb)
  blob operations on the loop thread during the transfer: 0
```

**The claim is checked by count, not by timing.** The loop marks its own thread
while it serves, and the blob store counts every operation that runs on a marked
thread (`oc_blobstore_loop_ops`). The test requires that count not to move while
the slow transfer streams: zero blob operations on the loop, whatever the
runner's load. A single blob call added to the loop's download path makes it
count dozens and fail. The round-trips are kept as a coarse check on the
**median** only (during < idle + half a backend stall). Single samples are left
alone: they are at the mercy of the runner's scheduler, and a check that allowed
one spike in nine failed a sanitizer run on CI whose median had *fallen* during
the transfer.

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
  `demo_client <host> <port> <user> <pass> token <apns|fcm> <tok> | send <channel> <text> | reply <channel> <parent> <text> | invite [uses]`
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

**Prove it rather than wait for it.** `tools/gui/drive.sh die <av|abort|badparam|fastfail|kill>`
forces one class each. `av`, `abort` and `badparam` produce a crash report;
`fastfail` and `kill` produce none, by construction, and are caught by the ring.

**The harness closes the client gracefully** (`CloseMainWindow`, falling back to
force after three seconds) for a reason that matters here: `Stop-Process -Force`
is a `TerminateProcess`, which is correctly reported as a death that ran no
handler. Force-killing on every launch would fill the test directory with
post-mortems the harness caused on purpose and bury the one that matters.

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

## Driving the web client

`tools/web/drive.sh` is `tools/gui/drive.sh` for the browser: the same
subcommands (`launch`, `devaddr`, `<verb...>`, `shot`, `dump`, `kill`), the
same verbs, the same ack contract (exit status 1 when the client did not
answer), and `tools/web/pair.sh` is `tools/gui/pair.sh` (`up`, `a`, `b`, `down`).
A feature script therefore runs against either client: every script sources
`tools/gui/lib.sh`, which picks the drivers from `OC_TEST_PLATFORM=win32|web`
(win32 unless said) and sets `PLAT` for the checks that are the Windows
platform's alone. `tools/gui/run.sh [--win32|--web] <name>... | all` runs
them by name and tallies the results, skipping on the web the scripts whose
checks are Windows-only.

**One driver per client.** Two drivers serving one command directory race for
each verb, so a run fails at random with nothing wrong in the client. `kill`
ends every driver serving the directory, found by the command line that names
it, and the browser it held. It asks first (`quit`), then sends SIGTERM, then
SIGKILL to whatever is still running 3 seconds later, and says how many it had
to force; a driver ends on SIGTERM or SIGINT by closing its browser. `launch`
runs `kill`, then refuses to start, naming the pids, if `held` still finds a
driver on the directory. `scripts/test/fast/web_driver.sh`, in `make test`,
checks both with stand-in drivers that ignore SIGTERM.

**How it drives.** `launch` builds and starts the fixture daemon as
`tools/gui/drive.sh` does (`OC_TEST_PORT`, `OC_TEST_DIR`, `OC_TEST_WS`,
`OC_TEST_USERS`; the health port off), then starts `tools/web/drive.mjs`: a
Node process holding a headless Chromium on the daemon's own `/app/` page,
signed in with the password quick-launch and the test hook on. The shell side
writes the verb to `$OC_TEST_DIR/web-<a|b>/cmd` and waits for `ack`, as it
does with the Windows client's scratch directory; the Node side relays the
verb to the page's hook, takes a `dump` or `shot` back out of the page's
filesystem into that directory, and copies an `attach`ed file onto the page
first so the verb names a path the client can read. One verb is the
browser's own: `press <chord>` (`ctrl+comma`, `ctrl+shift+slash`, `esc`)
dispatches the page's key events for the chord, every press and release in
one task, so the client receives them as one batch, the way a quick
person's chord arrives between two frames. `key` hands the client a chord
directly; `press` goes the way a keyboard does, through the page and SDL.
`tools/gui/chords.sh` (port 9700, web only) presses Ctrl+, and Ctrl+/
several times each and checks Preferences and the shortcuts open every
time: the client must read a key's modifiers from the key event, not from
the keyboard's state when it gets to the key, by which time a quick
Ctrl is already up. `press` acks once the events are dispatched, and the
script polls the dump for the state each chord leads to, as the smoke's
`wait_for` does. The browser runs with
Chromium's fake camera and microphone, its own permission prompts answered,
its screen picker choosing the screen; `OPENCHIME_DEV_AUDIO`, `_MIC`,
`_CAPTURE`, `_TONE` and the other switches reach the page as `?env=`, and a
`_MIC` WAV is served to it. Needs `node`, the `playwright-core` package
(`package.json`; `launch` installs it) and Chrome (`CHROME`, else the
installed `google-chrome`).

**What the ports skip in a browser**, by `PLAT`: UI Automation (`uia_*.ps1`),
the recording bar's own window and its capture exclusion, a window as a
recording source (the browser's picker is its own), the clipboard set from
PowerShell, and the tray's notification counters. Everything else -- the
dumps, the verbs, the media checks down to the tones in the recorded file --
is the same code.

## Driving the Win32 GUI

`tools/gui/drive.sh launch` **builds both sides and restarts the daemon if the
running one predates the binary it just built.** That is not convenience: the
client and daemon share a wire and ship together (ARCH-61), so a client built
from source N against a daemon still running N-1 decodes garbage and reports only
"connection lost — reconnecting", which points nowhere near the cause. It cost
real time three times in a single day before the guard existed.

`make` is incremental, so it is free when nothing changed. `OC_TEST_NO_BUILD=1`
skips the build and `OC_TEST_NO_DAEMON=1` leaves the daemon alone — for when a
mismatched pair is the thing under test, such as the version-reject path.

**The harness's variables** begin `OC_TEST_` (but `GUI_DRIVE_OUT`, the output
directory), and are read and defaulted in
`tools/gui/lib.sh` (the feature scripts), `tools/gui/drive.sh` and
`tools/web/drive.sh` (one client) and the two `pair.sh` (two clients); an older
prefix in the environment (`OC_DEV_`, `OC_PAIR_`, `OC_GUI_`, `OC_DRIVE_`,
`OC_WEB_`, `OC_SMOKE_`) is refused by name. The daemon and the clients they start run in
development mode (`tools/gui/lib.sh` exports `OPENCHIME_MODE=development`,
CONFIG.md "Mode"), and any build will do.

| Variable | Default | Meaning |
|---|---|---|
| `OC_TEST_PLATFORM` | `win32` | `win32` or `web`: which client the feature scripts drive (`tools/gui/run.sh --web` sets it). |
| `OC_TEST_PORT` | 8443 (one client), 9600 (a pair) | The fixture daemon's protocol port; every feature script sets its own. |
| `OC_TEST_DIR` | `/tmp/openchime-dev`, `/tmp/openchime-pair` | The fixture daemon's state directory, wiped per run. |
| `OC_TEST_WS` | `Acme HQ` | The fixture's workspace name. |
| `OC_TEST_USERS` | `alice,bob,carol` / `alice,bob` | The fixture's bootstrapped users (`name:pw:role`). |
| `OC_TEST_HOST` | the WSL machine's address / `127.0.0.1` | Where the clients reach the daemon. |
| `OC_TEST_CALL_MAX`, `OC_TEST_ADVERTISE_PORT` | 10, unset | The fixture's call size and advertised UDP port. |
| `OC_TEST_TONE_A`, `OC_TEST_TONE_B` | unset | Each paired client's synthetic microphone tone, in Hz. |
| `OC_TEST_NO_BUILD`, `OC_TEST_NO_DAEMON`, `OC_TEST_LOCAL` | 0 | Skip the build; leave the daemon alone; run the Windows client from a local copy. |
| `OC_TEST_DRIVE`, `OC_TEST_PAIR` | by platform | A driver of one's own, for a one-off. |
| `OC_TEST_WHO`, `OC_TEST_DEBUG_PORT` | `a`, unset | The web driver's client letter; Chromium's remote-debugging port. |
| `OC_TEST_HEALTH_PORT`, `OC_TEST_WEB_PORT` | 8080, 8551 | The fixture daemon's health port; the fixture daemon's port in `tools/gui/web_signin.sh` (the Win32 browser sign-in). |
| `OC_TEST_KEEP_DB`, `OC_TEST_WAIT_MS` | 0, the script's | The smoke keeps the database; a longer wait for a slow machine. |
| `GUI_DRIVE_OUT` | `/tmp/ocshot` (`/tmp/ocshot-web` in `web_signin.sh`) | Where `shot`/`dump` copies land on the Linux side. |

The suite's own share the prefix: `OC_TEST_ONLY`, `OC_TEST_EXCEPT`,
`OC_TEST_REPEAT`, `OC_TEST_JOBS` (`tests/main.c`) and `OC_TEST_TIMING`
(`tests/check.h`).

**The client is pointed at the address Windows reaches the daemon at.** Under
WSL2 that is the WSL machine's own address (eth0's), not 127.0.0.1, which reaches
a WSL listener only where localhost forwarding is on; elsewhere it is 127.0.0.1,
and `OC_TEST_HOST` overrides either. `tools/gui/drive.sh devaddr` prints the workspace
`launch` defaults to, for a script that names it. At an address that is not
loopback the dev daemon's self-signed certificate is asked about (ARCH-10), and
`launch` answers Trust and connect for it — the daemon is the one the harness
started — before it returns. It starts the client only once the daemon listens,
waiting up to ninety seconds: the bootstrap accounts are hashed first, which for
`tools/gui/members.sh`'s sixty people is half a minute, and a client whose first attempt
finds nothing there does not try again. Only the daemon on `OC_TEST_PORT` — found
by its environment — is restarted.

**Video messages drive on a synthetic camera.** Launch with
`WSLENV=OPENCHIME_DEV_CAPTURE:OPENCHIME_DEV_AUDIO OPENCHIME_DEV_CAPTURE=synthetic
OPENCHIME_DEV_AUDIO=synthetic tools/gui/drive.sh launch` (add
`OPENCHIME_DEV_VIDEO_CAP_MS` to shorten the five-minute cap, or
`OPENCHIME_DEV_CAPTURE=denied` for the blocked-camera path). The `vm` verb opens,
records, stops, sends, plays and closes by name, and `dump` reports the overlay on
its `vm=` line. The synthetic source never opens a real camera, so a run cannot
turn on the camera of the machine it happens to be on.

**A real camera needs the client on a local disk.** Windows refuses camera
activation to an executable started from the WSL share (`\\wsl.localhost\…`):
`ActivateObject` fails with `0x80070490`, element not found, and the card says
the recording could not start, with that step in brackets. `tools/gui/drive.sh launch`
runs `build/openchime.exe` from the share, which is fine for everything else;
for a camera test, copy it to a Windows directory and start it from there —
`OC_TEST_LOCAL=1` makes `launch` do exactly that.

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
in a run must be released**, which is what `tools/gui/newmsg.sh`'s `press` helper does:
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
> as `env OPENCHIME_PORT=… openchimed`, so the port never appears in the
> process's *cmdline* and `pkill -f OPENCHIME_PORT=9500` matches nothing —
> exiting 1, looking exactly like "nothing to kill". The daemon then keeps
> running on a directory that has been deleted underneath it (SQLite happily
> writes to the unlinked inode), and every upload fails with an opaque
> `transfer error`. Match `/proc/<pid>/environ`, as both harnesses do.

## Reading the startup harness

`tools/gui/startup.sh` answers the question the smoke suite deliberately
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

It is Win32-only and not in CI (the smoke's Win32 reason), and it is not the
pre-push gate — run it when a change touches how the client starts.

## Reading the New message harness

`tools/gui/newmsg.sh` asks the questions the New message pane (REQ-229)
answered wrongly for its whole life, in 32 checks on its own fixture daemon and
port (9520):

- **Does the field that looks focused get the keys?** Characters, and the keys
  that are not characters — Delete, Ctrl+A, Ctrl+V — while the To: field has
  focus. Falling through to the message body behind it would mean Ctrl+V
  pastes into a message nobody can see and Ctrl+A then Delete wipes it.
- **Does the message go where the pane says?** It posts into `#general` first, so
  a wrong send has somewhere visible to land, then addresses a message to bob and
  presses Enter. `#general`'s message count must not move and the DM's must.
- **Does what you typed survive?** Leaving and returning must bring back the words
  **and** the recipients, and the pane's text must never become the selected
  conversation's draft.
- **Does Backspace take one recipient per press?** One press takes one; the key
  held — pressed again without a release — takes no more, which is the rule that
  stops a held key walking backwards through the whole list.
- **Does a send with nobody addressed refuse, and say so?** The recipients being
  gone is asserted *before* Enter is pressed, the message must still be in the box
  afterwards, and the composer must say why (the dump's `hint` line). The harness releases Backspace between
  presses (see the `key`/`keyup` rule above); otherwise the check would run with a
  recipient still attached.

It reads the dump's `newmsg` line — `focus= chips= q= caret= sel= matches= body=
pending=` — which exists so those states can be asserted rather than described.
Both halves of the pane are in it: which field owns the keyboard, who the message
is addressed to, what the query holds and where its caret sits.

Like the other GUI harnesses it is not in CI (the daemon is Linux-only and
GitHub's Windows runners cannot host it). Run it when a change touches the pane,
the target picker, or the composer's key routing.

## Reading the GUI smoke

`tools/gui/smoke.sh` answers one question —
**does the client boot and run** — in about ten seconds across fourteen checks.
It drives the client through the test hook (`OPENCHIME_DEV_DIR`) and stands up
its own fixture daemon on its own port. It is the GUI check to run before pushing a client change (by hand; the pre-push
hook itself requires only `make test`), and the full suite runs it on the web
platform (§3.4); it is not a feature suite and does not aspire to be one.

What it checks is the whole of it, deliberately: the client launches, connects
and authenticates against the fixture; paints the Home shell (the
`sidebar_kind` / `main_is_conversation` / `window_is_covered` predicates);
takes the keyboard; types a message and gets it back from the daemon; opens and
closes the command palette from the composer; and is still answering at the
end. A new view or overlay is added to the predicates in `app.c`; it does not
get a line here.

**Assertions wait on state, never on a clock.** `expect_eventually`, `wait_grep`
and `settle` poll for the state being asserted, so a true assertion returns on
the first poll and only a real failure pays the timeout; the palette is asserted
as a whole closed→open→closed **round trip**, refusing to credit a close whose
open never happened. Tune the patience with `OC_TEST_WAIT_MS` (default 6000).

**Its failures can be believed on sight, which is a property that was built in
rather than hoped for.** It checks the **exit status of every verb**:
`tools/gui/drive.sh` exits non-zero when the client never acked, and a caller that discards that
status turns a dropped command into the *next* assertion failing for an
unrelated reason — a failure that needs interpretation before it can be
trusted.

**Its checks cannot pass by accident either.** The message it sends carries a
per-run unique string, so no assertion can be satisfied by something a previous
run left in the database; and the run ends by making the client answer one more
time, because every earlier check reads a dump file that looks identical whether
the client is alive or died on the last keystroke.

**Keeping it small is the maintenance rule.** A suite of a few hundred
assertions that runs for minutes is skipped, and so
catches nothing — the failure mode of a slow gate is not that it is slow, it is
that it stops being run. Verify a feature by driving it, through
`tools/gui/drive.sh` or a harness scoped to that feature. Do not add it here.

Its Win32 run is not in CI: the daemon is epoll-based so it is Linux-only, and
GitHub's Windows runners cannot host it (no Linux containers). CI runs it on the
web platform (`tools/gui/run.sh --web smoke`, §4).

*One caveat when reading a failure message:* the dump's `comp=` field is the
**IME composition length**, not the autocomplete popover — the dump exposes no
popover state, so `comp=0` beside a completion failure means nothing. The
fields worth reading are `error_seq` and `last_error`: without them a failed
intent and an intent that was never sent look identical. Prefer asserting on
`error_seq`, which only ever increments — `last_error` is cleared on
`OC_EV_CONNECTED`/`OC_EV_AUTH_OK`, so a reconnect between the failure and the
check can erase the evidence a wait is polling for.

## Reading the screen-recording harness

`tools/gui/screenrec.sh` records with real Windows.Graphics.Capture against its
own fixture daemon (port 9530), with a known picture in the camera box
(`OPENCHIME_DEV_CAPTURE=synthetic-camera`) and known sound
(`OPENCHIME_DEV_AUDIO=synthetic`: the microphone 440 Hz, the computer 660 Hz). It
asserts the screens and windows listed, the box in the chosen corner of the preview
(colour bars at the rectangle the dump's `vmsrc` line and `oc_inset_rect` put it),
the card stepping aside for the recording bar, the bar kept out of the capture and
read and pressed through UI Automation (`tools/gui/uia_recbar.ps1`), Stop ending the
take at the fitted size, the file the daemon stores — found in the fixture's blob
directory — being VP9 and Opus at that size with both tones in it (`ffprobe`,
`ffmpeg`), and a window closed mid-take keeping what came before.

**Keep the Remote Desktop window open while it runs.** A session that is not drawn
captures its screens as black; a window is captured either way. `tools/gui/drive.sh
launch` with `OC_TEST_LOCAL=1` runs the client from a local copy, which a real
camera needs.

## Reading the links harness

`tools/gui/links.sh` drives links in the transcript (REQ-220, MARKDOWN.md §4)
against its own fixture daemon (port 9650). It sends a labelled link and a bare
address, then asserts that hovering the label reports the real address, marked
labelled, in the dump's `link hover= labelled=` line; that clicking it opens the
confirmation naming that address (`confirm open= act= url=`) rather than the
browser, and Esc dismisses it; and that hovering the bare address reports it,
unlabelled. Nothing is ever opened: the bare link is only hovered and the
confirmation is always dismissed.

## Reading the reactions harness

`tools/gui/reactions.sh` drives the who-reacted pane (REQ-070/071) over
`tools/gui/pair.sh` (port 9580): bob reacts, so alice's pane has somebody else's
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

## Reading the polls harness

`tools/gui/polls.sh` drives polls (REQ-225) over the pair (port 9710; on Win32,
and in the browser with `tools/gui/run.sh --web polls`): alice makes and edits
them in the poll composer, and bob votes. The composer is driven by verbs that do
what its controls do -- `pollnew`, `polledit <mid>`, `polltype <q|time|0..9>
<text>`, `polladd`, `pollrm <i>`, `pollcheck <multi|anon> <0|1>`, `polldeadline
none|<YYYY-MM-DD> <time>`, `pollclose`, `pollsubmit`, `pollcancel` -- and every
control is also published to assistive technology (`pollc.*`), so a click works as
well. It asserts that a poll made with an option added, removed and added again,
and a deadline at an arbitrary local minute two days ahead, arrives on both
clients with exactly that deadline; that an edit before anyone votes changes the
question and the options for both; that bob's vote moves the count on alice's side
without marking it hers, and marks it on his; that pressing a row votes and
pressing it again takes the vote back, each row published; that "See who voted"
lists both people with what they picked; that a multiple-choice anonymous poll
says so and offers no list; that once someone has voted the composer locks the
options and settings but still moves the deadline; that "Close poll now" closes
it on both clients, after which no row takes a vote; and that no question, one
option or a deadline already past is refused at the field and posts nothing. It
reads the dump's `pollcomposer`, `pcrow`, `poll`, `pollopt` and `voters` lines.
Not in CI, for the smoke's reason.

## Reading the tables harness

`tools/gui/tables.sh` drives tables (REQ-220, MARKDOWN.md §1a) over the pair
(port 9720; on Win32, and in the browser with `tools/gui/run.sh --web tables`).
In the transcript it sends bodies with `sendraw` (which reads `\n` and `\t`) and
asserts on both clients that a table is drawn as a grid with its rows, columns,
header and alignment; that a table with no header, a cell with bold, a link,
emoji and a `<br>` (a taller row), and the text before and after a table all
render; that a wide table scrolls on its own (`tscroll`) while the message does
not; that a link in a cell asks before it opens; that the preview reads "Table:
N rows × M columns"; that a long table stops at its limit with "+N more rows";
that "Copy table" (`copytable`) puts tab-separated text on the clipboard; and
that a malformed table is text. In the composer it builds a table in the table
editor (`tablenew`, `tablecols`, `tableheader`, `tablealign`, `tablecell`,
`tableaddrow`, `tableok`), checks the composer holds one card and no pipes,
sends it between two lines of text, edits the sent message to find the card
again, reopens it (`tableedit`) filled in, changes a cell and saves, and
removes a card and takes the removal back with undo. Pasting puts a sheet's
text, its HTML or a picture of it on the clipboard (`clipset`, `cliphtml`,
`clippicture`) so no run depends on a real spreadsheet, then `paste`, and checks
the chooser offers Table, Image and Text, that Table makes a card keeping the
HTML's links and bold, Text inserts the tabs as typed, Image with no picture on
the clipboard attaches the cells drawn as a PNG (a decodable picture in the
upload tray) and with one attaches that, and Esc pastes nothing. It reads the dump's `table`, `tcell`, `composer`,
`ctable`, `tableeditor`, `tecell`, `pastechoice` and `clipboard` lines. Not in
CI, for the smoke's reason.

## Reading the shortcuts harness

`tools/gui/shortcuts.sh` drives the Win32 client's keyboard shortcuts over
`tools/gui/pair.sh` (port 9570) and asserts the EFFECT of each key, not that it
dispatched: two clients, so bob can leave something unread for alice. It
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

`tools/gui/composer.sh` drives the Win32 message box against its own fixture
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

`tools/gui/drafts.sh` deletes drafts in the Win32 client against its own fixture
daemon (port 9560): each Drafts row's Delete, published to assistive technology;
Delete taking the draft at once, asking nothing, its notice offering Undo, and
Undo putting the draft back; a delete gone from the list, the count and the
sidebar, and not written back by the composer, which still holds that
conversation's text; the row's context menu doing the same. Its notices are
dismissed before the next part, since one still up covers rows it reads. It then
checks that every context menu — a message's, a sidebar conversation's, a
member's, a draft's — keeps its row lit while it is open and only while, by
reading the row's pixels from a screenshot with the pointer moved off both the
menu and the row: hover alone could not, since a menu being open is what clears
or freezes it. It reads the dump's `draftn=`, `draftrows` (each row's
conversation, thread and Delete rect), `modal=`, `toast[n]`, `memrow` and
`a11yitem` lines. Not in CI, for the smoke's reason.

## Reading the files harness

`tools/gui/files.sh` pages the Win32 files view (REQ-143) against its own
fixture daemon (port 9590) with `OPENCHIME_DEV_FILE_PAGE=2`, so three uploads are
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

## Reading the lightbox harness

`tools/gui/lightbox.sh` drives the expanded image view against its own
fixture daemon (port 9710). It uploads three images with a text file between
them, opens one with a click, and pages with Left, Right and the next arrow,
asserting the view's place from the dump's `lightbox=` line: `lightbox=` (the
attachment on screen), `lightbox_pos=<i>/<n>` (its place in the conversation's
media; the text file is never counted), `prev=` and `next=` (whether each arrow
is drawn) and `next_r=` (the next arrow's rect). At the last image Right changes
nothing; Esc closes the view. Run on both platforms, `--web` included.

## Reading the uploads harness

`tools/gui/uploads.sh` drives the Win32 message box's upload tray (REQ-140)
against its own fixture daemon (port 9630). It attaches files through the
`attach` verb — the file dialog cannot be driven, the tray can — and asserts
that they wait as chips, an image with its picture, each chip's button published
to assistive technology, with nothing uploaded; that a chip's × takes it out;
that Send posts what is left and the text as ONE message and the tray empties;
and that a file that cannot be read stops the post, the chip coming back marked
and the text back in the box, with nothing posted — shown by the next message
being the last. Then it pastes, setting the Windows clipboard through PowerShell
the way other programs do and pressing Ctrl+V through the real key path: a copied
file becomes a chip as itself, a copied image a `pasted-image-…png` chip with its
picture that posts under that name, and text still pastes as text with no chip. It reads the dump's `ftray` line (`here=`, `all=`, `posts=`,
`height=`) and one `fchip` line per chip (`pic=`, `state=` -1 waiting for Send,
0 queued, 1 moving; `failed=`, `done=`, and the `x=` button's rect). Its last
step sends a large file so the bars can be seen moving in `uploads_moving`; that
shot asserts nothing, since when a local upload finishes is not something to
test against. The core underneath — one message, its files in order, a thread,
a failure — is proven in `test_client_core.c`, and cancelling in
`itest_slow_blob.c`. Not in CI, for the smoke's reason.

## Reading the members harness

`tools/gui/members.sh` scrolls the members pane (REQ-031) against its own
fixture daemon (port 9690) bootstrapped with sixty people — `OC_TEST_USERS`, all of
whom land in `#general` on registration — so the roster is three times what the
pane holds. It asserts that only what fits is drawn and the pane says the rest is
below it; that the wheel reaches **every** member, counted as the distinct people
drawn on the way down (sixty of sixty); that the bottom row is then somebody the
first screen never showed, and answers a click by opening that person's profile;
that the offset stops at both ends rather than running past them; that an
owner's menu on a member, which carries "Reset password and two-step sign-in…",
cuts none of its items (the dump's `menurow … cut=` says whether a label was wider
than its row as drawn); and that switching channel puts the pane back at its own
top. It reads the dump's
`members n= rows= scroll= max=` line and the `memrow uid= r=` lines, and drives
the `wheel` verb in detents of 120. Last, the owner's "Reset password…" on a
member opens the reset to send with all of it shown: the dump's `formlines` line
is the field's own wrapped line count against the lines its box shows, and the
first one visible.

**Counting the people is the check; reading the bottom row is not.** A first
version asserted that the last drawn row was a real member and that the offset
reached its maximum — both of which hold with the scroll offset removed entirely,
because the pane still draws a full first screen and the maximum is computed from
the roster either way. It passed 11/11 against a deliberately broken build. What
distinguishes the two is whether anybody past the first screen is ever drawn.

## Reading the keyboard-menu harness

`tools/gui/keymenu.sh` drives the keyboard's route to the context menu
(REQ-269) against its own fixture daemon (port 9610), with the pointer parked off
every row so nothing it asserts can be the mouse's doing. It covers Ctrl+Down and
Ctrl+Up putting the keyboard on a message and moving between them; Shift+F10
opening that message's actions with an item already highlighted; the arrows moving
the highlight and **Enter running the highlighted item**; Esc closing the menu and
Esc again taking the keyboard off the row; "Unread from here" on the middle
message moving the "New" line to it at once (REQ-235), read from the dump's
`unread_from=`; and Shift+F10 with nothing focused opening the conversation's own
menu. It reads the dump's `kbfocus mid= menuhover= menuhovercmd= subhover=
subopen=` line and `menu=`.

**Enter is proven by effect, and the effect is read back.** The harness walks to
"Copy link", presses Enter, then pastes into the composer and requires the
permalink to name the focused message — a menu that merely closed would prove
neither that the highlighted item ran nor that it ran on the right row. The
clipboard is **seeded with something else first**: Windows keeps it across runs,
so a link copied by an earlier run would satisfy that check whether or not this
run copied anything.

**Every verb's ack is checked** (`k` for keys, `snap` for the dump). The script
names a dropped verb where it happens — an unanswered `key` looks
exactly like a key that did nothing, and an unanswered `dump` leaves the previous
file on disk for everything after it to read.

## Reading the calls harness

`tools/gui/calls.sh` runs two Win32 clients in a call over `tools/gui/pair.sh`
against its own fixture daemon (port 9620): alice's synthetic microphone at
440 Hz, bob's at 660 Hz (`OPENCHIME_DEV_TONE`). The clients reach the daemon at
the WSL machine's own address rather than 127.0.0.1, because WSL forwards only TCP
there and a call's audio is UDP. It asserts the start and bob's invitation (his
Calls section and a notification), both in the call hearing each other with
every packet decrypting once the keys are in, noise suppression taking a steady
hum out, bob muted and seen muted, push to talk, a per-person volume, only the
starter ending, the missed-call line, and screen sharing — alice shares the
synthetic screen (`OPENCHIME_DEV_CAPTURE=synthetic`), bob's view shows it with
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

`tools/gui/calls_tcp.sh` is the same pair (port 9670) with UDP that goes
nowhere: the daemon advertises the relay at a port nothing answers on
(`OC_TEST_ADVERTISE_PORT`, which `tools/gui/pair.sh` passes as
`OPENCHIME_MANAGED_AUDIO_ADVERTISE_PORT`). It asserts that each client finds within
seconds that the relay does not answer, moves to the connection transport — the
dump's `call` line says `transport=tcp` — and hears the other's tone
(PROTOCOL.md §5.17, AUDIO.md §4).

`tools/gui/groups.sh` drives user groups (REQ-307–309) on a pair of its own
(port 9640, clients reaching the daemon at the WSL address; carol and u1–u9 exist without clients).
- Alice makes `@crew` in Admin → Groups through the tab's own buttons (the `grpbtn` verb presses them, `formnext` fills their forms).
- She adds carol and bob with the people picker (`grppick <query>` types into it through the real key path and presses Enter): `car` finds carol, `#gen` offers nothing and adds nobody, and one Add puts both in.
- In a new private channel, the channel menu's Add someone (`chmenu 6`) opens the members pane's people picker (the window widened so the pane is drawn); `CAR` finds carol, and Enter on an empty query invites her and closes the picker.
- She gives `@crew` to the channel from the channel menu (`chmenu 8`).
- Bob, never invited, is in the channel through the group, and a message naming `@crew` names him.
- His Leave is refused, with the daemon's reason.
- In alice's members pane, bob's Remove (hovered) says why it cannot, and he stays. The picker does not offer bob, who is in already, and Escape closes it. The group row's Remove asks first, and confirmed, takes bob out and leaves carol, who is also in directly. The pane's Add group puts the group back, and the header's Add people invites u1 and u2 in one go.
- Remove someone (`chmenu 7`) opens the roster with no mode and no picker; hovered, u1's row alone offers Remove, and it takes out u1 and nobody else.
- The pane has no rows of buttons: the header carries Add people and GROUPS its +, and resting on each shows its tooltip ("Add people", "Add a group").
- With the picker open, a click in the composer takes the keys: what is typed next is in the composer, not the picker.
- Each of these was shown to fail with its guard reverted: the roster ignoring `info_seq`, the picker offering current members, and the picker keeping the keys after a click elsewhere.
- New message's To field, which shares the picker, still takes a channel, replaces it with people, and refuses a ninth person, saying so. Opening a group's people afterwards gives an empty picker of the group's own, alice can add herself, and New message's recipients and text are still there on return.
- Taken out of the group, bob is out of the channel and told.

The dump's `groups` and `chgroups` lines carry the groups, the open channel's, whether this user is in it through one, and whether its newest message names them. `grppick` carries the picker's owner (`host` 1 a group's, 2 the members pane's), focus, whether the pane is adding (`memmode` 1), chips and matches, and the verb drives whichever of the two is open; `memgrp` the GROUPS subheading's + (`plus`), group-row Removes and the hovered row's Remove; `memppl` the header's Add people, the picker's Add and Cancel, and which icon's tooltip is up (`tip`); `memrow` names each row's person, with its Remove when shown and whether that is dimmed (`via`).

`tools/gui/pair.sh`'s command directories are per port (`ocpair-<port>-a`, `-b`), so two pairs up at once never read each other's commands.

The daemon side is in `make test`:
- `test_dbwriter` covers the rules, membership by reference, `@group`, and the invariant. The invariant is a random sequence of every membership operation checked against a reference model after each step.
- `itest_netloop` runs the same over the wire.
- `test_push` checks that a group mention pushes to its member only.
- `test_client_core` covers the client model and autocomplete.

Each guard was shown to fail with the change it guards reverted.

## Reading the search-box harness

`tools/gui/search_boxes.sh` checks the six search boxes (CLIENT.md, "The six
search boxes are one control") on a pair of its own (port 9660; only alice is
driven). At the default text size, the largest, and 200% DPI it opens each box in
turn — the sidebar's find box, search messages, Files, People, Jump to and the
emoji picker — and reads its `searchbox` line from the dump:

- `uifont=1`, and the face is Segoe UI or Segoe UI Variable Text;
- `lf`, the font height, is at least 14 DIP at that DPI (the stock dialog font
  these boxes used to wear is 11 px at every DPI);
- the EDIT is exactly `lh`, the font's line height, lies inside its chrome, and
  is centred in it to a pixel;
- and all six share one font height.

The checks were shown to fail with one box put back on the stock font, and with
the EDIT given a fixed height again.

## Reading the voice harness

`tools/gui/voice.sh` drives voice input in the Win32 client end to end against
its own fixture daemon (port 9680), with real recognition: read-aloud renders a
sentence, and `OPENCHIME_DEV_AUDIO=synthetic` with `OPENCHIME_DEV_MIC=<wav>`
makes the client's microphone speak it. It asserts push to talk by the held key,
by UI Automation and by the mouse (words in the composer, nothing sent), free talk
(the words posted, the composer untouched), that leaving the conversation and
opening the video recorder end a session, that a blocked microphone
(`OPENCHIME_DEV_AUDIO=mic-denied`) is reported, and that with `OPENCHIME_STT=0`
there are no controls at all. It reads the dump's `dictate` line — `avail=
offered= on= mode= hold= sent= answered= words= err=` and the button rects — and
its `toast[n]` lines. Not in CI, for the smoke's reason. Run it when a change
touches voice input, the composer's controls or the device layer.

## Reading the cursor harness

The pointer a detached Windows session reports is suppressed, and the harness's
hover is a synthetic mouse event, so neither says what shape the cursor took.
The client does: the test dump's `cursor kind=<arrow|ibeam|hand|none>` line, in
the signed-in dump and the signed-out one alike, is the shape it last set
(`none` before any). `tools/gui/cursor.sh` (Windows and the web) sends a
message with a link, then hovers the composer's text, the link and empty space
with `move`, and asserts `ibeam`, `hand` and `arrow` in turn.

## Seeing the whole window

`tools/gui/drive.sh shot <name>` produces one image of the entire
application: `test_shot` renders the scene through the window's own renderer
and reads it back (`gfx_readback`), on every platform the same way. Every field
is drawn now, so what the renderer holds is the whole application, cached
images included; there is no second route to composite and no control a
capture cannot see. (The native children that needed `PrintWindow` and
`WM_PRINTCLIENT` composited on top are gone with the children; that history is
in git.) Only the image *fetch* is suppressed during a capture, so driving the
harness cannot generate transfer traffic.

**A capture is a picture, and a picture is not the only evidence.** The scene
also keeps a **paint ledger** when the harness is driving — every primitive the
frame drew, in order, with its rect, its clip, its colour and a tag
(`gfx_ledger_dump`). It exists because a screenshot answers "what does this look
like" and nothing else: a label clipped to nothing, ink the colour of the
surface behind it, and two strings drawn over each other all produce a picture
that looks right to a comparison and wrong to a person.

Two rules for anything drawing text through it. A text raster carries its colour
in its pixels, so the ledger cannot read one back — the call site declares it
with `gfx_ink`, which also carries the rasterizer's transparent padding so the
recorded rect is the ink and not the bitmap. And a colour that is **content**
rather than a theme choice — an avatar disc derived from a user id, a swatch for
a colour scheme you have not picked — says so with a `content:` tag, or the
palette check reports it as a surface that will not follow the theme.

## `clipped=`: text that does not fit its own box

`chromefit` compares published rectangles, so a label whose box is the right size
and in the right place reads as clean however its glyphs actually landed — the
Notifications card at 240 DPI reported `overlaps=0 outside=0` with its section
titles cut through the middle. `clipped=` closes that: the count of strings whose
layout did not fit the rect it was drawn into, with `clip="…"` naming the first
and `clippx=` the deepest cut in DIPs. The tally belongs to the frame, reset as
each one begins.

**Whole lines, not pixels.** A line box is ascent + descent + leading, pinned per
size token (ARCH-108), and legitimately overhangs a tight row by a DIP or two
with nothing cut off; measuring that reports every second label in the client. A
line that does not fit is not a rounding question.

**Vertically always, horizontally only where nothing trims.** A format that does
not wrap is given DirectWrite's ellipsis, so running out of room ends in a "…" —
legible, deliberate, a question of layout style rather than a defect. A wrapping
format has no trimming to fall back on, so a word longer than its column is cut
mid-glyph and nothing says so. Colour emoji are exempt, as they are for the
ledger's truncation check and for the same reason: an emoji cell clips a few
pixels of trailing advance and looks perfectly right doing it.

**It has failed, which is the only evidence worth having.** Across nine views ×
four DPI settings × the text-size extremes it is silent, and names two real cuts:
the composer's `Message #general` placeholder, gone entirely at 240 DPI, and the
Activity empty state losing its last word at 192 DPI and the largest text size.
Both read `overlaps=0 outside=0`.

## Observing the tray balloon

`tools/gui/balloon.sh` answers whether the tray balloon is **drawn**, which
nothing else can: the balloon is the middle of the notification chain — WinRT
toast, else balloon, else the client's own window — and wherever toasts work it
is never reached. The run turns the toast off for its own duration (`wintoast 0`
through the test hook; nothing is saved), sends one notification through the real
chain, and reports two separate facts.

**Accepted** is read from the dump's `notify … by=` field: `2` means
`Shell_NotifyIconW` took the `NIF_INFO` update and the balloon carried the
notification. Anything else fails the run.

**Observed** is a capture of the whole desktop, because the shell draws the
balloon and a capture of our window cannot contain it. A disconnected or locked
session has no composited desktop; the capture is blank, and the run says *not
observed* (exit 2) rather than reporting a balloon that is missing. Focus Assist
hides a balloon the shell has accepted, and the picture is how that is told apart
from a balloon that is drawn. The picture is for a person to look at — what a
balloon looks like varies by Windows version, and on Windows 10 and later it is
rendered as a toast.

**It has been seen.** `by=2` with the desktop captured: the shell drew a dark
notification card with the app icon, `OpenChime`, and the message's source and
text on the two lines under it — the shape a real message produces, since the
run sends one through `notify_deliver` rather than calling the balloon directly.
A disconnected session is what the blank case is for and is not a failure of the
client: the first two runs of this script hit exactly that.
