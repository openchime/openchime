# OpenChime — Calls

How people talk to each other by voice: how a call is started, found, joined and ended,
what the Calls section shows, and how the audio is encrypted end to end so that neither
the daemon nor its relay can hear it, and how a screen is shared in a call (§6). Cross-referenced from ARCHITECTURE.md (ARCH-73,
ARCH-113), REQUIREMENTS.md (§6.2, REQ-150–152, REQ-301–306), PROTOCOL.md (§5.17)
and AUDIO.md (the media engine and echo cancellation).

The client is the Win32 GUI, and the same application in a browser (WEB.md), which has no
UDP and carries a call over its connection (§4); the TUI has no calls.

---

## 1. The model

A call belongs to a **conversation** — a channel, a DM or a group DM — and a
conversation has at most one call at a time (ARCH-73). Anyone who may read the
conversation may join its call. There is no ringing and no answering: a call is
**there**, listed, until it ends.

- **Starting.** From a conversation's header, or from the **+** on the Calls section.
  The starter lands in the call view. Starting in a channel or a group DM **invites every
  member**, up to the cap; where the members outnumber the cap the starter picks who, with
  the most recently active pre-ticked. A DM invites the other person.
- **Invitations** are listed in the Calls section and notified as a mention is (a toast on
  the desktop, a push on a phone), through the same decision a message takes (ARCH-103):
  a muted conversation, the notification schedule and a pause silence them. There is no
  ringing loop. An invitation lasts until it is taken, declined, or the call ends.
- **Inviting more.** Anyone in the call may invite anyone who may read the conversation,
  within the cap.
- **Ending.** A call ends when the last person leaves, or when its **starter ends it** for
  everyone.
- **Missed calls.** A call that ends with nobody but its starter ever having joined leaves
  a **"Missed call"** line in the conversation's history, authored by the starter (§4).
- **The cap.** At most ten people in a call (`OC_CALL_MAX`; development mode may set
  `OPENCHIME_DEV_CALL_MAX`, 2 to 32). A join or an invitation past it is refused with `CALL_FULL`. In a public demo a call ends after twenty minutes, and each person may join thirty a day (AUTH.md §8.12).
- **One call at a time.** A connection is in at most one call; joining another leaves the
  first. A user is in a call from one device: joining from a second moves them.
- **Losing the connection is not leaving.** The daemon holds a disconnected participant's
  seat for 15 s, and the client gets back into the call by itself — after a reconnect, a
  sweep, or its media path moving — saying "Reconnecting to the call…" meanwhile, and
  "Lost the call" if it cannot (§4). A call whose members all lose their connection at
  once is still there when they come back.

Calls are ephemeral, as presence is (ARCH-67): the daemon holds them in memory, and a
restart ends them.

## 2. The Calls section

The Home sidebar has a **Calls** section above Channels and Direct messages. It lists the
active calls in conversations you belong to, and every call you are invited to, one row
each: the conversation, who is in it, how many, and an **Invited** mark. Selecting a row
opens the **call view**. With nothing to list it says "No calls".

**The call view** shows the conversation's name and how long the call has run, the roster —
each person's avatar, a ring while they speak, a mark when they are muted, and a volume for
each — and the controls: **Mute**, the push-to-talk hint, the **microphone** and
**speaker** pickers with a live level, **Noise suppression**, **Invite**, **Share screen**,
**Leave** and, for the starter, **End for everyone**. Before joining it shows who is in the call and **Join**
(and **Decline** for an invitation). The conversation's messages are a click away.

**While in a call elsewhere**, a strip at the foot of the sidebar shows the conversation and
the call's length — or who is sharing a screen — with Mute, Leave, and a way back to the
call view.

**Sharing a screen** (REQ-161): anyone in the call can share a screen or a window, one
person at a time — starting a share takes over from whoever was sharing. The others see it
on the call view's stage, fitted, or full screen, or at actual size; the sharer sees a bar
saying so, with Stop sharing, and a green frame around what is shared. §6
is the design.

**Keys.** Ctrl+Shift+M mutes and unmutes. Holding **Ctrl+Shift+Space** talks while muted
(push to talk) — voice input's chord; in a call the call owns the microphone, so the two
never compete. **Ctrl+Shift+S** shares your screen, or stops sharing it.

**Devices and noise suppression** are remembered with the other preferences: the devices
by a hash of their ids, so another machine, which has other ids, uses its own defaults.

**Invitation toasts** carry **Join** and **Decline**, and the call sound.

## 3. The microphone and the media engine

