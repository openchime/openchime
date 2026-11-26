# OpenChime — Voice input

How a user speaks into a conversation and has it land as text. Cross-referenced from
ARCHITECTURE.md (ARCH-112), REQUIREMENTS.md (§6.5, REQ-296–300), PROTOCOL.md
(§5.14c), AUDIO.md (§3, §6) and VENDORS.md (§1, §2).

Voice input produces **text messages**, nothing else. No audio is stored, attached or
posted; a spoken message is an ordinary message — search, mentions, edit and delete
all work. It is not a clip (REQ-273) and not a call (REQ-150).

The client is the Win32 GUI and the web client (WEB.md); the TUI has no voice input.

---

## 1. Two modes

In the composer of the conversation on screen — a channel, a DM or a thread:

- **Push to talk.** Hold the talk key or the microphone button, speak, let go. The
  words land in the composer at the caret, unsent; the user corrects them and sends
  with an ordinary `SEND`.
- **Free talk.** Turn it on and talk. Speech is cut at pauses and each piece is
  **posted by the daemon** as a message from the speaker as soon as it is
  recognized, in the order spoken. It stays on until turned off or until the user
  leaves the conversation. A wrong message is edited or deleted like any other.

Absent where the operator turned it off or the recognizer's data is missing (REQ-300).

## 2. The microphone

- **One owner at a time.** Voice input, video recording and a call never hold
  the microphone together: the device layer refuses a second capture
  (`OC_AUDIO_BUSY`). Opening the video recorder ends voice input, and the recorder's
  overlay covers the composer, so voice input cannot start under it. Joining a call
  ends both; in a call, the talk key is the call's push to talk.
- Opened only while in use — press to release, or free talk on to off — with a
  live-microphone mark on the composer. A microphone the OS blocks is reported as
  blocked by Windows' privacy settings, or in the browser by its site settings (REQ-166).
- The device is the microphone chosen for video messages, else the default. Captured
  through the device layer (`client/core/media/audio_dev.c`) at **16 kHz mono,
  16-bit**, the recognizer's input format. Nothing is written to disk (ARCH-88).

## 3. Echo cancellation

The client's own playback — read-aloud, video messages, voice auditions — must never
be heard as speech. All of it plays through the device layer, which records the frames
it emits against the media clock; that playback stream is the **far-end reference**.
Capture passes through the `oc_audio_processor` seam (AUDIO.md §3.3) running
**speexdsp**'s linear echo canceller before the detector sees it. The call engine
uses both.

The ERLE harness (AUDIO.md §6.4) measures it; speexdsp's residual-echo suppressor is
left off because it deletes the near-end voice in double-talk.

## 4. Where an utterance ends

**libfvad** (BSD-3-Clause, pure C, vendored, client only) classifies each 20 ms frame
as speech or not; its first 200 ms are ignored while it settles. A segment opens on
60 ms of speech with 300 ms of pre-roll so the first syllable survives, closes after
700 ms of quiet, is dropped if it holds under 400 ms of speech, and is cut at a
200 ms pause before the daemon's cap — **30 s** by default, the limit the recognizer's
authors recommend — or at the cap if there is none. In push to talk the release ends the
segment; the detector drops only a press holding under 160 ms of speech, and cuts
inside the cap.

## 5. The wire

Layouts in PROTOCOL.md §5.14c. A segment is 16 kHz mono 16-bit PCM in chunks over the
existing TLS connection — 32 KB per second of speech.

- `CAPABILITIES` carries `stt` when voice input is running; `STT_INFO` gives the model
  version, language and segment cap.
- `STT_BEGIN` names the segment, its **mode**, its **target** (channel, thread root)
  and an **idempotency token**; `STT_CHUNK`s and `STT_END` carry the samples.
- **Free talk:** the daemon posts the recognized text through the ordinary send path
  as the speaker — the way scheduled messages fire (ARCH-102) — so mentions,
  notifications, unfurls, the send rate limit and idempotency behave as for typed
  text, and every member gets the usual `BROADCAST`. No text travels back to the
  client to be re-sent.
- **Push to talk:** the text comes back to the speaker's connection only.
- `STT_TEXT` closes every segment: its text and the posted `message_id` (0 in push to
  talk, or when nothing was said). Segments are answered and posted in the order
  sent; the client may send the next while earlier ones are still being recognized.

Not the UDP call relay (a lost packet is a silently missing word) and not Opus (a codec
in the client and a lossy input to the recognizer).

## 6. Recognition in the daemon

**The recognizer** — the smallest reliable English model we found (its source, licence
and how its data file is made are in VENDORS.md). Figures are its authors' (their
repository, 2026-08-24):

