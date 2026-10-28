# OpenChime: Encrypted DMs

An encrypted DM is a direct message between two people that only one device of each can
read. It is end to end encrypted in the sense of the IETF's definition
(draft-knodel-e2ee-definition, §2): the content is decrypted only at the ends, and the
daemon that carries it can never decrypt it. It is an ordinary DM channel marked `e2e`:
its messages are sent, edited, deleted, reacted to and read like any DM's, but every body
is ciphertext sealed on the sender's device. A person chooses it when what they say must
stay between two devices; the ordinary DM, which keeps every server feature, is unchanged
beside it.

This document is the design. Built: the cryptography (`shared/noise_xx.c`, `shared/ratchet.c`),
the daemon's side (PROTOCOL.md §5.19, the `e2e` byte of `CHANNEL_INFO` and `CHANNEL_LIST`,
SCHEMA.md §3bb), the client core (`client/core/pdm.c`) and the Win32, web and terminal
clients (ARCH-118, REQ-314). In the code and on the wire the session still goes by its old
name, `pdm` (private DM).

---

## 1. What an encrypted DM is

- **A DM channel, new each time.** Starting one makes a new `kind='dm'` channel with the
  two people as members and `e2e=1`. It never meets the pair's ordinary DM or an earlier
  encrypted one: each conversation is its own channel. Its id is the conversation's id
  everywhere, the handshake frames included.
- **Two devices.** The device that starts it and the device of the other person that
  accepts it. The other person's other devices see the invitation until one accepts it,
  and then not at all; the starter's other devices never see it. A client hides an
  encrypted DM it holds no session for, and the daemon refuses writes to a live one from
  any other connection (§2).
- **Short-lived.** Every message has its own key, used once and destroyed. The session
  lives in the memory of the two apps and ends when either side ends it or goes away,
  when the daemon restarts, or after 24 hours or 10,000 messages, after which the two can
  start a new one. What was said is readable on the two devices while they run, and by
  nobody afterwards: not the daemon, not the two people, not anyone who later takes
  either device.
- **Verified by the people.** Each side shows a safety number; two people who compare it
  in person or on a call know no one is in the middle. The first key is trusted; a later
  change is shown as a warning, and nothing more is sent until the person trusts it.
- **Text only.** No files, and nothing that would carry the text out of the conversation
  in the clear (§2).

## 2. The daemon

**Opening.** A client starts one with `PDM_OPEN` (the peer and the first handshake
message). The daemon makes the channel (`kind='dm'`, `e2e=1`, a `dm_key` of the form
`e:<lo>,<hi>:<ms>.<seq>` so it is unique), then sends the opener `PDM_OPENED` and the
channel's `CHANNEL_INFO` (`e2e=1`), and sends `PDM_INVITE` to each of the peer's
connections. The peer's device that answers first (`PDM_HS`) is given the channel's
`CHANNEL_INFO` then; the others are told it was taken. The rest of the handshake and the
opener's first, empty ratchet message travel as `PDM_*` frames (PROTOCOL.md §5.19).

**Messages.** After that the conversation uses the ordinary frames: `SEND`, `SEND_REPLY`,
`EDIT`, `DELETE`, `REACT`, `TYPING` and read cursors. A body is `oc-e2e:1:` followed by
the base64 of the 40-byte ratchet header and the AES-GCM ciphertext. The daemon stores
and relays it as given. While the conversation is live its results go only to its two
bound connections; `SEND`, `SEND_REPLY`, `EDIT`, `DELETE` and `REACT` from any other
connection are refused with `FORBIDDEN`, and `TYPING` from one is dropped.

