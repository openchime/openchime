# OpenChime — Read-aloud

How a channel is read aloud: what a listener gets, how a message becomes speakable text,
how the daemon renders it with the voice model installed beside it, caches and serves it, and how a client
plays a channel in order. This is the authoritative design; it is cross-referenced from
ARCHITECTURE.md (ARCH-111), REQUIREMENTS.md (§6.4, REQ-291–295), PROTOCOL.md (§5.14b),
SCHEMA.md (migration 0042) and CONFIG.md. Pronunciation — turning words into the phonemes
a voice model reads — and the voice model itself are run by ttskit, an in-tree C library
described in [TTSKIT.md](./TTSKIT.md).

Read-aloud renders **text that is already a message**. Nothing is recorded, posted or
stored as a message; a reader and a listener have the same conversation. It is not a
clip (REQ-273) and not a call (REQ-150).

---

## 1. What a listener can do

- **Listen to a conversation as it happens.** Turned on for the conversation on screen,
  each message that arrives from then on and has something to say plays in order, in its
  author's voice, until it is turned off or the listener goes elsewhere. It reads forward,
  not back: there is no starting from an earlier message, no pause and no skip, and a
  listener's own messages are not read to them.
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
| A table | "Table, 3 rows." — the cells are skipped |
| `inline code` | the contents, as written |
| A URL | its host, without `www.` — "github.com" |
| `@someone` | their display name |
| `@here` / `@channel`, `@everyone` | "everyone here" / "everyone" |
| `*bold*`, `_italic_`, `~strike~`, list markers, escapes | the text, markers dropped |
| A quoted line | "Quote:" once per quote, then the text |
| Emoji and `:shortcodes:` | dropped |
| A line break | the end of a sentence |
| An action, `/me is away` (REQ-058) | the actor's name, then the action — "Ada Starr is away" |

Numbers, times, currency, ordinals and common abbreviations are expanded to words by
ttskit (TTSKIT.md §4), not here. A message that leaves nothing to say — a bare attachment,
only emoji — is **not renderable** (REQ-294).

Speakable text is split at sentence ends into pieces of at most 120 characters; the pieces
are synthesized in turn and joined into one file, so a long message never holds more of the
model's working memory than a sentence needs: it is allocated for each piece and freed
when the piece is done.

## 3. Voices

**One language per deployment.** The daemon speaks the language its engine was
built for — `en-US` (`OC_TTS_LANG`), and there is no setting to ask for another:
a binary speaks the one it carries. The tag is stamped into the
pronunciation data (a lexicon of one language with a guesser of another
pronounces fluent nonsense, so the pair is checked), it is part of the model
version that keys the cache, and it is announced with every voice on `TTS_INFO`.
A voice belongs to a language: a second language is a second engine beside this
one, not a setting on it.

The voice model (Apache-2.0; VENDORS.md names its source) has eight voices. A user's voice is
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