A call owns the microphone, as voice input and the video recorder do; the device layer
refuses a second capture (ARCH-112). Joining a call stops voice input and closes the video
recorder.

The engine is AUDIO.md's design, in `client/core/call/`:

- **16 kHz mono, 20 ms frames.** Capture passes the **echo canceller**
  (`OC_PROCESSOR_SPEEX`) against the playback reference — the call's own mix plays through
  the device layer, so the reference carries it — then speexdsp's **preprocessor**: noise
  suppression and automatic gain, which the call view can switch off. The
  mute and push-to-talk gate follows. Muted, a client sends no audio: once a second it
  sends a packet saying it is muted, encrypted like the audio, which is how the others
  show the mark and the daemon cannot see it.
- **Opus**, VoIP mode, 24 kbit/s, **in-band FEC** with a packet-loss hint from the loss
  observed, and **DTX**, so a silent speaker sends almost nothing.
- **Keep-alive.** A packet at least every 5 s, even while DTX sends nothing, so the relay's
  silence sweep (20 s) only drops the vanished.
- **A jitter buffer per sender**, ordered by frame number, with duplicates dropped; loss is
  counted from gaps in the authenticated packet counter. Its target delay is the 95th
  percentile of recent arrival delay, between 40 and 240 ms; it is reached by waiting and
  given back only in silence, so speech is never cut. A lost frame is recovered from the
  next packet's FEC when that has arrived, otherwise concealed by Opus; after three
  concealed frames it is silence.
- **The mixer** pulls one frame per sender every 20 ms, applies each person's volume, sums
  and soft-limits. **Speaking** marks come from each sender's decoded level — the daemon
  cannot know, since it never hears the audio.

## 4. Signaling

PROTOCOL.md §5.17 has the frames. In short:

- `CALL_JOIN {channel, device key, codecs, invitees}` — start or join. Starting names the
  invitees; joining an existing call names none. `codecs` says which video codecs the
  joiner can decode.
- `CALL_JOINED` to the joiner: the relay's port, the joiner's media token and slot, the
  call's id, starter, start time and epoch, and every participant with their device key.
  `CALL_ROSTER` to the others: the participants and the epoch, on every change.
- `CALL_INVITE`, `CALL_DECLINE`, `CALL_END` (the starter only), `CALL_LEAVE`.
- `CALL_SHARE {channel, on}` — start or stop sharing a screen; one sharer at a time.
- `CALL_STATE {channel, call, starter, started, participants, invited, sharer, ended}` to every
  member of the conversation, every invitee and every participant on every change, and at
  sign-in for each call there is: what the Calls section lists.
- `CALL_KEY` / `CALL_KEY_FOR`: sealed media keys (§5), forwarded unread.

The cap rides on `WORKSPACE_INFO` (`call_max`), so a client starting a call in a large
channel knows when the starter has to pick; and `CAPABILITIES` names `calls` while the
relay is up to carry them, so a client shows no call control a daemon cannot back.

**The missed-call line** is a message whose **kind** is *call event* (SCHEMA.md
`messages.kind`), written by the daemon, with the body "Missed call" so it reads sensibly
anywhere. It is not searchable, not read aloud and never notifies. ARCH-90 excludes system
messages; this is the one kind it admits.

**The relay** runs in the daemon's event loop (ARCH-18/31): it forwards opaque payloads
tagged with the sender and never decodes them — audio, a shared screen and the viewers'
requests alike. What it forwards is ciphertext. When its silence sweep drops a participant
the loop removes them from the call so the roster stays honest, and tells that participant,
if its connection is still open, with a roster without it.

**Getting back in** (PROTOCOL.md §5.17, `client/core/callsig.c`). A device put out of a call
by anything but its own act stops its media — its token is dead — and remembers the call:

- **its connection dropped:** once a connection signs in again, it rejoins if the sign-in
  `CALL_STATE` still lists the call, and gives up if the call ended, another took its place,
  or none is reported within 5 s;
- **a roster without it:** if the roster names its user on another device, it moved there
  and is out; otherwise it was swept, and it rejoins at once;
- **its media path moved:** the engine reports UDP that worked going quiet for 12 s (AUDIO.md
  §4), and it rejoins for a fresh token, which the relay binds to the new address.

It gives up 60 s after the loss, when the rejoin is refused, or after three rejoins in 60 s.
Leaving while getting back in stops it, and a client closing on purpose sends `CALL_LEAVE`
first. Each rejoin is an ordinary join: a new epoch, new keys (§5.3). A participant whose
network passes no UDP carries the same packets over its connection (`CALL_MEDIA`, offered
as `calls-tcp`; PROTOCOL.md §5.17, AUDIO.md §4), and the relay forwards between the two
transports; nothing about the encryption changes, since the packets are the same.

