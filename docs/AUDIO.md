# OpenChime — Audio

How a voice call's audio works: the media path, the client audio engine, and
acoustic echo cancellation. The feature — the Calls section, invitations, ending,
missed calls — and the end-to-end encryption are [CALLS.md](./CALLS.md). This is
cross-referenced from ARCHITECTURE.md (ARCH-18, ARCH-28, ARCH-31, ARCH-73,
ARCH-113), REQUIREMENTS.md (§6.2, REQ-150–152, REQ-301–306), PROTOCOL.md (§5.17),
and CLIENT.md.

**This document is audio only.** **Screenshare is [VIDEO.md](./VIDEO.md)**
(REQ-161, ARCH-86/87) — it rides this same call, sidecar, and UDP path, and
builds on the media transport, jitter buffer and device layer of §§2–4. Camera
video remains out of scope (REQ-160).

**Two halves.** The daemon's signaling, its ephemeral call state and
the forked UDP relay (`daemon/audio_sidecar.c`); the client's signaling and keys
in the core (`client/core/callsig.c`, on the network thread); and the media
engine (`client/core/call/`): capture through the echo canceller and speexdsp's
preprocessor, Opus, SFrame, the relay socket, a jitter buffer and decoder per
sender, and the mixer. The Win32 client carries it; the TUI has no calls (§8).

---

## 1. The model: one call per conversation

A conversation has at most **one call** at a time, with an ephemeral roster of
up to `OPENCHIME_CALL_MAX` participants (ARCH-73). Anyone who can read the
conversation may join it; starting one invites its members; it lasts while one
participant remains, or until its starter ends it; a dropped participant rejoins
with a fresh `CALL_JOIN` (CALLS.md §1).

**A 1:1 call is the degenerate case, not a separate feature.** A DM channel has
two members, so a call there is a two-person huddle over exactly the same code
path. There is no pairwise call model anywhere in the system, and none should be
added.

### 1.1 The server is an SFU — it never mixes

ARCH-18/73 forbid the server from decoding Opus, so the sidecar **forwards
opaque payloads** rather than mixing them. The media framing makes this visible
(`daemon/audio.h`):

```
client  → sidecar :  token ‖ seq(u16 BE) ‖ payload
sidecar → client  :  sender_user_id(u64 BE) ‖ seq(u16 BE) ‖ payload
```

The payload is an SFrame ciphertext (CALLS.md §5.4) — a typed plaintext: the
Opus frame and its frame number, a mute state, or a shared screen's fragments and
its viewers' requests (VIDEO.md §5), encrypted under the sender's key for the
epoch — or empty, a keep-alive. The relay sees neither the audio nor who is
speaking.

Every packet a client receives is tagged with **who sent it**. In a five-person
call a client receives up to four independent streams.

This single decision drives most of the client design below: **the client
decodes N streams and mixes them itself.** Opus decoders are stateful per
stream, so N participants means N decoder instances and N jitter buffers. That
is the largest piece of work in this document, and it is a consequence of a
server decision already made and shipped — not something to relitigate here.

It also has one convenient effect: the client mixes down to **one** playback
stream, and that mix is exactly the far-end reference the echo canceller needs
(§6). Mixing before cancellation means one reference signal, not N.

### 1.2 What the client does not have to build

Already done, server-side: the SFU relay, roster maintenance and fan-out,
per-join tokens, authorization via the ordinary channel-read gate, participant
drop on TCP disconnect, and the media-side silence timeout. NAT traversal also
falls out of the existing design — the client sends an initial packet (empty
payload allowed) so the sidecar learns its UDP source address, which creates the
outbound mapping.

---

## 2. Pipeline shape

```
          ┌──────────────── network (UDP, to/from sidecar) ────────────────┐
          │                                                               │
   ┌──────┴──────┐                                                 ┌──────┴──────┐
   │ Opus encode │ ← AEC-cleaned capture                mixed PCM →│ Opus decode │ × N
   └──────┬──────┘                                                 └──────┬──────┘
          │                                                               │
   ┌──────┴───────────────────────────────────────────────────────────────┴──────┐
   │  audio engine (capture + playback devices, aligned by the reference)        │
   │     capture ──→ [ AEC ] ──→ [ NS/AGC ] ──→ gate ──→ encode ──→ SFrame       │
   │     playback ←── mixer ←── jitter buffers ←── decode                        │
   │                     └────────────────────────→ AEC far-end reference        │
   └─────────────────────────────────────────────────────────────────────────────┘
```

