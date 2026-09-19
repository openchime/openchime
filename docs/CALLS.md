# OpenChime — Calls

How people talk to each other by voice: how a call is started, found, joined and ended,
what the Calls section shows, and how the audio is encrypted end to end so that neither
the daemon nor its relay can hear it. Cross-referenced from ARCHITECTURE.md (ARCH-73,
ARCH-113), REQUIREMENTS.md (§6.2, REQ-150–152, REQ-301–306), PROTOCOL.md (§5.17)
and AUDIO.md (the media engine and echo cancellation).

The client is the Win32 GUI; the TUI has no calls.

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
- **The cap.** At most `OPENCHIME_CALL_MAX` people in a call (default 10, from 2 to 32;
  CONFIG.md). A join or an invitation past it is refused with `CALL_FULL`.
- **One call at a time.** A connection is in at most one call; joining another leaves the
  first. A user is in a call from one device: joining from a second moves them.

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
**speaker** pickers with a live level, **Noise suppression**, **Invite**, **Leave** and, for
the starter, **End for everyone**. Before joining it shows who is in the call and **Join**
(and **Decline** for an invitation). The conversation's messages are a click away.

**While in a call elsewhere**, a strip at the foot of the sidebar shows the conversation and
the call's length, with Mute, Leave, and a way back to the call view.

**Keys.** Ctrl+Shift+M mutes and unmutes. Holding **Ctrl+Shift+Space** talks while muted
(push to talk) — voice input's chord; in a call the call owns the microphone, so the two
never compete.

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

- `CALL_JOIN {channel, device key, invitees}` — start or join. Starting names the invitees;
  joining an existing call names none.
- `CALL_JOINED` to the joiner: the relay's port, the joiner's media token and slot, the
  call's id, starter, start time and epoch, and every participant with their device key.
  `CALL_ROSTER` to the others: the participants and the epoch, on every change.
- `CALL_INVITE`, `CALL_DECLINE`, `CALL_END` (the starter only), `CALL_LEAVE`.
- `CALL_STATE {channel, call, starter, started, participants, invited, ended}` to every
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

**The relay** (ARCH-31) is unchanged: it forwards opaque payloads tagged with the sender and
never decodes them. What it forwards is now ciphertext. When its silence sweep drops a
participant it tells the daemon, which removes them from the call so the roster stays
honest.

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
every join and every leave. On each epoch every participant device:

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
encryption, with the header as AAD, of the frame number (u32) and the Opus packet — or,
with the frame number `0xFFFFFFFF`, of one byte of state (bit 0: muted).

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

- The daemon, the relay and the network cannot decrypt any audio.
- A packet altered or replayed on the way is dropped.
- **Someone who leaves** cannot decrypt what is said after they leave — past the 1 s grace
  in which the others still use the previous epoch's key (and the relay stops forwarding
  to a leaver at once).
- **Someone who joins** cannot decrypt what was said before they joined.

### 5.6 What it does not

- **The daemon hands out the device keys.** A daemon that substituted its own key for a
  participant's could sit in the middle of the key exchange. Detecting that needs a
  **safety number** the people can compare, which is not built; until it is, the
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
never given a key for. `scripts/gui_calls.sh` runs two Win32 clients in a call and checks
each receives the other's packets, every one decrypting once the keys are in.
