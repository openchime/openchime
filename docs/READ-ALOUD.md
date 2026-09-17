# OpenChime — Read-aloud

How a channel is read aloud: what a listener gets, how a message becomes speakable text,
how the daemon renders it with the voice model installed beside it, caches and serves it, and how a client
plays a channel in order. This is the authoritative design; it is cross-referenced from
ARCHITECTURE.md (ARCH-111), REQUIREMENTS.md (§6.4, REQ-291–295), PROTOCOL.md (§5.14b),
SCHEMA.md (migration 0042) and CONFIG.md. Pronunciation — turning words into the phonemes
a voice model reads — is its own library, described in [TTSKIT.md](./TTSKIT.md).

Read-aloud renders **text that is already a message**. Nothing is recorded, posted or
stored as a message; a reader and a listener have the same conversation. It is not a
clip (REQ-273) and not a call (REQ-150).

---

## 1. What a listener can do

- **Listen from a message onward.** "Listen" on a channel starts at the first unread
  message (or the newest if none are unread); "Listen from here" on a message starts
  there. Each message with something to say plays in order, in its author's voice.
- **Play, pause, skip, stop.** Skip moves to the next message. New messages that arrive
  while listening join the end of the queue.
- **One speaking rate.** Every rendering is at 1.2× the model's native rate, chosen by ear.
  There is no speed control.
- **A voice of their own.** Each user's voice is on their profile, previewable and
  changeable. Everyone hears the same voice for the same author.

Where the operator has turned read-aloud off, none of this appears (REQ-295). Nothing has
to be installed to have it on.

## 2. Speakable text

`shared/speakable.c` turns a message body into what is said, over the same spans the
clients render (`shared/richtext.c`) and the same mentions the daemon resolves
(`shared/mention.c`). It runs in the daemon, before the cache key is computed, so every
listener gets the same text.

| In the body | Said |
|---|---|
| A fenced code block | "Code block, 12 lines." — the contents are skipped |
| `inline code` | the contents, as written |
| A URL | its host, without `www.` — "github.com" |
| `@someone` | their display name |
| `@here` / `@channel`, `@everyone` | "everyone here" / "everyone" |
| `*bold*`, `_italic_`, `~strike~`, list markers, escapes | the text, markers dropped |
| A quoted line | "Quote:" once per quote, then the text |
| Emoji and `:shortcodes:` | dropped |
| A line break | the end of a sentence |

Numbers, times, currency, ordinals and common abbreviations are expanded to words by
ttskit (TTSKIT.md §4), not here. A message that leaves nothing to say — a bare attachment,
only emoji — is **not renderable** (REQ-294).

Speakable text is split at sentence ends into pieces of at most 120 characters; the pieces
are synthesized in turn and joined into one file, so a long message never holds more of the
model's working memory than a sentence needs (three sentences in one run peak at 337 MB,
the same three in turn at 253 MB).

## 3. Voices

**One language per deployment.** The daemon speaks the language its engine was
built for — `en-US` today — set by `OPENCHIME_TTS_LANG` and refused at startup if
this binary has no engine for it, because reading every message in a language
nobody asked for is worse than not starting. The tag is stamped into the
pronunciation data (a lexicon of one language with a guesser of another
pronounces fluent nonsense, so the pair is checked), it is part of the model
version that keys the cache, and it is announced with every voice on `TTS_INFO`.
A voice belongs to a language: a second language is a second engine beside this
one, not a setting on it.

The model is **Kitten mini v0.8** (Apache-2.0), which has eight voices. A user's voice is
one of them, stored as `users.voice_id` and carried on the profile frames.

- **Default.** Declared pronouns select a voice of matching presentation — read as
  whole words, the first one naming a presentation deciding, so `she/they` is read as
  she and `they/them`, naming neither, is treated exactly like no pronouns at all;
  otherwise a stable hash of the user id picks one. The choice is written to the profile on first use,
  so it is visible and correctable rather than recomputed.
- **Webhook and bot authors** use the hash voice.
- **A change is not retroactive.** Earlier renderings keep the voice they were made with;
  the next message rendered uses the new one.

## 4. Synthesis in the daemon