**16 kHz mono, 20 ms frames (320 samples).** Opus encodes wideband natively at
16 kHz; the AEC's adaptive filter costs roughly a third of what it would at
48 kHz and converges faster; and 20 ms is the standard voice frame. The tradeoff
is wideband rather than fullband audio — clearly better than a phone call,
short of music-grade. For a work huddle that is the right trade, and it is the
single cheapest lever on echo-canceller cost.

Capture and playback are opened as **two devices**, and aligned through the
playback reference: the device layer records every frame a playback device hands
out against the media clock, and the canceller takes from it the frames played at
the instant a capture frame was taken (§3.3, `oc_audio_reference`). A device's
own **duplex mode** — both directions in one callback — would give that
alignment by construction, which is why it is worth understanding what it does
and does not buy.

**Duplex gives one callback, not necessarily one clock.** That distinction
matters and is easy to get wrong. On most platforms capture and playback are
*separate objects* even for a single piece of hardware: PulseAudio and PipeWire
expose a sink and a source, and **WASAPI has no duplex device concept at all** —
you open a render endpoint and a capture endpoint. Only ALSA against a single
card, and CoreAudio with one device or an aggregate, give a genuinely shared
clock. So duplex mode buys **alignment** (both buffers in one callback, with a
known relationship), which is what the delay contract in §6.3 needs. It does not
by itself buy a shared clock.

What decides drift is the **clock domain**, and it correlates usefully with where
echo cancellation is actually needed:

| Setup | Clock | Is AEC needed? |
|---|---|---|
| Built-in laptop speaker + mic | **shared** (one codec chip) | **Yes — the case AEC exists for** |
| USB headset | shared (one USB device) | Barely — it is in your ears |
| Bluetooth headset | the headset's own | Barely — in your ears |
| USB mic + desk speakers | **two crystals** | Yes, and the worst case for drift |

The happy part: the configuration where AEC matters most (built-in laptop) is
also the one with a shared clock. The nasty case — a separate microphone and
speakers — is both the hardest for drift and a real setup people use.

So the engine does not assume the problem away:

- **It prefers a single physical device.** The defaults are the system's, which
  on a laptop are the built-in pair.
- **The canceller re-converges** under 100 ppm of drift (§6.4).

**Bluetooth deserves its own note**, because it fails in a way clocks do not
explain. A headset is one physical device, but audio rides two profiles: **A2DP**
(good stereo, *playback only, no microphone*) and **HSP/HFP** (bidirectional, but
8 kHz mono with CVSD, or 16 kHz with mSBC where both ends support it). The moment
the microphone opens, the stack switches A2DP → HFP and playback quality
collapses — on Linux automatically, since WirePlumber auto-switches on detecting
an input stream. This is unavoidable and is why every Bluetooth headset sounds
markedly worse on a call than on music. One convenient consequence: our 16 kHz
pipeline is exactly mSBC's rate, so on a wideband-capable headset we lose nothing
to our own choice — the Bluetooth link is the bottleneck, not us.

Finally, **it is expensive to retrofit.** Decoupled capture and playback paths are
the natural thing to build and the hard thing to undo; the engine keeps them
decoupled but aligned through the reference, which is what the canceller needs.

---

## 3. The audio engine

The engine owns **both directions**, independent of the network and of any
frontend, on the device layer in `client/core/media/audio_dev.{c,h}` (§3.2).

### 3.1 The real-time contract

The device callback runs on a real-time thread supplied by the audio backend.
**It must never allocate, never take a lock, and never perform I/O.** Violating
this produces crackling and dropouts that are consistently misdiagnosed as
network problems.

Everything crossing the boundary therefore goes through **lock-free single-
producer/single-consumer ring buffers**:

```
audio callback thread          media thread
  capture → [ring] ──────────────→ encode → UDP send
  playback ← [ring] ←────────────── mix ← decode ← UDP recv
```

The media thread does the work that can block — encoding, decoding, socket I/O,
and allocation. The callback only moves bytes. This mirrors the existing
net-thread / DB-writer split (ARCH-52): the thread with the hard timing
constraint does no work that can stall.

### 3.2 Device backend

**miniaudio**, vendored as a single header, matching the existing pattern
(termbox2, utf8proc, jsmn — ARCH-75). MIT-0/public-domain, so it does not
disturb the licensing stance that ruled out notcurses. It wraps ALSA,
PulseAudio, PipeWire, CoreAudio, and WASAPI behind one API, and — critically —
supports the duplex mode §2 requires, with resampling so the engine can request
16 kHz regardless of what the hardware prefers.

**The device layer** is shared with video messages (ARCH-110):
`client/core/media/audio_dev.{c,h}` enumerates capture and playback devices,
opens either at a requested rate and channel count, and joins each device
callback to its media thread with a lock-free single-producer ring. The callback
never allocates, locks or does I/O; captured samples are stamped from the media
clock video frames use; playback reports the samples the device has consumed,
which is the clock a player — or a call's jitter buffer — runs on. Video messages
record at 48 kHz mono; calls open it at 16 kHz. `OPENCHIME_TEST_AUDIO=synthetic`
swaps the devices for a tone source and a real-time sink, so both run in
`make test` on a machine with no sound hardware; with it, `OPENCHIME_TEST_MIC=<wav>`
makes the synthetic microphone speak a recording, and `OPENCHIME_TEST_AUDIO=mic-denied`
refuses the microphone as the operating system refuses a blocked one — which is how
`scripts/gui_voice.sh` drives voice input — and `OPENCHIME_TEST_TONE` changes the
synthetic microphone's tone, which is how `scripts/gui_calls.sh` tells two clients
apart.

**Voice input opens it too** (ARCH-112, [VOICE-INPUT.md](./VOICE-INPUT.md)), at
16 kHz mono. The microphone has one owner: a second capture is refused
(`OC_AUDIO_BUSY`), and joining a call stops voice input and closes the video
recorder. Voice input and the call engine share the playback reference — every playback device's
output, mixed to mono at 16 kHz on the media clock (`oc_audio_reference`) — and
the processor seam of §3.3 with speexdsp (§6).

### 3.3 The processor seam

Between capture and encode sits an **optional processor**, a vtable in the shape
of `oc_blob_backend` (ARCH-70):

```c
typedef struct {
    void *(*open)(int sample_rate, int frame_samples);
    void  (*close)(void *p);
    /* Both directions, same frame, so a canceller can align them. */
    void  (*process)(void *p, int16_t *capture, const int16_t *playback,
                     int frame_samples);
} oc_audio_processor;
```

The first implementation is a **no-op**, so the engine ships and is testable
before any canceller exists. AEC then drops in as a swap rather than a
restructure, exactly as S3 did for blob storage.

The signature is the important part: `process` receives **both** the capture
frame and the playback frame that was emitted at the same instant. A processor
seam that only sees capture cannot ever host an echo canceller.

There are three processors: `OC_PROCESSOR_NONE`; `OC_PROCESSOR_SPEEX`, speexdsp's
linear canceller, which voice input runs at 16 kHz (ARCH-112); and
`OC_PROCESSOR_SPEEX_48K`, for full-band audio — a screen recording's microphone
against the computer's sound (VIDEO-MESSAGES.md §4.2) — which runs the 16 kHz
canceller on the band below 7 kHz and takes the echo it finds out of the 48 kHz
microphone, 1 ms late for its filters. The computer's own sound comes from the
device layer's **loopback** device (`oc_audio_loopback_open`), which captures what
an output device plays and writes the stretches where nothing played as silence,
so its samples stay on the media clock.

---

## 4. Media transport

