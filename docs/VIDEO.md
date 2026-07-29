# OpenChime — Screenshare

How screen sharing works: what it is and is not, why it rides the existing audio
relay unchanged, the codec decision, and the transport work the codec does *not*
solve. This is the authoritative design; it is cross-referenced from
ARCHITECTURE.md (ARCH-86, ARCH-87), REQUIREMENTS.md (§6.3, REQ-160/161),
PROTOCOL.md (§5.17), and [AUDIO.md](./AUDIO.md).

**Status. Designed, not started.** No screenshare code exists in any client or in
the daemon. This document exists so the decisions taken are recorded before the
work begins, not to signal that it is scheduled — it is explicitly **sequenced
behind the audio client** (§8), which itself is at phase 0.

**This document is screenshare only.** Camera video calling remains excluded by
REQ-160 and nothing here reverses that (§2).

---

## 1. Why it is in scope at all

Screenshare is the one video-shaped feature with a clear, bounded work-chat
justification: showing someone an error, a diff, a design, or a spreadsheet.
Both reference products ship it — Slack in huddles, Pumble from its $2.49 Pro
tier (see [CLIENT_GAP_ANALYSIS.md](./CLIENT_GAP_ANALYSIS.md) §2.15).

It is worth being clear-eyed about what that means competitively: shipping
screenshare buys **parity, not differentiation**. It closes a checkbox gap; it
does not win a deal the way self-hosting, data residency, or read receipts do.
That is an argument about *sequencing*, not about whether the feature is
legitimate.

## 2. Scope — screenshare is not video calling

| In scope | Out of scope |
|---|---|
| One participant shares a screen or window; others view it | Camera / webcam video (REQ-160 stands) |
| View-only for the receivers | Remote control of the sharer's machine |
| Screen audio is **not** captured; voice rides the existing audio path | Recording the share |
| One sharer at a time per call | Simultaneous shares / picture-in-picture grids |

**REQ-160 is amended, not repealed.** Camera video calling and general video
playback remain a deliberate exclusion. Screenshare is admitted as REQ-161
because its content profile (mostly static, low frame rate, one sender) is
fundamentally cheaper than camera video and its use case is concrete.

## 3. It rides the existing relay unchanged

The single most useful property of the current design: **the sidecar never
decodes anything.** `daemon/audio.h` states it — it "forwards opaque payloads (an
SFU), so there is no codec dependency here." A video payload relays exactly as an
Opus payload does, tagged with the sender's id.

So screenshare needs **no server-side codec, and no change to the relay's
forwarding logic** (ARCH-86). The daemon stays free of libvpx exactly as it is
free of libopus today (ARCH-73).

The same property has a hard consequence, recorded here because it is the thing
most likely to be forgotten:

> **There is no transcoding fallback, and there cannot be one.** A conventional
> media server bridges participants that disagree on a codec by decoding and
> re-encoding. ARCH-18/73 forbid the server touching the payload at all. A call
> can contain a macOS client and a Windows client simultaneously, so if two
> clients disagree on the codec **the call simply does not work**.

That makes the codec a **wire contract**, at the same level as the frame version
(ARCH-8) or the `oc/1` ALPN (ARCH-54) — not a per-frontend build choice. It is
settled in ARCH-87 and negotiated on the wire (§6).

## 4. Codec — VP9 via libvpx (ARCH-87)

**Exactly one mandatory baseline codec: VP9, encoded and decoded with libvpx
(BSD-3-Clause), with screen-content tuning enabled.**

Why VP9 specifically:

- **Permissive licence.** BSD-3-Clause. VENDORS.md's rule is *permissive* — "MIT,
  Apache-2.0, Public Domain, or optional dynamic LGPL" — and mbedTLS is already
  Apache-2.0, so BSD-3 fits the posture. It is not on the current list only
  because nothing has needed it.
- **Royalty-free in practice.** Google's patent grant; the MPEG-LA VP8 pool
  settled in 2013. Contrast **openh264**, whose BSD licence hides a trap: Cisco's
  royalty arrangement covers only the binaries **Cisco itself distributes**, so
  building from source leaves us exposed. **x264/x265 are GPL** and fail the
  licence posture outright.
- **Identical on every client by construction.** One C source built for Linux,
  Windows, macOS, Android, and iOS. This is what §3's no-transcode constraint
  actually requires — not "a codec available on each platform," but *the same
  codec everywhere*.