## 5. End-to-end encryption

The daemon and the relay can **never** hear a call. Every packet is encrypted by the
sender's device with a key only the other participants' devices hold.

### 5.1 Pieces

- **SFrame** (RFC 9605) encrypts each packet, suite **AES_128_GCM_SHA256_128** (`0x0004`).
- **HPKE** (RFC 9180) in **Auth mode** — `DHKEM(X25519, HKDF-SHA256)`, `HKDF-SHA256`,
  `AES-128-GCM` — carries each sender's key to each receiver. Auth mode authenticates the
  sender's static key as well as encrypting to the receiver's, so a key cannot be forged
  in another participant's name without that participant's private key, and no signature
  scheme is needed.
- Both over **mbedTLS** (X25519, HKDF, AES-GCM, CTR-DRBG), already vendored. Secrets are
  wiped when done with; comparisons of authentication data are constant-time (mbedTLS's).

### 5.2 Device keys

Each client device holds an **X25519 key pair per workspace**, made on the first call and
kept in the OS credential store beside the session token (`client/core/store.c`); a client
with no store (ARCH-88) makes one for the session. The **private key never leaves the
device.** The public key rides on `CALL_JOIN`; the daemon keeps it for the length of the
participation and hands it to the others in `CALL_JOINED` and `CALL_ROSTER`.

### 5.3 Media keys and epochs

The daemon numbers the call's membership: the **epoch** starts at 1 and goes up by one on
every join and every leave, and when a participant's connection goes and its seat is held.
On each epoch every participant device:

1. makes a fresh random 16-byte SFrame **base key**, used only by it, only for this epoch;
2. **seals** it to every other participant's device key with HPKE Auth mode (its own
   device key as the sender's), with

   ```
   info = "OpenChime call key v1" ‖ call_id(u64) ‖ epoch(u32)
          ‖ sender_user(u64) ‖ recipient_user(u64)
   ```

   and an empty AAD, so a sealed key opens only for that call, that epoch, that sender and
   that recipient;
3. sends the sealed copies in one `CALL_KEY`. The daemon checks that the sender and every
   recipient are in the call and forwards each copy as `CALL_KEY_FOR`; it cannot open them.

A receiver opens a `CALL_KEY_FOR` with its own private key and the **sender's device key
from the roster**, for the epoch it names. It installs a key only for the current epoch or
the one before — a key can cross a roster on the way — and only once per (sender, epoch):
a replayed or stale copy is ignored.

**Switching.** The sender keeps encrypting with its previous key for **1 s** after the new
epoch, so receivers have the new one before it is used, then switches. Receivers keep a
sender's previous key for **5 s** after the switch, for packets in flight, then wipe it.
A joiner is never given a previous key, so it hears the others from the end of their
grace — about a second after joining — and not before; the test that measures a join
counts those packets and requires none after.

### 5.4 Packets

Each packet is an SFrame ciphertext: the SFrame header (KID, CTR), then the AES-GCM
encryption, with the header as AAD, of a plaintext whose first byte is its **type**:

| Type | Then |
|---|---|
| 0 audio | the frame number (u32) and the Opus packet |
| 1 state | one byte: bit 0 muted |
| 2 video | a fragment of a shared screen's frame (§6.5.2) |
| 3 control | a viewer's NACK, keyframe request or report, to the sharer (§6.5.3) |

Every type is sealed under the same key with the same counter, so the counter never
repeats however the types interleave.

- **KID** = `epoch << 8 | slot`, where the **slot** (0–255) is the number the daemon gives a
  participant for as long as they are in the call. A KID therefore names one sender's key
  for one epoch.
- **CTR** is the sender's packet counter, restarted at 0 with each new key. Every key is
  random and used by one sender, and a counter never repeats under one key, so a (key,
  nonce) pair is never reused (RFC 9605 §4.4.1).
- The SFrame key and salt are derived from the base key as RFC 9605 §4.4.2 says.

The relay's own framing is unchanged: `token ‖ seq ‖ payload` in, `sender ‖ seq ‖ payload`
out, with the SFrame ciphertext as the payload. Its 16-bit `seq` is unauthenticated and not
used: the jitter buffer orders by the frame number inside the encryption, and loss is
counted from the authenticated CTR.

A receiver drops a packet whose KID it has no key for, whose authentication fails, or whose
CTR is older than a **1024-packet window** or already seen in it (replay).

### 5.5 What it guarantees

- The daemon, the relay and the network cannot decrypt any audio, or any shared screen.
- A packet altered or replayed on the way is dropped.
- **Someone who leaves** cannot decrypt what is said after they leave — past the 1 s grace
  in which the others still use the previous epoch's key (and the relay stops forwarding
  to a leaver at once).