**Send.** Every 20 ms of captured audio is a frame, numbered whether or not it is
sent. It passes the echo canceller, then speexdsp's **preprocessor** — noise
suppression (−25 dB) and automatic gain, which the call view can turn off — then
the mute and push-to-talk gate. speexdsp's voice-activity detector is not used:
its own warning calls it "a hack pending a complete rewrite"; whether someone is
speaking is judged by level. Opus encodes it: VoIP mode,
16 kHz, 24 kbit/s VBR, with **in-band FEC**, a packet-loss hint taken from the
loss the others' packets show, and **DTX**, so a packet of two bytes or fewer —
silence the encoder need not have sent — is not sent. What is sent is the audio
type, the frame number and the Opus packet, encrypted as one SFrame (CALLS.md §5.4), behind
`token ‖ seq` to the relay. A muted client sends no audio, only, once a second,
an encrypted packet saying it is muted; and every client sends an empty
**keep-alive** at least every 5 s, so the relay's 20 s silence sweep takes only
the vanished and never someone quiet.

**The return address is bound on first use.** The relay learns where to send a
participant's audio from the first datagram carrying their token, and after that
drops that token from any other address. The token leads every packet in the
clear, so re-learning the address freely would let anyone who saw one packet
redirect that participant's audio to themselves. A client whose address changes
mid-call — NAT rebinding, a switch from Wi-Fi to cellular — is therefore not
relayed from the new one: its packets stop counting, the relay's silence sweep
drops it and says so, and it rejoins with `CALL_JOIN`, which issues a fresh token
over the authenticated TCP connection.

**Reaching the relay.** The relay binds its UDP port on every address, IPv6 and
IPv4 on one socket, as the protocol port does. It answers each participant
**from the address that participant's packets arrived at**, not whichever one
the kernel would choose for the reply: a host with several addresses otherwise
answers from the wrong one, and the client's NAT, or a hosting platform's UDP
edge, discards it. Fly is the case that needs it — public UDP arrives at a
`fly-global-services` address, and a reply from the machine's own is dropped —
and the daemon needs no setting for it. A datagram is at most **1,300 bytes**
either way: Fly documents about that much, and 1,380 was the most measured
through it. The client sends to the relay over IPv4, at the address its
workspace's host name resolves to and the port `CALL_JOINED` names, since the
hosting platform carries UDP on IPv4 alone.

Behind a front door that forwards UDP for many workspaces from one address, the
relay is reached at the door's port rather than its own, and the door has to
tell workspaces apart by the packet alone. `OPENCHIME_AUDIO_ADVERTISE_PORT`
names the port `CALL_JOINED` gives out, and `OPENCHIME_AUDIO_TOKEN_PREFIX` puts
up to 16 fixed bytes in front of every token's 16 random ones, for the door to
route by (CONFIG.md). Both are off by default, and a daemon reached directly
behaves as it always has. The token is opaque to the client, which holds up to
32 bytes, so neither needs a client that knows about it.

The daemon advertises `calls` when its relay is running, not when clients can
reach it: nothing on the host can tell whether a firewall or platform in front
of it passes the relay's port. A deployment has to expose that port, pinned with
`OPENCHIME_AUDIO_PORT`, as it exposes the protocol port.

**Receive.** Demultiplex on `sender_user_id`; drop a packet whose SFrame KID is
not a key that sender gave, that fails authentication, or that the replay window
has seen; route audio to that sender's jitter buffer by frame number, and a
shared screen's packets to the share's reassembly (VIDEO.md §5). Loss is
counted from gaps in the authenticated SFrame counter — a gap in frame numbers
alone may be DTX.

**Jitter buffer**, one per sender (`client/core/call/jitter.c`): ordered by frame
number, duplicates dropped. Its **target delay adapts**: the 95th percentile of
how late packets arrive relative to the earliest over the last two seconds, plus
a frame, between 40 and 240 ms. It reaches the target by waiting — at the start
of a talk spurt, or by concealing a frame when a packet came too late — and gives
delay back only by skipping a frame while nothing is being said, so speech is
never cut to catch up. A lost frame whose successor has arrived is rebuilt from
the successor's **FEC**; otherwise **Opus PLC** (`opus_decode` with no payload)
conceals up to three frames, then there is silence until the next packet, which
starts a new talk spurt. `tests/test_call_media.c` holds it to that on a simulated
network.

