# OpenChime — Audio

How a voice call works: the huddle model, the media path, the client audio
engine, and acoustic echo cancellation. This is the authoritative design; it is
cross-referenced from ARCHITECTURE.md (ARCH-18, ARCH-28, ARCH-31, ARCH-73),
REQUIREMENTS.md (§6.2, REQ-150–152), PROTOCOL.md (§5.17), and CLIENT.md.

**This document is audio only.** **Screenshare is [VIDEO.md](./VIDEO.md)**
(REQ-161, ARCH-86/87) — it rides this same call, sidecar, and UDP path, and is
**sequenced behind everything here**: the media transport, jitter buffer, and
device layer of §§2–4 are its prerequisites, so building it first would build the
media stack twice. Camera video remains out of scope (REQ-160).

**Status.** **The server half is built and tested; the client half does not
exist.** `CALL_JOIN` / `CALL_LEAVE` / `CALL_JOINED` / `CALL_ROSTER` signaling,
the per-channel ephemeral roster, per-join bearer tokens, and the forked UDP
relay sidecar (`daemon/audio_sidecar.c`) all work, including disconnect and
rejoin (REQ-152). What is missing is everything client-side: there is no
`CALL_*` handling in `client/core`, no Opus, no UDP media path, no audio device
layer, and no echo cancellation. This document specifies that work.

---

## 1. The model: huddles, not calls

A call is **one per channel** (`call_id == channel_id`), with an ephemeral
roster of N participants (ARCH-73). It is a **huddle**: anyone with channel-read
access joins the channel's ongoing call, the call persists while at least one
participant remains, and a dropped participant rejoins with a fresh
`CALL_JOIN`.

**A 1:1 call is the degenerate case, not a separate feature.** A DM channel has
two members, so a call there is a two-person huddle over exactly the same code
path. There is no pairwise call model anywhere in the system, and none should be
added.

### 1.1 The server is an SFU — it never mixes

ARCH-18/73 forbid the server from decoding Opus, so the sidecar **forwards
opaque payloads** rather than mixing them. The media framing makes this visible
(`daemon/audio.h`):

```
client  → sidecar :  token(16) ‖ seq(u16 BE) ‖ opus-payload
sidecar → client  :  sender_user_id(u64 BE) ‖ seq(u16 BE) ‖ opus-payload
```

Every packet a client receives is tagged with **who sent it**. In a five-person
huddle a client receives five independent streams.

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
   │  audio engine (duplex callback; clock shared only on some hardware)         │
   │     capture ──→ [ processor: AEC ] ──→ encode                               │
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

The device is opened in **duplex mode** — both directions in one callback.

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

So the engine does three things rather than assuming the problem away:

- **Prefer a single physical device**, defaulting to the built-in one.
- **Detect drift at runtime** by tracking capture versus playback sample counts;
  steady divergence means separate clock domains. That is a few lines, and it
  converts an invisible failure into a known condition.
- **Degrade honestly.** On a split-device rig, either compensate by resampling
  one side or tell the user echo cancellation is unreliable — rather than
  silently cancelling nothing.

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
the natural thing to build and the hard thing to undo, which is the real argument
for settling this in Phase 1.

---

## 3. The audio engine

A new `client/core/audio.{c,h}` owning **both directions**, independent of the
network and of any frontend.

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

---

## 4. Media transport

**Send.** Encode a 20 ms frame, prepend `token(16) ‖ seq(u16)`, one UDP
datagram. Sequence numbers are per-sender and monotonic; the sidecar does not
interpret them.

**Receive.** Demultiplex on `sender_user_id`, route to that sender's jitter
buffer.

**Jitter buffer**, one per sender: a fixed initial depth of 60–100 ms, reordering
by sequence number and holding late packets briefly rather than discarding them.
A gap that cannot be filled is concealed with **Opus PLC** (`opus_decode` with a
NULL payload), which synthesizes plausible audio rather than emitting silence —
the difference between a call that sounds lossy and one that sounds broken.

Adaptive depth (growing the buffer under observed jitter, shrinking it when the
network is calm) is a later refinement; a fixed depth is correct and shippable
first.