- **Someone who joins** cannot decrypt what was said before they joined. A rejoin is a
  join: nothing said while a device was out is decryptable to it, and a seat held for a
  disconnected device receives nothing — its token is revoked at the disconnect.

### 5.6 What it does not

- **The daemon hands out the device keys.** A daemon that substituted its own key for a
  participant's could sit in the middle of the key exchange. Detecting that needs a
  **safety number** the people can compare, and there is none: the
  guarantee holds against a daemon that follows the protocol and against everything
  between the daemon and the devices — the network, the relay, a copy of its memory or
  database — but not against a daemon that has been altered to attack its own users.
- **Metadata is visible to the server**: who is in which call, when, and the timing and
  sizes of their packets.
- **Within a call**, every participant holds every sender's key, so a participant could
  forge audio in another's name (RFC 9605 §9.5). The KID is bound to a slot, and the
  receiver checks that it agrees with the sender the relay names, which stops a forgery
  through the relay — not a malicious participant who also controls the relay.

### 5.7 Tests

`tests/test_e2e.c` checks both constructions against the RFC test vectors (RFC 9180 A.1.3,
Auth mode; RFC 9605 C.1 headers and C.3 suite 0x0004), and then what the vectors do not
cover: tampering, a wrong key or another sender, another info or aad, low-order keys, the
replay window. The client end-to-end test, which runs the switches between epochs, (`test_client_core.c`) taps the relay and checks it sees only
ciphertext, and that after a participant leaves every packet is under an epoch it was
never given a key for; and a shared screen crosses it as SFrame too (§6.9).
`scripts/gui/calls.sh` runs two Win32 clients (two web clients with `OC_TEST_PLATFORM=web`) in a call and checks each receives the other's
packets, every one decrypting once the keys are in.

## 6. Screen sharing

What screen sharing is and is not, why it rides the call relay unchanged, the
codec, and the transport built on top of it — fragments, loss recovery and rate
control. Cross-referenced from ARCHITECTURE.md (ARCH-86, ARCH-87) and
REQUIREMENTS.md (§6.3, REQ-160/161). Camera video calling remains excluded by
REQ-160 and nothing here reverses that (§6.2). Recorded **video messages** are a
separate design with no real-time path: [VIDEO-MESSAGES.md](./VIDEO-MESSAGES.md).

### 6.1 Why it is in scope at all

Screenshare is the one video-shaped feature with a clear, bounded work-chat
justification: showing someone an error, a diff, a design, or a spreadsheet.
Both reference products ship it — Slack in huddles, Pumble from its $2.49 Pro
tier. It buys **parity, not differentiation**.

### 6.2 Scope — screenshare is not video calling

| In scope | Out of scope |
|---|---|
| One participant shares a screen or window; the others view it | Camera / webcam video (REQ-160 stands) |
| View-only for the viewers | Remote control of the sharer's machine |
| Screen audio is **not** captured; voice rides the call's audio | Recording the share |
| **One sharer at a time**; starting a share takes over from whoever was sharing | Simultaneous shares / picture-in-picture grids |
| | Drawing on someone's share |

Sharing is part of every call: there is no switch to turn it off. What bounds its
cost is the bitrate ceiling (§6.6).

### 6.3 It rides the existing relay unchanged

**The relay never decodes anything.** It forwards opaque payloads tagged with the
sender (`daemon/audio.h`), and a share's packets relay exactly as audio's do. So
screenshare needs **no server-side codec and no change to the relay's framing or
forwarding** (ARCH-86); the daemon stays free of libvpx as it is of libopus. A
participant whose network passes no UDP sends and receives the share over its
connection, as its audio (`CALL_MEDIA`, AUDIO.md §4), and nothing about the share
changes: a packet the daemon drops rather than let a backed-up connection grow is
recovered by the same resend and keyframe requests as a lost datagram.

The same property has a hard consequence:

> **There is no transcoding fallback, and there cannot be one.** A conventional
> media server bridges participants that disagree on a codec by decoding and
> re-encoding. ARCH-18/73 forbid the server touching the payload at all. A call
> can contain a macOS client and a Windows client simultaneously, so if two
> clients disagree on the codec **the call simply does not work**.

That makes the codec a **wire contract** (ARCH-87), carried on `CALL_JOIN` (§6.7).