Read-aloud's code is inside `openchimed` and its data is beside it; there is no second process
and nothing to fetch.

- **What is built in.** ttskit (TTSKIT.md) and ONNX Runtime itself, built from source as a
  minimal static library with only the operators and types this model uses
  (`daemon/tts_kitten.ops.config`). The daemon is under 9 MB.
- **What is beside it.** The Kitten mini model converted to ONNX Runtime's `.ort` format, its
  voices, and ttskit's `en-US` lexicon and guesser — about 108 MB — in a data directory: the
  first of `OPENCHIME_TTS_DATA_DIR`, `/usr/share/openchime/voices` (where the packages put it)
  and `voices/` beside the executable (where the tarball and a source build put it). They are
  mapped and used in place: ONNX Runtime reads the weights straight from the file's pages and
  ttskit binary-searches its tables there, so they cost disk, and memory only for the pages a
  render touches — the same as when they were embedded, which is why moving them changed the
  binary's size and nothing else.
- **A manifest, checked at startup.** `make` has the daemon write `manifest` into the data
  directory — `openchimed --tts-manifest DIR` — naming the model version it was built with and
  every file's SHA-256, so the version is exactly the one compiled in. At startup the daemon
  checks it; data that is absent, belongs to another build, or has been altered or partly
  copied turns read-aloud off with the reason in the log, and the daemon runs on without it.
- **One render worker thread.** The net loop puts a request (message handle, voice, speakable
  text) on a bounded queue (`OPENCHIME_TTS_QUEUE`, default 256; full answers
  `TTS_UNAVAILABLE`) and the worker wakes it through an eventfd when a render is stored, the
  pattern of the transfer pool.