- **Screen-content tuning.** `VP9E_SET_TUNE_CONTENT` = `VP9E_CONTENT_SCREEN`
  enables screen-oriented coding tools. This matters more than it sounds: text
  has hard edges that DCT-based codecs handle badly at low bitrate, producing
  ringing that makes a share *unreadable* — the one failure mode screenshare
  cannot have. Legibility, not perceptual quality, sets the bitrate floor.
- **Decode is cheap and often hardware-accelerated** (Android MediaCodec, most
  desktop GPUs, and natively in every current browser should a DOM/WASM frontend
  ever land, ARCH-74). This is the right asymmetry: one participant encodes, N
  decode.

**Ship VP9 alone — no VP8 fallback.** Two permitted codecs means a negotiation
matrix in which some client pairs cannot talk. One mandatory baseline is the
correct shape for a relay that cannot transcode.

**Rejected alternatives**, recorded so they are not relitigated:

| Option | Why not |
|---|---|
| **Platform-native codecs** (Media Foundation, VideoToolbox, MediaCodec) | Fatal under §3: a Mac and a Windows client in one call must interoperate, and the server cannot bridge them. A per-platform codec is not a shortcut, it is a broken call. |
| **openh264** | Patent grant covers only Cisco-distributed binaries. |
| **x264 / x265** | GPL — fails the licence posture. |
| **FFmpeg / libavcodec** | LGPL is acceptable dynamically linked (as libsecret is), but it is an enormous dependency for one codec. |
| **AV1** (SVT-AV1 + dav1d) | Technically the best screen-content option and genuinely royalty-free by design, but realtime 1080p software encode is materially harder than VP9 and the build is heavier. **Revisit when hardware AV1 encode is commonplace** — the wire-contract negotiation in §6 exists so this can change without breaking old clients. |
| **VP8** | Cheaper CPU, but no screen-content tuning — the specific reason to prefer VP9 here. |

**Honest costs.** libvpx offers software encode only; hardware VP9 *encoders* are
rare and not portably reachable through it, so the sharer's CPU carries a
realtime encode, and VP9 is materially slower than VP8. Practically this is
manageable — screenshare is fine at 5–10 fps, and `deadline=realtime` plus
`cpu-used` trade compression for speed — but it is a real load on the sharer's
machine and must be measured, not assumed. libvpx is also the **first genuinely
large dependency** in the tree; it does not fit the committed-single-file pattern
(termbox2 / utf8proc / jsmn) and belongs in VENDORS.md's *fetched at build* class
beside mbedTLS, pinned by a `scripts/build_*.sh`.

## 5. What the codec does not solve

Choosing VP9 settles the smallest question in the feature. The transport work is
untouched by it, and all of it is new:

1. **Fragmentation.** `OC_AUDIO_MAX_PACKET` is **1400** — one sub-MTU datagram.
   That is right for ~80-byte Opus frames and useless for a keyframe of tens of
   KB. A fragment/reassembly header is needed in the sidecar framing (§6).
2. **Loss recovery.** Opus conceals a dropped packet with PLC; a dropped video
   packet corrupts the picture until the next IDR. Needs NACK/retransmission,
   FEC, or a receiver-driven keyframe request — none exist.
3. **Rate control.** There is no congestion control and no bitrate adaptation
   anywhere in the media path. Without them the choice is a pinned conservative
   bitrate (unreadable the moment someone scrolls) or unshed spikes under load.
4. **Sequence width.** `seq` is `u16`, fine for 50 packets/s of audio, tight at
   video packet rates — it wraps in roughly a minute. Reassembly must tolerate
   wrap, or the field widens.

This is the real cost of the feature, and it is why §8 sequences it behind the
audio client rather than beside it: every one of these is a media-transport
problem that the audio work has to solve first in simpler form.

## 6. Protocol additions

**`CALL_JOINED` carries no codec field today** (`{channel_id, call_id, udp_port,
token, roster}`, PROTOCOL.md §5.17). Video cannot be added without one: the first
codec change would break every older client with no handshake to catch it — the
mistake ARCH-41 exists to prevent for frames.

The additions screenshare needs, none of which are built:

- a **codec identifier** negotiated at `CALL_JOINED`, so §4's baseline can change
  later (to AV1, per §4's rejected-alternatives note) without a flag day;
- **share start/stop signaling** — who is sharing, so clients render the right
  surface and the roster shows it;
- a **fragment header** in the sidecar's UDP framing (§5.1), which is the one
  media-plane wire change;
- a **keyframe request** path from receiver to sharer (§5.2).

Fragmentation and keyframe requests are relay-visible but not
relay-*interpreted*: the sidecar keeps forwarding opaque payloads and simply
carries a larger header. That preserves ARCH-18/73.

## 7. Client surfaces

**The TUI is excluded, permanently and by design.** ARCH-75: the TUI "never
renders graphics — no images, ever." A shared screen cannot be rendered in a cell
grid. The TUI's ceiling is showing *that* a share is in progress and who is
sharing — roster state, not pixels.

That leaves screenshare as a **graphical-frontend-only** feature, and today the
only graphical frontend is the Windows GUI. *(Updated 2026-07-28: the specific
gaps this paragraph used to cite — no error/toast surface, no settings screen, no
working search, no workspace switcher — are all closed. The argument does not
depend on them; it depends on there being exactly one graphical client, which is
still true, and on the audio prerequisite below, which is untouched.)*

Capture APIs are per-platform (DXGI Desktop Duplication, ScreenCaptureKit,
PipeWire portals) and that is fine — **capture is a frontend concern, the codec
is not.** The captured frames cross into `client/core` and are encoded there by
the one shared VP9 encoder, so the wire stays identical across platforms.

## 8. Sequencing and the prerequisite

**Screenshare is gated behind the audio client.** AUDIO.md §7 is a seven-phase
plan and the client half is at phase 0 — no device layer, no ring buffers, no
jitter buffer, no UDP media path, no `CALL_*` handling in `client/core` (verified:
`client/` contains no `CALL_JOIN`, Opus, or UDP media code).

Screenshare sits on top of all of it. Built first, the media transport gets built
twice.

The order that follows from this document and CLIENT_GAP_ANALYSIS.md §5:

1. ~~Win32 GUI P0 depth gaps (error/toast surface, search, sidebar, settings).~~
   **Done** — see CLIENT_GAP_ANALYSIS.md §5, which has been re-ordered around
   what is actually left.
2. AUDIO.md phases 1–5 — device layer, Opus, `CALL_*` signaling, UDP media,
   jitter buffer, mixing.
3. The transport gaps of §5, which audio needs in simpler form anyway.
4. Screenshare, revisiting the §4 codec choice against whatever hardware
   encode landscape exists by then.

## 9. Bandwidth and the flat-plan collision

Screenshare bitrate is far more content-dependent than camera video, because
codecs encode inter-frame differences and a static screen produces almost none:

| Content (1080p) | Typical bitrate |
|---|---|
| Static slides, a document, an idle editor (2–5 fps) | 100–300 kbps |
| Scrolling code, dragging windows (10–15 fps) | 500 kbps – 1.5 Mbps |
| Full motion — video playback, animation (30 fps) | 3–6 Mbps |

So typical work usage is **300 kbps – 1.5 Mbps**, not the multi-Mbps figure a
"video" label suggests. The problems are not the single stream:

- **Fan-out multiplies it.** The SFU sends every stream to every participant, so
  egress is `bitrate × (N−1)`. Even 1 Mbps is ~9 Mbps out of one box for a
  10-person call. WebRTC SFUs handle this with simulcast and per-receiver
  adaptation; we have neither (§5.3).
- **The variance is unbounded**, and without rate control it cannot be shed.
- **Egress is billed, and the hosted plan is flat.** CP-4 is $99/month with **no
  metering** — deliberately, so "the control plane needs zero runtime metric from
  the box for billing." A bandwidth-heavy feature on an unmetered flat plan means
  one screenshare-heavy tenant moves the unit economics with no pricing lever to
  absorb it.

The last point is a **business-model collision, not an engineering one**, and it
is the strongest argument against shipping screenshare casually. It is recorded
here as input to the control plane's CP-4, not as a pricing proposal — pricing is
out of scope for this repo (REQUIREMENTS.md preamble).

## 10. Open decisions

- **Rate-control policy.** Fixed conservative bitrate, receiver-feedback
  adaptation, or content-adaptive framerate. Undecided; §5.3 is the blocker.
- **Loss-recovery mechanism.** NACK vs FEC vs keyframe-on-request. Cheapest
  first is probably keyframe-on-request, at the cost of a visible stall.
- **Egress policy for the hosted plan.** Whether screenshare is capped, disabled,
  or metered on the flat plan — a CP-4 question (§9).
- **Whether a still-frame mode ships first.** Periodic JPEG stills over the
  existing TCP attachment path (§5.14) need no codec, no UDP, no fragmentation
  and no new dependency, and cover "look at my error message" — most of what
  screenshare is used for. It is days rather than months, but it is **not**
  screenshare and must not be presented as such.
- **Screen audio.** Currently out of scope (§2); sharing a video with sound would
  need it, and it interacts with the AEC design (AUDIO.md §6).
