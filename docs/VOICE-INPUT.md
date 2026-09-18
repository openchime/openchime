# OpenChime — Voice input

How a user speaks into a conversation and has it land as text. Cross-referenced from
ARCHITECTURE.md (ARCH-112), REQUIREMENTS.md (§6.5, REQ-296–300), PROTOCOL.md
(§5.14c), AUDIO.md (§3, §6) and VENDORS.md (§7).

Voice input produces **text messages**, nothing else. No audio is stored, attached or
posted; a spoken message is an ordinary message — search, mentions, edit and delete
all work. It is not a clip (REQ-273) and not a call (REQ-150).

**Not built.** REQ-296–300 carry the marker. The client is the Win32 GUI; the TUI has
no voice input.

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

- **One owner at a time.** Voice input, video recording and (later) a call never hold
  the microphone together; starting one ends the other.
- Opened only while in use — press to release, or free talk on to off — with a
  live-microphone mark on the composer. A microphone the OS blocks is reported as
  that, with the way to the privacy setting, as for a video message (REQ-166).
- Captured through the device layer (`client/core/media/audio_dev.c`) at **16 kHz
  mono, 16-bit**, the recognizer's input format. Nothing is written to disk
  (ARCH-88).

## 3. Echo cancellation

The client's own playback — read-aloud, video messages, voice auditions — must never
be heard as speech. All of it plays through the device layer, which records the frames
it emits against the media clock; that playback stream is the **far-end reference**.
Capture passes through the `oc_audio_processor` seam (AUDIO.md §3.3) running
**speexdsp**'s echo canceller before the detector sees it. The ERLE harness
(AUDIO.md §6.4) is built first and measures it. The call client reuses both.

## 4. Where an utterance ends

**libfvad** (BSD-3-Clause, pure C, vendored, client only) classifies each 20 ms frame
as speech or not. A segment opens on a short run of speech with a pre-roll so the
first syllable survives, closes after a pause long enough not to be a breath, is
dropped if it holds too little speech, and is cut at a pause before **30 s** — the
limit Moonshine recommends — or at 30 s if there is none. In push to talk the release
ends the segment; the detector only drops an empty press and cuts inside the cap.

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

Not the UDP sidecar (a lost packet is a silently missing word) and not Opus (a codec
in the client and a lossy input to the recognizer).

## 6. Recognition in the daemon

**Moonshine Tiny Streaming, English (MIT)** — the smallest reliable model that runs on
ONNX Runtime. Figures are the authors' (their repository, 2026-08-24) and are
re-measured on the daemon when built:

| | Moonshine Tiny Streaming | Whisper Tiny |
|---|---|---|
| Parameters | 34 million | 39 million |
| Open ASR Leaderboard average WER | 12.00% | 12.81% |
| LibriSpeech test-clean WER, shipped int8 model | 4.83% | — |
| Latency, Linux x86 (authors' benchmark) | 69 ms | 1,141 ms |

Compute follows the length of the audio, where Whisper always encodes 30 seconds. The
published files are int8-quantized `.ort` — `frontend.model.ort`,
`frontend.weights.ort`, `encoder.ort`, `adapter.ort`, `cross_kv.ort`,
`decoder_kv.ort`, `tokenizer.bin`, `streaming_config.json`, about 45 MB — fetched at
pinned SHA-256s by `scripts/build_moonshine.sh` from Moonshine's dated CDN directory
(`quantized_26_08_21`). The build confirms they load in the daemon's ONNX Runtime
1.30.0; if they do not, they are converted from Moonshine's float weights and
quantized per channel as Moonshine does.

- **In C, on the runtime read-aloud links** (ARCH-111): the daemon runs the graph chain
  through ONNX Runtime's C API, as it runs Kitten, sharing one `OrtEnv`; the operator
  config is the union of both models'. `tokenizer.bin` is read in place; decoding
  stops at the end token or at Moonshine's tokens-per-second ceiling, its guard
  against a decoder repeating itself.
- **A worker of its own** beside the render worker: a bounded queue, an eventfd back
  to the net loop, one intra-op thread, the session opened on first use and released
  when idle.
- **Its own data directory and manifest** — `OPENCHIME_STT_DATA_DIR`, else
  `/usr/share/openchime/stt`, else `stt/` beside the executable — so missing data
  turns voice input off without touching read-aloud. `make STT=0` builds without it.
- **Spoken mentions:** before posting or returning, "at" followed by a name becomes
  `@name` when it names exactly one member of the channel; the daemon holds the
  roster (ARCH-89).
- **Audio is never kept:** a segment's samples are freed once answered, never written
  or logged.

**Limits** (added to CONFIG.md when built): `OPENCHIME_STT` (on), `OPENCHIME_STT_QUEUE`
(64), `OPENCHIME_STT_IDLE_SECS` (300), `OPENCHIME_STT_RATE` (60 segments per
connection per minute), `OPENCHIME_STT_MAX_SECS` (30), `OPENCHIME_STT_DATA_DIR`.

## 7. The Win32 client

- **App-core:** `client/core/voice/` — the dictation session, the libfvad segmenter
  and the speexdsp processor — linked by the Win32 client. Segments go out beside,
  not behind, the transfer queue.
- **Composer:** a microphone button (hold to talk) and a free-talk toggle, a held
  shortcut and a toggle shortcut, and the live mark; nothing at all without `stt`.
  Push-to-talk text is inserted at the caret as one undo step per piece.

## 8. Later: recognition in the client

A capable device may recognize locally with a better model and post through `SEND`,
sending no segment; the wire and the daemon need no change. The daemon's recognizer
stays for clients that cannot carry a model.

## 9. Tests

| Where | What it proves |
|---|---|
| `make test` — segmenter | a PCM fixture with known pauses cuts where it should |
| `make test` — canceller | the ERLE harness: echo removed, re-convergence under drift, near-end speech kept in double-talk |
| `make test` — recognition worker | stub engine: order, full queue, idle release, the token ceiling |
| `make test` — daemon | the `STT_*` frames, order, cap, rate, off and missing data; free talk posts through the send path, idempotently, refused for an archived channel or a non-member; mention normalisation |
| `make test` — client core | free talk arrives as `BROADCAST`s in order with no client `SEND`; push to talk inserts and never sends; one microphone owner |
| Build job | read-aloud renders a sentence and `openchimed --stt-hear` transcribes it back |
| Win32 harness | both modes on synthetic audio; absent with voice input off; microphone denied |