- **What is built in.** ttskit (TTSKIT.md), which turns text into phonemes and runs the
  voice model in plain C on one thread, with no inference library: read-aloud links no ONNX
  Runtime and no C++ (voice input's recognizer has its own, VOICE-INPUT.md §6). And libopus,
  to encode the audio. The daemon is under 9 MB.
- **What is beside it.** Four files, about 107 MB, in a data directory: the first of
  `/usr/share/openchime/tts` (where the packages put it) and `tts/` beside the executable
  (where the tarball and a source build put it).

  | File | What | Copied from |
  |---|---|---|
  | `voice.bin` | the voice model: its weights and its eight voices' style tables, 86.9 MB (TTSKIT.md §6) | `ttskit/data/voice.bin` |
  | `VOICE-LICENSE` | the voice model's licence, Apache-2.0 | `ttskit/data/VOICE-LICENSE` |
  | `en-US/lexicon.bin` | the pronunciation dictionary | `ttskit/data/en-US/` |
  | `en-US/guesses.bin` | the guesser for words not in it | `ttskit/data/en-US/` |

  The `manifest` (below) is beside them. They are mapped and used in place: the voice model's kernels
  read the weights straight from the file's pages and ttskit binary-searches its tables
  there, so they cost disk, and memory only for the pages a render touches — the same as
  when they were embedded, which is why moving them changed the binary's size and nothing
  else.
- **A manifest, checked at startup.** A read-aloud build (`make run TTS=1`, or `make openchimed tts/manifest TTS=1`) has the daemon write `manifest` into the data
  directory — `openchimed --tts-manifest DIR` — naming the model version it was built with and
  every file's SHA-256, so the version is exactly the one compiled in. At startup the daemon
  checks it; data that is absent, belongs to another build, or has been altered or partly
  copied turns read-aloud off with the reason in the log, and the daemon runs on without it.
- **One render worker thread.** The net loop puts a request (message handle, voice, speakable
  text) on a bounded queue (`OC_TTS_QUEUE`, 256; full answers
  `TTS_UNAVAILABLE`) and the worker wakes it through an eventfd when a render is stored, the
  pattern of the transfer pool.
- **Inside a render.** ttskit turns each sentence into phonemes; the phonemes become the model's
  token ids (`daemon/tts_voice_tokens.c`, the model's own symbol table); ttskit runs the
  model (`voice_say`, TTSKIT.md §5b) on those ids with the voice's style row for the
  sentence's length in characters, at speed 1.2, with its noise drawn from the one fixed
  seed (`TTS_VOICE_SEED`), so the same text in the same voice is the same audio every time
  and on every machine (`daemon/tts_voice.c`); a 20 Hz high pass takes out the model's offset, and the noise it appends after the words is
  cut at the first quiet moment no earlier than its last 5,000 samples, with a 10 ms fade so
  the cut cannot click; libopus encodes
  24 kHz mono at 24 kbps; the audio-only MP4 writer from ARCH-110 wraps it; the worker writes
  the file to the blob store and hands back the key, size and duration. The key is the
  handle's 64 hex digits and 16 of a hash of the model version (`daemon/tts_worker.c`), so
  a render is written once per version and a retry rewrites the same object.
- **Memory only while in use.** The engine maps its files on the first render and
  unmaps them after `OC_TTS_IDLE_SECS` (300) with no work, and a render's working memory
  is allocated for each sentence and freed after it, so a daemon whose users are not
  listening holds none of it. Measured on one core of an i5-1335U, one render peaks at about 156 MB resident,
  about 86 MB of it `voice.bin`'s mapped pages, which the kernel can drop and read again.
- **Speed.** A render takes 0.83–0.87× the length of its audio on that core
  (`--tts-say`, GCC), and about 0.99× in the release build: faster than real time, on one
  thread.
- **Checking a build by ear:** `openchimed --tts-say VOICE TEXT OUT.wav` renders with the
  model in that binary and reports the real-time factor.

**Building it** (`make openchimed TTS=1`, CONFIG.md "Build switches") compiles ttskit and Opus
into the daemon and copies the four files from `ttskit/data/` into `tts/` beside it, with the
manifest; nothing is downloaded and nothing but a C compiler is needed. All four are
committed. `voice.bin` is made from the upstream model by `openchimed --voice-pack`, the same
bytes on every run and machine; VENDORS.md gives the pinned inputs and the commands that
recreate it byte for byte.

**The model version** renders are cached under is
`en-US/voice-088b5443/ttskit-b1338cd8.51cf60b3/rate-1.2/style-chars/tail-quiet/seed-1`
(`OC_TTS_MODEL_VERSION`, `daemon/tts.h`): the language; the voice model, by the first eight
hex digits of `voice.bin`'s SHA-256; the pronunciation data, likewise; the speaking rate;
the style row chosen by characters; the quiet-seeking tail trim; and the noise seed.
Moving the model from ONNX Runtime into ttskit changed the version — the model's part now
names the data file's hash, and the seed was added — so every render made before it is a
cache miss and is made again when next asked for, in ttskit, and the old rows, never hit
again, are the least recently used and the first reclaimed (§5). The auditions are warmed
again at the first start (§5).

## 5. Render and cache in the daemon

- **The handle** is SHA-256 of the speakable text, a separator, and the voice index. The
  cache key is `(handle, model_version)`; the model version names the language, the model,
  the ttskit data, the speaking rate and the noise seed together (§4), so changing any of
  them re-renders — and
  two languages cannot collide on one row, which is why no part of the language needs to be
  in the handle.
- **A request** checks read access to the message's channel, builds the speakable text,
  and looks the key up. A hit streams the stored render. A miss queues a render, unless one
  for the same key is already in flight, in which case the request waits for that one. A
  finished render writes the cache row and starts every waiting stream. Until no lookup can
  miss that row — it is written, and every lookup sent before it was has answered — the
  daemon keeps the finished render in memory and serves it from there, every one however
  deep the writer's queue: a request between the render and its row never renders again.
- **The auditions are warmed at startup.** All eight voices audition with the same one
  sentence, so there are exactly eight renderings to have, and the daemon checks for them
  when it starts — rendering only what is missing, through the ordinary preview path, so a
  restart costs one database read per voice and a fresh deployment costs about 20 seconds
  of the render worker once. **On an i5-1335U** an audition is 2.5–3.5 s of audio and
  renders at 0.83–0.87× real time, so a cold one takes about 2–3 s; warmed, the press of
  **Play** to the first sound was measured at **109–235 ms**, which is a database read, a
  blob open and the transfer. The rendering is the whole wait, which is
  why the answer is to have done it already rather than to make it faster.
- **Storage.** `rendered_audio` rows point at blobs. The maintenance pass reclaims renders
  before any attachment tier, least recently used first, deleting the row and the blob
  with no tombstone. A reclaimed render is synthesized again when next asked for.
- **Limits.** `AUDIO_GET` is rate-limited per connection (`OC_TTS_GETS_PER_MIN`, 60), since a
  client asking for a whole history at once is a cheap way to occupy the render worker.

**Storage arithmetic.** At 24 kbps a second of speech is 3 KB; a typical 100-character
message is about 5 seconds, 15 KB. A thousand rendered messages is about 15 MB.

## 6. The wire

Introduced in protocol version 13 (`TTS_INFO`, `AUDIO_*`, the voice on profile frames); voice
languages came in 14, `CAPABILITIES` in 15 and `VOICE_PREVIEW_GET` in 16 (`shared/protocol.h`).
Full layouts in PROTOCOL.md §5.14b.

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
  or the queue is full, or the render failed). A daemon built without read-aloud answers
  `AUDIO_GET` and `VOICE_PREVIEW_GET` the same way as one built with it and turned off:
  `TTS_UNAVAILABLE`, naming the message asked about (0 for a preview), and the connection
  stays open.
- **Profile:** `SET_PROFILE`, `PROFILE_INFO` and `USER_LIST` carry `voice_id`.

## 7. The client

- **Playback.** `oc_mp4` reads an audio-only file and `player.c` plays it with no video
  decoder; the audio clock drives it as for a video message.
- **The listen queue** in the client core: messages that arrive in the listened-to
  conversation, other than one's own, are queued (at most 64, dropping the oldest) and
  fetched and played in order, skipping any answered `NOT_RENDERABLE` or `TTS_UNAVAILABLE`.
- **GUI:** a read-aloud toggle in the conversation header, showing how many messages are
  queued, turned off when the listener leaves the conversation; the message being spoken
  is banded in the transcript; the voice picker in
  Edit profile, with a **Play** button beside it that says the audition sentence in the
  chosen voice. Play, not pick-to-play: walking down a list of eight voices with the
  keyboard would otherwise fire eight requests, and a sample that starts on its own is a
  surprise. While one is playing the button reads "Playing…" and does not take a second
  press; it comes back on its own if the daemon never answers.

## 8. Tests

| Where | What it proves |
|---|---|
| `make test` — ttskit | pronunciation, the data files, the guesser against its reference (TTSKIT.md §9); the voice model's token ids against its own tokenizer; its kernels against naive versions, `voice.bin`'s loader and converter, and the committed `voice.bin` |
| `make test` — speakable | code blocks, links, mentions, markdown, emoji, quotes, not-renderable |
| `make test` — render worker | with a stub synthesizer: queue order, a full queue, eventfd wake-up, idle release |
| `make test` — daemon | cache hit and miss, in-flight de-duplication, handle stability and edit → new handle, reclaim first with no tombstone, the rate limit, round trips of the read-aloud frames at the current protocol version, a two-client vertical |
| Full suite — client core | an audio-only MP4 round trip; the listen queue in order, skipping not-renderable |
| Full suite — `voice_parity` | the voice model against ONNX Runtime's reference renderings (`tests/data/voice/`, made by `tests/manual/voice_ref.c`): with the noise off, every fixture's sample count and at least 40 dB SNR; with it on, the noise's variance within 10% (TTSKIT.md §9) |
| Full suite (`make test-all`, in the release-candidate) | the real daemon: links statically (only the C runtime dynamic); the release binary renders a sentence (`--tts-say`) that voice input hears back (`--stt-hear`); manual listening on model changes |
| Win32 harness | no scripted Listen run. The harness's `voicepreview` verb asks for a voice's audition and reports how long it took (`preview wait_ms=`), and `tools/gui/features/voice.sh` has the real daemon render a sentence (`--tts-say`) that the client's synthetic microphone then speaks |
