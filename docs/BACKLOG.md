# OpenChime — Backlog

**The project's only issue list.** Every known open item for the **Win32 client
and the daemon** is here, numbered, in one order. Nothing else in the repository
tracks issues: `WIN32_BACKLOG.md`, `FEATURE_AUDIT.md`, `STATUS.md` and
`CLIENT_GAP_ANALYSIS.md` are deleted, their contents folded into this list, into
[REQUIREMENTS.md](./REQUIREMENTS.md) (which now records per requirement whether
it is built), or into [README.md](../README.md).

**Scope.** Win32 (`client/gui/win32/`) and the daemon. The TUI, GTK and macOS
frontends carry no entries: the frontend order is fixed — all of Win32 first
(ARCH-74) — so a capability the app-core exposes and only the TUI lacks is a
consequence of that order rather than an open item. Work whose implementation
lives in the separate control-plane repository is likewise out of scope, and is
listed at the end so the boundary is visible rather than implied.

**Ordering: item 1 is the lowest impact, item 118 the highest.** Impact is what
the issue costs a user or an operator, judged on three things in order — whether
it breaks something that ships and is used, whether it blocks a capability the
product claims, and how many people meet it. So a defect in a working feature
outranks an absent one, and both outrank hygiene. **The number is a position,
not an identifier**: it moves when items are added or closed. Cite an item by
its title.

**Evidence standard.** Every entry states what was observed rather than what was
inferred, and carries the file:line or the command that shows it. Two were
reproduced end to end against a daemon built from this commit; the rest were
verified by reading the code they describe. No entry asserts a cause it did not
confirm.

Several entries record an effect that was **tested and found not to occur** and
say so in place of a severity. They are kept rather than dropped, because "this
looks wrong and is not, for this reason" is what stops the same finding being
rediscovered as a defect by the next reader.

Verified on **2026-08-02** against commit `453782f`, with the daemon and
`build/openchime.exe` rebuilt from that tree.

---

## 1. `.gitignore` whitelists a directory that no longer exists

`third_party/sqlite/` is whitelisted (`.gitignore:12`) after the vendored SQLite
was removed with ARCH-88. It matches nothing.

*Impact:* none. Recorded so the whitelist is not read as evidence that a
client still vendors a database engine. **Verified** — `git ls-files third_party`
lists no `sqlite` path.

**FIXED 2026-08-02.** The whitelist line is removed. The tracked set under
`third_party/` is byte-identical before and after (23 files), the four real
whitelists still resolve as not-ignored, and `third_party/sqlite/` now reports
as ignored — which is what proves the line was doing nothing but is gone.
VENDORS.md's "five paths" is corrected to four.

## 2. jsmn ships without its licence file

`third_party/jsmn/` contains only `jsmn.h`. termbox2 and utf8proc each carry a
licence file; jsmn's MIT text exists only in the header comment.

*Impact:* an MIT attribution nicety, not a functional one. **Verified** —
`ls third_party/jsmn/`.

**FIXED 2026-08-02.** `third_party/jsmn/LICENSE` is committed, extracted
verbatim from the notice in `jsmn.h` rather than typed from memory, so it
provably matches the vendored source: all 19 lines of the header's notice appear
in it. The file is tracked and not caught by the `third_party/*` ignore rule.
All three MIT-vendored libraries now carry a licence file.

## 3. The MinIO dev images are not version-pinned

`docker-compose.yml` uses `minio/minio:latest` and `minio/mc:latest`.

*Impact:* a dev-stack reproducibility gap. MinIO is dev/test only and has no
consumer today (the blob backend defaults to local disk), so nothing shipped
depends on it. **Verified** — VENDORS.md §4 records the same gap.

**FIXED 2026-08-02.** All three are pinned by **digest**, which is immutable
where a release tag can be moved. **The item's title understated the scope**:
`docker-compose.federated.yml` carried a third unpinned image,
`curlimages/curl:latest`, that this backlog had missed — it is pinned with the
other two rather than left as a known defect one file away. Each digest was
resolved from Docker Hub and checked to resolve with `docker manifest inspect`;
both compose files still parse under `docker compose config` and resolve to the
pinned digests. `postgres:17` is deliberately left as a major-version tag, which
is the upstream's own stability contract.

## 4. The mbedTLS fetch verifies no checksum

`scripts/build_mbedtls.sh` downloads the release tarball over TLS from GitHub and
does not check a pinned hash.

*Impact:* a supply-chain hardening opportunity. The transport is authenticated;
the artifact is not. **Verified** — VENDORS.md §2 records it.

**FIXED 2026-08-02.** Both fetch scripts — the host one and the Windows
cross-compile one, which had the same gap — verify the tarball's SHA-256 before
unpacking. The expected value is not one I computed from my own download: it is
the digest **upstream publishes** in the `mbedtls-3.6.2-sha256sum.txt` release
asset, and our download matched it exactly. An unknown `MBEDTLS_VERSION` is
refused rather than fetched unverified.

Tested on all three paths, because a check that has never failed is a comment:
the real tarball is accepted; a tampered copy is **rejected and deleted**
(mismatch reported with both digests); an unpinned version exits with the
command that retrieves the upstream sum. Then end to end — the existing tree was
moved aside and `scripts/build_mbedtls.sh` re-fetched, verified, patched in
`MBEDTLS_THREADING_C` and built all three static libraries, after which the
daemon rebuilt and `make test` passed against them.

## 5. The Windows binary ships unstripped

make windows-gui ships a 4.4 MB unstripped binary; the Makefile never strips.

*Impact:* Downloads carry debug symbols. Any size figure must be re-measured, never
quoted from history.

**Verified** — Makefile:164 'No runtime cost; strip on release if size matters.'; Makefile:193
windows-gui has no strip step; 4.4 MB measured 2026-08-02 at commit 453782f

**FIXED 2026-08-02 — split-debug, not a plain strip.** A plain strip would have
traded every future crash report for a smaller download: the client writes real
minidumps (`crash_filter`), and symbolicating a mingw build needs its DWARF. So
`make windows-gui` now splits the symbols into `build/openchime.debug`, strips
the shipped `openchime.exe`, and records a `.gnu_debuglink` so a debugger loads
them back.

Measured: the shipped binary goes **4,399,617 → 1,429,919 bytes (67% smaller)**,
with 2,981,889 bytes of symbols kept beside it. The shipped binary has no
`.debug_info`; the debug file has it, and carries 28,229 DWARF name entries.

Crash reporting was then tested end to end on the **stripped** binary rather
than assumed: it launches and authenticates, `crashtest` produces an 8 MB
minidump and a report, and the report's module-relative `rva=0x3edd8` — an
offset stripping does not move — resolves through the split debug file to
`test_poll` at `client/gui/win32/winmain.c:15624`, which is the crashtest
handler itself. A stripped binary whose crashes still symbolicate is the whole
point of doing it this way.

## 6. Reason code 3014 is assigned to two different errors

`OC_ERR_INVALID_MESSAGE` and `OC_ERR_INVALID_DEVICE_TOKEN` are both `3014`
(`shared/protocol.h:311`, `:321`). 3020 is unused.

*Impact:* none today, tested rather than assumed: the two travel on different
frames, no shipped client sends `REGISTER_DEVICE_TOKEN`, and the Win32 "Send
later" path refuses an empty composer before it can produce the other. It becomes
a real ambiguity the moment a client renders either code. **Verified** —
adversarial refutation attempt, 2026-08-02, could not construct a reachable
collision.

**FIXED 2026-08-02.** `INVALID_MESSAGE` moves to **3020**, the next free value.
It was the newer of the two — added 2026-08-01 with scheduled messages, reusing
a number `INVALID_DEVICE_TOKEN` had held since 2026-07-24 — so the newcomer
yields. Both were also sitting out of numeric order in the enum, which is the
tell that they were inserted rather than placed; the block is ordered again.

**A test now makes the defect impossible to reintroduce**, because the reason it
happened is that nothing was checking. `test_reason_codes_unique` compares every
pair of the 30 reason codes and names both sides of any collision. It is listed
explicitly rather than derived — C cannot enumerate an enum, and a test that
walked a numeric range would pass for a code nobody remembered to add — and a
check verifies the list covers every enum member, so an omission is visible.