| | The recognizer | Whisper Tiny |
|---|---|---|
| Parameters | 34 million | 39 million |
| Open ASR Leaderboard average WER | 12.00% | 12.81% |
| LibriSpeech test-clean WER, shipped int8 model | 4.83% | — |
| Latency, Linux x86 (authors' benchmark) | 69 ms | 1,141 ms |

Compute follows the length of the audio, where Whisper always encodes 30 seconds. It is an
encoder-decoder transformer with int8 weights, run by **sttkit in plain C** on nnkit's
kernels (STTKIT.md): one committed file, `sttkit/data/recognizer.bin` (about 40 MB), made
from the published files by `openchimed --recognizer-pack`. Nothing is downloaded or built
for it outside the tree.

**Measured on the daemon** (x86-64, one core, i5-1335U): the model maps in no measurable
time on the first segment; recognition takes about 0.25× the length of the speech
(`stt_check bench`: 0.16–0.29× over the reference fixtures); one utterance of six seconds
peaks at about 52 MB resident, 40 MB of it the mapped `recognizer.bin`, which the kernel
may drop. An idle daemon holds 0.1 MB more with voice input built in than without it.

**Long segments are heard in windows.** The working memory grows with what the model
hears at once: when the recognizer ran on its previous engine, on a 256 MB machine a 20 s segment
left 3 MB free and a 30 s one took 413 s, the kernel re-reading the model from disk
(TESTING.md, "Speech models' memory"). sttkit needs far less, but the windows stay: they
bound the work and memory of one pass whatever the segment.
So a segment longer than `OC_STT_WINDOW_SECS` (6 s) is heard in windows of at most that,
each ending at the quietest 20 ms within `OC_STT_CUT_SEARCH_MS` (1.5 s) of its limit, and
the windows' words are joined with one space (`oc_stt_hear_windowed`,
`daemon/stt_worker.c`). The daemon's worker and `openchimed --stt-hear` hear the same way.

- **In C, in sttkit** (STTKIT.md), with read-aloud's kernels (nnkit, shared with
  ttskit): no inference library, one thread, no intrinsics. Its arithmetic is the
  published graphs' as the reference engine computed them, to the bit, so it hears what the
  model was measured to hear (STTKIT.md §4). Decoding stops at the end token or at the
  model's tokens-per-second ceiling, its guard against a decoder repeating itself.
- **A worker of its own** beside the render worker: a bounded queue, an eventfd back
  to the net loop, one thread, the model opened on first use and released when
  idle. The loop only copies a segment's bytes as they arrive; the worker puts
  the little-endian samples in host order before hearing them.
- **Its own data directory and manifest** — `/usr/share/openchime/stt`, else
  `stt/` beside the executable — so missing data
  turns voice input off without touching read-aloud. `make openchimed STT=1` builds it in
  (CONFIG.md "Build switches").
- **Spoken mentions:** before posting or returning, "at" followed by a name becomes
  `@name` when it names exactly one member of the channel; the daemon holds the
  roster (ARCH-89).
- **Audio is never kept:** a segment's samples are freed once answered, never written
  or logged.

**Limits.** The one setting is `OPENCHIME_STT` (on; CONFIG.md). The rest are
constants (`daemon/config.h`): `OC_STT_QUEUE` 64, `OC_STT_IDLE_SECS` 300,
`OC_STT_SEGS_PER_MIN` 60 per connection, `OC_STT_MAX_SECS` 30 — the model's
own limit.

## 7. The graphical client

- **App-core:** `client/core/voice/` — the dictation session and the libfvad
  segmenter, over the speexdsp processor in `client/core/media/processor.c` — linked by the Win32 client and, the same code, by the
  web client (WEB.md). Segments go out beside,
  not behind, the transfer queue.
- **Composer:** after @, a microphone button (hold to talk) and a free-talk toggle;
  nothing at all without `stt`, in the New message pane, or under the video recorder.
  Free talk is not offered in an archived channel. Live, the microphone is filled red
  and free talk the accent colour, ringed while speech is heard.
- **Keys:** hold **Ctrl+Shift+Space** to talk (letting go of Space ends it, and its
  auto-repeat is never typed); **Ctrl+Shift+T** turns free talk on and off. Both are in
  the shortcuts list.
- Push-to-talk text is inserted at the caret, spaced from the word before it, as one
  undo step per piece. The session ends — dropping what was being said — when the
  conversation on screen changes, and a hold ends when the window loses focus.
  Refused segments are said once each as a toast.
- **Accessibility** (REQ-290): `composer.mic` and `composer.freetalk`, with their
  state in the name. Invoke cannot be held, so invoking the microphone starts talking
  and invoking it again stops.

## 8. Later: recognition in the client

A capable device may recognize locally with a better model and post through `SEND`,
sending no segment; the wire and the daemon need no change. The daemon's recognizer
stays for clients that cannot carry a model.

## 9. Tests

| Where | What it proves |
|---|---|
| `make test` — segmenter | synthetic voiced sound with known pauses cuts where it should, in both modes and at the cap |
| `make test` — canceller | the ERLE harness: echo removed, re-convergence under drift, near-end speech kept in double-talk |
| `make test` — recognition worker | stub engine: order, full queue, idle release, the token ceiling |
| `make test` — daemon | the `STT_*` frames; the capability and cap; order, cap, rate, a chunk out of order, a failed recognition; off, and no engine as with missing data; free talk posts through the send path, idempotently, refused for an archived channel or a non-member, and never after its speaker has left; mention normalisation |
| Full suite — client core | free talk arrives as `BROADCAST`s in order with no client `SEND`; push to talk's words wait for the composer they were spoken into, and nothing is sent; one microphone owner |
| Full suite (`make test-all`, in the release-candidate), stripped and packaged | read-aloud renders a sentence and `openchimed --stt-hear` must hear its words — built, stripped, and installed from the `.deb` |