The relay does drain its socket in batches and asks for a 4 MB socket buffer each
way, since a keyframe arrives as a burst of a hundred packets or more.

### 6.4 Codec — VP9 via libvpx (ARCH-87)

**Exactly one mandatory codec: VP9, encoded and decoded with libvpx
(BSD-3-Clause), with screen-content tuning enabled** — the libvpx the video
recorder already links (VENDORS.md).

Why VP9:

- **Permissive licence.** BSD-3-Clause.
- **Royalty-free in practice.** Google's patent grant; the MPEG-LA VP8 pool
  settled in 2013. Contrast **openh264**, whose BSD licence covers only the
  binaries **Cisco itself distributes**; **x264/x265 are GPL**.
- **Identical on every client by construction.** One C source built for every
  platform — what §6.3's no-transcode constraint requires.
- **Screen-content tuning.** `VP9E_SET_TUNE_CONTENT` = `VP9E_CONTENT_SCREEN`
  keeps text's hard edges sharp at low bitrate. An unreadable share is the one
  failure screenshare cannot have.
- **Decode is cheap.** One participant encodes; N decode.

**Rejected alternatives**, recorded so they are not relitigated:

| Option | Why not |
|---|---|
| **Platform-native codecs** (Media Foundation, VideoToolbox, MediaCodec) | Fatal under §6.3: a Mac and a Windows client in one call must interoperate, and the server cannot bridge them. |
| **openh264** | Patent grant covers only Cisco-distributed binaries. |
| **x264 / x265** | GPL — fails the licence posture. |
| **FFmpeg / libavcodec** | An enormous dependency for one codec. |
| **AV1** (SVT-AV1 + dav1d) | The best screen-content option and royalty-free, but realtime software encode is materially harder. **The designated successor**: a client says which codecs it can decode (§6.7), so a second codec can arrive without a flag day. |
| **VP8** | Cheaper CPU, but no screen-content tuning. |

The encoder (`oc_vp9enc_open_share`) is realtime, `cpu-used` 8, CBR at the
bitrate rate control sets (§6.6), with each frame's duration taken from the time
since the one before — a share's frame rate varies — and **keyframes only when
asked for**: the loss recovery below decides when one is needed.

### 6.5 The media path

Everything below is in the client, inside the call's encryption. The pure rules —
fragmenting, reassembly, NACK, PLI, rate — are `client/core/call/share.c`
(`oc_share.h`), with no sockets or threads; the call engine
(`client/core/call/call_engine.c`) runs them.

#### 6.5.1 Packets

Every call packet's plaintext starts with a **type** (§5.4):

| Type | Plaintext after the type byte |
|---|---|
| 0 audio | `frame(u32) ‖ opus` |
| 1 state | `flags(u8)` — bit 0 muted |
| 2 video | `frame(u32) ‖ frag(u16) ‖ nfrags(u16) ‖ flags(u8) ‖ width(u16) ‖ height(u16) ‖ bytes` |
| 3 control | `target(u64) ‖ kind(u8) ‖ body` |

All four are sealed under the sender's SFrame key for the epoch with **one
counter across the types**, so a nonce is never reused. Nothing new is added to
key exchange.

#### 6.5.2 Fragments

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

#### 6.5.3 Loss recovery: NACK, then PLI

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

#### 6.5.4 Rate control

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

#### 6.5.5 What is sent

The source is captured at up to **15 fps**, fitted inside **1920×1080** (**960×540** in a
browser, `oc_plat_share_limits`). A frame
identical to the last one sent (a fingerprint of its pixels) is skipped, unless
half a second has passed: a still screen goes at **2 fps**, which is what lets VP9
sharpen it and costs almost nothing. Below 600 kbit/s the frame is fitted inside
**1280×720** instead, and back at the full size above 1000 — the gap keeps it from
flapping. A change of size reopens the encoder with a keyframe.

#### 6.5.6 The engine

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

### 6.6 Bandwidth

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

### 6.7 Signaling

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

### 6.8 Client surfaces

**Windows** (`client/gui/app/app.c`), after Slack's huddles:

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

**Capture** is per platform — Windows Graphics Capture, `getDisplayMedia` in a browser
(`cap_web.c`), with the synthetic screen
for tests (`OPENCHIME_DEV_CAPTURE=synthetic`) — and **the codec is not**: frames
cross into `client/core` and are encoded there, so the wire is the same everywhere.

### 6.9 Tests

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
- `scripts/gui/calls.sh`: two Win32 clients, or two web clients — alice shares, bob's view shows her
  screen with readable, rising frame numbers; full screen, Esc, actual size; the
  accessibility names; bob takes over; bob stops.
