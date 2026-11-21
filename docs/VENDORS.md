# OpenChime — Third-Party Dependencies

Every package that goes into building or running OpenChime, how it is pinned, and
its license. Kept current whenever a dependency is added, removed, or bumped.

OpenChime deliberately keeps its dependency surface small and permissive. There
are four ways a dependency enters the build; each row below says which:

| Class | How it's managed | Reproducible? |
|-------|------------------|---------------|
| **Vendored (committed)** | Source committed under `third_party/` (single headers to full release tarballs) | Yes — frozen in git |
| **Fetched at build** | Downloaded + built by a `scripts/build/*.sh` (gitignored output) | Version-pinned in the script |
| **System / OS package** | Provided by the host or base image, linked at build | Pinned by the OS, not by us |
| **Container image** | Base of the container image built from a release tarball; not used for dev or test | Per the image tag |

**License posture:** our own code is AGPL-3.0-or-later throughout
([LICENSING.md](../LICENSING.md)); every *third-party* component linked into the
shipping **daemon** and **TUI** is permissive — MIT, Apache-2.0, Public Domain,
or (for the *optional, dynamically linked* libsecret/glib) LGPL-2.1-or-later —
except Mozilla's CA roots, which are data under MPL-2.0, a file-level copyleft. No
third-party GPL/AGPL code is linked into any shipped binary (a host build's libgcc is GPL-3.0 with the GCC Runtime Library
Exception), and every one of these is compatible with the AGPL.

---

## 1. Vendored source — committed under `third_party/`

Libraries committed directly to the repo, from single headers to whole release
tarballs (libvpx, opus, speexdsp, Mbed TLS), so local and
CI builds share byte-identical sources with zero transitive dependencies
(ARCH-75). "Pinning" = the file is frozen in git; bumping means replacing it.