**Mixing.** The playout thread runs on the speaker's clock — it keeps 60 ms queued
and makes a frame whenever the device takes one — pulling a frame from every
sender, applying that person's volume (0 to 2), summing in 32 bits and limiting
with a soft knee above −3 dBFS, so many people at once get quieter rather than
distorted. The mix plays through the device layer, which is what puts it in the
canceller's reference.

**Speaking** marks come from each sender's decoded level, computed where it is
heard: the server cannot know, since it never decodes.

## 5. Call signaling in the app-core

Signaling is ordinary protocol work on the network thread (`client/core/callsig.c`):
`OC_CMD_CALL_JOIN/INVITE/LEAVE/DECLINE/END` out, and `CALL_JOINED`, `CALL_ROSTER`,
`CALL_STATE` and `CALL_KEY_FOR` in. It keeps the device key, makes and seals the
media key on every epoch and opens the others' (CALLS.md §5), and folds the call
into the model as `OC_EV_CALL_*`: the Calls section's list, the call this client
is in, a refusal. The media endpoint, the token and every key go to the engine
through the **`oc_call_media` seam** — start, roster, the key to send with, a
sender's key, stop — and never reach a frontend. The seam is why the core links
no codec: the TUI builds without the engine, and a frontend that does calls plugs
it in with `oc_client_set_call_media`.

## 6. Acoustic echo cancellation

### 6.1 The problem

The speaker plays the far end, the microphone picks it up, and it is sent back —
so the far end hears itself delayed. With both sides doing it, the loop can howl.

Subtracting the known playback signal does not work, for four reasons that
together define the difficulty:

1. **The room transforms it.** The microphone hears playback convolved with the
   room's impulse response — direct path, reflections, reverb. The filter must
   be *estimated adaptively*, not derived.
2. **Clocks drift.** Separate capture and playback crystals slide continuously,
   so a converged filter goes stale. §2's device preference and runtime drift
   detection are the mitigation — note duplex mode alone does not guarantee a
   shared clock.
3. **Speakers are nonlinear.** A linear filter mathematically cannot cancel a
   nonlinearly distorted echo, so a residual suppressor after the linear stage
   is mandatory, not optional polish.
4. **Double-talk.** When both parties speak, the filter tries to cancel the near
   end and corrupts itself; adaptation must freeze.

### 6.2 Choice of canceller

We do not write one. This is a mature field and a competitive implementation is
a research project.

| Option | Language | Assessment |
|---|---|---|
| **speexdsp** `speex_echo_state` | C | BSD, small, drops into this codebase with no toolchain change. **Attenuates rather than truly cancels**, and is drift-sensitive — it ships a diagnostic script whose purpose is detecting drift, which says how chronic that failure is. Old code, long-stable rather than maintained. |
| **WebRTC AEC3** | C++ | BSD, clearly the strongest: handles drift, nonlinearity, and double-talk properly. Costs a C++ toolchain in a pure-C codebase, and is **much less forgiving of integration error** — field reports describe it over-cancelling into a flat, dead microphone when fed bad delay or misaligned buffers. |

**Start with speexdsp**, behind §3.3's vtable. The reasoning is failure mode,
not quality: speexdsp degrades by leaking residual echo — annoying, still a
usable conversation — whereas a misintegrated AEC3 degrades by deleting the
user's voice, which is worse and much harder to diagnose. For a first
implementation, failing softly is the more valuable property, and the vtable
means the decision is reversible.

The mitigations in §2 (preferring a single physical device, 16 kHz, and capping output gain to
keep the speaker in its linear region) address speexdsp's three real weaknesses
directly. They move it from "mediocre" to "acceptable on most machines."

### 6.3 Delay is the thing to get right

The canceller must know how long after a sample is handed to playback its echo
appears in the capture stream. Get this wrong and cancellation is not degraded,
it is **absent** — the filter correlates against the wrong part of the signal and
converges to nothing.

