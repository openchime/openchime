# OpenChime — Screenshare

How screen sharing works: what it is and is not, why it rides the existing call
relay unchanged, the codec, and the transport built on top of it — fragments,
loss recovery and rate control. This is the authoritative design; it is
cross-referenced from ARCHITECTURE.md (ARCH-86, ARCH-87), REQUIREMENTS.md (§6.3,
REQ-160/161), PROTOCOL.md (§5.17), [CALLS.md](./CALLS.md) and
[AUDIO.md](./AUDIO.md).

**This document is screenshare only.** Camera video calling remains excluded by
REQ-160 and nothing here reverses that (§2). Recorded **video messages** are a
separate design with no real-time path: [VIDEO-MESSAGES.md](./VIDEO-MESSAGES.md).

---

## 1. Why it is in scope at all

Screenshare is the one video-shaped feature with a clear, bounded work-chat
justification: showing someone an error, a diff, a design, or a spreadsheet.
Both reference products ship it — Slack in huddles, Pumble from its $2.49 Pro
tier. It buys **parity, not differentiation**.

## 2. Scope — screenshare is not video calling

| In scope | Out of scope |
|---|---|
| One participant shares a screen or window; the others view it | Camera / webcam video (REQ-160 stands) |
| View-only for the viewers | Remote control of the sharer's machine |
| Screen audio is **not** captured; voice rides the call's audio | Recording the share |
| **One sharer at a time**; starting a share takes over from whoever was sharing | Simultaneous shares / picture-in-picture grids |
| | Drawing on someone's share |

Sharing is part of every call: there is no switch to turn it off. What bounds its
cost is the bitrate ceiling (§6).

## 3. It rides the existing relay unchanged

**The relay never decodes anything.** It forwards opaque payloads tagged with the
sender (`daemon/audio.h`), and a share's packets relay exactly as audio's do. So
screenshare needs **no server-side codec and no change to the relay's framing or
forwarding** (ARCH-86); the daemon stays free of libvpx as it is of libopus.

The same property has a hard consequence:

> **There is no transcoding fallback, and there cannot be one.** A conventional
> media server bridges participants that disagree on a codec by decoding and
> re-encoding. ARCH-18/73 forbid the server touching the payload at all. A call
> can contain a macOS client and a Windows client simultaneously, so if two
> clients disagree on the codec **the call simply does not work**.

That makes the codec a **wire contract** (ARCH-87), carried on `CALL_JOIN` (§7).

The relay does drain its socket in batches and asks for a 4 MB socket buffer each
way, since a keyframe arrives as a burst of a hundred packets or more.

## 4. Codec — VP9 via libvpx (ARCH-87)

**Exactly one mandatory codec: VP9, encoded and decoded with libvpx
(BSD-3-Clause), with screen-content tuning enabled** — the libvpx the video
recorder already links (VENDORS.md).

Why VP9:

- **Permissive licence.** BSD-3-Clause.
- **Royalty-free in practice.** Google's patent grant; the MPEG-LA VP8 pool
  settled in 2013. Contrast **openh264**, whose BSD licence covers only the
  binaries **Cisco itself distributes**; **x264/x265 are GPL**.
- **Identical on every client by construction.** One C source built for every
  platform — what §3's no-transcode constraint requires.
- **Screen-content tuning.** `VP9E_SET_TUNE_CONTENT` = `VP9E_CONTENT_SCREEN`
  keeps text's hard edges sharp at low bitrate. An unreadable share is the one
  failure screenshare cannot have.
- **Decode is cheap.** One participant encodes; N decode.

**Rejected alternatives**, recorded so they are not relitigated:

| Option | Why not |
|---|---|
| **Platform-native codecs** (Media Foundation, VideoToolbox, MediaCodec) | Fatal under §3: a Mac and a Windows client in one call must interoperate, and the server cannot bridge them. |
| **openh264** | Patent grant covers only Cisco-distributed binaries. |
| **x264 / x265** | GPL — fails the licence posture. |
| **FFmpeg / libavcodec** | An enormous dependency for one codec. |
| **AV1** (SVT-AV1 + dav1d) | The best screen-content option and royalty-free, but realtime software encode is materially harder. **The designated successor**: a client says which codecs it can decode (§7), so a second codec can arrive without a flag day. |
| **VP8** | Cheaper CPU, but no screen-content tuning. |

The encoder (`oc_vp9enc_open_share`) is realtime, `cpu-used` 8, CBR at the
bitrate rate control sets (§6), with each frame's duration taken from the time
since the one before — a share's frame rate varies — and **keyframes only when
asked for**: the loss recovery below decides when one is needed.