**The check was proved to fail before it was trusted:** putting
`INVALID_MESSAGE` back to 3014 turns the suite red with
`FAIL reason code 3014 is both INVALID_DEVICE_TOKEN and INVALID_MESSAGE`, and
restoring 3020 turns it green.

No protocol version bump: the `ERROR` frame's layout is unchanged, and §8's
design already has clients categorising an unrecognised code by its 3000-range.

## 7. Two message-type opcodes are used twice each

`PROFILE_INFO` and `TYPING` are both `0x0072`; `LIST_FILE_CHANNELS` and
`TYPING_UPDATE` are both `0x0073`.

*Impact:* none today. Each pair is direction-disjoint — one is client→server and
the other server→client — and each side's dispatch chain contains exactly one
branch for the value, so neither can be handed the other's frame. It becomes a
defect the moment anyone adds an inbound branch for the opposite direction.
**Verified** — both dispatch chains read in full and the typing round trip is
asserted by `tests/itest_netloop.c:942`. The pair that is *not* direction-disjoint —
`SET_PROFILE` and `SET_PRESENCE` — is a live defect; see "Editing a profile
drops the connection and saves nothing".

**FIXED 2026-08-02.** `TYPING` and `TYPING_UPDATE` move to `0x007E`/`0x007F`,
which clears both collisions by relocating two values rather than four: they are
the only contiguous pair among the three frames involved, so moving them leaves
`0x0072` to `PROFILE_INFO` and `0x0073` to `LIST_FILE_CHANNELS` without splitting
any other pair. `OC_PROTOCOL_VERSION` goes to **8** — no payload layout changed,
which is the one case the "bump on layout change" rule does not name, but a v7
peer's `TYPING` *is* a v8 peer's `PROFILE_INFO`, so it is exactly the silent
mis-decode the version exists to convert into a clean `REJECT`.

Measured: the full suite passes on the new values, including the typing round
trip (`itest_netloop.c:942`) that drives `TYPING` → `TYPING_UPDATE` end to end.
`test_version_reject` gains the other half of the bump's guarantee — a peer
speaking version 7 is refused with `VERSION_TOO_OLD` — proven to fail by widening
the daemon's accepted range to `OC_PROTOCOL_VERSION - 1`, which produced 3 failing
assertions.

The absence that let two collisions land is closed: `scripts/check_opcodes.sh`
parses every `OC_MSG_*` in `shared/protocol.h` and fails on any duplicate, run by
`make test` and by CI's build job. It reports 156 opcodes, all unique, with one
declared exception — `0x0070`, carried by both `SET_PROFILE` and `SET_PRESENCE`,
named in the script with its backlog item so that removing the line is what turns
enforcement on. Proven to fail: putting `TYPING` back on `0x0072` exits 1 naming
both types; pointed at a file with no opcodes it exits 2 rather than passing
vacuously.

## 8. Handshake frames are not stamped version 1

`PROTOCOL.md` and `shared/protocol.h:986` both state that `HELLO`/`WELCOME`/
`REJECT` are frozen at version 1 so negotiation can never hit a version
mismatch. The encoders stamp `OC_PROTOCOL_VERSION` (7) instead
(`shared/protocol.c:216/224/231`).

*Impact:* none between our own peers, because nothing validates the field (see
"The per-frame version is never validated after the handshake"). The property the
specification claims — that version negotiation is itself immune to a version
mismatch — does not hold: it rests on a constant the encoders never write. The
documentation now records the actual behaviour; the code is unchanged.
**Verified.**

**FIXED 2026-08-02.** The three encoders stamp `OC_HANDSHAKE_VERSION` (1), a
named constant rather than a literal so the freeze is stated where it is relied
on. The claimed property now holds in code, and the documentation goes back to
stating it rather than disclaiming it.

Measured: `test_handshake_frames` asserts `h.version == 1` on all three frames —
written as the **literal**, not as `OC_HANDSHAKE_VERSION`, because the property
is precisely that this value does not track `OC_PROTOCOL_VERSION`, and an
assertion phrased in terms of a constant follows the constant wherever it goes.
The old assertion was `h.version == OC_PROTOCOL_VERSION` under a comment reading
"HELLO is frozen at v1" — the test had been reconciled to the code rather than
to the specification, which is why the suite was green while the property was
false. Proven to fail: restamping `REJECT` with `OC_PROTOCOL_VERSION` produces
1 failing assertion.

`REJECT` is the frame that makes this matter — it is sent *to* a peer whose
version has just been refused, so it is the one that has to be decodable by a
peer that agrees about nothing else.

## 9. The per-frame version is never validated after the handshake

Neither side reads `hdr.version` on any post-handshake frame, and the negotiated
version is not stored on the connection at all.

*Impact:* none today — our only client stamps the current version on every
frame. It means the version field costs two bytes per frame and buys nothing,
and that a mismatched peer would misparse rather than be told. **Verified** —
full dispatch-path read; the `conn` struct has no version member.

## 10. `UNEXPECTED_MSG_TYPE` is never sent

`OC_ERR_UNEXPECTED_MSG_TYPE` (1005) appears in no `.c` file. A first frame that
is not `HELLO` closes the socket with nothing written; an unknown post-auth frame
type is ignored silently.

*Impact:* none for our own client, which always sends `HELLO` first and only
sends types the daemon dispatches. A third-party implementer gets a silent close
where the specification promises a reason. **Verified.**

## 11. Backfill replays public channels the caller has not joined

An explicit backfill cursor is gated on `channel_read_access` (public **or**
member), so a public channel a user never joined is replayed when they ask for
it. Only the cursorless case derives member channels.

*Impact:* none — this matches the read rule the requirements state (REQ-031: a
public channel is readable by anyone in the tenant), and the Win32 client reaches
it deliberately when you open a public channel from the palette or the directory.
This was a false statement in `PROTOCOL.md` §6.1, now corrected, rather than a
code defect. Kept in the list so the earlier reading is not rediscovered as a
security finding. **Verified** — no membership filter precedes the access check.

## 12. A stale timezone offset is never refreshed

`users.tz_offset_min` decides which local day a per-weekday quiet-hours schedule
applies to. The client writes it only as part of `SET_SCHEDULE`, so it is
refreshed when the user edits their schedule and at no other time; ARCH-103
describes a connect-time refresh that is not built.

*Impact:* none in any deployment today — the only reader is the push worker,
which requires an active enrollment and a push gateway. It becomes a real
mis-timing the moment push is enabled for a user who travels or crosses a
daylight-saving boundary. **Verified** — sole reader is `daemon/push.c:346/365`.

## 13. `UPLOAD_BEGIN` is not idempotent

The documented contract said it deduplicates on `(channel, token)` like `SEND`.
It does not: `process_attach_create` performs an unconditional `INSERT` and the
`attachments` table has no token column. The token is carried into the job and
never read.

*Impact:* none today, because no shipped client retries an `UPLOAD_BEGIN` — the
only emitter generates a fresh random token per attempt and there is no retry
loop. A client that did retry after a drop would create orphan pending rows,
which the maintenance sweep would later reclaim. The documentation has been
corrected. **Verified.**

## 14. The tray balloon's rendering has never been observed

The tray balloon's rendering has never been observed.

*Impact:* The one OS-level notification path could be silently broken and nothing would
catch it.

**Verified** — Shell_NotifyIconW with NIF_INFO is called and the shell accepts the icon
(winmain.c:694 g_tray_live), but §4's own note records that no capture on this
host shows the balloon; there is no Windows GUI CI (BACKLOG item 19)

## 15. A custom emoji keeps its shortcode's width

The Win32 client draws a custom emoji image over its hidden shortcode, so the run
keeps the text's width and a short emoji leaves uneven spacing around it. Closing
it needs an `IDWriteInlineObject` per emoji.

*Impact:* cosmetic, in one client. The image is drawn as a square at the line
height, centred in the run it replaces, so the leftover reads as symmetric
spacing rather than a missing character — the alternative, left-aligning it,
looked like a defect. **Verified** — `client/gui/win32/winmain.c:3217-3225`.

## 16. The emoji catalogue is small and has no skin tones

The emoji catalogue is 179 entries with no skin tones.

*Impact:* A reaction or emoji a user knows by name may simply not exist, and no one can
pick a skin tone. Custom workspace emoji (REQ-072) are built and partly
compensate.

**Verified** — client/core/complete.c EMOJI[] holds 179 rows (counted); `grep -i skin` returns
nothing anywhere in the client