Duplex mode makes this tractable, because both streams arrive in one callback
with a known relationship. The engine reports its own buffering; it must not
guess.

### 6.4 Measuring it: the ERLE harness

**Build this before tuning anything.** AEC is deterministically testable with no
hardware:

1. Take a far-end signal; convolve it with a synthetic room impulse response to
   produce an "echo."
2. Mix in a near-end signal.
3. Feed both to the processor.
4. Measure **ERLE** — echo return loss enhancement, in dB — the ratio of echo
   energy before and after cancellation.

That is a hermetic unit test in `make test`: no microphone, no flake, a number.
Extending it costs little and buys a lot:

- inject deliberate clock drift by resampling one side, and assert the filter
  re-converges;
- assert **near-end speech survives** during double-talk, which is the failure
  that makes AEC3 dangerous and would otherwise be invisible;
- compare cancellers on identical inputs, so "is speexdsp good enough" is a
  measurement rather than an argument.

Most homegrown echo cancellation is unverified precisely because this step is
skipped.

**The harness** (`tests/test_voice.c`). speexdsp's linear canceller at
16 kHz, 20 ms frames and a 300 ms echo path, against a synthetic room: a direct
path 30 ms late and a decaying tail:

| Case | Result |
|---|---|
| Converged | 22.5 dB ERLE |
| Far end resampled 100 ppm fast | 15.5 dB ERLE — re-converges |
| Double-talk | 13.2 dB of echo removed; output correlates 0.93 with the near-end voice |
| The same rooms at 48 kHz, on the 16 kHz band (`OC_PROCESSOR_SPEEX_48K`) | 22.1 dB converged; 13.2 dB in double-talk, voice correlating 0.93 — against 23.2 dB, 11.0 dB and 0.88 for speexdsp run at 48 kHz |

speexdsp's **residual-echo suppressor is left off**: in the same double-talk it
cut the near-end voice to a correlation of 0.05. The linear filter alone keeps the
speech, which is the failure the harness exists to catch.

**After the canceller, the preprocessor.** A call runs speexdsp's preprocessor on
the cancelled frame: noise suppression and automatic gain, after cancellation so
the far end's echo is gone before the gain can lift it. It is what makes a steady hum disappear — `scripts/gui_calls.sh` checks that
with a test tone, which to it is a hum — and why the call view offers it as a
switch.

---

## 7. Where it lives

| Piece | Where |
|---|---|
| Device layer, playback reference, processor seam, canceller | `client/core/media/` (ARCH-110/112) |
| Opus at 16 kHz with FEC, DTX and concealment | `client/core/media/opus.c` |
| Signaling, device key, media keys | `client/core/callsig.c`, `client/core/store.c` |
| Engine: capture, preprocessor, SFrame, socket, jitter buffers, mixer | `client/core/call/` |
| HPKE and SFrame | `shared/e2e_hpke.c`, `shared/e2e_sframe.c` |
| Call state, invitations, keys forwarded, missed calls | `daemon/netloop.c`, `daemon/dbwriter.c` |
| Relay | `daemon/audio_sidecar.c` |
| Calls section, call view, strip, toasts, keys | `client/gui/win32/winmain.c` |

## 8. TUI surface

The TUI has no calls: it links the core without the engine and offers nothing
of them. **There are no slash commands** (ARCH-83), so if it gains calls, joining
and leaving are actions in the Ctrl+K command palette and the channel action
menu, the roster renders in the Members panel with speaking and muted marks, mute
is a key binding rather than a menu item — it is used mid-sentence — and
**push-to-talk is a first-class control**, the natural terminal idiom, which also
sidesteps echo entirely while held.

## 9. Limits

- **Recording.** There is no call recording. It has obvious compliance weight
  (REQ-252), and a recording would have to be made by a
  participant, since nothing else can hear the call (ARCH-113).
- **IPv6.** The relay listens on IPv6 as well as IPv4, but a client sends to it
  over IPv4 only (§4), so a client with no IPv4 path to the workspace can sign in
  and cannot be heard.