**What the server sees.** Who talks to whom and when (the channel, its members and each
message's author and time), each message's size, which messages are edited or deleted,
the reaction emoji (a reaction is not encrypted), typing, and read cursors and unread
counts. It never sees what was said. The ciphertext rows are in the database, and so in
its backups, until the channel is removed.

**What the server does not do with it.** Everything that would read a body leaves an
encrypted DM alone:

- not indexed for search (migration 0063's triggers skip `e2e` channels), and `SEARCH`
  leaves e2e channels out;
- no mentions, keyword hits, actions, link previews, summaries, read-aloud or voice input;
- no forward into or out of it;
- no body text anywhere the server excerpts one: the channel list's preview, activity,
  threads, pins and saved items are blank for it;
- a deleted message is not held for undo;
- uploads and attachments are refused (`FORBIDDEN`), and so are incoming webhooks,
  scheduled messages, drafts, pins and saves.

A push notification carries no text for any message, and is not treated specially here.

**Ending and cleanup.** `PDM_END` from either device, or a side away too long, ends the
session and closes the channel (`archived_at_ms` set). A closed encrypted DM is immutable:
the daemon refuses any message, reply, edit, delete or reaction in it
(`CHANNEL_ARCHIVED`), and the clients no longer offer them.
At daemon start every open encrypted DM is closed, since the sessions it joined are gone.
The storage maintenance pass, every 300 seconds, deletes each encrypted DM closed more
than 24 hours ago, with every row that names the channel or one of its messages.

**The web client.** Its code is served by the workspace's daemon. A daemon that is
compromised could serve code that takes the keys, which an installed client is not exposed
to. An encrypted DM in the browser still keeps its content from the database, the backups
and the storage provider; the web client says so in every encrypted DM it shows.

## 3. The handshake

`Noise_XX_25519_AESGCM_SHA256` (the Noise Protocol Framework, revision 34), three messages
relayed by the daemon:

    -> e
    <- e, ee, s, es
    -> s, se

- **Static keys** are the X25519 device keys the clients already hold for calls (ARCH-113,
  CALLS.md §5), one per device and workspace, kept in the credential store and destroyed at
  sign-out. Signing out and back in is therefore a new key, and the other side's warning.
- **The prologue** binds a version string and both user ids, the opener's first, so a
  handshake cannot be passed off between other people or in the other direction.
  Another workspace or another DM cannot reuse one either: each device's static key
  is its own per workspace, and both sides' ephemerals are fresh for every handshake.
- **The result** is mutual authentication (each side has proved its static key) and forward
  secrecy (the ephemerals are destroyed), without a signature scheme, which the vendored
  mbedTLS lacks for Ed25519.
- `shared/noise_xx.c` reproduces the published test vectors (cacophony,
  `Noise_XX_25519_AESGCM_SHA256`) byte for byte (`tests/test_pdm_crypto.c`).

## 4. The messages

A Double Ratchet (the Signal specification, revision 4) started from the handshake:

- **The root key** is a third HKDF output of the handshake's final chaining key; **Bob's
  first ratchet key** is the responder's handshake ephemeral. The initiator can send at once,
  and does: the third handshake message is followed by a first, empty ratchet message
  (`PDM_SEND`), so the responder can answer.
- **Every message** takes the next key of its chain (HMAC-SHA256), is encrypted with
  AES-256-GCM under a key and nonce derived from it, and the key is wiped. The header (the
  sender's ratchet public key, the previous chain's length and the message number) and the
  handshake hash are the associated data, so a message opens only in its own session.
- **Every change of speaker** brings a new X25519 ratchet key: a key taken from a device stops
  working after the next reply in each direction.
- **Out of order** delivery is tolerated: the keys of up to 256 skipped messages are kept and
  each is wiped when used. A replay, a key already used, an altered message or more than 256
  skipped are refused, and a refused message leaves the session exactly as it was.
- **On the client** the body is sealed on the network thread on the way out and opened
  there on the way in. A ratchet cannot open its own output, and opens each message only
  once, so the client keeps, in memory, the text of every body it sealed or opened, found
  by a digest of the sealed body: its own message coming back, a catch-up after a
  reconnect and an edit read from that. An edit is a new sealed body. A body this device
  cannot open reads as "Encrypted message: only the device that took part in this
  conversation can read it." The texts are wiped when the conversation is put away or
  the app closes.

## 5. The safety number

SHA-256 over both static public keys and both user ids, in a fixed order, shown as 12 groups
of 5 digits; it is the same on both devices. The client keeps a 16-byte fingerprint of the
other device's key for each person it has had an encrypted DM with, in the credential store
beside its own key (credential blob version 4, at most 16 peers, bounded to fit the Windows
credential blob), which is how a later key change is noticed. Nothing else about an
encrypted DM is written to disk on a client.

## 6. The clients

**Starting one.** Only from New message, for one person, with "End-to-end encrypted"
ticked (the terminal client: New direct message, a people picker where typing filters,
Space toggles encrypted and Enter opens). The text typed in New message is sent once the
conversation is open. While the box is ticked, New message's draft is not saved to the
server.

**Win32 and web.** The encrypted DM opens in the normal conversation view, with a lock in
its header and its sidebar row, "Safety number" and "End" buttons, and a status strip:
waiting for the other device, open, the key changed (with "Compare and trust"), or ended
(with "Put away", which wipes and removes it). An invitation is a strip with "Accept on
this device" and "Decline". Not offered in it: attach, video, voice input, Send later, the
Files and Pins tabs, the pin, forward, copy link and save menu items, read aloud and
summarize. Files dropped or pasted into it are refused.

**Terminal.** An encrypted DM is a normal DM row with a lock, with a status line for its
state and safety number; Ctrl+K offers accept, decline, trust the new key, end and forget.

## 7. Not offered

Groups; more than one device per person; keeping anything after the app closes; files;
server features that read a message's text.