## 17. Quiet hours are typed HH:MM fields rather than time pickers

Quiet hours are typed HH:MM fields, not time pickers.

*Impact:* Minor entry friction on a settings surface. Note the same card is unusable at
high DPI — BACKLOG item 26.

**Verified** — The per-weekday schedule is built (winmain.c:4160-4350, SET_SCHEDULE
protocol.h:249, migrations 0034/0035) but the base window is two validated text
fields, not a picker — §3 item 4's residue

## 18. There is no persistent keybinding hint bar

No persistent keybinding hint bar.

*Impact:* Low, and arguably right for a GUI. Recorded because the TUI has one and the
table marks Win32 ❌.

**Verified** — There is a Ctrl+/ shortcuts pane (winmain.c:4538 ACC_KEYS) but no always-
visible footer of contextual chords

## 19. No Ctrl+1..9 workspace switching

No Ctrl+1..9 workspace switching.

*Impact:* With N-concurrent workspaces built (WIN-29), the fastest way between them is
still the mouse.

**Verified** — The accelerator table (winmain.c:4536-4546) binds Ctrl+K/F//,/=/-/0 and nothing
else; Ctrl+0 is zoom reset. Switching is mouse-only via the rail switcher menu
(:14520-14531)

## 20. Mark-all-read has no keyboard shortcut

Mark-all-read has no keyboard shortcut.

*Impact:* Trivial.

**Verified** — Item 73 'Mark all as read' in the workspace menu (winmain.c:5908) and the
palette; not in the accelerator table (:4526-4546). Slack binds Shift+Esc

## 21. No hover strip of one-click reactions on a message row

No hover strip of one-click reactions on a message row.

*Impact:* The most frequent single action in a chat product costs an extra click.

**Verified** — Six configurable quick reactions live inside the kebab menu (winmain.c:372-398
QUICK_DEFAULT, :11805 mi_emojirow) — the table's '6 hardcoded' is stale — but a
message row has no hover affordance, so reacting is two clicks

## 22. Activity has filters but no saved views

Activity has facets but no saved views.

*Impact:* Low. A user re-picks chips each visit.

**Verified** — AF_ALL/MENTIONS/REACTIONS/THREADS/UNREADS/DMS/CHANNELS chips exist
(winmain.c:8053-8090) — the doc's 'no DMs facet' is stale — but no way to name
and keep a filter combination

## 23. No long-form channel description, only the one topic line

No long-form channel description — only the one topic line.

*Impact:* A channel cannot carry purpose, norms or links the way both references allow.

**Verified** — UPDATE_CHANNEL ops cover topic/rename/visibility/archive only
(client/core/client.h:136 + winmain.c:12985-13036); REQ-034 defines a single
topic string

## 24. Custom emoji have no administration surface

Custom emoji have add/delete but no administration surface.

*Impact:* An admin cannot see or curate what has been added — only replace it by name.

**Verified** — ADD_EMOJI/DELETE_EMOJI/LIST_EMOJI on the wire (protocol.h:154-157) and 'Add
custom emoji' in the menu (winmain.c:14312); no pane listing the catalogue, no
per-role permission on who may add

## 25. A #channel reference is text, not a link

A #channel reference completes as text and is not navigable.

*Impact:* Pointing someone at another channel gives them a string to type into the
switcher rather than a link.

**Verified** — OC_AC_CHANNEL completion exists in client/core/complete.c, but the transcript's
span pass (winmain.c:3013-3065) styles user mentions only — there is no
channel-mention span and no click target

## 26. There is no back/forward navigation history

No back/forward navigation history.

*Impact:* Following a permalink or a search hit is a one-way trip — there is no way back
to where you were.

**Verified** — `grep -iE 'back/forward|history nav' client/gui/win32/winmain.c` → 0; the
accelerator table (winmain.c:4526-4546) has no Alt+Left/Right or mouse-
button-4/5 handling

## 27. There is no always-present search field

No always-present top-bar search field.

*Impact:* Message search is a mode you enter rather than a field that is always there —
the single most-used affordance in both reference products.

**Verified** — Search is an overlay reached by Ctrl+F (winmain.c:4537 ACC_SEARCH) with its
query box inside the overlay (:3781); the sidebar's 'Find a conversation' box
(:2551) filters conversations, not messages

## 28. `openchime://` is not registered with the OS

Copying a permalink puts the text on the clipboard and pasting one into the app
jumps to it, but the scheme is not registered, so a link is not clickable outside
the application.

*Impact:* a permalink works between people who paste it into OpenChime and does
nothing from a browser or a mail client. Registering it is an install-time act,
deliberately not done as a side effect of running the client. **Verified** —
ARCH-96 records the omission.

## 29. The members pane cannot scroll

`draw_members` stops drawing when it runs past the pane's height and has no
scroll offset, so members below the fold are unreachable. The pane has no search
either. The server caps a channel roster at 500.

*Impact:* in a channel with more members than fit the window, the rest cannot be
seen, opened or messaged from the roster. They remain reachable through the
People directory and the command palette, which is what keeps this below the
items that make a surface unusable outright. **Verified** — `if (y > H) break;`
with no scroll state anywhere in the pane.

## 30. The audit log filters only what has already been paged in

`AUDIT_QUERY` carries a timestamp cursor and a limit and nothing else, so the
family chips in the Win32 Admin pane narrow the rows already fetched rather than
re-asking the server. Actor and action filters do not exist on either side.

*Impact:* an admin looking for an old entry of one family pages back through
every other family to reach it. Paging and scrolling themselves work. Admin-only
surface. **Verified** — `oc_audit_query` is `{before_ms, limit}`
(`shared/protocol.h:879`); the family filter is the client-side `g_audit_family`.

## 31. The Files view stops at 200 rows with no paging

The Files view stops at 200 with no paging.

*Impact:* In a workspace older than a few hundred files, the Files view cannot reach the
older ones; the type/scope/sort filters (FF_LABEL/FS_LABEL/FSORT_LABEL,
:5023-5027) all operate over that same 200.

**Verified** — winmain.c:5169 draws 'Showing the most recent 200. Older files are in search.'
— LIST_FILES has no keyset cursor, unlike search (WIN-38)

## 32. The who-reacted overlay cannot add or remove a reaction

The who-reacted overlay cannot add or remove a reaction.

*Impact:* Low.

**Verified** — OVL_REACT is populated by oc_client_list_reactions (client/core/client.h:162)
and rendered as a list; no oc_client_react call from that pane

## 33. The create-channel flow takes name and visibility only

The create-channel flow takes name + visibility only — no topic, no initial
members.

*Impact:* A new channel is created empty and untitled; the creator must remember two
further steps. This is §3 item 10, still open.

**Verified** — winmain.c:14540-14544: `oc_field f[2] = { { FF_TEXT, "Channel name" ... }, {
FF_CHOICE, "Visibility", "Public|Private" ... } };` then create_channel_ex.
Topic is set afterwards from the About tab (:4694-4702); members afterwards
from the channel menu (:14868 'Add someone')

## 34. Search operators are typed text with no filters panel

Search operators are typed text only; no filters panel.

*Impact:* A user must know and correctly type the grammar; discoverability is zero for
anyone who has not read the docs.

**Verified** — shared/searchq.h parses from:/in:/has:/before:/after: and winmain.c:3808 echoes
what it understood, but there is no date picker, channel picker or has-
file/link checkbox UI anywhere in draw_search

## 35. Search has no Messages/Files/Channels/People tabs

Search has no Messages/Files/Channels/People result tabs.

*Impact:* Finding a file or a person by name means leaving search for the Files view or
the People directory.

**Verified** — draw_search (winmain.c:3777-3875) renders one flat list of message rows; the
only tab strips are TAB_MESSAGES/FILES/PINS/ABOUT (channel, :1020) and DTAB_*
(drafts, :1023)

## 36. Search is newest-first only, with no sort choice

Search is newest-first only — no relevance or oldest sort, on either side.

*Impact:* A query with many hits cannot surface the best match — only the most recent one
— which is the common case for an old decision buried in a busy channel.

**Verified** — daemon/dbwriter.c:3394 `ORDER BY m.id DESC LIMIT ?3;` is the only ordering;
oc_search (shared/protocol.h:967) carries no sort field

## 37. Search has no `on:` single-day operator

Search lacks the `on:` single-day operator.