## 5. The media path

Everything below is in the client, inside the call's encryption. The pure rules —
fragmenting, reassembly, NACK, PLI, rate — are `client/core/call/share.c`
(`oc_share.h`), with no sockets or threads; the call engine
(`client/core/call/call_engine.c`) runs them.

### 5.1 Packets

Every call packet's plaintext starts with a **type** (CALLS.md §5.4):

| Type | Plaintext after the type byte |
|---|---|
| 0 audio | `frame(u32) ‖ opus` |
| 1 state | `flags(u8)` — bit 0 muted |
| 2 video | `frame(u32) ‖ frag(u16) ‖ nfrags(u16) ‖ flags(u8) ‖ width(u16) ‖ height(u16) ‖ bytes` |
| 3 control | `target(u64) ‖ kind(u8) ‖ body` |

All four are sealed under the sender's SFrame key for the epoch with **one
counter across the types**, so a nonce is never reused. Nothing new is added to
key exchange.

### 5.2 Fragments

An encoded frame is split into fragments of at most **1100 bytes**, each its own
packet: with the 14-byte header, the SFrame overhead and the relay's framing, a
packet stays under the relay's 1300-byte datagram, behind a token of up to 32 bytes. A frame of up to 1024
fragments (1.1 MB) can be sent; `flags` bit 0 marks a keyframe. Frames and
fragments are numbered **inside the encryption**, so the relay's unauthenticated
16-bit `seq` is not used, as it is not for audio. Frame numbers carry on across a
sharer's shares, so a viewer tells an old share's stragglers from a new one's.

A viewer reassembles by frame number in 32 slots, tolerating reordering and
duplicates, and hands frames to the decoder **in order**. It starts at a keyframe,
and whenever frames were lost it starts again at the next complete keyframe.

### 5.3 Loss recovery: NACK, then PLI

Control packets go from a viewer to the sharer, relayed to everyone and acted on
only by the user they name.

- **NACK** `{frame(u32), n(u16), frag(u16)×n}` — n = 0 means the whole frame. A
  viewer asks for fragments missing **60 ms** after their frame began to arrive,
  at most three times, and for a whole frame missing when a later one arrives.
  The sharer keeps **a second** of what it sent and sends the named fragments
  again.
- **PLI** `{}` — a keyframe, please. A viewer asks when a frame it needs is still
  incomplete after **300 ms** (it gives that frame up), when the decoder fails, and
  on arriving: a viewer who joins late waits 250 ms for the keyframe a new share
  starts with, then asks. At most one PLI per 500 ms from a viewer, and none while
  a keyframe is already arriving. The sharer sends a keyframe for a PLI **at most
  once a second**, and **every ten seconds** regardless.

### 5.4 Rate control

Every **500 ms** each viewer sends a **REPORT** `{loss‰(u16), kbps(u32)}`: the
loss over that time, counted from gaps in the sharer's authenticated SFrame
counter, and the video bitrate received. The sharer's rate is what the
worst-placed viewer can take:

- any viewer losing **more than 5%** halves it (at most once a second);
- anyone losing more than 2% holds it;
- otherwise it grows by **a tenth a second**;
- between **150 kbit/s** and the built-in ceiling of **2500 kbit/s**, starting at
  1200.

A viewer who leaves the call stops holding the rate down.

### 5.5 What is sent

The source is captured at up to **15 fps**, fitted inside **1920×1080**. A frame
identical to the last one sent (a fingerprint of its pixels) is skipped, unless
half a second has passed: a still screen goes at **2 fps**, which is what lets VP9
sharpen it and costs almost nothing. Below 600 kbit/s the frame is fitted inside
**1280×720** instead, and back at the full size above 1000 — the gap keeps it from
flapping. A change of size reopens the encoder with a keyframe.

### 5.6 The engine

In the call engine, beside the audio threads:

- a **share thread** while this device shares: capture → change check → fit →
  encode → fragments → sealed and sent;
- a **view thread** for the whole call: asks for what is missing, reports, takes
  whole frames in order, decodes, and publishes the newest picture for the
  frontend (`oc_call_engine_share_frame`).

A share starts in two steps. `oc_call_engine_share_start` opens the source (so a
refusal is known at once), and the frontend sends `CALL_SHARE` on; frames go only
once the daemon's `CALL_STATE` names this device the sharer. When a `CALL_STATE`
names someone else, this device's share stops.

## 6. Bandwidth

Screenshare bitrate is content-dependent, because a static screen produces
almost no inter-frame difference:

| Content (1080p) | Typical bitrate |
|---|---|
| Static slides, a document, an idle editor | 100–300 kbps |
| Scrolling code, dragging windows | 500 kbps – 1.5 Mbps |
| Full motion — video playback, animation | more than the ceiling allows |

The **ceiling of 2.5 Mbit/s** bounds each share, and there is one share per
call, so the relay's egress for a share is at most `2.5 Mbit/s × (N−1)` — about
22 Mbit/s for a ten-person call. That is what makes sharing safe to have on
without a switch. Full-motion video shared at the ceiling is watchable, not
smooth.

**The hosted plan is flat** (CP-4 is $99/month with no metering). A
screenshare-heavy tenant moves its unit economics with no pricing lever; the
ceiling bounds the damage per call. This is recorded as input to the control
plane's pricing, which is out of scope for this repo.

## 7. Signaling

PROTOCOL.md §5.17 has the frames:

- `CALL_JOIN` carries **the video codecs the joiner can decode**, as bits
  (`OC_CALL_CODEC_VP9` = 1); `CALL_JOINED` and `CALL_ROSTER` carry each
  participant's. Every client decodes VP9, so it is VP9; another codec is a
  new bit, used only when everyone has it.
- `CALL_SHARE {channel, on}` starts or stops sharing. The daemon keeps **one
  sharer per call**: a start takes over. Someone not in the call is refused
  (`NOT_IN_CALL`); a stop from someone not sharing changes nothing.
- `CALL_STATE` carries `sharer` (0 for nobody), so the Calls section, the call
  view and every participant know who is sharing.
- A sharer who leaves, disconnects, is swept by the relay, or rejoins stops
  sharing.

## 8. Client surfaces

**Windows** (`client/gui/win32/winmain.c`), after Slack's huddles:

- **Share screen** beside Invite in the call view, and **Ctrl+Shift+S**, open the
  list of screens and windows — the video recorder's (`oc_capture_list_screens`).
- **While you share**: a small bar at the top of the screen — "You're sharing …"
  and **Stop sharing** — and a **green frame** around what is shared, both kept out
  of the capture where Windows allows it (`WDA_EXCLUDEFROMCAPTURE`), as the
  recording bar is. The call view says "You're sharing …" and does not show the
  picture, which would contain itself. Someone else starting to share stops yours
  and says so.
- **Watching**: the share takes the call view's stage, fitted, under "Alice is
  sharing their screen", with the people as a row of faces beneath it.
  **Full screen** covers the window (Esc returns); **Actual size** shows it one
  pixel to one pixel, scrolled with the wheel (Shift for across).
- **Elsewhere**: the in-call strip says "Alice is sharing", the Calls section's
  row shows a screen, and a toast says "Alice started sharing their screen" when
  the call is not on screen.
- **Accessibility**: `call.share`, `call.share.stop`, `call.share.fullscreen`,
  `call.share.actualsize`, and the picture itself as `call.share.stage`, named
  "Alice's shared screen".

**The TUI** renders no graphics (ARCH-75) and shows no calls, so no shares either.

**Capture** is per platform — Windows Graphics Capture, with the synthetic screen
for tests (`OPENCHIME_TEST_CAPTURE=synthetic`) — and **the codec is not**: frames
cross into `client/core` and are encoded there, so the wire is the same everywhere.

## 9. Tests

- `tests/test_share_media.c`: a sharer and a viewer over a simulated network that
  loses, delays, reorders and duplicates — fragments at the edge sizes, 5% loss
  recovered by NACKs, a whole frame lost and asked for, a frame that cannot be
  recovered given up for a PLI and a keyframe, a late joiner, the rate's halving,
  holding, growth, floor and ceiling, the size's step down and back, malformed
  fragments refused, and the synthetic screen through VP9 and a lossy network back
  to readable frame numbers.
- `tests/itest_netloop.c`: `CALL_SHARE` start, take-over, a stop that is not the
  sharer's, leave and disconnect clearing it, someone outside the call refused,
  and codecs on the roster.
- `tests/test_client_core.c`: through the real daemon and relay, with a tap
  between them — dana shares the synthetic screen and erik decodes it at its size
  with rising frame numbers; the relay carries only SFrame; 10% loss at the tap
  costs NACKs and resends, not the picture; faye joins late and has a picture
  within seconds; erik takes over, which stops dana's; erik stops and the picture
  goes.
- `scripts/gui_calls.sh`: two Win32 clients — alice shares, bob's view shows her
  screen with readable, rising frame numbers; full screen, Esc, actual size; the
  accessibility names; bob takes over; bob stops.