**Mixing.** Sum the decoded streams into one buffer with headroom, since
summing N speakers can clip. Simple attenuation proportional to active speakers
is sufficient; automatic gain control is out of scope for a first version.

**Silence suppression.** A participant who is not speaking should not send
packets. Opus's own DTX plus a simple energy gate is enough, and it matters more
than it sounds: in a ten-person huddle where one person is talking, naive
always-send costs ten times the bandwidth for nine streams of silence.

---

## 5. Call signaling in the app-core

Signaling is ordinary TCP protocol work and is independent of media, so it is
built and demonstrable first (§7).

- **Commands:** `OC_CMD_CALL_JOIN` / `OC_CMD_CALL_LEAVE`.
- **Events:** `OC_EV_CALL_JOINED` (roster + media endpoint + token),
  `OC_EV_CALL_ROSTER` (roster changed).
- **Model:** the active call's `channel_id`, the participant list, and per-
  participant state a frontend needs to render — at minimum *speaking* and
  *muted*.

The media endpoint and token from `CALL_JOINED` are handed to the audio engine;
they never reach a frontend.

**Speaking indication** is derived client-side from received packet energy per
sender, not signaled by the server — the server cannot know, since it never
decodes.

---

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

---

## 7. Build order

Each phase is independently demonstrable, and the risky work is deliberately
late — after the seams that make it replaceable exist.

| Phase | Deliverable | Proves |
|---|---|---|
| **1** | Duplex engine + ring buffers + drift detection + local loopback | the device layer, in isolation |
| **2** | Opus encode → decode round-trip locally | the codec |
| **3** | `CALL_*` signaling in the app-core; TUI shows the roster | the control plane, with no media |
| **4** | UDP media with one peer, jitter buffer, PLC | **1:1 audio works** |
| **5** | Per-sender decoders + mixer | **huddles work** |
| **6** | ERLE harness, then speexdsp behind the vtable | echo cancellation, measured |
| **7** | TUI: join/leave via the command palette + menus, roster, mute, push-to-talk, device pick | usable |

Phase 6 lands after Phase 5 because a mixer produces a single far-end reference
(§1.1), and because there is no real echo to cancel until real audio is playing
out of a real speaker.

**Nearly all of this is `client/core` work.** The TUI contributes commands and a
roster panel; every future GUI inherits the engine, the codec, the transport, and
the canceller unchanged.

---

## 8. TUI surface

**Corrected: there are no slash commands.** An earlier revision of this section
specified `/call` and `/hangup`; the slash-command dispatcher was **deleted** in
the TUI redesign (ARCH-83), and the TUI is now menu- and screen-driven. Joining
and leaving a huddle are therefore actions in the **Ctrl+K command palette** and
the channel action menu, exactly like every other TUI action.

The roster renders in the Members panel or a dedicated overlay, showing
per-participant *speaking* and *muted* state. Mute is a key binding, not a menu
item — it is used mid-sentence.

**Push-to-talk is a first-class control, not an echo workaround.** It is the
natural terminal idiom, it is what users of a keyboard-driven client expect, and
it happens to also sidestep echo entirely while held.

---

## 9. Open decisions

- **Split-device drift policy.** §2 detects divergent clocks; what to *do* then
  is undecided — resample one side to compensate, or disable cancellation and
  say so. Resampling is more work and can itself colour the far-end reference.
- **Adaptive jitter depth.** Fixed first; adaptive is a later refinement, and
  needs a policy for how fast to grow and shrink.
- **Automatic gain control.** Out of scope for a first version, but a quiet
  participant in a large huddle is a real complaint, and AGC interacts with AEC
  (it changes the far-end reference level) so the seam should be settled before
  it is added.
- **Recording.** Not designed. It has obvious compliance weight (REQ-252) and
  should not be added casually.
- **AEC3 escalation.** Whether the C++ dependency is ever acceptable. The ERLE
  harness (§6.4) is what should decide it, on numbers.
- **Bandwidth ceiling for large huddles.** With DTX and silence suppression a
  ten-person huddle is mostly one active stream, but the worst case is N × 24
  kbps downstream and there is currently no cap or policy.