*Impact:* A user searching a single day must type before:/after: around it; everything
else in the operator set works.

**Verified** — shared/searchq.c implements from: (:51), in: (:58), has: (:66), before: (:84),
after: (:89) — there is no `on:` branch, though REQ-081 names it explicitly.

## 38. The right-hand surfaces are read-only overlays

The right-hand surfaces are overlays and largely read-only.

*Impact:* Some panes are places you look rather than places you act — notably who-
reacted, where you cannot add or remove a reaction.

**Verified** — OVL_AUDIT/WEB/REACT/NOTIFY/KEYS/LATER/FILES/BROWSE/INVITES/SESSIONS
(winmain.c:3714) plus the context pane (ARCH-94); the reactions overlay is
view-only and the audit/storage panes have no per-row action

## 39. There is no unreads-only sidebar mode

No unreads-only sidebar mode.

*Impact:* You cannot collapse the sidebar to just what wants attention, though the
keyboard move-to-next-unread does exist (ACC_NAV_NEXT_UNREAD, :4569).

**Verified** — g_sb carries per-section sort/filter/collapse (winmain.c:504) with no unread-
only mode; the unread-only toggles that exist are the Threads pane's (:8837)
and Activity's Unreads facet (:8064)

## 40. File upload shows no progress and no pre-send preview

File upload shows no progress and no pre-send preview.

*Impact:* A large upload over a slow link looks like nothing is happening, and there is
no chance to cancel or check the wrong file was chosen.

**Verified** — `grep -i progress client/gui/win32/winmain.c` matches only 'edit-in-progress'
(:1968) and 'IME composition in progress' (:9453); no byte counter or thumbnail
between picking a file and its arrival in the transcript

## 41. No inline document preview and no media carousel

No inline PDF/document preview and no media carousel.

*Impact:* Every non-image file must be downloaded and opened elsewhere; a channel of
shared images has no way to page through them.

**Verified** — Non-image attachments get a coloured type badge only (winmain.c:4913 '.pdf' →
PDF tag); mime_is_image (:774) limits inline render to png/jpeg/gif/bmp/webp;
`grep -i 'carousel|gallery'` returns nothing

## 42. A DM cannot be closed, extended or converted to a channel

A DM cannot be closed from the sidebar, cannot gain people, and cannot become a
channel.

*Impact:* Every DM you ever open stays in the sidebar forever, and a 1:1 that grows into
a project has to be restarted as a new conversation with no history.

**Verified** — show_channel_menu (winmain.c:14840-14876): for a DM the menu is Star / Mute /
Mark as read / three notify levels and stops — the Add someone / Remove someone
/ Leave block is gated `if (c->kind != OC_CHANNEL_KIND_DM)`. No convert-to-
channel op on the wire

## 43. There is no channel bookmark bar

No channel bookmark bar.

*Impact:* A channel cannot surface its standing links (runbook, dashboard) — they get
pinned as messages and scroll away.

**Verified** — The only 'bookmark' hits in winmain.c are the OC_ICON_BOOKMARK glyph used by
Saved/Later (:2143, :3576, :9166); no per-channel pinned-link store in the
schema

## 44. There is no sortable admin members table

No sortable/bulk admin members table.

*Impact:* Onboarding or offboarding several people at once is one dialog per person.

**Verified** — The People directory (REQ-289, WIN-109, winmain.c:8736 draw_directory) is a
searchable list with per-row actions; no column sort, no multi-select, no bulk
role change or bulk removal

## 45. There is no analytics beyond the storage report

No analytics beyond the storage report.

*Impact:* An operator cannot answer 'is this being used, and by whom'.

**Verified** — STORAGE_STATUS (protocol.h:204) is the only metrics op; no
message/activity/adoption counters exist in the schema

## 46. A removed user can never be reactivated

A removed user can never be reactivated.

*Impact:* An accidental removal, or a returning employee, requires direct SQLite surgery
on the operator's database.

**Verified** — REMOVE_USER sets `UPDATE users SET disabled=1` (daemon/dbwriter.c:1119); `grep
-E 'disabled=0|disabled = 0' daemon/*.c | grep UPDATE` returns nothing — no
code path clears the flag. Win32 member menu (winmain.c:11963-11984) offers
View profile / Message / Role / Remove from workspace and nothing else

## 47. There is no out-of-office status distinct from a custom status

No out-of-office status distinct from a custom status.

*Impact:* An absence reads as an ordinary status line; nothing tells a sender their
message will wait.

**Verified** — `grep -i 'out.of.office|OOO'` in winmain.c and shared/protocol.h → nothing;
SET_STATUS (protocol.h:713) is emoji + text + expiry, with no OOO semantics.
REQ-122 names out-of-office alongside DND and custom status

## 48. There is no block or DM-privacy control

No block / DM-privacy control.

*Impact:* A user being harassed can silence the notification but cannot stop the messages
arriving.

**Verified** — SET_MUTE (protocol.h:165) mutes a conversation; `grep -i block
client/gui/win32/winmain.c` matches only unrelated words. Nothing prevents a
user from opening a DM to anyone

## 49. There is no catch-up or digest surface

No catch-up / digest surface.

*Impact:* After time away, a user works down the sidebar by hand. Partly served by
Activity's Unreads facet.

**Verified** — Rail destinations are
HOME/DMS/ACTIVITY/LATER/DRAFTS/NEWMSG/ADMIN/THREADS/DIRECTORY (winmain.c:245);
nothing walks unreads conversation-by-conversation

## 50. A user cannot edit their own email address

Email address cannot be edited by its owner.

*Impact:* A user whose email changes cannot correct it; only an operator with database
access can.

**Verified** — `oc_set_profile` is `{ oc_slice title; oc_slice timezone; }`
(shared/protocol.h:714) — email travels on PROFILE_INFO/USER_LIST for display
(:730, :928) but no op writes it

## 51. Pronouns and custom profile fields are not built

Pronouns is the one profile field not built.

*Impact:* A profile field users expect is absent.

**Verified** — grep -rn 'pronoun' daemon/ client/ shared/ -> 0 hits; migration 0027
(daemon/migrate.c:533) carries title/timezone/status, and avatars are an
attachment id

## 52. New members auto-join no default channels

No default channels for new members.

*Impact:* Every new joiner lands in an empty sidebar and must be told what to join.
Adjacent to REQ-042 workspace settings, which is already in BACKLOG.

**Verified** — `grep -i default_channel` across the tree returns nothing; invite redemption
joins nothing automatically

## 53. There are no per-channel posting or management permissions

No per-channel posting or management permissions.

*Impact:* An announcements channel cannot be made post-restricted — anyone in it can
post.

**Verified** — `grep -iE 'posting permission|who_can_post' client/gui/win32/winmain.c` → 0;
daemon/roles.c is tenant-role only; UPDATE_CHANNEL has no permission op

## 54. Send-later cannot be used on a thread reply

Send-later cannot be used on a thread reply, though the wire supports it.

*Impact:* A built daemon capability is unreachable: you cannot schedule a reply into a
thread. Also the send-later menu (winmain.c:10604-10612) offers three fixed
presets with no custom date/time.

**Verified** — `oc_client_schedule(oc_client*, uint64_t channel_id, uint64_t thread_root,
...)` (client/core/client.h:203) takes a thread root; the only caller passes 0
— winmain.c:10954 `oc_client_schedule(g_client, g_sel, 0, at, b);`

## 55. Typing indicators do not work inside a thread

Typing indicators do not work inside a thread.

*Impact:* Two people replying in the same thread cannot see each other composing — they
collide.