- **Inside a render.** ttskit turns each sentence into phonemes; the phonemes become Kitten's
  token ids (`daemon/tts_kitten_tokens.c`, the model's own symbol table); ONNX Runtime runs
  the model with the voice's style vector at speed 1.2, one intra-op thread, arena on; the
  model's trailing 5,000 samples of silence are cut, leaving a natural pause; libopus encodes
  24 kHz mono at 24 kbps; the audio-only MP4 writer from ARCH-110 wraps it; the worker writes
  the file to the blob store and hands back the key, size and duration. The key is the
  handle's 64 hex digits and 16 of a hash of the model version (`daemon/tts_worker.c`), so
  a render is written once per version and a retry rewrites the same object.
- **Memory only while in use.** The ONNX Runtime session is created on the first render and
  released after `OPENCHIME_TTS_IDLE_S` (default 300) with no work, so a daemon whose users
  are not listening holds none of the model's working memory. Measured peak while rendering:
  about 210 MB for a typical sentence, up to 315 MB for a long one; loading takes about a
  quarter of a second.
- **Checking a build by ear:** `openchimed --tts-say VOICE TEXT OUT.wav` renders with the
  model in that binary and reports the real-time factor.

**Building it.** `make` builds ONNX Runtime once (`scripts/build_onnxruntime.sh`, about 20
minutes; CMake, a C++17 compiler and Python are build-time needs) and fetches and converts the
model once (`scripts/build_kitten.sh`, pinned by SHA-256; the conversion uses the prebuilt
full ONNX Runtime, a build tool only). The converted model is not byte-reproducible — the
converter orders part of its header differently between runs — so the input model and the
converter are what is pinned. `make TTS=0` builds a daemon without read-aloud. The operator
config is regenerated with ONNX Runtime's `create_reduced_build_config.py --format ORT
--enable_type_reduction` when the model changes, from the model as converted on **both**
architectures: on arm64 ONNX Runtime keeps `MatMul` and `Add` in half precision, where amd64
casts them to float, so a config taken from one conversion leaves the other unable to load.

## 5. Render and cache in the daemon

- **The handle** is SHA-256 of the speakable text, a separator, and the voice index. The
  cache key is `(handle, model_version)`; the model version names the language, the model,
  the ttskit data and the speaking rate together, so changing any of them re-renders — and
  two languages cannot collide on one row, which is why no part of the language needs to be
  in the handle.
- **A request** checks read access to the message's channel, builds the speakable text,
  and looks the key up. A hit streams the stored render. A miss queues a render, unless one
  for the same key is already in flight, in which case the request waits for that one. A
  finished render writes the cache row and starts every waiting stream.
- **Storage.** `rendered_audio` rows point at blobs. The maintenance pass reclaims renders
  before any attachment tier, least recently used first, deleting the row and the blob
  with no tombstone. A reclaimed render is synthesized again when next asked for.
- **Limits.** `AUDIO_GET` is rate-limited per connection (`OPENCHIME_TTS_RATE`), since a
  client asking for a whole history at once is a cheap way to occupy the render worker.
- **Pre-render** (`OPENCHIME_TTS_PRERENDER`, off by default) queues new messages in
  channels marked listenable at low priority, so a backlog is ready when someone opens it.

**Storage arithmetic.** At 24 kbps a second of speech is 3 KB; a typical 100-character
message is about 5 seconds, 15 KB. A thousand rendered messages is about 15 MB.

## 6. The wire

Protocol version 16; full layouts in PROTOCOL.md §5.14b.

- **`CAPABILITIES` (S→C, `0x00E1`)** after authentication: the features this daemon offers, by
  name. `tts` is present exactly when read-aloud is running — built in, turned on, and its data
  found and verified. A client told no shows nothing of the feature.
- **`TTS_INFO` (S→C, `0x00DC`)** after it: the model version, the voices (id, label, language)
  and the audition sentence. Always sent, empty when read-aloud is not offered; whether it is
  offered is `CAPABILITIES`' to say, so there is one answer rather than two.
- **`AUDIO_GET` (C→S, `0x00DD`)** `{message_id}` → **`AUDIO_INFO` (`0x00DE`)**
  `{message_id, duration_ms, total_size}`, **`AUDIO_CHUNK` (`0x00DF`)**, **`AUDIO_END`
  (`0x00E0`)** — the attachment download's shape, backpressure and gate.
- **`VOICE_PREVIEW_GET` (C→S, `0x00E2`)** `{voice_id}` → the same `AUDIO_INFO` /
  `AUDIO_CHUNK` / `AUDIO_END`, with message id 0: the preview sentence in that voice,
  rendered once and cached for everyone.
- **Errors:** `3023 NOT_RENDERABLE` (nothing to say), `3024 TTS_UNAVAILABLE` (read-aloud off,
  or the queue is full, or the render failed).
- **Profile:** `SET_PROFILE`, `PROFILE_INFO` and `USER_LIST` carry `voice_id`.

## 7. The client

- **Playback.** `oc_mp4` reads an audio-only file and `player.c` plays it with no video
  decoder; the audio clock drives it as for a video message.
- **The listen queue** in the client core: from a starting message, fetch and play each
  message in order, skipping `NOT_RENDERABLE`, pausing on `TTS_UNAVAILABLE` with a notice,
  and appending messages that arrive while it plays.
- **Win32:** "Listen" in the channel header and "Listen from here" on a message; a
  mini-player bar with play/pause, skip, stop and the current author; the voice picker
  in Edit profile, where choosing a voice plays the preview sentence in it before the
  profile is saved.
- **TUI:** `/listen` and `/listen stop`, playing through the same queue (ARCH-75 exempts
  a terminal from graphics, not from audio).

## 8. Tests

| Where | What it proves |
|---|---|
| `make test` — ttskit | pronunciation, the data files, the guesser against its reference (TTSKIT.md §8) |
| `make test` — speakable | code blocks, links, mentions, markdown, emoji, quotes, not-renderable |
| `make test` — render worker | with a stub synthesizer: queue order, a full queue, eventfd wake-up, idle release; Kitten's token ids against its own tokenizer |
| `make test` — daemon | cache hit and miss, in-flight de-duplication, handle stability and edit → new handle, reclaim first with no tombstone, the rate limit, protocol v13 round trips, a two-client vertical |
| `make test` — client core | an audio-only MP4 round trip; the listen queue in order, skipping not-renderable |
| Build job | the real daemon: links statically (only glibc and SQLite dynamic), renders a reference sentence with the expected duration and level; manual listening on model changes |
| Win32 harness | Listen plays a channel in order with voice changes; pause, skip, stop; absent with read-aloud off |