| Package | Version | Purpose | Used by | Source | License |
|---------|---------|---------|---------|--------|---------|
| **termbox2** | v2.5.0 | Terminal cell grid + input | tuikit (→ TUI) | https://github.com/termbox/termbox2 | MIT |
| **utf8proc** | v2.11.3 | Unicode width + grapheme segmentation (correct emoji/CJK width); NFKC normalization of passwords | tuikit (→ TUI); sdltext (→ graphical clients); the daemon (AUTH.md §2) | https://github.com/JuliaStrings/utf8proc | MIT (bundled Unicode data under the Unicode license) |
| **qrcodegen** | v1.8.0 | QR codes: the device-code sign-in's URL, for a phone's camera (AUTH.md §8.11); a second step's setup key, for an authenticator app (AUTH.md §8.6) | tuikit `tk_qr` (→ TUI); the daemon's setup page | https://github.com/nayuki/QR-Code-generator | MIT |
| **Argon2** | 20190702 (the reference implementation) | Password hashing, Argon2id (AUTH.md §2): `argon2.c`, `core.c`, `encoding.c`, `ref.c`, `thread.c`, `blake2/`, as released, compiled as one unit (`argon2_all.c`) with its own flags | Daemon | https://github.com/P-H-C/phc-winner-argon2 | CC0-1.0 or Apache-2.0 |
| **Common passwords** | SecLists, `xato-net-10-million-passwords-1000000.txt` | The passwords of 15 characters or more a new one may not be, lowercased (`third_party/pwlist/common.txt`, compiled in by `scripts/build/pw_common.py`) | Daemon | https://github.com/danielmiessler/SecLists | MIT |
| **jsmn** | commit-pinned (upstream has no release tags) | Minimal JSON tokenizer | Daemon (OIDC/webhook JSON); client app-core (`client/core/summary.c`) | https://github.com/zserge/jsmn | MIT |
| **picohttpparser** | commit-pinned | HTTP/1.1 request line and header parsing for the daemon's one HTTP stack (ARCH-32) | Daemon (`daemon/http.c`) | https://github.com/h2o/picohttpparser | MIT (or the Perl licence; we use it under MIT) |
| **miniaudio** | 0.11.25 | Audio device I/O — capture and playback over WASAPI, CoreAudio, ALSA/PulseAudio/PipeWire on the desktops (the browser's is `client/gui/platform/web/audio_web.c`, not miniaudio) | Client media library (`client/core/media/audio_ma.c`, the desktop backend under `audio_dev.c`): video messages and the audio client (AUDIO.md §3.2) | https://github.com/mackron/miniaudio | Public Domain (Unlicense) **or** MIT-0, at our choice |
| **stb_image_write**, **stb_image** | commit-pinned | JPEG encode of a video message's poster, and PNG encode in the app (stb_image_write); stb_image.h is committed but nothing compiles it | Client media library (`client/core/media/recorder.c`) and `client/gui/app/app.c` | https://github.com/nothings/stb | Public Domain **or** MIT, at our choice |
| **libfvad** | commit `532ab666` (2024-02-07; upstream has no release tags) | Voice-activity detection: where an utterance ends (the WebRTC detector as a C library on its own) | Voice input in the graphical clients, Win32 and web (`client/core/voice/fvad_unit.c`, ARCH-112); never the daemon | https://github.com/dpirch/libfvad | BSD-3-Clause |
| **Mozilla CA roots** | curl's extract, dated (see `scripts/gen/ca_roots.sh`) | The roots every outbound HTTPS client verifies against (ARCH-10): S3, central, unfurls, the client's `.well-known` fetch | Daemon + every client (`third_party/ca-roots/ca_roots.c`, generated) | https://curl.se/docs/caextract.html | MPL-2.0 (the certificate data) |
| **SQLite** (amalgamation) | 3.53.4 | The daemon's database (ARCH-2), compiled in with `SQLITE_ENABLE_FTS5`. **Not linked by any client** (ARCH-88) | Daemon + tests (`third_party/sqlite-3.53.4/sqlite3.c`) | https://sqlite.org | Public Domain |
| **speexdsp** | 1.2.1 | The acoustic echo canceller (`speex_echo_state`), behind the processor seam (AUDIO.md §3.3) | Client media library (`client/core/media/processor.c`) — voice input (ARCH-112) and the audio client; never the daemon | https://www.speex.org | BSD-3-Clause |
| **libopus** | 1.6.1 | Opus encode and decode | Client media library — video messages (ARCH-110) and the audio client (ARCH-73); the daemon, to encode read-aloud (ARCH-111) | https://opus-codec.org | BSD-3-Clause |
| **libvpx** | 1.17.0 | VP9 encode and decode | Client media library — video messages (ARCH-110) and screenshare (ARCH-87) | https://chromium.googlesource.com/webm/libvpx | BSD-3-Clause (with a separate patent grant) |
| **Mbed TLS** | 3.6.7 | TLS transport (client + daemon), certificate verification, ACME's JWS, CSRs and challenge certificates, SHA-256, ES256 verify; calls' end-to-end encryption — X25519, HKDF, AES-GCM, CTR-DRBG for HPKE and SFrame (ARCH-113) | Daemon + client (`shared/tls.c`, `shared/e2e_*.c`) | https://github.com/Mbed-TLS/mbedtls | Apache-2.0 **OR** GPL-2.0-or-later (dual; we use it under Apache-2.0) |

**speexdsp** is committed in `third_party/speexdsp/` as Xiph's 1.2.1 release
tarball, unmodified, its `COPYING` with it. `scripts/build/speexdsp.sh` builds
it out of tree, static and floating point, `native` for `make test` and
`windows` for the Win32 client, into `third_party/speexdsp-1.2.1[-win]/`
(gitignored); nothing is downloaded. The daemon links none of it.

**libopus** is committed in `third_party/opus/` as Xiph's 1.6.1 release
tarball, unmodified, its `COPYING` with it. `scripts/build/opus.sh` builds it
out of tree, static, `native` for `make test` (with `OC_ARCH`, another
architecture's) and `windows` for the Win32 client, into
`third_party/opus-1.6.1[-win]/` (gitignored); nothing is downloaded. The daemon
links it only to encode read-aloud renders (ARCH-111); it relays Opus without
decoding it (ARCH-73). BSD-3-Clause, whose binary-redistribution clause wants
the notice carried with a shipped client.

**libvpx** is committed in `third_party/libvpx/` as WebM's 1.17.0 release
tarball, unmodified, its `LICENSE` and `PATENTS` with it (the tarball was
verified against the WebM release signing key, fingerprint 8DA7 0C3B 2338 B012
BAB3 0EE5 1C1C 1A9A 2A45 D4D4). `scripts/build/libvpx.sh` builds it out of
tree, **static-only**, VP9-only and real-time-only, with no examples, tools,
docs or unit tests, `native` for `make test` (with `OC_ARCH`, another
architecture's, as `make ARCH=aarch64` asks) and `windows` for the Win32 client,
into `third_party/libvpx-1.17.0[-win]/` (gitignored); nothing is downloaded. It
needs **nasm** for its assembly; its mingw build threads through winpthreads,
which the Win32 link therefore pulls in statically. **The daemon links no
libvpx**: it stores a video message as opaque bytes (ARCH-110). BSD-3-Clause,
whose binary-redistribution clause wants the notice carried with a shipped
client.

**Mbed TLS** is committed in `third_party/mbedtls/` as the 3.6.7 release
tarball (the 3.6 LTS line), unmodified, its `LICENSE` with it (the tarball was
checked against the SHA-256 upstream publishes beside the release,
`a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6`). Upstream's
make builds in place, so each script builds a copy and the committed tree is
only read: `scripts/build/mbedtls.sh` into `third_party/mbedtls-3.6.7/` (with
`OC_ARCH`, another architecture's), `scripts/build/mbedtls_windows.sh` into
`third_party/mbedtls-3.6.7-win/` (both gitignored), and
`scripts/build/mbedtls_wasm.sh` into `build/wasm/mbedtls`; nothing is
downloaded. Built as **static** libs (`libmbedtls/libmbedx509/libmbedcrypto.a`),
the native copy with **`MBEDTLS_THREADING`** enabled — the client runs a TLS
network thread per connection and the tests drive several TLS clients
concurrently, which races without it (see docs/TLS.md). A bump replaces the tree
and the version in the three scripts, `mk/sources.mk` and `mk/windows.mk`;
`make test` refuses a tree and a build that disagree
(`scripts/test/fast/vendored.sh`).

**Why the daemon compiles SQLite in rather than linking the host's (ARCH-20; no
client links SQLite at all, ARCH-88).**
The amalgamation is committed at `third_party/sqlite-3.53.4/` and built by the
daemon's own rule, so `ldd openchimed` reports libc, libm and the loader and
nothing else.

The deciding argument is not the dependency count, it is what the dependency
carried: linking the host's SQLite meant inheriting whatever that distribution
compiled, **including whether FTS5 was in it**. `daemon/migrate.c` creates
`messages_fts` as a virtual table `USING fts5`, so a host whose SQLite lacks the
module fails at migration time, on a machine the operator controls and we do not.
Pinning the version turns a deployment risk into a build fact, and the build sets
`SQLITE_ENABLE_FTS5` explicitly.

What this costs is real and worth stating plainly: **SQLite CVE response is
ours.** For an operator who installed the `.deb` or `.rpm`, `apt upgrade` does not
fix a SQLite flaw; it takes an OpenChime release.
The same price is paid for mbedTLS and libopus, which are static, and it is why watching upstream releases is a
standing obligation rather than a nicety. The container image and the tarball
have no distribution update path either way.

`SQLITE_THREADSAFE=1` matches the system library's default, so vendoring changed
no concurrency semantics: several threads hold their own connections (the
dbwriter's writer and its three readers, push, the summary worker and the blob
sweep's reference check). The omissions — extension loading,
deprecated APIs, shared cache, double-quoted string literals — are all things the
daemon provably does not use.

**Attribution when shipping binaries.** Building from source never triggered the
vendored licences' notice requirements; distributing packages does. `openchimed`
links **mbedTLS** (Apache-2.0), **Argon2**, **jsmn**, **picohttpparser**,
utf8proc, qrcodegen, SecLists' list, SQLite and the CA roots, and libopus built
with read-aloud,
beside the models' data; every channel ships their texts in full — `/usr/share/doc/openchimed/copyright` on Debian,
`/usr/share/licenses/openchimed/copyright` on RHEL, `COPYRIGHT` in the tarball.
`packaging/licenses.sh` reads them out of the trees the build actually used, so
the notice cannot drift from what was linked, and it **fails the build** if a
licence file is missing rather than shipping a binary without one. termbox2 and
lucide are deliberately absent from that file: they are TUI and client
components. utf8proc, qrcodegen and libopus are in it: the daemon normalizes
passwords with utf8proc, draws the second-step setup page's QR code with
qrcodegen, and encodes read-aloud's audio with libopus.

Since **ARCH-83**, `tuikit/` is the in-tree toolbox wrapping termbox2 + utf8proc
(terminal layer + width handling), which the TUI builds on. `client/tui/main.c`
also calls both libraries **directly** — `utf8proc_iterate` for width, and the
raw `tb_*` grid calls — so the toolbox is the shared layer rather than an
exclusive one.

Committed files: `third_party/{termbox2/termbox2.h, utf8proc/utf8proc.{c,h},
utf8proc/utf8proc_data.c, qrcodegen/qrcodegen.{c,h} (with its LICENSE), jsmn/jsmn.h, miniaudio/miniaudio.h,
stb/stb_image_write.h, stb/stb_image.h}`, libfvad's `include/` and `src/` trees
(its `LICENSE`, `AUTHORS` and `PATENTS` beside them, compiled in one translation
unit, `fvad_unit.c`, with our warnings held off vendored code), and **each carries
its licence file**:
termbox2's `LICENSE`, utf8proc's `LICENSE.md`, jsmn's `LICENSE` (extracted
verbatim from the notice in `jsmn.h`, which is where upstream keeps it), and the
`LICENSE` of miniaudio and of stb (each extracted from the notice at the foot of
its header). miniaudio's implementation is compiled in exactly one translation
unit, `audio_ma.c`, with only device I/O enabled. `.gitignore` ignores
`third_party/*` and whitelists the committed paths: `jsmn/`, `termbox2/`,
`picohttpparser/`, `utf8proc/`, `argon2/`, `pwlist/`, `qrcodegen/`, `miniaudio/`, `stb/`, `sqlite-3.53.4/`, `libfvad/`, `ca-roots/`, `speexdsp/`, `opus/`, `libvpx/`, `mbedtls/` and `lucide/`
(see below). Everything else under
`third_party/` — the build directories and what the build fetches — stays ignored.

## 2. Fetched at build time — `scripts/build/*.sh` (gitignored output)

**Toolchains** come this way too (`mk/toolchains.mk`): make fetches each on
first use, pinned by version and SHA-256 in its script (Emscripten by its emsdk
tag, whose own table pins the sum of everything it installs), into `third_party/`,
and puts it first on `PATH` for every recipe and vendor script, so nothing
depends on what a machine or a runner image happens to have installed.
The full build needs only cc and c++ (C++17), make, cmake, python3, git, curl, xz, bzip2 and nasm beyond them.
The release's Linux daemon is the exception: it compiles with the build
machine's own gcc 12, on Ubuntu 22.04 so that its glibc 2.35 is the binary's floor
(ARCHITECTURE.md, ARCH-20), and the arm64 daemon with Ubuntu's cross packages,
installed from the distribution, not fetched.
A bump is the version and the sum in one script and a pull request, built and
tested by the next release-candidate (dispatched by hand); `tools/toolchains.sh`, run by hand, says when upstream
has moved.

| Toolchain | Version | Script | Builds | License |
|-----------|---------|--------|--------|---------|
| **gcc 12** (Ubuntu 22.04's `gcc-12` and `g++-12`, with `gcc-12-aarch64-linux-gnu`, `g++-12-aarch64-linux-gnu` and `libc6-dev-arm64-cross` for arm64; not the default gcc 11, which leaves read-aloud's kernels unvectorised) | the distribution's 12.3 | none: installed from Ubuntu's packages on the runner (`release-candidate.yml`, `stage`) | the release's Linux daemon against glibc 2.35 (`make RELEASE=1`) and the arm64 daemon cross-built from x86 (`make ARCH=aarch64`) | GPL-3.0 with the GCC Runtime Library Exception |
| **Emscripten** (emsdk) | 6.0.13 | `scripts/build/emsdk.sh` | the web client (`make build/web/openchime.js`) and the loader compiled into a `WEB=1` daemon; brings its own Node for the web client's checks (`make test-all`) | MIT / NCSA |
| **llvm-mingw** (clang, lld, LLVM binutils, mingw-w64 headers and CRT) | 20261020 | `scripts/build/llvm_mingw.sh` | the Windows GUI and TUI and their libraries (`make build`), through the `x86_64-w64-mingw32-*` names | Apache-2.0 with LLVM exception; mingw-w64 under its own permissive terms |
| **makemsix** (Microsoft msix-packaging, with CMake 3.31.6 to build it) | commit `25a65f5` | `scripts/build/makemsix.sh` | the Windows client's MSIX, on Linux (`packaging/build-windows.sh`); built from source because no release compiles with a current clang | MIT (makemsix); BSD-3-Clause (CMake) |
| **WinShell** (NSIS plugin) | the NSIS wiki's `WinShell.zip`, SHA-256 pinned | `scripts/build/nsis_plugins.sh` | the installer's Start-menu shortcut AppUserModelID (`packaging/windows/openchime.nsi`); `makensis` itself is Ubuntu's `nsis` | zlib |
| **jsign** | 7.6 | `scripts/build/jsign.sh` | Authenticode signing of the Windows binaries and installer against Azure Trusted Signing, on Linux (`scripts/release/sign_windows.sh`) | Apache-2.0 |

| Package | Version | Purpose | Used by | Source | License |
|---------|---------|---------|---------|--------|---------|
| **SDL3** | 3.4.14 | Windowing, input, GPU-accelerated 2D renderer for the graphical clients | GUI client (`client/gui/gfx/`, oc_gfx) | https://github.com/libsdl-org/SDL | zlib (no notice required in binary distributions) |
| **Qwen3.5 2B** (Qwen) | commit `15852e8c` | The summary model: its configuration, tokenizer, licence and weights, **fetched by the daemon on first start** at the pinned commit, each checked against a pinned SHA-256, converted by `openchimed --summary-pack` into `summary.bin` (its SHA-256 pinned too) and run by sumkit in plain C. Never shipped; not fetched at build. The pinned files and the commands that recreate it are below | Daemon (summaries, ARCH-116) | https://huggingface.co/Qwen/Qwen3.5-2B | Apache-2.0 |
| **Kitten TTS mini** (KittenML) | 0.8 | The read-aloud voice model: `kitten_tts_mini_v0_8.onnx` and `voices.npz`, converted by `openchimed --voice-pack` and **committed** as `ttskit/data/voice.bin` (its licence `ttskit/data/VOICE-LICENSE`), run by ttskit in plain C and **shipped beside the daemon** as data. Not fetched at build; the pinned files and the commands that recreate it are below | Daemon (read-aloud, ARCH-111) | https://huggingface.co/KittenML/kitten-tts-mini-0.8 | Apache-2.0 |
| **Moonshine Tiny Streaming (English)** (Moonshine AI) | CDN directory `quantized_26_08_21` | The voice-input recognizer: its six published int8 graphs, `tokenizer.bin` and `streaming_config.json`, converted by `openchimed --recognizer-pack` and **committed** as `sttkit/data/recognizer.bin` (its licence `sttkit/data/RECOGNIZER-LICENSE`), run by sttkit in plain C and **shipped beside the daemon** as data in `stt/`. Not fetched at build; the pinned files and the commands that recreate it are below | Daemon (voice input, ARCH-112) | https://github.com/moonshine-ai/moonshine | MIT (English models; the legacy non-streaming models for other languages are under a non-commercial licence and are not used) |
| **Phonetisaurus** (+ OpenFst, MITLM in its wheel) | 0.3.0 | Trains ttskit's guesser. **Build tool for maintainers only**: never linked, never shipped, not needed to build OpenChime | `scripts/build/ttskit_data.sh` | https://github.com/AdolfVonKleist/Phonetisaurus | BSD-3-Clause (OpenFst Apache-2.0, MITLM MIT) |
| **CMUdict** | commit `74790861` | Source of ttskit's pronunciation data, which is generated from it and **committed** (`ttskit/data/en-US/`, its notice in `ttskit/data/CMUDICT-LICENSE`) | Daemon (read-aloud) | https://github.com/cmusphinx/cmudict | BSD-style (two clauses) |

**SDL3**: `scripts/build/sdl3_windows.sh` fetches the pinned release tarball
(SHA-256-verified before it is unpacked; a bump without a matching sum is
refused), cross-builds a **static-only**
`libSDL3.a` with llvm-mingw into `third_party/sdl3-3.4.14-win/` (gitignored), and
`make build` links it. SDL's own build is CMake; the script invoking
it is the same arrangement as invoking mbedTLS's make — the dependency's build
system stays its own business, and this tree's stays make. It is a
large dependency, fetched pinned rather than committed. zlib-licensed: no notice needs to travel with a shipped
binary, so `packaging/licenses.sh` (daemon-only regardless) is untouched.

**Read-aloud's voice model** (ARCH-111) is **Kitten TTS mini 0.8**, published by
KittenML under Apache-2.0. ttskit runs it in plain C (TTSKIT.md §5b); nothing of it is
fetched or built at build time. It is converted once into ttskit's own format and
**committed**: `ttskit/data/voice.bin` (86,851,136 bytes), with the model's licence beside it
as `ttskit/data/VOICE-LICENSE` (upstream's `LICENSE`, verbatim). The build copies both into
`tts/` beside `openchimed`, and `packaging/licenses.sh` puts the licence, with CMUdict's
notice, in every package that carries read-aloud.

Its source, pinned:

| File | From | Archived in | SHA-256 |
|---|---|---|---|
| `kitten_tts_mini_v0_8.onnx` | https://huggingface.co/KittenML/kitten-tts-mini-0.8, revision `c02725660cea441db4c383af69f1f26f5cd00947` | `voice-src.part*.gz` | `0f5bbae4fc4800c98dbc544a87ecfa79510de2fb8222db30d12e5bfe9177df91` |
| `voices.npz` | the same repository and revision | `voice-src.part*.gz` | `40ad2638952b77b7b2f30127e2608e169fc69dd256b53bd8aaa3409a33193c42` |
| `LICENSE` | https://github.com/KittenML/KittenTTS, commit `be5758500b731b8fc674acc62ea480d3022b7ebe` | `voice-src.part*.gz` | `787aa312b93eee80b636f35652469df29efdb7582239d6a9c088467555a3c7fd` |

**Recreating `voice.bin` byte for byte.** `openchimed --voice-pack` is in every build of the
daemon, with read-aloud or without. From the archive (below, *The archive*), from the
repository folder:

```
make openchimed
scripts/build/speech_sources.sh fetch build/speech-src
./openchimed --voice-pack build/speech-src/voice-src/kitten_tts_mini_v0_8.onnx build/speech-src/voice-src/voices.npz build/voice.bin
cmp build/voice.bin ttskit/data/voice.bin
cmp build/speech-src/voice-src/LICENSE ttskit/data/VOICE-LICENSE
```

Or from the upstream URLs:

```
make openchimed
mkdir -p build/voice-src && cd build/voice-src
HF=https://huggingface.co/KittenML/kitten-tts-mini-0.8/resolve/c02725660cea441db4c383af69f1f26f5cd00947
GH=https://raw.githubusercontent.com/KittenML/KittenTTS/be5758500b731b8fc674acc62ea480d3022b7ebe
curl -fsSLO "$HF/kitten_tts_mini_v0_8.onnx"
curl -fsSLO "$HF/voices.npz"
curl -fsSLO "$GH/LICENSE"
sha256sum -c <<'SUMS'
0f5bbae4fc4800c98dbc544a87ecfa79510de2fb8222db30d12e5bfe9177df91  kitten_tts_mini_v0_8.onnx
40ad2638952b77b7b2f30127e2608e169fc69dd256b53bd8aaa3409a33193c42  voices.npz
787aa312b93eee80b636f35652469df29efdb7582239d6a9c088467555a3c7fd  LICENSE
SUMS
cd ../..
./openchimed --voice-pack build/voice-src/kitten_tts_mini_v0_8.onnx build/voice-src/voices.npz build/voice-src/voice.bin
sha256sum build/voice-src/voice.bin
cmp build/voice-src/voice.bin ttskit/data/voice.bin
cmp build/voice-src/LICENSE ttskit/data/VOICE-LICENSE
```

The `voice.bin` it writes is 86,851,136 bytes with SHA-256
`088b5443f919c988852e8aa83e27ed072e575fcdc40bd006b37a164dd7438ed7`, the same on every run and
machine, and both `cmp`s print nothing. The converter (`daemon/voice_pack.c`) reads the `.onnx`
file and the stored zip with no library, refuses a model whose tensors are not the ones it
expects, and records the two inputs' SHA-256s in the file's header. Converting a different
model or revision is a change to OpenChime (TTSKIT.md §8), not a rebuild.

**The reference renderings** the C forward pass is tested against (`tests/data/voice/`)
were made once from the files above, before the engine they were made with was withdrawn.
They cannot be made again; the committed files are the reference (TTSKIT.md §9).

**Voice input's recognizer** (ARCH-112) is **Moonshine Tiny Streaming (English)**, published
by Moonshine AI under MIT. sttkit runs it in plain C (STTKIT.md §3–§4); nothing of it is
fetched or built at build time. It is converted once into sttkit's own format and
**committed**: `sttkit/data/recognizer.bin` (40,438,710 bytes), with the model's licence beside
it as `sttkit/data/RECOGNIZER-LICENSE` (upstream's `LICENSE`, verbatim). The build copies both
into `stt/` beside `openchimed`, and `packaging/licenses.sh` puts the licence in every package
that carries voice input.

Its source, pinned — the files as published in a dated CDN directory that is never
overwritten, already quantized to int8, and the licence at a pinned commit:

| File | From | Archived in | SHA-256 |
|---|---|---|---|
| `frontend.model.ort` | https://download.moonshine.ai/model/tiny-streaming-en/quantized_26_08_21/ | `recognizer-src.part*.gz` | `5121b561417b638afce0c6c31b760e37c93cf97f80d9b0031aad1fe7b6f25d61` |
| `frontend.weights.ort` | the same directory | `recognizer-src.part*.gz` | `217da24ac6f522ebf02da8ef288e77d1ac68d50d4a6821433182e4fbf4204bbd` |
| `encoder.ort` | the same directory | `recognizer-src.part*.gz` | `a8414e1a5dedf9f2093d7680601dd8a9b0433e7020260eafe0e370ead91134ca` |
| `adapter.ort` | the same directory | `recognizer-src.part*.gz` | `22ecc949e146c49667fda28d102d4e30749a107dc88a396292aa8f277ef1347c` |
| `cross_kv.ort` | the same directory | `recognizer-src.part*.gz` | `143a36667b8d05fd9d04e8c337b7ee121f37ef299aea6b3d82bdb3d3401950b4` |
| `decoder_kv.ort` | the same directory | `recognizer-src.part*.gz` | `8852553f312adb6c9aa4d17418015049b30f412209ee569d336548c0044627de` |
| `tokenizer.bin` | the same directory | `recognizer-src.part*.gz` | `6884b35fd6377d4c4d32336a0bc152f36b64d1e45b6503683cdc238250a8472d` |
| `streaming_config.json` | the same directory | `recognizer-src.part*.gz` | `74fe5ddebd63b17caf59e8a3b18c17547ff7bce1642050edbb1c3962674f8950` |
| `LICENSE` | https://github.com/moonshine-ai/moonshine, commit `234f60faa0eb388b01cdf7e60aca232af37aefda` | `recognizer-src.part*.gz` | `fa7d1174dd8af6a7cd280be20b80d10095ed4c19b5b20b61a7715c3ad790dc5f` |

**Recreating `recognizer.bin` byte for byte.** `openchimed --recognizer-pack` is in every build
of the daemon, with voice input or without. From the archive (below), from the repository
folder:

```
make openchimed
scripts/build/speech_sources.sh fetch build/speech-src
./openchimed --recognizer-pack build/speech-src/recognizer-src build/recognizer.bin
cmp build/recognizer.bin sttkit/data/recognizer.bin
cmp build/speech-src/recognizer-src/LICENSE sttkit/data/RECOGNIZER-LICENSE
```

Or from the upstream URLs:

```
make openchimed
mkdir -p build/recognizer-src && cd build/recognizer-src
CDN=https://download.moonshine.ai/model/tiny-streaming-en/quantized_26_08_21
GH=https://raw.githubusercontent.com/moonshine-ai/moonshine/234f60faa0eb388b01cdf7e60aca232af37aefda
for f in frontend.model.ort frontend.weights.ort encoder.ort adapter.ort cross_kv.ort decoder_kv.ort \
         tokenizer.bin streaming_config.json; do curl -fsSLO "$CDN/$f"; done
curl -fsSLO "$GH/LICENSE"
sha256sum -c <<'SUMS'
5121b561417b638afce0c6c31b760e37c93cf97f80d9b0031aad1fe7b6f25d61  frontend.model.ort
217da24ac6f522ebf02da8ef288e77d1ac68d50d4a6821433182e4fbf4204bbd  frontend.weights.ort
a8414e1a5dedf9f2093d7680601dd8a9b0433e7020260eafe0e370ead91134ca  encoder.ort
22ecc949e146c49667fda28d102d4e30749a107dc88a396292aa8f277ef1347c  adapter.ort
143a36667b8d05fd9d04e8c337b7ee121f37ef299aea6b3d82bdb3d3401950b4  cross_kv.ort
8852553f312adb6c9aa4d17418015049b30f412209ee569d336548c0044627de  decoder_kv.ort
6884b35fd6377d4c4d32336a0bc152f36b64d1e45b6503683cdc238250a8472d  tokenizer.bin
74fe5ddebd63b17caf59e8a3b18c17547ff7bce1642050edbb1c3962674f8950  streaming_config.json
fa7d1174dd8af6a7cd280be20b80d10095ed4c19b5b20b61a7715c3ad790dc5f  LICENSE
SUMS
cd ../..
./openchimed --recognizer-pack build/recognizer-src build/recognizer-src/recognizer.bin
sha256sum build/recognizer-src/recognizer.bin
cmp build/recognizer-src/recognizer.bin sttkit/data/recognizer.bin
cmp build/recognizer-src/LICENSE sttkit/data/RECOGNIZER-LICENSE
```

The `recognizer.bin` it writes is 40,438,710 bytes with SHA-256
`4ec535eb9ce144f3328879c5e54ad863519dbfe7440acd178168ed6332e6d21b`, the same on every run and
machine, and both `cmp`s print nothing. The converter (`daemon/recognizer_pack.c`) reads the
`.ort` files (FlatBuffers) with no library, refuses a model whose tensors or operators are not
the ones it expects, and records the eight inputs' SHA-256s in the file's header;
`openchimed --recognizer-pack --list DIR` prints every graph it reads. Converting a different
model or revision is a change to OpenChime (STTKIT.md §7), not a rebuild.

**The references** the C forward pass is tested against (`tests/data/recognizer/`) were made
once from the files above, before the engine they were made with was withdrawn. They cannot be
made again; the committed files are the reference (STTKIT.md §8).

**The archive.** Every file above is kept as an attachment of the project's public tracker, so
the speech data can be remade when an upstream URL is gone. The two source bundles are split under the attachment size limit.

| Attachment | Holds | SHA-256 |
|---|---|---|
| [`voice-src.part0.gz`](https://github.com/user-attachments/files/35148170/voice-src.part0.gz), [`.part1.gz`](https://github.com/user-attachments/files/35148174/voice-src.part1.gz), [`.part2.gz`](https://github.com/user-attachments/files/35148175/voice-src.part2.gz) | `voice-src.tar.gz`, joined: the voice model's three files above | the joined bundle `27758cb22f0969cf5440a5f48dd4e158b98963c62446707b49562f08dd0cf5e1` |
| [`recognizer-src.part0.gz`](https://github.com/user-attachments/files/35148176/recognizer-src.part0.gz), [`.part1.gz`](https://github.com/user-attachments/files/35148172/recognizer-src.part1.gz) | `recognizer-src.tar.gz`, joined: the recognizer's nine files above | the joined bundle `33c6a2062d7f01009942eaf2021d83318ff26258bdb0ebe08a1de8a4079c31c9` |
| [`SHA256SUMS-archive.txt`](https://github.com/user-attachments/files/35148169/SHA256SUMS-archive.txt), [`SHA256SUMS-inputs.txt`](https://github.com/user-attachments/files/35148171/SHA256SUMS-inputs.txt) | the SHA-256 of every attachment and bundle, and of every file inside the two bundles (the ones the tables above record) | `b06b80e8…`, `f748d001…` |

`scripts/build/speech_sources.sh fetch DIR` downloads the seven, joins the bundles, checks each
against `scripts/build/speech_sources.sha256`, unpacks `DIR/voice-src` and `DIR/recognizer-src`,
and checks every source file against `SHA256SUMS-inputs.txt`;
a file that does not match stops it, named. `scripts/build/speech_sources.sh check DIR` repeats the
checks on a copy, with no network. It is a maintainer's tool: nothing in the build, the suites or
CI fetches from the archive.

**The summary model** (ARCH-116) is Qwen3.5 2B, from Qwen's repository at commit
`15852e8c16360a2fea060d615a32b45270f8a8fc`, run by sumkit (SUMKIT.md). Nothing of it is in the
tree or the packages: the daemon fetches it on first start, checks it and converts it
(SUMMARIES.md §6). The pinned files (`daemon/sum_fetch.h`):

| File | SHA-256 |
|---|---|
| `config.json` | `ed1c1723241f23f7f4e23430759cbd7dcfb4103cbdfe052bfe7626b57c2615b4` |
| `tokenizer.json` | `5f9e4d4901a92b997e463c1f46055088b6cca5ca61a6522d1b9f64c4bb81cb42` |
| `LICENSE` | `bbedc3fda3305820b977265f01b8619d87570a6739de3a5582c3464840f1e57a` |
| `model.safetensors-00001-of-00001.safetensors` (4,548,221,488 bytes) | `aa33250c4fc64891ddfaba3a314fd9542ea371843c387178b425fbcc5ed680b1` |
| `summary.bin`, converted (1,399,305,116 bytes) | `c982e72f2e47f7d4e71cc5e977444c4641dcbefe2bb8ebeef6815374c19bf044` |

To recreate `summary.bin` by hand, with `R` the commit:

    for f in config.json tokenizer.json LICENSE; do
      curl -fsSLo "SRC/$f" "https://huggingface.co/Qwen/Qwen3.5-2B/resolve/$R/$f"; done
    curl -fsSLo SRC/model.safetensors \
      "https://huggingface.co/Qwen/Qwen3.5-2B/resolve/$R/model.safetensors-00001-of-00001.safetensors"
    sha256sum SRC/*                                   # against the table
    openchimed --summary-pack SRC summary.bin && sha256sum summary.bin

`openchimed --summary-pack --full SRC full.bin` writes the same model unquantised, for
`sum_check` (SUMKIT.md §6). The references in `tests/data/summary/` were made once from the
same unquantised weights, before the engine they were made with was withdrawn; they cannot be
made again. Unsloth's `Q4_K_M` conversion, the file the previous engine ran, is the
quantisation's baseline (SUMKIT.md §4) and nothing more.

**The web client** (`make build/web/openchime.js`) builds its own copies with Emscripten into
`build/wasm/` (gitignored): `scripts/build/mbedtls_wasm.sh` from the committed
mbedTLS (`third_party/mbedtls/`), `scripts/build/sdl3_wasm.sh` from the same pinned, SHA-256-verified
SDL3 release, and `scripts/build/media_wasm.sh` for libvpx, libopus and speexdsp from
`third_party/libvpx/`, `third_party/opus/` and `third_party/speexdsp/`.

## 3. System / OS packages (linked at build)

Provided by the host toolchain (local dev / CI) or the Debian base of the
container image; their versions track the OS, not this repo.

| Package | Purpose | Used by | Link | Source | License |
|---------|---------|---------|------|--------|---------|
| **glibc `resolv`** (`libresolv`) | DNS **SRV** lookup for workspace resolution (REQ-010) | TUI only (Linux) | `-lresolv` | glibc | LGPL-2.1 (glibc) |
| **pthreads** | Threads (net thread, queues, dbwriter) | Daemon + client | `-lpthread` | glibc | LGPL-2.1 |
| **libsecret** *(optional)* | OS keyring (Secret Service) backend for the session token on Linux (`client/shared/secret_libsecret.c`); Windows uses Credential Manager via `advapi32`, no vendored dependency | Linux TUI/GUI credential store | `pkg-config libsecret-1`, gated by `-DOC_HAVE_LIBSECRET` | https://gitlab.gnome.org/GNOME/libsecret | LGPL-2.1 |
| **GLib** *(optional)* | Pulled in transitively by libsecret | TUI (only when libsecret present) | via libsecret | https://gitlab.gnome.org/GNOME/glib | LGPL-2.1 |

`libsecret`/`glib` are **optional and dynamically linked**: the Linux TUI's build detects
libsecret via `pkg-config` and compiles the keyring backend, else compiles a stub
and that machine then persists no credential at all (headless / no D-Bus). They are
*not* linked into the daemon or the test binary.

- **Local build headers:** `libsecret-1-dev` for the keyring backend. SQLite needs
  none — it is compiled from the committed amalgamation.
- **CI installs:** `bzip2`, `nasm` and `xz-utils` (and `gdb`, and `ffmpeg` for
  `ffprobe`, neither linked); every compiler but the runner's own gcc is
  fetched by make (the toolchains above). The Windows TUI and GUI (`make build`) need
  neither libsecret nor libresolv: the mingw build reaches DNS through
  `DnsQuery` and the credential store through Credential Manager.

## 4. Container images — deployment only

| Image / tool | Version | Purpose | License | Source |
|--------------|---------|---------|---------|--------|
| **Debian** | `bookworm-slim` | Runtime base of the container image (glibc 2.36, above the binary's 2.35 floor) | mixed (base OS) | https://www.debian.org |
| **Litestream** | 0.5.17 | In the image only: streams `openchime.db` and the identity to the deployment's bucket, each to `<prefix>/db` under its own prefix, and restores each onto a box that has none, when the deployment writes its `replica-*` files (ARCH-3, ARCH-39; CONFIG.md); a separate program, not linked into the daemon | Apache-2.0 (its `LICENSE` ships in the image at `/usr/share/doc/litestream/LICENSE`) | https://github.com/benbjohnson/litestream |

One image, and it is an **output** rather than a tool: the OCI image the staging
build builds and the release tags (ARCH-20/76), and anyone can build from a release tarball. Nothing is compiled in it: the Dockerfile
lays the release tarball's daemon, speech data and web client onto the base
(`packaging/image-context.sh`), so the image carries the same binary as the
packages, and the arm64 image is built on the x86 runner with no emulation. It
adds no packages: the daemon links everything it needs, CA roots included.

Litestream is the one other program in it. `packaging/image-context.sh` fetches
the release's static `litestream-0.5.17-linux-{x86_64,arm64}.tar.gz` for the
image's architecture and checks it against the SHA-256 the release publishes
(`checksums.txt`) before unpacking it; the two digests are pinned in the
script, and a version bump changes the version and both digests together. The base has no CA
bundle, so the context also carries the daemon's own Mozilla roots
(`third_party/ca-roots`) written back out as PEM, which Litestream alone is
pointed at. The image carries no Litestream configuration: the entrypoint
writes it at boot from the deployment's `replica-*` files and passes the key in
Litestream's own `LITESTREAM_ACCESS_KEY_ID`/`LITESTREAM_SECRET_ACCESS_KEY`.
Litestream is not in the packages or the tarball: only the image can be told
to replicate.

The full suite's image check (`make test-all PART=image`, in `release-candidate.yml`) fetches one more tool, never shipped:
**rclone** v1.75.2 (MIT, https://github.com/rclone/rclone), checked against its
published SHA-256, whose `serve s3` is the bucket the image replicates to and
restores from on the runner (TESTING.md §3.2). The workflows' other rclone steps
use the runner's Ubuntu `rclone` package, installed through `scripts/build/apt.sh`.

The project runs no containers for development or testing (ARCH-36), so this
table holds exactly one image. The sole image the repo builds on is the
`FROM debian:bookworm-slim` in `Dockerfile` (the only other image named is
`rockylinux:9`, which the release's dnf smoke runs under podman) — a tag rather than a digest, which
is the upstream's own stability contract for a base OS.

## 5. Build & dev tooling (not linked into the product)

- **C99 toolchain** — gcc or clang (+ `ar`); the whole tree builds with
  `-std=c99 -Wall -Wextra`.
- **make** — the only build system (no CMake/autotools for OpenChime itself).
- **bash, curl, tar, bzip2, xz, git, python3, cmake** — used by the
  `scripts/build/*.sh` fetch scripts; cmake and python3 by SDL3's own builds.
- **pkg-config** — detects libsecret for the TUI.
- **Emscripten, llvm-mingw** — fetched by make, pinned (§2, "Toolchains").
  `make build` and a `make openchimed` of the host's daemon use the system cc;
  the release uses Ubuntu 22.04's `gcc-12`, and for arm64 Ubuntu's
  `gcc-12-aarch64-linux-gnu`, `g++-12-aarch64-linux-gnu` and `libc6-dev-arm64-cross`.
- **buildah / podman / skopeo** — **release-candidate and release only**.
  Build, start and push the OCI image, check its digest and copy it to
  `:N-rcK` and `:rc` (the release-candidate),
  copy it to its released tags and move the `:latest` and `:rc` tags (the
  release), daemonlessly.
- **Docker and Docker Compose are NOT used**, anywhere, by anything — not to
  build, not to test, not locally, not in CI. This is a standing constraint;
  see ARCH-36 and the header of `.github/workflows/release-candidate.yml`.

## 6. Windows cross-compile artifacts

The Windows GUI is the portable application (`client/gui/app/`, ARCH-80) over
`client/gui/platform/win32/`. Its cross-compile, and the Windows TUI's (ARCH-81), use:

| Item | Version | Note | License |
|------|---------|------|---------|
| `scripts/build/llvm_mingw.sh`, `third_party/llvm-mingw-20261020` | 20261020 | The toolchain itself (clang, lld, LLVM binutils, mingw-w64 headers and CRT, UCRT), fetched by make | Apache-2.0 with LLVM exception |
| `scripts/build/mbedtls_windows.sh`, `third_party/mbedtls-3.6.7-win` | 3.6.7 | Windows cross-compile; used by `make build` | Apache-2.0 |
| `scripts/build/sdl3_windows.sh`, `third_party/sdl3-3.4.14-win` | 3.4.14 | Windows cross-compile (static, CMake invoked by the script); used by `make build` | zlib |
| `scripts/build/libvpx.sh windows`, `third_party/libvpx-1.17.0-win` | 1.17.0 | Windows cross-compile (static, nasm); used by `make build` | BSD-3-Clause |
| `scripts/build/opus.sh windows`, `third_party/opus-1.6.1-win` | 1.6.1 | Windows cross-compile (static); used by `make build` | BSD-3-Clause |
| `scripts/build/speexdsp.sh windows`, `third_party/speexdsp-1.2.1-win` | 1.2.1 | Windows cross-compile (static); used by `make build` | BSD-3-Clause |
| Media Foundation (`mfplat`, `mfreadwrite`, `mf`, `mfuuid`) | OS | Camera capture for video messages (`client/core/media/cap_mf.c`) | Windows system libraries |

## 7. Why libvpx

libvpx is the video codec for video messages and screenshare; why, and
the alternatives rejected, are CALLS.md §6.4 (ARCH-87).

---

## Lucide (icons, ISC)

Most of the graphical clients' icons come from [Lucide](https://lucide.dev)
(ISC License). We vendor **only the handful of SVGs we use**
(`third_party/lucide/icons/*.svg` — 41 of them) plus the license
(`third_party/lucide/LICENSE`).

**Four icons are ours, not Lucide's**, and live outside `third_party/` for exactly
that reason: `send`, `smile`, `at-sign` and `download`, hand-authored in Lucide's
conventions (24×24, 2px round-capped strokes) under `client/shared/icons_src/`.
Keeping them out of the vendored tree is what keeps the ISC attribution truthful —
it covers Lucide's work and nothing else.
A dev-time script (`scripts/gen/icons.py`, uses `svgelements`) flattens those SVGs
into platform-agnostic cubic-bezier path data baked into `client/shared/icons.{h,c}`
— so there is **no icon-font dependency** and each native client strokes the same
geometry with its own 2D API (the GUI tessellates the strokes itself for the SDL renderer,
`client/gui/gfx/gfx_icons.c`). The
generated `icons.c` carries the ISC attribution in its header. Only the baked
paths ship; nothing is fetched at runtime.

---

## License summary

| License | Packages | Notes |
|---------|----------|-------|
| **MIT** | termbox2, utf8proc, qrcodegen, jsmn, picohttpparser, SecLists' common passwords | Vendored, committed |
| **CC0-1.0 or Apache-2.0** | Argon2 (reference) | Vendored, committed |
| **MIT** | Moonshine Tiny Streaming (the recognizer) | Converted and committed (`sttkit/data/`), shipped as data, **daemon only** (ARCH-112) |
| **Apache-2.0** | Qwen3.5 2B (the summary model) | Fetched from Qwen and converted by the daemon on first start, never shipped; its licence kept beside it (ARCH-116) |
| **MIT-0 / Public Domain** | miniaudio | Vendored, committed; client only |
| **MIT / Public Domain** | stb_image_write, stb_image | Vendored, committed; client only |
| **ISC** | Lucide (icon path data) | Baked into client/shared/icons.c; 41 SVGs + LICENSE vendored. The other 4 icons in that file are our own work (`client/shared/icons_src/`), not ISC-licensed material |
| **Apache-2.0** | Mbed TLS (chosen from its dual license) | Static-linked |
| **BSD-3-Clause** | libvpx (VP9), libopus | Committed and static-linked: both into the graphical clients, Win32 and web (ARCH-110), and libopus into the daemon to encode read-aloud renders (ARCH-111). Permissive, within this repo's posture (mbedTLS is already Apache-2.0, not MIT) |
| **BSD-3-Clause** | speexdsp, libfvad | Voice input in the graphical clients, Win32 and web (ARCH-112): speexdsp committed and static-linked, libfvad vendored and compiled in; **client only** |
| **BSD-2-style** | CMUdict (ttskit's pronunciation data) | Generated data, committed with its notice; daemon only |
| **Apache-2.0** | Kitten TTS mini (data) | Converted and committed (`ttskit/data/voice.bin`, its licence `ttskit/data/VOICE-LICENSE`), shipped beside the daemon |
| **zlib** | SDL3 | Fetched at build, static-linked; graphical clients only |
| **MPL-2.0** | Mozilla CA roots (data) | Generated, compiled in; daemon and every client. File-level copyleft |
| **Public Domain** | SQLite | Compiled in, **daemon only** — no client links it (ARCH-88) |
| **LGPL-2.1** | libsecret, glib, glibc (resolv/pthreads) | Dynamically linked / optional — LGPL satisfied by dynamic linking |
| **Unicode license** | utf8proc bundled data tables | Alongside utf8proc's MIT code |
| **Apache-2.0 WITH LLVM-exception** | llvm-mingw | Toolchain runtime; static in the Windows clients; the exception waives binary attribution |
| **GPL-3.0 with GCC Runtime Library Exception** | libgcc | Toolchain runtime of every Linux build, the release's included; the exception permits linking it into the daemon |

**Bottom line:** every third-party dependency in the shipped daemon and TUI is
permissively licensed (MIT, BSD, Apache-2.0, CC0, ISC, zlib, Public Domain, plus
optional dynamic LGPL), except the CA roots' data under MPL-2.0, a file-level
copyleft; all of them are compatible with the AGPL-3.0-or-later terms our own code carries.
No third-party AGPL code exists anywhere in the build.