**Verified** — OC_MSG_TYPING is channel-scoped (shared/protocol.h:181, 'I am typing in a
channel'); no thread root on the frame

## 56. Typing is channel-scoped and cannot be scoped to a thread

Typing is channel-scoped only; it cannot be scoped to a thread.

*Impact:* Typing in a thread shows as typing in the channel; the 6-second client-side
expiry half is built correctly.

**Verified** — oc_typing / oc_typing_update carry channel_id (and user_id) only —
shared/protocol.h:753-754. The requirement says "scoped to the active channel
or thread".

## 57. A thread reply cannot also be posted to the channel

No 'also send to channel' on a thread reply.

*Impact:* A reply that the whole channel should see has to be typed twice or forwarded.

**Verified** — `grep -iE 'also.send|also_send' client/gui/win32/winmain.c shared/protocol.h`
returns nothing; SEND_REPLY (protocol.h:97) has no broadcast flag

## 58. A thread reply cannot also go to the main scroll

A thread reply cannot also be posted to the channel's main scroll.

*Impact:* The "also send to #channel" checkbox every reference client offers is absent,
so a reply meant for the whole channel has to be pasted twice.

**Verified** — oc_send_reply has no such flag (shared/protocol.h:640-641: channel_id, idem,
parent_id, body, attachments). Follow/unfollow + the aggregated view ARE built
(thread_follows/thread_reads, daemon/migrate.c:751,759; LIST_THREADS 0x00D1 /
SET_THREAD_FOLLOW 0x00D4).

## 59. A parent message shows a reply count but not the recent repliers

The parent message shows a reply count but never the most recent repliers.

*Impact:* The transcript cannot show who is in a thread without opening it, so a reader
cannot tell a conversation they are in from one they are not.

**Verified** — oc_thread_meta carries {message_id, reply_count, last_reply_at} only
(shared/protocol.h:649); Win32 draws "↳ N replies"
(client/gui/win32/winmain.c:3331-3334) with no repliers.

## 60. Mark-all-read loops per channel with no bulk server operation

Mark-all-read has no bulk server op; the client loops CLIENT_ACK per channel.

*Impact:* Catch-up on a workspace with many channels costs one round trip per channel.

**Verified** — client/gui/win32/winmain.c:14728 loops oc_client_mark_read over every channel;
no bulk opcode in shared/protocol.h

## 61. The optional `.well-known` half of workspace resolution is unused

The optional .well-known half of workspace resolution is never consulted.

*Impact:* A deployment that can publish an HTTPS well-known document but not an SRV
record cannot be discovered.

**Verified** — client/core/resolve.c:83-108 does SRV _openchime._tcp.<domain> then an A-record
fallback at 443; grep 'well.known' client/core/resolve.c -> 0 hits

## 62. Switching workspaces does not flush the composer safely

Switching workspaces neither flushes nor clears the composer, and can write one
workspace's text as a draft in another.

*Impact:* A partly-typed message does not survive a switch as the requirement demands,
and worse, it can be silently saved as a draft in an unrelated workspace's
channel (channel ids collide across workspaces).

**Verified** — client/gui/win32/winmain.c:14132-14155 switch_workspace calls
ws_save_active()/ws_load() but never draft_flush()/draft_restore(); ws_load
(13624-13640) restores sel/scroll and clears selection/edit/toast but not the
edit control. The debounce at winmain.c:16266 then calls draft_flush(g_sel)
with the NEW workspace's channel id while the box still holds the OLD
workspace's text.

## 63. Context-menu actions are reachable only by right-click

Context-menu actions are reachable only by right-click — no keyboard route.

*Impact:* Pin, save, forward, copy-link, edit, delete, mark-unread and react are
unreachable without a pointer, so the requirement's first clause ("every action
reachable by pointer has had a keyboard route") fails for the richest action
set in the app.

**Verified** — The UIA provider is built (client/gui/win32/a11y.c, 1105 lines, ARCH-99). But
show_msg_menu() is called only from mouse handlers
(client/gui/win32/winmain.c:13435, 13442) and there is no VK_APPS or Shift+F10
handler anywhere; SHORTCUTS[] (winmain.c:4549) documents "Right-click — Actions
for a message, member or channel" as the only route.

## 64. A muted conversation still counts toward the rail badge

A muted conversation still counts toward the cross-workspace rail badge.

*Impact:* Muting a noisy channel in a background workspace still lights the rail badge,
which is the one badge the requirement's "excluded from the unread badge"
clause most obviously covers.

**Verified** — The sidebar dims and de-counts muted rows
(client/gui/win32/winmain.c:2726-2727) and push honours it (daemon/push.c:375),
but ws_unread_elsewhere (winmain.c:13540-13548) sums m->channels[c].unread with
no mute check.

## 65. Keywords and priority people are ignored by the desktop toast

Keywords and priority people are honoured by push but ignored by the Win32
desktop-toast gate.

*Impact:* On a channel set to `mentions`, a keyword hit or a message from a priority
person rings the phone but raises no desktop toast — exactly the client/server
drift ARCH-89/ARCH-103 exist to prevent.

**Verified** — Daemon side built (migration 0034 notify_keywords/priority_people at
daemon/migrate.c:724,731; daemon/dbwriter.c:1352 store_keyword_hits;
daemon/push.c:354,380 priority pierce). Win32 sets both lists
(oc_client_set_keywords/set_priority) and highlights keywords (winmain.c:3056),
but the toast gate at winmain.c:16463-16470 tests only oc_mention_targets().

## 66. A forwarded message loses its attachments and its reference

Forward is a client-side quoted copy; attachments do not travel and the
reference is not structured.

*Impact:* A forwarded message loses its files, and the attribution is prose a client can
only recognise by string-matching — the ARCH marker (quote reference vs.
embedded copy) was resolved by accident, not by decision.

**Verified** — client/gui/win32/winmain.c:12168 builds a plain body "Forwarded from X:\n>
excerpt\nopenchime://…" and sends it through the ordinary send path; no wire
field, no daemon handling, `n_attach` is not carried across.

## 67. Read receipts have no privacy control and no not-retroactive rule

Read receipts have no privacy control and no not-retroactive rule.

*Impact:* A user or operator who does not want read state exposed cannot turn it off, and
receipts apply to all pre-existing history.

**Verified** — Seen-by is built: READ_CURSOR 0x0033 fan-out (daemon/netloop.c:3234-3253),
oc_model_seen_by (client/core/model.c:304), rendered at
client/gui/win32/winmain.c:5676-5680. `grep -rn receipt daemon client` finds
nothing — no deployment switch, no per-user opt-out.

## 68. Only the transient pause is visible to other people

Only the transient pause is visible to other people; the recurring schedule and
OOO are not.

*Impact:* A colleague inside their configured quiet hours shows as ordinarily available
to everyone else, so the badge is only correct for one of the two DND
mechanisms.

**Verified** — PRESENCE_UPDATE carries a dnd bit (shared/protocol.h:752) and Win32 renders it
(winmain.c:2759, 6127, 8780 via oc_model_dnd_of). But daemon/netloop.c:374-381
dnd_of() reads only conn->dnd_until_ms — the REQ-278 pause.
dnd_mode/allow_start_min are never consulted, and no out-of-office state
exists.

## 69. The audit log records no successful sign-in

No access log: successful sign-ins, invite redemption, owner bootstrap and
channel-member removal are never audited.

*Impact:* An operator can see failed logins but cannot answer 'who signed in, when' or
'who joined this workspace' — the audit log claims a catalog it does not
implement.

**Verified** — Only audit call in the auth path is daemon/dbwriter.c:668 `audit_log(db,
OC_AUDIT_SECURITY, "auth.failed", ...)`. `grep OC_AUDIT_ daemon/*.c` shows no
entry for a successful auth, none in process_redeem_invite, none at the REQ-024
owner bootstrap (dbwriter.c:912), and none on channel kick — all four are named
in REQ-251's catalog (docs/REQUIREMENTS.md:1639-1648)

## 70. The low-hundreds concurrency target has never been measured

The low-hundreds concurrency claim has never been measured.

*Impact:* The lean-profile concurrency figure is an assertion, not a measurement; a
regression would be invisible.

**Verified** — daemon/netloop.c:33 sets OC_NETLOOP_MAX_FD 4096 and the loop is epoll-based, so
the capacity plausibly exists; there is no soak or scale test in tests/
(docs/BACKLOG.md #21 records the same absence).

## 71. There is no data import and no workspace export

No data import and no workspace export.

*Impact:* No migration path in (a Slack/Pumble team cannot bring history) and no way out
(an operator cannot take a portable copy) — the doc calls migration 'a real
Pumble go-to-market lever'.

**Verified** — `grep -ic 'import|export' client/gui/win32/winmain.c` → 0 each; no
import/export job in daemon/dbwriter.h's OC_JOB_* list. Not covered by REQ-252
(legal hold), whose export half docs/REQUIREMENTS.md:1665 explicitly narrowed
out

## 72. There is no migration path from another product

No data import / migration path.

*Impact:* A team leaving Slack starts empty; distinct from compliance export (REQ-252).

**Verified** — grep -rniE 'importer|REQ-254' daemon/ client/ -> 0 hits

## 73. The chrome-fit check cannot see clipped text

`chromefit` compares published element rectangles. It has no view of glyph
extents, so text cut mid-character inside a correctly-sized box reads as clean.

*Impact:* the check reported `overlaps=0 outside=0` for the Notifications card at
240 DPI while that card's section titles were visibly cut through the middle
("The Notifications card clips its own content at high DPI"). An invariant that
cannot see the defect class it is trusted for will
keep certifying it. **Verified** — screenshot compared against the same run's
`chromefit` line, 2026-08-02.

## 74. The chrome-fit overlap test has no z-layer exemption

The "outside" half of the check exempts elements that legitimately sit behind a
modal; the overlap half compares every published pair with only a containment
exemption, so a modal's own button overlapping a sidebar row *behind* it is
reported as a collision.

*Impact:* a false failure. The smoke's "the modal spills" failure on 2026-08-02
is this: the Done button is inside its card by construction, and the card is
painted opaquely over a dimmed scrim before the button is drawn. A check that
cries wolf gets discounted, which is how a real failure beside it gets missed.
**Verified** — the reported geometry is real; the interpretation is not.

## 75. Echo cancellation has no test harness

AUDIO.md §6.4 designs an ERLE measurement with a synthetic impulse response, and
the testing documentation described it as if it ran. `tests/test_audio.c` covers
the relay sidecar only.

*Impact:* none today, because the client-side audio path it would measure does
not exist ("The audio client does not exist"). The documentation has been
corrected. **Verified** — no ERLE
or AEC symbol anywhere in the tree.

## 76. There is no large-scale soak test

Capacity is measured at points in time (`Scripts/bench.sh`): ~5 MB baseline,
~50 KB per idle connection, p50 2–3 ms round-trip, ~2 logins/s.

*Impact:* nothing measures drift over hours — leaks, fragmentation, fd
exhaustion. The figures that exist are honest and were corrected once already
when the harness proved to be the limit. **Verified** — TESTING.md §5.

## 77. The GUI smoke suite does not run in CI

The daemon is epoll-based and Linux-only; GitHub's Windows runners cannot host
it. The job exists and skips.

*Impact:* every Win32 chrome guarantee rests on a human running the suite before
pushing. `REQ-290`'s automation ids (built, WIN-110) are what a UIA-driven suite
on a self-hosted Windows runner would need; that runner does not exist.
**Verified** — the suite ran locally on 2026-08-02 and reported 249 checks.

## 78. Most Win32 features have never been driven and observed

The feature inventory extracted from the code — every rail and shelf destination,
composer control, message/channel/member/profile/workspace menu item, keyboard
chord, and connection path — is largely unexercised: a feature counted as tested
only when it has been driven the way a person drives it and the result read back
from the model or the dump.

*Impact:* this is the gap that produced items 24 and 25. The suite grew alongside
the code and inherited its blind spots, so a clean run is evidence about the
checks that exist, not about the client. **Verified** — the inventory and its
method were recorded in `FEATURE_AUDIT.md`, now folded here; the rows were never
filled in.

## 79. A click on empty modal background reaches the shell behind it

`modal_frame_click` returns 0 for a click inside the card that matches no footer
button, and the notification card's hit tests fall through when nothing matches,
so a click on empty card background that happens to lie over the sidebar column
reaches the sidebar's shelf hit test and navigates the application behind the
open modal.

*Impact:* the shell changes underneath a modal the user is still in. Reachable
whenever a card overlaps the sidebar column, which at 240 DPI is the default
window. **Verified** — found while refuting "The chrome-fit overlap test has no
z-layer exemption"; distinct from it.

## 80. The Drafts pane's "Sent" tab is laid out off-window at large scale

The tab strip measures each label from the scaled font and advances by it, with
no wrap, no clamp to the pane's right edge and no scroller. Past roughly 591 DIP
of accumulated tab width the third tab's rectangle lies beyond the window, where
it is neither drawn nor clickable. The strip's divider still uses a raw
unscaled constant while the tab boxes scale.

*Impact:* the Sent list — and the `from:me` search behind it — becomes
unreachable at large text size with zoom, in a window narrower than the tabs
need. **Verified** — the smoke's chrome-fit matrix failed on exactly this
(`outside=1 out="drafts.tab.sent"`) on 2026-08-02.

## 81. The Notifications card clips its own content at high DPI, and the hidden rows stay live

The card's prologue advances by raw unscaled constants while the fonts and the
card frame scale, and the only scroller covers the per-channel list at the
bottom rather than the body. At 192–240 DPI in a default-sized window the section
titles are cut mid-glyph and everything from the allow-hours chips downward — the
mode selector, the per-weekday schedule editor, the per-conversation levels — is
clipped away entirely.

Worse than invisible: rows between the visible body bottom and the card bottom
remain hit-testable, so a click that lands on nothing visible toggles a weekday;
rows past the card bottom are treated as scrim and dismiss the modal. At that
scale the body's top is computed past its own bottom and the inverted rectangle
is handed to the clip call without a clamp.

*Impact:* the notification settings are unusable and silently mis-settable at
high DPI. This is the functional half that `WIN-111` explicitly deferred as a
behaviour change; the vertical clipping of the titles it recorded as fixed is not
fixed. **Verified** — screenshot plus the dump's own geometry, 2026-08-02.

## 82. A thread reply notifies nobody

Thread replies never produce a notification decision.

*Impact:* A reply in a thread you are in never rings a phone or raises a toast; only
channel-level sends do.

**Verified** — daemon/netloop.c:2488-2516 (OC_RES_REPLY_OK fans out THREAD_REPLY but never
calls oc_push_notify) vs daemon/netloop.c:2037 (OC_RES_SENT does);
daemon/dbwriter.c:3212 comment names it a gap

## 83. Notifications have no sound, no badge and no preview toggle

Notification rendering: no sounds, no unread/taskbar badge, no window flash, no
content-preview toggle.

*Impact:* A notification is a silent balloon only — no audible cue, no at-a-glance count
on the taskbar, and no way to keep message text off the screen.

**Verified** — client/gui/win32/winmain.c:694,706 Shell_NotifyIconW + NIF_INFO balloons exist;
grep -niE 'PlaySound|ITaskbarList|OverlayIcon|FlashWindow' winmain.c -> 0
notification hits; grep 'preview.*toast|show_preview' -> 0 hits

## 84. Closing the window quits the app and ends all notification

WM_CLOSE quits the app; no close-to-tray and no taskbar flash, with no stated
contract.

*Impact:* Closing the window ends all notifications; the tray icon exists but the app
does not keep running behind it. Default (keep-running-in-tray) is still a
product call.

**Verified** — client/gui/win32/winmain.c:17156-17188 WM_CLOSE falls through to DefWindowProcW
after the unsent-outbox prompt; tray_done() in WM_DESTROY:17193

## 85. Nothing states what an unread badge counts

Nothing states what an unread badge counts.

*Impact:* Two clients can disagree about the same count and both look right. Default
should be the quieter reading (badge = what was worth notifying).

**Verified** — daemon supplies channel-level `unread` (shared/protocol.h:684-691) while the
client keeps its own high-water (client/core/model.h:98-101); no requirement
reconciles them

## 86. A user cannot follow every thread in a channel

Cannot follow every thread in a channel.

*Impact:* You can only follow a thread you have already seen, which is the thing being
missed.

**Verified** — daemon/migrate.c:225-230 + :521 notification_prefs has only level and muted; no
per-channel thread-follow column and no op in shared/protocol.h

## 87. There are no reminders and no saved-item due dates

Reminders and saved-item due dates.

*Impact:* The only true capability gap in notifications. Open: sweep granularity — the
ARCH-78 pass defaults to 5 min, a different promise from firing on the minute.

**Verified** — grep -rniE 'remind_at|reminder' daemon/ shared/ client/core/ -> only an emoji
keyword; saved_items (migration 0025, daemon/migrate.c:494) has no remind_at_ms

## 88. A call starting raises no notification

A call starting raises no notification.

*Impact:* A missed message is read later; a missed call is missed. Needs no new call
machinery — CALL_JOIN on an empty roster is already the 'started' event. Ships
with the audio client.

**Verified** — daemon/netloop.c:1588-1596 CALL_JOIN submits an access-gate job only; no
oc_push_notify or fan-out on an empty roster becoming non-empty

## 89. The notify decision has no single evaluator and no precedence order

No single notify evaluator: the precedence rule is split between push SQL and
each client.

*Impact:* Highest-value open item: nine inputs feed one boolean in two places, which is
exactly how REQ-134 shipped two silent push defects. Precedence order itself is
still undecided, including whether priority people pierce a pause.

**Verified** — shared/notify.c is 26 lines and covers quiet-hours only
(shared/notify.h:20-32); the level/mute/default/mention/keyword/VIP/pause logic
lives in daemon/push.c:334-425 SQL and separately in
client/gui/win32/winmain.c:16462-16463

## 90. There are no workspace-default quiet hours

Workspace default DND hours — blocked on there being no tenant-settings
surface.

*Impact:* An organisation cannot set sane default quiet hours; every member must
configure their own.

**Verified** — daemon/migrate.c:672-735 (0034) stores dnd_mode/allow_*_min per user only; no
tenant-level default column or op

## 91. Invites are single-use only, with no shareable link

Invite management (pending list, revoke, expiry) IS built; only multi-use
shareable links are missing.

*Impact:* Onboarding still needs one token per person; STATUS's ⛔ and BACKLOG's 'not
started' both overstate the gap.

**Verified** — shared/protocol.h:279-282 LIST_INVITES/INVITE_LIST/REVOKE_INVITE/INVITE_REVOKED
(0x004B-0x004E); daemon/dbwriter.c:4894,4933; daemon/migrate.c:85-91 invites
has expires_at_ms + consumed_at_ms (single-use); client/core/client.c:681,688

## 92. There are no guest accounts

Guest accounts — no channel-scoped role.

*Impact:* An external collaborator must be a full member with tenant-wide visibility, or
be excluded.

**Verified** — daemon/roles.c/roles.h and daemon/migrate.c CHECK (role IN
('owner','admin','member')) — grep 'guest' -> 0 hits

## 93. The admin console has no member table and no bulk actions

Admin console exists as a rail view but has no member table, no bulk actions
and no analytics.

*Impact:* An owner administering more than a handful of people works one right-click at a
time and has no usage picture.

**Verified** — client/gui/win32/winmain.c:7697-7699 Admin tabs are Storage / Audit log /
Invites only; role change and remove are per-member context actions; no
aggregate/analytics op in shared/protocol.h

## 94. There is no workspace-settings surface at all

No workspace-settings surface at all: name is env-only, no icon, default
channels or join policy.

*Impact:* An owner cannot rename or brand the workspace without shell access to the
daemon's environment; blocks REQ-279.

**Verified** — daemon/config.c:82 workspace_name from OPENCHIME_WORKSPACE_NAME;
shared/protocol.h:566 oc_workspace_info is read-only (deployment_mode,
max_users, workspace_name); no SET_WORKSPACE_* opcode exists

## 95. A URL cannot be authored as a link and is not clickable

No hyperlink construct: cannot author a link, and a URL in a message is not
clickable.

*Impact:* A pasted URL is inert text — a reader must retype it into a browser. §2.1
'Hyperlink insertion dialog ❌' understates it: reading a link is broken too,
not just writing one.

**Verified** — client/core/richtext.h:30-37 lists
BOLD/ITALIC/STRIKE/CODE/CODEBLOCK/QUOTE/BULLET/ORDERED — no link style;
winmain.c:1184 FMT_* toolbar has no link button; `grep -E
'ShellExecute|CreateProcess|WinExec' client/gui/win32/winmain.c` returns
nothing, so no URL can be opened from the client at all

## 96. There are no link unfurls

Link unfurls.

*Impact:* A pasted URL is bare text with no title, description or image.

**Verified** — grep -rni 'unfurl' daemon/ client/ shared/ -> 0 hits

## 97. There are no native polls

Native polls.

*Impact:* No structured way to take a decision in a channel.

**Verified** — no poll/vote message type in shared/protocol.h; grep for poll+vote -> 0 hits

## 98. There is no first-class snippet object

Snippets: fenced code blocks render, but there is no first-class snippet
object.

*Impact:* A long paste is a fenced run inside a message — not a titled, named body you
can share and open on its own.

**Verified** — client/core/richtext.h:34 OC_RT_CODEBLOCK and client/core/richtext.c:4 handle
fenced blocks; grep 'snippet' finds only FTS search snippets
(client/core/model.h:137)

## 99. Local auth has no second factor

No MFA/2FA in local auth mode.

*Impact:* A stolen password is a full account takeover in every self-hosted stand-alone
deployment.

**Verified** — grep -rniE 'totp|mfa|2fa|two-factor' daemon/ shared/ client/core/ -> 0 hits;
auth is PBKDF2 password only (daemon/auth.c)

## 100. There is no IP allowlist

No IP allowlist / access restriction.

*Impact:* Cannot restrict a workspace to corporate egress ranges.

**Verified** — grep -rn 'allowlist|allow_list' daemon/ -> 0 hits; only
OPENCHIME_MAX_CONNS_PER_IP throttling in the accept loop

## 101. There is no message retention policy

Opt-in message retention policy — nothing ages out messages.

*Impact:* Cannot satisfy a customer with a delete-after-N-days obligation. Still marked
[needs ARCH decision].

**Verified** — daemon/storage.c:23 only OPENCHIME_ATTACH_MAX_AGE_DAYS (REQ-217); no message-
pruning statement anywhere in daemon/dbwriter.c

## 102. There is no legal hold

Legal hold — needs ARCH decision, narrowed to REQ-276/277.

*Impact:* No way to freeze a custodian's content. Export split to REQ-276, DLP to
REQ-277.

**Verified** — grep -rniE 'legal_hold|LEGAL' daemon/ -> 0 hits

## 103. There is no compliance capture

Compliance capture (vendor push + pull API) scoped but unbuilt.

*Impact:* Blocks regulated buyers. Held-open gaps: the extract schema, the credential
model, and the user→email mapping. Two mechanisms decided: Global Relay EML
over SMTP journaling (file-drop first) and our own documented pull API.

**Verified** — no SMTP/journaling/export code in daemon/; drafts are declared in scope for it
(docs/REQUIREMENTS.md:1745)

## 104. There is no send-time DLP

Send-time DLP pre-post webhook scoped but unbuilt.

*Impact:* Nothing can redact before storage. Open: fail-open vs fail-closed, the
contract, and request signing. Reference SSN redactor was to live in the test
suite.

**Verified** — no pre-post hook in daemon/dbwriter.c process_send; grep for DLP -> 0 hits

## 105. There is no app or bot platform

No app/bot identity model and no slash-command dispatch.

*Impact:* Every integration story that depends on the app platform (REQ-174/175/176 and
the third-party route around REQ-270) has no foundation.

**Verified** — No bot identity column or table in daemon/migrate.c; no install/registration
opcodes in shared/protocol.h; the Win32 composer has no slash dispatcher.

## 106. There are no outgoing webhooks

No outgoing webhooks / event subscriptions.

*Impact:* External systems cannot react to activity; the incoming-webhook half has no
complement.

**Verified** — daemon/http.c is a 116-line inbound handler only; no subscription table in
daemon/migrate.c; no outbound delivery/retry/signing code beyond enroll.c and
push.c, which are both fixed-endpoint.

## 107. There is no workflow automation

No workflow automation.

*Impact:* Marked as likely reducing to a consumer of REQ-172, which is itself unbuilt.

**Verified** — No trigger/action model anywhere in daemon/ or shared/protocol.h.

## 108. There is no app-directory client surface

No app directory (federated function).

*Impact:* Depends on REQ-172; also a control-plane concern living in openchime-saas, not
this repo.

**Verified** — No directory client in daemon/ (only enroll.c and push.c call out); nothing in
client/gui/win32.

## 109. There is no third-party API or SDK

No third-party API or SDK.

*Impact:* Nothing can be automated against the workspace except one-way inbound webhooks.

**Verified** — the only wire is the custom binary protocol (shared/protocol.h); the sole HTTP
surface is POST /webhook/<token> (daemon/netloop.c:1813)

## 110. There is no email-to-channel ingestion

No email-to-channel ingestion.

*Impact:* Cannot pipe alerting or ticketing mail into a channel; needs an out-of-daemon
mail receiver.

**Verified** — no inbound-mail path anywhere in daemon/; the only ingress handlers are the
oc/1 protocol and the webhook POST

## 111. Screenshare is designed and unbuilt

Screenshare designed but zero code, with four unresolved design questions.

*Impact:* No screen sharing anywhere; open before any build: fragmentation past the
1400-byte datagram cap, loss recovery, rate control, and a codec field on
CALL_JOINED. Sequenced behind the audio client.

**Verified** — grep -rniE 'vp9|libvpx|screenshare|screen_share' (excluding third_party) -> 0
hits

## 112. The webhook endpoint has no CA-signed certificate

No CA-signed certificate for the webhook endpoint; no ACME/on-demand issuance.

*Impact:* Any ordinary HTTPS sender must disable certificate verification to post a
webhook. With the ALPN defect below fixed, this is now the only thing between a
third-party sender and a working webhook.

**Verified** — grep -rniE 'acme|letsencrypt|ca-signed' daemon/ shared/ scripts/ -> 0 hits;
webhook endpoint rides the TOFU self-signed cert (shared/tls.c,
daemon/netloop.c:3776)

## 113. A webhook can post into an archived channel

Archiving makes a channel read-only, and `SEND`, `SEND_REPLY` and `UPLOAD_BEGIN`
all enforce it through one shared access check. The incoming-webhook post path
does not call it: it resolves the token, and inserts the message.

*Impact:* a third party holding a webhook token writes into a channel the product
presents as read-only — the Win32 composer is locked and the About panel says
"Archived". The message is stored and broadcast to every connected member, and
the sender is told `{"ok":true}`. Archiving does not disable a channel's
webhooks either. **Verified end to end** on 2026-08-02: against a daemon built
from `453782f`, a POST to a live token on an archived channel returned
`HTTP 200 {"ok":true,"message_id":1}` and the row is in `messages`. No test
covers it.

## 114. Editing a profile drops the connection and saves nothing

`OC_MSG_SET_PROFILE` and `OC_MSG_SET_PRESENCE` are both `0x0070`, and the
daemon's dispatch chain tests `SET_PRESENCE` first, so the `SET_PROFILE` branch
is unreachable. A profile frame carries two length-prefixed strings — at minimum
four bytes — where the presence decoder expects exactly one, so the decode fails
as malformed and the daemon closes the connection without an error frame.

*Impact:* opening **You → Edit profile**, entering a title or timezone and
confirming disconnects the client every time. The fields are never stored. The
client then auto-reconnects on its session token, so it presents as a recurring
"connection lost — reconnecting" blip beside an edit that silently did nothing,
which is why it has gone unnoticed. Nothing exercises `SET_PROFILE` over the
wire in any test. **Verified** — full chain read and the decode reproduced
against the real codec.

## 115. The push emitter has no client that can register a device

The daemon owns the device-token registry and emits signed, contentless push
batches to the control-plane gateway. `REGISTER_DEVICE_TOKEN` is sent by no
shipped client — only by `tests/demo_client.c`.

*Impact:* the whole push path (REQ-132/133, ARCH-85) is unreachable in
production. Mobile clients are what would populate the registry and they do not
exist. With email notification excluded by decision (REQ-280), a user who is not
looking at a client is not reached at all. **Verified** — CLIENT.md §3 lists the
frames no client sends.

---

## 116. The audio client does not exist

Call signalling, the per-channel roster, per-join tokens and the forked UDP relay
sidecar are built and tested server-side. The client half is absent: no Opus, no
UDP media path, no device enumeration, no echo cancellation.

*Impact:* calls cannot be made. The daemon carries and maintains a complete media
subsystem that nothing can reach, and three further requirements are sequenced
behind it — screenshare (REQ-161), the call-start notification (REQ-285), and the
Huddles surface the Win32 sidebar deliberately omits because a row would point at
nothing. **Verified** — no `CALL_*` in `client/core`.

## 117. No client can complete an OIDC login

The daemon verifies a re-issued ES256 token and the control plane mints one, but
the client half — the system-browser flow, PKCE, and the loopback redirect that
carries the token from one to the other — does not exist in `client/core` or the
Win32 client.

*Impact:* single sign-on does not work in any deployment model. Combined with the
deliberate absence of SAML (REQ-027), an organisation whose procurement requires
SSO is unserved, and the hosted model's stated auth mode cannot be used by a real
user. `scripts/demo-oidc.sh` proves the mint↔verify contract with a dev endpoint,
deliberately bypassing the browser flow. **Verified** — no OIDC entry point in
the facade; REQ-020 and AUTH.md §7 both record it.

## 118. The TLS handshake rejects every standard HTTPS client, so incoming webhooks are unreachable

The daemon advertises exactly one ALPN protocol, `oc/1`. A client that offers any
other protocol list — which is every ordinary HTTPS client, including `curl`,
GitHub and Zapier — is refused at the handshake with `no_application_protocol`
and never reaches the HTTP handler.

*Impact:* incoming webhooks (REQ-170) are the product's one integration surface,
and their entire stated audience is third-party senders that "are not under the
operator's control". None of them can connect. Only a client that offers **no**
ALPN at all gets through, which is what the daemon's own test client does —
`tests/itest_netloop.c:140` opens the webhook connection with ALPN explicitly
disabled, which is precisely why the suite has never seen this. The webhook
CA-certificate work (REQ-171) would not help; the connection dies before
certificate trust matters. **Verified end to end** on 2026-08-02: `curl` was
refused with `TLS alert, no application protocol (632)`, and the same request
with `--no-alpn` succeeded.

**FIXED 2026-08-02.** The daemon now advertises `oc/1` **and** `http/1.1`
(`OC_ALPN_HTTP11`); mbedTLS selects by the server's preference order, so `oc/1`
is still chosen whenever the peer offers it. Measured against a daemon built
from this branch: default `curl` (offering `h2,http/1.1`) now reports `ALPN:
server accepted http/1.1` and gets `HTTP/1.1 404` from the webhook handler for
an unknown token — reaching the handler is the thing that was impossible — where
before the fix the same command died at the handshake with OpenSSL reason 1120.
`curl --http1.1` likewise returns 404 rather than failing to connect.

The reason the suite never saw this is closed too: `tests/itest_netloop.c`'s
webhook client offered **no** ALPN, the one shape that never exercises server
selection. It now offers `h2,http/1.1` exactly as curl does, and drives the same
200-with-`author_name` and 404 assertions through it. `tests/itest_tls.c` gains
`test_tls_alpn_demux`, covering four peers against the real server config: an
HTTPS list selects `http/1.1`; a peer offering both gets `oc/1`; no ALPN
connects and selects nothing; and an `h2`-only peer is still refused, since
serving HTTP bytes to a peer that cannot parse them is not an improvement.
Both checks were proven to fail — reinstating the one-protocol server list
produced 13 failing assertions across the two suites, 2 of them in the new
`test_tls_alpn_demux` and the rest the webhook path in `itest_netloop`.

---

## Recorded and closed without a fix

- **The unreproduced typing crash.** Three crashes on 2026-07-29, all while
  typing, none reproduced. The crash filter went in *after* the last occurrence
  and the composer was replaced wholesale the next morning (WIN-80/ARCH-98), so
  every occurrence happened in code that no longer exists. The filter is verified
  working — a deliberate fault produces a report and a minidump — and has seen
  nothing since, across a targeted composer stress run and repeated suite runs. A
  recurrence is new information and a new entry.

## Excluded by decision, not missing

Each is a choice on the record rather than an omission: camera video (REQ-160),
GIF and sticker pickers (REQ-270), Canvas (REQ-271), Lists and boards (REQ-272),
Clips and async voice/video messages (REQ-273), Slack-Connect-style
cross-organisation channels (REQ-274), a first-party bot or MCP surface
(REQ-275), email notifications of any kind (REQ-280), SAML and bring-your-own-IdP
(REQ-027), and slash commands in the GUI (ARCH-82, which is affordance-driven).

## Out of this repository

These are real gaps in the product and belong to the **control plane**
(`openchime-saas`), not to the daemon or the Win32 client: the OIDC relay's
provider integrations for Microsoft Entra and Apple (REQ-021/022), the
workspace/audience registry, the APNs/FCM push gateway, SCIM provisioning
(REQ-253), and the app directory's hosting half (REQ-175's server side). The
daemon's side of each — token verification, enrollment, the push emitter — is
built and is covered above where it is not.
